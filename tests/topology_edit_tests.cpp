#include <carto/application/application.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/geometry/mesh.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                    \
    do {                                                                                      \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

carto::core::Result<carto::geometry::EditableMesh> make_tetrahedron() {
    carto::geometry::EditableMesh mesh;
    const auto v0 = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto v1 = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto v2 = mesh.add_vertex({0.0, 2.0, 0.0});
    const auto v3 = mesh.add_vertex({0.0, 0.0, 2.0});
    if (!v0) return carto::core::Result<carto::geometry::EditableMesh>::failure(v0.error());
    if (!v1) return carto::core::Result<carto::geometry::EditableMesh>::failure(v1.error());
    if (!v2) return carto::core::Result<carto::geometry::EditableMesh>::failure(v2.error());
    if (!v3) return carto::core::Result<carto::geometry::EditableMesh>::failure(v3.error());
    const auto first = mesh.add_face({v0.value(), v2.value(), v1.value()});
    const auto second = mesh.add_face({v0.value(), v1.value(), v3.value()});
    const auto third = mesh.add_face({v0.value(), v3.value(), v2.value()});
    const auto fourth = mesh.add_face({v1.value(), v2.value(), v3.value()});
    if (!first) return carto::core::Result<carto::geometry::EditableMesh>::failure(first.error());
    if (!second) return carto::core::Result<carto::geometry::EditableMesh>::failure(second.error());
    if (!third) return carto::core::Result<carto::geometry::EditableMesh>::failure(third.error());
    if (!fourth) return carto::core::Result<carto::geometry::EditableMesh>::failure(fourth.error());
    if (auto result = mesh.validate(); !result) {
        return carto::core::Result<carto::geometry::EditableMesh>::failure(result.error());
    }
    return carto::core::Result<carto::geometry::EditableMesh>::success(std::move(mesh));
}

void shared_vertex_position_patch_checks_all_incident_faces() {
    carto::geometry::EditableMesh mesh;
    const std::vector<carto::core::Vec3d> positions = {
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {1.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},
        {2.0, 0.0, 0.0},
        {2.0, 1.0, 0.0},
    };
    std::vector<carto::geometry::VertexId> vertices;
    for (const auto position : positions) {
        const auto added = mesh.add_vertex(position);
        REQUIRE(added);
        vertices.push_back(added.value());
    }
    REQUIRE(mesh.add_face({vertices[0], vertices[1], vertices[2], vertices[3]}));
    REQUIRE(mesh.add_face({vertices[1], vertices[4], vertices[5], vertices[2]}));
    REQUIRE(mesh.validate());

    const auto before_revision = mesh.revision();
    REQUIRE(mesh.set_vertex_position(vertices[1], {1.1, 0.1, 0.0}));
    REQUIRE(mesh.revision() == before_revision.next());
    REQUIRE(mesh.validate());

    const auto before_rejected_revision = mesh.revision();
    const auto before_rejected_position = mesh.find_vertex(vertices[1])->position;
    const auto rejected = mesh.set_vertex_position(vertices[1], {0.0, 0.0, 0.0});
    REQUIRE(!rejected);
    REQUIRE(mesh.revision() == before_rejected_revision);
    REQUIRE(mesh.find_vertex(vertices[1])->position.x == before_rejected_position.x);
    REQUIRE(mesh.find_vertex(vertices[1])->position.y == before_rejected_position.y);
    REQUIRE(mesh.find_vertex(vertices[1])->position.z == before_rejected_position.z);
    REQUIRE(mesh.validate());
}

void face_delete_is_atomic_and_reports_removed_identity() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto face = plane.value().faces_sorted().front().id;
    const auto before_revision = plane.value().revision();

    const auto deleted = plane.value().delete_face(face, true);
    REQUIRE(deleted);
    REQUIRE(deleted.value().revision_before == before_revision);
    REQUIRE(deleted.value().revision_after > before_revision);
    REQUIRE(deleted.value().removed_faces.size() == 1U);
    REQUIRE(deleted.value().removed_faces.front() == face);
    REQUIRE(deleted.value().removed_vertices.size() == 4U);
    REQUIRE(plane.value().face_count() == 0U);
    REQUIRE(plane.value().vertex_count() == 0U);
    REQUIRE(plane.value().validate());

    const auto missing = plane.value().delete_face(face, true);
    REQUIRE(!missing);
    REQUIRE(missing.error().code == carto::core::ErrorCode::not_found);
}

void face_delete_can_preserve_orphans_without_invalid_topology() {
    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto face = box.value().faces_sorted().front().id;
    const auto deleted = box.value().delete_face(face, false);
    REQUIRE(deleted);
    REQUIRE(deleted.value().removed_vertices.empty());
    REQUIRE(box.value().face_count() == 5U);
    REQUIRE(box.value().vertex_count() == 8U);
    REQUIRE(box.value().validate());
}

void face_delete_preserves_shared_vertices_when_removing_orphans() {
    carto::geometry::EditableMesh mesh;
    const std::vector<carto::core::Vec3d> positions = {
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {1.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},
        {2.0, 0.0, 0.0},
        {2.0, 1.0, 0.0},
    };
    std::vector<carto::geometry::VertexId> vertices;
    for (const auto position : positions) {
        const auto added = mesh.add_vertex(position);
        REQUIRE(added);
        vertices.push_back(added.value());
    }
    const auto first_face = mesh.add_face({vertices[0], vertices[1], vertices[2], vertices[3]});
    REQUIRE(first_face);
    REQUIRE(mesh.add_face({vertices[1], vertices[4], vertices[5], vertices[2]}));

    const auto deleted = mesh.delete_face(first_face.value(), true);
    REQUIRE(deleted);
    REQUIRE(deleted.value().removed_vertices.size() == 2U);
    REQUIRE(deleted.value().removed_vertices[0] == vertices[0]);
    REQUIRE(deleted.value().removed_vertices[1] == vertices[3]);
    REQUIRE(mesh.find_vertex(vertices[1]) != nullptr);
    REQUIRE(mesh.find_vertex(vertices[2]) != nullptr);
    REQUIRE(mesh.face_count() == 1U);
    REQUIRE(mesh.vertex_count() == 4U);
    REQUIRE(mesh.validate());
}

void face_extrude_is_atomic_and_advances_once() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto face = plane.value().faces_sorted().front().id;
    const auto before_revision = plane.value().revision();

    REQUIRE(plane.value().extrude_face(face, 0.25));
    REQUIRE(plane.value().revision() == before_revision.next());
    REQUIRE(plane.value().vertex_count() == 8U);
    REQUIRE(plane.value().face_count() == 5U);
    REQUIRE(plane.value().find_face(face) == nullptr);
    REQUIRE(plane.value().validate());
}

void edge_split_is_atomic_and_reports_topology_delta() {
    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto topology = box.value().topology();
    REQUIRE(topology);
    REQUIRE(!topology.value().edges.empty());
    const auto edge = topology.value().edges.front().id;
    const auto before_revision = box.value().revision();

    const auto split = box.value().split_edge(edge, 0.5);
    REQUIRE(split);
    REQUIRE(split.value().revision_before == before_revision);
    REQUIRE(split.value().revision_after > before_revision);
    REQUIRE(split.value().created_vertices.size() == 1U);
    REQUIRE(split.value().created_edges.size() >= 2U);
    REQUIRE(split.value().removed_edges.size() == 1U);
    REQUIRE(split.value().removed_edges.front() == edge);
    REQUIRE(box.value().vertex_count() == 9U);
    REQUIRE(box.value().face_count() == 6U);
    REQUIRE(box.value().find_edge(edge) == nullptr);
    REQUIRE(box.value().find_vertex(split.value().created_vertices.front()) != nullptr);
    REQUIRE(box.value().validate());
}

void edge_split_rejects_invalid_factors_without_mutation() {
    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto topology = box.value().topology();
    REQUIRE(topology);
    const auto edge = topology.value().edges.front().id;
    const auto before_revision = box.value().revision();
    const auto before_vertices = box.value().vertex_count();

    for (const double factor : {
             0.0,
             1.0,
             std::numeric_limits<double>::quiet_NaN(),
         }) {
        const auto split = box.value().split_edge(edge, factor);
        REQUIRE(!split);
        REQUIRE(split.error().code == carto::core::ErrorCode::invalid_argument);
        REQUIRE(box.value().revision() == before_revision);
        REQUIRE(box.value().vertex_count() == before_vertices);
        REQUIRE(box.value().find_edge(edge) != nullptr);
    }
}

