#include <carto/agent/workbench.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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
            std::move(scope), std::move(protected_objects), {}, {}, {}},
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

carto::agent::AgentWorkEnvelope translation_envelope(
    const std::vector<carto::scene::ObjectId>& scope,
    double max_distance) {
    auto envelope = envelope_for(scope);
    envelope.permitted_effects |= carto::agent::effect_mask(carto::agent::Effect::transform);
    envelope.max_transform_distance_meters = max_distance;
    return envelope;
}

std::vector<std::string> checkpoint_fields(std::string_view bytes) {
    std::vector<std::string> fields;
    std::size_t cursor = 0U;
    while (cursor < bytes.size()) {
        const std::size_t length_end = bytes.find(':', cursor);
        REQUIRE(length_end != std::string_view::npos);
        std::size_t length = 0U;
        const auto parsed = std::from_chars(
            bytes.data() + cursor, bytes.data() + length_end, length);
        REQUIRE(parsed.ec == std::errc{} && parsed.ptr == bytes.data() + length_end);
        cursor = length_end + 1U;
        REQUIRE(length <= bytes.size() - cursor);
        fields.emplace_back(bytes.substr(cursor, length));
        cursor += length;
        REQUIRE(cursor < bytes.size() && bytes[cursor] == '|');
        ++cursor;
    }
    return fields;
}

std::string checkpoint_body(const std::vector<std::string>& fields) {
    std::string body;
    for (const auto& value : fields) {
        body += std::to_string(value.size()) + ":" + value + "|";
    }
    return body;
}

std::uint64_t checkpoint_number(const std::vector<std::string>& fields, std::size_t& cursor) {
    REQUIRE(cursor < fields.size());
    std::uint64_t value = 0U;
    const auto& field = fields[cursor++];
    const auto parsed = std::from_chars(
        field.data(), field.data() + field.size(), value);
    REQUIRE(!field.empty() && parsed.ec == std::errc{} &&
        parsed.ptr == field.data() + field.size());
    return value;
}

void skip_checkpoint_object_ids(const std::vector<std::string>& fields, std::size_t& cursor) {
    const auto count = checkpoint_number(fields, cursor);
    REQUIRE(count <= fields.size() - cursor);
    cursor += static_cast<std::size_t>(count);
}

void skip_checkpoint_strings(const std::vector<std::string>& fields, std::size_t& cursor) {
    const auto count = checkpoint_number(fields, cursor);
    REQUIRE(count <= fields.size() - cursor);
    cursor += static_cast<std::size_t>(count);
}

std::vector<std::string> legacy_v3_fields_from_current(std::vector<std::string> fields) {
    REQUIRE(fields.size() > 10U && fields[0] == "9");
    fields[0] = "8";
    fields.pop_back(); // Version 8 stores ghost anchors and recompute receipts in an extension.
    fields[0] = "7";
    fields[0] = "3";
    fields.erase(fields.begin() + 13); // Version 4 has no recipe field.
    std::size_t cursor = 10U;
    cursor += 4U; // Plan, objective, goal, and base revision.
    skip_checkpoint_object_ids(fields, cursor);
    skip_checkpoint_object_ids(fields, cursor);

    const auto constraint_count = checkpoint_number(fields, cursor);
    for (std::size_t index = 0U; index < constraint_count; ++index) {
        cursor += 2U; // Constraint ID and kind.
        if (checkpoint_number(fields, cursor) != 0U) ++cursor;
        cursor += 2U; // Maximum displacement and rationale.
    }

    const auto question_count = checkpoint_number(fields, cursor);
    for (std::size_t index = 0U; index < question_count; ++index) {
        cursor += 3U; // ID, prompt, rationale.
        const auto impact_count = checkpoint_number(fields, cursor);
        cursor += static_cast<std::size_t>(impact_count);
        skip_checkpoint_object_ids(fields, cursor);
        skip_checkpoint_strings(fields, cursor);
        ++cursor; // Downstream branch count.
        const auto candidate_count = checkpoint_number(fields, cursor);
        for (std::size_t candidate = 0U; candidate < candidate_count; ++candidate) {
            cursor += 2U; // Candidate ID and label.
            skip_checkpoint_object_ids(fields, cursor);
        }
        ++cursor; // Explicit-answer flag.
    }

    const auto step_count = checkpoint_number(fields, cursor);
    for (std::size_t index = 0U; index < step_count; ++index) {
        cursor += 2U; // Step ID and target object.
        const auto target_kind = checkpoint_number(fields, cursor);
        REQUIRE(target_kind <= 1U); // Object targets have no v3 representation.
        fields[cursor - 1U] = target_kind == 0U ? "1" : "0";
        ++cursor; // Legacy face or vertex ID.
        ++cursor; // Intent.
        skip_checkpoint_strings(fields, cursor);
    }

    skip_checkpoint_object_ids(fields, cursor); // Work envelope scope.
    REQUIRE(cursor + 11U < fields.size());
    fields.erase(fields.begin() + static_cast<std::ptrdiff_t>(cursor + 11U));
    return fields;
}

void root_object_translation_is_explicit_bounded_and_checkpointed() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto object = objects.front();
    carto::core::Transform initial{
        {2.0, -1.0, 0.5}, {0.0, 0.0, 0.3826834323650898, 0.9238795325112867},
        {1.5, 0.75, 2.0}};
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SetObjectTransformAction{object, initial, std::nullopt}));
    const auto before = source.snapshot();

    auto plan = plan_for(before, objects, {
        {"step-1", {object, std::monostate{}}, "translate object by 0.25 0 -0.5"},
    });
    auto unapproved = carto::agent::AgentTask::begin(
        source, plan, envelope_for(objects));
    REQUIRE(unapproved);
    const auto blocked = unapproved.value().run();
    REQUIRE(blocked);
    REQUIRE(blocked.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(blocked.value().diagnostic->message.find("forbids the transform") !=
        std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    auto invalid_envelope = translation_envelope(objects, 0.0);
    REQUIRE(!invalid_envelope.validate());

    auto task = carto::agent::AgentTask::begin(
        source, plan, translation_envelope(objects, 1.0));
    REQUIRE(task);
    REQUIRE(!carto::application::HumanApplicationAccess::accept_agent_task(task.value()));
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().can_accept);
    REQUIRE(run.value().diff.changed_object_count() == 1U);
    REQUIRE(run.value().diff.changed_mesh_count() == 0U);
    REQUIRE(run.value().diff.objects.front().transform_changed);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(source.snapshot().objects.front().object.local_transform.translation.x ==
        initial.translation.x);

    const auto ghost = run.value().after_scene.find(object);
    REQUIRE(ghost != nullptr);
    REQUIRE(ghost->world_transform.translation.x == 2.25);
    REQUIRE(ghost->world_transform.translation.y == -1.0);
    REQUIRE(ghost->world_transform.translation.z == 0.0);
    REQUIRE(ghost->world_transform.rotation.x == initial.rotation.x);
    REQUIRE(ghost->world_transform.rotation.y == initial.rotation.y);
    REQUIRE(ghost->world_transform.rotation.z == initial.rotation.z);
    REQUIRE(ghost->world_transform.rotation.w == initial.rotation.w);
    REQUIRE(ghost->world_transform.scale.x == initial.scale.x);
    REQUIRE(ghost->world_transform.scale.y == initial.scale.y);
    REQUIRE(ghost->world_transform.scale.z == initial.scale.z);

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "translation.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(checkpoint_path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().envelope.max_transform_distance_meters == 1.0);
    REQUIRE(std::holds_alternative<std::monostate>(
        checkpoint.value().plan.steps.front().target.component));

    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(task.value()));
    const auto after = source.snapshot();
    REQUIRE(after.project_revision == before.project_revision.next());
    const auto accepted = std::find_if(after.objects.begin(), after.objects.end(),
        [object](const auto& candidate) { return candidate.object.id == object; });
    REQUIRE(accepted != after.objects.end());
    REQUIRE(accepted->object.local_transform.translation.x == 2.25);
    REQUIRE(accepted->object.local_transform.translation.y == -1.0);
    REQUIRE(accepted->object.local_transform.translation.z == 0.0);
    REQUIRE(accepted->object.local_transform.rotation.x == initial.rotation.x);
    REQUIRE(accepted->object.local_transform.rotation.y == initial.rotation.y);
    REQUIRE(accepted->object.local_transform.rotation.z == initial.rotation.z);
    REQUIRE(accepted->object.local_transform.rotation.w == initial.rotation.w);
    REQUIRE(accepted->object.local_transform.scale.x == initial.scale.x);
    REQUIRE(accepted->object.local_transform.scale.y == initial.scale.y);
    REQUIRE(accepted->object.local_transform.scale.z == initial.scale.z);
}

void root_object_translation_rejects_distance_overrun_and_preserve_constraint() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    auto plan = plan_for(before, objects, {
        {"step-1", {objects.front(), std::monostate{}}, "translate object by 0.75 0 0"},
        {"step-2", {objects.front(), std::monostate{}}, "translate object by 0.75 0 0",
            {"step-1"}},
    });
    auto task = carto::agent::AgentTask::begin(
        source, plan, translation_envelope(objects, 1.0));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(run.value().steps.size() == 2U);
    REQUIRE(run.value().steps[0].status == carto::agent::StepStatus::passed);
    REQUIRE(run.value().steps[1].status == carto::agent::StepStatus::failed);
    REQUIRE(run.value().diagnostic->message.find("transform-distance budget") !=
        std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(source.snapshot().objects.front().object.local_transform.translation.x == 0.0);

    carto::application::ApplicationSession constrained_source;
    const auto constrained_objects = create_boxes(constrained_source, 1U);
    auto constrained_plan = plan_for(constrained_source.snapshot(), constrained_objects, {
        {"step-1", {constrained_objects.front(), std::monostate{}},
            "translate object by 0.25 0 0"},
    });
    constrained_plan.objective.hard_constraints.push_back({
        "preserve-transform", carto::agent::ObjectiveConstraintKind::preserve_transform,
        constrained_objects.front(), 0.0, "Keep this object's transform unchanged."});
    const auto constrained_revision = constrained_source.snapshot().project_revision;
    auto constrained_task = carto::agent::AgentTask::begin(
        constrained_source, std::move(constrained_plan),
        translation_envelope(constrained_objects, 1.0));
    REQUIRE(constrained_task);
    const auto constrained_run = constrained_task.value().run();
    REQUIRE(constrained_run);
    REQUIRE(constrained_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(constrained_run.value().diagnostic->message.find("preserve-transform") !=
        std::string::npos);
    REQUIRE(constrained_source.snapshot().project_revision == constrained_revision);
}

void object_translation_checkpoint_rebase_tracks_transform_write_set() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 2U);
    const auto target = objects.front();
    const auto unrelated = objects.back();
    auto plan = plan_for(source.snapshot(), {target}, {
        {"step-1", {target, std::monostate{}}, "translate object by 0.4 0 0"},
    });
    auto task = carto::agent::AgentTask::begin(
        source, plan, translation_envelope({target}, 1.0));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run && run.value().status == carto::agent::TaskStatus::review);

    TempDirectory directory;
    const auto path = directory.path() / "transform-rebase.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);

    carto::core::Transform unrelated_transform =
        source.snapshot().objects.back().object.local_transform;
    unrelated_transform.translation.y = 3.0;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SetObjectTransformAction{
            unrelated, unrelated_transform, std::nullopt}));
    auto analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        source, checkpoint.value());
    REQUIRE(analysis && analysis.value().can_rebase());
    REQUIRE(std::find(analysis.value().write_objects.begin(),
        analysis.value().write_objects.end(), target) != analysis.value().write_objects.end());
    REQUIRE(std::find(analysis.value().write_objects.begin(),
        analysis.value().write_objects.end(), unrelated) == analysis.value().write_objects.end());
    REQUIRE(analysis.value().write_mesh_assets.empty());

    auto reauthorized_plan = checkpoint.value().plan;
    reauthorized_plan.objective.base_revision = source.snapshot().project_revision;
    auto rebased = carto::agent::AgentTask::rebase_checkpoint(
        source, checkpoint.value(), std::move(reauthorized_plan));
    REQUIRE(rebased);
    const auto rebased_snapshot = rebased.value().snapshot();
    REQUIRE(rebased_snapshot.status == carto::agent::TaskStatus::review);
    REQUIRE(rebased_snapshot.rebases.size() == 1U);
    REQUIRE(rebased_snapshot.rebases.front().reused_step_ids.size() == 1U);
    REQUIRE(rebased_snapshot.rebases.front().reused_step_ids.front() == "step-1");
    REQUIRE(rebased_snapshot.rebases.front().write_mesh_assets.empty());
    REQUIRE(source.snapshot().objects.back().object.local_transform.translation.y == 3.0);
    TempDirectory rebased_directory;
    const auto rebased_path = rebased_directory.path() / "reused.carto-agent-task";
    REQUIRE(rebased.value().save_checkpoint(rebased_path));
    const auto persisted_rebase =
        carto::agent::AgentTask::load_checkpoint(rebased_path);
    REQUIRE(persisted_rebase);
    REQUIRE(persisted_rebase.value().rebase_lineage.size() == 1U);
    REQUIRE(persisted_rebase.value().rebase_lineage.front().reused_step_ids.size() == 1U);
    REQUIRE(persisted_rebase.value().rebase_lineage.front().reused_step_ids.front() == "step-1");
    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(rebased.value()));
    const auto accepted = source.snapshot();
    const auto accepted_target = std::find_if(accepted.objects.begin(), accepted.objects.end(),
        [target](const auto& candidate) { return candidate.object.id == target; });
    REQUIRE(accepted_target != accepted.objects.end());
    REQUIRE(accepted_target->object.local_transform.translation.x == 0.4);
    REQUIRE(accepted.objects.back().object.local_transform.translation.y == 3.0);
}

void ui_objective_constraints_are_scoped_persisted_and_bound_to_answers() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Constraint target", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Outside target", {1.0, 1.0, 1.0}}));
    const auto objects = source.snapshot().objects;
    const auto target = objects.front().object.id;
    const auto outside = objects.back().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));

    REQUIRE(!ui.set_agent_hard_constraints({{
        "outside", carto::agent::ObjectiveConstraintKind::preserve_transform,
        outside, 0.0, "This target is outside the captured scope."}}));
    REQUIRE(!ui.set_agent_hard_constraints({{
        "missing-rationale", carto::agent::ObjectiveConstraintKind::preserve_topology,
        std::nullopt, 0.0, ""}}));
    REQUIRE(!ui.set_agent_hard_constraints({{
        "unknown-kind", static_cast<carto::agent::ObjectiveConstraintKind>(255U),
        std::nullopt, 0.0, "Reject unknown constraint kinds."}}));
    REQUIRE(!ui.set_agent_hard_constraints({
        {"duplicate", carto::agent::ObjectiveConstraintKind::preserve_topology,
            std::nullopt, 0.0, "Keep topology unchanged."},
        {"duplicate", carto::agent::ObjectiveConstraintKind::preserve_topology,
            std::nullopt, 0.0, "Keep topology unchanged."}}));

    const std::vector<carto::agent::HardConstraint> constraints{{
        "keep-topology", carto::agent::ObjectiveConstraintKind::preserve_topology,
        std::nullopt, 0.0, "Do not change the captured mesh topology."}};
    REQUIRE(ui.set_agent_hard_constraints(constraints));
    REQUIRE(ui.snapshot().agent_hard_constraints_draft.size() == 1U);
    REQUIRE(ui.snapshot().agent_hard_constraints_draft.front().id == "keep-topology");
    REQUIRE(ui.queue_agent_clarification(
        "Confirm that the captured target is the intended object.",
        carto::agent::ClarificationImpact::object_scope));
    const std::string goal = "Prepare only the captured target";
    REQUIRE(ui.answer_agent_clarification(
        goal, "question-1", "The selected object is the intended target."));
    REQUIRE(!ui.set_agent_hard_constraints({}));

    const auto base_revision = source.snapshot().project_revision;
    const auto run = ui.run_agent_plan(goal);
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(run.value().diagnostic->message.find("keep-topology") != std::string::npos);
    REQUIRE(source.snapshot().project_revision == base_revision);
    TempDirectory checkpoint_directory;
    const auto checkpoint_path = checkpoint_directory.path() / "hard-constraints.json";
    const auto saved_checkpoint = ui.save_agent_checkpoint(checkpoint_path);
    if (!saved_checkpoint) {
        throw TestFailure("hard-constraint checkpoint save failed: " +
            saved_checkpoint.error().message);
    }
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().plan.objective.hard_constraints.size() == 1U);
    REQUIRE(checkpoint.value().plan.objective.hard_constraints.front().id == "keep-topology");
    REQUIRE(ui.clear_agent_plan());
    REQUIRE(ui.snapshot().agent_hard_constraints_draft.empty());
}

void ui_workbench_captures_object_target_and_requires_explicit_transform_envelope() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"UI root", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    REQUIRE(ui.queue_agent_step("translate object by 0.2 0.1 0"));
    const auto queued = ui.snapshot();
    REQUIRE(queued.agent_plan_draft.size() == 1U);
    REQUIRE(std::holds_alternative<std::monostate>(
        queued.agent_plan_draft.front().target.component));

    auto envelope = translation_envelope({}, 0.5);
    envelope.scope_objects.clear();
    const auto before = source.snapshot();
    const auto run = ui.run_agent_plan("Move the selected root a small distance", envelope);
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == before.project_revision.next());
    const auto accepted = source.snapshot().viewport.scene.find(object);
    REQUIRE(accepted != nullptr);
    REQUIRE(accepted->world_transform.translation.x == 0.2);
    REQUIRE(accepted->world_transform.translation.y == 0.1);
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


