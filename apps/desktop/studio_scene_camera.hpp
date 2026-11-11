#pragma once

#include <carto/application/application.hpp>
#include <carto/editor/editor_camera.hpp>
#include <carto/production/authoring_viewport.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace carto::studio {

// Preserve the editor camera's right/up/forward convention when the native
// authoring renderer consumes its navigation state.
[[nodiscard]] inline core::Result<production::AuthoringView>
authoring_view_from_editor_camera(const editor::EditorCamera& camera, double aspect) {
    using Result = core::Result<production::AuthoringView>;
    if (!camera.valid() || !std::isfinite(aspect) || aspect <= 0.0 || aspect > 1.0e9)
        return Result::failure({core::ErrorCode::invalid_argument, "Viewport camera or aspect is invalid"});
    const double far_plane = std::max(1000.0, camera.distance() * 4.0);
    if (!std::isfinite(far_plane) || far_plane > 1.0e8 ||
        camera.vertical_fov() > 3.13 || camera.orthographic_height() > 1.0e8)
        return Result::failure({core::ErrorCode::unsupported, "Viewport camera exceeds renderer bounds"});
    const double sy = std::sin(camera.yaw() * 0.5), cy = std::cos(camera.yaw() * 0.5);
    const double sp = std::sin(camera.pitch() * 0.5), cp = std::cos(camera.pitch() * 0.5);
    production::AuthoringView view;
    view.position = camera.position();
    view.rotation = {cy * sp, -sy * cp, sy * sp, cy * cp};
    view.projection = camera.projection() == editor::EditorProjection::perspective
        ? project::CameraProjection::perspective : project::CameraProjection::orthographic;
    view.vertical_fov_radians = camera.vertical_fov();
    view.orthographic_height = camera.orthographic_height();
    view.near_plane = std::max(1.0e-6, camera.distance() * 1.0e-3);
    view.far_plane = far_plane;
    view.aspect_ratio = aspect;
    if (auto valid = view.validate(); !valid) return Result::failure(valid.error());
    return Result::success(std::move(view));
}

struct SavedScenePreviewCameraBinding {
    std::string state;
    std::string source_label;
    std::optional<scene::ObjectId> authored_object;
    std::optional<core::Revision> authored_revision;
};

// Bind a preview to either the current editor viewport or one explicitly
// selected saved camera. Project generation/revision are checked
// separately by the preview binding; this value captures the exact camera
// source so a changed selection or authored pose cannot reuse old pixels.
[[nodiscard]] inline core::Result<SavedScenePreviewCameraBinding>
saved_scene_preview_camera_binding(
    const application::ApplicationSnapshot& snapshot,
    std::optional<scene::ObjectId> authored_camera_object,
    std::uint64_t selection_project_generation,
    std::string_view viewport_camera_state) {
    using Result = core::Result<SavedScenePreviewCameraBinding>;
    if (!authored_camera_object.has_value()) {
        if (viewport_camera_state.empty()) {
            return Result::failure({core::ErrorCode::invalid_state,
                "preview requires a current 3D, Front, or Right editor camera"});
        }
        return Result::success({"viewport:" + std::string(viewport_camera_state),
            "Current viewport camera", std::nullopt, std::nullopt});
    }
    if (selection_project_generation != snapshot.project_generation) {
        return Result::failure({core::ErrorCode::stale_data,
            "selected authored camera belongs to a different project generation"});
    }

    const scene::ObjectId object_id = *authored_camera_object;
    const auto camera_iterator = snapshot.presentation_catalog.cameras().find(object_id);
    if (camera_iterator == snapshot.presentation_catalog.cameras().end()) {
        return Result::failure({core::ErrorCode::stale_data,
            "selected authored camera is absent from the current project"});
    }
    const auto& camera = camera_iterator->second;
    if (auto valid = camera.validate(); !valid) {
        return Result::failure(valid.error().with_context(
            "selected authored preview camera"));
    }
    if (camera.object != object_id) {
        return Result::failure({core::ErrorCode::unsupported,
            "selected authored camera object differs from its current catalog binding"});
    }
    if ((camera.projection == project::CameraProjection::perspective &&
         camera.vertical_fov_radians > 3.13) ||
        (camera.projection == project::CameraProjection::orthographic &&
         camera.orthographic_height > 1.0e8) || camera.near_plane < 0.0001 ||
        camera.far_plane > 1.0e8) {
        return Result::failure({core::ErrorCode::unsupported,
            "selected authored camera exceeds the direct renderer's projection or clip bounds"});
    }
    const auto object_iterator = std::find_if(snapshot.objects.begin(),
        snapshot.objects.end(), [object_id](const auto& object) {
            return object.object.id == object_id;
        });
    if (object_iterator == snapshot.objects.end() ||
        !object_iterator->world_transform.finite()) {
        return Result::failure({core::ErrorCode::stale_data,
            "selected authored camera has no current finite scene-object transform"});
    }

    const auto& transform = object_iterator->world_transform;
    std::ostringstream state;
    state.imbue(std::locale::classic());
    state << "authored:" << object_id.value << ':' << std::quoted(camera.identity)
          << ':' << camera.revision.value() << ':' << std::setprecision(17)
          << (camera.projection == project::CameraProjection::orthographic
              ? "orthographic" : "perspective") << ':'
          << camera.vertical_fov_radians << ':' << camera.orthographic_height
          << ':' << camera.near_plane << ':'
          << camera.far_plane << ':' << transform.translation.x << ':'
          << transform.translation.y << ':' << transform.translation.z << ':'
          << transform.rotation.x << ':' << transform.rotation.y << ':'
          << transform.rotation.z << ':' << transform.rotation.w << ':'
          << transform.scale.x << ':' << transform.scale.y << ':'
          << transform.scale.z;
    if (!state) {
        return Result::failure({core::ErrorCode::invalid_state,
            "selected authored camera state could not be represented"});
    }
    return Result::success({state.str(), camera.identity, object_id, camera.revision});
}

} // namespace carto::studio