void edge_bevel_is_bounded_atomic_and_reports_lineage() {
    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto topology = box.value().topology();
    REQUIRE(topology);
    REQUIRE(!topology.value().edges.empty());
    const auto edge = topology.value().edges.front().id;
    const auto before_revision = box.value().revision();

    const auto beveled = box.value().bevel_edge(edge, 0.1);
    REQUIRE(beveled);
    REQUIRE(beveled.value().revision_before == before_revision);
    REQUIRE(beveled.value().revision_after == before_revision.next());
    REQUIRE(beveled.value().created_vertices.size() == 4U);
    REQUIRE(beveled.value().created_faces.size() == 1U);
    REQUIRE(beveled.value().removed_edges.size() == 1U);
    REQUIRE(beveled.value().removed_edges.front() == edge);
    REQUIRE(beveled.value().vertex_origins.size() == 4U);
    REQUIRE(beveled.value().face_origins.at(beveled.value().created_faces.front()).kind ==
            carto::geometry::OriginKind::generated_from_edge);
    REQUIRE(beveled.value().validate());
    REQUIRE(box.value().vertex_count() == 12U);
    REQUIRE(box.value().face_count() == 7U);
    REQUIRE(box.value().find_edge(edge) == nullptr);
    REQUIRE(box.value().validate());
}

