#pragma once
#include <carto/geometry/mesh.hpp>
#include <optional>
#include <string>
namespace carto::production {
// Source aliases accepted by existing material assignment/import authoring.
// Both together are ambiguous and fail; absent means the default material.
[[nodiscard]] core::Result<std::optional<std::string>>
authoring_material_slot_layer(const geometry::EditableMesh& mesh);
}
