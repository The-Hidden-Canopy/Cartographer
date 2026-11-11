#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/editor/editor_camera.hpp>
#include <carto/editor/viewport_interaction.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace carto::studio {

struct SavedScenePreviewBinding {
    std::uint64_t project_generation = 0U;
    core::Revision project_revision;
    std::uint64_t environment_selection_generation = 0U;
    std::uint64_t render_settings_generation = 0U;
    std::string camera_state;
    core::Vec2d viewport_extent{};
};

struct SavedScenePreviewRenderSettings {
    core::Vec3d key_light_direction;
    core::Vec3d key_light_radiance;
    double exposure_ev = 0.0;
};

[[nodiscard]] inline bool saved_scene_preview_render_settings_match(
    const SavedScenePreviewRenderSettings& capture,
    const SavedScenePreviewRenderSettings& current) noexcept {
    return capture.key_light_direction.finite() &&
        capture.key_light_radiance.finite() && std::isfinite(capture.exposure_ev) &&
        current.key_light_direction.finite() && current.key_light_radiance.finite() &&
        std::isfinite(current.exposure_ev) &&
        capture.key_light_direction.x == current.key_light_direction.x &&
        capture.key_light_direction.y == current.key_light_direction.y &&
        capture.key_light_direction.z == current.key_light_direction.z &&
        capture.key_light_radiance.x == current.key_light_radiance.x &&
        capture.key_light_radiance.y == current.key_light_radiance.y &&
        capture.key_light_radiance.z == current.key_light_radiance.z &&
        capture.exposure_ev == current.exposure_ev;
}

[[nodiscard]] inline bool saved_scene_preview_binding_matches(
    const SavedScenePreviewBinding& capture,
    bool project_available,
    bool project_clean,
    bool supported_camera,
    bool live_project_source,
    std::uint64_t project_generation,
    core::Revision project_revision,
    std::string_view camera_state,
    core::Vec2d viewport_extent,
    std::uint64_t environment_selection_generation,
    std::uint64_t render_settings_generation) noexcept {
    return project_available && project_clean && supported_camera &&
        live_project_source && !capture.camera_state.empty() &&
        capture.project_generation == project_generation &&
        capture.project_revision == project_revision &&
        capture.environment_selection_generation == environment_selection_generation &&
        capture.render_settings_generation == render_settings_generation &&
        std::string_view(capture.camera_state) == camera_state &&
        capture.viewport_extent.finite() &&
        capture.viewport_extent.x > 0.0 && capture.viewport_extent.y > 0.0 &&
        viewport_extent.finite() && viewport_extent.x > 0.0 &&
        viewport_extent.y > 0.0 && capture.viewport_extent.x == viewport_extent.x &&
        capture.viewport_extent.y == viewport_extent.y;
}

inline constexpr std::size_t kMaxPerspectiveStrokeSegmentSamples = 64U;
inline constexpr std::size_t kMaxPerspectiveStrokeSegmentTriangleChecks = 1'000'000U;

enum class LoopCutPickDisposition : unsigned char {
    ignore,
    keep_armed,
    cancel,
    admit,
};

struct LoopCutPickDecision {
    LoopCutPickDisposition disposition = LoopCutPickDisposition::keep_armed;
    std::string_view message;
};

struct LoopCutPickContext {
    bool primary_press = false;
    bool workspace_surface = false;
    bool modeling_workspace = false;
    bool edge_selection = false;
    bool interactive_hit_cache = false;
    bool current_snapshot = false;
    bool speculative_preview = false;
    bool unmodified_primary_click = false;
    bool point_in_viewport = false;
};

