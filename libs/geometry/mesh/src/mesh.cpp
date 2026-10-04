#include <carto/geometry/mesh.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>
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

template <typename Id>
bool has_duplicate_ids(const std::vector<Id>& ids) {
    constexpr std::size_t kLinearScanLimit = 32U;
    if (ids.size() <= kLinearScanLimit) {
        for (std::size_t index = 0U; index < ids.size(); ++index) {
            if (std::find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(index), ids[index]) !=
                ids.begin() + static_cast<std::ptrdiff_t>(index)) {
                return true;
            }
        }
        return false;
    }

    std::unordered_set<Id, IdHash<Id>> unique;
    unique.reserve(ids.size());
    for (const Id id : ids) {
        if (!unique.insert(id).second) return true;
    }
    return false;
}

core::Result<void> validate_attribute_payload_for_topology(
    const attributes::AttributeSet& attributes,
    const attributes::UvSetTable& uv_sets,
    std::size_t vertex_count,
    std::size_t edge_count,
    std::size_t face_count,
    std::size_t corner_count) {
    if (auto result = attributes.validate(); !result) return result;

    const auto check_count = [&](attributes::AttributeDomain domain, std::size_t expected) {
        const auto count = attributes.domain_count(domain);
        if (count.has_value() && *count != expected) {
            return core::Result<void>::failure(validation(
                "attribute domain count does not match the mesh topology"));
        }
        return core::Result<void>::success();
    };
    if (auto result = check_count(attributes::AttributeDomain::vertex, vertex_count); !result) {
        return result;
    }
    if (auto result = check_count(attributes::AttributeDomain::edge, edge_count); !result) {
        return result;
    }
    if (auto result = check_count(attributes::AttributeDomain::face, face_count); !result) {
        return result;
    }
    if (auto result = check_count(attributes::AttributeDomain::corner, corner_count); !result) {
        return result;
    }
    return uv_sets.validate(attributes);
}

EdgeKey undirected(VertexId left, VertexId right) {
    return left < right ? EdgeKey{left, right} : EdgeKey{right, left};
}

double triangle_area_squared(core::Vec3d a, core::Vec3d b, core::Vec3d c) {
    return core::cross(b - a, c - a).length_squared() * 0.25;
}

bool same_position(core::Vec3d left, core::Vec3d right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

template <typename Id>
std::vector<Id> ids_not_in(const std::vector<Id>& source, const std::vector<Id>& other) {
    std::set<Id> other_ids(other.begin(), other.end());
    std::vector<Id> result;
    for (const Id id : source) {
        if (!other_ids.contains(id)) result.push_back(id);
    }
    return result;
}

std::vector<EdgeId> edge_ids(const TopologySnapshot& snapshot) {
    std::vector<EdgeId> result;
    result.reserve(snapshot.edges.size());
    for (const auto& edge : snapshot.edges) result.push_back(edge.id);
    return result;
}

std::vector<CornerId> corner_ids(const TopologySnapshot& snapshot) {
    std::vector<CornerId> result;
    result.reserve(snapshot.corners.size());
    for (const auto& corner : snapshot.corners) result.push_back(corner.id);
    return result;
}

core::Result<void> populate_identity_delta(
    const EditableMesh& before,
    const EditableMesh& after,
    TopologyEditReceipt& receipt) {
    const auto before_topology = before.topology();
    const auto after_topology = after.topology();
    if (!before_topology) return core::Result<void>::failure(before_topology.error());
    if (!after_topology) return core::Result<void>::failure(after_topology.error());

    const auto before_vertices = before.vertices_sorted();
    const auto after_vertices = after.vertices_sorted();
    std::vector<VertexId> before_vertex_ids;
    std::vector<VertexId> after_vertex_ids;
    before_vertex_ids.reserve(before_vertices.size());
    after_vertex_ids.reserve(after_vertices.size());
    for (const auto& vertex : before_vertices) before_vertex_ids.push_back(vertex.id);
    for (const auto& vertex : after_vertices) after_vertex_ids.push_back(vertex.id);

    const auto before_faces = before.faces_sorted();
    const auto after_faces = after.faces_sorted();
    std::vector<FaceId> before_face_ids;
    std::vector<FaceId> after_face_ids;
    before_face_ids.reserve(before_faces.size());
    after_face_ids.reserve(after_faces.size());
    for (const auto& face : before_faces) before_face_ids.push_back(face.id);
    for (const auto& face : after_faces) after_face_ids.push_back(face.id);

    receipt.created_vertices = ids_not_in(after_vertex_ids, before_vertex_ids);
    receipt.removed_vertices = ids_not_in(before_vertex_ids, after_vertex_ids);
    receipt.created_faces = ids_not_in(after_face_ids, before_face_ids);
    receipt.removed_faces = ids_not_in(before_face_ids, after_face_ids);
    receipt.created_edges = ids_not_in(
        edge_ids(after_topology.value()), edge_ids(before_topology.value()));
    receipt.removed_edges = ids_not_in(
        edge_ids(before_topology.value()), edge_ids(after_topology.value()));
    receipt.created_corners = ids_not_in(
        corner_ids(after_topology.value()), corner_ids(before_topology.value()));
    receipt.removed_corners = ids_not_in(
        corner_ids(before_topology.value()), corner_ids(after_topology.value()));
    return core::Result<void>::success();
}

template <typename Id>
void add_origin(
    std::map<Id, ElementOrigin>& origins,
    Id id,
    OriginKind kind,
    std::initializer_list<std::uint64_t> source_ids) {
    origins.emplace(id, ElementOrigin{kind, std::vector<std::uint64_t>(
        source_ids.begin(), source_ids.end())});
}

template <typename Id>
void add_origin(
    std::map<Id, ElementOrigin>& origins,
    Id id,
    OriginKind kind,
    std::vector<std::uint64_t> source_ids) {
    origins.emplace(id, ElementOrigin{kind, std::move(source_ids)});
}

bool valid_origin_kind(OriginKind kind) noexcept {
    switch (kind) {
    case OriginKind::preserved:
    case OriginKind::duplicated_from:
    case OriginKind::interpolated_from:
    case OriginKind::split_from:
    case OriginKind::generated_from_vertex:
    case OriginKind::generated_from_edge:
    case OriginKind::generated_from_face:
    case OriginKind::boolean_intersection:
    case OriginKind::subdivision_child:
        return true;
    }
    return false;
}

} // namespace

