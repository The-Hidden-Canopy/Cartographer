#include <carto/agent/workbench.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition) do { \
    if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
} while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-agent-workbench-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

std::vector<carto::scene::ObjectId> create_boxes(
    carto::application::ApplicationSession& session,
    std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto created = carto::application::HumanApplicationAccess::dispatch(
            session, carto::application::CreateBoxAction{
                "Box " + std::to_string(i + 1U), {1.0, 1.0, 1.0}});
        REQUIRE(created);
    }
    std::vector<carto::scene::ObjectId> result;
    for (const auto& object : session.snapshot().objects) result.push_back(object.object.id);
    return result;
}

carto::agent::AgentPlan plan_for(
    const carto::application::ApplicationSnapshot& snapshot,
    std::vector<carto::scene::ObjectId> scope,
    std::vector<carto::agent::AgentPlanStep> steps,
    std::vector<carto::scene::ObjectId> protected_objects = {}) {
    return carto::agent::AgentPlan{
        "agent-task-test",
        carto::agent::AuthoringObjective{
            "objective-test", "Prepare the selected faces", snapshot.project_revision,
            std::move(scope), std::move(protected_objects), {}, {}},
        std::move(steps)};
}

carto::agent::AgentWorkEnvelope envelope_for(
    const std::vector<carto::scene::ObjectId>& scope) {
    carto::agent::AgentWorkEnvelope envelope;
    envelope.scope_objects = scope;
    envelope.max_commands = 16U;
    envelope.max_objects_touched = std::max<std::size_t>(scope.size(), 1U);
    return envelope;
}

void multi_step_branch_isolated_until_reviewed_acceptance() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 2U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
        {"step-2", {objects[1], carto::geometry::FaceId{2U}}, "extrude face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope_for(objects));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().steps.size() == 2U);
    REQUIRE(run.value().can_accept);
    REQUIRE(run.value().before_scene.size() == 2U);
    REQUIRE(run.value().after_scene.size() == 2U);
    REQUIRE(run.value().diff.changed_mesh_count() == 2U);
    REQUIRE(run.value().diff.changed_object_count() == 2U);
    REQUIRE(std::all_of(run.value().diff.meshes.begin(), run.value().diff.meshes.end(),
        [](const carto::agent::MeshChange& mesh) {
            return !mesh.vertices.empty() || !mesh.faces.empty();
        }));
    REQUIRE(run.value().diff.digest.hex().size() == 64U);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(source.snapshot().objects[0].object.id == objects[0]);
    REQUIRE(source.snapshot().viewport.scene.instances().front().mesh->source_revision ==
            before.viewport.scene.instances().front().mesh->source_revision);

    const auto accepted = carto::application::HumanApplicationAccess::accept_agent_task(task.value());
    REQUIRE(accepted);
    REQUIRE(accepted.value().source == carto::application::OperationSource::ai_proposal);
    REQUIRE(accepted.value().provenance_id == "agent-task-test");
    REQUIRE(accepted.value().action.find(
        "objective=" + task.value().snapshot().objective_digest.hex()) != std::string::npos);
    REQUIRE(accepted.value().action.find("diff=" + task.value().snapshot().diff.digest.hex()) !=
            std::string::npos);
    const auto after = source.snapshot();
    REQUIRE(after.project_revision == before.project_revision.next());
    REQUIRE(after.viewport.scene.instances()[0].mesh->source_revision >
            before.viewport.scene.instances()[0].mesh->source_revision);
    REQUIRE(task.value().snapshot().status == carto::agent::TaskStatus::accepted);
    REQUIRE(after.undo_count == 1U);
    REQUIRE(!task.value().reject());
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::UndoAction{}));
    const auto undone = source.snapshot();
    REQUIRE(undone.project_revision == after.project_revision.next());
    REQUIRE(undone.viewport.scene.instances()[0].mesh->source_revision ==
            before.viewport.scene.instances()[0].mesh->source_revision);
}

void plan_dag_orders_dependencies_and_rejects_invalid_graphs() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 3U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-dependent", {objects[1], carto::geometry::FaceId{1U}},
            "inset face 0.1", {"step-prerequisite"}},
        {"step-independent", {objects[2], carto::geometry::FaceId{1U}},
            "inset face 0.1", {}},
        {"step-prerequisite", {objects[0], carto::geometry::FaceId{1U}},
            "inset face 0.1", {}},
    });
    const auto order = plan.schedule_order();
    REQUIRE(order);
    const std::vector<std::string> expected_order{
        "step-independent", "step-prerequisite", "step-dependent"};
    REQUIRE(order.value() == expected_order);
    REQUIRE(plan.validate());

    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().steps.size() == 3U);
    REQUIRE(run.value().steps[0].step_id == "step-independent");
    REQUIRE(run.value().steps[1].step_id == "step-prerequisite");
    REQUIRE(run.value().steps[2].step_id == "step-dependent");
    REQUIRE(std::all_of(run.value().plan_steps.begin(), run.value().plan_steps.end(),
        [](const auto& progress) {
            return progress.status == carto::agent::StepStatus::passed;
        }));

    auto cycle = plan;
    cycle.steps[2].dependencies = {"step-dependent"};
    REQUIRE(!cycle.validate());
    REQUIRE(cycle.schedule_order().error().message.find("cycle") != std::string::npos);

    auto missing_dependency = plan;
    missing_dependency.steps[0].dependencies = {"not-in-plan"};
    REQUIRE(!missing_dependency.validate());

    auto self_dependency = plan;
    self_dependency.steps[0].dependencies = {"step-dependent"};
    REQUIRE(!self_dependency.validate());

    auto duplicate_dependency = plan;
    duplicate_dependency.steps[0].dependencies = {"step-prerequisite", "step-prerequisite"};
    REQUIRE(!duplicate_dependency.validate());
}

void failed_dag_node_invalidates_only_dependent_steps() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 3U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-failed", {objects[0], carto::geometry::FaceId{999U}},
            "inset face 0.1", {}},
        {"step-dependent", {objects[1], carto::geometry::FaceId{1U}},
            "inset face 0.1", {"step-failed"}},
        {"step-independent", {objects[2], carto::geometry::FaceId{1U}},
            "inset face 0.1", {}},
    });
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope_for(objects));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!run.value().can_accept);
    REQUIRE(run.value().plan_steps.size() == 3U);
    REQUIRE(run.value().plan_steps[0].status == carto::agent::StepStatus::failed);
    REQUIRE(run.value().plan_steps[1].status == carto::agent::StepStatus::invalidated);
    REQUIRE(run.value().plan_steps[1].diagnostic.has_value());
    REQUIRE(run.value().plan_steps[1].diagnostic->message.find("step-failed") !=
        std::string::npos);
    REQUIRE(run.value().plan_steps[2].status == carto::agent::StepStatus::passed);
    REQUIRE(run.value().steps.size() == 2U);
    REQUIRE(run.value().steps[0].step_id == "step-failed");
    REQUIRE(run.value().steps[1].step_id == "step-independent");
    REQUIRE(source.snapshot().project_revision == before.project_revision);
}

