#include <carto/gpudef/emulation.hpp>

#include <carto/core/math.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace carto::gpudef {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

// Reads one packed scalar (mirror of the pack codec in layout.cpp; kept
// private so the stream layout has a single owner).
double decode_component(
    const std::uint8_t* bytes,
    QuantizationMode mode,
    bool is_weight,
    double unorm_min,
    double unorm_max) noexcept {
    std::uint16_t bits = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8U));
    if (mode != QuantizationMode::fp32) {
        if (mode == QuantizationMode::fp16) {
            return static_cast<double>(half_bits_to_float(bits));
        }
        const double unit = static_cast<double>(bits) / 65535.0;
        return is_weight ? unit
                         : unorm_min + unit * (unorm_max - unorm_min);
    }
    const std::uint32_t word =
        static_cast<std::uint32_t>(bits) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(word));
    std::memcpy(&value, &word, sizeof(value));
    return static_cast<double>(value);
}

} // namespace

Mat34 Mat34::from_transform(const core::Transform& transform) noexcept {
    const core::Quaternion rotation = transform.rotation.normalized();
    const core::Vec3d& scale = transform.scale;
    const core::Vec3d r0 = rotation.rotate({1.0, 0.0, 0.0});
    const core::Vec3d r1 = rotation.rotate({0.0, 1.0, 0.0});
    const core::Vec3d r2 = rotation.rotate({0.0, 0.0, 1.0});
    Mat34 matrix{};
    matrix.rows = {
        r0.x * scale.x, r1.x * scale.y, r2.x * scale.z, transform.translation.x,
        r0.y * scale.x, r1.y * scale.y, r2.y * scale.z, transform.translation.y,
        r0.z * scale.x, r1.z * scale.y, r2.z * scale.z, transform.translation.z,
    };
    return matrix;
}

Mat34 Mat34::compose(const Mat34& other) const noexcept {
    Mat34 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.rows[static_cast<std::size_t>(row) * 4U +
                static_cast<std::size_t>(column)] =
                rows[static_cast<std::size_t>(row) * 4U] *
                    other.rows[static_cast<std::size_t>(column)] +
                rows[static_cast<std::size_t>(row) * 4U + 1U] *
                    other.rows[static_cast<std::size_t>(column) + 4U] +
                rows[static_cast<std::size_t>(row) * 4U + 2U] *
                    other.rows[static_cast<std::size_t>(column) + 8U];
        }
        result.rows[static_cast<std::size_t>(row) * 4U + 3U] =
            rows[static_cast<std::size_t>(row) * 4U] * other.rows[3U] +
            rows[static_cast<std::size_t>(row) * 4U + 1U] * other.rows[7U] +
            rows[static_cast<std::size_t>(row) * 4U + 2U] * other.rows[11U] +
            rows[static_cast<std::size_t>(row) * 4U + 3U];
    }
    return result;
}

bool Mat34::finite() const noexcept {
    return std::all_of(rows.begin(), rows.end(),
        [](double value) { return std::isfinite(value); });
}

core::Result<Mat34> Mat34::inverted(double epsilon) const noexcept {
    // Closed-form 3x3 inverse via cofactors; translation follows as
    // -R^-1 * t.
    const double a = rows[0]; const double b = rows[1]; const double c = rows[2];
    const double d = rows[4]; const double e = rows[5]; const double f = rows[6];
    const double g = rows[8]; const double h = rows[9]; const double i = rows[10];
    const double A = e * i - f * h;
    const double B = c * h - b * i;
    const double C = b * f - c * e;
    const double determinant = a * A + d * B + g * C;
    if (!std::isfinite(determinant) || std::abs(determinant) <= epsilon) {
        return core::Result<Mat34>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "matrix is singular or near-singular and cannot be inverted"));
    }
    const double D = f * g - d * i;
    const double E = a * i - c * g;
    const double F = c * d - a * f;
    const double G = d * h - e * g;
    const double H = b * g - a * h;
    const double I = a * e - b * d;
    const double inv = 1.0 / determinant;
    Mat34 result{};
    result.rows = {
        A * inv, B * inv, C * inv, 0.0,
        D * inv, E * inv, F * inv, 0.0,
        G * inv, H * inv, I * inv, 0.0,
    };
    const double tx = rows[3]; const double ty = rows[7]; const double tz = rows[11];
    result.rows[3] = -(result.rows[0] * tx + result.rows[1] * ty + result.rows[2] * tz);
    result.rows[7] = -(result.rows[4] * tx + result.rows[5] * ty + result.rows[6] * tz);
    result.rows[11] = -(result.rows[8] * tx + result.rows[9] * ty + result.rows[10] * tz);
    return core::Result<Mat34>::success(result);
}

