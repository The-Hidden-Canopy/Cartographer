#include "application_access.hpp"

#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

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

void application_persists_project_topology_receipts_through_save_and_reopen() {
    TempDirectory temp;
    const auto path = temp.path() / "receipt-ledger.carto";

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Receipt persistence"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{object, carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.2;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.extrude-face", arguments}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    std::ifstream input(path, std::ios::binary);
    const std::string serialized(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(serialized.find("TOPOLOGY_RECEIPTS 1") != std::string::npos);
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().mesh_asset == 1U);

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 1U);
}

void application_persists_vertex_merge_lineage_through_save_and_reopen() {
    TempDirectory temp;
    const auto path = temp.path() / "merge-receipt.carto";

    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    REQUIRE(mesh.add_face({target.value(), source.value(), third.value()}));
    REQUIRE(mesh.add_face({source.value(), fourth.value(), third.value()}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateMeshObjectAction{"Weld", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, target.value()}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{
            object, source.value(), carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.merge-vertices", carto::editor::ToolArguments{}}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.merged_vertices.at(
                source.value()) == target.value());

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 3U);
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() == 1U);
}

void application_routes_face_poke_through_history_and_persists_lineage() {
    TempDirectory temp;
    const auto path = temp.path() / "poke-receipt.carto";

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"Poke", 2.0, 2.0}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectFaceAction{object, carto::geometry::FaceId{1U}}));
    const auto poked = carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.poke-face", carto::editor::ToolArguments{}});
    REQUIRE(poked);
    REQUIRE(session.snapshot().selection.faces.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 5U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() == 4U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    const auto& receipt = loaded.value().topology_receipts().front().receipt;
    REQUIRE(receipt.created_vertices.size() == 1U);
    REQUIRE(receipt.created_faces.size() == 4U);
    REQUIRE(receipt.removed_faces.size() == 1U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 5U);
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

void application_repeats_last_tool_against_current_document() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Repeat", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));

    carto::editor::ToolArguments arguments;
    arguments.position = {-0.25, -0.5, -0.5};
    REQUIRE(carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RepeatLastToolAction{}));
    const auto snapshot = session.snapshot();
    REQUIRE(snapshot.undo_count == 2U);
    REQUIRE(snapshot.redo_count == 0U);
    REQUIRE(snapshot.viewport.scene.instances().front().mesh->positions.front().x == -0.25);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    carto::editor::ToolArguments adjusted;
    adjusted.position = {-0.4, -0.5, -0.5};
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::AdjustLastToolAction{adjusted}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->positions.front().x == -0.4);
}

void application_rejects_repeat_without_prior_tool() {
    carto::application::ApplicationSession session;
    const auto repeated = carto::application::testing::dispatch(
        session, carto::application::RepeatLastToolAction{});
    REQUIRE(!repeated);
    REQUIRE(repeated.error().code == carto::core::ErrorCode::invalid_state);
    const auto adjusted = carto::application::testing::dispatch(
        session, carto::application::AdjustLastToolAction{carto::editor::ToolArguments{}});
    REQUIRE(!adjusted);
    REQUIRE(adjusted.error().code == carto::core::ErrorCode::invalid_state);
}

void application_rejects_repeat_when_saved_component_was_removed() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreatePlaneAction{"Removed repeat", 1.0, 1.0}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{object, carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.remove_orphaned_vertices = false;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.remove-face", arguments}));
    const auto before_repeat = session.snapshot();
    const auto repeated = carto::application::testing::dispatch(session,
        carto::application::RepeatLastToolAction{});
    REQUIRE(!repeated);
    REQUIRE(repeated.error().code == carto::core::ErrorCode::not_found);
    const auto after_repeat = session.snapshot();
    REQUIRE(after_repeat.project_revision == before_repeat.project_revision);
    REQUIRE(after_repeat.undo_count == before_repeat.undo_count);
    REQUIRE(after_repeat.selection.faces.empty());
}

void application_routes_component_selection_conversion_without_mutation() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"Plane", 2.0, 2.0}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, {1U}}));
    for (std::uint64_t vertex = 2U; vertex <= 4U; ++vertex) {
        REQUIRE(carto::application::testing::dispatch(
            session, carto::application::SelectVertexAction{
                object, {vertex}, carto::editor::SelectionOperation::add}));
    }

    const auto before = session.snapshot().project_revision;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ConvertSelectionAction{
            carto::editor::SelectionMode::face}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.mode == carto::editor::SelectionMode::face);
    REQUIRE(snapshot.selection.faces.size() == 1U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ConvertSelectionAction{
            carto::editor::SelectionMode::vertex}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.mode == carto::editor::SelectionMode::vertex);
    REQUIRE(snapshot.selection.vertices.size() == 4U);
}

