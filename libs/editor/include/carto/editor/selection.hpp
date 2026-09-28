#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <set>
#include <vector>

namespace carto::editor {

enum class SelectionMode {
    object,
    vertex,
    face,
};

enum class SelectionOperation {
    replace,
    add,
    toggle,
};

// Selection is editor/session state. It is deliberately not part of the
// project format or authoring revision and must be validated against context
// before a tool uses it.
class SelectionState {
public:
    [[nodiscard]] SelectionMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return active_count() == 0U; }

    void set_mode(SelectionMode mode) noexcept;
    void clear() noexcept;

    [[nodiscard]] core::Result<void> select_object(
        const scene::Scene& scene,
        scene::ObjectId object,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_vertex(
        const geometry::EditableMesh& mesh,
        geometry::VertexId vertex,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_face(
        const geometry::EditableMesh& mesh,
        geometry::FaceId face,
        SelectionOperation operation = SelectionOperation::replace);

    [[nodiscard]] core::Result<void> validate(
        const scene::Scene& scene,
        const geometry::EditableMesh* mesh = nullptr) const;
    [[nodiscard]] core::Result<void> validate(const geometry::EditableMesh& mesh) const;

    [[nodiscard]] std::vector<scene::ObjectId> selected_objects() const;
    [[nodiscard]] std::vector<geometry::VertexId> selected_vertices() const;
    [[nodiscard]] std::vector<geometry::FaceId> selected_faces() const;

private:
    void clear_all() noexcept;

    SelectionMode mode_ = SelectionMode::object;
    std::set<scene::ObjectId> objects_;
    std::set<geometry::VertexId> vertices_;
    std::set<geometry::FaceId> faces_;
};

} // namespace carto::editor
