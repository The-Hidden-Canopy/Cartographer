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
#include <type_traits>
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
    // A durable history operation runs against an independent command copy.
    // Unrecognized implementations fail closed rather than replay on rollback.
    [[nodiscard]] virtual std::unique_ptr<EditorCommand> checkpoint() const { return {}; }
};

template<class Derived>
class CheckpointableEditorCommand : public EditorCommand {
public:
    [[nodiscard]] std::unique_ptr<EditorCommand> checkpoint() const override {
        if constexpr (std::is_copy_constructible_v<Derived>) {
            return std::make_unique<Derived>(static_cast<const Derived&>(*this));
        }
        return {};
    }
};

// Deep-copy nested command state when taking a transactional checkpoint.
template<class T>
class CommandCheckpointPtr {
public:
    CommandCheckpointPtr() = default;
    CommandCheckpointPtr(std::unique_ptr<T> value) : value_(std::move(value)) {}
    CommandCheckpointPtr(const CommandCheckpointPtr& other)
        : value_(other.value_ ? std::make_unique<T>(*other.value_) : nullptr) {}
    CommandCheckpointPtr& operator=(const CommandCheckpointPtr& other) {
        if (this != &other) value_ = other.value_ ? std::make_unique<T>(*other.value_) : nullptr;
        return *this;
    }
    CommandCheckpointPtr(CommandCheckpointPtr&&) noexcept = default;
    CommandCheckpointPtr& operator=(CommandCheckpointPtr&&) noexcept = default;
    CommandCheckpointPtr& operator=(std::unique_ptr<T> value) noexcept {
        value_ = std::move(value); return *this;
    }
    explicit operator bool() const noexcept { return static_cast<bool>(value_); }
    T* operator->() const noexcept { return value_.get(); }
    void reset() noexcept { value_.reset(); }
private:
    std::unique_ptr<T> value_;
};

class CommandBus {
public:
    [[nodiscard]] core::Result<void> execute(std::unique_ptr<EditorCommand> command);
    // Removes and reverses the command most recently accepted by execute().
    // The application layer uses this when a durable side effect fails after
    // an in-memory command has already been admitted.
    [[nodiscard]] core::Result<void> rollback_last_execute();
    // Completes durable admission and discards the superseded redo branch.
    void finalize_last_execute() noexcept;
    [[nodiscard]] core::Result<void> undo();
    [[nodiscard]] core::Result<void> redo();

    // Stages on a copy. Caller must restore its document on abort and then
    // discard the pending copy; no inverse command or external IO is invoked.
    [[nodiscard]] core::Result<void> prepare_durable_undo();
    [[nodiscard]] core::Result<void> prepare_durable_redo();
    void commit_durable_history() noexcept;
    void abort_durable_history() noexcept;

    void clear_history() noexcept;
    [[nodiscard]] std::size_t undo_count() const noexcept { return undo_stack_.size(); }
    [[nodiscard]] std::size_t redo_count() const noexcept { return redo_stack_.size(); }

private:
    std::vector<std::unique_ptr<EditorCommand>> undo_stack_;
    std::vector<std::unique_ptr<EditorCommand>> redo_stack_;
    std::vector<std::unique_ptr<EditorCommand>> previous_redo_stack_;
    bool rollback_pending_ = false;
    enum class PendingHistory { none, undo, redo };
    PendingHistory pending_history_ = PendingHistory::none;
    std::unique_ptr<EditorCommand> pending_command_;
};

// Standalone mesh/scene commands are intentionally lower-level. Application
// authoring must use the ProjectDocument-bound command variants below so
// project revision and asset identity guards cannot be bypassed.
class SetObjectTransformCommand final : public CheckpointableEditorCommand<SetObjectTransformCommand> {
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

class SetProjectObjectTransformCommand final : public CheckpointableEditorCommand<SetProjectObjectTransformCommand> {
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

