#pragma once

#include <carto/ai/ai.hpp>
#include <carto/application/application.hpp>
#include <carto/agent/scene_query_budget.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/connection/graph.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/project/project.hpp>
#include <carto/project/authoring_standards.hpp>
#include <carto/scene/scene.hpp>
#include <carto/scientific/model.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace carto::agent {

using Effect = ai::CapabilityEffect;

[[nodiscard]] constexpr std::uint32_t effect_mask(Effect effect) noexcept {
    return ai::effect_mask(effect);
}

struct TargetRef {
    scene::ObjectId object;
    std::variant<geometry::FaceId, geometry::VertexId, std::monostate> component;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class ObjectiveConstraintKind : std::uint8_t {
    preserve_topology,
    preserve_transform,
    maximum_vertex_displacement,
};

struct HardConstraint {
    std::string id;
    ObjectiveConstraintKind kind = ObjectiveConstraintKind::preserve_topology;
    std::optional<scene::ObjectId> object;
    double maximum_displacement = 0.0;
    std::string rationale;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ParsedAgentGoal {
    std::optional<std::string> exact_object_name;
    std::optional<std::string> condition_tag_scope;
    std::string commands;
    struct NearestObjectScope {
        std::string anchor_name;
        double radius_world_units = 0.0;
    };
    std::optional<NearestObjectScope> nearest_object_scope;
};

// Parses an exact-name scope prefix, the explicit nearest-object scope
// `on nearest object within <radius> world units of "Anchor Name":`, or the
// exact CONDITION tag scope `on CONDITION tag "tag-name":`.
// The returned commands preserve the goal text and never infer a target.
[[nodiscard]] core::Result<ParsedAgentGoal> parse_agent_goal(std::string_view goal);

// Extracts only deterministic topology, world-transform, and numeric vertex-
// displacement constraints from a goal. Unsupported and conflicting restrictions
// fail closed instead of being ignored.
[[nodiscard]] core::Result<std::vector<HardConstraint>>
compile_explicit_hard_constraints(std::string_view goal);

enum class ClarificationImpact : std::uint8_t {
    object_scope,
    topology_deletion,
    familiarity_critical_feature,
    collision_or_navigation,
    interaction_anchor,
    source_hierarchy,
    large_spatial_transform,
    imported_source_integrity,
    publication_readiness,
};

// A plan may carry an answer only after the explicit human application
// boundary issued a receipt for this exact objective question and answer.
class HumanClarificationReceipt final {
public:
    HumanClarificationReceipt(const HumanClarificationReceipt&) = default;
    HumanClarificationReceipt& operator=(const HumanClarificationReceipt&) = default;
    HumanClarificationReceipt(HumanClarificationReceipt&&) noexcept = default;
    HumanClarificationReceipt& operator=(HumanClarificationReceipt&&) noexcept = default;

private:
    HumanClarificationReceipt(
        std::string question_id,
        assets::Sha256Digest context_digest,
        assets::Sha256Digest answer_digest,
        assets::Sha256Digest plan_context_digest)
        : question_id_(std::move(question_id)),
          context_digest_(context_digest),
          answer_digest_(answer_digest),
          plan_context_digest_(plan_context_digest) {}

    std::string question_id_;
    assets::Sha256Digest context_digest_;
    assets::Sha256Digest answer_digest_;
    assets::Sha256Digest plan_context_digest_;

    friend class ::carto::application::HumanApplicationAccess;
    friend struct ClarificationQuestion;
    friend struct AuthoringObjective;
    friend struct AgentPlan;
};

struct ClarificationCandidate {
    std::string id;
    std::string label;
    std::vector<scene::ObjectId> affected_objects;
    // A read-only identity for human disambiguation. This never adds the
    // referenced object to objective or work-envelope scope.
    std::optional<scene::ObjectId> target_object = std::nullopt;
};

struct ClarificationQuestion {
    std::string id;
    std::string prompt;
    std::string rationale;
    std::vector<ClarificationImpact> impacts;
    std::vector<scene::ObjectId> affected_objects;
    std::vector<std::string> affected_constraint_ids;
    std::size_t downstream_plan_branches = 0U;
    std::vector<ClarificationCandidate> candidates;
    std::optional<std::string> selected_candidate_id;
    std::optional<std::string> freeform_answer;
    std::optional<HumanClarificationReceipt> human_receipt;
    bool requires_explicit_answer = false;

    [[nodiscard]] core::Result<void> validate(
        const std::vector<scene::ObjectId>& scope,
        const std::vector<HardConstraint>& constraints) const;
};

struct RankedClarification {
    ClarificationQuestion question;
    std::uint32_t impact_score = 0U;
};

struct ClarificationFrontier {
    std::uint32_t threshold = 40U;
    std::vector<RankedClarification> questions;

    [[nodiscard]] bool requires_user_input() const noexcept;
};

struct AgentRecipeDefinition {
    std::string_view id;
    std::string_view label;
    std::string_view description;
    std::size_t max_topology_operations = 0U;
    std::uint32_t permitted_effects = 0U;
    bool preserve_topology = false;
    // Root-object transforms are refused by every recipe unless it permits
    // them explicitly; such a recipe's identity gate then tolerates transform
    // changes while still blocking add/remove, rename, hierarchy, binding,
    // visibility, and lock changes.
    bool permits_transform = false;
    // Requires every mesh to be byte-for-byte unchanged in vertex positions,
    // topology, attributes, and UV sets.
    bool preserve_mesh_content = false;
};

[[nodiscard]] const std::vector<AgentRecipeDefinition>& agent_recipe_catalog() noexcept;
[[nodiscard]] const AgentRecipeDefinition* find_agent_recipe(
    std::string_view id) noexcept;

struct AuthoringObjective {
    std::string id;
    std::string goal;
    core::Revision base_revision;
    std::vector<scene::ObjectId> scope_objects;
    std::vector<scene::ObjectId> protected_objects;
    std::vector<HardConstraint> hard_constraints;
    std::vector<ClarificationQuestion> clarifications;
    std::string recipe_id;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> digest() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> clarification_context_digest(
        std::string_view question_id) const;
    [[nodiscard]] core::Result<assets::Sha256Digest> clarification_answer_digest(
        std::string_view question_id) const;
    [[nodiscard]] core::Result<ClarificationFrontier> clarification_frontier(
        std::uint32_t threshold = 40U) const;
};

struct AgentPlanStep {
    std::string id;
    TargetRef target;
    std::string intent;
    std::vector<std::string> dependencies{};

    [[nodiscard]] core::Result<void> validate() const;
};

struct AgentPlan {
    std::string id;
    AuthoringObjective objective;
    std::vector<AgentPlanStep> steps;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<std::string>> schedule_order() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> context_digest() const;
};

struct AgentWorkEnvelope {
    std::vector<scene::ObjectId> scope_objects;
    std::uint32_t permitted_effects = ::carto::agent::effect_mask(Effect::geometry) |
        ::carto::agent::effect_mask(Effect::topology);
    std::size_t max_commands = 16U;
    std::size_t max_objects_touched = 16U;
    std::size_t max_topology_operations = 16384U;
    std::size_t max_scene_queries = 64U;
    std::size_t max_objects_examined = 4096U;
    std::size_t max_topology_elements_examined = 16384U;
    std::size_t max_semantic_records_examined = 8192U;
    std::size_t max_query_result_bytes = 1U * 1024U * 1024U;
    std::size_t max_repair_attempts = 2U;
    std::size_t max_additional_operations = 4U;
    std::size_t max_ghost_recomputations = 4U;
    double max_transform_distance_meters = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class StepStatus {
    pending,
    ready,
    running,
    passed,
    failed,
    invalidated,
};

struct PlanStepProgress {
    std::string step_id;
    std::vector<std::string> dependencies;
    StepStatus status = StepStatus::pending;
    std::optional<core::Diagnostic> diagnostic;
};

enum class AgentDemonstrationParameterization : std::uint8_t {
    normalized_face_distance,
    relative_displacement,
    parameterless,
    // Demonstration format v4: a material-region assignment expressed only
    // through record-local material, region, and optional variant aliases.
    material_region_assignment,
};

inline constexpr std::uint32_t kMaxDemonstrationMaterialAliases = 512U;

// Transferable typed values deliberately omit absolute object, vertex, and
// world-position data. Face distances are normalized by the source face's
// longest edge; vertex/object moves are stored as relative displacement;
// deterministic operations such as face poke carry no parameter values.
// Material assignments carry 1-based aliases that are meaningful only within
// one record; variant_alias 0 means "no variant".
struct AgentDemonstrationParameters {
    AgentDemonstrationParameterization parameterization =
        AgentDemonstrationParameterization::normalized_face_distance;
    double normalized_distance = 0.0;
    core::Vec3d relative_displacement;
    std::uint32_t material_alias = 0U;
    std::uint32_t region_alias = 0U;
    std::uint32_t variant_alias = 0U;

