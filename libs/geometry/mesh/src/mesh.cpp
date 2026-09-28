#include <carto/geometry/mesh.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace carto::geometry {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

struct EdgeKey {
    VertexId first;
    VertexId second;

    [[nodiscard]] constexpr auto operator<=>(const EdgeKey&) const noexcept = default;
};

EdgeKey undirected(VertexId left, VertexId right) {
    return left < right ? EdgeKey{left, right} : EdgeKey{right, left};
}

double triangle_area_squared(core::Vec3d a, core::Vec3d b, core::Vec3d c) {
    return core::cross(b - a, c - a).length_squared() * 0.25;
}

} // namespace

core::Result<VertexId> EditableMesh::add_vertex(core::Vec3d position) {
    if (!position.finite()) {
        return core::Result<VertexId>::failure(invalid("mesh vertex position must be finite"));
    }
    if (next_vertex_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<VertexId>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh vertex id space is exhausted"));
    }
    const VertexId id{next_vertex_id_++};
    vertices_.emplace(id, Vertex{id, position});
    bump_revision();
    return core::Result<VertexId>::success(id);
}

core::Result<void> EditableMesh::insert_vertex(Vertex vertex) {
    if (!vertex.id || !vertex.position.finite()) {
        return core::Result<void>::failure(
            invalid("inserted mesh vertex requires a non-zero id and finite position"));
    }
    if (vertices_.contains(vertex.id)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "duplicate mesh vertex id"));
    }
    const VertexId inserted_id = vertex.id;
    vertices_.emplace(inserted_id, std::move(vertex));
    if (inserted_id.value < std::numeric_limits<std::uint64_t>::max()) {
        next_vertex_id_ = std::max(next_vertex_id_, inserted_id.value + 1U);
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::set_vertex_position(VertexId id, core::Vec3d position) {
    if (!position.finite()) {
        return core::Result<void>::failure(invalid("mesh vertex position must be finite"));
    }
    auto iterator = vertices_.find(id);
    if (iterator == vertices_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot move a missing mesh vertex"));
    }
    const core::Vec3d before = iterator->second.position;
    iterator->second.position = position;
    if (auto result = validate(); !result) {
        iterator->second.position = before;
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::extrude_face(FaceId id, double distance) {
    if (!std::isfinite(distance) || distance <= 0.0) {
        return core::Result<void>::failure(
            invalid("face extrusion distance must be finite and strictly positive"));
    }
    const auto face_iterator = faces_.find(id);
    if (face_iterator == faces_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot extrude a missing mesh face"));
    }
    const Face original = face_iterator->second;
    if (original.vertices.size() < 3U) {
        return core::Result<void>::failure(validation("cannot extrude a face with fewer than three vertices"));
    }

    const core::Vec3d first = vertices_.at(original.vertices[0]).position;
    const core::Vec3d second = vertices_.at(original.vertices[1]).position;
    const core::Vec3d third = vertices_.at(original.vertices[2]).position;
    const core::Vec3d normal = core::cross(second - first, third - first).normalized();
    if (normal.length_squared() <= 1e-24) {
        return core::Result<void>::failure(validation("cannot extrude a zero-area face"));
    }

    const EditableMesh before = *this;
    faces_.erase(face_iterator);

    std::vector<VertexId> extruded_vertices;
    extruded_vertices.reserve(original.vertices.size());
    for (const VertexId vertex : original.vertices) {
        auto added = add_vertex(vertices_.at(vertex).position + normal * distance);
        if (!added) {
            *this = before;
            return core::Result<void>::failure(added.error().with_context("extruded vertex"));
        }
        extruded_vertices.push_back(added.value());
    }

    for (std::size_t index = 0U; index < original.vertices.size(); ++index) {
        const std::size_t next = (index + 1U) % original.vertices.size();
        auto side = add_face({
            original.vertices[index],
            original.vertices[next],
            extruded_vertices[next],
            extruded_vertices[index],
        });
        if (!side) {
            *this = before;
            return core::Result<void>::failure(side.error().with_context("extruded side face"));
        }
    }
    auto cap = add_face(extruded_vertices);
    if (!cap) {
        *this = before;
        return core::Result<void>::failure(cap.error().with_context("extruded cap face"));
    }
    if (auto result = validate(); !result) {
        *this = before;
        return result;
    }
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::restore_from(const EditableMesh& source) {
    if (auto result = source.validate(); !result) {
        return result;
    }
    vertices_ = source.vertices_;
    faces_ = source.faces_;
    next_vertex_id_ = source.next_vertex_id_;
    next_face_id_ = source.next_face_id_;
    bump_revision();
    return core::Result<void>::success();
}

core::Result<FaceId> EditableMesh::add_face(std::vector<VertexId> vertices) {
    if (vertices.size() < 3U) {
        return core::Result<FaceId>::failure(
            invalid("mesh face requires at least three vertices"));
    }
    if (next_face_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<FaceId>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh face id space is exhausted"));
    }
    std::set<VertexId> unique_vertices;
    for (VertexId vertex : vertices) {
        if (!vertex || !vertices_.contains(vertex)) {
            return core::Result<FaceId>::failure(
                Diagnostic(ErrorCode::not_found, "mesh face references a missing vertex"));
        }
        if (!unique_vertices.insert(vertex).second) {
            return core::Result<FaceId>::failure(
                validation("mesh face contains a duplicate vertex"));
        }
    }

    const FaceId id{next_face_id_++};
    faces_.emplace(id, Face{id, std::move(vertices)});
    if (auto result = validate(); !result) {
        faces_.erase(id);
        --next_face_id_;
        return core::Result<FaceId>::failure(result.error());
    }
    bump_revision();
    return core::Result<FaceId>::success(id);
}

core::Result<void> EditableMesh::insert_face(Face face) {
    if (!face.id || face.vertices.size() < 3U) {
        return core::Result<void>::failure(
            invalid("inserted mesh face requires a non-zero id and at least three vertices"));
    }
    if (faces_.contains(face.id)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "duplicate mesh face id"));
    }
    const FaceId inserted_id = face.id;
    faces_.emplace(inserted_id, std::move(face));
    if (inserted_id.value < std::numeric_limits<std::uint64_t>::max()) {
        next_face_id_ = std::max(next_face_id_, inserted_id.value + 1U);
    }
    if (auto result = validate(); !result) {
        faces_.erase(inserted_id);
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

const Vertex* EditableMesh::find_vertex(VertexId id) const noexcept {
    const auto iterator = vertices_.find(id);
    return iterator == vertices_.end() ? nullptr : &iterator->second;
}

const Face* EditableMesh::find_face(FaceId id) const noexcept {
    const auto iterator = faces_.find(id);
    return iterator == faces_.end() ? nullptr : &iterator->second;
}

std::vector<Vertex> EditableMesh::vertices_sorted() const {
    std::vector<Vertex> result;
    result.reserve(vertices_.size());
    for (const auto& [id, vertex] : vertices_) {
        static_cast<void>(id);
        result.push_back(vertex);
    }
    return result;
}

std::vector<Face> EditableMesh::faces_sorted() const {
    std::vector<Face> result;
    result.reserve(faces_.size());
    for (const auto& [id, face] : faces_) {
        static_cast<void>(id);
        result.push_back(face);
    }
    return result;
}

core::Result<void> EditableMesh::validate() const {
    std::map<EdgeKey, std::size_t> edge_use;
    std::map<std::pair<VertexId, VertexId>, FaceId> directed_edges;

    for (const auto& [face_id, face] : faces_) {
        if (!face_id || face.id != face_id || face.vertices.size() < 3U) {
            return core::Result<void>::failure(validation("mesh contains an invalid face record"));
        }
        std::set<VertexId> unique_vertices;
        for (VertexId vertex : face.vertices) {
            if (!vertex || !vertices_.contains(vertex) || !unique_vertices.insert(vertex).second) {
                return core::Result<void>::failure(
                    validation("mesh face has a dangling or duplicate vertex"));
            }
        }

        const VertexId first = face.vertices.front();
        const core::Vec3d first_position = vertices_.at(first).position;
        for (std::size_t index = 1U; index + 1U < face.vertices.size(); ++index) {
            const core::Vec3d second_position = vertices_.at(face.vertices[index]).position;
            const core::Vec3d third_position = vertices_.at(face.vertices[index + 1U]).position;
            if (triangle_area_squared(first_position, second_position, third_position) <= 1e-24) {
                return core::Result<void>::failure(validation("mesh face contains a zero-area triangle"));
            }
        }

        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const VertexId origin = face.vertices[index];
            const VertexId destination = face.vertices[(index + 1U) % face.vertices.size()];
            const EdgeKey edge = undirected(origin, destination);
            const std::size_t count = ++edge_use[edge];
            if (count > 2U) {
                return core::Result<void>::failure(
                    validation("mesh edge is incident to more than two faces"));
            }
            if (!directed_edges.emplace(std::make_pair(origin, destination), face_id).second) {
                return core::Result<void>::failure(
                    validation("mesh contains a duplicate directed edge"));
            }
        }
    }
    for (const auto& [id, vertex] : vertices_) {
        if (!id || vertex.id != id || !vertex.position.finite()) {
            return core::Result<void>::failure(validation("mesh contains an invalid vertex record"));
        }
    }
    return core::Result<void>::success();
}

core::Result<TopologySnapshot> EditableMesh::topology() const {
    if (auto result = validate(); !result) {
        return core::Result<TopologySnapshot>::failure(result.error());
    }

    TopologySnapshot snapshot;
    std::map<std::pair<VertexId, VertexId>, HalfEdgeId> directed;
    std::uint64_t next_half_edge = 1;

    for (const auto& [face_id, face] : faces_) {
        std::vector<HalfEdgeId> face_edges;
        face_edges.reserve(face.vertices.size());
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const VertexId origin = face.vertices[index];
            const VertexId destination = face.vertices[(index + 1U) % face.vertices.size()];
            const HalfEdgeId id{next_half_edge++};
            snapshot.half_edges.push_back(
                HalfEdgeRecord{id, origin, destination, face_id, {}, {}, std::nullopt});
            face_edges.push_back(id);
            directed.emplace(std::make_pair(origin, destination), id);
        }
        for (std::size_t index = 0U; index < face_edges.size(); ++index) {
            auto& edge = snapshot.half_edges.at(static_cast<std::size_t>(face_edges[index].value - 1U));
            edge.next = face_edges[(index + 1U) % face_edges.size()];
            edge.previous = face_edges[(index + face_edges.size() - 1U) % face_edges.size()];
        }
    }

    for (auto& edge : snapshot.half_edges) {
        const auto twin_iterator = directed.find(std::make_pair(edge.destination, edge.origin));
        if (twin_iterator != directed.end()) {
            edge.twin = twin_iterator->second;
        }
    }
    if (auto result = snapshot.validate(); !result) {
        return core::Result<TopologySnapshot>::failure(result.error());
    }
    return core::Result<TopologySnapshot>::success(std::move(snapshot));
}

core::Result<void> TopologySnapshot::validate() const {
    std::map<HalfEdgeId, const HalfEdgeRecord*> by_id;
    for (const auto& edge : half_edges) {
        if (!edge.id || !edge.origin || !edge.destination || !edge.face || !edge.next ||
            !edge.previous || edge.origin == edge.destination) {
            return core::Result<void>::failure(
                validation("topology contains a malformed half-edge"));
        }
        if (!by_id.emplace(edge.id, &edge).second) {
            return core::Result<void>::failure(validation("topology contains a duplicate half-edge id"));
        }
    }
    for (const auto& edge : half_edges) {
        const auto next = by_id.find(edge.next);
        const auto previous = by_id.find(edge.previous);
        if (next == by_id.end() || previous == by_id.end() || next->second->previous != edge.id ||
            previous->second->next != edge.id || next->second->face != edge.face ||
            previous->second->face != edge.face || next->second->origin != edge.destination ||
            previous->second->destination != edge.origin) {
            return core::Result<void>::failure(
                validation("topology next/previous links are not symmetric or contiguous"));
        }
        if (edge.twin.has_value()) {
            const auto twin = by_id.find(*edge.twin);
            if (twin == by_id.end() || !twin->second->twin.has_value() ||
                *twin->second->twin != edge.id || twin->second->origin != edge.destination ||
                twin->second->destination != edge.origin) {
                return core::Result<void>::failure(
                    validation("topology twin links are not symmetric"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<CompiledMesh> EditableMesh::compile() const {
    if (auto result = validate(); !result) {
        return core::Result<CompiledMesh>::failure(result.error());
    }

    CompiledMesh compiled;
    compiled.source_revision = revision_;
    std::map<VertexId, std::uint32_t> indices;
    for (const auto& [id, vertex] : vertices_) {
        indices.emplace(id, static_cast<std::uint32_t>(compiled.positions.size()));
        compiled.positions.push_back(vertex.position);
        compiled.normals.push_back({});
        compiled.bounds.include(vertex.position);
    }

    for (const auto& [face_id, face] : faces_) {
        static_cast<void>(face_id);
        const std::uint32_t first = indices.at(face.vertices.front());
        for (std::size_t index = 1U; index + 1U < face.vertices.size(); ++index) {
            const std::uint32_t second = indices.at(face.vertices[index]);
            const std::uint32_t third = indices.at(face.vertices[index + 1U]);
            compiled.indices.insert(compiled.indices.end(), {first, second, third});
            const core::Vec3d normal = core::cross(
                compiled.positions[second] - compiled.positions[first],
                compiled.positions[third] - compiled.positions[first]);
            compiled.normals[first] = compiled.normals[first] + normal;
            compiled.normals[second] = compiled.normals[second] + normal;
            compiled.normals[third] = compiled.normals[third] + normal;
        }
    }
    for (auto& normal : compiled.normals) {
        normal = normal.normalized();
    }
    if (!compiled.valid()) {
        return core::Result<CompiledMesh>::failure(
            validation("compiled mesh failed derived-data validation"));
    }
    return core::Result<CompiledMesh>::success(std::move(compiled));
}

} // namespace carto::geometry
