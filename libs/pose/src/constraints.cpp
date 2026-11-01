#include <carto/pose/constraints.hpp>

#include <carto/pose/rotation.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace carto::pose {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool active_at(const character::ConstraintDefinition& constraint,
               const character::RationalTime& time) {
    return character::compare(constraint.validity_start, time) !=
               std::strong_ordering::greater &&
           character::compare(time, constraint.validity_end) !=
               std::strong_ordering::greater;
}

// A blend of local transforms under the CPU reference convention:
// translation/scale lerp, rotation nlerp, `t` clamped to [0, 1].
core::Transform blend_transform(const core::Transform& from, const core::Transform& to,
                                double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    core::Transform result;
    result.translation = from.translation + (to.translation - from.translation) * clamped;
    result.rotation = nlerp(from.rotation, to.rotation, clamped);
    result.scale = from.scale + (to.scale - from.scale) * clamped;
    return result;
}

// q and -q represent the same orientation. The rotation-angle helper expects
// the coordinate policy's canonical non-negative-w hemisphere.
double orientation_residual(core::Quaternion source, core::Quaternion target) {
    auto delta = (source.normalized() * inverse(target.normalized())).normalized();
    if (delta.w < 0.0) delta = {-delta.x, -delta.y, -delta.z, -delta.w};
    return rotation_angle(delta);
}

struct Evaluator {
    const character::ConstraintGraph& constraints;
    const kinematic::KinematicGraph& graph;
    const ConstraintEvalContext& context;
    LocalPose current; // base pose plus every constraint applied so far

    const kinematic::JointLimit* limit_for(kinematic::JointId joint) const {
        return context.limits == nullptr ? nullptr : context.limits->find(joint);
    }

    core::Transform current_local(kinematic::JointId joint) const {
        const auto override = current.find(joint);
        return override != current.end() ? override->second : graph.find(joint)->rest;
    }

    // Ancestor chain root-first from `root` down to `tip`, both inclusive.
    core::Result<std::vector<kinematic::JointId>> chain_between(
        kinematic::JointId root, kinematic::JointId tip) const {
        std::vector<kinematic::JointId> chain;
        kinematic::JointId cursor = tip;
        while (true) {
            chain.push_back(cursor);
            if (cursor == root) break;
            const kinematic::KinematicNode* node = graph.find(cursor);
            if (!node->parent.has_value()) {
                return core::Result<std::vector<kinematic::JointId>>::failure(validation(
                    "constraint chain root is not an ancestor of the target joint"));
            }
            cursor = node->parent.value();
        }
        std::reverse(chain.begin(), chain.end());
        return core::Result<std::vector<kinematic::JointId>>::success(std::move(chain));
    }

