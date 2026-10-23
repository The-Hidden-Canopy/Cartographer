#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using namespace carto;

kinematic::KinematicGraph door_graph() {
    kinematic::KinematicGraph graph;
    kinematic::KinematicNode frame;
    frame.id = kinematic::JointId{1U};
    frame.kind = kinematic::JointKind::fixed;
    frame.node_class = kinematic::NodeClass::rigid;
    frame.semantic_type = "door.frame";
    kinematic::KinematicNode hinge;
    hinge.id = kinematic::JointId{2U};
    hinge.parent = kinematic::JointId{1U};
    hinge.kind = kinematic::JointKind::hinge;
    hinge.node_class = kinematic::NodeClass::rigid;
    hinge.semantic_type = "door.hinge";
    hinge.rest.translation = {0.0, 1.0, 0.0};
    require(static_cast<bool>(graph.insert_node(frame)), "door frame enters");
    require(static_cast<bool>(graph.insert_node(hinge)), "door hinge enters");
    return graph;
}

kinematic::JointLimit door_hinge_limit() {
    kinematic::JointLimit limit;
    limit.joint = kinematic::JointId{2U};
    limit.angular_enabled = true;
    limit.angular_min = {-1.5707963267948966, 0.0, 0.0};
    limit.angular_max = {1.5707963267948966, 0.0, 0.0};
    return limit;
}

void limits_validate_and_reference_containment() {
    kinematic::JointLimit hinge = door_hinge_limit();
    require(static_cast<bool>(hinge.validate()), "hinge limit validates");
    require(hinge.contains_angular({0.0, 0.0, 0.0}),
        "neutral hinge pose is inside the limit");
    require(hinge.contains_angular({1.5, 0.0, 0.0}),
        "hinge pose near the bound is inside");
    require(!hinge.contains_angular({2.0, 0.0, 0.0}),
        "hinge pose past the bound is outside");
    require(!hinge.contains_linear({0.0, 0.0, 0.0}),
        "disabled linear family admits nothing");

    kinematic::JointLimit slider;
    slider.joint = kinematic::JointId{3U};
    slider.linear_enabled = true;
    slider.linear_min = {0.0, -0.5, 0.0};
    slider.linear_max = {0.0, 0.5, 0.0};
    require(static_cast<bool>(slider.validate()), "slider limit validates");
    require(slider.contains_linear({0.0, 0.25, 0.0}),
        "slider travel inside the bound is admitted");
    require(!slider.contains_linear({0.0, 0.75, 0.0}),
        "slider travel past the bound is refused");
    require(!slider.contains_angular({0.0, 0.0, 0.0}),
        "disabled angular family admits nothing");
}

void limit_validation_failures_are_reported() {
    {
        kinematic::JointLimit inverted = door_hinge_limit();
        inverted.angular_min = {1.0, 0.0, 0.0};
        inverted.angular_max = {-1.0, 0.0, 0.0};
        const auto result = inverted.validate();
        require(!result, "inverted angular bounds refuse");
        require(result.error().code == core::ErrorCode::invalid_argument,
            "inverted bounds report invalid_argument");
    }
    {
        kinematic::JointLimit nan_limit = door_hinge_limit();
        nan_limit.angular_min.x = std::numeric_limits<double>::quiet_NaN();
        const auto result = nan_limit.validate();
        require(!result, "non-finite angular bound refuses");
        require(result.error().code == core::ErrorCode::invalid_argument,
            "non-finite bound reports invalid_argument");
    }
    {
        kinematic::JointLimit disabled_with_bounds;
        disabled_with_bounds.joint = kinematic::JointId{2U};
        disabled_with_bounds.angular_max = {1.0, 0.0, 0.0};
        const auto result = disabled_with_bounds.validate();
        require(!result, "disabled family with nonzero bounds refuses");
        require(result.error().code == core::ErrorCode::validation_failed,
            "disabled nonzero bounds report validation_failed");
    }
    {
        kinematic::JointLimit empty;
        empty.joint = kinematic::JointId{2U};
        const auto result = empty.validate();
        require(!result, "limit with no enabled family refuses");
        require(result.error().code == core::ErrorCode::validation_failed,
            "empty limit reports validation_failed");
    }
    {
        kinematic::JointLimit unbounded = door_hinge_limit();
        unbounded.angular_min = {-1000.0, 0.0, 0.0};
        unbounded.angular_max = {1000.0, 0.0, 0.0};
        require(!unbounded.validate(), "implausible travel bound refuses");
    }
    {
        kinematic::JointLimitSet set;
        require(static_cast<bool>(set.insert(door_hinge_limit())),
            "first hinge limit enters");
        require(!set.insert(door_hinge_limit()),
            "duplicate joint limit is rejected");
    }
    {
        kinematic::JointLimitSet set;
        kinematic::JointLimit orphan = door_hinge_limit();
        orphan.joint = kinematic::JointId{99U};
        require(static_cast<bool>(set.insert(orphan)), "orphan limit enters");
        const auto result = set.validate_for_graph(door_graph());
        require(!result, "limit for a joint missing from the graph refuses");
        require(result.error().code == core::ErrorCode::validation_failed,
            "orphan limit reports validation_failed");
    }
}

