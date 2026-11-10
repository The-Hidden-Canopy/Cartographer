#pragma once
#include <carto/geometry/mesh.hpp>
#include <carto/io/gltf_material_import.hpp>
#include <carto/scene/scene.hpp>
#include <cstdint>
#include <optional>
#include <filesystem>
namespace carto::io {
struct ImportedGltfScene {
    std::vector<geometry::EditableMesh> meshes;
    // Source node/mesh indices become one-based temporary identities.
    std::vector<scene::SceneObject> nodes;
    ImportedGltfMaterials materials;
    // This catalog entry is a derived projection, not a source material index.
    std::optional<std::uint64_t> projected_default_material;
    std::vector<std::string> warnings;
};
struct GltfSceneImportOptions {
    // glTF's implicit material is single-sided. The authoring viewport is
    // two-sided, so callers must explicitly accept this visual projection.
    bool allow_default_material_projection = false;
};
// Snapshots the exact document and referenced external sibling buffers into a
// single bounded source artifact. External images remain unsupported.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> capture_gltf_scene_source(
    const std::filesystem::path& document_path, std::span<const std::uint8_t> bytes);
// Static triangle scenes, embedded glTF/GLB or an owned captured source.
// Parsing never consults the filesystem, network or executable adapters.
// Unsupported visual/animation features fail before an authoring document can
// be changed.
[[nodiscard]] core::Result<ImportedGltfScene>
import_gltf_scene(std::span<const std::uint8_t> bytes,
                  GltfSceneImportOptions options = {});
} // namespace carto::io
