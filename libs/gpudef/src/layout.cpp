#include <carto/gpudef/layout.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace carto::gpudef {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic refused(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
    std::uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[shift / 8U]) << shift;
    }
    return value;
}

float read_f32(const std::uint8_t* bytes) noexcept {
    std::uint32_t bits = read_u32(bytes);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void append_f32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0U;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(out, bits);
}

// Stores one scalar component in the declared mode at the cursor. Weight
// components pass is_weight = true (unorm16 weights are scaled by 65535);
// position components pass the unorm axis bounds.
void append_component(
    std::vector<std::uint8_t>& out,
    QuantizationMode mode,
    double value,
    bool is_weight,
    double unorm_min,
    double unorm_max) {
    switch (mode) {
    case QuantizationMode::fp32:
        append_f32(out, static_cast<float>(value));
        return;
    case QuantizationMode::fp16:
        append_u16(out, float_to_half_bits(static_cast<float>(value)));
        return;
    case QuantizationMode::unorm16: {
        double scaled = 0.0;
        if (is_weight) {
            scaled = std::round(value * 65535.0);
        } else {
            const double span = unorm_max - unorm_min;
            scaled = std::round((value - unorm_min) * (65535.0 / span));
        }
        const auto quantized = static_cast<std::uint16_t>(scaled);
        append_u16(out, quantized);
        return;
    }
    }
}

double read_component(
    const std::uint8_t* bytes,
    QuantizationMode mode,
    bool is_weight,
    double unorm_min,
    double unorm_max) {
    switch (mode) {
    case QuantizationMode::fp32:
        return static_cast<double>(read_f32(bytes));
    case QuantizationMode::fp16:
        return static_cast<double>(half_bits_to_float(read_u16(bytes)));
    case QuantizationMode::unorm16: {
        const double unit = static_cast<double>(read_u16(bytes)) / 65535.0;
        return is_weight ? unit : unorm_min + unit * (unorm_max - unorm_min);
    }
    }
    return 0.0;
}

// Analytic declared bound for one component in the given mode. For fp
// modes the bound is half an ulp of the largest stored magnitude (binary32
// or binary16 respectively); for unorm16 it is half the quantum.
double declared_component_bound(
    QuantizationMode mode,
    double max_magnitude,
    double unorm_span) {
    switch (mode) {
    case QuantizationMode::fp32: {
        const float magnitude = static_cast<float>(max_magnitude);
        if (!std::isfinite(magnitude) || magnitude == 0.0F) return 0.0;
        return 0.5 * std::abs(static_cast<double>(std::nextafter(
            magnitude, std::numeric_limits<float>::infinity())) - magnitude);
    }
    case QuantizationMode::fp16: {
        const std::uint16_t bits = float_to_half_bits(
            static_cast<float>(max_magnitude));
        const double magnitude = std::abs(static_cast<double>(half_bits_to_float(bits)));
        if (magnitude == 0.0 || !std::isfinite(magnitude)) return 0.0;
        const std::uint16_t next_bits = static_cast<std::uint16_t>(bits + 1U);
        const double next = std::abs(static_cast<double>(half_bits_to_float(next_bits)));
        return 0.5 * std::abs(next - magnitude);
    }
    case QuantizationMode::unorm16:
        return 0.5 * unorm_span / 65535.0;
    }
    return 0.0;
}

