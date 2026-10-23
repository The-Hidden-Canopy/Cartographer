#include <carto/geometry/mesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <limits>
#include <locale>
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

struct ProjectedPoint {
    double x = 0.0;
    double y = 0.0;
};

struct PlanarPolygon {
    std::vector<ProjectedPoint> points;
    double signed_area_twice = 0.0;
    double scale = 0.0;
};

double orient2d(ProjectedPoint a, ProjectedPoint b, ProjectedPoint c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool point_on_segment(
    ProjectedPoint point,
    ProjectedPoint first,
    ProjectedPoint second,
    double area_tolerance,
    double coordinate_tolerance) {
    return std::abs(orient2d(first, second, point)) <= area_tolerance &&
        point.x >= std::min(first.x, second.x) - coordinate_tolerance &&
        point.x <= std::max(first.x, second.x) + coordinate_tolerance &&
        point.y >= std::min(first.y, second.y) - coordinate_tolerance &&
        point.y <= std::max(first.y, second.y) + coordinate_tolerance;
}

bool segments_intersect(
    ProjectedPoint a,
    ProjectedPoint b,
    ProjectedPoint c,
    ProjectedPoint d,
    double area_tolerance,
    double coordinate_tolerance) {
    const double ab_c = orient2d(a, b, c);
    const double ab_d = orient2d(a, b, d);
    const double cd_a = orient2d(c, d, a);
    const double cd_b = orient2d(c, d, b);
    const auto opposite = [area_tolerance](double left, double right) {
        return (left > area_tolerance && right < -area_tolerance) ||
            (left < -area_tolerance && right > area_tolerance);
    };
    if (opposite(ab_c, ab_d) && opposite(cd_a, cd_b)) return true;
    return point_on_segment(c, a, b, area_tolerance, coordinate_tolerance) ||
        point_on_segment(d, a, b, area_tolerance, coordinate_tolerance) ||
        point_on_segment(a, c, d, area_tolerance, coordinate_tolerance) ||
        point_on_segment(b, c, d, area_tolerance, coordinate_tolerance);
}

core::Result<PlanarPolygon> project_simple_planar_polygon(
    const std::vector<core::Vec3d>& positions) {
    if (positions.size() < 3U) {
        return core::Result<PlanarPolygon>::failure(Diagnostic(
            ErrorCode::unsupported, "dissolve boundary needs at least three vertices"));
    }

    double scale = 0.0;
    core::Vec3d newell_normal{};
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        const core::Vec3d current = positions[index];
        const core::Vec3d next = positions[(index + 1U) % positions.size()];
        if (!current.finite() || !next.finite()) {
            return core::Result<PlanarPolygon>::failure(Diagnostic(
                ErrorCode::unsupported, "dissolve boundary contains a non-finite position"));
        }
        scale = std::max(scale, (next - current).length());
        newell_normal.x += (current.y - next.y) * (current.z + next.z);
        newell_normal.y += (current.z - next.z) * (current.x + next.x);
        newell_normal.z += (current.x - next.x) * (current.y + next.y);
    }
    if (!std::isfinite(scale) || scale <= 0.0 || !std::isfinite(newell_normal.length())) {
        return core::Result<PlanarPolygon>::failure(Diagnostic(
            ErrorCode::unsupported, "dissolve boundary has degenerate geometry"));
    }
    const double area_tolerance = 1e-12 * scale * scale;
    const double coordinate_tolerance = 1e-12 * scale;
    const double normal_length = newell_normal.length();
    if (!std::isfinite(area_tolerance) || !std::isfinite(normal_length) ||
        normal_length <= area_tolerance) {
        return core::Result<PlanarPolygon>::failure(Diagnostic(
            ErrorCode::unsupported, "dissolve boundary has zero projected area"));
    }
    const core::Vec3d normal = newell_normal * (1.0 / normal_length);
    const double plane_tolerance = 1e-8 * scale;
    for (const core::Vec3d position : positions) {
        const double distance = core::dot(position - positions.front(), normal);
        if (!std::isfinite(distance) || std::abs(distance) > plane_tolerance) {
            return core::Result<PlanarPolygon>::failure(Diagnostic(
                ErrorCode::unsupported,
                "edge dissolve requires a coplanar merged boundary"));
        }
    }

    const double normal_x = std::abs(normal.x);
    const double normal_y = std::abs(normal.y);
    const double normal_z = std::abs(normal.z);
    const auto project = [normal_x, normal_y, normal_z](core::Vec3d position) {
        if (normal_x >= normal_y && normal_x >= normal_z) {
            return ProjectedPoint{position.y, position.z};
        }
        if (normal_y >= normal_z) return ProjectedPoint{position.x, position.z};
        return ProjectedPoint{position.x, position.y};
    };

    PlanarPolygon polygon;
    polygon.scale = scale;
    polygon.points.reserve(positions.size());
    for (const core::Vec3d position : positions) polygon.points.push_back(project(position));
    for (std::size_t index = 0U; index < polygon.points.size(); ++index) {
        const ProjectedPoint current = polygon.points[index];
        const ProjectedPoint next = polygon.points[(index + 1U) % polygon.points.size()];
        const double dx = next.x - current.x;
        const double dy = next.y - current.y;
        if (dx * dx + dy * dy <= coordinate_tolerance * coordinate_tolerance) {
            return core::Result<PlanarPolygon>::failure(Diagnostic(
                ErrorCode::unsupported, "dissolve boundary contains a zero-length edge"));
        }
        polygon.signed_area_twice += current.x * next.y - current.y * next.x;
    }
    if (!std::isfinite(polygon.signed_area_twice) ||
        std::abs(polygon.signed_area_twice) <= area_tolerance) {
        return core::Result<PlanarPolygon>::failure(Diagnostic(
            ErrorCode::unsupported, "dissolve boundary has zero area"));
    }

    for (std::size_t index = 0U; index < polygon.points.size(); ++index) {
        const std::size_t next = (index + 1U) % polygon.points.size();
        const std::size_t after_next = (index + 2U) % polygon.points.size();
        const ProjectedPoint first = polygon.points[index];
        const ProjectedPoint middle = polygon.points[next];
        const ProjectedPoint last = polygon.points[after_next];
        const double turn = orient2d(first, middle, last);
        const double incoming_x = middle.x - first.x;
        const double incoming_y = middle.y - first.y;
        const double outgoing_x = last.x - middle.x;
        const double outgoing_y = last.y - middle.y;
        if (std::abs(turn) <= area_tolerance &&
            incoming_x * outgoing_x + incoming_y * outgoing_y <
                -coordinate_tolerance * coordinate_tolerance) {
            return core::Result<PlanarPolygon>::failure(Diagnostic(
                ErrorCode::unsupported,
                "dissolve boundary doubles back along a collinear edge"));
        }
    }

    const std::size_t count = polygon.points.size();
    for (std::size_t first = 0U; first < count; ++first) {
        const std::size_t first_next = (first + 1U) % count;
        for (std::size_t second = first + 1U; second < count; ++second) {
            const std::size_t second_next = (second + 1U) % count;
            if (first == second || first_next == second || second_next == first) continue;
            if (segments_intersect(
                    polygon.points[first], polygon.points[first_next],
                    polygon.points[second], polygon.points[second_next],
                    area_tolerance, coordinate_tolerance)) {
                return core::Result<PlanarPolygon>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "dissolve boundary self-intersects or touches itself"));
            }
        }
    }
    return core::Result<PlanarPolygon>::success(std::move(polygon));
}

bool point_in_triangle(
    ProjectedPoint point,
    ProjectedPoint first,
    ProjectedPoint second,
    ProjectedPoint third,
    double winding,
    double area_tolerance) {
    return orient2d(first, second, point) * winding >= -area_tolerance &&
        orient2d(second, third, point) * winding >= -area_tolerance &&
        orient2d(third, first, point) * winding >= -area_tolerance;
}

core::Result<std::vector<std::array<std::size_t, 3U>>> triangulate_simple_polygon(
    const PlanarPolygon& polygon) {
    const std::size_t count = polygon.points.size();
    const double area_tolerance = 1e-12 * polygon.scale * polygon.scale;
    const double winding = polygon.signed_area_twice > 0.0 ? 1.0 : -1.0;
    std::vector<std::size_t> remaining(count);
    for (std::size_t index = 0U; index < count; ++index) remaining[index] = index;

    std::vector<std::array<std::size_t, 3U>> triangles;
    triangles.reserve(count - 2U);
    while (remaining.size() > 3U) {
        bool clipped = false;
        for (std::size_t current_index = 0U;
             current_index < remaining.size(); ++current_index) {
            const std::size_t previous_index =
                (current_index + remaining.size() - 1U) % remaining.size();
            const std::size_t next_index = (current_index + 1U) % remaining.size();
            const std::size_t previous = remaining[previous_index];
            const std::size_t current = remaining[current_index];
            const std::size_t next = remaining[next_index];
            if (orient2d(
                    polygon.points[previous], polygon.points[current],
                    polygon.points[next]) * winding <= area_tolerance) {
                continue;
            }

            bool diagonal_crosses_boundary = false;
            for (std::size_t edge_index = 0U; edge_index < remaining.size(); ++edge_index) {
                const std::size_t edge_first = remaining[edge_index];
                const std::size_t edge_second = remaining[(edge_index + 1U) % remaining.size()];
                if (edge_first == previous || edge_first == next ||
                    edge_second == previous || edge_second == next) {
                    continue;
                }
                if (segments_intersect(
                        polygon.points[previous], polygon.points[next],
                        polygon.points[edge_first], polygon.points[edge_second],
                        area_tolerance, 1e-12 * polygon.scale)) {
                    diagonal_crosses_boundary = true;
                    break;
                }
            }
            if (diagonal_crosses_boundary) continue;

            bool contains_other_vertex = false;
            for (const std::size_t candidate : remaining) {
                if (candidate == previous || candidate == current || candidate == next) continue;
                if (point_in_triangle(
                        polygon.points[candidate], polygon.points[previous],
                        polygon.points[current], polygon.points[next], winding,
                        area_tolerance)) {
                    contains_other_vertex = true;
                    break;
                }
            }
            if (contains_other_vertex) continue;

            triangles.push_back({previous, current, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(current_index));
            clipped = true;
            break;
        }
        if (!clipped) {
            return core::Result<std::vector<std::array<std::size_t, 3U>>>::failure(
                Diagnostic(ErrorCode::unsupported,
                           "simple polygon could not be triangulated without degeneracy"));
        }
    }
    if (orient2d(
            polygon.points[remaining[0U]], polygon.points[remaining[1U]],
            polygon.points[remaining[2U]]) * winding <= area_tolerance) {
        return core::Result<std::vector<std::array<std::size_t, 3U>>>::failure(
            Diagnostic(ErrorCode::unsupported,
                       "simple polygon leaves a degenerate final triangle"));
    }
    triangles.push_back({remaining[0U], remaining[1U], remaining[2U]});
    return core::Result<std::vector<std::array<std::size_t, 3U>>>::success(
        std::move(triangles));
}

bool is_concave_polygon(const PlanarPolygon& polygon) {
    const double winding = polygon.signed_area_twice > 0.0 ? 1.0 : -1.0;
    const double area_tolerance = 1e-12 * polygon.scale * polygon.scale;
    for (std::size_t index = 0U; index < polygon.points.size(); ++index) {
        const double turn = orient2d(
            polygon.points[index], polygon.points[(index + 1U) % polygon.points.size()],
            polygon.points[(index + 2U) % polygon.points.size()]) * winding;
        if (turn < -area_tolerance) return true;
    }
    return false;
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

core::Result<void> FaceDeletionPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "face deletion patch revision space is exhausted"));
    }
    if (!face) {
        return core::Result<void>::failure(invalid(
            "face deletion patch must name a non-zero face id"));
    }
    return core::Result<void>::success();
}

std::string FaceDeletionPatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_FACE_DELETION_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "FACE " << face.value << '\n'
           << "REMOVE_ORPHANS " << (remove_orphaned_vertices ? 1 : 0) << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<FaceDeletionPatch> FaceDeletionPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<FaceDeletionPatch>::failure(invalid(
            "serialized face deletion patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string face_label;
    std::uint64_t face_id = 0U;
    std::string orphan_label;
    std::uint32_t remove_orphans = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> face_label >> face_id >>
            orphan_label >> remove_orphans) ||
        magic != "CARTOGRAPHER_FACE_DELETION_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || face_label != "FACE" || face_id == 0U ||
        orphan_label != "REMOVE_ORPHANS" || remove_orphans > 1U) {
        return core::Result<FaceDeletionPatch>::failure(invalid(
            "serialized face deletion patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<FaceDeletionPatch>::failure(invalid(
            "serialized face deletion patch contains trailing data"));
    }
    FaceDeletionPatch patch{
        core::Revision(revision), FaceId{face_id}, remove_orphans == 1U};
    if (auto result = patch.validate(); !result) {
        return core::Result<FaceDeletionPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<FaceDeletionPatch>::failure(invalid(
            "serialized face deletion patch is not canonical"));
    }
    return core::Result<FaceDeletionPatch>::success(std::move(patch));
}

core::Result<void> EdgeSplitPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "edge split patch revision space is exhausted"));
    }
    if (!edge) {
        return core::Result<void>::failure(invalid(
            "edge split patch must name a non-zero edge id"));
    }
    if (!std::isfinite(factor) || factor <= 0.0 || factor >= 1.0) {
        return core::Result<void>::failure(invalid(
            "edge split patch factor must be finite and strictly between zero and one"));
    }
    return core::Result<void>::success();
}

