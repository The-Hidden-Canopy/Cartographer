#include <carto/d3d12/projection_worker.hpp>

#include <carto/device_ir/command.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::d3d12 {
namespace {

constexpr std::uint32_t kRecordsPerInputRow = 256U;
constexpr std::uint32_t kOutputRowWidth = 2048U;
constexpr std::uint32_t kThreadGroupWidth = 64U;
constexpr std::uint64_t kTexturePitchAlignment = 256U;
constexpr std::uint64_t kTexturePlacementAlignment = 512U;

constexpr std::string_view kProjectionShader = R"hlsl(
Texture2D<float4> ray_records : register(t0);
Texture2D<float4> triangle_records : register(t1);
RWTexture2D<float4> output_hits : register(u0);

cbuffer ProjectionConstants : register(b0) {
    uint ray_count;
    uint triangle_count;
    uint ray_records_per_row;
    uint triangle_records_per_row;
    uint output_width;
    uint backface_policy;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint ray_index = dispatch_id.y * output_width + dispatch_id.x;
    if (dispatch_id.x >= output_width || ray_index >= ray_count) {
        return;
    }

    uint ray_row = ray_index / ray_records_per_row;
    uint ray_x = (ray_index % ray_records_per_row) * 2u;
    float4 ray_origin_and_limit = ray_records.Load(int3(ray_x, ray_row, 0));
    float3 direction = ray_records.Load(int3(ray_x + 1u, ray_row, 0)).xyz;
    float3 ray_origin = ray_origin_and_limit.xyz;
    float maximum_distance = ray_origin_and_limit.w;

    uint best_triangle = 0xffffffffu;
    float best_distance = maximum_distance;
    float best_u = 0.0f;
    float best_v = 0.0f;
    bool best_backface = false;
    uint rejected_triangle = 0xffffffffu;
    float rejected_distance = maximum_distance;
    float rejected_u = 0.0f;
    float rejected_v = 0.0f;

    for (uint triangle_index = 0u; triangle_index < triangle_count; ++triangle_index) {
        uint triangle_row = triangle_index / triangle_records_per_row;
        uint triangle_x = (triangle_index % triangle_records_per_row) * 3u;
        float3 v0 = triangle_records.Load(int3(triangle_x, triangle_row, 0)).xyz;
        float3 v1 = triangle_records.Load(int3(triangle_x + 1u, triangle_row, 0)).xyz;
        float3 v2 = triangle_records.Load(int3(triangle_x + 2u, triangle_row, 0)).xyz;

        float3 edge1 = v1 - v0;
        float3 edge2 = v2 - v0;
        float3 p = cross(direction, edge2);
        float determinant = dot(edge1, p);
        float determinant_scale = length(edge1) * length(edge2);
        float determinant_epsilon = max(determinant_scale * 1.0e-7f, 1.0e-30f);
        bool rejected_backface = backface_policy == 0u &&
            determinant < -determinant_epsilon;
        if (backface_policy == 0u) {
            if (determinant <= determinant_epsilon && !rejected_backface) {
                continue;
            }
        } else if (abs(determinant) <= determinant_epsilon) {
            continue;
        }

        float inverse_determinant = 1.0f / determinant;
        float3 from_v0 = ray_origin - v0;
        float u = dot(from_v0, p) * inverse_determinant;
        if (u < 0.0f || u > 1.0f) {
            continue;
        }
        float3 q = cross(from_v0, edge1);
        float v = dot(direction, q) * inverse_determinant;
        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }
        float distance = dot(edge2, q) * inverse_determinant;
        if (distance <= 0.0f || distance > maximum_distance) {
            continue;
        }
        if (rejected_backface) {
            if (rejected_triangle == 0xffffffffu || distance < rejected_distance) {
                rejected_triangle = triangle_index;
                rejected_distance = distance;
                rejected_u = u;
                rejected_v = v;
            }
            continue;
        }
        // Strict comparison preserves the first triangle in stable source order
        // when two hits have exactly the same float distance.
        if (best_triangle == 0xffffffffu || distance < best_distance) {
            best_triangle = triangle_index;
            best_distance = distance;
            best_u = u;
            best_v = v;
            best_backface = determinant < 0.0f;
        }
    }