core::Result<void> validate_skin(const SkinInputView& skin, std::uint32_t capacity) {
    if (skin.positions.empty() ||
        skin.positions.size() > PackedVertexBuffer::kMaxVertices) {
        return core::Result<void>::failure(invalid(
            "skin input vertex count is empty or exceeds the packed buffer bound"));
    }
    if (skin.influence_counts.size() != skin.positions.size()) {
        return core::Result<void>::failure(invalid(
            "skin input influence counts must match the vertex count"));
    }
    std::uint64_t total = 0U;
    for (const std::uint32_t count : skin.influence_counts) {
        if (count > capacity) {
            return core::Result<void>::failure(refused(
                "vertex influence count exceeds the packed layout capacity; "
                "run an explicit top-four reduction first"));
        }
        total += count;
    }
    if (total != skin.influences.size()) {
        return core::Result<void>::failure(invalid(
            "skin input flattened influences must match the sum of influence counts"));
    }
    if (skin.joint_count == 0U || skin.joint_count > PackedVertexBuffer::kMaxJoints) {
        return core::Result<void>::failure(invalid(
            "skin input joint count is empty or exceeds the uint16 index bound"));
    }
    for (const core::Vec3d& position : skin.positions) {
        if (!position.finite()) {
            return core::Result<void>::failure(invalid(
                "skin input positions must be finite"));
        }
    }
    std::size_t cursor = 0U;
    for (std::size_t vertex = 0U; vertex < skin.positions.size(); ++vertex) {
        const std::size_t count = skin.influence_counts[vertex];
        const std::span<const kinematic::JointInfluence> list{
            skin.influences.data() + cursor, count};
        if (count > 0U) {
            if (auto result = character::validate_influences(list); !result) {
                return core::Result<void>::failure(result.error().with_context(
                    "skin input influences failed the shared admission contract"));
            }
            for (const kinematic::JointInfluence& influence : list) {
                if (!influence.joint || influence.joint.value >= skin.joint_count) {
                    return core::Result<void>::failure(invalid(
                        "skin input influence references a joint outside the declared count"));
                }
            }
        }
        cursor += count;
    }
    return core::Result<void>::success();
}

} // namespace

std::string_view influence_layout_name(InfluenceLayout layout) noexcept {
    switch (layout) {
    case InfluenceLayout::influences_8: return "influences-8";
    case InfluenceLayout::influences_4: return "influences-4";
    }
    return "unknown";
}

std::string_view quantization_mode_name(QuantizationMode mode) noexcept {
    switch (mode) {
    case QuantizationMode::fp32: return "fp32";
    case QuantizationMode::fp16: return "fp16";
    case QuantizationMode::unorm16: return "unorm16";
    }
    return "unknown";
}

core::Result<void> PositionQuantizationBounds::validate() const {
    if (!minimum.finite() || !maximum.finite() ||
        !(minimum.x < maximum.x) || !(minimum.y < maximum.y) ||
        !(minimum.z < maximum.z)) {
        return core::Result<void>::failure(invalid(
            "position quantization bounds must be finite with min < max per axis"));
    }
    return core::Result<void>::success();
}

core::Result<void> QuantizationErrorReceipt::validate() const {
    if (vertex_count == 0U || max_position_component_error < 0.0 ||
        max_weight_error < 0.0 ||
        !std::isfinite(max_position_component_error) ||
        !std::isfinite(max_weight_error) ||
        declared_position_error_bound < 0.0 || declared_weight_error_bound < 0.0 ||
        !std::isfinite(declared_position_error_bound) ||
        !std::isfinite(declared_weight_error_bound)) {
        return core::Result<void>::failure(invalid(
            "quantization error receipt counts or error fields are invalid"));
    }
    return core::Result<void>::success();
}

std::string QuantizationErrorReceipt::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTO_GPUDEF_QUANTIZATION_ERROR 1\n";
    output << "MODE " << quantization_mode_name(mode) << ' '
           << influence_layout_name(layout) << ' ' << vertex_count << '\n';
    output << "MEASURED " << max_position_component_error << ' '
           << max_weight_error << '\n';
    output << "DECLARED " << declared_position_error_bound << ' '
           << declared_weight_error_bound << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> QuantizationErrorReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<void> PackedVertexBuffer::validate() const {
    if (vertex_count == 0U || vertex_count > kMaxVertices ||
        joint_count == 0U || joint_count > kMaxJoints) {
        return core::Result<void>::failure(invalid(
            "packed vertex buffer counts are empty or unbounded"));
    }
    if (payload.size() != static_cast<std::size_t>(vertex_count) * stride_bytes()) {
        return core::Result<void>::failure(invalid(
            "packed vertex buffer payload does not match vertex count and stride"));
    }
    if (mode == QuantizationMode::unorm16) {
        if (!position_bounds) {
            return core::Result<void>::failure(invalid(
                "unorm16 packed buffers require position quantization bounds"));
        }
        if (auto result = position_bounds->validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
    }
    return core::Result<void>::success();
}

std::string PackedVertexBuffer::serialize() const {
    std::ostringstream output;
    output << "CARTO_GPUDEF_PACKED_BUFFER 1\n";
    output << "HEADER " << influence_layout_name(layout) << ' '
           << quantization_mode_name(mode) << ' ' << vertex_count << ' '
           << joint_count << '\n';
    if (position_bounds) {
        output << std::setprecision(17);
        output << "BOUNDS " << position_bounds->minimum.x << ' '
               << position_bounds->minimum.y << ' ' << position_bounds->minimum.z
               << ' ' << position_bounds->maximum.x << ' '
               << position_bounds->maximum.y << ' ' << position_bounds->maximum.z
               << '\n';
    }
    output << "PAYLOAD ";
    for (const std::uint8_t byte : payload) {
        output << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(byte);
    }
    output << std::dec << "\nEND\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PackedVertexBuffer::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::vector<std::uint8_t> canonical;
    const std::string header = serialize();
    // Hash the header up to (not including) the payload line, then the raw
    // payload bytes, so the digest binds exact stream bytes, not hex text.
    const std::string marker = "PAYLOAD ";
    const std::size_t cut = header.find(marker);
    const std::string header_only = header.substr(0U, cut);
    canonical.insert(canonical.end(),
        reinterpret_cast<const std::uint8_t*>(header_only.data()),
        reinterpret_cast<const std::uint8_t*>(header_only.data()) + header_only.size());
    canonical.insert(canonical.end(), payload.begin(), payload.end());
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(std::span<const std::uint8_t>{canonical}));
}

