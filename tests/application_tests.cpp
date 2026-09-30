#include "application_access.hpp"

#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("cartographer-application-tests-" + std::to_string(stamp));
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

void application_routes_authoring_through_snapshot_and_history() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Authoring"}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}}));

    auto snapshot = session.snapshot();
    REQUIRE(snapshot.project_name == "Authoring");
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.viewport.error == std::nullopt);
    REQUIRE(snapshot.viewport.scene.size() == 1U);
    const auto instances = snapshot.viewport.scene.instances();
    REQUIRE(instances.size() == 1U);
    REQUIRE(instances.front().mesh->vertex_ids.size() == 8U);
    REQUIRE(instances.front().mesh->triangle_faces.size() == 12U);
    REQUIRE(instances.front().mesh->vertex_ids.front() == carto::geometry::VertexId{1});
    REQUIRE(instances.front().mesh->triangle_faces.front() == carto::geometry::FaceId{1});

    const auto object = snapshot.objects.front().object.id;
    carto::core::Transform moved = snapshot.objects.front().object.local_transform;
    moved.translation = {1.0, 2.0, 3.0};
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetObjectTransformAction{object, moved}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 1.0);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 0.0);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 1.0);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectObjectAction{object}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1}}));

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.extrude-face", arguments}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.undo_count == 3U);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.viewport.error == std::nullopt);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().redo_count == 0U);
}

void application_routes_imported_mesh_through_the_durable_command_boundary() {
    TempDirectory temp;
    auto mesh = carto::geometry::make_plane(2.0, 3.0);
    REQUIRE(mesh);

    const auto path = temp.path() / "imported.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Imported"}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateMeshObjectAction{
        "Imported mesh", std::move(mesh.value())}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{std::nullopt}));

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.viewport.error == std::nullopt);

    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 2U);
    REQUIRE(entries.value().back().event_type == "cartographer.application_action");
    const std::string payload(
        entries.value().back().payload.begin(), entries.value().back().payload.end());
    REQUIRE(payload.find("operation=Create Mesh Object") != std::string::npos);
    REQUIRE(journal.verify());
}

void application_routes_vertex_edit_and_rejects_stale_reselection() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Box", {2.0, 2.0, 2.0}}));
    auto snapshot = session.snapshot();
    const auto object = snapshot.objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1}}));

    const auto before_instances = session.snapshot().viewport.scene.instances();
    REQUIRE(before_instances.size() == 1U);
    const auto before_mesh = before_instances.front().mesh;
    const auto before_vertex = std::find(
        before_mesh->vertex_ids.begin(), before_mesh->vertex_ids.end(),
        carto::geometry::VertexId{1});
    REQUIRE(before_vertex != before_mesh->vertex_ids.end());
    const auto before_index = static_cast<std::size_t>(
        before_vertex - before_mesh->vertex_ids.begin());
    const auto target = before_mesh->positions[before_index] + carto::core::Vec3d{0.1, 0.0, 0.0};

    carto::editor::ToolArguments arguments;
    arguments.position = target;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.undo_count == 2U);
    const auto after_instances = snapshot.viewport.scene.instances();
    const auto after_vertex = std::find(
        after_instances.front().mesh->vertex_ids.begin(),
        after_instances.front().mesh->vertex_ids.end(),
        carto::geometry::VertexId{1});
    REQUIRE(after_vertex != after_instances.front().mesh->vertex_ids.end());
    const auto after_index = static_cast<std::size_t>(
        after_vertex - after_instances.front().mesh->vertex_ids.begin());
    REQUIRE(after_instances.front().mesh->positions[after_index].x == target.x);

    REQUIRE(!carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(session.snapshot().undo_count == 2U);
    REQUIRE(!session.snapshot().problems.empty());
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().redo_count == 0U);
}