    [[nodiscard]] core::Result<void> validate(std::string_view tool_id) const;
};

struct StepReceipt {
    std::string step_id;
    std::string intent;
    std::string tool_id;
    scene::ObjectId target_object;
    core::Revision revision_before;
    core::Revision revision_after;
    StepStatus status = StepStatus::failed;
    std::optional<core::Diagnostic> diagnostic;
    // Derived from the validated native proposal and branch-before state.
    // Checkpoints rederive this when a plan is replayed.
    std::optional<AgentDemonstrationParameters> demonstration_parameters;
    // The exact region assignment a material step wrote on the speculative
    // branch. Demonstration capture replaces its identities with aliases.
    std::optional<project::MaterialRegionAssignment> material_assignment;
};

struct AcceptanceGateResult {
    std::string id;
    bool required = true;
    bool passed = false;
    std::string evidence;
    std::string validator_version;
    core::Revision source_revision;
    core::Revision evaluated_revision;
    std::vector<scene::ObjectId> affected_objects;
    std::vector<std::uint64_t> affected_asset_ids;
    assets::Sha256Digest result_digest;
    assets::Sha256Digest evidence_digest;
    std::optional<core::Diagnostic> diagnostic;
};

struct AgentRepairReceipt {
    std::size_t attempt = 0U;
    std::string previous_task_id;
    std::string repaired_task_id;
    std::string justification;
    std::size_t previous_plan_steps = 0U;
    std::size_t repaired_plan_steps = 0U;
    core::Revision source_revision;
    assets::Sha256Digest previous_plan_digest;
    assets::Sha256Digest repaired_plan_digest;
    assets::Sha256Digest previous_objective_digest;
    assets::Sha256Digest repaired_objective_digest;
    assets::Sha256Digest failure_evidence_digest;
    assets::Sha256Digest repair_evidence_digest;
    std::size_t previous_ghost_correction_count = 0U;
    assets::Sha256Digest previous_ghost_correction_digest;
    std::vector<StepReceipt> previous_step_receipts;
    std::vector<AcceptanceGateResult> previous_gate_results;
};

enum class GhostCorrectionKind : std::uint8_t {
    object_transform,
    vertex_position,
    face_inset,
    face_extrusion,
};

// Immutable evidence for an operator-authored edit on the speculative branch.
// The task may replay this correction during resume, rebase, or repair, but the
// receipt never grants authority to mutate the live source project.
struct GhostCorrectionReceipt {
    GhostCorrectionKind kind = GhostCorrectionKind::object_transform;
    std::string task_id;
    scene::ObjectId object;
    std::optional<geometry::VertexId> vertex;
    std::optional<geometry::FaceId> face;
    core::Transform before_transform = core::Transform::identity();
    core::Transform after_transform = core::Transform::identity();
    core::Vec3d before_position{};
    core::Vec3d after_position{};
    double distance = 0.0;
    assets::Sha256Digest before_mesh_digest;
    assets::Sha256Digest after_mesh_digest;
    core::Revision source_revision;
    core::Revision branch_revision_before;
    core::Revision branch_revision_after;
    assets::Sha256Digest plan_context_digest;
    assets::Sha256Digest parent_evidence_digest;
    // A bound empty value means the correction has no plan-step anchor and is
    // replayed after the plan. A missing value denotes a legacy receipt.
    std::optional<std::string> recompute_after_step_id;
    std::string justification;
    assets::Sha256Digest evidence_digest;

    [[nodiscard]] core::Result<assets::Sha256Digest> computed_evidence_digest() const;
    [[nodiscard]] core::Result<void> validate() const;
};

struct AgentRebaseReceipt {
    std::string checkpoint_task_id;
    assets::Sha256Digest checkpoint_plan_context_digest;
    core::Revision previous_source_revision;
    core::Revision rebased_source_revision;
    assets::Sha256Digest checkpoint_source_digest;
    assets::Sha256Digest rebased_source_digest;
    assets::Sha256Digest read_set_digest;
    assets::Sha256Digest write_set_digest;
    std::vector<scene::ObjectId> read_objects;
    std::vector<scene::ObjectId> write_objects;
    std::vector<std::uint64_t> read_mesh_assets;
    std::vector<std::uint64_t> write_mesh_assets;
    std::vector<project::MaterialRegionKey> read_material_regions;
    std::vector<project::MaterialRegionKey> write_material_regions;
    // Plan steps whose verified checkpoint results were reused verbatim on a
    // source revision that changed only outside their declared read/write set.
    std::vector<std::string> reused_step_ids;

    [[nodiscard]] core::Result<void> validate() const;
};

// Preserves the executed branch and DAG evidence surrounding an operator
// correction. The current task receipts describe the recomputed branch; this
// record keeps the superseded branch reviewable and binds the replay result.
struct AgentGhostRecomputeReceipt {
    std::string task_id;
    std::string anchor_step_id;
    assets::Sha256Digest correction_evidence_digest;
    std::vector<std::string> dependent_step_ids;
    std::vector<StepReceipt> previous_step_receipts;
    std::vector<AcceptanceGateResult> previous_gate_results;
    assets::Sha256Digest previous_diff_digest;
    assets::Sha256Digest result_diff_digest;
    core::Revision previous_speculative_revision;
    core::Revision result_speculative_revision;
    bool replay_completed = false;
    std::optional<core::Diagnostic> diagnostic;
    assets::Sha256Digest evidence_digest;

