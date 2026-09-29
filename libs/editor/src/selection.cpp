#include <carto/editor/selection.hpp>

#include <string>
#include <utility>

namespace carto::editor {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic missing(std::string message) {
    return Diagnostic(ErrorCode::not_found, std::move(message));
}

Diagnostic stale(std::string message) {
    return Diagnostic(ErrorCode::stale_data, std::move(message));
}

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

bool valid_mode(SelectionMode mode) noexcept {
    return mode == SelectionMode::object || mode == SelectionMode::vertex ||
        mode == SelectionMode::edge || mode == SelectionMode::face;
}

bool valid_operation(SelectionOperation operation) noexcept {
    return operation == SelectionOperation::replace ||
        operation == SelectionOperation::add || operation == SelectionOperation::toggle;
}

} // namespace

std::size_t SelectionState::active_count() const noexcept {
    switch (mode_) {
    case SelectionMode::object:
        return objects_.size();
    case SelectionMode::vertex:
        return vertices_.size();
    case SelectionMode::edge:
        return edges_.size();
    case SelectionMode::face:
        return faces_.size();
    }
    return 0U;
}

core::Result<void> SelectionState::set_mode(SelectionMode mode) {
    if (!valid_mode(mode)) {
        return core::Result<void>::failure(invalid("selection mode is invalid"));
    }
    if (mode_ == mode) {
        return core::Result<void>::success();
    }
    clear_all();
    mode_ = mode;
    return core::Result<void>::success();
}

void SelectionState::clear() noexcept { clear_all(); }