std::string EdgeSplitPatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_EDGE_SPLIT_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "EDGE " << edge.value << '\n'
           << std::setprecision(17) << "FACTOR " << factor << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<EdgeSplitPatch> EdgeSplitPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<EdgeSplitPatch>::failure(invalid(
            "serialized edge split patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string edge_label;
    std::uint64_t edge_id = 0U;
    std::string factor_label;
    double factor = 0.0;
    if (!(input >> magic >> version >> revision_label >> revision >> edge_label >> edge_id >>
            factor_label >> factor) ||
        magic != "CARTOGRAPHER_EDGE_SPLIT_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || edge_label != "EDGE" || edge_id == 0U ||
        factor_label != "FACTOR") {
        return core::Result<EdgeSplitPatch>::failure(invalid(
            "serialized edge split patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<EdgeSplitPatch>::failure(invalid(
            "serialized edge split patch contains trailing data"));
    }
    EdgeSplitPatch patch{core::Revision(revision), EdgeId{edge_id}, factor};
    if (auto result = patch.validate(); !result) {
        return core::Result<EdgeSplitPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<EdgeSplitPatch>::failure(invalid(
            "serialized edge split patch is not canonical"));
    }
    return core::Result<EdgeSplitPatch>::success(std::move(patch));
}

core::Result<void> EdgeDissolvePatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "edge dissolve patch revision space is exhausted"));
    }
    if (edges.empty() || edges.size() > kMaxEdges) {
        return core::Result<void>::failure(invalid(
            "edge dissolve patch must name a bounded non-empty edge set"));
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(edges.size());
    for (const EdgeId edge : edges) {
        if (!edge) {
            return core::Result<void>::failure(invalid(
                "edge dissolve patch must name non-zero edge ids"));
        }
        ids.push_back(edge.value);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return core::Result<void>::failure(invalid(
            "edge dissolve patch cannot name duplicate edge ids"));
    }
    return core::Result<void>::success();
}

std::string EdgeDissolvePatch::serialize() const {
    if (!validate()) return {};
    auto ordered = edges;
    std::sort(ordered.begin(), ordered.end(), [](EdgeId left, EdgeId right) {
        return left.value < right.value;
    });
    std::ostringstream output;
    output << "CARTOGRAPHER_EDGE_DISSOLVE_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "EDGES " << ordered.size();
    for (const EdgeId edge : ordered) output << ' ' << edge.value;
    output << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<EdgeDissolvePatch> EdgeDissolvePatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<EdgeDissolvePatch>::failure(invalid(
            "serialized edge dissolve patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string edges_label;
    std::uint64_t count = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> edges_label >> count) ||
        magic != "CARTOGRAPHER_EDGE_DISSOLVE_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || edges_label != "EDGES" || count == 0U ||
        count > kMaxEdges) {
        return core::Result<EdgeDissolvePatch>::failure(invalid(
            "serialized edge dissolve patch header or edge count is invalid"));
    }
    EdgeDissolvePatch patch;
    patch.expected_revision = core::Revision(revision);
    patch.edges.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        std::uint64_t edge_id = 0U;
        if (!(input >> edge_id)) {
            return core::Result<EdgeDissolvePatch>::failure(invalid(
                "serialized edge dissolve patch is missing an edge id"));
        }
        patch.edges.push_back(EdgeId{edge_id});
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<EdgeDissolvePatch>::failure(invalid(
            "serialized edge dissolve patch contains trailing data"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<EdgeDissolvePatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<EdgeDissolvePatch>::failure(invalid(
            "serialized edge dissolve patch is not canonical"));
    }
    return core::Result<EdgeDissolvePatch>::success(std::move(patch));
}

core::Result<void> MergeByDistancePatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "merge-by-distance patch revision space is exhausted"));
    }
    if (candidates.size() < 2U || candidates.size() > kMaxCandidates) {
        return core::Result<void>::failure(invalid(
            "merge-by-distance patch must name a bounded set of at least two vertices"));
    }
    if (!std::isfinite(distance) || distance <= 0.0) {
        return core::Result<void>::failure(invalid(
            "merge-by-distance patch distance must be finite and positive"));
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(candidates.size());
    for (const VertexId candidate : candidates) {
        if (!candidate) {
            return core::Result<void>::failure(invalid(
                "merge-by-distance patch must name non-zero vertex ids"));
        }
        ids.push_back(candidate.value);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return core::Result<void>::failure(invalid(
            "merge-by-distance patch cannot name duplicate vertex ids"));
    }
    return core::Result<void>::success();
}

std::string MergeByDistancePatch::serialize() const {
    if (!validate()) return {};
    auto ordered = candidates;
    std::sort(ordered.begin(), ordered.end(), [](VertexId left, VertexId right) {
        return left.value < right.value;
    });
    std::ostringstream output;
    output << "CARTOGRAPHER_MERGE_BY_DISTANCE_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << std::setprecision(17) << "DISTANCE " << distance << '\n'
           << "CANDIDATES " << ordered.size();
    for (const VertexId candidate : ordered) output << ' ' << candidate.value;
    output << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<MergeByDistancePatch> MergeByDistancePatch::deserialize(
    std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<MergeByDistancePatch>::failure(invalid(
            "serialized merge-by-distance patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string distance_label;
    double distance = 0.0;
    std::string candidates_label;
    std::uint64_t count = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> distance_label >>
            distance >> candidates_label >> count) ||
        magic != "CARTOGRAPHER_MERGE_BY_DISTANCE_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || distance_label != "DISTANCE" ||
        candidates_label != "CANDIDATES" || count < 2U || count > kMaxCandidates) {
        return core::Result<MergeByDistancePatch>::failure(invalid(
            "serialized merge-by-distance patch header or candidate count is invalid"));
    }
    MergeByDistancePatch patch;
    patch.expected_revision = core::Revision(revision);
    patch.distance = distance;
    patch.candidates.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        std::uint64_t vertex_id = 0U;
        if (!(input >> vertex_id)) {
            return core::Result<MergeByDistancePatch>::failure(invalid(
                "serialized merge-by-distance patch is missing a candidate id"));
        }
        patch.candidates.push_back(VertexId{vertex_id});
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<MergeByDistancePatch>::failure(invalid(
            "serialized merge-by-distance patch contains trailing data"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<MergeByDistancePatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<MergeByDistancePatch>::failure(invalid(
            "serialized merge-by-distance patch is not canonical"));
    }
    return core::Result<MergeByDistancePatch>::success(std::move(patch));
}

core::Result<void> VertexMergePatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "vertex merge patch revision space is exhausted"));
    }
    if (!target || !source || target == source) {
        return core::Result<void>::failure(invalid(
            "vertex merge patch must name distinct non-zero target and source ids"));
    }
    if (mode != VertexMergeMode::target && mode != VertexMergeMode::center) {
        return core::Result<void>::failure(invalid(
            "vertex merge patch mode must be target or center"));
    }
    return core::Result<void>::success();
}

std::string VertexMergePatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_VERTEX_MERGE_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "TARGET " << target.value << '\n'
           << "SOURCE " << source.value << '\n'
           << "MODE " << (mode == VertexMergeMode::center ? "CENTER" : "TARGET") << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<VertexMergePatch> VertexMergePatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<VertexMergePatch>::failure(invalid(
            "serialized vertex merge patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string target_label;
    std::uint64_t target_id = 0U;
    std::string source_label;
    std::uint64_t source_id = 0U;
    std::string mode_label;
    std::string mode_value;
    if (!(input >> magic >> version >> revision_label >> revision >> target_label >> target_id >>
            source_label >> source_id >> mode_label >> mode_value) ||
        magic != "CARTOGRAPHER_VERTEX_MERGE_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || target_label != "TARGET" ||
        source_label != "SOURCE" || mode_label != "MODE" ||
        (mode_value != "TARGET" && mode_value != "CENTER")) {
        return core::Result<VertexMergePatch>::failure(invalid(
            "serialized vertex merge patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<VertexMergePatch>::failure(invalid(
            "serialized vertex merge patch contains trailing data"));
    }
    VertexMergePatch patch{
        core::Revision(revision), VertexId{target_id}, VertexId{source_id},
        mode_value == "CENTER" ? VertexMergeMode::center : VertexMergeMode::target};
    if (auto result = patch.validate(); !result) {
        return core::Result<VertexMergePatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<VertexMergePatch>::failure(invalid(
            "serialized vertex merge patch is not canonical"));
    }
    return core::Result<VertexMergePatch>::success(std::move(patch));
}

core::Result<void> LoopCutPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "loop-cut patch revision space is exhausted"));
    }
    if (!edge) {
        return core::Result<void>::failure(invalid(
            "loop-cut patch must name a non-zero seed edge id"));
    }
    return core::Result<void>::success();
}

std::string LoopCutPatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_LOOP_CUT_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "EDGE " << edge.value << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<LoopCutPatch> LoopCutPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<LoopCutPatch>::failure(invalid(
            "serialized loop-cut patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string edge_label;
    std::uint64_t edge_id = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> edge_label >> edge_id) ||
        magic != "CARTOGRAPHER_LOOP_CUT_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || edge_label != "EDGE" || edge_id == 0U) {
        return core::Result<LoopCutPatch>::failure(invalid(
            "serialized loop-cut patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<LoopCutPatch>::failure(invalid(
            "serialized loop-cut patch contains trailing data"));
    }
    LoopCutPatch patch{core::Revision(revision), EdgeId{edge_id}};
    if (auto result = patch.validate(); !result) {
        return core::Result<LoopCutPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<LoopCutPatch>::failure(invalid(
            "serialized loop-cut patch is not canonical"));
    }
    return core::Result<LoopCutPatch>::success(std::move(patch));
}

core::Result<void> KnifeCutPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "knife-cut patch revision space is exhausted"));
    }
    if (!face) {
        return core::Result<void>::failure(invalid(
            "knife-cut patch must name a non-zero source face id"));
    }
    if (path.size() < 2U || path.size() > kMaxPathPoints) {
        return core::Result<void>::failure(invalid(
            "knife-cut patch path must contain between two and 4096 points"));
    }
    for (const core::Vec3d position : path) {
        if (!position.finite()) {
            return core::Result<void>::failure(invalid(
                "knife-cut patch path positions must be finite"));
        }
    }
    return core::Result<void>::success();
}

std::string KnifeCutPatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "CARTOGRAPHER_KNIFE_CUT_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "FACE " << face.value << '\n'
           << std::setprecision(17) << "POINTS " << path.size();
    for (const core::Vec3d position : path) {
        output << ' ' << position.x << ' ' << position.y << ' ' << position.z;
    }
    output << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<KnifeCutPatch> KnifeCutPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<KnifeCutPatch>::failure(invalid(
            "serialized knife-cut patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    input.imbue(std::locale::classic());
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string face_label;
    std::uint64_t face_id = 0U;
    std::string points_label;
    std::uint64_t count = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> face_label >> face_id >>
            points_label >> count) ||
        magic != "CARTOGRAPHER_KNIFE_CUT_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || face_label != "FACE" || face_id == 0U ||
        points_label != "POINTS" || count < 2U || count > kMaxPathPoints) {
        return core::Result<KnifeCutPatch>::failure(invalid(
            "serialized knife-cut patch header or point count is invalid"));
    }
    KnifeCutPatch patch;
    patch.expected_revision = core::Revision(revision);
    patch.face = FaceId{face_id};
    patch.path.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        core::Vec3d position;
        if (!(input >> position.x >> position.y >> position.z)) {
            return core::Result<KnifeCutPatch>::failure(invalid(
                "serialized knife-cut patch is missing a path coordinate"));
        }
        patch.path.push_back(position);
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<KnifeCutPatch>::failure(invalid(
            "serialized knife-cut patch contains trailing data"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<KnifeCutPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<KnifeCutPatch>::failure(invalid(
            "serialized knife-cut patch is not canonical"));
    }
    return core::Result<KnifeCutPatch>::success(std::move(patch));
}

core::Result<void> BevelEdgePatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "edge bevel patch revision space is exhausted"));
    }
    if (!edge) {
        return core::Result<void>::failure(invalid(
            "edge bevel patch must name a non-zero edge id"));
    }
    if (!std::isfinite(width) || width <= 0.0) {
        return core::Result<void>::failure(invalid(
            "edge bevel patch width must be finite and positive"));
    }
    if (segments == 0U || segments > 64U) {
        return core::Result<void>::failure(invalid(
            "edge bevel patch segments must be in the range 1..64"));
    }
    return core::Result<void>::success();
}

std::string BevelEdgePatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_EDGE_BEVEL_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "EDGE " << edge.value << '\n'
           << std::setprecision(17) << "WIDTH " << width << '\n'
           << "SEGMENTS " << segments << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<BevelEdgePatch> BevelEdgePatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<BevelEdgePatch>::failure(invalid(
            "serialized edge bevel patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string edge_label;
    std::uint64_t edge_id = 0U;
    std::string width_label;
    double width = 0.0;
    std::string segments_label;
    std::uint32_t segments = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> edge_label >> edge_id >>
            width_label >> width >> segments_label >> segments) ||
        magic != "CARTOGRAPHER_EDGE_BEVEL_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || edge_label != "EDGE" || edge_id == 0U ||
        width_label != "WIDTH" || segments_label != "SEGMENTS") {
        return core::Result<BevelEdgePatch>::failure(invalid(
            "serialized edge bevel patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<BevelEdgePatch>::failure(invalid(
            "serialized edge bevel patch contains trailing data"));
    }
    BevelEdgePatch patch{core::Revision(revision), EdgeId{edge_id}, width, segments};
    if (auto result = patch.validate(); !result) {
        return core::Result<BevelEdgePatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<BevelEdgePatch>::failure(invalid(
            "serialized edge bevel patch is not canonical"));
    }
    return core::Result<BevelEdgePatch>::success(std::move(patch));
}

core::Result<void> BevelVertexPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "vertex bevel patch revision space is exhausted"));
    }
    if (!vertex) {
        return core::Result<void>::failure(invalid(
            "vertex bevel patch must name a non-zero vertex id"));
    }
    if (!std::isfinite(width) || width <= 0.0) {
        return core::Result<void>::failure(invalid(
            "vertex bevel patch width must be finite and positive"));
    }
    if (segments == 0U || segments > 64U) {
        return core::Result<void>::failure(invalid(
            "vertex bevel patch segments must be in the range 1..64"));
    }
    return core::Result<void>::success();
}