core::Result<void> TopologyEditReceipt::validate() const {
    if (revision_before.exhausted() || revision_after.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "topology edit receipt revisions must not be exhausted"));
    }
    if (revision_after <= revision_before) {
        return core::Result<void>::failure(validation(
            "topology edit receipt must advance the mesh revision"));
    }

    const auto validate_ids = [](const auto& ids, const char* label) -> core::Result<void> {
        using Id = typename std::decay_t<decltype(ids)>::value_type;
        std::set<Id> unique;
        for (const Id id : ids) {
            if (!id || !unique.insert(id).second) {
                return core::Result<void>::failure(validation(
                    std::string("topology edit receipt contains an invalid or duplicate ") + label));
            }
        }
        if (!std::is_sorted(ids.begin(), ids.end())) {
            return core::Result<void>::failure(validation(
                std::string("topology edit receipt contains non-deterministically ordered ") + label +
                " identities"));
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_ids(created_vertices, "created vertex"); !result) return result;
    if (auto result = validate_ids(created_edges, "created edge"); !result) return result;
    if (auto result = validate_ids(created_faces, "created face"); !result) return result;
    if (auto result = validate_ids(removed_vertices, "removed vertex"); !result) return result;
    if (auto result = validate_ids(removed_edges, "removed edge"); !result) return result;
    if (auto result = validate_ids(removed_faces, "removed face"); !result) return result;
    if (auto result = validate_ids(created_corners, "created corner"); !result) return result;
    if (auto result = validate_ids(removed_corners, "removed corner"); !result) return result;

    const auto validate_disjoint = [](const auto& created, const auto& removed,
                                      const char* label) -> core::Result<void> {
        std::set<typename std::decay_t<decltype(created)>::value_type> removed_ids(
            removed.begin(), removed.end());
        for (const auto id : created) {
            if (removed_ids.contains(id)) {
                return core::Result<void>::failure(validation(
                    std::string("topology edit receipt marks the same ") + label +
                    " as created and removed"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_disjoint(created_vertices, removed_vertices, "vertex"); !result) {
        return result;
    }
    if (auto result = validate_disjoint(created_edges, removed_edges, "edge"); !result) {
        return result;
    }
    if (auto result = validate_disjoint(created_faces, removed_faces, "face"); !result) {
        return result;
    }
    if (auto result = validate_disjoint(created_corners, removed_corners, "corner"); !result) {
        return result;
    }

    for (const auto& [source, target] : merged_vertices) {
        if (!source || !target || source == target) {
            return core::Result<void>::failure(validation(
                "topology receipt contains an invalid vertex merge mapping"));
        }
        if (!std::binary_search(removed_vertices.begin(), removed_vertices.end(), source)) {
            return core::Result<void>::failure(validation(
                "topology receipt merge source is not a removed vertex"));
        }
        if (std::binary_search(removed_vertices.begin(), removed_vertices.end(), target)) {
            return core::Result<void>::failure(validation(
                "topology receipt merge target is also removed"));
        }
        if (std::binary_search(created_vertices.begin(), created_vertices.end(), source)) {
            return core::Result<void>::failure(validation(
                "topology receipt merge source is also a created vertex"));
        }
    }

    const auto validate_origins = [&]<typename Map, typename IdList>(
        const Map& origins, const IdList& created, const char* label) -> core::Result<void> {
        for (const auto& [id, origin] : origins) {
            if (std::find(created.begin(), created.end(), id) == created.end()) {
                return core::Result<void>::failure(validation(
                    std::string("origin map contains a non-created ") + label));
            }
            if (!valid_origin_kind(origin.kind) || origin.source_ids.empty()) {
                return core::Result<void>::failure(validation(
                    std::string("origin map contains an invalid ") + label + " origin"));
            }
            std::set<std::uint64_t> unique_sources(
                origin.source_ids.begin(), origin.source_ids.end());
            if (unique_sources.size() != origin.source_ids.size() ||
                unique_sources.contains(0U)) {
                return core::Result<void>::failure(validation(
                    std::string("origin map contains duplicate or zero ") + label + " sources"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_origins(vertex_origins, created_vertices, "vertex"); !result) return result;
    if (auto result = validate_origins(edge_origins, created_edges, "edge"); !result) return result;
    if (auto result = validate_origins(face_origins, created_faces, "face"); !result) return result;
    if (auto result = validate_origins(corner_origins, created_corners, "corner"); !result) return result;
    return core::Result<void>::success();
}

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

core::Result<void> EditableMesh::reject_topology_edit_with_attributes() const {
    if (!has_attribute_payload()) return core::Result<void>::success();
    return core::Result<void>::failure(Diagnostic(
        ErrorCode::invalid_state,
        "topology edits require explicit attribute transfer before UV payload is attached"));
}

core::Result<void> EditableMesh::validate_attribute_payload() const {
    return validate_attribute_payload_for_topology(
        attributes_, uv_sets_, vertices_.size(), edges_.size(), faces_.size(), corners_.size());
}

core::Result<void> EditableMesh::set_attribute_payload(
    attributes::AttributeSet attributes,
    attributes::UvSetTable uv_sets) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = validate_attribute_payload_for_topology(
            attributes, uv_sets, vertices_.size(), edges_.size(), faces_.size(), corners_.size());
        !result) {
        return result;
    }
    attributes_ = std::move(attributes);
    uv_sets_ = std::move(uv_sets);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> EditableMesh::clear_attribute_payload() {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (!has_attribute_payload()) return core::Result<void>::success();
    attributes_ = attributes::AttributeSet{};
    uv_sets_ = attributes::UvSetTable{};
    bump_revision();
    return core::Result<void>::success();
}

core::Result<VertexId> EditableMesh::add_vertex(core::Vec3d position) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<VertexId>::failure(result.error());
    }
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
    if (auto result = reject_topology_edit_with_attributes(); !result) return result;
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

core::Result<void> EditableMesh::slide_vertex(
    VertexId vertex,
    EdgeId support_edge,
    double factor) {
    if (!std::isfinite(factor) || factor <= 0.0 || factor >= 1.0) {
        return core::Result<void>::failure(invalid(
            "vertex slide factor must be finite and strictly between zero and one"));
    }
    const auto* source = find_vertex(vertex);
    if (!source) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "cannot slide a missing mesh vertex"));
    }
    const auto* edge = find_edge(support_edge);
    if (!edge) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "vertex slide references a missing support edge"));
    }
    VertexId destination;
    if (edge->first == vertex) {
        destination = edge->second;
    } else if (edge->second == vertex) {
        destination = edge->first;
    } else {
        return core::Result<void>::failure(invalid(
            "vertex slide support edge must be incident to the selected vertex"));
    }
    const auto* target = find_vertex(destination);
    if (!target) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data, "vertex slide support edge references a missing endpoint"));
    }
    const core::Vec3d position = source->position +
        (target->position - source->position) * factor;
    return set_vertex_position(vertex, position);
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
    std::vector<FaceId> affected_faces;
    for (const auto& change : patch.vertex_changes) {
        const auto incident = vertex_faces_.find(change.id);
        if (incident == vertex_faces_.end()) {
            continue;
        }
        for (const FaceId face_id : incident->second) {
            if (std::find(affected_faces.begin(), affected_faces.end(), face_id) ==
                affected_faces.end()) {
                affected_faces.push_back(face_id);
            }
        }
    }
    std::sort(affected_faces.begin(), affected_faces.end());

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

        for (const VertexId vertex : face.vertices) {
            if (!vertex || !vertices_.contains(vertex)) {
                return core::Result<void>::failure(
                    validation("mesh face has a dangling or duplicate vertex"));
            }
        }
        if (has_duplicate_ids(face.vertices)) {
            return core::Result<void>::failure(
                validation("mesh face has a dangling or duplicate vertex"));
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
    if (auto result = reject_topology_edit_with_attributes(); !result) return result;
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

core::Result<TopologyEditReceipt> EditableMesh::extrude_face_with_receipt(
    FaceId id, double distance) {
    const EditableMesh before = *this;
    const auto original = before.find_face(id);
    if (original == nullptr) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::not_found, "cannot create an extrusion receipt for a missing face"));
    }
    if (auto result = extrude_face(id, distance); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    TopologyEditReceipt receipt;
    receipt.revision_before = before.revision();
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    std::map<VertexId, VertexId> created_vertex_sources;
    if (receipt.created_vertices.size() != original->vertices.size()) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "extrusion receipt does not contain one generated vertex per source vertex"));
    }
    for (std::size_t index = 0U; index < receipt.created_vertices.size(); ++index) {
        const VertexId created = receipt.created_vertices[index];
        const VertexId source = original->vertices[index];
        created_vertex_sources.emplace(created, source);
        add_origin(receipt.vertex_origins, created, OriginKind::duplicated_from, {source.value});
    }
    for (const FaceId created : receipt.created_faces) {
        add_origin(receipt.face_origins, created, OriginKind::generated_from_face, {id.value});
    }

    const auto before_topology = before.topology();
    const auto after_topology = topology();
    if (!before_topology || !after_topology) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "extrusion receipt topology snapshot failed validation"));
    }
    std::map<EdgeKey, EdgeId> source_edges;
    for (const auto& edge : before_topology.value().edges) {
        source_edges.emplace(undirected(edge.first, edge.second), edge.id);
    }
    const auto source_vertex_ids = [&created_vertex_sources](VertexId vertex) {
        const auto found = created_vertex_sources.find(vertex);
        return found == created_vertex_sources.end()
            ? std::vector<VertexId>{vertex} : std::vector<VertexId>{found->second};
    };
    const auto source_edge_for = [&source_edges](
        const std::vector<VertexId>& first_sources,
        const std::vector<VertexId>& second_sources) -> std::optional<EdgeId> {
        for (const VertexId first : first_sources) {
            for (const VertexId second : second_sources) {
                if (first == second) continue;
                const auto source = source_edges.find(undirected(first, second));
                if (source != source_edges.end()) return source->second;
            }
        }
        for (const auto& [key, source] : source_edges) {
            const bool first_matches = std::find(
                first_sources.begin(), first_sources.end(), key.first) != first_sources.end() ||
                std::find(first_sources.begin(), first_sources.end(), key.second) != first_sources.end();
            const bool second_matches = std::find(
                second_sources.begin(), second_sources.end(), key.first) != second_sources.end() ||
                std::find(second_sources.begin(), second_sources.end(), key.second) != second_sources.end();
            if (first_matches || second_matches) return source;
        }
        return std::nullopt;
    };
    for (const auto& edge : after_topology.value().edges) {
        if (!std::binary_search(receipt.created_edges.begin(), receipt.created_edges.end(), edge.id)) {
            continue;
        }
        const auto source = source_edge_for(
            source_vertex_ids(edge.first), source_vertex_ids(edge.second));
        if (source.has_value()) {
            add_origin(receipt.edge_origins, edge.id, OriginKind::generated_from_edge,
                {source->value});
        }
    }

    std::map<std::pair<FaceId, VertexId>, CornerId> source_corners;
    for (const auto& corner : before_topology.value().corners) {
        source_corners.emplace(std::make_pair(corner.face, corner.vertex), corner.id);
    }
    for (const auto& corner : after_topology.value().corners) {
        if (!std::binary_search(receipt.created_corners.begin(), receipt.created_corners.end(), corner.id)) {
            continue;
        }
        const auto face_origin = receipt.face_origins.find(corner.face);
        if (face_origin == receipt.face_origins.end()) continue;
        const auto vertices = source_vertex_ids(corner.vertex);
        std::vector<std::uint64_t> source_ids;
        for (const VertexId vertex : vertices) {
            const auto source = source_corners.find(
                std::make_pair(FaceId{face_origin->second.source_ids.front()}, vertex));
            if (source != source_corners.end()) source_ids.push_back(source->second.value);
        }
        if (source_ids.empty()) continue;
        add_origin(receipt.corner_origins, corner.id,
            source_ids.size() == 1U ? OriginKind::duplicated_from : OriginKind::interpolated_from,
            std::move(source_ids));
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::delete_face(
    FaceId id,
    bool remove_orphaned_vertices) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
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
    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::split_edge(EdgeId id, double factor) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
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
    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    add_origin(receipt.vertex_origins, created_vertex, OriginKind::interpolated_from,
        {edge.first.value, edge.second.value});
    for (const EdgeId created : receipt.created_edges) {
        add_origin(receipt.edge_origins, created, OriginKind::split_from, {id.value});
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::bevel_vertex(
    VertexId id,
    double width,
    std::uint32_t segments) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!std::isfinite(width) || width <= 0.0) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "vertex bevel width must be finite and strictly positive"));
    }
    if (segments != 1U) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "vertex bevel currently supports exactly one segment"));
    }
    const auto vertex_iterator = vertices_.find(id);
    if (vertex_iterator == vertices_.end()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::not_found, "cannot bevel a missing mesh vertex"));
    }

    const auto incident_faces_iterator = vertex_faces_.find(id);
    if (incident_faces_iterator == vertex_faces_.end()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::stale_data, "vertex bevel has no indexed incident faces"));
    }
    const auto& incident_face_ids = incident_faces_iterator->second;
    if (incident_face_ids.size() != 3U) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "vertex bevel currently requires exactly three incident faces"));
    }

    std::vector<EdgeRecord> incident_edges;
    incident_edges.reserve(3U);
    for (const auto& [edge_id, edge] : edges_) {
        static_cast<void>(edge_id);
        if (edge.first == id || edge.second == id) {
            incident_edges.push_back(edge);
        }
    }
    if (incident_edges.size() != 3U) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "vertex bevel currently requires a valence-three vertex"));
    }

    const auto incident_face = [&incident_face_ids](FaceId face) {
        return std::find(incident_face_ids.begin(), incident_face_ids.end(), face) !=
            incident_face_ids.end();
    };
    const auto find_incident_edge = [&incident_edges](VertexId first, VertexId second)
        -> const EdgeRecord* {
        for (const auto& edge : incident_edges) {
            if ((edge.first == first && edge.second == second) ||
                (edge.first == second && edge.second == first)) {
                return &edge;
            }
        }
        return nullptr;
    };

    struct EdgeCut {
        EdgeId edge;
        VertexId other;
        core::Vec3d position{};
        VertexId created;
    };
    std::vector<EdgeCut> cuts;
    cuts.reserve(incident_edges.size());
    double minimum_length = std::numeric_limits<double>::infinity();
    for (const auto& edge : incident_edges) {
        if (!edge.second_half_edge.has_value()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "vertex bevel currently requires a closed internal manifold fan"));
        }
        const VertexId other = edge.first == id ? edge.second : edge.first;
        const auto other_iterator = vertices_.find(other);
        if (other_iterator == vertices_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "vertex bevel references a missing incident edge endpoint"));
        }
        const double length =
            (other_iterator->second.position - vertex_iterator->second.position).length();
        if (!std::isfinite(length) || length <= 1e-12) {
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel requires non-degenerate incident edges"));
        }
        minimum_length = std::min(minimum_length, length);

        const auto first_half_edge = half_edges_.find(edge.first_half_edge);
        const auto second_half_edge = half_edges_.find(*edge.second_half_edge);
        if (first_half_edge == half_edges_.end() || second_half_edge == half_edges_.end() ||
            !incident_face(first_half_edge->second.face) ||
            !incident_face(second_half_edge->second.face) ||
            first_half_edge->second.edge != edge.id ||
            second_half_edge->second.edge != edge.id) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "vertex bevel references an inconsistent incident edge"));
        }

        const core::Vec3d cut_position = vertex_iterator->second.position +
            (other_iterator->second.position - vertex_iterator->second.position) *
                (width / length);
        if (!cut_position.finite()) {
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel produced a non-finite edge cut"));
        }
        cuts.push_back(EdgeCut{
            edge.id,
            other,
            cut_position,
            {}});
    }
    if (!std::isfinite(minimum_length) || width >= 0.5 * minimum_length) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "vertex bevel width would collapse an incident face"));
    }

    for (const FaceId face_id : incident_face_ids) {
        const auto face_iterator = faces_.find(face_id);
        if (face_iterator == faces_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "vertex bevel references a missing incident face"));
        }
        const auto& face = face_iterator->second;
        if (face.vertices.size() != 3U) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "vertex bevel currently requires three incident triangular faces"));
        }
        const auto vertex_positions = std::count(face.vertices.begin(), face.vertices.end(), id);
        if (vertex_positions != 1) {
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel found an invalid repeated vertex in an incident face"));
        }
        const auto vertex_index = static_cast<std::size_t>(std::distance(
            face.vertices.begin(), std::find(face.vertices.begin(), face.vertices.end(), id)));
        const VertexId previous = face.vertices[(vertex_index + 2U) % 3U];
        const VertexId next = face.vertices[(vertex_index + 1U) % 3U];
        const EdgeRecord* previous_edge = find_incident_edge(id, previous);
        const EdgeRecord* next_edge = find_incident_edge(id, next);
        if (!previous_edge || !next_edge || previous_edge->id == next_edge->id) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "vertex bevel incident face does not match the indexed fan"));
        }
        const auto cut_for = [&cuts](EdgeId edge_id) -> const EdgeCut* {
            for (const auto& cut : cuts) {
                if (cut.edge == edge_id) return &cut;
            }
            return nullptr;
        };
        const EdgeCut* previous_cut = cut_for(previous_edge->id);
        const EdgeCut* next_cut = cut_for(next_edge->id);
        if (!previous_cut || !next_cut) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "vertex bevel could not resolve an incident edge cut"));
        }
    }

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (next_vertex_id_ > maximum - 3U || next_face_id_ == maximum) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "vertex bevel identity space is exhausted"));
    }

    // The cut points are keyed by the original edge. The temporary endpoint
    // values above let us validate the fan before allocating any IDs.
    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
    for (auto& cut : cuts) {
        cut.created = VertexId{next_vertex_id_++};
        vertices_.emplace(cut.created, Vertex{cut.created, cut.position});
    }

    const auto created_for_edge = [&cuts](EdgeId edge_id) -> std::optional<VertexId> {
        for (const auto& cut : cuts) {
            if (cut.edge == edge_id) return cut.created;
        }
        return std::nullopt;
    };
    std::map<FaceId, std::pair<VertexId, VertexId>> face_cut_ids;
    for (const FaceId face_id : incident_face_ids) {
        auto face_iterator = faces_.find(face_id);
        if (face_iterator == faces_.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "vertex bevel face disappeared during mutation"));
        }
        auto& face = face_iterator->second;
        const auto vertex_position = std::find(face.vertices.begin(), face.vertices.end(), id);
        if (vertex_position == face.vertices.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "vertex bevel face lost its selected vertex"));
        }
        const std::size_t vertex_index = static_cast<std::size_t>(
            std::distance(face.vertices.begin(), vertex_position));
        const VertexId previous = face.vertices[(vertex_index + 2U) % 3U];
        const VertexId next = face.vertices[(vertex_index + 1U) % 3U];
        const EdgeRecord* previous_edge = find_incident_edge(id, previous);
        const EdgeRecord* next_edge = find_incident_edge(id, next);
        if (!previous_edge || !next_edge) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "vertex bevel lost an incident edge during mutation"));
        }
        const auto cut_previous = created_for_edge(previous_edge->id);
        const auto cut_next = created_for_edge(next_edge->id);
        if (!cut_previous.has_value() || !cut_next.has_value()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "vertex bevel lost an edge cut during mutation"));
        }
        std::vector<VertexId> replaced;
        replaced.reserve(4U);
        for (const VertexId vertex : face.vertices) {
            if (vertex == id) {
                replaced.push_back(*cut_next);
                continue;
            }
            replaced.push_back(vertex);
            if (vertex == previous) replaced.push_back(*cut_previous);
        }
        if (replaced.size() != 4U) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel did not produce a four-sided incident face"));
        }
        face_cut_ids.emplace(face_id, std::make_pair(*cut_previous, *cut_next));
        face.vertices = std::move(replaced);
    }
    vertices_.erase(id);

    // Each incident face closes across the chamfer loop from cut_previous to
    // cut_next. The new cap must use the opposite direction on every shared
    // edge, so build a directed cycle from cut_next back to cut_previous.
    std::map<VertexId, VertexId> cap_next;
    for (const auto& [face_id, pair] : face_cut_ids) {
        static_cast<void>(face_id);
        const auto [iterator, inserted] = cap_next.emplace(pair.second, pair.first);
        if (!inserted && iterator->second != pair.first) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel chamfer loop has conflicting face directions"));
        }
    }
    if (cap_next.size() != 3U) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "vertex bevel did not produce a three-edge chamfer loop"));
    }
    std::vector<VertexId> cap_vertices;
    cap_vertices.reserve(3U);
    const VertexId start = cap_next.begin()->first;
    VertexId current = start;
    for (std::size_t count = 0U; count < cap_next.size(); ++count) {
        if (std::find(cap_vertices.begin(), cap_vertices.end(), current) != cap_vertices.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel chamfer loop repeats a cut vertex"));
        }
        cap_vertices.push_back(current);
        const auto next = cap_next.find(current);
        if (next == cap_next.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel chamfer loop lost a cut vertex"));
        }
        current = next->second;
    }
    if (current != start || cap_vertices.size() != 3U) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "vertex bevel chamfer loop does not close"));
    }
    const core::Vec3d cap_normal = core::cross(
        vertices_.at(cap_vertices[1U]).position - vertices_.at(cap_vertices[0U]).position,
        vertices_.at(cap_vertices[2U]).position - vertices_.at(cap_vertices[0U]).position);
    if (!cap_normal.normalized().finite()) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "vertex bevel produced a degenerate chamfer cap"));
    }

    const FaceId bevel_face{next_face_id_++};
    faces_.emplace(bevel_face, Face{bevel_face, std::move(cap_vertices)});
    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    bump_revision();

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    for (const auto& cut : cuts) {
        add_origin(receipt.vertex_origins, cut.created, OriginKind::interpolated_from,
            {id.value, cut.other.value});
    }
    add_origin(receipt.face_origins, bevel_face, OriginKind::generated_from_vertex, {id.value});
    for (const EdgeId created : receipt.created_edges) {
        add_origin(receipt.edge_origins, created, OriginKind::generated_from_vertex, {id.value});
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::bevel_edge(
    EdgeId id,
    double width,
    std::uint32_t segments) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!std::isfinite(width) || width <= 0.0) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "edge bevel width must be finite and strictly positive"));
    }
    if (segments != 1U) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "edge bevel currently supports exactly one segment"));
    }
    const auto edge_iterator = edges_.find(id);
    if (edge_iterator == edges_.end()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::not_found, "cannot bevel a missing mesh edge"));
    }
    const EdgeRecord edge = edge_iterator->second;
    if (!edge.second_half_edge.has_value()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "edge bevel currently requires an internal manifold edge"));
    }

    struct FaceCut {
        FaceId face;
        VertexId origin;
        VertexId destination;
        VertexId previous;
        VertexId next;
        std::size_t edge_index = 0U;
        core::Vec3d cut_origin_position{};
        core::Vec3d cut_destination_position{};
        VertexId cut_origin;
        VertexId cut_destination;
    };

    const auto validate_face_geometry = [this](const Face& face) -> core::Result<void> {
        if (face.vertices.size() < 3U) {
            return core::Result<void>::failure(validation(
                "edge bevel requires incident faces with at least three vertices"));
        }
        std::vector<core::Vec3d> positions;
        positions.reserve(face.vertices.size());
        for (const VertexId vertex : face.vertices) {
            const auto found = vertices_.find(vertex);
            if (found == vertices_.end()) {
                return core::Result<void>::failure(Diagnostic(
                    ErrorCode::stale_data,
                    "edge bevel incident face references a missing vertex"));
            }
            positions.push_back(found->second.position);
        }
        const core::Vec3d normal = core::cross(
            positions[1U] - positions[0U], positions[2U] - positions[0U]).normalized();
        if (!normal.finite()) {
            return core::Result<void>::failure(validation(
                "edge bevel requires non-degenerate planar incident faces"));
        }
        double scale = 1.0;
        for (std::size_t index = 0U; index < positions.size(); ++index) {
            scale = std::max(
                scale,
                (positions[(index + 1U) % positions.size()] - positions[index]).length());
            const double plane_distance = core::dot(positions[index] - positions[0U], normal);
            if (!std::isfinite(plane_distance) || std::abs(plane_distance) > 1e-8 * scale) {
                return core::Result<void>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "edge bevel currently requires planar incident faces"));
            }
        }
        const double area_epsilon = 1e-12 * scale * scale;
        for (std::size_t index = 0U; index < positions.size(); ++index) {
            const std::size_t next = (index + 1U) % positions.size();
            const std::size_t after_next = (index + 2U) % positions.size();
            const double turn = core::dot(
                core::cross(
                    positions[next] - positions[index],
                    positions[after_next] - positions[next]),
                normal);
            if (!std::isfinite(turn) || turn <= area_epsilon) {
                return core::Result<void>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "edge bevel currently requires strictly convex incident faces"));
            }
        }
        return core::Result<void>::success();
    };

    const auto build_face_cut = [this, &edge, width, validate_face_geometry](
        HalfEdgeId half_edge_id) -> core::Result<FaceCut> {
        const auto half_edge_iterator = half_edges_.find(half_edge_id);
        if (half_edge_iterator == half_edges_.end() || half_edge_iterator->second.edge != edge.id) {
            return core::Result<FaceCut>::failure(Diagnostic(
                ErrorCode::stale_data,
                "edge bevel references a missing or mismatched incident half-edge"));
        }
        const HalfEdgeRecord half_edge = half_edge_iterator->second;
        const auto face_iterator = faces_.find(half_edge.face);
        if (face_iterator == faces_.end()) {
            return core::Result<FaceCut>::failure(Diagnostic(
                ErrorCode::stale_data,
                "edge bevel references a missing incident face"));
        }
        if (auto result = validate_face_geometry(face_iterator->second); !result) {
            return core::Result<FaceCut>::failure(result.error());
        }

        const auto& vertices = face_iterator->second.vertices;
        std::optional<std::size_t> found_index;
        for (std::size_t index = 0U; index < vertices.size(); ++index) {
            const VertexId origin = vertices[index];
            const VertexId destination = vertices[(index + 1U) % vertices.size()];
            if (origin == half_edge.origin && destination == half_edge.destination) {
                if (found_index.has_value()) {
                    return core::Result<FaceCut>::failure(validation(
                        "edge bevel found the same directed edge more than once in a face"));
                }
                found_index = index;
            }
        }
        if (!found_index.has_value()) {
            return core::Result<FaceCut>::failure(Diagnostic(
                ErrorCode::stale_data,
                "edge bevel half-edge is not present in its authored face"));
        }

        const std::size_t index = *found_index;
        const VertexId origin = vertices[index];
        const VertexId destination = vertices[(index + 1U) % vertices.size()];
        const VertexId previous = vertices[(index + vertices.size() - 1U) % vertices.size()];
        const VertexId next = vertices[(index + 2U) % vertices.size()];
        const auto origin_iterator = vertices_.find(origin);
        const auto destination_iterator = vertices_.find(destination);
        const auto previous_iterator = vertices_.find(previous);
        const auto next_iterator = vertices_.find(next);
        if (origin_iterator == vertices_.end() || destination_iterator == vertices_.end() ||
            previous_iterator == vertices_.end() || next_iterator == vertices_.end()) {
            return core::Result<FaceCut>::failure(Diagnostic(
                ErrorCode::stale_data,
                "edge bevel incident face references a missing edge-neighbor vertex"));
        }

        const core::Vec3d origin_position = origin_iterator->second.position;
        const core::Vec3d destination_position = destination_iterator->second.position;
        const core::Vec3d previous_position = previous_iterator->second.position;
        const core::Vec3d next_position = next_iterator->second.position;
        const double edge_length = (destination_position - origin_position).length();
        const double origin_adjacent_length = (previous_position - origin_position).length();
        const double destination_adjacent_length = (next_position - destination_position).length();
        const double minimum_length = std::min({edge_length, origin_adjacent_length,
                                                destination_adjacent_length});
        if (!std::isfinite(minimum_length) || minimum_length <= 1e-12 ||
            width >= 0.5 * minimum_length) {
            return core::Result<FaceCut>::failure(Diagnostic(
                ErrorCode::invalid_argument,
                "edge bevel width would collapse an incident face"));
        }

        const double origin_factor = width / origin_adjacent_length;
        const double destination_factor = width / destination_adjacent_length;
        FaceCut result{
            half_edge.face,
            origin,
            destination,
            previous,
            next,
            index,
            origin_position + (previous_position - origin_position) * origin_factor,
            destination_position + (next_position - destination_position) * destination_factor,
            {},
            {}};
        if (!result.cut_origin_position.finite() || !result.cut_destination_position.finite() ||
            (result.cut_destination_position - result.cut_origin_position).length() <= 1e-12) {
            return core::Result<FaceCut>::failure(validation(
                "edge bevel produced a degenerate chamfer edge"));
        }
        return core::Result<FaceCut>::success(result);
    };

    auto first_cut = build_face_cut(edge.first_half_edge);
    auto second_cut = build_face_cut(*edge.second_half_edge);
    if (!first_cut) return core::Result<TopologyEditReceipt>::failure(first_cut.error());
    if (!second_cut) return core::Result<TopologyEditReceipt>::failure(second_cut.error());
    if (first_cut.value().face == second_cut.value().face ||
        first_cut.value().origin != second_cut.value().destination ||
        first_cut.value().destination != second_cut.value().origin) {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "edge bevel requires two oppositely oriented incident faces"));
    }

    const core::Vec3d bevel_a = first_cut.value().cut_origin_position;
    const core::Vec3d bevel_b = first_cut.value().cut_destination_position;
    const core::Vec3d bevel_c = second_cut.value().cut_destination_position;
    const core::Vec3d bevel_d = second_cut.value().cut_origin_position;
    const core::Vec3d bevel_normal = core::cross(bevel_a - bevel_b, bevel_c - bevel_b).normalized();
    if (!bevel_normal.finite()) {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "edge bevel produced a degenerate chamfer face"));
    }
    const double bevel_scale = std::max({1.0, (bevel_b - bevel_a).length(),
                                         (bevel_c - bevel_a).length(),
                                         (bevel_d - bevel_a).length()});
    if (std::abs(core::dot(bevel_d - bevel_a, bevel_normal)) > 1e-8 * bevel_scale) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "edge bevel currently requires a planar chamfer result"));
    }

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (next_vertex_id_ > maximum - 4U || next_face_id_ == maximum) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "edge bevel identity space is exhausted"));
    }

    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
    FaceCut first = first_cut.value();
    FaceCut second = second_cut.value();
    first.cut_origin = VertexId{next_vertex_id_++};
    first.cut_destination = VertexId{next_vertex_id_++};
    second.cut_origin = VertexId{next_vertex_id_++};
    second.cut_destination = VertexId{next_vertex_id_++};
    vertices_.emplace(first.cut_origin, Vertex{first.cut_origin, first.cut_origin_position});
    vertices_.emplace(
        first.cut_destination,
        Vertex{first.cut_destination, first.cut_destination_position});
    vertices_.emplace(second.cut_origin, Vertex{second.cut_origin, second.cut_origin_position});
    vertices_.emplace(
        second.cut_destination,
        Vertex{second.cut_destination, second.cut_destination_position});

    const auto replace_edge_in_face = [](Face& face, const FaceCut& cut) -> bool {
        std::vector<VertexId> replaced;
        replaced.reserve(face.vertices.size());
        bool found = false;
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            if (index == cut.edge_index) {
                replaced.push_back(cut.cut_origin);
                replaced.push_back(cut.cut_destination);
                found = true;
                continue;
            }
            if (index == (cut.edge_index + 1U) % face.vertices.size()) continue;
            replaced.push_back(face.vertices[index]);
        }
        if (found) face.vertices = std::move(replaced);
        return found;
    };
    auto first_face = faces_.find(first.face);
    auto second_face = faces_.find(second.face);
    if (first_face == faces_.end() || second_face == faces_.end() ||
        !replace_edge_in_face(first_face->second, first) ||
        !replace_edge_in_face(second_face->second, second)) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::stale_data, "edge bevel face disappeared during mutation"));
    }

    const FaceId bevel_face{next_face_id_++};
    faces_.emplace(bevel_face, Face{
        bevel_face,
        {first.cut_destination, first.cut_origin,
         second.cut_destination, second.cut_origin}});

    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    bump_revision();

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    add_origin(receipt.vertex_origins, first.cut_origin, OriginKind::interpolated_from,
        {first.origin.value, first.previous.value});
    add_origin(receipt.vertex_origins, first.cut_destination, OriginKind::interpolated_from,
        {first.destination.value, first.next.value});
    add_origin(receipt.vertex_origins, second.cut_origin, OriginKind::interpolated_from,
        {second.origin.value, second.previous.value});
    add_origin(receipt.vertex_origins, second.cut_destination, OriginKind::interpolated_from,
        {second.destination.value, second.next.value});
    add_origin(receipt.face_origins, bevel_face, OriginKind::generated_from_edge, {id.value});
    for (const EdgeId created : receipt.created_edges) {
        add_origin(receipt.edge_origins, created, OriginKind::generated_from_edge, {id.value});
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::dissolve_edge(EdgeId id) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    const auto edge_iterator = edges_.find(id);
    if (edge_iterator == edges_.end()) {
        return core::Result<TopologyEditReceipt>::failure(
            Diagnostic(ErrorCode::not_found, "cannot dissolve a missing mesh edge"));
    }
    const EdgeRecord edge = edge_iterator->second;
    if (!edge.second_half_edge.has_value()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "edge dissolve currently requires an internal manifold edge"));
    }

    const auto first_iterator = half_edges_.find(edge.first_half_edge);
    const auto second_iterator = half_edges_.find(*edge.second_half_edge);
    if (first_iterator == half_edges_.end() || second_iterator == half_edges_.end() ||
        first_iterator->second.edge != id || second_iterator->second.edge != id) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::stale_data,
            "edge dissolve references a missing or mismatched half-edge"));
    }
    const HalfEdgeRecord first = first_iterator->second;
    const HalfEdgeRecord second = second_iterator->second;
    if (first.face == second.face || first.origin != second.destination ||
        first.destination != second.origin || !first.twin.has_value() ||
        !second.twin.has_value() || *first.twin != second.id || *second.twin != first.id) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "edge dissolve found inconsistent internal edge adjacency"));
    }

    const auto first_face_iterator = faces_.find(first.face);
    const auto second_face_iterator = faces_.find(second.face);
    if (first_face_iterator == faces_.end() || second_face_iterator == faces_.end()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::stale_data,
            "edge dissolve references a missing incident face"));
    }

    // The first half-edge is first.origin -> first.destination. The merged
    // boundary walks the second face from first.origin to first.destination,
    // then the first face from first.destination back to first.origin. This
    // deliberately walks the outer paths and omits the dissolved edge.
    const auto outer_path = [](const Face& face, VertexId start, VertexId end)
        -> std::optional<std::vector<VertexId>> {
        const auto start_iterator = std::find(face.vertices.begin(), face.vertices.end(), start);
        if (start_iterator == face.vertices.end()) return std::nullopt;
        const std::size_t start_index = static_cast<std::size_t>(
            std::distance(face.vertices.begin(), start_iterator));
        std::vector<VertexId> path;
        path.reserve(face.vertices.size());
        for (std::size_t step = 0U; step < face.vertices.size(); ++step) {
            const VertexId vertex = face.vertices[(start_index + step) % face.vertices.size()];
            path.push_back(vertex);
            if (vertex == end) return path;
        }
        return std::nullopt;
    };

    const auto second_path = outer_path(
        second_face_iterator->second, first.origin, first.destination);
    const auto first_path = outer_path(
        first_face_iterator->second, first.destination, first.origin);
    if (!second_path.has_value() || !first_path.has_value() ||
        second_path->size() < 2U || first_path->size() < 2U) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "edge dissolve could not construct both incident face boundary paths"));
    }

    std::vector<VertexId> merged_vertices = *second_path;
    merged_vertices.insert(
        merged_vertices.end(), first_path->begin() + 1, first_path->end() - 1);
    if (merged_vertices.size() < 3U ||
        std::set<VertexId>(merged_vertices.begin(), merged_vertices.end()).size() !=
            merged_vertices.size()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "edge dissolve would create a non-simple merged boundary"));
    }

    std::vector<core::Vec3d> positions;
    positions.reserve(merged_vertices.size());
    double scale = 1.0;
    for (std::size_t index = 0U; index < merged_vertices.size(); ++index) {
        const auto vertex_iterator = vertices_.find(merged_vertices[index]);
        if (vertex_iterator == vertices_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "edge dissolve merged boundary references a missing vertex"));
        }
        positions.push_back(vertex_iterator->second.position);
        const core::Vec3d edge_vector = vertex_iterator->second.position -
            vertices_.at(merged_vertices[(index + merged_vertices.size() - 1U) %
                                         merged_vertices.size()]).position;
        scale = std::max(scale, edge_vector.length());
    }

    const core::Vec3d normal = core::cross(
        positions[1U] - positions[0U], positions[2U] - positions[0U]).normalized();
    if (!normal.finite()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "edge dissolve requires non-degenerate incident face geometry"));
    }
    const double plane_tolerance = 1e-8 * scale;
    for (const core::Vec3d position : positions) {
        const double plane_distance = core::dot(position - positions.front(), normal);
        if (!std::isfinite(plane_distance) || std::abs(plane_distance) > plane_tolerance) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "edge dissolve currently requires coplanar incident faces"));
        }
    }

    const double convexity_epsilon = 1e-12 * scale * scale;
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        const std::size_t next = (index + 1U) % positions.size();
        const std::size_t after_next = (index + 2U) % positions.size();
        const double turn = core::dot(
            core::cross(
                positions[next] - positions[index],
                positions[after_next] - positions[next]),
            normal);
        if (!std::isfinite(turn) || turn < -convexity_epsilon) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "edge dissolve currently requires a convex merged boundary"));
        }
        if (index >= 1U && index + 1U < positions.size() &&
            triangle_area_squared(
                positions.front(), positions[index], positions[index + 1U]) <= 1e-24) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "edge dissolve would create a zero-area compiled triangle"));
        }
    }

    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
    const FaceId survivor = std::min(first.face, second.face);
    const FaceId removed = std::max(first.face, second.face);
    const auto before_topology = before.topology();
    if (!before_topology) {
        return core::Result<TopologyEditReceipt>::failure(before_topology.error());
    }

    faces_.erase(first.face);
    faces_.erase(second.face);
    faces_.emplace(survivor, Face{survivor, std::move(merged_vertices)});

    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    bump_revision();

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    // The lower stable face identity survives the join. Newly-created corner
    // identities on that face can still carry the removed face's corner
    // lineage without pretending that the existing face identity was newly
    // generated.
    const auto after_topology = topology();
    if (!after_topology) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(after_topology.error());
    }
    for (const auto& corner : after_topology.value().corners) {
        if (!std::binary_search(receipt.created_corners.begin(), receipt.created_corners.end(),
                                corner.id) || corner.face != survivor) {
            continue;
        }
        const auto source = std::find_if(
            before_topology.value().corners.begin(), before_topology.value().corners.end(),
            [removed, &corner](const CornerRecord& candidate) {
                return candidate.face == removed && candidate.vertex == corner.vertex;
            });
        if (source != before_topology.value().corners.end()) {
            add_origin(receipt.corner_origins, corner.id, OriginKind::duplicated_from,
                       {source->id.value});
        }
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::tri_to_quad(
    FaceId first,
    FaceId second) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!first || !second || first == second) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "triangle-to-quad conversion requires two distinct face identities"));
    }
    const auto first_iterator = faces_.find(first);
    const auto second_iterator = faces_.find(second);
    if (first_iterator == faces_.end() || second_iterator == faces_.end()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::not_found,
            "triangle-to-quad conversion references a missing face"));
    }
    if (first_iterator->second.vertices.size() != 3U ||
        second_iterator->second.vertices.size() != 3U) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "triangle-to-quad conversion requires exactly two authored triangles"));
    }

    std::optional<EdgeId> shared_edge;
    for (const auto& [edge_id, edge] : edges_) {
        if (!edge.second_half_edge.has_value()) continue;
        const auto first_half_edge = half_edges_.find(edge.first_half_edge);
        const auto second_half_edge = half_edges_.find(*edge.second_half_edge);
        if (first_half_edge == half_edges_.end() || second_half_edge == half_edges_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "triangle-to-quad conversion found a missing edge half-edge"));
        }
        const bool matches =
            (first_half_edge->second.face == first && second_half_edge->second.face == second) ||
            (first_half_edge->second.face == second && second_half_edge->second.face == first);
        if (!matches) continue;
        if (shared_edge.has_value()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "triangle-to-quad conversion requires exactly one shared edge"));
        }
        shared_edge = edge_id;
    }
    if (!shared_edge.has_value()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "triangle-to-quad conversion requires one internal shared edge"));
    }
    return dissolve_edge(*shared_edge);
}

