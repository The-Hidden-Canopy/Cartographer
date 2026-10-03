#include <carto/editor/command_bus.hpp>

#include <utility>

namespace carto::editor {

core::Result<void> CommandBus::execute(std::unique_ptr<EditorCommand> command) {
    if (!command) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_argument, "command bus cannot execute a null command"));
    }
    if (auto result = command->execute(); !result) {
        return result;
    }
    undo_stack_.push_back(std::move(command));
    redo_stack_.clear();
    return core::Result<void>::success();
}

core::Result<void> CommandBus::rollback_last_execute() {
    if (undo_stack_.empty()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state,
                             "command history has no accepted command to roll back"));
    }
    auto command = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    if (auto result = command->undo(); !result) {
        undo_stack_.push_back(std::move(command));
        return result;
    }
    // The failed admission consumed the redo branch when execute() accepted
    // the command. It must not become visible as a successful history state.
    redo_stack_.clear();
    return core::Result<void>::success();
}

core::Result<void> CommandBus::undo() {
    if (undo_stack_.empty()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "command history has no undo entry"));
    }
    auto& command = undo_stack_.back();
    if (auto result = command->undo(); !result) {
        return result;
    }
    redo_stack_.push_back(std::move(command));
    undo_stack_.pop_back();
    return core::Result<void>::success();
}

core::Result<void> CommandBus::redo() {
    if (redo_stack_.empty()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "command history has no redo entry"));
    }
    auto& command = redo_stack_.back();
    if (auto result = command->execute(); !result) {
        return result;
    }
    undo_stack_.push_back(std::move(command));
    redo_stack_.pop_back();
    return core::Result<void>::success();
}

void CommandBus::clear_history() noexcept {
    undo_stack_.clear();
    redo_stack_.clear();
}

SetObjectTransformCommand::SetObjectTransformCommand(
    scene::Scene& scene,
    scene::ObjectId object,
    core::Transform before,
    core::Transform after)
    : scene_(&scene), object_(object), before_(before), after_(after) {}

core::Result<void> SetObjectTransformCommand::execute() { return apply(after_); }

core::Result<void> SetObjectTransformCommand::undo() { return apply(before_); }

core::Result<void> SetObjectTransformCommand::apply(core::Transform transform) {
    if (!scene_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "transform command has no scene"));
    }
    return scene_->set_local_transform(object_, transform);
}

SetProjectObjectTransformCommand::SetProjectObjectTransformCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    scene::ObjectId object,
    core::Transform before,
    core::Transform after)
    : document_(&document),
      object_(object),
      before_(before),
      after_(after),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectObjectTransformCommand::execute() {
    return apply(after_);
}

core::Result<void> SetProjectObjectTransformCommand::undo() {
    return apply(before_);
}

core::Result<void> SetProjectObjectTransformCommand::apply(core::Transform transform) {
    if (!document_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project transform command has no document"));
    }
    const core::Revision expected =
        current_project_revision_.value_or(expected_project_revision_);
    auto result = document_->set_object_transform_if_revision(object_, expected, transform);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_project_revision_ = result.value();
    return core::Result<void>::success();
}

SetVertexPositionCommand::SetVertexPositionCommand(
    geometry::EditableMesh& mesh,
    geometry::VertexId vertex,
    core::Vec3d before,
    core::Vec3d after)
    : mesh_(&mesh), vertex_(vertex), before_(before), after_(after) {}

core::Result<void> SetVertexPositionCommand::execute() { return apply(after_); }

core::Result<void> SetVertexPositionCommand::undo() { return apply(before_); }

core::Result<void> SetVertexPositionCommand::apply(core::Vec3d position) {
    if (!mesh_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "vertex command has no mesh"));
    }
    const auto* current = mesh_->find_vertex(vertex_);
    if (!current) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "vertex command references a missing mesh vertex"));
    }
    return mesh_->apply_patch(geometry::MeshPatch{
        mesh_->revision(),
        {geometry::VertexChange{vertex_, current->position, position}},
    });
}

SetProjectSelectedVertexPositionCommand::SetProjectSelectedVertexPositionCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    core::Vec3d position)
    : document_(&document), selection_(&selection), position_(position) {}

core::Result<void> SetProjectSelectedVertexPositionCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project vertex command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::vertex) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex editing requires vertex selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex editing requires an object-bound vertex selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected vertex owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected vertex owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected vertex owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto vertices = selection_->selected_vertices();
    if (vertices.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex editing requires one selected vertex"));
    }
    if (vertices.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-vertex project editing is not implemented"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    if (auto result = after.set_vertex_position(vertices.front(), position_); !result) {
        return result;
    }
    auto replaced = document_->replace_mesh_if_revision(mesh_asset, before.revision(), after);
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> SetProjectSelectedVertexPositionCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

