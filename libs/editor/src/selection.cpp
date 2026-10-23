#include <carto/editor/selection.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <type_traits>
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

bool valid_expansion(SelectionExpansion expansion) noexcept {
    return expansion == SelectionExpansion::linked ||
        expansion == SelectionExpansion::grow ||
        expansion == SelectionExpansion::shrink ||
        expansion == SelectionExpansion::invert ||
        expansion == SelectionExpansion::loop ||
        expansion == SelectionExpansion::ring ||
        expansion == SelectionExpansion::boundary;
}

} // namespace

std::size_t SelectionState::active_count() const noexcept {
    std::size_t count = 0U;
    switch (mode_) {
    case SelectionMode::object:
        return objects_.size();
    case SelectionMode::vertex:
        count = vertices_.size();
        break;
    case SelectionMode::edge:
        count = edges_.size();
        break;
    case SelectionMode::face:
        count = faces_.size();
        break;
    }
    for (const auto& [object, group] : other_component_selections_) {
        static_cast<void>(object);
        switch (mode_) {
        case SelectionMode::object:
            break;
        case SelectionMode::vertex:
            count += group.vertices.size();
            break;
        case SelectionMode::edge:
            count += group.edges.size();
            break;
        case SelectionMode::face:
            count += group.faces.size();
            break;
        }
    }
    return count;
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

core::Result<void> SelectionState::convert_mode(
    SelectionMode mode,
    const geometry::EditableMesh& mesh) {
    if (!other_component_selections_.empty()) {
        return core::Result<void>::failure(invalid(
            "multi-object component conversion requires every owner mesh context"));
    }
    if (!valid_mode(mode) || mode == SelectionMode::object) {
        return core::Result<void>::failure(invalid(
            "component selection conversion requires a vertex, edge, or face mode"));
    }
    if (mode_ == SelectionMode::object) {
        return core::Result<void>::failure(invalid(
            "object selection cannot be converted without a component selection"));
    }
    if (auto result = validate(mesh); !result) {
        return result;
    }
    if (mode_ == mode) {
        return core::Result<void>::success();
    }

    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<void>::failure(topology.error().with_context(
            "selection conversion topology"));
    }
    using EdgeKey = std::pair<geometry::VertexId, geometry::VertexId>;
    const auto edge_key = [](geometry::VertexId first, geometry::VertexId second) {
        return first < second ? EdgeKey{first, second} : EdgeKey{second, first};
    };
    std::map<EdgeKey, geometry::EdgeId> edge_by_vertices;
    for (const auto& edge : topology.value().edges) {
        const auto [iterator, inserted] = edge_by_vertices.emplace(
            edge_key(edge.first, edge.second), edge.id);
        static_cast<void>(iterator);
        if (!inserted) {
            return core::Result<void>::failure(stale(
                "selection conversion found duplicate edge endpoints"));
        }
    }

    std::set<geometry::VertexId> converted_vertices;
    std::set<geometry::EdgeId> converted_edges;
    std::set<geometry::FaceId> converted_faces;
    const auto face_edges = [&](const geometry::Face& face)
        -> core::Result<std::vector<geometry::EdgeId>> {
        std::vector<geometry::EdgeId> result;
        result.reserve(face.vertices.size());
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const auto iterator = edge_by_vertices.find(edge_key(
                face.vertices[index], face.vertices[(index + 1U) % face.vertices.size()]));
            if (iterator == edge_by_vertices.end()) {
                return core::Result<std::vector<geometry::EdgeId>>::failure(stale(
                    "selection conversion found a face edge missing from topology"));
            }
            result.push_back(iterator->second);
        }
        return core::Result<std::vector<geometry::EdgeId>>::success(std::move(result));
    };

    if (mode_ == SelectionMode::vertex && mode == SelectionMode::edge) {
        for (const auto& edge : topology.value().edges) {
            if (vertices_.contains(edge.first) && vertices_.contains(edge.second)) {
                converted_edges.insert(edge.id);
            }
        }
    } else if (mode_ == SelectionMode::vertex && mode == SelectionMode::face) {
        for (const auto& face : mesh.faces_sorted()) {
            if (std::all_of(
                    face.vertices.begin(), face.vertices.end(),
                    [this](geometry::VertexId vertex) { return vertices_.contains(vertex); })) {
                converted_faces.insert(face.id);
            }
        }
    } else if (mode_ == SelectionMode::edge && mode == SelectionMode::vertex) {
        for (const auto& edge : topology.value().edges) {
            if (edges_.contains(edge.id)) {
                converted_vertices.insert(edge.first);
                converted_vertices.insert(edge.second);
            }
        }
    } else if (mode_ == SelectionMode::edge && mode == SelectionMode::face) {
        for (const auto& face : mesh.faces_sorted()) {
            auto boundaries = face_edges(face);
            if (!boundaries) {
                return core::Result<void>::failure(boundaries.error());
            }
            if (std::all_of(
                    boundaries.value().begin(), boundaries.value().end(),
                    [this](geometry::EdgeId edge) { return edges_.contains(edge); })) {
                converted_faces.insert(face.id);
            }
        }
    } else if (mode_ == SelectionMode::face && mode == SelectionMode::vertex) {
        for (const auto& face : mesh.faces_sorted()) {
            if (faces_.contains(face.id)) {
                converted_vertices.insert(face.vertices.begin(), face.vertices.end());
            }
        }
    } else if (mode_ == SelectionMode::face && mode == SelectionMode::edge) {
        for (const auto& face : mesh.faces_sorted()) {
            if (!faces_.contains(face.id)) continue;
            auto boundaries = face_edges(face);
            if (!boundaries) {
                return core::Result<void>::failure(boundaries.error());
            }
            converted_edges.insert(boundaries.value().begin(), boundaries.value().end());
        }
    } else {
        return core::Result<void>::failure(invalid(
            "selection conversion does not support the requested mode pair"));
    }

    const auto owner = component_object_;
    const auto* component_mesh = component_mesh_;
    const auto component_revision = component_mesh_revision_;
    vertices_.clear();
    vertex_selection_order_.clear();
    edges_.clear();
    faces_.clear();
    mode_ = mode;
    component_object_ = owner;
    component_mesh_ = component_mesh;
    component_mesh_revision_ = component_revision;
    vertices_ = std::move(converted_vertices);
    if (mode_ == SelectionMode::vertex) {
        vertex_selection_order_.assign(vertices_.begin(), vertices_.end());
    }
    edges_ = std::move(converted_edges);
    faces_ = std::move(converted_faces);
    return core::Result<void>::success();
}

