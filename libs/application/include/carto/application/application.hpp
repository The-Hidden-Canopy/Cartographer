#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/core/diagnostic.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/assets/texture_source.hpp>
#include <carto/biological/proxy_mesh.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/condition/effect_preview.hpp>
#include <carto/condition/vegetation_preview.hpp>
#include <carto/condition/state_branch.hpp>
#include <carto/engineering/authoring.hpp>
#include <carto/editor/authoring_context.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/editor/preview_transaction.hpp>
#include <carto/editor/selection.hpp>
#include <carto/editor/tools.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/geometry/road_surface.hpp>
#include <carto/geometry/sign.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/production/decal.hpp>
#include <carto/production/sculpt_stack.hpp>
#include <carto/render/render_scene.hpp>
#include <carto/terrain/terrain_tile.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace carto::ui {
class UiController;
}

namespace carto::agent {
class AgentTask;
class SceneQuery;
enum class GhostCorrectionKind : std::uint8_t;
struct AgentPlan;
}

namespace carto::application {

class ApplicationSession;
class HumanApplicationAccess;

// Authoring actions require an admission object from a named front-end
// boundary. This prevents future proposal/AI code from treating the generic
// ApplicationAction variant as an implicit mutation authority.
class HumanActionAdmission final {
private:
    HumanActionAdmission() = default;

    friend class HumanApplicationAccess;
};

enum class Pane {
    outliner,
    viewport,
    inspector,
    problems,
    history,
};

struct WorkspaceState {
    std::vector<Pane> visible_panes{
        Pane::outliner,
        Pane::viewport,
        Pane::inspector,
        Pane::problems,
        Pane::history,
    };

    [[nodiscard]] core::Result<void> validate() const;
};

struct ObjectView {
    scene::SceneObject object;
    core::Transform world_transform = core::Transform::identity();
    std::optional<core::Revision> mesh_revision;
};

struct SelectionView {
    editor::SelectionMode mode = editor::SelectionMode::object;
    std::vector<scene::ObjectId> objects;
    std::vector<geometry::VertexId> vertices;
    std::vector<geometry::EdgeId> edges;
    std::vector<geometry::FaceId> faces;
    std::optional<scene::ObjectId> component_object;
    std::optional<core::Revision> component_mesh_revision;
    std::vector<editor::ComponentSelectionGroup> component_groups;
};

struct ViewportState {
    render::RenderScene scene;
    struct AttributeLine {
        core::Vec3d start;
        core::Vec3d end;
    };
    struct MeshAttributeOverlayData {
        core::Revision evaluated_mesh_revision;
        std::vector<AttributeLine> hard_edges;
        std::vector<AttributeLine> corner_normals;
        bool has_sharp_edge_layer = false;
        bool has_custom_normal_layer = false;
        bool hard_edge_scan_capped = false;
        bool corner_normal_scan_capped = false;
    };
    struct MeshAttributeOverlay {
        scene::ObjectId object;
        std::uint64_t mesh_asset = 0U;
        core::Revision source_revision;
        std::uint64_t project_generation = 0U;
        std::shared_ptr<const MeshAttributeOverlayData> data;
    };
    std::vector<MeshAttributeOverlay> attribute_overlays;
    core::Revision project_revision;
    std::optional<core::Diagnostic> error;
};

using OperationId = std::uint64_t;

enum class OperationSource {
    human,
    ai_proposal,
    agent_workbench,
    plugin,
    recovery,
};

enum class NativeTraceParameterization : std::uint8_t {
    normalized_face_distance,
    relative_displacement,
    parameterless,
};

enum class NativeTraceTargetKind : std::uint8_t {
    object,
    face,
    vertex,
};

struct NativeStructureMatch {
    scene::ObjectId object;
    std::variant<std::monostate, geometry::FaceId, geometry::VertexId> component;
};

// One step of a recorded face-topology chain (inset, extrude, or poke) as
// structural evidence only: a normalized distance plus the pre-command face
// signature and post-command object signature captured on the exemplar.
struct NativeFaceChainStep {
    std::string tool_id;
    double normalized_distance = 0.0;
    assets::Sha256Digest component_signature;
    assets::Sha256Digest result_object_signature;
    // True when this step targets the same face the previous step targeted;
    // otherwise its face is one the previous step created.
    bool reuse_previous_face = false;
};

struct NativeFaceChainResolution {
    geometry::FaceId face;
    // Absolute distance resolved against the face's longest boundary edge;
    // absent for parameterless operations.
    std::optional<double> distance;
};

inline constexpr std::size_t kMaxNativeFaceChainSteps = 64U;

// Bounded, in-memory evidence for successful human native edits. The trace
// retains source identities only for the current session so export can
// replace them with aliases; it is never written into the project journal.
struct NativeOperationTraceEntry {
    OperationId operation_id = 0U;
    std::string tool_id;
    OperationSource source = OperationSource::human;
    core::Revision revision_before;
    core::Revision revision_after;
    scene::ObjectId target_object;
    std::variant<geometry::FaceId, geometry::VertexId, std::monostate> target_component;
    NativeTraceParameterization parameterization =
        NativeTraceParameterization::normalized_face_distance;
    double normalized_distance = 0.0;
    core::Vec3d relative_displacement;
    // A topology-only, alias-safe match key. It contains no source IDs,
    // names, coordinates, transforms, or project path.
    std::optional<assets::Sha256Digest> structure_signature;
    // Post-command object topology lets a human select and verify the source
    // exemplar after a topology edit has replaced its original component.
    std::optional<assets::Sha256Digest> result_object_structure_signature;
};

struct SculptLayerView {
    std::string identity;
    std::string name;
    bool visible = true;
    double weight = 1.0;
    std::size_t stroke_count = 0U;
};

struct SpatialFieldView {
    std::string identity;
    spatial_field::FieldSemantic semantic = spatial_field::FieldSemantic::generic;
    core::Revision source_revision;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    assets::Sha256Digest digest;
};

struct CurvePathSourceView {
    std::string identity;
    core::Revision source_revision;
    std::size_t segment_count = 0U;
    assets::Sha256Digest digest;
};

struct RoadSourceView {
    std::string identity;
    core::Revision source_revision;
    std::string centerline_identity;
    core::Revision centerline_revision;
    double width_meters = 0.0;
    double shoulder_meters = 0.0;
    double maximum_grade_ratio = 0.0;
    double minimum_curve_radius_meters = 0.0;
    project::RoadSurfaceClass surface_class = project::RoadSurfaceClass::asphalt;
    project::RoadFunctionClass function_class = project::RoadFunctionClass::local;
    project::RoadAccessState access_state = project::RoadAccessState::open;
    project::RoadDrainageIntent drainage_intent = project::RoadDrainageIntent::none;
    assets::Sha256Digest digest;
};

struct SweepSourceView {
    geometry::SweepSource source;
    assets::Sha256Digest digest;
};

struct SignTextSourceView {
    geometry::SignTextSource source;
    assets::Sha256Digest digest;
};

struct DecalPlacementView {
    production::DecalPlacement placement;
    assets::Sha256Digest serialized_digest;
};

struct BiologicalAuthoringSourceView {
    project::BiologicalAuthoringKey key;
    assets::Sha256Digest payload_digest;
};

struct SourceAssetView {
    std::string identity;
    std::string relative_source_path;
    assets::SourceAssetKind kind = assets::SourceAssetKind::other;
    core::Revision source_generation;
    assets::Sha256Digest source_digest;
    std::string importer_identity;
    std::uint32_t importer_version = 0U;
    std::string artifact_identity;
    assets::Sha256Digest artifact_digest;
};

struct MaterialGraphView {
    std::string identity;
    core::Revision source_revision;
    std::size_t node_count = 0U;
};

struct MaterialGraphBindingView {
    render::MaterialId material = 0U;
    std::string graph_identity;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    bool source_current = false;
};

// Ephemeral road authoring output. Both source receipts are required before a
// consumer may display the derived ribbon; the mesh is never persisted here.
struct RoadSurfacePreview {
    std::string road_identity;
    core::Revision road_revision;
    std::uint64_t project_generation = 0U;
    assets::Sha256Digest road_digest;
    assets::Sha256Digest centerline_digest;
    project::RoadSurfaceClass surface_class = project::RoadSurfaceClass::asphalt;
    geometry::RoadSurfaceMesh mesh;

