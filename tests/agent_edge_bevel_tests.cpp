#include <carto/agent/workbench.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/transaction.hpp>
#include <carto/ui/ui.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <algorithm>
#include <iostream>
#include <span>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace carto;
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string("requirement failed at line ") + std::to_string(__LINE__) + ": " + #condition); } while (false)
class TempDirectory final {
public:
    TempDirectory() : path_(std::filesystem::temp_directory_path() /
        ("cartographer-edge-bevel-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))) {
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

scene::ObjectId create_box(ui::UiController& ui) {
    REQUIRE(ui.dispatch(application::CreateBoxAction{"Bevel target", {1.0, 1.0, 1.0}}));
    return ui.snapshot().objects.front().object.id;
}
void select_edge(ui::UiController& ui, scene::ObjectId object) {
    REQUIRE(ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::edge}));
    REQUIRE(ui.dispatch(application::SelectEdgeAction{object, geometry::EdgeId{1U}}));
}
void require_same_mesh_geometry(const application::ApplicationSnapshot& a,
    const application::ApplicationSnapshot& b, scene::ObjectId object) {
    const auto& left = *a.viewport.scene.find(object)->mesh;
    const auto& right = *b.viewport.scene.find(object)->mesh;
    REQUIRE(left.indices == right.indices);
    REQUIRE(left.vertex_ids == right.vertex_ids);
    REQUIRE(left.triangle_faces == right.triangle_faces);
    REQUIRE(left.positions.size() == right.positions.size());
    for (std::size_t i = 0U; i < left.positions.size(); ++i) {
        REQUIRE(left.positions[i].x == right.positions[i].x);
        REQUIRE(left.positions[i].y == right.positions[i].y);
        REQUIRE(left.positions[i].z == right.positions[i].z);
    }
}
void require_same_mesh(const application::ApplicationSnapshot& a,
    const application::ApplicationSnapshot& b, scene::ObjectId object,
    std::source_location location = std::source_location::current()) {
    require_same_mesh_geometry(a, b, object);
    if (a.viewport.scene.find(object)->mesh->triangle_edges !=
        b.viewport.scene.find(object)->mesh->triangle_edges) throw std::runtime_error(
            "compiled edge identity mismatch at comparison line " + std::to_string(location.line()));
}

void require_same_reopened_topology(const application::ApplicationSnapshot& reopened,
    const application::ApplicationSnapshot& original, scene::ObjectId object) {
    require_same_mesh_geometry(reopened, original, object);
    const auto& left = *reopened.viewport.scene.find(object)->mesh;
    const auto& right = *original.viewport.scene.find(object)->mesh;
    REQUIRE(left.triangle_edges.size() == right.triangle_edges.size());
    // Project serialization stores vertex/face identity and reconstructs edge
    // adjacency via insert_bulk. Live edge numbers can change on reopen. Bind
    // every boundary edge to the same vertex pair and require a bijection, not
    // merely equal counts or an unchecked omission of the edge topology.
    std::map<geometry::EdgeId, geometry::EdgeId> old_to_new, new_to_old;
    std::map<geometry::EdgeId, std::pair<geometry::VertexId, geometry::VertexId>> endpoints;
    for (std::size_t triangle = 0U; triangle < left.triangle_edges.size(); ++triangle) {
        for (std::size_t side = 0U; side < 3U; ++side) {
            const auto before = right.triangle_edges[triangle][side];
            const auto after = left.triangle_edges[triangle][side];
            REQUIRE(before.has_value() == after.has_value());
            if (!before) continue;
            REQUIRE(*before && *after);
            const auto old_entry = old_to_new.emplace(*before, *after);
            REQUIRE(old_entry.second || old_entry.first->second == *after);
            const auto new_entry = new_to_old.emplace(*after, *before);
            REQUIRE(new_entry.second || new_entry.first->second == *before);
            auto first = left.vertex_ids[left.indices[triangle * 3U + side]];
            auto second = left.vertex_ids[left.indices[triangle * 3U + (side + 1U) % 3U]];
            if (second < first) std::swap(first, second);
            const auto endpoint = endpoints.emplace(*after, std::make_pair(first, second));
            REQUIRE(endpoint.second || endpoint.first->second == std::make_pair(first, second));
        }
    }
    REQUIRE(!old_to_new.empty());
    REQUIRE(old_to_new.size() == new_to_old.size());
}

agent::AgentPlan plan_for(const application::ApplicationSession& source,
    scene::ObjectId target, std::vector<scene::ObjectId> scope,
    std::vector<scene::ObjectId> protected_objects = {}) {
    return {"bevel-task", agent::AuthoringObjective{"bevel-objective", "Bevel one edge",
        source.snapshot().project_revision, std::move(scope), std::move(protected_objects), {}, {}, {}},
        {{"bevel-step", {target, geometry::EdgeId{1U}}, "bevel edge 0.1 segments 1", {}}}};
}
agent::AgentWorkEnvelope envelope_for(std::vector<scene::ObjectId> scope) {
    agent::AgentWorkEnvelope result;
    result.scope_objects = std::move(scope);
    result.max_commands = 1U;
    result.max_objects_touched = result.scope_objects.size();
    return result;
}
void require_blocked(application::ApplicationSession& source, agent::AgentPlan plan,
    agent::AgentWorkEnvelope envelope, std::string_view diagnostic) {
    const auto before = source.snapshot();
    auto task = agent::AgentTask::begin(source, std::move(plan), std::move(envelope));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == agent::TaskStatus::blocked);
    REQUIRE(!run.value().can_accept);
    bool found = run.value().diagnostic &&
        run.value().diagnostic->message.find(diagnostic) != std::string::npos;
    for (const auto& gate : run.value().gates) {
        found = found || (gate.diagnostic && gate.diagnostic->message.find(diagnostic) != std::string::npos);
    }
    if (!found) throw std::runtime_error("Expected diagnostic: " + std::string(diagnostic) +
        "; got: " + (run.value().diagnostic ? run.value().diagnostic->message : "no task diagnostic"));
    REQUIRE(!application::HumanApplicationAccess::accept_agent_task(task.value()));
    const auto after = source.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.project_generation == before.project_generation);
    REQUIRE(after.undo_count == before.undo_count);
    for (const auto& object : before.objects) require_same_mesh(after, before, object.object.id);
}