    uint2 output_coordinate = uint2(dispatch_id.x * 2u, dispatch_id.y);
    uint2 rejected_coordinate = uint2(dispatch_id.x * 2u + 1u, dispatch_id.y);
    if (best_triangle == 0xffffffffu) {
        output_hits[output_coordinate] = float4(-1.0f, -1.0f, 0.0f, 0.0f);
    } else {
        output_hits[output_coordinate] = float4(
            best_backface ? -(float)(best_triangle + 2u) : (float)best_triangle,
            best_distance,
            best_u,
            best_v);
    }
    if (rejected_triangle == 0xffffffffu) {
        output_hits[rejected_coordinate] = float4(-1.0f, -1.0f, 0.0f, 0.0f);
    } else {
        output_hits[rejected_coordinate] = float4(
            (float)rejected_triangle, rejected_distance, rejected_u, rejected_v);
    }
}
)hlsl";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool finite_float(double value) noexcept {
    if (!std::isfinite(value) ||
        value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    return std::isfinite(static_cast<float>(value));
}

struct CanonicalBytes {
    std::vector<std::uint8_t> bytes;

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
    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }

    void digest(const assets::Sha256Digest& value) {
        bytes.insert(bytes.end(), value.bytes.begin(), value.bytes.end());
    }
};

std::span<const std::uint8_t> as_bytes(std::string_view value) {
    return {
        reinterpret_cast<const std::uint8_t*>(value.data()),
        value.size(),
    };
}

assets::Sha256Digest request_digest(const D3D12ProjectionRequest& request) {
    CanonicalBytes canonical;
    canonical.u32(kD3D12ProjectionWorkerVersion);
    canonical.digest(request.source_digest);
    canonical.digest(request.target_digest);
    canonical.digest(request.settings_digest);
    canonical.u8(static_cast<std::uint8_t>(request.backface_policy));
    canonical.u64(request.maximum_candidate_pairs);
    canonical.u64(request.triangles.size());
    for (const ProjectionTriangle& triangle : request.triangles) {
        canonical.u64(triangle.source_face_id);
        canonical.u32(triangle.fan_index);
        canonical.f64(triangle.v0.x);
        canonical.f64(triangle.v0.y);
        canonical.f64(triangle.v0.z);
        canonical.f64(triangle.v1.x);
        canonical.f64(triangle.v1.y);
        canonical.f64(triangle.v1.z);
        canonical.f64(triangle.v2.x);
        canonical.f64(triangle.v2.y);
        canonical.f64(triangle.v2.z);
    }
    canonical.u64(request.rays.size());
    for (const ProjectionRay& ray : request.rays) {
        canonical.u64(ray.target_face_id);
        canonical.u32(ray.texel_x);
        canonical.u32(ray.texel_y);
        canonical.u32(ray.sample_index);
        canonical.f64(ray.origin.x);
        canonical.f64(ray.origin.y);
        canonical.f64(ray.origin.z);
        canonical.f64(ray.direction.x);
        canonical.f64(ray.direction.y);
        canonical.f64(ray.direction.z);
        canonical.f64(ray.maximum_distance);
    }
    return assets::sha256(canonical.bytes);
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0U) return value;
    const std::uint64_t adjustment = alignment - remainder;
    if (value > std::numeric_limits<std::uint64_t>::max() - adjustment) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return value + adjustment;
}

struct PackedInputs {
    gpu::TextureDesc rays_texture;
    gpu::TextureDesc triangles_texture;
    std::uint32_t ray_row_pitch = 0U;
    std::uint32_t triangle_row_pitch = 0U;
    std::uint64_t triangle_offset = 0U;
    std::vector<std::uint8_t> staging;
};