    [[nodiscard]] core::Result<void> apply(
        core::Transform transform, const core::Transform& expected_current);

    project::ProjectDocument* document_;
    scene::ObjectId object_;
    core::Transform before_;
    core::Transform after_;
    core::Revision expected_project_revision_;
    std::optional<core::Revision> current_project_revision_;
};

class SetProjectSculptStackCommand final : public CheckpointableEditorCommand<SetProjectSculptStackCommand> {
public:
    SetProjectSculptStackCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::uint64_t mesh_asset,
        std::optional<project::SerializedSculptStack> before,
        project::SerializedSculptStack after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Project SculptStack"; }

private:
    project::ProjectDocument* document_;
    std::uint64_t mesh_asset_;
    std::optional<project::SerializedSculptStack> before_;
    project::SerializedSculptStack after_;
    core::Revision expected_project_revision_;
    bool applied_ = false;
};

// Binds the validated engineering authoring sidecar through the project
// command boundary so import, invalidation, undo, and redo share the same
// revision and journal semantics as ordinary authoring commands.
class SetProjectEngineeringAuthoringCommand final : public CheckpointableEditorCommand<SetProjectEngineeringAuthoringCommand> {
public:
    SetProjectEngineeringAuthoringCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::optional<project::SerializedEngineeringAuthoring> before,
        project::SerializedEngineeringAuthoring after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Set Project Engineering Authoring";
    }

private:
    project::ProjectDocument* document_;
    std::optional<project::SerializedEngineeringAuthoring> before_;
    project::SerializedEngineeringAuthoring after_;
    core::Revision expected_project_revision_;
    bool applied_ = false;
};

// Binds the validated public CONDITION source-authoring sidecar through the
// same governed command boundary as all other project mutations.
// Replaces a complete project snapshot as one admitted application command.
// Used for an explicitly accepted speculative result; undo restores the prior
// source content while keeping project revisions monotonic.
class ReplaceProjectDocumentCommand final : public CheckpointableEditorCommand<ReplaceProjectDocumentCommand> {
public:
    ReplaceProjectDocumentCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        project::ProjectDocument candidate,
        std::string label = "Accept Speculative Project Result");

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return label_;
    }

private:
    [[nodiscard]] bool matches_snapshot_ignoring_revision(
        const project::ProjectDocument& expected) const;

    project::ProjectDocument* document_;
    project::ProjectDocument before_;
    project::ProjectDocument after_;
    std::string label_;
    core::Revision expected_project_revision_;
    bool initialized_ = false;
    bool applied_ = false;
};

class SetProjectConditionAuthoringCommand final : public CheckpointableEditorCommand<SetProjectConditionAuthoringCommand> {
public:
    SetProjectConditionAuthoringCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::optional<project::SerializedConditionAuthoring> before,
        project::SerializedConditionAuthoring after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Set Project CONDITION Authoring";
    }

private:
    project::ProjectDocument* document_;
    std::optional<project::SerializedConditionAuthoring> before_;
    project::SerializedConditionAuthoring after_;
    core::Revision expected_project_revision_;
    bool has_executed_ = false;
    bool applied_ = false;
};

class SetProjectMaterialCatalogCommand final : public CheckpointableEditorCommand<SetProjectMaterialCatalogCommand> {
public:
    SetProjectMaterialCatalogCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        project::MaterialCatalog before,
        project::MaterialCatalog after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Material Catalog"; }

private:
    [[nodiscard]] core::Result<void> apply(const project::MaterialCatalog& catalog);

    project::ProjectDocument* document_;
    project::MaterialCatalog before_;
    project::MaterialCatalog after_;
    core::Revision expected_project_revision_;
    std::optional<core::Revision> current_project_revision_;
    bool applied_ = false;
};

class SetProjectPresentationCatalogCommand final : public CheckpointableEditorCommand<SetProjectPresentationCatalogCommand> {
public:
    SetProjectPresentationCatalogCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        project::PresentationCatalog before,
        project::PresentationCatalog after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Set Scene Presentation";
    }