void application_routes_persistent_edge_selection_without_mutation() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Edge selection", {2.0, 2.0, 2.0}}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    const auto object = snapshot.objects.front().object.id;
    const auto instances = snapshot.viewport.scene.instances();
    REQUIRE(instances.size() == 1U);
    REQUIRE(instances.front().mesh->triangle_edges.size() ==
            instances.front().mesh->triangle_faces.size());
    REQUIRE(instances.front().mesh->triangle_edges.front().front().has_value());
    const auto edge = *instances.front().mesh->triangle_edges.front().front();
    const auto revision = snapshot.project_revision;

    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectEdgeAction{object, edge}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == revision);
    REQUIRE(snapshot.selection.mode == carto::editor::SelectionMode::edge);
    REQUIRE(snapshot.selection.edges.size() == 1U);
    REQUIRE(snapshot.selection.edges.front() == edge);

    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SelectEdgeAction{
        object, carto::geometry::EdgeId{999}}));
    const auto after_failure = session.snapshot();
    REQUIRE(after_failure.project_revision == revision);
    REQUIRE(after_failure.selection.edges.size() == 1U);
    REQUIRE(after_failure.selection.edges.front() == edge);
}

void application_rejects_bad_selection_without_mutating_history() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Box", {1.0, 1.0, 1.0}}));
    const auto before = session.snapshot();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SelectFaceAction{
        carto::scene::ObjectId{999}, carto::geometry::FaceId{1}}));
    const auto after = session.snapshot();
    REQUIRE(after.undo_count == before.undo_count);
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(!after.problems.empty());

    REQUIRE(!carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "missing.tool", {}}));
    REQUIRE(session.snapshot().undo_count == before.undo_count);

    const auto before_mode = session.snapshot().selection.mode;
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        static_cast<carto::editor::SelectionMode>(99)}));
    REQUIRE(session.snapshot().selection.mode == before_mode);
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SelectObjectAction{
        carto::scene::ObjectId{1},
        static_cast<carto::editor::SelectionOperation>(99)}));
    REQUIRE(session.snapshot().selection.objects.empty());
}

void application_requires_explicit_discard_for_project_replacement() {
    TempDirectory temp;
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(source, carto::application::NewProjectAction{"Saved source"}));
    REQUIRE(carto::application::testing::dispatch(source, carto::application::CreatePlaneAction{
        "Saved source", 2.0, 2.0}));
    const auto source_path = temp.path() / "source.carto";
    REQUIRE(carto::application::testing::dispatch(source, carto::application::SaveProjectAction{source_path}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Unsaved", {1.0, 1.0, 1.0}}));
    const auto before = session.snapshot();
    REQUIRE(before.dirty);
    REQUIRE(!session.can_close());
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Replacement"}));
    REQUIRE(session.snapshot().project_name == before.project_name);
    REQUIRE(session.snapshot().objects.size() == before.objects.size());

    const auto generation = before.project_generation;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Replacement", true}));
    const auto after = session.snapshot();
    REQUIRE(after.project_name == "Replacement");
    REQUIRE(after.objects.empty());
    REQUIRE(!after.dirty);
    REQUIRE(after.project_generation != generation);
    REQUIRE(session.can_close());

    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Dirty before open", {1.0, 1.0, 1.0}}));
    const auto before_open = session.snapshot();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::OpenProjectAction{source_path}));
    REQUIRE(session.snapshot().project_name == before_open.project_name);
    REQUIRE(session.snapshot().objects.size() == before_open.objects.size());
    REQUIRE(carto::application::testing::dispatch(session, carto::application::OpenProjectAction{source_path, true}));
    REQUIRE(session.snapshot().project_name == "Saved source");
    REQUIRE(!session.snapshot().dirty);
}

void application_bounds_failure_diagnostics() {
    carto::application::ApplicationSession session;
    for (std::size_t index = 0U; index < 256U; ++index) {
        REQUIRE(!carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
            "missing.tool", {}}));
    }
    const auto snapshot = session.snapshot();
    REQUIRE(!snapshot.problems.empty());
    REQUIRE(snapshot.problems.size() <= 128U);
}