std::string BevelVertexPatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_VERTEX_BEVEL_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "VERTEX " << vertex.value << '\n'
           << std::setprecision(17) << "WIDTH " << width << '\n'
           << "SEGMENTS " << segments << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<BevelVertexPatch> BevelVertexPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<BevelVertexPatch>::failure(invalid(
            "serialized vertex bevel patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string vertex_label;
    std::uint64_t vertex_id = 0U;
    std::string width_label;
    double width = 0.0;
    std::string segments_label;
    std::uint32_t segments = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> vertex_label >> vertex_id >>
            width_label >> width >> segments_label >> segments) ||
        magic != "CARTOGRAPHER_VERTEX_BEVEL_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || vertex_label != "VERTEX" || vertex_id == 0U ||
        width_label != "WIDTH" || segments_label != "SEGMENTS") {
        return core::Result<BevelVertexPatch>::failure(invalid(
            "serialized vertex bevel patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<BevelVertexPatch>::failure(invalid(
            "serialized vertex bevel patch contains trailing data"));
    }
    BevelVertexPatch patch{core::Revision(revision), VertexId{vertex_id}, width, segments};
    if (auto result = patch.validate(); !result) {
        return core::Result<BevelVertexPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<BevelVertexPatch>::failure(invalid(
            "serialized vertex bevel patch is not canonical"));
    }
    return core::Result<BevelVertexPatch>::success(std::move(patch));
}

core::Result<void> FaceExtrusionPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "face extrusion patch revision space is exhausted"));
    }
    if (!face) {
        return core::Result<void>::failure(invalid(
            "face extrusion patch must name a non-zero face id"));
    }
    if (!std::isfinite(distance) || distance <= 0.0) {
        return core::Result<void>::failure(invalid(
            "face extrusion patch distance must be finite and positive"));
    }
    return core::Result<void>::success();
}

std::string FaceExtrusionPatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_FACE_EXTRUSION_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "FACE " << face.value << '\n'
           << std::setprecision(17) << "DISTANCE " << distance << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<FaceExtrusionPatch> FaceExtrusionPatch::deserialize(
    std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<FaceExtrusionPatch>::failure(invalid(
            "serialized face extrusion patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string face_label;
    std::uint64_t face_id = 0U;
    std::string distance_label;
    double distance = 0.0;
    if (!(input >> magic >> version >> revision_label >> revision >> face_label >> face_id >>
            distance_label >> distance) ||
        magic != "CARTOGRAPHER_FACE_EXTRUSION_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || face_label != "FACE" || face_id == 0U ||
        distance_label != "DISTANCE") {
        return core::Result<FaceExtrusionPatch>::failure(invalid(
            "serialized face extrusion patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<FaceExtrusionPatch>::failure(invalid(
            "serialized face extrusion patch contains trailing data"));
    }
    FaceExtrusionPatch patch{core::Revision(revision), FaceId{face_id}, distance};
    if (auto result = patch.validate(); !result) {
        return core::Result<FaceExtrusionPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<FaceExtrusionPatch>::failure(invalid(
            "serialized face extrusion patch is not canonical"));
    }
    return core::Result<FaceExtrusionPatch>::success(std::move(patch));
}

core::Result<void> FaceInsetPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "face inset patch revision space is exhausted"));
    }
    if (!face) {
        return core::Result<void>::failure(invalid(
            "face inset patch must name a non-zero face id"));
    }
    if (!std::isfinite(distance) || distance <= 0.0) {
        return core::Result<void>::failure(invalid(
            "face inset patch distance must be finite and positive"));
    }
    return core::Result<void>::success();
}

std::string FaceInsetPatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_FACE_INSET_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "FACE " << face.value << '\n'
           << std::setprecision(17) << "DISTANCE " << distance << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<FaceInsetPatch> FaceInsetPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<FaceInsetPatch>::failure(invalid(
            "serialized face inset patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string face_label;
    std::uint64_t face_id = 0U;
    std::string distance_label;
    double distance = 0.0;
    if (!(input >> magic >> version >> revision_label >> revision >> face_label >> face_id >>
            distance_label >> distance) ||
        magic != "CARTOGRAPHER_FACE_INSET_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || face_label != "FACE" || face_id == 0U ||
        distance_label != "DISTANCE") {
        return core::Result<FaceInsetPatch>::failure(invalid(
            "serialized face inset patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<FaceInsetPatch>::failure(invalid(
            "serialized face inset patch contains trailing data"));
    }
    FaceInsetPatch patch{core::Revision(revision), FaceId{face_id}, distance};
    if (auto result = patch.validate(); !result) {
        return core::Result<FaceInsetPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<FaceInsetPatch>::failure(invalid(
            "serialized face inset patch is not canonical"));
    }
    return core::Result<FaceInsetPatch>::success(std::move(patch));
}

core::Result<void> FacePokePatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "face poke patch revision space is exhausted"));
    }
    if (!face) {
        return core::Result<void>::failure(invalid(
            "face poke patch must name a non-zero face id"));
    }
    return core::Result<void>::success();
}

std::string FacePokePatch::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_FACE_POKE_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "FACE " << face.value << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<FacePokePatch> FacePokePatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<FacePokePatch>::failure(invalid(
            "serialized face poke patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string face_label;
    std::uint64_t face_id = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> face_label >> face_id) ||
        magic != "CARTOGRAPHER_FACE_POKE_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || face_label != "FACE" || face_id == 0U) {
        return core::Result<FacePokePatch>::failure(invalid(
            "serialized face poke patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<FacePokePatch>::failure(invalid(
            "serialized face poke patch contains trailing data"));
    }
    FacePokePatch patch{core::Revision(revision), FaceId{face_id}};
    if (auto result = patch.validate(); !result) {
        return core::Result<FacePokePatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<FacePokePatch>::failure(invalid(
            "serialized face poke patch is not canonical"));
    }
    return core::Result<FacePokePatch>::success(std::move(patch));
}

core::Result<void> TriToQuadPatch::validate() const {
    if (expected_revision.exhausted()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "triangle-to-quad patch revision space is exhausted"));
    }
    if (!first_face || !second_face || first_face == second_face) {
        return core::Result<void>::failure(invalid(
            "triangle-to-quad patch must name two distinct non-zero face ids"));
    }
    return core::Result<void>::success();
}

std::string TriToQuadPatch::serialize() const {
    if (!validate()) return {};
    const auto first = std::min(first_face.value, second_face.value);
    const auto second = std::max(first_face.value, second_face.value);
    std::ostringstream output;
    output << "CARTOGRAPHER_TRI_TO_QUAD_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "FACES " << first << ' ' << second << '\n';
    const std::string bytes = output.str();
    return bytes.size() <= kMaxSerializedBytes ? bytes : std::string{};
}

core::Result<TriToQuadPatch> TriToQuadPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<TriToQuadPatch>::failure(invalid(
            "serialized triangle-to-quad patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string faces_label;
    std::uint64_t first_face = 0U;
    std::uint64_t second_face = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> faces_label >>
            first_face >> second_face) ||
        magic != "CARTOGRAPHER_TRI_TO_QUAD_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || faces_label != "FACES" ||
        first_face == 0U || second_face == 0U || first_face == second_face) {
        return core::Result<TriToQuadPatch>::failure(invalid(
            "serialized triangle-to-quad patch header or operation is invalid"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<TriToQuadPatch>::failure(invalid(
            "serialized triangle-to-quad patch contains trailing data"));
    }
    TriToQuadPatch patch{
        core::Revision(revision), FaceId{first_face}, FaceId{second_face}};
    if (auto result = patch.validate(); !result) {
        return core::Result<TriToQuadPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<TriToQuadPatch>::failure(invalid(
            "serialized triangle-to-quad patch is not canonical"));
    }
    return core::Result<TriToQuadPatch>::success(std::move(patch));
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
    if (vertex_changes.size() > kMaxVertexChanges) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed, "mesh patch exceeds its vertex-change limit"));
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

std::string MeshPatch::serialize() const {
    if (vertex_changes.empty() || vertex_changes.size() > kMaxVertexChanges) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_MESH_PATCH " << kSchemaVersion << '\n'
           << "EXPECTED_REVISION " << expected_revision.value() << '\n'
           << "VERTEX_CHANGES " << vertex_changes.size() << '\n'
           << std::setprecision(17);
    for (std::size_t index = 0U; index < vertex_changes.size(); ++index) {
        const auto& change = vertex_changes[index];
        output << "VERTEX_CHANGE " << change.id.value << ' '
               << change.before.x << ' ' << change.before.y << ' ' << change.before.z << ' '
               << change.after.x << ' ' << change.after.y << ' ' << change.after.z << '\n';
        if ((index & 0xffU) == 0U &&
            static_cast<std::size_t>(output.tellp()) > kMaxSerializedBytes) {
            return {};
        }
    }
    if (static_cast<std::size_t>(output.tellp()) > kMaxSerializedBytes) return {};
    return output.str();
}

