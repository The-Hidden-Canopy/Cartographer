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

Diagnostic exhausted_revision() {
    return Diagnostic(ErrorCode::invalid_state, "mesh revision space is exhausted");
}

struct EdgeKey {
    VertexId first;
    VertexId second;

    [[nodiscard]] constexpr auto operator<=>(const EdgeKey&) const noexcept = default;
};

struct HalfEdgeKey {
    FaceId face;
    VertexId origin;
    VertexId destination;

    [[nodiscard]] constexpr auto operator<=>(const HalfEdgeKey&) const noexcept = default;
};

struct CornerKey {
    FaceId face;
    VertexId vertex;

    [[nodiscard]] constexpr auto operator<=>(const CornerKey&) const noexcept = default;
};

EdgeKey undirected(VertexId left, VertexId right) {
    return left < right ? EdgeKey{left, right} : EdgeKey{right, left};
}

double triangle_area_squared(core::Vec3d a, core::Vec3d b, core::Vec3d c) {
    return core::cross(b - a, c - a).length_squared() * 0.25;
}

bool same_position(core::Vec3d left, core::Vec3d right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

} // namespace

core::Result<void> MeshPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "mesh patch revision space is exhausted"));
    }
    if (vertex_changes.empty()) {
        return core::Result<void>::failure(
            invalid("mesh patch must contain at least one vertex change"));
    }
    std::set<VertexId> changed_vertices;
    for (const auto& change : vertex_changes) {
        if (!change.id || !change.before.finite() || !change.after.finite()) {
            return core::Result<void>::failure(
                invalid("mesh patch vertex changes must contain finite values"));
        }
        if (!changed_vertices.insert(change.id).second) {
            return core::Result<void>::failure(
                validation("mesh patch contains duplicate vertex changes"));
        }
    }
    return core::Result<void>::success();
}

MeshPatch MeshPatch::inverse(core::Revision target_revision) const {
    MeshPatch result;
    result.expected_revision = target_revision;
    result.vertex_changes.reserve(vertex_changes.size());
    for (const auto& change : vertex_changes) {
        result.vertex_changes.push_back(VertexChange{change.id, change.after, change.before});
    }
    return result;
}

core::Result<VertexId> EditableMesh::add_vertex(core::Vec3d position) {
    if (revision_.exhausted()) {
        return core::Result<VertexId>::failure(exhausted_revision());
    }
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    const auto* vertex = find_vertex(id);
    if (!vertex) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot move a missing mesh vertex"));
    }
    return apply_patch(MeshPatch{
        revision_,
        {VertexChange{id, vertex->position, position}},
    });
}

