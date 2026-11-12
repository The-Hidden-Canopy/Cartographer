#pragma once

#include "studio_view_settings.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace carto::studio {

// Resolve an instance's translation delta in the displayed authoring plane.
// Front/Right preserve the source's hidden coordinate, just as Plan preserves
// elevation. The 3D view retains its ground-projected X/Z placement behavior.
// This helper does not mutate the source, camera, selection or command history.
[[nodiscard]] inline core::Result<core::Vec3d> viewport_mesh_placement_offset(
    ViewportViewKind kind, const editor::EditorCamera& camera,
    const editor::WorkSurfaceCamera& plan, core::Vec2d screen_position,
    core::Vec2d viewport_origin, core::Vec2d viewport_extent,
    core::Vec3d source_translation) {
    using Result = core::Result<core::Vec3d>;
    const auto invalid = [](std::string message) {
        return Result::failure({core::ErrorCode::invalid_argument, std::move(message)});
    };
    if (!screen_position.finite() || !viewport_origin.finite() ||
        !viewport_extent.finite() || viewport_extent.x <= 0.0 ||
        viewport_extent.y <= 0.0 || viewport_extent.x > 1.0e9 ||
        viewport_extent.y > 1.0e9 || !source_translation.finite())
        return invalid("Mesh placement requires a finite source and bounded viewport");
    const auto pixel = screen_position - viewport_origin;
    if (!pixel.finite() || pixel.x < 0.0 || pixel.y < 0.0 ||
        pixel.x > viewport_extent.x || pixel.y > viewport_extent.y)
        return invalid("Mesh placement pointer is outside the current viewport");

    core::Vec3d offset{};
    if (kind == ViewportViewKind::top_plan_xz) {
        const auto world = plan.screen_to_world(screen_position, viewport_origin, viewport_extent);
        if (!world) return Result::failure(world.error());
        offset = {world.value().x - source_translation.x, 0.0,
            world.value().y - source_translation.z};
    } else {
        core::Vec3d plane_normal{0.0, 1.0, 0.0};
        core::Vec3d plane_point{};
        if (kind == ViewportViewKind::front_xy || kind == ViewportViewKind::right_zy) {
            const auto expected = viewport_view_axes(kind);
            if (!camera.valid() || !expected ||
                camera.projection() != editor::EditorProjection::orthographic ||
                core::dot(camera.axes().right, expected.value().screen_right) < 1.0 - 1.0e-10 ||
                core::dot(camera.axes().up, expected.value().screen_up) < 1.0 - 1.0e-10)
                return invalid("Mesh placement view does not match its fixed camera projection");
            plane_normal = kind == ViewportViewKind::front_xy
                ? core::Vec3d{0.0, 0.0, 1.0} : core::Vec3d{1.0, 0.0, 0.0};
            plane_point = source_translation;
        } else if (kind != ViewportViewKind::perspective_3d) {
            return invalid("Mesh placement view kind is unsupported");
        }
        const auto ray = camera.ray_for_pixel(pixel, viewport_extent);
        if (!ray) return Result::failure(ray.error());
        const double denominator = core::dot(ray.value().direction, plane_normal);
        if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-10)
            return invalid("Mesh placement ray is parallel to its authoring plane");
        const double distance = core::dot(plane_point - ray.value().origin, plane_normal) /
            denominator;
        if (!std::isfinite(distance) || distance < 0.0)
            return invalid("Mesh placement plane is behind the active camera");
        const auto world = ray.value().origin + ray.value().direction * distance;
        if (!world.finite()) return invalid("Mesh placement produces a non-finite position");
        offset = world - source_translation;
        if (kind == ViewportViewKind::front_xy) offset.z = 0.0;
        else if (kind == ViewportViewKind::right_zy) offset.x = 0.0;
        else offset.y = 0.0;
    }
    if (!offset.finite()) return invalid("Mesh placement translation cannot be represented");
    return Result::success(offset);
}

} // namespace carto::studio
