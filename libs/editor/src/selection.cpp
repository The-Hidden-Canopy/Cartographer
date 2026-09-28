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

} // namespace

std::size_t SelectionState::active_count() const noexcept {
    switch (mode_) {
    case SelectionMode::object:
        return objects_.size();
    case SelectionMode::vertex:
        return vertices_.size();
    case SelectionMode::face:
        return faces_.size();
    }
    return 0U;
}

void SelectionState::set_mode(SelectionMode mode) noexcept {
    if (mode_ == mode) {
        return;
    }
    clear_all();
    mode_ = mode;
}

void SelectionState::clear() noexcept { clear_all(); }

core::Result<void> SelectionState::select_object(
    const scene::Scene& scene,
    scene::ObjectId object,
    SelectionOperation operation) {
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
    if (!mesh.find_vertex(vertex)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh vertex"));
    }
    if (mode_ != SelectionMode::vertex) {
        clear_all();
        mode_ = SelectionMode::vertex;
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
    if (!mesh.find_face(face)) {
        return core::Result<void>::failure(missing("cannot select a missing mesh face"));
    }
    if (mode_ != SelectionMode::face) {
        clear_all();
        mode_ = SelectionMode::face;
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
        return validate(*mesh);
    case SelectionMode::face:
        if (!mesh) {
            return core::Result<void>::failure(
                invalid("face selection validation requires a mesh context"));
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
        for (const geometry::VertexId vertex : vertices_) {
            if (!mesh.find_vertex(vertex)) {
                return core::Result<void>::failure(
                    stale("selection contains a vertex no longer present in the mesh"));
            }
        }
        return core::Result<void>::success();
    case SelectionMode::face:
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

std::vector<geometry::FaceId> SelectionState::selected_faces() const {
    return {faces_.begin(), faces_.end()};
}

void SelectionState::clear_all() noexcept {
    objects_.clear();
    vertices_.clear();
    faces_.clear();
}

} // namespace carto::editor
