#include <carto/character/constraints_record.hpp>
#include <carto/character/controls.hpp>
#include <carto/character/coordinate.hpp>
#include <carto/character/time.hpp>
#include <carto/core/math.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/pose/constraints.hpp>
#include <carto/pose/contact.hpp>
#include <carto/pose/controls.hpp>
#include <carto/pose/fk.hpp>
#include <carto/pose/ik.hpp>
#include <carto/pose/ik_fk_match.hpp>
#include <carto/pose/pose_library.hpp>
#include <carto/pose/rotation.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using carto::core::Quaternion;
using carto::core::Transform;
using carto::core::Vec3d;
using carto::kinematic::JointId;
using carto::kinematic::KinematicGraph;
using carto::kinematic::KinematicNode;
using carto::kinematic::JointLimit;
using carto::kinematic::JointLimitSet;
using carto::pose::IkBudget;
using carto::pose::LocalPose;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double left, double right, double epsilon = 1e-9) {
    return std::abs(left - right) <= epsilon;
}

bool near_vec(Vec3d left, Vec3d right, double epsilon = 1e-9) {
    return near(left.x, right.x, epsilon) && near(left.y, right.y, epsilon) &&
        near(left.z, right.z, epsilon);
}

bool near_quat(Quaternion left, Quaternion right, double epsilon = 1e-9) {
    return near(left.x, right.x, epsilon) && near(left.y, right.y, epsilon) &&
        near(left.z, right.z, epsilon) && near(left.w, right.w, epsilon);
}

bool near_transform(const Transform& left, const Transform& right, double epsilon = 1e-9) {
    return near_vec(left.translation, right.translation, epsilon) &&
        near_quat(left.rotation, right.rotation, epsilon) &&
        near_vec(left.scale, right.scale, epsilon);
}

Transform translation(Vec3d value) {
    Transform transform = Transform::identity();
    transform.translation = value;
    return transform;
}

Transform rotation_z(double radians) {
    return Transform{{}, carto::pose::axis_angle({0.0, 0.0, 1.0}, radians), {1.0, 1.0, 1.0}};
}

// j1 -> j2 -> ... -> jN, each bone `bone_length` along +X from its parent.
KinematicGraph chain_graph(std::size_t joints, double bone_length, JointId first = JointId{1U}) {
    KinematicGraph graph;
    for (std::size_t index = 0U; index < joints; ++index) {
        KinematicNode node;
        node.id = JointId{first.value + index};
        node.parent = index == 0U ? std::optional<JointId>{} : JointId{first.value + index - 1U};
        node.rest = translation({index == 0U ? 0.0 : bone_length, 0.0, 0.0});
        node.kind = carto::kinematic::JointKind::ball;
        require(static_cast<bool>(graph.insert_node(node)), "chain node inserts");
    }
    return graph;
}

carto::character::RationalTime rt(std::int64_t tick, std::uint32_t tps = 24U) {
    return carto::character::RationalTime{tick, tps};
}

carto::character::ConstraintDefinition constraint(
    std::uint64_t id,
    carto::character::ConstraintType type,
    JointId source,
    JointId target,
    std::uint32_t priority,
    double tolerance,
    carto::character::ConstraintStage stage = carto::character::ConstraintStage::pre_deformation,
    carto::character::RationalTime start = rt(0),
    carto::character::RationalTime end = rt(1000)) {
    carto::character::ConstraintDefinition definition;
    definition.id = carto::character::ConstraintId{id};
    definition.type = type;
    definition.source.joint = source;
    definition.target.joint = target;
    definition.priority = priority;
    definition.tolerance = tolerance;
    definition.validity_start = start;
    definition.validity_end = end;
    definition.stage = stage;
    return definition;
}

std::string receipt_fingerprint(const carto::pose::PoseReceipt& receipt) {
    std::string text;
    for (const auto& [joint, transform] : receipt.solved_locals) {
        text += "j" + std::to_string(joint.value) + ":" +
            std::to_string(transform.translation.x) + "," +
            std::to_string(transform.translation.y) + "," +
            std::to_string(transform.translation.z) + ";";
    }
    for (const auto& [id, outcome] : receipt.outcomes) {
        text += "c" + std::to_string(id.value) + ":" +
            std::to_string(outcome.residual) + "/" +
            std::to_string(outcome.iterations) + "/" +
            (outcome.converged ? "y" : "n") + (outcome.superseded ? "s" : "") + ";";
    }
    text += std::to_string(receipt.stage_passes) + "/" +
        (receipt.all_converged ? "y" : "n") + "/" +
        std::to_string(receipt.limit_violations.size());
    return text;
}

