#include <carto/d3d12/sculpt_preview.hpp>

#include <carto/device_ir/command.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace carto::d3d12 {
namespace {

constexpr std::uint32_t kTextureRowWidth = 1024U;
constexpr std::uint32_t kComputeGroupWidth = 64U;
constexpr std::uint64_t kTexturePitchAlignment = 256U;
constexpr std::uint64_t kTexturePlacementAlignment = 512U;
constexpr std::uint64_t kMaxSculptPreviewUploadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr double kMaxSculptPreviewCoordinate = 1.0e9;

constexpr std::string_view kSculptPreviewShader = R"hlsl(
Texture2D<float4> vertex_records : register(t0);
Texture2D<float4> normal_records : register(t1);
Texture2D<float4> sample_records : register(t2);
RWTexture2D<float4> output_positions : register(u0);

cbuffer SculptPreviewConstants : register(b0) {
    uint vertex_count;
    uint vertex_width;
    uint sample_count;
    uint sample_copies_per_stroke_sample;
    uint sample_record_width;
    uint subtract_mode;
    uint reserved0;
    uint reserved1;
    float radius;
    float strength;
    float layer_weight;
    uint reserved2;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint vertex_index = dispatch_id.x;
    if (vertex_index >= vertex_count) return;

    uint2 coordinate = uint2(vertex_index % vertex_width, vertex_index / vertex_width);
    float4 source = vertex_records.Load(int3(coordinate, 0));
    float3 normal = normal_records.Load(int3(coordinate, 0)).xyz;
    float accumulated_falloff = 0.0f;
    for (uint sample_index = 0u; sample_index < sample_count; ++sample_index) {
        uint first_record = sample_index * sample_copies_per_stroke_sample;
        float pressure = 0.0f;
        float maximum_falloff = 0.0f;
        for (uint copy_index = 0u;
             copy_index < sample_copies_per_stroke_sample;
             ++copy_index) {
            uint record_index = first_record + copy_index;
            uint2 sample_coordinate = uint2(
                record_index % sample_record_width,
                record_index / sample_record_width);
            float4 sample = sample_records.Load(int3(sample_coordinate, 0));
            if (copy_index == 0u) pressure = sample.w;
            float remaining = 1.0f - length(source.xyz - sample.xyz) / radius;
            if (remaining <= 0.0f) continue;
            float t = min(1.0f, remaining);
            float falloff = t * t * (3.0f - 2.0f * t);
            maximum_falloff = max(maximum_falloff, falloff);
        }
        accumulated_falloff += maximum_falloff * pressure;
    }

    float direction_sign = subtract_mode != 0u ? -1.0f : 1.0f;
    float scale = strength * layer_weight * direction_sign * (1.0f - source.w) *
        accumulated_falloff;
    output_positions[coordinate] = float4(source.xyz + normal * scale, source.w);
}
)hlsl";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

class CanonicalBytes final {
public:
    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u32(std::uint32_t value) {
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void u64(std::uint64_t value) {
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

    void string(std::string_view value) {
        u64(static_cast<std::uint64_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void digest(const assets::Sha256Digest& value) {
        bytes.insert(bytes.end(), value.bytes.begin(), value.bytes.end());
    }

    std::vector<std::uint8_t> bytes;
};

struct KernelVertex {
    geometry::VertexId id;
    core::Vec3d position;
    core::Vec3d normal;
    double mask = 0.0;
};

struct KernelSampleCopy {
    core::Vec3d position;
    double pressure = 1.0;
};

struct ActiveStroke {
    const production::SculptLayer* layer = nullptr;
    const production::SculptStroke* stroke = nullptr;
};

struct PreparedPreview {
    ActiveStroke active;
    production::BakeMeshBinding source;
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest kernel_input_digest;
    std::uint32_t copies_per_sample = 1U;
    std::vector<KernelVertex> vertices;
    std::vector<KernelSampleCopy> samples;
};

core::Vec3d face_normal(
    const geometry::EditableMesh& mesh,
    const geometry::Face& face) {
    core::Vec3d normal{};
    if (face.vertices.size() < 3U) return normal;
    for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
        const geometry::Vertex* current = mesh.find_vertex(face.vertices[index]);
        const geometry::Vertex* next = mesh.find_vertex(
            face.vertices[(index + 1U) % face.vertices.size()]);
        if (current == nullptr || next == nullptr) return {};
        normal.x += (current->position.y - next->position.y) *
            (current->position.z + next->position.z);
        normal.y += (current->position.z - next->position.z) *
            (current->position.x + next->position.x);
        normal.z += (current->position.x - next->position.x) *
            (current->position.y + next->position.y);
    }
    return normal.normalized();
}

std::map<geometry::VertexId, core::Vec3d> vertex_normals(
    const geometry::EditableMesh& mesh) {
    std::map<geometry::VertexId, core::Vec3d> normals;
    for (const geometry::Vertex& vertex : mesh.vertices_sorted()) {
        normals.emplace(vertex.id, core::Vec3d{});
    }
    for (const geometry::Face& face : mesh.faces_sorted()) {
        const core::Vec3d normal = face_normal(mesh, face);
        if (!normal.finite() || normal.length_squared() <= 1.0e-24) continue;
        for (const geometry::VertexId id : face.vertices) {
            auto iterator = normals.find(id);
            if (iterator != normals.end()) iterator->second = iterator->second + normal;
        }
    }
    for (auto& [id, normal] : normals) {
        static_cast<void>(id);
        if (normal.finite() && normal.length_squared() > 1.0e-24) {
            normal = normal.normalized();
        } else {
            normal = {};
        }
    }
    return normals;
}

core::Result<std::vector<double>> read_vertex_mask(
    const geometry::EditableMesh& mesh,
    std::string_view layer_id,
    const std::vector<geometry::Vertex>& vertices) {
    std::vector<double> values(vertices.size(), 0.0);
    if (layer_id.empty()) {
        return core::Result<std::vector<double>>::success(std::move(values));
    }
    const attributes::AttributeLayer* layer = mesh.attributes().find(layer_id);
    if (layer == nullptr) {
        return core::Result<std::vector<double>>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "SculptStack mask layer does not exist on its source mesh"));
    }
    if (layer->descriptor.domain != attributes::AttributeDomain::vertex ||
        (layer->descriptor.type != attributes::AttributeType::float32 &&
         layer->descriptor.type != attributes::AttributeType::float64) ||
        layer->values.size() != vertices.size()) {
        return core::Result<std::vector<double>>::failure(validation(
            "SculptStack mask must be a complete vertex float32 or float64 layer"));
    }
    for (std::size_t index = 0U; index < layer->values.size(); ++index) {
        double value = 0.0;
        if (const auto* f32 = std::get_if<float>(&layer->values[index])) value = *f32;
        else if (const auto* f64 = std::get_if<double>(&layer->values[index])) value = *f64;
        else {
            return core::Result<std::vector<double>>::failure(validation(
                "SculptStack mask contains a value with the wrong stored type"));
        }
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return core::Result<std::vector<double>>::failure(validation(
                "SculptStack mask values must remain in the closed interval [0, 1]"));
        }
        values[index] = value;
    }
    return core::Result<std::vector<double>>::success(std::move(values));
}