private:
    project::ProjectDocument* document_;
    project::PresentationCatalog before_;
    project::PresentationCatalog after_;
    core::Revision expected_project_revision_;
    std::optional<core::Revision> current_project_revision_;
    bool applied_ = false;
};

class SetProjectAuthoringStandardsCommand final : public CheckpointableEditorCommand<SetProjectAuthoringStandardsCommand> {
public:
    SetProjectAuthoringStandardsCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        project::ProjectAuthoringStandards before,
        project::ProjectAuthoringStandards after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Set Project Authoring Standards";
    }

private:
    project::ProjectDocument* document_;
    project::ProjectAuthoringStandards before_;
    project::ProjectAuthoringStandards after_;
    core::Revision expected_project_revision_;
    std::optional<core::Revision> current_project_revision_;
    bool applied_ = false;
};

class ApplySourceReimportPlanCommand final : public CheckpointableEditorCommand<ApplySourceReimportPlanCommand> {
public:
    ApplySourceReimportPlanCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        assets::SourceReimportPlan reviewed_plan);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Apply Source Reimport Plan"; }

private:
    [[nodiscard]] assets::Sha256Digest history_state_digest() const;
    [[nodiscard]] core::Result<assets::SourceReimportPlan> plan_for_records(
        const std::vector<assets::SourceImportRecord>& records) const;

    project::ProjectDocument* document_;
    assets::SourceReimportPlan reviewed_plan_;
    std::vector<assets::SourceImportRecord> before_records_;
    std::vector<assets::SourceImportRecord> after_records_;
    core::Revision expected_project_revision_;
    assets::Sha256Digest expected_history_digest_;
    bool initialized_ = false;
    bool applied_ = false;
};

class SetProjectSpatialFieldCommand final : public CheckpointableEditorCommand<SetProjectSpatialFieldCommand> {
public:
    SetProjectSpatialFieldCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::optional<spatial_field::ScalarField2D> before,
        spatial_field::ScalarField2D after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Spatial Field"; }

private:
    project::ProjectDocument* document_;
    std::optional<spatial_field::ScalarField2D> before_;
    spatial_field::ScalarField2D after_;
    bool applied_ = false;
};

class SetProjectRoadAuthoringCommand final : public CheckpointableEditorCommand<SetProjectRoadAuthoringCommand> {
public:
    SetProjectRoadAuthoringCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        geometry::CurvePath centerline,
        project::RoadSource road);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Road Authoring"; }

private:
    project::ProjectDocument* document_;
    geometry::CurvePath authored_centerline_;
    project::RoadSource authored_road_;
    std::optional<geometry::CurvePath> expected_centerline_;
    std::optional<project::RoadSource> expected_road_;
    std::optional<geometry::CurvePath> previous_centerline_;
    std::optional<project::RoadSource> previous_road_;
    core::Revision expected_project_revision_;
    bool initialized_ = false;
    bool applied_ = false;
};

class SetProjectSweepSourceCommand final : public CheckpointableEditorCommand<SetProjectSweepSourceCommand> {
public:
    SetProjectSweepSourceCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        geometry::SweepSource source);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Procedural Sweep Source"; }

private:
    project::ProjectDocument* document_;
    geometry::SweepSource authored_source_;
    std::optional<geometry::SweepSource> previous_source_;
    std::optional<geometry::SweepSource> expected_current_source_;
    core::Revision expected_project_revision_;
    bool initialized_ = false;
    bool applied_ = false;
};

class SetProjectBiologicalAuthoringCommand final : public CheckpointableEditorCommand<SetProjectBiologicalAuthoringCommand> {
public:
    SetProjectBiologicalAuthoringCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        biological::BiologicalAuthoringDocument source);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Set Biological Authoring Source";
    }

