#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/project.hpp>
#include <carto/editor/selection.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace carto::editor {

// A project-bound command may mutate a ProjectDocument only when it carries
// this admission token. ApplicationSession creates the token for an
// application-dispatched command; low-level callers cannot manufacture one.
class ProjectCommandAdmission final {
private:
    ProjectCommandAdmission() = default;

    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;
};

class EditorCommand {
public:
    virtual ~EditorCommand() = default;

    [[nodiscard]] virtual core::Result<void> execute() = 0;
    [[nodiscard]] virtual core::Result<void> undo() = 0;
    [[nodiscard]] virtual std::string label() const = 0;
};

class CommandBus {
public:
    [[nodiscard]] core::Result<void> execute(std::unique_ptr<EditorCommand> command);
    // Removes and reverses the command most recently accepted by execute().
    // The application layer uses this when a durable side effect fails after
    // an in-memory command has already been admitted.
    [[nodiscard]] core::Result<void> rollback_last_execute();
    [[nodiscard]] core::Result<void> undo();
    [[nodiscard]] core::Result<void> redo();

    void clear_history() noexcept;
    [[nodiscard]] std::size_t undo_count() const noexcept { return undo_stack_.size(); }
    [[nodiscard]] std::size_t redo_count() const noexcept { return redo_stack_.size(); }

private:
    std::vector<std::unique_ptr<EditorCommand>> undo_stack_;
    std::vector<std::unique_ptr<EditorCommand>> redo_stack_;
};

// Standalone mesh/scene commands are intentionally lower-level. Application
// authoring must use the ProjectDocument-bound command variants below so
// project revision and asset identity guards cannot be bypassed.
class SetObjectTransformCommand final : public EditorCommand {
public:
    SetObjectTransformCommand(
        scene::Scene& scene,
        scene::ObjectId object,
        core::Transform before,
        core::Transform after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Object Transform"; }

private:
    [[nodiscard]] core::Result<void> apply(core::Transform transform);

    scene::Scene* scene_;
    scene::ObjectId object_;
    core::Transform before_;
    core::Transform after_;
};

class SetProjectObjectTransformCommand final : public EditorCommand {
public:
    SetProjectObjectTransformCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        scene::ObjectId object,
        core::Transform before,
        core::Transform after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Set Project Object Transform";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    [[nodiscard]] core::Result<void> apply(core::Transform transform);

    project::ProjectDocument* document_;
    scene::ObjectId object_;
    core::Transform before_;
    core::Transform after_;
    core::Revision expected_project_revision_;
    std::optional<core::Revision> current_project_revision_;
};

class SetVertexPositionCommand final : public EditorCommand {
public:
    SetVertexPositionCommand(
        geometry::EditableMesh& mesh,
        geometry::VertexId vertex,
        core::Vec3d before,
        core::Vec3d after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Vertex Position"; }

private:
    [[nodiscard]] core::Result<void> apply(core::Vec3d position);

    geometry::EditableMesh* mesh_;
    geometry::VertexId vertex_;
    core::Vec3d before_;
    core::Vec3d after_;
};

class SetProjectSelectedVertexPositionCommand final : public EditorCommand {
public:
    SetProjectSelectedVertexPositionCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        core::Vec3d position);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Set Project Selected Vertex Position";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    core::Vec3d position_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<core::Revision> current_mesh_revision_;
};

class SlideVertexCommand final : public EditorCommand {
public:
    SlideVertexCommand(
        geometry::EditableMesh& mesh,
        geometry::VertexId vertex,
        geometry::EdgeId support_edge,
        double factor);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Slide Vertex"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::VertexId vertex_;
    geometry::EdgeId support_edge_;
    double factor_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
};

class SlideProjectSelectedVertexCommand final : public EditorCommand {
public:
    SlideProjectSelectedVertexCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        std::optional<geometry::EdgeId> support_edge,
        double factor);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Slide Project Selected Vertex";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::optional<geometry::EdgeId> requested_support_edge_;
    double factor_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EdgeId> support_edge_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<core::Revision> current_mesh_revision_;
};

class ExtrudeFaceCommand final : public EditorCommand {
public:
    ExtrudeFaceCommand(geometry::EditableMesh& mesh, geometry::FaceId face, double distance);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Extrude Face"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::FaceId face_;
    double distance_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
};

class ExtrudeSelectedFaceCommand final : public EditorCommand {
public:
    ExtrudeSelectedFaceCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh,
        double distance);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Extrude Selected Face"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    double distance_;
    std::unique_ptr<ExtrudeFaceCommand> delegate_;
};

class ExtrudeProjectSelectedFaceCommand final : public EditorCommand {
public:
    ExtrudeProjectSelectedFaceCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        double distance);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Extrude Project Selected Face";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    double distance_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<core::Revision> current_mesh_revision_;
};

class InsetFaceCommand final : public EditorCommand {
public:
    InsetFaceCommand(
        geometry::EditableMesh& mesh,
        geometry::FaceId face,
        double distance);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Inset Face"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::FaceId face_;
    double distance_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class InsetSelectedFaceCommand final : public EditorCommand {
public:
    InsetSelectedFaceCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh,
        double distance);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Inset Selected Face"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    double distance_;
    std::unique_ptr<InsetFaceCommand> delegate_;
};

