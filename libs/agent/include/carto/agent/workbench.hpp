#pragma once

#include <carto/ai/ai.hpp>
#include <carto/application/application.hpp>
#include <carto/agent/scene_query_budget.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// Extracts only deterministic topology-preservation language from a goal.
// Unsupported and conflicting restrictions fail closed instead of being ignored.
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
};

// Transferable typed values deliberately omit absolute object, vertex, and
// world-position data. Face distances are normalized by the source face's
// longest edge; vertex/object moves are stored as relative displacement.
struct AgentDemonstrationParameters {
    AgentDemonstrationParameterization parameterization =
        AgentDemonstrationParameterization::normalized_face_distance;
    double normalized_distance = 0.0;
    core::Vec3d relative_displacement;

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

// Immutable evidence for an operator-authored transform on the speculative
// branch. The task may replay this correction during resume, rebase, or repair,
// but the receipt never grants authority to mutate the live source project.
struct GhostCorrectionReceipt {
    std::string task_id;
    scene::ObjectId object;
    core::Transform before_transform = core::Transform::identity();
    core::Transform after_transform = core::Transform::identity();
    core::Revision source_revision;
    core::Revision branch_revision_before;
    core::Revision branch_revision_after;
    assets::Sha256Digest plan_context_digest;
    assets::Sha256Digest parent_evidence_digest;
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

    [[nodiscard]] core::Result<void> validate() const;
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
};

struct ProjectDiff {
    core::Revision base_revision;
    core::Revision speculative_revision;
    std::vector<ObjectChange> objects;
    std::vector<MeshChange> meshes;
    bool engineering_changed = false;
    bool condition_changed = false;
    assets::Sha256Digest digest;

    [[nodiscard]] std::size_t changed_object_count() const noexcept;
    [[nodiscard]] std::size_t changed_mesh_count() const noexcept;
};

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
    static constexpr std::uint32_t kFormatVersion = 2U;

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
    static constexpr std::uint32_t kFormatVersion = 2U;

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
    static constexpr std::uint32_t kFormatVersion = 5U;

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
    std::string last_validation_report;

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
    std::vector<scene::ObjectId> conflicting_objects;
    std::vector<std::uint64_t> conflicting_mesh_assets;
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
    [[nodiscard]] core::Result<void> apply_ghost_corrections();
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
    std::vector<AgentRepairReceipt> repairs_;
    std::vector<AgentRebaseReceipt> rebases_;
    std::vector<GhostCorrectionReceipt> ghost_corrections_;
    bool replay_ghost_corrections_ = false;
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
