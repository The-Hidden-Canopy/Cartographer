#include <carto/pose/rotation.hpp>

#include <algorithm>
#include <cmath>

namespace carto::pose {
namespace {

core::Quaternion hemisphered(double x, double y, double z, double w) {
    core::Quaternion rotation{x, y, z, w};
    const double magnitude =
        std::hypot(std::hypot(x, y), std::hypot(z, w));
    rotation = {x / magnitude, y / magnitude, z / magnitude, w / magnitude};
    if (rotation.w < 0.0) {
        rotation = {-rotation.x, -rotation.y, -rotation.z, -rotation.w};
    }
    return rotation;
}

} // namespace

core::Quaternion axis_angle(core::Vec3d axis, double angle) {
    const double magnitude = axis.length();
    if (!std::isfinite(magnitude) || magnitude <= 1e-12) {
        return core::Quaternion::identity();
    }
    const double half = angle * 0.5;
    const double scale = std::sin(half) / magnitude;
    return hemisphered(axis.x * scale, axis.y * scale, axis.z * scale, std::cos(half));
}

core::Quaternion rotate_between(core::Vec3d from, core::Vec3d to) {
    const core::Vec3d a = from.normalized();
    const core::Vec3d b = to.normalized();
    if (!a.finite() || !b.finite()) {
        return core::Quaternion::identity();
    }
    const double d = std::clamp(dot(a, b), -1.0, 1.0);
    if (d > 1.0 - 1e-12) {
        return core::Quaternion::identity();
    }
    if (d < -1.0 + 1e-12) {
        // Antiparallel: any orthogonal axis gives the same half-turn; pick
        // the deterministic least-aligned coordinate axis.
        core::Vec3d fallback = std::abs(a.x) < 0.9
            ? core::Vec3d{1.0, 0.0, 0.0}
            : core::Vec3d{0.0, 1.0, 0.0};
        const core::Vec3d axis = cross(a, fallback).normalized();
        return hemisphered(axis.x, axis.y, axis.z, 0.0);
    }
    const core::Vec3d cross_product = cross(a, b);
    return hemisphered(cross_product.x, cross_product.y, cross_product.z, 1.0 + d);
}

core::Quaternion inverse(core::Quaternion rotation) {
    const core::Quaternion normalized = rotation.normalized();
    // Conjugate, re-hemisphered so the canonical sign rule holds.
    return hemisphered(-normalized.x, -normalized.y, -normalized.z, normalized.w);
}

core::Quaternion nlerp(core::Quaternion from, core::Quaternion to, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    core::Quaternion a = from.normalized();
    core::Quaternion b = to.normalized();
    // Keep the pair on one hemisphere before interpolating.
    if (dot(core::Vec3d{a.x, a.y, a.z}, core::Vec3d{b.x, b.y, b.z}) +
            a.w * b.w < 0.0) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    return hemisphered(
        a.x + (b.x - a.x) * clamped,
        a.y + (b.y - a.y) * clamped,
        a.z + (b.z - a.z) * clamped,
        a.w + (b.w - a.w) * clamped);
}

double rotation_angle(core::Quaternion rotation) {
    const core::Quaternion normalized = rotation.normalized();
    return 2.0 * std::acos(std::clamp(normalized.w, -1.0, 1.0));
}

core::Quaternion from_euler_xyz(core::Vec3d angles) {
    const double hx = angles.x * 0.5;
    const double hy = angles.y * 0.5;
    const double hz = angles.z * 0.5;
    const core::Quaternion qx{std::sin(hx), 0.0, 0.0, std::cos(hx)};
    const core::Quaternion qy{0.0, std::sin(hy), 0.0, std::cos(hy)};
    const core::Quaternion qz{0.0, 0.0, std::sin(hz), std::cos(hz)};
    const core::Quaternion composed = (qx * qy) * qz;
    return composed.normalized().w >= 0.0
        ? composed.normalized()
        : core::Quaternion{-composed.x, -composed.y, -composed.z, -composed.w}
              .normalized();
}

core::Vec3d to_euler_xyz(core::Quaternion rotation) {
    const core::Quaternion q = rotation.normalized();
    // R = Rx * Ry * Rz with elements read off the composed rotation matrix:
    //   R[0][2] = sin(y)            -> y
    //   R[1][2] = -sin(x)cos(y)     \
    //   R[2][2] =  cos(x)cos(y)      -> x = atan2(-R[1][2], R[2][2])
    //   R[0][1] = -cos(y)sin(z)     \
    //   R[0][0] =  cos(y)cos(z)      -> z = atan2(-R[0][1], R[0][0])
    // with each R element expanded in quaternion components below.
    const double sin_y = std::clamp(2.0 * (q.w * q.y + q.x * q.z), -1.0, 1.0);
    const double y = std::asin(sin_y);
    double x = 0.0;
    double z = 0.0;
    if (std::abs(sin_y) < 1.0 - 1e-9) {
        x = std::atan2(2.0 * (q.w * q.x - q.y * q.z),
                       1.0 - 2.0 * (q.x * q.x + q.y * q.y));
        z = std::atan2(2.0 * (q.w * q.z - q.x * q.y),
                       1.0 - 2.0 * (q.y * q.y + q.z * q.z));
    } else {
        // Gimbal lock (cos(y) == 0): x and z are coupled; fix z = 0 and
        // recover x + z deterministically from the middle row.
        x = std::atan2(2.0 * (q.x * q.y + q.w * q.z),
                       1.0 - 2.0 * (q.x * q.x + q.z * q.z));
    }
    return {x, y, z};
}

} // namespace carto::pose