void bounded_repair_restarts_from_source_and_preserves_attempt_lineage() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 2U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-failed", {objects[0], carto::geometry::FaceId{999U}}, "inset face 0.1"},
        {"step-dependent", {objects[1], carto::geometry::FaceId{1U}},
            "inset face 0.1", {"step-failed"}},
    });
    auto envelope = envelope_for(objects);
    envelope.max_repair_attempts = 2U;
    envelope.max_additional_operations = 1U;
    auto initial = carto::agent::AgentTask::begin(source, plan, envelope);
    REQUIRE(initial);
    const auto failed = initial.value().run();
    REQUIRE(failed);
    REQUIRE(failed.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(failed.value().plan_steps[0].status == carto::agent::StepStatus::failed);
    REQUIRE(failed.value().plan_steps[1].status == carto::agent::StepStatus::invalidated);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    auto revised = plan;
    revised.id = "agent-task-repair-1";
    revised.steps[0].target.component = carto::geometry::FaceId{1U};
    auto repaired = carto::agent::AgentTask::repair(
        source, initial.value(), revised, envelope,
        "The captured face identity was stale; use the verified face on the same object.");
    REQUIRE(repaired);
    const auto pending = repaired.value().snapshot();
    REQUIRE(pending.status == carto::agent::TaskStatus::prepared);
    REQUIRE(pending.repairs.size() == 1U);
    REQUIRE(pending.repairs.front().attempt == 1U);
    REQUIRE(pending.repairs.front().previous_task_id == plan.id);
    REQUIRE(pending.repairs.front().repaired_task_id == revised.id);
    REQUIRE(pending.repairs.front().justification.find("captured face identity") !=
        std::string::npos);
    REQUIRE(pending.repairs.front().previous_step_receipts.size() == 1U);
    REQUIRE(pending.repairs.front().previous_step_receipts.front().status ==
        carto::agent::StepStatus::failed);
    REQUIRE(pending.repairs.front().previous_gate_results.size() > 0U);
    REQUIRE(!pending.repairs.front().failure_evidence_digest.hex().empty());
    REQUIRE(!pending.repairs.front().repair_evidence_digest.hex().empty());
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    const auto repaired_run = repaired.value().run();
    REQUIRE(repaired_run);
    REQUIRE(repaired_run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(repaired_run.value().can_accept);
    REQUIRE(repaired_run.value().steps.size() == 2U);
    REQUIRE(std::all_of(repaired_run.value().steps.begin(), repaired_run.value().steps.end(),
        [](const auto& receipt) { return receipt.status == carto::agent::StepStatus::passed; }));
    REQUIRE(repaired_run.value().repairs.size() == 1U);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    TempDirectory checkpoint_directory;
    const auto checkpoint_path = checkpoint_directory.path() / "repair.carto-agent-task";
    REQUIRE(repaired.value().save_checkpoint(checkpoint_path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().repair_lineage.size() == 1U);
    REQUIRE(checkpoint.value().repair_lineage.front().previous_step_receipts.size() == 1U);
    REQUIRE(checkpoint.value().repair_lineage.front().previous_step_receipts.front().status ==
        carto::agent::StepStatus::failed);
    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(repaired.value()));
    REQUIRE(source.snapshot().project_revision == before.project_revision.next());
}

void persistent_checkpoint_replays_review_and_blocks_artifact_drift() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-1", {objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    const auto expected_diff = run.value().diff.digest;
    const auto expected_revision = source.snapshot().project_revision;

    TempDirectory directory;
    const auto path = directory.path() / "review.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path, "cartographer.native-test"));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().status == carto::agent::TaskStatus::review);
    REQUIRE(checkpoint.value().provider_identity == "cartographer.native-test");
    REQUIRE(checkpoint.value().plan.id == plan.id);
    REQUIRE(checkpoint.value().plan_progress.front().status == carto::agent::StepStatus::passed);
    REQUIRE(checkpoint.value().step_receipts.size() == 1U);
    REQUIRE(!checkpoint.value().source_project.empty());
    REQUIRE(!checkpoint.value().speculative_project.empty());
    REQUIRE(checkpoint.value().diff_digest == expected_diff);
    REQUIRE(source.snapshot().project_revision == expected_revision);

    auto resumed = carto::agent::AgentTask::resume_checkpoint(
        source, checkpoint.value(), checkpoint.value().plan);
    REQUIRE(resumed);
    REQUIRE(resumed.value().snapshot().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().snapshot().can_accept);
    REQUIRE(resumed.value().snapshot().diff.digest == expected_diff);
    REQUIRE(source.snapshot().project_revision == expected_revision);
    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(resumed.value()));
    REQUIRE(source.snapshot().project_revision == expected_revision.next());

    carto::application::ApplicationSession drift_source;
    const auto drift_objects = create_boxes(drift_source, 1U);
    auto drift_plan = plan_for(drift_source.snapshot(), drift_objects, {
        {"step-1", {drift_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto drift_task = carto::agent::AgentTask::begin(
        drift_source, drift_plan, envelope_for(drift_objects));
    REQUIRE(drift_task);
    REQUIRE(drift_task.value().run());
    TempDirectory drift_directory;
    const auto drift_path = drift_directory.path() / "drift.carto-agent-task";
    REQUIRE(drift_task.value().save_checkpoint(drift_path));
    auto drift_checkpoint = carto::agent::AgentTask::load_checkpoint(drift_path);
    REQUIRE(drift_checkpoint);
    drift_checkpoint.value().speculative_project = drift_checkpoint.value().source_project;
    drift_checkpoint.value().speculative_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(
            drift_checkpoint.value().speculative_project.data()),
        drift_checkpoint.value().speculative_project.size()));
    for (auto& receipt : drift_checkpoint.value().step_receipts) {
        receipt.revision_after = drift_checkpoint.value().source_revision;
    }
    REQUIRE(drift_checkpoint.value().validate());
    const auto replay = carto::agent::AgentTask::resume_checkpoint(
        drift_source, drift_checkpoint.value(), drift_checkpoint.value().plan);
    REQUIRE(replay);
    REQUIRE(replay.value().snapshot().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!replay.value().snapshot().can_accept);
    REQUIRE(replay.value().snapshot().diagnostic.has_value());
    REQUIRE(replay.value().snapshot().diagnostic->message.find("differs") != std::string::npos);
    REQUIRE(drift_source.snapshot().project_revision ==
        drift_checkpoint.value().source_revision);
}

void persistent_checkpoint_reauthorization_and_stale_source_are_fail_closed() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-1", {objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    plan.objective.clarifications.push_back({
        "confirm-plan", "Confirm the exact edit.", "This changes topology.",
        {carto::agent::ClarificationImpact::object_scope,
         carto::agent::ClarificationImpact::topology_deletion},
        objects, {}, 1U, {}, std::nullopt, std::nullopt, std::nullopt});
    REQUIRE(carto::application::HumanApplicationAccess::answer_agent_clarification(
        plan, "confirm-plan", std::nullopt,
        std::string("I approve the exact planned edit.")));
    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    REQUIRE(task.value().run());
    TempDirectory directory;
    const auto path = directory.path() / "clarified.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);
    REQUIRE(!checkpoint.value().plan.objective.clarifications.front().selected_candidate_id);
    REQUIRE(!checkpoint.value().plan.objective.clarifications.front().freeform_answer);
    REQUIRE(!checkpoint.value().plan.objective.clarifications.front().human_receipt);

    auto missing_reapproval = carto::agent::AgentTask::resume_checkpoint(
        source, checkpoint.value(), checkpoint.value().plan);
    REQUIRE(!missing_reapproval);
    REQUIRE(missing_reapproval.error().message.find("requires clarification") !=
        std::string::npos);

    auto reauthorized = checkpoint.value().plan;
    REQUIRE(carto::application::HumanApplicationAccess::answer_agent_clarification(
        reauthorized, "confirm-plan", std::nullopt,
        std::string("I reapprove the exact plan after reopening.")));
    const auto resumed = carto::agent::AgentTask::resume_checkpoint(
        source, checkpoint.value(), reauthorized);
    REQUIRE(resumed);
    REQUIRE(resumed.value().snapshot().status == carto::agent::TaskStatus::review);
    REQUIRE(source.snapshot().project_revision == checkpoint.value().source_revision);

    carto::application::ApplicationSession stale_source;
    const auto stale_objects = create_boxes(stale_source, 1U);
    auto stale_plan = plan_for(stale_source.snapshot(), stale_objects, {
        {"step-1", {stale_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto stale_task = carto::agent::AgentTask::begin(
        stale_source, stale_plan, envelope_for(stale_objects));
    REQUIRE(stale_task);
    REQUIRE(stale_task.value().run());
    TempDirectory stale_directory;
    const auto stale_path = stale_directory.path() / "stale.carto-agent-task";
    REQUIRE(stale_task.value().save_checkpoint(stale_path));
    const auto stale_checkpoint = carto::agent::AgentTask::load_checkpoint(stale_path);
    REQUIRE(stale_checkpoint);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        stale_source, carto::application::CreateBoxAction{"Outside checkpoint", {1.0, 1.0, 1.0}}));
    const auto current_revision = stale_source.snapshot().project_revision;
    const auto analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        stale_source, stale_checkpoint.value());
    REQUIRE(analysis);
    REQUIRE(analysis.value().can_rebase());
    REQUIRE(analysis.value().checkpoint_revision == stale_checkpoint.value().source_revision);
    REQUIRE(analysis.value().current_revision == current_revision);
    REQUIRE(analysis.value().read_objects == stale_objects);
    REQUIRE(analysis.value().write_objects == stale_objects);
    REQUIRE(analysis.value().read_mesh_assets.size() == 1U);
    REQUIRE(analysis.value().write_mesh_assets == analysis.value().read_mesh_assets);

    auto rebased_plan = stale_checkpoint.value().plan;
    rebased_plan.objective.base_revision = current_revision;
    auto rebased = carto::agent::AgentTask::rebase_checkpoint(
        stale_source, stale_checkpoint.value(), rebased_plan);
    REQUIRE(rebased);
    const auto rebased_snapshot = rebased.value().snapshot();
    REQUIRE(rebased_snapshot.status == carto::agent::TaskStatus::review);
    REQUIRE(rebased_snapshot.can_accept);
    REQUIRE(rebased_snapshot.base_revision == current_revision);
    REQUIRE(rebased_snapshot.rebases.size() == 1U);
    REQUIRE(rebased_snapshot.rebases.front().previous_source_revision ==
        stale_checkpoint.value().source_revision);
    REQUIRE(rebased_snapshot.rebases.front().rebased_source_revision == current_revision);
    REQUIRE(stale_source.snapshot().project_revision == current_revision);

    auto changed_intent = stale_checkpoint.value().plan;
    changed_intent.objective.base_revision = current_revision;
    changed_intent.steps.front().intent = "inset face 0.2";
    const auto unauthorized_replan = carto::agent::AgentTask::rebase_checkpoint(
        stale_source, stale_checkpoint.value(), changed_intent);
    REQUIRE(!unauthorized_replan);
    REQUIRE(unauthorized_replan.error().message.find("changed beyond its source revision") !=
        std::string::npos);

    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(rebased.value()));
    REQUIRE(stale_source.snapshot().project_revision == current_revision.next());
    REQUIRE(stale_source.snapshot().objects.size() == 2U);

    carto::application::ApplicationSession conflict_source;
    const auto conflict_objects = create_boxes(conflict_source, 1U);
    auto conflict_plan = plan_for(conflict_source.snapshot(), conflict_objects, {
        {"step-1", {conflict_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto conflict_task = carto::agent::AgentTask::begin(
        conflict_source, conflict_plan, envelope_for(conflict_objects));
    REQUIRE(conflict_task);
    REQUIRE(conflict_task.value().run());
    TempDirectory conflict_directory;
    const auto conflict_path = conflict_directory.path() / "target-conflict.carto-agent-task";
    REQUIRE(conflict_task.value().save_checkpoint(conflict_path));
    const auto conflict_checkpoint = carto::agent::AgentTask::load_checkpoint(conflict_path);
    REQUIRE(conflict_checkpoint);
    const auto before_transform = conflict_source.snapshot().objects.front().object.local_transform;
    auto changed_transform = before_transform;
    changed_transform.translation.x += 0.5;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        conflict_source, carto::application::SetObjectTransformAction{
            conflict_objects.front(), changed_transform, std::nullopt}));
    const auto conflict_analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        conflict_source, conflict_checkpoint.value());
    REQUIRE(conflict_analysis);
    REQUIRE(!conflict_analysis.value().can_rebase());
    REQUIRE(conflict_analysis.value().conflicting_objects == conflict_objects);
    auto conflict_reauthorized_plan = conflict_checkpoint.value().plan;
    conflict_reauthorized_plan.objective.base_revision =
        conflict_source.snapshot().project_revision;
    const auto blocked_rebase = carto::agent::AgentTask::rebase_checkpoint(
        conflict_source, conflict_checkpoint.value(), conflict_reauthorized_plan);
    REQUIRE(!blocked_rebase);
    REQUIRE(blocked_rebase.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(blocked_rebase.error().message.find("read object") != std::string::npos);
    REQUIRE(conflict_source.snapshot().objects.front().object.local_transform.translation.x ==
        changed_transform.translation.x);

    carto::application::ApplicationSession shared_asset_source;
    const auto shared_asset_objects = create_boxes(shared_asset_source, 1U);
    auto shared_asset_plan = plan_for(shared_asset_source.snapshot(), shared_asset_objects, {
        {"step-1", {shared_asset_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto shared_asset_task = carto::agent::AgentTask::begin(
        shared_asset_source, shared_asset_plan, envelope_for(shared_asset_objects));
    REQUIRE(shared_asset_task);
    REQUIRE(shared_asset_task.value().run());
    TempDirectory shared_asset_directory;
    const auto shared_asset_path =
        shared_asset_directory.path() / "shared-asset.carto-agent-task";
    REQUIRE(shared_asset_task.value().save_checkpoint(shared_asset_path));
    const auto shared_asset_checkpoint =
        carto::agent::AgentTask::load_checkpoint(shared_asset_path);
    REQUIRE(shared_asset_checkpoint);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        shared_asset_source, carto::application::DuplicateMeshInstanceAction{
            shared_asset_objects.front(), "Out of scope shared instance", {2.0, 0.0, 0.0}}));
    const auto shared_asset_analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        shared_asset_source, shared_asset_checkpoint.value());
    REQUIRE(shared_asset_analysis);
    REQUIRE(!shared_asset_analysis.value().can_rebase());
    REQUIRE(shared_asset_analysis.value().conflicting_mesh_assets.size() == 1U);
    REQUIRE(shared_asset_analysis.value().blockers.front().find("membership") !=
        std::string::npos);

    carto::application::ApplicationSession mesh_conflict_source;
    const auto mesh_conflict_objects = create_boxes(mesh_conflict_source, 1U);
    auto mesh_conflict_plan = plan_for(mesh_conflict_source.snapshot(), mesh_conflict_objects, {
        {"step-1", {mesh_conflict_objects.front(), carto::geometry::FaceId{1U}},
            "inset face 0.1"},
    });
    auto mesh_conflict_task = carto::agent::AgentTask::begin(
        mesh_conflict_source, mesh_conflict_plan, envelope_for(mesh_conflict_objects));
    REQUIRE(mesh_conflict_task);
    REQUIRE(mesh_conflict_task.value().run());
    TempDirectory mesh_conflict_directory;
    const auto mesh_conflict_path =
        mesh_conflict_directory.path() / "target-mesh-conflict.carto-agent-task";
    REQUIRE(mesh_conflict_task.value().save_checkpoint(mesh_conflict_path));
    const auto mesh_conflict_checkpoint =
        carto::agent::AgentTask::load_checkpoint(mesh_conflict_path);
    REQUIRE(mesh_conflict_checkpoint);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        mesh_conflict_source, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        mesh_conflict_source, carto::application::SelectFaceAction{
            mesh_conflict_objects.front(), carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments inset_arguments;
    inset_arguments.distance = 0.05;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        mesh_conflict_source, carto::application::InvokeToolAction{
            "mesh.inset-face", inset_arguments}));
    const auto mesh_conflict_analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        mesh_conflict_source, mesh_conflict_checkpoint.value());
    REQUIRE(mesh_conflict_analysis);
    REQUIRE(!mesh_conflict_analysis.value().can_rebase());
    REQUIRE(mesh_conflict_analysis.value().conflicting_mesh_assets.size() == 1U);
    REQUIRE(mesh_conflict_analysis.value().blockers.front().find("mesh asset") !=
        std::string::npos);

    carto::application::ApplicationSession sidecar_conflict_source;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        sidecar_conflict_source,
        carto::application::NewProjectAction{"Checkpoint sidecar rebase"}));
    const auto sidecar_objects = create_boxes(sidecar_conflict_source, 1U);
    auto sidecar_plan = plan_for(sidecar_conflict_source.snapshot(), sidecar_objects, {
        {"step-1", {sidecar_objects.front(), carto::geometry::FaceId{1U}},
            "inset face 0.1"},
    });
    auto sidecar_task = carto::agent::AgentTask::begin(
        sidecar_conflict_source, sidecar_plan, envelope_for(sidecar_objects));
    REQUIRE(sidecar_task);
    REQUIRE(sidecar_task.value().run());
    TempDirectory sidecar_directory;
    const auto sidecar_path =
        sidecar_directory.path() / "sidecar-conflict.carto-agent-task";
    REQUIRE(sidecar_task.value().save_checkpoint(sidecar_path));
    const auto sidecar_checkpoint = carto::agent::AgentTask::load_checkpoint(sidecar_path);
    REQUIRE(sidecar_checkpoint);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        sidecar_conflict_source, carto::application::ImportConditionFixtureAction{}));
    const auto sidecar_analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        sidecar_conflict_source, sidecar_checkpoint.value());
    REQUIRE(sidecar_analysis);
    REQUIRE(!sidecar_analysis.value().can_rebase());
    REQUIRE(std::any_of(sidecar_analysis.value().blockers.begin(),
        sidecar_analysis.value().blockers.end(), [](const std::string& blocker) {
            return blocker.find("sidecar") != std::string::npos;
        }));
}