    [[nodiscard]] bool matches(
        std::uint64_t current_project_generation,
        const RoadSourceView& road,
        const CurvePathSourceView& centerline) const {
        return project_generation == current_project_generation &&
            road.identity == road_identity && road.source_revision == road_revision &&
            road.digest == road_digest && road.surface_class == surface_class &&
            road.centerline_identity == mesh.centerline_identity &&
            road.centerline_revision == mesh.centerline_revision &&
            road.width_meters == mesh.road_width_meters &&
            road.shoulder_meters == mesh.shoulder_width_meters &&
            road.maximum_grade_ratio == mesh.maximum_grade_ratio &&
            road.minimum_curve_radius_meters == mesh.minimum_curve_radius_meters &&
            centerline.identity == mesh.centerline_identity &&
            centerline.source_revision == mesh.centerline_revision &&
            centerline.digest == centerline_digest &&
            static_cast<bool>(mesh.validate());
    }
};

enum class RoadTerrainCoordinateFrame : std::uint8_t {
    world_xz_elevation_y = 0U,
};

struct RoadTerrainCell final {
    std::uint32_t u = 0U;
    std::uint32_t v = 0U;
    core::Vec3d position;
    double terrain_elevation_meters = 0.0;
    double design_elevation_meters = 0.0;
    // Positive means terrain must be cut; negative means it must be filled.
    double signed_cut_depth_meters = 0.0;
    double represented_area_square_meters = 0.0;
};

// Disposable, explicitly non-engineering estimate where the authored road
// corridor intersects an exact terrain source. It does not model crossfall,
// compaction, drainage construction, or subsurface conditions.
struct RoadTerrainPreview final {
    static constexpr std::size_t kMaxCells = 65'536U;
    static constexpr std::uint64_t kMaxCandidateGridTests = 4'194'304U;

    std::uint64_t project_generation = 0U;
    RoadSurfacePreview road_surface;
    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    spatial_field::FieldSemantic terrain_semantic =
        spatial_field::FieldSemantic::generic;
    core::Vec2d terrain_origin_xz{};
    double terrain_cell_size_meters = 0.0;
    std::uint32_t terrain_width = 0U;
    std::uint32_t terrain_height = 0U;
    RoadTerrainCoordinateFrame coordinate_frame =
        RoadTerrainCoordinateFrame::world_xz_elevation_y;
    std::uint64_t candidate_grid_tests = 0U;
    std::vector<RoadTerrainCell> cells;
    double cut_volume_cubic_meters = 0.0;
    double fill_volume_cubic_meters = 0.0;
    assets::Sha256Digest preview_digest;