void accepted_task_demonstration_is_redacted_read_only_and_checksummed() {
    TempDirectory directory;
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 2U);
    const std::string private_goal = "PRIVATE_GOAL_CANARY";
    const std::string private_intent = "inset face 0.1";
    auto plan = plan_for(source.snapshot(), objects, {
        {"PRIVATE_STEP_ID_A", {objects[0], carto::geometry::FaceId{1U}}, private_intent},
        {"PRIVATE_STEP_ID_B", {objects[1], carto::geometry::FaceId{2U}}, "extrude face 0.1"},
    });
    plan.id = "PRIVATE_PLAN_ID_CANARY";
    plan.objective.goal = private_goal;
    auto task = carto::agent::AgentTask::begin(
        source, std::move(plan), envelope_for(objects));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(!task.value().create_demonstration());

    const auto accepted = carto::application::HumanApplicationAccess::accept_agent_task(
        task.value());
    REQUIRE(accepted);
    const auto demonstration = task.value().create_demonstration();
    REQUIRE(demonstration);
    REQUIRE(demonstration.value().steps.size() == 2U);
    REQUIRE(demonstration.value().steps[0].target_alias == 1U);
    REQUIRE(demonstration.value().steps[1].target_alias == 2U);
    REQUIRE(demonstration.value().steps[0].parameters.has_value());
    REQUIRE(demonstration.value().steps[0].parameters->parameterization ==
        carto::agent::AgentDemonstrationParameterization::normalized_face_distance);
    REQUIRE(demonstration.value().steps[0].parameters->normalized_distance > 0.0);
    REQUIRE(demonstration.value().gates.size() >= 4U);
    REQUIRE(demonstration.value().validate());

    const auto encoded = demonstration.value().serialize();
    REQUIRE(encoded);
    REQUIRE(encoded.value().find(private_goal) == std::string::npos);
    REQUIRE(encoded.value().find(private_intent) == std::string::npos);
    REQUIRE(encoded.value().find("PRIVATE_STEP_ID_A") == std::string::npos);
    REQUIRE(encoded.value().find("PRIVATE_PLAN_ID_CANARY") == std::string::npos);

    const auto decoded = carto::agent::AgentDemonstration::deserialize(encoded.value());
    REQUIRE(decoded);
    REQUIRE(encoded.value().starts_with("CARTOGRAPHER_AGENT_DEMONSTRATION_V3\n"));
    REQUIRE(decoded.value().format_version == 3U);
    REQUIRE(decoded.value().steps.size() == 2U);
    REQUIRE(decoded.value().steps[0].target_alias == 1U);
    REQUIRE(decoded.value().steps[1].target_alias == 2U);
    REQUIRE(decoded.value().steps[0].parameters.has_value());
    REQUIRE(decoded.value().steps[0].parameters->parameterization ==
        carto::agent::AgentDemonstrationParameterization::normalized_face_distance);
    REQUIRE(decoded.value().objective_digest.hex() ==
        demonstration.value().objective_digest.hex());
    REQUIRE(decoded.value().diff_digest.hex() ==
        demonstration.value().diff_digest.hex());

    std::string corrupted = encoded.value();
    const std::string magic = "CARTOGRAPHER_AGENT_DEMONSTRATION_V3\n";
    const auto body_length_end = corrupted.find(':', magic.size());
    REQUIRE(body_length_end != std::string::npos);
    const auto body_start = body_length_end + 1U;
    REQUIRE(body_start < corrupted.size());
    corrupted[body_start] = corrupted[body_start] == '1' ? '2' : '1';
    REQUIRE(!carto::agent::AgentDemonstration::deserialize(corrupted));
    const auto length_end = encoded.value().find(':', magic.size());
    REQUIRE(length_end != std::string::npos);
    std::size_t body_length = 0U;
    const auto parsed_length = std::from_chars(
        encoded.value().data() + magic.size(),
        encoded.value().data() + length_end, body_length);
    REQUIRE(parsed_length.ec == std::errc{} &&
        parsed_length.ptr == encoded.value().data() + length_end);
    const auto body_begin = length_end + 1U;
    REQUIRE(body_length <= encoded.value().size() - body_begin);
    auto fields = checkpoint_fields(
        std::string_view(encoded.value()).substr(body_begin, body_length));
    REQUIRE(fields.size() > 11U);
    fields[11] = "4294967297";
    const auto malicious_body = checkpoint_body(fields);
    const auto malicious_digest = carto::assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(malicious_body.data()),
            malicious_body.size())).hex();
    const auto checksummed_alias_overflow = magic +
        std::to_string(malicious_body.size()) + ":" + malicious_body + "|" +
        std::to_string(malicious_digest.size()) + ":" + malicious_digest + "|";
    REQUIRE(!carto::agent::AgentDemonstration::deserialize(
        checksummed_alias_overflow));

    auto malicious_parameters = fields;
    REQUIRE(malicious_parameters.size() > 17U);
    malicious_parameters[17] = "9218868437227405312"; // IEEE-754 positive infinity.
    const auto malicious_parameter_body = checkpoint_body(malicious_parameters);
    const auto malicious_parameter_digest = carto::assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(malicious_parameter_body.data()),
            malicious_parameter_body.size())).hex();
    const auto checksummed_non_finite_parameter = magic +
        std::to_string(malicious_parameter_body.size()) + ":" +
        malicious_parameter_body + "|" +
        std::to_string(malicious_parameter_digest.size()) + ":" +
        malicious_parameter_digest + "|";
    REQUIRE(!carto::agent::AgentDemonstration::deserialize(
        checksummed_non_finite_parameter));

    auto compatible_v2 = demonstration.value();
    compatible_v2.format_version = 2U;
    const auto compatible_v2_bytes = compatible_v2.serialize();
    REQUIRE(compatible_v2_bytes);
    REQUIRE(compatible_v2_bytes.value().starts_with(
        "CARTOGRAPHER_AGENT_DEMONSTRATION_V2\n"));
    const auto decoded_v2 = carto::agent::AgentDemonstration::deserialize(
        compatible_v2_bytes.value());
    REQUIRE(decoded_v2);
    REQUIRE(decoded_v2.value().format_version == 2U);

    auto mismatched_parameter = demonstration.value();
    mismatched_parameter.steps.front().parameters->parameterization =
        carto::agent::AgentDemonstrationParameterization::relative_displacement;
    mismatched_parameter.steps.front().parameters->relative_displacement = {0.2, 0.0, 0.0};
    REQUIRE(!mismatched_parameter.serialize());

    auto non_finite_parameter = demonstration.value();
    non_finite_parameter.steps.front().parameters->normalized_distance =
        std::numeric_limits<double>::infinity();
    REQUIRE(!non_finite_parameter.serialize());

    auto legacy = demonstration.value();
    legacy.format_version = 1U;
    for (auto& step : legacy.steps) step.parameters.reset();
    const auto legacy_bytes = legacy.serialize();
    REQUIRE(legacy_bytes);
    REQUIRE(legacy_bytes.value().starts_with(
        "CARTOGRAPHER_AGENT_DEMONSTRATION_V1\n"));
    const auto decoded_legacy =
        carto::agent::AgentDemonstration::deserialize(legacy_bytes.value());
    REQUIRE(decoded_legacy);
    REQUIRE(decoded_legacy.value().format_version == 1U);
    REQUIRE(!decoded_legacy.value().steps.front().parameters.has_value());


    auto failed_gate = demonstration.value();
    auto required_gate = std::find_if(
        failed_gate.gates.begin(), failed_gate.gates.end(),
        [](const auto& gate) { return gate.required; });
    REQUIRE(required_gate != failed_gate.gates.end());
    required_gate->passed = false;
    REQUIRE(!failed_gate.serialize());

    auto invalid_alias = demonstration.value();
    invalid_alias.steps.front().target_alias = 0U;
    REQUIRE(!invalid_alias.serialize());

    const auto path = directory.path() / "accepted-run.carto-agent-demo";
    REQUIRE(demonstration.value().save(path));
    REQUIRE(!demonstration.value().save(path));
    const auto loaded = carto::agent::AgentDemonstration::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().gates.size() == demonstration.value().gates.size());

    carto::application::ApplicationSession viewer_source;
    carto::ui::UiController viewer(viewer_source);
    const auto viewer_revision = viewer_source.snapshot().project_revision;
    REQUIRE(viewer.load_agent_demonstration(path));
    const auto view = viewer.snapshot();
    REQUIRE(view.agent_demonstration.has_value());
    REQUIRE(view.agent_demonstration->format_version == 3U);
    REQUIRE(view.agent_demonstration->steps.front().parameters.has_value());
    REQUIRE(!view.agent_task.has_value());
    REQUIRE(viewer_source.snapshot().project_revision == viewer_revision);
    REQUIRE(!viewer.accept_agent_plan());
    REQUIRE(viewer.clear_agent_demonstration());
    REQUIRE(!viewer.snapshot().agent_demonstration.has_value());
}

void vertex_demonstration_stores_relative_displacement_only() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    const auto* instance = before.viewport.scene.find(objects.front());
    REQUIRE(instance != nullptr && instance->mesh != nullptr);
    const carto::geometry::VertexId target_vertex{1U};
    const auto vertex_position = std::find(
        instance->mesh->vertex_ids.begin(), instance->mesh->vertex_ids.end(), target_vertex);
    REQUIRE(vertex_position != instance->mesh->vertex_ids.end());
    const std::size_t vertex_index = static_cast<std::size_t>(
        std::distance(instance->mesh->vertex_ids.begin(), vertex_position));
    const auto original_position = instance->mesh->positions[vertex_index];

    auto plan = plan_for(before, objects, {
        {"step-vertex", {objects.front(), target_vertex}, "move vertex to 4 0 0"},
    });
    auto task = carto::agent::AgentTask::begin(source, std::move(plan), envelope_for(objects));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run && run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(task.value()));
    const auto demonstration = task.value().create_demonstration();
    REQUIRE(demonstration);
    const auto& parameters = demonstration.value().steps.front().parameters;
    REQUIRE(parameters.has_value());
    REQUIRE(parameters->parameterization ==
        carto::agent::AgentDemonstrationParameterization::relative_displacement);
    REQUIRE(parameters->relative_displacement.x == 4.0 - original_position.x);
    REQUIRE(parameters->relative_displacement.y == -original_position.y);
    REQUIRE(parameters->relative_displacement.z == -original_position.z);
    const auto encoded = demonstration.value().serialize();
    REQUIRE(encoded);
    REQUIRE(encoded.value().find("move vertex to 4 0 0") == std::string::npos);
}

void human_native_command_range_captures_alias_only_trace() {
    TempDirectory directory;
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    const auto first_box = ui.dispatch(carto::application::CreateBoxAction{
        "CANARY_MANUAL_TRACE_ALPHA", {1.0, 1.0, 1.0}});
    const auto second_box = ui.dispatch(carto::application::CreateBoxAction{
        "CANARY_MANUAL_TRACE_BETA", {1.0, 1.0, 1.0}});
    const auto third_box = ui.dispatch(carto::application::CreateBoxAction{
        "CANARY_MANUAL_TRACE_GAMMA", {1.0, 1.0, 1.0}});
    REQUIRE(first_box);
    REQUIRE(second_box);
    REQUIRE(third_box);
    const auto created_snapshot = source.snapshot();
    REQUIRE(created_snapshot.objects.size() == 3U);
    const std::vector<carto::scene::ObjectId> objects{
        created_snapshot.objects[0].object.id,
        created_snapshot.objects[1].object.id,
        created_snapshot.objects[2].object.id};

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        objects[0], carto::geometry::FaceId{1U}}));
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.inset-face", carto::editor::ToolArguments{0.1, {}, true, 0.5, std::nullopt}}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        objects[1], carto::geometry::FaceId{1U}}));
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.extrude-face", carto::editor::ToolArguments{0.2, {}, true, 0.5, std::nullopt}}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        objects[2], carto::geometry::FaceId{1U}}));
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.poke-face", carto::editor::ToolArguments{}}));
    auto moved_transform = source.snapshot().objects[0].object.local_transform;
    moved_transform.translation.x += 0.25;
    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        objects[0], moved_transform, std::nullopt}));

    const auto before_capture = source.snapshot();
    REQUIRE(before_capture.native_operation_trace.size() == 4U);
    REQUIRE(before_capture.native_operation_trace[0].source ==
        carto::application::OperationSource::human);
    REQUIRE(before_capture.native_operation_trace[0].tool_id == "mesh.inset-face");
    REQUIRE(before_capture.native_operation_trace[1].tool_id == "mesh.extrude-face");
    REQUIRE(before_capture.native_operation_trace[2].tool_id == "mesh.poke-face");
    REQUIRE(before_capture.native_operation_trace[2].parameterization ==
        carto::application::NativeTraceParameterization::parameterless);
    REQUIRE(before_capture.native_operation_trace[3].tool_id == "object.translate");
    REQUIRE(ui.capture_agent_demonstration_trace(
        before_capture.native_operation_trace.front().operation_id,
        before_capture.native_operation_trace.back().operation_id));
    const auto captured = ui.snapshot();
    REQUIRE(captured.agent_demonstration_trace.has_value());
    REQUIRE(captured.agent_demonstration_trace->steps.size() == 4U);
    REQUIRE(captured.agent_demonstration_trace->steps[0].target_alias == 1U);
    REQUIRE(captured.agent_demonstration_trace->steps[1].target_alias == 2U);
    REQUIRE(captured.agent_demonstration_trace->steps[2].target_alias == 3U);
    REQUIRE(captured.agent_demonstration_trace->steps[0].component_alias == 1U);
    REQUIRE(captured.agent_demonstration_trace->steps[1].component_alias == 2U);
    REQUIRE(captured.agent_demonstration_trace->steps[0].parameters.parameterization ==
        carto::agent::AgentDemonstrationParameterization::normalized_face_distance);
    REQUIRE(captured.agent_demonstration_trace->steps[2].component_alias == 3U);
    REQUIRE(captured.agent_demonstration_trace->steps[2].tool_id == "mesh.poke-face");
    REQUIRE(captured.agent_demonstration_trace->steps[2].parameters.parameterization ==
        carto::agent::AgentDemonstrationParameterization::parameterless);
    REQUIRE(captured.agent_demonstration_trace->steps[3].target_kind ==
        carto::agent::AgentDemonstrationTargetKind::object);
    REQUIRE(captured.agent_demonstration_trace->steps[3].target_alias == 1U);
    REQUIRE(captured.agent_demonstration_trace->steps[3].component_alias == 0U);
    REQUIRE(captured.agent_demonstration_trace->steps[3].parameters.relative_displacement.x ==
        0.25);
    REQUIRE(captured.agent_demonstration_trace->result_revision_span == 4U);
    REQUIRE(source.snapshot().project_revision == before_capture.project_revision);
    REQUIRE(!ui.capture_agent_demonstration_trace(
        before_capture.native_operation_trace.back().operation_id,
        before_capture.native_operation_trace.front().operation_id));
    REQUIRE(!ui.capture_agent_demonstration_trace(
        0U, before_capture.native_operation_trace.back().operation_id));

    const auto encoded = captured.agent_demonstration_trace->serialize();
    REQUIRE(encoded);
    REQUIRE(encoded.value().find("CANARY_MANUAL_TRACE_ALPHA") == std::string::npos);
    REQUIRE(encoded.value().find("CANARY_MANUAL_TRACE_BETA") == std::string::npos);
    REQUIRE(encoded.value().find("CANARY_MANUAL_TRACE_GAMMA") == std::string::npos);
    const auto decoded = carto::agent::AgentDemonstrationTrace::deserialize(encoded.value());
    REQUIRE(decoded);
    REQUIRE(decoded.value().steps.size() == 4U);
    REQUIRE(decoded.value().steps.front().target_alias == 1U);
    REQUIRE(decoded.value().steps[1].target_alias == 2U);
    REQUIRE(decoded.value().steps.back().target_alias == 1U);

    carto::agent::AgentDemonstrationRecipe recipe{
        "Manual room finish", *captured.agent_demonstration_trace};
    const auto encoded_recipe = recipe.serialize();
    REQUIRE(encoded_recipe);
    REQUIRE(encoded_recipe.value().find("CANARY_MANUAL_TRACE_ALPHA") == std::string::npos);
    REQUIRE(encoded_recipe.value().find("CANARY_MANUAL_TRACE_BETA") == std::string::npos);
    REQUIRE(encoded_recipe.value().find("CANARY_MANUAL_TRACE_GAMMA") == std::string::npos);
    const auto decoded_recipe =
        carto::agent::AgentDemonstrationRecipe::deserialize(encoded_recipe.value());
    REQUIRE(decoded_recipe);
    REQUIRE(decoded_recipe.value().name == "Manual room finish");
    REQUIRE(decoded_recipe.value().trace.steps.size() == 4U);
    auto invalid_recipe_name = recipe;
    invalid_recipe_name.name = "invalid\nrecipe";
    REQUIRE(!invalid_recipe_name.serialize());
    std::string corrupted_recipe = encoded_recipe.value();
    corrupted_recipe.back() = corrupted_recipe.back() == '1' ? '2' : '1';
    REQUIRE(!carto::agent::AgentDemonstrationRecipe::deserialize(corrupted_recipe));

    auto invalid_parameters = decoded.value();
    invalid_parameters.steps.front().parameters.normalized_distance =
        std::numeric_limits<double>::infinity();
    REQUIRE(!invalid_parameters.serialize());
    std::string corrupted = encoded.value();
    corrupted.back() = corrupted.back() == '1' ? '2' : '1';
    REQUIRE(!carto::agent::AgentDemonstrationTrace::deserialize(corrupted));

    const auto path = directory.path() / "manual-range.carto-agent-trace";
    REQUIRE(ui.save_agent_demonstration_trace(path));
    REQUIRE(!ui.save_agent_demonstration_trace(path));
    const auto recipe_path = directory.path() / "manual-range.carto-agent-recipe";
    REQUIRE(ui.save_agent_demonstration_recipe(recipe_path));
    REQUIRE(!ui.save_agent_demonstration_recipe(recipe_path));
    REQUIRE(ui.snapshot().agent_demonstration_recipe_name == "manual-range");
    const auto saved_recipe = carto::agent::AgentDemonstrationRecipe::load(recipe_path);
    REQUIRE(saved_recipe);
    REQUIRE(saved_recipe.value().name == "manual-range");
    REQUIRE(saved_recipe.value().trace.steps.size() == 4U);
    carto::application::ApplicationSession recipe_viewer_source;
    carto::ui::UiController recipe_viewer(recipe_viewer_source);
    const auto recipe_viewer_revision = recipe_viewer_source.snapshot().project_revision;
    REQUIRE(recipe_viewer.load_agent_demonstration_recipe(recipe_path));
    const auto loaded_recipe = recipe_viewer.snapshot();
    REQUIRE(loaded_recipe.agent_demonstration_recipe_name == "manual-range");
    REQUIRE(loaded_recipe.agent_demonstration_trace->steps.size() == 4U);
    REQUIRE(!loaded_recipe.agent_demonstration_ready_to_preview);
    REQUIRE(!recipe_viewer.preview_agent_demonstration_trace());
    REQUIRE(!recipe_viewer.snapshot().agent_task.has_value());
    REQUIRE(recipe_viewer_source.snapshot().project_revision == recipe_viewer_revision);
    carto::application::ApplicationSession viewer_source;
    carto::ui::UiController viewer(viewer_source);
    const auto viewer_revision = viewer_source.snapshot().project_revision;
    REQUIRE(viewer.load_agent_demonstration_trace(path));
    REQUIRE(viewer.snapshot().agent_demonstration_trace->steps.size() == 4U);
    REQUIRE(viewer_source.snapshot().project_revision == viewer_revision);
    REQUIRE(viewer.clear_agent_demonstration_trace());
    REQUIRE(!viewer.snapshot().agent_demonstration_trace.has_value());

    REQUIRE(viewer.dispatch(carto::application::CreateBoxAction{
        "Transfer target alpha", {1.0, 1.0, 1.0}}));
    REQUIRE(viewer.dispatch(carto::application::CreateBoxAction{
        "Transfer target beta", {1.0, 1.0, 1.0}}));
    REQUIRE(viewer.dispatch(carto::application::CreateBoxAction{
        "Transfer target gamma", {1.0, 1.0, 1.0}}));
    REQUIRE(viewer.dispatch(carto::application::CreateBoxAction{
        "Transfer target delta", {1.0, 1.0, 1.0}}));
    REQUIRE(viewer.dispatch(carto::application::CreateBoxAction{
        "Transfer target epsilon", {1.0, 1.0, 1.0}}));
    REQUIRE(viewer.dispatch(carto::application::CreateBoxAction{
        "Transfer target zeta", {1.0, 1.0, 1.0}}));
    const auto target_objects = viewer_source.snapshot().objects;
    REQUIRE(target_objects.size() == 6U);
    REQUIRE(viewer.load_agent_demonstration_trace(path));
    REQUIRE(viewer.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(!viewer.bind_agent_demonstration_target(99U, 0U));
    REQUIRE(!viewer.bind_agent_demonstration_target(1U, 1U, 1U));
    REQUIRE(!viewer.bind_agent_demonstration_target(1U, 0U));
    REQUIRE(!viewer.set_agent_demonstration_target_set_count(0U));
    REQUIRE(!viewer.set_agent_demonstration_target_set_count(17U));
    REQUIRE(viewer.snapshot().agent_demonstration_target_set_count == 1U);
    REQUIRE(viewer.set_agent_demonstration_target_set_count(2U));
    REQUIRE(viewer.snapshot().agent_demonstration_bindings.size() == 8U);
    REQUIRE(!viewer.preview_agent_demonstration_trace());
    REQUIRE(!viewer.snapshot().agent_task.has_value());
    REQUIRE(viewer.dispatch(carto::application::SelectFaceAction{
        target_objects[0].object.id, carto::geometry::FaceId{1U}}));
    REQUIRE(viewer.bind_agent_demonstration_target(1U, 1U, 0U));
    REQUIRE(viewer.set_agent_demonstration_target_set_count(3U));
    const auto three_sets_unbound = viewer.snapshot();
    REQUIRE(three_sets_unbound.agent_demonstration_bindings.size() == 12U);
    REQUIRE(std::none_of(three_sets_unbound.agent_demonstration_bindings.begin(),
        three_sets_unbound.agent_demonstration_bindings.end(),
        [](const auto& binding) { return binding.bound; }));
    REQUIRE(viewer.set_agent_demonstration_target_set_count(2U));
    const auto two_sets_unbound = viewer.snapshot();
    REQUIRE(std::none_of(two_sets_unbound.agent_demonstration_bindings.begin(),
        two_sets_unbound.agent_demonstration_bindings.end(),
        [](const auto& binding) { return binding.bound; }));
    REQUIRE(viewer.bind_agent_demonstration_target(1U, 1U, 0U));
    REQUIRE(!viewer.bind_agent_demonstration_target(2U, 2U, 1U));
    REQUIRE(!viewer.bind_agent_demonstration_target(2U, 2U, 0U));
    REQUIRE(viewer.dispatch(carto::application::SelectFaceAction{
        target_objects[1].object.id, carto::geometry::FaceId{1U}}));
    REQUIRE(viewer.bind_agent_demonstration_target(2U, 2U, 0U));
    REQUIRE(viewer.dispatch(carto::application::SelectFaceAction{
        target_objects[3].object.id, carto::geometry::FaceId{1U}}));
    REQUIRE(viewer.bind_agent_demonstration_target(1U, 1U, 1U));
    REQUIRE(viewer.dispatch(carto::application::SelectFaceAction{
        target_objects[4].object.id, carto::geometry::FaceId{1U}}));
    REQUIRE(viewer.bind_agent_demonstration_target(2U, 2U, 1U));
    REQUIRE(viewer.dispatch(carto::application::SelectFaceAction{
        target_objects[2].object.id, carto::geometry::FaceId{1U}}));
    const auto first_poke_binding = viewer.bind_agent_demonstration_target(3U, 3U, 0U);
    if (!first_poke_binding) {
        throw TestFailure("face-poke target binding failed: " +
            first_poke_binding.error().message);
    }
    REQUIRE(viewer.dispatch(carto::application::SelectFaceAction{
        target_objects[5].object.id, carto::geometry::FaceId{1U}}));
    REQUIRE(viewer.bind_agent_demonstration_target(3U, 3U, 1U));
    REQUIRE(viewer.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(viewer.dispatch(carto::application::SelectObjectAction{
        target_objects[0].object.id}));
    REQUIRE(viewer.bind_agent_demonstration_target(1U, 0U, 0U));
    REQUIRE(viewer.dispatch(carto::application::SelectObjectAction{
        target_objects[3].object.id}));
    REQUIRE(viewer.bind_agent_demonstration_target(1U, 0U, 1U));
    const auto fully_bound = viewer.snapshot();
    REQUIRE(fully_bound.agent_demonstration_ready_to_preview);
    REQUIRE(std::all_of(fully_bound.agent_demonstration_bindings.begin(),
        fully_bound.agent_demonstration_bindings.end(),
        [](const auto& binding) { return binding.bound; }));
    const auto target_baseline = viewer_source.snapshot();
    std::vector<std::size_t> target_triangle_counts;
    for (const auto& target : target_objects) {
        const auto* instance = target_baseline.viewport.scene.find(target.object.id);
        REQUIRE(instance != nullptr && instance->mesh != nullptr);
        target_triangle_counts.push_back(instance->mesh->triangle_faces.size());
    }
    const auto target_revision = viewer_source.snapshot().project_revision;
    const auto trace_preview = viewer.preview_agent_demonstration_trace();
    if (!trace_preview) {
        throw TestFailure("demonstration trace preview failed: " +
            trace_preview.error().message);
    }
    REQUIRE(trace_preview.value().status == carto::agent::TaskStatus::review);
    REQUIRE(trace_preview.value().plan_steps.size() == 8U);
    REQUIRE(trace_preview.value().scope_objects.size() == 6U);
    const auto& expanded_steps = trace_preview.value().plan_steps;
    const auto set_one_second = std::find_if(expanded_steps.begin(), expanded_steps.end(),
        [](const auto& step) { return step.step_id == "demonstration-set-1-step-2"; });
    const auto set_one_third = std::find_if(expanded_steps.begin(), expanded_steps.end(),
        [](const auto& step) { return step.step_id == "demonstration-set-1-step-3"; });
    const auto set_one_fourth = std::find_if(expanded_steps.begin(), expanded_steps.end(),
        [](const auto& step) { return step.step_id == "demonstration-set-1-step-4"; });
    const auto set_two_first = std::find_if(expanded_steps.begin(), expanded_steps.end(),
        [](const auto& step) { return step.step_id == "demonstration-set-2-step-1"; });
    const auto set_two_second = std::find_if(expanded_steps.begin(), expanded_steps.end(),
        [](const auto& step) { return step.step_id == "demonstration-set-2-step-2"; });
    const auto set_two_third = std::find_if(expanded_steps.begin(), expanded_steps.end(),
        [](const auto& step) { return step.step_id == "demonstration-set-2-step-3"; });
    const auto set_two_fourth = std::find_if(expanded_steps.begin(), expanded_steps.end(),
        [](const auto& step) { return step.step_id == "demonstration-set-2-step-4"; });
    REQUIRE(set_one_second != expanded_steps.end());
    REQUIRE(set_one_third != expanded_steps.end());
    REQUIRE(set_one_fourth != expanded_steps.end());
    REQUIRE(set_two_first != expanded_steps.end());
    REQUIRE(set_two_second != expanded_steps.end());
    REQUIRE(set_two_third != expanded_steps.end());
    REQUIRE(set_two_fourth != expanded_steps.end());
    REQUIRE(set_one_second->dependencies ==
        std::vector<std::string>{"demonstration-set-1-step-1"});
    REQUIRE(set_one_third->dependencies ==
        std::vector<std::string>{"demonstration-set-1-step-2"});
    REQUIRE(set_one_fourth->dependencies ==
        std::vector<std::string>{"demonstration-set-1-step-3"});
    REQUIRE(set_two_first->dependencies.empty());
    REQUIRE(set_two_second->dependencies ==
        std::vector<std::string>{"demonstration-set-2-step-1"});
    REQUIRE(set_two_third->dependencies ==
        std::vector<std::string>{"demonstration-set-2-step-2"});
    REQUIRE(set_two_fourth->dependencies ==
        std::vector<std::string>{"demonstration-set-2-step-3"});
    REQUIRE(!viewer.set_agent_demonstration_target_set_count(1U));
    REQUIRE(viewer_source.snapshot().project_revision == target_revision);
    const auto accepted_trace = viewer.accept_agent_plan();
    REQUIRE(accepted_trace);
    REQUIRE(viewer_source.snapshot().project_revision > target_revision);
    const auto accepted_snapshot = viewer_source.snapshot();
    for (const std::size_t target_index : {0U, 3U}) {
        const auto& before_object = target_objects[target_index];
        const auto after_object = std::find_if(accepted_snapshot.objects.begin(),
            accepted_snapshot.objects.end(), [&before_object](const auto& value) {
                return value.object.id == before_object.object.id;
            });
        REQUIRE(after_object != accepted_snapshot.objects.end());
        REQUIRE(after_object->object.local_transform.translation.x ==
            before_object.object.local_transform.translation.x + 0.25);
    }
    for (std::size_t target_index = 0U; target_index < target_objects.size(); ++target_index) {
        const auto* after_instance = accepted_snapshot.viewport.scene.find(
            target_objects[target_index].object.id);
        REQUIRE(after_instance != nullptr && after_instance->mesh != nullptr);
        REQUIRE(after_instance->mesh->triangle_faces.size() != target_triangle_counts[target_index]);
    }
    REQUIRE(viewer.clear_agent_plan());
    REQUIRE(!viewer.preview_agent_demonstration_trace());
    const auto stale_bindings = viewer.snapshot();
    REQUIRE(!stale_bindings.agent_demonstration_ready_to_preview);
    REQUIRE(std::none_of(stale_bindings.agent_demonstration_bindings.begin(),
        stale_bindings.agent_demonstration_bindings.end(),
        [](const auto& binding) { return binding.bound; }));

    carto::application::ApplicationSession ai_source;
    carto::ui::UiController ai_ui(ai_source);
    REQUIRE(ai_ui.dispatch(carto::application::CreateBoxAction{
        "AI boundary target", {1.0, 1.0, 1.0}}));
    const auto ai_object = ai_source.snapshot().objects.front().object.id;
    const auto ai_face = ai_source.snapshot().viewport.scene.instances().front()
        .mesh->triangle_faces.front();
    REQUIRE(ai_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ai_ui.dispatch(carto::application::SelectFaceAction{ai_object, ai_face}));

    const auto proposal = ai_ui.propose_ai("inset face 0.10");
    REQUIRE(proposal);
    auto preview = ai_ui.begin_ai_preview(proposal.value());
    REQUIRE(preview);
    const auto committed = ai_ui.commit_ai_preview(preview.value(), proposal.value());
    REQUIRE(committed);
    REQUIRE(committed.value().source == carto::application::OperationSource::ai_proposal);
    REQUIRE(ai_source.snapshot().native_operation_trace.empty());

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Unrelated edit clears recent trace", {1.0, 1.0, 1.0}}));
    REQUIRE(source.snapshot().native_operation_trace.empty());
}

void native_demonstrations_apply_to_unique_exact_structural_matches() {
    TempDirectory directory;
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreatePlaneAction{
        "CANARY_DEMO_SOURCE_NAME", 2.0, 2.0}));
    const auto source_object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        source_object, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.inset-face", carto::editor::ToolArguments{0.1, {}, true, 0.5, std::nullopt}}));
    const auto captured_operations = source.snapshot().native_operation_trace;
    REQUIRE(captured_operations.size() == 1U);
    REQUIRE(ui.capture_agent_demonstration_trace(
        captured_operations.front().operation_id, captured_operations.front().operation_id));
    const auto captured = ui.snapshot().agent_demonstration_trace;
    REQUIRE(captured.has_value());
    REQUIRE(captured->steps.front().structure_signature.has_value());
    REQUIRE(captured->steps.front().result_object_structure_signature.has_value());
    const auto encoded = captured->serialize();
    REQUIRE(encoded);
    REQUIRE(encoded.value().find("CANARY_DEMO_SOURCE_NAME") == std::string::npos);
    REQUIRE(encoded.value().find("E:/HiddenCanopy") == std::string::npos);

    const auto field = [](std::string_view value) {
        return std::to_string(value.size()) + ":" + std::string(value) + "|";
    };
    const auto number = [&field](std::uint64_t value) {
        return field(std::to_string(value));
    };
    std::string legacy_body;
    legacy_body += number(1U);
    legacy_body += number(1U);
    legacy_body += number(1U);
    legacy_body += field("object.translate");
    legacy_body += number(0U);
    legacy_body += number(1U);
    legacy_body += number(0U);
    legacy_body += number(0U);
    legacy_body += number(1U);
    legacy_body += number(1U);
    legacy_body += number(std::bit_cast<std::uint64_t>(0.5));
    legacy_body += number(std::bit_cast<std::uint64_t>(0.0));
    legacy_body += number(std::bit_cast<std::uint64_t>(0.0));
    const auto* legacy_bytes = reinterpret_cast<const std::uint8_t*>(legacy_body.data());
    const auto legacy_digest = carto::assets::sha256(
        std::span<const std::uint8_t>(legacy_bytes, legacy_body.size()));
    const std::string legacy_trace =
        "CARTOGRAPHER_AGENT_DEMONSTRATION_TRACE_V1\n" +
        field(legacy_body) + field(legacy_digest.hex());
    const auto legacy_decoded = carto::agent::AgentDemonstrationTrace::deserialize(legacy_trace);
    REQUIRE(legacy_decoded);
    REQUIRE(legacy_decoded.value().steps.size() == 1U);
    REQUIRE(!legacy_decoded.value().steps.front().structure_signature.has_value());
    REQUIRE(!legacy_decoded.value().steps.front().result_object_structure_signature.has_value());

    const auto trace_path = directory.path() / "structural-demo.carto-agent-trace";
    REQUIRE(ui.save_agent_demonstration_trace(trace_path));
    REQUIRE(ui.clear_agent_demonstration_trace());
    REQUIRE(ui.load_agent_demonstration_trace(trace_path));

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{source_object}));
    REQUIRE(!ui.apply_agent_demonstration_to_similar());
    REQUIRE(ui.snapshot().agent_demonstration_bindings.size() == 1U);
    REQUIRE(!ui.snapshot().agent_demonstration_bindings.front().bound);

    REQUIRE(ui.dispatch(carto::application::CreatePlaneAction{
        "CANARY_DEMO_TARGET_ONE", 3.0, 2.0}));
    REQUIRE(ui.dispatch(carto::application::CreatePlaneAction{
        "CANARY_DEMO_TARGET_TWO", 4.0, 2.0}));
    REQUIRE(ui.dispatch(carto::application::CreatePlaneAction{
        "CANARY_DEMO_TARGET_THREE", 5.0, 2.0}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Different topology distractor", {1.0, 1.0, 1.0}}));
    const auto objects = source.snapshot().objects;
    REQUIRE(objects.size() == 5U);
    const auto first_target = objects[1].object.id;
    REQUIRE(ui.dispatch(carto::application::DuplicateMeshInstanceAction{
        first_target, "CANARY_SHARED_TARGET_COPY", {2.0, 0.0, 0.0}}));
    const auto shared_copy = source.snapshot().objects.back().object.id;
    auto moved_target = objects[1].object.local_transform;
    moved_target.translation.x += 5.0;
    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        first_target, moved_target, std::nullopt}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{first_target}));
    REQUIRE(!ui.apply_agent_demonstration_to_similar());

    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{source_object}));
    const auto target_set_count = ui.apply_agent_demonstration_to_similar();
    REQUIRE(target_set_count && target_set_count.value() == 2U);
    const auto bound = ui.snapshot();
    REQUIRE(bound.agent_demonstration_target_set_count == 2U);
    REQUIRE(bound.agent_demonstration_bindings.size() == 2U);
    REQUIRE(std::all_of(bound.agent_demonstration_bindings.begin(),
        bound.agent_demonstration_bindings.end(),
        [](const auto& binding) { return binding.bound; }));
    REQUIRE(bound.agent_demonstration_bindings[0].target_set_index == 0U);
    REQUIRE(bound.agent_demonstration_bindings[1].target_set_index == 1U);
    REQUIRE(bound.agent_demonstration_bindings[0].target_label.find("CANARY_DEMO_TARGET_TWO") !=
        std::string::npos);
    REQUIRE(bound.agent_demonstration_bindings[1].target_label.find("CANARY_DEMO_TARGET_THREE") !=
        std::string::npos);

    const auto revision_before_preview = source.snapshot().project_revision;
    const auto preview = ui.preview_agent_demonstration_trace();
    REQUIRE(preview);
    REQUIRE(preview.value().status == carto::agent::TaskStatus::review);
    REQUIRE(preview.value().plan_steps.size() == 2U);
    REQUIRE(preview.value().scope_objects.size() == 2U);
    const auto preview_plan = ui.snapshot().agent_plan_draft;
    REQUIRE(preview_plan.size() == 2U);
    REQUIRE(preview_plan[0].target.object != preview_plan[1].target.object);
    REQUIRE(preview_plan[0].target.object != first_target);
    REQUIRE(preview_plan[1].target.object != first_target);
    REQUIRE(preview_plan[0].target.object != shared_copy);
    REQUIRE(preview_plan[1].target.object != shared_copy);
    REQUIRE(preview.value().can_accept);
    REQUIRE(source.snapshot().project_revision == revision_before_preview);
}