void prepared_checkpoint_requires_explicit_resume_before_native_execution() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-1", {objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    const auto base_revision = source.snapshot().project_revision;
    TempDirectory directory;
    const auto path = directory.path() / "prepared.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().status == carto::agent::TaskStatus::prepared);
    REQUIRE(checkpoint.value().speculative_digest == checkpoint.value().source_digest);
    const auto resumed = carto::agent::AgentTask::resume_checkpoint(
        source, checkpoint.value(), checkpoint.value().plan);
    REQUIRE(resumed);
    REQUIRE(resumed.value().snapshot().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().snapshot().can_accept);
    REQUIRE(source.snapshot().project_revision == base_revision);
}

void persistent_checkpoint_rejects_corruption_and_overwrite() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-1", {objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    REQUIRE(task.value().run());
    TempDirectory directory;
    const auto path = directory.path() / "immutable.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    REQUIRE(!task.value().save_checkpoint(path));

    std::ifstream input(path, std::ios::binary);
    const std::string bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    REQUIRE(!bytes.empty());
    auto corrupted = bytes;
    corrupted.back() = corrupted.back() == '0' ? '1' : '0';
    REQUIRE(!carto::agent::AgentTaskCheckpoint::deserialize(corrupted));
    REQUIRE(carto::agent::AgentTask::load_checkpoint(path));
}

