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
// documented VANTA MeshAsset glTF subset.
struct VantaExportReport {
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

// Emits a deterministic, single-file glTF 2.0 artifact.  The artifact keeps
// Cartographer scene hierarchy and source revision in glTF extras, emits
// float32 POSITION/NORMAL attributes and triangle-list indices, and uses the
// smallest supported unsigned index width for each mesh.  It does not mutate
// the project or claim to carry materials, UVs, tangents, or other channels
// that the current Cartographer authoring model does not own.
[[nodiscard]] core::Result<VantaExportReport> export_vanta_gltf(
    const project::ProjectDocument& document,
    const std::filesystem::path& path);

} // namespace carto::io
