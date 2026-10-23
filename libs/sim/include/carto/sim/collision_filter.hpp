#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::sim {

// One authored collision filter: a sorted, unique set of joints excluded from
// collision response. Application is fail-closed and observable: a pinned
// cloth vertex whose attachment joint is excluded is an unsatisfiable
// constraint and refuses the bake, and every applied filter's content digest
// is recorded in the simulation receipt so the policy that shaped a cache is
// auditable afterwards.
struct CollisionFilter {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxExcludedJoints = 4'096U;

    std::vector<kinematic::JointId> excluded_joints;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool excludes(kinematic::JointId joint) const noexcept;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Union of the excluded-joint sets, canonically sorted and unique. The number
// of input filters is bounded by SimCaps::kMaxCollisionFilters; the merged
// set itself is bounded by CollisionFilter::kMaxExcludedJoints.
[[nodiscard]] core::Result<CollisionFilter> merge_collision_filters(
    std::span<const CollisionFilter> filters);

} // namespace carto::sim
