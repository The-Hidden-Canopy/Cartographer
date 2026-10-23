#include <carto/deform/dual_quaternion.hpp>

#include <carto/deform/detail/math_ops.hpp>

#include <cmath>
#include <limits>

namespace carto::deform {

namespace {

constexpr double kMagnitudeEpsilon = 1e-12;
constexpr double kRigidityTolerance = 1e-9;

[[nodiscard]] core::Quaternion quaternion_scaled(core::Quaternion value, double scalar) noexcept {
    return {
        value.x * scalar, value.y * scalar, value.z * scalar, value.w * scalar};
}

} // namespace

core::Result<DualQuaternion> dual_quaternion_from_rigid(
    const core::Transform& transform) noexcept {
    const double scale_error =
        std::abs(transform.scale.x - 1.0) + std::abs(transform.scale.y - 1.0) +
        std::abs(transform.scale.z - 1.0);
    if (!transform.finite() || scale_error > kRigidityTolerance) {
        return core::Result<DualQuaternion>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "dual quaternion conversion requires a rigid transform"));
    }
    DualQuaternion value;
    value.real = detail::hemisphere_normalized(transform.rotation);
    value.dual = quaternion_scaled(
        detail::quaternion_from_vector(transform.translation) * value.real, 0.5);
    return core::Result<DualQuaternion>::success(value);
}

core::Transform rigid_from_dual_quaternion(const DualQuaternion& value) noexcept {
    const DualQuaternion unit = normalized(value);
    core::Transform transform;
    transform.rotation = unit.real;
    const core::Quaternion translation_quaternion = quaternion_scaled(
        unit.dual * detail::quaternion_conjugate(unit.real), 2.0);
    transform.translation = detail::vector_from_quaternion(translation_quaternion);
    transform.scale = {1.0, 1.0, 1.0};
    return transform;
}

DualQuaternion operator+(DualQuaternion left, DualQuaternion right) noexcept {
    return {
        {
            left.real.x + right.real.x,
            left.real.y + right.real.y,
            left.real.z + right.real.z,
            left.real.w + right.real.w,
        },
        {
            left.dual.x + right.dual.x,
            left.dual.y + right.dual.y,
            left.dual.z + right.dual.z,
            left.dual.w + right.dual.w,
        },
    };
}

DualQuaternion operator*(DualQuaternion value, double scalar) noexcept {
    return {
        quaternion_scaled(value.real, scalar),
        quaternion_scaled(value.dual, scalar),
    };
}

double real_dot(const DualQuaternion& left, const DualQuaternion& right) noexcept {
    return left.real.x * right.real.x + left.real.y * right.real.y +
        left.real.z * right.real.z + left.real.w * right.real.w;
}

DualQuaternion normalized(const DualQuaternion& value) noexcept {
    const double real_magnitude = std::hypot(
        std::hypot(value.real.x, value.real.y),
        std::hypot(value.real.z, value.real.w));
    if (!std::isfinite(real_magnitude) || real_magnitude <= kMagnitudeEpsilon) {
        const double invalid = std::numeric_limits<double>::quiet_NaN();
        return {
            {invalid, invalid, invalid, invalid},
            {invalid, invalid, invalid, invalid},
        };
    }
    const double inverse = 1.0 / real_magnitude;
    DualQuaternion result = value * inverse;
    if (result.real.w < 0.0) {
        result.real = detail::quaternion_negated(result.real);
        result.dual = detail::quaternion_negated(result.dual);
    }
    return result;
}

} // namespace carto::deform
