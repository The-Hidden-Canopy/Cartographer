#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::condition {

using Id = std::uint64_t;

enum class SemanticMembershipKind : std::uint8_t {
    collection,
    prefab,
    region_kit,
};

enum class SourceKind : std::uint8_t {
    authored_reference,
    imported_asset,
    procedural_recipe,
    material_reference,
    authored_fixture,
};

enum class AssetClass : std::uint8_t {
    architecture,
    prop,
    vehicle,
    vegetation,
    terrain,
    water,
    fixture,
    character,
    wildlife,
    material,
};

enum class FidelityClass : std::uint8_t {
    hero,
    familiarity_critical,
    standard,
    background,
};

enum class VariantKind : std::uint8_t { morph, presentation };

enum class TemporalPhase : std::uint8_t {
    day,
    late_day,
    dusk,
    blue_hour,
    night,
    pre_dawn,
    dawn,
};

enum class OccupancyState : std::uint8_t {
    empty,
    recently_used,
    actively_occupied,
    long_term_occupied,
    abandoned,
};

enum class MaintenanceState : std::uint8_t {
    maintained,
    aging,
    neglected,
    improvised_repair,
    partially_renovated,
    condemned,
};

enum class DamageState : std::uint8_t {
    intact,
    broken_window,
    damaged_door,
    dented_sign,
    collapsed_shelf,
    cracked_wall,
    damaged_railing,
    roof_damage,
    collision_damage,
    burned,
};

enum class AnchorKind : std::uint8_t {
    interaction,
    approach,
    seat,
    sleep,
    traversal,
    audio_source,
};

enum class AudioBusClass : std::uint8_t {
    ambient,
    machinery,
    vehicle,
    radio,
    speech,
    music,
    effects,
};

enum class AudioOcclusionIntent : std::uint8_t {
    none,
    static_geometry,
    authored_geometry,
};

enum class AcousticZoneShape : std::uint8_t {
    box,
    sphere,
};

enum class EffectEmissionShape : std::uint8_t {
    point,
    box,
    sphere,
};

enum class EffectIntentClass : std::uint8_t {
    physical_state,
    presentation_only,
};

enum class EffectInfluenceKind : std::uint8_t {
    wind,
    thermal_lift,
    drainage,
    avoidance,
    turbulence,
};

enum class StateTargetKind : std::uint8_t { safe_place_profile, surface_state };

enum class StateProperty : std::uint8_t {
    privacy,
    human_proximity,
    controllable_entries,
    supply_access,
    ambient_noise,
    visibility_outward,
    visibility_inward,
    isolation,
    occupancy,
    maintenance,
    damage,
    wetness,
};

enum class PackageAdmissionStatus : std::uint8_t { admit, refuse };

[[nodiscard]] std::string_view source_kind_name(SourceKind value) noexcept;
[[nodiscard]] std::string_view asset_class_name(AssetClass value) noexcept;
[[nodiscard]] std::string_view fidelity_class_name(FidelityClass value) noexcept;
[[nodiscard]] std::string_view variant_kind_name(VariantKind value) noexcept;
[[nodiscard]] std::string_view temporal_phase_name(TemporalPhase value) noexcept;
[[nodiscard]] std::string_view occupancy_state_name(OccupancyState value) noexcept;
[[nodiscard]] std::string_view maintenance_state_name(MaintenanceState value) noexcept;
[[nodiscard]] std::string_view damage_state_name(DamageState value) noexcept;
[[nodiscard]] std::string_view anchor_kind_name(AnchorKind value) noexcept;
[[nodiscard]] std::string_view audio_bus_class_name(AudioBusClass value) noexcept;
[[nodiscard]] std::string_view audio_occlusion_intent_name(AudioOcclusionIntent value) noexcept;
[[nodiscard]] std::string_view acoustic_zone_shape_name(AcousticZoneShape value) noexcept;
[[nodiscard]] std::string_view effect_emission_shape_name(EffectEmissionShape value) noexcept;
[[nodiscard]] std::string_view effect_intent_class_name(EffectIntentClass value) noexcept;
[[nodiscard]] std::string_view effect_influence_kind_name(EffectInfluenceKind value) noexcept;
[[nodiscard]] std::string_view state_target_kind_name(StateTargetKind value) noexcept;
[[nodiscard]] std::string_view state_property_name(StateProperty value) noexcept;