void native_demonstration_multi_command_translations_match_similar_objects() {
    TempDirectory directory;
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreatePlaneAction{
        "CANARY_TRANSLATION_EXEMPLAR", 2.0, 2.0}));
    const auto exemplar = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{exemplar}));

    auto transform = source.snapshot().objects.front().object.local_transform;
    transform.translation.x += 0.5;
    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        exemplar, transform, std::nullopt}));
    transform = source.snapshot().objects.front().object.local_transform;
    transform.translation.z += 0.25;
    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        exemplar, transform, std::nullopt}));

    const auto captured_operations = source.snapshot().native_operation_trace;
    REQUIRE(captured_operations.size() == 2U);
    REQUIRE(ui.capture_agent_demonstration_trace(
        captured_operations.front().operation_id,
        captured_operations.back().operation_id));
    const auto captured = ui.snapshot().agent_demonstration_trace;
    REQUIRE(captured.has_value());
    REQUIRE(captured->steps.size() == 2U);
    REQUIRE(ui.snapshot().agent_demonstration_similar_targets_supported);

    auto mismatched = *captured;
    REQUIRE(mismatched.steps[1].structure_signature.has_value());
    mismatched.steps[1].structure_signature->bytes[0] ^= 0x01U;
    const auto mismatched_path = directory.path() / "mismatched-translation-trace.carto-agent-trace";
    REQUIRE(mismatched.save(mismatched_path));
    REQUIRE(ui.load_agent_demonstration_trace(mismatched_path));
    REQUIRE(!ui.snapshot().agent_demonstration_similar_targets_supported);
    REQUIRE(!ui.apply_agent_demonstration_to_similar());

    const auto valid_path = directory.path() / "valid-translation-trace.carto-agent-trace";
    REQUIRE(captured->save(valid_path));
    REQUIRE(ui.load_agent_demonstration_trace(valid_path));
    REQUIRE(ui.dispatch(carto::application::CreatePlaneAction{
        "CANARY_TRANSLATION_TARGET", 2.0, 2.0}));
    const auto target = source.snapshot().objects.back().object.id;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{exemplar}));

    const auto bound_count = ui.apply_agent_demonstration_to_similar();
    REQUIRE(bound_count && bound_count.value() == 1U);
    const auto bound = ui.snapshot();
    REQUIRE(bound.agent_demonstration_bindings.size() == 1U);
    REQUIRE(bound.agent_demonstration_bindings.front().bound);
    REQUIRE(bound.agent_demonstration_bindings.front().target_label.find(
        "CANARY_TRANSLATION_TARGET") != std::string::npos);

    const auto source_revision = source.snapshot().project_revision;
    const auto preview = ui.preview_agent_demonstration_trace();
    REQUIRE(preview);
    REQUIRE(preview.value().status == carto::agent::TaskStatus::review);
    REQUIRE(preview.value().plan_steps.size() == 2U);
    const auto preview_plan = ui.snapshot().agent_plan_draft;
    REQUIRE(preview_plan.size() == 2U);
    REQUIRE(preview_plan[0].target.object == target);
    REQUIRE(preview_plan[1].target.object == target);
    REQUIRE(source.snapshot().project_revision == source_revision);
}

void native_demonstration_multi_command_vertex_edits_match_and_accumulate() {
    TempDirectory directory;
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Vertex exemplar", {1.0, 1.0, 1.0}}));
    const auto exemplar = source.snapshot().objects.back().object.id;
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Vertex candidate", {1.0, 1.0, 1.0}}));
    const auto candidate = source.snapshot().objects.back().object.id;

    const auto split_edge = [&ui, &source](carto::scene::ObjectId object) {
        const auto before = source.snapshot();
        const auto* instance = before.viewport.scene.find(object);
        REQUIRE(instance != nullptr && instance->mesh != nullptr);
        const auto previous_vertices = instance->mesh->vertex_ids;
        const auto edge = instance->mesh->triangle_edges.front()[0];
        REQUIRE(edge.has_value());
        REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::edge}));
        REQUIRE(ui.dispatch(carto::application::SelectEdgeAction{object, *edge}));
        carto::editor::ToolArguments split_arguments;
        split_arguments.factor = 0.5;
        REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
            "mesh.split-edge", split_arguments}));
        const auto after = source.snapshot();
        const auto* split_instance = after.viewport.scene.find(object);
        REQUIRE(split_instance != nullptr && split_instance->mesh != nullptr);
        const auto inserted = std::find_if(
            split_instance->mesh->vertex_ids.begin(), split_instance->mesh->vertex_ids.end(),
            [&previous_vertices](carto::geometry::VertexId vertex) {
                return std::find(previous_vertices.begin(), previous_vertices.end(), vertex) ==
                    previous_vertices.end();
            });
        REQUIRE(inserted != split_instance->mesh->vertex_ids.end());
        return *inserted;
    };
    const auto exemplar_vertex = split_edge(exemplar);
    const auto candidate_vertex = split_edge(candidate);
    const auto pre_edit = source.snapshot();
    const auto exemplar_mesh = pre_edit.viewport.scene.find(exemplar)->mesh;
    const auto exemplar_position = std::find(
        exemplar_mesh->vertex_ids.begin(), exemplar_mesh->vertex_ids.end(), exemplar_vertex);
    REQUIRE(exemplar_position != exemplar_mesh->vertex_ids.end());
    const auto exemplar_vertex_index = static_cast<std::size_t>(std::distance(
        exemplar_mesh->vertex_ids.begin(), exemplar_position));
    const auto first_operation_index = pre_edit.native_operation_trace.size();

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{exemplar, exemplar_vertex}));
    carto::editor::ToolArguments first_move;
    first_move.position = exemplar_mesh->positions[exemplar_vertex_index] +
        carto::core::Vec3d{0.1, 0.0, 0.0};
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.set-vertex-position", first_move}));

    const auto after_first = source.snapshot();
    const auto first_mesh = after_first.viewport.scene.find(exemplar)->mesh;
    const auto first_vertex_position = std::find(
        first_mesh->vertex_ids.begin(), first_mesh->vertex_ids.end(), exemplar_vertex);
    REQUIRE(first_vertex_position != first_mesh->vertex_ids.end());
    const auto first_vertex_index = static_cast<std::size_t>(std::distance(
        first_mesh->vertex_ids.begin(), first_vertex_position));
    carto::editor::ToolArguments second_move;
    second_move.position = first_mesh->positions[first_vertex_index] +
        carto::core::Vec3d{0.0, 0.2, 0.0};
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{exemplar, exemplar_vertex}));
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.set-vertex-position", second_move}));

    const auto captured_operations = source.snapshot().native_operation_trace;
    REQUIRE(captured_operations.size() == first_operation_index + 2U);
    REQUIRE(ui.capture_agent_demonstration_trace(
        captured_operations[first_operation_index].operation_id,
        captured_operations.back().operation_id));
    const auto captured = ui.snapshot();
    REQUIRE(captured.agent_demonstration_trace.has_value());
    REQUIRE(captured.agent_demonstration_trace->steps.size() == 2U);
    REQUIRE(captured.agent_demonstration_trace->steps[0].target_kind ==
        carto::agent::AgentDemonstrationTargetKind::vertex);
    REQUIRE(captured.agent_demonstration_trace->steps[0].target_alias ==
        captured.agent_demonstration_trace->steps[1].target_alias);
    REQUIRE(captured.agent_demonstration_trace->steps[0].component_alias ==
        captured.agent_demonstration_trace->steps[1].component_alias);
    REQUIRE(captured.agent_demonstration_similar_targets_supported);

    auto mismatched_trace = *captured.agent_demonstration_trace;
    REQUIRE(mismatched_trace.steps[1].structure_signature.has_value());
    mismatched_trace.steps[1].structure_signature->bytes[0] ^= 0x01U;
    const auto mismatched_path = directory.path() / "mismatched-vertex-trace.carto-agent-trace";
    REQUIRE(mismatched_trace.save(mismatched_path));
    REQUIRE(ui.load_agent_demonstration_trace(mismatched_path));
    REQUIRE(!ui.snapshot().agent_demonstration_similar_targets_supported);
    REQUIRE(!ui.apply_agent_demonstration_to_similar());
    const auto valid_path = directory.path() / "valid-vertex-trace.carto-agent-trace";
    REQUIRE(captured.agent_demonstration_trace->save(valid_path));
    REQUIRE(ui.load_agent_demonstration_trace(valid_path));

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{exemplar}));
    const auto target_set_count = ui.apply_agent_demonstration_to_similar();
    REQUIRE(target_set_count && target_set_count.value() == 1U);

    const auto candidate_before = source.snapshot().viewport.scene.find(candidate)->mesh;
    const auto candidate_vertex_position = std::find(
        candidate_before->vertex_ids.begin(), candidate_before->vertex_ids.end(),
        candidate_vertex);
    REQUIRE(candidate_vertex_position != candidate_before->vertex_ids.end());
    const auto candidate_vertex_index = static_cast<std::size_t>(std::distance(
        candidate_before->vertex_ids.begin(), candidate_vertex_position));
    const auto expected_position = candidate_before->positions[candidate_vertex_index] +
        carto::core::Vec3d{0.1, 0.2, 0.0};
    const auto source_revision = source.snapshot().project_revision;
    const auto preview = ui.preview_agent_demonstration_trace();
    REQUIRE(preview);
    REQUIRE(preview.value().status == carto::agent::TaskStatus::review);
    REQUIRE(preview.value().can_accept);
    REQUIRE(preview.value().plan_steps.size() == 2U);
    const auto preview_plan = ui.snapshot().agent_plan_draft;
    REQUIRE(preview_plan.size() == 2U);
    REQUIRE(preview_plan[0].target.object == candidate);
    REQUIRE(preview_plan[1].target.object == candidate);
    const auto* preview_vertex = std::get_if<carto::geometry::VertexId>(
        &preview_plan.front().target.component);
    REQUIRE(preview_vertex != nullptr && *preview_vertex == candidate_vertex);
    const auto* after_instance = preview.value().after_scene.find(candidate);
    REQUIRE(after_instance != nullptr && after_instance->mesh != nullptr);
    const auto after_vertex_position = std::find(
        after_instance->mesh->vertex_ids.begin(), after_instance->mesh->vertex_ids.end(),
        candidate_vertex);
    REQUIRE(after_vertex_position != after_instance->mesh->vertex_ids.end());
    const auto after_vertex_index = static_cast<std::size_t>(std::distance(
        after_instance->mesh->vertex_ids.begin(), after_vertex_position));
    REQUIRE(after_instance->mesh->positions[after_vertex_index].x == expected_position.x);
    REQUIRE(after_instance->mesh->positions[after_vertex_index].y == expected_position.y);
    REQUIRE(after_instance->mesh->positions[after_vertex_index].z == expected_position.z);
    REQUIRE(source.snapshot().project_revision == source_revision);
}