void limit_set_round_trips_deterministically() {
    kinematic::JointLimitSet set;
    require(static_cast<bool>(set.insert(door_hinge_limit())),
        "hinge limit enters");
    kinematic::JointLimit slider;
    slider.joint = kinematic::JointId{3U};
    slider.linear_enabled = true;
    slider.linear_min = {0.0, -0.5, 0.0};
    slider.linear_max = {0.0, 0.5, 0.0};
    require(static_cast<bool>(set.insert(slider)), "slider limit enters");

    const auto first = set.canonical_digest();
    require(static_cast<bool>(first), "limit set digest computes");
    const auto decoded = kinematic::JointLimitSet::deserialize(set.serialize());
    require(static_cast<bool>(decoded), "limit set deserializes");
    require(decoded.value().canonical_digest().value() == first.value(),
        "limit set round trip preserves the digest");

    kinematic::JointLimitSet reordered;
    require(static_cast<bool>(reordered.insert(slider)),
        "slider limit re-enters");
    require(static_cast<bool>(reordered.insert(door_hinge_limit())),
        "hinge limit re-enters");
    require(reordered.canonical_digest().value() == first.value(),
        "limit set digest is independent of insertion order");

    require(!kinematic::JointLimitSet::deserialize(set.serialize() + "trailing"),
        "limit set parser rejects trailing unbound data");
    const auto future = kinematic::JointLimitSet::deserialize(
        "CARTOGRAPHER_JOINT_LIMITS 99\n");
    require(!future, "future limit set version is rejected");
    require(future.error().code == core::ErrorCode::version_mismatch,
        "future limit version reports version_mismatch");
    require(!kinematic::JointLimitSet::deserialize(
                "CARTOGRAPHER_JOINT_LIMITS 1\nLIMITS 4097\n"),
        "hostile limit count is rejected before allocation");
}

void motion_state_is_never_part_of_a_record() {
    const kinematic::KinematicGraph graph = door_graph();
    require(static_cast<bool>(
                kinematic::require_no_motion_state(graph.serialize())),
        "graph serialization passes the motion-state check");
    kinematic::JointLimitSet limits;
    require(static_cast<bool>(limits.insert(door_hinge_limit())),
        "hinge limit enters for motion scan");
    require(static_cast<bool>(
                kinematic::require_no_motion_state(limits.serialize())),
        "limit set serialization passes the motion-state check");

    require(!kinematic::require_no_motion_state(
                "CARTOGRAPHER_KINEMATIC_GRAPH 1\nPOSE 1 0 0 0\n"),
        "explicit pose payload is rejected");
    require(!kinematic::require_no_motion_state(
                "CARTOGRAPHER_KINEMATIC_GRAPH 1\nMOTION_STATE 1\n"),
        "explicit motion-state payload is rejected");
    require(!kinematic::require_no_motion_state(
                "SOME_RECORD 1\nVELOCITY 0.5 0.0 0.0\n"),
        "velocity payload is rejected");
    require(!kinematic::require_no_motion_state(
                "SOME_RECORD 1\nCURRENT_ANGLE 1.2\n"),
        "current-angle payload is rejected");

    // Quoted authoring text that happens to contain a tag substring as part of
    // a larger word must not trip the scanner.
    kinematic::KinematicGraph composed;
    kinematic::KinematicNode root;
    root.id = kinematic::JointId{1U};
    root.kind = kinematic::JointKind::fixed;
    root.node_class = kinematic::NodeClass::rigid;
    root.semantic_type = "COMPOSED";
    require(static_cast<bool>(composed.insert_node(root)),
        "composed-name node enters");
    require(static_cast<bool>(
                kinematic::require_no_motion_state(composed.serialize())),
        "quoted authoring text cannot impersonate a motion tag");
}

void door_mechanical_fixture_assembly() {
    const kinematic::KinematicGraph graph = door_graph();
    require(static_cast<bool>(graph.validate()), "door graph validates");

    kinematic::JointLimitSet limits;
    require(static_cast<bool>(limits.insert(door_hinge_limit())),
        "door hinge limit enters");
    require(static_cast<bool>(limits.validate_for_graph(graph)),
        "door limits resolve against the door graph");

    const kinematic::JointLimit* limit = limits.find(kinematic::JointId{2U});
    require(limit != nullptr && limit->angular_enabled,
        "door hinge limit is present");
    require(limit->contains_angular({0.0, 0.0, 0.0}),
        "closed door is inside its travel limit");
    require(!limit->contains_angular({3.0, 0.0, 0.0}),
        "door past its hinge limit is refused");

    require(static_cast<bool>(
                kinematic::require_no_motion_state(graph.serialize() + limits.serialize())),
        "assembled door record carries no motion state");
}

} // namespace

int main() {
    limits_validate_and_reference_containment();
    limit_validation_failures_are_reported();
    limit_set_round_trips_deterministically();
    motion_state_is_never_part_of_a_record();
    door_mechanical_fixture_assembly();
    std::cout << "kinematic limits tests passed\n";
    return EXIT_SUCCESS;
}
