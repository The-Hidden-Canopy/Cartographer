#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/render/render_scene.hpp>
#include <carto/scene/scene.hpp>
#include <vector>

namespace carto::editor {
// Computes bounds from the same compiled world-space instances displayed in
// the viewport. Selecting a group includes its rendered descendants. Empty,
// missing, stale or non-finite inputs fail without falling back to the scene.
[[nodiscard]] core::Result<core::Bounds3d> selected_render_bounds(
    const std::vector<scene::SceneObject>& objects,
    const std::vector<render::RenderInstance>& instances,
    const std::vector<scene::ObjectId>& selected_objects);
} // namespace carto::editor