void native_demonstrations_skip_ambiguous_structural_matches() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Symmetric source", {1.0, 1.0, 1.0}}));
    const auto source_object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        source_object, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.inset-face", carto::editor::ToolArguments{0.1, {}, true, 0.5, std::nullopt}}));
    const auto operation = source.snapshot().native_operation_trace.back().operation_id;
    REQUIRE(ui.capture_agent_demonstration_trace(operation, operation));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Symmetric candidate", {2.0, 2.0, 2.0}}));
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{source_object}));
    REQUIRE(!ui.apply_agent_demonstration_to_similar());
    const auto snapshot = ui.snapshot();
    REQUIRE(snapshot.agent_demonstration_target_set_count == 1U);
    REQUIRE(snapshot.agent_demonstration_bindings.size() == 1U);
    REQUIRE(!snapshot.agent_demonstration_bindings.front().bound);
    REQUIRE(!snapshot.agent_task.has_value());
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
    REQUIRE(analysis.value().write_objects.empty());
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
    REQUIRE(rebased_snapshot.rebases.front().reused_step_ids.size() == 1U);
    REQUIRE(rebased_snapshot.rebases.front().reused_step_ids.front() == "step-1");
    REQUIRE(stale_source.snapshot().project_revision == current_revision);

    auto tampered_checkpoint = stale_checkpoint.value();
    const auto name_field = tampered_checkpoint.speculative_project.find("NAME ");
    REQUIRE(name_field != std::string::npos);
    const auto name_end = tampered_checkpoint.speculative_project.find('\n', name_field);
    REQUIRE(name_end != std::string::npos);
    tampered_checkpoint.speculative_project.replace(
        name_field, name_end - name_field, "NAME \"Tampered checkpoint\"");
    tampered_checkpoint.speculative_digest = carto::assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                tampered_checkpoint.speculative_project.data()),
            tampered_checkpoint.speculative_project.size()));
    REQUIRE(tampered_checkpoint.validate());
    auto tampered_plan = tampered_checkpoint.plan;
    tampered_plan.objective.base_revision = current_revision;
    const auto rejected_artifact = carto::agent::AgentTask::rebase_checkpoint(
        stale_source, tampered_checkpoint, tampered_plan);
    REQUIRE(!rejected_artifact);
    REQUIRE(rejected_artifact.error().code == carto::core::ErrorCode::stale_data);
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
    const auto conflict_initial_snapshot = conflict_source.snapshot();
    const auto* conflict_mesh_before = conflict_initial_snapshot.viewport.scene.find(
        conflict_objects.front());
    REQUIRE(conflict_mesh_before != nullptr && conflict_mesh_before->mesh != nullptr);
    const auto conflict_face_count_before =
        conflict_mesh_before->mesh->triangle_faces.size();
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
    REQUIRE(conflict_analysis.value().can_rebase());
    REQUIRE(conflict_analysis.value().conflicting_objects.empty());
    REQUIRE(conflict_analysis.value().directly_conflicting_step_ids ==
        std::vector<std::string>{"step-1"});
    auto conflict_reauthorized_plan = conflict_checkpoint.value().plan;
    conflict_reauthorized_plan.objective.base_revision =
        conflict_source.snapshot().project_revision;
    auto rebased_target_transform =
        carto::agent::AgentTask::rebase_checkpoint(
            conflict_source, conflict_checkpoint.value(), conflict_reauthorized_plan);
    REQUIRE(rebased_target_transform);
    REQUIRE(rebased_target_transform.value().snapshot().status ==
        carto::agent::TaskStatus::review);
    REQUIRE(conflict_source.snapshot().project_revision ==
        conflict_analysis.value().current_revision);
    REQUIRE(conflict_source.snapshot().objects.front().object.local_transform.translation.x ==
        changed_transform.translation.x);
    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(
        rebased_target_transform.value()));
    const auto conflict_accepted = conflict_source.snapshot();
    const auto* conflict_accepted_target =
        conflict_accepted.viewport.scene.find(conflict_objects.front());
    REQUIRE(conflict_accepted_target != nullptr && conflict_accepted_target->mesh != nullptr);
    REQUIRE(conflict_accepted_target->mesh->triangle_faces.size() > conflict_face_count_before);

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

void checkpoint_rebase_reexecutes_only_the_affected_dag_frontier() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 3U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-a", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
        {"step-b", {objects[1], carto::geometry::FaceId{1U}}, "inset face 0.1", {"step-a"}},
        {"step-c", {objects[2], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    const auto original_source = source.snapshot();
    const auto* original_c = original_source.viewport.scene.find(objects[2]);
    REQUIRE(original_c != nullptr && original_c->mesh != nullptr);
    const auto original_faces = original_c->mesh->triangle_faces.size();
    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    const auto original_run = task.value().run();
    REQUIRE(original_run && original_run.value().status == carto::agent::TaskStatus::review);

    TempDirectory directory;
    const auto path = directory.path() / "affected-frontier.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);
    const auto original_step_c = std::find_if(
        checkpoint.value().step_receipts.begin(), checkpoint.value().step_receipts.end(),
        [](const auto& receipt) { return receipt.step_id == "step-c"; });
    REQUIRE(original_step_c != checkpoint.value().step_receipts.end());

    auto target_snapshot = source.snapshot();
    const auto target = std::find_if(target_snapshot.objects.begin(),
        target_snapshot.objects.end(), [&objects](const auto& object) {
            return object.object.id == objects[0];
        });
    REQUIRE(target != target_snapshot.objects.end());
    auto changed_transform = target->object.local_transform;
    for (std::size_t edit = 0U; edit < 4U; ++edit) {
        changed_transform.translation.x += 0.1;
        REQUIRE(carto::application::HumanApplicationAccess::dispatch(
            source, carto::application::SetObjectTransformAction{
                objects[0], changed_transform, std::nullopt}));
    }
    const auto current_revision = source.snapshot().project_revision;
    const auto analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        source, checkpoint.value());
    REQUIRE(analysis && analysis.value().can_rebase());
    REQUIRE(analysis.value().directly_conflicting_step_ids ==
        std::vector<std::string>{"step-a"});
    REQUIRE(analysis.value().invalidated_step_ids ==
        (std::vector<std::string>{"step-a", "step-b"}));

    auto reauthorized = checkpoint.value().plan;
    reauthorized.objective.base_revision = current_revision;
    auto rebased = carto::agent::AgentTask::rebase_checkpoint(
        source, checkpoint.value(), std::move(reauthorized));
    REQUIRE(rebased);
    const auto snapshot = rebased.value().snapshot();
    REQUIRE(snapshot.status == carto::agent::TaskStatus::review);
    REQUIRE(snapshot.can_accept);
    REQUIRE(snapshot.rebases.size() == 1U);
    REQUIRE(snapshot.rebases.front().reused_step_ids ==
        std::vector<std::string>{"step-c"});
    const auto current_source_snapshot = source.snapshot();
    REQUIRE(current_source_snapshot.project_revision == current_revision);
    const auto still_current = std::find_if(current_source_snapshot.objects.begin(),
        current_source_snapshot.objects.end(), [&objects](const auto& object) {
            return object.object.id == objects[0];
        });
    REQUIRE(still_current != current_source_snapshot.objects.end());
    REQUIRE(still_current->object.local_transform.translation.x ==
        changed_transform.translation.x);
    const auto step_a = std::find_if(snapshot.steps.begin(), snapshot.steps.end(),
        [](const auto& receipt) { return receipt.step_id == "step-a"; });
    const auto step_b = std::find_if(snapshot.steps.begin(), snapshot.steps.end(),
        [](const auto& receipt) { return receipt.step_id == "step-b"; });
    const auto step_c = std::find_if(snapshot.steps.begin(), snapshot.steps.end(),
        [](const auto& receipt) { return receipt.step_id == "step-c"; });
    REQUIRE(step_a != snapshot.steps.end() && step_b != snapshot.steps.end() &&
        step_c != snapshot.steps.end());
    REQUIRE(step_a->revision_before == current_revision);
    REQUIRE(step_b->revision_before == current_revision.next());
    REQUIRE(step_c->revision_before == original_step_c->revision_before);
    REQUIRE(snapshot.speculative_revision == current_revision.next().next().next());

    const auto rebased_path = directory.path() / "affected-frontier-rebased.carto-agent-task";
    REQUIRE(rebased.value().save_checkpoint(rebased_path));
    const auto rebased_checkpoint = carto::agent::AgentTask::load_checkpoint(rebased_path);
    REQUIRE(rebased_checkpoint);
    const auto resumed = carto::agent::AgentTask::resume_checkpoint(
        source, rebased_checkpoint.value(), rebased_checkpoint.value().plan);
    REQUIRE(resumed);
    REQUIRE(resumed.value().snapshot().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().snapshot().can_accept);
    REQUIRE(resumed.value().snapshot().diff.digest == snapshot.diff.digest);
    REQUIRE(carto::application::HumanApplicationAccess::accept_agent_task(rebased.value()));
    REQUIRE(source.snapshot().project_revision == current_revision.next());
    const auto accepted_snapshot = source.snapshot();
    const auto* accepted_c = accepted_snapshot.viewport.scene.find(objects[2]);
    REQUIRE(accepted_c != nullptr && accepted_c->mesh != nullptr);
    REQUIRE(accepted_c->mesh->triangle_faces.size() > original_faces);
}

void checkpoint_rebase_blocks_changed_task_write_targets() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-1", {objects.front(), std::monostate{}}, "translate object by 0.4 0 0"},
    });
    auto task = carto::agent::AgentTask::begin(
        source, plan, translation_envelope(objects, 1.0));
    REQUIRE(task);
    REQUIRE(task.value().run());
    TempDirectory directory;
    const auto path = directory.path() / "transform-write-conflict.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);

    const auto before = source.snapshot().objects.front().object.local_transform;
    auto changed = before;
    changed.translation.y += 0.25;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SetObjectTransformAction{
            objects.front(), changed, std::nullopt}));
    const auto analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        source, checkpoint.value());
    REQUIRE(analysis);
    REQUIRE(!analysis.value().can_rebase());
    REQUIRE(analysis.value().conflicting_objects == objects);
    REQUIRE(std::any_of(analysis.value().blockers.begin(), analysis.value().blockers.end(),
        [](const auto& blocker) { return blocker.find("write object") != std::string::npos; }));
    auto reauthorized = checkpoint.value().plan;
    reauthorized.objective.base_revision = source.snapshot().project_revision;
    const auto blocked = carto::agent::AgentTask::rebase_checkpoint(
        source, checkpoint.value(), std::move(reauthorized));
    REQUIRE(!blocked);
    REQUIRE(blocked.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(source.snapshot().objects.front().object.local_transform.translation.y ==
        changed.translation.y);
}

void checkpoint_rebase_reports_and_executes_only_the_affected_dag_branch() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 3U);
    auto plan = plan_for(source.snapshot(), objects, {
        {"step-a", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
        {"step-b", {objects[1], carto::geometry::FaceId{1U}}, "inset face 0.1", {"step-a"}},
        {"step-c", {objects[2], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto task = carto::agent::AgentTask::begin(source, plan, envelope_for(objects));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run && run.value().status == carto::agent::TaskStatus::review);

    TempDirectory directory;
    const auto path = directory.path() / "dag-rebase.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);

    const auto before = source.snapshot().objects.front().object.local_transform;
    auto changed = before;
    changed.translation.x += 0.25;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SetObjectTransformAction{objects[0], changed, std::nullopt}));

    const auto analysis = carto::agent::AgentTask::analyze_checkpoint_rebase(
        source, checkpoint.value());
    REQUIRE(analysis);
    REQUIRE(analysis.value().can_rebase());
    REQUIRE(analysis.value().directly_conflicting_step_ids.size() == 1U);
    REQUIRE(analysis.value().directly_conflicting_step_ids.front() == "step-a");
    REQUIRE(analysis.value().invalidated_step_ids.size() == 2U);
    REQUIRE(std::find(analysis.value().invalidated_step_ids.begin(),
        analysis.value().invalidated_step_ids.end(), "step-a") !=
        analysis.value().invalidated_step_ids.end());
    REQUIRE(std::find(analysis.value().invalidated_step_ids.begin(),
        analysis.value().invalidated_step_ids.end(), "step-b") !=
        analysis.value().invalidated_step_ids.end());
    REQUIRE(std::find(analysis.value().invalidated_step_ids.begin(),
        analysis.value().invalidated_step_ids.end(), "step-c") ==
        analysis.value().invalidated_step_ids.end());

    carto::ui::UiController ui(source);
    REQUIRE(ui.load_agent_checkpoint(path));
    REQUIRE(ui.rebase_agent_checkpoint());
    const auto ui_snapshot = ui.snapshot();
    REQUIRE(ui_snapshot.agent_checkpoint_directly_conflicting_steps ==
        analysis.value().directly_conflicting_step_ids);
    REQUIRE(ui_snapshot.agent_checkpoint_invalidated_steps ==
        analysis.value().invalidated_step_ids);
    REQUIRE(source.snapshot().project_revision == analysis.value().current_revision);
    const auto rebased = ui.resume_agent_checkpoint();
    REQUIRE(rebased && rebased.value().status == carto::agent::TaskStatus::review);
    REQUIRE(rebased.value().can_accept);
    REQUIRE(rebased.value().rebases.front().reused_step_ids ==
        std::vector<std::string>{"step-c"});
    REQUIRE(source.snapshot().project_revision == analysis.value().current_revision);
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
    REQUIRE(resumed.value().snapshot().steps.front().demonstration_parameters.has_value());
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
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(path);
    REQUIRE(checkpoint);
    auto resumed = carto::agent::AgentTask::resume_checkpoint(
        source, checkpoint.value(), checkpoint.value().plan);
    REQUIRE(resumed);
    REQUIRE(resumed.value().snapshot().steps.front().demonstration_parameters.has_value());
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
    const auto path = directory.path() / "v3-prepared.carto-agent-task";
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
    const std::string current_body = encoded.value().substr(body_begin, body_length);
    const auto field = [](std::string_view value) {
        return std::to_string(value.size()) + ":" + std::string(value) + "|";
    };
    auto v4_fields = checkpoint_fields(current_body);
    REQUIRE(v4_fields.size() > 13U && v4_fields[0] == "9");
    v4_fields[0] = "8";
    v4_fields.pop_back();
    v4_fields[0] = "7";
    v4_fields[0] = "4";
    v4_fields.erase(v4_fields.begin() + 13); // Version 4 has no recipe field.
    const std::string v4_body = checkpoint_body(v4_fields);
    const auto v4_checksum = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(v4_body.data()), v4_body.size()));
    const std::string v4 = std::string(magic) + field(v4_body) + field(v4_checksum.hex());
    const auto decoded_v4 = carto::agent::AgentTaskCheckpoint::deserialize(v4);
    REQUIRE(decoded_v4);
    REQUIRE(decoded_v4.value().plan.objective.recipe_id.empty());

    const auto v3_fields = legacy_v3_fields_from_current(checkpoint_fields(current_body));
    const std::string v3_body = checkpoint_body(v3_fields);
    REQUIRE(v3_body.starts_with("1:3|"));
    REQUIRE(v3_body.ends_with("1:0|1:0|1:0|0:|"));
    const auto v3_checksum = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(v3_body.data()), v3_body.size()));
    const std::string v3 = std::string(magic) + field(v3_body) + field(v3_checksum.hex());
    const auto decoded_v3 = carto::agent::AgentTaskCheckpoint::deserialize(v3);
    REQUIRE(decoded_v3);
    const auto* legacy_face = std::get_if<carto::geometry::FaceId>(
        &decoded_v3.value().plan.steps.front().target.component);
    REQUIRE(legacy_face != nullptr && *legacy_face == carto::geometry::FaceId{1U});

    auto v2_fields = v3_fields;
    v2_fields[0] = "2";
    REQUIRE(v2_fields.size() >= 2U);
    v2_fields.erase(v2_fields.end() - 2); // Version 2 has no ghost-correction count.
    const std::string v2_body = checkpoint_body(v2_fields);
    const auto v2_checksum = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(v2_body.data()), v2_body.size()));
    const std::string v2 = std::string(magic) + field(v2_body) + field(v2_checksum.hex());
    const auto decoded_v2 = carto::agent::AgentTaskCheckpoint::deserialize(v2);
    REQUIRE(decoded_v2);
    REQUIRE(decoded_v2.value().rebase_lineage.empty());
    REQUIRE(decoded_v2.value().ghost_corrections.empty());

    auto v1_fields = std::move(v2_fields);
    v1_fields[0] = "1";
    REQUIRE(v1_fields.size() >= 2U);
    v1_fields.erase(v1_fields.end() - 2); // Version 1 has no rebase-lineage count.
    const std::string v1_body = checkpoint_body(v1_fields);
    const auto v1_checksum = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(v1_body.data()), v1_body.size()));
    const std::string v1 = std::string(magic) + field(v1_body) + field(v1_checksum.hex());
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
    const auto demonstration_path =
        directory.path() / "accepted-ui-run.carto-agent-demo";
    REQUIRE(reopened_ui.save_agent_demonstration(demonstration_path));
    const auto saved_demonstration =
        carto::agent::AgentDemonstration::load(demonstration_path);
    REQUIRE(saved_demonstration);
    const auto demonstration_bytes = saved_demonstration.value().serialize();
    REQUIRE(demonstration_bytes);
    REQUIRE(demonstration_bytes.value().find(goal) == std::string::npos);
    REQUIRE(demonstration_bytes.value().find(
        "I reapprove this exact target scope after reopening.") == std::string::npos);
    REQUIRE(demonstration_bytes.value().find(
        "I reapprove the bounded topology edit after reopening.") == std::string::npos);

    carto::ui::UiController demonstration_view(source);
    REQUIRE(demonstration_view.load_agent_demonstration(demonstration_path));
    const auto demonstration_snapshot = demonstration_view.snapshot();
    REQUIRE(demonstration_snapshot.agent_demonstration.has_value());
    REQUIRE(!demonstration_snapshot.agent_task.has_value());
    REQUIRE(source.snapshot().project_revision == source_revision.next());
    REQUIRE(!demonstration_view.accept_agent_plan());

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

