#pragma once

#include <carto/application/application.hpp>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace carto::studio {

struct SavedScenePreviewCameraBinding {
    std::string state;
    std::string source_label;
    std::optional<scene::ObjectId> authored_object;
    std::optional<core::Revision> authored_revision;
};

// Bind a preview to either the current perspective viewport or one explicitly
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
                "preview requires a current perspective viewport camera"});
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
