#pragma once

#include <carto/geometry/topology_edit.hpp>
#include <carto/production/bake_graph.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::project {
class ProjectTransaction;
}

namespace carto::production {

inline constexpr std::uint32_t kLocalRemeshPatchVersion = 1U;
inline constexpr std::uint32_t kMaxLocalRemeshSplits = 64U;

struct LocalRemeshSettings {
    core::Vec3d center;
    double region_radius = 0.5;
    double maximum_edge_length = 0.25;
    std::uint32_t max_splits = 8U;
    bool preserve_boundary = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> digest() const;
};

struct LocalRemeshSplitRecord {
    geometry::EdgeId source_edge;
    double source_edge_length = 0.0;
    geometry::TopologyEditReceipt topology;
};

// Returned only after the entire bounded patch has been staged into an empty
// project transaction. The project commit journals the final document and
// each split receipt in order, preserving the source-to-destination chain.
struct LocalRemeshPatchReceipt {
    std::uint64_t mesh_asset = 0U;
    std::string algorithm{"carto.local-remesh.edge-split.v1"};
    BakeMeshBinding source_before;
    BakeMeshBinding source_after;
    LocalRemeshSettings settings;
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest result_digest;
    std::vector<LocalRemeshSplitRecord> splits;

    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string transaction_operation() const;
};

// Splits the longest eligible internal edge per step, with EdgeId tie-breaks.
// Every edge midpoint must remain inside the bounded region. The operation
// rejects unsupported attribute domains and rolls the transaction back if the
// split budget cannot finish the patch.
[[nodiscard]] core::Result<LocalRemeshPatchReceipt> stage_local_remesh_patch(
    project::ProjectTransaction& transaction,
    std::uint64_t mesh_asset,
    const LocalRemeshSettings& settings);

} // namespace carto::production