void persistent_checkpoint_v1_format_remains_readable() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-1", {objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    TempDirectory directory;
    const auto path = directory.path() / "v2-prepared.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);
    const auto encoded = checkpoint.value().serialize();
    REQUIRE(encoded);

    constexpr std::string_view magic = "CARTOGRAPHER_AGENT_TASK_CHECKPOINT_V1\n";
    const std::size_t body_length_end = encoded.value().find(':', magic.size());
    REQUIRE(body_length_end != std::string::npos);
    const auto body_length = static_cast<std::size_t>(std::stoull(
        encoded.value().substr(magic.size(), body_length_end - magic.size())));
    const std::size_t body_begin = body_length_end + 1U;
    std::string body = encoded.value().substr(body_begin, body_length);
    REQUIRE(body.starts_with("1:2|"));
    REQUIRE(body.ends_with("1:0|0:|"));
    body[2] = '1';
    body.erase(body.size() - 7U, 4U); // Version 1 has no rebase-lineage count.
    const auto checksum = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    const auto field = [](std::string_view value) {
        return std::to_string(value.size()) + ":" + std::string(value) + "|";
    };
    const std::string v1 = std::string(magic) + field(body) + field(checksum.hex());
    const auto decoded = carto::agent::AgentTaskCheckpoint::deserialize(v1);
    REQUIRE(decoded);
    REQUIRE(decoded.value().rebase_lineage.empty());
    REQUIRE(decoded.value().source_revision == checkpoint.value().source_revision);
}

