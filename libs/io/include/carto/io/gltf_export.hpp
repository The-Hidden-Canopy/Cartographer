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

// Emits a deterministic, single-file glTF 2.0 artifact.  The artifact keeps
// Cartographer scene hierarchy and source revision in glTF extras, emits
// float32 POSITION/NORMAL attributes, active corner-domain UV0 payloads and
// derived tangent frames when present, and triangle-list indices, and uses the
// smallest supported unsigned index width for each mesh. Corner-domain UV seams
// are expanded in the derived render stream without mutating the project.
// The CONDITION face-domain `condition.material_region` layer is preserved in
// mesh extras as stable face/region identity; final material realization remains
// consumer-owned and is reported explicitly.
[[nodiscard]] core::Result<GltfExportReport> export_gltf(
    const project::ProjectDocument& document,
    const std::filesystem::path& path);

} // namespace carto::io