    // Applies one constraint to `pose` (which may be the live pose or a
    // scratch copy for blending), recording the outcome. Does not mutate
    // arbitration state.
    core::Result<ConstraintOutcome> apply(
        const character::ConstraintDefinition& constraint, LocalPose& pose) {
        switch (constraint.type) {
        case character::ConstraintType::ik_pin:
        case character::ConstraintType::contact: {
            if (!constraint.source.is_joint() || !constraint.target.is_joint()) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "CPU reference IK constraints require joint referents; "
                    "resolve control goals to joint space first"));
            }
            const auto target_position =
                context.pin_targets.find(constraint.id);
            if (target_position == context.pin_targets.end()) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "pin/contact constraint has no authored goal position"));
            }
            auto chain = chain_between(constraint.source.joint, constraint.target.joint);
            if (!chain) return core::Result<ConstraintOutcome>::failure(chain.error());
            JacobianIkRequest request;
            request.chain = std::move(chain.value());
            request.target = target_position->second;
            request.budget = context.budget;
            request.allow_partial = true; // receipt-level proposal semantics
            auto solved = solve_chain_jacobian(graph, pose, context.limits, request);
            if (!solved) return core::Result<ConstraintOutcome>::failure(solved.error());
            for (auto& [joint, transform] : solved.value().solved_locals) {
                pose[joint] = std::move(transform);
            }
            ConstraintOutcome outcome;
            outcome.residual = solved.value().residual;
            outcome.iterations = solved.value().iterations;
            outcome.converged = solved.value().converged &&
                (constraint.type != character::ConstraintType::contact ||
                 solved.value().residual <= constraint.tolerance);
            return core::Result<ConstraintOutcome>::success(std::move(outcome));
        }
        case character::ConstraintType::orient: {
            if (!constraint.source.is_joint() || !constraint.target.is_joint()) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "orient constraints require joint referents on this path"));
            }
            auto fk = evaluate_fk(graph, pose);
            if (!fk) return core::Result<ConstraintOutcome>::failure(fk.error());
            const core::Quaternion target_world =
                fk.value().world.at(constraint.target.joint).rotation.normalized();
            const core::Quaternion source_world =
                fk.value().world.at(constraint.source.joint).rotation.normalized();
            const core::Quaternion delta =
                (source_world * inverse(target_world)).normalized();
            const core::Transform target_local = current_local(constraint.target.joint);
            const kinematic::KinematicNode* target_node = graph.find(constraint.target.joint);
            const core::Quaternion parent_rotation = target_node->parent.has_value()
                ? fk.value().world.at(target_node->parent.value()).rotation.normalized()
                : fk.value().root_frame.rotation.normalized();
            core::Transform solved_local = target_local;
            solved_local.rotation = ((inverse(parent_rotation) * delta * parent_rotation) *
                                     target_local.rotation)
                                        .normalized();
            pose[constraint.target.joint] = solved_local;
            auto aligned = evaluate_fk(graph, pose);
            if (!aligned) return core::Result<ConstraintOutcome>::failure(aligned.error());
            ConstraintOutcome outcome;
            outcome.residual = orientation_residual(
                aligned.value().world.at(constraint.source.joint).rotation,
                aligned.value().world.at(constraint.target.joint).rotation);
            if (!std::isfinite(outcome.residual))
                return core::Result<ConstraintOutcome>::failure(validation(
                    "orientation solver residual is not finite"));
            outcome.iterations = 1U;
            outcome.converged = outcome.residual <=
                std::max(constraint.tolerance, context.budget.convergence_tolerance);
            return core::Result<ConstraintOutcome>::success(std::move(outcome));
        }
        case character::ConstraintType::fk_blend: {
            if (!constraint.target.is_joint()) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "fk_blend constraints require a joint referent on this path"));
            }
            const auto goal = context.blend_targets.find(constraint.id);
            const auto weight = context.blend_weights.find(constraint.id);
            if (goal == context.blend_targets.end() || weight == context.blend_weights.end()) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "fk_blend constraint has no authored goal pose and weight"));
            }
            if (!std::isfinite(weight->second) ||
                weight->second < 0.0 || weight->second > 1.0) {
                return core::Result<ConstraintOutcome>::failure(invalid(
                    "fk_blend weight must be finite and within [0, 1]"));
            }
            pose[constraint.target.joint] = blend_transform(
                current_local(constraint.target.joint), goal->second, weight->second);
            return core::Result<ConstraintOutcome>::success(
                ConstraintOutcome{0.0, 1U, true, false});
        }
        case character::ConstraintType::limit: {
            if (!constraint.target.is_joint()) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "limit constraints require a joint referent on this path"));
            }
            if (context.limits == nullptr) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "limit constraints require an authored JointLimitSet"));
            }
            const kinematic::JointLimit* limit = context.limits->find(constraint.target.joint);
            if (limit == nullptr) {
                return core::Result<ConstraintOutcome>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "limit constraint targets a joint with no authored limit"));
            }
            const core::Transform base = current_local(constraint.target.joint);
            const core::Transform rest = graph.find(constraint.target.joint)->rest;
            const core::Quaternion relative =
                (inverse(rest.rotation) * base.rotation).normalized();
            core::Vec3d euler = to_euler_xyz(relative);
            core::Vec3d translation = base.translation;
            double violation = 0.0;
            if (limit->angular_enabled) {
                for (int axis = 0; axis < 3; ++axis) {
                    const double value = axis == 0 ? euler.x : (axis == 1 ? euler.y : euler.z);
                    const double low = axis == 0 ? limit->angular_min.x
                        : (axis == 1 ? limit->angular_min.y : limit->angular_min.z);
                    const double high = axis == 0 ? limit->angular_max.x
                        : (axis == 1 ? limit->angular_max.y : limit->angular_max.z);
                    violation = std::max(violation, std::max(low - value, value - high));
                }
                euler.x = std::clamp(euler.x, limit->angular_min.x, limit->angular_max.x);
                euler.y = std::clamp(euler.y, limit->angular_min.y, limit->angular_max.y);
                euler.z = std::clamp(euler.z, limit->angular_min.z, limit->angular_max.z);
            }
            if (limit->linear_enabled) {
                double* components[] = {&translation.x, &translation.y, &translation.z};
                const double* lows[] = {&limit->linear_min.x, &limit->linear_min.y, &limit->linear_min.z};
                const double* highs[] = {&limit->linear_max.x, &limit->linear_max.y, &limit->linear_max.z};
                for (int axis = 0; axis < 3; ++axis) {
                    const double value = *components[axis];
                    violation = std::max(violation,
                        std::max(*lows[axis] - value, value - *highs[axis]));
                    *components[axis] = std::clamp(value, *lows[axis], *highs[axis]);
                }
            }
            core::Transform solved = base;
            solved.rotation = (rest.rotation * from_euler_xyz(euler)).normalized();
            pose[constraint.target.joint] = solved;
            ConstraintOutcome outcome;
            outcome.residual = std::max(0.0, violation);
            outcome.iterations = 1U;
            outcome.converged = violation <= std::max(constraint.tolerance, 1e-12);
            return core::Result<ConstraintOutcome>::success(std::move(outcome));
        }
        }
        return core::Result<ConstraintOutcome>::failure(invalid(
            "constraint type is invalid"));
    }
};

