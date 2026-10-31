#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/d3d12/scene_renderer.hpp>
#include <carto/d3d12/viewport_input.hpp>

#include <cmath>
#include <cstdint>
#include <utility>

namespace carto::d3d12 {

// Builds each transient viewport camera from the same immutable base camera.
// This keeps cumulative pan state from being applied repeatedly to the prior
// frame's already-panned request, including during temporal accumulation.
[[nodiscard]] inline core::Result<SceneRenderCamera> apply_viewport_camera_input(
    const SceneRenderCamera& base_camera,
    double base_distance,
    std::uint32_t viewport_height,
    const ViewportInputState& input) {
    using Output = core::Result<SceneRenderCamera>;
    constexpr double kPi = 3.14159265358979323846;
    const double zoom = input.zoom();
    const double dpi_scale = input.dpi_scale();
    if (!base_camera.position.finite() || !base_camera.target.finite() ||
        !base_camera.up.finite() || !std::isfinite(base_distance) ||
        base_distance <= 0.0 || viewport_height == 0U ||
        !std::isfinite(zoom) || zoom <= 0.0 ||
        !std::isfinite(dpi_scale) || dpi_scale <= 0.0) {
        return Output::failure({core::ErrorCode::validation_failed,
            "viewport camera base or extent is invalid"});
    }
    if (base_camera.projection == SceneRenderProjection::perspective &&
        (!std::isfinite(base_camera.vertical_fov) ||
         base_camera.vertical_fov <= 0.0 || base_camera.vertical_fov >= kPi)) {
        return Output::failure({core::ErrorCode::validation_failed,
            "viewport perspective camera field of view is invalid"});
    }
    if (base_camera.projection == SceneRenderProjection::orthographic &&
        (!std::isfinite(base_camera.orthographic_height) ||
         base_camera.orthographic_height <= 0.0)) {
        return Output::failure({core::ErrorCode::validation_failed,
            "viewport orthographic camera height is invalid"});
    }

    constexpr double kDegreesToRadians = kPi / 180.0;
    const double yaw = input.yaw_degrees() * kDegreesToRadians;
    const double pitch = input.pitch_degrees() * kDegreesToRadians;
    const double cosine_pitch = std::cos(pitch);
    const core::Vec3d direction{
        std::sin(yaw) * cosine_pitch,
        std::sin(pitch),
        std::cos(yaw) * cosine_pitch};
    const core::Vec3d right{std::cos(yaw), 0.0, -std::sin(yaw)};
    const core::Vec3d up{
        -std::sin(yaw) * std::sin(pitch),
        cosine_pitch,
        -std::cos(yaw) * std::sin(pitch)};

    SceneRenderCamera camera = base_camera;
    const bool perspective = camera.projection == SceneRenderProjection::perspective;
    const double distance = perspective ? base_distance / zoom : base_distance;
    if (!perspective) camera.orthographic_height = base_camera.orthographic_height / zoom;
    const double logical_height = static_cast<double>(viewport_height) / dpi_scale;
    const double world_per_logical_pixel = perspective
        ? 2.0 * distance * std::tan(camera.vertical_fov * 0.5) / logical_height
        : camera.orthographic_height / logical_height;
    camera.target = base_camera.target + right * (
            input.pan_x_logical_pixels() * world_per_logical_pixel) +
        up * (input.pan_y_logical_pixels() * world_per_logical_pixel);
    camera.position = camera.target + direction * distance;
    camera.up = up;
    if (!camera.position.finite() || !camera.target.finite() || !camera.up.finite() ||
        !std::isfinite(world_per_logical_pixel) || world_per_logical_pixel <= 0.0) {
        return Output::failure({core::ErrorCode::validation_failed,
            "viewport camera input produced a non-finite view"});
    }
    return Output::success(std::move(camera));
}

} // namespace carto::d3d12
