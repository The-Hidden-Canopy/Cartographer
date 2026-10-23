#pragma once

#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace carto::editor {

enum class SelectionMode {
    object,
    vertex,
    edge,
    face,
};

enum class SelectionOperation {
    replace,
    add,
    toggle,
};

enum class SelectionExpansion {
    linked,
    grow,
    shrink,
    invert,
    loop,
    ring,
    boundary,
};

struct ComponentSelectionGroup {
    scene::ObjectId object;
    core::Revision mesh_revision;
    std::vector<geometry::VertexId> vertices;
    std::vector<geometry::VertexId> vertex_selection_order;
    std::vector<geometry::EdgeId> edges;
    std::vector<geometry::FaceId> faces;

    [[nodiscard]] constexpr bool operator==(const ComponentSelectionGroup&) const noexcept = default;
};

// Selection is editor/session state. It is deliberately not part of the
// project format or authoring revision and must be validated against context
// before a tool uses it.
class SelectionState {
public:
    [[nodiscard]] SelectionMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return active_count() == 0U; }

    [[nodiscard]] core::Result<void> set_mode(SelectionMode mode);
    [[nodiscard]] core::Result<void> convert_mode(
        SelectionMode mode,
        const geometry::EditableMesh& mesh);
    [[nodiscard]] core::Result<void> convert_mode(
        SelectionMode mode,
        const scene::Scene& scene,
        const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes);
    [[nodiscard]] core::Result<void> expand(
        SelectionExpansion expansion,
        const geometry::EditableMesh& mesh);
    [[nodiscard]] core::Result<void> expand(
        SelectionExpansion expansion,
        const scene::Scene& scene,
        const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes);
    [[nodiscard]] core::Result<void> select_shortest_path(
        geometry::VertexId goal,
        const geometry::EditableMesh& mesh);
    [[nodiscard]] core::Result<void> select_shortest_path(
        scene::ObjectId owner,
        geometry::VertexId goal,
        const scene::Scene& scene,
        const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes);
    void clear() noexcept;

    [[nodiscard]] core::Result<void> select_object(
        const scene::Scene& scene,
        scene::ObjectId object,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_vertex(
        const geometry::EditableMesh& mesh,
        geometry::VertexId vertex,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_vertex(
        const scene::Scene& scene,
        scene::ObjectId object,
        const geometry::EditableMesh& mesh,
        geometry::VertexId vertex,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_face(
        const geometry::EditableMesh& mesh,
        geometry::FaceId face,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_face(
        const scene::Scene& scene,
        scene::ObjectId object,
        const geometry::EditableMesh& mesh,
        geometry::FaceId face,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_edge(
        const geometry::EditableMesh& mesh,
        geometry::EdgeId edge,
        SelectionOperation operation = SelectionOperation::replace);
    [[nodiscard]] core::Result<void> select_edge(
        const scene::Scene& scene,
        scene::ObjectId object,
        const geometry::EditableMesh& mesh,
        geometry::EdgeId edge,
        SelectionOperation operation = SelectionOperation::replace);

    [[nodiscard]] core::Result<void> validate(
        const scene::Scene& scene,
        const geometry::EditableMesh* mesh = nullptr) const;
    [[nodiscard]] core::Result<void> validate(
        const scene::Scene& scene,
        const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes) const;
    [[nodiscard]] core::Result<void> validate(const geometry::EditableMesh& mesh) const;
    [[nodiscard]] std::optional<scene::ObjectId> component_object() const noexcept {
        return other_component_selections_.empty() ? component_object_ : std::nullopt;
    }
    [[nodiscard]] std::optional<core::Revision> component_mesh_revision() const noexcept {
        return other_component_selections_.empty() ? component_mesh_revision_ : std::nullopt;
    }
    [[nodiscard]] std::vector<ComponentSelectionGroup> component_groups() const;

    [[nodiscard]] std::vector<scene::ObjectId> selected_objects() const;
    [[nodiscard]] std::vector<geometry::VertexId> selected_vertices() const;
    [[nodiscard]] std::vector<geometry::VertexId> selected_vertices_in_order() const;
    [[nodiscard]] std::optional<geometry::VertexId> last_selected_vertex() const noexcept;
    [[nodiscard]] std::vector<geometry::EdgeId> selected_edges() const;
    [[nodiscard]] std::vector<geometry::FaceId> selected_faces() const;

private:
    [[nodiscard]] core::Result<void> prepare_component_selection(
        SelectionMode mode,
        std::optional<scene::ObjectId> object,
        const geometry::EditableMesh& mesh,
        SelectionOperation operation);
    [[nodiscard]] ComponentSelectionGroup active_component_group() const;
    void load_component_group(
        const ComponentSelectionGroup& group,
        const geometry::EditableMesh& mesh);
    void clear_all() noexcept;

    SelectionMode mode_ = SelectionMode::object;
    std::set<scene::ObjectId> objects_;
    std::set<geometry::VertexId> vertices_;
    std::vector<geometry::VertexId> vertex_selection_order_;
    std::set<geometry::EdgeId> edges_;
    std::set<geometry::FaceId> faces_;
    std::optional<scene::ObjectId> component_object_;
    const geometry::EditableMesh* component_mesh_ = nullptr;
    std::optional<core::Revision> component_mesh_revision_;
    std::map<scene::ObjectId, ComponentSelectionGroup> other_component_selections_;
    std::map<scene::ObjectId, const geometry::EditableMesh*> other_component_meshes_;
};

} // namespace carto::editor