    [[nodiscard]] core::Result<void> validate() const;
    // Compares source receipts only. UI caches should call this after the
    // builder has validated the product; it deliberately avoids rescanning
    // the bounded sample array on every frame.
    [[nodiscard]] bool matches_sources(
        std::uint64_t current_project_generation,
        const RoadSourceView& road,
        const CurvePathSourceView& centerline,
        const SpatialFieldView& terrain) const noexcept;
    [[nodiscard]] bool matches(
        std::uint64_t current_project_generation,
        const RoadSourceView& road,
        const CurvePathSourceView& centerline,
        const SpatialFieldView& terrain) const;
};

struct SpatialFieldPreview {
    static constexpr std::uint32_t kMaxPreviewAxis = 32U;
    std::string identity;
    spatial_field::FieldSemantic semantic = spatial_field::FieldSemantic::generic;
    core::Revision source_revision;
    core::Vec2d origin_uv{};
    double cell_size = 1.0;
    double minimum_value = 0.0;
    double maximum_value = 1.0;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t preview_width = 0U;
    std::uint32_t preview_height = 0U;
    std::vector<double> values;
};

// Read-only projection of the DV012 engineering authoring sidecar. The
// authoritative records remain owned by carto_engineering and are never
// exposed as a mutable UI copy.
struct EngineeringAuthoringView {
    bool present = false;
    assets::Sha256Digest digest;
    std::string project_namespace;
    core::Revision authored_revision;
    std::size_t source_count = 0U;
    std::size_t part_count = 0U;
    std::size_t occurrence_count = 0U;
    std::size_t topology_count = 0U;
    std::size_t bom_count = 0U;
    std::size_t requirement_count = 0U;
    std::size_t characteristic_count = 0U;
    std::size_t target_count = 0U;
    std::size_t derived_product_count = 0U;
    struct Source {
        std::uint64_t id = 0U;
        std::string kind;
        std::string locator;
        std::string revision;
        assets::Sha256Digest digest;
    };
    std::vector<Source> sources;
    struct Topology {
        std::uint64_t id = 0U;
        std::uint64_t source_id = 0U;
        std::uint64_t part_id = 0U;
        std::string kind;
        std::string external_id;
        std::string source_revision;
    };
    std::vector<Topology> topology;
    struct Requirement {
        std::uint64_t id = 0U;
        std::uint64_t source_id = 0U;
        std::vector<std::uint64_t> topology_ids;
        engineering::RequirementKind model_kind =
            engineering::RequirementKind::dimensional_distance;
        std::string kind;
        std::string extraction;
        double nominal = 0.0;
        double plus = 0.0;
        double minus = 0.0;
        std::string unit;
        std::string callout;
        std::string original_text;
        std::string acceptance_rule;
        bool enabled = true;
        std::uint32_t source_page = 0U;
        std::string source_section;
        std::string human_review_justification;
        std::string unsupported_content;
    };
    std::vector<Requirement> requirements;
    struct Characteristic {
        std::uint64_t id = 0U;
        std::uint64_t requirement_id = 0U;
        std::vector<std::uint64_t> topology_ids;
        std::string kind;
        std::string status;
        std::string sensor_type;
        std::string tool_center_frame;
        std::string coverage_kind;
        double coverage_percentage = 0.0;
        std::uint32_t priority = 0U;
        double min_standoff = 0.0;
        double preferred_standoff = 0.0;
        double max_standoff = 0.0;
        bool contact_required = false;
    };
    std::vector<Characteristic> characteristics;
    struct Target {
        std::uint64_t id = 0U;
        std::uint64_t characteristic_id = 0U;
        std::uint64_t source_topology_id = 0U;
        std::string status;
        std::string generation_basis;
        double standoff = 0.0;
        bool human_authored = false;
        core::Vec3d pose_translation{};
    };
    std::vector<Target> targets;
    struct WorkcellEntity {
        std::uint64_t id = 0U;
        std::string kind;
        std::string name;
        std::string source;
        core::Transform transform = core::Transform::identity();
        bool enabled = false;
        bool reference_frame = false;
    };
    struct WorkcellKeepOut {
        std::uint64_t id = 0U;
        std::string kind;
        std::string purpose;
        std::string source;
        core::Transform transform = core::Transform::identity();
        core::Vec3d dimensions{1.0, 1.0, 1.0};
        bool enabled = false;
    };
    struct Workcell {
        std::uint64_t id = 0U;
        std::string revision;
        std::string robot_model_identity;
        std::string robot_source_identity;
        std::string active_tool_frame;
        std::string capability_profile;
        std::string sensor_identity;
        std::vector<WorkcellEntity> entities;
        std::vector<WorkcellKeepOut> keep_outs;
        std::vector<engineering::WorkcellRevisionRecord> revision_history;
    };
    std::optional<Workcell> workcell;
    struct PathSample {
        std::uint32_t sequence = 0U;
        double normalized = 0.0;
        core::Transform tool_pose = core::Transform::identity();
        std::optional<std::uint64_t> target_id;
        std::string status;
        double clearance = 0.0;
    };
    struct Path {
        std::uint64_t id = 0U;
        assets::Sha256Digest basis_digest;
        core::Revision basis_revision;
        std::string status;
        std::optional<engineering::VantaPathResultReceipt> vanta_receipt;
        std::vector<PathSample> samples;
    };
    std::optional<Path> path;
    struct BomLine {
        std::uint64_t id = 0U;
        std::uint64_t source_id = 0U;
        std::string part_number;
        std::string revision;
        std::string description;
        std::string material;
        double quantity = 0.0;
        std::string unit;
        std::optional<std::uint64_t> mapped_part_id;
        std::string mapped_revision;
        std::string mapped_material;
        std::optional<double> assembly_quantity;
        std::string reconciliation;
    };
    std::vector<BomLine> bom;
    bool stale = false;
    bool workcell_present = false;
    bool path_present = false;
    bool publication_ready = false;
    bool consumer_admissible = false;
    std::string status;
};

// Read-only projection of the public CONDITION source-authoring contract.
// This contains authored counts and admission state only; it never exposes a
// mutable runtime or behavioral model to the UI.
struct ConditionAuthoringView {
    bool present = false;
    assets::Sha256Digest digest;
    std::string project_namespace;
    core::Revision authored_revision;
    std::size_t source_count = 0U;
    std::size_t asset_count = 0U;
    std::size_t safe_place_count = 0U;
    std::size_t familiarity_count = 0U;
    std::size_t view_condition_count = 0U;
    std::size_t variant_count = 0U;
    std::size_t temporal_package_count = 0U;
    std::size_t surface_state_count = 0U;
    std::size_t interaction_anchor_count = 0U;
    std::size_t collection_count = 0U;
    std::size_t prefab_count = 0U;
    std::size_t prefab_instance_count = 0U;
    std::size_t region_kit_count = 0U;
    std::size_t audio_source_count = 0U;
    std::size_t acoustic_zone_count = 0U;
    std::size_t effect_source_count = 0U;
    std::size_t particle_emitter_count = 0U;
    std::size_t field_influence_count = 0U;
    std::size_t vegetation_scatter_count = 0U;
    std::vector<std::uint64_t> vegetation_scatter_ids;
    std::size_t kinematic_assembly_count = 0U;
    std::size_t state_branch_count = 0U;
    struct Source {
        std::uint64_t id = 0U;
        std::string kind;
        std::string label;
        std::string locator;
        std::string revision;
        assets::Sha256Digest digest;
        bool current = true;
        bool reviewed = false;
        std::vector<std::string> diagnostics;
    };
    struct Asset {
        std::uint64_t id = 0U;
        std::string stable_name;
        std::string asset_class;
        std::uint64_t authored_object_id = 0U;
    };
    struct InteractionAnchor {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        std::string kind;
        std::string interaction_class;
        core::Transform transform = core::Transform::identity();
        core::Vec3d approach_direction{0.0, 0.0, 1.0};
        core::Vec3d clearance{1.0, 1.0, 1.0};
        bool enabled = true;
    };
    struct Prefab {
        std::uint64_t id = 0U;
        std::string name;
        std::vector<std::uint64_t> asset_ids;
        std::vector<std::string> sockets;
        std::vector<std::uint64_t> variant_ids;
        core::Revision source_revision;
    };
    struct PrefabInstance {
        std::uint64_t id = 0U;
        std::uint64_t prefab_id = 0U;
        std::string name;
        std::uint64_t root_object_id = 0U;
        std::size_t member_count = 0U;
        bool current = false;
        std::string status;
    };
    struct Collection {
        std::uint64_t id = 0U;
        std::string name;
        std::vector<std::uint64_t> asset_ids;
        std::vector<std::uint64_t> child_collection_ids;
        std::string semantic_class;
    };
    struct RegionKit {
        std::uint64_t id = 0U;
        std::string name;
        std::vector<std::uint64_t> asset_ids;
        std::vector<std::uint64_t> prefab_ids;
        std::vector<std::string> required_classes;
        double interaction_coverage = 0.0;
        double familiarity_coverage = 0.0;
        bool collision_validation_required = true;
        bool semantic_validation_required = true;
    };
    struct Variant {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        std::string name;
        std::string kind;
        std::string semantic_id;
    };
    struct AudioSource {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        std::uint64_t clip_source_id = 0U;
        core::Transform local_transform = core::Transform::identity();
        std::string bus;
        double gain = 1.0;
        double minimum_distance = 0.0;
        double maximum_distance = 0.0;
        bool spatial = true;
        bool enabled = true;
    };
    struct AcousticZone {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        core::Transform local_transform = core::Transform::identity();
        std::string shape;
        core::Vec3d half_extents{1.0, 1.0, 1.0};
        double radius = 0.0;
        std::string acoustic_class;
        std::string semantic_region;
        std::uint16_t priority = 0U;
        bool enabled = true;
    };
    struct EffectSource {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        std::string semantic_owner;
        std::string normal_state;
        std::string disabled_state;
        bool enabled = true;
    };
    struct KinematicAssembly {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        std::string source_identity;
        core::Revision source_revision;
        std::size_t joint_count = 0U;
        std::size_t part_binding_count = 0U;
        bool part_bindings_current = false;
        std::vector<std::uint64_t> hinge_joint_ids;
    };
    std::vector<Source> sources;
    struct SafePlace {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        std::array<double, 8> values{};
    };
    struct SurfaceState {
        std::uint64_t id = 0U;
        std::uint64_t asset_id = 0U;
        std::string occupancy;
        std::string maintenance;
        std::string damage;
        double wetness = 0.0;
    };
    struct StateBranch {
        std::uint64_t id = 0U;
        std::uint64_t parent_branch_id = 0U;
        std::string name;
        std::string applicability;
        std::size_t delta_count = 0U;
    };
    std::vector<Asset> assets;
    std::vector<InteractionAnchor> interaction_anchors;
    std::vector<Collection> collections;
    std::vector<Prefab> prefabs;
    std::vector<PrefabInstance> prefab_instances;
    std::vector<RegionKit> region_kits;
    std::vector<Variant> variants;
    std::vector<SafePlace> safe_places;
    std::vector<SurfaceState> surface_states;
    std::vector<StateBranch> state_branches;
    std::vector<AudioSource> audio_sources;
    std::vector<AcousticZone> acoustic_zones;
    std::vector<EffectSource> effect_sources;
    std::vector<KinematicAssembly> kinematic_assemblies;
    bool stale = false;
    bool publication_ready = false;
    bool consumer_admissible = false;
    std::string status;
};

struct ApplicationSnapshot {
    std::string project_name;
    std::optional<std::filesystem::path> project_path;
    std::uint64_t project_generation = 0;
    core::Revision project_revision;
    bool dirty = false;
    std::vector<ObjectView> objects;
    SelectionView selection;
    std::vector<editor::ToolDescriptor> tools;
    std::size_t undo_count = 0;
    std::size_t redo_count = 0;
    std::vector<core::Diagnostic> problems;
    std::vector<NativeOperationTraceEntry> native_operation_trace;
    WorkspaceState workspace;
    ViewportState viewport;
    std::optional<std::uint64_t> sculpt_mesh_asset;
    std::optional<assets::Sha256Digest> sculpt_stack_digest;
    std::vector<SculptLayerView> sculpt_layers;
    std::vector<SpatialFieldView> spatial_fields;
    std::vector<CurvePathSourceView> curve_paths;
    std::vector<RoadSourceView> road_sources;
    std::vector<SweepSourceView> sweep_sources;
    std::vector<SignTextSourceView> sign_text_sources;
    std::vector<DecalPlacementView> decal_placements;
    std::vector<BiologicalAuthoringSourceView> biological_sources;
    std::vector<SourceAssetView> source_assets;
    std::size_t source_asset_tombstone_count = 0U;
    std::vector<project::TextureSourceRecord> texture_sources;
    std::vector<render::MaterialAsset> materials;
    std::vector<MaterialGraphView> material_graphs;
    std::vector<MaterialGraphBindingView> material_graph_bindings;
    std::vector<project::MaterialRegionKey> material_regions;
    std::vector<project::MaterialRegionAssignment> material_assignments;
    project::PresentationCatalog presentation_catalog;
    project::ProjectAuthoringStandards authoring_standards;
    EngineeringAuthoringView engineering;
    ConditionAuthoringView condition;
};

enum class RecoveryInspectionState {
    unavailable,
    clean,
    pending,
    blocked,
};

struct RecoveryJournalEntry {
    core::Revision revision_before;
    core::Revision revision_after;
    std::string event_type;
};

// Read-only startup evidence. Inspection never mutates the project, journal,
// command history, or in-memory application state. `pending` means the
// journal is ahead of the saved project (or the project file is missing), so a
// future explicit recovery decision may be offered. `blocked` means the
// durable evidence is malformed or semantically inconsistent and must not be
// presented as recoverable state.
struct RecoveryInspection {
    std::filesystem::path project_path;
    std::filesystem::path journal_path;
    RecoveryInspectionState state = RecoveryInspectionState::unavailable;
    std::optional<core::Revision> project_revision;
    core::Revision durable_revision;
    std::size_t journal_entry_count = 0U;
    std::vector<RecoveryJournalEntry> journal_entries;
    std::optional<core::Diagnostic> diagnostic;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AffectedSet {
    std::vector<scene::ObjectId> objects;
    std::vector<geometry::VertexId> vertices;
    std::vector<geometry::EdgeId> edges;
    std::vector<geometry::FaceId> faces;
};

struct ParameterPayload {
    std::optional<core::Transform> transform;
    std::optional<double> distance;
    std::optional<core::Vec3d> position;
    std::optional<bool> remove_orphaned_vertices;
    std::optional<double> factor;
    std::optional<geometry::EdgeId> support_edge;
    std::optional<std::uint32_t> segments;
    std::optional<core::Vec3d> normal;
    std::optional<bool> edge_sharp;
};

struct DispatchReceipt {
    std::string action;
    core::Revision revision_before;
    core::Revision revision_after;
    bool document_changed = false;
    OperationId operation_id = 0U;
    OperationSource source = OperationSource::human;
    std::string provenance_id;
    AffectedSet affected;
    std::optional<ParameterPayload> parameters;
    std::vector<core::Diagnostic> warnings;
    // Set only when an opted-in open applied automatic startup recovery from
    // the durable journal; that receipt carries OperationSource::recovery so
    // callers can surface the automatic provenance like explicit recovery.
    bool automatic_recovery_applied = false;
};

struct NewProjectAction {
    std::string name;
    bool discard_dirty = false;
};

struct OpenProjectOptions {
    // Opt-in automatic startup recovery. When inspection reports `pending`,
    // the open applies the latest verified snapshot-bearing journal entry
    // through the same replay path as the explicit recovery action; any other
    // state keeps the existing open behavior.
    bool auto_recover = false;
};

struct OpenProjectAction {
    OpenProjectAction() = default;
    OpenProjectAction(
        std::filesystem::path project_path,
        bool discard = false,
        OpenProjectOptions open_options = {})
        : path(std::move(project_path)),
          discard_dirty(discard),
          options(open_options) {}