void application_validates_workspace_layouts() {
    carto::application::ApplicationSession session;
    const auto before = session.snapshot().workspace.visible_panes;

    carto::application::WorkspaceState duplicate;
    duplicate.visible_panes = {
        carto::application::Pane::viewport,
        carto::application::Pane::viewport,
    };
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{duplicate}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState empty;
    empty.visible_panes.clear();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{empty}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState unknown;
    unknown.visible_panes = {
        carto::application::Pane::viewport,
        static_cast<carto::application::Pane>(99),
    };
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{unknown}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState no_viewport;
    no_viewport.visible_panes = {carto::application::Pane::outliner};
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{no_viewport}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState compact;
    compact.visible_panes = {
        carto::application::Pane::viewport,
        carto::application::Pane::inspector,
    };
    const auto compact_receipt = carto::application::testing::dispatch(session,
        carto::application::SetWorkspaceAction{compact});
    REQUIRE(compact_receipt);
    REQUIRE(compact_receipt.value().action == "Set Workspace");
    REQUIRE(!compact_receipt.value().document_changed);
    REQUIRE(session.snapshot().workspace.visible_panes == compact.visible_panes);
    REQUIRE(session.snapshot().problems.size() == 4U);
}

void application_open_failure_preserves_current_project_and_save_is_atomic() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreatePlaneAction{
        "Plane", 2.0, 3.0}));
    const auto current = session.snapshot();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::OpenProjectAction{
        temp.path() / "missing.carto"}));
    REQUIRE(session.snapshot().objects.size() == current.objects.size());
    REQUIRE(session.snapshot().project_revision == current.project_revision);

    const auto path = temp.path() / "saved.carto";
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));
    REQUIRE(!session.snapshot().dirty);
    REQUIRE(std::filesystem::is_regular_file(path));

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 1U);
    REQUIRE(!reopened.snapshot().dirty);
}

void application_journals_committed_mutations_and_rolls_back_failed_append() {
    TempDirectory temp;
    const auto path = temp.path() / "journaled.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Journaled"}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));

    carto::journal::Journal journal(journal_path);
    auto baseline = journal.read_all();
    REQUIRE(baseline);
    REQUIRE(baseline.value().size() == 1U);
    REQUIRE(baseline.value().front().event_type == "cartographer.snapshot");

    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Box", {1.0, 1.0, 1.0}}));
    auto after_create = session.snapshot();
    REQUIRE(after_create.objects.size() == 1U);
    REQUIRE(after_create.undo_count == 1U);

    auto after_event = journal.read_all();
    REQUIRE(after_event);
    REQUIRE(after_event.value().size() == 2U);
    REQUIRE(after_event.value().back().event_type == "cartographer.application_action");
    REQUIRE(after_event.value().back().revision_after == after_create.project_revision);
    const std::string mutation_payload(
        after_event.value().back().payload.begin(), after_event.value().back().payload.end());
    REQUIRE(mutation_payload.find("CARTOGRAPHER_APPLICATION_MUTATION_V1") == 0U);
    REQUIRE(mutation_payload.find("CARTOGRAPHER_PROJECT 1") != std::string::npos);
    REQUIRE(journal.verify());

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    auto after_undo = journal.read_all();
    REQUIRE(after_undo);
    REQUIRE(after_undo.value().size() == 3U);
    REQUIRE(after_undo.value().back().event_type == "cartographer.application_action");

    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    auto after_redo = journal.read_all();
    REQUIRE(after_redo);
    REQUIRE(after_redo.value().size() == 4U);

    const auto before_failed_append = session.snapshot();
    REQUIRE(std::filesystem::remove(journal_path));
    REQUIRE(std::filesystem::create_directory(journal_path));
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::CreatePlaneAction{
        "Rejected", 1.0, 1.0}));
    const auto after_failed_append = session.snapshot();
    REQUIRE(after_failed_append.project_revision == before_failed_append.project_revision);
    REQUIRE(after_failed_append.objects.size() == before_failed_append.objects.size());
    REQUIRE(after_failed_append.undo_count == before_failed_append.undo_count);
    REQUIRE(std::filesystem::remove_all(journal_path) == 1U);
}