void ui_persistent_checkpoint_round_trip_preserves_human_boundary() {
    carto::application::ApplicationSession source;
    carto::ui::UiController authoring_ui(source);
    REQUIRE(authoring_ui.dispatch(carto::application::CreateBoxAction{
        "Persistent UI task", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(authoring_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(authoring_ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    REQUIRE(authoring_ui.queue_agent_step("inset face 0.1"));
    REQUIRE(authoring_ui.queue_agent_clarification(
        "Confirm the exact target scope.",
        carto::agent::ClarificationImpact::object_scope));
    REQUIRE(authoring_ui.queue_agent_clarification(
        "Confirm the topology change.",
        carto::agent::ClarificationImpact::topology_deletion));
    const std::string goal = "Prepare this persistent UI task";
    REQUIRE(authoring_ui.answer_agent_clarification(
        goal, "question-1", "I approve this exact target scope."));
    REQUIRE(authoring_ui.answer_agent_clarification(
        goal, "question-2", "I approve the bounded topology edit."));
    const auto run = authoring_ui.run_agent_plan(goal);
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    const auto source_revision = source.snapshot().project_revision;
    TempDirectory directory;
    const auto path = directory.path() / "ui-task.carto-agent-task";
    REQUIRE(authoring_ui.save_agent_checkpoint(path));

    carto::ui::UiController reopened_ui(source);
    REQUIRE(reopened_ui.load_agent_checkpoint(path));
    const auto loaded = reopened_ui.snapshot();
    REQUIRE(loaded.agent_checkpoint_loaded);
    REQUIRE(loaded.agent_checkpoint_task_id == "agent-task-1");
    REQUIRE(loaded.agent_checkpoint_base_revision == source_revision);
    REQUIRE(loaded.agent_clarifications_draft.size() == 2U);
    REQUIRE(!loaded.agent_clarifications_draft.front().human_receipt);
    REQUIRE(!reopened_ui.queue_agent_step("extrude face 0.2"));
    REQUIRE(!reopened_ui.resume_agent_checkpoint());
    REQUIRE(reopened_ui.answer_agent_clarification(
        goal, "question-1", "I reapprove this exact target scope after reopening."));
    REQUIRE(reopened_ui.answer_agent_clarification(
        goal, "question-2", "I reapprove the bounded topology edit after reopening."));
    const auto resumed = reopened_ui.resume_agent_checkpoint();
    REQUIRE(resumed);
    REQUIRE(resumed.value().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().can_accept);
    REQUIRE(!reopened_ui.snapshot().agent_checkpoint_loaded);
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(reopened_ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == source_revision.next());
}

void ui_rebases_disjoint_source_edits_and_reauthorizes_clarifications() {
    carto::application::ApplicationSession source;
    carto::ui::UiController authoring_ui(source);
    REQUIRE(authoring_ui.dispatch(carto::application::CreateBoxAction{
        "Rebase target", {1.0, 1.0, 1.0}}));
    const auto target = source.snapshot().objects.front().object.id;
    REQUIRE(authoring_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(authoring_ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(authoring_ui.queue_agent_step("inset face 0.1"));
    REQUIRE(authoring_ui.queue_agent_clarification(
        "Confirm the topology change.",
        carto::agent::ClarificationImpact::topology_deletion));
    const std::string goal = "Prepare the target while preserving other project edits";
    REQUIRE(authoring_ui.answer_agent_clarification(
        goal, "question-1", "I approve the bounded target edit."));
    REQUIRE(authoring_ui.run_agent_plan(goal));

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "before-rebase.carto-agent-task";
    REQUIRE(authoring_ui.save_agent_checkpoint(checkpoint_path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    const auto checkpoint_revision = checkpoint.value().source_revision;

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::CreateBoxAction{
            "Unrelated human edit", {2.0, 2.0, 2.0}}));
    auto current_revision = source.snapshot().project_revision;
    carto::ui::UiController reopened_ui(source);
    REQUIRE(reopened_ui.load_agent_checkpoint(checkpoint_path));
    auto loaded = reopened_ui.snapshot();
    REQUIRE(loaded.agent_checkpoint_loaded);
    REQUIRE(!loaded.agent_checkpoint_rebased);
    REQUIRE(loaded.project_revision == current_revision);
    REQUIRE(!reopened_ui.resume_agent_checkpoint());

    REQUIRE(reopened_ui.rebase_agent_checkpoint());
    loaded = reopened_ui.snapshot();
    REQUIRE(loaded.agent_checkpoint_rebased);
    REQUIRE(loaded.agent_checkpoint_rebase_revision == current_revision);
    REQUIRE(!reopened_ui.resume_agent_checkpoint());
    REQUIRE(reopened_ui.answer_agent_clarification(
        goal, "question-1", "I reapprove after the unrelated source edit."));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::CreateBoxAction{
            "Second unrelated human edit", {3.0, 3.0, 3.0}}));
    current_revision = source.snapshot().project_revision;
    REQUIRE(reopened_ui.rebase_agent_checkpoint());
    loaded = reopened_ui.snapshot();
    REQUIRE(loaded.agent_checkpoint_rebased);
    REQUIRE(loaded.agent_checkpoint_rebase_revision == current_revision);
    REQUIRE(!loaded.agent_clarifications_draft.front().freeform_answer);
    REQUIRE(!loaded.agent_clarifications_draft.front().human_receipt);
    REQUIRE(!reopened_ui.resume_agent_checkpoint());
    REQUIRE(reopened_ui.answer_agent_clarification(
        goal, "question-1", "I reapprove again after the next source revision."));
    const auto resumed = reopened_ui.resume_agent_checkpoint();
    REQUIRE(resumed);
    REQUIRE(resumed.value().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().can_accept);
    REQUIRE(resumed.value().base_revision == current_revision);
    REQUIRE(resumed.value().rebases.size() == 1U);
    REQUIRE(resumed.value().rebases.front().previous_source_revision == checkpoint_revision);
    REQUIRE(resumed.value().rebases.front().rebased_source_revision == current_revision);
    REQUIRE(source.snapshot().project_revision == current_revision);

    const auto rebased_checkpoint_path = directory.path() / "after-rebase.carto-agent-task";
    REQUIRE(reopened_ui.save_agent_checkpoint(rebased_checkpoint_path));
    const auto rebased_checkpoint =
        carto::agent::AgentTask::load_checkpoint(rebased_checkpoint_path);
    REQUIRE(rebased_checkpoint);
    REQUIRE(rebased_checkpoint.value().rebase_lineage.size() == 1U);
    REQUIRE(rebased_checkpoint.value().rebase_lineage.front().validate());
    REQUIRE(reopened_ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == current_revision.next());
    REQUIRE(source.snapshot().objects.size() == 3U);
}

void repair_rejects_expansion_stale_source_empty_justification_and_exhaustion() {
    const auto make_failed_attempt = [](
        carto::application::ApplicationSession& source,
        const std::vector<carto::scene::ObjectId>& objects,
        carto::agent::AgentWorkEnvelope envelope) {
        auto plan = plan_for(source.snapshot(), {objects.front()}, {
            {"step-1", {objects.front(), carto::geometry::FaceId{999U}}, "inset face 0.1"},
        });
        auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope);
        REQUIRE(task);
        REQUIRE(task.value().run());
        REQUIRE(task.value().snapshot().status == carto::agent::TaskStatus::blocked);
        return std::pair{std::move(task.value()), std::move(envelope)};
    };

    carto::application::ApplicationSession bounded_source;
    const auto bounded_objects = create_boxes(bounded_source, 2U);
    auto bounded_envelope = envelope_for({bounded_objects.front()});
    bounded_envelope.max_additional_operations = 1U;
    auto bounded_attempt = make_failed_attempt(
        bounded_source, bounded_objects, bounded_envelope);
    auto expanded_plan = plan_for(bounded_source.snapshot(), {bounded_objects.front()}, {
        {"step-1", {bounded_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
        {"step-2", {bounded_objects.front(), carto::geometry::FaceId{2U}}, "inset face 0.1"},
        {"step-3", {bounded_objects.front(), carto::geometry::FaceId{3U}}, "inset face 0.1"},
    });
    expanded_plan.id = "agent-task-expanded-repair";
    REQUIRE(!carto::agent::AgentTask::repair(
        bounded_source, bounded_attempt.first, std::move(expanded_plan),
        bounded_attempt.second,
        "Add two operations to repair the failed task."));

    auto changed_scope = plan_for(bounded_source.snapshot(), bounded_objects, {
        {"step-1", {bounded_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    changed_scope.id = "agent-task-scope-repair";
    REQUIRE(!carto::agent::AgentTask::repair(
        bounded_source, bounded_attempt.first, std::move(changed_scope),
        bounded_attempt.second, "Try another object."));
    auto revised_same_object = plan_for(bounded_source.snapshot(), {bounded_objects.front()}, {
        {"step-1", {bounded_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    revised_same_object.id = "agent-task-empty-justification";
    REQUIRE(!carto::agent::AgentTask::repair(
        bounded_source, bounded_attempt.first, revised_same_object,
        bounded_attempt.second, ""));

    carto::application::ApplicationSession effect_source;
    const auto effect_objects = create_boxes(effect_source, 1U);
    auto geometry_only = envelope_for(effect_objects);
    geometry_only.permitted_effects =
        carto::agent::effect_mask(carto::agent::Effect::geometry);
    auto effect_attempt = make_failed_attempt(
        effect_source, effect_objects, geometry_only);
    auto effect_plan = plan_for(effect_source.snapshot(), effect_objects, {
        {"step-1", {effect_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.2"},
    });
    effect_plan.id = "agent-task-effect-expansion";
    auto expanded_effects = geometry_only;
    expanded_effects.permitted_effects |=
        carto::agent::effect_mask(carto::agent::Effect::topology);
    const auto effect_repair = carto::agent::AgentTask::repair(
        effect_source, effect_attempt.first, std::move(effect_plan),
        std::move(expanded_effects), "Request the topology effect required by inset.");
    REQUIRE(!effect_repair);
    REQUIRE(effect_repair.error().message.find("cannot expand") != std::string::npos);

    carto::application::ApplicationSession topology_source;
    const auto topology_objects = create_boxes(topology_source, 1U);
    auto topology_envelope = envelope_for(topology_objects);
    topology_envelope.max_topology_operations = 0U;
    auto topology_plan = plan_for(topology_source.snapshot(), topology_objects, {
        {"step-1", {topology_objects.front(), carto::geometry::FaceId{1U}},
            "inset face 0.1"},
    });
    auto topology_attempt = carto::agent::AgentTask::begin(
        topology_source, std::move(topology_plan), topology_envelope);
    REQUIRE(topology_attempt);
    const auto topology_run = topology_attempt.value().run();
    REQUIRE(topology_run);
    REQUIRE(topology_run.value().status == carto::agent::TaskStatus::blocked);
    auto topology_repair_plan = plan_for(topology_source.snapshot(), topology_objects, {
        {"step-1", {topology_objects.front(), carto::geometry::FaceId{1U}},
            "inset face 0.2"},
    });
    topology_repair_plan.id = "agent-task-topology-expansion";
    auto expanded_topology_envelope = topology_envelope;
    expanded_topology_envelope.max_topology_operations = 1U;
    const auto topology_repair = carto::agent::AgentTask::repair(
        topology_source, topology_attempt.value(), std::move(topology_repair_plan),
        expanded_topology_envelope,
        "Request additional topology operations to pass the failed gate.");
    REQUIRE(!topology_repair);
    REQUIRE(topology_repair.error().message.find("cannot expand") != std::string::npos);

    carto::application::ApplicationSession no_retry_source;
    const auto no_retry_objects = create_boxes(no_retry_source, 1U);
    auto no_retry_envelope = envelope_for(no_retry_objects);
    no_retry_envelope.max_repair_attempts = 0U;
    auto no_retry_attempt = make_failed_attempt(
        no_retry_source, no_retry_objects, no_retry_envelope);
    auto no_retry_plan = plan_for(no_retry_source.snapshot(), no_retry_objects, {
        {"step-1", {no_retry_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    no_retry_plan.id = "agent-task-no-retry";
    REQUIRE(!carto::agent::AgentTask::repair(
        no_retry_source, no_retry_attempt.first, std::move(no_retry_plan),
        no_retry_attempt.second, "The local retry budget is intentionally zero."));

    carto::application::ApplicationSession stale_source;
    const auto stale_objects = create_boxes(stale_source, 1U);
    auto stale_envelope = envelope_for(stale_objects);
    auto stale_attempt = make_failed_attempt(stale_source, stale_objects, stale_envelope);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        stale_source, carto::application::CreateBoxAction{"Concurrent edit", {1.0, 1.0, 1.0}}));
    auto stale_plan = plan_for(stale_source.snapshot(), stale_objects, {
        {"step-1", {stale_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    stale_plan.id = "agent-task-stale-repair";
    const auto stale_repair = carto::agent::AgentTask::repair(
        stale_source, stale_attempt.first, std::move(stale_plan), stale_envelope,
        "The source changed while the failed plan was under review.");
    REQUIRE(!stale_repair);
    REQUIRE(stale_repair.error().code == carto::core::ErrorCode::stale_data);
}

void saved_acceptance_is_one_durable_application_event() {
    TempDirectory temp;
    const auto path = temp.path() / "agent-result.carto";
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto initial_save = carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SaveProjectAction{path});
    if (!initial_save) throw TestFailure(initial_save.error().message);
    const auto baseline = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(baseline);
    const auto before_count = baseline.value().journal_entry_count;
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope_for(objects));
    REQUIRE(task);
    REQUIRE(task.value().run());
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(carto::application::ApplicationSession::inspect_recovery(path).value().journal_entry_count ==
            before_count);
    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(task.value()));
    const auto pending = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(pending);
    REQUIRE(pending.value().journal_entry_count == before_count + 1U);
    REQUIRE(pending.value().state == carto::application::RecoveryInspectionState::pending);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SaveProjectAction{path}));
    const auto saved = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(saved);
    REQUIRE(saved.value().state == carto::application::RecoveryInspectionState::clean);
}

void work_envelope_blocks_topology_effect_before_live_mutation() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto envelope = envelope_for(objects);
    envelope.permitted_effects = carto::agent::effect_mask(carto::agent::Effect::geometry);
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), std::move(envelope));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!run.value().can_accept);
    REQUIRE(run.value().steps.size() == 1U);
    REQUIRE(run.value().steps.front().status == carto::agent::StepStatus::failed);
    REQUIRE(run.value().steps.front().diagnostic.has_value());
    REQUIRE(run.value().steps.front().diagnostic->message.find(
        "outside the native ontology") != std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
}

void work_envelope_blocks_topology_churn_over_budget() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto envelope = envelope_for(objects);
    envelope.max_topology_operations = 0U;
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope);
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!run.value().can_accept);
    const auto topology_gate = std::find_if(
        run.value().gates.begin(), run.value().gates.end(), [](const auto& gate) {
            return gate.id == "envelope.topology_operations";
        });
    REQUIRE(topology_gate != run.value().gates.end());
    REQUIRE(!topology_gate->passed);
    REQUIRE(topology_gate->evidence.find("/ 0 permitted") != std::string::npos);
    REQUIRE(topology_gate->diagnostic.has_value());
    REQUIRE(topology_gate->diagnostic->message.find("topology-operation budget") !=
        std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    auto invalid_budget = envelope_for(objects);
    invalid_budget.max_topology_operations = 1'000'001U;
    REQUIRE(!invalid_budget.validate());
}

void diff_digest_tracks_geometry_values_not_only_change_shape() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    const auto run_with_intent = [&](std::string intent) {
        auto plan = plan_for(before, objects, {
            {"step-1", {objects[0], carto::geometry::FaceId{1U}}, intent},
        });
        auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope_for(objects));
        REQUIRE(task);
        const auto result = task.value().run();
        REQUIRE(result);
        REQUIRE(result.value().can_accept);
        return result.value().diff;
    };

    const auto shallow = run_with_intent("inset face 0.1");
    const auto deep = run_with_intent("inset face 0.2");
    REQUIRE(shallow.digest != deep.digest);
    const auto changed_mesh = std::find_if(shallow.meshes.begin(), shallow.meshes.end(),
        [](const auto& mesh) { return !mesh.faces.empty(); });
    REQUIRE(changed_mesh != shallow.meshes.end());
    const auto changed_face = std::find_if(
        changed_mesh->faces.begin(), changed_mesh->faces.end(), [](const auto& face) {
            return face.added || face.removed || face.connectivity_changed;
        });
    REQUIRE(changed_face != changed_mesh->faces.end());
    REQUIRE(changed_face->added
        ? (changed_face->before_vertices.empty() && !changed_face->after_vertices.empty())
        : changed_face->removed
            ? (!changed_face->before_vertices.empty() && changed_face->after_vertices.empty())
            : (!changed_face->before_vertices.empty() &&
                !changed_face->after_vertices.empty()));
}

void shared_mesh_instances_must_be_in_scope_and_within_touch_budget() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::DuplicateMeshInstanceAction{
            objects[0], "Shared mesh copy", {2.0, 0.0, 0.0}}));
    const auto before = source.snapshot();
    REQUIRE(before.objects.size() == 2U);
    const auto shared_mesh = before.objects[0].object.mesh_asset;
    REQUIRE(shared_mesh.has_value());
    REQUIRE(before.objects[1].object.mesh_asset == shared_mesh);
    const auto sibling = before.objects[0].object.id == objects[0]
        ? before.objects[1].object.id : before.objects[0].object.id;

    auto outside_scope = plan_for(before, {objects[0]}, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto isolated = carto::agent::AgentTask::begin(
        source, std::move(outside_scope), envelope_for({objects[0]}));
    REQUIRE(isolated);
    const auto isolated_run = isolated.value().run();
    REQUIRE(isolated_run);
    REQUIRE(isolated_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(isolated_run.value().diagnostic->message.find("shared by an object outside") !=
            std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    auto over_budget = plan_for(before, {objects[0], sibling}, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto one_object_only = envelope_for({objects[0], sibling});
    one_object_only.max_objects_touched = 1U;
    auto bounded = carto::agent::AgentTask::begin(
        source, std::move(over_budget), std::move(one_object_only));
    REQUIRE(bounded);
    const auto bounded_run = bounded.value().run();
    REQUIRE(bounded_run);
    REQUIRE(bounded_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(bounded_run.value().diagnostic->message.find("object-touch budget") !=
            std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    auto protects_sibling = plan_for(before, {objects[0], sibling}, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    }, {sibling});
    auto protected_task = carto::agent::AgentTask::begin(
        source, std::move(protects_sibling), envelope_for({objects[0], sibling}));
    REQUIRE(protected_task);
    const auto protected_run = protected_task.value().run();
    REQUIRE(protected_run);
    REQUIRE(protected_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(protected_run.value().diagnostic->message.find("protected by the objective") !=
            std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
}

void scope_protection_and_stale_revision_fail_closed() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 2U);
    const auto before = source.snapshot();
    auto outside_scope = plan_for(before, {objects[0]}, {
        {"step-1", {objects[1], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    REQUIRE(!carto::agent::AgentTask::begin(
        source, std::move(outside_scope), envelope_for({objects[0]})));

    auto protected_target = plan_for(before, {objects[0]}, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    }, {objects[0]});
    REQUIRE(!carto::agent::AgentTask::begin(
        source, std::move(protected_target), envelope_for({objects[0]})));

    auto stale_plan = plan_for(before, {objects[0]}, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    carto::core::Transform moved = before.objects.front().object.local_transform;
    moved.translation.x += 0.25;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SetObjectTransformAction{objects[0], moved, std::nullopt}));
    REQUIRE(!carto::agent::AgentTask::begin(
        source, std::move(stale_plan), envelope_for({objects[0]})));
}

void failed_native_step_and_invalid_task_transitions_do_not_commit() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 1000"},
    });
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope_for(objects));
    REQUIRE(task);
    REQUIRE(!carto::application::HumanApplicationAccess::accept_agent_task(task.value()));
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!run.value().can_accept);
    REQUIRE(!carto::application::HumanApplicationAccess::accept_agent_task(task.value()));
    REQUIRE(!task.value().run());
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(task.value().reject());
    REQUIRE(task.value().snapshot().status == carto::agent::TaskStatus::rejected);
}

void missing_goal_and_source_change_after_run_fail_closed() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    auto unjustified = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    unjustified.objective.goal.clear();
    REQUIRE(!carto::agent::AgentTask::begin(
        source, std::move(unjustified), envelope_for(objects)));

    auto plan = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope_for(objects));
    REQUIRE(task);
    const auto reviewed = task.value().run();
    REQUIRE(reviewed);
    REQUIRE(reviewed.value().can_accept);

    auto moved = before.objects.front().object.local_transform;
    moved.translation.x += 0.5;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SetObjectTransformAction{objects[0], moved, std::nullopt}));
    const auto accepted = carto::application::HumanApplicationAccess::accept_agent_task(
        task.value());
    REQUIRE(!accepted);
    REQUIRE(accepted.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(task.value().snapshot().status == carto::agent::TaskStatus::blocked);
    REQUIRE(source.snapshot().objects.front().object.local_transform.translation.x ==
            moved.translation.x);
}

void clarification_frontier_blocks_consequential_ambiguity_and_binds_answers() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 2U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    plan.objective.hard_constraints.push_back({
        "preserve-entry", carto::agent::ObjectiveConstraintKind::preserve_transform,
        objects[1], 0.0, "Keep the north entry transform fixed."});
    plan.objective.clarifications.push_back({
        "choose-exterior-wall",
        "Which exterior wall should change?",
        "The two choices affect separate room boundaries.",
        {carto::agent::ClarificationImpact::object_scope,
         carto::agent::ClarificationImpact::topology_deletion},
        objects,
        {"preserve-entry"},
        2U,
        {{"east", "East guest-room wall", {objects[0]}},
         {"north", "North entry wall", {objects[1]}}},
        std::nullopt,
        std::nullopt,
        std::nullopt});

    auto frontier = plan.objective.clarification_frontier();
    REQUIRE(frontier);
    REQUIRE(frontier.value().questions.size() == 1U);
    REQUIRE(frontier.value().questions.front().impact_score == 55U);
    REQUIRE(frontier.value().requires_user_input());
    const auto before_revision = source.snapshot().project_revision;
    auto blocked = carto::agent::AgentTask::begin(
        source, plan, envelope_for(objects));
    REQUIRE(!blocked);
    REQUIRE(blocked.error().message.find("choose-exterior-wall") != std::string::npos);
    REQUIRE(source.snapshot().project_revision == before_revision);

    REQUIRE(!carto::application::HumanApplicationAccess::answer_agent_clarification(
        plan, "choose-exterior-wall", std::string("missing"), std::nullopt));
    plan.objective.clarifications.front().selected_candidate_id = "east";
    REQUIRE(!plan.objective.validate());
    plan.objective.clarifications.front().selected_candidate_id.reset();
    REQUIRE(carto::application::HumanApplicationAccess::answer_agent_clarification(
        plan, "choose-exterior-wall", std::string("east"), std::nullopt));
    frontier = plan.objective.clarification_frontier();
    REQUIRE(frontier);
    REQUIRE(frontier.value().questions.empty());
    REQUIRE(!frontier.value().requires_user_input());
    const auto objective_digest = plan.objective.digest();
    REQUIRE(objective_digest);
    auto altered_answer_context = plan.objective;
    altered_answer_context.clarifications.front().prompt = "A different question?";
    REQUIRE(!altered_answer_context.validate());
    auto reordered = plan.objective;
    std::reverse(reordered.scope_objects.begin(), reordered.scope_objects.end());
    REQUIRE(reordered.digest().value() == objective_digest.value());

    auto changed_plan = plan;
    changed_plan.steps.front().intent = "extrude face 0.4";
    REQUIRE(!changed_plan.validate());

    auto contradictory = plan;
    contradictory.steps.front().target.object = objects[1];
    REQUIRE(!contradictory.validate());
    auto accepted_choice = carto::agent::AgentTask::begin(
        source, std::move(plan), envelope_for(objects));
    REQUIRE(accepted_choice);
    const auto run = accepted_choice.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    const auto transform_gate = std::find_if(run.value().gates.begin(), run.value().gates.end(),
        [](const auto& gate) { return gate.id == "constraint.preserve-entry"; });
    REQUIRE(transform_gate != run.value().gates.end());
    REQUIRE(transform_gate->passed);
}

void clarification_receipt_rejects_changed_plan_graph() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
        {"step-2", {objects[0], carto::geometry::FaceId{1U}}, "extrude face 0.2",
            {"step-1"}},
    });
    plan.objective.clarifications.push_back({
        "confirm-plan", "Confirm the exact planned edits.",
        "The operator is confirming this specific step sequence.",
        {carto::agent::ClarificationImpact::topology_deletion},
        objects, {}, 2U, {}, std::nullopt, std::nullopt, std::nullopt});
    REQUIRE(plan.validate());
    REQUIRE(carto::application::HumanApplicationAccess::answer_agent_clarification(
        plan, "confirm-plan", std::nullopt,
        std::string("I reviewed these exact edits.")));
    REQUIRE(plan.validate());

    auto changed_intent = plan;
    changed_intent.steps[0].intent = "extrude face 0.2";
    REQUIRE(!changed_intent.validate());

    auto changed_target = plan;
    changed_target.steps[1].target.component = carto::geometry::FaceId{2U};
    REQUIRE(!changed_target.validate());

    auto changed_order = plan;
    std::reverse(changed_order.steps.begin(), changed_order.steps.end());
    REQUIRE(!changed_order.validate());

    auto changed_dependencies = plan;
    changed_dependencies.steps[1].dependencies.clear();
    REQUIRE(!changed_dependencies.validate());
}

void objective_hard_constraints_block_topology_and_excess_displacement() {
    carto::application::ApplicationSession topology_source;
    const auto topology_objects = create_boxes(topology_source, 1U);
    auto topology_plan = plan_for(topology_source.snapshot(), topology_objects, {
        {"step-1", {topology_objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    topology_plan.objective.hard_constraints.push_back({
        "preserve-topology", carto::agent::ObjectiveConstraintKind::preserve_topology,
        std::nullopt, 0.0, "Do not change component connectivity."});
    const auto topology_revision = topology_source.snapshot().project_revision;
    auto topology_task = carto::agent::AgentTask::begin(
        topology_source, std::move(topology_plan), envelope_for(topology_objects));
    REQUIRE(topology_task);
    const auto topology_run = topology_task.value().run();
    REQUIRE(topology_run);
    REQUIRE(topology_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(topology_run.value().diagnostic->message.find("preserve-topology") !=
        std::string::npos);
    REQUIRE(topology_source.snapshot().project_revision == topology_revision);

    carto::application::ApplicationSession displacement_source;
    const auto displacement_objects = create_boxes(displacement_source, 1U);
    const auto vertex = displacement_source.snapshot().viewport.scene.instances().front().mesh->
        vertex_ids.front();
    auto displacement_plan = plan_for(displacement_source.snapshot(), displacement_objects, {
        {"step-1", {displacement_objects[0], vertex}, "move vertex to 4 0 0"},
    });
    displacement_plan.objective.hard_constraints.push_back({
        "vertex-delta", carto::agent::ObjectiveConstraintKind::maximum_vertex_displacement,
        displacement_objects[0], 0.5, "Keep vertex movement below half a world unit."});
    const auto displacement_revision = displacement_source.snapshot().project_revision;
    auto displacement_task = carto::agent::AgentTask::begin(
        displacement_source, std::move(displacement_plan), envelope_for(displacement_objects));
    REQUIRE(displacement_task);
    const auto displacement_run = displacement_task.value().run();
    REQUIRE(displacement_run);
    REQUIRE(displacement_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!displacement_run.value().can_accept);
    const auto displacement_gate = std::find_if(
        displacement_run.value().gates.begin(), displacement_run.value().gates.end(),
        [](const auto& gate) { return gate.id == "constraint.vertex-delta"; });
    REQUIRE(displacement_gate != displacement_run.value().gates.end());
    REQUIRE(!displacement_gate->passed);
    REQUIRE(displacement_gate->evidence.find("0.500000") != std::string::npos);
    REQUIRE(displacement_source.snapshot().project_revision == displacement_revision);

    carto::application::ApplicationSession topology_displacement_source;
    const auto topology_displacement_objects = create_boxes(topology_displacement_source, 1U);
    auto topology_displacement_plan = plan_for(
        topology_displacement_source.snapshot(), topology_displacement_objects, {
            {"step-1", {topology_displacement_objects[0], carto::geometry::FaceId{1U}},
                "extrude face 0.1"},
        });
    topology_displacement_plan.objective.hard_constraints.push_back({
        "bounded-vertex-motion", carto::agent::ObjectiveConstraintKind::maximum_vertex_displacement,
        topology_displacement_objects[0], 0.5,
        "Every original vertex must remain within the stated world-space bound."});
    auto topology_displacement_task = carto::agent::AgentTask::begin(
        topology_displacement_source, std::move(topology_displacement_plan),
        envelope_for(topology_displacement_objects));
    REQUIRE(topology_displacement_task);
    const auto topology_displacement_run = topology_displacement_task.value().run();
    REQUIRE(topology_displacement_run);
    REQUIRE(topology_displacement_run.value().status == carto::agent::TaskStatus::blocked);
    const auto topology_displacement_gate = std::find_if(
        topology_displacement_run.value().gates.begin(),
        topology_displacement_run.value().gates.end(), [](const auto& gate) {
            return gate.id == "constraint.bounded-vertex-motion";
        });
    REQUIRE(topology_displacement_gate != topology_displacement_run.value().gates.end());
    REQUIRE(!topology_displacement_gate->passed);
    REQUIRE(topology_displacement_gate->evidence.find("complete correspondence unavailable") !=
        std::string::npos);
}

void acceptance_evidence_binds_validator_revision_assets_and_result() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    const auto asset_id = before.objects.front().object.mesh_asset;
    REQUIRE(asset_id.has_value());

    const auto run_with_intent = [&](std::string intent) {
        auto plan = plan_for(before, objects, {
            {"step-1", {objects[0], carto::geometry::FaceId{1U}}, std::move(intent)},
        });
        auto task = carto::agent::AgentTask::begin(
            source, std::move(plan), envelope_for(objects));
        REQUIRE(task);
        const auto run = task.value().run();
        REQUIRE(run);
        REQUIRE(run.value().can_accept);
        const auto gate = std::find_if(run.value().gates.begin(), run.value().gates.end(),
            [](const auto& candidate) { return candidate.id == "project.valid"; });
        REQUIRE(gate != run.value().gates.end());
        REQUIRE(gate->passed);
        REQUIRE(gate->validator_version == "cartographer.agent.acceptance.v1");
        REQUIRE(gate->source_revision == before.project_revision);
        REQUIRE(gate->evaluated_revision == run.value().speculative_revision);
        REQUIRE(gate->result_digest == run.value().diff.digest);
        REQUIRE(std::find(gate->affected_objects.begin(), gate->affected_objects.end(),
            objects[0]) != gate->affected_objects.end());
        REQUIRE(std::find(gate->affected_asset_ids.begin(), gate->affected_asset_ids.end(),
            *asset_id) != gate->affected_asset_ids.end());
        REQUIRE(gate->evidence_digest.hex().size() == 64U);
        REQUIRE(!gate->diagnostic.has_value());
        return gate->evidence_digest;
    };

    const auto shallow_evidence = run_with_intent("inset face 0.1");
    const auto deep_evidence = run_with_intent("inset face 0.2");
    REQUIRE(shallow_evidence != deep_evidence);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    const auto vertex = before.viewport.scene.instances().front().mesh->vertex_ids.front();
    auto constrained_plan = plan_for(before, objects, {
        {"step-1", {objects[0], vertex}, "move vertex to 4 0 0"},
    });
    constrained_plan.objective.hard_constraints.push_back({
        "bounded-displacement",
        carto::agent::ObjectiveConstraintKind::maximum_vertex_displacement,
        objects[0], 0.5, "Original vertices must remain within half a world unit."});
    auto constrained_task = carto::agent::AgentTask::begin(
        source, std::move(constrained_plan), envelope_for(objects));
    REQUIRE(constrained_task);
    const auto constrained_run = constrained_task.value().run();
    REQUIRE(constrained_run);
    const auto failed_gate = std::find_if(
        constrained_run.value().gates.begin(), constrained_run.value().gates.end(),
        [](const auto& candidate) { return candidate.id == "constraint.bounded-displacement"; });
    REQUIRE(failed_gate != constrained_run.value().gates.end());
    REQUIRE(!failed_gate->passed);
    REQUIRE(failed_gate->diagnostic.has_value());
    REQUIRE(failed_gate->diagnostic->message.find("could not be proven") != std::string::npos);
    REQUIRE(failed_gate->source_revision == before.project_revision);
    REQUIRE(failed_gate->result_digest == constrained_run.value().diff.digest);
    REQUIRE(failed_gate->evidence_digest.hex().size() == 64U);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
}

void ui_workbench_captures_selection_and_exposes_accept_reject_flow() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"UI box", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    auto draft = ui.snapshot();
    REQUIRE(draft.agent_plan_draft.size() == 1U);
    const auto source_revision = draft.project_revision;
    const auto ran = ui.run_agent_plan("Prepare selected face");
    REQUIRE(ran);
    REQUIRE(ran.value().status == carto::agent::TaskStatus::review);
    REQUIRE(ui.snapshot().project_revision == source_revision);
    REQUIRE(ui.snapshot().agent_task.has_value());
    REQUIRE(ui.snapshot().agent_task->can_accept);
    REQUIRE(ui.snapshot().agent_task_strip.has_value());
    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));
    const auto strip = ui.snapshot().agent_task_strip;
    REQUIRE(strip.has_value());
    REQUIRE(strip->task_id == ran.value().task_id);
    REQUIRE(strip->status == carto::agent::TaskStatus::review);
    REQUIRE(strip->passed_steps == 1U);
    REQUIRE(strip->total_steps == 1U);
    REQUIRE(strip->passed_required_gates == strip->required_gates);
    REQUIRE(strip->required_gates > 0U);
    REQUIRE(ui.set_workspace(carto::ui::Workspace::ai));
    REQUIRE(!ui.snapshot().agent_task_strip.has_value());
    REQUIRE(ui.snapshot().agent_task->status == carto::agent::TaskStatus::review);
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(ui.snapshot().agent_task->status == carto::agent::TaskStatus::accepted);
    REQUIRE(ui.snapshot().project_revision == source_revision.next());
}

void ui_clarification_requires_an_operator_answer_before_review() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Clarified box", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    REQUIRE(ui.queue_agent_clarification(
        "Confirm the original entry geometry remains the reference.",
        carto::agent::ClarificationImpact::publication_readiness));

    const auto source_revision = source.snapshot().project_revision;
    const auto draft = ui.snapshot();
    REQUIRE(draft.agent_clarifications_draft.size() == 1U);
    REQUIRE(draft.agent_clarifications_draft.front().requires_explicit_answer);
    REQUIRE(!ui.run_agent_plan("Prepare the selected face"));
    REQUIRE(!ui.snapshot().agent_task.has_value());
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(!ui.answer_agent_clarification(
        "Prepare the selected face", "question-1", ""));
    REQUIRE(ui.answer_agent_clarification(
        "Prepare the selected face", "question-1",
        "Keep the existing entry geometry as the reference."));
    REQUIRE(!ui.run_agent_plan("A changed goal must not reuse the answer"));
    REQUIRE(!ui.queue_agent_step("inset face 0.2"));

    const auto run = ui.run_agent_plan("Prepare the selected face");
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().clarifications.size() == 1U);
    REQUIRE(run.value().clarifications.front().human_receipt.has_value());
    REQUIRE(run.value().clarifications.front().freeform_answer ==
        std::optional<std::string>("Keep the existing entry geometry as the reference."));
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == source_revision.next());

    carto::application::ApplicationSession stale_source;
    carto::ui::UiController stale_ui(stale_source);
    REQUIRE(stale_ui.dispatch(carto::application::CreateBoxAction{
        "Stale clarification box", {1.0, 1.0, 1.0}}));
    const auto stale_object = stale_source.snapshot().objects.front().object.id;
    REQUIRE(stale_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(stale_ui.dispatch(carto::application::SelectFaceAction{
        stale_object, carto::geometry::FaceId{1U}}));
    REQUIRE(stale_ui.queue_agent_step("inset face 0.1"));
    REQUIRE(stale_ui.queue_agent_clarification(
        "Confirm source geometry before applying.",
        carto::agent::ClarificationImpact::imported_source_integrity));
    REQUIRE(stale_ui.answer_agent_clarification(
        "Prepare source geometry", "question-1", "The captured source is the intended basis."));
    REQUIRE(stale_ui.dispatch(carto::application::CreateBoxAction{
        "Concurrent source edit", {0.5, 0.5, 0.5}}));
    const auto edited_revision = stale_source.snapshot().project_revision;
    REQUIRE(!stale_ui.answer_agent_clarification(
        "Prepare source geometry", "question-1", "Reuse the previous answer."));
    REQUIRE(!stale_ui.run_agent_plan("Prepare source geometry"));
    REQUIRE(stale_source.snapshot().project_revision == edited_revision);
}

