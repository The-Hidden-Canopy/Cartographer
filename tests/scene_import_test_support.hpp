#pragma once
#include <carto/ui/ui.hpp>

namespace carto::testing {
inline core::Result<application::DispatchReceipt> import_gltf_scene_file(
    application::ApplicationSession& session, const std::filesystem::path& path, bool rights) {
    ui::UiController controller(session);
    const auto snapshot = controller.snapshot();
    return controller.import_static_gltf_scene(path, snapshot.project_generation,
        snapshot.project_revision, snapshot.source_namespace, rights, "Tested source import");
}
}