void application_routes_component_selection_expansion_without_mutation() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Expansion", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, carto::geometry::VertexId{1U}}));
    const auto before = session.snapshot().project_revision;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ExpandSelectionAction{
            carto::editor::SelectionExpansion::grow}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.vertices.size() == 4U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ExpandSelectionAction{
            carto::editor::SelectionExpansion::invert}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.vertices.size() == 4U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::object}));
    const auto rejected = carto::application::testing::dispatch(
        session, carto::application::ExpandSelectionAction{
            carto::editor::SelectionExpansion::linked});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before);
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
    REQUIRE(mutation_payload.find("CARTOGRAPHER_PROJECT 4") != std::string::npos);
    REQUIRE(mutation_payload.find(
                "AUTHORING \"cartographer.authoring\" \"meters\" \"right_handed_y_up\"") !=
            std::string::npos);
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

void application_inspects_recovery_without_mutation() {
    TempDirectory temp;
    const auto path = temp.path() / "inspect.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");

    const auto unavailable = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(unavailable);
    REQUIRE(unavailable.value().state ==
            carto::application::RecoveryInspectionState::unavailable);
    REQUIRE(unavailable.value().journal_entry_count == 0U);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Inspection"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto clean_before = session.snapshot();
    const auto clean = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(clean);
    REQUIRE(clean.value().state == carto::application::RecoveryInspectionState::clean);
    REQUIRE(clean.value().project_revision == clean_before.project_revision);
    REQUIRE(clean.value().durable_revision == clean_before.project_revision);
    REQUIRE(clean.value().journal_entry_count == 1U);
    REQUIRE(clean.value().journal_entries.front().event_type == "cartographer.snapshot");

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Pending", {1.0, 1.0, 1.0}}));
    const auto pending_before = session.snapshot();
    const auto pending = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(pending);
    REQUIRE(pending.value().state == carto::application::RecoveryInspectionState::pending);
    REQUIRE(pending.value().project_revision == clean_before.project_revision);
    REQUIRE(pending.value().durable_revision == pending_before.project_revision);
    REQUIRE(pending.value().journal_entry_count == 2U);
    REQUIRE(std::filesystem::is_regular_file(path));
    REQUIRE(std::filesystem::is_regular_file(journal_path));
    REQUIRE(session.snapshot().project_revision == pending_before.project_revision);

    REQUIRE(std::filesystem::remove(path));
    const auto missing_project = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(missing_project);
    REQUIRE(missing_project.value().state ==
            carto::application::RecoveryInspectionState::pending);
    REQUIRE(missing_project.value().diagnostic.has_value());
    REQUIRE(missing_project.value().diagnostic->code == carto::core::ErrorCode::not_found);
}

void application_recovery_inspection_serializes_with_project_writers() {
    TempDirectory temp;
    const auto path = temp.path() / "inspection-lock.carto";

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Inspection lock"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::thread worker;
    bool lock_acquired = false;
    bool started_seen = false;
    bool blocked_while_held = false;
    {
        carto::project::FileLock held(path);
        lock_acquired = static_cast<bool>(held.acquire());
        if (lock_acquired) {
            worker = std::thread([&] {
                started.store(true, std::memory_order_release);
                const auto result =
                    carto::application::ApplicationSession::inspect_recovery(path);
                succeeded.store(static_cast<bool>(result), std::memory_order_release);
                completed.store(true, std::memory_order_release);
            });
            for (unsigned attempt = 0U;
                 attempt < 1000U && !started.load(std::memory_order_acquire); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started_seen = started.load(std::memory_order_acquire);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            blocked_while_held = !completed.load(std::memory_order_acquire);
        }
    }
    if (worker.joinable()) worker.join();

    REQUIRE(lock_acquired);
    REQUIRE(started_seen);
    REQUIRE(blocked_while_held);
    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(succeeded.load(std::memory_order_acquire));
}

void application_blocks_malformed_recovery_evidence() {
    TempDirectory temp;
    const auto path = temp.path() / "malformed.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    {
        std::ofstream project(path, std::ios::binary | std::ios::trunc);
        project << "not a cartographer project\n";
        std::ofstream journal(journal_path, std::ios::binary | std::ios::trunc);
        journal << "CARTOGRAPHER_JOURNAL_V1\nmalformed\n";
    }

    const auto inspected = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(inspected);
    REQUIRE(inspected.value().state ==
            carto::application::RecoveryInspectionState::blocked);
    REQUIRE(inspected.value().diagnostic.has_value());
    REQUIRE(inspected.value().diagnostic->code == carto::core::ErrorCode::validation_failed);
    REQUIRE(inspected.value().journal_entry_count == 0U);

    auto forged = inspected.value();
    forged.state = carto::application::RecoveryInspectionState::clean;
    forged.diagnostic.reset();
    forged.durable_revision = carto::core::Revision{1U};
    REQUIRE(!forged.validate());
}

