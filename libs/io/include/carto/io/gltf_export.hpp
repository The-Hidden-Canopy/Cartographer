#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::io {

// The export is deliberately glTF rather than a second authoring document
// dialect.  Cartographer remains the source of truth; this profile is a
// bounded, derived interchange artifact for consumers that implement the
// supported Cartographer glTF profile.
struct GltfExportReport {
    static constexpr std::uint32_t kProfileVersion = 1U;

    std::filesystem::path path;
    core::Revision source_revision;
    std::size_t objects = 0U;
    std::size_t meshes = 0U;
    std::size_t vertices = 0U;
    std::size_t triangles = 0U;
    std::size_t index_bytes = 0U;
    bool used_uint32_indices = false;
    std::vector<std::string> warnings;
};

// Emits a deterministic, single-file glTF 2.0 artifact. The artifact keeps
// Cartographer scene hierarchy and source revision in glTF extras, emits
// float32 POSITION/NORMAL attributes, active corner-domain UV0 payloads and
// authored corner normals (cartographer.normal0), shared derived tangent frames
// when present, and triangle-list indices, and uses the
// smallest supported unsigned index width for each mesh. Corner-domain UV seams
// and custom normal discontinuities use the same production render-mesh stream
// as the authoring viewport, without mutating the project.
// The CONDITION face-domain `condition.material_region` layer is preserved in
// mesh extras as stable face/region identity; final material realization remains
// consumer-owned and is reported explicitly. Authored tangent/color layers,
// multiple/inactive UV sets, and rig/deformation records are refused until this
// profile can preserve them. Material catalogs and slots are accepted only for
// 1..32 explicitly assigned, texture-free opaque
// base-color/metallic/roughness materials when each mesh uses one nonzero
// region. glTF primitives carry their material; lighting and back-face shading
// are not claimed identical. Textures, graphs, variants, non-default lighting
// controls and multiple regions per mesh continue to fail closed.
[[nodiscard]] core::Result<GltfExportReport> export_gltf(
    const project::ProjectDocument& document,
    const std::filesystem::path& path);

// Emits the same bounded Cartographer profile in a GLB 2.0 container. JSON
// and BIN chunk padding is canonical, and export remains a derived artifact.
[[nodiscard]] core::Result<GltfExportReport> export_glb(
    const project::ProjectDocument& document,
    const std::filesystem::path& path);

} // namespace carto::io
