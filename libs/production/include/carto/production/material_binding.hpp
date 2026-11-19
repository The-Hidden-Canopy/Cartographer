#pragma once
#include <carto/geometry/mesh.hpp>
#include <optional>
#include <string>
namespace carto::production {
// Source aliases accepted by existing material assignment/import authoring.
// Both together are ambiguous and fail; absent means the default material.
[[nodiscard]] core::Result<std::optional<std::string>>
authoring_material_slot_layer(const geometry::EditableMesh& mesh);
// Match interchange: a single linear RGBA vertex- or corner-domain layer.
// Ambiguous or unsupported color layers are rejected rather than omitted.
[[nodiscard]] core::Result<std::optional<std::string>>
authoring_vertex_color_layer(const geometry::EditableMesh& mesh);
}