// A source reference is deliberately a logical locator, never an absolute
// machine path or a runtime handle. `current` is explicit so stale source
// evidence cannot silently be treated as live truth.
struct SourceEvidence {
    Id id = 0U;
    SourceKind kind = SourceKind::authored_reference;
    std::string label;
    std::string logical_locator;
    std::string revision;
    assets::Sha256Digest digest;
    std::string producer_identity;
    std::string producer_version;
    assets::Sha256Digest import_settings_digest;
    bool reviewed = false;
    bool current = true;
    std::vector<std::string> diagnostics;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SourceAsset {
    Id id = 0U;
    std::string stable_name;
    AssetClass asset_class = AssetClass::prop;
    FidelityClass fidelity = FidelityClass::standard;
    Id source_id = 0U;
    std::string source_revision;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest geometry_digest;
    assets::Sha256Digest material_digest;
    std::uint64_t authored_object_id = 0U;
    std::vector<std::string> tags;
    std::vector<std::string> protected_features;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SafePlaceProfile {
    Id id = 0U;
    Id asset_id = 0U;
    double privacy = 0.0;
    double human_proximity = 0.0;
    double controllable_entries = 0.0;
    double supply_access = 0.0;
    double ambient_noise = 0.0;
    double visibility_outward = 0.0;
    double visibility_inward = 0.0;
    double isolation = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FamiliarityRecord {
    Id id = 0U;
    Id asset_id = 0U;
    std::string semantic_id;
    double importance = 0.0;
    core::Transform baseline_transform = core::Transform::identity();
    std::vector<Id> expected_neighbors;
    std::string expected_state;
    bool may_change_physically = false;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ViewCondition {
    Id id = 0U;
    Id asset_id = 0U;
    double minimum_distance = 0.0;
    double maximum_distance = 0.0;
    double minimum_elevation_degrees = -90.0;
    double maximum_elevation_degrees = 90.0;
    double cone_degrees = 180.0;
    double minimum_visibility_fraction = 0.0;
    bool requires_occlusion = false;
    std::string lighting_condition;

    [[nodiscard]] core::Result<void> validate() const;
};

struct VariantCandidate {
    Id id = 0U;
    Id asset_id = 0U;
    std::string name;
    VariantKind kind = VariantKind::morph;
    core::Revision base_revision;
    double maximum_transform_delta = 0.0;
    bool collision_impact_declared = false;
    bool navigation_impact_declared = false;
    bool interaction_impact_declared = false;
    bool reversible = true;
    bool projection_only = false;
    std::string semantic_id;

    [[nodiscard]] core::Result<void> validate() const;
};

struct TemporalVisualPackage {
    Id id = 0U;
    Id asset_id = 0U;
    TemporalPhase phase = TemporalPhase::day;
    std::vector<Id> variant_ids;
    std::string material_variant;
    std::string source_alternate;
    bool visible = true;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SurfaceStateRecord {
    Id id = 0U;
    Id asset_id = 0U;
    OccupancyState occupancy = OccupancyState::empty;
    MaintenanceState maintenance = MaintenanceState::maintained;
    DamageState damage = DamageState::intact;
    double wetness = 0.0;
    std::vector<std::string> wear_tags;
    std::vector<std::string> repair_tags;
    std::string provenance_note;

    [[nodiscard]] core::Result<void> validate() const;
};

struct InteractionAnchor {
    Id id = 0U;
    Id asset_id = 0U;
    AnchorKind kind = AnchorKind::interaction;
    std::string interaction_class;
    core::Transform transform = core::Transform::identity();
    core::Vec3d approach_direction{0.0, 0.0, 1.0};
    core::Vec3d clearance{1.0, 1.0, 1.0};
    bool enabled = true;

    [[nodiscard]] core::Result<void> validate() const;
};

// Durable, Cartographer-owned articulation source for a CONDITION asset.
// Graph and travel limits are canonical provider-neutral records; current pose,
// velocity, animation state, and runtime handles are deliberately absent.
struct KinematicAssemblySource {
    Id id = 0U;
    Id asset_id = 0U;
    std::string source_identity;
    core::Revision source_revision;
    std::string graph_bytes;
    std::string joint_limit_bytes;
    struct PartBinding {
        std::uint64_t joint_id = 0U;
        std::uint64_t scene_object_id = 0U;
        std::uint64_t mesh_asset_id = 0U;
        core::Revision mesh_revision;

        [[nodiscard]] constexpr auto operator<=>(const PartBinding&) const noexcept = default;
    };
    std::vector<PartBinding> part_bindings;

    [[nodiscard]] core::Result<void> validate() const;
};

// Authored audio intent references reviewed source evidence. It carries no
// decoded samples, mixer handle, playback state, or runtime authority.
struct AuthoredAudioSource {
    Id id = 0U;
    Id asset_id = 0U;
    Id clip_source_id = 0U;
    core::Transform local_transform = core::Transform::identity();
    double gain = 1.0;
    double pitch = 1.0;
    bool loop = false;
    bool spatial = true;
    double minimum_distance = 1.0;
    double maximum_distance = 10.0;
    std::uint16_t priority = 128U;
    AudioBusClass bus = AudioBusClass::ambient;
    AudioOcclusionIntent occlusion = AudioOcclusionIntent::static_geometry;
    double reverb_send = 0.0;
    std::string normal_operating_state = "active";
    std::string familiarity_role;
    bool enabled = true;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AuthoredAcousticZone {
    Id id = 0U;
    Id asset_id = 0U;
    AcousticZoneShape shape = AcousticZoneShape::box;
    core::Transform local_transform = core::Transform::identity();
    core::Vec3d half_extents{1.0, 1.0, 1.0};
    double radius = 0.0;
    std::string acoustic_class;
    double gain_modifier = 1.0;
    std::string reverb_intent;
    std::uint16_t priority = 0U;
    std::string semantic_region;
    bool enabled = true;

    [[nodiscard]] core::Result<void> validate() const;
};

struct EffectSource {
    Id id = 0U;
    Id asset_id = 0U;
    std::string semantic_owner;
    EffectEmissionShape shape = EffectEmissionShape::point;
    core::Transform local_transform = core::Transform::identity();
    core::Vec3d half_extents{};
    double radius = 0.0;
    EffectIntentClass intent_class = EffectIntentClass::physical_state;
    std::string normal_operating_state = "active";
    std::string disabled_operating_state = "disabled";
    bool enabled = true;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ParticleEmitterSource {
    Id id = 0U;
    Id effect_source_id = 0U;
    double emission_rate_per_second = 1.0;
    double particle_lifetime_seconds = 1.0;
    double start_seconds = 0.0;
    double duration_seconds = 1.0;
    std::uint32_t burst_count = 0U;
    double burst_time_seconds = 0.0;
    core::Vec3d initial_velocity{};
    core::Vec3d velocity_spread{};
    core::Vec3d acceleration{};
    double fixed_timestep_seconds = 1.0 / 60.0;
    std::uint32_t total_particle_bound = 256U;
    std::uint64_t deterministic_seed = 1U;
    bool enabled = true;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldInfluenceBinding {
    Id id = 0U;
    Id effect_source_id = 0U;
    EffectInfluenceKind kind = EffectInfluenceKind::wind;
    std::string field_identity;
    core::Revision expected_field_revision;
    assets::Sha256Digest expected_field_digest;
    core::Vec3d direction{1.0, 0.0, 0.0};
    double strength = 1.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldSourceBinding {
    std::string field_identity;
    core::Revision expected_field_revision;
    assets::Sha256Digest expected_field_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PlantSpeciesSourceBinding {
    std::string source_identity;
    core::Revision expected_source_revision;
    assets::Sha256Digest expected_source_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// Persisted source recipe for deterministic, field-driven plant placement.
// Generated instances remain a derived preview and are not scene authority.
struct VegetationScatterRecipe {
    Id id = 0U;
    Id species_asset_id = 0U;
    FieldSourceBinding moisture;
    FieldSourceBinding shade;
    FieldSourceBinding disturbance;
    FieldSourceBinding suitability;
    FieldSourceBinding elevation;
    std::optional<PlantSpeciesSourceBinding> plant_source;
    double cell_spacing = 2.0;
    double density_per_square_meter = 0.05;
    double minimum_suitability = 0.2;
    double minimum_scale = 0.8;
    double maximum_scale = 1.2;
    std::uint32_t maximum_instances = 2048U;
    std::uint64_t deterministic_seed = 1U;
    bool enabled = true;

    [[nodiscard]] core::Result<void> validate() const;
};

struct StateValue {
    double numeric_value = 0.0;
    std::string text_value;
};

struct AuthoredStateDelta {
    Id id = 0U;
    Id target_record_id = 0U;
    Id target_asset_id = 0U;
    StateTargetKind target_kind = StateTargetKind::safe_place_profile;
    StateProperty property = StateProperty::privacy;
    StateValue before;
    StateValue after;

    [[nodiscard]] core::Result<void> validate() const;
};

// A branch stores explicit before/after source-state deltas. Branch id 0 is
// the immutable baseline and is never serialized as an authored branch.
struct AuthoredStateBranch {
    Id id = 0U;
    std::string name;
    Id parent_branch_id = 0U;
    std::string applicability;
    std::vector<AuthoredStateDelta> deltas;

    [[nodiscard]] core::Result<void> validate() const;
};

struct CollectionRecord {
    Id id = 0U;
    std::string name;
    std::vector<Id> asset_ids;
    std::vector<Id> child_collection_ids;
    std::string semantic_class;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PrefabRecord {
    Id id = 0U;
    std::string name;
    std::vector<Id> asset_ids;
    std::vector<std::string> sockets;
    std::vector<Id> variant_ids;
    core::Revision source_revision;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PrefabInstanceMember {
    Id asset_id = 0U;
    std::uint64_t source_object_id = 0U;
    std::uint64_t instance_object_id = 0U;
    std::uint64_t mesh_asset_id = 0U;
    core::Revision mesh_revision;
    std::string source_revision;
    assets::Sha256Digest source_digest;
    core::Transform source_world_transform = core::Transform::identity();

    [[nodiscard]] core::Result<void> validate() const;
};

// Scene instances retain exact source and mesh bindings so a runtime consumer
// can distinguish a current prefab placement from a stale authored derivative.
struct PrefabInstanceRecord {
    Id id = 0U;
    Id prefab_id = 0U;
    std::string name;
    std::uint64_t root_object_id = 0U;
    core::Revision captured_project_revision;
    core::Revision source_authoring_revision;
    core::Revision prefab_source_revision;
    std::vector<PrefabInstanceMember> members;

    [[nodiscard]] core::Result<void> validate() const;
};

struct RegionKitManifest {
    Id id = 0U;
    std::string name;
    std::vector<Id> asset_ids;
    std::vector<Id> prefab_ids;
    std::vector<AssetClass> required_classes;
    double interaction_coverage = 0.0;
    double familiarity_coverage = 0.0;
    bool collision_validation_required = true;
    bool semantic_validation_required = true;

    [[nodiscard]] core::Result<void> validate() const;
};

class AuthoringDocument final {
public:
    static constexpr std::uint32_t kSchemaVersion = 9U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecordsPerKind = 65'536U;
    static constexpr std::size_t kMaxAudioRecordsPerKind = 8'192U;
    static constexpr std::size_t kMaxEffectRecordsPerKind = 8'192U;
    static constexpr std::size_t kMaxVegetationRecipes = 1'024U;
    static constexpr std::size_t kMaxStateBranches = 1'024U;
    static constexpr std::size_t kMaxStateDeltasPerBranch = 4'096U;
    static constexpr std::size_t kMaxKinematicAssemblies = 4'096U;
    static constexpr std::size_t kMaxKinematicPartBindings = 4'096U;
    static constexpr std::size_t kMaxPrefabInstanceMembers = 4'096U;
    static constexpr std::size_t kMaxKinematicPayloadBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxAggregateKinematicPayloadBytes = 8U * 1024U * 1024U;

    std::string project_namespace;
    core::Revision project_revision;
    std::vector<SourceEvidence> sources;
    std::vector<SourceAsset> assets;
    std::vector<SafePlaceProfile> safe_places;
    std::vector<FamiliarityRecord> familiarity;
    std::vector<ViewCondition> view_conditions;
    std::vector<VariantCandidate> variants;
    std::vector<TemporalVisualPackage> temporal_packages;
    std::vector<SurfaceStateRecord> surface_states;
    std::vector<InteractionAnchor> interaction_anchors;
    std::vector<CollectionRecord> collections;
    std::vector<PrefabRecord> prefabs;
    std::vector<PrefabInstanceRecord> prefab_instances;
    std::vector<RegionKitManifest> region_kits;
    std::vector<AuthoredAudioSource> audio_sources;
    std::vector<AuthoredAcousticZone> acoustic_zones;
    std::vector<EffectSource> effect_sources;
    std::vector<ParticleEmitterSource> particle_emitters;
    std::vector<FieldInfluenceBinding> field_influences;
    std::vector<VegetationScatterRecipe> vegetation_scatters;
    std::vector<AuthoredStateBranch> state_branches;
    std::vector<KinematicAssemblySource> kinematic_assemblies;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_for_publication() const;
    [[nodiscard]] bool has_stale_sources() const noexcept;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<AuthoringDocument> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] core::Result<void> invalidate_source(
        Id source_id, std::string new_revision, assets::Sha256Digest new_digest);
};

struct Package final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 48U * 1024U * 1024U;

    std::string project_namespace;
    core::Revision project_revision;
    assets::Sha256Digest authoring_digest;
    std::string authoring_bytes;
    std::string publication_profile = "condition-source-authoring-v1";
    assets::Sha256Digest publication_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<Package> deserialize(std::string_view text);
    [[nodiscard]] static core::Result<Package> publish(
        const AuthoringDocument& authoring,
        core::Revision project_revision,
        std::string project_namespace);
};

struct AdmissionResult {
    PackageAdmissionStatus status = PackageAdmissionStatus::refuse;
    assets::Sha256Digest admitted_publication_digest;
    assets::Sha256Digest admitted_authoring_digest;
    std::vector<std::string> reasons;

    [[nodiscard]] core::Result<void> validate() const;
};

[[nodiscard]] core::Result<AdmissionResult> admit_for_consumer(const Package& package);
[[nodiscard]] core::Result<AuthoringDocument> make_condition_fixture();

} // namespace carto::condition