void application_requires_explicit_recovery_and_rebinds_saved_state() {
    TempDirectory temp;
    const auto path = temp.path() / "recover.carto";
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Recoverable"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::CreateBoxAction{"Recovered box", {1.0, 1.0, 1.0}}));

    carto::application::ApplicationSession target;
    const auto recovered = carto::application::testing::dispatch(
        target, carto::application::RecoverProjectAction{path});
    REQUIRE(recovered);
    REQUIRE(recovered.value().source == carto::application::OperationSource::recovery);
    REQUIRE(recovered.value().document_changed);
    REQUIRE(target.snapshot().objects.size() == 1U);
    REQUIRE(!target.snapshot().dirty);
    REQUIRE(target.snapshot().project_path == path);

    const auto clean = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(clean);
    REQUIRE(clean.value().state == carto::application::RecoveryInspectionState::clean);

    carto::application::ApplicationSession dirty;
    REQUIRE(carto::application::testing::dispatch(
        dirty, carto::application::CreatePlaneAction{"Unsaved", 1.0, 1.0}));
    const auto before = dirty.snapshot();
    const auto rejected = carto::application::testing::dispatch(
        dirty, carto::application::RecoverProjectAction{path});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(dirty.snapshot().project_revision == before.project_revision);
    REQUIRE(dirty.snapshot().objects.size() == before.objects.size());
}

void application_recovery_fails_closed_without_mutating_session_or_evidence() {
    TempDirectory temp;

    const auto unsupported_path = temp.path() / "unsupported-tail.carto";
    carto::application::ApplicationSession unsupported_source;
    REQUIRE(carto::application::testing::dispatch(
        unsupported_source, carto::application::NewProjectAction{"Unsupported tail"}));
    REQUIRE(carto::application::testing::dispatch(
        unsupported_source, carto::application::SaveProjectAction{unsupported_path}));
    const auto unsupported_journal_path =
        std::filesystem::path(unsupported_path.string() + ".journal");
    carto::journal::Journal unsupported_journal(unsupported_journal_path);
    const std::string unsupported_payload = "CARTOGRAPHER_UNKNOWN_RECOVERY_V1\n";
    REQUIRE(unsupported_journal.append(carto::journal::JournalAppend{
        carto::core::Revision{1U}, carto::core::Revision{2U}, "test.unsupported_recovery",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(unsupported_payload.data()),
            unsupported_payload.size()),
        2U}));
    const auto unsupported_entries_before = unsupported_journal.read_all();
    REQUIRE(unsupported_entries_before);
    REQUIRE(unsupported_entries_before.value().size() == 2U);

    carto::application::ApplicationSession unsupported_target;
    const auto unsupported_before = unsupported_target.snapshot();
    const auto unsupported_recovery = carto::application::testing::dispatch(
        unsupported_target, carto::application::RecoverProjectAction{unsupported_path});
    REQUIRE(!unsupported_recovery);
    REQUIRE(unsupported_target.snapshot().project_revision ==
            unsupported_before.project_revision);
    REQUIRE(unsupported_target.snapshot().objects.size() == unsupported_before.objects.size());
    REQUIRE(unsupported_target.snapshot().project_path == unsupported_before.project_path);
    const auto unsupported_entries_after = unsupported_journal.read_all();
    REQUIRE(unsupported_entries_after);
    REQUIRE(unsupported_entries_after.value().size() ==
            unsupported_entries_before.value().size());
    REQUIRE(unsupported_journal.verify());

    const auto save_failure_path = temp.path() / "atomic-save-failure.carto";
    carto::application::ApplicationSession save_failure_source;
    REQUIRE(carto::application::testing::dispatch(
        save_failure_source, carto::application::NewProjectAction{"Atomic recovery"}));
    REQUIRE(carto::application::testing::dispatch(
        save_failure_source, carto::application::SaveProjectAction{save_failure_path}));
    REQUIRE(carto::application::testing::dispatch(
        save_failure_source, carto::application::CreateBoxAction{"Recovered", {1.0, 1.0, 1.0}}));
    const auto save_failure_journal_path =
        std::filesystem::path(save_failure_path.string() + ".journal");
    carto::journal::Journal save_failure_journal(save_failure_journal_path);
    const auto save_failure_entries_before = save_failure_journal.read_all();
    REQUIRE(save_failure_entries_before);
    REQUIRE(save_failure_entries_before.value().size() == 2U);
    REQUIRE(std::filesystem::remove(save_failure_path));
    REQUIRE(std::filesystem::create_directory(save_failure_path));

    carto::application::ApplicationSession save_failure_target;
    const auto save_failure_before = save_failure_target.snapshot();
    const auto save_failure_recovery = carto::application::testing::dispatch(
        save_failure_target, carto::application::RecoverProjectAction{save_failure_path});
    REQUIRE(!save_failure_recovery);
    REQUIRE(save_failure_target.snapshot().project_revision ==
            save_failure_before.project_revision);
    REQUIRE(save_failure_target.snapshot().objects.size() == save_failure_before.objects.size());
    REQUIRE(!save_failure_target.snapshot().project_path.has_value());
    REQUIRE(std::filesystem::is_directory(save_failure_path));
    const auto save_failure_entries_after = save_failure_journal.read_all();
    REQUIRE(save_failure_entries_after);
    REQUIRE(save_failure_entries_after.value().size() ==
            save_failure_entries_before.value().size());
    REQUIRE(save_failure_journal.verify());
}

