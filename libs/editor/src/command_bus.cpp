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
    if (auto result = after.extrude_face(faces.front(), distance_); !result) {
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

CreateMeshObjectCommand::CreateMeshObjectCommand(
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
