#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/production/artifact_store.hpp>
#include <carto/production/dependency.hpp>
#include <carto/surface/core.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::production {

inline constexpr std::uint32_t kBakeRecipeVersion = 1U;
inline constexpr std::uint32_t kBakeArtifactVersion = 2U;
inline constexpr std::size_t kMaxBakeRecipeBytes = 8U * 1024U;
inline constexpr std::size_t kMaxBakeAmbientOcclusionRecipeBytes = 32U * 1024U;
inline constexpr std::uint32_t kMaxBakeTextureExtent = 8192U;
inline constexpr std::uint64_t kMaxBakeTextureTexels = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxBakeSourceTriangles = 1'000'000U;
inline constexpr std::uint64_t kMaxBakeRasterCandidates = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaxBakePaddingTexels = 64U;
inline constexpr std::size_t kMaxBakePaddingFaces = 1'000'000U;
inline constexpr std::uint32_t kMaxBakeAmbientOcclusionSamples = 1'024U;
inline constexpr std::uint64_t kMaxBakeAmbientOcclusionRays =
    64ULL * 1024ULL * 1024ULL;
inline constexpr std::string_view kBakeNormalAlgorithm =
    "carto.bake.cpu.projected-tangent-normal.v2";
inline constexpr std::string_view kBakeObjectNormalAlgorithm =
    "carto.bake.cpu.projected-object-normal.v1";
inline constexpr std::string_view kBakeAmbientOcclusionAlgorithm =
    "carto.bake.cpu.ambient-occlusion.v1";
inline constexpr std::string_view kBakeBentNormalAlgorithm =
    "carto.bake.cpu.bent-normal.v2";
inline constexpr std::string_view kBakeCurvatureAlgorithm =
    "carto.bake.cpu.cotangent-signed-mean-curvature.v1";
inline constexpr std::string_view kBakeThicknessAlgorithm =
    "carto.bake.cpu.ray-aligned-first-exit-thickness.v1";
inline constexpr std::string_view kBakeHeightDisplacementAlgorithm =
    "carto.bake.cpu.projected-signed-normal-height.v1";
inline constexpr std::string_view kBakeWorldPositionAlgorithm =
    "carto.bake.cpu.uv-raster-world-position.v1";
inline constexpr std::uint32_t kWorldPositionArtifactVersion = 1U;
inline constexpr std::uint64_t kMaxWorldPositionTexels = 4ULL * 1024ULL * 1024ULL;
inline constexpr std::string_view kBakeMaterialSurfaceIdAlgorithm =
    "carto.bake.cpu.material-surface-domain-ids.v1";
inline constexpr std::string_view kBakePaddingAlgorithm =
    "carto.bake.cpu.uv-island-nearest-dilation.v1";
inline constexpr std::uint32_t kMaterialSurfaceIdArtifactVersion = 1U;
inline constexpr std::uint64_t kMaxMaterialSurfaceIdTexels =
    4ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxMaterialSurfaceDomains = 4'096U;
inline constexpr std::size_t kMaxMaterialSurfaceIdRecipeBytes = 32U * 1024U * 1024U;
inline constexpr std::uint32_t kMaterialSurfaceIdTexelBytes = 40U;
inline constexpr std::size_t kMaxMaterialSurfaceIdArtifactBytes =
    static_cast<std::size_t>(kMaxMaterialSurfaceIdTexels *
        kMaterialSurfaceIdTexelBytes + kMaxMaterialSurfaceIdRecipeBytes +
        128ULL * 1024ULL);

enum class BakeNormalConvention : std::uint8_t {
    opengl_y_plus,
    directx_y_minus,
};

enum class BakeBackfacePolicy : std::uint8_t {
    front_only,
    both_sides,
};

enum class BakeNormalSpace : std::uint8_t {
    tangent,
    object,
};

// A stable source binding contains no runtime handles or file paths. The digest
// covers mesh identities, geometry, and (for a target) the selected UV set.
struct BakeMeshBinding {
    std::string identity;
    core::Revision revision;
    assets::Sha256Digest content_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const BakeMeshBinding&) const noexcept = default;
};