core::Result<PackedVertexBuffer> PackedVertexBuffer::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_GPUDEF_PACKED_BUFFER" || version != kSchemaVersion) {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer framing or schema version is invalid"));
    }
    PackedVertexBuffer buffer;
    std::string token;
    std::string layout_name;
    std::string mode_name;
    if (!(input >> token) || token != "HEADER" ||
        !(input >> layout_name >> mode_name >> buffer.vertex_count >>
            buffer.joint_count)) {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer header is malformed"));
    }
    if (layout_name == "influences-8") {
        buffer.layout = InfluenceLayout::influences_8;
    } else if (layout_name == "influences-4") {
        buffer.layout = InfluenceLayout::influences_4;
    } else {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer layout is unknown"));
    }
    if (mode_name == "fp32") {
        buffer.mode = QuantizationMode::fp32;
    } else if (mode_name == "fp16") {
        buffer.mode = QuantizationMode::fp16;
    } else if (mode_name == "unorm16") {
        buffer.mode = QuantizationMode::unorm16;
    } else {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer quantization mode is unknown"));
    }
    if (buffer.mode == QuantizationMode::unorm16) {
        PositionQuantizationBounds bounds;
        if (!(input >> token) || token != "BOUNDS" ||
            !(input >> bounds.minimum.x >> bounds.minimum.y >> bounds.minimum.z >>
                bounds.maximum.x >> bounds.maximum.y >> bounds.maximum.z)) {
            return core::Result<PackedVertexBuffer>::failure(invalid(
                "packed buffer unorm bounds are malformed"));
        }
        buffer.position_bounds = bounds;
    }
    std::string hex;
    if (!(input >> token) || token != "PAYLOAD" || !(input >> hex) ||
        !(input >> token) || token != "END") {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer payload framing is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer text contains trailing unbound data"));
    }
    const std::size_t stride = buffer.stride_bytes();
    if (stride == 0U || hex.size() != stride * buffer.vertex_count * 2U ||
        hex.size() % 2U != 0U) {
        return core::Result<PackedVertexBuffer>::failure(invalid(
            "packed buffer payload hex does not match the declared shape"));
    }
    buffer.payload.reserve(hex.size() / 2U);
    auto nibble = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        return 0xFFU;
    };
    for (std::size_t index = 0U; index < hex.size(); index += 2U) {
        const std::uint8_t high = nibble(hex[index]);
        const std::uint8_t low = nibble(hex[index + 1U]);
        if (high == 0xFFU || low == 0xFFU) {
            return core::Result<PackedVertexBuffer>::failure(invalid(
                "packed buffer payload hex is malformed"));
        }
        buffer.payload.push_back(static_cast<std::uint8_t>((high << 4U) | low));
    }
    if (auto result = buffer.validate(); !result) {
        return core::Result<PackedVertexBuffer>::failure(result.error());
    }
    return core::Result<PackedVertexBuffer>::success(std::move(buffer));
}

