#include <carto/application/application.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/geometry/mesh.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
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
    REQUIRE(decoded.value().revision_before == receipt.value().revision_before);
    REQUIRE(decoded.value().revision_after == receipt.value().revision_after);

    const auto trailing = carto::geometry::TopologyEditReceipt::deserialize(
        encoded + "TRAILING\n");
    REQUIRE(!trailing);
    REQUIRE(trailing.error().code == carto::core::ErrorCode::validation_failed);

    std::string future = encoded;
    const auto version = future.find("CARTOGRAPHER_TOPOLOGY_RECEIPT 1");
    REQUIRE(version != std::string::npos);
    future.replace(version, std::string("CARTOGRAPHER_TOPOLOGY_RECEIPT 1").size(),
                   "CARTOGRAPHER_TOPOLOGY_RECEIPT 2");
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
        admitted_edge_split_rejects_wrong_mode_and_multi_selection();
        convex_face_inset_is_atomic_and_reports_topology_delta();
        topology_receipts_preserve_operation_lineage();
        topology_traversals_are_validated_and_deterministic();
        invalid_topology_receipts_fail_closed();
        topology_receipts_round_trip_and_reject_hostile_text();
        face_inset_rejects_invalid_or_collapsing_distances_without_mutation();
        face_inset_rejects_concave_and_nonplanar_faces();
        admitted_face_delete_is_undoable_and_clears_removed_selection();
        admitted_edge_split_is_undoable_and_clears_removed_selection();
        admitted_face_inset_is_undoable_and_clears_removed_selection();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
