#pragma once

#include <carto/geometry/mesh.hpp>
#include <carto/production/bake_graph.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::project {
class ProjectDocument;
class ProjectTransaction;
}

namespace carto::production {

inline constexpr std::uint32_t kUniqueLightmapUvVersion = 2U;
inline constexpr std::uint64_t kMaxUniqueLightmapUvCorners = 1'000'000ULL;
inline constexpr std::uint32_t kMaxUniqueLightmapUdimTiles = 100U;
inline constexpr std::uint64_t kMaxUniqueLightmapPackCandidates = 1'000'000ULL;

struct UniqueLightmapUvRecipe {
    std::string project_namespace;
    std::uint64_t mesh_asset = 0U;
    BakeMeshBinding source;
    attributes::UvSetId output_uv_set = 0U;
    std::string output_name;
    std::string output_layer_id;
    std::uint32_t resolution = 0U;
    std::uint32_t padding_texels = 0U;
    std::uint32_t maximum_udim_tiles = 1U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> settings_digest() const;
};

struct LightmapUvCorner {
    geometry::CornerId corner;
    geometry::FaceId face;
    std::uint32_t udim_tile = 1001U;
    core::Vec2d uv;

    [[nodiscard]] bool operator==(const LightmapUvCorner& other) const noexcept {
        return corner == other.corner && face == other.face &&
            udim_tile == other.udim_tile && uv.x == other.uv.x && uv.y == other.uv.y;
    }
};

struct LightmapUvTileReceipt {
    std::uint32_t udim_tile = 1001U;
    std::uint64_t face_count = 0U;
    std::uint64_t corner_count = 0U;
    assets::Sha256Digest output_digest;

    [[nodiscard]] bool operator==(const LightmapUvTileReceipt&) const noexcept = default;
};

struct UniqueLightmapUvArtifact {
    UniqueLightmapUvRecipe recipe;
    std::string algorithm{"carto.lightmap.face-planar-udim-pack.v2"};
    std::string evidence_class{"carto.cpu-reference"};
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t byte_size = 0U;
    double texels_per_unit = 0.0;
    std::vector<LightmapUvCorner> corners;
    std::vector<LightmapUvTileReceipt> tiles;

    [[nodiscard]] core::Result<void> validate(
        const geometry::EditableMesh& current_source) const;
    [[nodiscard]] core::Result<void> refresh_digests();
};

[[nodiscard]] core::Result<UniqueLightmapUvRecipe> make_unique_lightmap_uv_recipe(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    attributes::UvSetId output_uv_set,
    std::string output_name,
    std::string output_layer_id,
    std::uint32_t resolution,
    std::uint32_t padding_texels,
    std::uint32_t maximum_udim_tiles = 1U);

// Reference generator uses one planar chart per convex face and deterministically
// packs across bounded UDIM tiles. It never changes the source mesh; unsupported
// face geometry and tile budgets that cannot fit fail.
[[nodiscard]] core::Result<UniqueLightmapUvArtifact> compile_unique_lightmap_uv(
    const UniqueLightmapUvRecipe& recipe,
    const geometry::EditableMesh& source);

// Builds a validated mesh containing the artifact's new corner-domain UV set.
// The source mesh remains unchanged; callers own transactional publication.
[[nodiscard]] core::Result<geometry::EditableMesh> with_unique_lightmap_uv(
    const geometry::EditableMesh& source,
    const UniqueLightmapUvArtifact& artifact);

// Applies a verified artifact only to the transaction's staged mesh. The caller
// owns commit/rollback and should use lightmap_uv_transaction_operation() when
// committing so the journal event records the exact settings and output digests.
[[nodiscard]] core::Result<void> apply_unique_lightmap_uv(
    project::ProjectTransaction& transaction,
    std::uint64_t mesh_asset,
    const UniqueLightmapUvArtifact& artifact);

[[nodiscard]] std::string lightmap_uv_transaction_operation(
    const UniqueLightmapUvArtifact& artifact);

} // namespace carto::production
