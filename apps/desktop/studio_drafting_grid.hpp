#pragma once

#include "studio_view_settings.hpp"

#include <array>
#include <limits>

namespace carto::studio {

struct DraftingGridLine {
    core::Vec3d first;
    core::Vec3d second;
    bool world_axis = false;
};

struct DraftingGrid {
    static constexpr std::size_t kMaximumLinesPerAxis = 256U;
    double spacing = 0.0;
    std::vector<DraftingGridLine> lines;
};

// Presentation only: Front/Right grids lie in the camera target's depth plane;
// Plan and 3D retain the world-Y=0 ground grid. Grid coordinates remain world
// multiples while panning. No snapping, authoring or camera state is changed.
[[nodiscard]] inline core::Result<DraftingGrid> drafting_grid_for_view(
    ViewportViewKind kind, const editor::EditorCamera& camera,
    const editor::WorkSurfaceCamera& plan, core::Vec2d extent,
    double target_pixel_spacing) {
    using Result = core::Result<DraftingGrid>;
    const auto invalid = [](std::string message) {
        return Result::failure({core::ErrorCode::invalid_argument, std::move(message)});
    };
    if (!extent.finite() || extent.x <= 0.0 || extent.y <= 0.0 ||
        extent.x > 1.0e7 || extent.y > 1.0e7 ||
        !std::isfinite(target_pixel_spacing) || target_pixel_spacing < 1.0 ||
        target_pixel_spacing > 1024.0)
        return invalid("Drafting grid requires a finite bounded viewport and spacing");

    core::Vec3d right{1.0, 0.0, 0.0}, up{0.0, 0.0, 1.0}, offset{};
    core::Vec2d target{};
    double height = 0.0;
    if (kind == ViewportViewKind::top_plan_xz) {
        if (!plan.valid()) return invalid("Drafting grid plan camera is invalid");
        target = plan.target();
        height = plan.view_height();
    } else {
        if (!camera.valid()) return invalid("Drafting grid editor camera is invalid");
        height = camera.projection() == editor::EditorProjection::orthographic
            ? camera.orthographic_height()
            : 2.0 * camera.distance() * std::tan(camera.vertical_fov() * 0.5);
        switch (kind) {
        case ViewportViewKind::perspective_3d:
            target = {camera.target().x, camera.target().z};
            break;
        case ViewportViewKind::front_xy:
            up = {0.0, 1.0, 0.0};
            offset = {0.0, 0.0, camera.target().z};
            target = {camera.target().x, camera.target().y};
            break;
        case ViewportViewKind::right_zy:
            right = {0.0, 0.0, -1.0};
            up = {0.0, 1.0, 0.0};
            offset = {camera.target().x, 0.0, 0.0};
            target = {-camera.target().z, camera.target().y};
            break;
        default:
            return invalid("Drafting grid view kind is unsupported");
        }
        if (kind != ViewportViewKind::perspective_3d &&
            (camera.projection() != editor::EditorProjection::orthographic ||
             core::dot(camera.axes().right, right) < 1.0 - 1.0e-10 ||
             core::dot(camera.axes().up, up) < 1.0 - 1.0e-10))
            return invalid("Drafting grid fixed view does not match its camera projection");
    }

    const double width = height * (extent.x / extent.y);
    const core::Vec2d minimum{target.x - width * 0.5, target.y - height * 0.5};
    const core::Vec2d maximum{target.x + width * 0.5, target.y + height * 0.5};
    if (!std::isfinite(height) || height <= 0.0 || !std::isfinite(width) || width <= 0.0 ||
        !minimum.finite() || !maximum.finite() ||
        minimum.x >= maximum.x || minimum.y >= maximum.y)
        return invalid("Drafting grid bounds cannot be represented at this camera position");

    // Increase density spacing when necessary instead of truncating one side
    // of an ultrawide viewport after a fixed number of iterations.
    const double required = std::max({height * target_pixel_spacing / extent.y,
        width / 255.0, height / 255.0});
    const double decade = std::pow(10.0, std::floor(std::log10(required)));
    const double normalized = required / decade;
    const double step = decade * (normalized <= 1.0 ? 1.0 :
        normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0);
    if (!std::isfinite(step) || step <= 0.0)
        return invalid("Drafting grid spacing cannot be represented");
    DraftingGrid grid;
    grid.spacing = step;
    grid.lines.reserve(DraftingGrid::kMaximumLinesPerAxis * 2U);
    for (const bool vertical : {true, false}) {
        const double lower = vertical ? minimum.x : minimum.y;
        const double upper = vertical ? maximum.x : maximum.y;
        const double first_index = std::ceil(lower / step);
        const double last_index = std::floor(upper / step);
        const double count = last_index - first_index + 1.0;
        if (!std::isfinite(first_index) || !std::isfinite(last_index) ||
            !std::isfinite(count) || count > static_cast<double>(DraftingGrid::kMaximumLinesPerAxis))
            return invalid("Drafting grid exceeds its bounded line budget");
        double previous = -std::numeric_limits<double>::infinity();
        for (std::size_t index = 0U; static_cast<double>(index) < count; ++index) {
            const double coordinate = (first_index + static_cast<double>(index)) * step;
            if (!std::isfinite(coordinate) || coordinate <= previous)
                return invalid("Drafting grid coordinates lose precision at this camera position");
            previous = coordinate;
            const auto first = vertical
                ? offset + right * coordinate + up * minimum.y
                : offset + right * minimum.x + up * coordinate;
            const auto second = vertical
                ? offset + right * coordinate + up * maximum.y
                : offset + right * maximum.x + up * coordinate;
            if (!first.finite() || !second.finite())
                return invalid("Drafting grid produces non-finite world coordinates");
            grid.lines.push_back({first, second, coordinate == 0.0});
        }
    }
    return Result::success(std::move(grid));
}

} // namespace carto::studio