void application_mutation_serializes_with_project_recovery_writer_lease() {
    TempDirectory temp;
    const auto path = temp.path() / "mutation-lock.carto";

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Mutation lock"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));

    carto::application::ApplicationSession writer;
    REQUIRE(carto::application::testing::dispatch(
        writer, carto::application::OpenProjectAction{path}));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::thread worker;
    bool lock_acquired = false;
    bool started_seen = false;
    bool blocked_while_held = false;
    {
        carto::project::FileLock held(path);
        lock_acquired = static_cast<bool>(held.acquire());
        if (lock_acquired) {
            worker = std::thread([&] {
                started.store(true, std::memory_order_release);
                const auto result = carto::application::testing::dispatch(
                    writer, carto::application::CreateBoxAction{
                        "Serialized", {1.0, 1.0, 1.0}});
                succeeded.store(static_cast<bool>(result), std::memory_order_release);
                completed.store(true, std::memory_order_release);
            });
            for (unsigned attempt = 0U;
                 attempt < 1000U && !started.load(std::memory_order_acquire); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started_seen = started.load(std::memory_order_acquire);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            blocked_while_held = !completed.load(std::memory_order_acquire);
        }
    }
    if (worker.joinable()) worker.join();

    REQUIRE(lock_acquired);
    REQUIRE(started_seen);
    REQUIRE(blocked_while_held);
    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(succeeded.load(std::memory_order_acquire));
    REQUIRE(writer.snapshot().project_revision == carto::core::Revision{4U});
    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 2U);
    REQUIRE(journal.verify());
}

} // namespace

int main() {
    try {
        application_routes_authoring_through_snapshot_and_history();
        application_persists_project_topology_receipts_through_save_and_reopen();
        application_persists_vertex_merge_lineage_through_save_and_reopen();
        application_routes_face_poke_through_history_and_persists_lineage();
        application_routes_imported_mesh_through_the_durable_command_boundary();
        application_routes_vertex_edit_and_rejects_stale_reselection();
        application_repeats_last_tool_against_current_document();
        application_rejects_repeat_without_prior_tool();
        application_rejects_repeat_when_saved_component_was_removed();
        application_routes_component_selection_conversion_without_mutation();
        application_routes_component_selection_expansion_without_mutation();
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
        application_inspects_recovery_without_mutation();
        application_recovery_inspection_serializes_with_project_writers();
        application_blocks_malformed_recovery_evidence();
        application_requires_explicit_recovery_and_rebinds_saved_state();
        application_recovery_fails_closed_without_mutating_session_or_evidence();
        application_mutation_serializes_with_project_recovery_writer_lease();
    } catch (const std::exception& error) {
        return (void(std::cerr << "FAIL " << error.what() << '\n'), 1);
    }
    return 0;
}
