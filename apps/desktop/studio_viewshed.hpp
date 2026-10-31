#pragma once

#include <carto/application/application.hpp>
#include <carto/editor/observation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace carto::studio {

struct ViewshedObserverBinding {
    std::optional<scene::ObjectId> object;
    std::uint64_t project_generation = 0U;
};

struct ResolvedViewshedObserver {
    core::Vec3d position;
    std::optional<scene::ObjectId> object;
};

struct ViewshedCellFootprint {
    // Clockwise in the terrain grid's X/Z plane, with the sample's authored
    // elevation retained for this geometry-only preview.
    std::array<core::Vec3d, 4U> corners{};
};

inline constexpr std::size_t kViewshedPerspectiveDepthGridAxis = 5U;
inline constexpr std::size_t kViewshedPerspectiveDepthSampleCount =
    kViewshedPerspectiveDepthGridAxis * kViewshedPerspectiveDepthGridAxis;
inline constexpr std::size_t kViewshedPerspectiveDepthGridAxisMax = 17U;

struct ViewshedCellDepthSamples {
    std::vector<core::Vec3d> points;
};

[[nodiscard]] inline core::Result<ViewshedCellDepthSamples>
viewshed_cell_depth_samples(
    const ViewshedCellFootprint& footprint,
    std::size_t grid_width = kViewshedPerspectiveDepthGridAxis,
    std::size_t grid_height = kViewshedPerspectiveDepthGridAxis) {
    using Output = core::Result<ViewshedCellDepthSamples>;
    if (grid_width < 2U || grid_width > kViewshedPerspectiveDepthGridAxisMax ||
        grid_height < 2U || grid_height > kViewshedPerspectiveDepthGridAxisMax) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "viewshed depth sample grid exceeds its bounded axis"));
    }
    if (std::any_of(footprint.corners.begin(), footprint.corners.end(),
            [](core::Vec3d point) { return !point.finite(); })) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "viewshed depth sampling requires four finite cell corners"));
    }
    ViewshedCellDepthSamples samples;
    samples.points.resize(grid_width * grid_height);
    for (std::size_t row = 0U; row < grid_height; ++row) {
        const double v = static_cast<double>(row) /
            static_cast<double>(grid_height - 1U);
        for (std::size_t column = 0U; column < grid_width; ++column) {
            const double u = static_cast<double>(column) /
                static_cast<double>(grid_width - 1U);
            const auto& corners = footprint.corners;
            const core::Vec3d point =
                corners[0U] * ((1.0 - u) * (1.0 - v)) +
                corners[1U] * (u * (1.0 - v)) +
                corners[2U] * (u * v) +
                corners[3U] * ((1.0 - u) * v);
            if (!point.finite()) {
                return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "viewshed depth sample exceeds finite world coordinates"));
            }
            samples.points[row * grid_width + column] = point;
        }
    }
    return Output::success(std::move(samples));
}

