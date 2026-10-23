#include <carto/deform/affine.hpp>

#include <carto/deform/detail/math_ops.hpp>

#include <cmath>

namespace carto::deform {

namespace {

constexpr double kRigidityTolerance = 1e-9;

[[nodiscard]] bool unit_scale(const core::Vec3d& scale) noexcept {
    return std::abs(scale.x - 1.0) <= kRigidityTolerance &&
        std::abs(scale.y - 1.0) <= kRigidityTolerance &&
        std::abs(scale.z - 1.0) <= kRigidityTolerance;
}

} // namespace

core::Vec3d apply_transform(const core::Transform& transform, core::Vec3d point) noexcept {
    return detail::transform_point(transform, point);
}

core::Vec3d apply_inverse_transform(const core::Transform& transform, core::Vec3d point) noexcept {
    return detail::inverse_transform_point(transform, point);
}

core::Result<core::Transform> rigid_delta_transform(
    const core::Transform& pose,
    const core::Transform& rest) noexcept {
    if (!pose.finite() || !rest.finite()) {
        return core::Result<core::Transform>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "rigid delta requires finite pose and rest transforms"));
    }
    if (!unit_scale(pose.scale) || !unit_scale(rest.scale)) {
        return core::Result<core::Transform>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "dual-quaternion skinning requires rigid joints: non-unit scale "
            "in pose or rest transform"));
    }
    const core::Quaternion inverse_rest_rotation =
        detail::quaternion_conjugate(rest.rotation.normalized());
    core::Transform delta;
    delta.rotation = detail::hemisphere_normalized(
        pose.rotation.normalized() * inverse_rest_rotation);
    delta.translation = pose.translation - delta.rotation.rotate(rest.translation);
    delta.scale = {1.0, 1.0, 1.0};
    return core::Result<core::Transform>::success(delta);
}

core::Vec3d transform_scale(const core::Transform& transform) noexcept {
    return transform.scale;
}

} // namespace carto::deform