core::Result<ActiveStroke> find_active_stroke(const production::SculptStack& stack) {
    ActiveStroke selected;
    std::size_t count = 0U;
    for (const production::SculptLayer& layer : stack.layers) {
        if (!layer.visible || layer.weight == 0.0) continue;
        for (const production::SculptStroke& stroke : layer.strokes) {
            if (!stroke.enabled) continue;
            selected = {&layer, &stroke};
            ++count;
        }
    }
    if (count != 1U) {
        return core::Result<ActiveStroke>::failure(unsupported(
            "D3D12 sculpt preview requires exactly one active stroke"));
    }
    const production::SculptStroke& stroke = *selected.stroke;
    if (stroke.mode != production::SculptStrokeMode::add &&
        stroke.mode != production::SculptStrokeMode::subtract &&
        stroke.mode != production::SculptStrokeMode::inflate) {
        return core::Result<ActiveStroke>::failure(unsupported(
            "D3D12 sculpt preview currently supports add, subtract, and inflate strokes"));
    }
    if (stroke.direction_mode != production::SculptStrokeDirectionMode::surface_normal) {
        return core::Result<ActiveStroke>::failure(unsupported(
            "D3D12 sculpt preview currently requires surface-normal direction"));
    }
    if (stroke.samples.empty()) {
        return core::Result<ActiveStroke>::failure(unsupported(
            "D3D12 sculpt preview requires at least one brush sample"));
    }
    return core::Result<ActiveStroke>::success(selected);
}

std::uint32_t symmetry_copy_count(const production::SculptStroke& stroke) {
    if (stroke.symmetry == production::SculptSymmetryMode::radial) {
        return stroke.radial_symmetry_count;
    }
    return stroke.symmetry == production::SculptSymmetryMode::none ? 1U : 2U;
}

core::Vec3d rotate_about_axis(
    core::Vec3d value,
    core::Vec3d unit_axis,
    double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return value * cosine + core::cross(unit_axis, value) * sine +
        unit_axis * (core::dot(unit_axis, value) * (1.0 - cosine));
}

bool within_symmetry_bounds(core::Vec3d value) {
    return value.finite() && std::abs(value.x) <= kMaxSculptPreviewCoordinate &&
        std::abs(value.y) <= kMaxSculptPreviewCoordinate &&
        std::abs(value.z) <= kMaxSculptPreviewCoordinate;
}

struct ExpandedSamples {
    std::uint32_t copies_per_sample = 1U;
    std::vector<KernelSampleCopy> copies;
};