core::Result<MeshPatch> MeshPatch::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<MeshPatch>::failure(invalid(
            "serialized mesh patch is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string revision_label;
    std::uint64_t revision = 0U;
    std::string changes_label;
    std::uint64_t change_count = 0U;
    if (!(input >> magic >> version >> revision_label >> revision >> changes_label >> change_count) ||
        magic != "CARTOGRAPHER_MESH_PATCH" || version != kSchemaVersion ||
        revision_label != "EXPECTED_REVISION" || changes_label != "VERTEX_CHANGES" ||
        change_count == 0U || change_count > kMaxVertexChanges) {
        return core::Result<MeshPatch>::failure(invalid(
            "serialized mesh patch header or change count is invalid"));
    }
    MeshPatch patch;
    patch.expected_revision = core::Revision(revision);
    patch.vertex_changes.reserve(static_cast<std::size_t>(change_count));
    for (std::uint64_t index = 0U; index < change_count; ++index) {
        std::string record_label;
        std::uint64_t vertex_id = 0U;
        VertexChange change{};
        if (!(input >> record_label >> vertex_id >> change.before.x >> change.before.y >>
                change.before.z >> change.after.x >> change.after.y >> change.after.z) ||
            record_label != "VERTEX_CHANGE" || vertex_id == 0U) {
            return core::Result<MeshPatch>::failure(invalid(
                "serialized mesh patch contains a malformed vertex change"));
        }
        change.id = VertexId{vertex_id};
        patch.vertex_changes.push_back(change);
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<MeshPatch>::failure(invalid(
            "serialized mesh patch contains trailing data"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<MeshPatch>::failure(result.error());
    }
    if (patch.serialize() != bytes) {
        return core::Result<MeshPatch>::failure(invalid(
            "serialized mesh patch is not canonical"));
    }
    return core::Result<MeshPatch>::success(std::move(patch));
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
    for (const CornerId created : receipt.created_corners) {
        const auto destination_corner = corners_.find(created);
        if (destination_corner == corners_.end() ||
            destination_corner->second.vertex != created_vertex) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "mesh edge split created a corner outside the inserted vertex"));
        }
        const auto first_corner = std::find_if(
            before.corners_.begin(), before.corners_.end(), [destination_corner, edge](const auto& item) {
                return item.second.face == destination_corner->second.face &&
                    item.second.vertex == edge.first;
            });
        const auto second_corner = std::find_if(
            before.corners_.begin(), before.corners_.end(), [destination_corner, edge](const auto& item) {
                return item.second.face == destination_corner->second.face &&
                    item.second.vertex == edge.second;
            });
        if (first_corner == before.corners_.end() || second_corner == before.corners_.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "mesh edge split cannot bind a created corner to both source endpoints"));
        }
        std::vector<std::uint64_t> source_corners{
            first_corner->first.value, second_corner->first.value};
        std::sort(source_corners.begin(), source_corners.end());
        add_origin(receipt.corner_origins, created, OriginKind::interpolated_from,
            std::move(source_corners));
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::loop_cut(EdgeId edge_id) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!edge_id) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "loop cut requires a non-zero seed edge"));
    }

    constexpr std::size_t kMaxLoopCutEdges = 65'536U;
    const auto before_topology = topology();
    if (!before_topology) {
        return core::Result<TopologyEditReceipt>::failure(before_topology.error());
    }
    const auto loop_result = before_topology.value().edge_loop(edge_id);
    if (!loop_result) {
        return core::Result<TopologyEditReceipt>::failure(loop_result.error());
    }
    const auto& loop_edges = loop_result.value();
    if (loop_edges.size() < 2U || loop_edges.size() > kMaxLoopCutEdges) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "loop cut requires two to 65536 edges in a quad loop"));
    }
    const std::set<EdgeId> loop_edge_set(loop_edges.begin(), loop_edges.end());
    if (loop_edge_set.size() != loop_edges.size()) {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "loop cut traversal repeated an edge identity"));
    }

    std::map<EdgeKey, EdgeId> edge_ids_by_endpoints;
    const auto refresh_edge_lookup = [this, &edge_ids_by_endpoints]() {
        edge_ids_by_endpoints.clear();
        for (const auto& [id, edge] : edges_) {
            edge_ids_by_endpoints.emplace(undirected(edge.first, edge.second), id);
        }
    };
    const auto find_edge_between = [this, &edge_ids_by_endpoints](
        VertexId first, VertexId second) -> const EdgeRecord* {
        const auto found = edge_ids_by_endpoints.find(undirected(first, second));
        return found == edge_ids_by_endpoints.end() ? nullptr : find_edge(found->second);
    };
    refresh_edge_lookup();

    struct FaceCut {
        FaceId face;
        std::array<VertexId, 4U> vertices{};
        std::size_t first_edge_index = 0U;
        EdgeId first_edge;
        EdgeId second_edge;
    };
    std::map<FaceId, FaceCut> face_cuts;
    for (const EdgeId loop_edge_id : loop_edges) {
        const auto edge_iterator = edges_.find(loop_edge_id);
        if (edge_iterator == edges_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "loop cut traversal references a missing edge"));
        }
        const EdgeRecord& edge = edge_iterator->second;
        const std::array<HalfEdgeId, 2U> incident_half_edges{
            edge.first_half_edge, edge.second_half_edge.value_or(HalfEdgeId{})};
        const std::size_t incident_count = edge.second_half_edge.has_value() ? 2U : 1U;
        for (std::size_t side = 0U; side < incident_count; ++side) {
            const auto half_edge_iterator = half_edges_.find(incident_half_edges[side]);
            if (half_edge_iterator == half_edges_.end() ||
                half_edge_iterator->second.edge != loop_edge_id) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::stale_data,
                    "loop cut edge references a missing or mismatched half-edge"));
            }
            const FaceId face_id = half_edge_iterator->second.face;
            const auto face_iterator = faces_.find(face_id);
            if (face_iterator == faces_.end()) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::stale_data, "loop cut edge references a missing face"));
            }
            const Face& face = face_iterator->second;
            if (face.vertices.size() != 4U) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "loop cut currently requires quadrilateral faces"));
            }

            std::vector<std::pair<std::size_t, EdgeId>> selected_face_edges;
            for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
                const auto* face_edge = find_edge_between(
                    face.vertices[index], face.vertices[(index + 1U) % face.vertices.size()]);
                if (!face_edge) {
                    return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                        ErrorCode::stale_data,
                        "loop cut face boundary references a missing edge"));
                }
                if (loop_edge_set.contains(face_edge->id)) {
                    selected_face_edges.emplace_back(index, face_edge->id);
                }
            }
            if (selected_face_edges.size() != 2U ||
                (selected_face_edges[0U].first + 2U) % 4U != selected_face_edges[1U].first) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "loop cut edges must cross each quad as opposite sides"));
            }

            std::vector<core::Vec3d> positions;
            positions.reserve(4U);
            for (const VertexId vertex : face.vertices) {
                const auto found = vertices_.find(vertex);
                if (found == vertices_.end()) {
                    return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                        ErrorCode::stale_data,
                        "loop cut face references a missing vertex"));
                }
                positions.push_back(found->second.position);
            }
            const auto projected = project_simple_planar_polygon(positions);
            if (!projected || is_concave_polygon(projected.value())) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "loop cut requires simple, planar, convex quad faces"));
            }
            const double winding = projected.value().signed_area_twice > 0.0 ? 1.0 : -1.0;
            const double area_tolerance =
                1e-12 * projected.value().scale * projected.value().scale;
            for (std::size_t index = 0U; index < 4U; ++index) {
                const double turn = orient2d(
                    projected.value().points[index],
                    projected.value().points[(index + 1U) % 4U],
                    projected.value().points[(index + 2U) % 4U]);
                if (!std::isfinite(turn) || turn * winding <= area_tolerance) {
                    return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                        ErrorCode::unsupported,
                        "loop cut requires strictly convex quad faces"));
                }
            }

            FaceCut cut{
                face_id,
                {face.vertices[0U], face.vertices[1U], face.vertices[2U], face.vertices[3U]},
                selected_face_edges[0U].first,
                selected_face_edges[0U].second,
                selected_face_edges[1U].second};
            const auto [inserted, was_inserted] = face_cuts.emplace(face_id, cut);
            if (!was_inserted &&
                (inserted->second.first_edge != cut.first_edge ||
                 inserted->second.second_edge != cut.second_edge)) {
                return core::Result<TopologyEditReceipt>::failure(validation(
                    "loop cut traversal gave one quad conflicting edge pairs"));
            }
        }
    }
    if (face_cuts.empty()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported, "loop cut traversal contains no quad faces"));
    }

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto created_vertex_count = static_cast<std::uint64_t>(loop_edges.size());
    const auto created_face_count = static_cast<std::uint64_t>(face_cuts.size());
    if (next_vertex_id_ > maximum - created_vertex_count ||
        next_face_id_ > maximum - created_face_count) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "loop cut identity space is exhausted"));
    }

    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
    std::map<EdgeId, VertexId> split_vertices;
    std::map<VertexId, EdgeId> source_edge_by_split_vertex;
    for (const EdgeId loop_edge_id : loop_edges) {
        const EdgeRecord& edge = edges_.at(loop_edge_id);
        const auto first = vertices_.find(edge.first);
        const auto second = vertices_.find(edge.second);
        if (first == vertices_.end() || second == vertices_.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "loop cut edge references a missing endpoint"));
        }
        const VertexId midpoint{next_vertex_id_++};
        const core::Vec3d position =
            first->second.position * 0.5 + second->second.position * 0.5;
        if (!position.finite()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "loop cut produced a non-finite midpoint"));
        }
        vertices_.emplace(midpoint, Vertex{midpoint, position});
        split_vertices.emplace(loop_edge_id, midpoint);
        source_edge_by_split_vertex.emplace(midpoint, loop_edge_id);
    }

    std::map<FaceId, FaceId> child_face_parents;
    std::map<FaceId, std::pair<VertexId, VertexId>> connector_vertices;
    for (const auto& [face_id, cut] : face_cuts) {
        auto face_iterator = faces_.find(face_id);
        const auto first_midpoint = split_vertices.find(cut.first_edge);
        const auto second_midpoint = split_vertices.find(cut.second_edge);
        if (face_iterator == faces_.end() || first_midpoint == split_vertices.end() ||
            second_midpoint == split_vertices.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                core::ErrorCode::stale_data, "loop cut lost a quad or split-edge midpoint"));
        }
        const std::size_t first = cut.first_edge_index;
        const std::vector<VertexId> retained_face{
            cut.vertices[first], first_midpoint->second, second_midpoint->second,
            cut.vertices[(first + 3U) % 4U]};
        const std::vector<VertexId> generated_face{
            first_midpoint->second, cut.vertices[(first + 1U) % 4U],
            cut.vertices[(first + 2U) % 4U], second_midpoint->second};
        face_iterator->second.vertices = retained_face;

        const FaceId child_face{next_face_id_++};
        faces_.emplace(child_face, Face{child_face, generated_face});
        child_face_parents.emplace(child_face, face_id);
        connector_vertices.emplace(child_face,
            std::make_pair(first_midpoint->second, second_midpoint->second));
    }

    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    refresh_edge_lookup();
    bump_revision();
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = compile(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    for (const auto& [split_edge_id, midpoint] : split_vertices) {
        const EdgeRecord& edge = before.edges_.at(split_edge_id);
        add_origin(receipt.vertex_origins, midpoint, OriginKind::interpolated_from,
            {edge.first.value, edge.second.value});
        const auto* first_child = find_edge_between(edge.first, midpoint);
        const auto* second_child = find_edge_between(midpoint, edge.second);
        if (!first_child || !second_child) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "loop cut could not resolve a split edge child"));
        }
        add_origin(receipt.edge_origins, first_child->id, OriginKind::split_from,
            {split_edge_id.value});
        add_origin(receipt.edge_origins, second_child->id, OriginKind::split_from,
            {split_edge_id.value});
    }
    for (const auto& [child_face, source_face] : child_face_parents) {
        add_origin(receipt.face_origins, child_face, OriginKind::generated_from_face,
            {source_face.value});
        const auto connector = connector_vertices.find(child_face);
        if (connector == connector_vertices.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "loop cut lost a generated face connector"));
        }
        const auto* connector_edge = find_edge_between(
            connector->second.first, connector->second.second);
        if (!connector_edge) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "loop cut could not resolve a generated connector edge"));
        }
        add_origin(receipt.edge_origins, connector_edge->id, OriginKind::generated_from_face,
            {source_face.value});
    }

    const auto after_topology = topology();
    if (!after_topology) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(after_topology.error());
    }
    for (const auto& corner : after_topology.value().corners) {
        if (!std::binary_search(
                receipt.created_corners.begin(), receipt.created_corners.end(), corner.id)) {
            continue;
        }
        const FaceId source_face = child_face_parents.contains(corner.face) ?
            child_face_parents.at(corner.face) : corner.face;
        const auto split_source = source_edge_by_split_vertex.find(corner.vertex);
        if (split_source != source_edge_by_split_vertex.end()) {
            const EdgeRecord& source_edge = before.edges_.at(split_source->second);
            std::vector<std::uint64_t> source_corner_ids;
            for (const VertexId endpoint : {source_edge.first, source_edge.second}) {
                const auto source_corner = std::find_if(
                    before_topology.value().corners.begin(), before_topology.value().corners.end(),
                    [source_face, endpoint](const CornerRecord& candidate) {
                        return candidate.face == source_face && candidate.vertex == endpoint;
                    });
                if (source_corner != before_topology.value().corners.end()) {
                    source_corner_ids.push_back(source_corner->id.value);
                }
            }
            if (source_corner_ids.size() == 2U) {
                std::sort(source_corner_ids.begin(), source_corner_ids.end());
                add_origin(receipt.corner_origins, corner.id,
                    OriginKind::interpolated_from, std::move(source_corner_ids));
            }
        } else {
            const auto source_corner = std::find_if(
                before_topology.value().corners.begin(), before_topology.value().corners.end(),
                [source_face, &corner](const CornerRecord& candidate) {
                    return candidate.face == source_face && candidate.vertex == corner.vertex;
                });
            if (source_corner != before_topology.value().corners.end()) {
                add_origin(receipt.corner_origins, corner.id,
                    OriginKind::duplicated_from, {source_corner->id.value});
            }
        }
    }
    if (auto result = receipt.validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<TopologyEditReceipt> EditableMesh::knife_cut(
    FaceId face_id,
    const std::vector<core::Vec3d>& path) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    const auto face_iterator = faces_.find(face_id);
    if (face_iterator == faces_.end()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::not_found, "cannot knife a missing mesh face"));
    }
    if (path.size() < 2U || path.size() > 4096U) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "knife path must contain between two and 4096 points"));
    }

    const Face source_face = face_iterator->second;
    std::vector<core::Vec3d> boundary_positions;
    boundary_positions.reserve(source_face.vertices.size());
    for (const VertexId vertex_id : source_face.vertices) {
        const auto vertex = vertices_.find(vertex_id);
        if (vertex == vertices_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "knife source face references a missing vertex"));
        }
        boundary_positions.push_back(vertex->second.position);
    }
    const auto projected_boundary = project_simple_planar_polygon(boundary_positions);
    if (!projected_boundary) {
        return core::Result<TopologyEditReceipt>::failure(projected_boundary.error());
    }
    if (auto triangulated = triangulate_simple_polygon(projected_boundary.value());
        !triangulated) {
        return core::Result<TopologyEditReceipt>::failure(triangulated.error());
    }

    core::Vec3d newell_normal{};
    for (std::size_t index = 0U; index < boundary_positions.size(); ++index) {
        const core::Vec3d current = boundary_positions[index];
        const core::Vec3d next = boundary_positions[(index + 1U) % boundary_positions.size()];
        newell_normal.x += (current.y - next.y) * (current.z + next.z);
        newell_normal.y += (current.z - next.z) * (current.x + next.x);
        newell_normal.z += (current.x - next.x) * (current.y + next.y);
    }
    const double normal_length = newell_normal.length();
    if (!std::isfinite(normal_length) || normal_length <= 0.0) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported, "knife source face has a degenerate plane"));
    }
    const core::Vec3d normal = newell_normal * (1.0 / normal_length);
    const double scale = projected_boundary.value().scale;
    const double coordinate_tolerance = 1e-12 * scale;
    const double area_tolerance = 1e-12 * scale * scale;
    const double plane_tolerance = 1e-8 * scale;
    const double normal_x = std::abs(normal.x);
    const double normal_y = std::abs(normal.y);
    const double normal_z = std::abs(normal.z);
    const auto project_position = [normal_x, normal_y, normal_z](core::Vec3d position) {
        if (normal_x >= normal_y && normal_x >= normal_z) {
            return ProjectedPoint{position.y, position.z};
        }
        if (normal_y >= normal_z) return ProjectedPoint{position.x, position.z};
        return ProjectedPoint{position.x, position.y};
    };
    const auto point_is_strictly_inside = [&](ProjectedPoint point) {
        bool inside = false;
        for (std::size_t index = 0U; index < projected_boundary.value().points.size(); ++index) {
            const ProjectedPoint first = projected_boundary.value().points[index];
            const ProjectedPoint second = projected_boundary.value().points[
                (index + 1U) % projected_boundary.value().points.size()];
            if (point_on_segment(
                    point, first, second, area_tolerance, coordinate_tolerance)) {
                return false;
            }
            if ((first.y > point.y) != (second.y > point.y)) {
                const double crossing_x = first.x +
                    (point.y - first.y) * (second.x - first.x) / (second.y - first.y);
                if (point.x < crossing_x) inside = !inside;
            }
        }
        return inside;
    };

    struct KnifeEndpoint {
        std::optional<std::size_t> boundary_vertex;
        std::optional<std::size_t> boundary_edge;
        double edge_factor = 0.0;
        VertexId vertex;
        core::Vec3d position{};
    };
    std::vector<KnifeEndpoint> endpoints;
    endpoints.reserve(2U);
    for (const std::size_t path_index : {std::size_t{0U}, path.size() - 1U}) {
        const core::Vec3d position = path[path_index];
        if (!position.finite()) {
            return core::Result<TopologyEditReceipt>::failure(invalid(
                "knife path contains a non-finite position"));
        }
        KnifeEndpoint endpoint;
        std::optional<std::size_t> vertex_match;
        for (std::size_t boundary_index = 0U;
             boundary_index < boundary_positions.size(); ++boundary_index) {
            if ((position - boundary_positions[boundary_index]).length() >
                coordinate_tolerance) {
                continue;
            }
            if (vertex_match.has_value()) {
                return core::Result<TopologyEditReceipt>::failure(invalid(
                    "knife endpoint ambiguously matches multiple face vertices"));
            }
            vertex_match = boundary_index;
        }
        if (vertex_match.has_value()) {
            endpoint.boundary_vertex = *vertex_match;
            endpoint.vertex = source_face.vertices[*vertex_match];
            endpoint.position = boundary_positions[*vertex_match];
        } else {
            for (std::size_t edge_index = 0U;
                 edge_index < boundary_positions.size(); ++edge_index) {
                const core::Vec3d first = boundary_positions[edge_index];
                const core::Vec3d second = boundary_positions[
                    (edge_index + 1U) % boundary_positions.size()];
                const core::Vec3d edge_vector = second - first;
                const double edge_length_squared = core::dot(edge_vector, edge_vector);
                if (!std::isfinite(edge_length_squared) || edge_length_squared <= 0.0) continue;
                const double factor = core::dot(position - first, edge_vector) /
                    edge_length_squared;
                if (!std::isfinite(factor) || factor <= 0.0 || factor >= 1.0) continue;
                const core::Vec3d projected = first + edge_vector * factor;
                if ((position - projected).length() > coordinate_tolerance) continue;
                if (endpoint.boundary_edge.has_value()) {
                    return core::Result<TopologyEditReceipt>::failure(invalid(
                        "knife endpoint ambiguously matches multiple source-face edges"));
                }
                endpoint.boundary_edge = edge_index;
                endpoint.edge_factor = factor;
                endpoint.position = projected;
            }
            if (!endpoint.boundary_edge.has_value()) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "knife path endpoints must lie on the source-face boundary"));
            }
        }
        endpoints.push_back(endpoint);
    }
    if ((endpoints[0U].boundary_vertex.has_value() &&
         endpoints[1U].boundary_vertex == endpoints[0U].boundary_vertex) ||
        (endpoints[0U].boundary_edge.has_value() &&
         endpoints[1U].boundary_edge == endpoints[0U].boundary_edge &&
         std::abs(endpoints[0U].edge_factor - endpoints[1U].edge_factor) <=
             coordinate_tolerance / std::max(
                 (boundary_positions[(*endpoints[0U].boundary_edge + 1U) %
                     boundary_positions.size()] -
                  boundary_positions[*endpoints[0U].boundary_edge]).length(),
                 coordinate_tolerance))) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "knife path endpoints must resolve to distinct boundary positions"));
    }
    std::vector<core::Vec3d> canonical_path = path;
    canonical_path.front() = endpoints.front().position;
    canonical_path.back() = endpoints.back().position;

    std::vector<ProjectedPoint> projected_path;
    projected_path.reserve(path.size());
    for (std::size_t index = 0U; index < path.size(); ++index) {
        const core::Vec3d position = canonical_path[index];
        if (!position.finite()) {
            return core::Result<TopologyEditReceipt>::failure(invalid(
                "knife path contains a non-finite position"));
        }
        const double plane_distance = core::dot(position - boundary_positions.front(), normal);
        if (!std::isfinite(plane_distance) || std::abs(plane_distance) > plane_tolerance) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported, "knife path leaves the source-face plane"));
        }
        const ProjectedPoint projected = project_position(position);
        if (index > 0U) {
            const ProjectedPoint previous = projected_path.back();
            const double dx = projected.x - previous.x;
            const double dy = projected.y - previous.y;
            if (dx * dx + dy * dy <= coordinate_tolerance * coordinate_tolerance) {
                return core::Result<TopologyEditReceipt>::failure(invalid(
                    "knife path contains a zero-length segment"));
            }
        }
        if (index > 0U && index + 1U < path.size()) {
            if (!point_is_strictly_inside(projected)) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "knife intermediate points must lie strictly inside the source face"));
            }
            for (const auto& [existing_id, existing_vertex] : vertices_) {
                (void)existing_id;
                if ((position - existing_vertex.position).length() <= coordinate_tolerance) {
                    return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                        ErrorCode::unsupported,
                        "knife intermediate point coincides with an existing mesh vertex"));
                }
            }
        }
        projected_path.push_back(projected);
    }

    std::vector<core::Vec3d> simplified_path;
    std::vector<ProjectedPoint> simplified_projected_path;
    simplified_path.reserve(canonical_path.size());
    simplified_projected_path.reserve(projected_path.size());
    for (std::size_t index = 0U; index < canonical_path.size(); ++index) {
        simplified_path.push_back(canonical_path[index]);
        simplified_projected_path.push_back(projected_path[index]);
        while (simplified_projected_path.size() >= 3U) {
            const std::size_t last = simplified_projected_path.size() - 1U;
            const ProjectedPoint first = simplified_projected_path[last - 2U];
            const ProjectedPoint middle = simplified_projected_path[last - 1U];
            const ProjectedPoint third = simplified_projected_path[last];
            if (std::abs(orient2d(first, middle, third)) > area_tolerance) break;
            const double incoming_x = middle.x - first.x;
            const double incoming_y = middle.y - first.y;
            const double outgoing_x = third.x - middle.x;
            const double outgoing_y = third.y - middle.y;
            if (incoming_x * outgoing_x + incoming_y * outgoing_y <=
                coordinate_tolerance * coordinate_tolerance) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported, "knife path doubles back along a collinear segment"));
            }
            simplified_path.erase(simplified_path.end() - 2);
            simplified_projected_path.erase(simplified_projected_path.end() - 2);
        }
    }
    canonical_path = std::move(simplified_path);
    projected_path = std::move(simplified_projected_path);

    for (std::size_t first = 0U; first + 1U < projected_path.size(); ++first) {
        for (std::size_t second = first + 2U; second + 1U < projected_path.size(); ++second) {
            if (segments_intersect(
                    projected_path[first], projected_path[first + 1U],
                    projected_path[second], projected_path[second + 1U],
                    area_tolerance, coordinate_tolerance)) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported, "knife path self-intersects or touches itself"));
            }
        }
    }
    for (std::size_t path_edge = 0U; path_edge + 1U < projected_path.size(); ++path_edge) {
        for (std::size_t boundary_edge = 0U;
             boundary_edge < projected_boundary.value().points.size(); ++boundary_edge) {
            if (!segments_intersect(
                    projected_path[path_edge], projected_path[path_edge + 1U],
                    projected_boundary.value().points[boundary_edge],
                    projected_boundary.value().points[(boundary_edge + 1U) %
                        projected_boundary.value().points.size()],
                    area_tolerance, coordinate_tolerance)) {
                continue;
            }
            const auto endpoint_attaches = [&](const KnifeEndpoint& endpoint) {
                if (endpoint.boundary_edge == boundary_edge) return true;
                if (!endpoint.boundary_vertex.has_value()) return false;
                const VertexId endpoint_vertex =
                    source_face.vertices[*endpoint.boundary_vertex];
                return source_face.vertices[boundary_edge] == endpoint_vertex ||
                    source_face.vertices[(boundary_edge + 1U) % source_face.vertices.size()] ==
                        endpoint_vertex;
            };
            const bool attaches_at_start = path_edge == 0U &&
                endpoint_attaches(endpoints[0U]);
            const bool attaches_at_end = path_edge + 2U == projected_path.size() &&
                endpoint_attaches(endpoints[1U]);
            if (!attaches_at_start && !attaches_at_end) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "knife path crosses the source-face boundary away from its endpoints"));
            }
        }
    }

    const std::size_t new_endpoint_count = static_cast<std::size_t>(
        endpoints[0U].boundary_edge.has_value()) + static_cast<std::size_t>(
        endpoints[1U].boundary_edge.has_value());
    const std::size_t new_vertex_count = new_endpoint_count + canonical_path.size() - 2U;
    if (new_vertex_count >
        std::numeric_limits<std::uint64_t>::max() - next_vertex_id_) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "mesh vertex id space is exhausted"));
    }
    if (next_face_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "mesh face id space is exhausted"));
    }

    std::map<VertexId, core::Vec3d> new_vertex_positions;
    std::uint64_t next_vertex_id = next_vertex_id_;
    for (KnifeEndpoint& endpoint : endpoints) {
        if (endpoint.boundary_edge.has_value()) {
            endpoint.vertex = VertexId{next_vertex_id++};
            new_vertex_positions.emplace(endpoint.vertex, endpoint.position);
        }
    }

    std::vector<VertexId> path_vertices;
    path_vertices.reserve(canonical_path.size());
    path_vertices.push_back(endpoints[0U].vertex);
    for (std::size_t index = 1U; index + 1U < canonical_path.size(); ++index) {
        const VertexId created{next_vertex_id++};
        path_vertices.push_back(created);
        new_vertex_positions.emplace(created, canonical_path[index]);
    }
    path_vertices.push_back(endpoints[1U].vertex);

    struct BoundarySplit {
        EdgeId edge;
        double edge_factor;
        VertexId vertex;
    };
    std::vector<BoundarySplit> boundary_splits;
    for (const KnifeEndpoint& endpoint : endpoints) {
        if (!endpoint.boundary_edge.has_value()) continue;
        const std::size_t edge_index = *endpoint.boundary_edge;
        const VertexId first = source_face.vertices[edge_index];
        const VertexId second = source_face.vertices[(edge_index + 1U) %
            source_face.vertices.size()];
        const auto source_edge = std::find_if(edges_.begin(), edges_.end(),
            [first, second](const auto& item) {
                return (item.second.first == first && item.second.second == second) ||
                    (item.second.first == second && item.second.second == first);
            });
        if (source_edge == edges_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "knife boundary edge is missing from the topology index"));
        }
        const double factor = source_edge->second.first == first
            ? endpoint.edge_factor : 1.0 - endpoint.edge_factor;
        boundary_splits.push_back(BoundarySplit{source_edge->first, factor, endpoint.vertex});
    }

    std::vector<VertexId> augmented_boundary;
    augmented_boundary.reserve(source_face.vertices.size() + boundary_splits.size());
    for (std::size_t edge_index = 0U; edge_index < source_face.vertices.size(); ++edge_index) {
        const VertexId first = source_face.vertices[edge_index];
        const VertexId second = source_face.vertices[(edge_index + 1U) %
            source_face.vertices.size()];
        augmented_boundary.push_back(first);
        std::vector<std::pair<double, VertexId>> edge_points;
        for (const BoundarySplit& split : boundary_splits) {
            const EdgeRecord& edge = edges_.at(split.edge);
            if (edge.first == first && edge.second == second) {
                edge_points.emplace_back(split.edge_factor, split.vertex);
            } else if (edge.first == second && edge.second == first) {
                edge_points.emplace_back(1.0 - split.edge_factor, split.vertex);
            }
        }
        std::sort(edge_points.begin(), edge_points.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        for (const auto& [factor, vertex] : edge_points) {
            static_cast<void>(factor);
            augmented_boundary.push_back(vertex);
        }
    }

    const auto boundary_chain = [&](VertexId first, VertexId last) {
        std::vector<VertexId> result;
        const auto first_position = std::find(
            augmented_boundary.begin(), augmented_boundary.end(), first);
        const auto last_position = std::find(
            augmented_boundary.begin(), augmented_boundary.end(), last);
        if (first_position == augmented_boundary.end() ||
            last_position == augmented_boundary.end()) return result;
        std::size_t current = static_cast<std::size_t>(
            std::distance(augmented_boundary.begin(), first_position));
        const std::size_t last_index = static_cast<std::size_t>(
            std::distance(augmented_boundary.begin(), last_position));
        for (std::size_t count = 0U; count <= augmented_boundary.size(); ++count) {
            result.push_back(augmented_boundary[current]);
            if (current == last_index) return result;
            current = (current + 1U) % augmented_boundary.size();
        }
        return std::vector<VertexId>{};
    };
    std::vector<VertexId> first_loop = boundary_chain(path_vertices.front(), path_vertices.back());
    std::vector<VertexId> second_loop = boundary_chain(path_vertices.back(), path_vertices.front());
    if (first_loop.empty() || second_loop.empty()) {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "knife path could not resolve source-face boundary chains"));
    }
    for (std::size_t index = path_vertices.size() - 1U; index-- > 1U;) {
        first_loop.push_back(path_vertices[index]);
    }
    for (std::size_t index = 1U; index + 1U < path_vertices.size(); ++index) {
        second_loop.push_back(path_vertices[index]);
    }

    const auto loop_positions = [&](const std::vector<VertexId>& loop) {
        std::vector<core::Vec3d> result;
        result.reserve(loop.size());
        for (const VertexId vertex_id : loop) {
            const auto created = new_vertex_positions.find(vertex_id);
            if (created != new_vertex_positions.end()) {
                result.push_back(created->second);
                continue;
            }
            const auto vertex = vertices_.find(vertex_id);
            if (vertex == vertices_.end()) return std::vector<core::Vec3d>{};
            result.push_back(vertex->second.position);
        }
        return result;
    };
    for (const auto* candidate : {&first_loop, &second_loop}) {
        const auto positions = loop_positions(*candidate);
        const auto projected = project_simple_planar_polygon(positions);
        if (!projected) return core::Result<TopologyEditReceipt>::failure(projected.error());
        if (auto triangulated = triangulate_simple_polygon(projected.value()); !triangulated) {
            return core::Result<TopologyEditReceipt>::failure(triangulated.error());
        }
    }

    const bool first_retains_identity =
        std::find(first_loop.begin(), first_loop.end(), source_face.vertices.front()) != first_loop.end();
    std::vector<VertexId> retained_loop = first_retains_identity ? first_loop : second_loop;
    std::vector<VertexId> generated_loop = first_retains_identity ? second_loop : first_loop;
    const EditableMesh before = *this;
    const auto before_topology = topology();
    if (!before_topology) {
        return core::Result<TopologyEditReceipt>::failure(before_topology.error());
    }
    const core::Revision revision_before = revision_;
    for (const auto& [created_vertex, position] : new_vertex_positions) {
        vertices_.emplace(created_vertex, Vertex{created_vertex, position});
    }
    next_vertex_id_ = next_vertex_id;

    std::set<FaceId> adjacent_faces;
    for (const BoundarySplit& split : boundary_splits) {
        const EdgeRecord& edge = edges_.at(split.edge);
        const auto add_incident_face = [&](HalfEdgeId half_edge_id) -> core::Result<void> {
            const auto half_edge = half_edges_.find(half_edge_id);
            if (half_edge == half_edges_.end() || half_edge->second.edge != split.edge ||
                !faces_.contains(half_edge->second.face)) {
                return core::Result<void>::failure(Diagnostic(
                    ErrorCode::stale_data,
                    "knife boundary edge references an invalid incident half-edge"));
            }
            if (half_edge->second.face != face_id) {
                adjacent_faces.insert(half_edge->second.face);
            }
            return core::Result<void>::success();
        };
        if (auto result = add_incident_face(edge.first_half_edge); !result) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(result.error());
        }
        if (edge.second_half_edge.has_value()) {
            if (auto result = add_incident_face(*edge.second_half_edge); !result) {
                *this = before;
                return core::Result<TopologyEditReceipt>::failure(result.error());
            }
        }
    }
    for (const FaceId adjacent_face_id : adjacent_faces) {
        auto adjacent_face = faces_.find(adjacent_face_id);
        if (adjacent_face == faces_.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "knife adjacent face disappeared during edge split"));
        }
        std::vector<VertexId> split_loop;
        split_loop.reserve(adjacent_face->second.vertices.size() + boundary_splits.size());
        bool inserted = false;
        for (std::size_t edge_index = 0U;
             edge_index < adjacent_face->second.vertices.size(); ++edge_index) {
            const VertexId first = adjacent_face->second.vertices[edge_index];
            const VertexId second = adjacent_face->second.vertices[
                (edge_index + 1U) % adjacent_face->second.vertices.size()];
            split_loop.push_back(first);
            std::vector<std::pair<double, VertexId>> points;
            for (const BoundarySplit& split : boundary_splits) {
                const EdgeRecord& edge = edges_.at(split.edge);
                if (edge.first == first && edge.second == second) {
                    points.emplace_back(split.edge_factor, split.vertex);
                } else if (edge.first == second && edge.second == first) {
                    points.emplace_back(1.0 - split.edge_factor, split.vertex);
                }
            }
            std::sort(points.begin(), points.end(), [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
            for (const auto& [factor, vertex] : points) {
                static_cast<void>(factor);
                split_loop.push_back(vertex);
                inserted = true;
            }
        }
        if (!inserted) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "knife edge split did not match its indexed adjacent face"));
        }
        const auto positions = loop_positions(split_loop);
        if (positions.size() != split_loop.size()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "knife adjacent face split produced an unresolved vertex"));
        }
        bool fan_valid = false;
        for (std::size_t offset = 0U; offset < split_loop.size(); ++offset) {
            bool candidate_valid = true;
            for (std::size_t index = 1U; index + 1U < split_loop.size(); ++index) {
                if (triangle_area_squared(
                        positions[offset],
                        positions[(offset + index) % split_loop.size()],
                        positions[(offset + index + 1U) % split_loop.size()]) <= 1e-24) {
                    candidate_valid = false;
                    break;
                }
            }
            if (candidate_valid) {
                std::rotate(split_loop.begin(), split_loop.begin() +
                    static_cast<std::ptrdiff_t>(offset), split_loop.end());
                fan_valid = true;
                break;
            }
        }
        if (!fan_valid) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "knife boundary split cannot form a non-degenerate adjacent face fan"));
        }
        adjacent_face->second.vertices = std::move(split_loop);
    }

    const auto rotate_to_valid_fan = [&](std::vector<VertexId>& loop) -> bool {
        const auto positions = loop_positions(loop);
        if (positions.size() != loop.size()) return false;
        for (std::size_t offset = 0U; offset < loop.size(); ++offset) {
            bool candidate_valid = true;
            for (std::size_t index = 1U; index + 1U < loop.size(); ++index) {
                if (triangle_area_squared(
                        positions[offset],
                        positions[(offset + index) % loop.size()],
                        positions[(offset + index + 1U) % loop.size()]) <= 1e-24) {
                    candidate_valid = false;
                    break;
                }
            }
            if (candidate_valid) {
                std::rotate(loop.begin(), loop.begin() +
                    static_cast<std::ptrdiff_t>(offset), loop.end());
                return true;
            }
        }
        return false;
    };
    if (new_endpoint_count != 0U &&
        (!rotate_to_valid_fan(retained_loop) || !rotate_to_valid_fan(generated_loop))) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported, "knife cut cannot form non-degenerate face fans"));
    }
    face_iterator->second.vertices = retained_loop;
    const FaceId child_face{next_face_id_++};
    faces_.emplace(child_face, Face{child_face, generated_loop});

    if (auto result = rebuild_topology(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    bump_revision();
    if (auto result = validate(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = compile(); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = revision_;
    if (auto result = populate_identity_delta(before, *this, receipt); !result) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    std::vector<std::uint64_t> source_vertex_ids;
    source_vertex_ids.reserve(source_face.vertices.size());
    for (const VertexId source_vertex : source_face.vertices) {
        source_vertex_ids.push_back(source_vertex.value);
    }
    std::sort(source_vertex_ids.begin(), source_vertex_ids.end());
    for (const KnifeEndpoint& endpoint : endpoints) {
        if (!endpoint.boundary_edge.has_value()) continue;
        const std::size_t edge_index = *endpoint.boundary_edge;
        std::vector<std::uint64_t> source_ids{
            source_face.vertices[edge_index].value,
            source_face.vertices[(edge_index + 1U) % source_face.vertices.size()].value};
        std::sort(source_ids.begin(), source_ids.end());
        add_origin(receipt.vertex_origins, endpoint.vertex,
            OriginKind::interpolated_from, std::move(source_ids));
    }
    for (std::size_t index = 1U; index + 1U < path_vertices.size(); ++index) {
        add_origin(receipt.vertex_origins, path_vertices[index],
            OriginKind::generated_from_face, source_vertex_ids);
    }
    add_origin(receipt.face_origins, child_face, OriginKind::generated_from_face,
        {face_id.value});

    std::set<VertexId> knife_vertices(path_vertices.begin(), path_vertices.end());
    std::vector<std::uint64_t> source_edge_ids;
    source_edge_ids.reserve(source_face.vertices.size());
    for (std::size_t index = 0U; index < source_face.vertices.size(); ++index) {
        const VertexId first = source_face.vertices[index];
        const VertexId second = source_face.vertices[(index + 1U) %
            source_face.vertices.size()];
        const auto source_edge = std::find_if(before.edges_.begin(), before.edges_.end(),
            [first, second](const auto& item) {
                return (item.second.first == first && item.second.second == second) ||
                    (item.second.first == second && item.second.second == first);
            });
        if (source_edge == before.edges_.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data, "knife source face edge is missing from its topology"));
        }
        source_edge_ids.push_back(source_edge->first.value);
    }
    std::sort(source_edge_ids.begin(), source_edge_ids.end());
    for (const EdgeId created_edge_id : receipt.created_edges) {
        const auto* created_edge = find_edge(created_edge_id);
        if (!created_edge) continue;
        bool split_edge = false;
        for (const BoundarySplit& split : boundary_splits) {
            const EdgeRecord& source_edge = before.edges_.at(split.edge);
            std::set<VertexId> source_edge_vertices{source_edge.first, source_edge.second};
            for (const BoundarySplit& candidate : boundary_splits) {
                if (candidate.edge == split.edge) source_edge_vertices.insert(candidate.vertex);
            }
            if (!source_edge_vertices.contains(created_edge->first) ||
                !source_edge_vertices.contains(created_edge->second) ||
                (!std::any_of(boundary_splits.begin(), boundary_splits.end(),
                    [split, created_edge](const BoundarySplit& candidate) {
                        return candidate.edge == split.edge &&
                            (candidate.vertex == created_edge->first ||
                             candidate.vertex == created_edge->second);
                    }))) {
                continue;
            }
            add_origin(receipt.edge_origins, created_edge_id,
                OriginKind::split_from, {split.edge.value});
            split_edge = true;
            break;
        }
        if (!split_edge && knife_vertices.contains(created_edge->first) &&
            knife_vertices.contains(created_edge->second)) {
            add_origin(receipt.edge_origins, created_edge_id,
                OriginKind::generated_from_face, source_edge_ids);
        }
    }

    std::vector<std::uint64_t> source_corner_ids;
    for (const auto& corner : before_topology.value().corners) {
        if (corner.face == face_id) source_corner_ids.push_back(corner.id.value);
    }
    std::sort(source_corner_ids.begin(), source_corner_ids.end());
    const auto after_topology = topology();
    if (!after_topology) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(after_topology.error());
    }
    for (const CornerId created_corner_id : receipt.created_corners) {
        const auto created_corner = std::find_if(
            after_topology.value().corners.begin(), after_topology.value().corners.end(),
            [created_corner_id](const CornerRecord& corner) {
                return corner.id == created_corner_id;
            });
        if (created_corner == after_topology.value().corners.end()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "knife receipt lost a created corner"));
        }
        const FaceId source_corner_face = created_corner->face == child_face
            ? face_id : created_corner->face;
        const auto endpoint = std::find_if(
            endpoints.begin(), endpoints.end(), [&created_corner](const KnifeEndpoint& item) {
                return item.boundary_edge.has_value() &&
                    item.vertex == created_corner->vertex;
            });
        if (endpoint != endpoints.end()) {
            const std::size_t edge_index = *endpoint->boundary_edge;
            const VertexId first_vertex = source_face.vertices[edge_index];
            const VertexId second_vertex = source_face.vertices[(edge_index + 1U) %
                source_face.vertices.size()];
            const auto first_corner = std::find_if(
                before_topology.value().corners.begin(), before_topology.value().corners.end(),
                [source_corner_face, first_vertex](const CornerRecord& candidate) {
                    return candidate.face == source_corner_face &&
                        candidate.vertex == first_vertex;
                });
            const auto second_corner = std::find_if(
                before_topology.value().corners.begin(), before_topology.value().corners.end(),
                [source_corner_face, second_vertex](const CornerRecord& candidate) {
                    return candidate.face == source_corner_face &&
                        candidate.vertex == second_vertex;
                });
            if (first_corner == before_topology.value().corners.end() ||
                second_corner == before_topology.value().corners.end()) {
                *this = before;
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::stale_data,
                    "knife endpoint corner cannot bind to both source edge corners"));
            }
            std::vector<std::uint64_t> source_ids{
                first_corner->id.value, second_corner->id.value};
            std::sort(source_ids.begin(), source_ids.end());
            add_origin(receipt.corner_origins, created_corner_id,
                OriginKind::interpolated_from, std::move(source_ids));
            continue;
        }
        if (std::find(path_vertices.begin() + 1, path_vertices.end() - 1,
                created_corner->vertex) != path_vertices.end() - 1) {
            add_origin(receipt.corner_origins, created_corner_id,
                OriginKind::generated_from_face, source_corner_ids);
            continue;
        }
        const auto source_corner = std::find_if(
            before_topology.value().corners.begin(), before_topology.value().corners.end(),
            [source_corner_face, &created_corner](const CornerRecord& corner) {
                return corner.face == source_corner_face &&
                    corner.vertex == created_corner->vertex;
            });
        if (source_corner != before_topology.value().corners.end()) {
            add_origin(receipt.corner_origins, created_corner_id,
                OriginKind::duplicated_from, {source_corner->id.value});
        } else {
            add_origin(receipt.corner_origins, created_corner_id,
                OriginKind::generated_from_face, source_corner_ids);
        }
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
    if (segments == 0U || segments > 64U) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "vertex bevel segment count must be between one and 64"));
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
    constexpr std::size_t kMaxVertexBevelFanFaces = 64U;
    if (incident_face_ids.size() < 3U ||
        incident_face_ids.size() > kMaxVertexBevelFanFaces) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "vertex bevel requires a triangular fan with 3 to 64 incident faces"));
    }

    std::vector<EdgeRecord> incident_edges;
    incident_edges.reserve(incident_face_ids.size());
    for (const auto& [edge_id, edge] : edges_) {
        static_cast<void>(edge_id);
        if (edge.first == id || edge.second == id) {
            incident_edges.push_back(edge);
        }
    }
    if (incident_edges.size() != incident_face_ids.size()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "vertex bevel requires one internal edge per incident triangular face"));
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
                "vertex bevel currently requires every incident face to be triangular"));
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
    const std::uint64_t fan_size = static_cast<std::uint64_t>(incident_edges.size());
    const std::uint64_t extra_boundary_vertices =
        segments > 1U ? fan_size * (static_cast<std::uint64_t>(segments) - 1ULL) : 0ULL;
    const std::uint64_t extra_center_vertices = segments > 1U ? 1ULL : 0ULL;
    const std::uint64_t created_vertex_count =
        fan_size + extra_boundary_vertices + extra_center_vertices;
    const std::uint64_t created_face_count =
        segments > 1U
            ? 2ULL * fan_size * static_cast<std::uint64_t>(segments)
            : 1ULL;
    if (next_vertex_id_ > maximum - created_vertex_count ||
        next_face_id_ > maximum - created_face_count) {
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
    if (cap_next.size() != fan_size) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "vertex bevel did not produce one chamfer edge per fan face"));
    }
    std::vector<VertexId> cap_vertices;
    cap_vertices.reserve(cap_next.size());
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
    if (current != start || cap_vertices.size() != cap_next.size()) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "vertex bevel chamfer loop does not close"));
    }
    std::vector<core::Vec3d> cap_positions;
    cap_positions.reserve(cap_vertices.size());
    for (const VertexId cap_vertex : cap_vertices) {
        cap_positions.push_back(vertices_.at(cap_vertex).position);
    }
    const auto projected_cap = project_simple_planar_polygon(cap_positions);
    if (!projected_cap || is_concave_polygon(projected_cap.value())) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "vertex bevel requires a simple, planar, convex chamfer cap"));
    }

    std::vector<FaceId> bevel_faces;
    std::vector<std::pair<FaceId, FaceId>> incident_face_children;
    std::map<VertexId, std::pair<VertexId, VertexId>> boundary_vertex_parents;
    std::optional<VertexId> cap_center;
    if (segments == 1U) {
        const FaceId bevel_face{next_face_id_++};
        faces_.emplace(bevel_face, Face{bevel_face, std::move(cap_vertices)});
        bevel_faces.push_back(bevel_face);
    } else {
        std::map<std::pair<VertexId, VertexId>, std::vector<VertexId>> cap_edge_samples;
        std::vector<VertexId> boundary_loop;
        boundary_loop.reserve(static_cast<std::size_t>(segments) * cap_vertices.size());
        for (std::size_t edge_index = 0U; edge_index < cap_vertices.size(); ++edge_index) {
            const VertexId edge_start = cap_vertices[edge_index];
            const VertexId edge_end = cap_vertices[(edge_index + 1U) % cap_vertices.size()];
            const core::Vec3d start_position = vertices_.at(edge_start).position;
            const core::Vec3d end_position = vertices_.at(edge_end).position;
            std::vector<VertexId> samples;
            samples.reserve(static_cast<std::size_t>(segments - 1U));
            boundary_loop.push_back(edge_start);
            for (std::uint32_t sample = 1U; sample < segments; ++sample) {
                const double t = static_cast<double>(sample) / static_cast<double>(segments);
                const VertexId sample_id{next_vertex_id_++};
                const core::Vec3d position = start_position + (end_position - start_position) * t;
                if (!position.finite()) {
                    *this = before;
                    return core::Result<TopologyEditReceipt>::failure(validation(
                        "vertex bevel boundary subdivision produced a non-finite point"));
                }
                vertices_.emplace(sample_id, Vertex{sample_id, position});
                samples.push_back(sample_id);
                boundary_loop.push_back(sample_id);
                boundary_vertex_parents.emplace(sample_id, std::make_pair(edge_start, edge_end));
            }
            cap_edge_samples.emplace(
                std::make_pair(edge_start, edge_end), std::move(samples));
        }

        for (const auto& [face_id, pair] : face_cut_ids) {
            const auto face_iterator = faces_.find(face_id);
            const auto samples_iterator = cap_edge_samples.find(
                std::make_pair(pair.second, pair.first));
            if (face_iterator == faces_.end() || samples_iterator == cap_edge_samples.end()) {
                *this = before;
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::stale_data,
                    "vertex bevel lost an incident cap boundary during subdivision"));
            }
            const auto& face_vertices = face_iterator->second.vertices;
            std::vector<VertexId> subdivided;
            subdivided.reserve(face_vertices.size() + samples_iterator->second.size());
            bool found_boundary = false;
            for (std::size_t index = 0U; index < face_vertices.size(); ++index) {
                const VertexId current_vertex = face_vertices[index];
                const VertexId next_vertex = face_vertices[(index + 1U) % face_vertices.size()];
                subdivided.push_back(current_vertex);
                if (current_vertex == pair.first && next_vertex == pair.second) {
                    if (found_boundary) {
                        *this = before;
                        return core::Result<TopologyEditReceipt>::failure(validation(
                            "vertex bevel found a repeated cap boundary in an incident face"));
                    }
                    found_boundary = true;
                    for (auto sample = samples_iterator->second.rbegin();
                         sample != samples_iterator->second.rend(); ++sample) {
                        subdivided.push_back(*sample);
                    }
                }
            }
            if (!found_boundary) {
                *this = before;
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::stale_data,
                    "vertex bevel could not subdivide an incident cap boundary"));
            }
            const VertexId fan_root = subdivided[1U];
            std::vector<std::array<VertexId, 3U>> face_triangles;
            face_triangles.reserve(subdivided.size() - 2U);
            for (std::size_t index = 2U; index + 1U < subdivided.size(); ++index) {
                face_triangles.push_back({fan_root, subdivided[index], subdivided[index + 1U]});
            }
            face_triangles.push_back({fan_root, subdivided.back(), subdivided.front()});
            if (face_triangles.empty()) {
                *this = before;
                return core::Result<TopologyEditReceipt>::failure(validation(
                    "vertex bevel could not triangulate an incident face"));
            }
            face_iterator->second.vertices.assign(
                face_triangles.front().begin(), face_triangles.front().end());
            for (std::size_t triangle_index = 0U;
                 triangle_index < face_triangles.size(); ++triangle_index) {
                const auto& triangle = face_triangles[triangle_index];
                const core::Vec3d a = vertices_.at(triangle[0U]).position;
                const core::Vec3d b = vertices_.at(triangle[1U]).position;
                const core::Vec3d c = vertices_.at(triangle[2U]).position;
                const double area = core::cross(b - a, c - a).length();
                if (!std::isfinite(area) || area <= 1e-12) {
                    *this = before;
                    return core::Result<TopologyEditReceipt>::failure(validation(
                        "vertex bevel produced a degenerate incident face segment"));
                }
                if (triangle_index == 0U) continue;
                const FaceId child_face{next_face_id_++};
                faces_.emplace(child_face, Face{
                    child_face, {triangle[0U], triangle[1U], triangle[2U]}});
                incident_face_children.emplace_back(child_face, face_id);
            }
        }

        if (boundary_loop.size() != static_cast<std::size_t>(segments) * cap_vertices.size()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel produced an invalid subdivided cap boundary"));
        }
        core::Vec3d center_position{};
        for (const VertexId cap_vertex : cap_vertices) {
            center_position = center_position + vertices_.at(cap_vertex).position;
        }
        center_position = center_position * (1.0 / static_cast<double>(cap_vertices.size()));
        if (!center_position.finite()) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "vertex bevel produced a non-finite cap center"));
        }
        cap_center = VertexId{next_vertex_id_++};
        vertices_.emplace(*cap_center, Vertex{*cap_center, center_position});
        bevel_faces.reserve(boundary_loop.size());
        for (std::size_t index = 0U; index < boundary_loop.size(); ++index) {
            const VertexId current_vertex = boundary_loop[index];
            const VertexId next_vertex = boundary_loop[(index + 1U) % boundary_loop.size()];
            const core::Vec3d current_position = vertices_.at(current_vertex).position;
            const core::Vec3d next_position = vertices_.at(next_vertex).position;
            const double triangle_area = core::cross(
                next_position - current_position,
                center_position - current_position).length();
            if (!std::isfinite(triangle_area) || triangle_area <= 1e-12) {
                *this = before;
                return core::Result<TopologyEditReceipt>::failure(validation(
                    "vertex bevel produced a degenerate cap segment"));
            }
            const FaceId bevel_face{next_face_id_++};
            faces_.emplace(bevel_face, Face{
                bevel_face, {current_vertex, next_vertex, *cap_center}});
            bevel_faces.push_back(bevel_face);
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
    for (const auto& cut : cuts) {
        add_origin(receipt.vertex_origins, cut.created, OriginKind::interpolated_from,
            {id.value, cut.other.value});
    }
    for (const auto& [vertex, parents] : boundary_vertex_parents) {
        add_origin(receipt.vertex_origins, vertex, OriginKind::interpolated_from,
            {parents.first.value, parents.second.value});
    }
    if (cap_center.has_value()) {
        add_origin(receipt.vertex_origins, *cap_center, OriginKind::generated_from_vertex, {id.value});
    }
    for (const auto& [child_face, source_face] : incident_face_children) {
        add_origin(receipt.face_origins, child_face, OriginKind::generated_from_face,
            {source_face.value});
    }
    for (const FaceId bevel_face : bevel_faces) {
        add_origin(receipt.face_origins, bevel_face, OriginKind::generated_from_vertex, {id.value});
    }
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
    if (segments == 0U || segments > 64U) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "edge bevel segment count must be between one and 64"));
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

    if (segments == 1U) {
        const core::Vec3d bevel_a = first_cut.value().cut_origin_position;
        const core::Vec3d bevel_b = first_cut.value().cut_destination_position;
        const core::Vec3d bevel_c = second_cut.value().cut_destination_position;
        const core::Vec3d bevel_d = second_cut.value().cut_origin_position;
        const core::Vec3d bevel_normal =
            core::cross(bevel_a - bevel_b, bevel_c - bevel_b).normalized();
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
                "single-segment edge bevel requires a planar chamfer result"));
        }
    }

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t created_vertex_count =
        4ULL + 2ULL * (static_cast<std::uint64_t>(segments) - 1ULL);
    const std::uint64_t created_face_count = static_cast<std::uint64_t>(segments);
    if (next_vertex_id_ > maximum - created_vertex_count ||
        next_face_id_ > maximum - created_face_count) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state, "edge bevel identity space is exhausted"));
    }

    FaceCut first = first_cut.value();
    FaceCut second = second_cut.value();

    const auto build_profile = [segments](
        core::Vec3d center,
        core::Vec3d side_a,
        core::Vec3d side_b) -> core::Result<std::vector<core::Vec3d>> {
        std::vector<core::Vec3d> profile;
        profile.reserve(static_cast<std::size_t>(segments) + 1U);
        if (segments == 1U) {
            profile.push_back(side_a);
            profile.push_back(side_b);
            return core::Result<std::vector<core::Vec3d>>::success(std::move(profile));
        }

        const core::Vec3d offset_a = side_a - center;
        const core::Vec3d offset_b = side_b - center;
        const double radius_a = offset_a.length();
        const double radius_b = offset_b.length();
        const core::Vec3d direction_a = offset_a.normalized();
        const core::Vec3d direction_b = offset_b.normalized();
        if (!std::isfinite(radius_a) || !std::isfinite(radius_b) ||
            radius_a <= 1e-12 || radius_b <= 1e-12 ||
            !direction_a.finite() || !direction_b.finite()) {
            return core::Result<std::vector<core::Vec3d>>::failure(validation(
                "edge bevel cannot construct a finite rounded profile"));
        }
        const double cosine = std::clamp(core::dot(direction_a, direction_b), -1.0, 1.0);
        const double angle = std::acos(cosine);
        if (!std::isfinite(angle) || angle >= std::acos(-1.0) - 1e-8) {
            return core::Result<std::vector<core::Vec3d>>::failure(Diagnostic(
                ErrorCode::unsupported,
                "edge bevel rounded profile is ambiguous for opposing face offsets"));
        }
        const double sine = std::sin(angle);
        for (std::uint32_t index = 0U; index <= segments; ++index) {
            const double t = static_cast<double>(index) / static_cast<double>(segments);
            if (index == 0U) {
                profile.push_back(side_a);
                continue;
            }
            if (index == segments) {
                profile.push_back(side_b);
                continue;
            }
            core::Vec3d direction{};
            if (angle <= 1e-8 || std::abs(sine) <= 1e-12) {
                direction = (direction_a * (1.0 - t) + direction_b * t).normalized();
            } else {
                direction = (direction_a * (std::sin((1.0 - t) * angle) / sine) +
                             direction_b * (std::sin(t * angle) / sine)).normalized();
            }
            const double radius = radius_a + (radius_b - radius_a) * t;
            const core::Vec3d point = center + direction * radius;
            if (!direction.finite() || !point.finite()) {
                return core::Result<std::vector<core::Vec3d>>::failure(validation(
                    "edge bevel rounded profile produced a non-finite point"));
            }
            profile.push_back(point);
        }
        return core::Result<std::vector<core::Vec3d>>::success(std::move(profile));
    };

    const auto origin_profile_positions = build_profile(
        vertices_.at(first.origin).position,
        first.cut_origin_position,
        second.cut_destination_position);
    const auto destination_profile_positions = build_profile(
        vertices_.at(first.destination).position,
        first.cut_destination_position,
        second.cut_origin_position);
    if (!origin_profile_positions) {
        return core::Result<TopologyEditReceipt>::failure(origin_profile_positions.error());
    }
    if (!destination_profile_positions) {
        return core::Result<TopologyEditReceipt>::failure(destination_profile_positions.error());
    }

    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
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

    std::vector<VertexId> origin_profile{first.cut_origin};
    std::vector<VertexId> destination_profile{first.cut_destination};
    origin_profile.reserve(static_cast<std::size_t>(segments) + 1U);
    destination_profile.reserve(static_cast<std::size_t>(segments) + 1U);
    for (std::uint32_t index = 1U; index < segments; ++index) {
        const VertexId origin_profile_vertex{next_vertex_id_++};
        const VertexId destination_profile_vertex{next_vertex_id_++};
        origin_profile.push_back(origin_profile_vertex);
        destination_profile.push_back(destination_profile_vertex);
        vertices_.emplace(origin_profile_vertex, Vertex{
            origin_profile_vertex,
            origin_profile_positions.value()[static_cast<std::size_t>(index)]});
        vertices_.emplace(destination_profile_vertex, Vertex{
            destination_profile_vertex,
            destination_profile_positions.value()[static_cast<std::size_t>(index)]});
    }
    origin_profile.push_back(second.cut_destination);
    destination_profile.push_back(second.cut_origin);

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

    std::vector<FaceId> bevel_faces;
    bevel_faces.reserve(segments);
    for (std::uint32_t index = 0U; index < segments; ++index) {
        const std::size_t current = static_cast<std::size_t>(index);
        const std::size_t next = current + 1U;
        const std::array<VertexId, 4U> face_vertices{
            destination_profile[current], origin_profile[current],
            origin_profile[next], destination_profile[next]};
        const core::Vec3d p0 = vertices_.at(face_vertices[0U]).position;
        const core::Vec3d p1 = vertices_.at(face_vertices[1U]).position;
        const core::Vec3d p2 = vertices_.at(face_vertices[2U]).position;
        const core::Vec3d p3 = vertices_.at(face_vertices[3U]).position;
        const double first_triangle_area = core::cross(p1 - p0, p2 - p0).length();
        const double second_triangle_area = core::cross(p2 - p0, p3 - p0).length();
        if (!std::isfinite(first_triangle_area) || !std::isfinite(second_triangle_area) ||
            first_triangle_area <= 1e-12 || second_triangle_area <= 1e-12) {
            *this = before;
            return core::Result<TopologyEditReceipt>::failure(validation(
                "edge bevel produced a degenerate profile segment"));
        }
        const FaceId bevel_face{next_face_id_++};
        faces_.emplace(bevel_face, Face{
            bevel_face,
            std::vector<VertexId>(face_vertices.begin(), face_vertices.end())});
        bevel_faces.push_back(bevel_face);
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
    add_origin(receipt.vertex_origins, first.cut_origin, OriginKind::interpolated_from,
        {first.origin.value, first.previous.value});
    add_origin(receipt.vertex_origins, first.cut_destination, OriginKind::interpolated_from,
        {first.destination.value, first.next.value});
    add_origin(receipt.vertex_origins, second.cut_origin, OriginKind::interpolated_from,
        {second.origin.value, second.previous.value});
    add_origin(receipt.vertex_origins, second.cut_destination, OriginKind::interpolated_from,
        {second.destination.value, second.next.value});
    for (std::uint32_t index = 1U; index < segments; ++index) {
        add_origin(receipt.vertex_origins, origin_profile[static_cast<std::size_t>(index)],
            OriginKind::interpolated_from,
            {first.origin.value, first.previous.value, second.next.value});
        add_origin(receipt.vertex_origins, destination_profile[static_cast<std::size_t>(index)],
            OriginKind::interpolated_from,
            {first.destination.value, first.next.value, second.previous.value});
    }
    for (const FaceId bevel_face : bevel_faces) {
        add_origin(receipt.face_origins, bevel_face, OriginKind::generated_from_edge, {id.value});
    }
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
    for (const VertexId vertex : merged_vertices) {
        const auto vertex_iterator = vertices_.find(vertex);
        if (vertex_iterator == vertices_.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "edge dissolve merged boundary references a missing vertex"));
        }
        positions.push_back(vertex_iterator->second.position);
    }

    const auto projected_boundary = project_simple_planar_polygon(positions);
    if (!projected_boundary) {
        return core::Result<TopologyEditReceipt>::failure(projected_boundary.error());
    }
    const auto boundary_triangles = triangulate_simple_polygon(projected_boundary.value());
    if (!boundary_triangles) {
        return core::Result<TopologyEditReceipt>::failure(boundary_triangles.error());
    }
    for (const auto& triangle : boundary_triangles.value()) {
        if (triangle[0U] >= positions.size() || triangle[1U] >= positions.size() ||
            triangle[2U] >= positions.size() ||
            triangle_area_squared(
                positions[triangle[0U]], positions[triangle[1U]], positions[triangle[2U]]) <=
                1e-24) {
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

core::Result<TopologyEditReceipt> EditableMesh::dissolve_edges(
    const std::vector<EdgeId>& ids) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (ids.empty()) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "multi-edge dissolve requires at least one edge identity"));
    }
    if (ids.size() > kMaxDissolveEdges) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported, "multi-edge dissolve exceeds its bounded edge count"));
    }
    std::vector<EdgeId> ordered_ids = ids;
    std::sort(ordered_ids.begin(), ordered_ids.end());
    if (std::adjacent_find(ordered_ids.begin(), ordered_ids.end()) != ordered_ids.end()) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "multi-edge dissolve requires distinct edge identities"));
    }
    for (const EdgeId id : ordered_ids) {
        if (!id || !edges_.contains(id)) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::not_found, "multi-edge dissolve references a missing mesh edge"));
        }
    }

    const EditableMesh before = *this;
    EditableMesh after = before;
    const core::Revision revision_before = revision_;
    std::map<FaceId, std::set<FaceId>> source_faces;
    for (const Face& face : before.faces_sorted()) {
        source_faces.emplace(face.id, std::set<FaceId>{face.id});
    }

    for (const EdgeId id : ordered_ids) {
        const auto current_topology = after.topology();
        if (!current_topology) {
            return core::Result<TopologyEditReceipt>::failure(current_topology.error());
        }
        const auto edge = std::find_if(
            current_topology.value().edges.begin(), current_topology.value().edges.end(),
            [id](const EdgeRecord& candidate) { return candidate.id == id; });
        if (edge == current_topology.value().edges.end() ||
            !edge->second_half_edge.has_value()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::unsupported,
                "multi-edge dissolve requires every selected edge to remain internal"));
        }
        const auto first_half_edge = std::find_if(
            current_topology.value().half_edges.begin(),
            current_topology.value().half_edges.end(),
            [edge](const HalfEdgeRecord& candidate) {
                return candidate.id == edge->first_half_edge;
            });
        const auto second_half_edge = std::find_if(
            current_topology.value().half_edges.begin(),
            current_topology.value().half_edges.end(),
            [edge](const HalfEdgeRecord& candidate) {
                return candidate.id == *edge->second_half_edge;
            });
        if (first_half_edge == current_topology.value().half_edges.end() ||
            second_half_edge == current_topology.value().half_edges.end()) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::stale_data,
                "multi-edge dissolve references a missing incident half-edge"));
        }
        const FaceId first_face = first_half_edge->face;
        const FaceId second_face = second_half_edge->face;
        if (first_face == second_face || !source_faces.contains(first_face) ||
            !source_faces.contains(second_face)) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::validation_failed,
                "multi-edge dissolve found inconsistent face lineage"));
        }

        auto step = after.dissolve_edge(id);
        if (!step) return core::Result<TopologyEditReceipt>::failure(step.error());

        const FaceId survivor = std::min(first_face, second_face);
        const FaceId removed = std::max(first_face, second_face);
        auto combined_sources = source_faces.at(survivor);
        combined_sources.insert(source_faces.at(removed).begin(), source_faces.at(removed).end());
        source_faces.erase(survivor);
        source_faces.erase(removed);
        source_faces.emplace(survivor, std::move(combined_sources));
    }

    after.revision_ = revision_before;
    after.bump_revision();
    if (auto result = after.validate(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    TopologyEditReceipt receipt;
    receipt.revision_before = revision_before;
    receipt.revision_after = after.revision_;
    if (auto result = populate_identity_delta(before, after, receipt); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    const auto before_topology = before.topology();
    const auto after_topology = after.topology();
    if (!before_topology) {
        return core::Result<TopologyEditReceipt>::failure(before_topology.error());
    }
    if (!after_topology) {
        return core::Result<TopologyEditReceipt>::failure(after_topology.error());
    }
    for (const CornerRecord& corner : after_topology.value().corners) {
        if (!std::binary_search(
                receipt.created_corners.begin(), receipt.created_corners.end(), corner.id)) {
            continue;
        }
        const auto group = source_faces.find(corner.face);
        if (group == source_faces.end()) continue;
        const auto source = std::find_if(
            before_topology.value().corners.begin(), before_topology.value().corners.end(),
            [&corner, &group](const CornerRecord& candidate) {
                return candidate.vertex == corner.vertex &&
                    group->second.contains(candidate.face);
            });
        if (source != before_topology.value().corners.end()) {
            add_origin(receipt.corner_origins, corner.id, OriginKind::duplicated_from,
                       {source->id.value});
        }
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    *this = std::move(after);
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
    VertexId source,
    VertexMergeMode mode) {
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
    if (mode == VertexMergeMode::last_selected) {
        return core::Result<TopologyEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "last-selected merge mode requires an editor selection context"));
    }
    if (mode != VertexMergeMode::target && mode != VertexMergeMode::center) {
        return core::Result<TopologyEditReceipt>::failure(
            invalid("vertex merge mode is not supported"));
    }
    std::map<VertexId, core::Vec3d> moved_targets;
    if (mode == VertexMergeMode::center) {
        const core::Vec3d& target_position = vertices_.at(target).position;
        const core::Vec3d& source_position = vertices_.at(source).position;
        const core::Vec3d center{
            0.5 * (target_position.x + source_position.x),
            0.5 * (target_position.y + source_position.y),
            0.5 * (target_position.z + source_position.z)};
        if (!center.finite()) {
            return core::Result<TopologyEditReceipt>::failure(
                validation("center vertex merge produced a non-finite position"));
        }
        moved_targets.emplace(target, center);
    }
    return apply_vertex_merges({{source, target}}, moved_targets);
}

core::Result<TopologyEditReceipt> EditableMesh::merge_by_distance(
    const std::vector<VertexId>& candidates,
    double distance) {
    if (auto result = reject_topology_edit_with_attributes(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (revision_.exhausted()) {
        return core::Result<TopologyEditReceipt>::failure(exhausted_revision());
    }
    if (!std::isfinite(distance) || distance <= 0.0) {
        return core::Result<TopologyEditReceipt>::failure(
            invalid("merge-by-distance threshold must be finite and strictly positive"));
    }
    if (candidates.size() < 2U) {
        return core::Result<TopologyEditReceipt>::failure(invalid(
            "merge-by-distance requires at least two candidate vertices"));
    }
    if (candidates.size() > kMaxMergeByDistanceCandidates) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::unsupported,
            "merge-by-distance candidate count exceeds the bounded limit"));
    }
    std::vector<VertexId> ordered = candidates;
    std::sort(ordered.begin(), ordered.end());
    for (std::size_t index = 0U; index < ordered.size(); ++index) {
        if (!ordered[index] || (index > 0U && ordered[index - 1U] == ordered[index])) {
            return core::Result<TopologyEditReceipt>::failure(invalid(
                "merge-by-distance candidates must be distinct non-zero vertex identities"));
        }
        if (!find_vertex(ordered[index])) {
            return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                ErrorCode::not_found,
                "merge-by-distance references a missing mesh vertex"));
        }
    }

    // Uniform cells of the threshold size bound the neighbour search to the
    // 27 surrounding cells. Cell coordinates must stay well inside int64 so a
    // tiny threshold over large coordinates fails closed instead of wrapping.
    using CellKey = std::array<std::int64_t, 3>;
    constexpr double kMaxCellCoordinate = 4.0e18;
    std::map<CellKey, std::vector<VertexId>> cells;
    std::map<VertexId, CellKey> vertex_cells;
    for (const VertexId vertex : ordered) {
        const core::Vec3d& position = vertices_.at(vertex).position;
        const std::array<double, 3> coordinates{position.x, position.y, position.z};
        CellKey key{};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            const double cell = std::floor(coordinates[axis] / distance);
            if (!std::isfinite(cell) || std::abs(cell) > kMaxCellCoordinate) {
                return core::Result<TopologyEditReceipt>::failure(Diagnostic(
                    ErrorCode::unsupported,
                    "merge-by-distance threshold is too small for the vertex coordinates"));
            }
            key[axis] = static_cast<std::int64_t>(cell);
        }
        cells[key].push_back(vertex);
        vertex_cells.emplace(vertex, key);
    }

    // Lowest stable ID wins: each unmerged candidate, visited in ascending ID
    // order, absorbs every later unmerged candidate within the threshold of
    // its own authored position. Clustering is not transitive, so a chain of
    // near points never drifts further than the threshold from its target.
    const double squared_distance = distance * distance;
    std::map<VertexId, VertexId> replacements;
    for (const VertexId target : ordered) {
        if (replacements.contains(target)) {
            continue;
        }
        const core::Vec3d& target_position = vertices_.at(target).position;
        const CellKey& center = vertex_cells.at(target);
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto cell = cells.find(
                        CellKey{center[0] + dx, center[1] + dy, center[2] + dz});
                    if (cell == cells.end()) {
                        continue;
                    }
                    for (const VertexId source : cell->second) {
                        if (source <= target || replacements.contains(source)) {
                            continue;
                        }
                        const core::Vec3d delta =
                            vertices_.at(source).position - target_position;
                        if (core::dot(delta, delta) <= squared_distance) {
                            replacements.emplace(source, target);
                        }
                    }
                }
            }
        }
    }
    if (replacements.empty()) {
        return core::Result<TopologyEditReceipt>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "no candidate vertices lie within the merge distance; no mutation was published"));
    }
    return apply_vertex_merges(replacements, {});
}