core::Result<PackResult> pack(
    const SkinInputView& skin,
    InfluenceLayout layout,
    QuantizationMode mode) {
    const std::uint32_t capacity = influences_per_vertex(layout);
    if (auto result = validate_skin(skin, capacity); !result) {
        return core::Result<PackResult>::failure(result.error());
    }
    if (mode == QuantizationMode::unorm16 &&
        (!skin.position_bounds || !skin.position_bounds->validate())) {
        return core::Result<PackResult>::failure(invalid(
            "unorm16 packing requires valid position quantization bounds"));
    }

    PackResult result;
    result.buffer.layout = layout;
    result.buffer.mode = mode;
    result.buffer.vertex_count = static_cast<std::uint32_t>(skin.positions.size());
    result.buffer.joint_count = skin.joint_count;
    result.buffer.position_bounds = skin.position_bounds;

    QuantizationErrorReceipt& error = result.error;
    error.mode = mode;
    error.layout = layout;
    error.vertex_count = result.buffer.vertex_count;

    double max_position_magnitude = 0.0;
    for (const core::Vec3d& position : skin.positions) {
        max_position_magnitude = std::max({
            max_position_magnitude,
            std::abs(position.x), std::abs(position.y), std::abs(position.z)});
    }
    double max_unorm_span = 1.0;
    if (mode == QuantizationMode::unorm16) {
        max_unorm_span = std::max({
            skin.position_bounds->maximum.x - skin.position_bounds->minimum.x,
            skin.position_bounds->maximum.y - skin.position_bounds->minimum.y,
            skin.position_bounds->maximum.z - skin.position_bounds->minimum.z});
    }
    error.declared_position_error_bound = declared_component_bound(
        mode, max_position_magnitude, max_unorm_span);
    error.declared_weight_error_bound =
        mode == QuantizationMode::fp32 ? 0.0
        : mode == QuantizationMode::fp16 ? std::ldexp(1.0, -11)
        : 0.5 / 65535.0;

    const double unorm_min = mode == QuantizationMode::unorm16
        ? skin.position_bounds->minimum.x : 0.0;

    result.buffer.payload.reserve(
        skin.positions.size() * vertex_stride_bytes(layout, mode));
    std::size_t cursor = 0U;
    for (std::size_t vertex = 0U; vertex < skin.positions.size(); ++vertex) {
        const core::Vec3d& position = skin.positions[vertex];
        // Positions: three components in the declared mode.
        for (int axis = 0; axis < 3; ++axis) {
            const double component = axis == 0 ? position.x
                : axis == 1 ? position.y : position.z;
            const double unorm_lo = mode == QuantizationMode::unorm16
                ? (axis == 0 ? skin.position_bounds->minimum.x
                    : axis == 1 ? skin.position_bounds->minimum.y
                    : skin.position_bounds->minimum.z)
                : unorm_min;
            const double unorm_hi = mode == QuantizationMode::unorm16
                ? (axis == 0 ? skin.position_bounds->maximum.x
                    : axis == 1 ? skin.position_bounds->maximum.y
                    : skin.position_bounds->maximum.z)
                : 0.0;
            append_component(
                result.buffer.payload, mode, component, false, unorm_lo, unorm_hi);
            const double decoded = read_component(
                result.buffer.payload.data() + result.buffer.payload.size() -
                    component_bytes(mode),
                mode, false, unorm_lo, unorm_hi);
            error.max_position_component_error = std::max(
                error.max_position_component_error, std::abs(decoded - component));
        }
        // Influences: capacity slots, admitted list first (sorted by joint
        // id), zero padding after.
        const std::size_t count = skin.influence_counts[vertex];
        const std::span<const kinematic::JointInfluence> list{
            skin.influences.data() + cursor, count};
        for (std::uint32_t slot = 0U; slot < capacity; ++slot) {
            if (slot < count) {
                const kinematic::JointInfluence& influence = list[slot];
                append_u16(result.buffer.payload,
                    static_cast<std::uint16_t>(influence.joint.value));
                append_component(result.buffer.payload, mode, influence.weight,
                    true, 0.0, 0.0);
                const double decoded = read_component(
                    result.buffer.payload.data() + result.buffer.payload.size() -
                        component_bytes(mode),
                    mode, true, 0.0, 0.0);
                error.max_weight_error = std::max(
                    error.max_weight_error, std::abs(decoded - influence.weight));
            } else {
                append_u16(result.buffer.payload, 0U);
                append_component(result.buffer.payload, mode, 0.0, true, 0.0, 0.0);
            }
        }
        cursor += count;
    }
    if (auto valid = result.buffer.validate(); !valid) {
        return core::Result<PackResult>::failure(valid.error());
    }
    if (auto valid = error.validate(); !valid) {
        return core::Result<PackResult>::failure(valid.error());
    }
    return core::Result<PackResult>::success(std::move(result));
}