private:
    project::ProjectDocument* document_;
    biological::BiologicalAuthoringDocument authored_source_;
    project::BiologicalAuthoringKey key_;
    std::optional<project::SerializedBiologicalAuthoring> previous_source_;
    std::optional<project::SerializedBiologicalAuthoring> expected_current_source_;
    core::Revision expected_project_revision_;
    bool initialized_ = false;
    bool applied_ = false;
};

class SetProjectSignTextSourceCommand final : public CheckpointableEditorCommand<SetProjectSignTextSourceCommand> {
public:
    SetProjectSignTextSourceCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        geometry::SignTextSource source);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Sign Text Source"; }

private:
    project::ProjectDocument* document_;
    geometry::SignTextSource authored_source_;
    std::optional<geometry::SignTextSource> previous_source_;
    std::optional<geometry::SignTextSource> expected_current_source_;
    core::Revision expected_project_revision_;
    bool initialized_ = false;
    bool applied_ = false;
};

class SetVertexPositionCommand final : public CheckpointableEditorCommand<SetVertexPositionCommand> {
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

class SetProjectSelectedVertexPositionCommand final : public CheckpointableEditorCommand<SetProjectSelectedVertexPositionCommand> {
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
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::MeshPatch patch;
        core::Revision current_mesh_revision;
        scene::ObjectId owner;
    };
    std::vector<AppliedMeshEdit> edits_;
};

// Replaces one validated project mesh revision as an undoable authoring command.
// The project and mesh revisions are both checked before each execute/undo.
class SetProjectMeshCommand final : public CheckpointableEditorCommand<SetProjectMeshCommand> {
public:
    SetProjectMeshCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::uint64_t mesh_asset,
        core::Revision expected_project_revision,
        core::Revision expected_mesh_revision,
        geometry::EditableMesh after);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Set Project Mesh"; }

private:
    project::ProjectDocument* document_;
    std::uint64_t mesh_asset_;
    core::Revision expected_project_revision_;
    core::Revision expected_mesh_revision_;
    std::optional<core::Revision> current_project_revision_;
    std::optional<core::Revision> current_mesh_revision_;
    std::optional<geometry::EditableMesh> before_;
    geometry::EditableMesh after_;
    bool applied_ = false;
};

class SlideVertexCommand final : public CheckpointableEditorCommand<SlideVertexCommand> {
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

class SlideProjectSelectedVertexCommand final : public CheckpointableEditorCommand<SlideProjectSelectedVertexCommand> {
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
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::MeshPatch patch;
        core::Revision current_mesh_revision;
    };
    std::vector<AppliedMeshEdit> edits_;
};

class ExtrudeFaceCommand final : public CheckpointableEditorCommand<ExtrudeFaceCommand> {
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

class ExtrudeSelectedFaceCommand final : public CheckpointableEditorCommand<ExtrudeSelectedFaceCommand> {
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
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
};

class ExtrudeProjectSelectedFaceCommand final : public CheckpointableEditorCommand<ExtrudeProjectSelectedFaceCommand> {
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
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        std::vector<geometry::FaceExtrusionPatch> patches;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
        scene::ObjectId owner;
    };
    std::vector<AppliedMeshEdit> edits_;
};

class InsetFaceCommand final : public CheckpointableEditorCommand<InsetFaceCommand> {
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

class InsetSelectedFaceCommand final : public CheckpointableEditorCommand<InsetSelectedFaceCommand> {
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
    CommandCheckpointPtr<InsetFaceCommand> delegate_;
};

class InsetProjectSelectedFaceCommand final : public CheckpointableEditorCommand<InsetProjectSelectedFaceCommand> {
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
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        geometry::FaceInsetPatch patch;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
    };
    std::vector<AppliedMeshEdit> edits_;
};