    std::filesystem::path path;
    bool discard_dirty = false;
    OpenProjectOptions options;
};

struct RecoverProjectAction {
    std::filesystem::path path;
    bool discard_dirty = false;
};

struct SaveProjectAction {
    std::optional<std::filesystem::path> path;
};

struct SetWorkspaceAction {
    WorkspaceState workspace;
};

struct SetSelectionModeAction {
    editor::SelectionMode mode;
};

struct ConvertSelectionAction {
    editor::SelectionMode mode;
};

struct ExpandSelectionAction {
    editor::SelectionExpansion expansion;
};

struct SelectShortestPathAction {
    geometry::VertexId goal;
    std::optional<scene::ObjectId> owner = std::nullopt;
};

struct SelectObjectAction {
    scene::ObjectId object;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SelectVertexAction {
    scene::ObjectId object;
    geometry::VertexId vertex;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SelectFaceAction {
    scene::ObjectId object;
    geometry::FaceId face;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SelectEdgeAction {
    scene::ObjectId object;
    geometry::EdgeId edge;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SetObjectTransformAction {
    scene::ObjectId object;
    core::Transform transform;
    std::optional<core::Revision> expected_project_revision;
};

struct InvokeToolAction {
    std::string tool_id;
    editor::ToolArguments arguments;
};

struct RepeatLastToolAction {};

struct MeshCleanupOptions {
    static constexpr std::size_t kMaxCandidates = 4096U;
    static constexpr std::size_t kMaxMergedVertices = 4096U;
    static constexpr std::size_t kMaxRemovedFaces = 64U;
    static constexpr std::size_t kMaxMeshElements = 100'000U;
    double merge_distance = 1.0e-6;
    std::size_t max_candidates = kMaxCandidates;
    std::size_t max_merged_vertices = kMaxMergedVertices;
    std::size_t max_removed_faces = kMaxRemovedFaces;
};

// Ephemeral human review of one exact merge proposal. This cannot be
// manufactured from caller-authored patch bytes or copied into a second token.
class MeshCleanupReview final {
public:
    MeshCleanupReview(const MeshCleanupReview&) = delete;
    MeshCleanupReview& operator=(const MeshCleanupReview&) = delete;
    MeshCleanupReview(MeshCleanupReview&&) noexcept = default;
    MeshCleanupReview& operator=(MeshCleanupReview&&) noexcept = default;
    [[nodiscard]] bool active() const noexcept { return active_ != nullptr; }
    void cancel() noexcept { active_.reset(); }
    [[nodiscard]] std::uint64_t project_generation() const { return project_generation_; }
    [[nodiscard]] core::Revision project_revision() const { return project_revision_; }
    [[nodiscard]] scene::ObjectId object() const { return object_; }
    [[nodiscard]] std::uint64_t mesh_asset() const { return mesh_asset_; }
    [[nodiscard]] core::Revision mesh_revision() const { return mesh_revision_; }
    [[nodiscard]] const assets::Sha256Digest& source_digest() const { return source_digest_; }
    [[nodiscard]] const assets::Sha256Digest& result_digest() const { return result_digest_; }
    [[nodiscard]] const MeshCleanupOptions& options() const { return options_; }
    [[nodiscard]] const std::vector<geometry::VertexId>& candidates() const { return candidates_; }
    [[nodiscard]] const geometry::TopologyEditReceipt& receipt() const { return receipt_; }
    [[nodiscard]] const std::vector<geometry::FaceId>& modified_faces() const { return modified_faces_; }

private:
    friend class ApplicationSession;
    MeshCleanupReview() = default;
    std::unique_ptr<bool> active_ = std::make_unique<bool>(true);
    std::weak_ptr<const std::uint8_t> session_identity_;
    std::uint64_t project_generation_ = 0U;
    core::Revision project_revision_;
    scene::ObjectId object_;
    std::uint64_t mesh_asset_ = 0U;
    core::Revision mesh_revision_;
    assets::Sha256Digest source_digest_;
    assets::Sha256Digest result_digest_;
    MeshCleanupOptions options_;
    std::vector<geometry::VertexId> candidates_;
    geometry::TopologyEditReceipt receipt_;
    std::vector<geometry::FaceId> modified_faces_;
};

// Re-evaluates the previous tool with a complete replacement argument set.
// The tool identity and authoring selection remain revision-bound session state.
struct AdjustLastToolAction {
    editor::ToolArguments arguments;
};

struct CreateBoxAction {
    std::string object_name;
    core::Vec3d size{1.0, 1.0, 1.0};
};

struct CreatePlaneAction {
    std::string object_name;
    double width = 1.0;
    double depth = 1.0;
};

struct CreateMeshObjectAction {
    std::string object_name;
    geometry::EditableMesh mesh;
    std::optional<assets::SourceImportRecord> source_record = std::nullopt;
    std::optional<core::Revision> expected_project_revision = std::nullopt;
};

// Canonical digest for the geometry domain imported by Cartographer's native
// OBJ/PLY/STL providers. Attributes and presentation data are intentionally
// excluded until those importers preserve them.
[[nodiscard]] assets::Sha256Digest canonical_mesh_geometry_digest(
    const geometry::EditableMesh& mesh);
[[nodiscard]] core::Result<std::optional<assets::Sha256Digest>>
canonical_mesh_material_region_digest(const geometry::EditableMesh& mesh);

// Appends one deterministic point stroke to the currently selected vertex's
// project-bound SculptStack. The UI snapshot revision is rechecked before
// authoring and only the human command boundary may apply this action.
struct AppendSculptStrokeAction {
    core::Revision expected_project_revision;
    production::SculptStrokeMode mode = production::SculptStrokeMode::add;
    double radius = 0.25;
    double strength = 0.05;
};

// Commits samples captured against a viewport snapshot. Each sample must carry
// a stable source-surface anchor that resolves against the current mesh.
struct AppendSculptSurfaceStrokeAction {
    core::Revision expected_project_revision;
    scene::ObjectId object;
    std::uint64_t mesh_asset = 0U;
    production::SculptStrokeMode mode = production::SculptStrokeMode::add;
    double radius = 0.25;
    double strength = 0.05;
    std::vector<production::SculptStrokeSample> samples;
};

// Updates one layer on the currently selected mesh. Visibility edits are
// revision-bound project mutations and may only enter through the human path.
struct SetSculptLayerVisibilityAction {
    core::Revision expected_project_revision;
    std::uint64_t mesh_asset = 0U;
    std::string layer_identity;
    bool visible = true;
};

// Duplicates only an authored mesh instance. The new root object retains the
// source mesh asset identity; it does not copy geometry or hierarchy state.
struct DuplicateMeshInstanceAction {
    scene::ObjectId source;
    std::string object_name;
    core::Vec3d local_translation_offset{};
};

// Duplicates one authored mesh instance into a bounded batch of new root
// objects that all reference the source mesh asset. Every placement carries
// an explicit finite local translation offset; the batch is validated and
// commits atomically through the same undoable, journaled history path.
struct DuplicateMeshInstanceBatchAction {
    scene::ObjectId source;
    std::vector<editor::MeshInstanceBatchInstance> instances;
};

// Commits only the exact current field-driven preview and a matching native
// botanical mesh source. ApplicationSession recomputes the preview and checks
// project/source revisions before admitting the single undoable batch.
struct CreatePlantScatterBatchAction {
    core::Revision expected_project_revision;
    condition::Id recipe_id = 0U;
    assets::Sha256Digest expected_preview_digest;
    scene::ObjectId botanical_source_object;
};

struct ImportEngineeringFixtureAction {};
struct ImportEngineeringAuthoringAction { std::string serialized_document; };
struct ImportEngineeringBomCsvAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::string locator;
    std::string csv_bytes;
};
struct ImportEngineeringRequirementCsvAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::string locator;
    std::string csv_bytes;
};
struct ImportEngineeringPathResultAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::string result_bytes;
};
struct AttachEngineeringEvidenceAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    engineering::SourceKind kind = engineering::SourceKind::drawing;
    std::string locator;
    std::string source_bytes;
};
struct CreateEngineeringRequirementAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::uint64_t source_id = 0U;
    std::vector<std::uint64_t> topology_ids;
    engineering::RequirementKind kind = engineering::RequirementKind::dimensional_distance;
    double nominal = 0.0;
    double plus = 0.0;
    double minus = 0.0;
    std::string unit = "millimeters";
    std::uint32_t source_page = 0U;
    std::string source_section;
    std::string source_callout;
    std::string original_text;
    std::string acceptance_rule;
};
enum class RequirementReviewDecision : std::uint8_t { verify, correct, reject };
struct ReviewEngineeringRequirementAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    engineering::Id requirement_id = 0U;
    RequirementReviewDecision decision = RequirementReviewDecision::verify;
    engineering::RequirementKind corrected_kind = engineering::RequirementKind::dimensional_distance;
    double corrected_nominal = 0.0;
    double corrected_plus = 0.0;
    double corrected_minus = 0.0;
    std::string corrected_unit;
    std::string corrected_acceptance_rule;
    std::string justification;
};
struct CreateEngineeringCharacteristicAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::uint64_t requirement_id = 0U;
    engineering::InspectionKind kind = engineering::InspectionKind::point_measurement;
    engineering::SensorConstraint sensor;
    engineering::CoverageConstraint coverage;
    engineering::AccessConstraint access;
    std::uint32_t priority = 0U;
};
struct CreateEngineeringTargetAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::uint64_t characteristic_id = 0U;
    std::uint64_t source_topology_id = 0U;
    core::Transform sensor_pose = core::Transform::identity();
    double standoff = 0.0;
    std::string pose_frame = "workcell";
    std::string justification;
};
struct UpdateEngineeringWorkcellAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::string expected_workcell_revision;
    std::string new_workcell_revision;
    std::optional<std::uint64_t> entity_id;
    std::optional<std::uint64_t> keep_out_id;
    core::Transform transform = core::Transform::identity();
    core::Vec3d dimensions{1.0, 1.0, 1.0};
    std::string justification;
};
struct CreateEngineeringKeepOutAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::string expected_workcell_revision;
    std::string new_workcell_revision;
    std::string purpose;
    core::Vec3d center{};
    core::Vec3d dimensions{1.0, 1.0, 1.0};
    std::string justification;
};
struct CreateEngineeringWorkcellFrameAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::string expected_workcell_revision;
    std::string new_workcell_revision;
    std::string name;
    core::Transform transform = core::Transform::identity();
    std::string justification;
};
struct ReviewEngineeringTargetAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    std::uint64_t target_id = 0U;
    engineering::TargetStatus decision = engineering::TargetStatus::proposed;
};