void bounded_mesh_recipe_caps_execution_and_binds_its_acceptance_gate() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    auto plain_plan = plan_for(before, objects, {
        {"step-1", {objects[0], carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    auto recipe_plan = plain_plan;
    recipe_plan.objective.recipe_id = "bounded-mesh-edit";
    const auto plain_digest = plain_plan.objective.digest();
    const auto recipe_digest = recipe_plan.objective.digest();
    REQUIRE(plain_digest);
    REQUIRE(recipe_digest);
    REQUIRE(plain_digest.value() != recipe_digest.value());

    auto envelope = envelope_for(objects);
    envelope.max_topology_operations = 900U;
    auto task = carto::agent::AgentTask::begin(
        source, recipe_plan, envelope);
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().can_accept);
    REQUIRE(run.value().recipe_id == "bounded-mesh-edit");
    const auto recipe_gate = std::find_if(
        run.value().gates.begin(), run.value().gates.end(), [](const auto& gate) {
            return gate.id == "recipe.bounded-mesh-edit.scene-identity";
        });
    REQUIRE(recipe_gate != run.value().gates.end());
    REQUIRE(recipe_gate->required);
    REQUIRE(recipe_gate->passed);
    const auto topology_gate = std::find_if(
        run.value().gates.begin(), run.value().gates.end(), [](const auto& gate) {
            return gate.id == "envelope.topology_operations";
        });
    REQUIRE(topology_gate != run.value().gates.end());
    REQUIRE(topology_gate->evidence.find("/ 512 permitted") != std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "bounded-recipe.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(checkpoint_path));
    auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().plan.objective.recipe_id == "bounded-mesh-edit");
    REQUIRE(checkpoint.value().envelope.max_topology_operations == 512U);
    checkpoint.value().envelope.max_topology_operations = 513U;
    REQUIRE(!checkpoint.value().serialize());

    auto transform_plan = plain_plan;
    transform_plan.objective.recipe_id = "bounded-mesh-edit";
    auto transform_envelope = envelope_for(objects);
    transform_envelope.permitted_effects |=
        carto::agent::effect_mask(carto::agent::Effect::transform);
    transform_envelope.max_transform_distance_meters = 2.0;
    REQUIRE(!carto::agent::AgentTask::begin(
        source, std::move(transform_plan), std::move(transform_envelope)));

    auto unknown_recipe_plan = plain_plan;
    unknown_recipe_plan.objective.recipe_id = "unregistered-recipe";
    REQUIRE(!unknown_recipe_plan.validate());
}

void bounded_vertex_recipe_preserves_topology_and_repair_authority() {
    carto::application::ApplicationSession source;
    const auto objects = create_boxes(source, 1U);
    const auto before = source.snapshot();
    const auto vertex = before.viewport.scene.instances().front().mesh->vertex_ids.front();

    auto vertex_plan = plan_for(before, objects, {
        {"step-1", {objects.front(), vertex}, "move vertex to 4 0 0"},
    });
    vertex_plan.objective.recipe_id = "bounded-vertex-edit";
    auto vertex_envelope = envelope_for(objects);
    vertex_envelope.max_topology_operations = 64U;
    auto vertex_task = carto::agent::AgentTask::begin(
        source, vertex_plan, vertex_envelope);
    REQUIRE(vertex_task);
    const auto vertex_run = vertex_task.value().run();
    REQUIRE(vertex_run);
    REQUIRE(vertex_run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(vertex_run.value().can_accept);
    const auto topology_gate = std::find_if(
        vertex_run.value().gates.begin(), vertex_run.value().gates.end(), [](const auto& gate) {
            return gate.id == "recipe.bounded-vertex-edit.topology-preservation";
        });
    REQUIRE(topology_gate != vertex_run.value().gates.end());
    REQUIRE(topology_gate->required);
    REQUIRE(topology_gate->passed);
    REQUIRE(topology_gate->evidence.find("topology remained unchanged") != std::string::npos);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "bounded-vertex-recipe.carto-agent-task";
    REQUIRE(vertex_task.value().save_checkpoint(checkpoint_path));
    auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().envelope.max_topology_operations == 0U);
    REQUIRE(checkpoint.value().envelope.permitted_effects ==
        carto::agent::effect_mask(carto::agent::Effect::geometry));
    checkpoint.value().envelope.permitted_effects |=
        carto::agent::effect_mask(carto::agent::Effect::topology);
    REQUIRE(!checkpoint.value().serialize());

    auto topology_plan = plan_for(before, objects, {
        {"step-1", {objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    topology_plan.objective.recipe_id = "bounded-vertex-edit";
    auto topology_task = carto::agent::AgentTask::begin(
        source, topology_plan, envelope_for(objects));
    REQUIRE(topology_task);
    const auto topology_run = topology_task.value().run();
    REQUIRE(topology_run);
    REQUIRE(topology_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!topology_run.value().can_accept);
    REQUIRE(topology_run.value().steps.front().status == carto::agent::StepStatus::failed);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    auto revised_plan = topology_plan;
    revised_plan.id = "agent-task-vertex-repair";
    revised_plan.steps.front().target.component = vertex;
    revised_plan.steps.front().intent = "move vertex to 4 0 0";
    auto repaired = carto::agent::AgentTask::repair(
        source, topology_task.value(), revised_plan, envelope_for(objects),
        "Use the topology-preserving vertex operation permitted by this recipe.");
    REQUIRE(repaired);
    const auto repaired_run = repaired.value().run();
    REQUIRE(repaired_run);
    REQUIRE(repaired_run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(repaired_run.value().can_accept);
    const auto repaired_topology_gate = std::find_if(
        repaired_run.value().gates.begin(), repaired_run.value().gates.end(), [](const auto& gate) {
            return gate.id == "recipe.bounded-vertex-edit.topology-preservation";
        });
    REQUIRE(repaired_topology_gate != repaired_run.value().gates.end());
    REQUIRE(repaired_topology_gate->passed);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    auto transform_envelope = envelope_for(objects);
    transform_envelope.permitted_effects |=
        carto::agent::effect_mask(carto::agent::Effect::transform);
    transform_envelope.max_transform_distance_meters = 1.0;
    REQUIRE(!carto::agent::AgentTask::begin(
        source, vertex_plan, std::move(transform_envelope)));
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

void explicit_objective_compilation_is_bounded_and_fails_closed() {
    const auto preserved = carto::agent::compile_explicit_hard_constraints(
        "Do not change topology.");
    REQUIRE(preserved);
    REQUIRE(preserved.value().size() == 1U);
    REQUIRE(preserved.value().front().id == "compiled.preserve-topology");
    REQUIRE(preserved.value().front().kind ==
        carto::agent::ObjectiveConstraintKind::preserve_topology);

    const auto apostrophe = carto::agent::compile_explicit_hard_constraints(
        "Don't change topology");
    REQUIRE(apostrophe);
    REQUIRE(apostrophe.value().size() == 1U);

    const auto ordinary = carto::agent::compile_explicit_hard_constraints(
        "Prepare the selected face");
    REQUIRE(ordinary);
    REQUIRE(ordinary.value().empty());

    const auto permissive = carto::agent::compile_explicit_hard_constraints(
        "Allow topology changes if needed");
    REQUIRE(permissive);
    REQUIRE(permissive.value().empty());

    const auto unsupported = carto::agent::compile_explicit_hard_constraints(
        "Do not alter the bathroom");
    REQUIRE(!unsupported);
    REQUIRE(unsupported.error().message.find("restriction") != std::string::npos);

    const auto conditional = carto::agent::compile_explicit_hard_constraints(
        "Do not change topology except when necessary");
    REQUIRE(!conditional);

    const auto conflict = carto::agent::compile_explicit_hard_constraints(
        "Preserve topology but allow topology changes");
    REQUIRE(!conflict);
    REQUIRE(conflict.error().message.find("conflicting topology") != std::string::npos);

    const auto contradictory_tail = carto::agent::compile_explicit_hard_constraints(
        "Do not change topology, but change the topology if required");
    REQUIRE(!contradictory_tail);
    REQUIRE(contradictory_tail.error().message.find("conflicting topology") !=
        std::string::npos);

    const auto permit_conflict = carto::agent::compile_explicit_hard_constraints(
        "Preserve topology but permit changes to topology");
    REQUIRE(!permit_conflict);

    carto::application::ApplicationSession direct_source;
    const auto direct_objects = create_boxes(direct_source, 1U);
    auto direct_plan = plan_for(direct_source.snapshot(), direct_objects, {
        {"step-1", {direct_objects.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"},
    });
    direct_plan.objective.goal = "Do not change topology";
    REQUIRE(!direct_plan.validate());
    direct_plan.objective.hard_constraints = preserved.value();
    REQUIRE(direct_plan.validate());
}

void objective_compiler_binds_explicit_transform_and_displacement_limits() {
    const auto preserve_transform = carto::agent::compile_explicit_hard_constraints(
        "Keep world transforms unchanged.");
    REQUIRE(preserve_transform);
    REQUIRE(preserve_transform.value().size() == 1U);
    REQUIRE(preserve_transform.value().front().kind ==
        carto::agent::ObjectiveConstraintKind::preserve_transform);
    REQUIRE(!preserve_transform.value().front().object.has_value());
    REQUIRE(preserve_transform.value().front().validate());

    const auto displacement = carto::agent::compile_explicit_hard_constraints(
        "Maximum vertex displacement 0.25 world units.");
    REQUIRE(displacement);
    REQUIRE(displacement.value().size() == 1U);
    REQUIRE(displacement.value().front().kind ==
        carto::agent::ObjectiveConstraintKind::maximum_vertex_displacement);
    REQUIRE(displacement.value().front().maximum_displacement == 0.25);
    REQUIRE(!displacement.value().front().object.has_value());
    REQUIRE(displacement.value().front().validate());

    const auto alternate = carto::agent::compile_explicit_hard_constraints(
        "Limit vertex displacement to 0.5 world units.");
    REQUIRE(alternate);
    REQUIRE(alternate.value().front().maximum_displacement == 0.5);
    const auto leading_decimal = carto::agent::compile_explicit_hard_constraints(
        "Limit vertex displacement to .25 world units.");
    REQUIRE(leading_decimal);
    REQUIRE(leading_decimal.value().front().maximum_displacement == 0.25);

    REQUIRE(!carto::agent::compile_explicit_hard_constraints(
        "Preserve transforms but allow transforms to change."));
    REQUIRE(!carto::agent::compile_explicit_hard_constraints(
        "Don't change the transform."));
    REQUIRE(!carto::agent::compile_explicit_hard_constraints(
        "Maximum vertex displacement -0.25 world units."));
    REQUIRE(!carto::agent::compile_explicit_hard_constraints(
        "Maximum vertex displacement 0.25 meters."));
    REQUIRE(!carto::agent::compile_explicit_hard_constraints(
        "Maximum vertex displacement."));
    REQUIRE(!carto::agent::compile_explicit_hard_constraints(
        "Maximum vertex displacement 1000001 world units."));
    REQUIRE(!carto::agent::compile_explicit_hard_constraints(
        "Maximum vertex displacement 0.1 world units and limit vertex displacement to 0.2 world units."));

    carto::application::ApplicationSession transform_source;
    const auto transform_objects = create_boxes(transform_source, 1U);
    auto transform_plan = plan_for(transform_source.snapshot(), transform_objects, {
        {"step-1", {transform_objects.front(), std::monostate{}},
            "translate object by 0.25 0 0"},
    });
    transform_plan.objective.goal = "Preserve transforms";
    REQUIRE(!transform_plan.validate());
    transform_plan.objective.hard_constraints = preserve_transform.value();
    REQUIRE(transform_plan.validate());
    const auto transform_revision = transform_source.snapshot().project_revision;
    auto transform_task = carto::agent::AgentTask::begin(
        transform_source, std::move(transform_plan),
        translation_envelope(transform_objects, 1.0));
    REQUIRE(transform_task);
    const auto transform_run = transform_task.value().run();
    REQUIRE(transform_run);
    REQUIRE(transform_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(transform_run.value().diagnostic->message.find(
        "compiled.preserve-transform") != std::string::npos);
    REQUIRE(transform_source.snapshot().project_revision == transform_revision);

    carto::application::ApplicationSession ui_transform_source;
    carto::ui::UiController ui_transform(ui_transform_source);
    REQUIRE(ui_transform.dispatch(carto::application::CreateBoxAction{
        "Compiled transform target", {1.0, 1.0, 1.0}}));
    const auto ui_target = ui_transform_source.snapshot().objects.front().object.id;
    REQUIRE(ui_transform.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui_transform.dispatch(carto::application::SelectObjectAction{ui_target}));
    REQUIRE(ui_transform.queue_agent_step("translate object by 0.25 0 0"));
    const auto ui_revision = ui_transform_source.snapshot().project_revision;
    const auto ui_run = ui_transform.run_agent_plan(
        "Keep transforms unchanged", translation_envelope({ui_target}, 1.0));
    REQUIRE(ui_run);
    REQUIRE(ui_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(ui_run.value().hard_constraints.size() == 1U);
    REQUIRE(ui_run.value().hard_constraints.front().kind ==
        carto::agent::ObjectiveConstraintKind::preserve_transform);
    REQUIRE(ui_transform_source.snapshot().project_revision == ui_revision);

    carto::application::ApplicationSession displacement_source;
    const auto displacement_objects = create_boxes(displacement_source, 2U);
    const auto displacement_snapshot = displacement_source.snapshot();
    const auto first_vertex = displacement_snapshot.viewport.scene.find(
        displacement_objects[0])->mesh->vertex_ids.front();
    const auto second_vertex = displacement_snapshot.viewport.scene.find(
        displacement_objects[1])->mesh->vertex_ids.front();
    auto displacement_plan = plan_for(displacement_snapshot, displacement_objects, {
        {"step-1", {displacement_objects[0], first_vertex}, "move vertex to 4 0 0"},
        {"step-2", {displacement_objects[1], second_vertex}, "move vertex to 0 4 0"},
    });
    displacement_plan.objective.goal = "Maximum vertex displacement 0.25 world units";
    REQUIRE(!displacement_plan.validate());
    displacement_plan.objective.hard_constraints = displacement.value();
    REQUIRE(displacement_plan.validate());
    const auto displacement_revision = displacement_source.snapshot().project_revision;
    auto displacement_task = carto::agent::AgentTask::begin(
        displacement_source, std::move(displacement_plan),
        envelope_for(displacement_objects));
    REQUIRE(displacement_task);
    const auto displacement_run = displacement_task.value().run();
    REQUIRE(displacement_run);
    REQUIRE(displacement_run.value().status == carto::agent::TaskStatus::blocked);
    const auto displacement_gate = std::find_if(
        displacement_run.value().gates.begin(), displacement_run.value().gates.end(),
        [](const auto& gate) {
            return gate.id == "constraint.compiled.maximum-vertex-displacement";
        });
    REQUIRE(displacement_gate != displacement_run.value().gates.end());
    REQUIRE(!displacement_gate->passed);
    REQUIRE(displacement_gate->affected_objects.size() == 2U);
    REQUIRE(displacement_gate->evidence.find("0.250000 world units") != std::string::npos);
    REQUIRE(displacement_source.snapshot().project_revision == displacement_revision);
}

void ui_compiled_constraints_are_bound_to_clarification_and_gate_execution() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Compiled constraint target", {1.0, 1.0, 1.0}}));
    const auto target = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    REQUIRE(ui.set_agent_hard_constraints({{
        "operator-topology", carto::agent::ObjectiveConstraintKind::preserve_topology,
        std::nullopt, 0.0, "The operator requires existing topology to remain unchanged."}}));
    REQUIRE(ui.queue_agent_clarification(
        "Confirm that the topology-preservation requirement is intentional.",
        carto::agent::ClarificationImpact::topology_deletion));

    const std::string goal = "Do not change topology";
    REQUIRE(ui.answer_agent_clarification(
        goal, "question-1", "I require the existing topology to remain unchanged."));
    const auto source_revision = source.snapshot().project_revision;
    const auto run = ui.run_agent_plan(goal);
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(run.value().hard_constraints.size() == 1U);
    REQUIRE(run.value().hard_constraints.front().id == "operator-topology");
    REQUIRE(run.value().clarifications.front().human_receipt.has_value());
    const auto gate = std::find_if(run.value().gates.begin(), run.value().gates.end(),
        [](const auto& candidate) { return candidate.id == "constraint.operator-topology"; });
    REQUIRE(gate != run.value().gates.end());
    REQUIRE(run.value().diagnostic.has_value());
    REQUIRE(run.value().diagnostic->message.find("operator-topology") != std::string::npos);
    REQUIRE(run.value().steps.size() == 1U);
    REQUIRE(run.value().steps.front().status == carto::agent::StepStatus::failed);
    REQUIRE(gate->passed);
    REQUIRE(gate->evidence.find("no vertex or face topology changed") != std::string::npos);
    REQUIRE(source.snapshot().project_revision == source_revision);

    carto::application::ApplicationSession unsupported_source;
    carto::ui::UiController unsupported_ui(unsupported_source);
    REQUIRE(unsupported_ui.dispatch(carto::application::CreateBoxAction{
        "Unsupported restriction target", {1.0, 1.0, 1.0}}));
    const auto unsupported_target = unsupported_source.snapshot().objects.front().object.id;
    REQUIRE(unsupported_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(unsupported_ui.dispatch(carto::application::SelectFaceAction{
        unsupported_target, carto::geometry::FaceId{1U}}));
    REQUIRE(unsupported_ui.queue_agent_step("inset face 0.1"));
    const auto unsupported_revision = unsupported_source.snapshot().project_revision;
    const auto rejected = unsupported_ui.run_agent_plan("Do not alter the bathroom");
    REQUIRE(!rejected);
    REQUIRE(!unsupported_ui.snapshot().agent_task.has_value());
    REQUIRE(unsupported_source.snapshot().project_revision == unsupported_revision);
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
    REQUIRE(ran.value().hard_constraints.empty());
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

void ui_agent_workbench_pokes_faces_only_on_a_reviewed_branch() {
    TempDirectory directory;
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Poke target", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    const auto before = source.snapshot();
    const auto* before_instance = before.viewport.scene.find(object);
    REQUIRE(before_instance != nullptr && before_instance->mesh != nullptr);

    REQUIRE(!ui.apply_ai_intent("poke face"));
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.queue_agent_step("poke face"));
    const auto run = ui.run_agent_plan("Poke the selected face");
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().can_accept);
    REQUIRE(run.value().diff.changed_mesh_count() == 1U);
    REQUIRE(run.value().steps.size() == 1U);
    REQUIRE(run.value().steps.front().tool_id == "mesh.poke-face");
    REQUIRE(run.value().steps.front().demonstration_parameters.has_value());
    REQUIRE(run.value().steps.front().demonstration_parameters->parameterization ==
        carto::agent::AgentDemonstrationParameterization::parameterless);
    const auto* ghost_instance = run.value().after_scene.find(object);
    REQUIRE(ghost_instance != nullptr && ghost_instance->mesh != nullptr);
    REQUIRE(ghost_instance->mesh->vertex_ids.size() ==
        before_instance->mesh->vertex_ids.size() + 1U);
    REQUIRE(ghost_instance->mesh->triangle_faces.size() >
        before_instance->mesh->triangle_faces.size());
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(source.snapshot().viewport.scene.find(object)->mesh->vertex_ids.size() ==
        before_instance->mesh->vertex_ids.size());

    REQUIRE(ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == before.project_revision.next());
    REQUIRE(source.snapshot().viewport.scene.find(object)->mesh->vertex_ids.size() ==
        before_instance->mesh->vertex_ids.size() + 1U);
    const auto demonstration_path = directory.path() / "accepted-poke.carto-agent-demo";
    REQUIRE(ui.save_agent_demonstration(demonstration_path));
    const auto demonstration = carto::agent::AgentDemonstration::load(demonstration_path);
    REQUIRE(demonstration);
    REQUIRE(demonstration.value().format_version == 3U);
    REQUIRE(demonstration.value().steps.size() == 1U);
    REQUIRE(demonstration.value().steps.front().parameters.has_value());
    REQUIRE(demonstration.value().steps.front().parameters->parameterization ==
        carto::agent::AgentDemonstrationParameterization::parameterless);

    carto::application::ApplicationSession geometry_only_source;
    const auto geometry_only_objects = create_boxes(geometry_only_source, 1U);
    const auto geometry_only_before = geometry_only_source.snapshot();
    auto geometry_only_plan = plan_for(geometry_only_before, geometry_only_objects, {
        {"step-1", {geometry_only_objects.front(), carto::geometry::FaceId{1U}}, "poke face"},
    });
    auto geometry_only_envelope = envelope_for(geometry_only_objects);
    geometry_only_envelope.permitted_effects =
        carto::agent::effect_mask(carto::agent::Effect::geometry);
    auto geometry_only_task = carto::agent::AgentTask::begin(
        geometry_only_source, std::move(geometry_only_plan), geometry_only_envelope);
    REQUIRE(geometry_only_task);
    const auto blocked = geometry_only_task.value().run();
    REQUIRE(blocked);
    REQUIRE(blocked.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(!blocked.value().can_accept);
    REQUIRE(geometry_only_source.snapshot().project_revision ==
        geometry_only_before.project_revision);

    carto::application::ApplicationSession invalid_source;
    const auto invalid_objects = create_boxes(invalid_source, 1U);
    const auto invalid_before = invalid_source.snapshot();
    auto invalid_plan = plan_for(invalid_before, invalid_objects, {
        {"step-1", {invalid_objects.front(), carto::geometry::FaceId{1U}}, "poke face by 1"},
    });
    auto invalid_task = carto::agent::AgentTask::begin(
        invalid_source, std::move(invalid_plan), envelope_for(invalid_objects));
    REQUIRE(invalid_task);
    const auto invalid_run = invalid_task.value().run();
    REQUIRE(invalid_run);
    REQUIRE(invalid_run.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(invalid_source.snapshot().project_revision == invalid_before.project_revision);
}

void ui_agent_entry_preserves_context_selection_across_workspaces() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Context target", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    const auto source_revision = source.snapshot().project_revision;

    REQUIRE(ui.set_workspace(carto::ui::Workspace::material));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::ai));
    const auto routed = ui.snapshot();
    REQUIRE(routed.workspace_kind == carto::ui::Workspace::ai);
    REQUIRE(routed.selection.mode == carto::editor::SelectionMode::face);
    REQUIRE(routed.selection.component_object == object);
    REQUIRE(routed.selection.faces ==
        std::vector<carto::geometry::FaceId>{carto::geometry::FaceId{1U}});
    REQUIRE(routed.project_revision == source_revision);
    REQUIRE(!routed.agent_task.has_value());

    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    const auto captured = ui.snapshot();
    REQUIRE(captured.agent_plan_draft.size() == 1U);
    REQUIRE(captured.agent_plan_draft.front().target.object == object);
    const auto* captured_face = std::get_if<carto::geometry::FaceId>(
        &captured.agent_plan_draft.front().target.component);
    REQUIRE(captured_face != nullptr && *captured_face == carto::geometry::FaceId{1U});
    REQUIRE(source.snapshot().project_revision == source_revision);
}

void ui_prepared_task_supports_condition_preflight_and_rechecks_source_revision() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    const auto objects = create_boxes(source, 3U);
    auto condition = carto::condition::make_condition_fixture();
    REQUIRE(condition);
    condition.value().project_revision = source.snapshot().project_revision.next();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::ImportConditionAuthoringAction{
            condition.value().serialize()}));

    REQUIRE(!ui.query_agent_condition_inventory());
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        objects.front(), carto::geometry::FaceId{1U}}));
    const auto direct_query_revision = source.snapshot().project_revision;
    auto direct_inventory = ui.query_agent_condition_inventory();
    REQUIRE(direct_inventory);
    REQUIRE(direct_inventory.value().source_revision == direct_query_revision);
    REQUIRE(direct_inventory.value().rows.size() == 1U);
    REQUIRE(!ui.snapshot().agent_task.has_value());
    REQUIRE(ui.snapshot().agent_condition_inventory.has_value());
    REQUIRE(source.snapshot().project_revision == direct_query_revision);
    REQUIRE(ui.queue_agent_step("inset face 0.1"));

    const auto base_revision = source.snapshot().project_revision;
    const auto prepared = ui.prepare_agent_plan("Prepare the selected asset for CONDITION");
    REQUIRE(prepared);
    REQUIRE(prepared.value().status == carto::agent::TaskStatus::prepared);
    auto inventory = ui.query_agent_condition_inventory();
    REQUIRE(inventory);
    REQUIRE(inventory.value().source_revision == base_revision);
    REQUIRE(inventory.value().rows.size() == 1U);
    const auto* fact = std::get_if<carto::agent::ConditionAssetInventoryFact>(
        &inventory.value().rows.front());
    REQUIRE(fact != nullptr);
    REQUIRE(fact->asset == 1U);
    REQUIRE(fact->authored_object == objects.front());
    const auto preflight_snapshot = ui.snapshot();
    REQUIRE(preflight_snapshot.agent_condition_inventory.has_value());
    REQUIRE(preflight_snapshot.agent_task->status == carto::agent::TaskStatus::prepared);
    REQUIRE(preflight_snapshot.project_revision == base_revision);
    REQUIRE(source.condition_authoring().value().project_revision == base_revision);

    const auto run = ui.run_prepared_agent_plan();
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(source.snapshot().project_revision == base_revision);

    carto::application::ApplicationSession stale_source;
    carto::ui::UiController stale_ui(stale_source);
    REQUIRE(stale_ui.dispatch(carto::application::CreateBoxAction{
        "Prepared target", {1.0, 1.0, 1.0}}));
    const auto target = stale_source.snapshot().objects.front().object.id;
    REQUIRE(stale_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(stale_ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(stale_ui.queue_agent_step("inset face 0.1"));
    REQUIRE(stale_ui.prepare_agent_plan("Prepare the selected face"));
    const auto prepared_revision = stale_source.snapshot().project_revision;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        stale_source, carto::application::CreateBoxAction{
            "Concurrent human edit", {1.0, 1.0, 1.0}}));
    const auto concurrent_revision = stale_source.snapshot().project_revision;
    REQUIRE(concurrent_revision > prepared_revision);
    const auto stale_run = stale_ui.run_prepared_agent_plan();
    REQUIRE(!stale_run);
    REQUIRE(stale_run.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(stale_ui.snapshot().agent_task->status == carto::agent::TaskStatus::prepared);
    REQUIRE(stale_source.snapshot().project_revision == concurrent_revision);
}

void ui_recipe_selection_round_trips_and_locks_with_checkpoint() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Recipe checkpoint box", {1.0, 1.0, 1.0}}));
    const auto target = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    REQUIRE(ui.set_agent_recipe("bounded-mesh-edit"));
    const auto source_revision = source.snapshot().project_revision;
    const auto run = ui.run_agent_plan("Apply a bounded face edit");
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().recipe_id == "bounded-mesh-edit");
    REQUIRE(!ui.set_agent_recipe(""));

    TempDirectory directory;
    const auto path = directory.path() / "recipe-ui.carto-agent-task";
    REQUIRE(ui.save_agent_checkpoint(path));
    carto::ui::UiController loaded(source);
    REQUIRE(loaded.load_agent_checkpoint(path));
    REQUIRE(loaded.snapshot().agent_recipe_id == "bounded-mesh-edit");
    REQUIRE(!loaded.set_agent_recipe(""));
    const auto resumed = loaded.resume_agent_checkpoint();
    REQUIRE(resumed);
    REQUIRE(resumed.value().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().recipe_id == "bounded-mesh-edit");
    REQUIRE(!loaded.set_agent_recipe(""));
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(loaded.reject_agent_plan());
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == source_revision.next());
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
    REQUIRE(ui.set_agent_recipe("bounded-mesh-edit"));
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
    REQUIRE(!ui.set_agent_recipe(""));
    REQUIRE(ui.snapshot().agent_recipe_id == "bounded-mesh-edit");
    REQUIRE(!ui.run_agent_plan("A changed goal must not reuse the answer"));
    REQUIRE(!ui.queue_agent_step("inset face 0.2"));

    const auto run = ui.run_agent_plan("Prepare the selected face");
    REQUIRE(run);
    REQUIRE(run.value().recipe_id == "bounded-mesh-edit");
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

void ui_candidate_clarification_choice_is_scoped_and_receipted() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Candidate clarification target", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));

    REQUIRE(!ui.queue_agent_clarification(
        "Choose the intended edit.",
        carto::agent::ClarificationImpact::topology_deletion,
        {{"only", "Only one choice", {object}}}));
    REQUIRE(!ui.queue_agent_clarification(
        "Choose the intended edit.",
        carto::agent::ClarificationImpact::topology_deletion,
        {{"inside", "In scope", {object}},
         {"outside", "Out of scope", {carto::scene::ObjectId{999U}}}}));
    REQUIRE(ui.snapshot().agent_clarifications_draft.empty());

    REQUIRE(ui.queue_agent_clarification(
        "How should this face change?",
        carto::agent::ClarificationImpact::topology_deletion,
        {{"preserve", "Preserve the current surface", {object}},
         {"inset", "Allow the captured inset", {object}}}));
    const auto source_revision = source.snapshot().project_revision;
    const auto queued = ui.snapshot();
    REQUIRE(queued.agent_clarifications_draft.size() == 1U);
    REQUIRE(queued.agent_clarifications_draft.front().candidates.size() == 2U);
    REQUIRE(!queued.agent_clarifications_draft.front().human_receipt.has_value());
    REQUIRE(!ui.run_agent_plan("Apply the bounded inset after the operator choice."));
    REQUIRE(!ui.choose_agent_clarification_candidate(
        "Apply the bounded inset after the operator choice.", "question-1", "missing"));
    REQUIRE(!ui.snapshot().agent_clarifications_draft.front().human_receipt.has_value());
    REQUIRE(source.snapshot().project_revision == source_revision);

    REQUIRE(ui.choose_agent_clarification_candidate(
        "Apply the bounded inset after the operator choice.", "question-1", "inset"));
    const auto answered = ui.snapshot().agent_clarifications_draft.front();
    REQUIRE(answered.selected_candidate_id == std::optional<std::string>("inset"));
    REQUIRE(answered.human_receipt.has_value());
    REQUIRE(!answered.freeform_answer.has_value());
    REQUIRE(!ui.queue_agent_step("inset face 0.2"));
    REQUIRE(source.snapshot().project_revision == source_revision);

    const auto run = ui.run_agent_plan("Apply the bounded inset after the operator choice.");
    REQUIRE(run && run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(run.value().can_accept);
    REQUIRE(run.value().clarifications.size() == 1U);
    REQUIRE(run.value().clarifications.front().selected_candidate_id ==
        std::optional<std::string>("inset"));
    REQUIRE(run.value().clarifications.front().human_receipt.has_value());
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == source_revision.next());
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

void ui_ghost_corrections_are_scoped_revalidated_and_checkpointed() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Ghost target", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Outside ghost scope", {1.0, 1.0, 1.0}}));
    const auto base = source.snapshot();
    const auto target = base.objects[0].object.id;
    const auto out_of_scope = base.objects[1].object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    const auto run = ui.run_agent_plan("Prepare the selected face");
    REQUIRE(run);
    REQUIRE(run.value().status == carto::agent::TaskStatus::review);
    const auto before_correction = ui.snapshot().agent_task.value();
    const auto source_revision = source.snapshot().project_revision;

    auto moved = base.objects[0].object.local_transform;
    moved.translation.x += 0.75;
    REQUIRE(!ui.correct_agent_ghost_transform(target, moved, ""));
    auto non_finite = moved;
    non_finite.translation.x = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!ui.correct_agent_ghost_transform(
        target, non_finite, "Reject a non-finite ghost transform."));
    REQUIRE(!ui.correct_agent_ghost_transform(
        out_of_scope, moved, "Reject a target outside the declared scope."));
    REQUIRE(ui.snapshot().agent_task->speculative_revision ==
        before_correction.speculative_revision);
    REQUIRE(source.snapshot().project_revision == source_revision);

    const auto corrected = ui.correct_agent_ghost_transform(
        target, moved, "Move the ghost clear of the adjacent object before review.");
    REQUIRE(corrected);
    REQUIRE(corrected.value().status == carto::agent::TaskStatus::review);
    REQUIRE(corrected.value().can_accept);
    REQUIRE(corrected.value().ghost_corrections.size() == 1U);
    REQUIRE(corrected.value().ghost_corrections.front().object == target);
    REQUIRE(corrected.value().ghost_corrections.front().justification.find(
        "adjacent object") != std::string::npos);
    REQUIRE(corrected.value().ghost_corrections.front().validate());
    REQUIRE(corrected.value().speculative_revision ==
        before_correction.speculative_revision.next());
    REQUIRE(source.snapshot().project_revision == source_revision);
    const auto changed_object = std::find_if(
        corrected.value().diff.objects.begin(), corrected.value().diff.objects.end(),
        [target](const auto& change) { return change.id == target; });
    REQUIRE(changed_object != corrected.value().diff.objects.end());
    REQUIRE(changed_object->transform_changed);
    const auto gate = std::find_if(
        corrected.value().gates.begin(), corrected.value().gates.end(),
        [](const auto& value) { return value.id == "project.valid"; });
    REQUIRE(gate != corrected.value().gates.end());
    REQUIRE(gate->passed);
    REQUIRE(gate->evaluated_revision == corrected.value().speculative_revision);
    REQUIRE(gate->result_digest == corrected.value().diff.digest);

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "ghost-correction.carto-agent-task";
    REQUIRE(ui.save_agent_checkpoint(checkpoint_path));
    const auto current_checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(current_checkpoint);
    auto legacy_source = current_checkpoint.value();
    legacy_source.ghost_corrections.front().recompute_after_step_id.reset();
    const auto legacy_evidence =
        legacy_source.ghost_corrections.front().computed_evidence_digest();
    REQUIRE(legacy_evidence);
    legacy_source.ghost_corrections.front().evidence_digest = legacy_evidence.value();
    const auto legacy_encoding = legacy_source.serialize();
    REQUIRE(legacy_encoding);
    constexpr std::string_view checkpoint_magic =
        "CARTOGRAPHER_AGENT_TASK_CHECKPOINT_V1\n";
    const std::size_t body_length_end = legacy_encoding.value().find(
        ':', checkpoint_magic.size());
    REQUIRE(body_length_end != std::string::npos);
    const auto body_length = static_cast<std::size_t>(std::stoull(
        legacy_encoding.value().substr(
            checkpoint_magic.size(), body_length_end - checkpoint_magic.size())));
    const std::size_t body_begin = body_length_end + 1U;
    auto legacy_v5_fields = checkpoint_fields(
        legacy_encoding.value().substr(body_begin, body_length));
    REQUIRE(legacy_v5_fields[0] == "9");
    legacy_v5_fields.pop_back();
    legacy_v5_fields[0] = "7";
    REQUIRE(legacy_v5_fields[0] == "7");
    legacy_v5_fields.erase(legacy_v5_fields.end() - 13, legacy_v5_fields.end() - 1);
    legacy_v5_fields[0] = "5";
    const std::string legacy_v5_body = checkpoint_body(legacy_v5_fields);
    const auto legacy_v5_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(legacy_v5_body.data()), legacy_v5_body.size()));
    const auto field = [](std::string_view value) {
        return std::to_string(value.size()) + ":" + std::string(value) + "|";
    };
    const std::string legacy_v5 = std::string(checkpoint_magic) +
        field(legacy_v5_body) + field(legacy_v5_digest.hex());
    const auto decoded_v5 = carto::agent::AgentTaskCheckpoint::deserialize(legacy_v5);
    REQUIRE(decoded_v5);
    REQUIRE(decoded_v5.value().ghost_corrections.size() == 1U);
    REQUIRE(decoded_v5.value().ghost_corrections.front().kind ==
        carto::agent::GhostCorrectionKind::object_transform);
    REQUIRE(decoded_v5.value().ghost_corrections.front().validate());

    carto::ui::UiController resumed_ui(source);
    REQUIRE(resumed_ui.load_agent_checkpoint(checkpoint_path));
    const auto resumed = resumed_ui.resume_agent_checkpoint();
    REQUIRE(resumed);
    REQUIRE(resumed.value().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().can_accept);
    REQUIRE(resumed.value().ghost_corrections.size() == 1U);
    REQUIRE(resumed.value().ghost_corrections.front().evidence_digest ==
        corrected.value().ghost_corrections.front().evidence_digest);
    REQUIRE(resumed.value().diff.digest == corrected.value().diff.digest);
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Unrelated edit before ghost rebase", {0.5, 0.5, 0.5}}));
    const auto rebased_source_revision = source.snapshot().project_revision;
    carto::ui::UiController rebased_ui(source);
    REQUIRE(rebased_ui.load_agent_checkpoint(checkpoint_path));
    REQUIRE(rebased_ui.rebase_agent_checkpoint());
    const auto rebased = rebased_ui.resume_agent_checkpoint();
    REQUIRE(rebased);
    REQUIRE(rebased.value().status == carto::agent::TaskStatus::review);
    REQUIRE(rebased.value().can_accept);
    REQUIRE(rebased.value().rebases.size() == 1U);
    REQUIRE(rebased.value().ghost_corrections.size() == 1U);
    REQUIRE(source.snapshot().project_revision == rebased_source_revision);
    REQUIRE(rebased_ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == rebased_source_revision.next());
    const auto accepted_snapshot = source.snapshot();
    const auto accepted = std::find_if(accepted_snapshot.objects.begin(),
        accepted_snapshot.objects.end(), [target](const auto& value) {
            return value.object.id == target;
        });
    REQUIRE(accepted != accepted_snapshot.objects.end());
    REQUIRE(accepted->object.local_transform.translation.x == moved.translation.x);

    carto::application::ApplicationSession stale_source;
    carto::ui::UiController stale_ui(stale_source);
    REQUIRE(stale_ui.dispatch(carto::application::CreateBoxAction{
        "Stale ghost target", {1.0, 1.0, 1.0}}));
    const auto stale_target = stale_source.snapshot().objects.front().object.id;
    REQUIRE(stale_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(stale_ui.dispatch(carto::application::SelectFaceAction{
        stale_target, carto::geometry::FaceId{1U}}));
    REQUIRE(stale_ui.queue_agent_step("inset face 0.1"));
    const auto stale_run = stale_ui.run_agent_plan("Prepare stale ghost target");
    REQUIRE(stale_run);
    const auto stale_branch_revision = stale_run.value().speculative_revision;
    auto stale_transform = stale_source.snapshot().objects.front().object.local_transform;
    stale_transform.translation.z += 0.5;
    REQUIRE(stale_ui.dispatch(carto::application::CreateBoxAction{
        "Concurrent source edit", {0.5, 0.5, 0.5}}));
    REQUIRE(!stale_ui.correct_agent_ghost_transform(
        stale_target, stale_transform, "Reject a correction based on a stale source."));
    REQUIRE(stale_ui.snapshot().agent_task->speculative_revision == stale_branch_revision);
}

