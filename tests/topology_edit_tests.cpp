#include <carto/application/application.hpp>
#include <carto/attributes/transfer.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/mesh_attributes/provenance.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
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

carto::core::Result<carto::geometry::EditableMesh> make_closed_triangular_fan(
    std::size_t side_count,
    carto::core::Vec3d apex_position = {0.0, 0.0, 1.0}) {
    using namespace carto;
    if (side_count < 3U) {
        return core::Result<geometry::EditableMesh>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "closed triangular fan fixture needs at least three sides"));
    }
    geometry::EditableMesh mesh;
    const auto apex = mesh.add_vertex(apex_position);
    if (!apex) return core::Result<geometry::EditableMesh>::failure(apex.error());
    std::vector<geometry::VertexId> ring;
    ring.reserve(side_count);
    constexpr double kPi = 3.14159265358979323846;
    for (std::size_t index = 0U; index < side_count; ++index) {
        const double angle = 2.0 * kPi * static_cast<double>(index) /
            static_cast<double>(side_count);
        const auto vertex = mesh.add_vertex({std::cos(angle), std::sin(angle), 0.0});
        if (!vertex) return core::Result<geometry::EditableMesh>::failure(vertex.error());
        ring.push_back(vertex.value());
    }
    for (std::size_t index = 0U; index < ring.size(); ++index) {
        const auto face = mesh.add_face({
            apex.value(), ring[index], ring[(index + 1U) % ring.size()]});
        if (!face) return core::Result<geometry::EditableMesh>::failure(face.error());
    }
    std::vector<geometry::VertexId> base;
    base.reserve(ring.size());
    base.push_back(ring.front());
    for (std::size_t index = ring.size(); index > 1U; --index) {
        base.push_back(ring[index - 1U]);
    }
    const auto base_face = mesh.add_face(std::move(base));
    if (!base_face) return core::Result<geometry::EditableMesh>::failure(base_face.error());
    if (auto result = mesh.validate(); !result) {
        return core::Result<geometry::EditableMesh>::failure(result.error());
    }
    return core::Result<geometry::EditableMesh>::success(std::move(mesh));
}

void mesh_patch_serialization_is_canonical_and_bounded() {
    auto mesh = make_tetrahedron();
    REQUIRE(mesh);
    const auto vertex = mesh.value().vertices_sorted().front();
    const carto::geometry::MeshPatch patch{
        mesh.value().revision(),
        {{vertex.id, vertex.position,
          {vertex.position.x + 0.05, vertex.position.y, vertex.position.z}}},
    };
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::MeshPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().vertex_changes.size() == 1U);
    REQUIRE(decoded.value().vertex_changes.front().id == vertex.id);
    REQUIRE(decoded.value().vertex_changes.front().before.x == vertex.position.x);
    REQUIRE(decoded.value().vertex_changes.front().after.x == vertex.position.x + 0.05);

    REQUIRE(!carto::geometry::MeshPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::MeshPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::MeshPatch::deserialize(std::string(
        carto::geometry::MeshPatch::kMaxSerializedBytes + 1U, 'x')));
}

void face_deletion_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::FaceDeletionPatch patch{
        carto::core::Revision{7U}, carto::geometry::FaceId{12U}, false};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::FaceDeletionPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().face == patch.face);
    REQUIRE(!decoded.value().remove_orphaned_vertices);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::FaceDeletionPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::FaceDeletionPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::FaceDeletionPatch::deserialize(
        "CARTOGRAPHER_FACE_DELETION_PATCH 1\nEXPECTED_REVISION 7\n"
        "FACE 12\nREMOVE_ORPHANS 2\n"));
    REQUIRE(!carto::geometry::FaceDeletionPatch::deserialize(std::string(
        carto::geometry::FaceDeletionPatch::kMaxSerializedBytes + 1U, 'x')));
    REQUIRE((!carto::geometry::FaceDeletionPatch{
        carto::core::Revision{7U}, carto::geometry::FaceId{}, true}.validate()));
}

void edge_split_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::EdgeSplitPatch patch{
        carto::core::Revision{7U}, carto::geometry::EdgeId{12U}, 0.375};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::EdgeSplitPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().edge == patch.edge);
    REQUIRE(decoded.value().factor == patch.factor);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::EdgeSplitPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::EdgeSplitPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::EdgeSplitPatch::deserialize(
        "CARTOGRAPHER_EDGE_SPLIT_PATCH 1\nEXPECTED_REVISION 7\n"
        "EDGE 12\nFACTOR 1\n"));
    REQUIRE(!carto::geometry::EdgeSplitPatch::deserialize(std::string(
        carto::geometry::EdgeSplitPatch::kMaxSerializedBytes + 1U, 'x')));
    REQUIRE((!carto::geometry::EdgeSplitPatch{
        carto::core::Revision{7U}, carto::geometry::EdgeId{}, 0.5}.validate()));
    REQUIRE((!carto::geometry::EdgeSplitPatch{
        carto::core::Revision{7U}, carto::geometry::EdgeId{12U},
        std::numeric_limits<double>::quiet_NaN()}.validate()));
}

void edge_dissolve_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::EdgeDissolvePatch patch{
        carto::core::Revision{9U},
        {carto::geometry::EdgeId{17U}, carto::geometry::EdgeId{4U}}};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    REQUIRE(encoded.find("EDGES 2 4 17\n") != std::string::npos);
    const auto decoded = carto::geometry::EdgeDissolvePatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().edges.size() == 2U);
    REQUIRE(decoded.value().edges[0] == carto::geometry::EdgeId{4U});
    REQUIRE(decoded.value().edges[1] == carto::geometry::EdgeId{17U});
    REQUIRE(decoded.value().serialize() == encoded);
    REQUIRE((carto::geometry::EdgeDissolvePatch{
        patch.expected_revision,
        {patch.edges[1], patch.edges[0]}}.serialize() == encoded));

    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(future_schema));
    std::string noncanonical_order = encoded;
    const auto canonical_ids = noncanonical_order.find("EDGES 2 4 17");
    REQUIRE(canonical_ids != std::string::npos);
    noncanonical_order.replace(canonical_ids, std::string("EDGES 2 4 17").size(),
        "EDGES 2 17 4");
    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(noncanonical_order));
    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(
        "CARTOGRAPHER_EDGE_DISSOLVE_PATCH 1\nEXPECTED_REVISION 9\nEDGES 2 4\n"));
    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(
        "CARTOGRAPHER_EDGE_DISSOLVE_PATCH 1\nEXPECTED_REVISION 9\nEDGES 2 4 4\n"));
    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(
        "CARTOGRAPHER_EDGE_DISSOLVE_PATCH 1\nEXPECTED_REVISION 9\nEDGES 1 0\n"));
    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(
        "CARTOGRAPHER_EDGE_DISSOLVE_PATCH 1\nEXPECTED_REVISION 9\nEDGES 1000001"));
    REQUIRE(!carto::geometry::EdgeDissolvePatch::deserialize(std::string(
        carto::geometry::EdgeDissolvePatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::EdgeDissolvePatch{
        carto::core::Revision{9U}, {}}.validate()));
    REQUIRE((!carto::geometry::EdgeDissolvePatch{
        carto::core::Revision{9U},
        {carto::geometry::EdgeId{4U}, carto::geometry::EdgeId{4U}}}.validate()));
    REQUIRE((!carto::geometry::EdgeDissolvePatch{
        carto::core::Revision{9U}, {carto::geometry::EdgeId{}}}.validate()));
    const carto::geometry::EdgeDissolvePatch exhausted_revision{
        carto::core::Revision{carto::core::Revision::max_value()},
        {carto::geometry::EdgeId{4U}}};
    REQUIRE(!exhausted_revision.validate());
    REQUIRE(exhausted_revision.serialize().empty());
}

void merge_by_distance_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::MergeByDistancePatch patch{
        carto::core::Revision{11U},
        {carto::geometry::VertexId{9U}, carto::geometry::VertexId{2U},
            carto::geometry::VertexId{5U}},
        0.125};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    REQUIRE(encoded.find("CANDIDATES 3 2 5 9\n") != std::string::npos);
    const auto decoded = carto::geometry::MergeByDistancePatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().distance == patch.distance);
    REQUIRE(decoded.value().candidates.size() == 3U);
    REQUIRE(decoded.value().candidates[0] == carto::geometry::VertexId{2U});
    REQUIRE(decoded.value().candidates[1] == carto::geometry::VertexId{5U});
    REQUIRE(decoded.value().candidates[2] == carto::geometry::VertexId{9U});
    REQUIRE(decoded.value().serialize() == encoded);
    REQUIRE((carto::geometry::MergeByDistancePatch{
        patch.expected_revision,
        {patch.candidates[2], patch.candidates[0], patch.candidates[1]},
        patch.distance}.serialize() == encoded));

    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(future_schema));
    std::string noncanonical_order = encoded;
    const auto canonical_ids = noncanonical_order.find("CANDIDATES 3 2 5 9");
    REQUIRE(canonical_ids != std::string::npos);
    noncanonical_order.replace(canonical_ids,
        std::string("CANDIDATES 3 2 5 9").size(), "CANDIDATES 3 9 2 5");
    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(noncanonical_order));
    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(
        "CARTOGRAPHER_MERGE_BY_DISTANCE_PATCH 1\nEXPECTED_REVISION 11\n"
        "DISTANCE 0.125\nCANDIDATES 3 2 5\n"));
    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(
        "CARTOGRAPHER_MERGE_BY_DISTANCE_PATCH 1\nEXPECTED_REVISION 11\n"
        "DISTANCE 0.125\nCANDIDATES 2 2 2\n"));
    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(
        "CARTOGRAPHER_MERGE_BY_DISTANCE_PATCH 1\nEXPECTED_REVISION 11\n"
        "DISTANCE 0.125\nCANDIDATES 2 0 2\n"));
    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(
        "CARTOGRAPHER_MERGE_BY_DISTANCE_PATCH 1\nEXPECTED_REVISION 11\n"
        "DISTANCE 0.125\nCANDIDATES 1000001"));
    REQUIRE(!carto::geometry::MergeByDistancePatch::deserialize(std::string(
        carto::geometry::MergeByDistancePatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::MergeByDistancePatch{
        carto::core::Revision{11U}, {}, 0.125}.validate()));
    REQUIRE((!carto::geometry::MergeByDistancePatch{
        carto::core::Revision{11U},
        {carto::geometry::VertexId{2U}, carto::geometry::VertexId{2U}}, 0.125}.validate()));
    REQUIRE((!carto::geometry::MergeByDistancePatch{
        carto::core::Revision{11U},
        {carto::geometry::VertexId{2U}, carto::geometry::VertexId{5U}}, 0.0}.validate()));
    REQUIRE((!carto::geometry::MergeByDistancePatch{
        carto::core::Revision{11U},
        {carto::geometry::VertexId{2U}, carto::geometry::VertexId{5U}},
        std::numeric_limits<double>::infinity()}.validate()));
    const carto::geometry::MergeByDistancePatch exhausted_revision{
        carto::core::Revision{carto::core::Revision::max_value()},
        {carto::geometry::VertexId{2U}, carto::geometry::VertexId{5U}}, 0.125};
    REQUIRE(!exhausted_revision.validate());
    REQUIRE(exhausted_revision.serialize().empty());
}

void vertex_merge_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::VertexMergePatch patch{
        carto::core::Revision{13U},
        carto::geometry::VertexId{7U},
        carto::geometry::VertexId{3U},
        carto::geometry::VertexMergeMode::center};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::VertexMergePatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().target == patch.target);
    REQUIRE(decoded.value().source == patch.source);
    REQUIRE(decoded.value().mode == patch.mode);
    REQUIRE(decoded.value().serialize() == encoded);
    REQUIRE(!carto::geometry::VertexMergePatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::VertexMergePatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::VertexMergePatch::deserialize(
        "CARTOGRAPHER_VERTEX_MERGE_PATCH 1\nEXPECTED_REVISION 13\n"
        "TARGET 7\nSOURCE 3\nMODE LAST_SELECTED\n"));
    REQUIRE(!carto::geometry::VertexMergePatch::deserialize(std::string(
        carto::geometry::VertexMergePatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::VertexMergePatch{
        carto::core::Revision{13U}, carto::geometry::VertexId{},
        carto::geometry::VertexId{3U}, carto::geometry::VertexMergeMode::target}.validate()));
    REQUIRE((!carto::geometry::VertexMergePatch{
        carto::core::Revision{13U}, carto::geometry::VertexId{7U},
        carto::geometry::VertexId{7U}, carto::geometry::VertexMergeMode::target}.validate()));
    REQUIRE((!carto::geometry::VertexMergePatch{
        carto::core::Revision{13U}, carto::geometry::VertexId{7U},
        carto::geometry::VertexId{3U},
        carto::geometry::VertexMergeMode::last_selected}.validate()));
    const carto::geometry::VertexMergePatch exhausted_revision{
        carto::core::Revision{carto::core::Revision::max_value()},
        carto::geometry::VertexId{7U}, carto::geometry::VertexId{3U},
        carto::geometry::VertexMergeMode::target};
    REQUIRE(!exhausted_revision.validate());
    REQUIRE(exhausted_revision.serialize().empty());
}

