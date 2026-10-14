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

} // namespace

int main() {
    try {
        multi_step_branch_isolated_until_reviewed_acceptance();
        plan_dag_orders_dependencies_and_rejects_invalid_graphs();
        failed_dag_node_invalidates_only_dependent_steps();
        saved_acceptance_is_one_durable_application_event();
        work_envelope_blocks_topology_effect_before_live_mutation();
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
        std::cout << "Cartographer agent workbench invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