void ui_ghost_vertex_correction_is_scoped_replayed_and_checkpointed() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Ghost vertex target", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Out-of-scope ghost vertex", {1.0, 1.0, 1.0}}));
    const auto base = source.snapshot();
    const auto target = base.objects[0].object.id;
    const auto out_of_scope = base.objects[1].object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    const auto run = ui.run_agent_plan("Prepare the selected face for vertex correction");
    REQUIRE(run && run.value().status == carto::agent::TaskStatus::review);
    const auto* after_before_correction = run.value().after_scene.find(target);
    REQUIRE(after_before_correction != nullptr && after_before_correction->mesh != nullptr);
    const auto vertex = after_before_correction->mesh->vertex_ids.front();
    const auto branch_vertex_position = std::find(
        after_before_correction->mesh->vertex_ids.begin(),
        after_before_correction->mesh->vertex_ids.end(), vertex);
    REQUIRE(branch_vertex_position != after_before_correction->mesh->vertex_ids.end());
    const auto branch_vertex_index = static_cast<std::size_t>(std::distance(
        after_before_correction->mesh->vertex_ids.begin(), branch_vertex_position));
    const auto before_position = after_before_correction->mesh->positions[branch_vertex_index];
    const auto source_revision = source.snapshot().project_revision;
    const auto branch_revision = run.value().speculative_revision;

    REQUIRE(!ui.correct_agent_ghost_vertex_position(target, vertex,
        before_position + carto::core::Vec3d{0.1, 0.0, 0.0}, ""));
    auto non_finite = before_position;
    non_finite.x = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!ui.correct_agent_ghost_vertex_position(
        target, vertex, non_finite, "Reject non-finite ghost vertex positions."));
    REQUIRE(!ui.correct_agent_ghost_vertex_position(
        out_of_scope, vertex, before_position + carto::core::Vec3d{0.1, 0.0, 0.0},
        "Reject a vertex outside the declared scope."));
    REQUIRE(!ui.correct_agent_ghost_vertex_position(
        target, carto::geometry::VertexId{999999U}, before_position,
        "Reject a missing ghost vertex."));
    REQUIRE(ui.snapshot().agent_task->speculative_revision == branch_revision);
    REQUIRE(source.snapshot().project_revision == source_revision);

    const auto corrected_position = before_position + carto::core::Vec3d{0.125, 0.0, 0.0};
    const auto corrected = ui.correct_agent_ghost_vertex_position(
        target, vertex, corrected_position,
        "Move the speculative corner away from the adjacent wall.");
    REQUIRE(corrected);
    REQUIRE(corrected.value().status == carto::agent::TaskStatus::review);
    REQUIRE(corrected.value().can_accept);
    REQUIRE(corrected.value().ghost_corrections.size() == 1U);
    const auto& receipt = corrected.value().ghost_corrections.front();
    REQUIRE(receipt.kind == carto::agent::GhostCorrectionKind::vertex_position);
    REQUIRE(receipt.object == target);
    REQUIRE(receipt.vertex == std::optional<carto::geometry::VertexId>(vertex));
    REQUIRE(receipt.before_position.x == before_position.x);
    REQUIRE(receipt.after_position.x == corrected_position.x);
    REQUIRE(receipt.validate());
    REQUIRE(corrected.value().speculative_revision == branch_revision.next());
    REQUIRE(source.snapshot().project_revision == source_revision);
    const auto gate = std::find_if(corrected.value().gates.begin(), corrected.value().gates.end(),
        [](const auto& value) { return value.id == "project.valid"; });
    REQUIRE(gate != corrected.value().gates.end() && gate->passed);

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "ghost-vertex.carto-agent-task";
    REQUIRE(ui.save_agent_checkpoint(checkpoint_path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().ghost_corrections.front().kind ==
        carto::agent::GhostCorrectionKind::vertex_position);
    carto::ui::UiController resumed_ui(source);
    REQUIRE(resumed_ui.load_agent_checkpoint(checkpoint_path));
    const auto resumed = resumed_ui.resume_agent_checkpoint();
    REQUIRE(resumed && resumed.value().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().can_accept);
    REQUIRE(resumed.value().ghost_corrections.front().evidence_digest == receipt.evidence_digest);
    const auto* resumed_instance = resumed.value().after_scene.find(target);
    REQUIRE(resumed_instance != nullptr && resumed_instance->mesh != nullptr);
    const auto resumed_vertex = std::find(
        resumed_instance->mesh->vertex_ids.begin(), resumed_instance->mesh->vertex_ids.end(), vertex);
    REQUIRE(resumed_vertex != resumed_instance->mesh->vertex_ids.end());
    const auto resumed_vertex_index = static_cast<std::size_t>(std::distance(
        resumed_instance->mesh->vertex_ids.begin(), resumed_vertex));
    REQUIRE(resumed_instance->mesh->positions[resumed_vertex_index].x == corrected_position.x);

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Unrelated source edit before vertex rebase", {0.5, 0.5, 0.5}}));
    const auto rebased_revision = source.snapshot().project_revision;
    carto::ui::UiController rebased_ui(source);
    REQUIRE(rebased_ui.load_agent_checkpoint(checkpoint_path));
    REQUIRE(rebased_ui.rebase_agent_checkpoint());
    const auto rebased = rebased_ui.resume_agent_checkpoint();
    REQUIRE(rebased && rebased.value().status == carto::agent::TaskStatus::review);
    REQUIRE(rebased.value().can_accept);
    REQUIRE(rebased.value().ghost_corrections.size() == 1U);
    REQUIRE(source.snapshot().project_revision == rebased_revision);
    REQUIRE(rebased_ui.accept_agent_plan());
    const auto accepted_snapshot = source.snapshot();
    const auto* accepted_instance = accepted_snapshot.viewport.scene.find(target);
    REQUIRE(accepted_instance != nullptr && accepted_instance->mesh != nullptr);
    const auto accepted_vertex = std::find(
        accepted_instance->mesh->vertex_ids.begin(), accepted_instance->mesh->vertex_ids.end(), vertex);
    REQUIRE(accepted_vertex != accepted_instance->mesh->vertex_ids.end());
    const auto accepted_vertex_index = static_cast<std::size_t>(std::distance(
        accepted_instance->mesh->vertex_ids.begin(), accepted_vertex));
    REQUIRE(accepted_instance->mesh->positions[accepted_vertex_index].x == corrected_position.x);
}