core::Result<DecodedSkin> unpack(const PackedVertexBuffer& buffer) {
    if (auto result = buffer.validate(); !result) {
        return core::Result<DecodedSkin>::failure(result.error());
    }
    DecodedSkin decoded;
    decoded.positions.resize(buffer.vertex_count);
    decoded.influence_counts.resize(buffer.vertex_count);
    const std::uint32_t capacity = influences_per_vertex(buffer.layout);
    const std::size_t stride = buffer.stride_bytes();
    std::size_t offset = 0U;
    for (std::uint32_t vertex = 0U; vertex < buffer.vertex_count; ++vertex) {
        const std::uint8_t* base = buffer.payload.data() + offset;
        core::Vec3d position{};
        std::size_t cursor_bytes = 0U;
        for (int axis = 0; axis < 3; ++axis) {
            const double unorm_lo = buffer.position_bounds
                ? (axis == 0 ? buffer.position_bounds->minimum.x
                    : axis == 1 ? buffer.position_bounds->minimum.y
                    : buffer.position_bounds->minimum.z)
                : 0.0;
            const double unorm_hi = buffer.position_bounds
                ? (axis == 0 ? buffer.position_bounds->maximum.x
                    : axis == 1 ? buffer.position_bounds->maximum.y
                    : buffer.position_bounds->maximum.z)
                : 0.0;
            const double component = read_component(
                base + cursor_bytes, buffer.mode, false, unorm_lo, unorm_hi);
            if (axis == 0) position.x = component;
            else if (axis == 1) position.y = component;
            else position.z = component;
            cursor_bytes += component_bytes(buffer.mode);
        }
        decoded.positions[vertex] = position;
        std::uint32_t count = 0U;
        for (std::uint32_t slot = 0U; slot < capacity; ++slot) {
            const std::uint8_t* slot_base = base + 3U * component_bytes(buffer.mode) +
                static_cast<std::size_t>(slot) * (2U + component_bytes(buffer.mode));
            const std::uint16_t joint_bits = read_u16(slot_base);
            const double weight = read_component(
                slot_base + 2U, buffer.mode, true, 0.0, 0.0);
            // Corrupt streams fail closed: a slot must never carry exactly one
            // of (nonzero joint, nonzero weight).
            if ((joint_bits != 0U) != (weight != 0.0)) {
                return core::Result<DecodedSkin>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "packed buffer influence slot is corrupt"));
            }
            if (joint_bits != 0U &&
                joint_bits >= buffer.joint_count) {
                return core::Result<DecodedSkin>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "packed buffer influence references a joint outside the declared count"));
            }
            if (joint_bits != 0U) {
                decoded.influences.push_back(kinematic::JointInfluence{
                    kinematic::JointId{
                        static_cast<std::uint64_t>(joint_bits)},
                    weight});
                ++count;
            }
        }
        decoded.influence_counts[vertex] = count;
        offset += stride;
    }
    return core::Result<DecodedSkin>::success(std::move(decoded));
}

std::uint16_t float_to_half_bits(float value) noexcept {
    std::uint32_t bits = 0U;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU);
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent == 0xFF) {
        // Inf/NaN: preserve the NaN payload bit so NaN stays NaN.
        return static_cast<std::uint16_t>(
            sign | 0x7C00U | (mantissa != 0U ? 0x200U | (mantissa >> 13U) : 0U));
    }
    // Unbias and rebias onto the binary16 exponent range.
    exponent = exponent - 127 + 15;
    if (exponent >= 0x1F) {
        return static_cast<std::uint16_t>(sign | 0x7C00U); // overflow -> inf
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign); // underflow -> zero
        }
        // Subnormal binary16: shift the implicit leading 1 down and round.
        mantissa |= 0x800000U;
        const int shift = 14 - exponent;
        const std::uint32_t half = mantissa >> static_cast<unsigned>(shift);
        const std::uint32_t remainder = mantissa &
            ((1ULL << static_cast<unsigned>(shift)) - 1U);
        const std::uint32_t halfway =
            1UL << static_cast<unsigned>(shift - 1);
        std::uint32_t rounded = half;
        if (remainder > halfway || (remainder == halfway && (half & 1U))) {
            ++rounded;
        }
        return static_cast<std::uint16_t>(sign | rounded);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10U) |
        (mantissa >> 13U);
    const std::uint32_t remainder = mantissa & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U))) {
        ++half; // round-to-nearest-even carry, including into the exponent
    }
    return static_cast<std::uint16_t>(sign | half);
}

