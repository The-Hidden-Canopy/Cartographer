#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

namespace carto::deform {

// Exact TRS application and inversion for the CPU reference skinning path.
// core::Transform composes as translation + rotation * (scale * p); these
// helpers are the inverse operations and never silently assume unit scale.
[[nodiscard]] core::Vec3d apply_transform(
    const core::Transform& transform,
    core::Vec3d point) noexcept;

[[nodiscard]] core::Vec3d apply_inverse_transform(
    const core::Transform& transform,
    core::Vec3d point) noexcept;

// Rotation/translation delta of a posed joint against its bind transform,
// valid only when both transforms are rigid (unit scale); returns failure
// otherwise so dual-quaternion skinning rejects shear/scale instead of
// approximating it.
[[nodiscard]] core::Result<core::Transform> rigid_delta_transform(
    const core::Transform& pose,
    const core::Transform& rest) noexcept;

// Componentwise scale of a transform; used by callers that must declare an
// affine subset before evaluation.
[[nodiscard]] core::Vec3d transform_scale(const core::Transform& transform) noexcept;

} // namespace carto::deform