void edge_bevel_rejects_boundary_large_and_multisegment_inputs_without_mutation() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto plane_topology = plane.value().topology();
    REQUIRE(plane_topology);
    const auto boundary_edge = plane_topology.value().edges.front().id;
    const auto plane_revision = plane.value().revision();
    const auto boundary = plane.value().bevel_edge(boundary_edge, 0.1);
    REQUIRE(!boundary);
    REQUIRE(boundary.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(plane.value().revision() == plane_revision);
    REQUIRE(plane.value().vertex_count() == 4U);
    REQUIRE(plane.value().face_count() == 1U);

    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto topology = box.value().topology();
    REQUIRE(topology);
    const auto edge = topology.value().edges.front().id;
    const auto before_revision = box.value().revision();
    const auto before_vertices = box.value().vertex_count();
    for (const double width : {
             0.0,
             -0.1,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
         }) {
        const auto invalid_width = box.value().bevel_edge(edge, width);
        REQUIRE(!invalid_width);
        REQUIRE(invalid_width.error().code == carto::core::ErrorCode::invalid_argument);
        REQUIRE(box.value().revision() == before_revision);
        REQUIRE(box.value().vertex_count() == before_vertices);
    }
    const auto missing = box.value().bevel_edge(carto::geometry::EdgeId{999999U}, 0.1);
    REQUIRE(!missing);
    REQUIRE(missing.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(box.value().revision() == before_revision);
    const auto too_wide = box.value().bevel_edge(edge, 0.5);
    REQUIRE(!too_wide);
    REQUIRE(too_wide.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(box.value().revision() == before_revision);
    REQUIRE(box.value().vertex_count() == before_vertices);

    const auto multisegment = box.value().bevel_edge(edge, 0.1, 2U);
    REQUIRE(!multisegment);
    REQUIRE(multisegment.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(box.value().revision() == before_revision);
    REQUIRE(box.value().vertex_count() == before_vertices);
    REQUIRE(box.value().validate());
}

void edge_bevel_rejects_nonconvex_and_nonplanar_faces_without_mutation() {
    carto::geometry::EditableMesh concave;
    const auto a = concave.add_vertex({0.0, 0.0, 0.0});
    const auto b = concave.add_vertex({2.0, 0.0, 0.0});
    const auto c = concave.add_vertex({2.0, 2.0, 0.0});
    const auto d = concave.add_vertex({1.0, 0.75, 0.0});
    const auto e = concave.add_vertex({0.0, 2.0, 0.0});
    const auto f = concave.add_vertex({1.0, -1.0, 0.0});
    REQUIRE(a && b && c && d && e && f);
    REQUIRE(concave.add_face({a.value(), b.value(), c.value(), d.value(), e.value()}));
    REQUIRE(concave.add_face({b.value(), a.value(), f.value()}));
    REQUIRE(concave.validate());
    const auto concave_topology = concave.topology();
    REQUIRE(concave_topology);
    const auto concave_edge = std::find_if(
        concave_topology.value().edges.begin(), concave_topology.value().edges.end(),
        [a, b](const auto& edge) {
            return edge.first == std::min(a.value(), b.value()) &&
                edge.second == std::max(a.value(), b.value());
        });
    REQUIRE(concave_edge != concave_topology.value().edges.end());
    const auto concave_revision = concave.revision();
    const auto rejected_concave = concave.bevel_edge(concave_edge->id, 0.1);
    REQUIRE(!rejected_concave);
    REQUIRE(rejected_concave.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(concave.revision() == concave_revision);
    REQUIRE(concave.validate());

    carto::geometry::EditableMesh nonplanar;
    const auto na = nonplanar.add_vertex({0.0, 0.0, 0.0});
    const auto nb = nonplanar.add_vertex({2.0, 0.0, 0.0});
    const auto nc = nonplanar.add_vertex({2.0, 2.0, 0.0});
    const auto nd = nonplanar.add_vertex({0.0, 2.0, 0.25});
    const auto ne = nonplanar.add_vertex({1.0, -1.0, 0.0});
    REQUIRE(na && nb && nc && nd && ne);
    REQUIRE(nonplanar.add_face({na.value(), nb.value(), nc.value(), nd.value()}));
    REQUIRE(nonplanar.add_face({nb.value(), na.value(), ne.value()}));
    REQUIRE(nonplanar.validate());
    const auto nonplanar_topology = nonplanar.topology();
    REQUIRE(nonplanar_topology);
    const auto nonplanar_edge = std::find_if(
        nonplanar_topology.value().edges.begin(), nonplanar_topology.value().edges.end(),
        [na, nb](const auto& edge) {
            return edge.first == std::min(na.value(), nb.value()) &&
                edge.second == std::max(na.value(), nb.value());
        });
    REQUIRE(nonplanar_edge != nonplanar_topology.value().edges.end());
    const auto nonplanar_revision = nonplanar.revision();
    const auto rejected_nonplanar = nonplanar.bevel_edge(nonplanar_edge->id, 0.1);
    REQUIRE(!rejected_nonplanar);
    REQUIRE(rejected_nonplanar.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(nonplanar.revision() == nonplanar_revision);
    REQUIRE(nonplanar.validate());
}

void vertex_bevel_is_bounded_atomic_and_reports_lineage() {
    auto tetrahedron = make_tetrahedron();
    REQUIRE(tetrahedron);
    auto mesh = std::move(tetrahedron.value());
    const carto::geometry::VertexId vertex{1U};
    const auto before_revision = mesh.revision();
    const auto before_faces = mesh.face_count();

    const auto beveled = mesh.bevel_vertex(vertex, 0.25);
    REQUIRE(beveled);
    REQUIRE(beveled.value().revision_before == before_revision);
    REQUIRE(beveled.value().revision_after == before_revision.next());
    REQUIRE(beveled.value().created_vertices.size() == 3U);
    REQUIRE(beveled.value().created_faces.size() == 1U);
    REQUIRE(beveled.value().removed_vertices.size() == 1U);
    REQUIRE(beveled.value().removed_vertices.front() == vertex);
    REQUIRE(beveled.value().vertex_origins.size() == 3U);
    REQUIRE(beveled.value().face_origins.size() == 1U);
    REQUIRE(mesh.find_vertex(vertex) == nullptr);
    REQUIRE(mesh.vertex_count() == 6U);
    REQUIRE(mesh.face_count() == before_faces + 1U);
    REQUIRE(mesh.validate());
    REQUIRE(beveled.value().validate());
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().source_revision == mesh.revision());
}

void vertex_bevel_rejects_boundary_shape_and_numeric_inputs_without_mutation() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto plane_vertex = plane.value().vertices_sorted().front().id;
    const auto plane_revision = plane.value().revision();
    const auto boundary = plane.value().bevel_vertex(plane_vertex, 0.1);
    REQUIRE(!boundary);
    REQUIRE(boundary.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(plane.value().revision() == plane_revision);
    REQUIRE(plane.value().find_vertex(plane_vertex) != nullptr);
    REQUIRE(plane.value().validate());

    auto tetrahedron = make_tetrahedron();
    REQUIRE(tetrahedron);
    auto mesh = std::move(tetrahedron.value());
    const auto before_revision = mesh.revision();
    const auto before_vertices = mesh.vertex_count();
    for (const auto [width, segments] : {
             std::pair{0.0, 1U},
             std::pair{std::numeric_limits<double>::quiet_NaN(), 1U},
             std::pair{std::numeric_limits<double>::infinity(), 1U},
             std::pair{1.0, 1U},
             std::pair{0.1, 2U},
         }) {
        const auto rejected = mesh.bevel_vertex(carto::geometry::VertexId{1U}, width, segments);
        REQUIRE(!rejected);
        REQUIRE(mesh.revision() == before_revision);
        REQUIRE(mesh.vertex_count() == before_vertices);
        REQUIRE(mesh.find_vertex(carto::geometry::VertexId{1U}) != nullptr);
        REQUIRE(mesh.validate());
    }
    const auto missing = mesh.bevel_vertex(carto::geometry::VertexId{999999U}, 0.1);
    REQUIRE(!missing);
    REQUIRE(missing.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(mesh.revision() == before_revision);

    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto box_vertex = box.value().vertices_sorted().front().id;
    const auto box_revision = box.value().revision();
    const auto non_triangular = box.value().bevel_vertex(box_vertex, 0.1);
    REQUIRE(!non_triangular);
    REQUIRE(non_triangular.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(box.value().revision() == box_revision);
    REQUIRE(box.value().validate());
}

void coplanar_internal_edge_dissolve_is_atomic_and_reports_lineage() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    const auto first_face = mesh.add_face({a.value(), b.value(), c.value()});
    const auto second_face = mesh.add_face({a.value(), c.value(), d.value()});
    REQUIRE(first_face && second_face);
    REQUIRE(mesh.validate());

    const auto topology = mesh.topology();
    REQUIRE(topology);
    const auto shared = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [a, c](const carto::geometry::EdgeRecord& edge) {
            return (edge.first == a.value() && edge.second == c.value());
        });
    REQUIRE(shared != topology.value().edges.end());
    const auto before_revision = mesh.revision();
    const auto survivor = std::min(first_face.value(), second_face.value());
    const auto removed = std::max(first_face.value(), second_face.value());

    const auto dissolved = mesh.dissolve_edge(shared->id);
    REQUIRE(dissolved);
    REQUIRE(dissolved.value().revision_before == before_revision);
    REQUIRE(dissolved.value().revision_after == before_revision.next());
    REQUIRE(dissolved.value().created_faces.empty());
    REQUIRE(dissolved.value().removed_faces.size() == 1U);
    REQUIRE(dissolved.value().removed_faces.front() == removed);
    REQUIRE(std::find(
        dissolved.value().removed_edges.begin(), dissolved.value().removed_edges.end(),
        shared->id) != dissolved.value().removed_edges.end());
    REQUIRE(!dissolved.value().created_corners.empty());
    REQUIRE(!dissolved.value().corner_origins.empty());
    REQUIRE(mesh.face_count() == 1U);
    REQUIRE(mesh.vertex_count() == 4U);
    REQUIRE(mesh.find_face(survivor) != nullptr);
    REQUIRE(mesh.find_face(removed) == nullptr);
    REQUIRE(mesh.find_edge(shared->id) == nullptr);
    REQUIRE(mesh.validate());
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().triangle_faces.size() == 2U);
    REQUIRE(dissolved.value().validate());

    const auto encoded = dissolved.value().serialize();
    const auto decoded = carto::geometry::TopologyEditReceipt::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == encoded);
}

void edge_dissolve_rejects_boundary_and_nonplanar_joins_without_mutation() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto plane_topology = plane.value().topology();
    REQUIRE(plane_topology);
    const auto boundary_edge = plane_topology.value().edges.front().id;
    const auto plane_revision = plane.value().revision();
    const auto boundary = plane.value().dissolve_edge(boundary_edge);
    REQUIRE(!boundary);
    REQUIRE(boundary.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(plane.value().revision() == plane_revision);
    REQUIRE(plane.value().find_edge(boundary_edge) != nullptr);
    REQUIRE(plane.value().validate());

    carto::geometry::EditableMesh nonplanar;
    const auto a = nonplanar.add_vertex({0.0, 0.0, 0.0});
    const auto b = nonplanar.add_vertex({1.0, 0.0, 0.0});
    const auto c = nonplanar.add_vertex({1.0, 1.0, 0.0});
    const auto d = nonplanar.add_vertex({0.0, 1.0, 0.5});
    REQUIRE(a && b && c && d);
    REQUIRE(nonplanar.add_face({a.value(), b.value(), c.value()}));
    REQUIRE(nonplanar.add_face({a.value(), c.value(), d.value()}));
    REQUIRE(nonplanar.validate());
    const auto nonplanar_topology = nonplanar.topology();
    REQUIRE(nonplanar_topology);
    const auto shared = std::find_if(
        nonplanar_topology.value().edges.begin(), nonplanar_topology.value().edges.end(),
        [a, c](const carto::geometry::EdgeRecord& edge) {
            return edge.first == a.value() && edge.second == c.value();
        });
    REQUIRE(shared != nonplanar_topology.value().edges.end());
    const auto nonplanar_revision = nonplanar.revision();
    const auto nonplanar_faces = nonplanar.face_count();
    const auto rejected = nonplanar.dissolve_edge(shared->id);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(nonplanar.revision() == nonplanar_revision);
    REQUIRE(nonplanar.face_count() == nonplanar_faces);
    REQUIRE(nonplanar.find_edge(shared->id) != nullptr);
    REQUIRE(nonplanar.validate());
}

void triangle_to_quad_requires_two_compatible_triangles() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    const auto first = mesh.add_face({a.value(), b.value(), c.value()});
    const auto second = mesh.add_face({a.value(), c.value(), d.value()});
    REQUIRE(first && second);
    const auto before_revision = mesh.revision();
    const auto converted = mesh.tri_to_quad(first.value(), second.value());
    REQUIRE(converted);
    REQUIRE(converted.value().revision_before == before_revision);
    REQUIRE(converted.value().removed_faces.size() == 1U);
    REQUIRE(mesh.face_count() == 1U);
    REQUIRE(mesh.validate());

    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto faces = box.value().faces_sorted();
    REQUIRE(faces.size() >= 2U);
    const auto box_revision = box.value().revision();
    const auto rejected = box.value().tri_to_quad(faces[0].id, faces[1].id);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(box.value().revision() == box_revision);
    REQUIRE(box.value().face_count() == faces.size());
    REQUIRE(box.value().validate());
}

void vertex_slide_is_bounded_and_atomic() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto vertices = plane.value().vertices_sorted();
    const auto topology = plane.value().topology();
    REQUIRE(vertices.size() == 4U);
    REQUIRE(topology);
    const auto support = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [vertex = vertices.front().id](const carto::geometry::EdgeRecord& edge) {
            return edge.first == vertex || edge.second == vertex;
        });
    REQUIRE(support != topology.value().edges.end());
    const auto nonincident = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [vertex = vertices.front().id](const carto::geometry::EdgeRecord& edge) {
            return edge.first != vertex && edge.second != vertex;
        });
    REQUIRE(nonincident != topology.value().edges.end());

    const auto before_revision = plane.value().revision();
    const auto before_position = plane.value().find_vertex(vertices.front().id)->position;
    REQUIRE(plane.value().slide_vertex(vertices.front().id, support->id, 0.25));
    const auto after_position = plane.value().find_vertex(vertices.front().id)->position;
    REQUIRE(after_position.x != before_position.x ||
            after_position.y != before_position.y ||
            after_position.z != before_position.z);
    REQUIRE(plane.value().revision() == before_revision.next());
    REQUIRE(plane.value().face_count() == 1U);
    REQUIRE(plane.value().vertex_count() == 4U);
    REQUIRE(plane.value().validate());

    const auto rejected_revision = plane.value().revision();
    REQUIRE(!plane.value().slide_vertex(vertices.front().id, support->id, 0.0));
    REQUIRE(!plane.value().slide_vertex(vertices.front().id, nonincident->id, 0.25));
    REQUIRE(!plane.value().slide_vertex(vertices.front().id, support->id, 1.0));
    REQUIRE(plane.value().revision() == rejected_revision);
    REQUIRE(plane.value().validate());
}

