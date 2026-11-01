#include <carto/character/rig_record.hpp>
#include <carto/pose/ik_control_frame.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

using namespace carto;
namespace {
void require(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

struct Fixture {
    kinematic::KinematicGraph graph;
    kinematic::JointLimitSet limits;
    character::RigDefinition rig;
    pose::IkFkAuthoring authored;
    pose::IkControlRigidFrame frame;
    pose::LocalPose locals;
    pose::ControlValues values;
    explicit Fixture(int unsupported = 0) {
        for (std::uint64_t i = 1; i <= 3; ++i) {
            kinematic::KinematicNode node;
            node.id = {i};
            if (i > 1) { node.parent = kinematic::JointId{i - 1}; node.rest.translation.x = 1; }
            node.kind = kinematic::JointKind::ball;
            node.node_class = kinematic::NodeClass::skinned;
            require(static_cast<bool>(graph.insert_node(node)), "insert joint");
        }
        rig.id = {11};
        rig.kinematic_graph_digest = graph.canonical_digest().value();
        rig.joint_limits.digest = limits.canonical_digest().value();
        for (std::uint64_t i = 1; i <= 7; ++i) {
            character::ControlDefinition control;
            control.id = {i};
            if (i <= 3) control.affected_joints.push_back({i});
            if (i == 4 || i == 5) {
                control.allowed_axes = character::kAxisTranslateX | character::kAxisTranslateY | character::kAxisTranslateZ;
                for (std::size_t a = 0; a < 3; ++a) { control.axis_minimum[a] = -100; control.axis_maximum[a] = 100; }
                if (i == 4 && unsupported == 1) {
                    control.allowed_axes = character::kAxisTranslateX;
                    control.axis_minimum[1] = control.axis_maximum[1] = 0;
                    control.axis_minimum[2] = control.axis_maximum[2] = 0;
                }
                if (i == 4 && unsupported == 2) control.follow_space = character::ControlId{1};
                if (i == 4 && unsupported == 3) control.affected_joints.push_back({1});
                if (i == 4 && unsupported == 5) for (std::size_t a = 0; a < 3; ++a) { control.axis_minimum[a] = 0; control.axis_maximum[a] = 0; }
                if (i == 5 && unsupported == 6) for (std::size_t a = 0; a < 3; ++a) {
                    control.axis_minimum[a] = -std::numeric_limits<double>::max();
                    control.axis_maximum[a] = std::numeric_limits<double>::max();
                }
            }
            if (i == 6) { control.allowed_axes = character::kAxisTranslateX; control.axis_maximum[0] = 1; }
            if (i == 7 && unsupported == 4) control.follow_space = character::ControlId{4};
            require(static_cast<bool>(rig.control_graph.insert(control)), "insert control");
        }
        authored.rig = rig.id;
        authored.rig_digest = character::rig_definition_digest(rig).value();
        authored.graph_digest = rig.kinematic_graph_digest;
        authored.limits_digest = rig.joint_limits.digest;
        authored.chain = {{1}, {2}, {3}};
        authored.fk_root = {1}; authored.fk_mid = {2}; authored.fk_effector = {3};
        authored.ik_target = {4}; authored.ik_pole = character::ControlId{5}; authored.mode_control = {6};
        frame.authoring_digest = authored.canonical_digest().value();
        frame.frame_identity = "asserted-frame"; frame.scene_unit_identity = "asserted-scene-unit";
        frame.graph_world_from_control.translation = {4, -2, 3};
        frame.graph_world_from_control.rotation = {0, 0, std::sqrt(0.5), std::sqrt(0.5)};
        frame.max_point_round_trip_error = 1e-10;
        frame.max_direction_round_trip_error = 1e-10;
        locals[{1}].rotation = {0, 0, std::sin(0.2), std::cos(0.2)};
        locals[{2}].translation.x = 1;
        locals[{2}].rotation = {0, 0, std::sin(-0.45), std::cos(-0.45)};
        values[{6}] = {0.5, 0, 0, 0, 0, 0};
    }
};
} // namespace

int main() {
    Fixture f;
    auto independent = f.values;
    independent[{4}] = {1, 2, 3, 0, 0, 0};
    independent[{5}] = {1, 0, 0, 0, 0, 0};
    auto decoded = pose::decode_ik_control_frame(f.frame, f.authored, f.rig, f.graph, f.limits, independent);
    require(decoded && near(decoded.value().target.x, 2) && near(decoded.value().target.y, -1) &&
        near(decoded.value().target.z, 6) && decoded.value().pole_vector &&
        near(decoded.value().pole_vector->x, 0) && near(decoded.value().pole_vector->y, 1) &&
        near(decoded.value().pole_vector->z, 0), "point translated, direction only rotated");
    auto proposal = pose::propose_fk_to_ik_control_frame(f.frame, f.authored, f.rig, f.graph, f.limits, f.locals, f.values);
    require(proposal && proposal.value().decoded_solve.converged &&
        proposal.value().decoded_position_jump <= f.authored.max_position_jump, "decoded channels actually solve to source pose");
    require(proposal.value().candidate_values.at({6}) == f.values.at({6}) && f.values.size() == 1,
        "unrelated channels preserved and input unmodified");
    auto stale = f.frame; stale.authoring_digest = {};
    require(!pose::decode_ik_control_frame(stale, f.authored, f.rig, f.graph, f.limits, independent), "stale binding refused");
    stale = f.frame; stale.graph_world_from_control.scale.x = 2;
    require(!pose::decode_ik_control_frame(stale, f.authored, f.rig, f.graph, f.limits, independent), "scale refused");
    stale = f.frame; stale.graph_world_from_control.rotation.w = 2;
    require(!pose::decode_ik_control_frame(stale, f.authored, f.rig, f.graph, f.limits, independent), "non-unit frame rotation refused");
    stale = f.frame; stale.frame_identity = "  ";
    require(!pose::decode_ik_control_frame(stale, f.authored, f.rig, f.graph, f.limits, independent), "missing asserted identity refused");
    auto bad_values = independent; bad_values[{5}] = {0, 0, 0, 0, 0, 0};
    require(!pose::decode_ik_control_frame(f.frame, f.authored, f.rig, f.graph, f.limits, bad_values), "zero pole refused");
    bad_values = independent; bad_values[{4}][0] = 101;
    require(!pose::decode_ik_control_frame(f.frame, f.authored, f.rig, f.graph, f.limits, bad_values), "out-of-range axis refused, never clamped");
    bad_values = independent; bad_values[{4}][3] = 1;
    require(!pose::decode_ik_control_frame(f.frame, f.authored, f.rig, f.graph, f.limits, bad_values), "mixed rotational axis refused");
    Fixture overflow(6);
    overflow.frame.graph_world_from_control.rotation = core::Quaternion::identity();
    auto huge = independent;
    huge[{5}] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0, 0, 0, 0};
    require(!pose::decode_ik_control_frame(overflow.frame, overflow.authored, overflow.rig, overflow.graph, overflow.limits, huge), "overflowing pole magnitude refused");
    for (int variant = 1; variant <= 5; ++variant) {
        Fixture unsupported(variant);
        require(!pose::propose_fk_to_ik_control_frame(unsupported.frame, unsupported.authored, unsupported.rig,
            unsupported.graph, unsupported.limits, unsupported.locals, unsupported.values), "unsupported axes, follow, joint mapping, dependent or limit refused");
    }
    pose::LocalPose straight;
    require(!pose::propose_fk_to_ik_control_frame(f.frame, f.authored, f.rig, f.graph, f.limits, straight, f.values), "undefined straight-chain pole refused");
    std::cout << "IK control frame tests passed\n";
}
