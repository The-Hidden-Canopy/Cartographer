#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/d3d12/device.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::d3d12 {

inline constexpr std::uint32_t kD3D12ProjectionWorkerVersion = 2U;
inline constexpr std::uint64_t kMaxD3D12ProjectionPairs = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxD3D12ProjectionRecords = 1'000'000ULL;
inline constexpr std::uint64_t kMaxD3D12ProjectionUploadBytes =
    64ULL * 1024ULL * 1024ULL;

enum class ProjectionBackfacePolicy : std::uint8_t {
    front_only,
    both_sides,
};

struct ProjectionTriangle {
    std::uint64_t source_face_id = 0U;
    std::uint32_t fan_index = 0U;
    core::Vec3d v0;
    core::Vec3d v1;
    core::Vec3d v2;
};

struct ProjectionRay {
    std::uint64_t target_face_id = 0U;
    std::uint32_t texel_x = 0U;
    std::uint32_t texel_y = 0U;
    std::uint32_t sample_index = 0U;
    core::Vec3d origin;
    core::Vec3d direction;
    double maximum_distance = 0.0;
};

// Inputs are caller-owned snapshots. The three digests are required provenance
// claims and are echoed into the result; this low-level worker cannot establish
// whether those snapshots are still current in a project.
struct D3D12ProjectionRequest {
    assets::Sha256Digest source_digest;
    assets::Sha256Digest target_digest;
    assets::Sha256Digest settings_digest;
    ProjectionBackfacePolicy backface_policy = ProjectionBackfacePolicy::both_sides;
    std::uint64_t maximum_candidate_pairs = kMaxD3D12ProjectionPairs;
    std::vector<ProjectionTriangle> triangles;
    std::vector<ProjectionRay> rays;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ProjectionHit {
    std::uint64_t target_face_id = 0U;
    std::uint32_t texel_x = 0U;
    std::uint32_t texel_y = 0U;
    std::uint32_t sample_index = 0U;
    bool hit = false;
    std::uint64_t source_face_id = 0U;
    std::uint32_t source_fan_index = 0U;
    bool backface = false;
    float distance = 0.0F;
    float barycentric_u = 0.0F;
    float barycentric_v = 0.0F;
    bool has_rejected_backface = false;
    std::uint64_t rejected_source_face_id = 0U;
    std::uint32_t rejected_source_fan_index = 0U;
    float rejected_distance = 0.0F;
    float rejected_barycentric_u = 0.0F;
    float rejected_barycentric_v = 0.0F;
};

struct D3D12ProjectionReport {
    std::uint32_t worker_version = kD3D12ProjectionWorkerVersion;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest target_digest;
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest request_digest;
    assets::Sha256Digest shader_source_digest;
    assets::Sha256Digest shader_binary_digest;
    assets::Sha256Digest output_digest;
    std::string compiler_invocation_digest;
    std::string adapter_name;
    std::string stable_adapter_id;
    std::string driver;
    std::uint64_t candidate_triangle_tests = 0U;
    std::uint64_t wall_time_nanoseconds = 0U;
    std::vector<ProjectionHit> hits;

    // Rehashes returned hit records so callers can reject mutated output.
    [[nodiscard]] bool output_digest_matches() const noexcept;
    // Verifies that worker provenance and timing still match the report
    // produced by run_d3d12_projection_worker().
    [[nodiscard]] bool evidence_digest_matches() const noexcept;

private:
    assets::Sha256Digest evidence_digest_;

    friend core::Result<D3D12ProjectionReport> run_d3d12_projection_worker(
        D3D12Device& device,
        const D3D12ProjectionRequest& request);
};

// Executes a bounded, deterministic-order ray/triangle scan on D3D12 compute.
// One thread owns one ray. Source triangles must be ordered by stable
// (face_id, fan_index); exact-distance ties keep the first such triangle.
// Front-only requests also retain the nearest rejected backface per ray.
// The report is transient worker evidence, not a serialized bake artifact.
[[nodiscard]] core::Result<D3D12ProjectionReport> run_d3d12_projection_worker(
    D3D12Device& device,
    const D3D12ProjectionRequest& request);

} // namespace carto::d3d12