void target_weld_preserves_target_and_reports_merge_lineage() {
    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    const auto collapsed_face = mesh.add_face({target.value(), source.value(), third.value()});
    const auto surviving_face = mesh.add_face({source.value(), fourth.value(), third.value()});
    REQUIRE(collapsed_face && surviving_face);
    REQUIRE(mesh.validate());

    const auto before_revision = mesh.revision();
    const auto before_target_position = mesh.find_vertex(target.value())->position;
    const auto merged = mesh.merge_vertices(target.value(), source.value());
    REQUIRE(merged);
    REQUIRE(merged.value().revision_before == before_revision);
    REQUIRE(merged.value().revision_after == before_revision.next());
    REQUIRE(merged.value().merged_vertices.at(source.value()) == target.value());
    REQUIRE(std::find(
        merged.value().removed_vertices.begin(), merged.value().removed_vertices.end(),
        source.value()) != merged.value().removed_vertices.end());
    REQUIRE(std::find(
        merged.value().removed_faces.begin(), merged.value().removed_faces.end(),
        collapsed_face.value()) != merged.value().removed_faces.end());
    REQUIRE(mesh.find_vertex(target.value()) != nullptr);
    REQUIRE(mesh.find_vertex(source.value()) == nullptr);
    REQUIRE(mesh.find_vertex(target.value())->position.x == before_target_position.x);
    REQUIRE(mesh.find_vertex(target.value())->position.y == before_target_position.y);
    REQUIRE(mesh.face_count() == 1U);
    REQUIRE(mesh.vertex_count() == 3U);
    REQUIRE(mesh.validate());

    const auto encoded = merged.value().serialize();
    REQUIRE(encoded.find("MERGED_VERTICES 1") != std::string::npos);
    const auto decoded = carto::geometry::TopologyEditReceipt::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().merged_vertices == merged.value().merged_vertices);
    REQUIRE(decoded.value().serialize() == encoded);
}

void target_weld_rejects_ambiguous_collapses_without_mutation() {
    carto::geometry::EditableMesh mesh;
    const auto first = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto second = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(first && second && third && fourth);
    REQUIRE(mesh.add_face({first.value(), second.value(), third.value(), fourth.value()}));
    REQUIRE(mesh.validate());
    const auto before_revision = mesh.revision();
    const auto before_serialized = mesh.compile().value().source_revision;

    const auto ambiguous = mesh.merge_vertices(first.value(), third.value());
    REQUIRE(!ambiguous);
    REQUIRE(ambiguous.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(mesh.revision() == before_revision);
    REQUIRE(mesh.find_vertex(third.value()) != nullptr);
    REQUIRE(mesh.face_count() == 1U);
    REQUIRE(mesh.compile().value().source_revision == before_serialized);
    REQUIRE(mesh.validate());

    const auto same_vertex = mesh.merge_vertices(first.value(), first.value());
    REQUIRE(!same_vertex);
    REQUIRE(same_vertex.error().code == carto::core::ErrorCode::invalid_argument);
    const auto missing_vertex = mesh.merge_vertices(first.value(), carto::geometry::VertexId{99U});
    REQUIRE(!missing_vertex);
    REQUIRE(missing_vertex.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(mesh.revision() == before_revision);
}

void component_selection_conversion_is_deterministic_and_non_mutating() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto vertices = plane.value().vertices_sorted();
    REQUIRE(vertices.size() == 4U);

    carto::editor::SelectionState selection;
    REQUIRE(selection.select_vertex(plane.value(), vertices[0].id));
    for (std::size_t index = 1U; index < vertices.size(); ++index) {
        REQUIRE(selection.select_vertex(
            plane.value(), vertices[index].id, carto::editor::SelectionOperation::add));
    }
    const auto before_revision = plane.value().revision();
    REQUIRE(selection.convert_mode(carto::editor::SelectionMode::edge, plane.value()));
    REQUIRE(selection.mode() == carto::editor::SelectionMode::edge);
    REQUIRE(selection.selected_edges().size() == 4U);
    REQUIRE(plane.value().revision() == before_revision);

    REQUIRE(selection.convert_mode(carto::editor::SelectionMode::face, plane.value()));
    REQUIRE(selection.mode() == carto::editor::SelectionMode::face);
    REQUIRE(selection.selected_faces().size() == 1U);
    REQUIRE(selection.convert_mode(carto::editor::SelectionMode::vertex, plane.value()));
    REQUIRE(selection.mode() == carto::editor::SelectionMode::vertex);
    REQUIRE(selection.selected_vertices().size() == 4U);
    REQUIRE(plane.value().revision() == before_revision);

    carto::editor::SelectionState shortest_path;
    REQUIRE(shortest_path.select_vertex(plane.value(), vertices.front().id));
    REQUIRE(shortest_path.select_shortest_path(vertices.back().id, plane.value()));
    REQUIRE(shortest_path.selected_vertices().size() >= 2U);
    REQUIRE(shortest_path.selected_vertices().front() == vertices.front().id);
    REQUIRE(shortest_path.selected_vertices().back() == vertices.back().id);
    REQUIRE(plane.value().revision() == before_revision);

    carto::editor::SelectionState partial;
    REQUIRE(partial.select_vertex(plane.value(), vertices[0].id));
    REQUIRE(partial.select_vertex(
        plane.value(), vertices[1].id, carto::editor::SelectionOperation::add));
    REQUIRE(partial.convert_mode(carto::editor::SelectionMode::face, plane.value()));
    REQUIRE(partial.selected_faces().empty());
    REQUIRE(partial.validate(plane.value()));
}

void component_selection_expansion_is_deterministic_and_non_mutating() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto vertices = plane.value().vertices_sorted();
    REQUIRE(vertices.size() == 4U);
    carto::editor::SelectionState selection;
    REQUIRE(selection.select_vertex(plane.value(), vertices.front().id));
    const auto before_revision = plane.value().revision();
    REQUIRE(selection.expand(carto::editor::SelectionExpansion::grow, plane.value()));
    REQUIRE(selection.selected_vertices().size() == 3U);
    REQUIRE(selection.expand(carto::editor::SelectionExpansion::shrink, plane.value()));
    REQUIRE(selection.selected_vertices().size() == 1U);
    REQUIRE(selection.expand(carto::editor::SelectionExpansion::invert, plane.value()));
    REQUIRE(selection.selected_vertices().size() == 3U);
    REQUIRE(selection.expand(carto::editor::SelectionExpansion::linked, plane.value()));
    REQUIRE(selection.selected_vertices().size() == 4U);
    REQUIRE(plane.value().revision() == before_revision);

    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto box_topology = box.value().topology();
    REQUIRE(box_topology);
    carto::editor::SelectionState edges;
    REQUIRE(edges.select_edge(box.value(), box_topology.value().edges.front().id));
    REQUIRE(edges.expand(carto::editor::SelectionExpansion::linked, box.value()));
    REQUIRE(edges.selected_edges().size() == box_topology.value().edges.size());

    carto::editor::SelectionState loop_edges;
    REQUIRE(loop_edges.select_edge(box.value(), box_topology.value().edges.front().id));
    REQUIRE(loop_edges.expand(carto::editor::SelectionExpansion::loop, box.value()));
    REQUIRE(!loop_edges.selected_edges().empty());
    const auto loop_selection = loop_edges.selected_edges();
    REQUIRE(std::find(
        loop_selection.begin(), loop_selection.end(),
        box_topology.value().edges.front().id) != loop_selection.end());

    carto::editor::SelectionState ring_edges;
    REQUIRE(ring_edges.select_edge(box.value(), box_topology.value().edges.front().id));
    REQUIRE(ring_edges.expand(carto::editor::SelectionExpansion::ring, box.value()));
    REQUIRE(!ring_edges.selected_edges().empty());

    auto plane_boundary = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane_boundary);
    const auto plane_topology = plane_boundary.value().topology();
    REQUIRE(plane_topology);
    carto::editor::SelectionState boundary_edges;
    REQUIRE(boundary_edges.select_edge(
        plane_boundary.value(), plane_topology.value().edges.front().id));
    REQUIRE(boundary_edges.expand(
        carto::editor::SelectionExpansion::boundary, plane_boundary.value()));
    REQUIRE(boundary_edges.selected_edges().size() == 4U);

    carto::editor::SelectionState stale;
    REQUIRE(stale.select_vertex(box.value(), box.value().vertices_sorted().front().id));
    REQUIRE(box.value().set_vertex_position(
        box.value().vertices_sorted().front().id, {0.6, -0.5, -0.5}));
    const auto rejected = stale.expand(carto::editor::SelectionExpansion::grow, box.value());
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::stale_data);
}

