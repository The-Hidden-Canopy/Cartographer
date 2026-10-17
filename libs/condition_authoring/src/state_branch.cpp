#include <carto/condition/state_branch.hpp>

#include <algorithm>
#include <array>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <tuple>
#include <utility>

namespace carto::condition {
namespace {

using StateKey = std::tuple<StateTargetKind, Id, Id, StateProperty>;
using StateMap = std::map<StateKey, StateValue>;

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool same_value(const StateValue& left, const StateValue& right) noexcept {
    return left.numeric_value == right.numeric_value && left.text_value == right.text_value;
}

void insert_safe_place_values(StateMap& values, const SafePlaceProfile& profile) {
    const std::array<std::pair<StateProperty, double>, 8> properties{{
        {StateProperty::privacy, profile.privacy},
        {StateProperty::human_proximity, profile.human_proximity},
        {StateProperty::controllable_entries, profile.controllable_entries},
        {StateProperty::supply_access, profile.supply_access},
        {StateProperty::ambient_noise, profile.ambient_noise},
        {StateProperty::visibility_outward, profile.visibility_outward},
        {StateProperty::visibility_inward, profile.visibility_inward},
        {StateProperty::isolation, profile.isolation},
    }};
    for (const auto& [property, value] : properties) {
        values.emplace(StateKey{StateTargetKind::safe_place_profile, profile.id,
            profile.asset_id, property}, StateValue{value, {}});
    }
}

void insert_surface_values(StateMap& values, const SurfaceStateRecord& surface) {
    values.emplace(StateKey{StateTargetKind::surface_state, surface.id,
        surface.asset_id, StateProperty::occupancy},
        StateValue{0.0, std::string(occupancy_state_name(surface.occupancy))});
    values.emplace(StateKey{StateTargetKind::surface_state, surface.id,
        surface.asset_id, StateProperty::maintenance},
        StateValue{0.0, std::string(maintenance_state_name(surface.maintenance))});
    values.emplace(StateKey{StateTargetKind::surface_state, surface.id,
        surface.asset_id, StateProperty::damage},
        StateValue{0.0, std::string(damage_state_name(surface.damage))});
    values.emplace(StateKey{StateTargetKind::surface_state, surface.id,
        surface.asset_id, StateProperty::wetness}, StateValue{surface.wetness, {}});
}

StateMap baseline_values(const AuthoringDocument& authoring) {
    StateMap values;
    for (const auto& profile : authoring.safe_places) insert_safe_place_values(values, profile);
    for (const auto& surface : authoring.surface_states) insert_surface_values(values, surface);
    return values;
}

std::map<Id, const AuthoredStateBranch*> branch_index(const AuthoringDocument& authoring) {
    std::map<Id, const AuthoredStateBranch*> result;
    for (const auto& branch : authoring.state_branches) result.emplace(branch.id, &branch);
    return result;
}

core::Result<std::vector<Id>> ancestry(
    Id branch_id,
    const std::map<Id, const AuthoredStateBranch*>& branches) {
    std::vector<Id> result;
    std::set<Id> visited;
    while (branch_id != 0U) {
        const auto found = branches.find(branch_id);
        if (found == branches.end()) {
            return core::Result<std::vector<Id>>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "authored state comparison references a missing branch"));
        }
        if (!visited.insert(branch_id).second) {
            return core::Result<std::vector<Id>>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "authored state comparison found a branch cycle"));
        }
        result.push_back(branch_id);
        branch_id = found->second->parent_branch_id;
    }
    result.push_back(0U);
    return core::Result<std::vector<Id>>::success(std::move(result));
}

core::Result<StateMap> materialize(
    const AuthoringDocument& authoring,
    Id branch_id,
    const std::map<Id, const AuthoredStateBranch*>& branches) {
    auto path = ancestry(branch_id, branches);
    if (!path) return core::Result<StateMap>::failure(path.error());
    StateMap result = baseline_values(authoring);
    auto& branch_ids = path.value();
    for (auto iterator = branch_ids.rbegin(); iterator != branch_ids.rend(); ++iterator) {
        if (*iterator == 0U) continue;
        const auto& branch = *branches.at(*iterator);
        for (const auto& delta : branch.deltas) {
            const StateKey key{delta.target_kind, delta.target_record_id,
                delta.target_asset_id, delta.property};
            const auto current = result.find(key);
            if (current == result.end()) {
                return core::Result<StateMap>::failure(stale(
                    "authored state target no longer exists in the source baseline"));
            }
            if (!same_value(current->second, delta.before)) {
                return core::Result<StateMap>::failure(stale(
                    "authored state before-value no longer matches its source or parent branch"));
            }
            current->second = delta.after;
        }
    }
    return core::Result<StateMap>::success(std::move(result));
}

