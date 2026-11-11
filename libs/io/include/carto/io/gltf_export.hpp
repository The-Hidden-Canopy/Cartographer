#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::production {
class AuthoringTextureLibrary;
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
    std::size_t materials = 0, textures = 0, images = 0;
    std::uint64_t embedded_image_bytes = 0;
    std::vector<std::string> warnings;
};

// Disposable source pixels or a saved-project location for the existing
// digest-addressed asset store. No arbitrary source locator is dereferenced.
struct GltfExportOptions {
    std::filesystem::path project_path;
    std::shared_ptr<const production::AuthoringTextureLibrary> textures;
    // Opt-in approximation only; zero preserves strict analytic projection.
    // Powers of two 4..256, one [0,1] UV tile, <=1M total sampled texels.
    std::uint32_t graph_bake_resolution = 0;
};

// Emits a deterministic, single-file glTF 2.0 artifact. The artifact keeps
// Cartographer scene hierarchy and source revision in glTF extras, emits
// float32 POSITION/NORMAL attributes, active corner-domain UV0 payloads and
// authored corner normals (cartographer.normal0), shared derived tangent frames
// when present, and triangle-list indices, and uses the
// smallest supported unsigned index width for each mesh. Corner-domain UV seams
// and custom normal discontinuities use the same production render-mesh stream
// as the authoring viewport, without mutating the project.
// The CONDITION face-domain `condition.material_region` table and source
// face identity are retained in extras. Visual assignments follow the
// viewport's material-region alias and resolved catalog variants. Unsupported
// appearance features fail before replacing a prior good artifact. Consumer
// lighting and back-face shading may differ from the authoring viewport.
// Unrecognized authored attribute layers also fail before publication. Declared
// UV streams retain the existing per-material selection and omission warnings;
// imported originals and reimport links remain in the source project and are
// explicitly reported as absent from this derived artifact.
[[nodiscard]] core::Result<GltfExportReport> export_gltf(
    const project::ProjectDocument& document,
    const std::filesystem::path& path);

// Emits the same bounded Cartographer profile in a GLB 2.0 container. JSON
// and BIN chunk padding is canonical, and export remains a derived artifact.
[[nodiscard]] core::Result<GltfExportReport> export_glb(const project::ProjectDocument &document,
                                                        const std::filesystem::path &path);

// Constant graphs project directly. Texture/UV-dependent graphs fail unless
// an explicit bounded bake is requested; the receipt labels its approximation.
[[nodiscard]] core::Result<GltfExportReport> export_gltf(const project::ProjectDocument &,
                                                         const std::filesystem::path &,
                                                         const GltfExportOptions &);
[[nodiscard]] core::Result<GltfExportReport> export_glb(const project::ProjectDocument &,
                                                        const std::filesystem::path &,
                                                        const GltfExportOptions &);

} // namespace carto::io