class InsetProjectSelectedFaceCommand final : public EditorCommand {
public:
    InsetProjectSelectedFaceCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        double distance);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Inset Project Selected Face";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    double distance_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
    std::optional<core::Revision> current_mesh_revision_;
};

class PokeFaceCommand final : public EditorCommand {
public:
    PokeFaceCommand(geometry::EditableMesh& mesh, geometry::FaceId face);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Poke Face"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::FaceId face_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class PokeSelectedFaceCommand final : public EditorCommand {
public:
    PokeSelectedFaceCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Poke Selected Face"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    std::unique_ptr<PokeFaceCommand> delegate_;
};

class PokeProjectSelectedFaceCommand final : public EditorCommand {
public:
    PokeProjectSelectedFaceCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Poke Project Selected Face";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
    std::optional<core::Revision> current_mesh_revision_;
};

class DeleteFaceCommand final : public EditorCommand {
public:
    DeleteFaceCommand(
        geometry::EditableMesh& mesh,
        geometry::FaceId face,
        bool remove_orphaned_vertices);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Delete Face"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::FaceId face_;
    bool remove_orphaned_vertices_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class DeleteSelectedFaceCommand final : public EditorCommand {
public:
    DeleteSelectedFaceCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh,
        bool remove_orphaned_vertices);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Delete Selected Face"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    bool remove_orphaned_vertices_;
    std::unique_ptr<DeleteFaceCommand> delegate_;
};

class DeleteProjectSelectedFaceCommand final : public EditorCommand {
public:
    DeleteProjectSelectedFaceCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        bool remove_orphaned_vertices);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Delete Project Selected Face";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    bool remove_orphaned_vertices_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
    std::optional<core::Revision> current_mesh_revision_;
};

class SplitEdgeCommand final : public EditorCommand {
public:
    SplitEdgeCommand(
        geometry::EditableMesh& mesh,
        geometry::EdgeId edge,
        double factor);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Split Edge"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::EdgeId edge_;
    double factor_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class SplitSelectedEdgeCommand final : public EditorCommand {
public:
    SplitSelectedEdgeCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh,
        double factor);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Split Selected Edge"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    double factor_;
    std::unique_ptr<SplitEdgeCommand> delegate_;
};

class SplitProjectSelectedEdgeCommand final : public EditorCommand {
public:
    SplitProjectSelectedEdgeCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        double factor);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Split Project Selected Edge";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    double factor_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
    std::optional<core::Revision> current_mesh_revision_;
};

class DissolveEdgeCommand final : public EditorCommand {
public:
    DissolveEdgeCommand(
        geometry::EditableMesh& mesh,
        geometry::EdgeId edge);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Dissolve Edge"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::EdgeId edge_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class DissolveSelectedEdgeCommand final : public EditorCommand {
public:
    DissolveSelectedEdgeCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Dissolve Selected Edge"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    std::unique_ptr<DissolveEdgeCommand> delegate_;
};

class DissolveProjectSelectedEdgeCommand final : public EditorCommand {
public:
    DissolveProjectSelectedEdgeCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Dissolve Project Selected Edge";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
    std::optional<core::Revision> current_mesh_revision_;
};

class TriToQuadCommand final : public EditorCommand {
public:
    TriToQuadCommand(
        geometry::EditableMesh& mesh,
        geometry::FaceId first,
        geometry::FaceId second);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Triangles to Quad"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::FaceId first_;
    geometry::FaceId second_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class TriToQuadSelectedFacesCommand final : public EditorCommand {
public:
    TriToQuadSelectedFacesCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Selected Triangles to Quad";
    }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    std::unique_ptr<TriToQuadCommand> delegate_;
};

class TriToQuadProjectSelectedFacesCommand final : public EditorCommand {
public:
    TriToQuadProjectSelectedFacesCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Project Selected Triangles to Quad";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
    std::optional<core::Revision> current_mesh_revision_;
};

class MergeVerticesCommand final : public EditorCommand {
public:
    MergeVerticesCommand(
        geometry::EditableMesh& mesh,
        geometry::VertexId target,
        geometry::VertexId source);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Merge Vertices"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::VertexId target_;
    geometry::VertexId source_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class MergeSelectedVerticesCommand final : public EditorCommand {
public:
    MergeSelectedVerticesCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Merge Selected Vertices"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    std::unique_ptr<MergeVerticesCommand> delegate_;
};

class MergeProjectSelectedVerticesCommand final : public EditorCommand {
public:
    MergeProjectSelectedVerticesCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Merge Project Selected Vertices";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
    std::optional<core::Revision> current_mesh_revision_;
};

class CreateMeshObjectCommand final : public EditorCommand {
public:
    CreateMeshObjectCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::string object_name,
        geometry::EditableMesh mesh);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Create Mesh Object"; }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    [[nodiscard]] core::Result<void> create_first_instance();
    [[nodiscard]] core::Result<void> restore_instance();
    [[nodiscard]] core::Result<void> remove_instance();

    project::ProjectDocument* document_;
    std::string object_name_;
    geometry::EditableMesh mesh_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<scene::SceneObject> object_;
};

} // namespace carto::editor