void admitted_edge_split_rejects_wrong_mode_and_multi_selection() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto initial = session.snapshot();
    const auto object = initial.objects.front().object.id;
    const auto triangle_edges = initial.viewport.scene.instances().front().mesh->triangle_edges.front();
    REQUIRE(triangle_edges[0].has_value());
    REQUIRE(triangle_edges[1].has_value());
    REQUIRE(*triangle_edges[0] != *triangle_edges[1]);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectEdgeAction{object, *triangle_edges[0]}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    const auto wrong_mode_revision = session.snapshot().project_revision;
    const auto wrong_mode = carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::InvokeToolAction{"mesh.split-edge", {}});
    REQUIRE(!wrong_mode);
    REQUIRE(wrong_mode.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == wrong_mode_revision);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 8U);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectEdgeAction{object, *triangle_edges[0]}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SelectEdgeAction{
            object, *triangle_edges[1], carto::editor::SelectionOperation::add}));
    const auto multi_selection_revision = session.snapshot().project_revision;
    const auto multi_selection = carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::InvokeToolAction{"mesh.split-edge", {}});
    REQUIRE(!multi_selection);
    REQUIRE(multi_selection.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == multi_selection_revision);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 8U);
}

void convex_face_inset_is_atomic_and_reports_topology_delta() {
    auto box = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(box);
    const auto face = box.value().faces_sorted().front().id;
    const auto before_revision = box.value().revision();

    const auto inset = box.value().inset_face(face, 0.25);
    REQUIRE(inset);
    REQUIRE(inset.value().revision_before == before_revision);
    REQUIRE(inset.value().revision_after > before_revision);
    REQUIRE(inset.value().created_vertices.size() == 4U);
    REQUIRE(inset.value().created_faces.size() == 5U);
    REQUIRE(inset.value().removed_faces.size() == 1U);
    REQUIRE(inset.value().removed_faces.front() == face);
    REQUIRE(box.value().vertex_count() == 12U);
    REQUIRE(box.value().face_count() == 10U);
    REQUIRE(box.value().find_face(face) == nullptr);
    REQUIRE(box.value().validate());
    REQUIRE(box.value().compile());
}

void convex_face_poke_is_atomic_and_reports_topology_delta() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto face = plane.value().faces_sorted().front().id;
    const auto before_revision = plane.value().revision();

    const auto poked = plane.value().poke_face(face);
    REQUIRE(poked);
    REQUIRE(poked.value().revision_before == before_revision);
    REQUIRE(poked.value().revision_after == before_revision.next());
    REQUIRE(poked.value().created_vertices.size() == 1U);
    REQUIRE(poked.value().created_faces.size() == 4U);
    REQUIRE(poked.value().removed_faces.size() == 1U);
    REQUIRE(poked.value().removed_faces.front() == face);
    REQUIRE(poked.value().vertex_origins.size() == 1U);
    REQUIRE(poked.value().face_origins.size() == 4U);
    REQUIRE(poked.value().edge_origins.size() == poked.value().created_edges.size());
    REQUIRE(!poked.value().corner_origins.empty());
    REQUIRE(plane.value().vertex_count() == 5U);
    REQUIRE(plane.value().face_count() == 4U);
    REQUIRE(plane.value().find_face(face) == nullptr);
    REQUIRE(plane.value().validate());
    REQUIRE(plane.value().compile());

    const auto encoded = poked.value().serialize();
    const auto decoded = carto::geometry::TopologyEditReceipt::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == encoded);
}

void face_poke_rejects_nonconvex_and_nonplanar_faces_without_mutation() {
    const auto assert_rejected = [](carto::geometry::EditableMesh mesh) {
        const auto before_revision = mesh.revision();
        const auto before_vertices = mesh.vertex_count();
        const auto before_faces = mesh.face_count();
        const auto face = mesh.faces_sorted().front().id;
        const auto result = mesh.poke_face(face);
        REQUIRE(!result);
        REQUIRE(result.error().code == carto::core::ErrorCode::unsupported ||
                result.error().code == carto::core::ErrorCode::validation_failed);
        REQUIRE(mesh.revision() == before_revision);
        REQUIRE(mesh.vertex_count() == before_vertices);
        REQUIRE(mesh.face_count() == before_faces);
        REQUIRE(mesh.find_face(face) != nullptr);
        REQUIRE(mesh.validate());
    };

    carto::geometry::EditableMesh concave;
    std::vector<carto::geometry::VertexId> concave_vertices;
    for (const carto::core::Vec3d position : {
             carto::core::Vec3d{0.0, 0.0, 0.0},
             carto::core::Vec3d{2.0, 0.0, 0.0},
             carto::core::Vec3d{2.0, 2.0, 0.0},
             carto::core::Vec3d{1.0, 0.5, 0.0},
             carto::core::Vec3d{0.0, 2.0, 0.0},
         }) {
        const auto vertex = concave.add_vertex(position);
        REQUIRE(vertex);
        concave_vertices.push_back(vertex.value());
    }
    REQUIRE(concave.add_face(concave_vertices));
    assert_rejected(std::move(concave));

    carto::geometry::EditableMesh nonplanar;
    std::vector<carto::geometry::VertexId> nonplanar_vertices;
    for (const carto::core::Vec3d position : {
             carto::core::Vec3d{0.0, 0.0, 0.0},
             carto::core::Vec3d{2.0, 0.0, 0.0},
             carto::core::Vec3d{2.0, 2.0, 0.25},
             carto::core::Vec3d{0.0, 2.0, 0.0},
         }) {
        const auto vertex = nonplanar.add_vertex(position);
        REQUIRE(vertex);
        nonplanar_vertices.push_back(vertex.value());
    }
    REQUIRE(nonplanar.add_face(nonplanar_vertices));
    assert_rejected(std::move(nonplanar));

}

void topology_receipts_preserve_operation_lineage() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto face = plane.value().faces_sorted().front().id;
    const auto extruded = plane.value().extrude_face_with_receipt(face, 0.25);
    REQUIRE(extruded);
    REQUIRE(extruded.value().validate());
    REQUIRE(extruded.value().vertex_origins.size() == 4U);
    REQUIRE(extruded.value().face_origins.size() == 5U);
    for (const auto& [created, origin] : extruded.value().vertex_origins) {
        REQUIRE(std::find(
            extruded.value().created_vertices.begin(),
            extruded.value().created_vertices.end(), created) !=
            extruded.value().created_vertices.end());
        REQUIRE(origin.kind == carto::geometry::OriginKind::duplicated_from);
        REQUIRE(origin.source_ids.size() == 1U);
    }
    REQUIRE(!extruded.value().edge_origins.empty());
    REQUIRE(!extruded.value().corner_origins.empty());

    auto split_mesh = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(split_mesh);
    const auto split_topology = split_mesh.value().topology();
    REQUIRE(split_topology);
    const auto split = split_mesh.value().split_edge(split_topology.value().edges.front().id);
    REQUIRE(split);
    REQUIRE(split.value().validate());
    REQUIRE(split.value().vertex_origins.size() == 1U);
    REQUIRE(split.value().vertex_origins.begin()->second.kind ==
            carto::geometry::OriginKind::interpolated_from);
    REQUIRE(split.value().edge_origins.size() == split.value().created_edges.size());

    auto inset_mesh = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(inset_mesh);
    const auto inset_face = inset_mesh.value().faces_sorted().front().id;
    const auto inset = inset_mesh.value().inset_face(inset_face, 0.25);
    REQUIRE(inset);
    REQUIRE(inset.value().validate());
    REQUIRE(inset.value().vertex_origins.size() == 4U);
    REQUIRE(inset.value().face_origins.size() == 5U);
    REQUIRE(inset.value().edge_origins.size() == inset.value().created_edges.size());
    REQUIRE(!inset.value().corner_origins.empty());
}