struct InvalidateEngineeringSourceAction {
    engineering::Id source_id = 0U;
    std::string new_revision;
    assets::Sha256Digest new_digest;
};

struct ImportConditionFixtureAction {};
struct ImportConditionAuthoringAction { std::string serialized_document; };
struct BindConditionAssetObjectAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    condition::Id asset_id = 0U;
    scene::ObjectId object_id{};
};
struct AddConditionInteractionAnchorAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    condition::InteractionAnchor anchor;
};
struct AddConditionPrefabAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    condition::PrefabRecord prefab;
};
struct InstantiateConditionPrefabAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    condition::Id prefab_id = 0U;
    std::string name;
    core::Vec3d world_position{};
};
struct AddConditionCollectionAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    condition::CollectionRecord collection;
};
struct AddConditionRegionKitAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    condition::RegionKitManifest region_kit;
};
struct AddConditionKinematicAssemblyAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    condition::KinematicAssemblySource assembly;
};
using ConditionAuthoredRecord = std::variant<
    condition::InteractionAnchor,
    condition::PrefabRecord,
    condition::CollectionRecord,
    condition::RegionKitManifest,
    condition::KinematicAssemblySource>;
struct ReplaceConditionAuthoredRecordAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    ConditionAuthoredRecord record;
};
enum class ConditionAuthoredRecordKind : std::uint8_t {
    interaction_anchor,
    prefab,
    collection,
    region_kit,
    kinematic_assembly,
};
struct DeleteConditionAuthoredRecordAction {
    core::Revision expected_project_revision;
    core::Revision expected_authoring_revision;
    ConditionAuthoredRecordKind kind = ConditionAuthoredRecordKind::interaction_anchor;
    condition::Id id = 0U;
};

struct InvalidateConditionSourceAction {
    condition::Id source_id = 0U;
    std::string new_revision;
    assets::Sha256Digest new_digest;
};

struct ApplySourceReimportPlanAction {
    assets::SourceReimportPlan plan;
};

struct RelinkMovedSourceAction {
    std::string identity;
    assets::Sha256Digest staged_digest;
};

struct CreateSpatialFieldAction {
    std::string identity;
    spatial_field::FieldSemantic semantic = spatial_field::FieldSemantic::generic;
    core::Vec2d origin_uv{};
    double cell_size = 1.0;
    std::uint32_t width = 32U;
    std::uint32_t height = 32U;
    double minimum_value = 0.0;
    double maximum_value = 1.0;
    double initial_value = 0.0;
};

struct PaintSpatialFieldAction {
    std::string identity;
    spatial_field::FieldBrush brush;
};

struct CreateMaterialAction {
    core::Revision expected_project_revision;
    std::string name;
    render::StandardMaterial definition;
};

struct UpdateMaterialAction {
    core::Revision expected_project_revision;
    render::MaterialId material_id = 0U;
    core::Revision expected_material_revision;
    render::StandardMaterial definition;
};

struct SetMaterialRegionAssignmentAction {
    core::Revision expected_project_revision;
    project::MaterialRegionKey key;
    std::optional<render::MaterialId> material;
    std::optional<project::MaterialVariantId> variant;
};

struct GenerateUniqueLightmapUvAction {
    core::Revision expected_project_revision;
    scene::ObjectId object;
    attributes::UvSetId output_uv_set = 0U;
    std::uint32_t resolution = 0U;
    std::uint32_t padding_texels = 0U;
    std::uint32_t maximum_udim_tiles = 0U;
};

struct SetMaterialGraphAction {
    core::Revision expected_project_revision;
    std::optional<core::Revision> expected_graph_revision;
    std::optional<assets::Sha256Digest> expected_graph_digest;
    project::MaterialGraph graph;
};