class PokeFaceCommand final : public CheckpointableEditorCommand<PokeFaceCommand> {
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

class PokeSelectedFaceCommand final : public CheckpointableEditorCommand<PokeSelectedFaceCommand> {
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
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
};

class PokeProjectSelectedFaceCommand final : public CheckpointableEditorCommand<PokeProjectSelectedFaceCommand> {
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
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        std::vector<geometry::FacePokePatch> patches;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
    };
    std::vector<AppliedMeshEdit> edits_;
};

class DeleteFaceCommand final : public CheckpointableEditorCommand<DeleteFaceCommand> {
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

class DeleteSelectedFaceCommand final : public CheckpointableEditorCommand<DeleteSelectedFaceCommand> {
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
    CommandCheckpointPtr<DeleteFaceCommand> delegate_;
};

class DeleteProjectSelectedFaceCommand final : public CheckpointableEditorCommand<DeleteProjectSelectedFaceCommand> {
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

    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        geometry::FaceDeletionPatch patch;
        core::Revision current_mesh_revision;
    };

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    bool remove_orphaned_vertices_;
    std::vector<AppliedMeshEdit> edits_;
};

class SplitEdgeCommand final : public CheckpointableEditorCommand<SplitEdgeCommand> {
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

class SplitSelectedEdgeCommand final : public CheckpointableEditorCommand<SplitSelectedEdgeCommand> {
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
    CommandCheckpointPtr<SplitEdgeCommand> delegate_;
};

class SplitProjectSelectedEdgeCommand final : public CheckpointableEditorCommand<SplitProjectSelectedEdgeCommand> {
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

    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        geometry::EdgeSplitPatch patch;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
    };

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    double factor_;
    std::vector<AppliedMeshEdit> edits_;
};

class BevelEdgeCommand final : public CheckpointableEditorCommand<BevelEdgeCommand> {
public:
    BevelEdgeCommand(
        geometry::EditableMesh& mesh,
        geometry::EdgeId edge,
        double width,
        std::uint32_t segments);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Bevel Edge"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::EdgeId edge_;
    double width_;
    std::uint32_t segments_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class BevelSelectedEdgeCommand final : public CheckpointableEditorCommand<BevelSelectedEdgeCommand> {
public:
    BevelSelectedEdgeCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh,
        double width,
        std::uint32_t segments);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Bevel Selected Edge"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    double width_;
    std::uint32_t segments_;
    CommandCheckpointPtr<BevelEdgeCommand> delegate_;
};

class BevelProjectSelectedEdgeCommand final : public CheckpointableEditorCommand<BevelProjectSelectedEdgeCommand> {
public:
    BevelProjectSelectedEdgeCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        double width,
        std::uint32_t segments);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Bevel Project Selected Edge";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        std::vector<geometry::BevelEdgePatch> patches;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
    };

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    double width_;
    std::uint32_t segments_;
    std::vector<AppliedMeshEdit> edits_;
};

class BevelVertexCommand final : public CheckpointableEditorCommand<BevelVertexCommand> {
public:
    BevelVertexCommand(
        geometry::EditableMesh& mesh,
        geometry::VertexId vertex,
        double width,
        std::uint32_t segments);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Bevel Vertex"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::VertexId vertex_;
    double width_;
    std::uint32_t segments_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class BevelSelectedVertexCommand final : public CheckpointableEditorCommand<BevelSelectedVertexCommand> {
public:
    BevelSelectedVertexCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh,
        double width,
        std::uint32_t segments);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Bevel Selected Vertex"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    double width_;
    std::uint32_t segments_;
    CommandCheckpointPtr<BevelVertexCommand> delegate_;
};

class BevelProjectSelectedVertexCommand final : public CheckpointableEditorCommand<BevelProjectSelectedVertexCommand> {
public:
    BevelProjectSelectedVertexCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        double width,
        std::uint32_t segments);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Bevel Project Selected Vertex";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    double width_;
    std::uint32_t segments_;
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        std::vector<geometry::BevelVertexPatch> patches;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
    };
    std::vector<AppliedMeshEdit> edits_;
};