void topology_traversals_are_validated_and_deterministic() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto plane_topology = plane.value().topology();
    REQUIRE(plane_topology);
    const auto plane_edge = plane_topology.value().edges.front().id;
    const auto boundary = plane_topology.value().boundary_loop(plane_edge);
    REQUIRE(boundary);
    REQUIRE(boundary.value().size() == 4U);
    REQUIRE(boundary.value().front() == plane_edge);
    const auto plane_loop = plane_topology.value().edge_loop(plane_edge);
    REQUIRE(plane_loop);
    REQUIRE(plane_loop.value().size() == 2U);
    REQUIRE(plane_loop.value().front() == plane_edge);
    const auto plane_ring = plane_topology.value().edge_ring(plane_edge);
    REQUIRE(plane_ring);
    REQUIRE(plane_ring.value().size() == 2U);
    REQUIRE(plane_ring.value().front() == plane_edge);
    const auto plane_fan = plane_topology.value().vertex_fan(
        plane.value().vertices_sorted().front().id);
    REQUIRE(plane_fan);
    REQUIRE(plane_fan.value().size() == 1U);
    const auto plane_region = plane_topology.value().face_region(
        plane.value().faces_sorted().front().id);
    REQUIRE(plane_region);
    REQUIRE(plane_region.value().size() == 1U);
    const auto plane_vertices = plane.value().vertices_sorted();
    const auto plane_path = plane_topology.value().shortest_path(
        plane_vertices.front().id, plane_vertices.back().id);
    REQUIRE(plane_path);
    REQUIRE(plane_path.value().front() == plane_vertices.front().id);
    REQUIRE(plane_path.value().back() == plane_vertices.back().id);

    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto box_topology = box.value().topology();
    REQUIRE(box_topology);
    const auto box_loop = box_topology.value().edge_loop(box_topology.value().edges.front().id);
    REQUIRE(box_loop);
    REQUIRE(!box_loop.value().empty());
    REQUIRE(std::set<carto::geometry::EdgeId>(
        box_loop.value().begin(), box_loop.value().end()).size() == box_loop.value().size());
    const auto box_ring = box_topology.value().edge_ring(box_topology.value().edges.front().id);
    REQUIRE(box_ring);
    REQUIRE(!box_ring.value().empty());
    REQUIRE(std::set<carto::geometry::EdgeId>(
        box_ring.value().begin(), box_ring.value().end()).size() == box_ring.value().size());
    const auto box_fan = box_topology.value().vertex_fan(
        box.value().vertices_sorted().front().id);
    REQUIRE(box_fan);
    REQUIRE(box_fan.value().size() == 3U);
    const auto box_region = box_topology.value().face_region(
        box.value().faces_sorted().front().id);
    REQUIRE(box_region);
    REQUIRE(box_region.value().size() == 6U);
    const auto box_component = box_topology.value().linked_component(
        box.value().vertices_sorted().front().id);
    REQUIRE(box_component);
    REQUIRE(box_component.value().size() == 8U);
    const auto box_path = box_topology.value().shortest_path(
        box.value().vertices_sorted().front().id, box.value().vertices_sorted().back().id);
    REQUIRE(box_path);
    REQUIRE(box_path.value().front() == box.value().vertices_sorted().front().id);
    REQUIRE(box_path.value().back() == box.value().vertices_sorted().back().id);
    const auto closed_boundary = box_topology.value().boundary_loop(
        box_topology.value().edges.front().id);
    REQUIRE(!closed_boundary);
    REQUIRE(closed_boundary.error().code == carto::core::ErrorCode::unsupported);
    const auto missing_edge = box_topology.value().edge_loop(carto::geometry::EdgeId{999999U});
    REQUIRE(!missing_edge);
    REQUIRE(missing_edge.error().code == carto::core::ErrorCode::not_found);

    carto::geometry::EditableMesh triangle;
    const auto a = triangle.add_vertex({0.0, 0.0, 0.0});
    const auto b = triangle.add_vertex({1.0, 0.0, 0.0});
    const auto c = triangle.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(triangle.add_face({a.value(), b.value(), c.value()}));
    const auto triangle_topology = triangle.topology();
    REQUIRE(triangle_topology);
    const auto unsupported_loop = triangle_topology.value().edge_loop(
        triangle_topology.value().edges.front().id);
    REQUIRE(!unsupported_loop);
    REQUIRE(unsupported_loop.error().code == carto::core::ErrorCode::unsupported);
    const auto unsupported_ring = triangle_topology.value().edge_ring(
        triangle_topology.value().edges.front().id);
    REQUIRE(!unsupported_ring);
    REQUIRE(unsupported_ring.error().code == carto::core::ErrorCode::unsupported);
}

void invalid_topology_receipts_fail_closed() {
    carto::geometry::TopologyEditReceipt invalid;
    invalid.revision_before = carto::core::Revision{1U};
    invalid.revision_after = carto::core::Revision{2U};
    invalid.created_vertices = {{1U}, {1U}};
    const auto rejected = invalid.validate();
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::validation_failed);

    invalid.created_vertices = {{2U}, {1U}};
    const auto unordered = invalid.validate();
    REQUIRE(!unordered);
    REQUIRE(unordered.error().code == carto::core::ErrorCode::validation_failed);
}

void topology_receipts_round_trip_and_reject_hostile_text() {
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(plane);
    const auto face = plane.value().faces_sorted().front().id;
    const auto receipt = plane.value().extrude_face_with_receipt(face, 0.25);
    REQUIRE(receipt);

    const std::string encoded = receipt.value().serialize();
    const auto decoded = carto::geometry::TopologyEditReceipt::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == encoded);

    const std::string merge_section = "MERGED_VERTICES 0\n";
    const auto legacy_section = encoded.find(merge_section);
    REQUIRE(legacy_section != std::string::npos);
    std::string legacy = encoded;
    legacy.erase(legacy_section, merge_section.size());
    const auto legacy_version = legacy.find("CARTOGRAPHER_TOPOLOGY_RECEIPT 2");
    REQUIRE(legacy_version != std::string::npos);
    legacy.replace(legacy_version, std::string("CARTOGRAPHER_TOPOLOGY_RECEIPT 2").size(),
                   "CARTOGRAPHER_TOPOLOGY_RECEIPT 1");
    const auto legacy_decoded = carto::geometry::TopologyEditReceipt::deserialize(legacy);
    REQUIRE(legacy_decoded);
    REQUIRE(legacy_decoded.value().merged_vertices.empty());

    std::string invalid_merge = encoded;
    invalid_merge.replace(
        legacy_section, merge_section.size(), "MERGED_VERTICES 1\nMERGE 0 1\n");
    const auto invalid_merge_decoded =
        carto::geometry::TopologyEditReceipt::deserialize(invalid_merge);
    REQUIRE(!invalid_merge_decoded);
    REQUIRE(invalid_merge_decoded.error().code == carto::core::ErrorCode::validation_failed);
    REQUIRE(decoded.value().revision_before == receipt.value().revision_before);
    REQUIRE(decoded.value().revision_after == receipt.value().revision_after);

    const auto trailing = carto::geometry::TopologyEditReceipt::deserialize(
        encoded + "TRAILING\n");
    REQUIRE(!trailing);
    REQUIRE(trailing.error().code == carto::core::ErrorCode::validation_failed);

    std::string future = encoded;
    const auto version = future.find("CARTOGRAPHER_TOPOLOGY_RECEIPT 2");
    REQUIRE(version != std::string::npos);
    future.replace(version, std::string("CARTOGRAPHER_TOPOLOGY_RECEIPT 2").size(),
                   "CARTOGRAPHER_TOPOLOGY_RECEIPT 3");
    const auto future_rejected = carto::geometry::TopologyEditReceipt::deserialize(future);
    REQUIRE(!future_rejected);
    REQUIRE(future_rejected.error().code == carto::core::ErrorCode::version_mismatch);

    std::string oversized = encoded;
    const auto created_vertices = oversized.find("CREATED_VERTICES 4");
    REQUIRE(created_vertices != std::string::npos);
    oversized.replace(created_vertices, std::string("CREATED_VERTICES 4").size(),
                      "CREATED_VERTICES 1000001");
    const auto oversized_rejected = carto::geometry::TopologyEditReceipt::deserialize(oversized);
    REQUIRE(!oversized_rejected);
    REQUIRE(oversized_rejected.error().code == carto::core::ErrorCode::validation_failed);

    std::string aggregate_oversized =
        "CARTOGRAPHER_TOPOLOGY_RECEIPT 1\n"
        "REVISION_BEFORE 1\n"
        "REVISION_AFTER 2\n"
        "CREATED_VERTICES 1000000";
    aggregate_oversized.reserve(16U * 1024U * 1024U);
    for (std::uint64_t id = 1U; id <= 1'000'000U; ++id) {
        aggregate_oversized += ' ' + std::to_string(id);
    }
    aggregate_oversized += "\nCREATED_EDGES 1000000";
    for (std::uint64_t id = 1U; id <= 1'000'000U; ++id) {
        aggregate_oversized += ' ' + std::to_string(id);
    }
    aggregate_oversized += "\nCREATED_FACES 1\n";
    const auto aggregate_rejected =
        carto::geometry::TopologyEditReceipt::deserialize(aggregate_oversized);
    REQUIRE(!aggregate_rejected);
    REQUIRE(aggregate_rejected.error().code == carto::core::ErrorCode::validation_failed);
    REQUIRE(aggregate_rejected.error().message.find("element budget") != std::string::npos);
}