core::Result<ExpandedSamples> expand_samples(const production::SculptStroke& stroke) {
    ExpandedSamples expanded;
    expanded.copies_per_sample = symmetry_copy_count(stroke);
    const std::uint64_t copy_total = static_cast<std::uint64_t>(stroke.samples.size()) *
        expanded.copies_per_sample;
    if (expanded.copies_per_sample == 0U ||
        copy_total > production::kMaxSculptKernelPairs ||
        copy_total > std::numeric_limits<std::size_t>::max()) {
        return core::Result<ExpandedSamples>::failure(unsupported(
            "D3D12 sculpt preview symmetry expansion exceeds its bounded copy count"));
    }
    expanded.copies.reserve(static_cast<std::size_t>(copy_total));

    for (const production::SculptStrokeSample& sample : stroke.samples) {
        if (stroke.symmetry == production::SculptSymmetryMode::none) {
            expanded.copies.push_back({sample.position, sample.pressure});
        } else if (stroke.symmetry == production::SculptSymmetryMode::mirror_x) {
            core::Vec3d mirrored = sample.position;
            mirrored.x = 2.0 * stroke.symmetry_plane_x - mirrored.x;
            expanded.copies.push_back({sample.position, sample.pressure});
            expanded.copies.push_back({mirrored, sample.pressure});
        } else if (stroke.symmetry == production::SculptSymmetryMode::mirror_plane) {
            const core::Vec3d normal = stroke.symmetry_axis_local.normalized();
            const core::Vec3d offset = sample.position - stroke.symmetry_origin_local;
            const core::Vec3d mirrored = sample.position -
                normal * (2.0 * core::dot(offset, normal));
            expanded.copies.push_back({sample.position, sample.pressure});
            expanded.copies.push_back({mirrored, sample.pressure});
        } else if (stroke.symmetry == production::SculptSymmetryMode::radial) {
            const core::Vec3d axis = stroke.symmetry_axis_local.normalized();
            for (std::uint32_t copy_index = 0U;
                 copy_index < expanded.copies_per_sample;
                 ++copy_index) {
                const double angle = 2.0 * std::numbers::pi_v<double> *
                    static_cast<double>(copy_index) /
                    static_cast<double>(expanded.copies_per_sample);
                const core::Vec3d center = copy_index == 0U ? sample.position :
                    stroke.symmetry_origin_local + rotate_about_axis(
                        sample.position - stroke.symmetry_origin_local, axis, angle);
                expanded.copies.push_back({center, sample.pressure});
            }
        } else {
            return core::Result<ExpandedSamples>::failure(unsupported(
                "D3D12 sculpt preview received an unknown symmetry mode"));
        }
    }
    if (std::any_of(expanded.copies.begin(), expanded.copies.end(),
            [](const KernelSampleCopy& copy) {
                return !within_symmetry_bounds(copy.position) ||
                    !std::isfinite(copy.pressure);
            })) {
        return core::Result<ExpandedSamples>::failure(validation(
            "D3D12 sculpt preview symmetry copy is outside finite source bounds"));
    }
    return core::Result<ExpandedSamples>::success(std::move(expanded));
}

assets::Sha256Digest kernel_input_digest(
    const production::BakeMeshBinding& source,
    std::span<const KernelVertex> vertices,
    std::span<const KernelSampleCopy> samples,
    std::uint32_t copies_per_sample) {
    CanonicalBytes canonical;
    canonical.u32(2U);
    canonical.string(source.identity);
    canonical.u64(source.revision.value());
    canonical.digest(source.content_digest);
    canonical.u32(copies_per_sample);
    canonical.u64(static_cast<std::uint64_t>(samples.size()));
    for (const KernelSampleCopy& sample : samples) {
        canonical.f64(sample.position.x);
        canonical.f64(sample.position.y);
        canonical.f64(sample.position.z);
        canonical.f64(sample.pressure);
    }
    canonical.u64(static_cast<std::uint64_t>(vertices.size()));
    for (const KernelVertex& vertex : vertices) {
        canonical.u64(vertex.id.value);
        canonical.f64(vertex.position.x);
        canonical.f64(vertex.position.y);
        canonical.f64(vertex.position.z);
        canonical.f64(vertex.normal.x);
        canonical.f64(vertex.normal.y);
        canonical.f64(vertex.normal.z);
        canonical.f64(vertex.mask);
    }
    return assets::sha256(canonical.bytes);
}

core::Result<PreparedPreview> prepare_preview(
    const production::SculptStack& stack,
    const geometry::EditableMesh& base_mesh) {
    if (auto result = stack.validate(); !result) {
        return core::Result<PreparedPreview>::failure(result.error());
    }
    if (base_mesh.vertex_count() == 0U ||
        base_mesh.vertex_count() > production::kMaxSculptVertices) {
        return core::Result<PreparedPreview>::failure(validation(
            "D3D12 sculpt preview vertex count is outside the supported bound"));
    }
    const auto current = production::bind_bake_source(stack.source.identity, base_mesh);
    if (!current) return core::Result<PreparedPreview>::failure(current.error());
    if (current.value() != stack.source) {
        return core::Result<PreparedPreview>::failure(stale(
            "D3D12 sculpt preview source revision or content digest is stale"));
    }
    const auto active = find_active_stroke(stack);
    if (!active) return core::Result<PreparedPreview>::failure(active.error());
    if (auto result = active.value().stroke->validate(); !result) {
        return core::Result<PreparedPreview>::failure(result.error());
    }
    const std::uint64_t sample_count = static_cast<std::uint64_t>(
        active.value().stroke->samples.size());
    const std::uint32_t copies_per_sample = symmetry_copy_count(*active.value().stroke);
    if (sample_count > production::kMaxSculptSamplesPerStroke ||
        copies_per_sample == 0U ||
        sample_count > production::kMaxSculptKernelPairs / copies_per_sample) {
        return core::Result<PreparedPreview>::failure(unsupported(
            "D3D12 sculpt preview exceeds its bounded symmetry-copy work budget"));
    }
    const std::uint64_t sample_copy_count = sample_count * copies_per_sample;
    if (static_cast<std::uint64_t>(base_mesh.vertex_count()) >
        production::kMaxSculptKernelPairs / sample_copy_count) {
        return core::Result<PreparedPreview>::failure(unsupported(
            "D3D12 sculpt preview exceeds its bounded vertex-symmetry work budget"));
    }
    const auto expanded = expand_samples(*active.value().stroke);
    if (!expanded) return core::Result<PreparedPreview>::failure(expanded.error());
    const auto settings = stack.digest();
    if (!settings) return core::Result<PreparedPreview>::failure(settings.error());

    const std::vector<geometry::Vertex> vertices = base_mesh.vertices_sorted();
    const auto masks = read_vertex_mask(
        base_mesh, active.value().stroke->mask_layer_id, vertices);
    if (!masks) return core::Result<PreparedPreview>::failure(masks.error());
    const auto normals = vertex_normals(base_mesh);

    PreparedPreview prepared;
    prepared.active = active.value();
    prepared.source = stack.source;
    prepared.settings_digest = settings.value();
    prepared.copies_per_sample = expanded.value().copies_per_sample;
    prepared.samples = expanded.value().copies;
    prepared.vertices.reserve(vertices.size());
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        const auto normal = normals.find(vertices[index].id);
        if (normal == normals.end()) {
            return core::Result<PreparedPreview>::failure(validation(
                "D3D12 sculpt preview could not resolve a source vertex normal"));
        }
        prepared.vertices.push_back(KernelVertex{
            vertices[index].id,
            vertices[index].position,
            normal->second,
            masks.value()[index],
        });
    }
    prepared.kernel_input_digest = kernel_input_digest(prepared.source,
        prepared.vertices, prepared.samples, prepared.copies_per_sample);
    return core::Result<PreparedPreview>::success(std::move(prepared));
}