    [[nodiscard]] core::Result<assets::Sha256Digest> computed_evidence_digest() const;
    [[nodiscard]] core::Result<void> validate() const;
};

struct ObjectStateDetail {
    std::string name;
    core::Transform local_transform = core::Transform::identity();
    std::optional<scene::ObjectId> parent;
    std::optional<std::uint64_t> mesh_asset;
    bool visible = true;
    bool locked = false;
};

struct ObjectChange {
    scene::ObjectId id;
    bool added = false;
    bool removed = false;
    bool name_changed = false;
    bool transform_changed = false;
    bool parent_changed = false;
    bool mesh_binding_changed = false;
    bool mesh_content_changed = false;
    bool visibility_changed = false;
    bool lock_changed = false;
    std::optional<ObjectStateDetail> before_state;
    std::optional<ObjectStateDetail> after_state;
};

struct VertexValueChange {
    geometry::VertexId id;
    bool added = false;
    bool removed = false;
    bool position_changed = false;
    core::Vec3d before_position;
    core::Vec3d after_position;
};

struct FaceTopologyChange {
    geometry::FaceId id;
    bool added = false;
    bool removed = false;
    bool connectivity_changed = false;
    std::vector<geometry::VertexId> before_vertices;
    std::vector<geometry::VertexId> after_vertices;
};

struct AttributeLayerChange {
    std::string layer_id;
    attributes::AttributeDomain domain = attributes::AttributeDomain::vertex;
    attributes::AttributeType type = attributes::AttributeType::float32;
    std::optional<attributes::AttributeLayerDescriptor> before_descriptor;
    std::optional<attributes::AttributeLayerDescriptor> after_descriptor;
    bool added = false;
    bool removed = false;
    bool descriptor_changed = false;
    std::size_t before_value_count = 0U;
    std::size_t after_value_count = 0U;
    std::size_t changed_value_count = 0U;
};

struct UvCornerValueChange {
    geometry::CornerId corner;
    std::optional<core::Vec2d> before;
    std::optional<core::Vec2d> after;
};

inline constexpr std::size_t kMaxProjectDiffUvSamples = 256U;

struct UvSetChange {
    attributes::UvSetId id = 0U;
    std::optional<attributes::UvSetDescriptor> before;
    std::optional<attributes::UvSetDescriptor> after;
    bool descriptor_changed = false;
    bool details_available = true;
    std::size_t changed_corner_count = 0U;
    std::vector<UvCornerValueChange> samples;
    bool samples_truncated = false;
};

struct MaterialBindingChange {
    project::MaterialRegionKey key;
    std::optional<project::MaterialRegionAssignment> before;
    std::optional<project::MaterialRegionAssignment> after;
    std::optional<std::string> before_material_name;
    std::optional<std::string> after_material_name;
    bool added = false;
    bool removed = false;
    bool material_changed = false;
    bool variant_changed = false;
};

struct MaterialSourceChange {
    render::MaterialId id = 0U;
    std::optional<std::string> before_name;
    std::optional<std::string> after_name;
    std::optional<render::StandardMaterial> before_definition;
    std::optional<render::StandardMaterial> after_definition;
    std::optional<core::Revision> before_revision;
    std::optional<core::Revision> after_revision;
    bool added = false;
    bool removed = false;
    bool source_changed = false;
    bool definition_changed = false;
    bool texture_dependencies_changed = false;
};

struct TextureSourceChange {
    assets::TextureAssetId id = 0U;
    std::optional<project::TextureSourceRecord> before;
    std::optional<project::TextureSourceRecord> after;
    std::optional<std::string> before_name;
    std::optional<std::string> after_name;
    std::optional<assets::Sha256Digest> before_content_digest;
    std::optional<assets::Sha256Digest> after_content_digest;
    bool added = false;
    bool removed = false;
    bool source_changed = false;
    bool content_changed = false;
    bool metadata_changed = false;
};

inline constexpr std::size_t kMaxProjectDiffVariantMeshScopeSamples = 16U;

struct MaterialVariantChange {
    project::MaterialVariantId id = 0U;
    std::optional<std::string> before_name;
    std::optional<std::string> after_name;
    std::optional<render::MaterialId> before_base_material;
    std::optional<render::MaterialId> after_base_material;
    std::optional<project::MaterialVariantOverride> before_overrides;
    std::optional<project::MaterialVariantOverride> after_overrides;
    std::optional<core::Revision> before_revision;
    std::optional<core::Revision> after_revision;
    std::size_t before_override_count = 0U;
    std::size_t after_override_count = 0U;
    std::size_t before_mesh_scope_count = 0U;
    std::size_t after_mesh_scope_count = 0U;
    std::vector<std::uint64_t> before_mesh_scope_sample;
    std::vector<std::uint64_t> after_mesh_scope_sample;
    bool before_mesh_scope_truncated = false;
    bool after_mesh_scope_truncated = false;
    bool added = false;
    bool removed = false;
    bool name_changed = false;
    bool base_material_changed = false;
    bool overrides_changed = false;
    bool mesh_scope_changed = false;
};

struct SamplerChange {
    render::SamplerId id = 0U;
    std::optional<project::SamplerIntent> before;
    std::optional<project::SamplerIntent> after;
    bool added = false;
    bool removed = false;
    bool name_changed = false;
    bool addressing_changed = false;
    bool filtering_changed = false;
};

inline constexpr std::size_t kMaxProjectDiffMaterialGraphNodeSamples = 32U;

struct MaterialGraphNodeChange {
    std::uint64_t id = 0U;
    std::optional<project::MaterialGraphNode> before;
    std::optional<project::MaterialGraphNode> after;
    bool added = false;
    bool removed = false;
};

struct MaterialGraphChange {
    std::string identity;
    std::optional<core::Revision> before_revision;
    std::optional<core::Revision> after_revision;
    std::optional<project::MaterialGraphOutputs> before_outputs;
    std::optional<project::MaterialGraphOutputs> after_outputs;
    std::optional<assets::Sha256Digest> before_digest;
    std::optional<assets::Sha256Digest> after_digest;
    std::size_t before_node_count = 0U;
    std::size_t after_node_count = 0U;
    std::size_t added_node_count = 0U;
    std::size_t removed_node_count = 0U;
    std::size_t changed_node_count = 0U;
    std::vector<MaterialGraphNodeChange> node_samples;
    bool node_samples_truncated = false;
    bool added = false;
    bool removed = false;
    bool outputs_changed = false;
    bool content_changed = false;
};

struct MaterialGraphBindingChange {
    render::MaterialId material = 0U;
    std::optional<project::MaterialGraphBinding> before;
    std::optional<project::MaterialGraphBinding> after;
    bool added = false;
    bool removed = false;
    bool graph_changed = false;
    bool source_changed = false;
    bool inputs_changed = false;
};

// A content-addressed texture blob as referenced by the catalog. The blob
// bytes live in the project package store; the diff reports which texture
// records reference each digest before and after.
struct TextureBlobChange {
    assets::Sha256Digest content_digest;
    std::vector<assets::TextureAssetId> before_textures;
    std::vector<assets::TextureAssetId> after_textures;
    bool added = false;
    bool removed = false;
    bool references_changed = false;
};

// Engineering and CONDITION sidecars are opaque, validated bytes in the
// public project; the review reports presence, digest, and size only.
struct SidecarChange {
    bool before_present = false;
    bool after_present = false;
    std::optional<assets::Sha256Digest> before_digest;
    std::optional<assets::Sha256Digest> after_digest;
    std::size_t before_bytes = 0U;
    std::size_t after_bytes = 0U;
};

enum class PresentationRecordKind : std::uint8_t {
    camera,
    light,
    reference_plane,
};

struct PresentationRecordChange {
    PresentationRecordKind kind = PresentationRecordKind::camera;
    std::string identity;
    std::optional<scene::ObjectId> object;
    std::optional<project::AuthoredCamera> camera_before;
    std::optional<project::AuthoredCamera> camera_after;
    std::optional<project::AuthoredLight> light_before;
    std::optional<project::AuthoredLight> light_after;
    std::optional<project::ReferenceImagePlane> plane_before;
    std::optional<project::ReferenceImagePlane> plane_after;
    std::optional<core::Revision> before_revision;
    std::optional<core::Revision> after_revision;
    bool added = false;
    bool removed = false;
};

inline constexpr std::size_t kMaxProjectDiffPresentationRecordSamples = 64U;

struct SpatialFieldSourceSummary {
    std::string identity;
    spatial_field::FieldSemantic semantic = spatial_field::FieldSemantic::generic;
    core::Vec2d origin_uv;
    double cell_size = 1.0;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    double minimum_value = 0.0;
    double maximum_value = 1.0;
    core::Revision source_revision;
    std::optional<assets::Sha256Digest> digest;
    std::size_t sample_count = 0U;
};

struct SpatialFieldSampleChange {
    std::uint32_t u = 0U;
    std::uint32_t v = 0U;
    std::optional<double> before;
    std::optional<double> after;
};

inline constexpr std::size_t kMaxProjectDiffSpatialFieldSamples = 64U;

struct SpatialFieldChange {
    std::string identity;
    std::optional<SpatialFieldSourceSummary> before;
    std::optional<SpatialFieldSourceSummary> after;
    bool added = false;
    bool removed = false;
    bool metadata_changed = false;
    bool grid_changed = false;
    bool sample_correspondence_available = true;
    std::size_t changed_sample_count = 0U;
    std::vector<SpatialFieldSampleChange> samples;
    bool samples_truncated = false;
};

struct BotanicalSourceReceiptSummary {
    assets::Sha256Digest job_identity_digest;
    assets::Sha256Digest input_artifact_digest;
    assets::Sha256Digest output_artifact_digest;
};

struct SourceAssetRecordSummary {
    std::string identity;
    std::string relative_source_path;
    core::Revision source_generation;
    assets::SourceAssetKind kind = assets::SourceAssetKind::other;
    assets::Sha256Digest source_digest;
    std::string importer_identity;
    std::uint32_t importer_version = 0U;
    assets::SourceImportOptions options;
    std::string artifact_identity;
    assets::Sha256Digest artifact_digest;
    std::vector<assets::SourceDomainDigest> domain_digests;
    std::optional<BotanicalSourceReceiptSummary> botanical_receipt;
    std::size_t authored_tag_count = 0U;
    std::vector<std::string> authored_tag_sample;
    bool authored_tags_truncated = false;
    std::size_t dependency_count = 0U;
    std::vector<std::string> dependency_sample;
    bool dependencies_truncated = false;
    assets::Sha256Digest record_digest;
};

struct SourceAssetRecordChange {
    std::string identity;
    std::optional<SourceAssetRecordSummary> before;
    std::optional<SourceAssetRecordSummary> after;
    bool added = false;
    bool removed = false;
};

inline constexpr std::size_t kMaxProjectDiffSourceAssetRecords = 64U;
inline constexpr std::size_t kMaxProjectDiffSourceAssetMetadataSamples = 16U;

struct ConnectionPortSourceSummary {
    std::uint64_t identity = 0U;
    std::uint64_t owner_object = 0U;
    std::string frame_identity;
    std::optional<std::string> parent_frame;
    core::Transform transform_to_parent;
    std::string coordinate_convention;
    std::size_t compatibility_tag_count = 0U;
    std::vector<connection::PortCompatibilityTag> compatibility_tag_sample;
    bool compatibility_tags_truncated = false;
    std::size_t required_capability_count = 0U;
    std::vector<std::string> required_capability_sample;
    bool required_capabilities_truncated = false;
    std::size_t offered_capability_count = 0U;
    std::vector<std::string> offered_capability_sample;
    bool offered_capabilities_truncated = false;
    assets::Sha256Digest record_digest;
};

struct ConnectionEdgeSourceSummary {
    std::uint64_t identity = 0U;
    std::uint64_t first_port = 0U;
    std::uint64_t second_port = 0U;
    connection::ConnectionKind kind = connection::ConnectionKind::mechanical;
    std::size_t shared_tag_count = 0U;
    std::vector<connection::PortCompatibilityTag> shared_tag_sample;
    bool shared_tags_truncated = false;
    std::size_t first_requirement_count = 0U;
    std::vector<std::string> first_requirement_sample;
    bool first_requirements_truncated = false;
    std::size_t second_requirement_count = 0U;
    std::vector<std::string> second_requirement_sample;
    bool second_requirements_truncated = false;
    assets::Sha256Digest record_digest;
};

struct ConnectionPortChange {
    std::uint64_t identity = 0U;
    bool added = false;
    bool removed = false;
    std::optional<ConnectionPortSourceSummary> before;
    std::optional<ConnectionPortSourceSummary> after;
};

struct ConnectionEdgeChange {
    std::uint64_t identity = 0U;
    bool added = false;
    bool removed = false;
    std::optional<ConnectionEdgeSourceSummary> before;
    std::optional<ConnectionEdgeSourceSummary> after;
};

struct ConnectionPortGraphChange {
    bool changed = false;
    bool details_available = true;
    std::optional<assets::Sha256Digest> before_digest;
    std::optional<assets::Sha256Digest> after_digest;
    std::size_t before_port_count = 0U;
    std::size_t after_port_count = 0U;
    std::size_t before_edge_count = 0U;
    std::size_t after_edge_count = 0U;
    std::size_t changed_port_count = 0U;
    std::size_t changed_edge_count = 0U;
    std::vector<ConnectionPortChange> ports;
    std::vector<ConnectionEdgeChange> edges;
    bool ports_truncated = false;
    bool edges_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffConnectionPortSamples = 64U;
inline constexpr std::size_t kMaxProjectDiffConnectionEdgeSamples = 64U;
inline constexpr std::size_t kMaxProjectDiffConnectionMetadataSamples = 16U;

struct ProjectStandardMeasurementSummary {
    std::uint64_t object_id = 0U;
    std::uint64_t mesh_asset = 0U;
    core::Revision mesh_revision;
    double measured_value_meters = 0.0;
};

struct ProjectStandardEvidenceSummary {
    core::Revision source_revision;
    double tolerance_meters = 0.0;
    std::size_t measurement_count = 0U;
    std::vector<ProjectStandardMeasurementSummary> measurements;
    bool measurements_truncated = false;
    assets::Sha256Digest digest;
};

struct ProjectAuthoringStandardSummary {
    std::string identity;
    std::string label;
    project::ProjectStandardCategory category =
        project::ProjectStandardCategory::naming_policy;
    std::string value;
    std::string unit;
    std::size_t rationale_byte_count = 0U;
    assets::Sha256Digest rationale_digest;
    std::optional<ProjectStandardEvidenceSummary> evidence;
    assets::Sha256Digest record_digest;
};

struct ProjectAuthoringStandardChange {
    std::string identity;
    bool added = false;
    bool removed = false;
    std::optional<ProjectAuthoringStandardSummary> before;
    std::optional<ProjectAuthoringStandardSummary> after;
};

struct ProjectAuthoringStandardsChange {
    bool changed = false;
    bool details_available = true;
    std::optional<assets::Sha256Digest> before_digest;
    std::optional<assets::Sha256Digest> after_digest;
    std::size_t before_record_count = 0U;
    std::size_t after_record_count = 0U;
    std::size_t changed_record_count = 0U;
    std::vector<ProjectAuthoringStandardChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffAuthoringStandards = 64U;
inline constexpr std::size_t kMaxProjectDiffStandardMeasurementSamples = 16U;

struct WorldModelSourceSummary {
    assets::Sha256Digest digest;
    std::size_t areas = 0U;
    std::size_t paths = 0U;
    std::size_t regions = 0U;
    std::size_t structure_graphs = 0U;
    std::size_t instance_fields = 0U;
    std::size_t variant_layers = 0U;
};

struct WorldRegionSourceSummary {
    std::uint64_t revision = 0U;
    std::int64_t cell_x = 0;
    std::int64_t cell_y = 0;
    std::size_t area_count = 0U;
    std::size_t path_count = 0U;
    std::size_t neighbor_count = 0U;
    std::size_t cross_region_path_count = 0U;
    std::vector<std::uint64_t> area_id_sample;
    std::vector<std::uint64_t> path_id_sample;
    std::vector<std::uint64_t> neighbor_id_sample;
    std::vector<std::uint64_t> cross_region_path_id_sample;
    bool membership_ids_truncated = false;
};

struct WorldRegionSourceChange {
    world::WorldRegionId id;
    bool added = false;
    bool removed = false;
    std::optional<WorldRegionSourceSummary> before;
    std::optional<WorldRegionSourceSummary> after;
};

struct WorldAreaSourceSummary {
    std::string semantic_type;
    world::AuthoringBindingKind geometry_kind = world::AuthoringBindingKind::none;
    std::uint64_t geometry_identity = 0U;
    std::optional<std::uint64_t> parent_id;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
    std::size_t tag_count = 0U;
    std::vector<std::string> tag_sample;
    bool tags_truncated = false;
};

struct WorldAreaSourceChange {
    world::AreaId id;
    bool added = false;
    bool removed = false;
    std::optional<WorldAreaSourceSummary> before;
    std::optional<WorldAreaSourceSummary> after;
};

struct WorldPathSourceSummary {
    std::string semantic_type;
    std::optional<std::uint64_t> start_area_id;
    std::optional<std::uint64_t> end_area_id;
    double width = 0.0;
    bool closed = false;
    std::size_t control_point_count = 0U;
    std::vector<core::Vec3d> control_point_sample;
    bool control_points_truncated = false;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct WorldPathSourceChange {
    world::PathId id;
    bool added = false;
    bool removed = false;
    std::optional<WorldPathSourceSummary> before;
    std::optional<WorldPathSourceSummary> after;
};

struct WorldInstanceFieldSourceSummary {
    std::string semantic_type;
    std::optional<std::uint64_t> area_id;
    core::Vec3d bounds_minimum;
    core::Vec3d bounds_maximum;
    std::string recipe_identity;
    assets::Sha256Digest recipe_digest;
    std::uint64_t seed = 0U;
    std::uint64_t maximum_instances = 0U;
    std::size_t suppressed_instance_count = 0U;
    std::vector<std::uint64_t> suppressed_instance_sample;
    bool suppressed_instances_truncated = false;
    std::optional<assets::Sha256Digest> geometry_evidence;
    std::string physical_material_intent;
    std::string construction_profile;
    std::string surface_domain_identity;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct WorldInstanceFieldSourceChange {
    world::InstanceFieldId id;
    bool added = false;
    bool removed = false;
    std::optional<WorldInstanceFieldSourceSummary> before;
    std::optional<WorldInstanceFieldSourceSummary> after;
};

struct WorldVariantEditSourceSummary {
    world::VariantTargetKind target_kind = world::VariantTargetKind::semantic_area;
    std::uint64_t target_identity = 0U;
    world::VariantOperation operation = world::VariantOperation::set_value;
    std::string property;
    std::size_t value_bytes = 0U;
    std::string value_sample;
    assets::Sha256Digest value_digest;
    bool value_truncated = false;
};

struct WorldVariantLayerSourceSummary {
    std::string name;
    std::uint64_t base_project_revision = 0U;
    std::size_t edit_count = 0U;
    std::vector<WorldVariantEditSourceSummary> edit_sample;
    bool edits_truncated = false;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct WorldVariantLayerSourceChange {
    world::VariantLayerId id;
    bool added = false;
    bool removed = false;
    std::optional<WorldVariantLayerSourceSummary> before;
    std::optional<WorldVariantLayerSourceSummary> after;
};

struct WorldStructureGraphSourceSummary {
    std::string semantic_type;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
    std::size_t node_count = 0U;
    std::size_t edge_count = 0U;
    std::vector<std::uint64_t> node_id_sample;
    std::vector<std::uint64_t> edge_id_sample;
    bool ids_truncated = false;
};

struct WorldStructureGraphSourceChange {
    world::StructureGraphId id;
    bool added = false;
    bool removed = false;
    std::optional<WorldStructureGraphSourceSummary> before;
    std::optional<WorldStructureGraphSourceSummary> after;
};

inline constexpr std::size_t kMaxProjectDiffWorldRegionRecords = 64U;
inline constexpr std::size_t kMaxProjectDiffWorldRegionMembershipIds = 8U;
inline constexpr std::size_t kMaxProjectDiffWorldAreas = 64U;
inline constexpr std::size_t kMaxProjectDiffWorldAreaTags = 8U;
inline constexpr std::size_t kMaxProjectDiffWorldPaths = 64U;
inline constexpr std::size_t kMaxProjectDiffWorldControlPoints = 8U;
inline constexpr std::size_t kMaxProjectDiffWorldInstanceFields = 64U;
inline constexpr std::size_t kMaxProjectDiffWorldSuppressedInstances = 8U;
inline constexpr std::size_t kMaxProjectDiffWorldVariantLayers = 64U;
inline constexpr std::size_t kMaxProjectDiffWorldVariantEdits = 8U;
inline constexpr std::size_t kMaxProjectDiffWorldVariantValueSampleBytes = 64U;
inline constexpr std::size_t kMaxProjectDiffWorldStructureGraphs = 64U;
inline constexpr std::size_t kMaxProjectDiffWorldStructureGraphIds = 8U;

struct WorldModelChange {
    bool changed = false;
    bool details_available = true;
    std::optional<WorldModelSourceSummary> before;
    std::optional<WorldModelSourceSummary> after;
    std::size_t before_region_count = 0U;
    std::size_t after_region_count = 0U;
    std::size_t changed_region_count = 0U;
    std::vector<WorldRegionSourceChange> regions;
    bool region_records_truncated = false;
    std::size_t before_area_count = 0U;
    std::size_t after_area_count = 0U;
    std::size_t changed_area_count = 0U;
    std::vector<WorldAreaSourceChange> areas;
    bool areas_truncated = false;
    bool area_details_available = true;
    std::size_t before_path_count = 0U;
    std::size_t after_path_count = 0U;
    std::size_t changed_path_count = 0U;
    std::vector<WorldPathSourceChange> paths;
    bool paths_truncated = false;
    bool path_details_available = true;
    std::size_t before_instance_field_count = 0U;
    std::size_t after_instance_field_count = 0U;
    std::size_t changed_instance_field_count = 0U;
    std::vector<WorldInstanceFieldSourceChange> instance_fields;
    bool instance_fields_truncated = false;
    bool instance_field_details_available = true;
    std::size_t before_variant_layer_count = 0U;
    std::size_t after_variant_layer_count = 0U;
    std::size_t changed_variant_layer_count = 0U;
    std::vector<WorldVariantLayerSourceChange> variant_layers;
    bool variant_layers_truncated = false;
    bool variant_layer_details_available = true;
    std::size_t before_structure_graph_count = 0U;
    std::size_t after_structure_graph_count = 0U;
    std::size_t changed_structure_graph_count = 0U;
    std::vector<WorldStructureGraphSourceChange> structure_graphs;
    bool structure_graphs_truncated = false;
    bool structure_graph_details_available = true;
};

struct ScientificModelSourceSummary {
    assets::Sha256Digest digest;
    std::size_t serialized_bytes = 0U;
    std::size_t regions = 0U;
    std::size_t boundaries = 0U;
    std::size_t interfaces = 0U;
    std::size_t sources = 0U;
    std::size_t loads = 0U;
    std::size_t parameters = 0U;
    std::size_t fields = 0U;
    std::size_t probes = 0U;
    std::size_t studies = 0U;
    std::size_t result_sets = 0U;
    std::vector<std::string> populated_extensions;
};

struct ScientificModelChange {
    bool changed = false;
    bool details_available = true;
    std::optional<ScientificModelSourceSummary> before;
    std::optional<ScientificModelSourceSummary> after;
};

enum class ProceduralSourceKind : std::uint8_t {
    curve_path,
    road,
    sweep,
    sign_text,
};

struct CurvePathSourceSummary {
    core::Revision revision;
    assets::Sha256Digest digest;
    std::size_t serialized_bytes = 0U;
    std::size_t segment_count = 0U;
};

struct RoadSourceSummary {
    core::Revision revision;
    assets::Sha256Digest digest;
    std::size_t serialized_bytes = 0U;
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
};

struct SweepSourceSummary {
    core::Revision revision;
    assets::Sha256Digest digest;
    std::size_t serialized_bytes = 0U;
    std::string centerline_identity;
    core::Revision centerline_revision;
    std::size_t profile_point_count = 0U;
    std::uint32_t samples_per_segment = 0U;
    double start_scale = 0.0;
    double end_scale = 0.0;
    geometry::SweepCapPolicy cap_policy = geometry::SweepCapPolicy::both;
    double inner_profile_scale = 0.0;
};

struct SignTextSourceSummary {
    core::Revision revision;
    assets::Sha256Digest digest;
    std::size_t serialized_bytes = 0U;
    std::size_t text_bytes = 0U;
    assets::Sha256Digest text_digest;
    geometry::SignTextAlignment alignment = geometry::SignTextAlignment::center;
    double board_width = 0.0;
    double board_height = 0.0;
    double board_depth = 0.0;
    double glyph_height = 0.0;
    double glyph_depth = 0.0;
    double tracking = 0.0;
    double line_spacing = 0.0;
};

using ProceduralSourceRecordSummary = std::variant<
    CurvePathSourceSummary,
    RoadSourceSummary,
    SweepSourceSummary,
    SignTextSourceSummary>;

struct ProceduralSourceRecordChange {
    ProceduralSourceKind kind = ProceduralSourceKind::curve_path;
    std::string identity;
    bool added = false;
    bool removed = false;
    std::optional<ProceduralSourceRecordSummary> before;
    std::optional<ProceduralSourceRecordSummary> after;
};

struct ProceduralSourceRecordCounts {
    std::size_t curve_paths = 0U;
    std::size_t roads = 0U;
    std::size_t sweeps = 0U;
    std::size_t sign_texts = 0U;
};

struct ProceduralSourcesChange {
    bool changed = false;
    bool details_available = true;
    ProceduralSourceRecordCounts before_counts;
    ProceduralSourceRecordCounts after_counts;
    std::size_t changed_record_count = 0U;
    std::vector<ProceduralSourceRecordChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffProceduralSources = 64U;

struct BiologicalAuthoringSourceSummary {
    std::string source_namespace;
    std::uint64_t asset_id = 0U;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct BiologicalAuthoringSourceChange {
    std::string identity;
    bool added = false;
    bool removed = false;
    std::optional<BiologicalAuthoringSourceSummary> before;
    std::optional<BiologicalAuthoringSourceSummary> after;
};

struct BiologicalAuthoringSourcesChange {
    bool changed = false;
    bool details_available = true;
    std::size_t before_record_count = 0U;
    std::size_t after_record_count = 0U;
    std::size_t changed_record_count = 0U;
    std::vector<BiologicalAuthoringSourceChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffBiologicalAuthoringSources = 64U;

struct RigModuleTemplateSourceSummary {
    core::Revision revision;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct RigModuleTemplateSourceChange {
    std::string identity;
    bool added = false;
    bool removed = false;
    std::optional<RigModuleTemplateSourceSummary> before;
    std::optional<RigModuleTemplateSourceSummary> after;
};

struct RigModuleTemplateSourcesChange {
    bool changed = false;
    bool details_available = true;
    std::size_t before_record_count = 0U;
    std::size_t after_record_count = 0U;
    std::size_t changed_record_count = 0U;
    std::vector<RigModuleTemplateSourceChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffRigModuleTemplates = 64U;

struct DecalPlacementSourceSummary {
    core::Revision revision;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct DecalPlacementSourceChange {
    std::uint64_t placement_id = 0U;
    bool added = false;
    bool removed = false;
    std::optional<DecalPlacementSourceSummary> before;
    std::optional<DecalPlacementSourceSummary> after;
};

struct DecalPlacementSourcesChange {
    bool changed = false;
    std::size_t before_record_count = 0U;
    std::size_t after_record_count = 0U;
    std::size_t changed_record_count = 0U;
    std::vector<DecalPlacementSourceChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffDecalPlacementSources = 64U;

struct PoseLibrarySourceSummary {
    core::Revision revision;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct PoseLibrarySourceChange {
    std::string identity;
    bool added = false;
    bool removed = false;
    std::optional<PoseLibrarySourceSummary> before;
    std::optional<PoseLibrarySourceSummary> after;
};

struct PoseLibrarySourcesChange {
    bool changed = false;
    std::size_t before_record_count = 0U;
    std::size_t after_record_count = 0U;
    std::size_t changed_record_count = 0U;
    std::vector<PoseLibrarySourceChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffPoseLibraries = 64U;

struct KinematicGraphSourceSummary {
    core::Revision revision;
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct KinematicGraphSourceChange {
    std::string identity;
    bool added = false;
    bool removed = false;
    std::optional<KinematicGraphSourceSummary> before;
    std::optional<KinematicGraphSourceSummary> after;
};

struct KinematicGraphSourcesChange {
    bool changed = false;
    std::size_t before_record_count = 0U;
    std::size_t after_record_count = 0U;
    std::size_t changed_record_count = 0U;
    std::vector<KinematicGraphSourceChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffKinematicGraphs = 64U;

enum class ProjectEditReceiptKind : std::uint8_t {
    weight,
    keyed_curve,
    topology,
    mesh_patch,
};

struct ProjectEditReceiptSummary {
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
    std::uint64_t mesh_asset = 0U;
    std::optional<assets::Sha256Digest> source_mesh_digest;
    std::optional<assets::Sha256Digest> result_mesh_digest;
};

struct ProjectEditReceiptChange {
    ProjectEditReceiptKind kind = ProjectEditReceiptKind::weight;
    std::size_t sequence_index = 0U;
    bool added = false;
    bool removed = false;
    std::optional<ProjectEditReceiptSummary> before;
    std::optional<ProjectEditReceiptSummary> after;
};

struct ProjectEditReceiptCounts {
    std::size_t weight = 0U;
    std::size_t keyed_curve = 0U;
    std::size_t topology = 0U;
    std::size_t mesh_patch = 0U;
};

struct ProjectEditReceiptsChange {
    bool changed = false;
    ProjectEditReceiptCounts before_counts;
    ProjectEditReceiptCounts after_counts;
    std::size_t changed_record_count = 0U;
    std::vector<ProjectEditReceiptChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffEditReceipts = 64U;

enum class ProjectInstanceReceiptKind : std::uint8_t {
    sign_text,
    road_surface,
    biological_proxy,
    condition_vegetation,
};

struct SignTextInstanceReceiptSummary {
    std::uint64_t mesh_asset = 0U;
    std::string source_identity;
    core::Revision source_revision;
    std::size_t source_snapshot_bytes = 0U;
    assets::Sha256Digest source_digest;
};

struct RoadSurfaceInstanceReceiptSummary {
    std::uint64_t mesh_asset = 0U;
    project::RoadSurfaceClass surface_class = project::RoadSurfaceClass::asphalt;
    std::string road_identity;
    core::Revision road_revision;
    std::size_t road_snapshot_bytes = 0U;
    assets::Sha256Digest road_digest;
    std::string centerline_identity;
    core::Revision centerline_revision;
    std::size_t centerline_snapshot_bytes = 0U;
    assets::Sha256Digest centerline_digest;
    std::uint32_t samples_per_segment = 0U;
    assets::Sha256Digest generated_mesh_digest;
};

struct BiologicalProxyInstanceReceiptSummary {
    std::uint64_t mesh_asset = 0U;
    std::string source_namespace;
    std::uint64_t source_asset_id = 0U;
    core::Revision source_revision;
    std::uint64_t structure_id = 0U;
    std::size_t source_snapshot_bytes = 0U;
    assets::Sha256Digest source_snapshot_digest;
    assets::Sha256Digest product_digest;
};

struct ConditionVegetationFieldReceiptSummary {
    std::string identity;
    spatial_field::FieldSemantic semantic = spatial_field::FieldSemantic::generic;
    core::Revision source_revision;
    assets::Sha256Digest digest;
};

struct ConditionVegetationInstanceReceiptSummary {
    std::uint64_t mesh_asset = 0U;
    std::uint64_t stable_placement_id = 0U;
    condition::Id recipe_id = 0U;
    condition::Id species_asset_id = 0U;
    std::string plant_source_identity;
    core::Revision plant_source_generation;
    assets::Sha256Digest plant_source_digest;
    core::Revision source_project_revision;
    core::Revision authoring_revision;
    assets::Sha256Digest authoring_digest;
    assets::Sha256Digest preview_digest;
    std::vector<ConditionVegetationFieldReceiptSummary> fields;
    core::Vec3d position{};
    double yaw_radians = 0.0;
    double uniform_scale = 1.0;
    double suitability = 0.0;
};

using ProjectInstanceReceiptSummary = std::variant<
    SignTextInstanceReceiptSummary,
    RoadSurfaceInstanceReceiptSummary,
    BiologicalProxyInstanceReceiptSummary,
    ConditionVegetationInstanceReceiptSummary>;

struct ProjectInstanceReceiptChange {
    ProjectInstanceReceiptKind kind = ProjectInstanceReceiptKind::sign_text;
    scene::ObjectId object;
    bool added = false;
    bool removed = false;
    std::optional<ProjectInstanceReceiptSummary> before;
    std::optional<ProjectInstanceReceiptSummary> after;
};

struct ProjectInstanceReceiptCounts {
    std::size_t sign_text = 0U;
    std::size_t road_surface = 0U;
    std::size_t biological_proxy = 0U;
    std::size_t condition_vegetation = 0U;
};

struct ProjectInstanceReceiptsChange {
    bool changed = false;
    ProjectInstanceReceiptCounts before_counts;
    ProjectInstanceReceiptCounts after_counts;
    ProjectInstanceReceiptCounts changed_record_counts;
    std::size_t changed_record_count = 0U;
    std::vector<ProjectInstanceReceiptChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffInstanceReceipts = 64U;

enum class ProjectMeshAuthoringSourceKind : std::uint8_t {
    sculpt_stack,
    garment_asset,
    cloth_rest_pose_receipt,
};

struct ProjectMeshAuthoringSourceCounts {
    std::size_t sculpt_stack = 0U;
    std::size_t garment_asset = 0U;
    std::size_t cloth_rest_pose_receipt = 0U;
};

struct ProjectMeshAuthoringSourceSummary {
    std::size_t serialized_bytes = 0U;
    assets::Sha256Digest digest;
};

struct ProjectMeshAuthoringSourceChange {
    ProjectMeshAuthoringSourceKind kind =
        ProjectMeshAuthoringSourceKind::sculpt_stack;
    std::uint64_t mesh_asset = 0U;
    bool added = false;
    bool removed = false;
    std::optional<ProjectMeshAuthoringSourceSummary> before;
    std::optional<ProjectMeshAuthoringSourceSummary> after;
};

struct ProjectMeshAuthoringSourcesChange {
    bool changed = false;
    ProjectMeshAuthoringSourceCounts before_counts;
    ProjectMeshAuthoringSourceCounts after_counts;
    ProjectMeshAuthoringSourceCounts changed_record_counts;
    std::size_t changed_record_count = 0U;
    std::vector<ProjectMeshAuthoringSourceChange> records;
    bool records_truncated = false;
};

inline constexpr std::size_t kMaxProjectDiffMeshAuthoringSources = 64U;

struct MeshChange {
    std::uint64_t asset_id = 0U;
    bool added = false;
    bool removed = false;
    core::Revision before_revision;
    core::Revision after_revision;
    std::size_t before_vertices = 0U;
    std::size_t after_vertices = 0U;
    std::size_t before_faces = 0U;
    std::size_t after_faces = 0U;
    std::vector<VertexValueChange> vertices;
    std::vector<FaceTopologyChange> faces;
    std::vector<AttributeLayerChange> attribute_layers;
    std::vector<UvSetChange> uv_sets;
};

struct ProjectMetadataChange {
    bool changed = false;
    bool name_changed = false;
    bool source_namespace_changed = false;
    bool evaluation_graph_changed = false;
    std::string before_name;
    std::string after_name;
    std::string before_source_namespace;
    std::string after_source_namespace;
    std::optional<assets::Sha256Digest> before_evaluation_graph_digest;
    std::optional<assets::Sha256Digest> after_evaluation_graph_digest;
    std::size_t changed_field_count = 0U;
};

struct ProjectDiff {
    core::Revision base_revision;
    core::Revision speculative_revision;
    // Full-project digests bind exact snapshots.
    // Source details remain partial per category.
    bool presentation_catalog_changed = false;
    assets::Sha256Digest base_digest;
    assets::Sha256Digest digest;
    std::vector<ObjectChange> objects;
    std::vector<MeshChange> meshes;
    std::vector<MaterialBindingChange> material_bindings;
    std::vector<MaterialSourceChange> material_sources;
    std::vector<TextureSourceChange> texture_sources;
    ProjectMetadataChange project_metadata;
    std::vector<MaterialVariantChange> material_variants;
    std::vector<SamplerChange> samplers;
    std::vector<MaterialGraphChange> material_graphs;
    std::vector<MaterialGraphBindingChange> material_graph_bindings;
    std::vector<TextureBlobChange> texture_blobs;
    std::optional<SidecarChange> semantic_sidecar;
    std::optional<SidecarChange> engineering;
    std::optional<SidecarChange> condition;
    bool material_catalog_changed = false;
    bool semantic_sidecar_changed = false;
    bool engineering_changed = false;
    bool condition_changed = false;
    [[nodiscard]] static ProjectDiff between(
        const project::ProjectDocument& before,
        const project::ProjectDocument& after);
    [[nodiscard]] std::size_t changed_object_count() const noexcept;
    [[nodiscard]] std::size_t changed_mesh_count() const noexcept;
    [[nodiscard]] std::size_t changed_material_binding_count() const noexcept;
    [[nodiscard]] std::size_t changed_material_source_count() const noexcept;
    [[nodiscard]] std::size_t changed_texture_source_count() const noexcept;
    [[nodiscard]] std::size_t changed_spatial_field_count() const noexcept;
    [[nodiscard]] std::size_t changed_source_asset_record_count() const noexcept;
    [[nodiscard]] bool has_changes() const noexcept;
    [[nodiscard]] bool has_unclassified_source_changes() const noexcept;
    [[nodiscard]] bool source_projection_is_partial() const noexcept;
    std::vector<PresentationRecordChange> presentation_records;
    std::size_t presentation_camera_change_count = 0U;
    std::size_t presentation_light_change_count = 0U;
    std::size_t presentation_plane_change_count = 0U;
    bool presentation_records_truncated = false;
    bool presentation_records_details_available = true;
    std::vector<SpatialFieldChange> spatial_fields;
    bool source_asset_registry_changed = false;
    bool source_asset_record_details_available = true;
    bool source_asset_records_truncated = false;
    std::size_t source_asset_record_change_count = 0U;
    std::vector<SourceAssetRecordChange> source_asset_records;
    ConnectionPortGraphChange connection_port_graph;
    ProjectAuthoringStandardsChange authoring_standards;
    WorldModelChange world_model;
    ScientificModelChange scientific_model;
    ProceduralSourcesChange procedural_sources;
    BiologicalAuthoringSourcesChange biological_authoring_sources;
    RigModuleTemplateSourcesChange rig_module_template_sources;
    DecalPlacementSourcesChange decal_placement_sources;
    PoseLibrarySourcesChange pose_libraries;
    KinematicGraphSourcesChange kinematic_graphs;
    ProjectEditReceiptsChange edit_receipts;
    ProjectInstanceReceiptsChange instance_receipts;
    ProjectMeshAuthoringSourcesChange mesh_authoring_sources;
};

enum class ProjectMergeDisposition : std::uint8_t {
    unchanged,
    left_only,
    right_only,
    identical_edits,
    contested,
};

enum class ProjectMergeSource : std::uint8_t {
    common,
    left,
    right,
};

// A bounded, whole-snapshot three-way comparison. Opaque authored domains
// remain bound by their full-document digests; this review never invents
// field-level merge semantics.
struct ProjectMergeReview {
    core::Revision base_revision;
    core::Revision left_revision;
    core::Revision right_revision;
    assets::Sha256Digest base_digest;
    assets::Sha256Digest left_digest;
    assets::Sha256Digest right_digest;
    ProjectMergeDisposition disposition = ProjectMergeDisposition::unchanged;
    assets::Sha256Digest digest;