void native_and_agent_bevel_match_with_reviewed_history_and_persistence() {
    TempDirectory directory;
    application::ApplicationSession source, human;
    ui::UiController ui(source), human_ui(human);
    const auto object = create_box(ui);
    REQUIRE(create_box(human_ui) == object);
    select_edge(ui, object);
    select_edge(human_ui, object);
    const auto before = source.snapshot();
    REQUIRE(!ui.apply_ai_intent("bevel edge 0.1"));
    REQUIRE(ui.queue_agent_step("bevel edge 0.1"));
    const auto run = ui.run_agent_plan("Bevel the selected edge");
    REQUIRE(run);
    if (!run.value().can_accept) throw std::runtime_error(
        run.value().diagnostic ? run.value().diagnostic->message : "bevel not reviewable");
    REQUIRE(run.value().status == agent::TaskStatus::review);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    require_same_mesh(source.snapshot(), before, object);
    editor::ToolArguments arguments;
    arguments.distance = 0.1;
    arguments.segments = 1U;
    REQUIRE(human_ui.dispatch(application::InvokeToolAction{"mesh.bevel-edge", arguments}));
    REQUIRE(ui.accept_agent_plan());
    REQUIRE(!ui.accept_agent_plan());
    const auto after = source.snapshot();
    require_same_mesh(after, human.snapshot(), object);
    REQUIRE(after.project_revision == before.project_revision.next());
    REQUIRE(after.viewport.scene.find(object)->mesh->vertex_ids.size() >
        before.viewport.scene.find(object)->mesh->vertex_ids.size());
    REQUIRE(after.undo_count == 1U);
    REQUIRE(ui.dispatch(application::UndoAction{}));
    require_same_mesh(source.snapshot(), before, object);
    REQUIRE(ui.dispatch(application::RedoAction{}));
    require_same_mesh(source.snapshot(), after, object);
    const auto path = directory.path() / "beveled.carto";
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    application::ApplicationSession reopened;
    REQUIRE(application::HumanApplicationAccess::dispatch(reopened, application::OpenProjectAction{path}));
    require_same_reopened_topology(reopened.snapshot(), after, object);
}