core::Result<void> SelectionState::select_object(
    const scene::Scene& scene,
    scene::ObjectId object,
    SelectionOperation operation) {
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (!scene.find(object)) {
        return core::Result<void>::failure(missing("cannot select a missing scene object"));
    }
    if (mode_ != SelectionMode::object) {
        clear_all();
        mode_ = SelectionMode::object;
    }
    if (operation == SelectionOperation::replace) {
        objects_.clear();
    }
    if (operation == SelectionOperation::toggle && objects_.contains(object)) {
        objects_.erase(object);
    } else {
        objects_.insert(object);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_vertex(
    const geometry::EditableMesh& mesh,
    geometry::VertexId vertex,
    SelectionOperation operation) {
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (!mesh.find_vertex(vertex)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh vertex"));
    }
    if (auto result = prepare_component_selection(
            SelectionMode::vertex, std::nullopt, mesh, operation);
        !result) {
        return result;
    }
    if (operation == SelectionOperation::replace) {
        vertices_.clear();
    }
    if (operation == SelectionOperation::toggle && vertices_.contains(vertex)) {
        vertices_.erase(vertex);
    } else {
        vertices_.insert(vertex);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_vertex(
    const scene::Scene& scene,
    scene::ObjectId object,
    const geometry::EditableMesh& mesh,
    geometry::VertexId vertex,
    SelectionOperation operation) {
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (!scene.find(object)) {
        return core::Result<void>::failure(missing("cannot select a vertex on a missing object"));
    }
    if (!mesh.find_vertex(vertex)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh vertex"));
    }
    if (auto result = prepare_component_selection(SelectionMode::vertex, object, mesh, operation);
        !result) {
        return result;
    }
    if (operation == SelectionOperation::replace) {
        vertices_.clear();
    }
    if (operation == SelectionOperation::toggle && vertices_.contains(vertex)) {
        vertices_.erase(vertex);
    } else {
        vertices_.insert(vertex);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_face(
    const geometry::EditableMesh& mesh,
    geometry::FaceId face,
    SelectionOperation operation) {
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (!mesh.find_face(face)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh face"));
    }
    if (auto result = prepare_component_selection(
            SelectionMode::face, std::nullopt, mesh, operation);
        !result) {
        return result;
    }
    if (operation == SelectionOperation::replace) {
        faces_.clear();
    }
    if (operation == SelectionOperation::toggle && faces_.contains(face)) {
        faces_.erase(face);
    } else {
        faces_.insert(face);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_edge(
    const geometry::EditableMesh& mesh,
    geometry::EdgeId edge,
    SelectionOperation operation) {
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (!mesh.find_edge(edge)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh edge"));
    }
    if (auto result = prepare_component_selection(
            SelectionMode::edge, std::nullopt, mesh, operation);
        !result) {
        return result;
    }
    if (operation == SelectionOperation::replace) {
        edges_.clear();
    }
    if (operation == SelectionOperation::toggle && edges_.contains(edge)) {
        edges_.erase(edge);
    } else {
        edges_.insert(edge);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_edge(
    const scene::Scene& scene,
    scene::ObjectId object,
    const geometry::EditableMesh& mesh,
    geometry::EdgeId edge,
    SelectionOperation operation) {
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (!scene.find(object)) {
        return core::Result<void>::failure(missing("cannot select an edge on a missing object"));
    }
    if (!mesh.find_edge(edge)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh edge"));
    }
    if (auto result = prepare_component_selection(SelectionMode::edge, object, mesh, operation);
        !result) {
        return result;
    }
    if (operation == SelectionOperation::replace) {
        edges_.clear();
    }
    if (operation == SelectionOperation::toggle && edges_.contains(edge)) {
        edges_.erase(edge);
    } else {
        edges_.insert(edge);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_face(
    const scene::Scene& scene,
    scene::ObjectId object,
    const geometry::EditableMesh& mesh,
    geometry::FaceId face,
    SelectionOperation operation) {
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (!scene.find(object)) {
        return core::Result<void>::failure(missing("cannot select a face on a missing object"));
    }
    if (!mesh.find_face(face)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh face"));
    }
    if (auto result = prepare_component_selection(SelectionMode::face, object, mesh, operation);
        !result) {
        return result;
    }
    if (operation == SelectionOperation::replace) {
        faces_.clear();
    }
    if (operation == SelectionOperation::toggle && faces_.contains(face)) {
        faces_.erase(face);
    } else {
        faces_.insert(face);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::validate(
    const scene::Scene& scene,
    const geometry::EditableMesh* mesh) const {
    switch (mode_) {
    case SelectionMode::object:
        for (const scene::ObjectId object : objects_) {
            if (!scene.find(object)) {
                return core::Result<void>::failure(
                    stale("selection contains an object no longer present in the scene"));
            }
        }
        return core::Result<void>::success();
    case SelectionMode::vertex:
        if (!mesh) {
            return core::Result<void>::failure(
                invalid("vertex selection validation requires a mesh context"));
        }
        if (component_object_.has_value() && !scene.find(*component_object_)) {
            return core::Result<void>::failure(
                stale("selection owner object is no longer present in the scene"));
        }
        return validate(*mesh);
    case SelectionMode::edge:
        if (!mesh) {
            return core::Result<void>::failure(
                invalid("edge selection validation requires a mesh context"));
        }
        if (component_object_.has_value() && !scene.find(*component_object_)) {
            return core::Result<void>::failure(
                stale("selection owner object is no longer present in the scene"));
        }
        return validate(*mesh);
    case SelectionMode::face:
        if (!mesh) {
            return core::Result<void>::failure(
                invalid("face selection validation requires a mesh context"));
        }
        if (component_object_.has_value() && !scene.find(*component_object_)) {
            return core::Result<void>::failure(
                stale("selection owner object is no longer present in the scene"));
        }
        return validate(*mesh);
    }
    return core::Result<void>::failure(invalid("selection mode is invalid"));
}

core::Result<void> SelectionState::validate(const geometry::EditableMesh& mesh) const {
    switch (mode_) {
    case SelectionMode::object:
        return core::Result<void>::success();
    case SelectionMode::vertex:
        if (component_mesh_ && component_mesh_ != &mesh) {
            return core::Result<void>::failure(
                stale("selection belongs to a different mesh context"));
        }
        if (component_mesh_revision_.has_value() &&
            *component_mesh_revision_ != mesh.revision()) {
            return core::Result<void>::failure(
                stale("selection was created against an older mesh revision"));
        }
        for (const geometry::VertexId vertex : vertices_) {
            if (!mesh.find_vertex(vertex)) {
                return core::Result<void>::failure(
                    stale("selection contains a vertex no longer present in the mesh"));
            }
        }
        return core::Result<void>::success();
    case SelectionMode::edge:
        if (component_mesh_ && component_mesh_ != &mesh) {
            return core::Result<void>::failure(
                stale("selection belongs to a different mesh context"));
        }
        if (component_mesh_revision_.has_value() &&
            *component_mesh_revision_ != mesh.revision()) {
            return core::Result<void>::failure(
                stale("selection was created against an older mesh revision"));
        }
        for (const geometry::EdgeId edge : edges_) {
            if (!mesh.find_edge(edge)) {
                return core::Result<void>::failure(
                    stale("selection contains an edge no longer present in the mesh"));
            }
        }
        return core::Result<void>::success();
    case SelectionMode::face:
        if (component_mesh_ && component_mesh_ != &mesh) {
            return core::Result<void>::failure(
                stale("selection belongs to a different mesh context"));
        }
        if (component_mesh_revision_.has_value() &&
            *component_mesh_revision_ != mesh.revision()) {
            return core::Result<void>::failure(
                stale("selection was created against an older mesh revision"));
        }
        for (const geometry::FaceId face : faces_) {
            if (!mesh.find_face(face)) {
                return core::Result<void>::failure(
                    stale("selection contains a face no longer present in the mesh"));
            }
        }
        return core::Result<void>::success();
    }
    return core::Result<void>::failure(invalid("selection mode is invalid"));
}

std::vector<scene::ObjectId> SelectionState::selected_objects() const {
    return {objects_.begin(), objects_.end()};
}

std::vector<geometry::VertexId> SelectionState::selected_vertices() const {
    return {vertices_.begin(), vertices_.end()};
}

std::vector<geometry::EdgeId> SelectionState::selected_edges() const {
    return {edges_.begin(), edges_.end()};
}

std::vector<geometry::FaceId> SelectionState::selected_faces() const {
    return {faces_.begin(), faces_.end()};
}

void SelectionState::clear_all() noexcept {
    objects_.clear();
    vertices_.clear();
    edges_.clear();
    faces_.clear();
    component_object_.reset();
    component_mesh_ = nullptr;
    component_mesh_revision_.reset();
}

core::Result<void> SelectionState::prepare_component_selection(
    SelectionMode mode,
    std::optional<scene::ObjectId> object,
    const geometry::EditableMesh& mesh,
    SelectionOperation operation) {
    if (!valid_mode(mode)) {
        return core::Result<void>::failure(invalid("selection mode is invalid"));
    }
    if (!valid_operation(operation)) {
        return core::Result<void>::failure(invalid("selection operation is invalid"));
    }
    if (mode_ != mode) {
        clear_all();
        mode_ = mode;
        component_object_ = object;
        component_mesh_ = &mesh;
        component_mesh_revision_ = mesh.revision();
    } else if (object.has_value()) {
        const bool stale_mesh_context =
            component_mesh_ &&
            (component_mesh_ != &mesh || component_mesh_revision_ != mesh.revision());
        if (stale_mesh_context && operation != SelectionOperation::replace) {
            return core::Result<void>::failure(stale(
                "component selection belongs to an older or different mesh context"));
        }
        if (!component_object_.has_value() && active_count() != 0U &&
            operation != SelectionOperation::replace) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "unbound component selection requires replace before object binding"));
        }
        if (component_object_.has_value() && component_object_ != object &&
            operation != SelectionOperation::replace) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "component selection cannot span multiple scene objects"));
        }
        if (operation == SelectionOperation::replace || !component_object_.has_value()) {
            component_object_ = object;
        }
        if (operation == SelectionOperation::replace || stale_mesh_context || !component_mesh_) {
            component_mesh_ = &mesh;
            component_mesh_revision_ = mesh.revision();
        }
    } else {
        if (component_object_.has_value() && operation != SelectionOperation::replace) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "component selection requires an owning scene object"));
        }
        const bool stale_mesh_context =
            component_mesh_ &&
            (component_mesh_ != &mesh || component_mesh_revision_ != mesh.revision());
        if (stale_mesh_context && operation != SelectionOperation::replace) {
            return core::Result<void>::failure(stale(
                "component selection belongs to an older or different mesh context"));
        }
        if (operation == SelectionOperation::replace) {
            component_object_.reset();
            component_mesh_ = &mesh;
            component_mesh_revision_ = mesh.revision();
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::editor