bool finite_float(double value) noexcept {
    return std::isfinite(value) &&
        value >= -static_cast<double>(std::numeric_limits<float>::max()) &&
        value <= static_cast<double>(std::numeric_limits<float>::max()) &&
        std::isfinite(static_cast<float>(value));
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    const std::uint64_t remainder = value % alignment;
    return remainder == 0U ? value : value + alignment - remainder;
}

struct UploadTexture {
    gpu::TextureDesc descriptor;
    std::uint32_t row_pitch = 0U;
    std::uint64_t offset = 0U;
};

struct PackedPreviewInputs {
    UploadTexture vertices;
    UploadTexture normals;
    UploadTexture samples;
    std::vector<std::uint8_t> staging;
    std::uint32_t vertex_width = 0U;
    std::uint32_t vertex_height = 0U;
    std::uint32_t sample_width = 0U;
    std::uint32_t sample_height = 0U;
};

core::Result<PackedPreviewInputs> pack_inputs(const PreparedPreview& prepared) {
    PackedPreviewInputs packed;
    packed.vertex_width = std::min(
        kTextureRowWidth, static_cast<std::uint32_t>(prepared.vertices.size()));
    packed.vertex_height = static_cast<std::uint32_t>(
        (prepared.vertices.size() + packed.vertex_width - 1U) / packed.vertex_width);
    const std::uint32_t sample_record_count = static_cast<std::uint32_t>(
        prepared.samples.size());
    packed.sample_width = std::min(kTextureRowWidth, sample_record_count);
    packed.sample_height = static_cast<std::uint32_t>(
        (prepared.samples.size() + packed.sample_width - 1U) / packed.sample_width);
    packed.vertices.descriptor = gpu::TextureDesc{
        packed.vertex_width, packed.vertex_height, 1U, 1U, 1U,
        gpu::Format::rgba32_float, gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
    };
    packed.normals.descriptor = packed.vertices.descriptor;
    packed.samples.descriptor = gpu::TextureDesc{
        packed.sample_width, packed.sample_height, 1U, 1U, 1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
    };

    std::uint64_t cursor = 0U;
    const auto plan_texture = [&cursor](UploadTexture& texture) -> bool {
        const std::uint64_t row_bytes =
            static_cast<std::uint64_t>(texture.descriptor.width) * 4U * sizeof(float);
        const std::uint64_t row_pitch = align_up(row_bytes, kTexturePitchAlignment);
        const std::uint64_t offset = align_up(cursor, kTexturePlacementAlignment);
        if (row_pitch > std::numeric_limits<std::uint32_t>::max() ||
            offset > kMaxSculptPreviewUploadBytes ||
            row_pitch * texture.descriptor.height >
                kMaxSculptPreviewUploadBytes - offset) {
            return false;
        }
        texture.row_pitch = static_cast<std::uint32_t>(row_pitch);
        texture.offset = offset;
        cursor = offset + row_pitch * texture.descriptor.height;
        return true;
    };
    if (!plan_texture(packed.vertices) || !plan_texture(packed.normals) ||
        !plan_texture(packed.samples)) {
        return core::Result<PackedPreviewInputs>::failure(unsupported(
            "D3D12 sculpt preview input staging exceeds its bounded upload budget"));
    }
    packed.staging.assign(static_cast<std::size_t>(cursor), 0U);

    const auto write_texel = [&packed](
        const UploadTexture& texture,
        std::uint32_t x,
        std::uint32_t y,
        const std::array<float, 4U>& value) {
        const std::uint64_t offset = texture.offset +
            static_cast<std::uint64_t>(texture.row_pitch) * y +
            static_cast<std::uint64_t>(x) * 4U * sizeof(float);
        std::memcpy(packed.staging.data() + static_cast<std::size_t>(offset),
            value.data(), sizeof(value));
    };
    for (std::size_t index = 0U; index < prepared.vertices.size(); ++index) {
        const KernelVertex& vertex = prepared.vertices[index];
        const std::uint32_t x = static_cast<std::uint32_t>(index) % packed.vertex_width;
        const std::uint32_t y = static_cast<std::uint32_t>(index) / packed.vertex_width;
        const std::array<double, 7U> values{
            vertex.position.x, vertex.position.y, vertex.position.z,
            vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.mask,
        };
        if (!std::all_of(values.begin(), values.end(), finite_float)) {
            return core::Result<PackedPreviewInputs>::failure(unsupported(
                "D3D12 sculpt preview input cannot be represented as finite float32"));
        }
        write_texel(packed.vertices, x, y, {
            static_cast<float>(vertex.position.x),
            static_cast<float>(vertex.position.y),
            static_cast<float>(vertex.position.z),
            static_cast<float>(vertex.mask),
        });
        write_texel(packed.normals, x, y, {
            static_cast<float>(vertex.normal.x),
            static_cast<float>(vertex.normal.y),
            static_cast<float>(vertex.normal.z), 0.0F,
        });
    }
    for (std::size_t index = 0U; index < prepared.samples.size(); ++index) {
        const KernelSampleCopy& sample = prepared.samples[index];
        const std::array<double, 4U> values{
            sample.position.x, sample.position.y, sample.position.z, sample.pressure,
        };
        if (!std::all_of(values.begin(), values.end(), finite_float)) {
            return core::Result<PackedPreviewInputs>::failure(unsupported(
                "D3D12 sculpt preview sample cannot be represented as finite float32"));
        }
        const std::uint32_t x = static_cast<std::uint32_t>(index) % packed.sample_width;
        const std::uint32_t y = static_cast<std::uint32_t>(index) / packed.sample_width;
        write_texel(packed.samples, x, y, {
            static_cast<float>(sample.position.x),
            static_cast<float>(sample.position.y),
            static_cast<float>(sample.position.z),
            static_cast<float>(sample.pressure),
        });
    }
    return core::Result<PackedPreviewInputs>::success(std::move(packed));
}