core::Result<void> EditableMesh::apply_patch(const MeshPatch& patch) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = patch.validate(); !result) {
        return result;
    }
    if (patch.expected_revision != revision_) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current mesh"));
    }

    for (const auto& change : patch.vertex_changes) {
        const auto iterator = vertices_.find(change.id);
        if (iterator == vertices_.end()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::not_found, "mesh patch references a missing vertex"));
        }
        if (!same_position(iterator->second.position, change.before)) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data, "mesh patch vertex state does not match its before value"));
        }
    }

    std::vector<core::Vec3d> previous_positions;
    previous_positions.reserve(patch.vertex_changes.size());
    for (const auto& change : patch.vertex_changes) {
        previous_positions.push_back(vertices_.at(change.id).position);
        vertices_.at(change.id).position = change.after;
    }
    if (auto result = validate(); !result) {
        for (std::size_t index = 0U; index < patch.vertex_changes.size(); ++index) {
            vertices_.at(patch.vertex_changes[index].id).position = previous_positions[index];
        }
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::extrude_face(FaceId id, double distance) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (!normal.finite() || normal.length_squared() <= 1e-24) {
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

core::Result<TopologyEditReceipt> EditableMesh::delete_face(
    FaceId id,
    bool remove_orphaned_vertices) {
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    const auto face_iterator = faces_.find(id);
    if (face_iterator == faces_.end()) {
        return core::Result<TopologyEditReceipt>::failure(
            Diagnostic(ErrorCode::not_found, "cannot delete a missing mesh face"));
    }

    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
    const Face deleted = face_iterator->second;
    faces_.erase(face_iterator);

    std::vector<VertexId> removed_vertices;
    if (remove_orphaned_vertices) {
        std::set<VertexId> used_vertices;
        for (const auto& [face_id, face] : faces_) {
            static_cast<void>(face_id);
            used_vertices.insert(face.vertices.begin(), face.vertices.end());
        }
        for (const VertexId vertex : deleted.vertices) {
            if (!used_vertices.contains(vertex)) {
                vertices_.erase(vertex);
                removed_vertices.push_back(vertex);
            }
        }
    }

    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    bump_revision();
    return core::Result<TopologyEditReceipt>::success(TopologyEditReceipt{
        revision_before,
        revision_,
        {},
        {},
        {},
        std::move(removed_vertices),
        {},
        {id},
    });
}

core::Result<TopologyEditReceipt> EditableMesh::split_edge(EdgeId id, double factor) {
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!std::isfinite(factor) || factor <= 0.0 || factor >= 1.0) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "edge split factor must be finite and strictly between zero and one"));
    }
    if (auto result = validate(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    const auto edge_iterator = edges_.find(id);
    if (edge_iterator == edges_.end()) {
        return core::Result<TopologyEditReceipt>::failure(
            Diagnostic(ErrorCode::not_found, "cannot split a missing mesh edge"));
    }
    if (next_vertex_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "mesh vertex id space is exhausted"));
    }

    const EditableMesh before = *this;
    const TopologySnapshot before_topology = topology_snapshot();
    const core::Revision revision_before = revision_;
    const EdgeRecord edge = edge_iterator->second;
    const auto first = vertices_.find(edge.first);
    const auto second = vertices_.find(edge.second);
    if (first == vertices_.end() || second == vertices_.end()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh edge references a missing endpoint"));
    }

    const VertexId created_vertex{next_vertex_id_++};
    vertices_.emplace(
        created_vertex,
        Vertex{created_vertex, first->second.position * (1.0 - factor) +
            second->second.position * factor});
    std::size_t incident_faces = 0U;
    for (auto& [face_id, face] : faces_) {
        static_cast<void>(face_id);
        std::vector<VertexId> split_vertices;
        split_vertices.reserve(face.vertices.size() + 1U);
        bool split = false;
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const VertexId origin = face.vertices[index];
            const VertexId destination = face.vertices[(index + 1U) % face.vertices.size()];
            split_vertices.push_back(origin);
            if ((origin == edge.first && destination == edge.second) ||
                (origin == edge.second && destination == edge.first)) {
                if (split) {
                    *this = before;
                    return core::Result<TopologyEditReceipt>::failure(validation(
                        "mesh edge occurs more than once in one face"));
                }
                split_vertices.push_back(created_vertex);
                split = true;
            }
        }
        if (split) {
            const auto anchor = std::find_if(
                split_vertices.begin(), split_vertices.end(),
                [edge, created_vertex](VertexId vertex) {
                    return vertex != edge.first && vertex != edge.second &&
                        vertex != created_vertex;
                });
            if (anchor == split_vertices.end()) {
                *this = before;
                return core::Result<TopologyEditReceipt>::failure(validation(
                    "mesh edge split has no non-collinear face anchor"));
            }
            // The compiled mesh currently uses a fan triangulation. Rotate the
            // authored boundary so the fan anchor is not one of the collinear
            // endpoints/midpoint introduced by this edit.
            std::rotate(split_vertices.begin(), anchor, split_vertices.end());
            ++incident_faces;
            face.vertices = std::move(split_vertices);
        }
    }
    if (incident_faces == 0U) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh edge has no authored incident face"));
    }

    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    const TopologySnapshot after_topology = topology_snapshot();
    bump_revision();

    std::set<EdgeId> before_edges;
    for (const auto& edge_record : before_topology.edges) before_edges.insert(edge_record.id);
    std::set<EdgeId> after_edges;
    for (const auto& edge_record : after_topology.edges) after_edges.insert(edge_record.id);
    std::vector<EdgeId> created_edges;
    std::vector<EdgeId> removed_edges;
    for (const EdgeId edge_id : after_edges) {
        if (!before_edges.contains(edge_id)) created_edges.push_back(edge_id);
    }
    for (const EdgeId edge_id : before_edges) {
        if (!after_edges.contains(edge_id)) removed_edges.push_back(edge_id);
    }
    return core::Result<TopologyEditReceipt>::success(TopologyEditReceipt{
        revision_before,
        revision_,
        {created_vertex},
        std::move(created_edges),
        {},
        {},
        std::move(removed_edges),
        {},
    });
}

