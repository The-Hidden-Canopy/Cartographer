#include <carto/application/application.hpp>

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
    REQUIRE(session.dispatch(carto::application::NewProjectAction{"Authoring"}));
    REQUIRE(session.dispatch(carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}}));

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
    REQUIRE(session.dispatch(carto::application::SetObjectTransformAction{object, moved}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 1.0);
    REQUIRE(session.dispatch(carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 0.0);
    REQUIRE(session.dispatch(carto::application::RedoAction{}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 1.0);

    REQUIRE(session.dispatch(carto::application::SelectObjectAction{object}));
    REQUIRE(session.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(session.dispatch(carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1}}));

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    REQUIRE(session.dispatch(carto::application::InvokeToolAction{
        "mesh.extrude-face", arguments}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.undo_count == 3U);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.viewport.error == std::nullopt);

    REQUIRE(session.dispatch(carto::application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    REQUIRE(session.dispatch(carto::application::RedoAction{}));
    REQUIRE(session.snapshot().redo_count == 0U);
}

void application_routes_vertex_edit_and_rejects_stale_reselection() {
    carto::application::ApplicationSession session;
    REQUIRE(session.dispatch(carto::application::CreateBoxAction{
        "Box", {2.0, 2.0, 2.0}}));
    auto snapshot = session.snapshot();
    const auto object = snapshot.objects.front().object.id;
    REQUIRE(session.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(session.dispatch(carto::application::SelectVertexAction{
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
    REQUIRE(session.dispatch(carto::application::InvokeToolAction{
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

    REQUIRE(!session.dispatch(carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(session.snapshot().undo_count == 2U);
    REQUIRE(!session.snapshot().problems.empty());
    REQUIRE(session.dispatch(carto::application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    REQUIRE(session.dispatch(carto::application::RedoAction{}));
    REQUIRE(session.snapshot().redo_count == 0U);
}

void application_rejects_bad_selection_without_mutating_history() {
    carto::application::ApplicationSession session;
    REQUIRE(session.dispatch(carto::application::CreateBoxAction{
        "Box", {1.0, 1.0, 1.0}}));
    const auto before = session.snapshot();
    REQUIRE(!session.dispatch(carto::application::SelectFaceAction{
        carto::scene::ObjectId{999}, carto::geometry::FaceId{1}}));
    const auto after = session.snapshot();
    REQUIRE(after.undo_count == before.undo_count);
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(!after.problems.empty());

    REQUIRE(!session.dispatch(carto::application::InvokeToolAction{
        "missing.tool", {}}));
    REQUIRE(session.snapshot().undo_count == before.undo_count);

    const auto before_mode = session.snapshot().selection.mode;
    REQUIRE(!session.dispatch(carto::application::SetSelectionModeAction{
        static_cast<carto::editor::SelectionMode>(99)}));
    REQUIRE(session.snapshot().selection.mode == before_mode);
    REQUIRE(!session.dispatch(carto::application::SelectObjectAction{
        carto::scene::ObjectId{1},
        static_cast<carto::editor::SelectionOperation>(99)}));
    REQUIRE(session.snapshot().selection.objects.empty());
}

void application_requires_explicit_discard_for_project_replacement() {
    TempDirectory temp;
    carto::application::ApplicationSession source;
    REQUIRE(source.dispatch(carto::application::NewProjectAction{"Saved source"}));
    REQUIRE(source.dispatch(carto::application::CreatePlaneAction{
        "Saved source", 2.0, 2.0}));
    const auto source_path = temp.path() / "source.carto";
    REQUIRE(source.dispatch(carto::application::SaveProjectAction{source_path}));

    carto::application::ApplicationSession session;
    REQUIRE(session.dispatch(carto::application::CreateBoxAction{
        "Unsaved", {1.0, 1.0, 1.0}}));
    const auto before = session.snapshot();
    REQUIRE(before.dirty);
    REQUIRE(!session.can_close());
    REQUIRE(!session.dispatch(carto::application::NewProjectAction{"Replacement"}));
    REQUIRE(session.snapshot().project_name == before.project_name);
    REQUIRE(session.snapshot().objects.size() == before.objects.size());

    const auto generation = before.project_generation;
    REQUIRE(session.dispatch(carto::application::NewProjectAction{"Replacement", true}));
    const auto after = session.snapshot();
    REQUIRE(after.project_name == "Replacement");
    REQUIRE(after.objects.empty());
    REQUIRE(!after.dirty);
    REQUIRE(after.project_generation != generation);
    REQUIRE(session.can_close());

    REQUIRE(session.dispatch(carto::application::CreateBoxAction{
        "Dirty before open", {1.0, 1.0, 1.0}}));
    const auto before_open = session.snapshot();
    REQUIRE(!session.dispatch(carto::application::OpenProjectAction{source_path}));
    REQUIRE(session.snapshot().project_name == before_open.project_name);
    REQUIRE(session.snapshot().objects.size() == before_open.objects.size());
    REQUIRE(session.dispatch(carto::application::OpenProjectAction{source_path, true}));
    REQUIRE(session.snapshot().project_name == "Saved source");
    REQUIRE(!session.snapshot().dirty);
}

void application_bounds_failure_diagnostics() {
    carto::application::ApplicationSession session;
    for (std::size_t index = 0U; index < 256U; ++index) {
        REQUIRE(!session.dispatch(carto::application::InvokeToolAction{
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
    REQUIRE(!session.dispatch(carto::application::SetWorkspaceAction{duplicate}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState empty;
    empty.visible_panes.clear();
    REQUIRE(!session.dispatch(carto::application::SetWorkspaceAction{empty}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState unknown;
    unknown.visible_panes = {
        carto::application::Pane::viewport,
        static_cast<carto::application::Pane>(99),
    };
    REQUIRE(!session.dispatch(carto::application::SetWorkspaceAction{unknown}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState no_viewport;
    no_viewport.visible_panes = {carto::application::Pane::outliner};
    REQUIRE(!session.dispatch(carto::application::SetWorkspaceAction{no_viewport}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState compact;
    compact.visible_panes = {
        carto::application::Pane::viewport,
        carto::application::Pane::inspector,
    };
    REQUIRE(session.dispatch(carto::application::SetWorkspaceAction{compact}));
    REQUIRE(session.snapshot().workspace.visible_panes == compact.visible_panes);
    REQUIRE(session.snapshot().problems.size() == 4U);
}

void application_open_failure_preserves_current_project_and_save_is_atomic() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    REQUIRE(session.dispatch(carto::application::CreatePlaneAction{
        "Plane", 2.0, 3.0}));
    const auto current = session.snapshot();
    REQUIRE(!session.dispatch(carto::application::OpenProjectAction{
        temp.path() / "missing.carto"}));
    REQUIRE(session.snapshot().objects.size() == current.objects.size());
    REQUIRE(session.snapshot().project_revision == current.project_revision);

    const auto path = temp.path() / "saved.carto";
    REQUIRE(session.dispatch(carto::application::SaveProjectAction{path}));
    REQUIRE(!session.snapshot().dirty);
    REQUIRE(std::filesystem::is_regular_file(path));

    carto::application::ApplicationSession reopened;
    REQUIRE(reopened.dispatch(carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 1U);
    REQUIRE(!reopened.snapshot().dirty);
}

} // namespace

int main() {
    try {
        application_routes_authoring_through_snapshot_and_history();
        application_routes_vertex_edit_and_rejects_stale_reselection();
        application_rejects_bad_selection_without_mutating_history();
        application_requires_explicit_discard_for_project_replacement();
        application_bounds_failure_diagnostics();
        application_validates_workspace_layouts();
        application_open_failure_preserves_current_project_and_save_is_atomic();
    } catch (const std::exception& error) {
        return (void(std::cerr << "FAIL " << error.what() << '\n'), 1);
    }
    return 0;
}
