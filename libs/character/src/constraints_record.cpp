#include <carto/character/constraints_record.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_type(ConstraintType type) {
    switch (type) {
    case ConstraintType::ik_pin:
    case ConstraintType::fk_blend:
    case ConstraintType::contact:
    case ConstraintType::orient:
    case ConstraintType::limit:
        return true;
    }
    return false;
}

bool valid_stage(ConstraintStage stage) {
    switch (stage) {
    case ConstraintStage::preprocess:
    case ConstraintStage::pre_deformation:
    case ConstraintStage::post_deformation:
    case ConstraintStage::post_simulation:
        return true;
    }
    return false;
}

} // namespace

std::string_view constraint_type_name(ConstraintType type) noexcept {
    switch (type) {
    case ConstraintType::ik_pin: return "ik-pin";
    case ConstraintType::fk_blend: return "fk-blend";
    case ConstraintType::contact: return "contact";
    case ConstraintType::orient: return "orient";
    case ConstraintType::limit: return "limit";
    }
    return "unknown";
}

std::string_view constraint_stage_name(ConstraintStage stage) noexcept {
    switch (stage) {
    case ConstraintStage::preprocess: return "preprocess";
    case ConstraintStage::pre_deformation: return "pre-deformation";
    case ConstraintStage::post_deformation: return "post-deformation";
    case ConstraintStage::post_simulation: return "post-simulation";
    }
    return "unknown";
}

std::string_view arbitration_rule_name(ArbitrationRule rule) noexcept {
    switch (rule) {
    case ArbitrationRule::none: return "none";
    case ArbitrationRule::priority_wins: return "priority-wins";
    case ArbitrationRule::blend: return "blend";
    case ArbitrationRule::sequential: return "sequential";
    }
    return "unknown";
}

core::Result<void> ConstraintTargetRef::validate() const {
    if ((joint.value != 0U) == (control.value != 0U)) {
        return core::Result<void>::failure(invalid(
            "constraint referent must set exactly one joint or control"));
    }
    return core::Result<void>::success();
}

bool ConstraintTargetRef::operator<(const ConstraintTargetRef& other) const noexcept {
    return std::tuple{joint.value, control.value} <
        std::tuple{other.joint.value, other.control.value};
}

core::Result<void> ConstraintDefinition::validate() const {
    if (!id || !valid_type(type) || !valid_stage(stage)) {
        return core::Result<void>::failure(invalid(
            "constraint identity, type, or stage is invalid"));
    }
    if (auto result = source.validate(); !result) return result;
    if (auto result = target.validate(); !result) return result;
    if (std::isfinite(tolerance) == false || tolerance < 0.0) {
        return core::Result<void>::failure(invalid(
            "constraint tolerance must be non-negative and finite"));
    }
    if (auto result = validity_start.validate(); !result) return result;
    if (auto result = validity_end.validate(); !result) return result;
    if (validity_start.ticks_per_second != validity_end.ticks_per_second ||
        compare(validity_end, validity_start) == std::strong_ordering::less) {
        return core::Result<void>::failure(validation(
            "constraint validity window must share one timebase and not invert"));
    }
    return core::Result<void>::success();
}

