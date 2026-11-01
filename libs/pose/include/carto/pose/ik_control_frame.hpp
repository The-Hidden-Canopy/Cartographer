#pragma once

#include <carto/pose/controls.hpp>
#include <carto/pose/ik_fk_authoring.hpp>

#include <string>

namespace carto::pose {

// A caller assertion, not an owned ControlSpace or a verified source frame.
// XYZ of the target control is an absolute point in this frame; XYZ of the
// pole control is a direction in this frame. Translation never applies to the
// direction. Both use the rig's default coordinate policy and asserted scene
// unit; no conversion, runtime binding or physical-world claim is implied.
struct IkControlRigidFrame {
    assets::Sha256Digest authoring_digest;
    std::string frame_identity;
    std::string scene_unit_identity;
    core::Transform graph_world_from_control;
    double max_point_round_trip_error = 0.0; // scene units
    double max_direction_round_trip_error = 0.0; // dimensionless vector norm
};

// The candidate is not an applied or admitted edit. The solver has evaluated
// the decoded channels only for the supplied FK pose, not a time interval.
struct FkToIkControlFrameProposal {
    ControlValues candidate_values;
    IkControlState decoded_ik;
    IkFkSwitchProposal match;
    IkChainResult decoded_solve;
    double decoded_position_jump = 0.0;
};

[[nodiscard]] core::Result<IkControlState> decode_ik_control_frame(
    const IkControlRigidFrame&, const IkFkAuthoring&, const character::RigDefinition&,
    const kinematic::KinematicGraph&, const kinematic::JointLimitSet&, const ControlValues&);

[[nodiscard]] core::Result<FkToIkControlFrameProposal> propose_fk_to_ik_control_frame(
    const IkControlRigidFrame&, const IkFkAuthoring&, const character::RigDefinition&,
    const kinematic::KinematicGraph&, const kinematic::JointLimitSet&,
    const LocalPose& fk_locals, const ControlValues& current_values);

} // namespace carto::pose