void fk_matches_hand_computed_transforms() {
    KinematicGraph graph;
    KinematicNode j1; j1.id = JointId{1U}; j1.rest = translation({0.0, 0.0, 0.0});
    KinematicNode j2; j2.id = JointId{2U}; j2.parent = JointId{1U}; j2.rest = translation({1.0, 0.0, 0.0});
    KinematicNode j3; j3.id = JointId{3U}; j3.parent = JointId{2U}; j3.rest = translation({0.0, 2.0, 0.0});
    require(static_cast<bool>(graph.insert_node(j1)) &&
        static_cast<bool>(graph.insert_node(j2)) &&
        static_cast<bool>(graph.insert_node(j3)), "fk graph inserts");

    auto rest = carto::pose::evaluate_fk(graph);
    require(static_cast<bool>(rest), "rest FK evaluates");
    require(near_vec(rest.value().world.at(JointId{1U}).translation, {0.0, 0.0, 0.0}),
        "rest world j1");
    require(near_vec(rest.value().world.at(JointId{2U}).translation, {1.0, 0.0, 0.0}),
        "rest world j2");
    require(near_vec(rest.value().world.at(JointId{3U}).translation, {1.0, 2.0, 0.0}),
        "rest world j3");
    require(near_transform(rest.value().root_frame, Transform::identity()), "identity policy frame");

    // Pose j2 by +90 deg about Z: j3 local (0,2,0) maps to (-2,0,0) in world.
    // A local override replaces the whole local transform (rest translation is
    // not implicit), so the rest offset rides along in the override.
    LocalPose pose;
    Transform j2_pose = rotation_z(3.14159265358979323846 * 0.5);
    j2_pose.translation = {1.0, 0.0, 0.0};
    pose.emplace(JointId{2U}, j2_pose);
    auto fk = carto::pose::evaluate_fk(graph, pose);
    require(static_cast<bool>(fk), "posed FK evaluates");
    require(near_vec(fk.value().world.at(JointId{2U}).translation, {1.0, 0.0, 0.0}),
        "posed world j2 unchanged");
    require(near_vec(fk.value().world.at(JointId{3U}).translation, {-1.0, 0.0, 0.0}),
        "posed world j3 rotated by parent");

    // Coordinate policy awareness: meters_per_unit scales root translation;
    // handedness flips the right axis.
    carto::character::CoordinatePolicy scale_policy;
    scale_policy.meters_per_unit = 2.0;
    KinematicGraph scaled_graph;
    KinematicNode root; root.id = JointId{1U}; root.rest = translation({1.0, 0.0, 0.0});
    require(static_cast<bool>(scaled_graph.insert_node(root)), "scaled root inserts");
    auto scaled = carto::pose::evaluate_fk(scaled_graph, {}, scale_policy);
    require(static_cast<bool>(scaled), "scaled FK evaluates");
    require(near_vec(scaled.value().world.at(JointId{1U}).translation, {2.0, 0.0, 0.0}),
        "policy meters_per_unit scales root translation");

    carto::character::CoordinatePolicy left_policy;
    left_policy.handedness = carto::character::Handedness::left;
    auto left_frame = carto::pose::policy_frame_transform(left_policy);
    require(static_cast<bool>(left_frame), "left-handed policy frame builds");
    require(near_vec(left_frame.value().rotation.rotate({1.0, 0.0, 0.0}), {-1.0, 0.0, 0.0}),
        "left-handed policy negates right axis");

    carto::character::CoordinatePolicy bad_policy;
    bad_policy.up = carto::character::Axis::positive_z;
    bad_policy.forward = carto::character::Axis::positive_z;
    require(!carto::pose::policy_frame_transform(bad_policy),
        "collinear up/forward policy fails closed");

    LocalPose hostile;
    hostile.emplace(JointId{99U}, Transform::identity());
    require(!carto::pose::evaluate_fk(graph, hostile),
        "FK override outside the graph fails closed");
}

void euler_convention_round_trips() {
    const Vec3d angles{0.3, -0.6, 0.9};
    const Quaternion q = carto::pose::from_euler_xyz(angles);
    const Vec3d back = carto::pose::to_euler_xyz(q);
    require(near_vec(back, angles, 1e-12), "euler xyz round trips");
    require(near_quat(carto::pose::from_euler_xyz(back), q, 1e-12),
        "euler composition round trips");
    const Quaternion combined = carto::pose::axis_angle({0.0, 0.0, 1.0}, 0.5) *
        carto::pose::axis_angle({1.0, 0.0, 0.0}, 0.25);
    require(near_quat(carto::pose::from_euler_xyz(carto::pose::to_euler_xyz(combined)),
        combined, 1e-12), "arbitrary rotation round trips");
}

