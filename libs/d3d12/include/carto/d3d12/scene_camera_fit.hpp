#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace carto::d3d12 {

// Returns the camera distance needed to keep a sphere inside the horizontal
// and vertical field of view for the current viewport extent.
[[nodiscard]] inline std::optional<double> fit_camera_distance_to_sphere(
    double sphere_radius,
    double vertical_fov_radians,
    std::uint32_t width,
    std::uint32_t height,
    double margin = 1.2) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    if (!std::isfinite(sphere_radius) || sphere_radius <= 0.0 ||
        !std::isfinite(vertical_fov_radians) ||
        vertical_fov_radians <= 0.0 || vertical_fov_radians >= kPi ||
        width == 0U || height == 0U || !std::isfinite(margin) || margin < 1.0) {
        return std::nullopt;
    }

    const double aspect = static_cast<double>(width) /
        static_cast<double>(height);
    const double horizontal_fov = 2.0 * std::atan(
        std::tan(vertical_fov_radians * 0.5) * aspect);
    const double limiting_fov = std::min(vertical_fov_radians, horizontal_fov);
    const double half_fov_sine = std::sin(limiting_fov * 0.5);
    if (!std::isfinite(aspect) || aspect <= 0.0 ||
        !std::isfinite(half_fov_sine) || half_fov_sine <= 0.0) {
        return std::nullopt;
    }

    const double distance = sphere_radius * margin / half_fov_sine;
    return std::isfinite(distance) && distance > 0.0
        ? std::optional<double>{distance}
        : std::nullopt;
}

} // namespace carto::d3d12