void loop_cut_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::LoopCutPatch patch{
        carto::core::Revision{17U}, carto::geometry::EdgeId{42U}};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::LoopCutPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().edge == patch.edge);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::LoopCutPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::LoopCutPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::LoopCutPatch::deserialize(
        "CARTOGRAPHER_LOOP_CUT_PATCH 1\nEXPECTED_REVISION 17\nEDGE 0\n"));
    REQUIRE(!carto::geometry::LoopCutPatch::deserialize(
        "CARTOGRAPHER_LOOP_CUT_PATCH 1\nEXPECTED_REVISION 17\n"));
    REQUIRE(!carto::geometry::LoopCutPatch::deserialize(std::string(
        carto::geometry::LoopCutPatch::kMaxSerializedBytes + 1U, 'x')));
    REQUIRE((!carto::geometry::LoopCutPatch{
        carto::core::Revision{17U}, carto::geometry::EdgeId{}}.validate()));
}

void knife_cut_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::KnifeCutPatch patch{
        carto::core::Revision{19U}, carto::geometry::FaceId{7U},
        {{0.0, 0.0, 0.0}, {0.75, 1.0, 0.0}, {2.0, 2.0, 0.0}}};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    REQUIRE(encoded.find("CARTOGRAPHER_KNIFE_CUT_PATCH 1\n") == 0U);
    REQUIRE(encoded.find("FACE 7\nPOINTS 3 ") != std::string::npos);
    const auto decoded = carto::geometry::KnifeCutPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().face == patch.face);
    REQUIRE(decoded.value().path.size() == patch.path.size());
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::KnifeCutPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::KnifeCutPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::KnifeCutPatch::deserialize(
        "CARTOGRAPHER_KNIFE_CUT_PATCH 1\nEXPECTED_REVISION 19\n"
        "FACE 7\nPOINTS 2 0 0 0\n"));
    REQUIRE(!carto::geometry::KnifeCutPatch::deserialize(
        "CARTOGRAPHER_KNIFE_CUT_PATCH 1\nEXPECTED_REVISION 19\n"
        "FACE 7\nPOINTS 4097\n"));
    REQUIRE(!carto::geometry::KnifeCutPatch::deserialize(std::string(
        carto::geometry::KnifeCutPatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::KnifeCutPatch{
        carto::core::Revision{19U}, carto::geometry::FaceId{}, patch.path}.validate()));
    REQUIRE((!carto::geometry::KnifeCutPatch{
        carto::core::Revision{19U}, patch.face, {{0.0, 0.0, 0.0}}}.validate()));
    REQUIRE((!carto::geometry::KnifeCutPatch{
        carto::core::Revision{19U}, patch.face,
        {{0.0, 0.0, 0.0}, {std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0}}}.validate()));
    std::vector<carto::core::Vec3d> oversized_path(
        carto::geometry::KnifeCutPatch::kMaxPathPoints + 1U);
    REQUIRE((!carto::geometry::KnifeCutPatch{
        carto::core::Revision{19U}, patch.face, std::move(oversized_path)}.validate()));
    const carto::geometry::KnifeCutPatch exhausted_revision{
        carto::core::Revision{carto::core::Revision::max_value()},
        patch.face, patch.path};
    REQUIRE(!exhausted_revision.validate());
    REQUIRE(exhausted_revision.serialize().empty());
}

void bevel_edge_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::BevelEdgePatch patch{
        carto::core::Revision{23U}, carto::geometry::EdgeId{57U}, 0.125, 3U};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::BevelEdgePatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().edge == patch.edge);
    REQUIRE(decoded.value().width == patch.width);
    REQUIRE(decoded.value().segments == patch.segments);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::BevelEdgePatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::BevelEdgePatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::BevelEdgePatch::deserialize(
        "CARTOGRAPHER_EDGE_BEVEL_PATCH 1\nEXPECTED_REVISION 23\nEDGE 57\n"
        "WIDTH 0.125\n"));
    REQUIRE(!carto::geometry::BevelEdgePatch::deserialize(
        "CARTOGRAPHER_EDGE_BEVEL_PATCH 1\nEXPECTED_REVISION 23\nEDGE 0\n"
        "WIDTH 0.125\nSEGMENTS 3\n"));
    REQUIRE(!carto::geometry::BevelEdgePatch::deserialize(std::string(
        carto::geometry::BevelEdgePatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::BevelEdgePatch{
        carto::core::Revision{23U}, carto::geometry::EdgeId{}, 0.125, 3U}.validate()));
    for (const double width : {
             0.0,
             -0.1,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         }) {
        REQUIRE((!carto::geometry::BevelEdgePatch{
            carto::core::Revision{23U}, carto::geometry::EdgeId{57U}, width, 3U}.validate()));
    }
    for (const std::uint32_t segments : {0U, 65U}) {
        REQUIRE((!carto::geometry::BevelEdgePatch{
            carto::core::Revision{23U}, carto::geometry::EdgeId{57U}, 0.125,
            segments}.validate()));
    }
}

void bevel_vertex_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::BevelVertexPatch patch{
        carto::core::Revision{31U}, carto::geometry::VertexId{73U}, 0.25, 3U};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::BevelVertexPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().vertex == patch.vertex);
    REQUIRE(decoded.value().width == patch.width);
    REQUIRE(decoded.value().segments == patch.segments);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::BevelVertexPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::BevelVertexPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::BevelVertexPatch::deserialize(
        "CARTOGRAPHER_VERTEX_BEVEL_PATCH 1\nEXPECTED_REVISION 31\nVERTEX 73\n"
        "WIDTH 0.25\n"));
    REQUIRE(!carto::geometry::BevelVertexPatch::deserialize(
        "CARTOGRAPHER_VERTEX_BEVEL_PATCH 1\nEXPECTED_REVISION 31\nVERTEX 0\n"
        "WIDTH 0.25\nSEGMENTS 3\n"));
    REQUIRE(!carto::geometry::BevelVertexPatch::deserialize(std::string(
        carto::geometry::BevelVertexPatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::BevelVertexPatch{
        carto::core::Revision{31U}, carto::geometry::VertexId{}, 0.25, 3U}.validate()));
    for (const double width : {
             0.0,
             -0.1,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         }) {
        REQUIRE((!carto::geometry::BevelVertexPatch{
            carto::core::Revision{31U}, carto::geometry::VertexId{73U}, width, 3U}.validate()));
    }
    for (const std::uint32_t segments : {0U, 65U}) {
        REQUIRE((!carto::geometry::BevelVertexPatch{
            carto::core::Revision{31U}, carto::geometry::VertexId{73U}, 0.25,
            segments}.validate()));
    }
}

void face_extrusion_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::FaceExtrusionPatch patch{
        carto::core::Revision{37U}, carto::geometry::FaceId{81U}, 0.375};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::FaceExtrusionPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().face == patch.face);
    REQUIRE(decoded.value().distance == patch.distance);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::FaceExtrusionPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::FaceExtrusionPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::FaceExtrusionPatch::deserialize(
        "CARTOGRAPHER_FACE_EXTRUSION_PATCH 1\nEXPECTED_REVISION 37\nFACE 81\n"));
    REQUIRE(!carto::geometry::FaceExtrusionPatch::deserialize(
        "CARTOGRAPHER_FACE_EXTRUSION_PATCH 1\nEXPECTED_REVISION 37\nFACE 0\n"
        "DISTANCE 0.375\n"));
    REQUIRE(!carto::geometry::FaceExtrusionPatch::deserialize(std::string(
        carto::geometry::FaceExtrusionPatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::FaceExtrusionPatch{
        carto::core::Revision{37U}, carto::geometry::FaceId{}, 0.375}.validate()));
    for (const double distance : {
             0.0,
             -0.1,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         }) {
        REQUIRE((!carto::geometry::FaceExtrusionPatch{
            carto::core::Revision{37U}, carto::geometry::FaceId{81U}, distance}.validate()));
    }
}

void face_inset_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::FaceInsetPatch patch{
        carto::core::Revision{41U}, carto::geometry::FaceId{93U}, 0.125};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::FaceInsetPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().face == patch.face);
    REQUIRE(decoded.value().distance == patch.distance);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::FaceInsetPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::FaceInsetPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::FaceInsetPatch::deserialize(
        "CARTOGRAPHER_FACE_INSET_PATCH 1\nEXPECTED_REVISION 41\nFACE 93\n"));
    REQUIRE(!carto::geometry::FaceInsetPatch::deserialize(
        "CARTOGRAPHER_FACE_INSET_PATCH 1\nEXPECTED_REVISION 41\nFACE 0\n"
        "DISTANCE 0.125\n"));
    REQUIRE(!carto::geometry::FaceInsetPatch::deserialize(std::string(
        carto::geometry::FaceInsetPatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::FaceInsetPatch{
        carto::core::Revision{41U}, carto::geometry::FaceId{}, 0.125}.validate()));
    for (const double distance : {
             0.0,
             -0.1,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         }) {
        REQUIRE((!carto::geometry::FaceInsetPatch{
            carto::core::Revision{41U}, carto::geometry::FaceId{93U}, distance}.validate()));
    }
}

void face_poke_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::FacePokePatch patch{
        carto::core::Revision{47U}, carto::geometry::FaceId{101U}};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    const auto decoded = carto::geometry::FacePokePatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().face == patch.face);
    REQUIRE(decoded.value().serialize() == encoded);

    REQUIRE(!carto::geometry::FacePokePatch::deserialize(encoded + "TRAILING\n"));
    REQUIRE(!carto::geometry::FacePokePatch::deserialize(
        encoded.substr(0U, encoded.size() - 1U)));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::FacePokePatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::FacePokePatch::deserialize(
        "CARTOGRAPHER_FACE_POKE_PATCH 1\nEXPECTED_REVISION 47\n"));
    REQUIRE(!carto::geometry::FacePokePatch::deserialize(
        "CARTOGRAPHER_FACE_POKE_PATCH 1\nEXPECTED_REVISION 47\nFACE 0\n"));
    REQUIRE(!carto::geometry::FacePokePatch::deserialize(std::string(
        carto::geometry::FacePokePatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::FacePokePatch{
        carto::core::Revision{47U}, carto::geometry::FaceId{}}.validate()));
    const carto::geometry::FacePokePatch exhausted_revision{
        carto::core::Revision{carto::core::Revision::max_value()},
        carto::geometry::FaceId{101U}};
    REQUIRE(!exhausted_revision.validate());
    REQUIRE(exhausted_revision.serialize().empty());
}

void tri_to_quad_patch_serialization_is_canonical_and_bounded() {
    const carto::geometry::TriToQuadPatch patch{
        carto::core::Revision{53U},
        carto::geometry::FaceId{109U},
        carto::geometry::FaceId{103U}};
    const std::string encoded = patch.serialize();
    REQUIRE(!encoded.empty());
    REQUIRE(encoded.find("FACES 103 109\n") != std::string::npos);
    const auto decoded = carto::geometry::TriToQuadPatch::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().expected_revision == patch.expected_revision);
    REQUIRE(decoded.value().first_face == carto::geometry::FaceId{103U});
    REQUIRE(decoded.value().second_face == carto::geometry::FaceId{109U});
    REQUIRE(decoded.value().serialize() == encoded);
    REQUIRE((carto::geometry::TriToQuadPatch{
        patch.expected_revision, patch.second_face, patch.first_face}.serialize() == encoded));

    REQUIRE(!carto::geometry::TriToQuadPatch::deserialize(encoded + "TRAILING\n"));
    std::string future_schema = encoded;
    const auto schema_version = future_schema.find("PATCH 1\n");
    REQUIRE(schema_version != std::string::npos);
    future_schema.replace(schema_version, std::string("PATCH 1\n").size(), "PATCH 2\n");
    REQUIRE(!carto::geometry::TriToQuadPatch::deserialize(future_schema));
    REQUIRE(!carto::geometry::TriToQuadPatch::deserialize(
        "CARTOGRAPHER_TRI_TO_QUAD_PATCH 1\nEXPECTED_REVISION 53\nFACES 103 103\n"));
    REQUIRE(!carto::geometry::TriToQuadPatch::deserialize(
        "CARTOGRAPHER_TRI_TO_QUAD_PATCH 1\nEXPECTED_REVISION 53\nFACES 0 109\n"));
    REQUIRE(!carto::geometry::TriToQuadPatch::deserialize(std::string(
        carto::geometry::TriToQuadPatch::kMaxSerializedBytes + 1U, 'x')));

    REQUIRE((!carto::geometry::TriToQuadPatch{
        carto::core::Revision{53U}, carto::geometry::FaceId{},
        carto::geometry::FaceId{109U}}.validate()));
    REQUIRE((!carto::geometry::TriToQuadPatch{
        carto::core::Revision{53U}, carto::geometry::FaceId{109U},
        carto::geometry::FaceId{109U}}.validate()));
    const carto::geometry::TriToQuadPatch exhausted_revision{
        carto::core::Revision{carto::core::Revision::max_value()},
        carto::geometry::FaceId{103U}, carto::geometry::FaceId{109U}};
    REQUIRE(!exhausted_revision.validate());
    REQUIRE(exhausted_revision.serialize().empty());
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