void bevel_checkpoint_roundtrips_and_replays_without_implicit_acceptance() {
    TempDirectory directory;
    application::ApplicationSession source;
    ui::UiController ui(source);
    const auto object = create_box(ui);
    const auto before = source.snapshot();
    const auto plan = plan_for(source, object, {object});
    auto task = agent::AgentTask::begin(source, plan, envelope_for({object}));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run && run.value().can_accept);
    const auto checkpoint_path = directory.path() / "bevel.carto-agent-task";
    REQUIRE(task.value().save_checkpoint(checkpoint_path));
    const auto checkpoint = agent::AgentTask::load_checkpoint(checkpoint_path);
    REQUIRE(checkpoint);
    REQUIRE(std::get<geometry::EdgeId>(checkpoint.value().plan.steps.front().target.component).value == 1U);
    REQUIRE(checkpoint.value().plan.steps.front().intent == "bevel edge 0.1 segments 1");
    auto resumed = agent::AgentTask::resume_checkpoint(source, checkpoint.value(), plan);
    REQUIRE(resumed);
    REQUIRE(resumed.value().snapshot().can_accept);
    REQUIRE(resumed.value().snapshot().diff.digest == checkpoint.value().diff_digest);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    require_same_mesh(source.snapshot(), before, object);
    REQUIRE(application::HumanApplicationAccess::accept_agent_task(resumed.value()));
    REQUIRE(!application::HumanApplicationAccess::accept_agent_task(resumed.value()));
    REQUIRE(!agent::AgentTask::resume_checkpoint(source, checkpoint.value(), plan));
}

void shared_scope_protection_and_budget_guards_preserve_source() {
    application::ApplicationSession source;
    ui::UiController ui(source);
    const auto target = create_box(ui);
    REQUIRE(ui.dispatch(application::DuplicateMeshInstanceAction{target, "Shared sibling", {2.0, 0.0, 0.0}}));
    const auto objects = source.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto sibling = objects[0].object.id == target ? objects[1].object.id : objects[0].object.id;
    require_blocked(source, plan_for(source, target, {target}), envelope_for({target}),
        "shared by an object outside the work envelope");
    require_blocked(source, plan_for(source, target, {target, sibling}, {sibling}),
        envelope_for({target, sibling}), "protected by the objective");
    auto budget = envelope_for({target, sibling});
    budget.max_objects_touched = 1U;
    require_blocked(source, plan_for(source, target, {target, sibling}), budget, "object-touch budget");
    require_blocked(source, plan_for(source, target, {target, sibling}), envelope_for({target, sibling}),
        "project edge bevel refuses mesh assets shared by other objects");
}

void effects_and_topology_budget_preserve_source() {
    application::ApplicationSession source;
    ui::UiController ui(source);
    const auto target = create_box(ui);
    auto geometry_only = envelope_for({target});
    geometry_only.permitted_effects = agent::effect_mask(agent::Effect::geometry);
    require_blocked(source, plan_for(source, target, {target}), geometry_only,
        "outside the native ontology");
    auto no_topology = envelope_for({target});
    no_topology.max_topology_operations = 0U;
    require_blocked(source, plan_for(source, target, {target}), no_topology, "topology-operation budget");
}