class DissolveEdgeCommand final : public CheckpointableEditorCommand<DissolveEdgeCommand> {
public:
    DissolveEdgeCommand(
        geometry::EditableMesh& mesh,
        geometry::EdgeId edge);
    DissolveEdgeCommand(
        geometry::EditableMesh& mesh,
        std::vector<geometry::EdgeId> edges);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Dissolve Edge"; }

private:
    geometry::EditableMesh* mesh_;
    std::vector<geometry::EdgeId> edges_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class DissolveSelectedEdgeCommand final : public CheckpointableEditorCommand<DissolveSelectedEdgeCommand> {
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
    CommandCheckpointPtr<DissolveEdgeCommand> delegate_;
};

class DissolveProjectSelectedEdgeCommand final : public CheckpointableEditorCommand<DissolveProjectSelectedEdgeCommand> {
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
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        geometry::EdgeDissolvePatch patch;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
    };
    std::vector<AppliedMeshEdit> edits_;
};

class TriToQuadCommand final : public CheckpointableEditorCommand<TriToQuadCommand> {
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

class TriToQuadSelectedFacesCommand final : public CheckpointableEditorCommand<TriToQuadSelectedFacesCommand> {
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
    CommandCheckpointPtr<TriToQuadCommand> delegate_;
};

class TriToQuadProjectSelectedFacesCommand final : public CheckpointableEditorCommand<TriToQuadProjectSelectedFacesCommand> {
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

    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        geometry::TriToQuadPatch patch;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        core::Revision current_mesh_revision;
    };

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::vector<AppliedMeshEdit> edits_;
};

class MergeVerticesCommand final : public CheckpointableEditorCommand<MergeVerticesCommand> {
public:
    MergeVerticesCommand(
        geometry::EditableMesh& mesh,
        geometry::VertexId target,
        geometry::VertexId source,
        geometry::VertexMergeMode mode = geometry::VertexMergeMode::target);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Merge Vertices"; }

private:
    geometry::EditableMesh* mesh_;
    geometry::VertexId target_;
    geometry::VertexId source_;
    geometry::VertexMergeMode mode_;
    std::optional<geometry::EditableMesh> before_;
    std::optional<geometry::EditableMesh> after_;
    std::optional<geometry::TopologyEditReceipt> receipt_;
};

class MergeSelectedVerticesCommand final : public CheckpointableEditorCommand<MergeSelectedVerticesCommand> {
public:
    MergeSelectedVerticesCommand(
        const SelectionState& selection,
        geometry::EditableMesh& mesh,
        geometry::VertexMergeMode mode = geometry::VertexMergeMode::target);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Merge Selected Vertices"; }

private:
    const SelectionState* selection_;
    geometry::EditableMesh* mesh_;
    geometry::VertexMergeMode mode_;
    CommandCheckpointPtr<MergeVerticesCommand> delegate_;
};

class MergeProjectSelectedVerticesCommand final : public CheckpointableEditorCommand<MergeProjectSelectedVerticesCommand> {
public:
    // With merge_distance set, every selected vertex (at least two) is a
    // merge-by-distance candidate; otherwise exactly two vertices are welded.
    MergeProjectSelectedVerticesCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        geometry::VertexMergeMode mode = geometry::VertexMergeMode::target,
        std::optional<double> merge_distance = std::nullopt);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return merge_distance_.has_value() ?
            "Merge Project Selected Vertices By Distance" :
            "Merge Project Selected Vertices";
    }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    geometry::VertexMergeMode mode_;
    std::optional<double> merge_distance_;
    struct AppliedMeshEdit {
        std::uint64_t mesh_asset = 0U;
        std::optional<geometry::MergeByDistancePatch> merge_by_distance_patch;
        std::optional<geometry::VertexMergePatch> vertex_merge_patch;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_mesh_revision;
    };
    std::vector<AppliedMeshEdit> edits_;
};

