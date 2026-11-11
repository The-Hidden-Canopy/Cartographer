#pragma once

#include <carto/editor/viewport_interaction.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace carto::studio {

struct ViewportTranslateHandle {
    editor::ViewportTranslateAxis axis;
    core::Vec2d start;
    core::Vec2d tip;
    core::Vec2d screen_axis_per_world;
};

// Samples are the projected origin plus one world unit along X, Y, and Z.
// A hidden or collapsed axis removes only its handle, never scene picking.
[[nodiscard]] inline std::vector<ViewportTranslateHandle> viewport_translate_handles(
    core::Vec2d origin, const std::array<std::optional<core::Vec2d>, 3U>& samples,
    double handle_length = 64.0) {
    std::vector<ViewportTranslateHandle> result;
    if (!origin.finite() || !std::isfinite(handle_length) || handle_length <= 8.0)
        return result;
    constexpr std::array axes{editor::ViewportTranslateAxis::world_x,
        editor::ViewportTranslateAxis::world_y, editor::ViewportTranslateAxis::world_z};
    for (std::size_t index = 0U; index < axes.size(); ++index) {
        if (!samples[index].has_value() || !samples[index]->finite()) continue;
        const auto delta = *samples[index] - origin;
        const double length = std::hypot(delta.x, delta.y);
        if (!std::isfinite(length) || length <= 1.0e-4) continue;
        const auto direction = delta * (1.0 / length);
        const auto tip = origin + direction * handle_length;
        const auto start = origin + direction * 8.0;
        if (tip.finite() && start.finite())
            result.push_back({axes[index], start, tip, delta});
    }
    return result;
}

// Stable X/Y/Z tie order. Rendering, pointer capture and pan exclusion all
// consume this same set of admitted handles.
[[nodiscard]] inline std::optional<std::size_t> hit_viewport_translate_handle(
    std::span<const ViewportTranslateHandle> handles, core::Vec2d pointer,
    double radius) {
    if (!pointer.finite() || !std::isfinite(radius) || radius < 0.0) return std::nullopt;
    std::optional<std::size_t> result;
    double best = radius * radius;
    for (std::size_t index = 0U; index < handles.size(); ++index) {
        const auto& handle = handles[index];
        const auto delta = handle.tip - handle.start;
        const double length_squared = delta.length_squared();
        if (!handle.start.finite() || !handle.tip.finite() ||
            !std::isfinite(length_squared) || length_squared <= 0.0) continue;
        const double along = std::clamp(
            core::dot(pointer - handle.start, delta) / length_squared, 0.0, 1.0);
        const double distance = (pointer - (handle.start + delta * along)).length_squared();
        if (std::isfinite(distance) && distance <= best && (!result || distance < best)) {
            result = index;
            best = distance;
        }
    }
    return result;
}

} // namespace carto::studio
