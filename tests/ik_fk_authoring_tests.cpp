#include <carto/character/rig_record.hpp>
#include <carto/pose/ik_fk_authoring.hpp>

#include <cmath>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

using namespace carto;
namespace {
void require(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
core::Transform rotation(double angle, double x = 0.0) {
    core::Transform transform;
    transform.translation.x = x;
    transform.rotation = {0, 0, std::sin(angle / 2), std::cos(angle / 2)};
    return transform;
}
struct Fixture {
    kinematic::KinematicGraph graph;
    kinematic::JointLimitSet limits;
    character::RigDefinition rig;
    pose::IkFkAuthoring authored;
    Fixture() {
        for (std::uint64_t i = 1; i <= 3; ++i) {
            kinematic::KinematicNode node;
            node.id = {i};
            if (i > 1) { node.parent = kinematic::JointId{i - 1}; node.rest.translation.x = 1; }
            node.kind = kinematic::JointKind::ball;
            node.node_class = kinematic::NodeClass::skinned;
            require(static_cast<bool>(graph.insert_node(node)), "insert chain");
        }
        rig.id = {11};
        rig.kinematic_graph_digest = graph.canonical_digest().value();
        rig.joint_limits.digest = limits.canonical_digest().value();
        for (std::uint64_t i = 1; i <= 6; ++i) {
            character::ControlDefinition control;
            control.id = {i};
            if (i <= 3) control.affected_joints.push_back({i});
            if (i == 6) {
                control.allowed_axes = character::kAxisTranslateX;
                control.axis_minimum[0] = 0;
                control.axis_maximum[0] = 1;
            }
            require(static_cast<bool>(rig.control_graph.insert(control)), "insert control");
        }
        authored.rig = rig.id;
        authored.rig_digest = character::rig_definition_digest(rig).value();
        authored.graph_digest = rig.kinematic_graph_digest;
        authored.limits_digest = rig.joint_limits.digest;
        authored.chain = {{1}, {2}, {3}};
        authored.fk_root = {1}; authored.fk_mid = {2}; authored.fk_effector = {3};
        authored.ik_target = {4}; authored.ik_pole = character::ControlId{5};
        authored.mode_control = {6};
    }
};
} // namespace

int main() {
    Fixture f;
    require(static_cast<bool>(f.authored.validate_against(f.rig, f.graph, f.limits)), "valid authored roles");
    const auto bytes = f.authored.serialize();
    const auto decoded = pose::IkFkAuthoring::deserialize(bytes);
    require(decoded && decoded.value().serialize() == bytes &&
        decoded.value().canonical_digest().value() == f.authored.canonical_digest().value(), "canonical record round trip");
    std::array<pose::IkFkAuthoring, 2> set{f.authored, f.authored};
    require(static_cast<bool>(pose::validate_ik_fk_authoring_set(set, f.rig, f.graph, f.limits)),
        "shared validation matches independent record validation");
    set.back().rig_digest = {};
    require(!pose::validate_ik_fk_authoring_set(set, f.rig, f.graph, f.limits),
        "shared validation refuses any invalid record");
    set.back() = f.authored;
    set.back().graph_digest = f.authored.rig_digest;
    require(!pose::validate_ik_fk_authoring_set(set, f.rig, f.graph, f.limits),
        "shared validation refuses mismatched pins in any record");
    std::array<pose::IkFkAuthoring, pose::IkFkAuthoring::kMaxSetSize + 1U> too_many{};
    require(!pose::validate_ik_fk_authoring_set(too_many, f.rig, f.graph, f.limits),
        "shared validation refuses over-budget set before record checks");
    require(!pose::IkFkAuthoring::deserialize(bytes + "x"), "trailing bytes refused");
    require(!pose::IkFkAuthoring::deserialize(std::string(pose::IkFkAuthoring::kMaxSerializedBytes + 1U, 'x')), "oversized record refused");
    auto noncanonical = bytes;
    noncanonical.insert(noncanonical.find("CHAIN ") + 6, "0");
    require(!pose::IkFkAuthoring::deserialize(noncanonical), "noncanonical integer refused");
    auto invalid = f.authored;
    invalid.fk_root = invalid.fk_mid;
    require(!invalid.validate(), "duplicate roles refused");
    invalid = f.authored; invalid.ik_target = {99};
    require(!invalid.validate_against(f.rig, f.graph, f.limits), "dangling role refused");
    invalid = f.authored; invalid.chain = {{1}, {3}, {2}};
    require(!invalid.validate_against(f.rig, f.graph, f.limits), "noncontiguous chain refused");
    invalid = f.authored; invalid.fk_root = {4}; invalid.ik_target = {1};
    require(!invalid.validate_against(f.rig, f.graph, f.limits), "unrelated FK role refused");
    invalid = f.authored; invalid.mode_axis = 1;
    require(!invalid.validate_against(f.rig, f.graph, f.limits), "unavailable mode axis refused");
    invalid = f.authored; invalid.ik_mode_value = 2;
    require(!invalid.validate_against(f.rig, f.graph, f.limits), "mode outside bounds refused");
    invalid = f.authored; invalid.budget.max_seconds = 1;
    require(!invalid.validate(), "nondeterministic wall clock budget refused");
    invalid = f.authored; invalid.max_position_jump = std::numeric_limits<double>::infinity();
    require(!invalid.validate(), "nonfinite jump refused");
    auto changed_rig = f.rig;
    changed_rig.semantic_tags.push_back("changed");
    require(!f.authored.validate_against(changed_rig, f.graph, f.limits), "stale rig digest refused");
    pose::LocalPose fk;
    fk[{1}] = rotation(0.4);
    fk[{2}] = rotation(-0.9, 1.0);
    const auto original_fk = fk;
    auto to_ik = pose::propose_ik_fk_switch(f.authored, f.rig, f.graph, f.limits, pose::IkFkMode::ik, fk);
    require(to_ik && to_ik.value().mode_control == f.authored.mode_control &&
        to_ik.value().mode_axis == 0 && to_ik.value().mode_value == 1 &&
        to_ik.value().match.position_jump <= f.authored.max_position_jump, "FK to IK proposal");
    require(fk.size() == original_fk.size() && fk.at({2}).translation.x == original_fk.at({2}).translation.x,
        "proposal does not mutate FK inputs");
    auto without_pole = f.authored;
    without_pole.ik_pole.reset();
    require(!pose::propose_ik_fk_switch(without_pole, f.rig, f.graph, f.limits, pose::IkFkMode::ik, fk),
        "bent FK match requires a pole control");
    auto to_fk = pose::propose_ik_fk_switch(f.authored, f.rig, f.graph, f.limits, pose::IkFkMode::fk,
        {}, to_ik.value().match.ik);
    require(to_fk && to_fk.value().mode_value == 0 &&
        to_fk.value().match.position_jump <= f.authored.max_position_jump, "IK to FK proposal");
    pose::IkControlState unreachable; unreachable.target = {10, 0, 0};
    require(!pose::propose_ik_fk_switch(f.authored, f.rig, f.graph, f.limits, pose::IkFkMode::fk, {}, unreachable),
        "unreachable target refused");
    require(!pose::propose_ik_fk_switch(f.authored, f.rig, f.graph, f.limits, pose::IkFkMode::fk, {}),
        "IK inputs required for FK proposal");
    require(!pose::propose_ik_fk_switch(f.authored, f.rig, f.graph, f.limits, pose::IkFkMode::ik, fk, unreachable),
        "unexpected IK inputs refused");
    auto other_basis = f.rig;
    other_basis.rest_basis.meters_per_unit = 0.01;
    auto other_authored = f.authored;
    other_authored.rig_digest = character::rig_definition_digest(other_basis).value();
    require(static_cast<bool>(other_authored.validate_against(other_basis, f.graph, f.limits)), "other basis is valid authoring");
    require(!pose::propose_ik_fk_switch(other_authored, other_basis, f.graph, f.limits, pose::IkFkMode::ik, fk),
        "unsupported solver coordinate policy refused");
    kinematic::KinematicGraph parented;
    kinematic::KinematicNode parent;
    parent.id = {10};
    require(static_cast<bool>(parented.insert_node(parent)), "insert ancestor");
    for (std::uint64_t i = 1; i <= 3; ++i) {
        auto node = *f.graph.find({i});
        if (i == 1) node.parent = kinematic::JointId{10};
        require(static_cast<bool>(parented.insert_node(node)), "insert parented chain");
    }
    auto parented_rig = f.rig;
    parented_rig.kinematic_graph_digest = parented.canonical_digest().value();
    auto parented_authored = f.authored;
    parented_authored.graph_digest = parented_rig.kinematic_graph_digest;
    parented_authored.rig_digest = character::rig_definition_digest(parented_rig).value();
    require(static_cast<bool>(parented_authored.validate_against(parented_rig, parented, f.limits)), "parented authoring valid");
    auto posed_ancestor = fk;
    posed_ancestor[{10}] = rotation(0.3);
    require(!pose::propose_ik_fk_switch(parented_authored, parented_rig, parented, f.limits,
        pose::IkFkMode::ik, posed_ancestor), "posed ancestor refused");
    std::cout << "IK/FK authoring tests passed\n";
}