core::Result<TopologyEditReceipt> EditableMesh::apply_vertex_merges(
    const std::map<VertexId, VertexId>& replacements,
    const std::map<VertexId, core::Vec3d>& moved_targets) {
    const EditableMesh before = *this;
    const core::Revision revision_before = revision_;
    std::map<FaceId, Face> merged_faces;
    for (const auto& [face_id, face] : faces_) {
        std::vector<VertexId> collapsed;
        collapsed.reserve(face.vertices.size());
        for (const VertexId vertex : face.vertices) {
            const auto replacement_iterator = replacements.find(vertex);
            const VertexId replacement = replacement_iterator == replacements.end() ?
                vertex : replacement_iterator->second;
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
    for (const auto& replacement : replacements) {
        vertices_.erase(replacement.first);
    }
    for (const auto& [target, position] : moved_targets) {
        vertices_.at(target).position = position;
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
    receipt.merged_vertices = replacements;
    const auto before_topology = before.topology();
    const auto after_topology = topology();
    if (!before_topology || !after_topology) {
        *this = before;
        return core::Result<TopologyEditReceipt>::failure(validation(
            "vertex merge receipt topology snapshot failed validation"));
    }
    for (const auto& corner : after_topology.value().corners) {
        if (!std::binary_search(
                receipt.created_corners.begin(), receipt.created_corners.end(), corner.id)) {
            continue;
        }
        std::vector<std::uint64_t> source_corner_ids;
        for (const auto& [source, target] : replacements) {
            if (target != corner.vertex) continue;
            const auto source_corner = std::find_if(
                before_topology.value().corners.begin(), before_topology.value().corners.end(),
                [&](const CornerRecord& candidate) {
                    return candidate.face == corner.face && candidate.vertex == source;
                });
            if (source_corner != before_topology.value().corners.end()) {
                source_corner_ids.push_back(source_corner->id.value);
            }
        }
        if (source_corner_ids.size() == 1U) {
            add_origin(receipt.corner_origins, corner.id,
                OriginKind::duplicated_from, std::move(source_corner_ids));
        }
    }
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
    return faces_sorted(faces_.size());
}

std::vector<Face> EditableMesh::faces_sorted(std::size_t limit) const {
    std::vector<Face> result;
    result.reserve(std::min(limit, faces_.size()));
    for (const auto& [id, face] : faces_) {
        if (result.size() >= limit) break;
        static_cast<void>(id);
        result.push_back(face);
    }
    return result;
}

std::vector<EdgeRecord> EditableMesh::edges_sorted(std::size_t limit) const {
    std::vector<EdgeRecord> result;
    result.reserve(std::min(limit, edges_.size()));
    for (const auto& [id, edge] : edges_) {
        if (result.size() >= limit) break;
        static_cast<void>(id);
        result.push_back(edge);
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
        std::vector<std::array<std::size_t, 3U>> local_triangles;
        if (face.vertices.size() > 3U) {
            std::vector<core::Vec3d> face_positions;
            face_positions.reserve(face.vertices.size());
            for (const VertexId vertex : face.vertices) {
                face_positions.push_back(vertices_.at(vertex).position);
            }
            const auto projected = project_simple_planar_polygon(face_positions);
            if (projected && is_concave_polygon(projected.value())) {
                auto triangulated = triangulate_simple_polygon(projected.value());
                if (!triangulated) {
                    return core::Result<CompiledMesh>::failure(triangulated.error());
                }
                local_triangles = std::move(triangulated.value());
            }
        }
        if (local_triangles.empty()) {
            local_triangles.reserve(face.vertices.size() - 2U);
            for (std::size_t index = 1U; index + 1U < face.vertices.size(); ++index) {
                local_triangles.push_back({0U, index, index + 1U});
            }
        }
        for (const auto& local_triangle : local_triangles) {
            const VertexId first_id = face.vertices[local_triangle[0U]];
            const VertexId second_id = face.vertices[local_triangle[1U]];
            const VertexId third_id = face.vertices[local_triangle[2U]];
            const std::uint32_t first = indices.at(first_id);
            const std::uint32_t second = indices.at(second_id);
            const std::uint32_t third = indices.at(third_id);
            compiled.indices.insert(compiled.indices.end(), {first, second, third});
            compiled.triangle_faces.push_back(face_id);
            compiled.triangle_edges.push_back({
                find_edge(first_id, second_id),
                find_edge(second_id, third_id),
                find_edge(third_id, first_id),
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