struct BakeRecipe {
    std::string identity;
    BakeMeshBinding source;
    BakeMeshBinding target;
    attributes::UvSetId target_uv_set = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    // One explicit UDIM tile. Cross-tile triangles are rejected in this tranche.
    std::uint32_t tile_u = 0U;
    std::uint32_t tile_v = 0U;
    BakeNormalConvention normal_convention = BakeNormalConvention::opengl_y_plus;
    BakeBackfacePolicy backface_policy = BakeBackfacePolicy::both_sides;
    // Rays start this distance above the target and travel twice this distance
    // along the negative target normal, covering a symmetric cage interval.
    double projection_half_extent = 0.1;
    bool reject_projection_misses = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> settings_digest() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] bool operator==(const BakeRecipe&) const = default;
    [[nodiscard]] static core::Result<BakeRecipe> deserialize(std::string_view text);
};

// Content identity for reuse of one baked UDIM tile. target_revision records
// where the key was observed, while same_dependencies deliberately permits a
// new target revision when the selected tile inputs and all geometry are
// unchanged. Source revisions remain exact because source geometry can affect
// every target tile through projection.
struct BakeTileDependencyKey {
    std::string source_identity;
    core::Revision source_revision;
    std::string target_identity;
    core::Revision target_revision;
    attributes::UvSetId target_uv_set = 0U;
    std::uint32_t tile_u = 0U;
    std::uint32_t tile_v = 0U;
    assets::Sha256Digest source_geometry_digest;
    assets::Sha256Digest target_geometry_digest;
    assets::Sha256Digest target_tile_digest;
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest dependency_digest;

    [[nodiscard]] bool same_dependencies(
        const BakeTileDependencyKey& other) const noexcept {
        return source_identity == other.source_identity &&
            source_revision == other.source_revision &&
            target_identity == other.target_identity &&
            target_uv_set == other.target_uv_set &&
            tile_u == other.tile_u && tile_v == other.tile_v &&
            source_geometry_digest == other.source_geometry_digest &&
            target_geometry_digest == other.target_geometry_digest &&
            target_tile_digest == other.target_tile_digest &&
            settings_digest == other.settings_digest &&
            dependency_digest == other.dependency_digest;
    }
};

struct BakeDiagnostic {
    std::string code;
    std::string message;
    bool is_error = true;
    std::optional<geometry::FaceId> target_face;
    std::optional<geometry::FaceId> source_face;
    std::optional<std::uint32_t> texel_x;
    std::optional<std::uint32_t> texel_y;
};

struct BakeProjectionMissSample {
    geometry::FaceId target_face;
    std::uint32_t texel_x = 0U;
    std::uint32_t texel_y = 0U;

    [[nodiscard]] bool operator==(const BakeProjectionMissSample&) const noexcept = default;
};

struct NormalMapArtifact {
    BakeRecipe recipe;
    BakeNormalSpace normal_space = BakeNormalSpace::tangent;
    std::string algorithm{ kBakeNormalAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t projection_misses = 0U;
    std::vector<BakeProjectionMissSample> projection_miss_samples;
    bool projection_miss_samples_truncated = false;
    // Linear, uncompressed RGBA8; row zero represents the lower edge of UV space.
    // Uncovered texels are neutral tangent normals with alpha zero.
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<NormalMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

enum class BakePaddingInputKind : std::uint32_t {
    normal_map = 0U,
    ambient_occlusion = 1U,
    bent_normal = 2U,
    curvature = 3U,
    thickness = 4U,
    height_displacement = 5U,
};

// A padding recipe binds one verified RGBA8 bake artifact and a bounded
// 8-connected dilation radius. Padding copies RGB into uncovered texels while
// preserving alpha as the original coverage mask.
struct BakePaddingRecipe {
    BakeRecipe binding;
    BakeNormalSpace input_normal_space = BakeNormalSpace::tangent;
    assets::Sha256Digest input_artifact_digest;
    std::uint32_t padding_texels = 0U;
    // Appended to preserve existing aggregate initializers and v1 normal-map
    // recipe identity.
    BakePaddingInputKind input_kind = BakePaddingInputKind::normal_map;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] bool operator==(const BakePaddingRecipe&) const noexcept = default;
};

struct BakePaddingArtifact {
    BakePaddingRecipe recipe;
    std::string algorithm{kBakePaddingAlgorithm};
    std::string evidence_class{"carto.cpu-reference"};
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t padded_texels = 0U;
    std::uint64_t byte_size = 0U;
    // Linear RGBA8; RGB is dilated within each UV island's raster region and
    // alpha retains the input bake's coverage/miss semantics.
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<BakePaddingArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

// Serialized CPU reference cache entry for one normal-map UDIM tile. The
// dependency key permits reuse across target revisions only when all source
// geometry, target geometry, selected-tile UV inputs, and bake settings match.
struct BakeTileArtifactCacheEntry {
    BakeTileDependencyKey dependencies;
    NormalMapArtifact artifact;
    assets::Sha256Digest entry_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<BakeTileArtifactCacheEntry> deserialize(
        std::span<const std::uint8_t> bytes);
};

inline constexpr std::size_t kMaxBakeTileScheduleTiles = 100U;

struct BakeTileCoordinate {
    std::uint32_t tile_u = 0U;
    std::uint32_t tile_v = 0U;