core::Result<TopologyEditReceipt> EditableMesh::inset_face(FaceId id, double distance) {
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!std::isfinite(distance) || distance <= 0.0) {
        return core::Result<TopologyEditReceipt>::failure(
            invalid("face inset distance must be finite and strictly positive"));
    }
    if (auto result = validate(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    const auto face_iterator = faces_.find(id);
    if (face_iterator == faces_.end()) {
        return core::Result<TopologyEditReceipt>::failure(
            Diagnostic(ErrorCode::not_found, "cannot inset a missing mesh face"));
    }
    const Face original = face_iterator->second;
    const std::size_t vertex_count = original.vertices.size();
    if (vertex_count < 3U) {
        return core::Result<TopologyEditReceipt>::failure(
            validation("face inset requires at least three boundary vertices"));
    }

    std::vector<core::Vec3d> positions;
    positions.reserve(vertex_count);
    for (const VertexId vertex : original.vertices) {
        positions.push_back(vertices_.at(vertex).position);
    }
    const core::Vec3d normal = core::cross(
        positions[1U] - positions[0U], positions[2U] - positions[0U]).normalized();
    if (!normal.finite()) {
        return core::Result<TopologyEditReceipt>::failure(
            validation("face inset requires a non-degenerate planar face"));
    }

    double scale = 1.0;
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const core::Vec3d edge = positions[(index + 1U) % vertex_count] - positions[index];
        scale = std::max(scale, edge.length());
        const double plane_distance = core::dot(positions[index] - positions[0U], normal);
        if (!std::isfinite(plane_distance) || std::abs(plane_distance) > 1e-8 * scale) {
            return core::Result<TopologyEditReceipt>::failure(core::Diagnostic(
                ErrorCode::unsupported,
                "face inset currently requires a planar face"));
        }
    }
    const double area_epsilon = 1e-12 * scale * scale;
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const std::size_t next = (index + 1U) % vertex_count;
        const std::size_t after_next = (index + 2U) % vertex_count;
        const double turn = core::dot(
            core::cross(
                positions[next] - positions[index],
                positions[after_next] - positions[next]),
            normal);
        if (!std::isfinite(turn) || turn <= area_epsilon) {
            return core::Result<TopologyEditReceipt>::failure(core::Diagnostic(
                ErrorCode::unsupported,
                "face inset currently requires a strictly convex face"));
        }
    }

    std::vector<core::Vec3d> inward;
    std::vector<core::Vec3d> offset_starts;
    inward.reserve(vertex_count);
    offset_starts.reserve(vertex_count);
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const std::size_t next = (index + 1U) % vertex_count;
        const core::Vec3d edge = positions[next] - positions[index];
        const core::Vec3d direction = edge.normalized();
        const core::Vec3d inside = core::cross(normal, direction).normalized();
        if (!inside.finite()) {
            return core::Result<TopologyEditReceipt>::failure(
                validation("face inset found a degenerate boundary edge"));
        }
        inward.push_back(inside);
        offset_starts.push_back(positions[index] + inside * distance);
    }

    std::vector<core::Vec3d> inset_positions;
    inset_positions.reserve(vertex_count);
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const std::size_t previous = (index + vertex_count - 1U) % vertex_count;
        const core::Vec3d first_direction =
            positions[index] - positions[previous];
        const core::Vec3d second_direction =
            positions[(index + 1U) % vertex_count] - positions[index];
        const double denominator = core::dot(
            core::cross(first_direction, second_direction), normal);
        if (!std::isfinite(denominator) || std::abs(denominator) <= area_epsilon) {
            return core::Result<TopologyEditReceipt>::failure(core::Diagnostic(
                ErrorCode::unsupported,
                "face inset offset boundaries do not intersect uniquely"));
        }
        const core::Vec3d offset_delta = offset_starts[index] - offset_starts[previous];
        const double parameter = core::dot(
            core::cross(offset_delta, second_direction), normal) / denominator;
        const core::Vec3d inset = offset_starts[previous] + first_direction * parameter;
        if (!inset.finite()) {
            return core::Result<TopologyEditReceipt>::failure(
                validation("face inset produced a non-finite vertex"));
        }
        inset_positions.push_back(inset);
    }

    const double tolerance = 1e-8 * std::max(scale, distance);
    for (const auto& inset : inset_positions) {
        for (std::size_t index = 0U; index < vertex_count; ++index) {
            if (core::dot(inset - positions[index], inward[index]) < distance - tolerance) {
                return core::Result<TopologyEditReceipt>::failure(core::Diagnostic(
                    ErrorCode::invalid_argument,
                    "face inset distance leaves no valid inner face"));
            }
        }
    }
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const std::size_t next = (index + 1U) % vertex_count;
        const std::size_t after_next = (index + 2U) % vertex_count;
        const double turn = core::dot(
            core::cross(
                inset_positions[next] - inset_positions[index],
                inset_positions[after_next] - inset_positions[next]),
            normal);
        if (!std::isfinite(turn) || turn <= area_epsilon) {
            return core::Result<TopologyEditReceipt>::failure(core::Diagnostic(
                ErrorCode::invalid_argument,
                "face inset distance collapses or inverts the inner face"));
        }
    }

    const std::uint64_t new_vertex_count = static_cast<std::uint64_t>(vertex_count);
    const std::uint64_t new_face_count = new_vertex_count + 1U;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (next_vertex_id_ > maximum - new_vertex_count) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "mesh vertex id space is exhausted"));
    }
    if (next_face_id_ > maximum - new_face_count) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "mesh face id space is exhausted"));
    }

    const EditableMesh before = *this;
    const TopologySnapshot before_topology = topology_snapshot();
    const core::Revision revision_before = revision_;
    faces_.erase(face_iterator);

    std::vector<VertexId> inset_vertices;
    inset_vertices.reserve(vertex_count);
    for (const core::Vec3d position : inset_positions) {
        const VertexId vertex{next_vertex_id_++};
        vertices_.emplace(vertex, Vertex{vertex, position});
        inset_vertices.push_back(vertex);
    }

    std::vector<FaceId> created_faces;
    created_faces.reserve(static_cast<std::size_t>(new_face_count));
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const std::size_t next = (index + 1U) % vertex_count;
        const FaceId face{next_face_id_++};
        faces_.emplace(face, Face{
            face,
            {original.vertices[index], original.vertices[next],
             inset_vertices[next], inset_vertices[index]},
        });
        created_faces.push_back(face);
    }
    const FaceId inner_face{next_face_id_++};
    faces_.emplace(inner_face, Face{inner_face, inset_vertices});
    created_faces.push_back(inner_face);

    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    const TopologySnapshot after_topology = topology_snapshot();
    bump_revision();

    std::set<EdgeId> before_edges;
    for (const auto& edge : before_topology.edges) before_edges.insert(edge.id);
    std::set<EdgeId> after_edges;
    for (const auto& edge : after_topology.edges) after_edges.insert(edge.id);
    std::vector<EdgeId> created_edges;
    std::vector<EdgeId> removed_edges;
    for (const EdgeId edge : after_edges) {
        if (!before_edges.contains(edge)) created_edges.push_back(edge);
    }
    for (const EdgeId edge : before_edges) {
        if (!after_edges.contains(edge)) removed_edges.push_back(edge);
    }
    return core::Result<TopologyEditReceipt>::success(TopologyEditReceipt{
        revision_before,
        revision_,
        std::move(inset_vertices),
        std::move(created_edges),
        std::move(created_faces),
        {},
        std::move(removed_edges),
        {id},
    });
}