core::Result<TopologyEditReceipt> EditableMesh::merge_vertices(
    VertexId target,
    VertexId source) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!target || !source || target == source) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "vertex merge requires two distinct non-zero vertex identities"));
    }
    if (!find_vertex(target) || !find_vertex(source)) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::not_found, "vertex merge references a missing mesh vertex"));
    }

    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
    std::map<FaceId, Face> merged_faces;
    for (const auto& [face_id, face] : faces_) {
        std::vector<VertexId> collapsed;
        collapsed.reserve(face.vertices.size());
        for (const VertexId vertex : face.vertices) {
            const VertexId replacement = vertex == source ? target : vertex;
            if (collapsed.empty() || collapsed.back() != replacement) {
                collapsed.push_back(replacement);
            }
        }
        if (collapsed.size() > 1U && collapsed.front() == collapsed.back()) {
            collapsed.pop_back();
        }

        if (collapsed.size() < 3U) {
            // Collapsing an edge of a face removes that degenerate face. The
            // receipt records the deletion so downstream attribute transfer
            // can invalidate face/corner data instead of guessing.
            continue;
        }
        std::set<VertexId> unique_vertices(collapsed.begin(), collapsed.end());
        if (unique_vertices.size() != collapsed.size()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "vertex merge would create a non-simple face; no mutation was published"));
        }
        merged_faces.emplace(face_id, Face{face_id, std::move(collapsed)});
    }

    faces_ = std::move(merged_faces);
    vertices_.erase(source);
    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    bump_revision();

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    receipt.merged_vertices.emplace(source, target);
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::inset_face(FaceId id, double distance) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
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

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    std::map<VertexId, std::vector<VertexId>> source_vertices;
    for (std::size_t index = 0U; index < inset_vertices.size(); ++index) {
        const std::size_t previous = (index + vertex_count - 1U) % vertex_count;
        const std::size_t next = (index + 1U) % vertex_count;
        source_vertices.emplace(inset_vertices[index], std::vector<VertexId>{
            original.vertices[previous], original.vertices[index], original.vertices[next]});
        add_origin(receipt.vertex_origins, inset_vertices[index],
            OriginKind::interpolated_from,
            {original.vertices[previous].value, original.vertices[index].value,
             original.vertices[next].value});
    }
    for (const FaceId created : receipt.created_faces) {
        add_origin(receipt.face_origins, created, OriginKind::generated_from_face, {id.value});
    }

    const auto before_topology = before.topology();
    const auto after_topology = topology();
    if (!before_topology || !after_topology) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "inset receipt topology snapshot failed validation"));
    }
    std::map<EdgeKey, EdgeId> source_edges;
    for (const auto& edge : before_topology.value().edges) {
        source_edges.emplace(undirected(edge.first, edge.second), edge.id);
    }
    const auto expanded_vertices = [&source_vertices](VertexId vertex) {
        const auto found = source_vertices.find(vertex);
        return found == source_vertices.end()
            ? std::vector<VertexId>{vertex} : found->second;
    };
    const auto source_edge_for = [&source_edges](
        const std::vector<VertexId>& first_sources,
        const std::vector<VertexId>& second_sources) -> std::optional<EdgeId> {
        for (const VertexId first : first_sources) {
            for (const VertexId second : second_sources) {
                if (first == second) continue;
                const auto source = source_edges.find(undirected(first, second));
                if (source != source_edges.end()) return source->second;
            }
        }
        for (const auto& [key, source] : source_edges) {
            const bool first_matches = std::find(
                first_sources.begin(), first_sources.end(), key.first) != first_sources.end() ||
                std::find(first_sources.begin(), first_sources.end(), key.second) != first_sources.end();
            const bool second_matches = std::find(
                second_sources.begin(), second_sources.end(), key.first) != second_sources.end() ||
                std::find(second_sources.begin(), second_sources.end(), key.second) != second_sources.end();
            if (first_matches || second_matches) return source;
        }
        return std::nullopt;
    };
    for (const auto& edge : after_topology.value().edges) {
        if (!std::binary_search(receipt.created_edges.begin(), receipt.created_edges.end(), edge.id)) {
            continue;
        }
        const auto source = source_edge_for(
            expanded_vertices(edge.first), expanded_vertices(edge.second));
        if (source.has_value()) {
            add_origin(receipt.edge_origins, edge.id, OriginKind::generated_from_edge,
                {source->value});
        }
    }

    std::map<std::pair<FaceId, VertexId>, CornerId> source_corners;
    for (const auto& corner : before_topology.value().corners) {
        source_corners.emplace(std::make_pair(corner.face, corner.vertex), corner.id);
    }
    for (const auto& corner : after_topology.value().corners) {
        if (!std::binary_search(receipt.created_corners.begin(), receipt.created_corners.end(), corner.id)) {
            continue;
        }
        const auto face_origin = receipt.face_origins.find(corner.face);
        if (face_origin == receipt.face_origins.end()) continue;
        const FaceId source_face{face_origin->second.source_ids.front()};
        const auto vertex_sources = expanded_vertices(corner.vertex);
        std::vector<std::uint64_t> source_ids;
        for (const VertexId vertex : vertex_sources) {
            const auto source = source_corners.find(std::make_pair(source_face, vertex));
            if (source != source_corners.end()) source_ids.push_back(source->second.value);
        }
        if (source_ids.empty()) continue;
        if (source_ids.size() == 1U) {
            add_origin(receipt.corner_origins, corner.id, OriginKind::duplicated_from,
                std::move(source_ids));
        } else {
            add_origin(receipt.corner_origins, corner.id, OriginKind::interpolated_from,
                std::move(source_ids));
        }
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::poke_face(FaceId id) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    const auto face_iterator = faces_.find(id);
    if (face_iterator == faces_.end()) {
        return core::Result<TopologyEditReceipt>::failure(
            Diagnostic(ErrorCode::not_found, "cannot poke a missing mesh face"));
    }
    const Face original = face_iterator->second;
    const std::size_t vertex_count = original.vertices.size();
    if (vertex_count < 3U) {
        return core::Result<TopologyEditReceipt>::failure(
            validation("face poke requires at least three boundary vertices"));
    }

    std::vector<core::Vec3d> positions;
    positions.reserve(vertex_count);
    double scale = 1.0;
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const auto vertex = vertices_.find(original.vertices[index]);
        if (vertex == vertices_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "face poke references a missing mesh vertex"));
        }
        positions.push_back(vertex->second.position);
        const auto next_vertex = vertices_.find(original.vertices[(index + 1U) % vertex_count]);
        if (next_vertex == vertices_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "face poke references a missing boundary vertex"));
        }
        const core::Vec3d edge = next_vertex->second.position - vertex->second.position;
        scale = std::max(scale, edge.length());
    }
    const core::Vec3d raw_normal = core::cross(
        positions[1U] - positions[0U], positions[2U] - positions[0U]);
    const core::Vec3d normal = raw_normal.normalized();
    if (!normal.finite()) {
        return core::Result<TopologyEditReceipt>::failure(
            validation("face poke requires a non-degenerate planar face"));
    }
    const double plane_tolerance = 1e-8 * scale;
    for (const core::Vec3d position : positions) {
        const double plane_distance = core::dot(position - positions.front(), normal);
        if (!std::isfinite(plane_distance) || std::abs(plane_distance) > plane_tolerance) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "face poke currently requires a planar face"));
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
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "face poke currently requires a strictly convex face"));
        }
    }

    core::Vec3d center{};
    for (const core::Vec3d position : positions) center = center + position;
    center = center * (1.0 / static_cast<double>(vertex_count));
    if (!center.finite()) {
        return core::Result<TopologyEditReceipt>::failure(
            validation("face poke produced a non-finite centroid"));
    }
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const std::size_t next = (index + 1U) % vertex_count;
        const double inside = core::dot(
            core::cross(positions[next] - positions[index], center - positions[index]),
            normal);
        if (!std::isfinite(inside) || inside <= area_epsilon) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "face poke centroid is not strictly inside the face"));
        }
    }

    const std::uint64_t new_face_count = static_cast<std::uint64_t>(vertex_count);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (next_vertex_id_ > maximum - 1U) {
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

    const VertexId center_vertex{next_vertex_id_++};
    vertices_.emplace(center_vertex, Vertex{center_vertex, center});
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const std::size_t next = (index + 1U) % vertex_count;
        const FaceId triangle{next_face_id_++};
        faces_.emplace(triangle, Face{
            triangle,
            {original.vertices[index], original.vertices[next], center_vertex},
        });
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

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    add_origin(receipt.vertex_origins, center_vertex, OriginKind::interpolated_from,
        [&original]() {
            std::vector<std::uint64_t> sources;
            sources.reserve(original.vertices.size());
            for (const VertexId vertex : original.vertices) sources.push_back(vertex.value);
            return sources;
        }());
    for (const FaceId created : receipt.created_faces) {
        add_origin(receipt.face_origins, created, OriginKind::generated_from_face, {id.value});
    }

    const auto before_topology = before.topology();
    const auto after_topology = topology();
    if (!before_topology || !after_topology) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "face poke receipt topology snapshot failed validation"));
    }
    std::map<std::pair<FaceId, VertexId>, CornerId> source_corners;
    for (const auto& corner : before_topology.value().corners) {
        source_corners.emplace(std::make_pair(corner.face, corner.vertex), corner.id);
    }
    for (const auto& edge : after_topology.value().edges) {
        if (!std::binary_search(receipt.created_edges.begin(), receipt.created_edges.end(), edge.id)) {
            continue;
        }
        add_origin(receipt.edge_origins, edge.id, OriginKind::generated_from_face, {id.value});
    }
    for (const auto& corner : after_topology.value().corners) {
        if (!std::binary_search(
                receipt.created_corners.begin(), receipt.created_corners.end(), corner.id)) {
            continue;
        }
        if (corner.vertex == center_vertex) {
            std::vector<std::uint64_t> sources;
            sources.reserve(original.vertices.size());
            for (const VertexId vertex : original.vertices) {
                const auto source = source_corners.find(std::make_pair(id, vertex));
                if (source != source_corners.end()) sources.push_back(source->second.value);
            }
            if (!sources.empty()) {
                add_origin(receipt.corner_origins, corner.id,
                    OriginKind::interpolated_from, std::move(sources));
            }
        } else {
            const auto source = source_corners.find(std::make_pair(id, corner.vertex));
            if (source != source_corners.end()) {
                add_origin(receipt.corner_origins, corner.id,
                    OriginKind::duplicated_from, {source->second.value});
            }
        }
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
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
    attributes_ = source.attributes_;
    uv_sets_ = source.uv_sets_;
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
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<FaceId>::failure(result.error());
    }
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
    if (auto result = reject_topology_edit_with_attributes(); !result) return result;
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
    if (auto result = reject_topology_edit_with_attributes(); !result) return result;
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
        for (VertexId vertex : face.vertices) {
            if (!vertex || !vertices_.contains(vertex)) {
                return core::Result<void>::failure(
                    validation("mesh face has a dangling or duplicate vertex"));
            }
        }
        if (has_duplicate_ids(face.vertices)) {
            return core::Result<void>::failure(
                validation("mesh face has a dangling or duplicate vertex"));
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
    if (auto result = validate_topology_state(); !result) {
        return result;
    }
    return validate_attribute_payload();
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

core::Result<std::vector<EdgeId>> TopologySnapshot::edge_loop(EdgeId edge) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<EdgeId>>::failure(result.error());
    }
    if (!edge) {
        return core::Result<std::vector<EdgeId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology edge id must be non-zero"));
    }

    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(half_edges.size());
    for (const auto& half_edge : half_edges) by_id.emplace(half_edge.id, &half_edge);
    const auto edge_iterator = std::find_if(
        edges.begin(), edges.end(), [edge](const EdgeRecord& record) { return record.id == edge; });
    if (edge_iterator == edges.end()) {
        return core::Result<std::vector<EdgeId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology edge loop was not found"));
    }

    const auto walk = [&by_id, this](HalfEdgeId start) -> core::Result<std::vector<EdgeId>> {
        std::vector<EdgeId> result;
        std::unordered_set<HalfEdgeId, IdHash<HalfEdgeId>> visited_half_edges;
        std::unordered_set<EdgeId, IdHash<EdgeId>> visited_edges;
        visited_half_edges.reserve(half_edges.size());
        visited_edges.reserve(edges.size());
        HalfEdgeId current = start;
        for (std::size_t step = 0U; step <= half_edges.size(); ++step) {
            if (!visited_half_edges.insert(current).second) {
                return core::Result<std::vector<EdgeId>>::success(std::move(result));
            }
            const auto current_iterator = by_id.find(current);
            if (current_iterator == by_id.end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("edge loop references a missing half-edge"));
            }
            const HalfEdgeRecord& current_record = *current_iterator->second;
            if (result.empty()) {
                visited_edges.insert(current_record.edge);
                result.push_back(current_record.edge);
            }

            const auto boundary = face_boundary(current_record.face);
            if (!boundary) {
                return core::Result<std::vector<EdgeId>>::failure(boundary.error());
            }
            if (boundary.value().size() != 4U) {
                return core::Result<std::vector<EdgeId>>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "edge loop traversal currently requires quadrilateral faces"));
            }
            const auto position = std::find(
                boundary.value().begin(), boundary.value().end(), current_record.id);
            if (position == boundary.value().end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("edge loop half-edge is absent from its face boundary"));
            }
            const std::size_t index = static_cast<std::size_t>(
                std::distance(boundary.value().begin(), position));
            const HalfEdgeId opposite_id = boundary.value()[(index + 2U) % 4U];
            const auto opposite_iterator = by_id.find(opposite_id);
            if (opposite_iterator == by_id.end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("edge loop opposite half-edge is missing"));
            }
            if (!visited_edges.insert(opposite_iterator->second->edge).second) {
                return core::Result<std::vector<EdgeId>>::success(std::move(result));
            }
            result.push_back(opposite_iterator->second->edge);
            if (!opposite_iterator->second->twin.has_value()) {
                return core::Result<std::vector<EdgeId>>::success(std::move(result));
            }
            current = *opposite_iterator->second->twin;
        }
        return core::Result<std::vector<EdgeId>>::failure(
            validation("edge loop exceeds the available half-edges"));
    };

    const auto forward = walk(edge_iterator->first_half_edge);
    if (!forward) return forward;
    if (!edge_iterator->second_half_edge.has_value()) return forward;

    const auto backward = walk(*edge_iterator->second_half_edge);
    if (!backward) return backward;
    std::vector<EdgeId> result;
    result.reserve(backward.value().size() + forward.value().size());
    std::unordered_set<EdgeId, IdHash<EdgeId>> appended;
    appended.reserve(backward.value().size() + forward.value().size());
    for (auto iterator = backward.value().rbegin(); iterator != backward.value().rend(); ++iterator) {
        if (*iterator != edge && appended.insert(*iterator).second) result.push_back(*iterator);
    }
    for (const EdgeId item : forward.value()) {
        if (appended.insert(item).second) result.push_back(item);
    }
    return core::Result<std::vector<EdgeId>>::success(std::move(result));
}

