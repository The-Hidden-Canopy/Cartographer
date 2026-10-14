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
#include <memory>
#include <optional>
#include <set>
#include <string>
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
    std::variant<geometry::FaceId, geometry::VertexId> component;

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

struct AuthoringObjective {
    std::string id;
    std::string goal;
    core::Revision base_revision;
    std::vector<scene::ObjectId> scope_objects;
    std::vector<scene::ObjectId> protected_objects;
    std::vector<HardConstraint> hard_constraints;
    std::vector<ClarificationQuestion> clarifications;

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
};

struct AgentWorkEnvelope {
    std::vector<scene::ObjectId> scope_objects;
    std::uint32_t permitted_effects = ::carto::agent::effect_mask(Effect::geometry) |
        ::carto::agent::effect_mask(Effect::topology);
    std::size_t max_commands = 16U;
    std::size_t max_objects_touched = 16U;
    std::size_t max_scene_queries = 64U;
    std::size_t max_objects_examined = 4096U;
    std::size_t max_topology_elements_examined = 16384U;
    std::size_t max_semantic_records_examined = 8192U;
    std::size_t max_query_result_bytes = 1U * 1024U * 1024U;
    std::size_t max_repair_attempts = 2U;
    std::size_t max_additional_operations = 4U;

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

struct StepReceipt {
    std::string step_id;
    std::string intent;
    std::string tool_id;
    scene::ObjectId target_object;
    core::Revision revision_before;
    core::Revision revision_after;
    StepStatus status = StepStatus::failed;
    std::optional<core::Diagnostic> diagnostic;
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
    std::vector<StepReceipt> previous_step_receipts;
    std::vector<AcceptanceGateResult> previous_gate_results;
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
    assets::Sha256Digest objective_digest;
    std::vector<HardConstraint> hard_constraints;
    std::vector<ClarificationQuestion> clarifications;
    core::Revision base_revision;
    core::Revision speculative_revision;
    std::vector<PlanStepProgress> plan_steps;
    std::vector<StepReceipt> steps;
    std::vector<AgentRepairReceipt> repairs;
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
        std::vector<AgentRepairReceipt> repairs = {});

    [[nodiscard]] core::Result<void> execute_step(const AgentPlanStep& step);
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
    std::vector<PlanStepProgress> plan_progress_;
    std::vector<AcceptanceGateResult> gates_;
    SceneQueryBudget query_budget_;
    std::set<scene::ObjectId> touched_objects_;
    TaskStatus status_ = TaskStatus::prepared;
    std::optional<core::Diagnostic> diagnostic_;
    mutable std::optional<AgentTaskSnapshot> cached_snapshot_;
};

[[nodiscard]] const char* task_status_name(TaskStatus status) noexcept;
[[nodiscard]] const char* step_status_name(StepStatus status) noexcept;

} // namespace carto::agent
