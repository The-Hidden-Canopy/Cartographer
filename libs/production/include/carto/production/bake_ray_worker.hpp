#pragma once

#include <carto/production/bake_graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace carto::production {

inline constexpr std::uint32_t kBakeRayQueryProtocolVersion = 1U;
inline constexpr std::size_t kMaxBakeRayQueryTriangles = kMaxBakeSourceTriangles;
inline constexpr std::size_t kMaxBakeRayQueryRays = 1'000'000U;
inline constexpr std::size_t kMaxBakeRayQueryRequestBytes = 256U * 1024U * 1024U;

enum class BakeRayEvidenceClass : std::uint8_t {
    software = 0U,
    hardware = 1U,
    simulated = 2U,
};

struct BakeRayQueryTriangle {
    std::uint64_t face_id = 0U;
    std::uint32_t fan_index = 0U;
    std::array<core::Vec3d, 3U> positions{};
};

struct BakeRayQueryRay {
    // Index into the canonical raster sample array, independent of request order.
    std::uint64_t sample_index = 0U;
    std::uint64_t target_face_id = 0U;
    std::uint32_t texel_x = 0U;
    std::uint32_t texel_y = 0U;
    core::Vec3d origin;
    core::Vec3d direction;
    double maximum_distance = 0.0;
};

struct BakeRayQueryRequest {
    std::uint32_t protocol_version = kBakeRayQueryProtocolVersion;
    BakeRecipe recipe;
    BakeNormalSpace normal_space = BakeNormalSpace::tangent;
    assets::Sha256Digest recipe_digest;
    assets::Sha256Digest settings_digest;
    std::vector<BakeRayQueryTriangle> triangles;
    std::vector<BakeRayQueryRay> rays;

    [[nodiscard]] core::Result<void> validate() const;
};

struct BakeRayQueryOutcome {
    std::uint64_t sample_index = 0U;
    std::uint64_t target_face_id = 0U;
    std::uint32_t texel_x = 0U;
    std::uint32_t texel_y = 0U;
    bool hit = false;
    std::uint64_t source_face_id = 0U;
    std::uint32_t source_fan_index = 0U;
    bool backface = false;
    double distance = 0.0;
    std::array<double, 3U> barycentric{};
    std::optional<std::pair<std::uint64_t, std::uint32_t>> rejected_backface;
};

struct BakeRayQueryReport {
    std::uint32_t protocol_version = kBakeRayQueryProtocolVersion;
    std::uint32_t worker_version = 0U;
    BakeRayEvidenceClass evidence_class = BakeRayEvidenceClass::software;
    std::string worker_id;
    // Digest of the worker implementation or executable used for this report.
    assets::Sha256Digest worker_build_digest;
    std::uint64_t elapsed_nanoseconds = 0U;
    bool success = false;
    std::string error;
    assets::Sha256Digest request_digest;
    assets::Sha256Digest geometry_digest;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest target_digest;
    assets::Sha256Digest recipe_digest;
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest worker_provenance_digest;
    assets::Sha256Digest output_digest;
    // Exactly one outcome per request ray, in canonical request order.
    std::vector<BakeRayQueryOutcome> outcomes;

    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] core::Result<void> validate_for(
        const BakeRayQueryRequest& request) const;
    [[nodiscard]] bool matches_request(const BakeRayQueryRequest& request) const;
};

[[nodiscard]] core::Result<assets::Sha256Digest> bake_ray_query_geometry_digest(
    const BakeRayQueryRequest& request);
[[nodiscard]] core::Result<assets::Sha256Digest> bake_ray_query_request_digest(
    const BakeRayQueryRequest& request);
[[nodiscard]] assets::Sha256Digest bake_ray_query_output_digest(
    const BakeRayQueryReport& report);
[[nodiscard]] core::Result<void> refresh_bake_ray_query_report(
    const BakeRayQueryRequest& request,
    BakeRayQueryReport& report);
// The profile is derived from worker provenance. Callers cannot choose an
// arbitrary profile when admitting a worker-produced artifact.
[[nodiscard]] core::Result<std::string> bake_ray_worker_platform_profile(
    const BakeRayQueryReport& report);

using BakeRayQueryCallback =
    std::function<BakeRayQueryReport(const BakeRayQueryRequest&)>;

struct BakeRayQueryCompileResult {
    BakeExecutionReport bake;
    BakeRayQueryRequest request;
    BakeRayQueryReport worker;
    assets::Sha256Digest cpu_reference_artifact_digest;

    [[nodiscard]] bool succeeded() const;

private:
    // Prevent a caller from editing public report fields after successful
    // compilation and then treating the modified evidence as compiler output.
    assets::Sha256Digest validation_digest_;

    friend BakeRayQueryCompileResult compile_tangent_normal_map_with_ray_worker(
        const BakeRecipe& recipe,
        const geometry::EditableMesh& source,
        const geometry::EditableMesh& target,
        const BakeRayQueryCallback& worker);
};

// Compiles a tangent-space normal map through a bounded ray worker. The worker
// result is admitted only when its request binding and hit geometry are valid
// and its final artifact exactly matches the deterministic CPU reference.
[[nodiscard]] BakeRayQueryCompileResult compile_tangent_normal_map_with_ray_worker(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    const BakeRayQueryCallback& worker);

[[nodiscard]] core::Result<void> validate_tangent_normal_map_ray_worker_result(
    const BakeRayQueryCompileResult& result,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target);

// Publishes only a successfully parity-checked worker result under the
// platform profile derived from its validated worker provenance.
[[nodiscard]] core::Result<void> publish_tangent_normal_map_with_ray_worker(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BakeRayQueryCompileResult& result,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy);

} // namespace carto::production
