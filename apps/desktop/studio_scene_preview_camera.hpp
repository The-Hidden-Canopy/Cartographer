#pragma once

#include "studio_scene_camera.hpp"

#include <carto/d3d12/scene_renderer.hpp>

namespace carto::studio {

// The offline scene renderer consumes the same bounded editor projection as
// the live authoring renderer. This does not turn the separate X/Z plan camera
// into an EditorCamera or change an explicitly selected authored camera.
[[nodiscard]] inline core::Result<d3d12::SceneRenderCamera>
scene_preview_camera_from_editor_camera(const editor::EditorCamera& camera,
                                       double exposure_ev) {
    using Result = core::Result<d3d12::SceneRenderCamera>;
    const auto view = authoring_view_from_editor_camera(camera, 1.0);
    if (!view) return Result::failure(view.error());
    if (camera.projection() == editor::EditorProjection::perspective &&
        camera.vertical_fov() < 0.01)
        return Result::failure({core::ErrorCode::unsupported,
            "Scene preview perspective field of view is below the renderer's minimum"});
    if (!std::isfinite(exposure_ev) || std::abs(exposure_ev) > 20.0)
        return Result::failure({core::ErrorCode::invalid_argument,
            "Scene preview exposure exceeds the bounded renderer range"});

    d3d12::SceneRenderCamera result;
    result.position = view.value().position;
    result.target = camera.target();
    result.up = camera.axes().up;
    result.projection = view.value().projection == project::CameraProjection::perspective
        ? d3d12::SceneRenderProjection::perspective
        : d3d12::SceneRenderProjection::orthographic;
    result.vertical_fov = view.value().vertical_fov_radians;
    result.orthographic_height = view.value().orthographic_height;
    // Preserve the saved-preview clip policy, including the direct renderer's
    // minimum near distance, without altering the navigation camera itself.
    result.near_plane = std::max(0.001, std::min(0.1, camera.distance() * 0.001));
    result.far_plane = view.value().far_plane;
    result.exposure_ev = exposure_ev;
    return Result::success(std::move(result));
}

// Authored-camera selection has its own generation and source checks and must
// not depend on whichever navigation camera happens to be behind the preview.
[[nodiscard]] inline core::Result<SavedScenePreviewCameraBinding>
scene_preview_camera_source_binding(const application::ApplicationSnapshot& snapshot,
    std::optional<scene::ObjectId> authored_camera, std::uint64_t selection_generation,
    bool editor_camera_active, const editor::EditorCamera& editor_camera, double exposure_ev) {
    std::string viewport_state;
    if (!authored_camera.has_value() && editor_camera_active) {
        const auto converted = scene_preview_camera_from_editor_camera(editor_camera, exposure_ev);
        if (!converted)
            return core::Result<SavedScenePreviewCameraBinding>::failure(converted.error());
        viewport_state = editor_camera.serialize();
    }
    return saved_scene_preview_camera_binding(snapshot, authored_camera,
        selection_generation, viewport_state);
}

} // namespace carto::studio