void prepared_edge_checkpoint_refuses_rebound_identity() {
    TempDirectory directory;
    application::ApplicationSession original, canonical;
    ui::UiController ui(original);
    const auto object = create_box(ui);
    select_edge(ui, object);
    editor::ToolArguments arguments;
    arguments.distance = 0.1;
    arguments.segments = 1U;
    REQUIRE(ui.dispatch(application::InvokeToolAction{"mesh.bevel-edge", arguments}));
    const auto path = directory.path() / "edited.carto";
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    REQUIRE(application::HumanApplicationAccess::dispatch(canonical, application::OpenProjectAction{path}));
    const auto old_snapshot = original.snapshot(), new_snapshot = canonical.snapshot();
    REQUIRE(old_snapshot.project_revision == new_snapshot.project_revision);
    const auto edge_endpoints = [object](const application::ApplicationSnapshot& snapshot) {
        std::map<geometry::EdgeId, std::pair<geometry::VertexId, geometry::VertexId>> result;
        const auto& mesh = *snapshot.viewport.scene.find(object)->mesh;
        for (std::size_t t = 0U; t < mesh.triangle_edges.size(); ++t) {
            for (std::size_t side = 0U; side < 3U; ++side) {
                if (!mesh.triangle_edges[t][side]) continue;
                auto a = mesh.vertex_ids[mesh.indices[t * 3U + side]];
                auto b = mesh.vertex_ids[mesh.indices[t * 3U + (side + 1U) % 3U]];
                if (b < a) std::swap(a, b);
                result.emplace(*mesh.triangle_edges[t][side], std::make_pair(a, b));
            }
        }
        return result;
    };
    const auto old_edges = edge_endpoints(old_snapshot), new_edges = edge_endpoints(new_snapshot);
    geometry::EdgeId rebound{};
    for (const auto& [id, endpoints] : old_edges) {
        const auto found = new_edges.find(id);
        if (found != new_edges.end() && found->second != endpoints) { rebound = id; break; }
    }
    REQUIRE(rebound);
    auto plan = plan_for(original, object, {object});
    plan.steps.front().target.component = rebound;
    plan.steps.front().intent = "split edge 0.25";
    auto unsafe = agent::AgentTask::begin(original, plan, envelope_for({object}));
    REQUIRE(unsafe);
    const auto refused_path = directory.path() / "rebound.carto-agent-task";
    const auto saved = unsafe.value().save_checkpoint(refused_path);
    REQUIRE(!saved);
    REQUIRE(saved.error().message.find("endpoint identity") != std::string::npos);
    REQUIRE(!std::filesystem::exists(refused_path));
    auto safe = agent::AgentTask::begin(canonical, plan, envelope_for({object}));
    REQUIRE(safe);
    const auto safe_path = directory.path() / "canonical.carto-agent-task";
    REQUIRE(safe.value().save_checkpoint(safe_path));
    const auto checkpoint = agent::AgentTask::load_checkpoint(safe_path);
    REQUIRE(checkpoint && checkpoint.value().status == agent::TaskStatus::prepared);
    // Serialized source bytes and revisions match, but the live edge binding
    // differs. A prepared replay must reject before executing the wrong edge.
    const auto wrong_resume = agent::AgentTask::resume_checkpoint(original, checkpoint.value(), plan);
    REQUIRE(!wrong_resume);
    REQUIRE(wrong_resume.error().message.find("endpoint identity") != std::string::npos);
    REQUIRE(!agent::AgentTask::analyze_checkpoint_rebase(original, checkpoint.value()));
    REQUIRE(!agent::AgentTask::rebase_checkpoint(original, checkpoint.value(), plan));
    const auto right_resume = agent::AgentTask::resume_checkpoint(canonical, checkpoint.value(), plan);
    REQUIRE(right_resume && right_resume.value().snapshot().can_accept);
    // Vertex target plus an embedded edge must receive the identical fence.
    auto slide_plan = plan;
    slide_plan.steps.front().target.component = new_edges.at(rebound).first;
    slide_plan.steps.front().intent = "slide vertex 0.25 along edge " + std::to_string(rebound.value);
    auto unsafe_slide = agent::AgentTask::begin(original, slide_plan, envelope_for({object}));
    REQUIRE(unsafe_slide);
    const auto slide_path = directory.path() / "slide.carto-agent-task";
    const auto refused_slide = unsafe_slide.value().save_checkpoint(slide_path);
    REQUIRE(!refused_slide);
    REQUIRE(refused_slide.error().message.find("endpoint identity") != std::string::npos);
    REQUIRE(!std::filesystem::exists(slide_path));
    auto safe_slide = agent::AgentTask::begin(canonical, slide_plan, envelope_for({object}));
    REQUIRE(safe_slide);
    REQUIRE(safe_slide.value().save_checkpoint(slide_path));
    const auto slide_checkpoint = agent::AgentTask::load_checkpoint(slide_path);
    REQUIRE(slide_checkpoint);
    REQUIRE(!agent::AgentTask::resume_checkpoint(original, slide_checkpoint.value(), slide_plan));
    REQUIRE(!agent::AgentTask::analyze_checkpoint_rebase(original, slide_checkpoint.value()));
    REQUIRE(!agent::AgentTask::rebase_checkpoint(original, slide_checkpoint.value(), slide_plan));
    const auto resumed_slide = agent::AgentTask::resume_checkpoint(canonical, slide_checkpoint.value(), slide_plan);
    REQUIRE(resumed_slide && resumed_slide.value().snapshot().can_accept);
    slide_plan.steps.front().intent = "slide vertex 0.25";
    auto implicit_slide = agent::AgentTask::begin(canonical, slide_plan, envelope_for({object}));
    REQUIRE(implicit_slide);
    REQUIRE(!implicit_slide.value().save_checkpoint(directory.path() / "implicit.carto-agent-task"));
    REQUIRE(original.snapshot().project_revision == old_snapshot.project_revision);
    REQUIRE(canonical.snapshot().project_revision == new_snapshot.project_revision);
    require_same_mesh(original.snapshot(), old_snapshot, object);
    require_same_mesh(canonical.snapshot(), new_snapshot, object);
}

