#pragma once

#include <carto/core/diagnostic.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/editor/authoring_context.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/editor/preview_transaction.hpp>
#include <carto/editor/selection.hpp>
#include <carto/editor/tools.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/render/render_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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

namespace carto::application {

class ApplicationSession;
class HumanApplicationAccess;

// Authoring actions require an admission object from a named front-end
// boundary. This prevents future proposal/AI code from treating the generic
// ApplicationAction variant as an implicit mutation authority.
class HumanActionAdmission final {
private:
    HumanActionAdmission() = default;

    friend class HumanApplicationAccess;
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

enum class RecoveryInspectionState {
    unavailable,
    clean,
    pending,
    blocked,
};

struct RecoveryJournalEntry {
    core::Revision revision_before;
    core::Revision revision_after;
    std::string event_type;
};

// Read-only startup evidence. Inspection never mutates the project, journal,
// command history, or in-memory application state. `pending` means the
// journal is ahead of the saved project (or the project file is missing), so a
// future explicit recovery decision may be offered. `blocked` means the
// durable evidence is malformed or semantically inconsistent and must not be
// presented as recoverable state.
struct RecoveryInspection {
    std::filesystem::path project_path;
    std::filesystem::path journal_path;
    RecoveryInspectionState state = RecoveryInspectionState::unavailable;
    std::optional<core::Revision> project_revision;
    core::Revision durable_revision;
    std::size_t journal_entry_count = 0U;
    std::vector<RecoveryJournalEntry> journal_entries;
    std::optional<core::Diagnostic> diagnostic;

    [[nodiscard]] core::Result<void> validate() const;
};

using OperationId = std::uint64_t;

enum class OperationSource {
    human,
    ai_proposal,
    plugin,
    recovery,
};

struct AffectedSet {
    std::vector<scene::ObjectId> objects;
    std::vector<geometry::VertexId> vertices;
    std::vector<geometry::EdgeId> edges;
    std::vector<geometry::FaceId> faces;
};

struct ParameterPayload {
    std::optional<core::Transform> transform;
    std::optional<double> distance;
    std::optional<core::Vec3d> position;
    std::optional<bool> remove_orphaned_vertices;
    std::optional<double> factor;
    std::optional<geometry::EdgeId> support_edge;
    std::optional<std::uint32_t> segments;
};

struct DispatchReceipt {
    std::string action;
    core::Revision revision_before;
    core::Revision revision_after;
    bool document_changed = false;
    OperationId operation_id = 0U;
    OperationSource source = OperationSource::human;
    std::string provenance_id;
    AffectedSet affected;
    std::optional<ParameterPayload> parameters;
    std::vector<core::Diagnostic> warnings;
};

struct NewProjectAction {
    std::string name;
    bool discard_dirty = false;
};

struct OpenProjectAction {
    std::filesystem::path path;
    bool discard_dirty = false;
};

struct RecoverProjectAction {
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

struct ConvertSelectionAction {
    editor::SelectionMode mode;
};

struct ExpandSelectionAction {
    editor::SelectionExpansion expansion;
};

struct SelectShortestPathAction {
    geometry::VertexId goal;
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

struct RepeatLastToolAction {};

// Re-evaluates the previous tool with a complete replacement argument set.
// The tool identity and authoring selection remain revision-bound session state.
struct AdjustLastToolAction {
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
    RecoverProjectAction,
    SaveProjectAction,
    SetWorkspaceAction,
    SetSelectionModeAction,
    ConvertSelectionAction,
    ExpandSelectionAction,
    SelectShortestPathAction,
    SelectObjectAction,
    SelectVertexAction,
    SelectFaceAction,
    SelectEdgeAction,
    SetObjectTransformAction,
    InvokeToolAction,
    RepeatLastToolAction,
    AdjustLastToolAction,
    CreateBoxAction,
    CreatePlaneAction,
    CreateMeshObjectAction,
    UndoAction,
    RedoAction>;

// The explicit human/local front-end adapter is the only public production
// factory for HumanActionAdmission. The source/provenance overload is private
// to the application host boundary and cannot be used as a public raw mutation
// route.
class HumanApplicationAccess final {
public:
    [[nodiscard]] static core::Result<DispatchReceipt> dispatch(
        ApplicationSession& session,
        const ApplicationAction& action);
    [[nodiscard]] static core::Result<DispatchReceipt> commit_preview(
        ApplicationSession& session,
        editor::AuthoringPreview& preview);

private:
    friend class ::carto::ui::UiController;