    [[nodiscard]] static ProjectMergeReview between(
        const project::ProjectDocument& base,
        const project::ProjectDocument& left,
        const project::ProjectDocument& right);
    [[nodiscard]] core::Result<void> validate() const;
};

// Resolution returns an in-memory copy of one exact source snapshot. It does
// not commit, journal, or authorize that candidate for the live project.
struct ProjectMergeResolution {
    project::ProjectDocument candidate;
    ProjectDiff candidate_diff;
    ProjectMergeSource selected_source = ProjectMergeSource::common;
    core::Revision source_revision;
    assets::Sha256Digest base_digest;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest review_digest;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// Contested snapshots require an explicit common/left/right choice. For
// unambiguous outcomes, the only candidate is carried forward automatically.
[[nodiscard]] core::Result<ProjectMergeResolution> resolve_project_merge(
    const ProjectMergeReview& review,
    const project::ProjectDocument& base,
    const project::ProjectDocument& left,
    const project::ProjectDocument& right,
    std::optional<ProjectMergeSource> contested_choice = std::nullopt);

enum class TaskStatus {
    prepared,
    running,
    review,
    blocked,
    accepted,
    rejected,
};

struct AgentTaskSnapshot {
    TaskStatus status = TaskStatus::prepared;
    std::string task_id;
    std::string goal;
    std::string recipe_id;
    assets::Sha256Digest objective_digest;
    std::vector<HardConstraint> hard_constraints;
    std::vector<ClarificationQuestion> clarifications;
    std::vector<scene::ObjectId> scope_objects;
    std::vector<scene::ObjectId> protected_objects;
    core::Revision base_revision;
    core::Revision speculative_revision;
    std::vector<PlanStepProgress> plan_steps;
    std::vector<StepReceipt> steps;
    std::vector<AgentRepairReceipt> repairs;
    std::vector<AgentRebaseReceipt> rebases;
    std::vector<GhostCorrectionReceipt> ghost_corrections;
    std::vector<AgentGhostRecomputeReceipt> ghost_recomputes;
    std::size_t repair_attempts_remaining = 0U;
    std::vector<AcceptanceGateResult> gates;
    ProjectDiff diff;
    SceneQueryBudget query_budget;
    render::RenderScene before_scene;
    render::RenderScene after_scene;
    std::optional<core::Diagnostic> before_viewport_error;
    std::optional<core::Diagnostic> after_viewport_error;
    std::optional<core::Diagnostic> diagnostic;
    bool can_accept = false;
};


// A demonstration is a redacted, read-only trace of an accepted task. It omits
// project bytes, prompts, freeform goals and answers, filesystem paths, and
// source object IDs. Digests detect accidental corruption but do not prove
// authorship; loading a record never creates or authorizes a task.
struct AgentDemonstrationStep {
    assets::Sha256Digest intent_digest;
    std::string tool_id;
    std::uint32_t target_alias = 0U;
    std::uint64_t revision_before_offset = 0U;
    std::uint64_t revision_after_offset = 0U;
    StepStatus status = StepStatus::failed;
    std::optional<AgentDemonstrationParameters> parameters;
};

struct AgentDemonstrationGate {
    std::string id;
    bool required = true;
    bool passed = false;
    std::string validator_version;
    std::uint64_t source_revision_offset = 0U;
    std::uint64_t evaluated_revision_offset = 0U;
    std::size_t affected_object_count = 0U;
    std::size_t affected_asset_count = 0U;
    assets::Sha256Digest result_digest;
    assets::Sha256Digest evidence_digest;
};

struct AgentDemonstration {
    static constexpr std::uint32_t kFormatVersion = 4U;

