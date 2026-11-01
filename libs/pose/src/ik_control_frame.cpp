#include <carto/pose/ik_control_frame.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace carto::pose {
namespace {
core::Diagnostic invalid(const char* text) {
    return {core::ErrorCode::validation_failed, text};
}

bool identity(const std::string& value) {
    return !value.empty() && value.size() <= 128U &&
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c > 32U && c < 127U; }) &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32U && c < 127U; });
}

core::Result<void> validate_mapping(const IkControlRigidFrame& frame,
    const IkFkAuthoring& authored, const character::RigDefinition& rig,
    const kinematic::KinematicGraph& graph, const kinematic::JointLimitSet& limits) {
    if (auto result = authored.validate_against(rig, graph, limits); !result) return result;
    auto digest = authored.canonical_digest();
    const auto& transform = frame.graph_world_from_control;
    const auto& q = transform.rotation;
    const double norm = std::hypot(std::hypot(q.x, q.y), std::hypot(q.z, q.w));
    if (!digest || frame.authoring_digest != digest.value() ||
        !(rig.rest_basis == character::CoordinatePolicy{}) || !authored.ik_pole ||
        !identity(frame.frame_identity) || !identity(frame.scene_unit_identity) ||
        !transform.finite() || !std::isfinite(norm) || std::abs(norm - 1.0) > 1e-12 ||
        transform.scale.x != 1.0 || transform.scale.y != 1.0 || transform.scale.z != 1.0 ||
        !std::isfinite(frame.max_point_round_trip_error) || frame.max_point_round_trip_error <= 0.0 ||
        !std::isfinite(frame.max_direction_round_trip_error) || frame.max_direction_round_trip_error <= 0.0)
        return core::Result<void>::failure(invalid("invalid or unsupported asserted IK control frame"));
    constexpr std::uint8_t xyz = character::kAxisTranslateX | character::kAxisTranslateY | character::kAxisTranslateZ;
    for (auto id : {authored.ik_target, *authored.ik_pole}) {
        const auto* control = rig.control_graph.find(id);
        if (!control || control->allowed_axes != xyz || control->follow_space || !control->affected_joints.empty())
            return core::Result<void>::failure(invalid("IK point and direction require isolated XYZ-only controls without follow or joint offsets"));
        for (const auto& [other_id, other] : rig.control_graph.nodes()) {
            (void)other_id;
            if (other.follow_space && *other.follow_space == id)
                return core::Result<void>::failure(invalid("IK control with follow-space dependents is unsupported"));
        }
    }
    return core::Result<void>::success();
}

core::Result<core::Vec3d> read_xyz(const character::ControlGraph& graph,
    const ControlValues& values, character::ControlId id) {
    using Result = core::Result<core::Vec3d>;
    const auto found = values.find(id);
    const auto* control = graph.find(id);
    if (!control || found == values.end()) return Result::failure(invalid("missing IK control channels"));
    for (std::size_t axis = 0; axis < character::kAxisCount; ++axis) {
        const double value = found->second[axis];
        if (!std::isfinite(value) || (axis >= 3U && value != 0.0) ||
            (axis < 3U && (value < control->axis_minimum[axis] || value > control->axis_maximum[axis])))
            return Result::failure(invalid("IK control channels violate their axes or limits"));
    }
    return Result::success({found->second[0], found->second[1], found->second[2]});
}

core::Quaternion inverse_rotation(const core::Quaternion& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

} // namespace

core::Result<IkControlState> decode_ik_control_frame(
    const IkControlRigidFrame& frame, const IkFkAuthoring& authored,
    const character::RigDefinition& rig, const kinematic::KinematicGraph& graph,
    const kinematic::JointLimitSet& limits, const ControlValues& values) {
    using Result = core::Result<IkControlState>;
    if (auto checked = validate_mapping(frame, authored, rig, graph, limits); !checked)
        return Result::failure(checked.error());
    auto point = read_xyz(rig.control_graph, values, authored.ik_target);
    auto direction = read_xyz(rig.control_graph, values, *authored.ik_pole);
    if (!point) return Result::failure(point.error());
    if (!direction) return Result::failure(direction.error());
    const auto& transform = frame.graph_world_from_control;
    IkControlState result;
    result.target = transform.translation + transform.rotation.rotate(point.value());
    result.pole_vector = transform.rotation.rotate(direction.value());
    const double pole_length = result.pole_vector->length();
    if (!result.target.finite() || !result.pole_vector->finite() ||
        !std::isfinite(pole_length) || pole_length <= 1e-12)
        return Result::failure(invalid("decoded IK point or pole direction is invalid"));
    return Result::success(result);
}

