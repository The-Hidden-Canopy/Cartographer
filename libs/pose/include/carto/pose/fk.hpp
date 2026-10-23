#pragma once

#include <carto/character/coordinate.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <map>
#include <optional>

namespace carto::pose {

// Forward kinematics for one posed state of an authored kinematic tree.
//
// A posed state is described by local transform overrides keyed by JointId;
// every joint absent from the map evaluates at its authored rest transform.
// The empty map therefore means "rest pose". Overrides are durable evaluation
// input here, never persisted motion state: the kinematic records themselves
// still carry no pose (see kinematic::require_no_motion_state).
using LocalPose = std::map<kinematic::JointId, core::Transform>;

// Result of one FK evaluation. `world` holds the composed world transform of
// every joint in the graph; `root_frame` is the coordinate-policy frame the
// evaluation was composed in (identity for the default right-handed +Y-up
// policy at unit scale).
struct FkPose {
    std::map<kinematic::JointId, core::Transform> world;
    core::Transform root_frame = core::Transform::identity();
};

// Builds the world-space root frame implied by a coordinate policy: the
// declared up/forward axes become the frame's Y/Z basis columns, the right
// axis follows the declared handedness (right-handed: right = up x forward),
// and meters_per_unit becomes a uniform scale on translation transport.
// Up and forward must not be collinear; scale must be finite and positive.
[[nodiscard]] core::Result<core::Transform> policy_frame_transform(
    const character::CoordinatePolicy& policy);

// Composes local->world transforms for the whole tree at `locals` (rest
// defaults for absent joints). `policy` defaults to the Cartographer canonical
// policy (right-handed, +Y up, +Z forward, unit scale); a non-default policy
// composes the whole tree under its root frame so downstream consumers see
// one consistent convention with no hidden runtime axis flip.
//
// Fails closed when the graph is invalid, a local override names a joint that
// does not exist, or a policy field is unsupported.
[[nodiscard]] core::Result<FkPose> evaluate_fk(
    const kinematic::KinematicGraph& graph,
    const LocalPose& locals = {},
    const std::optional<character::CoordinatePolicy>& policy = std::nullopt);

// Extracts the local pose of one joint from a composed FK result, i.e. the
// transform that, given its parent's world transform, reproduces `world`.
// Fails closed when the parent world transform carries a non-unit scale
// (the CPU reference solvers refuse non-uniform scale rather than guessing).
[[nodiscard]] core::Result<core::Transform> local_from_world(
    const core::Transform& parent_world,
    const core::Transform& world);

} // namespace carto::pose
