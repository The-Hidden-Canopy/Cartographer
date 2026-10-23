#pragma once

#include <carto/condition/authoring.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace carto::condition {

struct StateDifference {
    Id target_record_id = 0U;
    Id target_asset_id = 0U;
    StateTargetKind target_kind = StateTargetKind::safe_place_profile;
    StateProperty property = StateProperty::privacy;
    StateValue common_value;
    StateValue left_value;
    StateValue right_value;
    bool contested = false;
};

struct StateComparisonResult {
    Id left_branch_id = 0U;
    Id right_branch_id = 0U;
    Id common_ancestor_branch_id = 0U;
    core::Revision project_revision;
    assets::Sha256Digest authoring_digest;
    assets::Sha256Digest comparison_digest;
    std::vector<StateDifference> differences;
};

enum class StateMergeSide : std::uint8_t { common_ancestor, left, right };

struct StateMergeChoice {
    std::size_t difference_index = 0U;
    StateMergeSide side = StateMergeSide::common_ancestor;
};

// Compare two authored branches over their immutable source baseline. Branch
// deltas must match their explicit before-values; drift is returned as stale
// instead of being silently rebased or merged.
[[nodiscard]] core::Result<StateComparisonResult> compare_state_branches(
    const AuthoringDocument& authoring,
    Id left_branch_id,
    Id right_branch_id);

// Build a new branch from the common ancestor. Every value that differs
// between the two compared sides requires an explicit choice, including a
// choice to keep the common-ancestor value. Identical changes on both sides
// are retained without creating a conflict.
[[nodiscard]] core::Result<AuthoredStateBranch> resolve_state_branch_merge(
    const AuthoringDocument& authoring,
    const StateComparisonResult& comparison,
    const std::vector<StateMergeChoice>& choices,
    Id merged_branch_id,
    std::string name,
    std::string applicability = {});

} // namespace carto::condition
