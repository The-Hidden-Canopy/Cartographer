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

struct AuthoringObjective {
    std::string id;
    std::string goal;
    core::Revision base_revision;
    std::vector<scene::ObjectId> scope_objects;
    std::vector<scene::ObjectId> protected_objects;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AgentPlanStep {
    std::string id;
    TargetRef target;
    std::string intent;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AgentPlan {
    std::string id;
    AuthoringObjective objective;
    std::vector<AgentPlanStep> steps;

    [[nodiscard]] core::Result<void> validate() const;
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

    [[nodiscard]] core::Result<void> validate() const;
};

enum class StepStatus {
    passed,
    failed,
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
    core::Revision base_revision;
    core::Revision speculative_revision;
    std::vector<StepReceipt> steps;
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
        application::ApplicationSnapshot source_snapshot);

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
    std::vector<AcceptanceGateResult> gates_;
    SceneQueryBudget query_budget_;
    std::set<scene::ObjectId> touched_objects_;
    TaskStatus status_ = TaskStatus::prepared;
    std::optional<core::Diagnostic> diagnostic_;
    mutable std::optional<AgentTaskSnapshot> cached_snapshot_;
};

[[nodiscard]] const char* task_status_name(TaskStatus status) noexcept;

} // namespace carto::agent
