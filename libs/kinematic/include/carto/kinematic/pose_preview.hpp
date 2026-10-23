#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>

namespace carto::kinematic {

// Read-only editor preview coordinates for one authored hinge. The angle is
// derived from a normalized control position and authored limits; it is not
// current pose, simulation state, or serialized project truth.
struct HingePosePreview {
    JointId joint;
    core::Vec3d pivot_in_root;
    core::Vec3d axis_in_root;
    double angle_radians = 0.0;
};

// Evaluates one bounded hinge at normalized_travel in [0, 1]. The returned
// pivot and axis are expressed in the graph root frame. Unsupported joint
// kinds/axis conventions fail closed instead of inventing a pose policy.
[[nodiscard]] core::Result<HingePosePreview> preview_hinge_pose(
    const KinematicGraph& graph,
    const JointLimitSet& limits,
    JointId joint,
    double normalized_travel);

// Evaluates the whole chain at bounded per-joint normalized travel values in
// [0, 1], mapping each control through the joint's authored limits; a joint
// without a control samples its authored lower bound. Every returned transform
// is derived and expressed in the graph root frame: it is not current pose,
// simulation state, or serialized project truth. Fixed, hinge, and slider
// joints compose deterministically from the root; ball joints, unbounded
// motion axes, and malformed graphs fail closed instead of inventing a pose
// policy.
[[nodiscard]] core::Result<std::map<JointId, core::Transform>> preview_chain_poses(
    const KinematicGraph& graph,
    const JointLimitSet& limits,
    const std::map<JointId, double>& normalized_travel);

} // namespace carto::kinematic