[[nodiscard]] inline LoopCutPickDecision decide_loop_cut_pick_context(
    const LoopCutPickContext& context) noexcept {
    if (!context.primary_press) {
        return {LoopCutPickDisposition::ignore, {}};
    }
    if (!context.workspace_surface || !context.modeling_workspace ||
        !context.edge_selection) {
        return {LoopCutPickDisposition::cancel,
            "Loop cut pick cancelled because the modeling edge-selection context changed."};
    }
    if (!context.interactive_hit_cache) {
        return {LoopCutPickDisposition::keep_armed,
            "Loop cut pick needs an interactive viewport; turn off Pan view or preview mode."};
    }
    if (!context.current_snapshot) {
        return {LoopCutPickDisposition::cancel,
            "Loop cut pick cancelled because its viewport snapshot is stale."};
    }
    if (context.speculative_preview) {
        return {LoopCutPickDisposition::keep_armed,
            "Loop cut picking is unavailable in speculative agent preview."};
    }
    if (!context.unmodified_primary_click) {
        return {LoopCutPickDisposition::keep_armed,
            "Loop cut pick requires an unmodified primary click."};
    }
    if (!context.point_in_viewport) {
        return {LoopCutPickDisposition::keep_armed,
            "Loop cut pick missed the interactive viewport; choose a clear edge."};
    }
    return {LoopCutPickDisposition::admit, {}};
}

enum class LoopCutEdgePickStatus : unsigned char {
    miss,
    ambiguous,
    invalid,
    hit,
};

struct LoopCutPickTarget {
    LoopCutEdgePickStatus status = LoopCutEdgePickStatus::invalid;
    bool perspective = false;
    bool edge_source_found = false;
    bool frontmost = false;
    bool owner_eligible = false;
};

[[nodiscard]] inline LoopCutPickDecision decide_loop_cut_pick_target(
    const LoopCutPickTarget& target,
    std::string_view depth_diagnostic = {}) noexcept {
    if (target.status == LoopCutEdgePickStatus::miss) {
        return {LoopCutPickDisposition::keep_armed,
            "Loop cut pick found no edge; click a clear edge or press Esc to cancel."};
    }
    if (target.status == LoopCutEdgePickStatus::ambiguous) {
        return {LoopCutPickDisposition::keep_armed,
            "Loop cut pick is ambiguous; move closer to one edge before clicking."};
    }
    if (target.status != LoopCutEdgePickStatus::hit) {
        return {LoopCutPickDisposition::cancel,
            "Loop cut pick cancelled because viewport edge data is invalid."};
    }
    if (target.perspective && !target.edge_source_found) {
        return {LoopCutPickDisposition::cancel,
            "Loop cut pick cancelled because its edge source is missing."};
    }
    if (target.perspective && !target.frontmost) {
        return {LoopCutPickDisposition::keep_armed,
            depth_diagnostic.empty()
                ? "The selected edge is not on the frontmost visible surface."
                : depth_diagnostic};
    }
    if (!target.owner_eligible) {
        return {LoopCutPickDisposition::keep_armed,
            "Loop cut pick refused a missing, hidden, or locked mesh owner."};
    }
    return {LoopCutPickDisposition::admit, {}};
}

[[nodiscard]] inline bool use_legacy_screen_pick(
    bool workspace_surface,
    bool perspective_camera_active) noexcept {
    return !workspace_surface || !perspective_camera_active;
}

[[nodiscard]] inline bool viewport_camera_state_matches(
    bool current_perspective,
    const editor::EditorCamera& current_perspective_camera,
    const editor::WorkSurfaceCamera& current_plan_camera,
    bool expected_perspective,
    const editor::EditorCamera& expected_perspective_camera,
    const editor::WorkSurfaceCamera& expected_plan_camera) noexcept {
    if (current_perspective != expected_perspective) return false;
    if (expected_perspective) {
        const auto current_target = current_perspective_camera.target();
        const auto expected_target = expected_perspective_camera.target();
        return current_perspective_camera.projection() ==
                expected_perspective_camera.projection() &&
            current_target.x == expected_target.x &&
            current_target.y == expected_target.y &&
            current_target.z == expected_target.z &&
            current_perspective_camera.distance() ==
                expected_perspective_camera.distance() &&
            current_perspective_camera.yaw() == expected_perspective_camera.yaw() &&
            current_perspective_camera.pitch() == expected_perspective_camera.pitch() &&
            current_perspective_camera.orthographic_height() ==
                expected_perspective_camera.orthographic_height() &&
            current_perspective_camera.vertical_fov() ==
                expected_perspective_camera.vertical_fov();
    }
    const auto current_target = current_plan_camera.target();
    const auto expected_target = expected_plan_camera.target();
    return current_target.x == expected_target.x &&
        current_target.y == expected_target.y &&
        current_plan_camera.view_height() == expected_plan_camera.view_height();
}

