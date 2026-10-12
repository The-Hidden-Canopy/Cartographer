#pragma once

#include <carto/production/material_artifact.hpp>
#include <carto/production/render_mesh.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

inline constexpr std::uint32_t kLookdevDrawPackageVersion = 1U;
inline constexpr std::string_view kLookdevDrawPackageAlgorithm =
    "carto.lookdev-draw-package.v1";
inline constexpr std::uint64_t kMaxLookdevDrawPackageBytes = 32U * 1024U * 1024U;

// Explicit input mapping from an authored render-mesh material slot to one
// immutable native material product. An absent slot is valid only for a mesh
// with one un-slotted submesh.
struct LookdevMaterialBinding {
    std::optional<std::uint32_t> material_slot;
    CompiledMaterialArtifact material;
};

// Native-ready draw metadata. It carries no GPU handles or runtime policy;
// texture asset IDs remain in gpu_constants for backend descriptor resolution.
struct LookdevSubmeshDraw {
    std::optional<std::uint32_t> material_slot;
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;
    std::string material_source_identity;
    render::MaterialId material_id = 0U;
    core::Revision material_source_revision;
    assets::Sha256Digest material_source_digest;
    std::uint32_t feature_mask = 0U;
    render::GpuMaterialConstants gpu_constants;
    assets::Sha256Digest material_artifact_digest;
};

// Immutable binding between one exact render-mesh product and the exact
// compiled material product used by every submesh. The package is suitable for
// persistence and native draw preparation, but does not claim descriptor or
// presented-frame execution by itself.
struct LookdevDrawPackage {
    std::uint32_t package_version = kLookdevDrawPackageVersion;
    std::string algorithm{kLookdevDrawPackageAlgorithm};
    core::Revision mesh_source_revision;
    core::Revision mesh_revision;
    assets::Sha256Digest mesh_source_digest;
    assets::Sha256Digest mesh_artifact_digest;
    std::uint64_t mesh_resident_bytes = 0U;
    std::vector<LookdevSubmeshDraw> draws;
    assets::Sha256Digest package_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept;
};

[[nodiscard]] core::Result<LookdevDrawPackage> compile_lookdev_draw_package(
    const RenderMeshArtifact& mesh,
    std::span<const LookdevMaterialBinding> bindings);

// Recompiles the package identity from current derived sources immediately
// before use. Any mesh content/revision change, material replacement, missing
// slot, or extra slot is rejected; no previous material silently fills a gap.
[[nodiscard]] core::Result<void> validate_lookdev_draw_package_sources(
    const LookdevDrawPackage& package,
    const RenderMeshArtifact& current_mesh,
    std::span<const LookdevMaterialBinding> current_bindings);

[[nodiscard]] core::Result<std::vector<std::uint8_t>>
serialize_lookdev_draw_package(const LookdevDrawPackage& package);
[[nodiscard]] core::Result<LookdevDrawPackage> deserialize_lookdev_draw_package(
    std::span<const std::uint8_t> bytes);

} // namespace carto::production