void ui_repairs_failed_step_with_operator_justification_and_scope_checks() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Repair box", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Protected box", {1.0, 1.0, 1.0}}));
    const auto snapshot = source.snapshot();
    const auto target = snapshot.objects[0].object.id;
    const auto outside_scope = snapshot.objects[1].object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 1000"));
    const auto base_revision = source.snapshot().project_revision;
    const auto failed = ui.run_agent_plan("Repair only the captured face");
    REQUIRE(failed);
    REQUIRE(failed.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(source.snapshot().project_revision == base_revision);
    REQUIRE(ui.set_workspace(carto::ui::Workspace::condition));
    const auto blocked_strip = ui.snapshot().agent_task_strip;
    REQUIRE(blocked_strip.has_value());
    REQUIRE(blocked_strip->status == carto::agent::TaskStatus::blocked);
    REQUIRE(blocked_strip->blocker.has_value());
    REQUIRE(blocked_strip->total_steps == 1U);
    REQUIRE(blocked_strip->passed_steps == 0U);
    REQUIRE(ui.set_workspace(carto::ui::Workspace::ai));

    auto revised_step = ui.snapshot().agent_plan_draft.front();
    revised_step.target.object = outside_scope;
    REQUIRE(!ui.revise_agent_plan_step(revised_step));
    revised_step.target.object = target;
    revised_step.intent = "inset face 0.1";
    REQUIRE(ui.revise_agent_plan_step(revised_step));
    REQUIRE(!ui.repair_agent_plan(""));
    const auto repaired = ui.repair_agent_plan(
        "The requested face index was invalid; retry the captured object with a bounded inset.");
    REQUIRE(repaired);
    REQUIRE(repaired.value().status == carto::agent::TaskStatus::review);
    REQUIRE(repaired.value().can_accept);
    REQUIRE(repaired.value().repairs.size() == 1U);
    REQUIRE(repaired.value().repairs.front().previous_step_receipts.front().status ==
        carto::agent::StepStatus::failed);
    REQUIRE(source.snapshot().project_revision == base_revision);
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == base_revision.next());
}