SlideVertexCommand::SlideVertexCommand(
    geometry::EditableMesh& mesh,
    geometry::VertexId vertex,
    geometry::EdgeId support_edge,
    double factor)
    : mesh_(&mesh), vertex_(vertex), support_edge_(support_edge), factor_(factor) {}

core::Result<void> SlideVertexCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "vertex slide command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "vertex slide command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }
    geometry::EditableMesh before = *mesh_;
    if (auto result = mesh_->slide_vertex(vertex_, support_edge_, factor_); !result) {
        return result;
    }
    before_ = std::move(before);
    after_ = *mesh_;
    return core::Result<void>::success();
}

core::Result<void> SlideVertexCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "vertex slide command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

SlideProjectSelectedVertexCommand::SlideProjectSelectedVertexCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    std::optional<geometry::EdgeId> support_edge,
    double factor)
    : document_(&document),
      selection_(&selection),
      requested_support_edge_(support_edge),
      factor_(factor) {}

core::Result<void> SlideProjectSelectedVertexCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex slide command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !support_edge_.has_value() ||
            !after_.has_value() || !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project vertex slide command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) return core::Result<void>::failure(result.error());
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::vertex) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex sliding requires vertex selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex sliding requires an object-bound vertex selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected vertex slide owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected vertex slide owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected vertex slide owner references a missing mesh"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto vertices = selection_->selected_vertices();
    if (vertices.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex sliding requires one selected vertex"));
    }
    if (vertices.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-vertex sliding is not implemented in this tool boundary"));
    }

    std::optional<geometry::EdgeId> support_edge = requested_support_edge_;
    if (!support_edge.has_value()) {
        const auto topology = mesh_iterator->second.topology();
        if (!topology) return core::Result<void>::failure(topology.error());
        for (const auto& edge : topology.value().edges) {
            if ((edge.first == vertices.front() || edge.second == vertices.front()) &&
                (!support_edge.has_value() || edge.id < *support_edge)) {
                support_edge = edge.id;
            }
        }
    }
    if (!support_edge.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "project vertex sliding requires an incident support edge"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    if (auto result = after.slide_vertex(vertices.front(), *support_edge, factor_); !result) {
        return result;
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after);
    if (!replaced) return core::Result<void>::failure(replaced.error());
    mesh_asset_ = mesh_asset;
    support_edge_ = *support_edge;
    before_ = std::move(before);
    after_ = std::move(after);
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> SlideProjectSelectedVertexCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex slide command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) return core::Result<void>::failure(result.error());
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

ExtrudeFaceCommand::ExtrudeFaceCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    double distance)
    : mesh_(&mesh), face_(face), distance_(distance) {}

core::Result<void> ExtrudeFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "extrude command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "extrude command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    const geometry::EditableMesh before = *mesh_;
    if (auto result = mesh_->extrude_face(face_, distance_); !result) {
        return result;
    }
    before_ = before;
    after_ = *mesh_;
    return core::Result<void>::success();
}

core::Result<void> ExtrudeFaceCommand::undo() {
    if (!mesh_ || !before_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "extrude command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

ExtrudeSelectedFaceCommand::ExtrudeSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double distance)
    : selection_(&selection), mesh_(&mesh), distance_(distance) {}

core::Result<void> ExtrudeSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face extrusion command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face extrusion requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face extrusion requires one selected face"));
        }
        if (faces.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-face extrusion is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<ExtrudeFaceCommand>(*mesh_, faces.front(), distance_);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> ExtrudeSelectedFaceCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face extrusion has no completed operation to undo"));
    }
    return delegate_->undo();
}

ExtrudeProjectSelectedFaceCommand::ExtrudeProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double distance)
    : document_(&document), selection_(&selection), distance_(distance) {}

core::Result<void> ExtrudeProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project extrusion command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project extrusion command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project extrusion requires face selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project extrusion requires an object-bound face selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected face owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto faces = selection_->selected_faces();
    if (faces.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project extrusion requires one selected face"));
    }
    if (faces.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-face project extrusion is not implemented"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto extrusion = after.extrude_face_with_receipt(faces.front(), distance_);
    if (!extrusion) {
        return core::Result<void>::failure(extrusion.error());
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, extrusion.value());
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> ExtrudeProjectSelectedFaceCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project extrusion command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

InsetFaceCommand::InsetFaceCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    double distance)
    : mesh_(&mesh), face_(face), distance_(distance) {}

