#pragma once
#include <carto/io/gltf_export.hpp>
#include <carto/project/project.hpp>
#include <carto/render/material.hpp>
namespace carto::io::detail {
struct GltfMaterialProjection {
    std::size_t index = 0;
    std::optional<render::UvSetId> uv;
    bool normal_mapping = false;
    bool baked = false;
};
class GltfMaterialWriter {
  public:
    GltfMaterialWriter(const project::ProjectDocument &, const GltfExportOptions &,
                       GltfExportReport &);
    ~GltfMaterialWriter();
    [[nodiscard]] core::Result<GltfMaterialProjection> material(std::uint64_t mesh,
                                                                std::optional<std::uint32_t> slot);
    // Preserve the complete sorted index mapping for the legacy constant profile.
    [[nodiscard]] core::Result<void> prepare_catalog();
    // Complete comma-terminated top-level JSON members, added to the same
    // atomic single-file GLTF/GLB document, never a sidecar authoring source.
    [[nodiscard]] core::Result<std::string> finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace carto::io::detail