void ui_repair_invalidates_plan_bound_human_receipts_until_reanswered() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Clarified repair box", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 1000"));
    REQUIRE(ui.queue_agent_clarification(
        "Confirm the exact face edit before repair.",
        carto::agent::ClarificationImpact::publication_readiness));
    const std::string goal = "Prepare the selected face";
    REQUIRE(ui.answer_agent_clarification(
        goal, "question-1", "I approved the originally captured face edit."));
    const auto before = source.snapshot().project_revision;
    const auto failed = ui.run_agent_plan(goal);
    REQUIRE(failed);
    REQUIRE(failed.value().status == carto::agent::TaskStatus::blocked);

    auto revised_step = ui.snapshot().agent_plan_draft.front();
    revised_step.intent = "inset face 0.1";
    REQUIRE(ui.revise_agent_plan_step(revised_step));
    REQUIRE(!ui.repair_agent_plan(
        "The invalid face index is corrected, subject to renewed human approval."));
    REQUIRE(ui.answer_agent_clarification(
        goal, "question-1", "I approve this corrected bounded inset on the same object."));
    const auto repaired = ui.repair_agent_plan(
        "The invalid face index is corrected, subject to renewed human approval.");
    REQUIRE(repaired);
    REQUIRE(repaired.value().status == carto::agent::TaskStatus::review);
    REQUIRE(repaired.value().clarifications.front().human_receipt.has_value());
    REQUIRE(repaired.value().clarifications.front().freeform_answer ==
        std::optional<std::string>(
            "I approve this corrected bounded inset on the same object."));
    REQUIRE(repaired.value().repairs.size() == 1U);
    REQUIRE(source.snapshot().project_revision == before);
}

} // namespace