core::Result<void> EditableMesh::restore_from(const EditableMesh& source) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = source.validate(); !result) {
        return result;
    }
    vertices_ = source.vertices_;
    faces_ = source.faces_;
    edges_ = source.edges_;
    half_edges_ = source.half_edges_;
    corners_ = source.corners_;
    face_boundaries_ = source.face_boundaries_;
    next_vertex_id_ = source.next_vertex_id_;
    next_face_id_ = source.next_face_id_;
    next_edge_id_ = source.next_edge_id_;
    next_half_edge_id_ = source.next_half_edge_id_;
    next_corner_id_ = source.next_corner_id_;
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::restore_revision(core::Revision revision) {
    if (revision.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    compiled_cache_.reset();
    revision_ = revision;
    return core::Result<void>::success();
}

core::Result<FaceId> EditableMesh::add_face(std::vector<VertexId> vertices) {
    if (revision_.exhausted()) {
        return core::Result<FaceId>::failure(exhausted_revision());
    }
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

    const EditableMesh before = *this;
    const FaceId id{next_face_id_++};
    faces_.emplace(id, Face{id, std::move(vertices)});
    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<FaceId>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<FaceId>::failure(result.error());
    }
    bump_revision();
    return core::Result<FaceId>::success(id);
}

core::Result<void> EditableMesh::insert_face(Face face) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (!face.id || face.vertices.size() < 3U) {
        return core::Result<void>::failure(
            invalid("inserted mesh face requires a non-zero id and at least three vertices"));
    }
    if (faces_.contains(face.id)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "duplicate mesh face id"));
    }
    const EditableMesh before = *this;
    const FaceId inserted_id = face.id;
    faces_.emplace(inserted_id, std::move(face));
    if (inserted_id.value < std::numeric_limits<std::uint64_t>::max()) {
        next_face_id_ = std::max(next_face_id_, inserted_id.value + 1U);
    }
    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return result;
    }
    if (auto result = validate(); !result) {
        *this = before;
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::rebuild_topology() {
    std::map<EdgeKey, EdgeId> previous_edges;
    for (const auto& [id, edge] : edges_) {
        previous_edges.emplace(undirected(edge.first, edge.second), id);
    }
    std::map<HalfEdgeKey, HalfEdgeId> previous_half_edges;
    for (const auto& [id, edge] : half_edges_) {
        previous_half_edges.emplace(
            HalfEdgeKey{edge.face, edge.origin, edge.destination}, id);
    }
    std::map<CornerKey, CornerId> previous_corners;
    for (const auto& [id, corner] : corners_) {
        previous_corners.emplace(CornerKey{corner.face, corner.vertex}, id);
    }

    std::map<EdgeId, EdgeRecord> new_edges;
    std::map<HalfEdgeId, HalfEdgeRecord> new_half_edges;
    std::map<CornerId, CornerRecord> new_corners;
    std::map<FaceId, HalfEdgeId> new_face_boundaries;
    std::map<std::pair<VertexId, VertexId>, HalfEdgeId> directed;
    std::map<EdgeKey, EdgeId> allocated_edges;
    std::uint64_t next_edge_id = next_edge_id_;
    std::uint64_t next_half_edge_id = next_half_edge_id_;
    std::uint64_t next_corner_id = next_corner_id_;

    const auto allocate_edge_id = [&](const EdgeKey& key) -> core::Result<EdgeId> {
        const auto allocated = allocated_edges.find(key);
        if (allocated != allocated_edges.end()) {
            return core::Result<EdgeId>::success(allocated->second);
        }
        const auto previous = previous_edges.find(key);
        if (previous != previous_edges.end()) {
            allocated_edges.emplace(key, previous->second);
            return core::Result<EdgeId>::success(previous->second);
        }
        if (next_edge_id == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<EdgeId>::failure(Diagnostic(
                ErrorCode::invalid_state, "mesh edge id space is exhausted"));
        }
        const EdgeId allocated_id{next_edge_id++};
        allocated_edges.emplace(key, allocated_id);
        return core::Result<EdgeId>::success(allocated_id);
    };
    const auto allocate_half_edge_id =
        [&](const HalfEdgeKey& key) -> core::Result<HalfEdgeId> {
        const auto previous = previous_half_edges.find(key);
        if (previous != previous_half_edges.end()) {
            return core::Result<HalfEdgeId>::success(previous->second);
        }
        if (next_half_edge_id == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<HalfEdgeId>::failure(Diagnostic(
                ErrorCode::invalid_state, "mesh half-edge id space is exhausted"));
        }
        return core::Result<HalfEdgeId>::success(HalfEdgeId{next_half_edge_id++});
    };
    const auto allocate_corner_id = [&](const CornerKey& key) -> core::Result<CornerId> {
        const auto previous = previous_corners.find(key);
        if (previous != previous_corners.end()) {
            return core::Result<CornerId>::success(previous->second);
        }
        if (next_corner_id == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<CornerId>::failure(Diagnostic(
                ErrorCode::invalid_state, "mesh corner id space is exhausted"));
        }
        return core::Result<CornerId>::success(CornerId{next_corner_id++});
    };

    for (const auto& [face_id, face] : faces_) {
        if (!face_id || face.id != face_id || face.vertices.size() < 3U) {
            return core::Result<void>::failure(validation("mesh contains an invalid face record"));
        }
        std::vector<HalfEdgeId> boundary;
        boundary.reserve(face.vertices.size());
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const VertexId origin = face.vertices[index];
            const VertexId destination = face.vertices[(index + 1U) % face.vertices.size()];
            if (!origin || !destination || origin == destination || !vertices_.contains(origin) ||
                !vertices_.contains(destination)) {
                return core::Result<void>::failure(
                    validation("mesh topology references an invalid vertex"));
            }
            const auto directed_inserted = directed.emplace(
                std::make_pair(origin, destination), HalfEdgeId{});
            if (!directed_inserted.second) {
                return core::Result<void>::failure(
                    validation("mesh contains a duplicate directed edge"));
            }

            const EdgeKey edge_key = undirected(origin, destination);
            auto edge_id = allocate_edge_id(edge_key);
            if (!edge_id) {
                return core::Result<void>::failure(edge_id.error());
            }
            auto half_edge_id = allocate_half_edge_id(
                HalfEdgeKey{face_id, origin, destination});
            if (!half_edge_id) {
                return core::Result<void>::failure(half_edge_id.error());
            }
            auto corner_id = allocate_corner_id(CornerKey{face_id, origin});
            if (!corner_id) {
                return core::Result<void>::failure(corner_id.error());
            }

            const auto [edge_iterator, edge_inserted] = new_edges.emplace(
                edge_id.value(),
                EdgeRecord{edge_id.value(), edge_key.first, edge_key.second,
                           half_edge_id.value(), std::nullopt});
            if (!edge_inserted) {
                if (edge_iterator->second.second_half_edge.has_value()) {
                    return core::Result<void>::failure(
                        validation("mesh edge is incident to more than two faces"));
                }
                edge_iterator->second.second_half_edge = half_edge_id.value();
            }

            new_half_edges.emplace(
                half_edge_id.value(),
                HalfEdgeRecord{half_edge_id.value(), origin, destination, face_id, {}, {},
                               std::nullopt, edge_id.value(), corner_id.value()});
            new_corners.emplace(
                corner_id.value(), CornerRecord{corner_id.value(), face_id, origin, half_edge_id.value()});
            directed_inserted.first->second = half_edge_id.value();
            boundary.push_back(half_edge_id.value());
        }

        for (std::size_t index = 0U; index < boundary.size(); ++index) {
            auto& edge = new_half_edges.at(boundary[index]);
            edge.next = boundary[(index + 1U) % boundary.size()];
            edge.previous = boundary[(index + boundary.size() - 1U) % boundary.size()];
        }
        new_face_boundaries.emplace(face_id, boundary.front());
    }

    for (auto& [id, edge] : new_half_edges) {
        const auto twin = directed.find(std::make_pair(edge.destination, edge.origin));
        if (twin != directed.end()) {
            edge.twin = twin->second;
        }
        static_cast<void>(id);
    }

    edges_ = std::move(new_edges);
    half_edges_ = std::move(new_half_edges);
    corners_ = std::move(new_corners);
    face_boundaries_ = std::move(new_face_boundaries);
    next_edge_id_ = next_edge_id;
    next_half_edge_id_ = next_half_edge_id;
    next_corner_id_ = next_corner_id;
    return core::Result<void>::success();
}

