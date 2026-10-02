#include <carto/geometry/mesh.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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

void hash_combine(std::size_t& seed, std::uint64_t value) noexcept {
    const auto hashed = std::hash<std::uint64_t>{}(value);
    seed ^= hashed + static_cast<std::size_t>(0x9e3779b9U) + (seed << 6U) + (seed >> 2U);
}

struct EdgeKeyHash {
    [[nodiscard]] std::size_t operator()(const EdgeKey& key) const noexcept {
        std::size_t seed = 0U;
        hash_combine(seed, key.first.value);
        hash_combine(seed, key.second.value);
        return seed;
    }
};

struct HalfEdgeKeyHash {
    [[nodiscard]] std::size_t operator()(const HalfEdgeKey& key) const noexcept {
        std::size_t seed = 0U;
        hash_combine(seed, key.face.value);
        hash_combine(seed, key.origin.value);
        hash_combine(seed, key.destination.value);
        return seed;
    }
};

struct CornerKeyHash {
    [[nodiscard]] std::size_t operator()(const CornerKey& key) const noexcept {
        std::size_t seed = 0U;
        hash_combine(seed, key.face.value);
        hash_combine(seed, key.vertex.value);
        return seed;
    }
};

struct VertexPairHash {
    [[nodiscard]] std::size_t operator()(
        const std::pair<VertexId, VertexId>& key) const noexcept {
        std::size_t seed = 0U;
        hash_combine(seed, key.first.value);
        hash_combine(seed, key.second.value);
        return seed;
    }
};