struct SetMaterialGraphBindingAction {
    core::Revision expected_project_revision;
    render::MaterialId material_id = 0U;
    core::Revision expected_material_revision;
    std::optional<std::string> graph_identity;
    std::optional<core::Revision> expected_graph_revision;
    std::optional<assets::Sha256Digest> expected_graph_digest;
};

struct ImportMaterialTextureSourceAction {
    core::Revision expected_project_revision;
    assets::Sha256Digest content_digest;
    assets::TextureSourceFormat format = assets::TextureSourceFormat::png;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct SetPresentationCatalogAction {
    std::uint64_t expected_project_generation = 0U;
    core::Revision expected_project_revision;
    project::PresentationCatalog catalog;
};

struct SetProjectAuthoringStandardsAction {
    std::uint64_t expected_project_generation = 0U;
    core::Revision expected_project_revision;
    project::ProjectAuthoringStandards standards;
};

struct SetRoadAuthoringAction {
    core::Revision expected_project_revision;
    geometry::CurvePath centerline;
    project::RoadSource road;
};

struct CreateRoadSurfaceInstanceAction {
    std::uint64_t expected_project_generation = 0U;
    core::Revision expected_project_revision;
    std::string road_identity;
    core::Revision expected_road_revision;
    assets::Sha256Digest expected_road_digest;
    std::string centerline_identity;
    core::Revision expected_centerline_revision;
    assets::Sha256Digest expected_centerline_digest;
    std::uint32_t samples_per_segment = 0U;
};

struct SetSweepSourceAction {
    core::Revision expected_project_revision;
    geometry::SweepSource source;
};

struct SetBiologicalAuthoringAction {
    core::Revision expected_project_revision;
    biological::BiologicalAuthoringDocument source;
};

struct CreateBiologicalProxyInstanceAction {
    core::Revision expected_project_revision;
    project::BiologicalAuthoringKey source_key;
    core::Revision expected_source_revision;
    biological::StructureId structure;
    assets::Sha256Digest expected_source_payload_digest;
    assets::Sha256Digest expected_product_digest;
};

struct SetSignTextSourceAction {
    core::Revision expected_project_revision;
    geometry::SignTextSource source;
};

struct SetDecalPlacementAction {
    core::Revision expected_project_revision;
    core::Revision expected_placement_revision;
    production::DecalPlacement placement;
};

struct CreateSignTextObjectAction {
    core::Revision expected_project_revision;
    std::string source_identity;
    core::Revision expected_source_revision;
};

struct UndoAction {};
struct RedoAction {};

using ApplicationAction = std::variant<
    NewProjectAction,
    OpenProjectAction,
    RecoverProjectAction,
    SaveProjectAction,
    SetWorkspaceAction,
    SetSelectionModeAction,
    ConvertSelectionAction,
    ExpandSelectionAction,
    SelectShortestPathAction,
    SelectObjectAction,
    SelectVertexAction,
    SelectFaceAction,
    SelectEdgeAction,
    SetObjectTransformAction,
    InvokeToolAction,
    RepeatLastToolAction,
    AdjustLastToolAction,
    CreateBoxAction,
    CreatePlaneAction,
    CreateMeshObjectAction,
    AppendSculptStrokeAction,
    AppendSculptSurfaceStrokeAction,
    SetSculptLayerVisibilityAction,
    DuplicateMeshInstanceAction,
    DuplicateMeshInstanceBatchAction,
    CreatePlantScatterBatchAction,
    ImportEngineeringFixtureAction,
    ImportEngineeringAuthoringAction,
    ImportEngineeringBomCsvAction,
    ImportEngineeringRequirementCsvAction,
    ImportEngineeringPathResultAction,
    AttachEngineeringEvidenceAction,
    CreateEngineeringRequirementAction,
    ReviewEngineeringRequirementAction,
    CreateEngineeringCharacteristicAction,
    CreateEngineeringTargetAction,
    UpdateEngineeringWorkcellAction,
    CreateEngineeringKeepOutAction,
    CreateEngineeringWorkcellFrameAction,
    ReviewEngineeringTargetAction,
    InvalidateEngineeringSourceAction,
    ImportConditionFixtureAction,
    ImportConditionAuthoringAction,
    BindConditionAssetObjectAction,
    AddConditionInteractionAnchorAction,
    AddConditionPrefabAction,
    InstantiateConditionPrefabAction,
    AddConditionCollectionAction,
    AddConditionRegionKitAction,
    AddConditionKinematicAssemblyAction,
    ReplaceConditionAuthoredRecordAction,
    DeleteConditionAuthoredRecordAction,
    InvalidateConditionSourceAction,
    ApplySourceReimportPlanAction,
    RelinkMovedSourceAction,
    CreateSpatialFieldAction,
    PaintSpatialFieldAction,
    CreateMaterialAction,
    UpdateMaterialAction,
    SetMaterialRegionAssignmentAction,
    GenerateUniqueLightmapUvAction,
    SetMaterialGraphAction,
    SetMaterialGraphBindingAction,
    ImportMaterialTextureSourceAction,
    SetPresentationCatalogAction,
    SetProjectAuthoringStandardsAction,
    SetRoadAuthoringAction,
    CreateRoadSurfaceInstanceAction,
    SetSweepSourceAction,
    SetBiologicalAuthoringAction,
    SetSignTextSourceAction,
    SetDecalPlacementAction,
    CreateSignTextObjectAction,
    CreateBiologicalProxyInstanceAction,
    UndoAction,
    RedoAction>;

// The explicit human/local front-end adapter is the only production factory
// for HumanActionAdmission. AI proposals use the separate UI proposal API;
// the source/provenance overload is private to that host boundary and cannot
// be used as a public raw AI mutation route.
class HumanApplicationAccess final {
public:
    [[nodiscard]] static core::Result<DispatchReceipt> commit_mesh_cleanup(
        ApplicationSession& session, MeshCleanupReview& review, std::string justification);
    [[nodiscard]] static core::Result<DispatchReceipt> dispatch(
        ApplicationSession& session,
        const ApplicationAction& action);
    [[nodiscard]] static core::Result<DispatchReceipt> commit_preview(
        ApplicationSession& session,
        editor::AuthoringPreview& preview);
    [[nodiscard]] static core::Result<DispatchReceipt> accept_agent_task(
        ::carto::agent::AgentTask& task);
    [[nodiscard]] static core::Result<void> answer_agent_clarification(
        ::carto::agent::AgentPlan& plan,
        std::string_view question_id,
        std::optional<std::string> candidate_id,
        std::optional<std::string> freeform_answer);

private:
    friend class ::carto::ui::UiController;
    friend class ::carto::agent::AgentTask;

    [[nodiscard]] static core::Result<void> correct_agent_ghost_transform(
        ::carto::agent::AgentTask& task,
        scene::ObjectId object,
        core::Transform transform,
        std::string justification);
    [[nodiscard]] static core::Result<void> correct_agent_ghost_vertex_position(
        ::carto::agent::AgentTask& task,
        scene::ObjectId object,
        geometry::VertexId vertex,
        core::Vec3d position,
        std::string justification);
    [[nodiscard]] static core::Result<void> correct_agent_ghost_face(
        ::carto::agent::AgentTask& task,
        scene::ObjectId object,
        geometry::FaceId face,
        ::carto::agent::GhostCorrectionKind kind,
        double distance,
        std::string justification);

    [[nodiscard]] static core::Result<DispatchReceipt> commit_preview(
        ApplicationSession& session,
        editor::AuthoringPreview& preview,
        OperationSource source,
        std::string_view provenance_id);
};

class ApplicationSession {
public:
    ApplicationSession();

    [[nodiscard]] static core::Result<RecoveryInspection> inspect_recovery(
        const std::filesystem::path& project_path);