[[nodiscard]] inline core::Result<std::size_t> perspective_stroke_segment_intervals(
    core::Vec2d start,
    core::Vec2d end,
    std::size_t triangle_count) {
    using Result = core::Result<std::size_t>;
    if (!start.finite() || !end.finite()) {
        return Result::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "Perspective stroke segment is invalid."));
    }
    const core::Vec2d delta = end - start;
    const double length_squared = delta.length_squared();
    if (!std::isfinite(length_squared) || length_squared < 0.0) {
        return Result::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "Perspective stroke segment is invalid."));
    }
    if (length_squared <= 1.0e-18) return Result::success(1U);

    const double required_steps_value = std::ceil(std::sqrt(length_squared));
    if (!std::isfinite(required_steps_value) || triangle_count == 0U) {
        return Result::failure(core::Diagnostic(core::ErrorCode::not_found,
            "Perspective stroke visibility data is unavailable."));
    }
    const std::size_t work_limited_steps = std::max<std::size_t>(
        1U, kMaxPerspectiveStrokeSegmentTriangleChecks / triangle_count);
    const std::size_t allowed_steps = std::min(
        kMaxPerspectiveStrokeSegmentSamples, work_limited_steps);
    if (required_steps_value > static_cast<double>(allowed_steps)) {
        return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "Perspective stroke cancelled because the pointer gap exceeds its visibility-sampling bound."));
    }
    return Result::success(std::max<std::size_t>(
        1U, static_cast<std::size_t>(required_steps_value)));
}

[[nodiscard]] inline core::Result<double> perspective_edge_camera_depth(
    double first_camera_depth,
    double second_camera_depth,
    double along) {
    using Result = core::Result<double>;
    if (!std::isfinite(first_camera_depth) || !std::isfinite(second_camera_depth) ||
        !std::isfinite(along) || along < 0.0 || along > 1.0 ||
        first_camera_depth >= 0.0 || second_camera_depth >= 0.0) {
        return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "Perspective edge depth is invalid."));
    }
    const double first_view_depth = -first_camera_depth;
    const double second_view_depth = -second_camera_depth;
    const double inverse_depth = (1.0 - along) / first_view_depth +
        along / second_view_depth;
    if (!std::isfinite(inverse_depth) || inverse_depth <= 0.0) {
        return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "Perspective edge depth is invalid."));
    }
    const double depth = -1.0 / inverse_depth;
    if (!std::isfinite(depth)) {
        return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "Perspective edge depth is invalid."));
    }
    return Result::success(depth);
}

[[nodiscard]] inline core::Result<bool> perspective_depth_is_frontmost(
    double overlay_camera_depth,
    double surface_camera_depth) {
    using Result = core::Result<bool>;
    if (!std::isfinite(overlay_camera_depth) ||
        !std::isfinite(surface_camera_depth) || overlay_camera_depth >= 0.0 ||
        surface_camera_depth >= 0.0) {
        return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "Perspective overlay depth is invalid."));
    }
    const double depth_tolerance = 1.0e-8 * std::max({
        1.0, std::abs(overlay_camera_depth), std::abs(surface_camera_depth)});
    return Result::success(
        overlay_camera_depth + depth_tolerance >= surface_camera_depth);
}

[[nodiscard]] inline core::Result<bool> perspective_edge_is_frontmost(
    double edge_camera_depth,
    double surface_camera_depth) {
    return perspective_depth_is_frontmost(edge_camera_depth, surface_camera_depth);
}

} // namespace carto::studio