class CreateMeshObjectCommand final : public CheckpointableEditorCommand<CreateMeshObjectCommand> {
public:
    CreateMeshObjectCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::string object_name,
        geometry::EditableMesh mesh,
        core::Transform object_transform = core::Transform::identity(),
        std::optional<std::uint64_t> existing_mesh_asset = std::nullopt,
        std::optional<project::SignTextInstanceReceipt> sign_text_receipt = std::nullopt,
        std::optional<project::RoadSurfaceInstanceReceipt> road_surface_receipt = std::nullopt,
        std::optional<project::BiologicalProxyInstanceReceipt>
            biological_proxy_receipt = std::nullopt);

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
    std::optional<project::SignTextInstanceReceipt> sign_text_receipt_;
    std::optional<project::RoadSurfaceInstanceReceipt> road_surface_receipt_;
    std::optional<project::BiologicalProxyInstanceReceipt> biological_proxy_receipt_;
    core::Transform object_transform_ = core::Transform::identity();
    std::optional<std::uint64_t> existing_mesh_asset_;
    bool owns_mesh_asset_ = false;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<scene::SceneObject> object_;
};

// Imports or reimports one source-backed mesh as a single journaled command.
// The mesh asset and registry record are restored together on undo/redo.
class ImportSourceMeshCommand final : public CheckpointableEditorCommand<ImportSourceMeshCommand> {
public:
    ImportSourceMeshCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::string object_name,
        geometry::EditableMesh mesh,
        assets::SourceImportRecord source_record);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Import Source Mesh"; }

private:
    [[nodiscard]] assets::Sha256Digest history_state_digest() const;
    [[nodiscard]] core::Result<void> apply_registry_record(
        std::optional<assets::SourceImportRecord> record);
    [[nodiscard]] core::Result<void> apply_imported_mesh();
    [[nodiscard]] core::Result<void> apply_previous_mesh();
    [[nodiscard]] core::Result<void> restore_created_instance();
    [[nodiscard]] core::Result<void> remove_created_instance();

    project::ProjectDocument* document_;
    std::string object_name_;
    geometry::EditableMesh imported_mesh_;
    assets::SourceImportRecord imported_record_;
    std::optional<assets::SourceImportRecord> previous_record_;
    std::optional<geometry::EditableMesh> previous_mesh_;
    std::optional<std::uint64_t> mesh_asset_;
    std::optional<scene::SceneObject> object_;
    core::Revision expected_project_revision_;
    assets::Sha256Digest expected_history_digest_;
    bool created_instance_ = false;
    bool mesh_changed_ = false;
    bool initialized_ = false;
    bool applied_ = false;
};

// One source-bound scene import through the ordinary command history. Inputs
// are already mapped and validated by ApplicationSession; execution still
// rechecks the current project and registry before publishing any product.
class ImportGltfSceneCommand final : public CheckpointableEditorCommand<ImportGltfSceneCommand> {
public:
    ImportGltfSceneCommand(ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        std::vector<std::pair<std::uint64_t, geometry::EditableMesh>> meshes,
        std::vector<scene::SceneObject> nodes,
        project::MaterialCatalog catalog,
        assets::SourceImportRecord source, std::filesystem::path blob_root);
    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Import glTF Scene"; }
private:
    [[nodiscard]] assets::Sha256Digest history_state_digest() const;
    [[nodiscard]] core::Result<void> apply_source(bool present);
    project::ProjectDocument* document_;
    std::vector<std::pair<std::uint64_t, geometry::EditableMesh>> meshes_;
    std::vector<scene::SceneObject> nodes_;
    project::MaterialCatalog before_catalog_;
    project::MaterialCatalog after_catalog_;
    assets::SourceImportRecord source_;
    std::filesystem::path blob_root_;
    core::Revision initial_revision_;
    assets::Sha256Digest history_digest_;
    bool initialized_ = false;
    bool applied_ = false;
};

