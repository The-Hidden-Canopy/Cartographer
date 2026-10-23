#pragma once

#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace carto::character {

enum class WeightProvenance : std::uint8_t {
    authored_paint,
    automatic_proposal,
    transferred,
    imported,
};

enum class WeightStaleness : std::uint8_t {
    fresh,
    stale_mesh_topology,
    stale_joint_graph,
    invalid,
};

[[nodiscard]] std::string_view weight_provenance_name(WeightProvenance provenance) noexcept;
[[nodiscard]] std::string_view weight_staleness_name(WeightStaleness staleness) noexcept;

// Shared admission contract for one vertex's influence list, used by every
// producer of skin weights (manual paint, automatic fit, transfer, import).
// Admission requires exactly 1..8 influences (matching
// kinematic::SkinBinding::kMaxInfluencesPerVertex), strictly positive finite
// weights, unique sorted joints, and a sum within
// kinematic::SkinBinding::kWeightSumTolerance of exactly one.
[[nodiscard]] core::Result<void> validate_influences(
    std::span<const kinematic::JointInfluence> influences);

// Record skeleton for one skin weight layer. The per-vertex joint/weight maps
// are produced and owned by the weights subsystem; this record pins the layer
// to its exact mesh topology and joint graph digests, summarizes the admitted
// influence population, and carries provenance and staleness so a stale layer
// fails closed instead of silently deforming a changed source.
struct SkinWeightLayer {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxInfluencedVertices = 262'144U;
    static constexpr std::size_t kMaxInfluencesPerVertex =
        kinematic::SkinBinding::kMaxInfluencesPerVertex;

    RigLayerId layer_id;
    assets::Sha256Digest mesh_topology_digest;
    assets::Sha256Digest joint_graph_digest;
    // Influence population summary: how many vertices carry influences and
    // the total admitted influence count across the layer.
    std::uint64_t influenced_vertex_count = 0U;
    std::uint64_t total_influence_count = 0U;
    std::optional<assets::Sha256Digest> locks_digest;
    std::optional<assets::Sha256Digest> masks_digest;
    WeightProvenance provenance = WeightProvenance::authored_paint;
    // Recipe or import payload this layer was produced from; mandatory for
    // automatic proposals so a refinement can be reproduced or rejected.
    assets::Sha256Digest provenance_digest;
    WeightStaleness staleness = WeightStaleness::fresh;
    // Project revision at which the layer became stale; meaningful exactly
    // when staleness != fresh.
    core::Revision stale_since;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
