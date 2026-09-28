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
    return mesh_->set_vertex_position(vertex_, position);
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
    auto mesh_asset = document_->add_mesh(mesh_);
    if (!mesh_asset) {
        return core::Result<void>::failure(mesh_asset.error());
    }
    mesh_asset_ = mesh_asset.value();

    auto object = document_->create_object(object_name_);
    if (!object) {
        static_cast<void>(document_->remove_mesh(*mesh_asset_));
        mesh_asset_.reset();
        return core::Result<void>::failure(object.error());
    }
    const scene::ObjectId object_id = object.value();
    if (auto attach = document_->attach_mesh(object_id, *mesh_asset_); !attach) {
        static_cast<void>(document_->remove_object(object_id));
        static_cast<void>(document_->remove_mesh(*mesh_asset_));
        mesh_asset_.reset();
        return attach;
    }
    const auto* inserted = document_->scene().find(object_id);
    if (!inserted) {
        static_cast<void>(document_->remove_object(object_id));
        static_cast<void>(document_->remove_mesh(*mesh_asset_));
        mesh_asset_.reset();
        return core::Result<void>::failure(core::Diagnostic(
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
    if (auto result = document_->insert_mesh(*mesh_asset_, mesh_); !result) {
        return result;
    }
    if (auto result = document_->insert_object(*object_); !result) {
        static_cast<void>(document_->remove_mesh(*mesh_asset_));
        return result;
    }
    return core::Result<void>::success();
}

core::Result<void> CreateMeshObjectCommand::remove_instance() {
    const scene::ObjectId object_id = object_->id;
    if (auto result = document_->remove_object(object_id); !result) {
        return result;
    }
    if (auto result = document_->remove_mesh(*mesh_asset_); !result) {
        // Restore the object if the mesh cannot be removed, keeping undo atomic
        // under unexpected external mutation.
        static_cast<void>(document_->insert_object(*object_));
        return result;
    }
    return core::Result<void>::success();
}

} // namespace carto::editor