core::Result<FkToIkControlFrameProposal> propose_fk_to_ik_control_frame(
    const IkControlRigidFrame& frame, const IkFkAuthoring& authored,
    const character::RigDefinition& rig, const kinematic::KinematicGraph& graph,
    const kinematic::JointLimitSet& limits, const LocalPose& locals, const ControlValues& current) {
    using Result = core::Result<FkToIkControlFrameProposal>;
    if (auto checked = validate_mapping(frame, authored, rig, graph, limits); !checked)
        return Result::failure(checked.error());
    // Validate all input identities and axes without applying the evaluation.
    if (auto checked = evaluate_controls(rig.control_graph, current); !checked)
        return Result::failure(checked.error());
    // Do not silently replace already out-of-limit role channels, even though
    // the generic evaluator would clamp them. Missing role values are allowed.
    for (auto id : {authored.ik_target, *authored.ik_pole}) {
        if (current.contains(id)) {
            auto checked = read_xyz(rig.control_graph, current, id);
            if (!checked) return Result::failure(checked.error());
        }
    }
    auto match = propose_ik_fk_switch(authored, rig, graph, limits, IkFkMode::ik, locals);
    if (!match) return Result::failure(match.error());
    if (!match.value().match.ik.pole_vector)
        return Result::failure(invalid("a straight chain has no matched pole direction"));
    const auto& transform = frame.graph_world_from_control;
    const auto inverse = inverse_rotation(transform.rotation);
    const core::Vec3d point = inverse.rotate(match.value().match.ik.target - transform.translation);
    const core::Vec3d direction = inverse.rotate(*match.value().match.ik.pole_vector);
    if (!point.finite() || !direction.finite()) return Result::failure(invalid("IK frame encoding is not finite"));
    auto candidate = current;
    candidate[authored.ik_target] = {point.x, point.y, point.z, 0.0, 0.0, 0.0};
    candidate[*authored.ik_pole] = {direction.x, direction.y, direction.z, 0.0, 0.0, 0.0};
    auto decoded = decode_ik_control_frame(frame, authored, rig, graph, limits, candidate);
    if (!decoded) return Result::failure(decoded.error());
    const double point_error = (decoded.value().target - match.value().match.ik.target).length();
    const double pole_error = (*decoded.value().pole_vector - *match.value().match.ik.pole_vector).length();
    if (!std::isfinite(point_error) || !std::isfinite(pole_error) ||
        point_error > frame.max_point_round_trip_error || pole_error > frame.max_direction_round_trip_error)
        return Result::failure(invalid("IK frame round-trip exceeds asserted error budget"));
    TwoBoneIkRequest request;
    request.root = authored.chain.root; request.mid = authored.chain.mid; request.effector = authored.chain.effector;
    request.target = decoded.value().target; request.pole_vector = decoded.value().pole_vector;
    request.budget = authored.budget;
    auto solved = solve_two_bone(graph, locals, &limits, request);
    if (!solved) return Result::failure(solved.error());
    if (!solved.value().converged || !std::isfinite(solved.value().residual) ||
        solved.value().residual > std::max(authored.budget.convergence_tolerance, 1e-9))
        return Result::failure(invalid("decoded IK solve did not converge within its residual bound"));
    LocalPose after = locals;
    for (const auto& [id, value] : solved.value().solved_locals) after[id] = value;
    auto before_fk = evaluate_fk(graph, locals);
    auto after_fk = evaluate_fk(graph, after);
    if (!before_fk) return Result::failure(before_fk.error());
    if (!after_fk) return Result::failure(after_fk.error());
    double jump = 0.0;
    for (auto id : {authored.chain.root, authored.chain.mid, authored.chain.effector}) {
        const auto before = before_fk.value().world.find(id);
        const auto result = after_fk.value().world.find(id);
        if (before == before_fk.value().world.end() || result == after_fk.value().world.end())
            return Result::failure(invalid("solved IK chain is absent from FK evaluation"));
        const double delta = (before->second.translation - result->second.translation).length();
        if (!std::isfinite(delta)) return Result::failure(invalid("non-finite decoded pose jump"));
        jump = std::max(jump, delta);
    }
    if (jump > authored.max_position_jump) return Result::failure(invalid("decoded channels exceed position jump bound"));
    return Result::success({std::move(candidate), std::move(decoded.value()), std::move(match.value()),
                            std::move(solved.value()), jump});
}
} // namespace carto::pose