    [[nodiscard]] core::Result<DispatchReceipt> dispatch(
        HumanActionAdmission admission,
        const ApplicationAction& action,
        OperationSource source = OperationSource::human,
        std::string_view provenance_id = {});
    [[nodiscard]] core::Result<editor::AuthoringContext> authoring_context() const;
    [[nodiscard]] core::Result<engineering::AuthoringDocument> engineering_authoring() const;
    [[nodiscard]] core::Result<condition::AuthoringDocument> condition_authoring() const;
    [[nodiscard]] core::Result<assets::SourceReimportPlan> preview_source_reimport(
        std::vector<assets::SourceImportRecord> incoming) const;
    // Builds a full-registry reimport plan that adds one new source record.
    // The current registry is preserved and the generation is assigned from
    // retained lineage; this method never mutates project state.
    [[nodiscard]] core::Result<assets::SourceReimportPlan> preview_source_asset_addition(
        assets::SourceImportRecord incoming) const;
    // Decodes only a verified project-local TGA reference source. The result is
    // bounded and digest-bound; unsupported containers and missing/stale bytes
    // fail without fallback or mutation.
    [[nodiscard]] core::Result<assets::DecodedRgba8Texture>
    decode_reference_image_tga(std::string_view identity) const;
    [[nodiscard]] core::Result<condition::EffectPreviewResult> condition_effect_preview(
        condition::Id effect_source_id,
        std::string_view operating_state,
        double elapsed_seconds) const;
    [[nodiscard]] core::Result<condition::VegetationPreviewResult>
    condition_vegetation_preview(condition::Id recipe_id) const;
    [[nodiscard]] core::Result<condition::StateComparisonResult>
    condition_state_comparison(condition::Id left_branch_id, condition::Id right_branch_id) const;
    [[nodiscard]] core::Result<SpatialFieldPreview> spatial_field_preview(
        std::string_view identity) const;
    [[nodiscard]] core::Result<spatial_field::VectorFieldPreview>
    spatial_vector_field_preview(
        std::string_view u_component_identity,
        std::string_view v_component_identity,
        std::uint32_t max_preview_axis =
            spatial_field::VectorFieldPreview::kMaxPreviewAxis) const;
    [[nodiscard]] core::Result<terrain::TerrainTileProduct> terrain_tile_preview(
        std::string_view identity,
        std::uint32_t lod_step = 1U,
        double max_navigation_slope_degrees = 35.0,
        std::optional<double> water_level = std::nullopt) const;
    [[nodiscard]] core::Result<RoadSurfacePreview> road_surface_preview(
        std::string_view identity,
        std::uint32_t samples_per_segment = 32U) const;
    [[nodiscard]] core::Result<RoadTerrainPreview> road_terrain_preview(
        std::string_view road_identity,
        std::string_view terrain_identity,
        RoadTerrainCoordinateFrame coordinate_frame,
        std::uint32_t samples_per_segment = 32U) const;
    [[nodiscard]] core::Result<geometry::SweptMeshProduct> sweep_source_preview(
        std::string_view identity) const;
    [[nodiscard]] core::Result<production::DecalProjection> decal_placement_preview(
        const production::DecalPlacement& placement,
        core::Revision expected_project_revision) const;
    [[nodiscard]] core::Result<geometry::CompiledMesh> decal_placement_source_mesh(
        const production::DecalPlacement& placement,
        core::Revision expected_project_revision) const;
    [[nodiscard]] core::Result<production::DecalPlacement> make_decal_placement(
        scene::ObjectId target_object,
        std::uint64_t material_id,
        core::Vec3d center,
        core::Vec3d normal,
        core::Vec3d tangent,
        double width,
        double height,
        double depth,
        double minimum_normal_dot,
        core::Revision expected_project_revision) const;
    [[nodiscard]] core::Result<biological::BiologicalAuthoringDocument>
    biological_authoring_source(
        const project::BiologicalAuthoringKey& key,
        core::Revision expected_project_revision) const;
    [[nodiscard]] core::Result<biological::ProxyMeshProduct> biological_proxy_preview(
        const project::BiologicalAuthoringKey& key,
        core::Revision expected_project_revision,
        biological::StructureId structure) const;
    [[nodiscard]] core::Result<geometry::CurvePath> curve_path_source(
        std::string_view identity) const;
    [[nodiscard]] core::Result<core::Revision> next_curve_path_revision(
        std::string_view identity) const;
    [[nodiscard]] core::Result<core::Revision> next_road_source_revision(
        std::string_view identity) const;
    [[nodiscard]] core::Result<core::Revision> next_sweep_source_revision(
        std::string_view identity) const;
    [[nodiscard]] core::Result<core::Revision> next_biological_authoring_revision(
        const project::BiologicalAuthoringKey& key) const;
    [[nodiscard]] core::Result<core::Revision> next_sign_text_source_revision(
        std::string_view identity) const;
    [[nodiscard]] core::Result<project::MaterialGraph> material_graph_source(
        std::string_view identity) const;
    [[nodiscard]] core::Result<editor::AuthoringPreview> begin_preview(
        editor::PreviewKind kind) const;
    [[nodiscard]] core::Result<MeshCleanupReview> review_mesh_cleanup(
        scene::ObjectId object, std::vector<geometry::VertexId> candidates,
        MeshCleanupOptions options) const;
    [[nodiscard]] core::Result<DispatchReceipt> commit_preview(
        HumanActionAdmission admission,
        editor::AuthoringPreview& preview,
        OperationSource source = OperationSource::human,
        std::string_view provenance_id = {});
    [[nodiscard]] ApplicationSnapshot snapshot() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> native_structure_signature(
        scene::ObjectId object,
        std::variant<std::monostate, geometry::FaceId, geometry::VertexId> component) const;
    [[nodiscard]] core::Result<std::vector<NativeStructureMatch>> find_native_structure_matches(
        NativeTraceTargetKind kind,
        const assets::Sha256Digest& signature,
        std::optional<scene::ObjectId> excluded_object = std::nullopt) const;
    // Replays a face-topology chain on a private copy of the object's mesh to
    // resolve every step's face and absolute distance, verifying each step's
    // recorded pre-command face signature and post-command object signature.
    // Native face IDs are allocated deterministically per mesh, so the
    // resolved faces are the ones a speculative run of the same chain edits.
    // Read-only: the project document is never modified.
    [[nodiscard]] core::Result<std::vector<NativeFaceChainResolution>> resolve_native_face_chain(
        scene::ObjectId object,
        geometry::FaceId first_face,
        std::span<const NativeFaceChainStep> steps) const;

    [[nodiscard]] const WorkspaceState& workspace() const noexcept { return workspace_; }
    [[nodiscard]] core::Result<void> can_close(bool discard_dirty = false) const;

private:
    friend class ::carto::agent::AgentTask;
    friend class ::carto::agent::SceneQuery;
    friend class HumanApplicationAccess;
    [[nodiscard]] core::Result<DispatchReceipt> commit_mesh_cleanup(
        HumanActionAdmission admission, MeshCleanupReview& review, std::string justification);
    std::shared_ptr<const std::uint8_t> cleanup_review_identity_ =
        std::make_shared<const std::uint8_t>(0U);

