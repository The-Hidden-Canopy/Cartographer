#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/render/gpu_abi.hpp>
#include <carto/render/material.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

inline constexpr std::uint32_t kMaterialArtifactVersion = 1U;
inline constexpr std::string_view kMaterialArtifactAlgorithm =
    "carto.material.native-pbr.v1";
inline constexpr std::uint32_t kMaterialFeatureStandardPbr = 1U << 0U;
inline constexpr std::uint32_t kMaterialFeatureDielectricF0 = 1U << 1U;
inline constexpr std::uint32_t kMaterialFeatureClearcoat = 1U << 2U;
inline constexpr std::uint32_t kKnownMaterialFeatureMask =
    kMaterialFeatureStandardPbr |
    kMaterialFeatureDielectricF0 |
    kMaterialFeatureClearcoat;
inline constexpr std::uint64_t kMaxMaterialArtifactBytes = 64U * 1024U;

// Immutable, revision-bound result of compiling a supported authored material
// into the current native PBR ABI. It intentionally contains only fields the
// shader can execute; unsupported authored binding features fail compilation.
struct CompiledMaterialArtifact {
    std::uint32_t artifact_version = kMaterialArtifactVersion;
    std::string algorithm{kMaterialArtifactAlgorithm};
    std::string source_identity;
    render::MaterialId material_id = 0U;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::uint32_t feature_mask = 0U;
    render::GpuMaterialConstants gpu_constants;
    assets::Sha256Digest artifact_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] constexpr std::uint64_t resident_bytes() const noexcept {
        return sizeof(render::GpuMaterialConstants);
    }
};

[[nodiscard]] core::Result<assets::Sha256Digest> material_source_digest(
    const render::MaterialAsset& material);

[[nodiscard]] core::Result<CompiledMaterialArtifact> compile_material_artifact(
    std::string source_identity,
    const render::MaterialAsset& material);

// Revalidates immediately before use. Revision and content must both match;
// callers cannot reuse an artifact after an authored edit or identity swap.
[[nodiscard]] core::Result<void> validate_material_artifact_source(
    const CompiledMaterialArtifact& artifact,
    std::string_view current_source_identity,
    const render::MaterialAsset& current_material);

[[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize_material_artifact(
    const CompiledMaterialArtifact& artifact);
[[nodiscard]] core::Result<CompiledMaterialArtifact> deserialize_material_artifact(
    std::span<const std::uint8_t> bytes);

} // namespace carto::production