core::Result<void> ConstraintArbitration::validate() const {
    if (!valid_type(first) || !valid_type(second) || first >= second) {
        return core::Result<void>::failure(invalid(
            "constraint arbitration must name a valid unordered distinct type pair"));
    }
    switch (rule) {
    case ArbitrationRule::none:
    case ArbitrationRule::priority_wins:
    case ArbitrationRule::blend:
    case ArbitrationRule::sequential:
        break;
    default:
        return core::Result<void>::failure(invalid(
            "constraint arbitration rule is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ConstraintGraph::insert(ConstraintDefinition constraint) {
    if (auto result = constraint.validate(); !result) return result;
    if (constraints_.size() >= kMaxConstraints) {
        return core::Result<void>::failure(validation(
            "constraint graph has reached its bounded capacity"));
    }
    // Dependencies may reference constraints inserted later; validate()
    // enforces existence and acyclicity on the complete graph.
    if (!constraints_.emplace(constraint.id, std::move(constraint)).second) {
        return core::Result<void>::failure(invalid(
            "constraint identity is duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<void> ConstraintGraph::declare_arbitration(ConstraintArbitration arbitration) {
    if (auto result = arbitration.validate(); !result) return result;
    if (arbitrations_.size() >= kMaxArbitrations) {
        return core::Result<void>::failure(validation(
            "constraint arbitrations have reached their bounded capacity"));
    }
    if (!std::is_sorted(arbitrations_.begin(), arbitrations_.end(),
            [](const ConstraintArbitration& left, const ConstraintArbitration& right) {
                return std::tuple{left.first, left.second} <
                    std::tuple{right.first, right.second};
            })) {
        return core::Result<void>::failure(validation(
            "constraint arbitrations must remain sorted by type pair"));
    }
    const auto ordering = [](const ConstraintArbitration& value) {
        return std::tuple{value.first, value.second};
    };
    const auto position = std::lower_bound(
        arbitrations_.begin(), arbitrations_.end(), arbitration,
        [&](const ConstraintArbitration& left, const ConstraintArbitration& right) {
            return ordering(left) < ordering(right);
        });
    if (position != arbitrations_.end() && ordering(*position) == ordering(arbitration)) {
        return core::Result<void>::failure(invalid(
            "constraint arbitration for this type pair is already declared"));
    }
    arbitrations_.insert(position, arbitration);
    return core::Result<void>::success();
}

core::Result<void> ConstraintGraph::validate() const {
    if (constraints_.size() > kMaxConstraints || arbitrations_.size() > kMaxArbitrations) {
        return core::Result<void>::failure(validation(
            "constraint graph exceeds its bounded capacity"));
    }
    for (const auto& [identity, constraint] : constraints_) {
        if (identity != constraint.id) {
            return core::Result<void>::failure(validation(
                "constraint map identity does not match its record"));
        }
        if (auto result = constraint.validate(); !result) return result;
        if (constraint.depends_on.has_value() &&
            !constraints_.contains(constraint.depends_on.value())) {
            return core::Result<void>::failure(validation(
                "constraint depends_on references a missing constraint"));
        }
    }
    // Acyclic depends_on DAG.
    std::map<ConstraintId, std::vector<ConstraintId>> dependents;
    for (const auto& [identity, constraint] : constraints_) {
        if (constraint.depends_on.has_value()) {
            dependents[constraint.depends_on.value()].push_back(identity);
        }
    }
    std::map<ConstraintId, int> state; // 0 unvisited, 1 visiting, 2 done
    for (const auto& [identity, constraint] : constraints_) {
        static_cast<void>(constraint);
        state.emplace(identity, 0);
    }
    for (const auto& [identity, initial] : state) {
        if (initial != 0) continue;
        std::vector<ConstraintId> stack{identity};
        state[identity] = 1;
        while (!stack.empty()) {
            const ConstraintId current = stack.back();
            bool descended = false;
            const auto found = dependents.find(current);
            if (found != dependents.end()) {
                for (const ConstraintId next : found->second) {
                    if (state[next] == 1) {
                        return core::Result<void>::failure(validation(
                            "constraint depends_on DAG contains a cycle"));
                    }
                    if (state[next] == 0) {
                        state[next] = 1;
                        stack.push_back(next);
                        descended = true;
                        break;
                    }
                }
            }
            if (!descended) {
                state[current] = 2;
                stack.pop_back();
            }
        }
    }
    // Declared conflict arbitration: every pair of constraints sharing a
    // target referent with different types requires a non-none rule.
    std::vector<const ConstraintDefinition*> ordered;
    ordered.reserve(constraints_.size());
    for (const auto& [identity, constraint] : constraints_) {
        static_cast<void>(identity);
        ordered.push_back(&constraint);
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const ConstraintDefinition* left, const ConstraintDefinition* right) {
            return std::tuple{left->target, left->type, left->id} <
                std::tuple{right->target, right->type, right->id};
        });
    const auto arbitrated = [this](ConstraintType first, ConstraintType second) {
        const auto low = first < second ? first : second;
        const auto high = first < second ? second : first;
        return std::any_of(arbitrations_.begin(), arbitrations_.end(),
            [low, high](const ConstraintArbitration& arbitration) {
                return arbitration.first == low && arbitration.second == high &&
                    arbitration.rule != ArbitrationRule::none;
            });
    };
    for (std::size_t left = 0U; left < ordered.size(); ++left) {
        for (std::size_t right = left + 1U; right < ordered.size(); ++right) {
            if (!(ordered[left]->target == ordered[right]->target)) break;
            if (ordered[left]->type != ordered[right]->type &&
                !arbitrated(ordered[left]->type, ordered[right]->type)) {
                return core::Result<void>::failure(validation(
                    "conflicting constraints on one target require declared arbitration"));
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