core::Result<PackedInputs> pack_inputs(const D3D12ProjectionRequest& request) {
    PackedInputs packed;
    const std::uint32_t ray_rows = static_cast<std::uint32_t>(
        (request.rays.size() + kRecordsPerInputRow - 1U) / kRecordsPerInputRow);
    const std::uint32_t triangle_rows = static_cast<std::uint32_t>(
        (request.triangles.size() + kRecordsPerInputRow - 1U) /
        kRecordsPerInputRow);
    packed.rays_texture = gpu::TextureDesc{
        kRecordsPerInputRow * 2U,
        ray_rows,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
    };
    packed.triangles_texture = gpu::TextureDesc{
        kRecordsPerInputRow * 3U,
        triangle_rows,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
    };

    const std::uint64_t ray_row_bytes =
        static_cast<std::uint64_t>(packed.rays_texture.width) * 4U * sizeof(float);
    const std::uint64_t triangle_row_bytes =
        static_cast<std::uint64_t>(packed.triangles_texture.width) * 4U * sizeof(float);
    const std::uint64_t ray_pitch = align_up(ray_row_bytes, kTexturePitchAlignment);
    const std::uint64_t triangle_pitch =
        align_up(triangle_row_bytes, kTexturePitchAlignment);
    const std::uint64_t ray_bytes = ray_pitch * ray_rows;
    packed.triangle_offset = align_up(ray_bytes, kTexturePlacementAlignment);
    const std::uint64_t triangle_bytes = triangle_pitch * triangle_rows;
    if (ray_pitch > std::numeric_limits<std::uint32_t>::max() ||
        triangle_pitch > std::numeric_limits<std::uint32_t>::max() ||
        packed.triangle_offset > kMaxD3D12ProjectionUploadBytes ||
        triangle_bytes > kMaxD3D12ProjectionUploadBytes - packed.triangle_offset ||
        packed.triangle_offset + triangle_bytes >
            std::numeric_limits<std::size_t>::max()) {
        return core::Result<PackedInputs>::failure(validation(
            "D3D12 projection input staging exceeds the bounded upload budget"));
    }
    packed.ray_row_pitch = static_cast<std::uint32_t>(ray_pitch);
    packed.triangle_row_pitch = static_cast<std::uint32_t>(triangle_pitch);
    packed.staging.assign(
        static_cast<std::size_t>(packed.triangle_offset + triangle_bytes), 0U);

    const auto write_texel = [&packed](
        std::uint64_t base,
        std::uint32_t row_pitch,
        std::uint32_t x,
        std::uint32_t y,
        std::array<float, 4U> value) {
        const std::uint64_t offset = base +
            static_cast<std::uint64_t>(row_pitch) * y +
            static_cast<std::uint64_t>(x) * 4U * sizeof(float);
        std::memcpy(packed.staging.data() + static_cast<std::size_t>(offset),
                    value.data(), sizeof(value));
    };

    for (std::size_t index = 0U; index < request.rays.size(); ++index) {
        const ProjectionRay& ray = request.rays[index];
        const std::uint32_t row = static_cast<std::uint32_t>(
            index / kRecordsPerInputRow);
        const std::uint32_t x = static_cast<std::uint32_t>(
            index % kRecordsPerInputRow) * 2U;
        write_texel(0U, packed.ray_row_pitch, x, row, {
            static_cast<float>(ray.origin.x),
            static_cast<float>(ray.origin.y),
            static_cast<float>(ray.origin.z),
            static_cast<float>(ray.maximum_distance),
        });
        write_texel(0U, packed.ray_row_pitch, x + 1U, row, {
            static_cast<float>(ray.direction.x),
            static_cast<float>(ray.direction.y),
            static_cast<float>(ray.direction.z),
            0.0F,
        });
    }
    for (std::size_t index = 0U; index < request.triangles.size(); ++index) {
        const ProjectionTriangle& triangle = request.triangles[index];
        const std::uint32_t row = static_cast<std::uint32_t>(
            index / kRecordsPerInputRow);
        const std::uint32_t x = static_cast<std::uint32_t>(
            index % kRecordsPerInputRow) * 3U;
        const auto pack_vertex = [&](std::uint32_t vertex_x, core::Vec3d vertex) {
            write_texel(packed.triangle_offset, packed.triangle_row_pitch,
                        vertex_x, row, {
                static_cast<float>(vertex.x),
                static_cast<float>(vertex.y),
                static_cast<float>(vertex.z),
                0.0F,
            });
        };
        pack_vertex(x, triangle.v0);
        pack_vertex(x + 1U, triangle.v1);
        pack_vertex(x + 2U, triangle.v2);
    }
    return core::Result<PackedInputs>::success(std::move(packed));
}

struct ProjectionConstants {
    std::uint32_t ray_count = 0U;
    std::uint32_t triangle_count = 0U;
    std::uint32_t ray_records_per_row = kRecordsPerInputRow;
    std::uint32_t triangle_records_per_row = kRecordsPerInputRow;
    std::uint32_t output_width = 0U;
    std::uint32_t backface_policy = 0U;
};

class DeviceResourceGuard final {
public:
    explicit DeviceResourceGuard(D3D12Device& device) : device_(device) {}
    ~DeviceResourceGuard() { static_cast<void>(cleanup()); }

    DeviceResourceGuard(const DeviceResourceGuard&) = delete;
    DeviceResourceGuard& operator=(const DeviceResourceGuard&) = delete;

    void set_pending(gpu::SubmissionSerial serial) noexcept {
        pending_serial_ = serial;
    }

    void clear_pending() noexcept { pending_serial_.reset(); }