void preserve_topology_objective_refuses_bevel() {
    application::ApplicationSession source;
    ui::UiController ui(source);
    const auto target = create_box(ui);
    auto plan = plan_for(source, target, {target});
    plan.objective.hard_constraints.push_back(agent::HardConstraint{
        "preserve-shape-topology", agent::ObjectiveConstraintKind::preserve_topology,
        target, 0.0, "The existing topology must remain unchanged."});
    require_blocked(source, std::move(plan), envelope_for({target}),
        "forbids topology changes for this target");
}

void edge_bevel_acceptance_rolls_back_on_journal_failure() {
    TempDirectory temp;
    const auto path = temp.path() / "agent-rollback.carto";
    carto::application::ApplicationSession source;
    ui::UiController ui(source);
    const auto target = create_box(ui);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        source, carto::application::SaveProjectAction{path}));
    const auto baseline_recovery =
        carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(baseline_recovery);
    REQUIRE(baseline_recovery.value().state ==
        carto::application::RecoveryInspectionState::clean);

    const auto read_bytes = [](const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary);
        REQUIRE(input.good());
        return std::string(std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };
    const std::string project_bytes = read_bytes(path);
    const std::filesystem::path journal_path = baseline_recovery.value().journal_path;
    REQUIRE(journal_path.parent_path() == temp.path());
    const std::string journal_bytes = read_bytes(journal_path);
    const auto before = source.snapshot();
    auto plan = plan_for(source, target, {target});
    auto task = agent::AgentTask::begin(source, std::move(plan), envelope_for({target}));
    REQUIRE(task);
    REQUIRE(task.value().run());
    REQUIRE(task.value().snapshot().status == carto::agent::TaskStatus::review);
    REQUIRE(source.snapshot().project_revision == before.project_revision);

    std::filesystem::path preserved_journal = journal_path;
    preserved_journal += ".preserved";
    std::error_code error;
    std::filesystem::rename(journal_path, preserved_journal, error);
    REQUIRE(!error);
    REQUIRE(std::filesystem::create_directory(journal_path, error));
    REQUIRE(!error);

    const auto acceptance =
        carto::application::HumanApplicationAccess::accept_agent_task(task.value());
    REQUIRE(!acceptance);
    REQUIRE(task.value().snapshot().status == carto::agent::TaskStatus::blocked);
    const auto after_failure = source.snapshot();
    REQUIRE(after_failure.project_revision == before.project_revision);
    REQUIRE(after_failure.project_generation == before.project_generation);
    REQUIRE(after_failure.dirty == before.dirty);
    REQUIRE(after_failure.undo_count == before.undo_count);
    REQUIRE(after_failure.redo_count == before.redo_count);
    require_same_mesh(after_failure, before, target);
    REQUIRE(read_bytes(path) == project_bytes);

    std::filesystem::remove(journal_path, error);
    REQUIRE(!error);
    std::filesystem::rename(preserved_journal, journal_path, error);
    REQUIRE(!error);
    REQUIRE(read_bytes(journal_path) == journal_bytes);
    const auto restored_recovery =
        carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(restored_recovery);
    REQUIRE(restored_recovery.value().state ==
        carto::application::RecoveryInspectionState::clean);
    REQUIRE(restored_recovery.value().journal_entry_count ==
        baseline_recovery.value().journal_entry_count);
}