core::Result<void> SelectionState::convert_mode(
    SelectionMode mode,
    const scene::Scene& scene,
    const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes) {
    if (!valid_mode(mode) || mode == SelectionMode::object) {
        return core::Result<void>::failure(invalid(
            "component selection conversion requires a vertex, edge, or face mode"));
    }
    if (mode_ == SelectionMode::object) {
        return core::Result<void>::failure(invalid(
            "object selection cannot be converted without a component selection"));
    }
    const auto groups = component_groups();
    if (groups.empty()) {
        if (active_count() != 0U) {
            return core::Result<void>::failure(invalid(
                "unbound component selection cannot be converted across scene objects"));
        }
        mode_ = mode;
        vertices_.clear();
        vertex_selection_order_.clear();
        edges_.clear();
        faces_.clear();
        return core::Result<void>::success();
    }
    if (auto result = validate(scene, meshes); !result) return result;

    std::vector<ComponentSelectionGroup> converted_groups;
    converted_groups.reserve(groups.size());
    for (const auto& group : groups) {
        const auto mesh_entry = meshes.find(group.object);
        if (mesh_entry == meshes.end() || mesh_entry->second == nullptr) {
            return core::Result<void>::failure(missing(
                "selection conversion is missing an owner mesh context"));
        }
        SelectionState converted;
        converted.mode_ = mode_;
        converted.component_object_ = group.object;
        converted.component_mesh_ = mesh_entry->second;
        converted.component_mesh_revision_ = group.mesh_revision;
        converted.vertices_ = std::set<geometry::VertexId>(
            group.vertices.begin(), group.vertices.end());
        converted.vertex_selection_order_ = group.vertex_selection_order;
        converted.edges_ = std::set<geometry::EdgeId>(group.edges.begin(), group.edges.end());
        converted.faces_ = std::set<geometry::FaceId>(group.faces.begin(), group.faces.end());
        if (auto result = converted.convert_mode(mode, *mesh_entry->second); !result) {
            return result;
        }
        converted_groups.push_back(converted.active_component_group());
    }

    std::optional<scene::ObjectId> primary_object = component_object_;
    if (!primary_object.has_value() && !converted_groups.empty()) {
        primary_object = converted_groups.front().object;
    }
    mode_ = mode;
    vertices_.clear();
    vertex_selection_order_.clear();
    edges_.clear();
    faces_.clear();
    component_object_.reset();
    component_mesh_ = nullptr;
    component_mesh_revision_.reset();
    other_component_selections_.clear();
    other_component_meshes_.clear();

    if (primary_object.has_value()) {
        const auto primary_group = std::find_if(
            converted_groups.begin(), converted_groups.end(),
            [&primary_object](const ComponentSelectionGroup& group) {
                return group.object == *primary_object;
            });
        if (primary_group != converted_groups.end()) {
            load_component_group(*primary_group, *meshes.at(primary_group->object));
        }
    }
    for (const auto& group : converted_groups) {
        if (component_object_.has_value() && group.object == *component_object_) continue;
        other_component_selections_.emplace(group.object, group);
        other_component_meshes_.emplace(group.object, meshes.at(group.object));
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::expand(
    SelectionExpansion expansion,
    const geometry::EditableMesh& mesh) {
    if (!other_component_selections_.empty()) {
        return core::Result<void>::failure(invalid(
            "multi-object component expansion requires per-owner topology traversal"));
    }
    if (!valid_expansion(expansion)) {
        return core::Result<void>::failure(invalid("selection expansion is invalid"));
    }
    if (mode_ == SelectionMode::object) {
        return core::Result<void>::failure(invalid(
            "object selection cannot use component expansion"));
    }
    if ((expansion == SelectionExpansion::loop ||
         expansion == SelectionExpansion::ring ||
         expansion == SelectionExpansion::boundary) && mode_ != SelectionMode::edge) {
        return core::Result<void>::failure(invalid(
            "loop, ring, and boundary expansion require edge selection mode"));
    }
    if (auto result = validate(mesh); !result) {
        return result;
    }

    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<void>::failure(topology.error().with_context(
            "selection expansion topology"));
    }

    std::map<geometry::VertexId, std::set<geometry::VertexId>> vertex_neighbors;
    std::map<geometry::VertexId, std::set<geometry::EdgeId>> incident_edges;
    for (const auto& edge : topology.value().edges) {
        vertex_neighbors[edge.first].insert(edge.second);
        vertex_neighbors[edge.second].insert(edge.first);
        incident_edges[edge.first].insert(edge.id);
        incident_edges[edge.second].insert(edge.id);
    }

    std::map<geometry::EdgeId, std::set<geometry::EdgeId>> edge_neighbors;
    for (const auto& [vertex, edges] : incident_edges) {
        static_cast<void>(vertex);
        for (const geometry::EdgeId edge : edges) {
            auto& neighbors = edge_neighbors[edge];
            neighbors.insert(edges.begin(), edges.end());
            neighbors.erase(edge);
        }
    }

    std::map<geometry::EdgeId, std::set<geometry::FaceId>> faces_by_edge;
    for (const auto& half_edge : topology.value().half_edges) {
        faces_by_edge[half_edge.edge].insert(half_edge.face);
    }
    std::map<geometry::FaceId, std::set<geometry::FaceId>> face_neighbors;
    for (const auto& [edge, faces] : faces_by_edge) {
        static_cast<void>(edge);
        for (const geometry::FaceId face : faces) {
            auto& neighbors = face_neighbors[face];
            neighbors.insert(faces.begin(), faces.end());
            neighbors.erase(face);
        }
    }

    const auto linked_vertices = [this, &topology]() -> core::Result<std::set<geometry::VertexId>> {
        std::set<geometry::VertexId> result;
        for (const geometry::VertexId seed : vertices_) {
            auto component = topology.value().linked_component(seed);
            if (!component) {
                return core::Result<std::set<geometry::VertexId>>::failure(component.error());
            }
            result.insert(component.value().begin(), component.value().end());
        }
        return core::Result<std::set<geometry::VertexId>>::success(std::move(result));
    };
    const auto linked_from_graph = [](const auto& selected, const auto& graph) {
        using Id = typename std::decay_t<decltype(selected)>::value_type;
        std::set<Id> result;
        std::vector<Id> pending;
        for (const Id seed : selected) {
            if (result.insert(seed).second) pending.push_back(seed);
        }
        for (std::size_t index = 0U; index < pending.size(); ++index) {
            const auto neighbors = graph.find(pending[index]);
            if (neighbors == graph.end()) continue;
            for (const Id neighbor : neighbors->second) {
                if (result.insert(neighbor).second) pending.push_back(neighbor);
            }
        }
        return result;
    };

    const auto grow_from_graph = [](const auto& selected, const auto& graph) {
        using Id = typename std::decay_t<decltype(selected)>::value_type;
        std::set<Id> result = selected;
        for (const Id seed : selected) {
            const auto neighbors = graph.find(seed);
            if (neighbors == graph.end()) continue;
            result.insert(neighbors->second.begin(), neighbors->second.end());
        }
        return result;
    };
    const auto shrink_from_graph = [](const auto& selected, const auto& graph) {
        using Id = typename std::decay_t<decltype(selected)>::value_type;
        std::set<Id> result;
        for (const Id candidate : selected) {
            const auto neighbors = graph.find(candidate);
            if (neighbors == graph.end() || neighbors->second.empty() ||
                std::all_of(
                    neighbors->second.begin(), neighbors->second.end(),
                    [&selected](Id neighbor) { return selected.contains(neighbor); })) {
                result.insert(candidate);
            }
        }
        return result;
    };

    std::set<geometry::VertexId> next_vertices;
    std::set<geometry::EdgeId> next_edges;
    std::set<geometry::FaceId> next_faces;
    if (mode_ == SelectionMode::vertex) {
        if (expansion == SelectionExpansion::linked) {
            auto result = linked_vertices();
            if (!result) return core::Result<void>::failure(result.error());
            next_vertices = std::move(result.value());
        } else if (expansion == SelectionExpansion::grow) {
            next_vertices = grow_from_graph(vertices_, vertex_neighbors);
        } else if (expansion == SelectionExpansion::shrink) {
            next_vertices = shrink_from_graph(vertices_, vertex_neighbors);
        } else {
            for (const auto& vertex : mesh.vertices_sorted()) {
                if (!vertices_.contains(vertex.id)) next_vertices.insert(vertex.id);
            }
        }
    } else if (mode_ == SelectionMode::edge) {
        if (expansion == SelectionExpansion::loop ||
            expansion == SelectionExpansion::ring ||
            expansion == SelectionExpansion::boundary) {
            for (const geometry::EdgeId seed : edges_) {
                core::Result<std::vector<geometry::EdgeId>> traversal =
                    expansion == SelectionExpansion::loop
                    ? topology.value().edge_loop(seed)
                    : expansion == SelectionExpansion::ring
                    ? topology.value().edge_ring(seed)
                    : topology.value().boundary_loop(seed);
                if (!traversal) return core::Result<void>::failure(traversal.error());
                next_edges.insert(traversal.value().begin(), traversal.value().end());
            }
        } else if (expansion == SelectionExpansion::linked) {
            next_edges = linked_from_graph(edges_, edge_neighbors);
        } else if (expansion == SelectionExpansion::grow) {
            next_edges = grow_from_graph(edges_, edge_neighbors);
        } else if (expansion == SelectionExpansion::shrink) {
            next_edges = shrink_from_graph(edges_, edge_neighbors);
        } else {
            for (const auto& edge : topology.value().edges) {
                if (!edges_.contains(edge.id)) next_edges.insert(edge.id);
            }
        }
    } else {
        if (expansion == SelectionExpansion::linked) {
            next_faces = linked_from_graph(faces_, face_neighbors);
        } else if (expansion == SelectionExpansion::grow) {
            next_faces = grow_from_graph(faces_, face_neighbors);
        } else if (expansion == SelectionExpansion::shrink) {
            next_faces = shrink_from_graph(faces_, face_neighbors);
        } else {
            for (const auto& face : mesh.faces_sorted()) {
                if (!faces_.contains(face.id)) next_faces.insert(face.id);
            }
        }
    }

    if (mode_ == SelectionMode::vertex) {
        const auto previous_order = vertex_selection_order_;
        const std::set<geometry::VertexId> previously_selected(
            previous_order.begin(), previous_order.end());
        vertices_ = std::move(next_vertices);
        vertex_selection_order_.clear();
        for (const auto vertex : previous_order) {
            if (vertices_.contains(vertex)) vertex_selection_order_.push_back(vertex);
        }
        for (const auto vertex : vertices_) {
            if (!previously_selected.contains(vertex)) {
                vertex_selection_order_.push_back(vertex);
            }
        }
        edges_.clear();
        faces_.clear();
    } else if (mode_ == SelectionMode::edge) {
        vertices_.clear();
        vertex_selection_order_.clear();
        edges_ = std::move(next_edges);
        faces_.clear();
    } else {
        vertices_.clear();
        vertex_selection_order_.clear();
        edges_.clear();
        faces_ = std::move(next_faces);
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::expand(
    SelectionExpansion expansion,
    const scene::Scene& scene,
    const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes) {
    if (!valid_expansion(expansion)) {
        return core::Result<void>::failure(invalid("selection expansion is invalid"));
    }
    if (mode_ == SelectionMode::object) {
        return core::Result<void>::failure(invalid(
            "object selection cannot use component expansion"));
    }
    if ((expansion == SelectionExpansion::loop ||
         expansion == SelectionExpansion::ring ||
         expansion == SelectionExpansion::boundary) && mode_ != SelectionMode::edge) {
        return core::Result<void>::failure(invalid(
            "loop, ring, and boundary expansion require edge selection mode"));
    }
    if (auto result = validate(scene, meshes); !result) return result;

    const auto groups = component_groups();
    std::vector<ComponentSelectionGroup> expanded_groups;
    expanded_groups.reserve(groups.size());
    for (const auto& group : groups) {
        const auto mesh_entry = meshes.find(group.object);
        if (mesh_entry == meshes.end() || mesh_entry->second == nullptr) {
            return core::Result<void>::failure(missing(
                "selection expansion is missing an owner mesh context"));
        }
        SelectionState expanded;
        expanded.mode_ = mode_;
        expanded.component_object_ = group.object;
        expanded.component_mesh_ = mesh_entry->second;
        expanded.component_mesh_revision_ = group.mesh_revision;
        expanded.vertices_ = std::set<geometry::VertexId>(
            group.vertices.begin(), group.vertices.end());
        expanded.vertex_selection_order_ = group.vertex_selection_order;
        expanded.edges_ = std::set<geometry::EdgeId>(group.edges.begin(), group.edges.end());
        expanded.faces_ = std::set<geometry::FaceId>(group.faces.begin(), group.faces.end());
        if (auto result = expanded.expand(expansion, *mesh_entry->second); !result) {
            return result;
        }
        expanded_groups.push_back(expanded.active_component_group());
    }

    const std::optional<scene::ObjectId> primary_object = component_object_.has_value()
        ? component_object_
        : (expanded_groups.empty() ? std::nullopt
                                   : std::optional<scene::ObjectId>(expanded_groups.front().object));
    vertices_.clear();
    vertex_selection_order_.clear();
    edges_.clear();
    faces_.clear();
    component_object_.reset();
    component_mesh_ = nullptr;
    component_mesh_revision_.reset();
    other_component_selections_.clear();
    other_component_meshes_.clear();
    if (primary_object.has_value()) {
        const auto primary_group = std::find_if(
            expanded_groups.begin(), expanded_groups.end(),
            [&primary_object](const ComponentSelectionGroup& group) {
                return group.object == *primary_object;
            });
        if (primary_group != expanded_groups.end()) {
            load_component_group(*primary_group, *meshes.at(primary_group->object));
        }
    }
    for (const auto& group : expanded_groups) {
        if (component_object_.has_value() && group.object == *component_object_) continue;
        other_component_selections_.emplace(group.object, group);
        other_component_meshes_.emplace(group.object, meshes.at(group.object));
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_shortest_path(
    geometry::VertexId goal,
    const geometry::EditableMesh& mesh) {
    if (!other_component_selections_.empty()) {
        return core::Result<void>::failure(invalid(
            "multi-object shortest path requires an explicit owner group"));
    }
    if (mode_ != SelectionMode::vertex) {
        return core::Result<void>::failure(invalid(
            "shortest-path selection requires vertex selection mode"));
    }
    if (auto result = validate(mesh); !result) {
        return result;
    }
    if (vertices_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "shortest-path selection requires one selected start vertex"));
    }
    if (vertices_.size() != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "shortest-path selection requires exactly one selected start vertex"));
    }
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<void>::failure(topology.error().with_context(
            "shortest-path selection topology"));
    }
    const auto path = topology.value().shortest_path(*vertices_.begin(), goal);
    if (!path) {
        return core::Result<void>::failure(path.error());
    }
    vertices_ = std::set<geometry::VertexId>(path.value().begin(), path.value().end());
    vertex_selection_order_ = path.value();
    return core::Result<void>::success();
}