void topology_receipts_reject_invalid_merge_lineage() {
    carto::geometry::TopologyEditReceipt receipt;
    receipt.revision_before = carto::core::Revision{1U};
    receipt.revision_after = carto::core::Revision{2U};
    receipt.removed_vertices = {{2U}};

    receipt.merged_vertices.emplace(carto::geometry::VertexId{2U}, carto::geometry::VertexId{2U});
    REQUIRE(!receipt.validate());
    receipt.merged_vertices.clear();
    receipt.merged_vertices.emplace(carto::geometry::VertexId{3U}, carto::geometry::VertexId{1U});
    REQUIRE(!receipt.validate());
    receipt.merged_vertices.clear();
    receipt.removed_vertices.push_back(carto::geometry::VertexId{1U});
    receipt.merged_vertices.emplace(carto::geometry::VertexId{2U}, carto::geometry::VertexId{1U});
    REQUIRE(!receipt.validate());
}

void face_inset_rejects_invalid_or_collapsing_distances_without_mutation() {
    auto box = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(box);
    const auto face = box.value().faces_sorted().front().id;
    const auto before_revision = box.value().revision();
    const auto before_vertices = box.value().vertex_count();
    const auto before_faces = box.value().face_count();

    for (const double distance : {
             0.0,
             std::numeric_limits<double>::quiet_NaN(),
             10.0,
         }) {
        const auto inset = box.value().inset_face(face, distance);
        REQUIRE(!inset);
        REQUIRE(inset.error().code == carto::core::ErrorCode::invalid_argument);
        REQUIRE(box.value().revision() == before_revision);
        REQUIRE(box.value().vertex_count() == before_vertices);
        REQUIRE(box.value().face_count() == before_faces);
        REQUIRE(box.value().find_face(face) != nullptr);
    }
}

void face_inset_rejects_concave_and_nonplanar_faces() {
    carto::geometry::EditableMesh concave;
    const std::vector<carto::core::Vec3d> concave_positions = {
        {0.0, 0.0, 0.0},
        {2.0, 0.0, 0.0},
        {2.0, 2.0, 0.0},
        {1.0, 0.75, 0.0},
        {0.0, 2.0, 0.0},
    };
    std::vector<carto::geometry::VertexId> concave_vertices;
    for (const auto position : concave_positions) {
        const auto added = concave.add_vertex(position);
        REQUIRE(added);
        concave_vertices.push_back(added.value());
    }
    const auto concave_face = concave.add_face(concave_vertices);
    REQUIRE(concave_face);
    const auto concave_revision = concave.revision();
    const auto concave_inset = concave.inset_face(concave_face.value(), 0.1);
    REQUIRE(!concave_inset);
    REQUIRE(concave_inset.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(concave.revision() == concave_revision);

    carto::geometry::EditableMesh nonplanar;
    const std::vector<carto::core::Vec3d> nonplanar_positions = {
        {0.0, 0.0, 0.0},
        {2.0, 0.0, 0.0},
        {2.0, 2.0, 0.2},
        {0.0, 2.0, 0.0},
    };
    std::vector<carto::geometry::VertexId> nonplanar_vertices;
    for (const auto position : nonplanar_positions) {
        const auto added = nonplanar.add_vertex(position);
        REQUIRE(added);
        nonplanar_vertices.push_back(added.value());
    }
    const auto nonplanar_face = nonplanar.add_face(nonplanar_vertices);
    REQUIRE(nonplanar_face);
    const auto nonplanar_revision = nonplanar.revision();
    const auto nonplanar_inset = nonplanar.inset_face(nonplanar_face.value(), 0.1);
    REQUIRE(!nonplanar_inset);
    REQUIRE(nonplanar_inset.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(nonplanar.revision() == nonplanar_revision);
}

void admitted_face_delete_is_undoable_and_clears_removed_selection() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto before_selection = session.snapshot();
    const auto object = before_selection.objects.front().object.id;
    const auto face = before_selection.viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{object, face}));
    const auto before_delete = session.snapshot();
    const auto before_triangles = before_delete.viewport.scene.instances().front().mesh->triangle_faces.size();

    carto::editor::ToolArguments arguments;
    arguments.remove_orphaned_vertices = true;
    const auto deleted = carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::InvokeToolAction{"mesh.remove-face", arguments});
    REQUIRE(deleted);
    REQUIRE(deleted.value().affected.faces.size() == 1U);
    REQUIRE(deleted.value().affected.faces.front() == face);
    REQUIRE(deleted.value().parameters.has_value());
    REQUIRE(deleted.value().parameters->remove_orphaned_vertices.has_value());
    REQUIRE(*deleted.value().parameters->remove_orphaned_vertices);
    const auto after_delete = session.snapshot();
    REQUIRE(after_delete.viewport.scene.instances().front().mesh->triangle_faces.size() + 2U ==
            before_triangles);
    REQUIRE(after_delete.selection.faces.empty());

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() ==
            before_triangles);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() + 2U ==
            before_triangles);
}

void admitted_edge_split_is_undoable_and_clears_removed_selection() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto before_selection = session.snapshot();
    const auto object = before_selection.objects.front().object.id;
    const auto selected_edge = before_selection.viewport.scene.instances().front().mesh->triangle_edges.front()[0];
    REQUIRE(selected_edge.has_value());
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectEdgeAction{object, *selected_edge}));
    const auto before_split = session.snapshot();
    const auto before_vertices = before_split.viewport.scene.instances().front().mesh->vertex_ids.size();

    carto::editor::ToolArguments arguments;
    arguments.factor = 0.5;
    const auto split = carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::InvokeToolAction{"mesh.split-edge", arguments});
    REQUIRE(split);
    REQUIRE(split.value().affected.edges.size() == 1U);
    REQUIRE(split.value().affected.edges.front() == *selected_edge);
    REQUIRE(split.value().parameters.has_value());
    REQUIRE(split.value().parameters->factor.has_value());
    REQUIRE(*split.value().parameters->factor == 0.5);
    const auto after_split = session.snapshot();
    REQUIRE(after_split.viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before_vertices + 1U);
    REQUIRE(after_split.selection.edges.empty());

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before_vertices);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before_vertices + 1U);
}

void admitted_face_inset_is_undoable_and_clears_removed_selection() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}}));
    const auto before_selection = session.snapshot();
    const auto object = before_selection.objects.front().object.id;
    const auto face = before_selection.viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{object, face}));
    const auto before_inset = session.snapshot();
    const auto before_vertices = before_inset.viewport.scene.instances().front().mesh->vertex_ids.size();
    const auto before_triangles = before_inset.viewport.scene.instances().front().mesh->triangle_faces.size();

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto inset = carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::InvokeToolAction{"mesh.inset-face", arguments});
    REQUIRE(inset);
    REQUIRE(inset.value().affected.faces.size() == 1U);
    REQUIRE(inset.value().affected.faces.front() == face);
    REQUIRE(inset.value().parameters.has_value());
    REQUIRE(inset.value().parameters->distance.has_value());
    REQUIRE(*inset.value().parameters->distance == 0.25);
    const auto after_inset = session.snapshot();
    REQUIRE(after_inset.viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before_vertices + 4U);
    REQUIRE(after_inset.viewport.scene.instances().front().mesh->triangle_faces.size() ==
            before_triangles + 8U);
    REQUIRE(after_inset.selection.faces.empty());

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before_vertices);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before_vertices + 4U);
}

void admitted_vertex_merge_is_undoable_and_clears_selection() {
    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    REQUIRE(mesh.add_face({target.value(), source.value(), third.value()}));
    REQUIRE(mesh.add_face({source.value(), fourth.value(), third.value()}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{"Weld", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{object, target.value()}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            object, source.value(), carto::editor::SelectionOperation::add}));

    const auto merged = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.merge-vertices", carto::editor::ToolArguments{}});
    REQUIRE(merged);
    REQUIRE(merged.value().affected.vertices.size() == 2U);
    REQUIRE(session.snapshot().selection.vertices.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 3U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() == 1U);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 3U);
}