int main() {
    try {
        multi_step_branch_isolated_until_reviewed_acceptance();
        plan_dag_orders_dependencies_and_rejects_invalid_graphs();
        failed_dag_node_invalidates_only_dependent_steps();
        bounded_repair_restarts_from_source_and_preserves_attempt_lineage();
        repair_rejects_expansion_stale_source_empty_justification_and_exhaustion();
        saved_acceptance_is_one_durable_application_event();
        work_envelope_blocks_topology_effect_before_live_mutation();
        work_envelope_blocks_topology_churn_over_budget();
        diff_digest_tracks_geometry_values_not_only_change_shape();
        shared_mesh_instances_must_be_in_scope_and_within_touch_budget();
        scope_protection_and_stale_revision_fail_closed();
        failed_native_step_and_invalid_task_transitions_do_not_commit();
        missing_goal_and_source_change_after_run_fail_closed();
        clarification_frontier_blocks_consequential_ambiguity_and_binds_answers();
        clarification_receipt_rejects_changed_plan_graph();
        objective_hard_constraints_block_topology_and_excess_displacement();
        acceptance_evidence_binds_validator_revision_assets_and_result();
        ui_workbench_captures_selection_and_exposes_accept_reject_flow();
        ui_clarification_requires_an_operator_answer_before_review();
        ui_repairs_failed_step_with_operator_justification_and_scope_checks();
        ui_repair_invalidates_plan_bound_human_receipts_until_reanswered();
        persistent_checkpoint_replays_review_and_blocks_artifact_drift();
        persistent_checkpoint_reauthorization_and_stale_source_are_fail_closed();
        prepared_checkpoint_requires_explicit_resume_before_native_execution();
        persistent_checkpoint_rejects_corruption_and_overwrite();
        persistent_checkpoint_v1_format_remains_readable();
        ui_persistent_checkpoint_round_trip_preserves_human_boundary();
        ui_rebases_disjoint_source_edits_and_reauthorizes_clarifications();
        std::cout << "Cartographer agent workbench invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
