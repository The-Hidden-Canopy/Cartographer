#pragma once

#include <carto/core/diagnostic.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/editor/selection.hpp>
#include <carto/editor/tools.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/render/render_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace carto::ui {
class UiController;
}

namespace carto::sdk {
class ApplicationAccess;
}

namespace carto::cli {
class ApplicationAccess;
}

namespace carto::application {

class ApplicationSession;

// Authoring actions require an admission object from a named front-end
// boundary. This prevents future proposal/AI code from treating the generic
// ApplicationAction variant as an implicit mutation authority.
class HumanActionAdmission final {
private:
    HumanActionAdmission() = default;

    friend class ApplicationSession;
    friend class ::carto::ui::UiController;
    friend class ::carto::sdk::ApplicationAccess;
    friend class ::carto::cli::ApplicationAccess;
};

enum class Pane {
    outliner,
    viewport,
    inspector,
    problems,
    history,
};

struct WorkspaceState {
    std::vector<Pane> visible_panes{
        Pane::outliner,
        Pane::viewport,
        Pane::inspector,
        Pane::problems,
        Pane::history,
    };

    [[nodiscard]] core::Result<void> validate() const;
};

struct ObjectView {
    scene::SceneObject object;
    core::Transform world_transform = core::Transform::identity();
};

struct SelectionView {
    editor::SelectionMode mode = editor::SelectionMode::object;
    std::vector<scene::ObjectId> objects;
    std::vector<geometry::VertexId> vertices;
    std::vector<geometry::EdgeId> edges;
    std::vector<geometry::FaceId> faces;
    std::optional<scene::ObjectId> component_object;
    std::optional<core::Revision> component_mesh_revision;
};

struct ViewportState {
    render::RenderScene scene;
    core::Revision project_revision;
    std::optional<core::Diagnostic> error;
};

struct ApplicationSnapshot {
    std::string project_name;
    std::optional<std::filesystem::path> project_path;
    std::uint64_t project_generation = 0;
    core::Revision project_revision;
    bool dirty = false;
    std::vector<ObjectView> objects;
    SelectionView selection;
    std::vector<editor::ToolDescriptor> tools;
    std::size_t undo_count = 0;
    std::size_t redo_count = 0;
    std::vector<core::Diagnostic> problems;
    WorkspaceState workspace;
    ViewportState viewport;
};

struct DispatchReceipt {
    std::string action;
    core::Revision revision_before;
    core::Revision revision_after;
    bool document_changed = false;
};

struct NewProjectAction {
    std::string name;
    bool discard_dirty = false;
};

struct OpenProjectAction {
    std::filesystem::path path;
    bool discard_dirty = false;
};

struct SaveProjectAction {
    std::optional<std::filesystem::path> path;
};

struct SetWorkspaceAction {
    WorkspaceState workspace;
};

struct SetSelectionModeAction {
    editor::SelectionMode mode;
};

struct SelectObjectAction {
    scene::ObjectId object;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SelectVertexAction {
    scene::ObjectId object;
    geometry::VertexId vertex;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SelectFaceAction {
    scene::ObjectId object;
    geometry::FaceId face;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SelectEdgeAction {
    scene::ObjectId object;
    geometry::EdgeId edge;
    editor::SelectionOperation operation = editor::SelectionOperation::replace;
};

struct SetObjectTransformAction {
    scene::ObjectId object;
    core::Transform transform;
};

struct InvokeToolAction {
    std::string tool_id;
    editor::ToolArguments arguments;
};

struct CreateBoxAction {
    std::string object_name;
    core::Vec3d size{1.0, 1.0, 1.0};
};

struct CreatePlaneAction {
    std::string object_name;
    double width = 1.0;
    double depth = 1.0;
};

struct CreateMeshObjectAction {
    std::string object_name;
    geometry::EditableMesh mesh;
};

struct UndoAction {};
struct RedoAction {};

using ApplicationAction = std::variant<
    NewProjectAction,
    OpenProjectAction,
    SaveProjectAction,
    SetWorkspaceAction,
    SetSelectionModeAction,
    SelectObjectAction,
    SelectVertexAction,
    SelectFaceAction,
    SelectEdgeAction,
    SetObjectTransformAction,
    InvokeToolAction,
    CreateBoxAction,
    CreatePlaneAction,
    CreateMeshObjectAction,
    UndoAction,
    RedoAction>;

class ApplicationSession {
public:
    ApplicationSession();

    [[nodiscard]] core::Result<DispatchReceipt> dispatch(
        HumanActionAdmission admission,
        const ApplicationAction& action);
    [[nodiscard]] ApplicationSnapshot snapshot() const;

    [[nodiscard]] const WorkspaceState& workspace() const noexcept { return workspace_; }
    [[nodiscard]] core::Result<void> can_close(bool discard_dirty = false) const;

private:
    [[nodiscard]] core::Result<void> set_workspace(WorkspaceState workspace);
    [[nodiscard]] core::Result<DispatchReceipt> new_project(const NewProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> open_project(const OpenProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> save_project(const SaveProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> execute_command(
        std::unique_ptr<editor::EditorCommand> command,
        std::string action);
    [[nodiscard]] core::Result<DispatchReceipt> accept_command_mutation(
        std::string action,
        core::Revision revision_before,
        project::ProjectDocument before_document);
    [[nodiscard]] core::Result<DispatchReceipt> select_object(const SelectObjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_vertex(const SelectVertexAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_face(const SelectFaceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_edge(const SelectEdgeAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> invoke_tool(const InvokeToolAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_box(const CreateBoxAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_plane(const CreatePlaneAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_mesh_object(
        const CreateMeshObjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> undo();
    [[nodiscard]] core::Result<DispatchReceipt> redo();
    [[nodiscard]] core::Result<void> append_mutation_event(
        std::string_view action,
        core::Revision revision_before,
        core::Revision revision_after);
    struct PreparedJournal {
        journal::Journal journal;
        bool created_file = false;
        bool appended_baseline = false;
        std::uintmax_t original_bytes = 0U;
    };
    [[nodiscard]] core::Result<PreparedJournal> prepare_journal(
        const std::filesystem::path& project_path,
        const project::ProjectDocument& document) const;
    [[nodiscard]] core::Result<void> require_discard_confirmation(bool discard_dirty) const;

    [[nodiscard]] core::Result<DispatchReceipt> accepted(
        std::string action,
        core::Revision revision_before) const;
    [[nodiscard]] core::Result<DispatchReceipt> failure(core::Diagnostic diagnostic);
    void reset_editor_state() noexcept;

    project::ProjectDocument document_;
    std::optional<std::filesystem::path> project_path_;
    core::Revision saved_revision_;
    std::uint64_t project_generation_ = 0;
    editor::SelectionState selection_;
    editor::CommandBus history_;
    editor::ToolRegistry tools_;
    std::optional<journal::Journal> journal_;
    WorkspaceState workspace_;
    std::vector<core::Diagnostic> problems_;
};

} // namespace carto::application