    std::uint32_t format_version = kFormatVersion;
    std::string recipe_id;
    assets::Sha256Digest objective_digest;
    assets::Sha256Digest diff_digest;
    std::uint64_t result_revision_span = 0U;
    std::vector<AgentDemonstrationStep> steps;
    std::vector<AgentDemonstrationGate> gates;
    std::size_t repair_count = 0U;
    std::size_t rebase_count = 0U;
    std::size_t ghost_correction_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<AgentDemonstration> deserialize(
        std::string_view bytes);
    [[nodiscard]] core::Result<void> save(const std::filesystem::path& path) const;
    [[nodiscard]] static core::Result<AgentDemonstration> load(
        const std::filesystem::path& path);

private:
    [[nodiscard]] static core::Result<AgentDemonstration> capture(
        const AgentTaskSnapshot& snapshot);

    friend class AgentTask;
};

enum class AgentDemonstrationTargetKind : std::uint8_t {
    object,
    face,
    vertex,
};

struct AgentDemonstrationTraceStep {
    std::string tool_id;
    AgentDemonstrationTargetKind target_kind = AgentDemonstrationTargetKind::object;
    std::uint32_t target_alias = 0U;
    std::uint32_t component_alias = 0U;
    std::uint64_t revision_before_offset = 0U;
    std::uint64_t revision_after_offset = 0U;
    AgentDemonstrationParameters parameters;
    std::optional<assets::Sha256Digest> structure_signature;
    std::optional<assets::Sha256Digest> result_object_structure_signature;
};

// An explicitly selected range of successful, human-authored native
// operations. The serialized trace contains aliases and transferable typed
// values only; it has no project, source identity, mutation, or replay path.
struct AgentDemonstrationTrace {
    static constexpr std::uint32_t kFormatVersion = 3U;