core::Result<std::vector<EdgeId>> TopologySnapshot::edge_ring(EdgeId edge) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<EdgeId>>::failure(result.error());
    }
    if (!edge) {
        return core::Result<std::vector<EdgeId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology edge id must be non-zero"));
    }

    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(half_edges.size());
    for (const auto& half_edge : half_edges) by_id.emplace(half_edge.id, &half_edge);
    const auto edge_iterator = std::find_if(
        edges.begin(), edges.end(), [edge](const EdgeRecord& record) { return record.id == edge; });
    if (edge_iterator == edges.end()) {
        return core::Result<std::vector<EdgeId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology edge ring was not found"));
    }

    const auto walk = [&by_id, this](HalfEdgeId start) -> core::Result<std::vector<EdgeId>> {
        std::vector<EdgeId> result;
        std::unordered_set<HalfEdgeId, IdHash<HalfEdgeId>> visited_half_edges;
        std::unordered_set<EdgeId, IdHash<EdgeId>> visited_edges;
        visited_half_edges.reserve(half_edges.size());
        visited_edges.reserve(edges.size());
        HalfEdgeId current = start;
        for (std::size_t step = 0U; step <= half_edges.size(); ++step) {
            if (!visited_half_edges.insert(current).second) {
                return core::Result<std::vector<EdgeId>>::success(std::move(result));
            }
            const auto current_iterator = by_id.find(current);
            if (current_iterator == by_id.end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("edge ring references a missing half-edge"));
            }
            const HalfEdgeRecord& current_record = *current_iterator->second;
            if (result.empty()) {
                visited_edges.insert(current_record.edge);
                result.push_back(current_record.edge);
            }

            const auto boundary = face_boundary(current_record.face);
            if (!boundary) {
                return core::Result<std::vector<EdgeId>>::failure(boundary.error());
            }
            if (boundary.value().size() != 4U) {
                return core::Result<std::vector<EdgeId>>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "edge ring traversal currently requires quadrilateral faces"));
            }
            const auto position = std::find(
                boundary.value().begin(), boundary.value().end(), current_record.id);
            if (position == boundary.value().end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("edge ring half-edge is absent from its face boundary"));
            }
            const std::size_t index = static_cast<std::size_t>(
                std::distance(boundary.value().begin(), position));
            const HalfEdgeId side_id = boundary.value()[(index + 1U) % 4U];
            const auto side_iterator = by_id.find(side_id);
            if (side_iterator == by_id.end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("edge ring side half-edge is missing"));
            }
            if (!visited_edges.insert(side_iterator->second->edge).second) {
                return core::Result<std::vector<EdgeId>>::success(std::move(result));
            }
            result.push_back(side_iterator->second->edge);
            if (!side_iterator->second->twin.has_value()) {
                return core::Result<std::vector<EdgeId>>::success(std::move(result));
            }
            current = *side_iterator->second->twin;
        }
        return core::Result<std::vector<EdgeId>>::failure(
            validation("edge ring exceeds the available half-edges"));
    };

    const auto forward = walk(edge_iterator->first_half_edge);
    if (!forward) return forward;
    if (!edge_iterator->second_half_edge.has_value()) return forward;
    const auto backward = walk(*edge_iterator->second_half_edge);
    if (!backward) return backward;

    std::vector<EdgeId> result;
    result.reserve(backward.value().size() + forward.value().size());
    std::unordered_set<EdgeId, IdHash<EdgeId>> appended;
    appended.reserve(backward.value().size() + forward.value().size());
    for (auto iterator = backward.value().rbegin(); iterator != backward.value().rend(); ++iterator) {
        if (*iterator != edge && appended.insert(*iterator).second) result.push_back(*iterator);
    }
    for (const EdgeId item : forward.value()) {
        if (appended.insert(item).second) result.push_back(item);
    }
    return core::Result<std::vector<EdgeId>>::success(std::move(result));
}

