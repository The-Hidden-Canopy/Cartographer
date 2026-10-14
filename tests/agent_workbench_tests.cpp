#include <carto/agent/workbench.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
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
        return result.value().diff.digest.hex();
    };

    const std::string shallow = run_with_intent("inset face 0.1");
    const std::string deep = run_with_intent("inset face 0.2");
    REQUIRE(shallow != deep);
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
        plan.objective, "choose-exterior-wall", std::string("missing"), std::nullopt));
    plan.objective.clarifications.front().selected_candidate_id = "east";
    REQUIRE(!plan.objective.validate());
    plan.objective.clarifications.front().selected_candidate_id.reset();
    REQUIRE(carto::application::HumanApplicationAccess::answer_agent_clarification(
        plan.objective, "choose-exterior-wall", std::string("east"), std::nullopt));
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
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(ui.snapshot().agent_task->status == carto::agent::TaskStatus::accepted);
    REQUIRE(ui.snapshot().project_revision == source_revision.next());
}

} // namespace

int main() {
    try {
        multi_step_branch_isolated_until_reviewed_acceptance();
        saved_acceptance_is_one_durable_application_event();
        work_envelope_blocks_topology_effect_before_live_mutation();
        diff_digest_tracks_geometry_values_not_only_change_shape();
        shared_mesh_instances_must_be_in_scope_and_within_touch_budget();
        scope_protection_and_stale_revision_fail_closed();
        failed_native_step_and_invalid_task_transitions_do_not_commit();
        missing_goal_and_source_change_after_run_fail_closed();
        clarification_frontier_blocks_consequential_ambiguity_and_binds_answers();
        objective_hard_constraints_block_topology_and_excess_displacement();
        ui_workbench_captures_selection_and_exposes_accept_reject_flow();
        std::cout << "Cartographer agent workbench invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