    std::uint64_t result_revision_span = 0U;
    std::vector<AgentDemonstrationTraceStep> steps;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<AgentDemonstrationTrace> deserialize(
        std::string_view bytes);
    [[nodiscard]] core::Result<void> save(const std::filesystem::path& path) const;
    [[nodiscard]] static core::Result<AgentDemonstrationTrace> load(
        const std::filesystem::path& path);

private:
    [[nodiscard]] static core::Result<AgentDemonstrationTrace> capture(
        std::span<const application::NativeOperationTraceEntry> operations);

    friend class ::carto::ui::UiController;
};

// A reusable name for a redacted demonstration trace. It carries no target
// bindings or authority; loading it remains inspection-only until a human
// explicitly binds current targets and reviews a speculative task.
struct AgentDemonstrationRecipe {
    static constexpr std::uint32_t kFormatVersion = 1U;

    std::string name;
    AgentDemonstrationTrace trace;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<AgentDemonstrationRecipe> deserialize(
        std::string_view bytes);
    [[nodiscard]] core::Result<void> save(const std::filesystem::path& path) const;
    [[nodiscard]] static core::Result<AgentDemonstrationRecipe> load(
        const std::filesystem::path& path);
};

// Immutable, checksummed workflow state. Human clarification answers are
// deliberately removed from the persisted plan and must be reissued before a
// resumed task can run. Project bytes remain artifacts owned by this record;
// they are never applied to the live source by loading a checkpoint.
struct AgentTaskCheckpoint {
    static constexpr std::uint32_t kFormatVersion = 12U;

