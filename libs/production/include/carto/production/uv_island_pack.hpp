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

inline constexpr std::uint32_t kUvIslandPackVersion = 2U;
inline constexpr std::uint64_t kMaxUvIslandPackCorners = 1'000'000ULL;
inline constexpr std::uint32_t kMaxUvIslandPackTiles = 100U;
inline constexpr std::uint64_t kMaxUvIslandPackCandidates = 1'000'000ULL;

enum class UvIslandDensityPolicy : std::uint32_t {
    preserve_source_uv = 0U,
    uniform_world_area = 1U,
};

struct UvIslandPackRecipe {
    std::string project_namespace;
    std::uint64_t mesh_asset = 0U;
    BakeMeshBinding source;
    attributes::UvSetId source_uv_set = 0U;
    attributes::UvSetId output_uv_set = 0U;
    std::string output_name;
    std::string output_layer_id;
    std::uint32_t resolution = 0U;
    std::uint32_t padding_texels = 0U;
    std::uint32_t maximum_udim_tiles = 1U;
    UvIslandDensityPolicy density_policy = UvIslandDensityPolicy::preserve_source_uv;
    // Zero selects the highest feasible density. A positive value is an exact
    // target and fails if the declared UDIM budget cannot fit it.
    double target_texels_per_world_unit = 0.0;
    // Allows deterministic 90-degree chart rotations during shelf placement.
    bool allow_quarter_turn_rotation = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> settings_digest() const;
};

struct UvIslandPackCorner {
    geometry::CornerId corner;
    geometry::FaceId face;
    std::uint32_t udim_tile = 1001U;
    core::Vec2d uv;

    [[nodiscard]] bool operator==(const UvIslandPackCorner& other) const noexcept {
        return corner == other.corner && face == other.face &&
            udim_tile == other.udim_tile && uv.x == other.uv.x && uv.y == other.uv.y;
    }
};

struct UvIslandPackTileReceipt {
    std::uint32_t udim_tile = 1001U;
    std::uint64_t face_count = 0U;
    std::uint64_t corner_count = 0U;
    assets::Sha256Digest output_digest;

    [[nodiscard]] bool operator==(const UvIslandPackTileReceipt&) const noexcept = default;
};

struct UvIslandPackArtifact {
    UvIslandPackRecipe recipe;
    std::string algorithm{"carto.uv.island-shelf-pack.v2"};
    std::string evidence_class{"carto.cpu-reference"};
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t byte_size = 0U;
    // Uniform packing scale after the recipe's per-island density policy.
    // This is measured in source UV units for preserve_source_uv and in
    // area-normalized world units for uniform_world_area.
    double pixels_per_uv_unit = 0.0;
    // Zero when no world-area policy was requested; otherwise the resulting
    // area-equivalent texels per world unit after packing.
    double texels_per_world_unit = 0.0;
    std::vector<UvIslandPackCorner> corners;
    std::vector<UvIslandPackTileReceipt> tiles;

    [[nodiscard]] core::Result<void> validate(
        const geometry::EditableMesh& current_source) const;
    [[nodiscard]] core::Result<void> refresh_digests();
};

[[nodiscard]] core::Result<UvIslandPackRecipe> make_uv_island_pack_recipe(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    attributes::UvSetId source_uv_set,
    attributes::UvSetId output_uv_set,
    std::string output_name,
    std::string output_layer_id,
    std::uint32_t resolution,
    std::uint32_t padding_texels,
    std::uint32_t maximum_udim_tiles = 1U,
    UvIslandDensityPolicy density_policy =
        UvIslandDensityPolicy::preserve_source_uv,
    double target_texels_per_world_unit = 0.0,
    bool allow_quarter_turn_rotation = false);

// Packs connected islands from the selected corner UV set. Faces join one
// island only when both UV endpoints agree exactly across their shared edge.
// Corner identity and seam continuity are preserved; the selected density
// policy may uniformly rescale each island. Non-manifold fans fail closed.
[[nodiscard]] core::Result<UvIslandPackArtifact> compile_uv_island_pack(
    const UvIslandPackRecipe& recipe,
    const geometry::EditableMesh& source);

[[nodiscard]] core::Result<void> apply_uv_island_pack(
    project::ProjectTransaction& transaction,
    std::uint64_t mesh_asset,
    const UvIslandPackArtifact& artifact);

[[nodiscard]] std::string uv_island_pack_transaction_operation(
    const UvIslandPackArtifact& artifact);

} // namespace carto::production