core::Result<std::vector<EdgeId>> TopologySnapshot::boundary_loop(EdgeId edge) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<EdgeId>>::failure(result.error());
    }
    if (!edge) {
        return core::Result<std::vector<EdgeId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology edge id must be non-zero"));
    }

    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(half_edges.size());
    for (const auto& half_edge : half_edges) by_id.emplace(half_edge.id, &half_edge);
    const auto edge_iterator = std::find_if(
        edges.begin(), edges.end(), [edge](const EdgeRecord& record) { return record.id == edge; });
    if (edge_iterator == edges.end()) {
        return core::Result<std::vector<EdgeId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology boundary loop edge was not found"));
    }
    HalfEdgeId start = edge_iterator->first_half_edge;
    const auto first_iterator = by_id.find(start);
    if (first_iterator == by_id.end()) {
        return core::Result<std::vector<EdgeId>>::failure(
            validation("topology boundary loop edge references a missing half-edge"));
    }
    if (first_iterator->second->twin.has_value()) {
        if (!edge_iterator->second_half_edge.has_value()) {
            return core::Result<std::vector<EdgeId>>::failure(
                validation("topology boundary edge has an inconsistent twin record"));
        }
        start = *edge_iterator->second_half_edge;
    }
    const auto start_iterator = by_id.find(start);
    if (start_iterator == by_id.end() || start_iterator->second->twin.has_value()) {
        return core::Result<std::vector<EdgeId>>::failure(Diagnostic(
            ErrorCode::unsupported, "boundary loop traversal requires a boundary edge"));
    }

    std::vector<EdgeId> result;
    std::unordered_set<HalfEdgeId, IdHash<HalfEdgeId>> visited;
    visited.reserve(half_edges.size());
    HalfEdgeId current = start;
    for (std::size_t step = 0U; step <= half_edges.size(); ++step) {
        if (current == start && !result.empty()) {
            return core::Result<std::vector<EdgeId>>::success(std::move(result));
        }
        if (!visited.insert(current).second) {
            return core::Result<std::vector<EdgeId>>::failure(
                validation("boundary loop repeats before closing"));
        }
        const auto current_iterator = by_id.find(current);
        if (current_iterator == by_id.end() || current_iterator->second->twin.has_value()) {
            return core::Result<std::vector<EdgeId>>::failure(
                validation("boundary loop crossed an internal half-edge"));
        }
        const HalfEdgeRecord& current_record = *current_iterator->second;
        result.push_back(current_record.edge);

        const auto next_iterator = by_id.find(current_record.next);
        if (next_iterator == by_id.end()) {
            return core::Result<std::vector<EdgeId>>::failure(
                validation("boundary loop references a missing next half-edge"));
        }
        const HalfEdgeRecord* candidate = next_iterator->second;
        std::unordered_set<HalfEdgeId, IdHash<HalfEdgeId>> transition_visited;
        transition_visited.reserve(half_edges.size());
        while (candidate->twin.has_value()) {
            if (!transition_visited.insert(candidate->id).second) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("boundary loop cannot reach its next boundary half-edge"));
            }
            const auto twin_iterator = by_id.find(*candidate->twin);
            if (twin_iterator == by_id.end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("boundary loop references a missing twin half-edge"));
            }
            const auto after_twin_iterator = by_id.find(twin_iterator->second->next);
            if (after_twin_iterator == by_id.end()) {
                return core::Result<std::vector<EdgeId>>::failure(
                    validation("boundary loop references a missing post-twin half-edge"));
            }
            candidate = after_twin_iterator->second;
        }
        current = candidate->id;
    }
    return core::Result<std::vector<EdgeId>>::failure(
        validation("boundary loop exceeds the available half-edges"));
}