    TaskStatus status = TaskStatus::prepared;
    AgentPlan plan;
    AgentWorkEnvelope envelope;
    std::string provider_identity;
    core::Revision source_revision;
    assets::Sha256Digest plan_context_digest;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest speculative_digest;
    assets::Sha256Digest diff_digest;
    std::string source_project;
    std::string speculative_project;
    std::vector<PlanStepProgress> plan_progress;
    std::vector<StepReceipt> step_receipts;
    std::vector<AgentRepairReceipt> repair_lineage;
    std::vector<AgentRebaseReceipt> rebase_lineage;
    std::vector<GhostCorrectionReceipt> ghost_corrections;
    std::vector<AgentGhostRecomputeReceipt> ghost_recomputes;
    std::string last_validation_report;
    SceneQueryBudget query_budget;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<AgentTaskCheckpoint> deserialize(
        std::string_view bytes);
};

struct AgentRebaseAnalysis {
    core::Revision checkpoint_revision;
    core::Revision current_revision;
    assets::Sha256Digest current_source_digest;
    assets::Sha256Digest read_set_digest;
    assets::Sha256Digest write_set_digest;
    std::vector<scene::ObjectId> read_objects;
    std::vector<scene::ObjectId> write_objects;
    std::vector<std::uint64_t> read_mesh_assets;
    std::vector<std::uint64_t> write_mesh_assets;
    std::vector<project::MaterialRegionKey> read_material_regions;
    std::vector<project::MaterialRegionKey> write_material_regions;
    std::vector<scene::ObjectId> conflicting_objects;
    std::vector<std::uint64_t> conflicting_mesh_assets;
    std::vector<std::string> directly_conflicting_step_ids;
    std::vector<std::string> invalidated_step_ids;
    std::vector<std::string> blockers;