class DeviceResourceGuard final {
public:
    explicit DeviceResourceGuard(D3D12Device& device) : device_(device) {}
    ~DeviceResourceGuard() { static_cast<void>(cleanup()); }

    DeviceResourceGuard(const DeviceResourceGuard&) = delete;
    DeviceResourceGuard& operator=(const DeviceResourceGuard&) = delete;

    void set_pending(gpu::SubmissionSerial serial) noexcept { pending_ = serial; }
    void clear_pending() noexcept { pending_.reset(); }

    core::Result<void> cleanup() {
        std::optional<core::Diagnostic> first_error;
        const auto retain = [&first_error](const core::Result<void>& result) {
            if (!result && !first_error.has_value()) first_error = result.error();
        };
        bool idle = true;
        if (pending_.has_value()) {
            const auto waited = device_.wait(*pending_);
            if (!waited) {
                retain(waited);
                const auto idle_result = device_.wait_idle();
                retain(idle_result);
                idle = static_cast<bool>(idle_result);
            }
            pending_.reset();
        } else {
            const auto idle_result = device_.wait_idle();
            retain(idle_result);
            idle = static_cast<bool>(idle_result);
        }
        if (!idle) return core::Result<void>::failure(std::move(*first_error));
        if (pipeline) {
            retain(device_.destroy_pipeline(pipeline));
            pipeline = {};
        }
        if (shader) {
            retain(device_.destroy_shader(shader));
            shader = {};
        }
        if (upload_buffer) {
            retain(device_.destroy_buffer(upload_buffer));
            upload_buffer = {};
        }
        for (gpu::TextureHandle& texture : textures) {
            if (texture) {
                retain(device_.destroy_texture(texture));
                texture = {};
            }
        }
        if (first_error.has_value()) {
            return core::Result<void>::failure(std::move(*first_error));
        }
        return core::Result<void>::success();
    }

    gpu::BufferHandle upload_buffer;
    std::array<gpu::TextureHandle, 4U> textures{};
    gpu::ShaderHandle shader;
    gpu::PipelineHandle pipeline;

private:
    D3D12Device& device_;
    std::optional<gpu::SubmissionSerial> pending_;
};

struct SculptPreviewConstants {
    std::uint32_t vertex_count = 0U;
    std::uint32_t vertex_width = 0U;
    std::uint32_t sample_count = 0U;
    std::uint32_t sample_copies_per_stroke_sample = 0U;
    std::uint32_t sample_record_width = 0U;
    std::uint32_t subtract_mode = 0U;
    std::uint32_t reserved0 = 0U;
    std::uint32_t reserved1 = 0U;
    float radius = 0.0F;
    float strength = 0.0F;
    float layer_weight = 0.0F;
    std::uint32_t reserved2 = 0U;
};
static_assert(sizeof(SculptPreviewConstants) == 48U);

double reference_error_tolerance(const core::Vec3d& reference) noexcept {
    const double scale = std::max({
        1.0, std::abs(reference.x), std::abs(reference.y), std::abs(reference.z)});
    return kD3D12SculptPreviewAbsoluteTolerance +
        kD3D12SculptPreviewRelativeTolerance * scale;
}

