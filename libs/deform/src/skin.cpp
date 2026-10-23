#include <carto/deform/skin.hpp>

#include <carto/deform/detail/math_ops.hpp>

#include <carto/core/diagnostic.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace carto::deform {

namespace {

core::Result<void> validate_skin_input(const SkinEvaluationInput& input) {
    if (input.binding == nullptr || input.poses == nullptr) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "skin evaluation requires a binding and a joint pose set"));
    }
    const core::Result<void> binding_valid = input.binding->validate();
    if (!binding_valid) {
        return binding_valid;
    }
    if (input.mesh_digest_attested && input.mesh_content_digest != input.binding->mesh_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "posed positions do not match the binding's source mesh digest"));
    }
    if (input.bind_positions.empty() ||
        input.bind_positions.size() >
            static_cast<std::size_t>(kinematic::SkinBinding::kMaxVertexIndex + 1U)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "bind position stream is empty or exceeds the vertex bound"));
    }
    for (const core::Vec3d& position : input.bind_positions) {
        if (!position.finite()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "bind positions must be finite"));
        }
    }
    for (const auto& [vertex_index, influences] : input.binding->influences) {
        if (vertex_index >= input.bind_positions.size()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "binding references a vertex outside the position stream"));
        }
        for (const kinematic::JointInfluence& influence : influences) {
            if (input.poses->world_rest.find(influence.joint) ==
                    input.poses->world_rest.end() ||
                input.poses->world_pose.find(influence.joint) ==
                    input.poses->world_pose.end()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "binding references a joint missing from the pose set"));
            }
        }
    }
    return core::Result<void>::success();
}

// Bind-space point of one vertex moved into model space; vertices absent from
// the influence map are rigid and pass through the bind transform only.
core::Vec3d model_point(
    const SkinEvaluationInput& input,
    std::size_t vertex_index) {
    return detail::transform_point(
        input.binding->mesh_bind_transform, input.bind_positions[vertex_index]);
}

} // namespace

std::string_view skin_method_name(SkinMethod method) noexcept {
    switch (method) {
    case SkinMethod::linear_blend:
        return "linear_blend";
    case SkinMethod::dual_quaternion:
        return "dual_quaternion";
    }
    return "unknown";
}

core::Result<SkinEvaluationResult> evaluate_lbs(const SkinEvaluationInput& input) {
    const core::Result<void> input_valid = validate_skin_input(input);
    if (!input_valid) {
        return core::Result<SkinEvaluationResult>::failure(input_valid.error());
    }

    SkinEvaluationResult result;
    result.positions.reserve(input.bind_positions.size());
    for (std::size_t vertex_index = 0U; vertex_index < input.bind_positions.size();
         ++vertex_index) {
        const core::Vec3d base = model_point(input, vertex_index);
        const auto influences = input.binding->influences.find(
            static_cast<std::uint32_t>(vertex_index));
        if (influences == input.binding->influences.end()) {
            result.positions.push_back(base);
            continue;
        }
        core::Vec3d accumulated{0.0, 0.0, 0.0};
        for (const kinematic::JointInfluence& influence : influences->second) {
            const core::Transform& rest =
                input.poses->world_rest.at(influence.joint);
            const core::Transform& pose =
                input.poses->world_pose.at(influence.joint);
            const core::Vec3d joint_space =
                detail::inverse_transform_point(rest, base);
            const core::Vec3d skinned = detail::transform_point(pose, joint_space);
            accumulated = accumulated + skinned * influence.weight;
        }
        if (!accumulated.finite()) {
            return core::Result<SkinEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "linear blend produced a non-finite position"));
        }
        result.positions.push_back(accumulated);
    }
    return core::Result<SkinEvaluationResult>::success(std::move(result));
}

core::Result<SkinEvaluationResult> evaluate_dqs(const SkinEvaluationInput& input) {
    const core::Result<void> input_valid = validate_skin_input(input);
    if (!input_valid) {
        return core::Result<SkinEvaluationResult>::failure(input_valid.error());
    }

    // Precompute one rigid delta dual quaternion per referenced joint.
    std::map<kinematic::JointId, DualQuaternion> joint_deltas;
    for (const auto& [vertex_index, influences] : input.binding->influences) {
        (void)vertex_index;
        for (const kinematic::JointInfluence& influence : influences) {
            if (joint_deltas.find(influence.joint) != joint_deltas.end()) {
                continue;
            }
            const core::Transform& rest =
                input.poses->world_rest.at(influence.joint);
            const core::Transform& pose =
                input.poses->world_pose.at(influence.joint);
            const core::Result<core::Transform> delta =
                rigid_delta_transform(pose, rest);
            if (!delta) {
                return core::Result<SkinEvaluationResult>::failure(delta.error());
            }
            const core::Result<DualQuaternion> dual =
                dual_quaternion_from_rigid(delta.value());
            if (!dual) {
                return core::Result<SkinEvaluationResult>::failure(dual.error());
            }
            joint_deltas.emplace(influence.joint, dual.value());
        }
    }

    SkinEvaluationResult result;
    result.positions.reserve(input.bind_positions.size());
    for (std::size_t vertex_index = 0U; vertex_index < input.bind_positions.size();
         ++vertex_index) {
        const core::Vec3d base = model_point(input, vertex_index);
        const auto influences = input.binding->influences.find(
            static_cast<std::uint32_t>(vertex_index));
        if (influences == input.binding->influences.end()) {
            result.positions.push_back(base);
            continue;
        }
        // Deterministic hemisphere reference: the highest-weight influence,
        // ties broken by the lowest joint id (influences are joint-sorted).
        DualQuaternion reference = joint_deltas.at(influences->second.front().joint);
        double reference_weight = -1.0;
        for (const kinematic::JointInfluence& influence : influences->second) {
            if (influence.weight > reference_weight) {
                reference_weight = influence.weight;
                reference = joint_deltas.at(influence.joint);
            }
        }
        DualQuaternion blended{{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}};
        for (const kinematic::JointInfluence& influence : influences->second) {
            DualQuaternion term = joint_deltas.at(influence.joint) * influence.weight;
            if (real_dot(reference, term) < 0.0) {
                term.real = detail::quaternion_negated(term.real);
                term.dual = detail::quaternion_negated(term.dual);
            }
            blended = blended + term;
        }
        const DualQuaternion unit = normalized(blended);
        if (!unit.real.finite() || !unit.dual.finite()) {
            return core::Result<SkinEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "dual quaternion blend collapsed to zero magnitude"));
        }
        const core::Vec3d skinned =
            detail::transform_point(rigid_from_dual_quaternion(unit), base);
        if (!skinned.finite()) {
            return core::Result<SkinEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "dual quaternion skinning produced a non-finite position"));
        }
        result.positions.push_back(skinned);
    }
    return core::Result<SkinEvaluationResult>::success(std::move(result));
}

core::Result<DeviationSummary> compare_skin_methods(const SkinEvaluationInput& input) {
    const core::Result<SkinEvaluationResult> lbs = evaluate_lbs(input);
    if (!lbs) {
        return core::Result<DeviationSummary>::failure(lbs.error());
    }
    const core::Result<SkinEvaluationResult> dqs = evaluate_dqs(input);
    if (!dqs) {
        return core::Result<DeviationSummary>::failure(dqs.error());
    }
    return compute_deviation(dqs.value().positions, lbs.value().positions);
}

} // namespace carto::deform