core::Result<std::vector<FaceId>> TopologySnapshot::vertex_fan(VertexId vertex) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<FaceId>>::failure(result.error());
    }
    if (!vertex) {
        return core::Result<std::vector<FaceId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology vertex id must be non-zero"));
    }

    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(half_edges.size());
    std::vector<const HalfEdgeRecord*> outgoing;
    for (const auto& half_edge : half_edges) {
        by_id.emplace(half_edge.id, &half_edge);
        if (half_edge.origin == vertex) outgoing.push_back(&half_edge);
    }
    if (outgoing.empty()) {
        return core::Result<std::vector<FaceId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology vertex fan was not found"));
    }
    std::sort(outgoing.begin(), outgoing.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });

    const HalfEdgeRecord* start = outgoing.front();
    for (const auto* candidate : outgoing) {
        const auto previous = by_id.find(candidate->previous);
        if (previous == by_id.end()) {
            return core::Result<std::vector<FaceId>>::failure(
                validation("vertex fan references a missing previous half-edge"));
        }
        if (!previous->second->twin.has_value()) {
            start = candidate;
            break;
        }
    }

    std::vector<FaceId> result;
    std::unordered_set<HalfEdgeId, IdHash<HalfEdgeId>> visited;
    visited.reserve(outgoing.size());
    const HalfEdgeRecord* current = start;
    for (std::size_t step = 0U; step <= outgoing.size(); ++step) {
        if (!visited.insert(current->id).second) {
            if (current->id == start->id && visited.size() == outgoing.size()) {
                return core::Result<std::vector<FaceId>>::success(std::move(result));
            }
            return core::Result<std::vector<FaceId>>::failure(
                Diagnostic(ErrorCode::unsupported, "vertex fan is non-manifold"));
        }
        result.push_back(current->face);
        const auto previous = by_id.find(current->previous);
        if (previous == by_id.end()) {
            return core::Result<std::vector<FaceId>>::failure(
                validation("vertex fan references a missing previous half-edge"));
        }
        if (!previous->second->twin.has_value()) {
            if (visited.size() != outgoing.size()) {
                return core::Result<std::vector<FaceId>>::failure(
                    Diagnostic(ErrorCode::unsupported, "vertex fan is non-manifold"));
            }
            return core::Result<std::vector<FaceId>>::success(std::move(result));
        }
        const auto next = by_id.find(*previous->second->twin);
        if (next == by_id.end() || next->second->origin != vertex) {
            return core::Result<std::vector<FaceId>>::failure(
                validation("vertex fan twin does not originate at the requested vertex"));
        }
        current = next->second;
    }
    return core::Result<std::vector<FaceId>>::failure(
        validation("vertex fan exceeds the available incident half-edges"));
}