core::Result<void> SelectionState::select_shortest_path(
    scene::ObjectId owner,
    geometry::VertexId goal,
    const scene::Scene& scene,
    const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes) {
    if (mode_ != SelectionMode::vertex || !owner) {
        return core::Result<void>::failure(invalid(
            "owner-scoped shortest path requires vertex mode and a valid object"));
    }
    if (auto result = validate(scene, meshes); !result) return result;

    const auto mesh = meshes.find(owner);
    if (mesh == meshes.end() || mesh->second == nullptr) {
        return core::Result<void>::failure(missing(
            "shortest-path owner has no current mesh context"));
    }
    const auto groups = component_groups();
    const auto owner_group = std::find_if(groups.begin(), groups.end(),
        [owner](const ComponentSelectionGroup& candidate) { return candidate.object == owner; });
    if (owner_group == groups.end()) {
        return core::Result<void>::failure(invalid(
            "shortest-path owner has no component selection"));
    }

    SelectionState isolated;
    isolated.mode_ = mode_;
    isolated.load_component_group(*owner_group, *mesh->second);
    if (auto result = isolated.select_shortest_path(goal, *mesh->second); !result) return result;
    const auto updated = isolated.active_component_group();
    if (component_object_.has_value() && *component_object_ == owner) {
        load_component_group(updated, *mesh->second);
    } else {
        const auto stored = other_component_selections_.find(owner);
        if (stored == other_component_selections_.end()) {
            return core::Result<void>::failure(invalid(
                "shortest-path owner selection storage is inconsistent"));
        }
        stored->second = updated;
    }
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
        vertex_selection_order_.clear();
    }
    if (operation == SelectionOperation::toggle && vertices_.contains(vertex)) {
        vertices_.erase(vertex);
        const auto selected = std::find(
            vertex_selection_order_.begin(), vertex_selection_order_.end(), vertex);
        if (selected != vertex_selection_order_.end()) vertex_selection_order_.erase(selected);
    } else {
        vertices_.insert(vertex);
        const auto selected = std::find(
            vertex_selection_order_.begin(), vertex_selection_order_.end(), vertex);
        if (selected != vertex_selection_order_.end()) vertex_selection_order_.erase(selected);
        vertex_selection_order_.push_back(vertex);
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
        vertex_selection_order_.clear();
    }
    if (operation == SelectionOperation::toggle && vertices_.contains(vertex)) {
        vertices_.erase(vertex);
        const auto selected = std::find(
            vertex_selection_order_.begin(), vertex_selection_order_.end(), vertex);
        if (selected != vertex_selection_order_.end()) vertex_selection_order_.erase(selected);
    } else {
        vertices_.insert(vertex);
        const auto selected = std::find(
            vertex_selection_order_.begin(), vertex_selection_order_.end(), vertex);
        if (selected != vertex_selection_order_.end()) vertex_selection_order_.erase(selected);
        vertex_selection_order_.push_back(vertex);
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
    if (!other_component_selections_.empty()) {
        return core::Result<void>::failure(invalid(
            "multi-object component selection requires every owner mesh context"));
    }
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

core::Result<void> SelectionState::validate(
    const scene::Scene& scene,
    const std::map<scene::ObjectId, const geometry::EditableMesh*>& meshes) const {
    if (mode_ == SelectionMode::object) return validate(scene, nullptr);
    const auto groups = component_groups();
    if (groups.empty()) {
        return active_count() == 0U
            ? core::Result<void>::success()
            : core::Result<void>::failure(invalid(
                "unbound component selection has no scene owner"));
    }
    for (const auto& group : groups) {
        if (!scene.find(group.object)) {
            return core::Result<void>::failure(stale(
                "component selection owner is no longer present in the scene"));
        }
        const auto mesh_entry = meshes.find(group.object);
        if (mesh_entry == meshes.end() || mesh_entry->second == nullptr) {
            return core::Result<void>::failure(missing(
                "component selection owner has no current mesh context"));
        }
        const geometry::EditableMesh* bound_mesh = nullptr;
        if (component_object_.has_value() && group.object == *component_object_) {
            bound_mesh = component_mesh_;
        } else if (const auto stored = other_component_meshes_.find(group.object);
                   stored != other_component_meshes_.end()) {
            bound_mesh = stored->second;
        }
        if (bound_mesh != nullptr && bound_mesh != mesh_entry->second) {
            return core::Result<void>::failure(stale(
                "component selection owner now resolves to a different mesh instance"));
        }
        if (group.mesh_revision != mesh_entry->second->revision()) {
            return core::Result<void>::failure(stale(
                "component selection was created against an older owner mesh revision"));
        }
        SelectionState isolated;
        isolated.mode_ = mode_;
        isolated.component_object_ = group.object;
        isolated.component_mesh_ = mesh_entry->second;
        isolated.component_mesh_revision_ = group.mesh_revision;
        isolated.vertices_ = std::set<geometry::VertexId>(
            group.vertices.begin(), group.vertices.end());
        isolated.vertex_selection_order_ = group.vertex_selection_order;
        isolated.edges_ = std::set<geometry::EdgeId>(group.edges.begin(), group.edges.end());
        isolated.faces_ = std::set<geometry::FaceId>(group.faces.begin(), group.faces.end());
        if (auto result = isolated.validate(*mesh_entry->second); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionState::validate(const geometry::EditableMesh& mesh) const {
    if (!other_component_selections_.empty()) {
        return core::Result<void>::failure(invalid(
            "multi-object component selection cannot be validated against one mesh"));
    }
    switch (mode_) {
    case SelectionMode::object:
        return core::Result<void>::success();
    case SelectionMode::vertex: {
        if (vertex_selection_order_.size() != vertices_.size()) {
            return core::Result<void>::failure(invalid(
                "vertex selection order does not match selected vertex identities"));
        }
        std::set<geometry::VertexId> ordered_vertices;
        for (const auto vertex : vertex_selection_order_) {
            if (!vertices_.contains(vertex) || !ordered_vertices.insert(vertex).second) {
                return core::Result<void>::failure(invalid(
                    "vertex selection order contains a missing or duplicate identity"));
            }
        }
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
    }
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

ComponentSelectionGroup SelectionState::active_component_group() const {
    ComponentSelectionGroup group;
    if (component_object_.has_value()) group.object = *component_object_;
    if (component_mesh_revision_.has_value()) group.mesh_revision = *component_mesh_revision_;
    group.vertices.assign(vertices_.begin(), vertices_.end());
    group.vertex_selection_order = vertex_selection_order_;
    group.edges.assign(edges_.begin(), edges_.end());
    group.faces.assign(faces_.begin(), faces_.end());
    return group;
}

void SelectionState::load_component_group(
    const ComponentSelectionGroup& group,
    const geometry::EditableMesh& mesh) {
    component_object_ = group.object;
    component_mesh_ = &mesh;
    component_mesh_revision_ = group.mesh_revision;
    vertices_ = std::set<geometry::VertexId>(group.vertices.begin(), group.vertices.end());
    vertex_selection_order_ = group.vertex_selection_order;
    edges_ = std::set<geometry::EdgeId>(group.edges.begin(), group.edges.end());
    faces_ = std::set<geometry::FaceId>(group.faces.begin(), group.faces.end());
}

std::vector<ComponentSelectionGroup> SelectionState::component_groups() const {
    std::vector<ComponentSelectionGroup> result;
    if (mode_ == SelectionMode::object) return result;
    result.reserve(other_component_selections_.size() +
        static_cast<std::size_t>(component_object_.has_value()));
    for (const auto& [object, group] : other_component_selections_) {
        static_cast<void>(object);
        result.push_back(group);
    }
    if (component_object_.has_value()) result.push_back(active_component_group());
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.object < right.object;
    });
    return result;
}

std::vector<geometry::VertexId> SelectionState::selected_vertices() const {
    return {vertices_.begin(), vertices_.end()};
}

std::vector<geometry::VertexId> SelectionState::selected_vertices_in_order() const {
    return vertex_selection_order_;
}

std::optional<geometry::VertexId> SelectionState::last_selected_vertex() const noexcept {
    if (vertex_selection_order_.empty()) return std::nullopt;
    return vertex_selection_order_.back();
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
    vertex_selection_order_.clear();
    edges_.clear();
    faces_.clear();
    component_object_.reset();
    component_mesh_ = nullptr;
    component_mesh_revision_.reset();
    other_component_selections_.clear();
    other_component_meshes_.clear();
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
    }

    const auto active_count_for_mode = [this] {
        switch (mode_) {
        case SelectionMode::object: return std::size_t{0U};
        case SelectionMode::vertex: return vertices_.size();
        case SelectionMode::edge: return edges_.size();
        case SelectionMode::face: return faces_.size();
        }
        return std::size_t{0U};
    };

    if (operation == SelectionOperation::replace) {
        other_component_selections_.clear();
        other_component_meshes_.clear();
        component_object_ = object;
        component_mesh_ = &mesh;
        component_mesh_revision_ = mesh.revision();
        return core::Result<void>::success();
    }

    if (!object.has_value()) {
        if (component_object_.has_value() || !other_component_selections_.empty()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "component selection requires an owning scene object"));
        }
        const bool stale_mesh_context = component_mesh_ &&
            (component_mesh_ != &mesh || component_mesh_revision_ != mesh.revision());
        if (stale_mesh_context) {
            return core::Result<void>::failure(stale(
                "component selection belongs to an older or different mesh context"));
        }
        if (!component_mesh_) {
            component_mesh_ = &mesh;
            component_mesh_revision_ = mesh.revision();
        }
        return core::Result<void>::success();
    }

    if (!component_object_.has_value()) {
        if (active_count_for_mode() != 0U) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "unbound component selection requires replace before object binding"));
        }
        component_object_ = object;
        component_mesh_ = &mesh;
        component_mesh_revision_ = mesh.revision();
        return core::Result<void>::success();
    }

    if (*component_object_ == *object) {
        const bool stale_mesh_context = component_mesh_ &&
            (component_mesh_ != &mesh || component_mesh_revision_ != mesh.revision());
        if (stale_mesh_context) {
            return core::Result<void>::failure(stale(
                "component selection belongs to an older or different mesh context"));
        }
        return core::Result<void>::success();
    }

    const auto target_group = other_component_selections_.find(*object);
    if (target_group != other_component_selections_.end()) {
        const auto target_mesh = other_component_meshes_.find(*object);
        if (target_mesh == other_component_meshes_.end() || target_mesh->second != &mesh ||
            target_group->second.mesh_revision != mesh.revision()) {
            return core::Result<void>::failure(stale(
                "component selection belongs to an older or different owner mesh context"));
        }
    }

    const scene::ObjectId old_owner = *component_object_;
    if (active_count_for_mode() != 0U) {
        other_component_selections_[old_owner] = active_component_group();
        other_component_meshes_[old_owner] = component_mesh_;
    }

    if (target_group != other_component_selections_.end()) {
        const ComponentSelectionGroup group = target_group->second;
        load_component_group(group, mesh);
        other_component_selections_.erase(*object);
        other_component_meshes_.erase(*object);
    } else {
        vertices_.clear();
        vertex_selection_order_.clear();
        edges_.clear();
        faces_.clear();
        component_object_ = object;
        component_mesh_ = &mesh;
        component_mesh_revision_ = mesh.revision();
    }
    return core::Result<void>::success();
}

} // namespace carto::editor