float half_bits_to_float(std::uint16_t bits) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16U;
    const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x3FFU;
    std::uint32_t out = 0U;
    if (exponent == 0U) {
        if (mantissa == 0U) {
            out = sign;
        } else {
            // Subnormal binary16: normalize into binary32.
            int leading = 0;
            std::uint32_t value = mantissa;
            while ((value & 0x400U) == 0U) {
                value <<= 1U;
                ++leading;
            }
            // value = 1.f * 2^(-14 - leading); mantissa field carries the
            // fractional bits of 1.f starting at bit 23.
            const std::uint32_t exp32 = static_cast<std::uint32_t>(113 - leading);
            out = sign | (exp32 << 23U) |
                ((mantissa << static_cast<unsigned>(leading + 13)) & 0x7FFFFFU);
        }
    } else if (exponent == 0x1FU) {
        out = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        out = sign |
            ((exponent - 15U + 127U) << 23U) | (mantissa << 13U);
    }
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(out));
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

core::Result<TopFourReduction> reduce_to_top_four(
    std::span<const std::uint32_t> influence_counts,
    std::span<const kinematic::JointInfluence> influences) {
    std::uint64_t total = 0U;
    for (const std::uint32_t count : influence_counts) {
        if (count > character::SkinWeightLayer::kMaxInfluencesPerVertex) {
            return core::Result<TopFourReduction>::failure(invalid(
                "top-four reduction input exceeds the admitted influence bound"));
        }
        total += count;
    }
    if (total != influences.size()) {
        return core::Result<TopFourReduction>::failure(invalid(
            "top-four reduction inputs must match the flattened influence count"));
    }
    TopFourReduction reduced;
    reduced.influence_counts.reserve(influence_counts.size());
    std::size_t cursor = 0U;
    for (const std::uint32_t count : influence_counts) {
        const std::span<const kinematic::JointInfluence> list{
            influences.data() + cursor, count};
        if (count > 0U) {
            if (auto result = character::validate_influences(list); !result) {
                return core::Result<TopFourReduction>::failure(result.error());
            }
        }
        std::vector<kinematic::JointInfluence> kept{
            list.begin(), list.end()};
        // Deterministic selection: sort by (weight desc, joint id asc), keep
        // the first four, then restore joint-id order for canonical storage.
        std::sort(kept.begin(), kept.end(),
            [](const kinematic::JointInfluence& left,
               const kinematic::JointInfluence& right) {
                if (left.weight != right.weight) return left.weight > right.weight;
                return left.joint.value < right.joint.value;
            });
        double discarded = 0.0;
        if (kept.size() > 4U) {
            for (std::size_t index = 4U; index < kept.size(); ++index) {
                discarded += kept[index].weight;
            }
            kept.resize(4U);
        }
        double sum = 0.0;
        for (const kinematic::JointInfluence& influence : kept) {
            sum += influence.weight;
        }
        for (kinematic::JointInfluence& influence : kept) {
            influence.weight /= sum;
        }
        std::sort(kept.begin(), kept.end(),
            [](const kinematic::JointInfluence& left,
               const kinematic::JointInfluence& right) {
                return left.joint.value < right.joint.value;
            });
        reduced.influence_counts.push_back(
            static_cast<std::uint32_t>(kept.size()));
        reduced.influences.insert(reduced.influences.end(), kept.begin(), kept.end());
        reduced.max_discarded_weight_mass = std::max(
            reduced.max_discarded_weight_mass, discarded);
        cursor += count;
    }
    return core::Result<TopFourReduction>::success(std::move(reduced));
}

} // namespace carto::gpudef
