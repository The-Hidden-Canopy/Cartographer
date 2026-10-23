#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/pose/fk.hpp>
#include <carto/pose/ik.hpp>

#include <optional>

namespace carto::pose {

// IK/FK matching for one two-bone limb (ch10 "IK/FK switch without pose
// jumps"). Switching a limb between FK and IK controls must reproduce the
// pose the artist sees: matching computes the other mode's control values
// from the current pose and proves the switch with a measured jump before a
// caller may key or commit it. Both directions are pure; they never mutate
// the graph or the caller's pose.

struct TwoBoneChain {
    kinematic::JointId root;
    kinematic::JointId mid;
    kinematic::JointId effector;
};

// IK control values for a two-bone limb: an effector target and an optional
// world-space pole direction selecting the bend plane (the same contract as
// TwoBoneIkRequest).
struct IkControlState {
    core::Vec3d target{};
    std::optional<core::Vec3d> pole_vector;
};

struct IkFkMatchRequest {
    TwoBoneChain chain;
    IkBudget budget;
    // Largest admitted world-space position change of root, mid, or effector
    // between the source pose and the matched pose. Strictly positive and
    // finite.
    double max_position_jump = 1e-6;
};

struct IkFkMatchResult {
    // IK controls that reproduce the matched pose.
    IkControlState ik;
    // FK local overrides for the chain joints that reproduce the matched pose
    // when composed over the caller's base locals.
    LocalPose fk_locals;
    // Measured world-space position change of the chain joints between the
    // source pose and the matched pose.
    double position_jump = 0.0;
    // Measured effector orientation change in radians. Two-bone IK does not
    // solve effector orientation, so this is reported, not enforced; callers
    // that need orientation continuity key the effector rotation explicitly.
    double effector_rotation_jump = 0.0;
    // Final IK residual of the verification solve.
    double residual = 0.0;
};

// FK -> IK: derives the IK target and pole from the posed FK chain, solves
// two-bone IK with those controls, and refuses when the IK solution would
// move a chain joint further than max_position_jump. A straight (fully
// extended) limb has no bend plane; it matches with no pole vector.
[[nodiscard]] core::Result<IkFkMatchResult> match_ik_to_fk(
    const kinematic::KinematicGraph& graph,
    const LocalPose& fk_locals,
    const kinematic::JointLimitSet* limits,
    const IkFkMatchRequest& request);

// IK -> FK: solves the IK controls over the base pose and returns the FK
// locals that hold that pose once IK is switched off. Refuses when the IK
// solve does not converge within max_position_jump of the target.
[[nodiscard]] core::Result<IkFkMatchResult> match_fk_to_ik(
    const kinematic::KinematicGraph& graph,
    const LocalPose& base_locals,
    const kinematic::JointLimitSet* limits,
    const IkControlState& ik,
    const IkFkMatchRequest& request);

} // namespace carto::pose