    core::Result<void> cleanup() {
        std::optional<core::Diagnostic> first_error;
        const auto retain_error = [&first_error](const core::Result<void>& result) {
            if (!result && !first_error.has_value()) first_error = result.error();
        };
        bool queue_idle = true;
        if (pending_serial_.has_value()) {
            const auto waited = device_.wait(*pending_serial_);
            if (!waited) {
                retain_error(waited);
                const auto idle = device_.wait_idle();
                retain_error(idle);
                queue_idle = static_cast<bool>(idle);
            }
            pending_serial_.reset();
        } else {
            const auto idle = device_.wait_idle();
            retain_error(idle);
            queue_idle = static_cast<bool>(idle);
        }
        if (!queue_idle) {
            return core::Result<void>::failure(std::move(*first_error));
        }
        if (pipeline) {
            retain_error(device_.destroy_pipeline(pipeline));
            pipeline = {};
        }
        if (shader) {
            retain_error(device_.destroy_shader(shader));
            shader = {};
        }
        if (upload_buffer) {
            retain_error(device_.destroy_buffer(upload_buffer));
            upload_buffer = {};
        }
        if (output_texture) {
            retain_error(device_.destroy_texture(output_texture));
            output_texture = {};
        }
        if (triangles_texture) {
            retain_error(device_.destroy_texture(triangles_texture));
            triangles_texture = {};
        }
        if (rays_texture) {
            retain_error(device_.destroy_texture(rays_texture));
            rays_texture = {};
        }
        if (first_error.has_value()) {
            return core::Result<void>::failure(std::move(*first_error));
        }
        return core::Result<void>::success();
    }

    gpu::TextureHandle rays_texture;
    gpu::TextureHandle triangles_texture;
    gpu::TextureHandle output_texture;
    gpu::BufferHandle upload_buffer;
    gpu::ShaderHandle shader;
    gpu::PipelineHandle pipeline;

private:
    D3D12Device& device_;
    std::optional<gpu::SubmissionSerial> pending_serial_;
};

assets::Sha256Digest hash_hits(std::span<const ProjectionHit> hits) {
    CanonicalBytes canonical;
    canonical.u32(kD3D12ProjectionWorkerVersion);
    canonical.u64(hits.size());
    for (const ProjectionHit& hit : hits) {
        canonical.u64(hit.target_face_id);
        canonical.u32(hit.texel_x);
        canonical.u32(hit.texel_y);
        canonical.u32(hit.sample_index);
        canonical.u8(hit.hit ? 1U : 0U);
        canonical.u64(hit.source_face_id);
        canonical.u32(hit.source_fan_index);
        canonical.u8(hit.backface ? 1U : 0U);
        canonical.f32(hit.distance);
        canonical.f32(hit.barycentric_u);
        canonical.f32(hit.barycentric_v);
        canonical.u8(hit.has_rejected_backface ? 1U : 0U);
        canonical.u64(hit.rejected_source_face_id);
        canonical.u32(hit.rejected_source_fan_index);
        canonical.f32(hit.rejected_distance);
        canonical.f32(hit.rejected_barycentric_u);
        canonical.f32(hit.rejected_barycentric_v);
    }
    return assets::sha256(canonical.bytes);
}

assets::Sha256Digest hash_report_evidence(const D3D12ProjectionReport& report) {
    CanonicalBytes canonical;
    const auto text = [&canonical](std::string_view value) {
        canonical.u64(value.size());
        canonical.bytes.insert(canonical.bytes.end(), value.begin(), value.end());
    };
    canonical.u32(report.worker_version);
    canonical.digest(report.source_digest);
    canonical.digest(report.target_digest);
    canonical.digest(report.settings_digest);
    canonical.digest(report.request_digest);
    canonical.digest(report.shader_source_digest);
    canonical.digest(report.shader_binary_digest);
    canonical.digest(report.output_digest);
    text(report.compiler_invocation_digest);
    text(report.adapter_name);
    text(report.stable_adapter_id);
    text(report.driver);
    canonical.u64(report.candidate_triangle_tests);
    canonical.u64(report.wall_time_nanoseconds);
    return assets::sha256(canonical.bytes);
}

} // namespace

bool D3D12ProjectionReport::output_digest_matches() const noexcept {
    if (output_digest.is_zero()) return false;
    try {
        return hash_hits(hits) == output_digest;
    } catch (...) {
        return false;
    }
}

bool D3D12ProjectionReport::evidence_digest_matches() const noexcept {
    if (evidence_digest_.is_zero()) return false;
    try {
        return hash_report_evidence(*this) == evidence_digest_;
    } catch (...) {
        return false;
    }
}

