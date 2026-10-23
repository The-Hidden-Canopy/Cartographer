#pragma once

#include <carto/core/math.hpp>

namespace carto::deform::detail {

// Shared exact quaternion/TRS helpers for the CPU reference evaluators.

[[nodiscard]] inline core::Quaternion quaternion_conjugate(
    core::Quaternion value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

[[nodiscard]] inline core::Quaternion quaternion_from_vector(
    core::Vec3d value) noexcept {
    return {value.x, value.y, value.z, 0.0};
}

[[nodiscard]] inline core::Vec3d vector_from_quaternion(
    core::Quaternion value) noexcept {
    return {value.x, value.y, value.z};
}

// Canonical hemisphere rule (character::coordinate): every stored or blended
// rotation is normalized to w >= 0.
[[nodiscard]] inline core::Quaternion hemisphere_normalized(
    core::Quaternion value) noexcept {
    core::Quaternion result = value.normalized();
    if (result.w < 0.0) {
        result = {-result.x, -result.y, -result.z, -result.w};
    }
    return result;
}

[[nodiscard]] inline core::Quaternion quaternion_negated(
    core::Quaternion value) noexcept {
    return {-value.x, -value.y, -value.z, -value.w};
}

// Exact TRS point application: translation + rotation * (scale * p).
[[nodiscard]] inline core::Vec3d transform_point(
    const core::Transform& transform,
    core::Vec3d point) noexcept {
    return transform.translation +
        transform.rotation.normalized().rotate(
            core::componentwise_multiply(transform.scale, point));
}

// Exact inverse TRS point application: scale^-1 * (rotation^-1 * (p - t)).
[[nodiscard]] inline core::Vec3d inverse_transform_point(
    const core::Transform& transform,
    core::Vec3d point) noexcept {
    const core::Quaternion inverse_rotation =
        quaternion_conjugate(transform.rotation.normalized());
    const core::Vec3d unrotated =
        inverse_rotation.rotate(point - transform.translation);
    return {
        unrotated.x / transform.scale.x,
        unrotated.y / transform.scale.y,
        unrotated.z / transform.scale.z,
    };
}

} // namespace carto::deform::detail