core::Result<std::vector<FaceId>> TopologySnapshot::face_region(FaceId face) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<FaceId>>::failure(result.error());
    }
    if (!face) {
        return core::Result<std::vector<FaceId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology face id must be non-zero"));
    }

    std::unordered_map<HalfEdgeId, const HalfEdgeRecord*, IdHash<HalfEdgeId>> by_id;
    by_id.reserve(half_edges.size());
    std::map<FaceId, std::vector<FaceId>> adjacency;
    for (const auto& half_edge : half_edges) {
        by_id.emplace(half_edge.id, &half_edge);
        adjacency.try_emplace(half_edge.face);
    }
    if (!adjacency.contains(face)) {
        return core::Result<std::vector<FaceId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology face region seed was not found"));
    }
    for (const auto& half_edge : half_edges) {
        if (!half_edge.twin.has_value()) continue;
        const auto twin = by_id.find(*half_edge.twin);
        if (twin == by_id.end()) {
            return core::Result<std::vector<FaceId>>::failure(
                validation("topology face region references a missing twin half-edge"));
        }
        if (twin->second->face != half_edge.face) {
            adjacency[half_edge.face].push_back(twin->second->face);
        }
    }
    for (auto& [ignored, neighbors] : adjacency) {
        static_cast<void>(ignored);
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    std::vector<FaceId> result;
    std::vector<FaceId> pending{face};
    std::set<FaceId> visited{face};
    for (std::size_t index = 0U; index < pending.size(); ++index) {
        const FaceId current = pending[index];
        result.push_back(current);
        for (const FaceId neighbor : adjacency.at(current)) {
            if (visited.insert(neighbor).second) pending.push_back(neighbor);
        }
    }
    return core::Result<std::vector<FaceId>>::success(std::move(result));
}