    [[nodiscard]] core::Result<void> set_workspace(WorkspaceState workspace);
    [[nodiscard]] core::Result<DispatchReceipt> new_project(const NewProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> open_project(const OpenProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> recover_project(
        const RecoverProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> save_project(const SaveProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> execute_command(
        std::unique_ptr<editor::EditorCommand> command,
        std::string action,
        AffectedSet affected = {},
        std::optional<ParameterPayload> parameters = std::nullopt);
    [[nodiscard]] core::Result<DispatchReceipt> accept_command_mutation(
        std::string action,
        core::Revision revision_before,
        project::ProjectDocument before_document,
        AffectedSet affected = {},
        std::optional<ParameterPayload> parameters = std::nullopt);
    [[nodiscard]] core::Result<DispatchReceipt> select_object(const SelectObjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_vertex(const SelectVertexAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_face(const SelectFaceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_edge(const SelectEdgeAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> convert_selection(
        const ConvertSelectionAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> expand_selection(
        const ExpandSelectionAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_shortest_path(
        const SelectShortestPathAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> invoke_tool(const InvokeToolAction& action);
    [[nodiscard]] core::Result<void> rebind_last_tool_selection();
    [[nodiscard]] core::Result<DispatchReceipt> repeat_last_tool();
    [[nodiscard]] core::Result<DispatchReceipt> adjust_last_tool(
        const AdjustLastToolAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_box(const CreateBoxAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_plane(const CreatePlaneAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_mesh_object(
        const CreateMeshObjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> append_sculpt_stroke(
        const AppendSculptStrokeAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> append_sculpt_surface_stroke(
        const AppendSculptSurfaceStrokeAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> commit_sculpt_stroke(
        std::uint64_t mesh_asset,
        scene::ObjectId object,
        production::SculptStroke stroke,
        std::string action_name,
        AffectedSet affected,
        std::optional<ParameterPayload> parameters = std::nullopt);
    [[nodiscard]] core::Result<DispatchReceipt> set_sculpt_layer_visibility(
        const SetSculptLayerVisibilityAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> duplicate_mesh_instance(
        const DuplicateMeshInstanceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> duplicate_mesh_instance_batch(
        const DuplicateMeshInstanceBatchAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_plant_scatter_batch(
        const CreatePlantScatterBatchAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> import_engineering_fixture();
    [[nodiscard]] core::Result<DispatchReceipt> import_engineering_authoring(
        const ImportEngineeringAuthoringAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> import_engineering_bom_csv(
        const ImportEngineeringBomCsvAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> import_engineering_requirement_csv(
        const ImportEngineeringRequirementCsvAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> attach_engineering_evidence(
        const AttachEngineeringEvidenceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_engineering_requirement(
        const CreateEngineeringRequirementAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> review_engineering_requirement(
        const ReviewEngineeringRequirementAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_engineering_characteristic(
        const CreateEngineeringCharacteristicAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_engineering_target(
        const CreateEngineeringTargetAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> import_engineering_path_result(
        const ImportEngineeringPathResultAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> update_engineering_workcell(
        const UpdateEngineeringWorkcellAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_engineering_keep_out(
        const CreateEngineeringKeepOutAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_engineering_workcell_frame(
        const CreateEngineeringWorkcellFrameAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> review_engineering_target(
        const ReviewEngineeringTargetAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> invalidate_engineering_source(
        const InvalidateEngineeringSourceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> import_condition_fixture();
    [[nodiscard]] core::Result<DispatchReceipt> import_condition_authoring(
        const ImportConditionAuthoringAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> bind_condition_asset_object(
        const BindConditionAssetObjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> add_condition_interaction_anchor(
        const AddConditionInteractionAnchorAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> add_condition_prefab(
        const AddConditionPrefabAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> instantiate_condition_prefab(
        const InstantiateConditionPrefabAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> add_condition_collection(
        const AddConditionCollectionAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> add_condition_region_kit(
        const AddConditionRegionKitAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> add_condition_kinematic_assembly(
        const AddConditionKinematicAssemblyAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> replace_condition_authored_record(
        const ReplaceConditionAuthoredRecordAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> delete_condition_authored_record(
        const DeleteConditionAuthoredRecordAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> invalidate_condition_source(
        const InvalidateConditionSourceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> apply_source_reimport_plan(
        const ApplySourceReimportPlanAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> relink_moved_source(
        const RelinkMovedSourceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_spatial_field(
        const CreateSpatialFieldAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> paint_spatial_field(
        const PaintSpatialFieldAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_material(
        const CreateMaterialAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> update_material(
        const UpdateMaterialAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_material_region_assignment(
        const SetMaterialRegionAssignmentAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> generate_unique_lightmap_uv(
        const GenerateUniqueLightmapUvAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_material_graph(
        const SetMaterialGraphAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_material_graph_binding(
        const SetMaterialGraphBindingAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> import_material_texture_source(
        const ImportMaterialTextureSourceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_presentation_catalog(
        const SetPresentationCatalogAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_project_authoring_standards(
        const SetProjectAuthoringStandardsAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> commit_material_catalog(
        project::MaterialCatalog catalog,
        core::Revision expected_project_revision,
        std::string action);
    [[nodiscard]] core::Result<DispatchReceipt> set_road_authoring(
        const SetRoadAuthoringAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_road_surface_instance(
        const CreateRoadSurfaceInstanceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_sweep_source(
        const SetSweepSourceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_biological_authoring(
        const SetBiologicalAuthoringAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_sign_text_source(
        const SetSignTextSourceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> set_decal_placement(
        const SetDecalPlacementAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_sign_text_object(
        const CreateSignTextObjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_biological_proxy_instance(
        const CreateBiologicalProxyInstanceAction& action);
    [[nodiscard]] core::Result<ApplicationAction> preview_action(
        const editor::AuthoringPreview& preview) const;
    [[nodiscard]] core::Result<DispatchReceipt> undo();
    [[nodiscard]] core::Result<DispatchReceipt> redo();
    [[nodiscard]] core::Result<void> append_mutation_event(
        std::string_view action,
        core::Revision revision_before,
        core::Revision revision_after);
    [[nodiscard]] core::Result<DispatchReceipt> accept_speculative_result(
        const project::ProjectDocument& candidate,
        core::Revision expected_revision,
        std::string_view task_id,
        std::string_view objective_digest,
        std::string_view diff_digest);
    [[nodiscard]] core::Result<DispatchReceipt> install_agent_rebase_branch(
        const project::ProjectDocument& candidate,
        core::Revision expected_revision,
        std::string_view checkpoint_task_id,
        std::string_view plan_context_digest,
        std::string_view checkpoint_source_digest);
    [[nodiscard]] core::Result<project::ProjectDocument> merge_agent_rebase_document(
        const project::ProjectDocument& base,
        const project::ProjectDocument& speculative,
        const std::vector<scene::ObjectId>& read_objects,
        const std::vector<std::uint64_t>& read_mesh_assets,
        const std::vector<scene::ObjectId>& write_objects,
        const std::vector<std::uint64_t>& write_mesh_assets,
        const std::vector<project::MaterialRegionKey>& read_material_regions = {},
        const std::vector<project::MaterialRegionKey>& write_material_regions = {}) const;
    [[nodiscard]] core::Result<project::ProjectDocument> merge_agent_rebase_step_document(
        const project::ProjectDocument& base,
        const project::ProjectDocument& speculative,
        const std::vector<scene::ObjectId>& write_objects,
        const std::vector<std::uint64_t>& write_mesh_assets,
        std::uint64_t revision_delta) const;
    [[nodiscard]] core::Result<void> install_agent_checkpoint_source(
        project::ProjectDocument checkpoint_source);
    struct SpeculativeCopyTag final {};
    ApplicationSession(const ApplicationSession& source, SpeculativeCopyTag);
    struct PreparedJournal {
        journal::Journal journal;
        bool created_file = false;
        bool appended_baseline = false;
        std::uintmax_t original_bytes = 0U;
    };
    [[nodiscard]] core::Result<PreparedJournal> prepare_journal(
        const std::filesystem::path& project_path,
        const project::ProjectDocument& document) const;
    [[nodiscard]] core::Result<void> require_discard_confirmation(bool discard_dirty) const;

    [[nodiscard]] core::Result<DispatchReceipt> accepted(
        std::string action,
        core::Revision revision_before,
        AffectedSet affected = {},
        std::optional<ParameterPayload> parameters = std::nullopt);
    void append_native_operation_trace(NativeOperationTraceEntry entry);
    [[nodiscard]] core::Result<DispatchReceipt> failure(core::Diagnostic diagnostic);
    void reset_editor_state() noexcept;

    struct ViewportMeshCacheEntry {
        std::uint64_t project_generation = 0U;
        const geometry::EditableMesh* source = nullptr;
        core::Revision source_revision;
        std::optional<assets::Sha256Digest> sculpt_stack_digest;
        std::shared_ptr<const geometry::CompiledMesh> mesh;
        std::shared_ptr<const ViewportState::MeshAttributeOverlayData> attribute_overlay;
        std::optional<core::Diagnostic> attribute_overlay_error;
    };

    project::ProjectDocument document_;
    std::optional<std::filesystem::path> project_path_;
    core::Revision saved_revision_;
    std::uint64_t project_generation_ = 0;
    // snapshot() is a UI-thread read boundary. The cache is mutable so a
    // read-only snapshot can retain derived geometry without changing the
    // authoritative project document.
    mutable std::map<std::uint64_t, ViewportMeshCacheEntry> viewport_mesh_cache_;
    editor::SelectionState selection_;
    editor::CommandBus history_;
    editor::ToolRegistry tools_;
    std::optional<journal::Journal> journal_;
    WorkspaceState workspace_;
    std::vector<core::Diagnostic> problems_;
    std::vector<NativeOperationTraceEntry> native_operation_trace_;
    std::optional<InvokeToolAction> last_tool_action_;
    std::optional<editor::AuthoringContext> last_tool_context_;
    OperationId next_operation_id_ = 1U;
    OperationSource active_source_ = OperationSource::human;
    std::string active_provenance_id_;
};

} // namespace carto::application