core::Result<void> InsetFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "inset command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "inset command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto inset = mesh_->inset_face(face_, distance_);
    if (!inset) {
        return core::Result<void>::failure(inset.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(inset.value());
    return core::Result<void>::success();
}

core::Result<void> InsetFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "inset command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

InsetSelectedFaceCommand::InsetSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double distance)
    : selection_(&selection), mesh_(&mesh), distance_(distance) {}

core::Result<void> InsetSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face inset command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face inset requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face inset requires one selected face"));
        }
        if (faces.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-face inset is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<InsetFaceCommand>(*mesh_, faces.front(), distance_);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> InsetSelectedFaceCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face inset has no completed operation to undo"));
    }
    return delegate_->undo();
}

InsetProjectSelectedFaceCommand::InsetProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double distance)
    : document_(&document), selection_(&selection), distance_(distance) {}

core::Result<void> InsetProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face inset command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() || !receipt_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project face inset command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face inset requires face selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face inset requires an object-bound face selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected face owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto faces = selection_->selected_faces();
    if (faces.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face inset requires one selected face"));
    }
    if (faces.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-face project inset is not implemented"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto inset = after.inset_face(faces.front(), distance_);
    if (!inset) {
        return core::Result<void>::failure(inset.error());
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, inset.value());
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    receipt_ = std::move(inset.value());
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> InsetProjectSelectedFaceCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !receipt_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face inset command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

PokeFaceCommand::PokeFaceCommand(geometry::EditableMesh& mesh, geometry::FaceId face)
    : mesh_(&mesh), face_(face) {}

core::Result<void> PokeFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "poke command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "poke command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto poked = mesh_->poke_face(face_);
    if (!poked) {
        return core::Result<void>::failure(poked.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(poked.value());
    return core::Result<void>::success();
}

core::Result<void> PokeFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "poke command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

PokeSelectedFaceCommand::PokeSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh)
    : selection_(&selection), mesh_(&mesh) {}

core::Result<void> PokeSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face poke command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face poke requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face poke requires one selected face"));
        }
        if (faces.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-face poke is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<PokeFaceCommand>(*mesh_, faces.front());
        auto result = delegate_->execute();
        if (!result) delegate_.reset();
        return result;
    }
    return delegate_->execute();
}

core::Result<void> PokeSelectedFaceCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face poke has no completed operation to undo"));
    }
    return delegate_->undo();
}

PokeProjectSelectedFaceCommand::PokeProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(&document), selection_(&selection) {}

core::Result<void> PokeProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face poke command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() || !receipt_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project face poke command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) return core::Result<void>::failure(result.error());
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face poke requires face selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face poke requires an object-bound face selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected face owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto faces = selection_->selected_faces();
    if (faces.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face poke requires one selected face"));
    }
    if (faces.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-face project poke is not implemented"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto poked = after.poke_face(faces.front());
    if (!poked) return core::Result<void>::failure(poked.error());
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, poked.value());
    if (!replaced) return core::Result<void>::failure(replaced.error());
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    receipt_ = std::move(poked.value());
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> PokeProjectSelectedFaceCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !receipt_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face poke command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) return core::Result<void>::failure(result.error());
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

DeleteFaceCommand::DeleteFaceCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    bool remove_orphaned_vertices)
    : mesh_(&mesh), face_(face), remove_orphaned_vertices_(remove_orphaned_vertices) {}

core::Result<void> DeleteFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "delete face command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "delete face command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto deleted = mesh_->delete_face(face_, remove_orphaned_vertices_);
    if (!deleted) {
        return core::Result<void>::failure(deleted.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(deleted.value());
    return core::Result<void>::success();
}

core::Result<void> DeleteFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "delete face command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

DeleteSelectedFaceCommand::DeleteSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    bool remove_orphaned_vertices)
    : selection_(&selection),
      mesh_(&mesh),
      remove_orphaned_vertices_(remove_orphaned_vertices) {}

core::Result<void> DeleteSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face deletion command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face deletion requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face deletion requires one selected face"));
        }
        if (faces.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-face deletion is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<DeleteFaceCommand>(
            *mesh_, faces.front(), remove_orphaned_vertices_);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> DeleteSelectedFaceCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face deletion has no completed operation to undo"));
    }
    return delegate_->undo();
}