core::Result<std::vector<VertexId>> TopologySnapshot::linked_component(VertexId vertex) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<VertexId>>::failure(result.error());
    }
    if (!vertex) {
        return core::Result<std::vector<VertexId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology vertex id must be non-zero"));
    }

    std::map<VertexId, std::vector<VertexId>> adjacency;
    for (const auto& edge : edges) {
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }
    if (!adjacency.contains(vertex)) {
        return core::Result<std::vector<VertexId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology linked-component seed was not found"));
    }
    for (auto& [ignored, neighbors] : adjacency) {
        static_cast<void>(ignored);
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    std::vector<VertexId> result;
    std::vector<VertexId> pending{vertex};
    std::set<VertexId> visited{vertex};
    for (std::size_t index = 0U; index < pending.size(); ++index) {
        const VertexId current = pending[index];
        result.push_back(current);
        for (const VertexId neighbor : adjacency.at(current)) {
            if (visited.insert(neighbor).second) pending.push_back(neighbor);
        }
    }
    return core::Result<std::vector<VertexId>>::success(std::move(result));
}

core::Result<std::vector<VertexId>> TopologySnapshot::shortest_path(
    VertexId start, VertexId goal) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<VertexId>>::failure(result.error());
    }
    if (!start || !goal) {
        return core::Result<std::vector<VertexId>>::failure(
            Diagnostic(ErrorCode::invalid_argument, "topology path vertex ids must be non-zero"));
    }

    std::map<VertexId, std::vector<VertexId>> adjacency;
    for (const auto& edge : edges) {
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }
    if (!adjacency.contains(start) || !adjacency.contains(goal)) {
        return core::Result<std::vector<VertexId>>::failure(
            Diagnostic(ErrorCode::not_found, "topology shortest-path endpoint was not found"));
    }
    if (start == goal) {
        return core::Result<std::vector<VertexId>>::success({start});
    }
    for (auto& [ignored, neighbors] : adjacency) {
        static_cast<void>(ignored);
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    std::vector<VertexId> pending{start};
    std::set<VertexId> visited{start};
    std::map<VertexId, VertexId> predecessor;
    for (std::size_t index = 0U; index < pending.size(); ++index) {
        const VertexId current = pending[index];
        for (const VertexId neighbor : adjacency.at(current)) {
            if (!visited.insert(neighbor).second) continue;
            predecessor.emplace(neighbor, current);
            if (neighbor == goal) {
                std::vector<VertexId> result{goal};
                VertexId cursor = goal;
                while (cursor != start) {
                    cursor = predecessor.at(cursor);
                    result.push_back(cursor);
                }
                std::reverse(result.begin(), result.end());
                return core::Result<std::vector<VertexId>>::success(std::move(result));
            }
            pending.push_back(neighbor);
        }
    }
    return core::Result<std::vector<VertexId>>::failure(
        Diagnostic(ErrorCode::not_found, "topology vertices are not linked by a path"));
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
