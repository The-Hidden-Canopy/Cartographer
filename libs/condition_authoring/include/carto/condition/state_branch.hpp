#pragma once

#include <carto/condition/authoring.hpp>

#include <vector>

namespace carto::condition {

struct StateDifference {
    Id target_record_id = 0U;
    Id target_asset_id = 0U;
    StateTargetKind target_kind = StateTargetKind::safe_place_profile;
    StateProperty property = StateProperty::privacy;
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

// Compare two authored branches over their immutable source baseline. Branch
// deltas must match their explicit before-values; drift is returned as stale
// instead of being silently rebased or merged.
[[nodiscard]] core::Result<StateComparisonResult> compare_state_branches(
    const AuthoringDocument& authoring,
    Id left_branch_id,
    Id right_branch_id);

} // namespace carto::condition