DeleteProjectSelectedFaceCommand::DeleteProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    bool remove_orphaned_vertices)
    : document_(&document),
      selection_(&selection),
      remove_orphaned_vertices_(remove_orphaned_vertices) {}

core::Result<void> DeleteProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face deletion command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() || !receipt_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project face deletion command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face deletion requires face selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face deletion requires an object-bound face selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected face owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto faces = selection_->selected_faces();
    if (faces.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face deletion requires one selected face"));
    }
    if (faces.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-face project deletion is not implemented"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto deleted = after.delete_face(faces.front(), remove_orphaned_vertices_);
    if (!deleted) {
        return core::Result<void>::failure(deleted.error());
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, deleted.value());
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    receipt_ = std::move(deleted.value());
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> DeleteProjectSelectedFaceCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !receipt_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face deletion command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

SplitEdgeCommand::SplitEdgeCommand(
    geometry::EditableMesh& mesh,
    geometry::EdgeId edge,
    double factor)
    : mesh_(&mesh), edge_(edge), factor_(factor) {}

core::Result<void> SplitEdgeCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "split edge command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "split edge command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto split = mesh_->split_edge(edge_, factor_);
    if (!split) {
        return core::Result<void>::failure(split.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(split.value());
    return core::Result<void>::success();
}

core::Result<void> SplitEdgeCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "split edge command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

SplitSelectedEdgeCommand::SplitSelectedEdgeCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double factor)
    : selection_(&selection), mesh_(&mesh), factor_(factor) {}

core::Result<void> SplitSelectedEdgeCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge split command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::edge) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-edge splitting requires edge selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto edges = selection_->selected_edges();
        if (edges.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-edge splitting requires one selected edge"));
        }
        if (edges.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-edge splitting is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<SplitEdgeCommand>(*mesh_, edges.front(), factor_);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> SplitSelectedEdgeCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge splitting has no completed operation to undo"));
    }
    return delegate_->undo();
}

SplitProjectSelectedEdgeCommand::SplitProjectSelectedEdgeCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double factor)
    : document_(&document), selection_(&selection), factor_(factor) {}

core::Result<void> SplitProjectSelectedEdgeCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge split command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() || !receipt_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project edge split command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::edge) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge splitting requires edge selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge splitting requires an object-bound edge selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected edge owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected edge owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected edge owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto edges = selection_->selected_edges();
    if (edges.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge splitting requires one selected edge"));
    }
    if (edges.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-edge project splitting is not implemented"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto split = after.split_edge(edges.front(), factor_);
    if (!split) {
        return core::Result<void>::failure(split.error());
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, split.value());
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    receipt_ = std::move(split.value());
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> SplitProjectSelectedEdgeCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !receipt_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge split command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

DissolveEdgeCommand::DissolveEdgeCommand(
    geometry::EditableMesh& mesh,
    geometry::EdgeId edge)
    : mesh_(&mesh), edge_(edge) {}

core::Result<void> DissolveEdgeCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "dissolve edge command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "dissolve edge command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto dissolved = mesh_->dissolve_edge(edge_);
    if (!dissolved) {
        return core::Result<void>::failure(dissolved.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(dissolved.value());
    return core::Result<void>::success();
}

core::Result<void> DissolveEdgeCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "dissolve edge command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

DissolveSelectedEdgeCommand::DissolveSelectedEdgeCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh)
    : selection_(&selection), mesh_(&mesh) {}

core::Result<void> DissolveSelectedEdgeCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge dissolve command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::edge) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-edge dissolve requires edge selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto edges = selection_->selected_edges();
        if (edges.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-edge dissolve requires one selected edge"));
        }
        if (edges.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-edge dissolve is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<DissolveEdgeCommand>(*mesh_, edges.front());
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> DissolveSelectedEdgeCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge dissolve has no completed operation to undo"));
    }
    return delegate_->undo();
}

DissolveProjectSelectedEdgeCommand::DissolveProjectSelectedEdgeCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(&document), selection_(&selection) {}

core::Result<void> DissolveProjectSelectedEdgeCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge dissolve command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() || !receipt_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project edge dissolve command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::edge) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge dissolve requires edge selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge dissolve requires an object-bound edge selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected edge owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected edge owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected edge owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto edges = selection_->selected_edges();
    if (edges.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge dissolve requires one selected edge"));
    }
    if (edges.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-edge project dissolve is not implemented"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto dissolved = after.dissolve_edge(edges.front());
    if (!dissolved) {
        return core::Result<void>::failure(dissolved.error());
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, dissolved.value());
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    receipt_ = std::move(dissolved.value());
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> DissolveProjectSelectedEdgeCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !receipt_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge dissolve command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

TriToQuadCommand::TriToQuadCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId first,
    geometry::FaceId second)
    : mesh_(&mesh), first_(first), second_(second) {}