[[nodiscard]] inline core::Result<ViewshedCellFootprint> viewshed_cell_footprint(
    const editor::LandmarkViewshedQuery& query,
    std::size_t row,
    std::size_t column) {
    using Output = core::Result<ViewshedCellFootprint>;
    const std::size_t width = query.width;
    const std::size_t height = query.height;
    if (width < 2U || height < 2U ||
        width > editor::TerrainLandmarkViewshedGrid::kMaxAxis ||
        height > editor::TerrainLandmarkViewshedGrid::kMaxAxis ||
        height > std::numeric_limits<std::size_t>::max() / width ||
        width * height != query.target_positions.size() ||
        row >= height || column >= width) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "viewshed cell footprint does not match a complete bounded sample grid"));
    }
    const std::size_t index = row * width + column;
    const core::Vec3d target = query.target_positions[index];
    if (!target.finite()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "viewshed cell target is not finite"));
    }

    const auto spacing = [&](bool horizontal) -> std::optional<double> {
        const std::size_t axis_count = horizontal ? width : height;
        const std::size_t coordinate_index = horizontal ? column : row;
        const auto coordinate = [&](std::size_t axis_index) {
            const std::size_t target_index = horizontal
                ? row * width + axis_index : axis_index * width + column;
            const auto& point = query.target_positions[target_index];
            return horizontal ? point.x : point.z;
        };
        const double current = coordinate(coordinate_index);
        if (!std::isfinite(current)) return std::nullopt;
        double result = 0.0;
        if (coordinate_index == 0U) {
            result = std::abs(coordinate(1U) - current);
        } else if (coordinate_index + 1U == axis_count) {
            result = std::abs(current - coordinate(axis_count - 2U));
        } else {
            result = std::min(
                std::abs(current - coordinate(coordinate_index - 1U)),
                std::abs(coordinate(coordinate_index + 1U) - current));
        }
        if (!std::isfinite(result) || result <= 0.0) return std::nullopt;
        return result;
    };
    const auto spacing_x = spacing(true);
    const auto spacing_z = spacing(false);
    if (!spacing_x || !spacing_z) {
        return Output::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "viewshed cell has no stable two-dimensional sample spacing"));
    }
    const double half_x = *spacing_x * 0.5;
    const double half_z = *spacing_z * 0.5;
    ViewshedCellFootprint footprint{
        std::array<core::Vec3d, 4U>{
            core::Vec3d{target.x - half_x, target.y, target.z - half_z},
            core::Vec3d{target.x + half_x, target.y, target.z - half_z},
            core::Vec3d{target.x + half_x, target.y, target.z + half_z},
            core::Vec3d{target.x - half_x, target.y, target.z + half_z}}};
    if (std::any_of(footprint.corners.begin(), footprint.corners.end(),
            [](core::Vec3d corner) { return !corner.finite(); })) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "viewshed cell footprint exceeds finite world coordinates"));
    }
    return Output::success(std::move(footprint));
}

[[nodiscard]] inline core::Result<ResolvedViewshedObserver> resolve_viewshed_observer(
    const application::ApplicationSnapshot& snapshot,
    const ViewshedObserverBinding& binding,
    core::Vec3d explicit_position) {
    if (!binding.object.has_value()) {
        if (binding.project_generation != 0U) {
            return core::Result<ResolvedViewshedObserver>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "explicit viewshed observer cannot retain an object project binding"));
        }
        if (!explicit_position.finite()) {
            return core::Result<ResolvedViewshedObserver>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "explicit viewshed observer coordinates must be finite"));
        }
        return core::Result<ResolvedViewshedObserver>::success(
            {explicit_position, std::nullopt});
    }
    if (!*binding.object || binding.project_generation == 0U ||
        binding.project_generation != snapshot.project_generation) {
        return core::Result<ResolvedViewshedObserver>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "viewshed observer binding belongs to a different project generation"));
    }
    const auto object = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
        [&binding](const auto& view) { return view.object.id == *binding.object; });
    if (object == snapshot.objects.end()) {
        return core::Result<ResolvedViewshedObserver>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "bound viewshed observer no longer exists in the current project"));
    }
    if (!object->world_transform.finite() || !object->world_transform.translation.finite()) {
        return core::Result<ResolvedViewshedObserver>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "bound viewshed observer has an invalid world transform"));
    }
    return core::Result<ResolvedViewshedObserver>::success(
        {object->world_transform.translation, *binding.object});
}

[[nodiscard]] inline bool viewshed_observer_matches(
    const core::Result<ResolvedViewshedObserver>& current,
    core::Vec3d captured_position,
    std::optional<scene::ObjectId> captured_object) {
    return current && current.value().object == captured_object &&
        current.value().position.x == captured_position.x &&
        current.value().position.y == captured_position.y &&
        current.value().position.z == captured_position.z;
}

} // namespace carto::studio