void edge_split_interpolates_corner_origins_without_flattening_uv_seams() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {-1.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{4U}, {-1.0, 1.0, 0.0}},
         {geometry::VertexId{5U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{6U}, {1.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{5U}, geometry::VertexId{4U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{2U}, geometry::VertexId{3U},
             geometry::VertexId{6U}, geometry::VertexId{5U}}}}));
    const auto source_topology = mesh.topology();
    REQUIRE(source_topology);
    const auto shared_edge = std::find_if(
        source_topology.value().edges.begin(), source_topology.value().edges.end(),
        [](const geometry::EdgeRecord& edge) {
            return edge.second_half_edge.has_value() &&
                edge.first == geometry::VertexId{2U} && edge.second == geometry::VertexId{5U};
        });
    REQUIRE(shared_edge != source_topology.value().edges.end());

    std::vector<geometry::CornerRecord> source_corners = source_topology.value().corners;
    std::sort(source_corners.begin(), source_corners.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    std::vector<attributes::AttributeValue> uv_values;
    std::map<geometry::CornerId, core::Vec2d> uv_by_corner;
    uv_values.reserve(source_corners.size());
    for (const auto& corner : source_corners) {
        const double face_seam = static_cast<double>(corner.face.value) * 10.0;
        const core::Vec2d uv{face_seam + static_cast<double>(corner.vertex.value),
            face_seam - static_cast<double>(corner.vertex.value)};
        uv_by_corner.emplace(corner.id, uv);
        uv_values.emplace_back(uv);
    }
    attributes::AttributeSet source_attributes({
        {attributes::AttributeDomain::vertex, mesh.vertex_count()},
        {attributes::AttributeDomain::edge, source_topology.value().edges.size()},
        {attributes::AttributeDomain::face, mesh.face_count()},
        {attributes::AttributeDomain::corner, source_corners.size()}});
    REQUIRE(source_attributes.add_layer(attributes::AttributeLayer{
        {"uv0", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        std::move(uv_values)}));
    REQUIRE(mesh.set_attribute_payload(std::move(source_attributes), attributes::UvSetTable{}));

    const geometry::EditableMesh source_with_attributes = mesh;
    geometry::EditableMesh geometry_source = mesh;
    REQUIRE(geometry_source.clear_attribute_payload());
    const geometry::EditableMesh source_without_attributes = geometry_source;
    const auto split = geometry_source.split_edge(shared_edge->id, 0.5);
    REQUIRE(split);
    REQUIRE(split.value().corner_origins.size() == split.value().created_corners.size());
    const auto provenance = mesh_attributes::provenance_from_receipt(
        source_without_attributes, geometry_source, split.value());
    REQUIRE(provenance);
    attributes::AttributeTransferPolicy policy;
    policy.per_layer.emplace("uv0", attributes::TransferMode::interpolate);
    auto transferred = attributes::transfer(
        source_with_attributes.attributes(), provenance.value(), policy);
    REQUIRE(transferred);
    REQUIRE(geometry_source.set_attribute_payload(
        std::move(transferred.value()), source_with_attributes.uv_sets()));

    auto compound_receipt = split.value();
    compound_receipt.revision_before = source_with_attributes.revision();
    compound_receipt.revision_after = geometry_source.revision();
    REQUIRE(compound_receipt.validate());
    REQUIRE(mesh_attributes::provenance_from_receipt(
        source_with_attributes, geometry_source, compound_receipt));

    const auto destination_topology = geometry_source.topology();
    REQUIRE(destination_topology);
    std::vector<geometry::CornerRecord> destination_corners = destination_topology.value().corners;
    std::sort(destination_corners.begin(), destination_corners.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    const auto* transferred_uv = geometry_source.attributes().find("uv0");
    REQUIRE(transferred_uv != nullptr);
    std::map<geometry::FaceId, core::Vec2d> split_uv_by_face;
    for (std::size_t index = 0U; index < destination_corners.size(); ++index) {
        const auto& corner = destination_corners[index];
        if (corner.vertex != split.value().created_vertices.front()) continue;
        const auto first_source = std::find_if(
            source_corners.begin(), source_corners.end(), [&](const auto& candidate) {
                return candidate.face == corner.face && candidate.vertex == shared_edge->first;
            });
        const auto second_source = std::find_if(
            source_corners.begin(), source_corners.end(), [&](const auto& candidate) {
                return candidate.face == corner.face && candidate.vertex == shared_edge->second;
            });
        REQUIRE(first_source != source_corners.end());
        REQUIRE(second_source != source_corners.end());
        const auto first_uv = uv_by_corner.at(first_source->id);
        const auto second_uv = uv_by_corner.at(second_source->id);
        const core::Vec2d expected{(first_uv.x + second_uv.x) * 0.5,
            (first_uv.y + second_uv.y) * 0.5};
        const core::Vec2d actual = std::get<core::Vec2d>(transferred_uv->values[index]);
        REQUIRE(actual.x == expected.x);
        REQUIRE(actual.y == expected.y);
        split_uv_by_face.emplace(corner.face, actual);
    }
    REQUIRE(split_uv_by_face.size() == 2U);
    REQUIRE(split_uv_by_face.at(geometry::FaceId{1U}).x !=
        split_uv_by_face.at(geometry::FaceId{2U}).x);
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

void quad_loop_cut_is_atomic_and_reports_full_lineage() {
    carto::geometry::EditableMesh mesh;
    const auto top_left = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto top_middle = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto top_right = mesh.add_vertex({2.0, 1.0, 0.0});
    const auto bottom_left = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto bottom_middle = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto bottom_right = mesh.add_vertex({2.0, 0.0, 0.0});
    REQUIRE(top_left && top_middle && top_right && bottom_left && bottom_middle && bottom_right);
    const auto left_face = mesh.add_face({
        top_left.value(), top_middle.value(), bottom_middle.value(), bottom_left.value()});
    const auto right_face = mesh.add_face({
        top_middle.value(), top_right.value(), bottom_right.value(), bottom_middle.value()});
    REQUIRE(left_face && right_face);
    REQUIRE(mesh.validate());

    const auto before_topology = mesh.topology();
    REQUIRE(before_topology);
    const auto seed = std::find_if(
        before_topology.value().edges.begin(), before_topology.value().edges.end(),
        [top_left, bottom_left](const carto::geometry::EdgeRecord& edge) {
            return edge.first == std::min(top_left.value(), bottom_left.value()) &&
                edge.second == std::max(top_left.value(), bottom_left.value());
        });
    REQUIRE(seed != before_topology.value().edges.end());
    const auto loop = before_topology.value().edge_loop(seed->id);
    REQUIRE(loop);
    REQUIRE(loop.value().size() == 3U);

    const auto before_revision = mesh.revision();
    const auto cut = mesh.loop_cut(seed->id);
    REQUIRE(cut);
    REQUIRE(cut.value().revision_before == before_revision);
    REQUIRE(cut.value().revision_after == before_revision.next());
    REQUIRE(cut.value().created_vertices.size() == 3U);
    REQUIRE(cut.value().created_faces.size() == 2U);
    REQUIRE(cut.value().removed_edges.size() == 3U);
    REQUIRE(cut.value().vertex_origins.size() == 3U);
    REQUIRE(cut.value().face_origins.size() == 2U);
    REQUIRE(cut.value().edge_origins.size() == cut.value().created_edges.size());
    REQUIRE(!cut.value().corner_origins.empty());
    REQUIRE(cut.value().validate());
    REQUIRE(mesh.vertex_count() == 9U);
    REQUIRE(mesh.face_count() == 4U);
    REQUIRE(mesh.find_face(left_face.value()) != nullptr);
    REQUIRE(mesh.find_face(right_face.value()) != nullptr);
    REQUIRE(mesh.find_edge(seed->id) == nullptr);
    REQUIRE(mesh.validate());
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().triangle_faces.size() == 8U);

    carto::geometry::EditableMesh triangle;
    const auto a = triangle.add_vertex({0.0, 0.0, 0.0});
    const auto b = triangle.add_vertex({1.0, 0.0, 0.0});
    const auto c = triangle.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(triangle.add_face({a.value(), b.value(), c.value()}));
    const auto triangle_topology = triangle.topology();
    REQUIRE(triangle_topology);
    const auto triangle_edge = triangle_topology.value().edges.front().id;
    const auto triangle_revision = triangle.revision();
    const auto triangle_vertices = triangle.vertex_count();
    const auto triangle_faces = triangle.face_count();
    const auto refused = triangle.loop_cut(triangle_edge);
    REQUIRE(!refused);
    REQUIRE(refused.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(triangle.revision() == triangle_revision);
    REQUIRE(triangle.vertex_count() == triangle_vertices);
    REQUIRE(triangle.face_count() == triangle_faces);
    REQUIRE(triangle.find_edge(triangle_edge) != nullptr);
    REQUIRE(triangle.validate());
}

void knife_cut_splits_one_face_and_rejects_adversarial_paths_atomically() {
    const auto make_square = [] {
        carto::geometry::EditableMesh mesh;
        const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
        const auto b = mesh.add_vertex({2.0, 0.0, 0.0});
        const auto c = mesh.add_vertex({2.0, 2.0, 0.0});
        const auto d = mesh.add_vertex({0.0, 2.0, 0.0});
        REQUIRE(a && b && c && d);
        const auto face = mesh.add_face({a.value(), b.value(), c.value(), d.value()});
        REQUIRE(face);
        return std::pair{std::move(mesh), face.value()};
    };

    auto [mesh, source_face] = make_square();
    const auto before_revision = mesh.revision();
    const auto split = mesh.knife_cut(source_face, {
        {0.0, 0.0, 0.0}, {0.75, 1.0, 0.0}, {2.0, 2.0, 0.0}});
    REQUIRE(split);
    REQUIRE(split.value().revision_before == before_revision);
    REQUIRE(split.value().revision_after == before_revision.next());
    REQUIRE(split.value().created_vertices.size() == 1U);
    REQUIRE(split.value().created_edges.size() == 2U);
    REQUIRE(split.value().created_faces.size() == 1U);
    REQUIRE(split.value().created_corners.size() >= 3U);
    REQUIRE(split.value().vertex_origins.size() == 1U);
    REQUIRE(split.value().edge_origins.size() == split.value().created_edges.size());
    REQUIRE(split.value().face_origins.size() == 1U);
    REQUIRE(split.value().corner_origins.size() == split.value().created_corners.size());
    REQUIRE(split.value().validate());
    REQUIRE(mesh.face_count() == 2U);
    REQUIRE(mesh.vertex_count() == 5U);
    REQUIRE(mesh.find_face(source_face) != nullptr);
    REQUIRE(mesh.validate());
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().triangle_faces.size() == 4U);
    const auto encoded = split.value().serialize();
    const auto decoded = carto::geometry::TopologyEditReceipt::deserialize(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == encoded);

    auto [straight_mesh, straight_face] = make_square();
    const auto straight = straight_mesh.knife_cut(straight_face, {
        {0.0, 0.0, 0.0}, {0.5, 0.5, 0.0}, {1.0, 1.0, 0.0},
        {1.5, 1.5, 0.0}, {2.0, 2.0, 0.0}});
    if (!straight) std::cerr << "straight knife refused: " << straight.error().message << '\n';
    REQUIRE(straight);
    REQUIRE(straight.value().created_vertices.empty());
    REQUIRE(straight.value().created_edges.size() == 1U);
    REQUIRE(straight_mesh.face_count() == 2U);
    REQUIRE(straight_mesh.validate());

    auto [adversarial, adversarial_face] = make_square();
    const auto before_bad_revision = adversarial.revision();
    const auto before_bad_vertices = adversarial.vertices_sorted();
    const auto before_bad_faces = adversarial.faces_sorted();
    const auto before_bad_topology = adversarial.topology();
    REQUIRE(before_bad_topology);
    const auto expect_atomic_refusal = [&](const std::vector<carto::core::Vec3d>& path) {
        const auto refused = adversarial.knife_cut(adversarial_face, path);
        REQUIRE(!refused);
        REQUIRE(adversarial.revision() == before_bad_revision);
        REQUIRE(adversarial.vertex_count() == before_bad_vertices.size());
        REQUIRE(adversarial.face_count() == before_bad_faces.size());
        const auto after_vertices = adversarial.vertices_sorted();
        REQUIRE(after_vertices.size() == before_bad_vertices.size());
        for (std::size_t index = 0U; index < before_bad_vertices.size(); ++index) {
            REQUIRE(after_vertices[index].id == before_bad_vertices[index].id);
            REQUIRE(after_vertices[index].position.x == before_bad_vertices[index].position.x);
            REQUIRE(after_vertices[index].position.y == before_bad_vertices[index].position.y);
            REQUIRE(after_vertices[index].position.z == before_bad_vertices[index].position.z);
        }
        const auto after_faces = adversarial.faces_sorted();
        REQUIRE(after_faces.size() == before_bad_faces.size());
        for (std::size_t index = 0U; index < before_bad_faces.size(); ++index) {
            REQUIRE(after_faces[index].id == before_bad_faces[index].id);
            REQUIRE(after_faces[index].vertices == before_bad_faces[index].vertices);
        }
        const auto after_topology = adversarial.topology();
        REQUIRE(after_topology);
        REQUIRE(after_topology.value().edges.size() == before_bad_topology.value().edges.size());
        REQUIRE(after_topology.value().corners.size() == before_bad_topology.value().corners.size());
        for (std::size_t index = 0U; index < before_bad_topology.value().edges.size(); ++index) {
            REQUIRE(after_topology.value().edges[index].id ==
                    before_bad_topology.value().edges[index].id);
        }
    };

    expect_atomic_refusal({
        {0.0, 0.0, 0.0}, {1.5, 1.5, 0.0}, {0.5, 1.5, 0.0},
        {1.5, 0.5, 0.0}, {2.0, 2.0, 0.0}});
    expect_atomic_refusal({{0.0, 0.0, 0.0}, {1.0, 1.0, 0.25}, {2.0, 2.0, 0.0}});
    expect_atomic_refusal({{0.0, 0.0, 0.0}, {2.5, 1.0, 0.0}, {2.0, 2.0, 0.0}});
}

void knife_cut_admits_edge_interior_endpoints_and_preserves_shared_boundary() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({2.0, 2.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 2.0, 0.0});
    const auto e = mesh.add_vertex({4.0, 0.0, 0.0});
    const auto f = mesh.add_vertex({4.0, 2.0, 0.0});
    REQUIRE(a && b && c && d && e && f);
    const auto target = mesh.add_face({a.value(), b.value(), c.value(), d.value()});
    const auto adjacent = mesh.add_face({b.value(), e.value(), f.value(), c.value()});
    REQUIRE(target && adjacent);
    const auto before_topology = mesh.topology();
    REQUIRE(before_topology);
    const auto shared_edge = std::find_if(
        before_topology.value().edges.begin(), before_topology.value().edges.end(),
        [&](const auto& edge) {
            return (edge.first == b.value() && edge.second == c.value()) ||
                (edge.first == c.value() && edge.second == b.value());
        });
    REQUIRE(shared_edge != before_topology.value().edges.end());
    REQUIRE(shared_edge->second_half_edge.has_value());

    const auto cut = mesh.knife_cut(target.value(), {
        {2.0, 1.0, 0.0}, {1.0, 1.0, 0.0}, {1.0, 2.0, 0.0}});
    if (!cut) std::cerr << "edge-interior knife refused: " << cut.error().message << '\n';
    REQUIRE(cut);
    REQUIRE(cut.value().created_vertices.size() == 3U);
    REQUIRE(cut.value().created_faces.size() == 1U);
    REQUIRE(cut.value().removed_edges.size() == 2U);
    REQUIRE(cut.value().edge_origins.size() == cut.value().created_edges.size());
    REQUIRE(cut.value().corner_origins.size() == cut.value().created_corners.size());
    REQUIRE(cut.value().validate());
    REQUIRE(mesh.face_count() == 3U);
    REQUIRE(mesh.vertex_count() == 9U);
    REQUIRE(mesh.validate());
    REQUIRE(mesh.compile());

    carto::geometry::VertexId shared_split_vertex;
    carto::geometry::VertexId outer_split_vertex;
    std::size_t interpolated_endpoints = 0U;
    for (const auto vertex_id : cut.value().created_vertices) {
        const auto* vertex = mesh.find_vertex(vertex_id);
        REQUIRE(vertex != nullptr);
        if (std::abs(vertex->position.x - 2.0) < 1e-12 &&
            std::abs(vertex->position.y - 1.0) < 1e-12) {
            shared_split_vertex = vertex_id;
        } else if (std::abs(vertex->position.x - 1.0) < 1e-12 &&
                   std::abs(vertex->position.y - 2.0) < 1e-12) {
            outer_split_vertex = vertex_id;
        }
        const auto origin = cut.value().vertex_origins.find(vertex_id);
        REQUIRE(origin != cut.value().vertex_origins.end());
        if (vertex_id == shared_split_vertex || vertex_id == outer_split_vertex) {
            REQUIRE(origin->second.kind == carto::geometry::OriginKind::interpolated_from);
            REQUIRE(origin->second.source_ids.size() == 2U);
            ++interpolated_endpoints;
        } else {
            REQUIRE(origin->second.kind == carto::geometry::OriginKind::generated_from_face);
        }
    }
    REQUIRE(interpolated_endpoints == 2U);
    REQUIRE(shared_split_vertex);
    REQUIRE(outer_split_vertex);
    const auto* adjacent_after = mesh.find_face(adjacent.value());
    REQUIRE(adjacent_after != nullptr);
    REQUIRE(std::find(adjacent_after->vertices.begin(), adjacent_after->vertices.end(),
        shared_split_vertex) != adjacent_after->vertices.end());

    const auto after_topology = mesh.topology();
    REQUIRE(after_topology);
    std::size_t conforming_shared_segments = 0U;
    for (const auto& edge : after_topology.value().edges) {
        const bool segment =
            (edge.first == b.value() && edge.second == shared_split_vertex) ||
            (edge.second == b.value() && edge.first == shared_split_vertex) ||
            (edge.first == c.value() && edge.second == shared_split_vertex) ||
            (edge.second == c.value() && edge.first == shared_split_vertex);
        if (!segment) continue;
        ++conforming_shared_segments;
        REQUIRE(edge.second_half_edge.has_value());
        const auto origin = cut.value().edge_origins.find(edge.id);
        REQUIRE(origin != cut.value().edge_origins.end());
        REQUIRE(origin->second.kind == carto::geometry::OriginKind::split_from);
        REQUIRE(origin->second.source_ids == std::vector<std::uint64_t>{shared_edge->id.value});
    }
    REQUIRE(conforming_shared_segments == 2U);

    carto::geometry::EditableMesh same_edge_mesh;
    const auto p0 = same_edge_mesh.add_vertex({0.0, 0.0, 0.0});
    const auto p1 = same_edge_mesh.add_vertex({2.0, 0.0, 0.0});
    const auto p2 = same_edge_mesh.add_vertex({2.0, 2.0, 0.0});
    const auto p3 = same_edge_mesh.add_vertex({0.0, 2.0, 0.0});
    REQUIRE(p0 && p1 && p2 && p3);
    const auto same_edge_face = same_edge_mesh.add_face(
        {p0.value(), p1.value(), p2.value(), p3.value()});
    REQUIRE(same_edge_face);
    const auto same_edge_cut = same_edge_mesh.knife_cut(same_edge_face.value(), {
        {0.5, 0.0, 0.0}, {1.0, 0.75, 0.0}, {1.5, 0.0, 0.0}});
    if (!same_edge_cut) std::cerr << "same-edge knife refused: " <<
        same_edge_cut.error().message << '\n';
    REQUIRE(same_edge_cut);
    REQUIRE(same_edge_mesh.validate());
    REQUIRE(same_edge_mesh.compile());

    const auto before_refusal_revision = same_edge_mesh.revision();
    const auto before_refusal_vertices = same_edge_mesh.vertex_count();
    const auto before_refusal_faces = same_edge_mesh.face_count();
    REQUIRE(!same_edge_mesh.knife_cut(same_edge_face.value(), {
        {0.5, 0.0, 0.0}, {1.0, 1.0, 0.0}, {2.5, 1.0, 0.0}}));
    REQUIRE(!same_edge_mesh.knife_cut(same_edge_face.value(), {
        {0.5, 0.0, 0.0}, {1.0, 1.0, 0.0}, {0.5, 0.0, 0.0}}));
    REQUIRE(same_edge_mesh.revision() == before_refusal_revision);
    REQUIRE(same_edge_mesh.vertex_count() == before_refusal_vertices);
    REQUIRE(same_edge_mesh.face_count() == before_refusal_faces);
}

void knife_cut_attribute_transfer_uses_edge_parameters_and_preserves_uv_seams() {
    using namespace carto;
    geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({2.0, 2.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 2.0, 0.0});
    REQUIRE(a && b && c && d);
    const auto face = mesh.add_face({a.value(), b.value(), c.value(), d.value()});
    REQUIRE(face);
    const auto source_topology = mesh.topology();
    REQUIRE(source_topology);
    const auto source_vertices = mesh.vertices_sorted();
    const auto source_edges = mesh.edges_sorted(32U);
    REQUIRE(source_edges.size() == source_topology.value().edges.size());
    std::vector<attributes::AttributeValue> vertex_values;
    for (const auto& vertex : source_vertices) {
        vertex_values.emplace_back(vertex.position.x + 10.0 * vertex.position.y);
    }
    std::vector<attributes::AttributeValue> edge_values;
    for (const auto& edge : source_edges) {
        edge_values.emplace_back(static_cast<double>(edge.id.value * 10U));
    }
    std::vector<geometry::CornerRecord> source_corners = source_topology.value().corners;
    std::sort(source_corners.begin(), source_corners.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::vector<attributes::AttributeValue> corner_uvs;
    for (const auto& corner : source_corners) {
        const auto* vertex = mesh.find_vertex(corner.vertex);
        REQUIRE(vertex != nullptr);
        corner_uvs.emplace_back(core::Vec2d{vertex->position.x, vertex->position.y});
    }
    attributes::AttributeSet source_attributes({
        {attributes::AttributeDomain::vertex, source_vertices.size()},
        {attributes::AttributeDomain::edge, source_edges.size()},
        {attributes::AttributeDomain::face, 1U},
        {attributes::AttributeDomain::corner, source_corners.size()}});
    REQUIRE(source_attributes.add_layer({
        {"vertex_signal", attributes::AttributeDomain::vertex, attributes::AttributeType::float64},
        std::move(vertex_values)}));
    REQUIRE(source_attributes.add_layer({
        {"edge_signal", attributes::AttributeDomain::edge, attributes::AttributeType::float64},
        std::move(edge_values)}));
    REQUIRE(source_attributes.add_layer({
        {"material", attributes::AttributeDomain::face, attributes::AttributeType::uint32},
        {std::uint32_t{7U}}}));
    REQUIRE(source_attributes.add_layer({
        {"uv0", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        std::move(corner_uvs)}));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({1U, "UV0", "uv0", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(source_attributes), std::move(uv_sets)));

    const auto source_revision = mesh.revision();
    const auto receipt = mesh_attributes::knife_cut_with_attribute_transfer(
        mesh, face.value(), {{0.5, 0.0, 0.0}, {1.0, 0.75, 0.0}, {2.0, 0.5, 0.0}});
    if (!receipt) std::cerr << "attribute knife refused: " << receipt.error().message << '\n';
    REQUIRE(receipt);
    REQUIRE(receipt.value().revision_before == source_revision);
    REQUIRE(receipt.value().revision_after == mesh.revision());
    REQUIRE(receipt.value().validate());
    REQUIRE(mesh.uv_sets().find(1U) != nullptr);

    const auto* transferred_vertices = mesh.attributes().find("vertex_signal");
    const auto* transferred_edges = mesh.attributes().find("edge_signal");
    const auto* transferred_faces = mesh.attributes().find("material");
    const auto* transferred_uvs = mesh.attributes().find("uv0");
    REQUIRE(transferred_vertices && transferred_edges && transferred_faces && transferred_uvs);
    std::size_t interpolated_vertex_count = 0U;
    geometry::VertexId generated_path_vertex;
    for (const auto vertex_id : receipt.value().created_vertices) {
        const auto* vertex = mesh.find_vertex(vertex_id);
        REQUIRE(vertex != nullptr);
        const auto sorted_vertices = mesh.vertices_sorted();
        const auto position = std::find_if(sorted_vertices.begin(), sorted_vertices.end(),
            [vertex_id](const auto& candidate) { return candidate.id == vertex_id; });
        REQUIRE(position != sorted_vertices.end());
        const std::size_t value_index = static_cast<std::size_t>(
            std::distance(sorted_vertices.begin(), position));
        const auto origin = receipt.value().vertex_origins.at(vertex_id);
        const double value = std::get<double>(transferred_vertices->values[value_index]);
        if (origin.kind == geometry::OriginKind::interpolated_from) {
            const double expected = vertex->position.x + 10.0 * vertex->position.y;
            REQUIRE(std::abs(value - expected) < 1e-12);
            ++interpolated_vertex_count;
        } else {
            REQUIRE(origin.kind == geometry::OriginKind::generated_from_face);
            REQUIRE(value == 0.0);
            generated_path_vertex = vertex_id;
        }
    }
    REQUIRE(interpolated_vertex_count == 2U);
    REQUIRE(generated_path_vertex);

    const auto destination_topology = mesh.topology();
    REQUIRE(destination_topology);
    std::vector<geometry::CornerRecord> destination_corners =
        destination_topology.value().corners;
    std::sort(destination_corners.begin(), destination_corners.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::size_t endpoint_corner_count = 0U;
    for (std::size_t index = 0U; index < destination_corners.size(); ++index) {
        const auto& corner = destination_corners[index];
        const auto* vertex = mesh.find_vertex(corner.vertex);
        REQUIRE(vertex != nullptr);
        const auto uv = std::get<core::Vec2d>(transferred_uvs->values[index]);
        if (corner.vertex == generated_path_vertex) {
            REQUIRE(uv.x == 0.0 && uv.y == 0.0);
            continue;
        }
        if (!receipt.value().corner_origins.contains(corner.id) ||
            receipt.value().corner_origins.at(corner.id).kind !=
                geometry::OriginKind::interpolated_from) {
            continue;
        }
        REQUIRE(std::abs(uv.x - vertex->position.x) < 1e-12);
        REQUIRE(std::abs(uv.y - vertex->position.y) < 1e-12);
        ++endpoint_corner_count;
    }
    REQUIRE(endpoint_corner_count == 4U);

    for (const auto edge_id : receipt.value().created_edges) {
        const auto* edge = mesh.find_edge(edge_id);
        REQUIRE(edge != nullptr);
        const auto origin = receipt.value().edge_origins.at(edge_id);
        const auto sorted_edges = mesh.edges_sorted(64U);
        const auto position = std::find_if(sorted_edges.begin(), sorted_edges.end(),
            [edge_id](const auto& candidate) { return candidate.id == edge_id; });
        REQUIRE(position != sorted_edges.end());
        const std::size_t value_index = static_cast<std::size_t>(
            std::distance(sorted_edges.begin(), position));
        const double value = std::get<double>(transferred_edges->values[value_index]);
        if (origin.kind == geometry::OriginKind::split_from) {
            REQUIRE(value == static_cast<double>(origin.source_ids.front() * 10U));
        } else {
            REQUIRE(origin.kind == geometry::OriginKind::generated_from_face);
            REQUIRE(value == 0.0);
        }
    }
    REQUIRE(transferred_faces->values.size() == 2U);
    REQUIRE(std::get<std::uint32_t>(transferred_faces->values[0U]) == 7U);
    REQUIRE(std::get<std::uint32_t>(transferred_faces->values[1U]) == 7U);
    REQUIRE(mesh.validate());

    geometry::EditableMesh discrete_mesh;
    const auto p0 = discrete_mesh.add_vertex({0.0, 0.0, 0.0});
    const auto p1 = discrete_mesh.add_vertex({2.0, 0.0, 0.0});
    const auto p2 = discrete_mesh.add_vertex({2.0, 2.0, 0.0});
    const auto p3 = discrete_mesh.add_vertex({0.0, 2.0, 0.0});
    REQUIRE(p0 && p1 && p2 && p3);
    const auto discrete_face = discrete_mesh.add_face(
        {p0.value(), p1.value(), p2.value(), p3.value()});
    REQUIRE(discrete_face);
    attributes::AttributeSet discrete_payload({
        {attributes::AttributeDomain::vertex, 4U}});
    REQUIRE(discrete_payload.add_layer({
        {"vertex_class", attributes::AttributeDomain::vertex, attributes::AttributeType::uint32},
        {std::uint32_t{1U}, std::uint32_t{2U}, std::uint32_t{3U}, std::uint32_t{4U}}}));
    REQUIRE(discrete_mesh.set_attribute_payload(
        std::move(discrete_payload), attributes::UvSetTable{}));
    const auto discrete_revision = discrete_mesh.revision();
    const auto discrete_cut = mesh_attributes::knife_cut_with_attribute_transfer(
        discrete_mesh, discrete_face.value(),
        {{0.5, 0.0, 0.0}, {1.0, 0.75, 0.0}, {2.0, 0.5, 0.0}});
    REQUIRE(!discrete_cut);
    REQUIRE(discrete_cut.error().code == core::ErrorCode::unsupported);
    REQUIRE(discrete_mesh.revision() == discrete_revision);
    REQUIRE(discrete_mesh.face_count() == 1U);
    REQUIRE(discrete_mesh.vertex_count() == 4U);
    REQUIRE(discrete_mesh.has_attribute_payload());
    const auto* unchanged_classes = discrete_mesh.attributes().find("vertex_class");
    REQUIRE(unchanged_classes != nullptr);
    REQUIRE(std::get<std::uint32_t>(unchanged_classes->values[0U]) == 1U);
    REQUIRE(std::get<std::uint32_t>(unchanged_classes->values[3U]) == 4U);
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
    const auto too_wide_multisegment = box.value().bevel_edge(edge, 0.5, 3U);
    REQUIRE(!too_wide_multisegment);
    REQUIRE(too_wide_multisegment.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(box.value().revision() == before_revision);
    REQUIRE(box.value().vertex_count() == before_vertices);

    for (const std::uint32_t invalid_segments : {0U, 65U}) {
        const auto invalid_count = box.value().bevel_edge(edge, 0.1, invalid_segments);
        REQUIRE(!invalid_count);
        REQUIRE(invalid_count.error().code == carto::core::ErrorCode::invalid_argument);
        REQUIRE(box.value().revision() == before_revision);
        REQUIRE(box.value().vertex_count() == before_vertices);
    }
    REQUIRE(box.value().validate());

    auto segmented_mesh = box.value();
    const auto segmented = segmented_mesh.bevel_edge(edge, 0.1, 3U);
    if (!segmented) throw TestFailure("three-segment edge bevel: " + segmented.error().message);
    REQUIRE(segmented);
    REQUIRE(segmented.value().created_vertices.size() == 8U);
    REQUIRE(segmented.value().created_faces.size() == 3U);
    REQUIRE(segmented.value().vertex_origins.size() == 8U);
    REQUIRE(segmented.value().face_origins.size() == 3U);
    REQUIRE(segmented.value().validate());
    REQUIRE(segmented_mesh.validate());
    REQUIRE(segmented_mesh.revision() == before_revision.next());
    for (const auto& [face, origin] : segmented.value().face_origins) {
        static_cast<void>(face);
        REQUIRE(origin.kind == carto::geometry::OriginKind::generated_from_edge);
        REQUIRE(origin.source_ids == std::vector<std::uint64_t>{edge.value});
    }
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
             std::pair{1.0, 2U},
             std::pair{0.1, 0U},
             std::pair{0.1, 65U},
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

    auto segmented_tetrahedron = make_tetrahedron();
    REQUIRE(segmented_tetrahedron);
    auto segmented_mesh = std::move(segmented_tetrahedron.value());
    const auto segmented = segmented_mesh.bevel_vertex(
        carto::geometry::VertexId{1U}, 0.25, 3U);
    if (!segmented) throw TestFailure("three-segment vertex bevel: " + segmented.error().message);
    REQUIRE(segmented);
    REQUIRE(segmented.value().created_vertices.size() == 10U);
    REQUIRE(segmented.value().created_faces.size() == 18U);
    REQUIRE(segmented.value().vertex_origins.size() == 10U);
    REQUIRE(segmented.value().face_origins.size() == 18U);
    REQUIRE(segmented.value().validate());
    REQUIRE(segmented_mesh.validate());
    REQUIRE(segmented_mesh.revision() == before_revision.next());
    std::size_t cap_face_origins = 0U;
    std::size_t incident_face_origins = 0U;
    for (const auto& [face, origin] : segmented.value().face_origins) {
        static_cast<void>(face);
        if (origin.kind == carto::geometry::OriginKind::generated_from_vertex) {
            ++cap_face_origins;
            REQUIRE(origin.source_ids == std::vector<std::uint64_t>{1U});
        } else if (origin.kind == carto::geometry::OriginKind::generated_from_face) {
            ++incident_face_origins;
            REQUIRE(origin.source_ids.size() == 1U);
        }
    }
    REQUIRE(cap_face_origins == 9U);
    REQUIRE(incident_face_origins == 9U);
}

void vertex_bevel_supports_bounded_closed_triangular_fans() {
    using namespace carto;
    auto pyramid = make_closed_triangular_fan(4U);
    REQUIRE(pyramid);
    auto single_segment = std::move(pyramid.value());
    const auto source_revision = single_segment.revision();
    const auto source_faces = single_segment.face_count();
    const auto single = single_segment.bevel_vertex(geometry::VertexId{1U}, 0.1, 1U);
    REQUIRE(single);
    REQUIRE(single.value().revision_before == source_revision);
    REQUIRE(single.value().revision_after == source_revision.next());
    REQUIRE(single.value().created_vertices.size() == 4U);
    REQUIRE(single.value().created_faces.size() == 1U);
    REQUIRE(single_segment.face_count() == source_faces + 1U);
    REQUIRE(single_segment.find_vertex(geometry::VertexId{1U}) == nullptr);
    REQUIRE(single_segment.validate());
    REQUIRE(single.value().validate());

    auto segmented_fixture = make_closed_triangular_fan(4U);
    REQUIRE(segmented_fixture);
    auto segmented = std::move(segmented_fixture.value());
    const auto segmented_receipt = segmented.bevel_vertex(
        geometry::VertexId{1U}, 0.1, 3U);
    REQUIRE(segmented_receipt);
    REQUIRE(segmented_receipt.value().created_vertices.size() == 13U);
    REQUIRE(segmented_receipt.value().created_faces.size() == 24U);
    REQUIRE(segmented_receipt.value().vertex_origins.size() == 13U);
    REQUIRE(segmented_receipt.value().face_origins.size() == 24U);
    REQUIRE(segmented_receipt.value().validate());
    REQUIRE(segmented.validate());

    auto skewed_fixture = make_closed_triangular_fan(4U, {0.25, 0.15, 1.0});
    REQUIRE(skewed_fixture);
    auto skewed = std::move(skewed_fixture.value());
    const auto skewed_revision = skewed.revision();
    const auto skewed_vertex_count = skewed.vertex_count();
    const auto skewed_face_count = skewed.face_count();
    const auto nonplanar_cap = skewed.bevel_vertex(geometry::VertexId{1U}, 0.1, 1U);
    REQUIRE(!nonplanar_cap);
    REQUIRE(nonplanar_cap.error().code == core::ErrorCode::validation_failed);
    REQUIRE(skewed.revision() == skewed_revision);
    REQUIRE(skewed.vertex_count() == skewed_vertex_count);
    REQUIRE(skewed.face_count() == skewed_face_count);
    REQUIRE(skewed.find_vertex(geometry::VertexId{1U}) != nullptr);
    REQUIRE(skewed.validate());

    geometry::EditableMesh concave_fan;
    const auto concave_apex = concave_fan.add_vertex({0.0, 0.0, 1.0});
    REQUIRE(concave_apex);
    constexpr std::array<core::Vec2d, 5U> concave_ring{{
        {1.0, 0.0}, {0.2, 0.2}, {0.0, 1.0}, {-1.0, 0.0}, {0.0, -1.0}}};
    std::vector<geometry::VertexId> concave_ring_ids;
    concave_ring_ids.reserve(concave_ring.size());
    for (const auto position : concave_ring) {
        const auto vertex = concave_fan.add_vertex({position.x, position.y, 0.0});
        REQUIRE(vertex);
        concave_ring_ids.push_back(vertex.value());
    }
    for (std::size_t index = 0U; index < concave_ring_ids.size(); ++index) {
        REQUIRE(concave_fan.add_face({
            concave_apex.value(), concave_ring_ids[index],
            concave_ring_ids[(index + 1U) % concave_ring_ids.size()]}));
    }
    std::vector<geometry::VertexId> concave_base;
    concave_base.push_back(concave_ring_ids.front());
    for (std::size_t index = concave_ring_ids.size(); index > 1U; --index) {
        concave_base.push_back(concave_ring_ids[index - 1U]);
    }
    REQUIRE(concave_fan.add_face(std::move(concave_base)));
    REQUIRE(concave_fan.validate());
    const auto concave_revision = concave_fan.revision();
    const auto concave_vertex_count = concave_fan.vertex_count();
    const auto concave_face_count = concave_fan.face_count();
    const auto nonconvex_cap = concave_fan.bevel_vertex(
        concave_apex.value(), 0.05, 1U);
    REQUIRE(!nonconvex_cap);
    REQUIRE(nonconvex_cap.error().code == core::ErrorCode::validation_failed);
    REQUIRE(concave_fan.revision() == concave_revision);
    REQUIRE(concave_fan.vertex_count() == concave_vertex_count);
    REQUIRE(concave_fan.face_count() == concave_face_count);
    REQUIRE(concave_fan.find_vertex(concave_apex.value()) != nullptr);
    REQUIRE(concave_fan.validate());

    auto maximum_fan_fixture = make_closed_triangular_fan(64U);
    REQUIRE(maximum_fan_fixture);
    auto maximum_fan = std::move(maximum_fan_fixture.value());
    const auto maximum_fan_receipt = maximum_fan.bevel_vertex(
        geometry::VertexId{1U}, 0.001, 1U);
    REQUIRE(maximum_fan_receipt);
    REQUIRE(maximum_fan_receipt.value().created_vertices.size() == 64U);
    REQUIRE(maximum_fan_receipt.value().created_faces.size() == 1U);
    REQUIRE(maximum_fan_receipt.value().validate());
    REQUIRE(maximum_fan.validate());

    auto over_limit_fixture = make_closed_triangular_fan(65U);
    REQUIRE(over_limit_fixture);
    auto over_limit = std::move(over_limit_fixture.value());
    const auto over_limit_revision = over_limit.revision();
    const auto over_limit_vertex_count = over_limit.vertex_count();
    const auto over_limit_face_count = over_limit.face_count();
    const auto excessive_fan = over_limit.bevel_vertex(
        geometry::VertexId{1U}, 0.01, 1U);
    REQUIRE(!excessive_fan);
    REQUIRE(excessive_fan.error().code == core::ErrorCode::unsupported);
    REQUIRE(over_limit.revision() == over_limit_revision);
    REQUIRE(over_limit.vertex_count() == over_limit_vertex_count);
    REQUIRE(over_limit.face_count() == over_limit_face_count);
    REQUIRE(over_limit.find_vertex(geometry::VertexId{1U}) != nullptr);
    REQUIRE(over_limit.validate());

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

    carto::geometry::EditableMesh concave;
    const auto ca = concave.add_vertex({0.0, 0.0, 0.0});
    const auto cb = concave.add_vertex({1.0, 1.0, 0.0});
    const auto cc = concave.add_vertex({2.0, 0.0, 0.0});
    const auto cd = concave.add_vertex({3.0, -0.1, 0.0});
    REQUIRE(ca && cb && cc && cd);
    const auto concave_first = concave.add_face({ca.value(), cb.value(), cc.value()});
    const auto concave_second = concave.add_face({ca.value(), cc.value(), cd.value()});
    REQUIRE(concave_first && concave_second);
    REQUIRE(concave.validate());
    const auto concave_topology = concave.topology();
    REQUIRE(concave_topology);
    const auto concave_shared = std::find_if(
        concave_topology.value().edges.begin(), concave_topology.value().edges.end(),
        [ca, cc](const carto::geometry::EdgeRecord& edge) {
            return edge.first == ca.value() && edge.second == cc.value();
        });
    REQUIRE(concave_shared != concave_topology.value().edges.end());
    const auto concave_revision = concave.revision();
    const auto concave_before_conversion = concave;
    const auto concave_dissolved = concave.dissolve_edge(concave_shared->id);
    REQUIRE(concave_dissolved);
    REQUIRE(concave_dissolved.value().revision_after == concave_revision.next());
    REQUIRE(concave_dissolved.value().removed_faces.size() == 1U);
    REQUIRE(concave.face_count() == 1U);
    REQUIRE(concave.validate());
    const auto concave_compiled = concave.compile();
    REQUIRE(concave_compiled);
    REQUIRE(concave_compiled.value().triangle_faces.size() == 2U);
    REQUIRE(concave_dissolved.value().validate());

    auto concave_pair = concave_before_conversion;
    const auto concave_pair_result = concave_pair.tri_to_quad(
        concave_first.value(), concave_second.value());
    REQUIRE(concave_pair_result);
    REQUIRE(concave_pair.face_count() == 1U);
    const auto concave_pair_compiled = concave_pair.compile();
    REQUIRE(concave_pair_compiled);
    REQUIRE(concave_pair_compiled.value().triangle_faces.size() == 2U);

    carto::geometry::EditableMesh self_crossing;
    const auto sa = self_crossing.add_vertex({0.0, 0.0, 0.0});
    const auto sb = self_crossing.add_vertex({2.0, 2.0, 0.0});
    const auto sc = self_crossing.add_vertex({2.0, 0.0, 0.0});
    const auto sd = self_crossing.add_vertex({0.0, 2.0, 0.0});
    REQUIRE(sa && sb && sc && sd);
    REQUIRE(self_crossing.add_face({sa.value(), sb.value(), sc.value()}));
    REQUIRE(self_crossing.add_face({sa.value(), sc.value(), sd.value()}));
    REQUIRE(self_crossing.validate());
    const auto self_crossing_topology = self_crossing.topology();
    REQUIRE(self_crossing_topology);
    const auto self_crossing_edge = std::find_if(
        self_crossing_topology.value().edges.begin(), self_crossing_topology.value().edges.end(),
        [sa, sc](const carto::geometry::EdgeRecord& edge) {
            return edge.first == sa.value() && edge.second == sc.value();
        });
    REQUIRE(self_crossing_edge != self_crossing_topology.value().edges.end());
    const auto self_crossing_revision = self_crossing.revision();
    const auto self_crossing_vertices = self_crossing.vertices_sorted();
    const auto self_crossing_faces = self_crossing.faces_sorted();
    const auto self_crossing_rejected = self_crossing.dissolve_edge(self_crossing_edge->id);
    REQUIRE(!self_crossing_rejected);
    REQUIRE(self_crossing_rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(self_crossing.revision() == self_crossing_revision);
    const auto self_crossing_vertices_after = self_crossing.vertices_sorted();
    const auto self_crossing_faces_after = self_crossing.faces_sorted();
    REQUIRE(self_crossing_vertices_after.size() == self_crossing_vertices.size());
    REQUIRE(self_crossing_faces_after.size() == self_crossing_faces.size());
    for (std::size_t index = 0U; index < self_crossing_vertices.size(); ++index) {
        REQUIRE(self_crossing_vertices_after[index].id == self_crossing_vertices[index].id);
        REQUIRE(self_crossing_vertices_after[index].position.x == self_crossing_vertices[index].position.x);
        REQUIRE(self_crossing_vertices_after[index].position.y == self_crossing_vertices[index].position.y);
        REQUIRE(self_crossing_vertices_after[index].position.z == self_crossing_vertices[index].position.z);
    }
    for (std::size_t index = 0U; index < self_crossing_faces.size(); ++index) {
        REQUIRE(self_crossing_faces_after[index].id == self_crossing_faces[index].id);
        REQUIRE(self_crossing_faces_after[index].vertices == self_crossing_faces[index].vertices);
    }
    REQUIRE(self_crossing.find_edge(self_crossing_edge->id) != nullptr);
    REQUIRE(self_crossing.validate());
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
    const auto e = mesh.add_vertex({3.0, 0.0, 0.0});
    const auto f = mesh.add_vertex({4.0, 0.0, 0.0});
    const auto g = mesh.add_vertex({3.5, 1.0, 0.0});
    REQUIRE(first && second && e && f && g);
    const auto unrelated = mesh.add_face({e.value(), f.value(), g.value()});
    REQUIRE(unrelated);
    REQUIRE(mesh.validate());
    const auto before_revision = mesh.revision();
    const auto before_conversion = mesh;
    const auto converted = mesh.tri_to_quad(first.value(), second.value());
    REQUIRE(converted);
    REQUIRE(converted.value().revision_before == before_revision);
    REQUIRE(converted.value().removed_faces.size() == 1U);
    REQUIRE(mesh.face_count() == 2U);
    REQUIRE(mesh.find_face(unrelated.value()) != nullptr);
    REQUIRE(mesh.validate());
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().triangle_faces.size() == 3U);

    auto reversed_pair = before_conversion;
    const auto reversed = reversed_pair.tri_to_quad(second.value(), first.value());
    REQUIRE(reversed);
    REQUIRE(reversed_pair.face_count() == 2U);
    REQUIRE(reversed_pair.find_face(unrelated.value()) != nullptr);
    REQUIRE(reversed_pair.validate());

    auto non_adjacent_pair = before_conversion;
    const auto non_adjacent_revision = non_adjacent_pair.revision();
    const auto non_adjacent = non_adjacent_pair.tri_to_quad(first.value(), unrelated.value());
    REQUIRE(!non_adjacent);
    REQUIRE(non_adjacent.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(non_adjacent_pair.revision() == non_adjacent_revision);
    REQUIRE(non_adjacent_pair.face_count() == 3U);
    REQUIRE(non_adjacent_pair.validate());

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
    const auto selection_only_mode = mesh.merge_vertices(
        first.value(), second.value(), carto::geometry::VertexMergeMode::last_selected);
    REQUIRE(!selection_only_mode);
    REQUIRE(selection_only_mode.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(mesh.revision() == before_revision);
}

void center_weld_moves_target_to_midpoint_and_keeps_target_identity() {
    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    const auto collapsed_face = mesh.add_face({target.value(), source.value(), third.value()});
    const auto surviving_face = mesh.add_face({source.value(), fourth.value(), third.value()});
    REQUIRE(collapsed_face && surviving_face);

    const auto before_revision = mesh.revision();
    const auto merged = mesh.merge_vertices(
        target.value(), source.value(), carto::geometry::VertexMergeMode::center);
    REQUIRE(merged);
    REQUIRE(merged.value().revision_after == before_revision.next());
    REQUIRE(merged.value().merged_vertices.at(source.value()) == target.value());
    REQUIRE(mesh.find_vertex(source.value()) == nullptr);
    const auto* survivor = mesh.find_vertex(target.value());
    REQUIRE(survivor != nullptr);
    REQUIRE(survivor->position.x == 0.5);
    REQUIRE(survivor->position.y == 0.0);
    REQUIRE(survivor->position.z == 0.0);
    REQUIRE(mesh.face_count() == 1U);
    REQUIRE(mesh.find_face(surviving_face.value()) != nullptr);
    REQUIRE(mesh.validate());
}

void center_weld_rejects_ambiguous_collapse_without_moving_target() {
    carto::geometry::EditableMesh mesh;
    const auto first = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto second = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(first && second && third && fourth);
    REQUIRE(mesh.add_face({first.value(), second.value(), third.value(), fourth.value()}));
    const auto before_revision = mesh.revision();

    const auto ambiguous = mesh.merge_vertices(
        first.value(), third.value(), carto::geometry::VertexMergeMode::center);
    REQUIRE(!ambiguous);
    REQUIRE(ambiguous.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(mesh.revision() == before_revision);
    REQUIRE(mesh.find_vertex(first.value())->position.x == 0.0);
    REQUIRE(mesh.find_vertex(first.value())->position.y == 0.0);
    REQUIRE(mesh.find_vertex(third.value()) != nullptr);

    const auto unsupported_mode = mesh.merge_vertices(
        first.value(), second.value(), static_cast<carto::geometry::VertexMergeMode>(7U));
    REQUIRE(!unsupported_mode);
    REQUIRE(unsupported_mode.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(mesh.revision() == before_revision);
    REQUIRE(mesh.validate());
}

void merge_by_distance_stitches_coincident_seam_in_one_receipt() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto b_copy = mesh.add_vertex({1.0 + 1e-5, 0.0, 0.0});
    const auto a_copy = mesh.add_vertex({0.0, 1e-5, 0.0});
    const auto d = mesh.add_vertex({0.5, -1.0, 0.0});
    REQUIRE(a && b && c && b_copy && a_copy && d);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    REQUIRE(mesh.add_face({b_copy.value(), a_copy.value(), d.value()}));
    REQUIRE(mesh.validate());
    const auto before_revision = mesh.revision();

    const auto merged = mesh.merge_by_distance(
        {d.value(), c.value(), b_copy.value(), a.value(), a_copy.value(), b.value()}, 1e-3);
    REQUIRE(merged);
    REQUIRE(merged.value().revision_before == before_revision);
    REQUIRE(merged.value().revision_after == before_revision.next());
    REQUIRE(mesh.revision() == before_revision.next());
    REQUIRE(merged.value().merged_vertices.size() == 2U);
    REQUIRE(merged.value().merged_vertices.at(b_copy.value()) == b.value());
    REQUIRE(merged.value().merged_vertices.at(a_copy.value()) == a.value());
    REQUIRE(merged.value().removed_faces.empty());
    REQUIRE(mesh.vertex_count() == 4U);
    REQUIRE(mesh.face_count() == 2U);
    REQUIRE(mesh.edge_count() == 5U);
    REQUIRE(mesh.find_vertex(a.value())->position.y == 0.0);
    REQUIRE(mesh.find_vertex(b.value())->position.x == 1.0);
    REQUIRE(mesh.validate());

    const auto decoded = carto::geometry::TopologyEditReceipt::deserialize(
        merged.value().serialize());
    REQUIRE(decoded);
    REQUIRE(decoded.value().merged_vertices == merged.value().merged_vertices);
}

void merge_by_distance_indexes_exact_corner_origins_for_many_sources() {
    using namespace carto;
    constexpr std::size_t triangle_count = 512U;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    std::vector<geometry::VertexId> candidates;
    vertices.reserve(triangle_count * 3U);
    faces.reserve(triangle_count);
    candidates.reserve(triangle_count);
    for (std::size_t index = 0U; index < triangle_count; ++index) {
        const geometry::VertexId apex{index * 3U + 1U};
        const geometry::VertexId second{index * 3U + 2U};
        const geometry::VertexId third{index * 3U + 3U};
        const double extent = static_cast<double>(index + 1U);
        vertices.push_back({apex, {0.0, 0.0, 0.0}});
        vertices.push_back({second, {extent, 1.0, 0.0}});
        vertices.push_back({third, {extent, 0.0, 1.0}});
        faces.push_back({geometry::FaceId{index + 1U}, {apex, second, third}});
        candidates.push_back(apex);
    }
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    REQUIRE(mesh.validate());
    const auto before_revision = mesh.revision();
    const auto before_digest = application::canonical_mesh_geometry_digest(mesh);
    const auto before_topology = mesh.topology();
    REQUIRE(before_topology);
    std::map<geometry::FaceId, geometry::CornerId> old_apex_corners;
    for (const auto& corner : before_topology.value().corners) {
        if (std::binary_search(candidates.begin(), candidates.end(), corner.vertex)) {
            old_apex_corners.emplace(corner.face, corner.id);
        }
    }
    REQUIRE(old_apex_corners.size() == triangle_count);
    auto invalid_candidates = candidates;
    invalid_candidates.push_back(candidates.front());
    REQUIRE(!mesh.merge_by_distance(invalid_candidates, 1e-6));
    REQUIRE(mesh.revision() == before_revision);
    REQUIRE(application::canonical_mesh_geometry_digest(mesh) == before_digest);
    const auto unchanged_topology = mesh.topology();
    REQUIRE(unchanged_topology);
    REQUIRE(unchanged_topology.value().corners.size() == before_topology.value().corners.size());
    for (std::size_t index = 0U; index < before_topology.value().corners.size(); ++index) {
        const auto& before = before_topology.value().corners[index];
        const auto& unchanged = unchanged_topology.value().corners[index];
        REQUIRE(unchanged.id == before.id);
        REQUIRE(unchanged.face == before.face);
        REQUIRE(unchanged.vertex == before.vertex);
    }

    auto repeated = mesh;
    const auto merged = mesh.merge_by_distance(candidates, 1e-6);
    REQUIRE(merged && merged.value().validate());
    REQUIRE(mesh.validate());
    REQUIRE(merged.value().merged_vertices.size() == triangle_count - 1U);
    REQUIRE(merged.value().created_corners.size() == triangle_count - 1U);
    REQUIRE(merged.value().corner_origins.size() == triangle_count - 1U);
    REQUIRE(merged.value().removed_faces.empty());
    REQUIRE(mesh.face_count() == triangle_count);
    REQUIRE(mesh.vertex_count() == triangle_count * 2U + 1U);
    const auto after_topology = mesh.topology();
    REQUIRE(after_topology);
    std::size_t checked_origins = 0U;
    for (const auto& corner : after_topology.value().corners) {
        if (corner.vertex != candidates.front()) continue;
        if (corner.face == geometry::FaceId{1U}) {
            REQUIRE(corner.id == old_apex_corners.at(corner.face));
            REQUIRE(!merged.value().corner_origins.contains(corner.id));
            continue;
        }
        REQUIRE(std::binary_search(merged.value().created_corners.begin(),
            merged.value().created_corners.end(), corner.id));
        const auto& origin = merged.value().corner_origins.at(corner.id);
        REQUIRE(origin.kind == geometry::OriginKind::duplicated_from);
        REQUIRE(origin.source_ids == std::vector<std::uint64_t>{old_apex_corners.at(corner.face).value});
        ++checked_origins;
    }
    REQUIRE(checked_origins == triangle_count - 1U);
    std::reverse(candidates.begin(), candidates.end());
    const auto replayed = repeated.merge_by_distance(candidates, 1e-6);
    REQUIRE(replayed);
    REQUIRE(replayed.value().serialize() == merged.value().serialize());
    const auto decoded = geometry::TopologyEditReceipt::deserialize(merged.value().serialize());
    REQUIRE(decoded && decoded.value().serialize() == merged.value().serialize());
}

void merge_by_distance_does_not_guess_ambiguous_corner_origins() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {0.001, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.002, 0.001, 0.0}},
         {geometry::VertexId{4U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{5U}, {-1.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{2U}, geometry::VertexId{3U},
            geometry::VertexId{4U}, geometry::VertexId{5U}}}}));
    const auto merged = mesh.merge_by_distance(
        {geometry::VertexId{1U}, geometry::VertexId{2U}, geometry::VertexId{3U}}, 0.01);
    REQUIRE(merged && merged.value().validate());
    REQUIRE(mesh.validate());
    REQUIRE(merged.value().merged_vertices.size() == 2U);
    REQUIRE(merged.value().created_corners.size() == 1U);
    REQUIRE(!merged.value().corner_origins.contains(merged.value().created_corners.front()));
}

void merge_by_distance_is_not_transitive() {
    carto::geometry::EditableMesh mesh;
    const auto first = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto second = mesh.add_vertex({0.6, 0.0, 0.0});
    const auto third = mesh.add_vertex({1.2, 0.0, 0.0});
    REQUIRE(first && second && third);

    const auto merged = mesh.merge_by_distance(
        {first.value(), second.value(), third.value()}, 1.0);
    REQUIRE(merged);
    REQUIRE(merged.value().merged_vertices.size() == 1U);
    REQUIRE(merged.value().merged_vertices.at(second.value()) == first.value());
    REQUIRE(mesh.find_vertex(first.value()) != nullptr);
    REQUIRE(mesh.find_vertex(second.value()) == nullptr);
    REQUIRE(mesh.find_vertex(third.value()) != nullptr);
    REQUIRE(mesh.find_vertex(first.value())->position.x == 0.0);
    REQUIRE(mesh.validate());
}

void merge_by_distance_preserves_extreme_finite_distance_boundaries() {
    for (const double distance : {1e200, 1e-200}) {
        for (const double separation : {
                distance * 0.5, distance,
                std::nextafter(distance, std::numeric_limits<double>::infinity()),
                distance * 1.9}) {
            carto::geometry::EditableMesh mesh;
            const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
            const auto source = mesh.add_vertex({separation, 0.0, 0.0});
            REQUIRE(target && source);
            const auto before_revision = mesh.revision();

            const auto merged = mesh.merge_by_distance(
                {source.value(), target.value()}, distance);
            if (separation <= distance) {
                REQUIRE(merged);
                REQUIRE(mesh.revision() == before_revision.next());
                REQUIRE(merged.value().merged_vertices.size() == 1U);
                REQUIRE(merged.value().merged_vertices.at(source.value()) == target.value());
                REQUIRE(merged.value().removed_vertices ==
                        std::vector<carto::geometry::VertexId>{source.value()});
                REQUIRE(mesh.vertex_count() == 1U);
                REQUIRE(mesh.find_vertex(source.value()) == nullptr);
                REQUIRE(merged.value().validate());
            } else {
                REQUIRE(!merged);
                REQUIRE(merged.error().code == carto::core::ErrorCode::invalid_state);
                REQUIRE(mesh.revision() == before_revision);
                REQUIRE(mesh.vertex_count() == 2U);
                REQUIRE(mesh.find_vertex(source.value())->position.x == separation);
            }
            REQUIRE(mesh.find_vertex(target.value())->position.x == 0.0);
            REQUIRE(mesh.validate());
        }
    }
}

void merge_by_distance_rejects_invalid_requests_without_mutation() {
    carto::geometry::EditableMesh mesh;
    const auto first = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto second = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto near_first = mesh.add_vertex({0.002, 0.002, 0.0});
    const auto fourth = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(first && second && near_first && fourth);
    REQUIRE(mesh.add_face({first.value(), second.value(), near_first.value(), fourth.value()}));
    REQUIRE(mesh.validate());
    const auto before_revision = mesh.revision();
    const auto all = std::vector<carto::geometry::VertexId>{
        first.value(), second.value(), near_first.value(), fourth.value()};

    const auto non_simple = mesh.merge_by_distance(all, 0.01);
    REQUIRE(!non_simple);
    REQUIRE(non_simple.error().code == carto::core::ErrorCode::unsupported);

    const auto nothing_close = mesh.merge_by_distance(
        {second.value(), fourth.value()}, 0.01);
    REQUIRE(!nothing_close);
    REQUIRE(nothing_close.error().code == carto::core::ErrorCode::invalid_state);

    for (const double distance : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN()}) {
        const auto rejected = mesh.merge_by_distance(all, distance);
        REQUIRE(!rejected);
        REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_argument);
    }
    const auto single = mesh.merge_by_distance({first.value()}, 0.01);
    REQUIRE(!single);
    REQUIRE(single.error().code == carto::core::ErrorCode::invalid_argument);
    const auto duplicate = mesh.merge_by_distance({first.value(), first.value()}, 0.01);
    REQUIRE(!duplicate);
    REQUIRE(duplicate.error().code == carto::core::ErrorCode::invalid_argument);
    const auto missing = mesh.merge_by_distance(
        {first.value(), carto::geometry::VertexId{99U}}, 0.01);
    REQUIRE(!missing);
    REQUIRE(missing.error().code == carto::core::ErrorCode::not_found);
    const auto tiny = mesh.merge_by_distance({first.value(), second.value()}, 1e-300);
    REQUIRE(!tiny);
    REQUIRE(tiny.error().code == carto::core::ErrorCode::unsupported);

    REQUIRE(mesh.revision() == before_revision);
    REQUIRE(mesh.vertex_count() == 4U);
    REQUIRE(mesh.face_count() == 1U);
    REQUIRE(mesh.find_vertex(near_first.value()) != nullptr);
    REQUIRE(mesh.validate());
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

void selected_face_poke_batches_all_faces_as_one_undoable_edit() {
    auto mesh = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(mesh);
    const auto faces = mesh.value().faces_sorted();
    REQUIRE(faces.size() == 6U);

    carto::editor::SelectionState selection;
    REQUIRE(selection.set_mode(carto::editor::SelectionMode::face));
    REQUIRE(selection.select_face(mesh.value(), faces[0].id));
    REQUIRE(selection.select_face(
        mesh.value(), faces[1].id, carto::editor::SelectionOperation::add));
    const auto before_revision = mesh.value().revision();
    const std::size_t before_vertices = mesh.value().vertex_count();
    const std::size_t before_faces = mesh.value().face_count();

    carto::editor::PokeSelectedFaceCommand command(selection, mesh.value());
    REQUIRE(command.execute());
    REQUIRE(mesh.value().revision() == before_revision.next());
    REQUIRE(mesh.value().vertex_count() == before_vertices + 2U);
    REQUIRE(mesh.value().face_count() == before_faces + 6U);
    REQUIRE(mesh.value().validate());

    REQUIRE(command.undo());
    REQUIRE(mesh.value().vertex_count() == before_vertices);
    REQUIRE(mesh.value().face_count() == before_faces);
    REQUIRE(mesh.value().validate());

    REQUIRE(command.execute());
    REQUIRE(mesh.value().vertex_count() == before_vertices + 2U);
    REQUIRE(mesh.value().face_count() == before_faces + 6U);
    REQUIRE(mesh.value().validate());
}

void selected_face_extrusion_batches_adjacent_faces_as_one_undoable_edit() {
    auto mesh = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(mesh);
    const auto faces = mesh.value().faces_sorted();
    REQUIRE(faces.size() == 6U);

    carto::editor::SelectionState selection;
    REQUIRE(selection.set_mode(carto::editor::SelectionMode::face));
    REQUIRE(selection.select_face(mesh.value(), faces[0].id));
    REQUIRE(selection.select_face(
        mesh.value(), faces[1].id, carto::editor::SelectionOperation::add));
    const auto before_revision = mesh.value().revision();
    const std::size_t before_vertices = mesh.value().vertex_count();
    const std::size_t before_faces = mesh.value().face_count();
    auto sequential_reference = mesh.value();
    REQUIRE(sequential_reference.extrude_face(faces[0].id, 0.2));
    REQUIRE(sequential_reference.extrude_face(faces[1].id, 0.2));

    carto::editor::ExtrudeSelectedFaceCommand command(selection, mesh.value(), 0.2);
    REQUIRE(command.execute());
    REQUIRE(mesh.value().revision() == before_revision.next());
    REQUIRE(mesh.value().vertex_count() == sequential_reference.vertex_count());
    REQUIRE(mesh.value().face_count() == sequential_reference.face_count());
    REQUIRE(mesh.value().vertex_count() > before_vertices);
    REQUIRE(mesh.value().face_count() > before_faces);
    REQUIRE(mesh.value().validate());

    REQUIRE(command.undo());
    REQUIRE(mesh.value().vertex_count() == before_vertices);
    REQUIRE(mesh.value().face_count() == before_faces);
    REQUIRE(mesh.value().validate());
    REQUIRE(command.execute());
    REQUIRE(mesh.value().vertex_count() == sequential_reference.vertex_count());
    REQUIRE(mesh.value().face_count() == sequential_reference.face_count());
    REQUIRE(mesh.value().validate());
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

void admitted_center_vertex_merge_moves_survivor_to_midpoint() {
    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({2.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    REQUIRE(mesh.add_face({target.value(), source.value(), third.value()}));
    REQUIRE(mesh.add_face({source.value(), fourth.value(), third.value()}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{"Center Weld", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{object, target.value()}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            object, source.value(), carto::editor::SelectionOperation::add}));

    carto::editor::ToolArguments arguments;
    arguments.merge_mode = carto::geometry::VertexMergeMode::center;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{"mesh.merge-vertices", arguments}));
    const auto& compiled = *session.snapshot().viewport.scene.instances().front().mesh;
    REQUIRE(compiled.vertex_ids.size() == 3U);
    const auto survivor = std::find(
        compiled.vertex_ids.begin(), compiled.vertex_ids.end(), target.value());
    REQUIRE(survivor != compiled.vertex_ids.end());
    const auto& position = compiled.positions[static_cast<std::size_t>(
        std::distance(compiled.vertex_ids.begin(), survivor))];
    REQUIRE(position.x == 1.0);
    REQUIRE(position.y == 0.0);
    REQUIRE(std::find(compiled.vertex_ids.begin(), compiled.vertex_ids.end(), source.value()) ==
            compiled.vertex_ids.end());

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
}

void admitted_last_selected_merge_preserves_selection_order_across_history() {
    carto::geometry::EditableMesh mesh;
    const auto first = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto second = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({2.0, 1.0, 0.0});
    REQUIRE(first && second && third && fourth);
    REQUIRE(mesh.add_face({first.value(), second.value(), third.value()}));
    REQUIRE(mesh.add_face({second.value(), fourth.value(), third.value()}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{"Last Selected Weld", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{object, second.value()}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            object, first.value(), carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            object, second.value(), carto::editor::SelectionOperation::toggle}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            object, second.value(), carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            object, first.value(), carto::editor::SelectionOperation::add}));

    const auto selected_context = session.authoring_context();
    REQUIRE(selected_context);
    REQUIRE((selected_context.value().vertices ==
             std::vector<carto::geometry::VertexId>{first.value(), second.value()}));
    REQUIRE((selected_context.value().vertex_selection_order ==
             std::vector<carto::geometry::VertexId>{second.value(), first.value()}));
    auto forged_order = selected_context.value();
    forged_order.vertex_selection_order = {first.value(), first.value()};
    REQUIRE(!forged_order.validate());

    carto::editor::ToolArguments arguments;
    arguments.merge_mode = carto::geometry::VertexMergeMode::last_selected;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{"mesh.merge-vertices", arguments}));
    const auto verify_last_survives = [&] {
        const auto& compiled = *session.snapshot().viewport.scene.instances().front().mesh;
        const auto survivor = std::find(
            compiled.vertex_ids.begin(), compiled.vertex_ids.end(), first.value());
        REQUIRE(survivor != compiled.vertex_ids.end());
        REQUIRE(std::find(compiled.vertex_ids.begin(), compiled.vertex_ids.end(), second.value()) ==
                compiled.vertex_ids.end());
        const auto& position = compiled.positions[static_cast<std::size_t>(
            std::distance(compiled.vertex_ids.begin(), survivor))];
        REQUIRE(position.x == 0.0);
        REQUIRE(position.y == 0.0);
    };
    verify_last_survives();

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RepeatLastToolAction{}));
    verify_last_survives();

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    verify_last_survives();
}