void application_rejects_journal_revision_drift_before_open() {
    TempDirectory temp;
    const auto path = temp.path() / "drifted.carto";
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(source, carto::application::NewProjectAction{"Drifted"}));
    REQUIRE(carto::application::testing::dispatch(source, carto::application::SaveProjectAction{path}));

    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    carto::journal::Journal journal(journal_path);
    const auto current = journal.current_revision();
    REQUIRE(current);
    const std::array<std::uint8_t, 1> payload{0x01U};
    REQUIRE(journal.append(carto::journal::JournalAppend{
        current.value(), current.value().next(), "test.drift", payload, 1U}));

    carto::application::ApplicationSession target;
    REQUIRE(!carto::application::testing::dispatch(target, carto::application::OpenProjectAction{path}));
    REQUIRE(target.snapshot().project_name != "Drifted");
    REQUIRE(!target.snapshot().problems.empty());
}

void application_rolls_back_preexisting_journal_baseline_on_save_failure() {
    TempDirectory temp;
    const auto path = temp.path() / "blocked.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    const std::string header = "CARTOGRAPHER_JOURNAL_V1\n";
    {
        std::ofstream seed(journal_path, std::ios::binary | std::ios::trunc);
        seed << header;
    }
    REQUIRE(std::filesystem::create_directory(path));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Save rollback"}));
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));

    std::ifstream restored(journal_path, std::ios::binary);
    const std::string contents(
        std::istreambuf_iterator<char>{restored}, std::istreambuf_iterator<char>{});
    REQUIRE(contents == header);
    carto::journal::Journal journal(journal_path);
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().empty());
}

void application_rejects_semantically_wrong_journal_tail() {
    TempDirectory temp;
    const auto path = temp.path() / "semantic-drift.carto";
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(source, carto::application::NewProjectAction{"Semantic drift"}));
    REQUIRE(carto::application::testing::dispatch(source, carto::application::SaveProjectAction{path}));

    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    carto::journal::Journal journal(journal_path);
    const auto baseline = journal.read_all();
    REQUIRE(baseline);
    REQUIRE(baseline.value().size() == 1U);
    const std::string baseline_payload(
        baseline.value().front().payload.begin(), baseline.value().front().payload.end());
    const std::size_t marker = baseline_payload.find("document_bytes=");
    REQUIRE(marker != std::string::npos);
    const std::size_t document_start = baseline_payload.find('\n', marker);
    REQUIRE(document_start != std::string::npos);
    std::string revision_two = baseline_payload.substr(document_start + 1U);
    const std::string revision_marker = "REVISION 1\n";
    const std::size_t revision = revision_two.find(revision_marker);
    REQUIRE(revision != std::string::npos);
    revision_two.replace(revision, revision_marker.size(), "REVISION 2\n");

    const std::string fake_payload =
        "CARTOGRAPHER_PROJECT_SNAPSHOT_V1\n" +
        std::string("document_bytes=") + std::to_string(revision_two.size()) + "\n" +
        revision_two;
    REQUIRE(journal.append(carto::journal::JournalAppend{
        carto::core::Revision{1U}, carto::core::Revision{2U}, "test.fake_snapshot",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(fake_payload.data()), fake_payload.size()),
        2U}));
    std::ofstream replace(path, std::ios::binary | std::ios::trunc);
    replace << revision_two;
    replace.close();
    REQUIRE(replace);

    carto::application::ApplicationSession target;
    const auto opened = carto::application::testing::dispatch(target, carto::application::OpenProjectAction{path});
    REQUIRE(!opened);
    REQUIRE(opened.error().code == carto::core::ErrorCode::validation_failed);
}

} // namespace

int main() {
    try {
        application_routes_authoring_through_snapshot_and_history();
        application_routes_imported_mesh_through_the_durable_command_boundary();
        application_routes_vertex_edit_and_rejects_stale_reselection();
        application_routes_persistent_edge_selection_without_mutation();
        application_rejects_bad_selection_without_mutating_history();
        application_requires_explicit_discard_for_project_replacement();
        application_bounds_failure_diagnostics();
        application_validates_workspace_layouts();
        application_open_failure_preserves_current_project_and_save_is_atomic();
        application_journals_committed_mutations_and_rolls_back_failed_append();
        application_rejects_journal_revision_drift_before_open();
        application_rolls_back_preexisting_journal_baseline_on_save_failure();
        application_rejects_semantically_wrong_journal_tail();
    } catch (const std::exception& error) {
        return (void(std::cerr << "FAIL " << error.what() << '\n'), 1);
    }
    return 0;
}
