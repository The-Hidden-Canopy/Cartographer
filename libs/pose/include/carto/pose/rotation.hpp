#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cmath>

namespace carto::pose {

// Quaternion utilities shared by every pose evaluator in this library. All
// constructors normalize to the canonical w >= 0 hemisphere declared by the
// character coordinate policy, and all inputs must be finite (callers validate
// at record level). These helpers exist because carto::core::Quaternion is a
// deliberately minimal storage type.

// Rotation of `angle` radians about a unit `axis`. Fails closed on a
// non-finite axis or angle; a near-zero axis yields the identity rotation.
[[nodiscard]] core::Quaternion axis_angle(core::Vec3d axis, double angle);

// Quaternion rotating unit `from` onto unit `to`, shortest arc. Antiparallel
// inputs resolve to a deterministic orthogonal axis so the result never
// depends on unordered state or luck.
[[nodiscard]] core::Quaternion rotate_between(core::Vec3d from, core::Vec3d to);

// Inverse of a normalized rotation (conjugate).
[[nodiscard]] core::Quaternion inverse(core::Quaternion rotation);

// Deterministic normalized-lerp on the same quaternion hemisphere. Suitable
// for CPU-reference blending where slerp's branch behavior is unnecessary;
// `t` is clamped to [0, 1].
[[nodiscard]] core::Quaternion nlerp(core::Quaternion from, core::Quaternion to, double t);

// Rotation magnitude in radians for a normalized quaternion in [0, pi].
[[nodiscard]] double rotation_angle(core::Quaternion rotation);

// Euler decomposition convention for the whole library (documented CPU
// reference convention): q = qx(rx) * qy(ry) * qz(rz), i.e. the composed
// rotation applies the Z angle first, then Y, then X in the parent frame.
// Both directions are deterministic; gimbal-lock neighborhoods collapse z to
// zero and recover x + z, matching to_euler_xyz exactly.
[[nodiscard]] core::Quaternion from_euler_xyz(core::Vec3d angles);
[[nodiscard]] core::Vec3d to_euler_xyz(core::Quaternion rotation);

} // namespace carto::pose