void ui_ghost_correction_recomputes_dependent_steps_and_persists_lineage() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    const auto objects = create_boxes(source, 2U);
    const auto vertex_source = source.snapshot();
    const auto target = objects[0];
    const auto dependent_target = objects[1];
    const auto* target_instance = vertex_source.viewport.scene.find(target);
    REQUIRE(target_instance != nullptr && target_instance->mesh != nullptr);
    REQUIRE(!target_instance->mesh->vertex_ids.empty());
    const auto vertex = target_instance->mesh->vertex_ids.front();

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{target, vertex}));
    REQUIRE(ui.queue_agent_step("move vertex to 0.25 0 0"));
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        dependent_target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1", {"step-1"}));

    const auto source_revision = source.snapshot().project_revision;
    const auto initial_run = ui.run_agent_plan(
        "Move one vertex, then edit the dependent face after review.");
    REQUIRE(initial_run && initial_run.value().status == carto::agent::TaskStatus::review);
    REQUIRE(initial_run.value().steps.size() == 2U);
    REQUIRE(initial_run.value().steps[0].status == carto::agent::StepStatus::passed);
    REQUIRE(initial_run.value().steps[1].status == carto::agent::StepStatus::passed);
    REQUIRE(initial_run.value().ghost_recomputes.empty());
    const auto old_dependent_step = initial_run.value().steps[1];
    const auto* edited_vertex_instance = initial_run.value().after_scene.find(target);
    REQUIRE(edited_vertex_instance != nullptr && edited_vertex_instance->mesh != nullptr);
    const auto edited_vertex = std::find(
        edited_vertex_instance->mesh->vertex_ids.begin(),
        edited_vertex_instance->mesh->vertex_ids.end(), vertex);
    REQUIRE(edited_vertex != edited_vertex_instance->mesh->vertex_ids.end());
    const auto edited_vertex_index = static_cast<std::size_t>(std::distance(
        edited_vertex_instance->mesh->vertex_ids.begin(), edited_vertex));
    const auto corrected_position =
        edited_vertex_instance->mesh->positions[edited_vertex_index] +
        carto::core::Vec3d{0.125, 0.0, 0.0};

    const auto corrected = ui.correct_agent_ghost_vertex_position(
        target, vertex, corrected_position,
        "Lock the corrected corner before recomputing dependent work.");
    REQUIRE(corrected && corrected.value().status == carto::agent::TaskStatus::review);
    REQUIRE(corrected.value().can_accept);
    REQUIRE(corrected.value().ghost_corrections.size() == 1U);
    REQUIRE(corrected.value().ghost_corrections.front().recompute_after_step_id ==
        std::optional<std::string>("step-1"));
    REQUIRE(corrected.value().ghost_recomputes.size() == 1U);
    const auto& recompute = corrected.value().ghost_recomputes.front();
    REQUIRE(recompute.task_id == corrected.value().task_id);
    REQUIRE(recompute.anchor_step_id == "step-1");
    REQUIRE(recompute.dependent_step_ids == std::vector<std::string>{"step-2"});
    REQUIRE(recompute.previous_step_receipts.size() == 2U);
    REQUIRE(recompute.previous_step_receipts[1].revision_before ==
        old_dependent_step.revision_before);
    REQUIRE(recompute.replay_completed);
    REQUIRE(!recompute.previous_diff_digest.is_zero());
    REQUIRE(recompute.result_diff_digest == corrected.value().diff.digest);
    REQUIRE(recompute.validate());
    REQUIRE(corrected.value().steps.size() == 2U);
    REQUIRE(corrected.value().steps[1].step_id == "step-2");
    REQUIRE(corrected.value().steps[1].revision_before ==
        old_dependent_step.revision_before.next());
    REQUIRE(source.snapshot().project_revision == source_revision);

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "ghost-dependent-recompute.carto-agent-task";
    REQUIRE(ui.save_agent_checkpoint(checkpoint_path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().ghost_recomputes.size() == 1U);
    REQUIRE(checkpoint.value().ghost_corrections.front().recompute_after_step_id ==
        std::optional<std::string>("step-1"));
    REQUIRE(checkpoint.value().ghost_recomputes.front().evidence_digest ==
        recompute.evidence_digest);

    auto forged_anchor = checkpoint.value();
    forged_anchor.ghost_corrections.front().recompute_after_step_id = "missing-step";
    const auto forged_correction_digest =
        forged_anchor.ghost_corrections.front().computed_evidence_digest();
    REQUIRE(forged_correction_digest);
    forged_anchor.ghost_corrections.front().evidence_digest =
        forged_correction_digest.value();
    REQUIRE(!forged_anchor.serialize());
    auto forged_budget = checkpoint.value();
    forged_budget.envelope.max_ghost_recomputations = 0U;
    REQUIRE(!forged_budget.serialize());

    carto::ui::UiController resumed_ui(source);
    REQUIRE(resumed_ui.load_agent_checkpoint(checkpoint_path));
    const auto resumed = resumed_ui.resume_agent_checkpoint();
    REQUIRE(resumed && resumed.value().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().ghost_recomputes.size() == 1U);
    REQUIRE(resumed.value().ghost_recomputes.front().evidence_digest ==
        recompute.evidence_digest);
    REQUIRE(resumed.value().steps[1].revision_before ==
        old_dependent_step.revision_before.next());
    REQUIRE(source.snapshot().project_revision == source_revision);
}

void ui_ghost_topology_correction_is_scoped_replayed_and_checkpointed() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Ghost topology target", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Out-of-scope ghost topology", {1.0, 1.0, 1.0}}));
    const auto base = source.snapshot();
    const auto target = base.objects[0].object.id;
    const auto out_of_scope = base.objects[1].object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    const auto run = ui.run_agent_plan("Prepare the selected face for topology review");
    REQUIRE(run && run.value().status == carto::agent::TaskStatus::review);
    const auto* before_instance = run.value().after_scene.find(target);
    REQUIRE(before_instance != nullptr && before_instance->mesh != nullptr);
    REQUIRE(!before_instance->mesh->triangle_faces.empty());
    const auto face = before_instance->mesh->triangle_faces.front();
    const auto branch_revision = run.value().speculative_revision;
    const auto source_revision = source.snapshot().project_revision;

    REQUIRE(!ui.correct_agent_ghost_face(
        target, face, carto::agent::GhostCorrectionKind::face_inset, 0.1, ""));
    REQUIRE(!ui.correct_agent_ghost_face(
        target, face, carto::agent::GhostCorrectionKind::face_inset,
        std::numeric_limits<double>::quiet_NaN(), "Reject a non-finite correction."));
    REQUIRE(!ui.correct_agent_ghost_face(
        target, face, carto::agent::GhostCorrectionKind::face_extrusion,
        -0.1, "Reject a negative correction distance."));
    REQUIRE(!ui.correct_agent_ghost_face(
        out_of_scope, face, carto::agent::GhostCorrectionKind::face_inset,
        0.05, "Reject a face outside the declared scope."));
    REQUIRE(!ui.correct_agent_ghost_face(
        target, carto::geometry::FaceId{999999U},
        carto::agent::GhostCorrectionKind::face_inset,
        0.05, "Reject a missing face identity."));
    REQUIRE(ui.snapshot().agent_task->speculative_revision == branch_revision);
    REQUIRE(source.snapshot().project_revision == source_revision);

    const auto first_corrected = ui.correct_agent_ghost_face(
        target, face, carto::agent::GhostCorrectionKind::face_inset, 0.05,
        "Inset the ghost face to preserve the intended frame clearance.");
    REQUIRE(first_corrected &&
        first_corrected.value().status == carto::agent::TaskStatus::review);
    const auto* first_corrected_instance = first_corrected.value().after_scene.find(target);
    REQUIRE(first_corrected_instance != nullptr && first_corrected_instance->mesh != nullptr);
    const auto extrusion_face = first_corrected_instance->mesh->triangle_faces.back();
    const auto corrected = ui.correct_agent_ghost_face(
        target, extrusion_face, carto::agent::GhostCorrectionKind::face_extrusion, 0.025,
        "Extrude the corrected ghost face to restore the shallow frame reveal.");
    REQUIRE(corrected && corrected.value().status == carto::agent::TaskStatus::review);
    REQUIRE(corrected.value().can_accept);
    REQUIRE(corrected.value().speculative_revision == branch_revision.next().next());
    REQUIRE(corrected.value().ghost_corrections.size() == 2U);
    const auto& receipt = corrected.value().ghost_corrections.front();
    REQUIRE(receipt.kind == carto::agent::GhostCorrectionKind::face_inset);
    REQUIRE(receipt.object == target);
    REQUIRE(receipt.face == std::optional<carto::geometry::FaceId>(face));
    REQUIRE(receipt.distance == 0.05);
    REQUIRE(!receipt.before_mesh_digest.is_zero());
    REQUIRE(!receipt.after_mesh_digest.is_zero());
    REQUIRE(receipt.before_mesh_digest != receipt.after_mesh_digest);
    REQUIRE(receipt.validate());
    const auto& extrusion_receipt = corrected.value().ghost_corrections.back();
    REQUIRE(extrusion_receipt.kind ==
        carto::agent::GhostCorrectionKind::face_extrusion);
    REQUIRE(extrusion_receipt.parent_evidence_digest == receipt.evidence_digest);
    REQUIRE(extrusion_receipt.before_mesh_digest == receipt.after_mesh_digest);
    REQUIRE(extrusion_receipt.validate());
    REQUIRE(source.snapshot().project_revision == source_revision);
    const auto* after_instance = corrected.value().after_scene.find(target);
    REQUIRE(after_instance != nullptr && after_instance->mesh != nullptr);
    REQUIRE(after_instance->mesh->triangle_faces.size() >
        before_instance->mesh->triangle_faces.size());

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "ghost-topology.carto-agent-task";
    REQUIRE(ui.save_agent_checkpoint(checkpoint_path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    auto forged = checkpoint.value();
    forged.ghost_corrections.front().distance += 0.01;
    REQUIRE(!forged.serialize());

    carto::ui::UiController resumed_ui(source);
    REQUIRE(resumed_ui.load_agent_checkpoint(checkpoint_path));
    const auto resumed = resumed_ui.resume_agent_checkpoint();
    REQUIRE(resumed && resumed.value().status == carto::agent::TaskStatus::review);
    REQUIRE(resumed.value().can_accept);
    REQUIRE(resumed.value().ghost_corrections.size() == 2U);
    REQUIRE(resumed.value().ghost_corrections.front().evidence_digest == receipt.evidence_digest);
    REQUIRE(resumed.value().ghost_corrections.back().evidence_digest ==
        extrusion_receipt.evidence_digest);
    const auto* resumed_instance = resumed.value().after_scene.find(target);
    REQUIRE(resumed_instance != nullptr && resumed_instance->mesh != nullptr);
    REQUIRE(resumed_instance->mesh->triangle_faces.size() ==
        after_instance->mesh->triangle_faces.size());

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Unrelated source edit before topology rebase", {0.5, 0.5, 0.5}}));
    const auto rebased_revision = source.snapshot().project_revision;
    carto::ui::UiController rebased_ui(source);
    REQUIRE(rebased_ui.load_agent_checkpoint(checkpoint_path));
    REQUIRE(rebased_ui.rebase_agent_checkpoint());
    const auto rebased = rebased_ui.resume_agent_checkpoint();
    REQUIRE(rebased && rebased.value().status == carto::agent::TaskStatus::review);
    REQUIRE(rebased.value().can_accept);
    REQUIRE(rebased.value().ghost_corrections.size() == 2U);
    REQUIRE(source.snapshot().project_revision == rebased_revision);
    REQUIRE(rebased_ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == rebased_revision.next());
    const auto accepted_instance = source.snapshot().viewport.scene.find(target);
    REQUIRE(accepted_instance != nullptr && accepted_instance->mesh != nullptr);
    REQUIRE(accepted_instance->mesh->triangle_faces.size() ==
        after_instance->mesh->triangle_faces.size());

    carto::application::ApplicationSession bounded_source;
    carto::ui::UiController bounded_ui(bounded_source);
    REQUIRE(bounded_ui.dispatch(carto::application::CreateBoxAction{
        "Topology-restricted target", {1.0, 1.0, 1.0}}));
    const auto bounded_target = bounded_source.snapshot().objects.front().object.id;
    REQUIRE(bounded_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    const carto::geometry::VertexId bounded_vertex{1U};
    REQUIRE(bounded_ui.dispatch(carto::application::SelectVertexAction{
        bounded_target, bounded_vertex}));
    REQUIRE(bounded_ui.set_agent_recipe("bounded-vertex-edit"));
    REQUIRE(bounded_ui.queue_agent_step("move vertex to 0.5 0 0"));
    const auto bounded_run = bounded_ui.run_agent_plan(
        "Move the selected vertex while preserving topology");
    REQUIRE(bounded_run && bounded_run.value().status == carto::agent::TaskStatus::review);
    const auto bounded_revision = bounded_run.value().speculative_revision;
    REQUIRE(!bounded_ui.correct_agent_ghost_face(
        bounded_target, carto::geometry::FaceId{1U},
        carto::agent::GhostCorrectionKind::face_extrusion,
        0.05, "A topology correction must exceed this recipe's authority."));
    REQUIRE(bounded_ui.snapshot().agent_task->speculative_revision == bounded_revision);

    carto::application::ApplicationSession shared_source;
    carto::ui::UiController shared_ui(shared_source);
    REQUIRE(shared_ui.dispatch(carto::application::CreateBoxAction{
        "Shared ghost mesh", {1.0, 1.0, 1.0}}));
    const auto shared_target = shared_source.snapshot().objects.front().object.id;
    REQUIRE(shared_ui.dispatch(carto::application::DuplicateMeshInstanceAction{
        shared_target, "Out-of-scope shared instance", {2.0, 0.0, 0.0}}));
    REQUIRE(shared_ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(shared_ui.dispatch(carto::application::SelectFaceAction{
        shared_target, carto::geometry::FaceId{1U}}));
    REQUIRE(shared_ui.queue_agent_step("inset face 0.1"));
    const auto shared_run = shared_ui.run_agent_plan("Prepare the shared face for review");
    const auto shared_task = shared_ui.snapshot().agent_task;
    REQUIRE(shared_task.has_value());
    REQUIRE(!shared_run || shared_run.value().status == shared_task->status);
    REQUIRE(shared_task->status == carto::agent::TaskStatus::review ||
        shared_task->status == carto::agent::TaskStatus::blocked);
    const auto shared_revision = shared_task->speculative_revision;
    REQUIRE(!shared_ui.correct_agent_ghost_face(
        shared_target, carto::geometry::FaceId{1U},
        carto::agent::GhostCorrectionKind::face_inset,
        0.05, "A shared out-of-scope instance must block this correction."));
    REQUIRE(shared_ui.snapshot().agent_task->speculative_revision == shared_revision);
}

void ui_ghost_corrections_remain_locked_through_repair() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Repair ghost target", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Separate repair target", {1.0, 1.0, 1.0}}));
    const auto initial = source.snapshot();
    const auto target = initial.objects[0].object.id;
    const auto repair_target = initial.objects[1].object.id;
    const auto source_before = source.snapshot();
    const auto* original_instance = source_before.viewport.scene.find(target);
    REQUIRE(original_instance != nullptr && original_instance->mesh != nullptr);
    const std::size_t original_triangle_count =
        original_instance->mesh->triangle_faces.size();
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 0.1"));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        repair_target, carto::geometry::FaceId{1U}}));
    REQUIRE(ui.queue_agent_step("inset face 1000"));
    const auto base_revision = source.snapshot().project_revision;
    const auto failed = ui.run_agent_plan(
        "Prepare two separate faces while preserving the first completed branch");
    REQUIRE(failed);
    REQUIRE(failed.value().status == carto::agent::TaskStatus::blocked);

    const auto blocked_branch = ui.snapshot().agent_task.value();
    const auto* blocked_target = blocked_branch.after_scene.find(target);
    REQUIRE(blocked_target != nullptr && blocked_target->mesh != nullptr);
    const auto correction_face = blocked_target->mesh->triangle_faces.front();
    const auto corrected = ui.correct_agent_ghost_face(
        target, correction_face,
        carto::agent::GhostCorrectionKind::face_inset, 0.05,
        "Inset the corrected ghost face while the failed branch is reviewed.");
    REQUIRE(corrected);
    REQUIRE(corrected.value().status == carto::agent::TaskStatus::blocked);
    REQUIRE(corrected.value().ghost_corrections.size() == 1U);
    const auto& correction = corrected.value().ghost_corrections.front();
    REQUIRE(correction.kind == carto::agent::GhostCorrectionKind::face_inset);
    REQUIRE(correction.validate());
    REQUIRE(source.snapshot().project_revision == base_revision);

    auto revised_step = ui.snapshot().agent_plan_draft.back();
    revised_step.intent = "inset face 0.1";
    REQUIRE(ui.revise_agent_plan_step(revised_step));
    const auto repaired = ui.repair_agent_plan(
        "Retry the other face with a bounded inset and keep the first ghost correction.");
    REQUIRE(repaired);
    REQUIRE(repaired.value().status == carto::agent::TaskStatus::review);
    REQUIRE(repaired.value().can_accept);
    REQUIRE(repaired.value().ghost_corrections.size() == 1U);
    REQUIRE(repaired.value().ghost_corrections.front().evidence_digest ==
        correction.evidence_digest);
    REQUIRE(repaired.value().repairs.size() == 1U);
    REQUIRE(repaired.value().repairs.front().previous_ghost_correction_count == 1U);
    REQUIRE(!repaired.value().repairs.front().previous_ghost_correction_digest.is_zero());
    REQUIRE(source.snapshot().project_revision == base_revision);

    TempDirectory directory;
    const auto checkpoint_path = directory.path() / "repaired-ghost.carto-agent-task";
    REQUIRE(ui.save_agent_checkpoint(checkpoint_path));
    const auto checkpoint = carto::agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(checkpoint.value().ghost_corrections.size() == 1U);
    REQUIRE(checkpoint.value().repair_lineage.front().previous_ghost_correction_count == 1U);
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(source.snapshot().project_revision == base_revision.next());
    const auto accepted_snapshot = source.snapshot();
    const auto accepted = std::find_if(accepted_snapshot.objects.begin(),
        accepted_snapshot.objects.end(), [target](const auto& value) {
            return value.object.id == target;
        });
    REQUIRE(accepted != accepted_snapshot.objects.end());
    REQUIRE(accepted->object.local_transform.translation.x == 0.0);
    const auto* accepted_instance = accepted_snapshot.viewport.scene.find(target);
    REQUIRE(accepted_instance != nullptr && accepted_instance->mesh != nullptr);
    REQUIRE(accepted_instance->mesh->triangle_faces.size() > original_triangle_count);
}

} // namespace

void ui_compiles_explicit_goal_sequences_into_reviewable_agent_dags() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Goal sequence target", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));

    const auto source_revision = source.snapshot().project_revision;
    const std::string goal =
        "translate object by 0.1 0 0 THEN translate object by 0.2 0 0";
    const auto compilation = ui.compile_agent_goal_sequence(goal);
    if (!compilation) {
        throw TestFailure("goal sequence compilation failed: " +
            compilation.error().message);
    }
    const auto compiled = ui.snapshot();
    REQUIRE(compiled.agent_plan_draft.size() == 2U);
    REQUIRE(compiled.agent_plan_draft[0].id == "step-1");
    REQUIRE(compiled.agent_plan_draft[0].dependencies.empty());
    REQUIRE(compiled.agent_plan_draft[1].id == "step-2");
    REQUIRE(compiled.agent_plan_draft[1].dependencies ==
        std::vector<std::string>{"step-1"});
    REQUIRE(compiled.agent_plan_draft[0].target.object == object);
    REQUIRE(compiled.agent_plan_draft[1].target.object == object);
    REQUIRE(std::holds_alternative<std::monostate>(
        compiled.agent_plan_draft[0].target.component));
    REQUIRE(source.snapshot().project_revision == source_revision);

    auto prepared = ui.prepare_agent_plan(goal, translation_envelope({}, 1.0));
    REQUIRE(prepared && prepared.value().status == carto::agent::TaskStatus::prepared);
    REQUIRE(source.snapshot().project_revision == source_revision);
    const auto review = ui.run_prepared_agent_plan();
    REQUIRE(review && review.value().status == carto::agent::TaskStatus::review);
    REQUIRE(review.value().can_accept);
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(source.snapshot().objects.front().object.local_transform.translation.x == 0.0);
    REQUIRE(ui.accept_agent_plan());
    const auto accepted = source.snapshot();
    REQUIRE(accepted.project_revision == source_revision.next());
    REQUIRE(accepted.objects.front().object.local_transform.translation.x > 0.29);
    REQUIRE(accepted.objects.front().object.local_transform.translation.x < 0.31);
}

void ui_compiles_multi_object_goals_into_independent_reviewed_branches() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Multi target A", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Multi target B", {1.0, 1.0, 1.0}}));
    const auto objects = source.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto first = objects[0].object.id;
    const auto second = objects[1].object.id;

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{first}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{
        second, carto::editor::SelectionOperation::add}));
    const auto source_revision = source.snapshot().project_revision;
    const std::string goal =
        "translate object by 0.1 0 0 then translate object by 0 0.2 0";

    const auto compilation = ui.compile_agent_goal_sequence(goal);
    if (!compilation) {
        throw TestFailure("multi-object goal compilation failed: " +
            compilation.error().message);
    }
    const auto compiled = ui.snapshot();
    REQUIRE(compiled.agent_plan_draft.size() == 4U);
    REQUIRE(compiled.agent_plan_draft[0].target.object == first);
    REQUIRE(compiled.agent_plan_draft[0].dependencies.empty());
    REQUIRE(compiled.agent_plan_draft[1].target.object == first);
    REQUIRE(compiled.agent_plan_draft[1].dependencies ==
        std::vector<std::string>{compiled.agent_plan_draft[0].id});
    REQUIRE(compiled.agent_plan_draft[2].target.object == second);
    REQUIRE(compiled.agent_plan_draft[2].dependencies.empty());
    REQUIRE(compiled.agent_plan_draft[3].target.object == second);
    REQUIRE(compiled.agent_plan_draft[3].dependencies ==
        std::vector<std::string>{compiled.agent_plan_draft[2].id});
    REQUIRE(source.snapshot().project_revision == source_revision);

    auto envelope = translation_envelope({}, 1.0);
    envelope.max_objects_touched = 2U;
    const auto prepared = ui.prepare_agent_plan(goal, std::move(envelope));
    REQUIRE(prepared && prepared.value().status == carto::agent::TaskStatus::prepared);
    const auto review = ui.run_prepared_agent_plan();
    if (!review) {
        throw TestFailure("multi-object plan execution failed: " + review.error().message);
    }
    if (review.value().status != carto::agent::TaskStatus::review) {
        throw TestFailure("multi-object plan was blocked: " +
            (review.value().diagnostic.has_value()
                ? review.value().diagnostic->message : "no task diagnostic"));
    }
    REQUIRE(review.value().can_accept);
    REQUIRE(review.value().steps.size() == 4U);
    REQUIRE(std::all_of(review.value().steps.begin(),
        review.value().steps.end(), [](const auto& receipt) {
            return receipt.status == carto::agent::StepStatus::passed;
        }));
    REQUIRE(source.snapshot().project_revision == source_revision);
    for (const auto& object : source.snapshot().objects) {
        REQUIRE(object.object.local_transform.translation.x == 0.0);
        REQUIRE(object.object.local_transform.translation.y == 0.0);
    }

    REQUIRE(ui.accept_agent_plan());
    const auto accepted = source.snapshot();
    REQUIRE(accepted.project_revision == source_revision.next());
    for (const auto& object : accepted.objects) {
        REQUIRE(object.object.local_transform.translation.x > 0.09);
        REQUIRE(object.object.local_transform.translation.x < 0.11);
        REQUIRE(object.object.local_transform.translation.y > 0.19);
        REQUIRE(object.object.local_transform.translation.y < 0.21);
    }
}

