#pragma once
#include <carto/geometry/mesh.hpp>
#include <carto/io/gltf_material_import.hpp>
#include <carto/scene/scene.hpp>
namespace carto::io {
struct ImportedGltfScene {
    std::vector<geometry::EditableMesh> meshes;
    // Source node/mesh indices become one-based temporary identities.
    std::vector<scene::SceneObject> nodes;
    ImportedGltfMaterials materials;
    std::vector<std::string> warnings;
};
// Static triangle scenes, glTF 2.0 embedded buffers or GLB BIN. No filesystem,
// network, executable adapters or external references are consulted.
// Unsupported visual/animation features fail before an authoring document can
// be changed.
[[nodiscard]] core::Result<ImportedGltfScene>
import_gltf_scene(std::span<const std::uint8_t> bytes);
} // namespace carto::io