void admitted_edge_dissolve_is_undoable_and_clears_selection() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    REQUIRE(mesh.add_face({a.value(), c.value(), d.value()}));
    const auto topology = mesh.topology();
    REQUIRE(topology);
    const auto shared = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [a, c](const carto::geometry::EdgeRecord& edge) {
            return edge.first == a.value() && edge.second == c.value();
        });
    REQUIRE(shared != topology.value().edges.end());
    const auto shared_id = shared->id;

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{"Dissolve", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectEdgeAction{object, shared_id}));

    const auto before = session.snapshot();
    const auto dissolved = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.dissolve-edge", carto::editor::ToolArguments{}});
    REQUIRE(dissolved);
    REQUIRE(dissolved.value().affected.edges.size() == 1U);
    REQUIRE(dissolved.value().affected.edges.front() == shared_id);
    REQUIRE(session.snapshot().selection.edges.empty());
    const auto after = session.snapshot();
    const auto contains_shared_edge = [](const auto& compiled, carto::geometry::EdgeId edge) {
        return std::any_of(compiled.triangle_edges.begin(), compiled.triangle_edges.end(),
            [edge](const auto& triangle) {
                return std::any_of(triangle.begin(), triangle.end(),
                    [edge](const auto& candidate) {
                        return candidate.has_value() && *candidate == edge;
                    });
            });
    };
    REQUIRE(!contains_shared_edge(*after.viewport.scene.instances().front().mesh, shared_id));

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(contains_shared_edge(
        *session.snapshot().viewport.scene.instances().front().mesh, shared_id));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(!contains_shared_edge(
        *session.snapshot().viewport.scene.instances().front().mesh, shared_id));
    REQUIRE(session.snapshot().project_revision > before.project_revision);
}

void admitted_triangle_to_quad_is_undoable_and_clears_selection() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    const auto first = mesh.add_face({a.value(), b.value(), c.value()});
    const auto second = mesh.add_face({a.value(), c.value(), d.value()});
    REQUIRE(first && second);
    const auto topology = mesh.topology();
    REQUIRE(topology);
    const auto shared = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [a, c](const carto::geometry::EdgeRecord& edge) {
            return edge.first == a.value() && edge.second == c.value();
        });
    REQUIRE(shared != topology.value().edges.end());
    const auto shared_id = shared->id;

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{"TriQuad", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{object, first.value()}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{
            object, second.value(), carto::editor::SelectionOperation::add}));

    const auto converted = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.tri-to-quad", carto::editor::ToolArguments{}});
    REQUIRE(converted);
    REQUIRE(converted.value().affected.faces.size() == 2U);
    REQUIRE(session.snapshot().selection.faces.empty());
    const auto has_shared_edge = [](const auto& compiled, carto::geometry::EdgeId edge) {
        return std::any_of(compiled.triangle_edges.begin(), compiled.triangle_edges.end(),
            [edge](const auto& triangle) {
                return std::any_of(triangle.begin(), triangle.end(),
                    [edge](const auto& candidate) {
                        return candidate.has_value() && *candidate == edge;
                    });
            });
    };
    REQUIRE(!has_shared_edge(
        *session.snapshot().viewport.scene.instances().front().mesh, shared_id));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(has_shared_edge(
        *session.snapshot().viewport.scene.instances().front().mesh, shared_id));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(!has_shared_edge(
        *session.snapshot().viewport.scene.instances().front().mesh, shared_id));
}

void admitted_shortest_path_selection_is_non_mutating_and_stale_safe() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreatePlaneAction{"Path", 2.0, 2.0}));
    const auto initial = session.snapshot();
    const auto object = initial.objects.front().object.id;
    const auto vertices = initial.viewport.scene.instances().front().mesh->vertex_ids;
    REQUIRE(vertices.size() == 4U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{object, vertices.front()}));
    const auto before = session.snapshot().project_revision;
    const auto path = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectShortestPathAction{vertices.back()});
    REQUIRE(path);
    REQUIRE(session.snapshot().project_revision == before);
    REQUIRE(session.snapshot().selection.vertices.size() >= 2U);
    REQUIRE(session.snapshot().selection.vertices.front() == vertices.front());
    REQUIRE(session.snapshot().selection.vertices.back() == vertices.back());

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    const auto rejected = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectShortestPathAction{vertices.back()});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before);
}

void admitted_vertex_slide_is_undoable_and_revision_bound() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Slide", {1.0, 1.0, 1.0}}));
    const auto initial = session.snapshot();
    const auto object = initial.objects.front().object.id;
    const auto before_mesh = initial.viewport.scene.instances().front().mesh;
    const auto before_vertex = std::find(
        before_mesh->vertex_ids.begin(), before_mesh->vertex_ids.end(),
        carto::geometry::VertexId{1U});
    REQUIRE(before_vertex != before_mesh->vertex_ids.end());
    const auto before_index = static_cast<std::size_t>(
        before_vertex - before_mesh->vertex_ids.begin());
    const auto before_position = before_mesh->positions[before_index];
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{object, {1U}}));
    carto::editor::ToolArguments arguments;
    arguments.factor = 0.25;
    const auto slided = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.slide-vertex", arguments});
    REQUIRE(slided);
    REQUIRE(slided.value().affected.vertices.size() == 1U);
    REQUIRE(slided.value().affected.vertices.front() == carto::geometry::VertexId{1U});
    REQUIRE(slided.value().parameters.has_value());
    REQUIRE(slided.value().parameters->factor.has_value());
    REQUIRE(*slided.value().parameters->factor == 0.25);
    const auto after_mesh = session.snapshot().viewport.scene.instances().front().mesh;
    const auto after_vertex = std::find(
        after_mesh->vertex_ids.begin(), after_mesh->vertex_ids.end(),
        carto::geometry::VertexId{1U});
    REQUIRE(after_vertex != after_mesh->vertex_ids.end());
    const auto after_index = static_cast<std::size_t>(
        after_vertex - after_mesh->vertex_ids.begin());
    const auto after_position = after_mesh->positions[after_index];
    REQUIRE(after_position.x != before_position.x ||
            after_position.y != before_position.y ||
            after_position.z != before_position.z);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before_mesh->vertex_ids.size());
}

} // namespace

int main() {
    try {
        shared_vertex_position_patch_checks_all_incident_faces();
        face_delete_is_atomic_and_reports_removed_identity();
        face_delete_can_preserve_orphans_without_invalid_topology();
        face_delete_preserves_shared_vertices_when_removing_orphans();
        face_extrude_is_atomic_and_advances_once();
        edge_split_is_atomic_and_reports_topology_delta();
        edge_split_rejects_invalid_factors_without_mutation();
        edge_bevel_is_bounded_atomic_and_reports_lineage();
        edge_bevel_rejects_boundary_large_and_multisegment_inputs_without_mutation();
        edge_bevel_rejects_nonconvex_and_nonplanar_faces_without_mutation();
        vertex_bevel_is_bounded_atomic_and_reports_lineage();
        vertex_bevel_rejects_boundary_shape_and_numeric_inputs_without_mutation();
        coplanar_internal_edge_dissolve_is_atomic_and_reports_lineage();
        edge_dissolve_rejects_boundary_and_nonplanar_joins_without_mutation();
        triangle_to_quad_requires_two_compatible_triangles();
        vertex_slide_is_bounded_and_atomic();
        target_weld_preserves_target_and_reports_merge_lineage();
        target_weld_rejects_ambiguous_collapses_without_mutation();
        component_selection_conversion_is_deterministic_and_non_mutating();
        component_selection_expansion_is_deterministic_and_non_mutating();
        admitted_edge_split_rejects_wrong_mode_and_multi_selection();
        convex_face_inset_is_atomic_and_reports_topology_delta();
        convex_face_poke_is_atomic_and_reports_topology_delta();
        face_poke_rejects_nonconvex_and_nonplanar_faces_without_mutation();
        topology_receipts_preserve_operation_lineage();
        topology_traversals_are_validated_and_deterministic();
        invalid_topology_receipts_fail_closed();
        topology_receipts_round_trip_and_reject_hostile_text();
        topology_receipts_reject_invalid_merge_lineage();
        face_inset_rejects_invalid_or_collapsing_distances_without_mutation();
        face_inset_rejects_concave_and_nonplanar_faces();
        admitted_face_delete_is_undoable_and_clears_removed_selection();
        admitted_edge_split_is_undoable_and_clears_removed_selection();
        admitted_face_inset_is_undoable_and_clears_removed_selection();
        admitted_vertex_merge_is_undoable_and_clears_selection();
        admitted_edge_dissolve_is_undoable_and_clears_selection();
        admitted_triangle_to_quad_is_undoable_and_clears_selection();
        admitted_shortest_path_selection_is_non_mutating_and_stale_safe();
        admitted_vertex_slide_is_undoable_and_revision_bound();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
