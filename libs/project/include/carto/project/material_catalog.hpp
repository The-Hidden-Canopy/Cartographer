#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/assets/texture_source.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/render/material.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {

using MaterialVariantId = std::uint64_t;

// Durable source-image identity. The locator is authoring input only; prepared
// products bind immutable bytes by digest and never depend on a workstation
// path at runtime.
struct TextureSourceRecord {
    assets::TextureAssetId id = 0U;
    std::string name;
    std::string relative_locator;
    assets::Sha256Digest content_digest;
    assets::TextureSourceFormat format = assets::TextureSourceFormat::png;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::string provenance;
    core::Revision revision{};

    [[nodiscard]] core::Result<void> validate() const;
};

enum class SamplerAddressMode : std::uint8_t {
    repeat,
    mirrored_repeat,
    clamp_to_edge,
};

enum class SamplerFilter : std::uint8_t {
    nearest,
    linear,
};

// Source sampler intent remains engine independent. Destination adapters map
// this record into their own native sampler representations.
struct SamplerIntent {
    render::SamplerId id = 0U;
    std::string name;
    SamplerAddressMode address_u = SamplerAddressMode::repeat;
    SamplerAddressMode address_v = SamplerAddressMode::repeat;
    SamplerAddressMode address_w = SamplerAddressMode::repeat;
    SamplerFilter minification = SamplerFilter::linear;
    SamplerFilter magnification = SamplerFilter::linear;
    SamplerFilter mip = SamplerFilter::linear;
    core::Revision revision{};

    [[nodiscard]] core::Result<void> validate() const;
};

// Optional fields distinguish "not overridden" from an explicit reset to the
// default value. An optional TextureRef may contain an unbound TextureRef to
// remove a base material's texture binding deliberately.
struct MaterialVariantOverride {
    std::optional<core::Vec4d> base_color_factor;
    std::optional<double> metallic;
    std::optional<double> roughness;
    std::optional<double> normal_scale;
    std::optional<double> occlusion_strength;
    std::optional<core::Vec3d> emissive_factor;
    std::optional<double> emissive_strength;
    std::optional<double> dielectric_f0;
    std::optional<double> clearcoat;
    std::optional<double> clearcoat_roughness;
    std::optional<render::AlphaMode> alpha_mode;
    std::optional<double> alpha_cutoff;
    std::optional<render::TextureRef> base_color;
    std::optional<render::TextureRef> metallic_roughness;
    std::optional<render::TextureRef> normal;
    std::optional<render::TextureRef> occlusion;
    std::optional<render::TextureRef> emissive;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
};

struct MaterialVariant {
    MaterialVariantId id = 0U;
    std::string name;
    render::MaterialId base_material = 0U;
    MaterialVariantOverride overrides;
    // Empty means the variant is not restricted to specific mesh assets.
    // Otherwise this list is canonical, sorted, unique, and non-zero.
    std::vector<std::uint64_t> permitted_mesh_assets;
    core::Revision revision{};

    [[nodiscard]] core::Result<void> validate() const;
};

struct MaterialRegionKey {
    std::uint64_t mesh_asset = 0U;
    std::uint32_t region_id = 0U;

    [[nodiscard]] constexpr auto operator<=>(const MaterialRegionKey&) const noexcept = default;
};

struct MaterialRegionAssignment {
    MaterialRegionKey key;
    render::MaterialId material = 0U;
    std::optional<MaterialVariantId> variant;

    [[nodiscard]] core::Result<void> validate() const;
};

// Provider-neutral authoring truth owned by ProjectDocument. Compiled
// materials, generated mips, native engine materials, and cache entries are
// derived products and cannot replace this catalog.
class MaterialCatalog final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecordsPerKind = 65'536U;
    static constexpr std::size_t kMaxVariantMeshScope = 65'536U;

    [[nodiscard]] core::Result<void> insert_texture(TextureSourceRecord texture);
    [[nodiscard]] core::Result<void> insert_sampler(SamplerIntent sampler);
    [[nodiscard]] core::Result<void> insert_material(render::MaterialAsset material);
    [[nodiscard]] core::Result<void> insert_variant(MaterialVariant variant);
    [[nodiscard]] core::Result<void> insert_assignment(MaterialRegionAssignment assignment);

    [[nodiscard]] const std::map<assets::TextureAssetId, TextureSourceRecord>&
    textures() const noexcept {
        return textures_;
    }
    [[nodiscard]] const std::map<render::SamplerId, SamplerIntent>&
    samplers() const noexcept {
        return samplers_;
    }
    [[nodiscard]] const std::map<render::MaterialId, render::MaterialAsset>&
    materials() const noexcept {
        return materials_;
    }
    [[nodiscard]] const std::map<MaterialVariantId, MaterialVariant>&
    variants() const noexcept {
        return variants_;
    }
    [[nodiscard]] const std::map<MaterialRegionKey, MaterialRegionAssignment>&
    assignments() const noexcept {
        return assignments_;
    }

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<render::StandardMaterial> resolve_material(
        render::MaterialId material,
        std::optional<MaterialVariantId> variant = std::nullopt) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<MaterialCatalog> deserialize(std::string_view text);

private:
    std::map<assets::TextureAssetId, TextureSourceRecord> textures_;
    std::map<render::SamplerId, SamplerIntent> samplers_;
    std::map<render::MaterialId, render::MaterialAsset> materials_;
    std::map<MaterialVariantId, MaterialVariant> variants_;
    std::map<MaterialRegionKey, MaterialRegionAssignment> assignments_;
};

} // namespace carto::project