core::Vec3d reference_deform_position(
    core::Vec3d bind_position,
    std::span<const kinematic::JointInfluence> influences,
    std::span<const Mat34> skin_matrices) noexcept {
    core::Vec3d result{0.0, 0.0, 0.0};
    for (const kinematic::JointInfluence& influence : influences) {
        if (!influence.joint ||
            influence.joint.value >= skin_matrices.size()) {
            continue; // defensive: validated inputs never take this path
        }
        const core::Vec3d transformed = skin_matrices[influence.joint.value].apply(bind_position);
        result = result + transformed * influence.weight;
    }
    return result;
}

core::Result<std::vector<core::Vec3d>> reference_deform(
    const SkinInputView& skin,
    std::span<const Mat34> skin_matrices) {
    if (skin.influence_counts.size() != skin.positions.size()) {
        return core::Result<std::vector<core::Vec3d>>::failure(invalid(
            "skin input influence counts must match the vertex count"));
    }
    std::vector<core::Vec3d> output;
    output.reserve(skin.positions.size());
    std::size_t cursor = 0U;
    for (std::size_t vertex = 0U; vertex < skin.positions.size(); ++vertex) {
        const std::size_t count = skin.influence_counts[vertex];
        if (cursor + count > skin.influences.size()) {
            return core::Result<std::vector<core::Vec3d>>::failure(invalid(
                "skin input flattened influences are truncated"));
        }
        output.push_back(reference_deform_position(
            skin.positions[vertex],
            std::span<const kinematic::JointInfluence>{
                skin.influences.data() + cursor, count},
            skin_matrices));
        cursor += count;
    }
    return core::Result<std::vector<core::Vec3d>>::success(std::move(output));
}

core::Result<std::vector<core::Vec3d>> emulate_packed_deform(
    const PackedVertexBuffer& buffer,
    std::span<const Mat34> skin_matrices) {
    if (auto result = buffer.validate(); !result) {
        return core::Result<std::vector<core::Vec3d>>::failure(result.error());
    }
    const std::uint32_t capacity = influences_per_vertex(buffer.layout);
    const std::size_t component = component_bytes(buffer.mode);
    const std::size_t stride = buffer.stride_bytes();
    std::vector<core::Vec3d> output;
    output.reserve(buffer.vertex_count);
    for (std::uint32_t vertex = 0U; vertex < buffer.vertex_count; ++vertex) {
        const std::uint8_t* base = buffer.payload.data() +
            static_cast<std::size_t>(vertex) * stride;
        core::Vec3d bind{};
        for (int axis = 0; axis < 3; ++axis) {
            const double unorm_min = buffer.position_bounds
                ? (axis == 0 ? buffer.position_bounds->minimum.x
                    : axis == 1 ? buffer.position_bounds->minimum.y
                    : buffer.position_bounds->minimum.z)
                : 0.0;
            const double unorm_max = buffer.position_bounds
                ? (axis == 0 ? buffer.position_bounds->maximum.x
                    : axis == 1 ? buffer.position_bounds->maximum.y
                    : buffer.position_bounds->maximum.z)
                : 0.0;
            const double value = decode_component(
                base + static_cast<std::size_t>(axis) * component,
                buffer.mode, false, unorm_min, unorm_max);
            if (axis == 0) bind.x = value;
            else if (axis == 1) bind.y = value;
            else bind.z = value;
        }
        core::Vec3d deformed{0.0, 0.0, 0.0};
        const std::uint8_t* influence_base = base + 3U * component;
        for (std::uint32_t slot = 0U; slot < capacity; ++slot) {
            const std::uint8_t* slot_base = influence_base +
                static_cast<std::size_t>(slot) * (2U + component);
            const std::uint16_t joint_bits = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(slot_base[0]) |
                (static_cast<std::uint16_t>(slot_base[1]) << 8U));
            const double weight = decode_component(
                slot_base + 2U, buffer.mode, true, 0.0, 0.0);
            if (joint_bits == 0U || weight == 0.0) {
                continue;
            }
            if (joint_bits >= skin_matrices.size()) {
                return core::Result<std::vector<core::Vec3d>>::failure(
                    core::Diagnostic(core::ErrorCode::invalid_state,
                        "packed buffer influence references a joint without a skin matrix"));
            }
            deformed = deformed +
                skin_matrices[joint_bits].apply(bind) * weight;
        }
        output.push_back(deformed);
    }
    return core::Result<std::vector<core::Vec3d>>::success(std::move(output));
}

} // namespace carto::gpudef
