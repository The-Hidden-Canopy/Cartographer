#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/dependency.hpp>
#include <carto/production/lookdev_draw_package.hpp>
#include <carto/production/preparation_scope.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/material_catalog.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::production {
struct WorldPositionMapArtifact;
struct MaterialSurfaceIdMapArtifact;
}

namespace carto::navgraph {
struct PreparedNavigationEnvelope;
}

namespace carto::production {

inline constexpr std::uint32_t kPreparedSceneVersion = 1U;
inline constexpr std::size_t kMaxPreparedSceneBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxPreparedSceneRecords = 65'536U;
inline constexpr std::size_t kMaxPreparedExtensionBindings = 4'096U;

enum class PreparedHandedness : std::uint8_t {
    right_handed,
    left_handed,
};

enum class PreparedAxis : std::uint8_t {
    positive_x,
    positive_y,
    positive_z,
    negative_x,
    negative_y,
    negative_z,
};

struct PreparedCoordinateConvention {
    double meters_per_unit = 1.0;
    PreparedHandedness handedness = PreparedHandedness::right_handed;
    PreparedAxis up = PreparedAxis::positive_y;
    PreparedAxis forward = PreparedAxis::positive_z;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class PreparedProductKind : std::uint8_t {
    render_mesh,
    lookdev_draw_package,
    world_model,
    material_catalog,
    texture_source,
    semantic_sidecar,
    connection_port_graph,
    physical_twin,
    collision_candidates,
    navigation,
    kinematic_graph,
    temporal_clip,
    archetype,
    world_position_data,
    material_surface_id_data,
};

enum PreparedMeshChannel : std::uint32_t {
    prepared_positions = 1U << 0U,
    prepared_normals = 1U << 1U,
    prepared_uv0 = 1U << 2U,
    prepared_uv1 = 1U << 3U,
    prepared_colors = 1U << 4U,
    prepared_tangents = 1U << 5U,
    prepared_source_correspondence = 1U << 6U,
    prepared_material_regions = 1U << 7U,
};

inline constexpr std::uint32_t kKnownPreparedMeshChannels =
    prepared_positions | prepared_normals | prepared_uv0 | prepared_uv1 |
    prepared_colors | prepared_tangents | prepared_source_correspondence |
    prepared_material_regions;

struct PreparedProductReference {
    std::string identity;
    PreparedProductKind kind = PreparedProductKind::render_mesh;
    std::uint32_t format_version = 0U;
    assets::Sha256Digest content_digest;
    std::string relative_path;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PreparedAssetDefinition {
    std::uint64_t id = 0U;
    std::string name;
    std::string render_mesh_product;
    std::optional<std::string> lookdev_product;
    std::uint32_t channel_mask = 0U;
    std::vector<std::uint32_t> material_regions;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PreparedSceneNode {
    std::uint64_t id = 0U;
    std::string name;
    std::optional<std::uint64_t> parent;
    std::optional<std::uint64_t> asset;
    core::Transform local_transform = core::Transform::identity();
    bool visible = true;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PreparedMaterialBinding {
    std::uint64_t asset = 0U;
    std::uint32_t region = 0U;
    render::MaterialId material = 0U;
    std::optional<std::uint64_t> variant;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class PreparedDependencyDisposition : std::uint8_t {
    source_input,
    included_product,
    declared_external,
    unresolved,
};

struct PreparedDependencyReference {
    std::string identity;
    PreparedDependencyDisposition disposition =
        PreparedDependencyDisposition::source_input;
    bool required = true;
    core::Revision revision;
    std::optional<assets::Sha256Digest> content_digest;
    std::string relative_path;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class PreparedCapabilityEvidence : std::uint8_t {
    preserved_source,
    derived_product,
    native_realization,
    runtime_validated,
    unsupported,
};

struct PreparedCapabilityResult {
    std::string feature;
    PreparedCapabilityEvidence evidence =
        PreparedCapabilityEvidence::preserved_source;
    bool required = false;
    std::string detail;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class PreparedSourceEntityKind : std::uint8_t {
    mesh_asset,
    scene_node,
    material_region,
    semantic_area,
    semantic_path,
    structure_graph,
    instance_field,
};

enum class PreparedEntityKind : std::uint8_t {
    asset,
    node,
    material_binding,
    semantic_area,
    semantic_path,
    structure_graph,
    instance_field,
};

struct PreparedSourceCorrespondence {
    PreparedSourceEntityKind source_kind = PreparedSourceEntityKind::mesh_asset;
    std::uint64_t source_primary = 0U;
    std::uint64_t source_secondary = 0U;
    PreparedEntityKind prepared_kind = PreparedEntityKind::asset;
    std::uint64_t prepared_primary = 0U;
    std::uint64_t prepared_secondary = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Stable, digest-bound reference to provider extension content that augments a
// prepared scene. The binding names the extension source and the exact payload
// bytes through content identity; it grants no runtime handle and no mutation
// authority, and the payload itself is carried by the destination-side adapter.
struct PreparedExtensionBinding {
    std::string extension_identity;
    std::string schema_identity;
    assets::Sha256Digest payload_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// Provider-neutral, immutable preparation manifest. It contains no destination
// handles, native engine types, policy implementation, or mutation authority.
// Every product reference is content-addressed and every scene relationship is
// tied back to stable source identity.
struct PreparedSceneEnvelope {
    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string profile_identity;
    assets::Sha256Digest profile_digest;
    PreparedCoordinateConvention coordinates;
    std::vector<PreparedProductReference> products;
    std::vector<PreparedAssetDefinition> assets;
    std::vector<PreparedSceneNode> nodes;
    std::vector<PreparedMaterialBinding> material_bindings;
    std::vector<PreparedDependencyReference> dependencies;
    std::vector<PreparedCapabilityResult> capabilities;
    std::vector<PreparedSourceCorrespondence> correspondences;
    std::vector<PreparedExtensionBinding> extension_bindings;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PreparedSceneEnvelope> deserialize(
        std::string_view text);
};

// Exact derived products for one source mesh. Pointers are borrowed only for
// the duration of compose_prepared_scene and are never retained.
struct PreparedAssetInput {
    std::uint64_t mesh_asset = 0U;
    const RenderMeshArtifact* render_mesh = nullptr;
    const LookdevDrawPackage* lookdev = nullptr;
    // Optional exact material/surface ID product for this mesh and one UV tile.
    const MaterialSurfaceIdMapArtifact* material_surface_ids = nullptr;
};

// Optional instance products are keyed by their embedded scene-node identity,
// not by a mesh asset, because repeated placements have different world space.
struct PreparedWorldPositionInput {
    const WorldPositionMapArtifact* artifact = nullptr;
};

// Extracts exactly the material definitions, variants, source textures,
// samplers, and region assignments reachable from the resolved scope. The
// returned catalog remains source truth; destination realization stays in an
// adapter. Unrelated out-of-scope catalog records are not disclosed.
[[nodiscard]] core::Result<project::MaterialCatalog>
compose_prepared_material_catalog(
    const project::ProjectDocument& document,
    const ResolvedPreparationScope& scope);

// Composes a full-project neutral envelope and verifies each supplied product
// against the exact ProjectDocument revision and digest. Missing products for
// referenced scene assets, stale products, and material-binding drift fail.
[[nodiscard]] core::Result<PreparedSceneEnvelope> compose_prepared_scene(
    std::string source_namespace,
    const project::ProjectDocument& document,
    std::string profile_identity,
    assets::Sha256Digest profile_digest,
    std::span<const PreparedAssetInput> asset_inputs,
    std::span<const PreparedWorldPositionInput> world_position_inputs = {});

[[nodiscard]] core::Result<PreparedSceneEnvelope> compose_prepared_scene(
    std::string source_namespace,
    const project::ProjectDocument& document,
    const ResolvedPreparationScope& scope,
    std::string profile_identity,
    assets::Sha256Digest profile_digest,
    std::span<const PreparedAssetInput> asset_inputs,
    std::span<const PreparedWorldPositionInput> world_position_inputs = {});

// Composes the prepared navigation product reference for a scene-scope
// prepared composition that includes a navigation product, following the
// world.model product pattern: one content-addressed product bound to the
// envelope's canonical prepared digest. The envelope itself stays the
// authoritative compiled record; this reference only names it inside the
// prepared composition.
[[nodiscard]] core::Result<PreparedProductReference> compose_prepared_navigation_product(
    const navgraph::PreparedNavigationEnvelope& envelope);

// Wires a prepared navigation product into a production cook graph. The
// registered node is keyed by the envelope's stable prepared identity — its
// source namespace, source revision, and canonical digest — so the cook key
// changes exactly when the prepared navigation bytes change. Its spatial
// footprint is the union of the envelope's navigation tile footprints mapped
// field-for-field onto SpatialTileRange, and that canonical range is bound
// into the key's spatial_scope, so a changed durable area or path re-cooks
// the navigation product only while the dirty region intersects the
// footprint instead of recooking the whole scene. With no footprint evidence
// the node stays global and is never spatially skipped. Returns the
// registered cook key.
[[nodiscard]] core::Result<ProductionCookKey> register_prepared_navigation_node(
    ProductionCookGraph& graph,
    const navgraph::PreparedNavigationEnvelope& envelope,
    std::vector<ProductionCookKey> dependencies = {});

} // namespace carto::production
