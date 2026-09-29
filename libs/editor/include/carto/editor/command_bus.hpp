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
    friend class ::carto::project::testing::ProjectDocumentAccess;
    friend class ::carto::project::testing::EditorProjectAccess;

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