void locked_shared_instance_is_rejected_before_native_mutation() {
    TempDirectory directory;
    journal::Journal journal(directory.path() / "fixture.journal");
    auto document = project::ProjectDocument::create("Locked bevel fixture");
    REQUIRE(document);
    const auto initial = document.value().serialize();
    REQUIRE(journal.append(journal::JournalAppend{core::Revision{}, document.value().revision(),
        "project.create", std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(initial.data()), initial.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(document.value(), journal, "bevel-fixture");
    REQUIRE(transaction);
    auto mesh = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(mesh);
    const auto asset = transaction.value().add_mesh(std::move(mesh.value()));
    REQUIRE(asset);
    scene::SceneObject first;
    first.id = scene::ObjectId{1U}; first.name = "Bevel target"; first.mesh_asset = asset.value();
    REQUIRE(transaction.value().insert_object(first));
    auto second = first;
    second.id = scene::ObjectId{2U}; second.name = "Locked sibling"; second.locked = true;
    second.local_transform.translation.x = 2.0;
    REQUIRE(transaction.value().insert_object(second));
    REQUIRE(transaction.value().commit("bevel-lock.fixture"));
    const auto path = directory.path() / "locked.carto";
    REQUIRE(document.value().save_atomic(path));
    application::ApplicationSession source;
    REQUIRE(application::HumanApplicationAccess::dispatch(source, application::OpenProjectAction{path}));
    require_blocked(source, plan_for(source, first.id, {first.id, second.id}),
        envelope_for({first.id, second.id}), "locked shared-mesh instance");
}
} // namespace

int main() {
    try {
        native_and_agent_bevel_match_with_reviewed_history_and_persistence();
        bevel_checkpoint_roundtrips_and_replays_without_implicit_acceptance();
        shared_scope_protection_and_budget_guards_preserve_source();
        effects_and_topology_budget_preserve_source();
        locked_shared_instance_is_rejected_before_native_mutation();
        prepared_edge_checkpoint_refuses_rebound_identity();
        preserve_topology_objective_refuses_bevel();
        edge_bevel_acceptance_rolls_back_on_journal_failure();
        std::cout << "edge bevel tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
