#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace carto::core {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] bool finite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    [[nodiscard]] double length_squared() const noexcept { return x * x + y * y + z * z; }
    [[nodiscard]] double length() const noexcept { return std::hypot(std::hypot(x, y), z); }

    [[nodiscard]] Vec3d normalized(double epsilon = 1e-12) const noexcept {
        const double magnitude = length();
        if (!std::isfinite(magnitude) || magnitude <= epsilon) {
            const double invalid = std::numeric_limits<double>::quiet_NaN();
            return {invalid, invalid, invalid};
        }
        return {x / magnitude, y / magnitude, z / magnitude};
    }
};

[[nodiscard]] constexpr Vec3d operator+(Vec3d left, Vec3d right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] constexpr Vec3d operator-(Vec3d left, Vec3d right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] constexpr Vec3d operator*(Vec3d value, double scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] constexpr Vec3d operator*(double scalar, Vec3d value) noexcept {
    return value * scalar;
}

[[nodiscard]] constexpr Vec3d componentwise_multiply(Vec3d left, Vec3d right) noexcept {
    return {left.x * right.x, left.y * right.y, left.z * right.z};
}

[[nodiscard]] constexpr double dot(Vec3d left, Vec3d right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] constexpr Vec3d cross(Vec3d left, Vec3d right) noexcept {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

struct Quaternion {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;

    [[nodiscard]] static constexpr Quaternion identity() noexcept { return {}; }

    [[nodiscard]] bool finite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
    }

    [[nodiscard]] bool normalizable(double epsilon = 1e-12) const noexcept {
        const double magnitude = std::hypot(std::hypot(x, y), std::hypot(z, w));
        return finite() && std::isfinite(magnitude) && magnitude > epsilon;
    }

    [[nodiscard]] Quaternion normalized(double epsilon = 1e-12) const noexcept {
        const double magnitude = std::hypot(std::hypot(x, y), std::hypot(z, w));
        if (!normalizable(epsilon)) {
            const double invalid = std::numeric_limits<double>::quiet_NaN();
            return {invalid, invalid, invalid, invalid};
        }
        return {x / magnitude, y / magnitude, z / magnitude, w / magnitude};
    }

    [[nodiscard]] Vec3d rotate(Vec3d value) const noexcept {
        const Quaternion q = normalized();
        const Vec3d q_vector{q.x, q.y, q.z};
        const Vec3d twice_cross = cross(q_vector, value) * 2.0;
        return value + twice_cross * q.w + cross(q_vector, twice_cross);
    }
};

[[nodiscard]] constexpr Quaternion operator*(Quaternion left, Quaternion right) noexcept {
    return {
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    };
}

struct Transform {
    Vec3d translation{};
    Quaternion rotation = Quaternion::identity();
    Vec3d scale{1.0, 1.0, 1.0};

    [[nodiscard]] static constexpr Transform identity() noexcept { return {}; }

    [[nodiscard]] bool finite() const noexcept {
        return translation.finite() && rotation.normalizable() && scale.finite();
    }

    [[nodiscard]] Transform combine(const Transform& child) const noexcept {
        const Quaternion parent_rotation = rotation.normalized();
        const Quaternion child_rotation = child.rotation.normalized();
        return {
            translation + parent_rotation.rotate(componentwise_multiply(scale, child.translation)),
            (parent_rotation * child_rotation).normalized(),
            componentwise_multiply(scale, child.scale),
        };
    }
};

struct Bounds3d {
    Vec3d minimum{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
    };
    Vec3d maximum{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };

    void include(Vec3d point) noexcept {
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        maximum.z = std::max(maximum.z, point.z);
    }

    [[nodiscard]] bool valid() const noexcept {
        return minimum.finite() && maximum.finite() && minimum.x <= maximum.x &&
               minimum.y <= maximum.y && minimum.z <= maximum.z;
    }
};

} // namespace carto::core
