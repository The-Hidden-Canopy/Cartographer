#pragma once
#include <carto/project/material_catalog.hpp>
#include <span>
#include <vector>
namespace carto::io {
struct ImportedGltfMaterials {
    project::MaterialCatalog catalog;
    // Index+1 is the temporary image asset ID in catalog. Caller remaps IDs
    // and admits these verified PNG/JPEGs to its own content-addressed source store.
    std::vector<std::vector<std::uint8_t>> encoded_images;
    assets::Sha256Digest artifact_digest;
};
// Materials-only reimport of Cartographer's bounded embedded GLTF/GLB profile.
// The scene adapter can disable the profile requirement for the same strictly
// supported glTF 2.0 material subset; this never relaxes appearance validation.
// Geometry/hierarchy are not imported. No external URI is read. Unknown visual
// extensions, texture-coordinate sets and unrepresentable samplers fail closed.
[[nodiscard]] core::Result<ImportedGltfMaterials>
import_gltf_materials(std::span<const std::uint8_t> bytes,
                      bool require_cartographer_profile = true);
} // namespace carto::io