std::string canonical_comparison(const StateComparisonResult& result) {
    std::ostringstream output;
    output << std::setprecision(17) << "CARTOGRAPHER_STATE_COMPARISON 1\n"
           << result.left_branch_id << ' ' << result.right_branch_id << ' '
           << result.common_ancestor_branch_id << ' '
           << result.project_revision.value() << ' '
           << result.authoring_digest.hex() << '\n';
    for (const auto& difference : result.differences) {
        output << difference.target_record_id << ' ' << difference.target_asset_id << ' '
               << state_target_kind_name(difference.target_kind) << ' '
               << state_property_name(difference.property) << ' '
               << difference.left_value.numeric_value << ' '
               << std::quoted(difference.left_value.text_value) << ' '
               << difference.right_value.numeric_value << ' '
               << std::quoted(difference.right_value.text_value) << ' '
               << (difference.contested ? 1 : 0) << '\n';
    }
    return output.str();
}

} // namespace

core::Result<StateComparisonResult> compare_state_branches(
    const AuthoringDocument& authoring,
    Id left_branch_id,
    Id right_branch_id) {
    if (auto result = authoring.validate(); !result) {
        return core::Result<StateComparisonResult>::failure(result.error().with_context(
            "authored state comparison"));
    }
    const auto digest = authoring.canonical_digest();
    if (!digest) return core::Result<StateComparisonResult>::failure(digest.error());
    const auto branches = branch_index(authoring);
    const auto left_ancestry = ancestry(left_branch_id, branches);
    if (!left_ancestry) return core::Result<StateComparisonResult>::failure(left_ancestry.error());
    const auto right_ancestry = ancestry(right_branch_id, branches);
    if (!right_ancestry) return core::Result<StateComparisonResult>::failure(right_ancestry.error());
    const std::set<Id> right_ancestors(right_ancestry.value().begin(),
        right_ancestry.value().end());
    Id common = 0U;
    for (const Id candidate : left_ancestry.value()) {
        if (right_ancestors.contains(candidate)) {
            common = candidate;
            break;
        }
    }
    const auto left_values = materialize(authoring, left_branch_id, branches);
    if (!left_values) return core::Result<StateComparisonResult>::failure(left_values.error());
    const auto right_values = materialize(authoring, right_branch_id, branches);
    if (!right_values) return core::Result<StateComparisonResult>::failure(right_values.error());
    const auto common_values = materialize(authoring, common, branches);
    if (!common_values) return core::Result<StateComparisonResult>::failure(common_values.error());

    StateComparisonResult result;
    result.left_branch_id = left_branch_id;
    result.right_branch_id = right_branch_id;
    result.common_ancestor_branch_id = common;
    result.project_revision = authoring.project_revision;
    result.authoring_digest = digest.value();
    std::set<StateKey> keys;
    for (const auto& [key, value] : left_values.value()) {
        static_cast<void>(value);
        keys.insert(key);
    }
    for (const auto& [key, value] : right_values.value()) {
        static_cast<void>(value);
        keys.insert(key);
    }
    for (const auto& key : keys) {
        const auto left = left_values.value().find(key);
        const auto right = right_values.value().find(key);
        if (left == left_values.value().end() || right == right_values.value().end() ||
            same_value(left->second, right->second)) continue;
        const auto base = common_values.value().find(key);
        const bool left_changed = base == common_values.value().end() ||
            !same_value(left->second, base->second);
        const bool right_changed = base == common_values.value().end() ||
            !same_value(right->second, base->second);
        result.differences.push_back(StateDifference{
            std::get<1>(key),
            std::get<2>(key),
            std::get<0>(key),
            std::get<3>(key),
            left->second,
            right->second,
            left_changed && right_changed});
    }
    const std::string canonical = canonical_comparison(result);
    result.comparison_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    return core::Result<StateComparisonResult>::success(std::move(result));
}

} // namespace carto::condition