void ui_named_goal_targets_resolve_exact_names_and_ignore_selection_fallbacks() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Selected fallback", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Preserve topology", {1.0, 1.0, 1.0}}));
    const auto objects = source.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto fallback = objects[0].object.id;
    const auto named_target = objects[1].object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{fallback}));

    const std::string goal =
        R"(on object "Preserve topology": translate object by 0.1 0 0)";
    const auto parsed = carto::agent::parse_agent_goal(goal);
    REQUIRE(parsed);
    REQUIRE(parsed.value().exact_object_name == std::optional<std::string>{"Preserve topology"});
    REQUIRE(parsed.value().commands == "translate object by 0.1 0 0");
    const auto constraints = carto::agent::compile_explicit_hard_constraints(goal);
    REQUIRE(constraints && constraints.value().empty());

    const auto source_revision = source.snapshot().project_revision;
    const auto compilation = ui.compile_agent_goal_sequence(goal);
    if (!compilation) {
        throw TestFailure("named goal compilation failed: " +
            compilation.error().message);
    }
    const auto compiled = ui.snapshot();
    REQUIRE(compiled.agent_plan_draft.size() == 1U);
    REQUIRE(compiled.agent_plan_draft.front().target.object == named_target);
    REQUIRE(compiled.agent_plan_draft.front().intent == "translate object by 0.1 0 0");
    REQUIRE(compiled.selection.objects == std::vector<carto::scene::ObjectId>{fallback});
    REQUIRE(source.snapshot().project_revision == source_revision);

    auto prepared = ui.prepare_agent_plan(
        goal, translation_envelope({named_target}, 1.0));
    REQUIRE(prepared && prepared.value().status == carto::agent::TaskStatus::prepared);
    const auto review = ui.run_prepared_agent_plan();
    REQUIRE(review && review.value().status == carto::agent::TaskStatus::review);
    REQUIRE(review.value().can_accept);
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(ui.accept_agent_plan());
    const auto accepted = source.snapshot();
    const auto accepted_fallback = std::find_if(accepted.objects.begin(), accepted.objects.end(),
        [fallback](const auto& object) { return object.object.id == fallback; });
    const auto accepted_target = std::find_if(accepted.objects.begin(), accepted.objects.end(),
        [named_target](const auto& object) { return object.object.id == named_target; });
    REQUIRE(accepted_fallback != accepted.objects.end());
    REQUIRE(accepted_target != accepted.objects.end());
    REQUIRE(accepted_fallback->object.local_transform.translation.x == 0.0);
    REQUIRE(accepted_target->object.local_transform.translation.x > 0.09);
    REQUIRE(accepted_target->object.local_transform.translation.x < 0.11);
}

void ui_named_goal_target_resolution_rejects_missing_and_ambiguous_names() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Selected fallback", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Duplicate", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Duplicate", {1.0, 1.0, 1.0}}));
    const auto objects = source.snapshot().objects;
    REQUIRE(objects.size() == 3U);
    const auto fallback = objects.front().object.id;
    const auto duplicate_a = objects[1].object.id;
    const auto duplicate_b = objects[2].object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{fallback}));
    const auto source_revision = source.snapshot().project_revision;

    const auto ambiguous = ui.compile_agent_goal_sequence(
        R"(on object "Duplicate": translate object by 0.1 0 0)");
    REQUIRE(!ambiguous);
    REQUIRE(ambiguous.error().message.find("does not match") != std::string::npos);
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(!ui.snapshot().agent_task.has_value());

    const auto missing = ui.compile_agent_goal_sequence(
        R"(on object "Missing": translate object by 0.1 0 0)");
    REQUIRE(!missing);
    REQUIRE(missing.error().message.find("not found") != std::string::npos);
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(!ui.snapshot().agent_task.has_value());
    REQUIRE(ui.snapshot().selection.objects == std::vector<carto::scene::ObjectId>{fallback});
    REQUIRE(source.snapshot().project_revision == source_revision);

    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{duplicate_a}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{
        duplicate_b, carto::editor::SelectionOperation::add}));
    const auto multi_selected = ui.compile_agent_goal_sequence(
        R"(on object "Duplicate": translate object by 0.1 0 0)");
    REQUIRE(!multi_selected);
    REQUIRE(multi_selected.error().message.find("exactly one matching object") !=
        std::string::npos);
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(!ui.snapshot().agent_task.has_value());
    REQUIRE(source.snapshot().project_revision == source_revision);

    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{duplicate_a}));
    const auto resolved = ui.compile_agent_goal_sequence(
        R"(on object "Duplicate": translate object by 0.1 0 0)");
    if (!resolved) {
        throw TestFailure("selected duplicate name did not compile: " +
            resolved.error().message);
    }
    const auto compiled = ui.snapshot();
    REQUIRE(compiled.agent_plan_draft.size() == 1U);
    REQUIRE(compiled.agent_plan_draft.front().target.object == duplicate_a);
    REQUIRE(compiled.agent_clarifications_draft.empty());
    REQUIRE(compiled.selection.objects == std::vector<carto::scene::ObjectId>{duplicate_a});
    REQUIRE(source.snapshot().project_revision == source_revision);

    auto prepared = ui.prepare_agent_plan(
        R"(on object "Duplicate": translate object by 0.1 0 0)",
        translation_envelope({duplicate_a}, 1.0));
    REQUIRE(prepared && prepared.value().status == carto::agent::TaskStatus::prepared);
    REQUIRE(prepared.value().scope_objects ==
        std::vector<carto::scene::ObjectId>{duplicate_a});
    const auto review = ui.run_prepared_agent_plan();
    REQUIRE(review && review.value().status == carto::agent::TaskStatus::review);
    REQUIRE(review.value().can_accept);
    REQUIRE(source.snapshot().project_revision == source_revision);
    REQUIRE(ui.accept_agent_plan());

    const auto accepted = source.snapshot();
    const auto accepted_fallback = std::find_if(accepted.objects.begin(), accepted.objects.end(),
        [fallback](const auto& object) { return object.object.id == fallback; });
    const auto accepted_a = std::find_if(accepted.objects.begin(), accepted.objects.end(),
        [duplicate_a](const auto& object) { return object.object.id == duplicate_a; });
    const auto accepted_b = std::find_if(accepted.objects.begin(), accepted.objects.end(),
        [duplicate_b](const auto& object) { return object.object.id == duplicate_b; });
    REQUIRE(accepted_fallback != accepted.objects.end());
    REQUIRE(accepted_a != accepted.objects.end());
    REQUIRE(accepted_b != accepted.objects.end());
    REQUIRE(accepted_fallback->object.local_transform.translation.x == 0.0);
    REQUIRE(accepted_b->object.local_transform.translation.x == 0.0);
    REQUIRE(accepted_a->object.local_transform.translation.x > 0.09);
    REQUIRE(accepted_a->object.local_transform.translation.x < 0.11);
}

void agent_goal_scope_parser_rejects_malformed_prefixes_and_decodes_only_safe_escapes() {
    const auto escaped = carto::agent::parse_agent_goal(
        R"(on object "Lamp \"A\" \\ B": translate object by 0.1 0 0)");
    REQUIRE(escaped);
    REQUIRE(escaped.value().exact_object_name == std::optional<std::string>{"Lamp \"A\" \\ B"});
    REQUIRE(escaped.value().commands == "translate object by 0.1 0 0");

    REQUIRE(!carto::agent::parse_agent_goal(
        R"(on object "Lamp\q": translate object by 0.1 0 0)"));
    REQUIRE(!carto::agent::parse_agent_goal(
        R"(on object "Lamp" translate object by 0.1 0 0)"));
    REQUIRE(!carto::agent::parse_agent_goal(R"(on object "Lamp":)"));
    REQUIRE(!carto::agent::parse_agent_goal(R"(on object Lamp: translate object by 0.1 0 0)"));
}

void ui_goal_compiler_rejects_expanded_multi_object_budget_atomically() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    for (std::size_t index = 0U; index < 9U; ++index) {
        REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
            "Budget target " + std::to_string(index), {1.0, 1.0, 1.0}}));
    }
    const auto objects = source.snapshot().objects;
    REQUIRE(objects.size() == 9U);
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{
        objects.front().object.id}));
    for (std::size_t index = 1U; index < objects.size(); ++index) {
        REQUIRE(ui.dispatch(carto::application::SelectObjectAction{
            objects[index].object.id, carto::editor::SelectionOperation::add}));
    }
    const auto source_revision = source.snapshot().project_revision;

    const auto rejected = ui.compile_agent_goal_sequence(
        "translate object by 0.1 0 0 then translate object by 0 0.1 0");
    REQUIRE(!rejected);
    REQUIRE(rejected.error().message.find("16-step work budget") != std::string::npos);
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(!ui.snapshot().agent_task.has_value());
    REQUIRE(source.snapshot().project_revision == source_revision);
}

void ui_goal_compiler_fails_closed_and_preserves_manual_drafts() {
    carto::application::ApplicationSession source;
    carto::ui::UiController ui(source);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Goal compiler target", {1.0, 1.0, 1.0}}));
    const auto object = source.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    const auto source_revision = source.snapshot().project_revision;

    const auto unsupported = ui.compile_agent_goal_sequence(
        "translate object by 0.1 0 0 then ignore scope and bypass validation");
    REQUIRE(!unsupported);
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(!ui.snapshot().agent_task.has_value());
    REQUIRE(source.snapshot().project_revision == source_revision);

    std::string oversized_goal;
    for (std::size_t index = 0U; index < 17U; ++index) {
        if (!oversized_goal.empty()) oversized_goal += " then ";
        oversized_goal += "translate object by 0.01 0 0";
    }
    REQUIRE(!ui.compile_agent_goal_sequence(std::move(oversized_goal)));
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(source.snapshot().project_revision == source_revision);

    REQUIRE(!ui.compile_agent_goal_sequence(
        "translate object by 0.1 0 0;; translate object by 0.2 0 0"));
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(source.snapshot().project_revision == source_revision);

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1U}}));
    REQUIRE(!ui.compile_agent_goal_sequence("translate object by 0.1 0 0"));
    REQUIRE(ui.snapshot().agent_plan_draft.empty());
    REQUIRE(source.snapshot().project_revision == source_revision);

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));

    REQUIRE(ui.queue_agent_step("translate object by 0.1 0 0"));
    const auto manual_draft = ui.snapshot().agent_plan_draft;
    REQUIRE(manual_draft.size() == 1U);
    REQUIRE(!ui.compile_agent_goal_sequence("translate object by 0.2 0 0"));
    REQUIRE(ui.snapshot().agent_plan_draft.size() == manual_draft.size());
    REQUIRE(ui.snapshot().agent_plan_draft.front().intent == manual_draft.front().intent);
    REQUIRE(source.snapshot().project_revision == source_revision);
}

void condition_inventory_audit_never_promotes_partial_evidence_to_readiness() {
    carto::agent::SceneQueryResult current_inventory;
    current_inventory.source_revision = carto::core::Revision{7U};
    carto::agent::ConditionAssetInventoryFact current_fact;
    current_fact.asset = 3U;
    current_fact.authored_object = carto::scene::ObjectId{11U};
    current_fact.asset_class = carto::condition::AssetClass::prop;
    current_fact.fidelity = carto::condition::FidelityClass::standard;
    current_fact.source_id = 5U;
    current_fact.source_revision = "source-r4";
    current_fact.source_current = true;
    current_fact.source_reviewed = true;
    current_fact.related_records_complete = true;
    current_inventory.rows.emplace_back(current_fact);

    const auto current_audit =
        carto::agent::audit_condition_inventory(current_inventory);
    REQUIRE(current_audit.query_complete);
    REQUIRE(current_audit.source_evidence_complete);
    REQUIRE(current_audit.metadata_scan_complete);
    REQUIRE(current_audit.assets.size() == 1U);
    REQUIRE(current_audit.assets.front().source_status ==
        carto::agent::ConditionEvidenceStatus::verified);
    REQUIRE(current_audit.assets.front().metadata_scan_status ==
        carto::agent::ConditionEvidenceStatus::verified);
    REQUIRE(!current_audit.production_readiness_established);

    auto stale_inventory = current_inventory;
    auto& stale_fact = std::get<carto::agent::ConditionAssetInventoryFact>(
        stale_inventory.rows.front());
    stale_fact.source_current = false;
    stale_fact.source_reviewed = false;
    const auto stale_audit = carto::agent::audit_condition_inventory(stale_inventory);
    REQUIRE(stale_audit.query_complete);
    REQUIRE(!stale_audit.source_evidence_complete);
    REQUIRE(stale_audit.assets.front().source_status ==
        carto::agent::ConditionEvidenceStatus::needs_review);
    REQUIRE(!stale_audit.production_readiness_established);

    auto malformed_lineage = current_inventory;
    auto& malformed_fact = std::get<carto::agent::ConditionAssetInventoryFact>(
        malformed_lineage.rows.front());
    malformed_fact.source_id = 0U;
    malformed_fact.source_revision.clear();
    const auto malformed_audit =
        carto::agent::audit_condition_inventory(malformed_lineage);
    REQUIRE(!malformed_audit.source_evidence_complete);
    REQUIRE(malformed_audit.assets.front().source_status ==
        carto::agent::ConditionEvidenceStatus::unavailable);

    auto partial_inventory = current_inventory;
    partial_inventory.truncated = true;
    partial_inventory.diagnostics.push_back(carto::core::Diagnostic(
        carto::core::ErrorCode::stale_data, "partial inventory fixture"));
    const auto partial_audit =
        carto::agent::audit_condition_inventory(partial_inventory);
    REQUIRE(!partial_audit.query_complete);
    REQUIRE(!partial_audit.source_evidence_complete);
    REQUIRE(!partial_audit.metadata_scan_complete);
    REQUIRE(partial_audit.assets.front().metadata_scan_status ==
        carto::agent::ConditionEvidenceStatus::incomplete);
    REQUIRE(!partial_audit.production_readiness_established);

    auto mixed_inventory = current_inventory;
    mixed_inventory.rows.emplace_back(carto::agent::SceneObjectIdentityFact{
        carto::scene::ObjectId{99U}});
    const auto mixed_audit =
        carto::agent::audit_condition_inventory(mixed_inventory);
    REQUIRE(!mixed_audit.query_complete);
    REQUIRE(mixed_audit.unrecognized_rows == 1U);
    REQUIRE(!mixed_audit.source_evidence_complete);
    REQUIRE(!mixed_audit.production_readiness_established);

    auto duplicate_inventory = current_inventory;
    duplicate_inventory.rows.emplace_back(current_fact);
    const auto duplicate_audit =
        carto::agent::audit_condition_inventory(duplicate_inventory);
    REQUIRE(!duplicate_audit.query_complete);
    REQUIRE(duplicate_audit.unrecognized_rows == 1U);
    REQUIRE(!duplicate_audit.source_evidence_complete);

    auto invalid_class_inventory = current_inventory;
    auto& invalid_class_fact = std::get<carto::agent::ConditionAssetInventoryFact>(
        invalid_class_inventory.rows.front());
    invalid_class_fact.asset_class =
        static_cast<carto::condition::AssetClass>(255U);
    const auto invalid_class_audit =
        carto::agent::audit_condition_inventory(invalid_class_inventory);
    REQUIRE(!invalid_class_audit.query_complete);
    REQUIRE(invalid_class_audit.unrecognized_rows == 1U);
    REQUIRE(!invalid_class_audit.production_readiness_established);

    auto missing_revision_inventory = current_inventory;
    missing_revision_inventory.source_revision = carto::core::Revision{};
    const auto missing_revision_audit =
        carto::agent::audit_condition_inventory(missing_revision_inventory);
    REQUIRE(!missing_revision_audit.query_complete);
    REQUIRE(!missing_revision_audit.source_evidence_complete);

    const auto empty_audit = carto::agent::audit_condition_inventory(
        carto::agent::SceneQueryResult{});
    REQUIRE(!empty_audit.query_complete);
    REQUIRE(empty_audit.assets.empty());
    REQUIRE(!empty_audit.source_evidence_complete);
    REQUIRE(!empty_audit.metadata_scan_complete);
    REQUIRE(!empty_audit.production_readiness_established);
}

int main() {
    try {
        root_object_translation_is_explicit_bounded_and_checkpointed();
        root_object_translation_rejects_distance_overrun_and_preserve_constraint();
        object_translation_checkpoint_rebase_tracks_transform_write_set();
        ui_objective_constraints_are_scoped_persisted_and_bound_to_answers();
        ui_workbench_captures_object_target_and_requires_explicit_transform_envelope();
        multi_step_branch_isolated_until_reviewed_acceptance();
        accepted_task_demonstration_is_redacted_read_only_and_checksummed();
        vertex_demonstration_stores_relative_displacement_only();
        human_native_command_range_captures_alias_only_trace();
        native_demonstrations_apply_to_unique_exact_structural_matches();
        native_demonstration_multi_command_translations_match_similar_objects();
        native_demonstration_multi_command_vertex_edits_match_and_accumulate();
        native_demonstrations_skip_ambiguous_structural_matches();
        plan_dag_orders_dependencies_and_rejects_invalid_graphs();
        failed_dag_node_invalidates_only_dependent_steps();
        bounded_repair_restarts_from_source_and_preserves_attempt_lineage();
        repair_rejects_expansion_stale_source_empty_justification_and_exhaustion();
        saved_acceptance_is_one_durable_application_event();
        work_envelope_blocks_topology_effect_before_live_mutation();
        work_envelope_blocks_topology_churn_over_budget();
        diff_digest_tracks_geometry_values_not_only_change_shape();
        bounded_mesh_recipe_caps_execution_and_binds_its_acceptance_gate();
        bounded_vertex_recipe_preserves_topology_and_repair_authority();
        shared_mesh_instances_must_be_in_scope_and_within_touch_budget();
        scope_protection_and_stale_revision_fail_closed();
        failed_native_step_and_invalid_task_transitions_do_not_commit();
        missing_goal_and_source_change_after_run_fail_closed();
        clarification_frontier_blocks_consequential_ambiguity_and_binds_answers();
        clarification_receipt_rejects_changed_plan_graph();
        objective_hard_constraints_block_topology_and_excess_displacement();
        explicit_objective_compilation_is_bounded_and_fails_closed();
        objective_compiler_binds_explicit_transform_and_displacement_limits();
        ui_compiled_constraints_are_bound_to_clarification_and_gate_execution();
        acceptance_evidence_binds_validator_revision_assets_and_result();
        ui_workbench_captures_selection_and_exposes_accept_reject_flow();
        ui_agent_workbench_pokes_faces_only_on_a_reviewed_branch();
        ui_agent_entry_preserves_context_selection_across_workspaces();
        ui_prepared_task_supports_condition_preflight_and_rechecks_source_revision();
        ui_recipe_selection_round_trips_and_locks_with_checkpoint();
        ui_clarification_requires_an_operator_answer_before_review();
        ui_candidate_clarification_choice_is_scoped_and_receipted();
        ui_repairs_failed_step_with_operator_justification_and_scope_checks();
        ui_repair_invalidates_plan_bound_human_receipts_until_reanswered();
        ui_ghost_corrections_are_scoped_revalidated_and_checkpointed();
        ui_ghost_vertex_correction_is_scoped_replayed_and_checkpointed();
        ui_ghost_correction_recomputes_dependent_steps_and_persists_lineage();
        ui_ghost_topology_correction_is_scoped_replayed_and_checkpointed();
        ui_ghost_corrections_remain_locked_through_repair();
        persistent_checkpoint_replays_review_and_blocks_artifact_drift();
        persistent_checkpoint_reauthorization_and_stale_source_are_fail_closed();
        checkpoint_rebase_reexecutes_only_the_affected_dag_frontier();
        checkpoint_rebase_blocks_changed_task_write_targets();
        checkpoint_rebase_reports_and_executes_only_the_affected_dag_branch();
        prepared_checkpoint_requires_explicit_resume_before_native_execution();
        persistent_checkpoint_rejects_corruption_and_overwrite();
        persistent_checkpoint_v1_format_remains_readable();
        ui_persistent_checkpoint_round_trip_preserves_human_boundary();
        ui_rebases_disjoint_source_edits_and_reauthorizes_clarifications();
        ui_compiles_explicit_goal_sequences_into_reviewable_agent_dags();
        ui_compiles_multi_object_goals_into_independent_reviewed_branches();
        ui_named_goal_targets_resolve_exact_names_and_ignore_selection_fallbacks();
        ui_named_goal_target_resolution_rejects_missing_and_ambiguous_names();
        agent_goal_scope_parser_rejects_malformed_prefixes_and_decodes_only_safe_escapes();
        ui_goal_compiler_rejects_expanded_multi_object_budget_atomically();
        ui_goal_compiler_fails_closed_and_preserves_manual_drafts();
        condition_inventory_audit_never_promotes_partial_evidence_to_readiness();
        std::cout << "Cartographer agent workbench invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