    [[nodiscard]] bool can_rebase() const noexcept { return blockers.empty(); }
};

// Executes revision-bound native proposals on a private ApplicationSession
// copy. The source session is only changed through the human application
// acceptance boundary after all local gates pass and the base revision matches.
class AgentTask final {
public:
    [[nodiscard]] static core::Result<AgentTask> begin(
        application::ApplicationSession& source,
        AgentPlan plan,
        AgentWorkEnvelope envelope);
    [[nodiscard]] static core::Result<AgentTask> repair(
        application::ApplicationSession& source,
        const AgentTask& failed_attempt,
        AgentPlan revised_plan,
        AgentWorkEnvelope envelope,
        std::string justification);

    AgentTask(AgentTask&&) noexcept = default;
    AgentTask& operator=(AgentTask&&) noexcept = delete;
    AgentTask(const AgentTask&) = delete;
    AgentTask& operator=(const AgentTask&) = delete;

    [[nodiscard]] core::Result<AgentTaskSnapshot> run();
    [[nodiscard]] AgentTaskSnapshot snapshot() const;
    [[nodiscard]] core::Result<ProjectMergeReview> compare_current_project_merge() const;
    [[nodiscard]] core::Result<ProjectDiff> project_merge_source_diff(
        const ProjectMergeReview& review,
        ProjectMergeSource source) const;
    [[nodiscard]] core::Result<ProjectMergeResolution> resolve_current_project_merge(
        const ProjectMergeReview& review,
        std::optional<ProjectMergeSource> contested_choice = std::nullopt) const;
    [[nodiscard]] core::Result<void> reject();
    [[nodiscard]] core::Result<void> save_checkpoint(
        const std::filesystem::path& path,
        std::string provider_identity = "cartographer.native-planner") const;
    [[nodiscard]] core::Result<AgentDemonstration> create_demonstration() const;
    [[nodiscard]] static core::Result<AgentTaskCheckpoint> load_checkpoint(
        const std::filesystem::path& path);
    [[nodiscard]] static core::Result<AgentTask> resume_checkpoint(
        application::ApplicationSession& source,
        const AgentTaskCheckpoint& checkpoint,
        AgentPlan reauthorized_plan);
    [[nodiscard]] static core::Result<AgentRebaseAnalysis> analyze_checkpoint_rebase(
        const application::ApplicationSession& source,
        const AgentTaskCheckpoint& checkpoint);
    [[nodiscard]] static core::Result<AgentTask> rebase_checkpoint(
        application::ApplicationSession& source,
        const AgentTaskCheckpoint& checkpoint,
        AgentPlan reauthorized_plan);

private:
    friend class SceneQuery;
    friend class ::carto::application::HumanApplicationAccess;
    [[nodiscard]] core::Result<application::DispatchReceipt> accept(
        application::HumanActionAdmission admission);
    [[nodiscard]] core::Result<application::DispatchReceipt> accept_project_merge(
        application::HumanActionAdmission admission,
        const ProjectMergeReview& review,
        const ProjectMergeResolution& resolution,
        std::string justification);
    AgentTask(
        application::ApplicationSession& source,
        AgentPlan plan,
        AgentWorkEnvelope envelope,
        std::unique_ptr<application::ApplicationSession> speculative,
        application::ApplicationSnapshot source_snapshot,
        std::vector<AgentRepairReceipt> repairs = {},
        std::vector<AgentRebaseReceipt> rebases = {});

    [[nodiscard]] core::Result<void> execute_step(const AgentPlanStep& step);
    [[nodiscard]] core::Result<void> correct_ghost_transform(
        scene::ObjectId object,
        core::Transform transform,
        std::string justification);
    [[nodiscard]] core::Result<void> correct_ghost_vertex_position(
        scene::ObjectId object,
        geometry::VertexId vertex,
        core::Vec3d position,
        std::string justification);
    [[nodiscard]] core::Result<void> correct_ghost_face(
        scene::ObjectId object,
        geometry::FaceId face,
        GhostCorrectionKind kind,
        double distance,
        std::string justification);
    [[nodiscard]] core::Result<std::string> ghost_correction_anchor(
        scene::ObjectId object,
        GhostCorrectionKind kind) const;
    [[nodiscard]] core::Result<std::optional<std::string>> effective_ghost_anchor(
        const GhostCorrectionReceipt& correction) const;
    [[nodiscard]] std::vector<std::string> dependent_plan_steps(
        std::string_view anchor_step_id) const;
    [[nodiscard]] core::Result<void> validate_ghost_correction_lineage() const;
    [[nodiscard]] core::Result<void> apply_ghost_corrections(
        std::optional<std::string_view> after_step_id = std::nullopt);
    [[nodiscard]] core::Result<void> recompute_after_ghost_correction(
        const assets::Sha256Digest& correction_evidence_digest,
        std::string anchor_step_id,
        std::vector<std::string> dependent_step_ids);
    [[nodiscard]] const project::ProjectDocument& scene_query_document() const;
    [[nodiscard]] AgentTaskSnapshot build_snapshot() const;
    [[nodiscard]] ProjectDiff build_diff() const;
    [[nodiscard]] std::vector<AcceptanceGateResult> evaluate_gates() const;
    [[nodiscard]] bool scope_contains(scene::ObjectId object) const noexcept;

    application::ApplicationSession* source_ = nullptr;
    AgentPlan plan_;
    AgentWorkEnvelope envelope_;
    project::ProjectDocument base_document_;
    render::RenderScene base_scene_;
    std::optional<core::Diagnostic> base_viewport_error_;
    std::unique_ptr<application::ApplicationSession> speculative_;
    ai::NativePlanner planner_;
    std::vector<StepReceipt> receipts_;
    std::optional<project::ProjectDocument> rebase_source_document_;
    std::optional<project::ProjectDocument> rebase_speculative_document_;
    std::set<std::string> rebase_reused_step_ids_;
    std::map<std::string, StepReceipt> rebase_reused_step_receipts_;
    std::map<std::string, double> step_transform_distance_meters_;
    std::vector<AgentRepairReceipt> repairs_;
    std::vector<AgentRebaseReceipt> rebases_;
    std::vector<GhostCorrectionReceipt> ghost_corrections_;
    std::vector<AgentGhostRecomputeReceipt> ghost_recompute_lineage_;
    bool replay_ghost_corrections_ = false;
    bool ghost_correction_replay_failed_ = false;
    std::vector<PlanStepProgress> plan_progress_;
    std::vector<AcceptanceGateResult> gates_;
    SceneQueryBudget query_budget_;
    std::set<scene::ObjectId> touched_objects_;
    double transform_distance_used_meters_ = 0.0;
    TaskStatus status_ = TaskStatus::prepared;
    std::optional<core::Diagnostic> diagnostic_;
    mutable std::optional<AgentTaskSnapshot> cached_snapshot_;
};

[[nodiscard]] const char* task_status_name(TaskStatus status) noexcept;
[[nodiscard]] const char* step_status_name(StepStatus status) noexcept;

} // namespace carto::agent