template <typename Id>
struct IdHash {
    [[nodiscard]] std::size_t operator()(Id id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
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
    if (auto result = validate_patch_geometry(patch); !result) {
        for (std::size_t index = 0U; index < patch.vertex_changes.size(); ++index) {
            vertices_.at(patch.vertex_changes[index].id).position = previous_positions[index];
        }
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::validate_patch_geometry(const MeshPatch& patch) const {
    // A mesh patch changes positions only. All topology mutation paths rebuild and validate
    // topology before publishing, so rechecking maps and identity records here would duplicate
    // the dominant cost without expanding the invariant boundary. Use the maintained incidence
    // index to visit only faces whose geometry can be invalidated by this patch.
    std::set<FaceId> affected_faces;
    for (const auto& change : patch.vertex_changes) {
        const auto incident = vertex_faces_.find(change.id);
        if (incident == vertex_faces_.end()) {
            continue;
        }
        affected_faces.insert(incident->second.begin(), incident->second.end());
    }

    for (const FaceId face_id : affected_faces) {
        const auto face_iterator = faces_.find(face_id);
        if (face_iterator == faces_.end()) {
            return core::Result<void>::failure(
                validation("vertex-to-face incidence references a missing face"));
        }
        const auto& face = face_iterator->second;
        if (!face_id || face.id != face_id || face.vertices.size() < 3U) {
            return core::Result<void>::failure(validation("mesh contains an invalid face record"));
        }

        std::set<VertexId> unique_vertices;
        for (const VertexId vertex : face.vertices) {
            if (!vertex || !vertices_.contains(vertex) || !unique_vertices.insert(vertex).second) {
                return core::Result<void>::failure(
                    validation("mesh face has a dangling or duplicate vertex"));
            }
        }

        const core::Vec3d first_position = vertices_.at(face.vertices.front()).position;
        for (std::size_t index = 1U; index + 1U < face.vertices.size(); ++index) {
            const core::Vec3d second_position = vertices_.at(face.vertices[index]).position;
            const core::Vec3d third_position = vertices_.at(face.vertices[index + 1U]).position;
            if (triangle_area_squared(first_position, second_position, third_position) <= 1e-24) {
                return core::Result<void>::failure(validation("mesh face contains a zero-area triangle"));
            }
        }
    }
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

    const std::uint64_t new_vertex_count = static_cast<std::uint64_t>(original.vertices.size());
    const std::uint64_t new_face_count = new_vertex_count + 1U;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (next_vertex_id_ > maximum - new_vertex_count) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh vertex id space is exhausted"));
    }
    if (next_face_id_ > maximum - new_face_count) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh face id space is exhausted"));
    }

    std::vector<core::Vec3d> extruded_positions;
    extruded_positions.reserve(original.vertices.size());
    for (const VertexId vertex : original.vertices) {
        const core::Vec3d position = vertices_.at(vertex).position + normal * distance;
        if (!position.finite()) {
            return core::Result<void>::failure(
                invalid("extruded vertex position must be finite"));
        }
        extruded_positions.push_back(position);
    }

    const EditableMesh before = *this;
    faces_.erase(face_iterator);

    std::vector<VertexId> extruded_vertices;
    extruded_vertices.reserve(original.vertices.size());
    for (const core::Vec3d position : extruded_positions) {
        const VertexId vertex{next_vertex_id_++};
        vertices_.emplace(vertex, Vertex{vertex, position});
        extruded_vertices.push_back(vertex);
    }

    for (std::size_t index = 0U; index < original.vertices.size(); ++index) {
        const std::size_t next = (index + 1U) % original.vertices.size();
        const FaceId face{next_face_id_++};
        faces_.emplace(face, Face{
            face,
            {original.vertices[index], original.vertices[next],
             extruded_vertices[next], extruded_vertices[index]},
        });
    }

    const FaceId cap{next_face_id_++};
    faces_.emplace(cap, Face{cap, extruded_vertices});

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
        for (const VertexId vertex : deleted.vertices) {
            const auto incident = vertex_faces_.find(vertex);
            const bool used_by_remaining_face = incident == vertex_faces_.end()
                ? std::any_of(
                    faces_.begin(), faces_.end(),
                    [vertex](const auto& entry) {
                        const auto& face = entry.second;
                        return std::find(face.vertices.begin(), face.vertices.end(), vertex) !=
                            face.vertices.end();
                    })
                : std::any_of(
                    incident->second.begin(), incident->second.end(),
                    [this](const FaceId face_id) { return faces_.contains(face_id); });
            if (!used_by_remaining_face) {
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

    // The edge record is already the authoritative narrow-phase index for this
    // edit. Avoid scanning unrelated faces in large authored meshes. Keep the
    // stale-record checks here so a corrupted index fails closed and still
    // rolls back through the existing transaction snapshot.
    std::vector<FaceId> incident_face_ids;
    incident_face_ids.reserve(2U);
    const auto collect_incident_face = [&](HalfEdgeId half_edge_id) -> core::Result<void> {
        const auto half_edge_iterator = half_edges_.find(half_edge_id);
        if (half_edge_iterator == half_edges_.end() || half_edge_iterator->second.edge != id) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data, "mesh edge references a missing or mismatched half-edge"));
        }
        const FaceId face_id = half_edge_iterator->second.face;
        if (!faces_.contains(face_id)) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data, "mesh edge references a missing incident face"));
        }
        if (std::find(incident_face_ids.begin(), incident_face_ids.end(), face_id) !=
            incident_face_ids.end()) {
            return core::Result<void>::failure(validation(
                "mesh edge occurs more than once in one face"));
        }
        incident_face_ids.push_back(face_id);
        return core::Result<void>::success();
    };
    if (auto result = collect_incident_face(edge.first_half_edge); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (edge.second_half_edge.has_value()) {
        if (auto result = collect_incident_face(*edge.second_half_edge); !result) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(result.error());
        }
    }

    std::size_t incident_faces = 0U;
    for (const FaceId face_id : incident_face_ids) {
        auto face_iterator = faces_.find(face_id);
        if (face_iterator == faces_.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "mesh edge incident face disappeared during split"));
        }
        auto& face = face_iterator->second;
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
    bump_revision();

    std::vector<EdgeId> created_edges;
    for (const auto& [edge_id, edge_record] : edges_) {
        if (edge_record.first == created_vertex || edge_record.second == created_vertex) {
            created_edges.push_back(edge_id);
        }
    }
    return core::Result<TopologyEditReceipt>::success(TopologyEditReceipt{
        revision_before,
        revision_,
        {created_vertex},
        std::move(created_edges),
        {},
        {},
        {id},
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
    bump_revision();

    std::vector<EdgeId> created_edges;
    const std::set<VertexId> inset_vertex_set(inset_vertices.begin(), inset_vertices.end());
    for (const auto& [edge_id, edge_record] : edges_) {
        if (inset_vertex_set.contains(edge_record.first) ||
            inset_vertex_set.contains(edge_record.second)) {
            created_edges.push_back(edge_id);
        }
    }
    return core::Result<TopologyEditReceipt>::success(TopologyEditReceipt{
        revision_before,
        revision_,
        std::move(inset_vertices),
        std::move(created_edges),
        std::move(created_faces),
        {},
        {},
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
    vertex_faces_ = source.vertex_faces_;
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

core::Result<void> EditableMesh::insert_bulk(
    std::vector<Vertex> vertices,
    std::vector<Face> faces) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }

    std::map<VertexId, Vertex> new_vertices;
    std::uint64_t next_vertex_id = 1U;
    for (Vertex& vertex : vertices) {
        if (!vertex.id || !vertex.position.finite()) {
            return core::Result<void>::failure(
                invalid("bulk mesh vertex requires a non-zero id and finite position"));
        }
        if (!new_vertices.emplace(vertex.id, std::move(vertex)).second) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::invalid_state, "bulk mesh contains a duplicate vertex id"));
        }
        if (new_vertices.rbegin()->first.value < std::numeric_limits<std::uint64_t>::max()) {
            next_vertex_id = std::max(next_vertex_id, new_vertices.rbegin()->first.value + 1U);
        }
    }

    std::map<FaceId, Face> new_faces;
    std::uint64_t next_face_id = 1U;
    for (Face& face : faces) {
        if (!face.id || face.vertices.size() < 3U) {
            return core::Result<void>::failure(
                invalid("bulk mesh face requires a non-zero id and at least three vertices"));
        }
        std::set<VertexId> unique_vertices;
        for (const VertexId vertex : face.vertices) {
            if (!vertex || !new_vertices.contains(vertex)) {
                return core::Result<void>::failure(
                    Diagnostic(ErrorCode::not_found, "bulk mesh face references a missing vertex"));
            }
            if (!unique_vertices.insert(vertex).second) {
                return core::Result<void>::failure(
                    validation("bulk mesh face contains a duplicate vertex"));
            }
        }
        if (!new_faces.emplace(face.id, std::move(face)).second) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::invalid_state, "bulk mesh contains a duplicate face id"));
        }
        if (new_faces.rbegin()->first.value < std::numeric_limits<std::uint64_t>::max()) {
            next_face_id = std::max(next_face_id, new_faces.rbegin()->first.value + 1U);
        }
    }

    const EditableMesh before = *this;
    vertices_ = std::move(new_vertices);
    faces_ = std::move(new_faces);
    edges_.clear();
    half_edges_.clear();
    corners_.clear();
    face_boundaries_.clear();
    vertex_faces_.clear();
    next_vertex_id_ = next_vertex_id;
    next_face_id_ = next_face_id;
    next_edge_id_ = 1U;
    next_half_edge_id_ = 1U;
    next_corner_id_ = 1U;
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
    std::unordered_map<EdgeKey, EdgeId, EdgeKeyHash> previous_edges;
    previous_edges.reserve(edges_.size());
    for (const auto& [id, edge] : edges_) {
        previous_edges.emplace(undirected(edge.first, edge.second), id);
    }
    std::unordered_map<HalfEdgeKey, HalfEdgeId, HalfEdgeKeyHash> previous_half_edges;
    previous_half_edges.reserve(half_edges_.size());
    for (const auto& [id, edge] : half_edges_) {
        previous_half_edges.emplace(
            HalfEdgeKey{edge.face, edge.origin, edge.destination}, id);
    }
    std::unordered_map<CornerKey, CornerId, CornerKeyHash> previous_corners;
    previous_corners.reserve(corners_.size());
    for (const auto& [id, corner] : corners_) {
        previous_corners.emplace(CornerKey{corner.face, corner.vertex}, id);
    }

    std::map<EdgeId, EdgeRecord> new_edges;
    std::map<HalfEdgeId, HalfEdgeRecord> new_half_edges;
    std::map<CornerId, CornerRecord> new_corners;
    std::map<FaceId, HalfEdgeId> new_face_boundaries;
    std::map<VertexId, std::vector<FaceId>> new_vertex_faces;
    std::unordered_map<std::pair<VertexId, VertexId>, HalfEdgeId, VertexPairHash> directed;
    directed.reserve(half_edges_.size() + faces_.size());
    std::unordered_map<EdgeKey, EdgeId, EdgeKeyHash> allocated_edges;
    allocated_edges.reserve(edges_.size() + faces_.size());
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
        for (const VertexId vertex : face.vertices) {
            new_vertex_faces[vertex].push_back(face_id);
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

    for (auto& [vertex, faces] : new_vertex_faces) {
        static_cast<void>(vertex);
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    }

    edges_ = std::move(new_edges);
    half_edges_ = std::move(new_half_edges);
    corners_ = std::move(new_corners);
    face_boundaries_ = std::move(new_face_boundaries);
    vertex_faces_ = std::move(new_vertex_faces);
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

    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(snapshot.half_edges.size());
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

core::Result<void> EditableMesh::validate_vertex_face_index() const {
    for (const auto& [vertex_id, face_ids] : vertex_faces_) {
        if (!vertex_id || !vertices_.contains(vertex_id)) {
            return core::Result<void>::failure(
                validation("vertex-to-face incidence references a missing vertex"));
        }
        if (!std::is_sorted(face_ids.begin(), face_ids.end()) ||
            std::adjacent_find(face_ids.begin(), face_ids.end()) != face_ids.end()) {
            return core::Result<void>::failure(
                validation("vertex-to-face incidence is not sorted and unique"));
        }
        for (const FaceId face_id : face_ids) {
            const auto face = faces_.find(face_id);
            if (face == faces_.end() ||
                std::find(face->second.vertices.begin(), face->second.vertices.end(), vertex_id) ==
                    face->second.vertices.end()) {
                return core::Result<void>::failure(
                    validation("vertex-to-face incidence disagrees with authored faces"));
            }
        }
    }

    for (const auto& [face_id, face] : faces_) {
        for (const VertexId vertex_id : face.vertices) {
            const auto incident = vertex_faces_.find(vertex_id);
            if (incident == vertex_faces_.end() ||
                !std::binary_search(incident->second.begin(), incident->second.end(), face_id)) {
                return core::Result<void>::failure(
                    validation("authored face is missing from vertex-to-face incidence"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::validate() const {
    std::unordered_map<EdgeKey, std::size_t, EdgeKeyHash> edge_use;
    edge_use.reserve(edges_.size());
    std::unordered_map<std::pair<VertexId, VertexId>, FaceId, VertexPairHash> directed_edges;
    directed_edges.reserve(half_edges_.size());

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
    if (auto result = validate_vertex_face_index(); !result) {
        return result;
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
    std::unordered_map<EdgeId, const EdgeRecord*, IdHash<EdgeId>> edges_by_id;
    edges_by_id.reserve(edges.size());
    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(half_edges.size());
    std::unordered_map<CornerId, const CornerRecord*, IdHash<CornerId>> corners_by_id;
    corners_by_id.reserve(corners.size());
    std::unordered_map<EdgeId, std::size_t, IdHash<EdgeId>> edge_use;
    edge_use.reserve(edges.size());
    std::unordered_map<CornerId, std::size_t, IdHash<CornerId>> corner_use;
    corner_use.reserve(corners.size());
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
    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(half_edges.size());
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
    std::unordered_set<HalfEdgeId, IdHash<HalfEdgeId>> visited;
    visited.reserve(half_edges.size());
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