TopologySnapshot EditableMesh::topology_snapshot() const {
    TopologySnapshot snapshot;
    snapshot.edges.reserve(edges_.size());
    snapshot.half_edges.reserve(half_edges_.size());
    snapshot.corners.reserve(corners_.size());
    for (const auto& [id, edge] : edges_) {
        static_cast<void>(id);
        snapshot.edges.push_back(edge);
    }
    for (const auto& [id, edge] : half_edges_) {
        static_cast<void>(id);
        snapshot.half_edges.push_back(edge);
    }
    for (const auto& [id, corner] : corners_) {
        static_cast<void>(id);
        snapshot.corners.push_back(corner);
    }
    return snapshot;
}

core::Result<void> EditableMesh::validate_topology_state() const {
    const TopologySnapshot snapshot = topology_snapshot();
    if (auto result = snapshot.validate(); !result) {
        return result;
    }
    if (face_boundaries_.size() != faces_.size()) {
        return core::Result<void>::failure(
            validation("mesh topology is missing a face boundary"));
    }

    std::map<HalfEdgeId, const HalfEdgeRecord*> by_id;
    for (const auto& edge : snapshot.half_edges) {
        by_id.emplace(edge.id, &edge);
    }
    for (const auto& [face_id, face] : faces_) {
        const auto boundary_id = face_boundaries_.find(face_id);
        if (boundary_id == face_boundaries_.end()) {
            return core::Result<void>::failure(
                validation("mesh topology is missing a face boundary"));
        }
        std::vector<HalfEdgeId> boundary;
        HalfEdgeId current = boundary_id->second;
        for (std::size_t count = 0U; count <= half_edges_.size(); ++count) {
            const auto edge = by_id.find(current);
            if (edge == by_id.end() || edge->second->face != face_id) {
                return core::Result<void>::failure(
                    validation("mesh face boundary references an invalid half-edge"));
            }
            boundary.push_back(current);
            current = edge->second->next;
            if (current == boundary_id->second) {
                break;
            }
        }
        if (current != boundary_id->second || boundary.size() != face.vertices.size()) {
            return core::Result<void>::failure(
                validation("mesh face boundary does not close on the authored face"));
        }
        for (std::size_t index = 0U; index < boundary.size(); ++index) {
            const auto& edge = *by_id.at(boundary[index]);
            if (edge.origin != face.vertices[index] ||
                edge.destination != face.vertices[(index + 1U) % face.vertices.size()]) {
                return core::Result<void>::failure(
                    validation("mesh face boundary disagrees with authored vertex order"));
            }
        }
    }
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

const EdgeRecord* EditableMesh::find_edge(EdgeId id) const noexcept {
    const auto iterator = edges_.find(id);
    return iterator == edges_.end() ? nullptr : &iterator->second;
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
    return validate_topology_state();
}

core::Result<TopologySnapshot> EditableMesh::topology() const {
    if (auto result = validate(); !result) {
        return core::Result<TopologySnapshot>::failure(result.error());
    }
    return core::Result<TopologySnapshot>::success(topology_snapshot());
}

core::Result<void> TopologySnapshot::validate() const {
    std::map<EdgeId, const EdgeRecord*> edges_by_id;
    std::map<HalfEdgeId, const HalfEdgeRecord*> by_id;
    std::map<CornerId, const CornerRecord*> corners_by_id;
    std::map<EdgeId, std::size_t> edge_use;
    std::map<CornerId, std::size_t> corner_use;
    for (const auto& edge : edges) {
        if (!edge.id || !edge.first || !edge.second || edge.first == edge.second ||
            !(edge.first < edge.second) || !edge.first_half_edge) {
            return core::Result<void>::failure(validation("topology contains a malformed edge"));
        }
        if (edge.second_half_edge.has_value() &&
            *edge.second_half_edge == edge.first_half_edge) {
            return core::Result<void>::failure(
                validation("topology edge references the same half-edge twice"));
        }
        if (!edges_by_id.emplace(edge.id, &edge).second) {
            return core::Result<void>::failure(validation("topology contains a duplicate edge id"));
        }
    }
    for (const auto& edge : half_edges) {
        if (!edge.id || !edge.origin || !edge.destination || !edge.face || !edge.next ||
            !edge.previous || !edge.edge || !edge.corner || edge.origin == edge.destination) {
            return core::Result<void>::failure(
                validation("topology contains a malformed half-edge"));
        }
        if (!by_id.emplace(edge.id, &edge).second) {
            return core::Result<void>::failure(validation("topology contains a duplicate half-edge id"));
        }
    }
    for (const auto& corner : corners) {
        if (!corner.id || !corner.face || !corner.vertex || !corner.half_edge) {
            return core::Result<void>::failure(validation("topology contains a malformed corner"));
        }
        if (!corners_by_id.emplace(corner.id, &corner).second) {
            return core::Result<void>::failure(validation("topology contains a duplicate corner id"));
        }
    }
    for (const auto& edge : edges) {
        const auto first = by_id.find(edge.first_half_edge);
        if (first == by_id.end() || first->second->edge != edge.id) {
            return core::Result<void>::failure(
                validation("topology edge does not reference its first half-edge"));
        }
        if (edge.second_half_edge.has_value()) {
            const auto second = by_id.find(*edge.second_half_edge);
            if (second == by_id.end() || second->second->edge != edge.id) {
                return core::Result<void>::failure(
                    validation("topology edge does not reference its second half-edge"));
            }
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
        const auto edge_record = edges_by_id.find(edge.edge);
        if (edge_record == edges_by_id.end()) {
            return core::Result<void>::failure(
                validation("topology half-edge references a missing edge"));
        }
        const bool endpoint_match =
            (edge.origin == edge_record->second->first && edge.destination == edge_record->second->second) ||
            (edge.origin == edge_record->second->second && edge.destination == edge_record->second->first);
        if (!endpoint_match ||
            (edge_record->second->first_half_edge != edge.id &&
             (!edge_record->second->second_half_edge.has_value() ||
              *edge_record->second->second_half_edge != edge.id))) {
            return core::Result<void>::failure(
                validation("topology half-edge disagrees with its edge record"));
        }
        ++edge_use[edge.edge];
        const auto corner = corners_by_id.find(edge.corner);
        if (corner == corners_by_id.end() || corner->second->face != edge.face ||
            corner->second->vertex != edge.origin || corner->second->half_edge != edge.id) {
            return core::Result<void>::failure(
                validation("topology half-edge disagrees with its corner record"));
        }
        ++corner_use[edge.corner];
        if (edge.twin.has_value()) {
            const auto twin = by_id.find(*edge.twin);
            if (twin == by_id.end() || !twin->second->twin.has_value() ||
                *twin->second->twin != edge.id || twin->second->origin != edge.destination ||
                twin->second->destination != edge.origin || twin->second->edge != edge.edge ||
                twin->second->face == edge.face) {
                return core::Result<void>::failure(
                    validation("topology twin links are not symmetric"));
            }
        }
    }
    for (const auto& edge : edges) {
        const std::size_t expected = edge.second_half_edge.has_value() ? 2U : 1U;
        if (edge_use[edge.id] != expected) {
            return core::Result<void>::failure(
                validation("topology edge reference count is inconsistent"));
        }
    }
    for (const auto& corner : corners) {
        if (corner_use[corner.id] != 1U) {
            return core::Result<void>::failure(
                validation("topology corner reference count is inconsistent"));
        }
    }
    return core::Result<void>::success();
}

core::Result<std::vector<HalfEdgeId>> TopologySnapshot::face_boundary(FaceId face) const {
    if (!face) {
        return core::Result<std::vector<HalfEdgeId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology face id must be non-zero"));
    }
    std::map<HalfEdgeId, const HalfEdgeRecord*> by_id;
    std::optional<HalfEdgeId> start;
    for (const auto& edge : half_edges) {
        by_id.emplace(edge.id, &edge);
        if (!start.has_value() && edge.face == face) {
            start = edge.id;
        }
    }
    if (!start.has_value()) {
        return core::Result<std::vector<HalfEdgeId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology face boundary was not found"));
    }

    std::vector<HalfEdgeId> boundary;
    std::set<HalfEdgeId> visited;
    HalfEdgeId current = *start;
    for (std::size_t count = 0U; count <= half_edges.size(); ++count) {
        if (current == *start && !boundary.empty()) {
            return core::Result<std::vector<HalfEdgeId>>::success(std::move(boundary));
        }
        if (!visited.insert(current).second) {
            return core::Result<std::vector<HalfEdgeId>>::failure(
                validation("topology face boundary repeats before closing"));
        }
        const auto edge = by_id.find(current);
        if (edge == by_id.end() || edge->second->face != face) {
            return core::Result<std::vector<HalfEdgeId>>::failure(
                validation("topology face boundary leaves its face"));
        }
        boundary.push_back(current);
        current = edge->second->next;
    }
    return core::Result<std::vector<HalfEdgeId>>::failure(
        validation("topology face boundary exceeds the available half-edges"));
}

std::vector<HalfEdgeId> TopologySnapshot::boundary_half_edges() const {
    std::vector<HalfEdgeId> result;
    for (const auto& edge : half_edges) {
        if (!edge.twin.has_value()) {
            result.push_back(edge.id);
        }
    }
    return result;
}

core::Result<CompiledMesh> EditableMesh::compile() const {
    if (auto result = validate(); !result) {
        return core::Result<CompiledMesh>::failure(result.error());
    }
    if (compiled_cache_.has_value() && compiled_cache_->source_revision == revision_) {
        return core::Result<CompiledMesh>::success(*compiled_cache_);
    }

    CompiledMesh compiled;
    compiled.source_revision = revision_;
    std::map<VertexId, std::uint32_t> indices;
    std::map<EdgeKey, EdgeId> edge_ids;
    for (const auto& [id, edge] : edges_) {
        edge_ids.emplace(undirected(edge.first, edge.second), id);
    }
    const auto find_edge = [&edge_ids](VertexId first, VertexId second) {
        const auto found = edge_ids.find(undirected(first, second));
        if (found == edge_ids.end()) return std::optional<EdgeId>{};
        return std::optional<EdgeId>{found->second};
    };
    for (const auto& [id, vertex] : vertices_) {
        indices.emplace(id, static_cast<std::uint32_t>(compiled.positions.size()));
        compiled.positions.push_back(vertex.position);
        compiled.normals.push_back({});
        compiled.vertex_ids.push_back(id);
        compiled.bounds.include(vertex.position);
    }

    for (const auto& [face_id, face] : faces_) {
        const std::uint32_t first = indices.at(face.vertices.front());
        for (std::size_t index = 1U; index + 1U < face.vertices.size(); ++index) {
            const std::uint32_t second = indices.at(face.vertices[index]);
            const std::uint32_t third = indices.at(face.vertices[index + 1U]);
            compiled.indices.insert(compiled.indices.end(), {first, second, third});
            compiled.triangle_faces.push_back(face_id);
            compiled.triangle_edges.push_back({
                find_edge(face.vertices.front(), face.vertices[index]),
                find_edge(face.vertices[index], face.vertices[index + 1U]),
                find_edge(face.vertices[index + 1U], face.vertices.front()),
            });
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
    compiled_cache_ = std::move(compiled);
    return core::Result<CompiledMesh>::success(*compiled_cache_);
}

} // namespace carto::geometry