core::Result<void> D3D12ProjectionRequest::validate() const {
    if (source_digest.is_zero() || target_digest.is_zero() || settings_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "D3D12 projection requires source, target, and settings digests"));
    }
    if (backface_policy != ProjectionBackfacePolicy::front_only &&
        backface_policy != ProjectionBackfacePolicy::both_sides) {
        return core::Result<void>::failure(invalid(
            "D3D12 projection backface policy is unsupported"));
    }
    if (maximum_candidate_pairs == 0U ||
        maximum_candidate_pairs > kMaxD3D12ProjectionPairs ||
        triangles.empty() || rays.empty() ||
        triangles.size() > kMaxD3D12ProjectionRecords ||
        rays.size() > kMaxD3D12ProjectionRecords) {
        return core::Result<void>::failure(validation(
            "D3D12 projection record counts or work budget are outside the supported bounds"));
    }
    const std::uint64_t pair_count =
        static_cast<std::uint64_t>(triangles.size()) *
        static_cast<std::uint64_t>(rays.size());
    if (pair_count > maximum_candidate_pairs) {
        return core::Result<void>::failure(validation(
            "D3D12 projection exceeds its explicit candidate-pair budget"));
    }

    for (std::size_t index = 0U; index < triangles.size(); ++index) {
        const ProjectionTriangle& triangle = triangles[index];
        if (triangle.source_face_id == 0U || !triangle.v0.finite() ||
            !triangle.v1.finite() || !triangle.v2.finite() ||
            !finite_float(triangle.v0.x) || !finite_float(triangle.v0.y) ||
            !finite_float(triangle.v0.z) || !finite_float(triangle.v1.x) ||
            !finite_float(triangle.v1.y) || !finite_float(triangle.v1.z) ||
            !finite_float(triangle.v2.x) || !finite_float(triangle.v2.y) ||
            !finite_float(triangle.v2.z)) {
            return core::Result<void>::failure(validation(
                "D3D12 projection triangle identity or coordinates are invalid"));
        }
        const auto key = std::pair{triangle.source_face_id, triangle.fan_index};
        if (index != 0U) {
            const ProjectionTriangle& previous = triangles[index - 1U];
            if (!(std::pair{previous.source_face_id, previous.fan_index} < key)) {
                return core::Result<void>::failure(validation(
                    "D3D12 projection triangles must be strictly ordered by stable identity"));
            }
        }
        const core::Vec3d edge1 = triangle.v1 - triangle.v0;
        const core::Vec3d edge2 = triangle.v2 - triangle.v0;
        const core::Vec3d normal = core::cross(edge1, edge2);
        const core::Vec3d float_v0{
            static_cast<float>(triangle.v0.x),
            static_cast<float>(triangle.v0.y),
            static_cast<float>(triangle.v0.z),
        };
        const core::Vec3d float_v1{
            static_cast<float>(triangle.v1.x),
            static_cast<float>(triangle.v1.y),
            static_cast<float>(triangle.v1.z),
        };
        const core::Vec3d float_v2{
            static_cast<float>(triangle.v2.x),
            static_cast<float>(triangle.v2.y),
            static_cast<float>(triangle.v2.z),
        };
        const core::Vec3d float_normal = core::cross(float_v1 - float_v0, float_v2 - float_v0);
        if (!normal.finite() || normal.length_squared() <= 0.0 ||
            !float_normal.finite() || float_normal.length_squared() <= 0.0) {
            return core::Result<void>::failure(validation(
                "D3D12 projection rejects degenerate triangles in float32 execution space"));
        }
    }

    for (std::size_t index = 0U; index < rays.size(); ++index) {
        const ProjectionRay& ray = rays[index];
        if (ray.target_face_id == 0U || !ray.origin.finite() ||
            !ray.direction.finite() || !finite_float(ray.origin.x) ||
            !finite_float(ray.origin.y) || !finite_float(ray.origin.z) ||
            !finite_float(ray.direction.x) || !finite_float(ray.direction.y) ||
            !finite_float(ray.direction.z) || !std::isfinite(ray.maximum_distance) ||
            !finite_float(ray.maximum_distance) || ray.maximum_distance <= 0.0 ||
            static_cast<float>(ray.maximum_distance) <= 0.0F) {
            return core::Result<void>::failure(validation(
                "D3D12 projection ray identity, coordinates, or distance are invalid"));
        }
        const double direction_length = ray.direction.length();
        if (!std::isfinite(direction_length) ||
            std::abs(direction_length - 1.0) > 1.0e-3) {
            return core::Result<void>::failure(validation(
                "D3D12 projection ray directions must be normalized"));
        }
        if (index != 0U) {
            const ProjectionRay& previous = rays[index - 1U];
            const auto previous_key = std::tuple{
                previous.target_face_id, previous.texel_y, previous.texel_x,
                previous.sample_index};
            const auto key = std::tuple{
                ray.target_face_id, ray.texel_y, ray.texel_x, ray.sample_index};
            if (!(previous_key < key)) {
                return core::Result<void>::failure(validation(
                    "D3D12 projection rays must be strictly ordered by target identity"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<D3D12ProjectionReport> run_d3d12_projection_worker(
    D3D12Device& device,
    const D3D12ProjectionRequest& request) {
    if (auto result = request.validate(); !result) {
        return core::Result<D3D12ProjectionReport>::failure(result.error());
    }
    if (!device.capabilities().compute_queue || !device.capabilities().copy_queue) {
        return core::Result<D3D12ProjectionReport>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "D3D12 projection requires compute and copy queues"));
    }

    try {
        const auto started = std::chrono::steady_clock::now();
        auto packed_inputs_result = pack_inputs(request);
        if (!packed_inputs_result) {
            return core::Result<D3D12ProjectionReport>::failure(
                packed_inputs_result.error());
        }
        PackedInputs packed_inputs = std::move(packed_inputs_result).value();
        const std::uint32_t ray_count = static_cast<std::uint32_t>(request.rays.size());
        const std::uint32_t triangle_count =
            static_cast<std::uint32_t>(request.triangles.size());
        const std::uint32_t output_width = std::min(kOutputRowWidth, ray_count);
        const std::uint32_t output_height =
            (ray_count + output_width - 1U) / output_width;
        const std::uint64_t upload_bytes = packed_inputs.staging.size();

        DeviceResourceGuard resources(device);
        const auto upload_buffer = device.create_buffer(gpu::BufferDesc{
            upload_bytes,
            gpu::MemoryClass::upload,
            gpu::BufferUsage::transfer_source,
        });
        if (!upload_buffer) {
            return core::Result<D3D12ProjectionReport>::failure(upload_buffer.error());
        }
        resources.upload_buffer = upload_buffer.value();
        if (auto result = device.write_buffer(
                resources.upload_buffer, 0U, packed_inputs.staging); !result) {
            return core::Result<D3D12ProjectionReport>::failure(result.error());
        }

        const auto rays_texture = device.create_texture(packed_inputs.rays_texture);
        if (!rays_texture) {
            return core::Result<D3D12ProjectionReport>::failure(rays_texture.error());
        }
        resources.rays_texture = rays_texture.value();
        const auto triangles_texture =
            device.create_texture(packed_inputs.triangles_texture);
        if (!triangles_texture) {
            return core::Result<D3D12ProjectionReport>::failure(
                triangles_texture.error());
        }
        resources.triangles_texture = triangles_texture.value();
        const std::uint32_t output_texture_width = output_width * 2U;
        const auto output_texture = device.create_texture(gpu::TextureDesc{
            output_texture_width,
            output_height,
            1U,
            1U,
            1U,
            gpu::Format::rgba32_float,
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
        });
        if (!output_texture) {
            return core::Result<D3D12ProjectionReport>::failure(output_texture.error());
        }
        resources.output_texture = output_texture.value();

        device_ir::DeviceCommandStream upload_stream;
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.upload_buffer},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_source,
        });
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.rays_texture},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_destination,
        });
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.triangles_texture},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_destination,
        });
        upload_stream.append(device_ir::CmdCopyBufferToTexture{
            resources.upload_buffer, resources.rays_texture, 0U, 0U});
        upload_stream.append(device_ir::CmdCopyBufferToTexture{
            resources.upload_buffer, resources.triangles_texture,
            packed_inputs.triangle_offset, 0U});
        const auto upload_serial = device.submit(upload_stream, gpu::QueueType::copy);
        if (!upload_serial) {
            return core::Result<D3D12ProjectionReport>::failure(upload_serial.error());
        }
        resources.set_pending(upload_serial.value());
        if (auto result = device.wait(upload_serial.value()); !result) {
            return core::Result<D3D12ProjectionReport>::failure(result.error());
        }
        resources.clear_pending();

        const ShaderSource shader_source{
            "carto_d3d12_ray_projection_v2",
            gpu::ShaderStage::compute,
            "main",
            "carto_d3d12_ray_projection_v2.hlsl",
            std::string(kProjectionShader),
            true,
            false,
        };
        const auto compiled_shader = compile_hlsl(shader_source);
        if (!compiled_shader) {
            return core::Result<D3D12ProjectionReport>::failure(
                compiled_shader.error());
        }
        const auto shader = device.create_shader(compiled_shader.value());
        if (!shader) {
            return core::Result<D3D12ProjectionReport>::failure(shader.error());
        }
        resources.shader = shader.value();
        gpu::PipelineDesc pipeline_descriptor;
        pipeline_descriptor.debug_name = "carto_d3d12_ray_projection_v2";
        pipeline_descriptor.shaders = {resources.shader};
        pipeline_descriptor.descriptor_bindings = {
            gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
            gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
            gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::compute},
        };
        pipeline_descriptor.sampled_texture_count = 2U;
        pipeline_descriptor.depth_test = false;
        pipeline_descriptor.push_constant_bytes = sizeof(ProjectionConstants);
        const auto pipeline = device.create_pipeline(pipeline_descriptor);
        if (!pipeline) {
            return core::Result<D3D12ProjectionReport>::failure(pipeline.error());
        }
        resources.pipeline = pipeline.value();

        device_ir::DeviceCommandStream compute_stream;
        compute_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.rays_texture},
            device_ir::ResourceState::copy_destination,
            device_ir::ResourceState::shader_read,
        });
        compute_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.triangles_texture},
            device_ir::ResourceState::copy_destination,
            device_ir::ResourceState::shader_read,
        });
        compute_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resources.output_texture},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::shader_write,
        });
        compute_stream.append(device_ir::CmdBindPipeline{resources.pipeline});
        compute_stream.append(device_ir::CmdBindResources{{
            device_ir::ResourceBinding{
                0U, 0U, device_ir::ResourceHandle{resources.rays_texture}, std::nullopt},
            device_ir::ResourceBinding{
                0U, 1U, device_ir::ResourceHandle{resources.triangles_texture}, std::nullopt},
            device_ir::ResourceBinding{
                0U, 2U, device_ir::ResourceHandle{resources.output_texture}, std::nullopt},
        }});
        const ProjectionConstants constants{
            ray_count,
            triangle_count,
            kRecordsPerInputRow,
            kRecordsPerInputRow,
            output_width,
            request.backface_policy == ProjectionBackfacePolicy::front_only ? 0U : 1U,
        };
        std::vector<std::uint8_t> push_constant_bytes(sizeof(constants));
        std::memcpy(push_constant_bytes.data(), &constants, sizeof(constants));
        compute_stream.append(device_ir::CmdPushConstants{std::move(push_constant_bytes)});
        compute_stream.append(device_ir::CmdDispatch{
            (output_width + kThreadGroupWidth - 1U) / kThreadGroupWidth,
            output_height,
            1U,
        });
        compute_stream.append(device_ir::CmdUavBarrier{
            device_ir::ResourceHandle{resources.output_texture}});
        const auto compute_serial = device.submit(
            compute_stream, gpu::QueueType::compute);
        if (!compute_serial) {
            return core::Result<D3D12ProjectionReport>::failure(compute_serial.error());
        }
        resources.set_pending(compute_serial.value());
        if (auto result = device.wait(compute_serial.value()); !result) {
            return core::Result<D3D12ProjectionReport>::failure(result.error());
        }
        resources.clear_pending();

        const auto gpu_pixels = device.read_texture_rgba32f(resources.output_texture);
        if (!gpu_pixels) {
            return core::Result<D3D12ProjectionReport>::failure(gpu_pixels.error());
        }
        if (gpu_pixels.value().size() !=
            static_cast<std::size_t>(output_texture_width) * output_height * 4U) {
            return core::Result<D3D12ProjectionReport>::failure(validation(
                "D3D12 projection readback returned a non-canonical pixel count"));
        }

        D3D12ProjectionReport report;
        report.source_digest = request.source_digest;
        report.target_digest = request.target_digest;
        report.settings_digest = request.settings_digest;
        report.request_digest = request_digest(request);
        report.shader_source_digest = assets::sha256(as_bytes(kProjectionShader));
        report.shader_binary_digest = assets::sha256(compiled_shader.value().bytes);
        report.compiler_invocation_digest = compiled_shader.value().compiler_digest;
        report.adapter_name = device.identity().adapter_name;
        report.stable_adapter_id = device.identity().stable_adapter_id;
        report.driver = device.identity().driver;
        report.candidate_triangle_tests =
            static_cast<std::uint64_t>(request.rays.size()) * request.triangles.size();
        report.hits.reserve(request.rays.size());
        for (std::size_t index = 0U; index < request.rays.size(); ++index) {
            const std::uint32_t x = static_cast<std::uint32_t>(index % output_width);
            const std::uint32_t y = static_cast<std::uint32_t>(index / output_width);
            const std::size_t offset =
                (static_cast<std::size_t>(y) * output_texture_width + x * 2U) * 4U;
            const float triangle_index_value = gpu_pixels.value()[offset];
            const float distance = gpu_pixels.value()[offset + 1U];
            const float barycentric_u = gpu_pixels.value()[offset + 2U];
            const float barycentric_v = gpu_pixels.value()[offset + 3U];
            if (!std::isfinite(triangle_index_value) || !std::isfinite(distance) ||
                !std::isfinite(barycentric_u) || !std::isfinite(barycentric_v)) {
                return core::Result<D3D12ProjectionReport>::failure(validation(
                    "D3D12 projection readback contains non-finite worker output"));
            }
            const ProjectionRay& ray = request.rays[index];
            ProjectionHit hit;
            hit.target_face_id = ray.target_face_id;
            hit.texel_x = ray.texel_x;
            hit.texel_y = ray.texel_y;
            hit.sample_index = ray.sample_index;
            if (triangle_index_value != -1.0F) {
                const bool backface = triangle_index_value <= -2.0F;
                const float decoded_triangle_index = backface
                    ? -triangle_index_value - 2.0F
                    : triangle_index_value;
                const float encoded_maximum_distance =
                    static_cast<float>(ray.maximum_distance);
                if (decoded_triangle_index < 0.0F ||
                    std::floor(decoded_triangle_index) != decoded_triangle_index ||
                    decoded_triangle_index >= static_cast<float>(request.triangles.size()) ||
                    distance <= 0.0F || distance > encoded_maximum_distance ||
                    barycentric_u < 0.0F || barycentric_v < 0.0F ||
                    barycentric_u > 1.0F || barycentric_v > 1.0F ||
                    barycentric_u + barycentric_v > 1.00001F) {
                    return core::Result<D3D12ProjectionReport>::failure(validation(
                        "D3D12 projection readback contains an invalid hit record"));
                }
                const std::size_t triangle_index =
                    static_cast<std::size_t>(decoded_triangle_index);
                const ProjectionTriangle& triangle = request.triangles[triangle_index];
                hit.hit = true;
                hit.source_face_id = triangle.source_face_id;
                hit.source_fan_index = triangle.fan_index;
                hit.backface = backface;
                hit.distance = distance;
                hit.barycentric_u = barycentric_u;
                hit.barycentric_v = barycentric_v;
            } else if (distance != -1.0F || barycentric_u != 0.0F ||
                       barycentric_v != 0.0F) {
                return core::Result<D3D12ProjectionReport>::failure(validation(
                    "D3D12 projection readback contains an invalid miss sentinel"));
            }

            const std::size_t rejected_offset = offset + 4U;
            const float rejected_index_value = gpu_pixels.value()[rejected_offset];
            const float rejected_distance = gpu_pixels.value()[rejected_offset + 1U];
            const float rejected_u = gpu_pixels.value()[rejected_offset + 2U];
            const float rejected_v = gpu_pixels.value()[rejected_offset + 3U];
            if (!std::isfinite(rejected_index_value) || !std::isfinite(rejected_distance) ||
                !std::isfinite(rejected_u) || !std::isfinite(rejected_v)) {
                return core::Result<D3D12ProjectionReport>::failure(validation(
                    "D3D12 projection rejected-backface readback contains non-finite output"));
            }
            if (rejected_index_value != -1.0F) {
                const float encoded_maximum_distance = static_cast<float>(ray.maximum_distance);
                if (request.backface_policy != ProjectionBackfacePolicy::front_only ||
                    rejected_index_value < 0.0F ||
                    std::floor(rejected_index_value) != rejected_index_value ||
                    rejected_index_value >= static_cast<float>(request.triangles.size()) ||
                    rejected_distance <= 0.0F || rejected_distance > encoded_maximum_distance ||
                    rejected_u < 0.0F || rejected_v < 0.0F || rejected_u > 1.0F ||
                    rejected_v > 1.0F || rejected_u + rejected_v > 1.00001F) {
                    return core::Result<D3D12ProjectionReport>::failure(validation(
                        "D3D12 projection returned an invalid rejected-backface record"));
                }
                const std::size_t rejected_triangle_index =
                    static_cast<std::size_t>(rejected_index_value);
                const ProjectionTriangle& rejected_triangle =
                    request.triangles[rejected_triangle_index];
                hit.has_rejected_backface = true;
                hit.rejected_source_face_id = rejected_triangle.source_face_id;
                hit.rejected_source_fan_index = rejected_triangle.fan_index;
                hit.rejected_distance = rejected_distance;
                hit.rejected_barycentric_u = rejected_u;
                hit.rejected_barycentric_v = rejected_v;
            } else if (rejected_distance != -1.0F || rejected_u != 0.0F ||
                       rejected_v != 0.0F) {
                return core::Result<D3D12ProjectionReport>::failure(validation(
                    "D3D12 projection returned an invalid rejected-backface miss sentinel"));
            }
            report.hits.push_back(hit);
        }
        report.output_digest = hash_hits(report.hits);
        report.wall_time_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        if (auto result = resources.cleanup(); !result) {
            return core::Result<D3D12ProjectionReport>::failure(result.error());
        }
        report.evidence_digest_ = hash_report_evidence(report);
        return core::Result<D3D12ProjectionReport>::success(std::move(report));
    } catch (const std::bad_alloc&) {
        return core::Result<D3D12ProjectionReport>::failure(validation(
            "D3D12 projection exceeded host allocation capacity"));
    }
}

} // namespace carto::d3d12
