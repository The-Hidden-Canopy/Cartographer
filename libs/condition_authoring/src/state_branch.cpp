#include <carto/condition/state_branch.hpp>

#include <algorithm>
#include <array>
#include <iomanip>
#include <map>
#include <optional>
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
               << difference.common_value.numeric_value << ' '
               << std::quoted(difference.common_value.text_value) << ' '
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
        if (base == common_values.value().end()) continue;
        const bool left_changed = base == common_values.value().end() ||
            !same_value(left->second, base->second);
        const bool right_changed = base == common_values.value().end() ||
            !same_value(right->second, base->second);
        result.differences.push_back(StateDifference{
            std::get<1>(key),
            std::get<2>(key),
            std::get<0>(key),
            std::get<3>(key),
            base->second,
            left->second,
            right->second,
            left_changed && right_changed});
    }
    const std::string canonical = canonical_comparison(result);
    result.comparison_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    return core::Result<StateComparisonResult>::success(std::move(result));
}

core::Result<AuthoredStateBranch> resolve_state_branch_merge(
    const AuthoringDocument& authoring,
    const StateComparisonResult& comparison,
    const std::vector<StateMergeChoice>& choices,
    Id merged_branch_id,
    std::string name,
    std::string applicability) {
    const auto current = compare_state_branches(
        authoring, comparison.left_branch_id, comparison.right_branch_id);
    if (!current) {
        return core::Result<AuthoredStateBranch>::failure(current.error());
    }
    if (current.value().project_revision != comparison.project_revision ||
        current.value().authoring_digest != comparison.authoring_digest ||
        current.value().common_ancestor_branch_id != comparison.common_ancestor_branch_id ||
        current.value().comparison_digest != comparison.comparison_digest) {
        return core::Result<AuthoredStateBranch>::failure(stale(
            "authored state comparison changed before resolution"));
    }
    const StateComparisonResult& reviewed = current.value();
    if (merged_branch_id == 0U ||
        std::any_of(authoring.state_branches.begin(), authoring.state_branches.end(),
            [merged_branch_id](const AuthoredStateBranch& branch) {
                return branch.id == merged_branch_id;
            })) {
        return core::Result<AuthoredStateBranch>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "resolved state branch identity is invalid or already in use"));
    }
    if (choices.size() != reviewed.differences.size()) {
        return core::Result<AuthoredStateBranch>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "every differing state value requires exactly one resolution choice"));
    }

    std::vector<std::optional<StateMergeSide>> selected_sides(
        reviewed.differences.size());
    for (const StateMergeChoice& choice : choices) {
        if (choice.difference_index >= selected_sides.size() ||
            (choice.side != StateMergeSide::common_ancestor &&
             choice.side != StateMergeSide::left &&
             choice.side != StateMergeSide::right) ||
            selected_sides[choice.difference_index].has_value()) {
            return core::Result<AuthoredStateBranch>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "state merge choices contain an invalid or duplicate difference"));
        }
        selected_sides[choice.difference_index] = choice.side;
    }

    const auto branches = branch_index(authoring);
    const auto common_values = materialize(
        authoring, reviewed.common_ancestor_branch_id, branches);
    const auto left_values = materialize(authoring, reviewed.left_branch_id, branches);
    const auto right_values = materialize(authoring, reviewed.right_branch_id, branches);
    if (!common_values) return core::Result<AuthoredStateBranch>::failure(common_values.error());
    if (!left_values) return core::Result<AuthoredStateBranch>::failure(left_values.error());
    if (!right_values) return core::Result<AuthoredStateBranch>::failure(right_values.error());

    std::map<StateKey, std::size_t> difference_indexes;
    for (std::size_t index = 0U; index < reviewed.differences.size(); ++index) {
        const StateDifference& difference = reviewed.differences[index];
        difference_indexes.emplace(StateKey{difference.target_kind,
            difference.target_record_id, difference.target_asset_id,
            difference.property}, index);
    }

    AuthoredStateBranch merged;
    merged.id = merged_branch_id;
    merged.name = std::move(name);
    merged.parent_branch_id = reviewed.common_ancestor_branch_id;
    merged.applicability = std::move(applicability);
    Id next_delta_id = 1U;
    for (const auto& [key, common_value] : common_values.value()) {
        const auto left = left_values.value().find(key);
        const auto right = right_values.value().find(key);
        if (left == left_values.value().end() || right == right_values.value().end()) {
            return core::Result<AuthoredStateBranch>::failure(stale(
                "authored state targets changed during merge resolution"));
        }

        const StateValue* selected_value = nullptr;
        if (same_value(left->second, right->second)) {
            selected_value = &left->second;
        } else {
            const auto difference = difference_indexes.find(key);
            if (difference == difference_indexes.end() ||
                !selected_sides[difference->second].has_value()) {
                return core::Result<AuthoredStateBranch>::failure(stale(
                    "state merge comparison omitted a differing value"));
            }
            switch (*selected_sides[difference->second]) {
            case StateMergeSide::common_ancestor:
                selected_value = &common_value;
                break;
            case StateMergeSide::left:
                selected_value = &left->second;
                break;
            case StateMergeSide::right:
                selected_value = &right->second;
                break;
            }
        }

        if (!same_value(common_value, *selected_value)) {
            AuthoredStateDelta delta;
            delta.id = next_delta_id++;
            delta.target_kind = std::get<0>(key);
            delta.target_record_id = std::get<1>(key);
            delta.target_asset_id = std::get<2>(key);
            delta.property = std::get<3>(key);
            delta.before = common_value;
            delta.after = *selected_value;
            merged.deltas.push_back(std::move(delta));
        }
    }
    if (auto valid = merged.validate(); !valid) {
        return core::Result<AuthoredStateBranch>::failure(valid.error().with_context(
            "resolved state merge branch"));
    }
    return core::Result<AuthoredStateBranch>::success(std::move(merged));
}

} // namespace carto::condition