using GroupKey = character::ConstraintTargetRef;

// Deterministic group order: priority descending, then id ascending.
bool member_before(const character::ConstraintDefinition& left,
                   const character::ConstraintDefinition& right) {
    if (left.priority != right.priority) return left.priority > right.priority;
    return left.id.value < right.id.value;
}

} // namespace

core::Result<PoseReceipt> evaluate_constraints(
    const character::ConstraintGraph& constraints,
    const kinematic::KinematicGraph& graph,
    const ConstraintEvalContext& context) {
    if (auto result = constraints.validate(); !result) {
        return core::Result<PoseReceipt>::failure(result.error().with_context(
            "constraint evaluation requires a valid constraint graph"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<PoseReceipt>::failure(result.error().with_context(
            "constraint evaluation requires a valid kinematic graph"));
    }
    if (auto result = context.time.validate(); !result) {
        return core::Result<PoseReceipt>::failure(result.error());
    }
    if (auto result = context.budget.validate(); !result) {
        return core::Result<PoseReceipt>::failure(result.error());
    }
    for (const auto& [joint, transform] : context.base_locals) {
        if (!joint || graph.find(joint) == nullptr || !transform.finite()) {
            return core::Result<PoseReceipt>::failure(invalid(
                "base pose locals must be finite and stay inside the graph"));
        }
    }
    for (const auto& [constraint_id, weight] : context.blend_weights) {
        if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0) {
            return core::Result<PoseReceipt>::failure(invalid(
                "fk_blend weights must be finite and within [0, 1]"));
        }
        static_cast<void>(constraint_id);
    }

    Evaluator evaluator{constraints, graph, context, context.base_locals};
    PoseReceipt receipt;
    receipt.sampled_time = context.time;

    // Stage passes in the authored stage order.
    for (std::uint8_t stage_value = 0U;
         stage_value <= static_cast<std::uint8_t>(character::ConstraintStage::post_simulation);
         ++stage_value) {
        const auto stage = static_cast<character::ConstraintStage>(stage_value);
        std::vector<const character::ConstraintDefinition*> active;
        for (const auto& [identity, constraint] : constraints.constraints()) {
            static_cast<void>(identity);
            if (constraint.stage != stage) continue;
            if (!character::same_timebase(constraint.validity_start, context.time)) {
                return core::Result<PoseReceipt>::failure(invalid(
                    "sampled time must share the constraint window timebase"));
            }
            if (!active_at(constraint, context.time)) continue;
            // Resolve every active referent before arbitration. A superseded
            // member must not hide a dangling joint or an unsupported control.
            if (!constraint.source.is_joint() || !constraint.target.is_joint()) {
                return core::Result<PoseReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "CPU constraint evaluation requires joint referents"));
            }
            if (!graph.find(constraint.source.joint) || !graph.find(constraint.target.joint)) {
                return core::Result<PoseReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "active constraint references a joint absent from the graph"));
            }
            // Authored-goal completeness is validated for every active
            // constraint up front, before arbitration: a missing goal is an
            // authoring error and fails closed even when the constraint would
            // be superseded (mirrors the timebase check above).
            if ((constraint.type == character::ConstraintType::ik_pin ||
                    constraint.type == character::ConstraintType::contact) &&
                !context.pin_targets.contains(constraint.id)) {
                return core::Result<PoseReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "pin/contact constraint has no authored goal position"));
            }
            if ((constraint.type == character::ConstraintType::ik_pin ||
                    constraint.type == character::ConstraintType::contact) &&
                !context.pin_targets.at(constraint.id).finite()) {
                return core::Result<PoseReceipt>::failure(invalid(
                    "pin/contact constraint goal must be finite"));
            }
            if (constraint.type == character::ConstraintType::fk_blend &&
                (!context.blend_targets.contains(constraint.id) ||
                    !context.blend_weights.contains(constraint.id))) {
                return core::Result<PoseReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "fk_blend constraint has no authored goal pose and weight"));
            }
            if (constraint.type == character::ConstraintType::fk_blend &&
                !context.blend_targets.at(constraint.id).finite()) {
                return core::Result<PoseReceipt>::failure(invalid(
                    "fk_blend constraint goal must be finite"));
            }
            active.push_back(&constraint);
        }
        if (active.empty()) continue;
        ++receipt.stage_passes;

        // Kahn topological order over depends_on; ready set tie-broken by
        // (priority descending, id ascending) for deterministic application.
        std::map<character::ConstraintId, std::size_t> indegree;
        std::map<character::ConstraintId, std::vector<character::ConstraintId>> dependents;
        std::set<character::ConstraintId> active_ids;
        for (const character::ConstraintDefinition* constraint : active) {
            active_ids.insert(constraint->id);
            indegree.emplace(constraint->id, 0U);
        }
        for (const character::ConstraintDefinition* constraint : active) {
            if (constraint->depends_on.has_value() &&
                active_ids.contains(constraint->depends_on.value())) {
                ++indegree[constraint->id];
                dependents[constraint->depends_on.value()].push_back(constraint->id);
            }
        }
        std::vector<const character::ConstraintDefinition*> order;
        std::vector<character::ConstraintId> ready;
        for (const auto& [identity, degree] : indegree) {
            if (degree == 0U) ready.push_back(identity);
        }
        std::map<character::ConstraintId, const character::ConstraintDefinition*> by_id;
        for (const character::ConstraintDefinition* constraint : active) {
            by_id.emplace(constraint->id, constraint);
        }
        while (!ready.empty()) {
            std::sort(ready.begin(), ready.end(),
                [&](character::ConstraintId left, character::ConstraintId right) {
                    const auto& l = *by_id[left];
                    const auto& r = *by_id[right];
                    return member_before(l, r);
                });
            const character::ConstraintId current_id = ready.front();
            ready.erase(ready.begin());
            order.push_back(by_id[current_id]);
            for (const character::ConstraintId next : dependents[current_id]) {
                if (--indegree[next] == 0U) ready.push_back(next);
            }
        }
        if (order.size() != active.size()) {
            return core::Result<PoseReceipt>::failure(validation(
                "constraint depends_on cycle survived graph validation"));
        }

        // Group by target referent for arbitration; a group applies
        // atomically at the topo position of its highest-priority member.
        std::map<GroupKey, std::vector<const character::ConstraintDefinition*>> groups;
        for (const character::ConstraintDefinition* constraint : order) {
            groups[constraint->target].push_back(constraint);
        }
        std::set<character::ConstraintId> applied;
        const auto arbitration_rule = [&](
            const std::vector<const character::ConstraintDefinition*>& members)
            -> core::Result<character::ArbitrationRule> {
            // A single atomic group cannot silently discard any of its
            // declared pairwise rules. Mixed rules need a group policy that
            // this evaluator does not yet represent.
            std::optional<character::ArbitrationRule> rule;
            for (std::size_t left = 0U; left < members.size(); ++left) {
                for (std::size_t right = left + 1U; right < members.size(); ++right) {
                    if (members[left]->type == members[right]->type) continue;
                    const auto low = members[left]->type < members[right]->type
                        ? members[left]->type : members[right]->type;
                    const auto high = members[left]->type < members[right]->type
                        ? members[right]->type : members[left]->type;
                    for (const character::ConstraintArbitration& arbitration :
                         constraints.arbitrations()) {
                        if (arbitration.first == low && arbitration.second == high) {
                            if (rule && *rule != arbitration.rule) {
                                return core::Result<character::ArbitrationRule>::failure(validation(
                                    "conflicting arbitration rules in one target group, constraints " +
                                    std::to_string(members[left]->id.value) + " and " +
                                    std::to_string(members[right]->id.value)));
                            }
                            rule = arbitration.rule;
                        }
                    }
                }
            }
            return core::Result<character::ArbitrationRule>::success(
                rule.value_or(character::ArbitrationRule::none));
        };
        for (const character::ConstraintDefinition* constraint : order) {
            if (applied.contains(constraint->id)) continue;
            std::vector<const character::ConstraintDefinition*> members =
                groups.at(constraint->target);
            // groups preserve the already computed dependency-aware topo
            // order. Sorting again by priority could reverse a dependency.

            bool same_type = true;
            for (std::size_t index = 1U; index < members.size(); ++index) {
                if (members[index]->type != members.front()->type) same_type = false;
            }
            auto selected_rule = arbitration_rule(members);
            if (!selected_rule) return core::Result<PoseReceipt>::failure(selected_rule.error());
            const character::ArbitrationRule rule = selected_rule.value();

            for (const auto* member : members) {
                if (!member->depends_on) continue;
                const auto& predecessor = constraints.constraints().at(*member->depends_on);
                const bool same_group = predecessor.stage == stage &&
                    predecessor.target == member->target && active_ids.contains(predecessor.id);
                if (same_group) {
                    if (!same_type && rule != character::ArbitrationRule::sequential) {
                        return core::Result<PoseReceipt>::failure(validation(
                            "arbitration cannot supersede or independently blend an in-group dependency"));
                    }
                    continue;
                }
                const auto prior = receipt.outcomes.find(predecessor.id);
                if (predecessor.stage > stage || prior == receipt.outcomes.end() || prior->second.superseded) {
                    return core::Result<PoseReceipt>::failure(validation(
                        "constraint " + std::to_string(member->id.value) +
                        " has an inactive, superseded, later-stage or interleaved dependency " +
                        std::to_string(predecessor.id.value)));
                }
            }

            if (members.size() == 1U) {
                auto outcome = evaluator.apply(*members.front(), evaluator.current);
                if (!outcome) return core::Result<PoseReceipt>::failure(outcome.error());
                receipt.outcomes.emplace(members.front()->id, std::move(outcome.value()));
                applied.insert(members.front()->id);
                continue;
            }

            if (same_type) {
                // Same-type members: sequential application in group order.
                for (const character::ConstraintDefinition* member : members) {
                    auto outcome = evaluator.apply(*member, evaluator.current);
                    if (!outcome) return core::Result<PoseReceipt>::failure(outcome.error());
                    receipt.outcomes.emplace(member->id, std::move(outcome.value()));
                    applied.insert(member->id);
                }
                continue;
            }
            if (rule == character::ArbitrationRule::none) {
                return core::Result<PoseReceipt>::failure(validation(
                    "conflicting target group lacks declared arbitration"));
            }
            if (rule == character::ArbitrationRule::priority_wins) {
                auto outcome = evaluator.apply(*members.front(), evaluator.current);
                if (!outcome) return core::Result<PoseReceipt>::failure(outcome.error());
                receipt.outcomes.emplace(members.front()->id, std::move(outcome.value()));
                applied.insert(members.front()->id);
                for (std::size_t index = 1U; index < members.size(); ++index) {
                    ConstraintOutcome superseded;
                    superseded.superseded = true;
                    receipt.outcomes.emplace(members[index]->id, superseded);
                    applied.insert(members[index]->id);
                }
                continue;
            }
            if (rule == character::ArbitrationRule::sequential) {
                for (const character::ConstraintDefinition* member : members) {
                    auto outcome = evaluator.apply(*member, evaluator.current);
                    if (!outcome) return core::Result<PoseReceipt>::failure(outcome.error());
                    receipt.outcomes.emplace(member->id, std::move(outcome.value()));
                    applied.insert(member->id);
                }
                continue;
            }
            // Individual pre-blend residuals cannot certify the blended
            // pose. Refuse until final-pose residual revalidation exists.
            return core::Result<PoseReceipt>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "blend arbitration requires final-pose residual revalidation"));
        }
    }

    // Later constraints may change an earlier measured goal. Recompute
    // position and orientation residuals on the returned pose. Preserve a
    // solver's failure even if another constraint later happens to meet the
    // goal: the single convergence bit cannot distinguish those two facts.
    auto final_pose = evaluate_fk(graph, evaluator.current);
    if (!final_pose) return core::Result<PoseReceipt>::failure(final_pose.error());
    for (const auto& [joint, transform] : final_pose.value().world) {
        (void)joint;
        if (!transform.finite()) return core::Result<PoseReceipt>::failure(validation(
            "final world pose is not finite"));
    }
    for (auto& [identity, outcome] : receipt.outcomes) {
        const auto& definition = constraints.constraints().at(identity);
        if (outcome.superseded) continue;
        if (definition.type == character::ConstraintType::contact ||
            definition.type == character::ConstraintType::ik_pin) {
            const auto target = context.pin_targets.find(identity);
            if (target == context.pin_targets.end() || !target->second.finite()) {
                return core::Result<PoseReceipt>::failure(validation(
                    "final pin/contact measurement requires its finite authored target"));
            }
            const auto position = final_pose.value().world.at(definition.target.joint).translation;
            outcome.residual = (position - target->second).length();
            if (!std::isfinite(outcome.residual)) {
                return core::Result<PoseReceipt>::failure(validation(
                    "final pin/contact residual is not finite"));
            }
            const double tolerance = definition.type == character::ConstraintType::contact
                ? definition.tolerance : std::max(context.budget.convergence_tolerance, 1e-12);
            outcome.converged = outcome.converged && outcome.residual <= tolerance;
        } else if (definition.type == character::ConstraintType::orient) {
            const auto source = final_pose.value().world.at(definition.source.joint).rotation;
            const auto target = final_pose.value().world.at(definition.target.joint).rotation;
            outcome.residual = orientation_residual(source, target);
            if (!std::isfinite(outcome.residual)) {
                return core::Result<PoseReceipt>::failure(validation(
                    "final orientation residual is not finite"));
            }
            outcome.converged = outcome.converged && outcome.residual <=
                std::max(definition.tolerance, context.budget.convergence_tolerance);
        }
    }

    // Final hierarchy verification against enabled authored limits.
    if (context.limits != nullptr) {
        for (const auto& [joint, limit] : context.limits->limits()) {
            if (graph.find(joint) == nullptr) continue;
            const core::Transform base = evaluator.current_local(joint);
            const core::Transform rest = graph.find(joint)->rest;
            bool violation = false;
            if (limit.angular_enabled) {
                const core::Quaternion relative =
                    (inverse(rest.rotation) * base.rotation).normalized();
                const core::Vec3d euler = to_euler_xyz(relative);
                const double* values[] = {&euler.x, &euler.y, &euler.z};
                const double* lows[] = {&limit.angular_min.x, &limit.angular_min.y, &limit.angular_min.z};
                const double* highs[] = {&limit.angular_max.x, &limit.angular_max.y, &limit.angular_max.z};
                for (int axis = 0; axis < 3; ++axis) {
                    if (*values[axis] < *lows[axis] - 1e-12 ||
                        *values[axis] > *highs[axis] + 1e-12) {
                        violation = true;
                    }
                }
            }
            if (limit.linear_enabled) {
                const double* values[] = {&base.translation.x, &base.translation.y, &base.translation.z};
                const double* lows[] = {&limit.linear_min.x, &limit.linear_min.y, &limit.linear_min.z};
                const double* highs[] = {&limit.linear_max.x, &limit.linear_max.y, &limit.linear_max.z};
                for (int axis = 0; axis < 3; ++axis) {
                    if (*values[axis] < *lows[axis] - 1e-12 ||
                        *values[axis] > *highs[axis] + 1e-12) {
                        violation = true;
                    }
                }
            }
            if (violation) receipt.limit_violations.push_back(joint);
        }
    }

    receipt.solved_locals = std::move(evaluator.current);
    receipt.all_converged = true;
    for (const auto& [identity, outcome] : receipt.outcomes) {
        static_cast<void>(identity);
        const bool unverified_contact = outcome.superseded &&
            constraints.constraints().at(identity).type == character::ConstraintType::contact;
        if ((!outcome.converged && !outcome.superseded) || unverified_contact)
            receipt.all_converged = false;
    }
    return core::Result<PoseReceipt>::success(std::move(receipt));
}

} // namespace carto::pose