    [[nodiscard]] static core::Result<DispatchReceipt> commit_preview(
        ApplicationSession& session,
        editor::AuthoringPreview& preview,
        OperationSource source,
        std::string_view provenance_id);
};

class ApplicationSession {
public:
    ApplicationSession();

    [[nodiscard]] static core::Result<RecoveryInspection> inspect_recovery(
        const std::filesystem::path& project_path);

    [[nodiscard]] core::Result<DispatchReceipt> dispatch(
        HumanActionAdmission admission,
        const ApplicationAction& action,
        OperationSource source = OperationSource::human,
        std::string_view provenance_id = {});
    [[nodiscard]] core::Result<editor::AuthoringContext> authoring_context() const;
    [[nodiscard]] core::Result<editor::AuthoringPreview> begin_preview(
        editor::PreviewKind kind) const;
    [[nodiscard]] core::Result<DispatchReceipt> commit_preview(
        HumanActionAdmission admission,
        editor::AuthoringPreview& preview,
        OperationSource source = OperationSource::human,
        std::string_view provenance_id = {});
    [[nodiscard]] ApplicationSnapshot snapshot() const;

    [[nodiscard]] const WorkspaceState& workspace() const noexcept { return workspace_; }
    [[nodiscard]] core::Result<void> can_close(bool discard_dirty = false) const;

private:
    [[nodiscard]] core::Result<void> set_workspace(WorkspaceState workspace);
    [[nodiscard]] core::Result<DispatchReceipt> new_project(const NewProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> open_project(const OpenProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> recover_project(
        const RecoverProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> save_project(const SaveProjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> execute_command(
        std::unique_ptr<editor::EditorCommand> command,
        std::string action,
        AffectedSet affected = {},
        std::optional<ParameterPayload> parameters = std::nullopt);
    [[nodiscard]] core::Result<DispatchReceipt> accept_command_mutation(
        std::string action,
        core::Revision revision_before,
        project::ProjectDocument before_document,
        AffectedSet affected = {},
        std::optional<ParameterPayload> parameters = std::nullopt);
    [[nodiscard]] core::Result<DispatchReceipt> select_object(const SelectObjectAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_vertex(const SelectVertexAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_face(const SelectFaceAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_edge(const SelectEdgeAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> convert_selection(
        const ConvertSelectionAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> expand_selection(
        const ExpandSelectionAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> select_shortest_path(
        const SelectShortestPathAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> invoke_tool(const InvokeToolAction& action);
    [[nodiscard]] core::Result<void> rebind_last_tool_selection();
    [[nodiscard]] core::Result<DispatchReceipt> repeat_last_tool();
    [[nodiscard]] core::Result<DispatchReceipt> adjust_last_tool(
        const AdjustLastToolAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_box(const CreateBoxAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_plane(const CreatePlaneAction& action);
    [[nodiscard]] core::Result<DispatchReceipt> create_mesh_object(
        const CreateMeshObjectAction& action);
    [[nodiscard]] core::Result<ApplicationAction> preview_action(
        const editor::AuthoringPreview& preview) const;
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
        core::Revision revision_before,
        AffectedSet affected = {},
        std::optional<ParameterPayload> parameters = std::nullopt);
    [[nodiscard]] core::Result<DispatchReceipt> failure(core::Diagnostic diagnostic);
    void reset_editor_state() noexcept;

    struct ViewportMeshCacheEntry {
        std::uint64_t project_generation = 0U;
        const geometry::EditableMesh* source = nullptr;
        core::Revision source_revision;
        std::shared_ptr<const geometry::CompiledMesh> mesh;
    };

    project::ProjectDocument document_;
    std::optional<std::filesystem::path> project_path_;
    core::Revision saved_revision_;
    std::uint64_t project_generation_ = 0;
    // snapshot() is a UI-thread read boundary. The cache is mutable so a
    // read-only snapshot can retain derived geometry without changing the
    // authoritative project document.
    mutable std::map<std::uint64_t, ViewportMeshCacheEntry> viewport_mesh_cache_;
    editor::SelectionState selection_;
    editor::CommandBus history_;
    editor::ToolRegistry tools_;
    std::optional<journal::Journal> journal_;
    WorkspaceState workspace_;
    std::vector<core::Diagnostic> problems_;
    std::optional<InvokeToolAction> last_tool_action_;
    std::optional<editor::AuthoringContext> last_tool_context_;
    OperationId next_operation_id_ = 1U;
    OperationSource active_source_ = OperationSource::human;
    std::string active_provenance_id_;
};

} // namespace carto::application