// Creates a second root-level scene object that references an existing mesh
// asset. This deliberately duplicates no geometry and clones no hierarchy:
// modelling edits to the shared asset remain explicit, while object placement
// stays independently undoable.
class DuplicateProjectMeshInstanceCommand final : public CheckpointableEditorCommand<DuplicateProjectMeshInstanceCommand> {
public:
    DuplicateProjectMeshInstanceCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        scene::ObjectId source,
        std::string object_name,
        core::Vec3d local_translation_offset);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override { return "Duplicate Mesh Instance"; }

private:
    friend class ::carto::application::ApplicationSession;
    friend struct ToolContext;

    [[nodiscard]] core::Result<void> create_first_instance();
    [[nodiscard]] core::Result<void> restore_instance();
    [[nodiscard]] core::Result<void> remove_instance();

    project::ProjectDocument* document_;
    scene::ObjectId source_;
    std::string object_name_;
    core::Vec3d local_translation_offset_;
    std::optional<scene::SceneObject> object_;
    std::optional<project::BiologicalProxyInstanceReceipt> biological_proxy_receipt_;
};

inline constexpr std::size_t kMaxMeshInstanceBatchInstances = 4'096U;
inline constexpr double kMaxMeshInstanceBatchOffsetMagnitude = 1.0e9;

struct MeshInstanceBatchInstance {
    std::string object_name;
    core::Vec3d local_translation_offset;
};

// Duplicates one visible, unlocked, parentless root mesh object into a
// bounded batch of new root-level instances that all reference the source
// mesh asset. Every placement carries an explicit finite local translation
// offset inside the bounded authoring extent. All entries are validated
// before the first mutation, and a mid-batch failure reverses the instances
// already admitted, so the command commits atomically and journals through
// the same undo/redo path as the single-instance duplicate command.
class DuplicateProjectMeshInstanceBatchCommand final : public CheckpointableEditorCommand<DuplicateProjectMeshInstanceBatchCommand> {
public:
    DuplicateProjectMeshInstanceBatchCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        scene::ObjectId source,
        std::vector<MeshInstanceBatchInstance> instances);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Duplicate Mesh Instance Batch";
    }

private:
    [[nodiscard]] core::Result<void> create_first_instances();
    [[nodiscard]] core::Result<void> restore_instances();
    [[nodiscard]] core::Result<void> remove_instances();

    project::ProjectDocument* document_;
    ProjectCommandAdmission admission_;
    scene::ObjectId source_;
    std::vector<MeshInstanceBatchInstance> instances_;
    std::vector<CommandCheckpointPtr<DuplicateProjectMeshInstanceCommand>> delegates_;
};

// Commits one exact CONDITION vegetation preview as a source-mesh instance
// batch. Scene objects and their durable placement receipts are inserted,
// undone, and redone together; partial batches are rolled back atomically.
class CreatePlantScatterBatchCommand final : public CheckpointableEditorCommand<CreatePlantScatterBatchCommand> {
public:
    CreatePlantScatterBatchCommand(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        scene::ObjectId botanical_source_object,
        condition::VegetationPreviewResult preview);

    [[nodiscard]] core::Result<void> execute() override;
    [[nodiscard]] core::Result<void> undo() override;
    [[nodiscard]] std::string label() const override {
        return "Commit CONDITION Vegetation Placements";
    }
    [[nodiscard]] const std::vector<scene::ObjectId>& created_objects() const noexcept {
        return created_objects_;
    }

private:
    [[nodiscard]] core::Result<void> create_first_instances();
    [[nodiscard]] core::Result<void> restore_instances();
    [[nodiscard]] core::Result<void> remove_instances();

    project::ProjectDocument* document_;
    scene::ObjectId botanical_source_object_;
    condition::VegetationPreviewResult preview_;
    std::vector<scene::SceneObject> instances_;
    std::vector<project::ConditionVegetationPlacementReceipt> receipts_;
    std::vector<scene::ObjectId> created_objects_;
};

} // namespace carto::editor