core::Result<void> TriToQuadCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "triangle-to-quad command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "triangle-to-quad command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto converted = mesh_->tri_to_quad(first_, second_);
    if (!converted) {
        return core::Result<void>::failure(converted.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(converted.value());
    return core::Result<void>::success();
}

core::Result<void> TriToQuadCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "triangle-to-quad command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

TriToQuadSelectedFacesCommand::TriToQuadSelectedFacesCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh)
    : selection_(&selection), mesh_(&mesh) {}

core::Result<void> TriToQuadSelectedFacesCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected triangle-to-quad command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "triangle-to-quad conversion requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.size() != 2U) {
            return core::Result<void>::failure(core::Diagnostic(
                faces.empty() ? core::ErrorCode::invalid_state : core::ErrorCode::unsupported,
                "triangle-to-quad conversion requires exactly two selected faces"));
        }
        delegate_ = std::make_unique<TriToQuadCommand>(*mesh_, faces[0], faces[1]);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> TriToQuadSelectedFacesCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected triangle-to-quad conversion has no completed operation to undo"));
    }
    return delegate_->undo();
}

TriToQuadProjectSelectedFacesCommand::TriToQuadProjectSelectedFacesCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(&document), selection_(&selection) {}

core::Result<void> TriToQuadProjectSelectedFacesCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project triangle-to-quad command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() || !receipt_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project triangle-to-quad command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project triangle-to-quad conversion requires face selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project triangle-to-quad conversion requires an object-bound face selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected face owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected face owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto faces = selection_->selected_faces();
    if (faces.size() != 2U) {
        return core::Result<void>::failure(core::Diagnostic(
            faces.empty() ? core::ErrorCode::invalid_state : core::ErrorCode::unsupported,
            "project triangle-to-quad conversion requires exactly two selected faces"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto converted = after.tri_to_quad(faces[0], faces[1]);
    if (!converted) {
        return core::Result<void>::failure(converted.error());
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, converted.value());
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    receipt_ = std::move(converted.value());
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> TriToQuadProjectSelectedFacesCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !receipt_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project triangle-to-quad command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

MergeVerticesCommand::MergeVerticesCommand(
    geometry::EditableMesh& mesh,
    geometry::VertexId target,
    geometry::VertexId source)
    : mesh_(&mesh), target_(target), source_(source) {}

core::Result<void> MergeVerticesCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "merge vertices command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "merge vertices command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto merged = mesh_->merge_vertices(target_, source_);
    if (!merged) {
        return core::Result<void>::failure(merged.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(merged.value());
    return core::Result<void>::success();
}

core::Result<void> MergeVerticesCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "merge vertices command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

MergeSelectedVerticesCommand::MergeSelectedVerticesCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh)
    : selection_(&selection), mesh_(&mesh) {}

core::Result<void> MergeSelectedVerticesCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-vertex merge command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::vertex) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-vertex merge requires vertex selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto vertices = selection_->selected_vertices();
        if (vertices.size() != 2U) {
            return core::Result<void>::failure(core::Diagnostic(
                vertices.empty() ? core::ErrorCode::invalid_state :
                    core::ErrorCode::unsupported,
                "selected-vertex merge requires exactly two selected vertices"));
        }
        // SelectionState exposes sorted stable IDs, so the target choice is
        // deterministic and can be reproduced by a remote/native adapter.
        delegate_ = std::make_unique<MergeVerticesCommand>(
            *mesh_, vertices.front(), vertices.back());
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> MergeSelectedVerticesCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-vertex merge has no completed operation to undo"));
    }
    return delegate_->undo();
}

MergeProjectSelectedVerticesCommand::MergeProjectSelectedVerticesCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(&document), selection_(&selection) {}

core::Result<void> MergeProjectSelectedVerticesCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex merge command has incomplete context"));
    }
    if (before_.has_value()) {
        if (!mesh_asset_.has_value() || !after_.has_value() || !receipt_.has_value() ||
            !current_mesh_revision_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project vertex merge command has incomplete history state"));
        }
        auto result = document_->replace_mesh_if_revision(
            *mesh_asset_, *current_mesh_revision_, *after_);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        current_mesh_revision_ = result.value();
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::vertex) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex merge requires vertex selection mode"));
    }
    const auto owner = selection_->component_object();
    if (!owner.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex merge requires an object-bound vertex selection"));
    }
    const auto* object = document_->scene().find(*owner);
    if (!object) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected vertex owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected vertex owner has no mesh asset"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_->meshes().find(mesh_asset);
    if (mesh_iterator == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected vertex owner references a missing mesh asset"));
    }
    if (auto result = selection_->validate(document_->scene(), &mesh_iterator->second); !result) {
        return result;
    }
    const auto vertices = selection_->selected_vertices();
    if (vertices.size() != 2U) {
        return core::Result<void>::failure(core::Diagnostic(
            vertices.empty() ? core::ErrorCode::invalid_state :
                core::ErrorCode::unsupported,
            "project vertex merge requires exactly two selected vertices"));
    }

    geometry::EditableMesh before = mesh_iterator->second;
    geometry::EditableMesh after = before;
    auto merged = after.merge_vertices(vertices.front(), vertices.back());
    if (!merged) {
        return core::Result<void>::failure(merged.error());
    }
    auto replaced = document_->replace_mesh_if_revision(
        mesh_asset, before.revision(), after, merged.value());
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    mesh_asset_ = mesh_asset;
    before_ = std::move(before);
    after_ = std::move(after);
    receipt_ = std::move(merged.value());
    current_mesh_revision_ = replaced.value();
    return core::Result<void>::success();
}

core::Result<void> MergeProjectSelectedVerticesCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !before_.has_value() ||
        !after_.has_value() || !receipt_.has_value() || !current_mesh_revision_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex merge command has no completed operation to undo"));
    }
    auto result = document_->replace_mesh_if_revision(
        *mesh_asset_, *current_mesh_revision_, *before_);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_mesh_revision_ = result.value();
    return core::Result<void>::success();
}

CreateMeshObjectCommand::CreateMeshObjectCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::string object_name,
    geometry::EditableMesh mesh)
    : document_(&document), object_name_(std::move(object_name)), mesh_(std::move(mesh)) {}

core::Result<void> CreateMeshObjectCommand::execute() {
    if (!document_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "create mesh command has no project"));
    }
    if (mesh_asset_.has_value() || object_.has_value()) {
        return restore_instance();
    }
    return create_first_instance();
}

core::Result<void> CreateMeshObjectCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !object_.has_value()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "create mesh command has no instance to undo"));
    }
    return remove_instance();
}

core::Result<void> CreateMeshObjectCommand::create_first_instance() {
    const project::ProjectDocument before = *document_;
    const auto rollback = [this, &before](core::Diagnostic diagnostic) {
        *document_ = before;
        mesh_asset_.reset();
        object_.reset();
        return core::Result<void>::failure(std::move(diagnostic));
    };

    auto mesh_asset = document_->add_mesh(mesh_);
    if (!mesh_asset) {
        return core::Result<void>::failure(mesh_asset.error());
    }
    mesh_asset_ = mesh_asset.value();

    auto object = document_->create_object(object_name_);
    if (!object) {
        return rollback(object.error());
    }
    const scene::ObjectId object_id = object.value();
    if (auto attach = document_->attach_mesh(object_id, *mesh_asset_); !attach) {
        return rollback(attach.error());
    }
    const auto* inserted = document_->scene().find(object_id);
    if (!inserted) {
        return rollback(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "created mesh object disappeared before command capture"));
    }
    object_ = *inserted;
    return core::Result<void>::success();
}

core::Result<void> CreateMeshObjectCommand::restore_instance() {
    if (!mesh_asset_.has_value() || !object_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "create mesh command has incomplete restoration state"));
    }
    const project::ProjectDocument before = *document_;
    if (auto result = document_->insert_mesh(*mesh_asset_, mesh_); !result) {
        return result;
    }
    if (auto result = document_->insert_object(*object_); !result) {
        *document_ = before;
        return result;
    }
    return core::Result<void>::success();
}

core::Result<void> CreateMeshObjectCommand::remove_instance() {
    const project::ProjectDocument before = *document_;
    const scene::ObjectId object_id = object_->id;
    if (auto result = document_->remove_object(object_id); !result) {
        return result;
    }
    if (auto result = document_->remove_mesh(*mesh_asset_); !result) {
        *document_ = before;
        return result;
    }
    return core::Result<void>::success();
}

} // namespace carto::editor