void admitted_merge_by_distance_welds_selected_seam_and_is_undoable() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto b_copy = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto a_copy = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto d = mesh.add_vertex({0.5, -1.0, 0.0});
    REQUIRE(a && b && c && b_copy && a_copy && d);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    REQUIRE(mesh.add_face({b_copy.value(), a_copy.value(), d.value()}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{"Seam", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{object, a.value()}));
    for (const auto vertex : {b.value(), b_copy.value(), a_copy.value()}) {
        REQUIRE(carto::application::HumanApplicationAccess::dispatch(
            session, carto::application::SelectVertexAction{
                object, vertex, carto::editor::SelectionOperation::add}));
    }

    carto::editor::ToolArguments arguments;
    arguments.distance = 1e-4;
    const auto merged = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{"mesh.merge-by-distance", arguments});
    REQUIRE(merged);
    REQUIRE(merged.value().parameters.has_value());
    REQUIRE(merged.value().parameters->distance == 1e-4);
    REQUIRE(session.snapshot().selection.vertices.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() == 2U);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 6U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
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

void selected_multi_edge_dissolve_commits_one_revision_and_is_undoable() {
    carto::geometry::EditableMesh mesh;
    const auto c0 = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto c1 = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c2 = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto c3 = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto center = mesh.add_vertex({0.5, 0.5, 0.0});
    REQUIRE(c0 && c1 && c2 && c3 && center);
    REQUIRE(mesh.add_face({c0.value(), c1.value(), center.value()}));
    REQUIRE(mesh.add_face({c1.value(), c2.value(), center.value()}));
    REQUIRE(mesh.add_face({c2.value(), c3.value(), center.value()}));
    REQUIRE(mesh.add_face({c3.value(), c0.value(), center.value()}));
    REQUIRE(mesh.validate());

    const auto topology = mesh.topology();
    REQUIRE(topology);
    std::vector<carto::geometry::EdgeId> internal_edges;
    for (const auto& edge : topology.value().edges) {
        if (edge.second_half_edge.has_value()) internal_edges.push_back(edge.id);
    }
    REQUIRE(internal_edges.size() >= 2U);

    auto batch_mesh = mesh;
    const auto batch_revision = batch_mesh.revision();
    const auto batch = batch_mesh.dissolve_edges({internal_edges[0], internal_edges[1]});
    REQUIRE(batch);
    REQUIRE(batch.value().revision_before == batch_revision);
    REQUIRE(batch.value().revision_after == batch_revision.next());
    REQUIRE(batch.value().removed_faces.size() == 2U);
    REQUIRE(batch.value().removed_edges.size() == 2U);
    REQUIRE(batch.value().validate());
    REQUIRE(batch_mesh.face_count() == 2U);
    REQUIRE(batch_mesh.validate());
    const auto batch_compiled = batch_mesh.compile();
    REQUIRE(batch_compiled);
    REQUIRE(batch_compiled.value().triangle_faces.size() == 4U);

    auto rejected_batch = mesh;
    const auto boundary_edge = std::find_if(
        topology.value().edges.rbegin(), topology.value().edges.rend(),
        [](const carto::geometry::EdgeRecord& edge) {
            return !edge.second_half_edge.has_value();
        });
    REQUIRE(boundary_edge != topology.value().edges.rend());
    const auto rejected_revision = rejected_batch.revision();
    const auto rejected_faces = rejected_batch.faces_sorted();
    const auto refused_batch = rejected_batch.dissolve_edges(
        {internal_edges[0], internal_edges[1], boundary_edge->id});
    REQUIRE(!refused_batch);
    REQUIRE(rejected_batch.revision() == rejected_revision);
    REQUIRE(rejected_batch.faces_sorted().size() == rejected_faces.size());
    REQUIRE(rejected_batch.validate());

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{"MultiEdgeDissolve", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectEdgeAction{
            object, internal_edges[0], carto::editor::SelectionOperation::replace}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectEdgeAction{
            object, internal_edges[1], carto::editor::SelectionOperation::toggle}));

    const auto before = session.snapshot();
    REQUIRE(before.selection.edges.size() == 2U);
    const auto dissolved = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.dissolve-edge", carto::editor::ToolArguments{}});
    REQUIRE(dissolved);
    REQUIRE(dissolved.value().affected.edges.size() == 2U);

    const auto after = session.snapshot();
    REQUIRE(after.project_revision > before.project_revision);
    REQUIRE(after.selection.edges.empty());
    const auto& before_mesh = *before.viewport.scene.instances().front().mesh;
    const auto& after_mesh = *after.viewport.scene.instances().front().mesh;
    REQUIRE(after_mesh.triangle_faces.size() == before_mesh.triangle_faces.size());
    REQUIRE(after_mesh.vertex_ids == before_mesh.vertex_ids);
    REQUIRE(after_mesh.positions.size() == before_mesh.positions.size());
    for (std::size_t index = 0U; index < before_mesh.positions.size(); ++index) {
        REQUIRE(after_mesh.positions[index].x == before_mesh.positions[index].x);
        REQUIRE(after_mesh.positions[index].y == before_mesh.positions[index].y);
        REQUIRE(after_mesh.positions[index].z == before_mesh.positions[index].z);
    }
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces ==
            before_mesh.triangle_faces);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() ==
            after_mesh.triangle_faces.size());
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

