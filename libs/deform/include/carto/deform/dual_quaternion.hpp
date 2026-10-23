#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

namespace carto::deform {

// Dual quaternion in (real, dual) form, following the character::coordinate
// orientation transport: stored rotations live on the canonical hemisphere
// w >= 0, so blended sums are sign-corrected against a reference before
// accumulation and re-normalized to that hemisphere on extraction.
struct DualQuaternion {
    core::Quaternion real = core::Quaternion::identity();
    core::Quaternion dual{};
};

[[nodiscard]] core::Result<DualQuaternion> dual_quaternion_from_rigid(
    const core::Transform& transform) noexcept;

[[nodiscard]] core::Transform rigid_from_dual_quaternion(
    const DualQuaternion& value) noexcept;

[[nodiscard]] DualQuaternion operator+(
    DualQuaternion left,
    DualQuaternion right) noexcept;

[[nodiscard]] DualQuaternion operator*(
    DualQuaternion value,
    double scalar) noexcept;

// Real-part dot product; negative values mean the operands live on opposite
// quaternion hemispheres and one of them must be negated before blending.
[[nodiscard]] double real_dot(
    const DualQuaternion& left,
    const DualQuaternion& right) noexcept;

// Normalization of a (possibly non-unit) blended dual quaternion back onto
// the canonical w >= 0 hemisphere.
[[nodiscard]] DualQuaternion normalized(const DualQuaternion& value) noexcept;

} // namespace carto::deform