void controls_evaluate_with_limits_and_follow_space() {
    using carto::character::ControlDefinition;
    using carto::character::ControlGraph;
    using carto::character::ControlId;
    KinematicGraph graph = chain_graph(1, 1.0);
    const JointId joint{1U};

    ControlGraph control_graph;
    ControlDefinition driver;
    driver.id = ControlId{1U};
    driver.affected_joints = {joint};
    driver.allowed_axes = carto::character::kAxisRotateZ;
    driver.axis_minimum = {0.0, 0.0, 0.0, 0.0, 0.0, -0.5};
    driver.axis_maximum = {0.0, 0.0, 0.0, 0.0, 0.0, 0.5};
    require(static_cast<bool>(control_graph.insert(driver)), "driver control inserts");
    ControlDefinition follower = driver;
    follower.id = ControlId{2U};
    follower.follow_space = ControlId{1U};
    require(static_cast<bool>(control_graph.insert(follower)), "follower control inserts");

    carto::pose::ControlValues values;
    values.emplace(ControlId{1U}, std::array<double, 6>{0.0, 0.0, 0.0, 0.0, 0.0, 0.8});
    values.emplace(ControlId{2U}, std::array<double, 6>{0.0, 0.0, 0.0, 0.0, 0.0, 0.1});
    auto evaluation = carto::pose::evaluate_controls(control_graph, values);
    require(static_cast<bool>(evaluation), "controls evaluate");
    require(evaluation.value().evaluation_order.size() == 2U &&
        evaluation.value().evaluation_order.front() == ControlId{1U} &&
        evaluation.value().evaluation_order.back() == ControlId{2U},
        "follow driver evaluates before follower");
    // Driver clamps 0.8 -> 0.5, follower adds 0.1: net rotation 0.6 about Z.
    const Transform& offset = evaluation.value().joint_offsets.at(joint);
    require(near_vec(carto::pose::to_euler_xyz(offset.rotation), {0.0, 0.0, 0.6}, 1e-12),
        "clamped value plus inherited follow offset");

    carto::pose::ControlValues disallowed;
    disallowed.emplace(ControlId{1U}, std::array<double, 6>{1.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    require(!carto::pose::evaluate_controls(control_graph, disallowed),
        "value on a disallowed axis fails closed");
    carto::pose::ControlValues unknown;
    unknown.emplace(ControlId{77U}, std::array<double, 6>{});
    require(!carto::pose::evaluate_controls(control_graph, unknown),
        "value on an unknown control fails closed");
    auto zero = carto::pose::evaluate_controls(control_graph, {});
    require(static_cast<bool>(zero) && zero.value().joint_offsets.empty(),
        "empty values produce no offsets");
}

void two_bone_ik_reaches_target_and_respects_limits() {
    const KinematicGraph graph = chain_graph(3, 1.0);
    const JointId root{1U}, mid{2U}, tip{3U};

    carto::pose::TwoBoneIkRequest request;
    request.root = root; request.mid = mid; request.effector = tip;
    request.target = {1.0, 0.5, 0.0};
    request.budget = IkBudget{};
    auto solved = carto::pose::solve_two_bone(graph, {}, nullptr, request);
    require(static_cast<bool>(solved), "two-bone solve succeeds");
    require(solved.value().converged, "two-bone solve converges");
    auto fk = carto::pose::evaluate_fk(graph, solved.value().solved_locals);
    require(static_cast<bool>(fk), "solved pose FK evaluates");
    require(near_vec(fk.value().world.at(tip).translation, request.target, 1e-9),
        "effector lands on the target");
    require(solved.value().residual <= 1e-9, "honest residual is near zero");

    // Pole vector selects the bend plane deterministically.
    request.pole_vector = Vec3d{0.0, 0.0, 1.0};
    auto positive = carto::pose::solve_two_bone(graph, {}, nullptr, request);
    request.pole_vector = Vec3d{0.0, 0.0, -1.0};
    auto negative = carto::pose::solve_two_bone(graph, {}, nullptr, request);
    require(static_cast<bool>(positive) && static_cast<bool>(negative), "pole solves succeed");
    auto fk_positive = carto::pose::evaluate_fk(graph, positive.value().solved_locals);
    auto fk_negative = carto::pose::evaluate_fk(graph, negative.value().solved_locals);
    require(fk_positive.value().world.at(mid).translation.z > 0.0 &&
        fk_negative.value().world.at(mid).translation.z < 0.0,
        "pole vector flips the bend side");

    // Authored limits clamp the solve and the residual reports it honestly.
    JointLimit hinge;
    hinge.joint = mid;
    hinge.angular_enabled = true;
    hinge.angular_min = {0.0, 0.0, -0.1};
    hinge.angular_max = {0.0, 0.0, 0.1};
    JointLimitSet limits;
    require(static_cast<bool>(limits.insert(hinge)), "hinge limit inserts");
    request.pole_vector = std::optional<Vec3d>{};
    request.allow_partial = true;
    auto clamped = carto::pose::solve_two_bone(graph, {}, &limits, request);
    require(static_cast<bool>(clamped), "limited solve returns a proposal");
    require(!clamped.value().converged && clamped.value().residual > 1e-3,
        "limited solve reports honest non-convergence");
    const Transform& mid_local = clamped.value().solved_locals.at(mid);
    const Vec3d mid_euler = carto::pose::to_euler_xyz(
        (carto::pose::inverse(rotation_z(0.0).rotation) * mid_local.rotation).normalized());
    require(std::abs(mid_euler.z) <= 0.1 + 1e-9,
        "solved mid rotation respects the authored limit");

    // Unreachable targets refuse by default and report the residual.
    request.target = {10.0, 0.0, 0.0};
    request.allow_partial = false;
    auto refused = carto::pose::solve_two_bone(graph, {}, nullptr, request);
    require(!refused, "unreachable target refuses without allow_partial");
    request.allow_partial = true;
    auto stretched = carto::pose::solve_two_bone(graph, {}, nullptr, request);
    require(static_cast<bool>(stretched) && !stretched.value().converged &&
        stretched.value().residual > 7.9,
        "unreachable stretch reports the honest residual");
}

void jacobian_chain_converges_and_refuses_unreachable() {
    const KinematicGraph graph = chain_graph(4, 0.5);
    const JointId tip{4U};

    carto::pose::JacobianIkRequest request;
    request.chain = {JointId{1U}, JointId{2U}, JointId{3U}, tip};
    // Reach is 1.5 (three 0.5 bones to the tip joint); keep the target
    // strictly inside it.
    request.target = {1.4, 0.5, 0.0};
    IkBudget budget;
    budget.max_iterations = 256U;
    budget.convergence_tolerance = 1e-9;
    request.budget = budget;
    auto solved = carto::pose::solve_chain_jacobian(graph, {}, nullptr, request);
    require(static_cast<bool>(solved), "jacobian chain converges on a reachable target");
    require(solved.value().converged && solved.value().residual <= 1e-6,
        "reachable residual within tolerance");
    require(solved.value().iterations > 0U && solved.value().iterations <= 256U,
        "iteration count within budget");
    auto fk = carto::pose::evaluate_fk(graph, solved.value().solved_locals);
    require(near_vec(fk.value().world.at(tip).translation, request.target, 1e-6),
        "FK of solved pose matches the target");

    // Joint limits bound the reachable set: tightly capping every chain joint
    // leaves the target unreachable, and the limited solve reports that
    // honestly as an explicit proposal (mirrors the two-bone limited case).
    JointLimitSet limits;
    for (const JointId joint : request.chain) {
        JointLimit cap;
        cap.joint = joint;
        cap.angular_enabled = true;
        cap.angular_min = {0.0, 0.0, -0.05};
        cap.angular_max = {0.0, 0.0, 0.05};
        require(static_cast<bool>(limits.insert(cap)), "cap limit inserts");
    }
    request.allow_partial = true;
    auto capped = carto::pose::solve_chain_jacobian(graph, {}, &limits, request);
    require(static_cast<bool>(capped), "limited jacobian returns a proposal");
    require(!capped.value().converged && capped.value().residual > 1e-4,
        "limited jacobian reports honest non-convergence");

    request.target = {10.0, 0.0, 0.0};
    request.budget = budget;
    request.allow_partial = false;
    auto refused = carto::pose::solve_chain_jacobian(graph, {}, nullptr, request);
    require(!refused, "unreachable jacobian target refuses");
    require(!refused.error().context.empty(), "refusal carries residual context");
    request.allow_partial = true;
    auto partial = carto::pose::solve_chain_jacobian(graph, {}, nullptr, request);
    require(static_cast<bool>(partial) && !partial.value().converged,
        "unreachable jacobian returns an explicit proposal");

    // Hostile and invalid requests fail closed.
    carto::pose::JacobianIkRequest broken;
    broken.chain = {JointId{1U}, JointId{3U}};
    broken.target = {1.0, 0.0, 0.0};
    broken.budget = budget;
    require(!carto::pose::solve_chain_jacobian(graph, {}, nullptr, broken),
        "non-contiguous chain fails closed");
    carto::pose::JacobianIkRequest bad_budget;
    bad_budget.chain = {JointId{1U}};
    bad_budget.target = {1.0, 0.0, 0.0};
    bad_budget.budget.max_iterations = 0U;
    require(!carto::pose::solve_chain_jacobian(graph, {}, nullptr, bad_budget),
        "zero iteration budget fails closed");
}

void constraint_evaluation_resolves_arbitration_deterministically() {
    const KinematicGraph graph = chain_graph(3, 1.0);
    const JointId root{1U}, tip{3U};

    carto::character::ConstraintGraph constraints;
    auto ik = constraint(1U, carto::character::ConstraintType::ik_pin, root, tip, 1U, 1e-6);
    auto orient = constraint(2U, carto::character::ConstraintType::orient, root, tip, 5U, 1e-6);
    require(static_cast<bool>(constraints.insert(ik)) &&
        static_cast<bool>(constraints.insert(orient)), "conflicting constraints insert");
    require(!constraints.validate(),
        "conflicting target without arbitration fails closed");
    carto::character::ConstraintArbitration arbitration;
    arbitration.first = carto::character::ConstraintType::ik_pin;
    arbitration.second = carto::character::ConstraintType::orient;
    arbitration.rule = carto::character::ArbitrationRule::priority_wins;
    require(static_cast<bool>(constraints.declare_arbitration(arbitration)) &&
        static_cast<bool>(constraints.validate()), "declared arbitration validates");

    carto::pose::ConstraintEvalContext context;
    context.time = rt(12);
    context.pin_targets.emplace(carto::character::ConstraintId{1U}, Vec3d{1.0, 0.5, 0.0});
    auto receipt = carto::pose::evaluate_constraints(constraints, graph, context);
    require(static_cast<bool>(receipt), "arbitrated evaluation succeeds");
    require(receipt.value().outcomes.at(carto::character::ConstraintId{1U}).superseded,
        "lower priority constraint is superseded");
    require(!receipt.value().outcomes.at(carto::character::ConstraintId{2U}).superseded &&
        receipt.value().outcomes.at(carto::character::ConstraintId{2U}).converged,
        "higher priority constraint wins and converges");
    require(receipt.value().stage_passes == 1U, "one stage pass ran");

    // Determinism: identical inputs produce identical receipts.
    auto again = carto::pose::evaluate_constraints(constraints, graph, context);
    require(static_cast<bool>(again), "repeat evaluation succeeds");
    require(receipt_fingerprint(receipt.value()) == receipt_fingerprint(again.value()),
        "constraint evaluation is deterministic");

    // Blend arbitration evaluates both members instead of superseding.
    carto::character::ConstraintGraph blended;
    require(static_cast<bool>(blended.insert(ik)) &&
        static_cast<bool>(blended.insert(orient)), "blend constraints insert");
    arbitration.rule = carto::character::ArbitrationRule::blend;
    require(static_cast<bool>(blended.declare_arbitration(arbitration)),
        "blend arbitration declares");
    auto blend_receipt = carto::pose::evaluate_constraints(blended, graph, context);
    require(static_cast<bool>(blend_receipt), "blend evaluation succeeds");
    require(!blend_receipt.value().outcomes.at(carto::character::ConstraintId{1U}).superseded &&
        !blend_receipt.value().outcomes.at(carto::character::ConstraintId{2U}).superseded,
        "blend arbitration evaluates every member");

    // Same-type conflicts apply in deterministic priority order.
    carto::character::ConstraintGraph sequence;
    auto first_blend = constraint(1U, carto::character::ConstraintType::fk_blend, root, root, 1U, 0.0);
    auto second_blend = constraint(2U, carto::character::ConstraintType::fk_blend, root, root, 2U, 0.0);
    require(static_cast<bool>(sequence.insert(first_blend)) &&
        static_cast<bool>(sequence.insert(second_blend)), "blend constraints insert");
    carto::pose::ConstraintEvalContext blend_context;
    blend_context.time = rt(0);
    Transform goal_a = translation({0.0, 0.0, 3.0});
    Transform goal_b = translation({0.0, 0.0, 7.0});
    blend_context.blend_targets.emplace(carto::character::ConstraintId{1U}, goal_a);
    blend_context.blend_targets.emplace(carto::character::ConstraintId{2U}, goal_b);
    blend_context.blend_weights.emplace(carto::character::ConstraintId{1U}, 1.0);
    blend_context.blend_weights.emplace(carto::character::ConstraintId{2U}, 1.0);
    auto sequence_receipt = carto::pose::evaluate_constraints(sequence, graph, blend_context);
    require(static_cast<bool>(sequence_receipt), "same-type evaluation succeeds");
    // Application order is priority descending (2 then 1); the last writer
    // (constraint 1) wins, deterministically.
    require(near_vec(sequence_receipt.value().solved_locals.at(root).translation,
        goal_a.translation, 1e-12), "last writer in priority order wins");

    // Timebase mismatch and missing goals fail closed.
    carto::pose::ConstraintEvalContext bad_time;
    bad_time.time = rt(12, 30U);
    require(!carto::pose::evaluate_constraints(constraints, graph, bad_time),
        "sampled time on another timebase fails closed");
    carto::pose::ConstraintEvalContext no_goal;
    no_goal.time = rt(12);
    require(!carto::pose::evaluate_constraints(constraints, graph, no_goal),
        "missing pin goal fails closed");

    // Inactive constraints (outside the validity window) produce no outcome.
    carto::character::ConstraintGraph windowed;
    auto early = constraint(1U, carto::character::ConstraintType::limit, root, tip, 0U, 0.0,
        carto::character::ConstraintStage::preprocess, rt(0), rt(4));
    require(static_cast<bool>(windowed.insert(early)), "windowed constraint inserts");
    carto::pose::ConstraintEvalContext late;
    late.time = rt(12);
    late.limits = nullptr;
    auto inactive = carto::pose::evaluate_constraints(windowed, graph, late);
    require(static_cast<bool>(inactive) && inactive.value().outcomes.empty() &&
        inactive.value().stage_passes == 0U,
        "inactive constraints leave no outcome and run no stage pass");
}

void ik_pin_and_limit_constraints_produce_receipts() {
    const KinematicGraph graph = chain_graph(3, 1.0);
    const JointId root{1U}, mid{2U}, tip{3U};

    carto::character::ConstraintGraph constraints;
    auto pin = constraint(1U, carto::character::ConstraintType::ik_pin, root, tip, 0U, 1e-6);
    require(static_cast<bool>(constraints.insert(pin)), "pin constraint inserts");

    JointLimit elbow;
    elbow.joint = mid;
    elbow.angular_enabled = true;
    elbow.angular_min = {0.0, 0.0, -2.0};
    elbow.angular_max = {0.0, 0.0, 2.0};
    JointLimitSet limits;
    require(static_cast<bool>(limits.insert(elbow)), "elbow limit inserts");

    carto::pose::ConstraintEvalContext context;
    context.limits = &limits;
    context.time = rt(6);
    context.pin_targets.emplace(carto::character::ConstraintId{1U}, Vec3d{1.0, 0.5, 0.0});
    auto receipt = carto::pose::evaluate_constraints(constraints, graph, context);
    require(static_cast<bool>(receipt), "pin evaluation succeeds");
    require(receipt.value().all_converged, "pin receipt converged");
    const auto& outcome = receipt.value().outcomes.at(carto::character::ConstraintId{1U});
    require(outcome.converged && outcome.residual <= 1e-6 && outcome.iterations > 0U,
        "pin outcome carries residual and iterations");
    require(receipt.value().limit_violations.empty(), "final limit verification is clean");
    auto fk = carto::pose::evaluate_fk(graph, receipt.value().solved_locals);
    require(near_vec(fk.value().world.at(tip).translation, {1.0, 0.5, 0.0}, 1e-6),
        "solved locals FK to the pin target");

    // A contact constraint whose tolerance the clamped solve cannot meet is
    // a proposal: all_converged is false and the receipt must not commit.
    JointLimit tight;
    tight.joint = JointId{2U};
    tight.angular_enabled = true;
    tight.angular_min = {0.0, 0.0, -0.05};
    tight.angular_max = {0.0, 0.0, 0.05};
    JointLimitSet tight_limits;
    require(static_cast<bool>(tight_limits.insert(tight)), "tight limit inserts");
    carto::character::ConstraintGraph contact_graph;
    auto contact = constraint(2U, carto::character::ConstraintType::contact, root, tip, 0U, 0.01);
    require(static_cast<bool>(contact_graph.insert(contact)), "contact constraint inserts");
    carto::pose::ConstraintEvalContext contact_context;
    contact_context.limits = &tight_limits;
    contact_context.time = rt(6);
    contact_context.pin_targets.emplace(carto::character::ConstraintId{2U}, Vec3d{1.0, 0.5, 0.0});
    auto contact_receipt = carto::pose::evaluate_constraints(contact_graph, graph, contact_context);
    require(static_cast<bool>(contact_receipt), "contact evaluation succeeds");
    require(!contact_receipt.value().all_converged,
        "violated hard tolerance refuses commit on the receipt");
    require(!contact_receipt.value().outcomes.at(carto::character::ConstraintId{2U}).converged,
        "contact outcome reports non-convergence");

    // limit constraints clamp and verify against the authored set.
    carto::character::ConstraintGraph limit_graph;
    auto limit = constraint(3U, carto::character::ConstraintType::limit, root, mid, 0U, 1e-12,
        carto::character::ConstraintStage::preprocess);
    require(static_cast<bool>(limit_graph.insert(limit)), "limit constraint inserts");
    LocalPose over_rotated;
    over_rotated.emplace(mid, rotation_z(2.5));
    carto::pose::ConstraintEvalContext limit_context;
    limit_context.limits = &limits;
    limit_context.base_locals = over_rotated;
    limit_context.time = rt(6);
    auto limit_receipt = carto::pose::evaluate_constraints(limit_graph, graph, limit_context);
    require(static_cast<bool>(limit_receipt), "limit evaluation succeeds");
    const double clamped_euler = carto::pose::to_euler_xyz(
        limit_receipt.value().solved_locals.at(mid).rotation).z;
    require(std::abs(clamped_euler) <= 2.0 + 1e-9, "limit constraint clamps the pose");
    require(limit_receipt.value().limit_violations.empty(),
        "clamped pose passes final verification");
    require(!limit_receipt.value().outcomes.at(carto::character::ConstraintId{3U}).converged,
        "pre-clamp violation is reported on the outcome");
}

void contact_hold_across_sampled_window() {
    const KinematicGraph graph = chain_graph(3, 1.0);
    const JointId root{1U}, mid{2U}, tip{3U};

    carto::character::ConstraintGraph constraints;
    auto foot = constraint(1U, carto::character::ConstraintType::contact, root, tip, 0U, 0.1,
        carto::character::ConstraintStage::post_simulation, rt(0), rt(100));
    auto knee = constraint(2U, carto::character::ConstraintType::contact, root, mid, 0U, 0.1,
        carto::character::ConstraintStage::post_simulation, rt(0), rt(100));
    require(static_cast<bool>(constraints.insert(foot)) &&
        static_cast<bool>(constraints.insert(knee)), "contact constraints insert");

    std::map<carto::character::ConstraintId, Vec3d> pins;
    pins.emplace(carto::character::ConstraintId{1U}, Vec3d{2.0, 0.0, 0.0});
    pins.emplace(carto::character::ConstraintId{2U}, Vec3d{1.0, 0.0, 0.0});

    // Static base pose: both contacts hold on every one of 24 samples.
    const auto static_pose = [](carto::character::RationalTime) {
        return carto::core::Result<LocalPose>::success(LocalPose{});
    };
    carto::pose::ContactHoldRequest request;
    request.contacts = {carto::character::ConstraintId{1U}, carto::character::ConstraintId{2U}};
    request.window_start = rt(0);
    request.window_end = rt(23);
    request.samples = 24U;
    auto held = carto::pose::evaluate_contact_hold(constraints, graph, pins, static_pose, request);
    require(static_cast<bool>(held), "static contact hold evaluates");
    require(held.value().held && held.value().sample_count == 24U &&
        held.value().worst_residual <= 1e-12 &&
        held.value().worst_residuals.size() == 2U, "static contacts hold on every sample");

    // Moving root: the pinned foot slips by exactly the root translation;
    // the report exposes the worst residual and refuses "held".
    const auto moving_pose = [](carto::character::RationalTime time) {
        LocalPose pose;
        pose.emplace(JointId{1U}, translation({0.01 * static_cast<double>(time.tick), 0.0, 0.0}));
        return carto::core::Result<LocalPose>::success(pose);
    };
    auto slipped = carto::pose::evaluate_contact_hold(constraints, graph, pins, moving_pose, request);
    require(static_cast<bool>(slipped), "moving contact hold evaluates");
    require(!slipped.value().held, "slipping contact reports not-held");
    require(near(slipped.value().worst_residuals.at(carto::character::ConstraintId{1U}), 0.23, 1e-12),
        "worst foot residual matches the hand-computed slip");
    require(near(slipped.value().worst_residual, 0.23, 1e-12),
        "overall worst residual matches");

    // Hostile windows fail closed.
    carto::pose::ContactHoldRequest indivisible = request;
    indivisible.samples = 12U; // 23 ticks do not divide by 11
    require(!carto::pose::evaluate_contact_hold(constraints, graph, pins, static_pose, indivisible),
        "non-dividing sample grid fails closed");
    carto::pose::ContactHoldRequest too_few = request;
    too_few.samples = 1U;
    require(!carto::pose::evaluate_contact_hold(constraints, graph, pins, static_pose, too_few),
        "fewer than two samples fails closed");
    carto::pose::ContactHoldRequest outside = request;
    outside.window_end = rt(200);
    require(!carto::pose::evaluate_contact_hold(constraints, graph, pins, static_pose, outside),
        "window beyond constraint validity fails closed");
    carto::pose::ContactHoldRequest unknown = request;
    unknown.contacts = {carto::character::ConstraintId{99U}};
    require(!carto::pose::evaluate_contact_hold(constraints, graph, pins, static_pose, unknown),
        "unknown contact fails closed");
}

void full_evaluation_is_deterministic() {
    const KinematicGraph graph = chain_graph(4, 0.5);
    const JointId root{1U}, tip{4U};

    carto::character::ConstraintGraph constraints;
    auto pin = constraint(1U, carto::character::ConstraintType::ik_pin, root, JointId{3U}, 2U, 1e-6,
        carto::character::ConstraintStage::pre_deformation);
    auto contact = constraint(2U, carto::character::ConstraintType::contact, root, tip, 1U, 1e-3,
        carto::character::ConstraintStage::post_simulation);
    auto align = constraint(3U, carto::character::ConstraintType::orient, root, tip, 0U, 1.0,
        carto::character::ConstraintStage::post_deformation);
    require(static_cast<bool>(constraints.insert(pin)) &&
        static_cast<bool>(constraints.insert(contact)) &&
        static_cast<bool>(constraints.insert(align)), "full graph inserts");
    carto::character::ConstraintArbitration arbitration;
    arbitration.first = carto::character::ConstraintType::contact;
    arbitration.second = carto::character::ConstraintType::orient;
    arbitration.rule = carto::character::ArbitrationRule::sequential;
    require(static_cast<bool>(constraints.declare_arbitration(arbitration)) &&
        static_cast<bool>(constraints.validate()),
        "shared-target arbitration declares and validates");

    JointLimit guard;
    guard.joint = JointId{2U};
    guard.angular_enabled = true;
    guard.angular_min = {0.0, 0.0, -2.5};
    guard.angular_max = {0.0, 0.0, 2.5};
    JointLimitSet limits;
    require(static_cast<bool>(limits.insert(guard)), "guard limit inserts");

    carto::pose::ConstraintEvalContext context;
    context.limits = &limits;
    context.time = rt(7);
    IkBudget budget;
    budget.max_iterations = 128U;
    context.budget = budget;
    context.pin_targets.emplace(carto::character::ConstraintId{1U}, Vec3d{1.0, 0.4, 0.0});
    context.pin_targets.emplace(carto::character::ConstraintId{2U}, Vec3d{1.6, 0.2, 0.0});

    auto first = carto::pose::evaluate_constraints(constraints, graph, context);
    auto second = carto::pose::evaluate_constraints(constraints, graph, context);
    require(static_cast<bool>(first) && static_cast<bool>(second), "deterministic runs succeed");
    require(receipt_fingerprint(first.value()) == receipt_fingerprint(second.value()),
        "full constraint evaluation replays bit-identically");
    require(first.value().stage_passes == 3U, "three stage passes ran in stage order");
}

} // namespace

void ik_fk_match_switches_without_pose_jump() {
    const KinematicGraph graph = chain_graph(3, 1.0);
    const JointId root{1U}, mid{2U}, tip{3U};
    carto::pose::LocalPose fk_pose;
    fk_pose[root] = rotation_z(0.4);
    Transform bent = rotation_z(-0.9);
    bent.translation = {1.0, 0.0, 0.0};
    fk_pose[mid] = bent;
    auto source = carto::pose::evaluate_fk(graph, fk_pose);
    require(static_cast<bool>(source), "bent FK pose evaluates");

    carto::pose::IkFkMatchRequest request;
    request.chain = {root, mid, tip};
    request.max_position_jump = 1e-6;
    auto to_ik = carto::pose::match_ik_to_fk(graph, fk_pose, nullptr, request);
    require(static_cast<bool>(to_ik), "FK to IK match succeeds");
    require(near_vec(to_ik.value().ik.target, source.value().world.at(tip).translation, 1e-12),
        "IK target snaps to the FK effector");
    require(to_ik.value().ik.pole_vector.has_value(), "a bent limb yields a pole");
    require(to_ik.value().position_jump <= 1e-6, "FK to IK switch does not jump");

    auto to_fk = carto::pose::match_fk_to_ik(graph, {}, nullptr, to_ik.value().ik, request);
    require(static_cast<bool>(to_fk), "IK to FK match succeeds");
    auto matched = carto::pose::evaluate_fk(graph, to_fk.value().fk_locals);
    require(static_cast<bool>(matched), "matched FK locals evaluate");
    for (const JointId joint : {root, mid, tip}) {
        require(near_vec(matched.value().world.at(joint).translation,
                    source.value().world.at(joint).translation, 1e-6),
            "IK to FK round trip reproduces the original limb");
    }

    carto::pose::IkControlState unreachable;
    unreachable.target = {10.0, 0.0, 0.0};
    require(!carto::pose::match_fk_to_ik(graph, {}, nullptr, unreachable, request),
        "an unreachable IK target refuses the switch");

    carto::pose::IkFkMatchRequest hostile = request;
    hostile.chain = {root, root, tip};
    require(!carto::pose::match_ik_to_fk(graph, fk_pose, nullptr, hostile),
        "a repeated chain joint refuses");
    hostile = request;
    hostile.max_position_jump = 0.0;
    require(!carto::pose::match_ik_to_fk(graph, fk_pose, nullptr, hostile),
        "a zero jump bound refuses");
    hostile = request;
    hostile.chain = {root, mid, JointId{42U}};
    require(!carto::pose::match_ik_to_fk(graph, fk_pose, nullptr, hostile),
        "an unknown chain joint refuses");
}

void pose_library_blends_masks_mirrors_and_paths() {
    const KinematicGraph graph = chain_graph(3, 1.0);
    const JointId root{1U}, mid{2U}, tip{3U};
    carto::pose::PoseLibrary library;
    carto::pose::LocalPose wave;
    wave[root] = rotation_z(0.8);
    Transform bent = rotation_z(-0.6);
    bent.translation = {1.0, 0.0, 0.0};
    wave[mid] = bent;
    require(static_cast<bool>(library.add("wave", wave)), "pose stores");
    require(!library.add("wave", wave), "duplicate pose name refuses");
    require(!library.add("bad name", wave), "invalid pose name refuses");
    require(!library.add("empty", {}), "empty pose refuses");
    const auto first_digest = library.canonical_digest();

    carto::pose::PoseApplyRequest full;
    full.name = "wave";
    auto applied = carto::pose::apply_library_pose(graph, {}, library, full);
    require(static_cast<bool>(applied), "stored pose applies");
    require(near_transform(applied.value().at(root), wave.at(root)) &&
            near_transform(applied.value().at(mid), wave.at(mid)),
        "a full-weight apply reproduces the stored pose");

    carto::pose::PoseApplyRequest half = full;
    half.weight = 0.5;
    auto blended = carto::pose::apply_library_pose(graph, {}, library, half);
    require(static_cast<bool>(blended), "half-weight pose applies");
    require(near(carto::pose::rotation_angle(blended.value().at(root).rotation), 0.4, 1e-6),
        "a half-weight blend lands halfway in rotation");

    carto::pose::PoseApplyRequest masked = full;
    masked.joint_mask = std::set<JointId>{mid};
    auto partial = carto::pose::apply_library_pose(graph, {}, library, masked);
    require(static_cast<bool>(partial) && !partial.value().contains(root) &&
            partial.value().contains(mid),
        "a joint mask restricts the apply");

    carto::pose::PoseApplyRequest unknown = full;
    unknown.name = "missing";
    require(!carto::pose::apply_library_pose(graph, {}, library, unknown),
        "an unknown pose name refuses");
    carto::pose::PoseApplyRequest heavy = full;
    heavy.weight = 1.5;
    require(!carto::pose::apply_library_pose(graph, {}, library, heavy),
        "an out-of-range blend weight refuses");
    carto::pose::LocalPose foreign;
    foreign[JointId{99U}] = Transform::identity();
    require(static_cast<bool>(library.add("foreign", foreign)), "foreign pose stores");
    carto::pose::PoseApplyRequest stranger = full;
    stranger.name = "foreign";
    require(!carto::pose::apply_library_pose(graph, {}, library, stranger),
        "a stored joint absent from the graph refuses");
    require(!(library.canonical_digest() == first_digest), "library digest tracks content");
    require(static_cast<bool>(library.remove("foreign")) && library.canonical_digest() == first_digest,
        "removing restores the prior digest");

    // Mirror across the XZ plane (axis y): z rotations flip sign, and
    // mirroring twice is the identity.
    std::map<JointId, JointId> no_pairs;
    auto mirrored = carto::pose::mirror_pose(graph, wave, no_pairs, carto::pose::PoseMirrorAxis::y);
    require(static_cast<bool>(mirrored), "pose mirrors");
    require(near_quat(mirrored.value().at(root).rotation, rotation_z(-0.8).rotation),
        "a z rotation mirrors to its negative across the y axis");
    auto twice = carto::pose::mirror_pose(graph, mirrored.value(), no_pairs,
        carto::pose::PoseMirrorAxis::y);
    require(static_cast<bool>(twice) && near_transform(twice.value().at(mid), wave.at(mid)),
        "double mirror is the identity");
    std::map<JointId, JointId> one_sided{{root, mid}};
    require(!carto::pose::mirror_pose(graph, wave, one_sided, carto::pose::PoseMirrorAxis::x),
        "an asymmetric pairing refuses");
    std::map<JointId, JointId> swap{{root, tip}, {tip, root}};
    auto swapped = carto::pose::mirror_pose(graph, wave, swap, carto::pose::PoseMirrorAxis::y);
    require(static_cast<bool>(swapped) && swapped.value().contains(tip) &&
            near_quat(swapped.value().at(tip).rotation, rotation_z(-0.8).rotation),
        "paired joints exchange mirrored transforms");

    // Motion path of the tip while the root sweeps a quarter turn.
    const auto sampler = [root](carto::character::RationalTime time)
        -> carto::core::Result<carto::pose::LocalPose> {
        carto::pose::LocalPose locals;
        locals[root] = rotation_z(1.5707963267948966 * static_cast<double>(time.tick) / 24.0);
        return carto::core::Result<carto::pose::LocalPose>::success(std::move(locals));
    };
    auto path = carto::pose::compute_motion_path(graph, tip, rt(0), rt(24), 6, sampler);
    require(static_cast<bool>(path) && path.value().samples.size() == 5U,
        "motion path samples inclusively");
    require(near_vec(path.value().samples.front().position, {2.0, 0.0, 0.0}, 1e-9) &&
            near_vec(path.value().samples.back().position, {0.0, 2.0, 0.0}, 1e-9),
        "motion path follows the evaluated joint");
    require(path.value().length > 3.0 && path.value().length < 3.1416,
        "motion path length approximates the arc");
    require(!carto::pose::compute_motion_path(graph, tip, rt(24), rt(0), 6, sampler),
        "a reversed motion path range refuses");
    require(!carto::pose::compute_motion_path(graph, tip, rt(0), rt(24, 30U), 6, sampler),
        "a mixed-timebase motion path refuses");
    require(!carto::pose::compute_motion_path(graph, tip, rt(0), rt(24), 0, sampler),
        "a zero motion path step refuses");
    require(!carto::pose::compute_motion_path(graph, tip, rt(0), rt(100000), 1, sampler),
        "an oversized motion path refuses");
}

int main() {
    fk_matches_hand_computed_transforms();
    euler_convention_round_trips();
    controls_evaluate_with_limits_and_follow_space();
    two_bone_ik_reaches_target_and_respects_limits();
    ik_fk_match_switches_without_pose_jump();
    pose_library_blends_masks_mirrors_and_paths();
    jacobian_chain_converges_and_refuses_unreachable();
    constraint_evaluation_resolves_arbitration_deterministically();
    ik_pin_and_limit_constraints_produce_receipts();
    contact_hold_across_sampled_window();
    full_evaluation_is_deterministic();
    std::cout << "pose tests passed\n";
    return EXIT_SUCCESS;
}
