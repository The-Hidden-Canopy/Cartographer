#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/eval/graph.hpp>
#include <carto/web_geometry/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::web_geometry {

enum class EvaluationStage : std::uint8_t {
    compile_mesh,
    surface_metadata,
    leaf_clusters,
    hierarchy,
    page_pack,
    package,
};

inline constexpr std::size_t kEvaluationStageCount = 6U;

struct PageDigest {
    RuntimePageId page = 0U;
    assets::Sha256Digest digest;

    [[nodiscard]] constexpr auto operator<=>(const PageDigest&) const noexcept = default;
};

// This is a graph publication receipt, not an evaluated mesh cache. It binds
// the already validated C1-C5 package to a typed graph and records the
// content identities needed to reject stale or partial rebuilds.
struct WebGeometryEvaluation {
    eval::EvaluationGraph graph;
    std::array<eval::NodeId, kEvaluationStageCount> nodes{};
    std::array<std::string, kEvaluationStageCount> output_digests;
    core::Revision source_revision;
    std::string options_digest;
    std::vector<std::string> cluster_digests;
    std::vector<PageDigest> page_digests;
    std::string package_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct WebGeometryInvalidation {
    std::vector<EvaluationStage> stages;
    std::vector<ClusterId> clusters;
    std::vector<RuntimePageId> pages;

    [[nodiscard]] bool empty() const noexcept { return stages.empty(); }
    [[nodiscard]] bool stage_dirty(EvaluationStage stage) const noexcept;
};

// Publishes the package's derived stages as a deterministic graph intent. No
// worker, cache, GPU resource, or project mutation is performed here.
[[nodiscard]] core::Result<WebGeometryEvaluation> publish_evaluation(
    const WebGeometryPackage& package);

// Compares two validated publication receipts. The returned stage set is
// downstream-closed: the earliest changed stage and every dependent stage are
// marked dirty. Cluster/page identity changes are reported independently for
// targeted rebuild and residency callers.
[[nodiscard]] core::Result<WebGeometryInvalidation> compute_invalidation(
    const WebGeometryEvaluation& before,
    const WebGeometryEvaluation& after);

} // namespace carto::web_geometry