    [[nodiscard]] bool operator==(const BakeTileCoordinate&) const noexcept = default;
};

struct BakeTileScheduleEntry {
    BakeTileCoordinate tile;
    NormalMapArtifact artifact;
    bool reused_from_cache = false;
};

struct BakeTileScheduleResult {
    std::vector<BakeTileScheduleEntry> entries;
    std::size_t compiled_tiles = 0U;
    std::size_t reused_tiles = 0U;
};

// Kept as a source-compatible spelling for tangent-space callers.
using TangentNormalMapArtifact = NormalMapArtifact;

// Deterministic cosine-hemisphere ray settings shared by scalar AO and bent-normal outputs.
struct AmbientOcclusionRecipe {
    BakeRecipe binding;
    std::uint32_t sample_count = 32U;
    double max_distance = 1.0;
    double origin_bias = 1e-4;
    std::uint64_t seed = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<AmbientOcclusionRecipe> deserialize(
        std::string_view text);
};

struct AmbientOcclusionMapArtifact {
    AmbientOcclusionRecipe recipe;
    std::string algorithm{ kBakeAmbientOcclusionAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::uint64_t rays_cast = 0U;
    std::uint64_t occluded_rays = 0U;
    // Worst-case standard error of the Bernoulli visibility estimate, not a
    // confidence interval or a measured cross-backend error.
    double sampling_standard_error_upper_bound = 0.0;
    // Linear RGBA8 grayscale. Uncovered texels are white with alpha zero.
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<AmbientOcclusionMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

struct BentNormalMapArtifact {
    AmbientOcclusionRecipe recipe;
    std::string algorithm{ kBakeBentNormalAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::uint64_t rays_cast = 0U;
    std::uint64_t occluded_rays = 0U;
    double sampling_standard_error_upper_bound = 0.0;
    // Linear tangent-space RGBA8. Uncovered texels are neutral with alpha zero.
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<BentNormalMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

struct CurvatureRecipe {
    BakeRecipe binding;
    // Signed cotangent mean curvature is clamped to this range in inverse meters.
    double normalization_scale = 10.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<CurvatureRecipe> deserialize(
        std::string_view text);
};

struct CurvatureMapArtifact {
    CurvatureRecipe recipe;
    std::string algorithm{ kBakeCurvatureAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t projection_misses = 0U;
    std::vector<BakeProjectionMissSample> projection_miss_samples;
    bool projection_miss_samples_truncated = false;
    std::uint64_t clamped_texels = 0U;
    double minimum_signed_curvature = 0.0;
    double maximum_signed_curvature = 0.0;
    double maximum_quantization_error = 0.0;
    // Linear RGBA8 signed scalar. 0 curvature encodes as 128; misses/uncovered are alpha zero.
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<CurvatureMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

struct ThicknessRecipe {
    BakeRecipe binding;
    // Maximum inward distance measured from the projected source surface, in meters.
    double maximum_distance = 0.25;
    // Starts the inward ray outside the projected surface and suppresses self-hits.
    double surface_bias = 1e-5;
    bool reject_thickness_misses = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ThicknessRecipe> deserialize(
        std::string_view text);
};

struct ThicknessMapArtifact {
    ThicknessRecipe recipe;
    std::string algorithm{ kBakeThicknessAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t thickness_hits = 0U;
    std::uint64_t projection_misses = 0U;
    std::uint64_t thickness_misses = 0U;
    std::vector<BakeProjectionMissSample> projection_miss_samples;
    bool projection_miss_samples_truncated = false;
    std::vector<BakeProjectionMissSample> thickness_miss_samples;
    bool thickness_miss_samples_truncated = false;
    double minimum_thickness = 0.0;
    double maximum_thickness = 0.0;
    // Worst-case RGBA8 rounding error only; this excludes geometric discretization error.
    double maximum_quantization_error = 0.0;
    // Linear RGBA8 meters normalized by recipe.maximum_distance; uncovered/missed are alpha zero.
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<ThicknessMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

struct HeightDisplacementRecipe {
    BakeRecipe binding;
    // Signed target-normal offset is encoded over [-normalization_distance,+distance].
    double normalization_distance = 0.1;
    bool reject_clamped_samples = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<HeightDisplacementRecipe> deserialize(
        std::string_view text);
};

struct HeightDisplacementMapArtifact {
    HeightDisplacementRecipe recipe;
    std::string algorithm{ kBakeHeightDisplacementAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t projection_misses = 0U;
    std::vector<BakeProjectionMissSample> projection_miss_samples;
    bool projection_miss_samples_truncated = false;
    std::uint64_t clamped_texels = 0U;
    double minimum_signed_displacement = 0.0;
    double maximum_signed_displacement = 0.0;
    // Worst-case RGBA8 rounding error only; this excludes source/target sampling error.
    double maximum_quantization_error = 0.0;
    // Linear RGBA8 signed scalar. Zero offset encodes as 128; misses/uncovered have alpha zero.
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<HeightDisplacementMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

using BakePaddingInputRef = std::variant<
    std::reference_wrapper<const NormalMapArtifact>,
    std::reference_wrapper<const AmbientOcclusionMapArtifact>,
    std::reference_wrapper<const BentNormalMapArtifact>,
    std::reference_wrapper<const CurvatureMapArtifact>,
    std::reference_wrapper<const ThicknessMapArtifact>,
    std::reference_wrapper<const HeightDisplacementMapArtifact>>;

// Instance-specific world-space positions for one stable scene node. The
// project snapshot digest binds hierarchy and transforms; the explicit world
// transform makes the coordinate frame inspectable in the recipe. Coordinates
// use the project convention: meters, right-handed, Y-up. The output stores
// one little-endian record per texel: face id (u64), then world x/y/z
// (IEEE-754 f64). A zero face id marks an uncovered texel.
struct WorldPositionRecipe {
    std::string identity;
    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::uint64_t target_scene_node = 0U;
    std::uint64_t target_mesh_asset = 0U;
    core::Revision target_mesh_revision;
    assets::Sha256Digest target_mesh_digest;
    attributes::UvSetId target_uv_set = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t tile_u = 0U;
    std::uint32_t tile_v = 0U;
    core::Transform target_world_transform = core::Transform::identity();

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<WorldPositionRecipe> deserialize(
        std::string_view text);
};

struct WorldPositionMapArtifact {
    WorldPositionRecipe recipe;
    std::string algorithm{ kBakeWorldPositionAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    // Canonical dense output: each texel is 32 bytes (face id + three f64s).
    std::vector<std::uint8_t> output_bytes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<WorldPositionMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

// A discrete ID map for the authored material assignment and explicitly
// identified surface domain covering each target UV texel. Surface membership
// remains caller-authored through SurfaceDomain records; this recipe does not
// infer semantic regions from geometry.
struct MaterialSurfaceIdRecipe {
    std::string identity;
    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::uint64_t target_mesh_asset = 0U;
    core::Revision target_mesh_revision;
    assets::Sha256Digest target_mesh_digest;
    attributes::UvSetId target_uv_set = 0U;
    std::string material_region_layer;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t tile_u = 0U;
    std::uint32_t tile_v = 0U;
    std::vector<surface::SurfaceDomain> surface_domains;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<MaterialSurfaceIdRecipe> deserialize(
        std::string_view text);
};

struct MaterialSurfaceIdMapArtifact {
    MaterialSurfaceIdRecipe recipe;
    std::string algorithm{ kBakeMaterialSurfaceIdAlgorithm };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    // Per texel: stable FaceId, material ID, optional variant ID (zero when
    // absent), SurfaceDomainId, region ID, and a zero reserved word. Integer
    // fields use canonical little-endian.
    std::vector<std::uint8_t> output_bytes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<MaterialSurfaceIdMapArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

struct AmbientOcclusionExecutionReport {
    std::optional<AmbientOcclusionMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;
    std::uint64_t rays_cast = 0U;
    std::uint64_t occluded_rays = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

struct BentNormalExecutionReport {
    std::optional<BentNormalMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;
    std::uint64_t rays_cast = 0U;
    std::uint64_t occluded_rays = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

struct CurvatureExecutionReport {
    std::optional<CurvatureMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t projection_misses = 0U;
    std::uint64_t clamped_texels = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

struct ThicknessExecutionReport {
    std::optional<ThicknessMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t thickness_hits = 0U;
    std::uint64_t projection_misses = 0U;
    std::uint64_t thickness_misses = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

struct HeightDisplacementExecutionReport {
    std::optional<HeightDisplacementMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t projection_misses = 0U;
    std::uint64_t clamped_texels = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

struct WorldPositionExecutionReport {
    std::optional<WorldPositionMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

struct MaterialSurfaceIdExecutionReport {
    std::optional<MaterialSurfaceIdMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

struct BakeExecutionReport {
    std::optional<NormalMapArtifact> artifact;
    std::vector<BakeDiagnostic> diagnostics;
    std::uint64_t target_texels_visited = 0U;
    std::uint64_t covered_texels = 0U;
    std::uint64_t projected_texels = 0U;
    std::uint64_t projection_misses = 0U;
    // Texels whose selected source hit was a backface under the two-sided policy.
    std::uint64_t projection_backface_hits = 0U;
    // Texels with no accepted hit because front-only policy rejected backfaces.
    std::uint64_t projection_backface_rejections = 0U;

    [[nodiscard]] bool succeeded() const noexcept {
        return artifact.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    }
};

[[nodiscard]] core::Result<BakeMeshBinding> bind_bake_source(
    std::string identity,
    const geometry::EditableMesh& mesh);
[[nodiscard]] core::Result<BakeMeshBinding> bind_bake_target(
    std::string identity,
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set);

[[nodiscard]] core::Result<WorldPositionRecipe> bind_world_position_recipe(
    std::string identity,
    const project::ProjectDocument& document,
    std::uint64_t target_scene_node,
    attributes::UvSetId target_uv_set,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t tile_u = 0U,
    std::uint32_t tile_v = 0U);
[[nodiscard]] core::Result<void> validate_world_position_source(
    const WorldPositionRecipe& recipe,
    const project::ProjectDocument& document);
[[nodiscard]] core::Result<MaterialSurfaceIdRecipe> bind_material_surface_id_recipe(
    std::string identity,
    const project::ProjectDocument& document,
    std::uint64_t target_mesh_asset,
    attributes::UvSetId target_uv_set,
    std::string material_region_layer,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<surface::SurfaceDomain> surface_domains,
    std::uint32_t tile_u = 0U,
    std::uint32_t tile_v = 0U);
[[nodiscard]] core::Result<void> validate_material_surface_id_source(
    const MaterialSurfaceIdRecipe& recipe,
    const project::ProjectDocument& document);
[[nodiscard]] core::Result<void> validate_material_surface_id_artifact_source(
    const MaterialSurfaceIdMapArtifact& artifact,
    const project::ProjectDocument& document);

[[nodiscard]] core::Result<assets::Sha256Digest> bake_mesh_source_digest(
    const geometry::EditableMesh& mesh);
[[nodiscard]] core::Result<assets::Sha256Digest> bake_mesh_target_digest(
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set);

// Fails closed on identity, revision, topology, UV, or content drift. Callers
// can re-run this immediately before putting the immutable artifact in a store.
[[nodiscard]] core::Result<void> validate_bake_sources(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);

// Hashes only the selected tile's target faces/UVs plus the mesh-wide target
// geometry separately in BakeTileDependencyKey. Cross-tile faces and empty
// tiles fail closed because the CPU reference baker cannot produce them.
[[nodiscard]] core::Result<assets::Sha256Digest> bake_mesh_target_tile_digest(
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set,
    std::uint32_t tile_u,
    std::uint32_t tile_v);
[[nodiscard]] core::Result<BakeTileDependencyKey> make_bake_tile_dependency_key(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    BakeNormalSpace normal_space = BakeNormalSpace::tangent);
[[nodiscard]] core::Result<BakeTileArtifactCacheEntry> make_bake_tile_cache_entry(
    const NormalMapArtifact& artifact,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
// Rebinds verified cached pixels to a current revision when only unrelated
// target UV tiles changed. A dependency mismatch returns stale_data.
[[nodiscard]] core::Result<NormalMapArtifact> reuse_bake_tile_artifact(
    const BakeTileArtifactCacheEntry& entry,
    const BakeRecipe& current_recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target);
// Discovers the UDIM tiles used by complete target faces. Faces crossing a
// tile boundary are unsupported because the reference baker produces one
// independent artifact per tile.
[[nodiscard]] core::Result<std::vector<BakeTileCoordinate>> enumerate_bake_tiles(
    const geometry::EditableMesh& target,
    attributes::UvSetId target_uv_set);
// Compiles all discovered tiles in stable (U,V) order. A failure in any tile
// returns no partial result; matching cache entries are validated before reuse.
[[nodiscard]] core::Result<BakeTileScheduleResult> schedule_normal_map_tiles_cpu(
    const BakeRecipe& base_recipe,
    BakeNormalSpace normal_space,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::span<const BakeTileArtifactCacheEntry> reusable_tiles = {});

// Uses the shared production cook graph for bake dependencies and invalidation.
// The recipe digest binds the complete source, target, UV, tile, projection,
// output-convention, and size contract into one deterministic cook key.
[[nodiscard]] core::Result<ProductionCookKey> bake_production_cook_key(
    const BakeRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> bake_production_cook_key(
    const BakeRecipe& recipe,
    BakeNormalSpace normal_space,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> bake_padding_cook_key(
    const BakePaddingRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_bake_padding_graph_node(
    ProductionCookGraph& graph,
    const BakePaddingRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_bake_graph_node(
    ProductionCookGraph& graph,
    const BakeRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> bake_ambient_occlusion_cook_key(
    const AmbientOcclusionRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_ambient_occlusion_graph_node(
    ProductionCookGraph& graph,
    const AmbientOcclusionRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> bake_bent_normal_cook_key(
    const AmbientOcclusionRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_bent_normal_graph_node(
    ProductionCookGraph& graph,
    const AmbientOcclusionRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> bake_curvature_cook_key(
    const CurvatureRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_curvature_graph_node(
    ProductionCookGraph& graph,
    const CurvatureRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> bake_thickness_cook_key(
    const ThicknessRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_thickness_graph_node(
    ProductionCookGraph& graph,
    const ThicknessRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> bake_height_displacement_cook_key(
    const HeightDisplacementRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_height_displacement_graph_node(
    ProductionCookGraph& graph,
    const HeightDisplacementRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> world_position_cook_key(
    const WorldPositionRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_world_position_graph_node(
    ProductionCookGraph& graph,
    const WorldPositionRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> material_surface_id_cook_key(
    const MaterialSurfaceIdRecipe& recipe,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_material_surface_id_graph_node(
    ProductionCookGraph& graph,
    const MaterialSurfaceIdRecipe& recipe,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ProductionCookKey> register_bake_graph_node(
    ProductionCookGraph& graph,
    const BakeRecipe& recipe,
    BakeNormalSpace normal_space,
    std::vector<ProductionCookKey> dependencies = {},
    std::string platform_profile = "carto.cpu-reference.v1");

// Publishes a normal-map artifact only after revalidating both authored meshes,
// the exact cook key, serialized artifact bytes, receipt, and admission policy.
[[nodiscard]] core::Result<void> publish_normal_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const NormalMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");

// Reads only the exact currently bound recipe artifact. Stale source or target
// content cannot fall back to a prior admitted output.
[[nodiscard]] core::Result<NormalMapArtifact> read_normal_map(
    const ProductionArtifactStore& store,
    const BakeRecipe& recipe,
    BakeNormalSpace normal_space,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<void> publish_ambient_occlusion_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const AmbientOcclusionMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<AmbientOcclusionMapArtifact> read_ambient_occlusion_map(
    const ProductionArtifactStore& store,
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<void> publish_bent_normal_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BentNormalMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<BentNormalMapArtifact> read_bent_normal_map(
    const ProductionArtifactStore& store,
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<void> publish_curvature_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const CurvatureMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<CurvatureMapArtifact> read_curvature_map(
    const ProductionArtifactStore& store,
    const CurvatureRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<void> publish_thickness_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const ThicknessMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<ThicknessMapArtifact> read_thickness_map(
    const ProductionArtifactStore& store,
    const ThicknessRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<void> publish_height_displacement_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const HeightDisplacementMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<HeightDisplacementMapArtifact> read_height_displacement_map(
    const ProductionArtifactStore& store,
    const HeightDisplacementRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");

// Deterministic CPU reference bake. It rasterizes the declared target UV tile,
// casts bounded rays into the source mesh, and returns no artifact if the recipe
// rejects misses, UV overlap, stale input, or malformed geometry.
[[nodiscard]] BakeExecutionReport compile_tangent_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
[[nodiscard]] BakeExecutionReport compile_tangent_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token);
[[nodiscard]] BakeExecutionReport compile_object_space_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
[[nodiscard]] BakeExecutionReport compile_object_space_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token);
[[nodiscard]] core::Result<BakePaddingRecipe> make_bake_padding_recipe(
    const NormalMapArtifact& input,
    std::uint32_t padding_texels);
[[nodiscard]] core::Result<BakePaddingRecipe> make_bake_padding_recipe(
    BakePaddingInputRef input,
    std::uint32_t padding_texels);
[[nodiscard]] core::Result<BakePaddingArtifact> compile_bake_padding_cpu(
    const BakePaddingRecipe& recipe,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
[[nodiscard]] core::Result<BakePaddingArtifact> compile_bake_padding_cpu(
    const BakePaddingRecipe& recipe,
    BakePaddingInputRef input,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
[[nodiscard]] core::Result<void> validate_bake_padding_sources(
    const BakePaddingArtifact& artifact,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target);
[[nodiscard]] core::Result<void> validate_bake_padding_sources(
    const BakePaddingArtifact& artifact,
    BakePaddingInputRef input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target);
[[nodiscard]] core::Result<void> publish_bake_padding(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BakePaddingArtifact& artifact,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<void> publish_bake_padding(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BakePaddingArtifact& artifact,
    BakePaddingInputRef input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<BakePaddingArtifact> read_bake_padding(
    const ProductionArtifactStore& store,
    const BakePaddingRecipe& recipe,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] core::Result<BakePaddingArtifact> read_bake_padding(
    const ProductionArtifactStore& store,
    const BakePaddingRecipe& recipe,
    BakePaddingInputRef input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile = "carto.cpu-reference.v1");
[[nodiscard]] AmbientOcclusionExecutionReport compile_ambient_occlusion_cpu(
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
[[nodiscard]] BentNormalExecutionReport compile_bent_normal_cpu(
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
[[nodiscard]] CurvatureExecutionReport compile_curvature_cpu(
    const CurvatureRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
// Projects target texels onto the source, then measures the first inward exit
// along the target shading normal. Missing projections/exits follow their
// separately bound fail-closed policies.
[[nodiscard]] ThicknessExecutionReport compile_thickness_cpu(
    const ThicknessRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
// Encodes signed source-to-target displacement along the target shading normal.
// Clamping is explicit in the recipe; rejected clamping publishes no artifact.
[[nodiscard]] HeightDisplacementExecutionReport compile_height_displacement_cpu(
    const HeightDisplacementRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);
// Rasterizes the bound scene instance's surface positions into its selected
// UV tile. It emits no product for stale project state, missing/overlapping
// UVs, cross-tile triangles, or malformed topology.
[[nodiscard]] WorldPositionExecutionReport compile_world_position_cpu(
    const WorldPositionRecipe& recipe,
    const project::ProjectDocument& document);
[[nodiscard]] MaterialSurfaceIdExecutionReport compile_material_surface_id_cpu(
    const MaterialSurfaceIdRecipe& recipe,
    const project::ProjectDocument& document);

} // namespace carto::production