void admitted_owner_scoped_shortest_path_preserves_other_object_groups() {
    auto first_mesh = make_tetrahedron();
    auto second_mesh = make_tetrahedron();
    REQUIRE(first_mesh && second_mesh);
    const auto first_vertices = first_mesh.value().vertices_sorted();
    const auto second_vertices = second_mesh.value().vertices_sorted();
    REQUIRE(first_vertices.size() == 4U);
    REQUIRE(second_vertices.size() == 4U);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{
            "First Path Owner", std::move(first_mesh.value())}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{
            "Second Path Owner", std::move(second_mesh.value())}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first_object = initial.objects[0].object.id;
    const auto second_object = initial.objects[1].object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            first_object, first_vertices.front().id}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectVertexAction{
            second_object, second_vertices[1].id,
            carto::editor::SelectionOperation::add}));

    const auto before = session.snapshot();
    const auto ambiguous = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectShortestPathAction{second_vertices.back().id});
    REQUIRE(!ambiguous);
    REQUIRE(ambiguous.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);

    const auto selected = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectShortestPathAction{
            second_vertices.back().id, second_object});
    REQUIRE(selected);
    const auto after = session.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.selection.component_groups.size() == 2U);
    const auto first_group = std::find_if(after.selection.component_groups.begin(),
        after.selection.component_groups.end(),
        [first_object](const auto& group) { return group.object == first_object; });
    const auto second_group = std::find_if(after.selection.component_groups.begin(),
        after.selection.component_groups.end(),
        [second_object](const auto& group) { return group.object == second_object; });
    REQUIRE(first_group != after.selection.component_groups.end());
    REQUIRE(second_group != after.selection.component_groups.end());
    REQUIRE(first_group->vertices ==
        std::vector<carto::geometry::VertexId>{first_vertices.front().id});
    REQUIRE(second_group->vertex_selection_order.front() == second_vertices[1].id);
    REQUIRE(second_group->vertex_selection_order.back() == second_vertices.back().id);

    const auto unknown_owner = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectShortestPathAction{
            second_vertices.back().id, carto::scene::ObjectId{999U}});
    REQUIRE(!unknown_owner);
    REQUIRE(unknown_owner.error().code == carto::core::ErrorCode::not_found);
    const auto after_unknown_owner = session.snapshot();
    REQUIRE(after_unknown_owner.project_revision == after.project_revision);
    REQUIRE(after_unknown_owner.selection.component_groups.size() == 2U);
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
        mesh_patch_serialization_is_canonical_and_bounded();
        face_deletion_patch_serialization_is_canonical_and_bounded();
        edge_split_patch_serialization_is_canonical_and_bounded();
        edge_dissolve_patch_serialization_is_canonical_and_bounded();
        merge_by_distance_patch_serialization_is_canonical_and_bounded();
        vertex_merge_patch_serialization_is_canonical_and_bounded();
        loop_cut_patch_serialization_is_canonical_and_bounded();
        knife_cut_patch_serialization_is_canonical_and_bounded();
        bevel_edge_patch_serialization_is_canonical_and_bounded();
        bevel_vertex_patch_serialization_is_canonical_and_bounded();
        face_extrusion_patch_serialization_is_canonical_and_bounded();
        face_inset_patch_serialization_is_canonical_and_bounded();
        face_poke_patch_serialization_is_canonical_and_bounded();
        tri_to_quad_patch_serialization_is_canonical_and_bounded();
        shared_vertex_position_patch_checks_all_incident_faces();
        face_delete_is_atomic_and_reports_removed_identity();
        face_delete_can_preserve_orphans_without_invalid_topology();
        face_delete_preserves_shared_vertices_when_removing_orphans();
        face_extrude_is_atomic_and_advances_once();
        edge_split_is_atomic_and_reports_topology_delta();
        edge_split_interpolates_corner_origins_without_flattening_uv_seams();
        edge_split_rejects_invalid_factors_without_mutation();
        quad_loop_cut_is_atomic_and_reports_full_lineage();
        knife_cut_splits_one_face_and_rejects_adversarial_paths_atomically();
        knife_cut_admits_edge_interior_endpoints_and_preserves_shared_boundary();
        knife_cut_attribute_transfer_uses_edge_parameters_and_preserves_uv_seams();
        edge_bevel_is_bounded_atomic_and_reports_lineage();
        edge_bevel_rejects_boundary_large_and_multisegment_inputs_without_mutation();
        edge_bevel_rejects_nonconvex_and_nonplanar_faces_without_mutation();
        vertex_bevel_is_bounded_atomic_and_reports_lineage();
        vertex_bevel_rejects_boundary_shape_and_numeric_inputs_without_mutation();
        vertex_bevel_supports_bounded_closed_triangular_fans();
        coplanar_internal_edge_dissolve_is_atomic_and_reports_lineage();
        edge_dissolve_rejects_boundary_and_nonplanar_joins_without_mutation();
        triangle_to_quad_requires_two_compatible_triangles();
        vertex_slide_is_bounded_and_atomic();
        target_weld_preserves_target_and_reports_merge_lineage();
        target_weld_rejects_ambiguous_collapses_without_mutation();
        center_weld_moves_target_to_midpoint_and_keeps_target_identity();
        center_weld_rejects_ambiguous_collapse_without_moving_target();
        merge_by_distance_stitches_coincident_seam_in_one_receipt();
        merge_by_distance_indexes_exact_corner_origins_for_many_sources();
        merge_by_distance_does_not_guess_ambiguous_corner_origins();
        merge_by_distance_is_not_transitive();
        merge_by_distance_preserves_extreme_finite_distance_boundaries();
        merge_by_distance_rejects_invalid_requests_without_mutation();
        component_selection_conversion_is_deterministic_and_non_mutating();
        component_selection_expansion_is_deterministic_and_non_mutating();
        selected_face_poke_batches_all_faces_as_one_undoable_edit();
        selected_face_extrusion_batches_adjacent_faces_as_one_undoable_edit();
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
        admitted_center_vertex_merge_moves_survivor_to_midpoint();
        admitted_last_selected_merge_preserves_selection_order_across_history();
        admitted_merge_by_distance_welds_selected_seam_and_is_undoable();
        admitted_edge_dissolve_is_undoable_and_clears_selection();
        selected_multi_edge_dissolve_commits_one_revision_and_is_undoable();
        admitted_triangle_to_quad_is_undoable_and_clears_selection();
        admitted_shortest_path_selection_is_non_mutating_and_stale_safe();
        admitted_owner_scoped_shortest_path_preserves_other_object_groups();
        admitted_vertex_slide_is_undoable_and_revision_bound();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