core::Result<void> validate_product_against_reference(
    const D3D12SculptPreviewArtifact& artifact,
    const production::SculptStack& stack,
    const PreparedPreview& prepared,
    const production::SculptEvaluationArtifact& reference) {
    if (artifact.project_namespace != stack.project_namespace ||
        artifact.mesh_asset != stack.mesh_asset || artifact.source != stack.source ||
        artifact.stroke_identity != prepared.active.stroke->identity ||
        artifact.settings_digest != prepared.settings_digest ||
        artifact.kernel_input_digest != prepared.kernel_input_digest ||
        artifact.positions.size() != reference.vertices.size() ||
        artifact.positions.size() != prepared.vertices.size() ||
        artifact.algorithm != kD3D12SculptPreviewAlgorithm ||
        artifact.evidence_class != kD3D12SculptPreviewEvidenceClass ||
        !artifact.non_authoritative) {
        return core::Result<void>::failure(validation(
            "D3D12 sculpt preview provenance or evidence class is invalid"));
    }

    double maximum_displacement = 0.0;
    double maximum_reference_error = 0.0;
    double maximum_reference_error_tolerance = 0.0;
    for (std::size_t index = 0U; index < artifact.positions.size(); ++index) {
        const SculptPreviewPosition& output = artifact.positions[index];
        const KernelVertex& source = prepared.vertices[index];
        const geometry::Vertex& expected = reference.vertices[index];
        if (!output.vertex || output.vertex != source.id || output.vertex != expected.id ||
            !output.position.finite()) {
            return core::Result<void>::failure(validation(
                "D3D12 sculpt preview output identities or positions are invalid"));
        }
        const core::Vec3d displacement = output.position - source.position;
        const core::Vec3d error = output.position - expected.position;
        const double displacement_length = std::sqrt(displacement.length_squared());
        const double error_length = std::sqrt(error.length_squared());
        const double tolerance = reference_error_tolerance(expected.position);
        if (!std::isfinite(displacement_length) || !std::isfinite(error_length) ||
            error_length > tolerance) {
            return core::Result<void>::failure(validation(
                "D3D12 sculpt preview exceeds its declared CPU-reference tolerance"));
        }
        maximum_displacement = std::max(maximum_displacement, displacement_length);
        maximum_reference_error = std::max(maximum_reference_error, error_length);
        maximum_reference_error_tolerance = std::max(
            maximum_reference_error_tolerance, tolerance);
    }
    if (artifact.maximum_displacement != maximum_displacement ||
        artifact.maximum_reference_error != maximum_reference_error ||
        artifact.maximum_reference_error_tolerance != maximum_reference_error_tolerance) {
        return core::Result<void>::failure(validation(
            "D3D12 sculpt preview quality metrics do not match its output"));
    }
    auto rebuilt = artifact;
    if (auto result = rebuilt.refresh_digests(); !result) return result;
    if (rebuilt.byte_size != artifact.byte_size ||
        rebuilt.output_digest != artifact.output_digest ||
        rebuilt.artifact_digest != artifact.artifact_digest) {
        return core::Result<void>::failure(validation(
            "D3D12 sculpt preview digest or byte count does not match its contents"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> D3D12SculptPreviewArtifact::refresh_digests() {
    if (project_namespace.empty() || mesh_asset == 0U ||
        !source.validate() || stroke_identity.empty() ||
        algorithm != kD3D12SculptPreviewAlgorithm ||
        evidence_class != kD3D12SculptPreviewEvidenceClass ||
        settings_digest.is_zero() || kernel_input_digest.is_zero() ||
        positions.empty() || positions.size() > production::kMaxSculptVertices ||
        !std::isfinite(maximum_displacement) || maximum_displacement < 0.0 ||
        !std::isfinite(maximum_reference_error) || maximum_reference_error < 0.0 ||
        !std::isfinite(maximum_reference_error_tolerance) ||
        maximum_reference_error_tolerance <= 0.0 ||
        maximum_reference_error > maximum_reference_error_tolerance ||
        !non_authoritative) {
        return core::Result<void>::failure(validation(
            "D3D12 sculpt preview artifact metadata is invalid"));
    }
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        if (!positions[index].vertex || !positions[index].position.finite() ||
            (index != 0U && positions[index - 1U].vertex.value >=
                positions[index].vertex.value)) {
            return core::Result<void>::failure(validation(
                "D3D12 sculpt preview positions must have sorted unique identities"));
        }
    }

    byte_size = static_cast<std::uint64_t>(positions.size()) * 32ULL;
    CanonicalBytes output;
    output.u32(1U);
    output.u64(static_cast<std::uint64_t>(positions.size()));
    for (const SculptPreviewPosition& position : positions) {
        output.u64(position.vertex.value);
        output.f64(position.position.x);
        output.f64(position.position.y);
        output.f64(position.position.z);
    }
    output_digest = assets::sha256(output.bytes);

    CanonicalBytes artifact;
    artifact.u32(1U);
    artifact.string(project_namespace);
    artifact.u64(mesh_asset);
    artifact.string(source.identity);
    artifact.u64(source.revision.value());
    artifact.digest(source.content_digest);
    artifact.string(stroke_identity);
    artifact.string(algorithm);
    artifact.string(evidence_class);
    artifact.digest(settings_digest);
    artifact.digest(kernel_input_digest);
    artifact.digest(output_digest);
    artifact.u64(byte_size);
    artifact.f64(maximum_displacement);
    artifact.f64(maximum_reference_error);
    artifact.f64(maximum_reference_error_tolerance);
    artifact.u8(non_authoritative ? 1U : 0U);
    artifact_digest = assets::sha256(artifact.bytes);
    return core::Result<void>::success();
}

core::Result<void> D3D12SculptPreviewArtifact::validate(
    const production::SculptStack& stack,
    const geometry::EditableMesh& base_mesh) const {
    const auto prepared = prepare_preview(stack, base_mesh);
    if (!prepared) return core::Result<void>::failure(prepared.error());
    const auto reference = production::compile_sculpt_stack(stack, base_mesh);
    if (!reference) return core::Result<void>::failure(reference.error());
    return validate_product_against_reference(
        *this, stack, prepared.value(), reference.value());
}

core::Result<D3D12SculptPreviewArtifact> run_d3d12_sculpt_preview(
    D3D12Device& device,
    const production::SculptStack& stack,
    const geometry::EditableMesh& base_mesh) {
    const auto prepared_result = prepare_preview(stack, base_mesh);
    if (!prepared_result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(
            prepared_result.error());
    }
    PreparedPreview prepared = prepared_result.value();
    const auto reference = production::compile_sculpt_stack(stack, base_mesh);
    if (!reference) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(reference.error());
    }
    if (!finite_float(prepared.active.stroke->radius) ||
        static_cast<float>(prepared.active.stroke->radius) <= 0.0F ||
        !finite_float(prepared.active.stroke->strength) ||
        !finite_float(prepared.active.layer->weight)) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(unsupported(
            "D3D12 sculpt preview settings cannot be represented as finite float32"));
    }
    const auto packed_result = pack_inputs(prepared);
    if (!packed_result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(packed_result.error());
    }
    PackedPreviewInputs packed = packed_result.value();

    DeviceResourceGuard resources(device);
    const auto upload = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(packed.staging.size()),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    if (!upload) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(upload.error());
    }
    resources.upload_buffer = upload.value();
    if (auto result = device.write_buffer(
            resources.upload_buffer, 0U, packed.staging); !result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(result.error());
    }
    const std::array<UploadTexture, 3U> inputs{
        packed.vertices, packed.normals, packed.samples,
    };
    for (std::size_t index = 0U; index < inputs.size(); ++index) {
        const auto texture = device.create_texture(inputs[index].descriptor);
        if (!texture) {
            return core::Result<D3D12SculptPreviewArtifact>::failure(texture.error());
        }
        resources.textures[index] = texture.value();
    }
    const auto output = device.create_texture(gpu::TextureDesc{
        packed.vertex_width, packed.vertex_height, 1U, 1U, 1U,
        gpu::Format::rgba32_float, gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    if (!output) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(output.error());
    }
    resources.textures[3U] = output.value();

    device_ir::DeviceCommandStream upload_stream;
    upload_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{resources.upload_buffer},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    for (std::size_t index = 0U; index < inputs.size(); ++index) {
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.textures[index]},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_destination,
        });
        upload_stream.append(device_ir::CmdCopyBufferToTexture{
            resources.upload_buffer, resources.textures[index], inputs[index].offset, 0U});
    }
    const auto upload_serial = device.submit(upload_stream, gpu::QueueType::copy);
    if (!upload_serial) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(upload_serial.error());
    }
    resources.set_pending(upload_serial.value());
    if (auto result = device.wait(upload_serial.value()); !result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(result.error());
    }
    resources.clear_pending();

    const auto shader_binary = compile_hlsl(ShaderSource{
        "carto_d3d12_sculpt_preview_v2",
        gpu::ShaderStage::compute,
        "main",
        "carto_d3d12_sculpt_preview_v2.hlsl",
        std::string(kSculptPreviewShader),
        true,
        false,
    });
    if (!shader_binary) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(shader_binary.error());
    }
    const auto shader = device.create_shader(shader_binary.value());
    if (!shader) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(shader.error());
    }
    resources.shader = shader.value();
    gpu::PipelineDesc pipeline_descriptor;
    pipeline_descriptor.debug_name = "carto_d3d12_sculpt_preview_v2";
    pipeline_descriptor.shaders = {resources.shader};
    pipeline_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::compute},
    };
    pipeline_descriptor.sampled_texture_count = 3U;
    pipeline_descriptor.depth_test = false;
    pipeline_descriptor.push_constant_bytes = sizeof(SculptPreviewConstants);
    const auto pipeline = device.create_pipeline(pipeline_descriptor);
    if (!pipeline) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(pipeline.error());
    }
    resources.pipeline = pipeline.value();

    device_ir::DeviceCommandStream compute_stream;
    for (std::size_t index = 0U; index < inputs.size(); ++index) {
        compute_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.textures[index]},
            device_ir::ResourceState::copy_destination,
            device_ir::ResourceState::shader_read,
        });
    }
    compute_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{resources.textures[3U]},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    compute_stream.append(device_ir::CmdBindPipeline{resources.pipeline});
    compute_stream.append(device_ir::CmdBindResources{{
        {0U, 0U, device_ir::ResourceHandle{resources.textures[0U]}, std::nullopt},
        {0U, 1U, device_ir::ResourceHandle{resources.textures[1U]}, std::nullopt},
        {0U, 2U, device_ir::ResourceHandle{resources.textures[2U]}, std::nullopt},
        {0U, 3U, device_ir::ResourceHandle{resources.textures[3U]}, std::nullopt},
    }});
    const SculptPreviewConstants constants{
        static_cast<std::uint32_t>(prepared.vertices.size()),
        packed.vertex_width,
        static_cast<std::uint32_t>(prepared.active.stroke->samples.size()),
        prepared.copies_per_sample,
        packed.sample_width,
        prepared.active.stroke->mode == production::SculptStrokeMode::subtract ? 1U : 0U,
        0U,
        0U,
        static_cast<float>(prepared.active.stroke->radius),
        static_cast<float>(prepared.active.stroke->strength),
        static_cast<float>(prepared.active.layer->weight),
        0U,
    };
    std::vector<std::uint8_t> constant_bytes(sizeof(constants));
    std::memcpy(constant_bytes.data(), &constants, sizeof(constants));
    compute_stream.append(device_ir::CmdPushConstants{std::move(constant_bytes)});
    compute_stream.append(device_ir::CmdDispatch{
        (constants.vertex_count + kComputeGroupWidth - 1U) / kComputeGroupWidth,
        1U,
        1U,
    });
    compute_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{resources.textures[3U]}});
    const auto compute_serial = device.submit(compute_stream, gpu::QueueType::compute);
    if (!compute_serial) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(compute_serial.error());
    }
    resources.set_pending(compute_serial.value());
    if (auto result = device.wait(compute_serial.value()); !result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(result.error());
    }
    resources.clear_pending();

    const auto pixels = device.read_texture_rgba32f(resources.textures[3U]);
    if (!pixels) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(pixels.error());
    }
    const std::size_t pixel_count = static_cast<std::size_t>(
        packed.vertex_width) * packed.vertex_height * 4U;
    if (pixels.value().size() != pixel_count) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(validation(
            "D3D12 sculpt preview readback returned a non-canonical texel count"));
    }

    D3D12SculptPreviewArtifact artifact;
    artifact.project_namespace = stack.project_namespace;
    artifact.mesh_asset = stack.mesh_asset;
    artifact.source = prepared.source;
    artifact.stroke_identity = prepared.active.stroke->identity;
    artifact.settings_digest = prepared.settings_digest;
    artifact.kernel_input_digest = prepared.kernel_input_digest;
    artifact.maximum_displacement = 0.0;
    artifact.maximum_reference_error = 0.0;
    artifact.maximum_reference_error_tolerance = 0.0;
    artifact.positions.reserve(prepared.vertices.size());
    for (std::size_t index = 0U; index < prepared.vertices.size(); ++index) {
        const std::uint32_t x = static_cast<std::uint32_t>(index) % packed.vertex_width;
        const std::uint32_t y = static_cast<std::uint32_t>(index) / packed.vertex_width;
        const std::size_t offset =
            (static_cast<std::size_t>(y) * packed.vertex_width + x) * 4U;
        const core::Vec3d position{
            pixels.value()[offset], pixels.value()[offset + 1U],
            pixels.value()[offset + 2U],
        };
        artifact.positions.push_back({prepared.vertices[index].id, position});
    }
    const auto comparison = [&]() -> core::Result<void> {
        if (artifact.positions.size() != reference.value().vertices.size()) {
            return core::Result<void>::failure(validation(
                "D3D12 sculpt preview output count differs from the CPU reference"));
        }
        for (std::size_t index = 0U; index < artifact.positions.size(); ++index) {
            const SculptPreviewPosition& actual = artifact.positions[index];
            const KernelVertex& source = prepared.vertices[index];
            const geometry::Vertex& expected = reference.value().vertices[index];
            if (actual.vertex != source.id || actual.vertex != expected.id ||
                !actual.position.finite()) {
                return core::Result<void>::failure(validation(
                    "D3D12 sculpt preview output contains invalid identity or position data"));
            }
            const core::Vec3d displacement = actual.position - source.position;
            const core::Vec3d error = actual.position - expected.position;
            const double displacement_length = std::sqrt(displacement.length_squared());
            const double error_length = std::sqrt(error.length_squared());
            const double tolerance = reference_error_tolerance(expected.position);
            if (!std::isfinite(displacement_length) || !std::isfinite(error_length) ||
                error_length > tolerance) {
                return core::Result<void>::failure(validation(
                    "D3D12 sculpt preview exceeds its declared CPU-reference tolerance"));
            }
            artifact.maximum_displacement = std::max(
                artifact.maximum_displacement, displacement_length);
            artifact.maximum_reference_error = std::max(
                artifact.maximum_reference_error, error_length);
            artifact.maximum_reference_error_tolerance = std::max(
                artifact.maximum_reference_error_tolerance, tolerance);
        }
        return core::Result<void>::success();
    }();
    if (!comparison) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(comparison.error());
    }
    if (auto result = artifact.refresh_digests(); !result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(result.error());
    }
    if (auto result = validate_product_against_reference(
            artifact, stack, prepared, reference.value()); !result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(result.error());
    }
    if (auto result = resources.cleanup(); !result) {
        return core::Result<D3D12SculptPreviewArtifact>::failure(result.error());
    }
    return core::Result<D3D12SculptPreviewArtifact>::success(std::move(artifact));
}

} // namespace carto::d3d12
