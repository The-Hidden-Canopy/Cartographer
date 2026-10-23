// The editor command admission token is private to ApplicationSession and
// ToolContext. These tests construct editor commands directly, so the
// application header is pulled in with access specifiers exposed first, like
// the project/editor test access headers; this translation unit stays
// test-only and the exposure does not leak into other code.
#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "application_access.hpp"
#undef private
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif


#include <carto/attributes/set.hpp>
#include <carto/biological/authoring.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/assets/source_registry.hpp>
#include <carto/journal/journal.hpp>
#include <carto/editor/viewport_interaction.hpp>
#include <carto/project/presentation_catalog.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
                ("cartographer-application-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

carto::core::Result<carto::geometry::EditableMesh> make_application_tetrahedron() {
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

carto::core::Result<carto::geometry::EditableMesh> make_application_square_pyramid() {
    using namespace carto;
    geometry::EditableMesh mesh;
    const auto apex = mesh.add_vertex({0.0, 0.0, 1.0});
    const auto first = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto second = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto third = mesh.add_vertex({-1.0, 0.0, 0.0});
    const auto fourth = mesh.add_vertex({0.0, -1.0, 0.0});
    if (!apex) return core::Result<geometry::EditableMesh>::failure(apex.error());
    if (!first) return core::Result<geometry::EditableMesh>::failure(first.error());
    if (!second) return core::Result<geometry::EditableMesh>::failure(second.error());
    if (!third) return core::Result<geometry::EditableMesh>::failure(third.error());
    if (!fourth) return core::Result<geometry::EditableMesh>::failure(fourth.error());
    for (const auto& side : {
             std::vector<geometry::VertexId>{apex.value(), first.value(), second.value()},
             std::vector<geometry::VertexId>{apex.value(), second.value(), third.value()},
             std::vector<geometry::VertexId>{apex.value(), third.value(), fourth.value()},
             std::vector<geometry::VertexId>{apex.value(), fourth.value(), first.value()}}) {
        const auto face = mesh.add_face(side);
        if (!face) return core::Result<geometry::EditableMesh>::failure(face.error());
    }
    const auto base = mesh.add_face({
        first.value(), fourth.value(), third.value(), second.value()});
    if (!base) return core::Result<geometry::EditableMesh>::failure(base.error());
    if (auto result = mesh.validate(); !result) {
        return core::Result<geometry::EditableMesh>::failure(result.error());
    }
    return core::Result<geometry::EditableMesh>::success(std::move(mesh));
}

carto::core::Result<carto::geometry::EditableMesh> make_application_disconnected_tetrahedra(
    std::size_t count,
    bool append_open_triangle = false) {
    carto::geometry::EditableMesh mesh;
    for (std::size_t component = 0U; component < count; ++component) {
        const double offset = static_cast<double>(component) * 4.0;
        const auto v0 = mesh.add_vertex({offset, 0.0, 0.0});
        const auto v1 = mesh.add_vertex({offset + 2.0, 0.0, 0.0});
        const auto v2 = mesh.add_vertex({offset, 2.0, 0.0});
        const auto v3 = mesh.add_vertex({offset, 0.0, 2.0});
        if (!v0) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(v0.error());
        }
        if (!v1) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(v1.error());
        }
        if (!v2) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(v2.error());
        }
        if (!v3) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(v3.error());
        }
        const auto first = mesh.add_face({v0.value(), v2.value(), v1.value()});
        const auto second = mesh.add_face({v0.value(), v1.value(), v3.value()});
        const auto third = mesh.add_face({v0.value(), v3.value(), v2.value()});
        const auto fourth = mesh.add_face({v1.value(), v2.value(), v3.value()});
        if (!first) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(first.error());
        }
        if (!second) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(second.error());
        }
        if (!third) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(third.error());
        }
        if (!fourth) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(fourth.error());
        }
    }
    if (append_open_triangle) {
        const double offset = static_cast<double>(count) * 4.0;
        const auto a = mesh.add_vertex({offset, 0.0, 0.0});
        const auto b = mesh.add_vertex({offset + 1.0, 0.0, 0.0});
        const auto c = mesh.add_vertex({offset, 1.0, 0.0});
        if (!a) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(a.error());
        }
        if (!b) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(b.error());
        }
        if (!c) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(c.error());
        }
        const auto face = mesh.add_face({a.value(), b.value(), c.value()});
        if (!face) {
            return carto::core::Result<carto::geometry::EditableMesh>::failure(face.error());
        }
    }
    if (auto result = mesh.validate(); !result) {
        return carto::core::Result<carto::geometry::EditableMesh>::failure(result.error());
    }
    return carto::core::Result<carto::geometry::EditableMesh>::success(std::move(mesh));
}

struct ApplicationDissolveFixture {
    carto::geometry::EditableMesh mesh;
    carto::geometry::EdgeId internal_edge;
    carto::geometry::EdgeId boundary_edge;
};

ApplicationDissolveFixture make_application_dissolve_fixture() {
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
    const auto topology = mesh.topology();
    REQUIRE(topology);
    const auto internal = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [](const carto::geometry::EdgeRecord& edge) {
            return edge.second_half_edge.has_value();
        });
    const auto boundary = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [](const carto::geometry::EdgeRecord& edge) {
            return !edge.second_half_edge.has_value();
        });
    REQUIRE(internal != topology.value().edges.end());
    REQUIRE(boundary != topology.value().edges.end());
    return {std::move(mesh), internal->id, boundary->id};
}

struct ApplicationLoopCutFixture {
    carto::geometry::EditableMesh mesh;
    carto::geometry::EdgeId seed_edge;
};

ApplicationLoopCutFixture make_application_loop_cut_fixture() {
    carto::geometry::EditableMesh mesh;
    const auto top_left = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto top_middle = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto top_right = mesh.add_vertex({2.0, 1.0, 0.0});
    const auto bottom_left = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto bottom_middle = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto bottom_right = mesh.add_vertex({2.0, 0.0, 0.0});
    REQUIRE(top_left && top_middle && top_right && bottom_left && bottom_middle && bottom_right);
    REQUIRE(mesh.add_face({top_left.value(), top_middle.value(),
        bottom_middle.value(), bottom_left.value()}));
    REQUIRE(mesh.add_face({top_middle.value(), top_right.value(),
        bottom_right.value(), bottom_middle.value()}));
    REQUIRE(mesh.validate());
    const auto topology = mesh.topology();
    REQUIRE(topology);
    const auto first = std::min(top_left.value(), bottom_left.value());
    const auto second = std::max(top_left.value(), bottom_left.value());
    const auto seed = std::find_if(topology.value().edges.begin(), topology.value().edges.end(),
        [first, second](const auto& edge) { return edge.first == first && edge.second == second; });
    REQUIRE(seed != topology.value().edges.end());
    return {std::move(mesh), seed->id};
}

carto::geometry::EditableMesh make_application_weld_fixture() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    REQUIRE(mesh.add_face({b.value(), d.value(), c.value()}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_application_merge_distance_fixture() {
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
    REQUIRE(mesh.validate());
    return mesh;
}

struct ApplicationTriToQuadFixture {
    carto::geometry::EditableMesh mesh;
    carto::geometry::FaceId first_face;
    carto::geometry::FaceId second_face;
};

ApplicationTriToQuadFixture make_application_tri_to_quad_fixture(
    bool prepend_unrelated_triangle = false) {
    carto::geometry::EditableMesh mesh;
    if (prepend_unrelated_triangle) {
        const auto a = mesh.add_vertex({-3.0, 0.0, 0.0});
        const auto b = mesh.add_vertex({-2.0, 0.0, 0.0});
        const auto c = mesh.add_vertex({-3.0, 1.0, 0.0});
        REQUIRE(a && b && c);
        REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    }
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    const auto first = mesh.add_face({a.value(), b.value(), c.value()});
    const auto second = mesh.add_face({a.value(), c.value(), d.value()});
    REQUIRE(first && second);
    REQUIRE(mesh.validate());
    return {std::move(mesh), first.value(), second.value()};
}

ApplicationTriToQuadFixture make_application_disconnected_triangles_fixture() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({3.0, 0.0, 0.0});
    const auto e = mesh.add_vertex({4.0, 0.0, 0.0});
    const auto f = mesh.add_vertex({3.0, 1.0, 0.0});
    REQUIRE(a && b && c && d && e && f);
    const auto first = mesh.add_face({a.value(), b.value(), c.value()});
    const auto second = mesh.add_face({d.value(), e.value(), f.value()});
    REQUIRE(first && second);
    REQUIRE(mesh.validate());
    return {std::move(mesh), first.value(), second.value()};
}

std::unique_ptr<carto::editor::CreateMeshObjectCommand> make_test_mesh_object_command(
    carto::project::ProjectDocument& document,
    std::string object_name,
    carto::geometry::EditableMesh mesh) {
    carto::editor::ProjectCommandAdmission admission;
    return std::make_unique<carto::editor::CreateMeshObjectCommand>(
        admission, document, std::move(object_name), std::move(mesh));
}

std::unique_ptr<carto::editor::DuplicateProjectMeshInstanceBatchCommand>
make_test_mesh_instance_batch_command(
    carto::project::ProjectDocument& document,
    carto::scene::ObjectId source,
    std::vector<carto::editor::MeshInstanceBatchInstance> instances) {
    carto::editor::ProjectCommandAdmission admission;
    return std::make_unique<carto::editor::DuplicateProjectMeshInstanceBatchCommand>(
        admission, document, source, std::move(instances));
}

std::uintmax_t journal_prefix_bytes_through_record(
    const std::filesystem::path& journal_path,
    std::size_t records) {
    std::ifstream stream(journal_path, std::ios::binary);
    std::string line;
    std::uintmax_t bytes = 0U;
    for (std::size_t index = 0U; index <= records; ++index) {
        if (!std::getline(stream, line)) {
            throw TestFailure("journal fixture has fewer records than requested");
        }
        bytes += static_cast<std::uintmax_t>(line.size()) + 1U;
    }
    return bytes;
}

void application_mesh_geometry_digest_is_canonical_and_revision_independent() {
    auto mesh = make_application_tetrahedron();
    REQUIRE(mesh);
    const auto original_digest =
        carto::application::canonical_mesh_geometry_digest(mesh.value());
    auto edited = mesh.value();
    const auto vertex = edited.vertices_sorted().front();
    REQUIRE(edited.set_vertex_position(vertex.id, {vertex.position.x + 0.25,
        vertex.position.y, vertex.position.z}));
    REQUIRE(carto::application::canonical_mesh_geometry_digest(edited) != original_digest);
    REQUIRE(edited.set_vertex_position(vertex.id, vertex.position));
    REQUIRE(edited.revision() != mesh.value().revision());
    REQUIRE(carto::application::canonical_mesh_geometry_digest(edited) == original_digest);
}

void application_routes_authoring_through_snapshot_and_history() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Authoring"}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}}));

    auto snapshot = session.snapshot();
    REQUIRE(snapshot.project_name == "Authoring");
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.viewport.error == std::nullopt);
    REQUIRE(snapshot.viewport.scene.size() == 1U);
    const auto instances = snapshot.viewport.scene.instances();
    REQUIRE(instances.size() == 1U);
    REQUIRE(instances.front().mesh_asset == snapshot.objects.front().object.mesh_asset);
    REQUIRE(instances.front().mesh->vertex_ids.size() == 8U);
    REQUIRE(instances.front().mesh->triangle_faces.size() == 12U);
    REQUIRE(instances.front().mesh->vertex_ids.front() == carto::geometry::VertexId{1});
    REQUIRE(instances.front().mesh->triangle_faces.front() == carto::geometry::FaceId{1});

    const auto object = snapshot.objects.front().object.id;
    carto::core::Transform moved = snapshot.objects.front().object.local_transform;
    moved.translation = {1.0, 2.0, 3.0};
    const auto transform_source_revision = snapshot.project_revision;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetObjectTransformAction{
            object, moved, transform_source_revision}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 1.0);
    moved.translation.x = 9.0;
    const auto stale_transform = carto::application::testing::dispatch(session,
        carto::application::SetObjectTransformAction{
            object, moved, transform_source_revision});
    REQUIRE(!stale_transform &&
        stale_transform.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 1.0);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 0.0);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().objects.front().world_transform.translation.x == 1.0);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectObjectAction{object}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectFaceAction{
        object, carto::geometry::FaceId{1}}));

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.extrude-face", arguments}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.undo_count == 3U);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.viewport.error == std::nullopt);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().redo_count == 0U);
}

void application_synthetic_room_snap_gesture_commits_undoes_and_rejects_stale_source() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Synthetic Room Snap Fixture"}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Room prop", {1.0, 1.0, 1.0}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Thin wall target", {1.0, 2.0, 0.1}}));

    auto snapshot = session.snapshot();
    const auto find_object = [&snapshot](std::string_view name) {
        const auto found = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
            [name](const auto& object) { return object.object.name == name; });
        return found == snapshot.objects.end()
            ? carto::scene::ObjectId{} : found->object.id;
    };
    const auto moving_object = find_object("Room prop");
    const auto target_object = find_object("Thin wall target");
    REQUIRE(moving_object && target_object && moving_object != target_object);

    const auto target_view = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
        [target_object](const auto& object) { return object.object.id == target_object; });
    REQUIRE(target_view != snapshot.objects.end());
    auto target_transform = target_view->object.local_transform;
    target_transform.translation = {3.0, 0.0, 0.0};
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetObjectTransformAction{
            target_object, target_transform, snapshot.project_revision}));
    snapshot = session.snapshot();

    const auto moving_view = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
        [moving_object](const auto& object) { return object.object.id == moving_object; });
    REQUIRE(moving_view != snapshot.objects.end());
    constexpr double kPixelsPerMeter = 100.0;
    const carto::core::Vec2d pointer_start{100.0, 100.0};
    const carto::core::Vec2d pointer_end{345.0, 100.0};
    carto::editor::ViewportTransformSession gesture;
    REQUIRE(gesture.begin(moving_object, moving_view->object.local_transform,
        snapshot.project_revision, carto::editor::ViewportTranslateAxis::world_x,
        pointer_start, kPixelsPerMeter));
    const auto raw_preview = gesture.preview(pointer_end, snapshot.project_revision);
    REQUIRE(raw_preview && std::abs(raw_preview.value().translation.x - 2.45) < 1e-12);

    constexpr std::array<carto::editor::SnapKind, 3> snap_kinds{
        carto::editor::SnapKind::vertex,
        carto::editor::SnapKind::edge_midpoint,
        carto::editor::SnapKind::face_center};
    const auto candidate_probe = carto::editor::find_snap_candidates(
        snapshot.viewport.scene, raw_preview.value().translation, snap_kinds,
        snapshot.project_revision, 0.75);
    REQUIRE(candidate_probe);

    carto::editor::SnapSession snap_session;
    const auto snapped_preview = carto::editor::snap_viewport_transform_preview(
        raw_preview.value(), carto::editor::ViewportTranslateAxis::world_x,
        snapshot.viewport.scene, snapshot.project_revision, false, true, snap_session,
        moving_object);
    REQUIRE(snapped_preview && snapped_preview.value().candidate.has_value());
    REQUIRE(snapped_preview.value().candidate->object == target_object);
    REQUIRE(std::abs(snapped_preview.value().transform.translation.x - 2.5) < 1e-12);
    REQUIRE(snapped_preview.value().transform.translation.y ==
        moving_view->object.local_transform.translation.y);
    REQUIRE(snapped_preview.value().transform.translation.z ==
        moving_view->object.local_transform.translation.z);

    const auto completed = gesture.complete(pointer_end, snapshot.project_revision);
    REQUIRE(completed);
    const auto confirmed = carto::editor::snap_viewport_transform_preview(
        completed.value(), carto::editor::ViewportTranslateAxis::world_x,
        snapshot.viewport.scene, snapshot.project_revision, false, true, snap_session,
        moving_object);
    REQUIRE(confirmed && confirmed.value().candidate.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetObjectTransformAction{
            moving_object, confirmed.value().transform, snapshot.project_revision}));
    snapshot = session.snapshot();
    const auto moved = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
        [moving_object](const auto& object) { return object.object.id == moving_object; });
    REQUIRE(moved != snapshot.objects.end());
    REQUIRE(std::abs(moved->object.local_transform.translation.x - 2.5) < 1e-12);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    auto after_undo = session.snapshot();
    const auto undone = std::find_if(after_undo.objects.begin(), after_undo.objects.end(),
        [moving_object](const auto& object) { return object.object.id == moving_object; });
    REQUIRE(undone != after_undo.objects.end());
    REQUIRE(undone->object.local_transform.translation.x == 0.0);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    snapshot = session.snapshot();
    const auto redone = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
        [moving_object](const auto& object) { return object.object.id == moving_object; });
    REQUIRE(redone != snapshot.objects.end());
    REQUIRE(std::abs(redone->object.local_transform.translation.x - 2.5) < 1e-12);

    carto::editor::ViewportTransformSession stale_gesture;
    REQUIRE(stale_gesture.begin(moving_object, redone->object.local_transform,
        snapshot.project_revision, carto::editor::ViewportTranslateAxis::world_x,
        pointer_start, kPixelsPerMeter));
    target_transform.translation.x = 4.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetObjectTransformAction{
            target_object, target_transform, snapshot.project_revision}));
    const auto current_revision = session.snapshot().project_revision;
    const auto stale_preview = stale_gesture.preview(pointer_end, current_revision);
    REQUIRE(!stale_preview && stale_preview.error().code ==
        carto::core::ErrorCode::stale_data);
    const auto after_stale = session.snapshot();
    const auto unchanged = std::find_if(after_stale.objects.begin(), after_stale.objects.end(),
        [moving_object](const auto& object) { return object.object.id == moving_object; });
    REQUIRE(unchanged != after_stale.objects.end());
    REQUIRE(std::abs(unchanged->object.local_transform.translation.x - 2.5) < 1e-12);
}

void application_reuses_revision_keyed_viewport_meshes_without_cross_project_staleness() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Cache", {2.0, 2.0, 2.0}}));

    const auto first = session.snapshot();
    const auto first_mesh = first.viewport.scene.instances().front().mesh;
    const auto second = session.snapshot();
    REQUIRE(second.viewport.scene.instances().front().mesh.get() == first_mesh.get());

    const auto object = first.objects.front().object.id;
    carto::core::Transform moved = first.objects.front().object.local_transform;
    moved.translation.x = 3.0;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetObjectTransformAction{object, moved, std::nullopt}));
    const auto transformed = session.snapshot();
    REQUIRE(transformed.viewport.scene.instances().front().mesh.get() == first_mesh.get());

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectFaceAction{object, carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{"mesh.extrude-face", arguments}));
    const auto edited = session.snapshot();
    REQUIRE(edited.viewport.scene.instances().front().mesh.get() != first_mesh.get());
    REQUIRE(edited.viewport.scene.instances().front().mesh->vertex_ids.size() >
            first_mesh->vertex_ids.size());

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Replacement", true}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Replacement box", {1.0, 1.0, 1.0}}));
    const auto replacement = session.snapshot();
    REQUIRE(replacement.viewport.scene.instances().front().mesh.get() != first_mesh.get());
    REQUIRE(replacement.viewport.scene.instances().front().mesh.get() != edited.viewport.scene.instances().front().mesh.get());
}

void application_persists_project_topology_receipts_through_save_and_reopen() {
    TempDirectory temp;
    const auto path = temp.path() / "receipt-ledger.carto";

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Receipt persistence"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{object, carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.2;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.extrude-face", arguments}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    std::ifstream input(path, std::ios::binary);
    const std::string serialized(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(serialized.find("TOPOLOGY_RECEIPTS 1") != std::string::npos);
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().mesh_asset == 1U);

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 1U);
}

void application_persists_vertex_merge_lineage_through_save_and_reopen() {
    TempDirectory temp;
    const auto path = temp.path() / "merge-receipt.carto";

    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    REQUIRE(mesh.add_face({target.value(), source.value(), third.value()}));
    REQUIRE(mesh.add_face({source.value(), fourth.value(), third.value()}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateMeshObjectAction{"Weld", std::move(mesh)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, target.value()}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{
            object, source.value(), carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.merge-vertices", carto::editor::ToolArguments{}}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.merged_vertices.at(
                source.value()) == target.value());

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 3U);
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() == 1U);
}

void application_persists_center_vertex_merge_position_through_save_and_reopen() {
    TempDirectory temp;
    const auto path = temp.path() / "center-merge-receipt.carto";

    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({2.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    REQUIRE(mesh.add_face({target.value(), source.value(), third.value()}));
    REQUIRE(mesh.add_face({source.value(), fourth.value(), third.value()}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateMeshObjectAction{"Center Weld", std::move(mesh)}));
    const auto created_object = session.snapshot().objects.front().object;
    REQUIRE(created_object.mesh_asset.has_value());
    const auto object = created_object.id;
    const auto source_mesh = session.document_.meshes().at(*created_object.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, target.value()}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{
            object, source.value(), carto::editor::SelectionOperation::add}));
    carto::editor::ToolArguments arguments;
    arguments.merge_mode = carto::geometry::VertexMergeMode::center;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{"mesh.merge-vertices", arguments}));
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(session.document_.mesh_patches().front().replay(source_mesh));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().mesh_patches().size() == 1U);
    REQUIRE(loaded.value().mesh_patches().front().replay(source_mesh));
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.merged_vertices.at(
                source.value()) == target.value());
    REQUIRE(loaded.value().meshes().size() == 1U);
    const auto* survivor =
        loaded.value().meshes().begin()->second.find_vertex(target.value());
    REQUIRE(survivor != nullptr);
    REQUIRE(survivor->position.x == 1.0);
    REQUIRE(survivor->position.y == 0.0);
    REQUIRE(loaded.value().meshes().begin()->second.find_vertex(source.value()) == nullptr);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.document_.mesh_patches().size() == 1U);
    REQUIRE(reopened.document_.mesh_patches().front().replay(source_mesh));
}

void application_merges_uv_attributes_and_persists_payload_through_reopen() {
    TempDirectory temp;
    const auto path = temp.path() / "attribute-weld.carto";

    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    REQUIRE(mesh.add_face({target.value(), source.value(), third.value()}));
    const auto surviving_face = mesh.add_face({source.value(), fourth.value(), third.value()});
    REQUIRE(surviving_face);
    const auto topology = mesh.topology();
    REQUIRE(topology);

    auto vertices = mesh.vertices_sorted();
    auto corners = topology.value().corners;
    std::sort(corners.begin(), corners.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    std::vector<carto::attributes::AttributeValue> weights;
    weights.emplace_back(0.2F);
    weights.emplace_back(0.8F);
    weights.emplace_back(0.4F);
    weights.emplace_back(0.6F);
    std::vector<carto::attributes::AttributeValue> uvs;
    for (const auto& corner : corners) {
        const double face = static_cast<double>(corner.face.value) * 100.0;
        const double vertex = static_cast<double>(corner.vertex.value);
        uvs.emplace_back(carto::core::Vec2d{face + vertex, face - vertex});
    }
    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::vertex, vertices.size()},
        {carto::attributes::AttributeDomain::edge, topology.value().edges.size()},
        {carto::attributes::AttributeDomain::face, mesh.face_count()},
        {carto::attributes::AttributeDomain::corner, corners.size()}});
    REQUIRE(attributes.add_layer({
        {"weight", carto::attributes::AttributeDomain::vertex,
            carto::attributes::AttributeType::float32},
        std::move(weights)}));
    REQUIRE(attributes.add_layer({
        {"uv0", carto::attributes::AttributeDomain::corner,
            carto::attributes::AttributeType::vec2},
        std::move(uvs)}));
    carto::attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({1U, "UV0", "uv0", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateMeshObjectAction{"Attribute Weld", std::move(mesh)}));
    const auto created_object = session.snapshot().objects.front().object;
    REQUIRE(created_object.mesh_asset.has_value());
    const auto object = created_object.id;
    const auto source_mesh = session.document_.meshes().at(*created_object.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, target.value()}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{
            object, source.value(), carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.merge-vertices", carto::editor::ToolArguments{}}));
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(session.document_.mesh_patches().front().replay(source_mesh));
    const auto& edited_mesh = session.document_.meshes().begin()->second;
    REQUIRE(edited_mesh.validate());
    const auto* edited_uvs = edited_mesh.attributes().find("uv0");
    REQUIRE(edited_uvs != nullptr);
    const auto edited_topology = edited_mesh.topology();
    REQUIRE(edited_topology);
    auto edited_corners = edited_topology.value().corners;
    std::sort(edited_corners.begin(), edited_corners.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    const auto transferred_corner = std::find_if(
        edited_corners.begin(), edited_corners.end(), [&](const auto& corner) {
            return corner.face == surviving_face.value() && corner.vertex == target.value();
        });
    REQUIRE(transferred_corner != edited_corners.end());
    const auto edited_uv_index = static_cast<std::size_t>(
        std::distance(edited_corners.begin(), transferred_corner));
    const auto transferred_uv = std::get<carto::core::Vec2d>(
        edited_uvs->values.at(edited_uv_index));
    const double expected_u =
        static_cast<double>(surviving_face.value().value) * 100.0 +
        static_cast<double>(source.value().value);
    const double expected_v =
        static_cast<double>(surviving_face.value().value) * 100.0 -
        static_cast<double>(source.value().value);
    if (transferred_uv.x != expected_u || transferred_uv.y != expected_v) {
        std::cerr << "attribute weld transferred UV mismatch: got "
                  << transferred_uv.x << ',' << transferred_uv.y << '\n';
    }
    REQUIRE(transferred_uv.x == expected_u);
    REQUIRE(transferred_uv.y == expected_v);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::UndoAction{}));
    const auto redo_source = session.document_.meshes().at(*created_object.mesh_asset);
    REQUIRE(redo_source.vertex_count() == 4U);
    REQUIRE(redo_source.has_attribute_payload());
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.mesh_patches()[0].replay(source_mesh));
    REQUIRE(session.document_.mesh_patches()[1].replay(redo_source));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 2U);
    REQUIRE(loaded.value().mesh_patches().size() == 2U);
    REQUIRE(loaded.value().mesh_patches()[0].replay(source_mesh));
    REQUIRE(loaded.value().mesh_patches()[1].replay(redo_source));
    const auto& saved_mesh = loaded.value().meshes().begin()->second;
    REQUIRE(saved_mesh.validate());
    REQUIRE(saved_mesh.has_attribute_payload());
    const auto* saved_weights = saved_mesh.attributes().find("weight");
    const auto* saved_uvs = saved_mesh.attributes().find("uv0");
    REQUIRE(saved_weights != nullptr);
    REQUIRE(saved_uvs != nullptr);
    const auto saved_vertices = saved_mesh.vertices_sorted();
    const auto survivor_index = std::find_if(
        saved_vertices.begin(), saved_vertices.end(), [&](const auto& vertex) {
            return vertex.id == target.value();
        });
    REQUIRE(survivor_index != saved_vertices.end());
    const auto weight_index = static_cast<std::size_t>(
        std::distance(saved_vertices.begin(), survivor_index));
    REQUIRE(std::get<float>(saved_weights->values.at(weight_index)) == 0.5F);
    REQUIRE(saved_mesh.uv_sets().active_for_editing() != nullptr);
    REQUIRE(saved_mesh.uv_sets().active_for_editing()->layer_id == "uv0");
    const auto saved_topology = saved_mesh.topology();
    REQUIRE(saved_topology);
    auto saved_corners = saved_topology.value().corners;
    std::sort(saved_corners.begin(), saved_corners.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    const auto saved_transferred_corner = std::find_if(
        saved_corners.begin(), saved_corners.end(), [&](const auto& corner) {
            return corner.face == surviving_face.value() && corner.vertex == target.value();
        });
    REQUIRE(saved_transferred_corner != saved_corners.end());
    const auto saved_uv_index = static_cast<std::size_t>(
        std::distance(saved_corners.begin(), saved_transferred_corner));
    const auto saved_uv = std::get<carto::core::Vec2d>(saved_uvs->values.at(saved_uv_index));
    REQUIRE(saved_uv.x == expected_u);
    REQUIRE(saved_uv.y == expected_v);
}

void application_refuses_unmapped_edge_attributes_atomically_during_weld() {
    carto::geometry::EditableMesh mesh;
    const auto target = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto source = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto fourth = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(target && source && third && fourth);
    REQUIRE(mesh.add_face({target.value(), source.value(), third.value()}));
    REQUIRE(mesh.add_face({source.value(), fourth.value(), third.value()}));
    const auto topology = mesh.topology();
    REQUIRE(topology);
    const auto vertices_before = mesh.vertex_count();
    const auto revision_before = mesh.revision();

    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::vertex, mesh.vertex_count()},
        {carto::attributes::AttributeDomain::edge, topology.value().edges.size()},
        {carto::attributes::AttributeDomain::face, mesh.face_count()},
        {carto::attributes::AttributeDomain::corner, topology.value().corners.size()}});
    std::vector<carto::attributes::AttributeValue> edge_tags(
        topology.value().edges.size(), std::int32_t{7});
    REQUIRE(attributes.add_layer({
        {"edge_tag", carto::attributes::AttributeDomain::edge,
            carto::attributes::AttributeType::int32},
        std::move(edge_tags)}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), {}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateMeshObjectAction{"Unsupported Attribute Weld", mesh}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, target.value()}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{
            object, source.value(), carto::editor::SelectionOperation::add}));
    const auto project_revision = session.snapshot().project_revision;
    const auto rejected = carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.merge-vertices", carto::editor::ToolArguments{}});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == project_revision);
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() ==
            vertices_before);

    const auto direct_kernel_merge = mesh.merge_vertices(target.value(), source.value());
    REQUIRE(!direct_kernel_merge);
    REQUIRE(mesh.revision() == revision_before.next());
    REQUIRE(mesh.vertex_count() == vertices_before);
    REQUIRE(mesh.validate());
}

void application_routes_face_poke_through_history_and_persists_lineage() {
    TempDirectory temp;
    const auto path = temp.path() / "poke-receipt.carto";

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"Poke", 2.0, 2.0}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectFaceAction{object, carto::geometry::FaceId{1U}}));
    const auto poked = carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{
            "mesh.poke-face", carto::editor::ToolArguments{}});
    REQUIRE(poked);
    REQUIRE(session.snapshot().selection.faces.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 5U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.size() == 4U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    const auto& receipt = loaded.value().topology_receipts().front().receipt;
    REQUIRE(receipt.created_vertices.size() == 1U);
    REQUIRE(receipt.created_faces.size() == 4U);
    REQUIRE(receipt.removed_faces.size() == 1U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 5U);
}

void application_routes_edge_bevel_through_history_and_persistence() {
    TempDirectory temp;
    const auto path = temp.path() / "bevel-receipt.carto";

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Bevel", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto selected_edge = session.snapshot().viewport.scene.instances().front()
        .mesh->triangle_edges.front()[0];
    REQUIRE(selected_edge.has_value());
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectEdgeAction{object, *selected_edge}));

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.1;
    arguments.segments = 3U;
    const auto beveled = carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(beveled);
    REQUIRE(session.document_.topology_receipts().size() == 1U);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(beveled.value().parameters.has_value());
    REQUIRE(beveled.value().parameters->distance.has_value());
    REQUIRE(*beveled.value().parameters->distance == 0.1);
    REQUIRE(beveled.value().parameters->segments.has_value());
    REQUIRE(*beveled.value().parameters->segments == 3U);
    REQUIRE(session.snapshot().selection.edges.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 16U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.document_.topology_receipts().size() == 1U);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 8U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 16U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 2U);
    REQUIRE(loaded.value().mesh_patches().size() == 2U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.created_vertices.size() == 8U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.created_faces.size() == 3U);

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 16U);
}

void application_bevels_selected_edges_across_objects_atomically_and_persists_lineage() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-bevel-receipt.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"First bevel box", {1.0, 1.0, 1.0}}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Second bevel box", {1.0, 1.0, 1.0}}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value());
    REQUIRE(second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);
    const auto first_edges = first_source.edges_sorted(100U);
    const auto second_edges = second_source.edges_sorted(100U);
    REQUIRE(!first_edges.empty());
    REQUIRE(!second_edges.empty());
    const auto first_edge = first_edges.front().id;
    const auto second_edge = second_edges.back().id;
    REQUIRE(first_edge != second_edge);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{first.id, first_edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{second.id, second_edge,
            carto::editor::SelectionOperation::add}));
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.1;
    arguments.segments = 3U;
    const auto beveled = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(beveled);
    REQUIRE(beveled.value().affected.objects.size() == 2U);
    REQUIRE(beveled.value().affected.objects[0] == first.id);
    REQUIRE(beveled.value().affected.objects[1] == second.id);
    REQUIRE(beveled.value().affected.edges.size() == 2U);
    REQUIRE(beveled.value().affected.edges[0] == first_edge);
    REQUIRE(beveled.value().affected.edges[1] == second_edge);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() ==
        first_source.vertex_count() + 8U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() ==
        second_source.vertex_count() + 8U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto assert_receipt = [&](std::uint64_t mesh_asset, carto::geometry::EdgeId edge) {
        const auto& records = session.document_.topology_receipts();
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset](const auto& candidate) { return candidate.mesh_asset == mesh_asset; });
        REQUIRE(record != records.end());
        REQUIRE(record->receipt.removed_edges.size() == 1U);
        REQUIRE(record->receipt.removed_edges.front() == edge);
        REQUIRE(record->receipt.created_vertices.size() == 8U);
        REQUIRE(record->receipt.created_faces.size() == 3U);
    };
    const auto assert_patch = [](const std::vector<carto::project::MeshPatchRecord>& records,
                                 std::uint64_t mesh_asset,
                                 const carto::geometry::EditableMesh& source,
                                 carto::geometry::EdgeId edge,
                                 const carto::geometry::EditableMesh& actual) {
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset, source_revision = source.revision()](const auto& candidate) {
                if (candidate.mesh_asset != mesh_asset) return false;
                const auto decoded = carto::geometry::BevelEdgePatch::deserialize(
                    candidate.patch_bytes);
                return decoded && decoded.value().expected_revision == source_revision;
            });
        REQUIRE(record != records.end());
        REQUIRE(record->validate());
        REQUIRE(!record->patch_digest.is_zero());
        REQUIRE(!record->source_mesh_digest.is_zero());
        REQUIRE(!record->result_mesh_digest.is_zero());
        REQUIRE(record->source_mesh_digest != record->result_mesh_digest);
        const auto decoded = carto::geometry::BevelEdgePatch::deserialize(record->patch_bytes);
        REQUIRE(decoded);
        REQUIRE(decoded.value().edge == edge);
        REQUIRE(decoded.value().width == 0.1);
        REQUIRE(decoded.value().segments == 3U);
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        REQUIRE(replayed.value().revision() == actual.revision());
        REQUIRE(replayed.value().vertex_count() == actual.vertex_count());
        REQUIRE(replayed.value().face_count() == actual.face_count());
    };
    assert_receipt(*first.mesh_asset, first_edge);
    assert_receipt(*second.mesh_asset, second_edge);

    const auto first_beveled = session.document_.meshes().at(*first.mesh_asset);
    const auto second_beveled = session.document_.meshes().at(*second.mesh_asset);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_source,
        first_edge, first_beveled);
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_source,
        second_edge, second_beveled);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto assert_same_geometry = [](
        const carto::geometry::EditableMesh& actual,
        const carto::geometry::EditableMesh& expected) {
        const auto actual_vertices = actual.vertices_sorted();
        const auto expected_vertices = expected.vertices_sorted();
        REQUIRE(actual_vertices.size() == expected_vertices.size());
        for (std::size_t index = 0U; index < expected_vertices.size(); ++index) {
            REQUIRE(actual_vertices[index].id == expected_vertices[index].id);
            REQUIRE(actual_vertices[index].position.x == expected_vertices[index].position.x);
            REQUIRE(actual_vertices[index].position.y == expected_vertices[index].position.y);
            REQUIRE(actual_vertices[index].position.z == expected_vertices[index].position.z);
        }
        const auto actual_faces = actual.faces_sorted();
        const auto expected_faces = expected.faces_sorted();
        REQUIRE(actual_faces.size() == expected_faces.size());
        for (std::size_t index = 0U; index < expected_faces.size(); ++index) {
            REQUIRE(actual_faces[index].id == expected_faces[index].id);
            REQUIRE(actual_faces[index].vertices == expected_faces[index].vertices);
        }
    };
    assert_same_geometry(session.document_.meshes().at(*first.mesh_asset), first_source);
    assert_same_geometry(session.document_.meshes().at(*second.mesh_asset), second_source);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    assert_same_geometry(session.document_.meshes().at(*first.mesh_asset), first_beveled);
    assert_same_geometry(session.document_.meshes().at(*second.mesh_asset), second_beveled);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        first_edge, session.document_.meshes().at(*first.mesh_asset));
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        second_edge, session.document_.meshes().at(*second.mesh_asset));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 4U);
    REQUIRE(loaded.value().mesh_patches().size() == 4U);
    REQUIRE(loaded.value().meshes().at(*first.mesh_asset).vertex_count() ==
        first_beveled.vertex_count());
    REQUIRE(loaded.value().meshes().at(*second.mesh_asset).vertex_count() ==
        second_beveled.vertex_count());
    assert_patch(loaded.value().mesh_patches(), *first.mesh_asset, first_redo_source,
        first_edge, loaded.value().meshes().at(*first.mesh_asset));
    assert_patch(loaded.value().mesh_patches(), *second.mesh_asset, second_redo_source,
        second_edge, loaded.value().meshes().at(*second.mesh_asset));

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);
    REQUIRE(reopened.document_.topology_receipts().size() == 4U);
    REQUIRE(reopened.document_.mesh_patches().size() == 4U);
    REQUIRE(reopened.document_.meshes().at(*first.mesh_asset).vertex_count() ==
        first_beveled.vertex_count());
    REQUIRE(reopened.document_.meshes().at(*second.mesh_asset).vertex_count() ==
        second_beveled.vertex_count());
    assert_patch(reopened.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        first_edge, reopened.document_.meshes().at(*first.mesh_asset));
    assert_patch(reopened.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        second_edge, reopened.document_.meshes().at(*second.mesh_asset));
}

void application_rejects_multi_object_bevel_on_shared_mesh_assets_atomically() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Shared bevel mesh", {1.0, 1.0, 1.0}}));
    const auto source_object = session.snapshot().objects.front().object;
    REQUIRE(source_object.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source_object.id, "Shared bevel instance", {3.0, 0.0, 0.0}}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto instance = objects.back().object;
    REQUIRE(instance.mesh_asset == source_object.mesh_asset);
    const auto mesh_before = session.document_.meshes().at(*source_object.mesh_asset);
    const auto edge = mesh_before.edges_sorted(1U).front().id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{source_object.id, edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{instance.id, edge,
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.1;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source_object.mesh_asset).revision() ==
        mesh_before.revision());
    REQUIRE(session.document_.meshes().at(*source_object.mesh_asset).vertex_count() ==
        mesh_before.vertex_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_object_bevel_on_stale_or_invalid_second_owner_atomically() {
    carto::application::ApplicationSession stale_session;
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::CreateBoxAction{"First stale bevel box", {1.0, 1.0, 1.0}}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::CreateBoxAction{"Second stale bevel box", {1.0, 1.0, 1.0}}));
    const auto stale_objects = stale_session.snapshot().objects;
    const auto stale_first = stale_objects[0].object;
    const auto stale_second = stale_objects[1].object;
    REQUIRE(stale_first.mesh_asset.has_value() && stale_second.mesh_asset.has_value());
    const auto stale_first_before = stale_session.document_.meshes().at(*stale_first.mesh_asset);
    const auto stale_second_before = stale_session.document_.meshes().at(*stale_second.mesh_asset);
    const auto stale_first_edge = stale_first_before.edges_sorted(100U).front().id;
    const auto stale_second_edge = stale_second_before.edges_sorted(100U).back().id;
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SelectEdgeAction{stale_first.id, stale_first_edge}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SelectEdgeAction{stale_second.id, stale_second_edge,
            carto::editor::SelectionOperation::add}));
    REQUIRE(stale_session.document_.replace_mesh_if_revision(
        *stale_second.mesh_asset, stale_second_before.revision(), stale_second_before));
    const auto stale_before = stale_session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.1;
    const auto stale_rejected = carto::application::testing::dispatch(stale_session,
        carto::application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(!stale_rejected);
    REQUIRE(stale_rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(stale_session.snapshot().project_revision == stale_before.project_revision);
    REQUIRE(stale_session.snapshot().undo_count == stale_before.undo_count);
    REQUIRE(stale_session.document_.meshes().at(*stale_first.mesh_asset).revision() ==
        stale_first_before.revision());
    REQUIRE(stale_session.document_.meshes().at(*stale_first.mesh_asset).vertex_count() ==
        stale_first_before.vertex_count());
    REQUIRE(stale_session.document_.meshes().at(*stale_second.mesh_asset).revision() !=
        stale_second_before.revision());
    REQUIRE(stale_session.document_.topology_receipts().empty());
    REQUIRE(stale_session.document_.mesh_patches().empty());

    carto::application::ApplicationSession invalid_session;
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::CreateBoxAction{"Valid first bevel box", {1.0, 1.0, 1.0}}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::CreatePlaneAction{"Invalid second bevel plane", 1.0, 1.0}));
    const auto invalid_objects = invalid_session.snapshot().objects;
    const auto invalid_first = invalid_objects[0].object;
    const auto invalid_second = invalid_objects[1].object;
    REQUIRE(invalid_first.mesh_asset.has_value() && invalid_second.mesh_asset.has_value());
    REQUIRE(invalid_first.mesh_asset != invalid_second.mesh_asset);
    const auto valid_mesh_before = invalid_session.document_.meshes().at(*invalid_first.mesh_asset);
    const auto boundary_mesh_before = invalid_session.document_.meshes().at(*invalid_second.mesh_asset);
    const auto valid_edge = valid_mesh_before.edges_sorted(1U).front().id;
    const auto boundary_edge = boundary_mesh_before.edges_sorted(1U).front().id;
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SelectEdgeAction{invalid_first.id, valid_edge}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SelectEdgeAction{invalid_second.id, boundary_edge,
            carto::editor::SelectionOperation::add}));
    const auto invalid_before = invalid_session.snapshot();
    const auto invalid_rejected = carto::application::testing::dispatch(invalid_session,
        carto::application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(!invalid_rejected);
    REQUIRE(invalid_rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(invalid_session.snapshot().project_revision == invalid_before.project_revision);
    REQUIRE(invalid_session.snapshot().undo_count == invalid_before.undo_count);
    REQUIRE(invalid_session.document_.meshes().at(*invalid_first.mesh_asset).revision() ==
        valid_mesh_before.revision());
    REQUIRE(invalid_session.document_.meshes().at(*invalid_first.mesh_asset).vertex_count() ==
        valid_mesh_before.vertex_count());
    REQUIRE(invalid_session.document_.meshes().at(*invalid_second.mesh_asset).revision() ==
        boundary_mesh_before.revision());
    REQUIRE(invalid_session.document_.meshes().at(*invalid_second.mesh_asset).vertex_count() ==
        boundary_mesh_before.vertex_count());
    REQUIRE(invalid_session.document_.topology_receipts().empty());
    REQUIRE(invalid_session.document_.mesh_patches().empty());
}

void application_converts_triangle_pairs_across_objects_atomically_and_persists_lineage() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-triangle-to-quad.carto";
    auto first_fixture = make_application_tri_to_quad_fixture();
    auto second_fixture = make_application_tri_to_quad_fixture(true);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First triangle pair", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second triangle pair", second_fixture.mesh}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    REQUIRE(first_fixture.first_face != second_fixture.first_face);
    REQUIRE(first_fixture.second_face != second_fixture.second_face);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, first_fixture.first_face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, first_fixture.second_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, second_fixture.first_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, second_fixture.second_face,
            carto::editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    REQUIRE(selected.selection.component_groups[0].faces.size() == 2U);
    REQUIRE(selected.selection.component_groups[1].faces.size() == 2U);

    const auto converted = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.tri-to-quad", {}});
    if (!converted) {
        throw std::runtime_error("multi-object triangle-to-quad: " + converted.error().message);
    }
    REQUIRE(converted);
    REQUIRE(converted.value().affected.objects.size() == 2U);
    REQUIRE(converted.value().affected.objects[0] == first.id);
    REQUIRE(converted.value().affected.objects[1] == second.id);
    REQUIRE(converted.value().affected.faces.size() == 4U);
    REQUIRE(converted.value().affected.faces[0] == first_fixture.first_face);
    REQUIRE(converted.value().affected.faces[1] == first_fixture.second_face);
    REQUIRE(converted.value().affected.faces[2] == second_fixture.first_face);
    REQUIRE(converted.value().affected.faces[3] == second_fixture.second_face);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() == 1U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() == 2U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.mesh_patches()[0].replay(first_source));
    REQUIRE(session.document_.mesh_patches()[1].replay(second_source));
    const auto assert_receipt = [&](std::uint64_t mesh_asset,
                                    carto::geometry::FaceId first_face,
                                    carto::geometry::FaceId second_face) {
        const auto& records = session.document_.topology_receipts();
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset](const auto& candidate) { return candidate.mesh_asset == mesh_asset; });
        REQUIRE(record != records.end());
        REQUIRE(record->receipt.removed_faces.size() == 1U);
        REQUIRE(record->receipt.removed_faces.front() == first_face ||
            record->receipt.removed_faces.front() == second_face);
        REQUIRE(record->receipt.created_faces.empty());
    };
    assert_receipt(*first.mesh_asset, first_fixture.first_face, first_fixture.second_face);
    assert_receipt(*second.mesh_asset, second_fixture.first_face, second_fixture.second_face);
    const auto first_converted = session.document_.meshes().at(*first.mesh_asset);
    const auto second_converted = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto assert_same_faces = [](
        const carto::geometry::EditableMesh& actual,
        const carto::geometry::EditableMesh& expected) {
        const auto actual_faces = actual.faces_sorted(100U);
        const auto expected_faces = expected.faces_sorted(100U);
        REQUIRE(actual_faces.size() == expected_faces.size());
        for (std::size_t index = 0U; index < expected_faces.size(); ++index) {
            REQUIRE(actual_faces[index].id == expected_faces[index].id);
            REQUIRE(actual_faces[index].vertices == expected_faces[index].vertices);
        }
    };
    assert_same_faces(session.document_.meshes().at(*first.mesh_asset), first_source);
    assert_same_faces(session.document_.meshes().at(*second.mesh_asset), second_source);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    assert_same_faces(session.document_.meshes().at(*first.mesh_asset), first_converted);
    assert_same_faces(session.document_.meshes().at(*second.mesh_asset), second_converted);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    REQUIRE(session.document_.mesh_patches()[2].replay(first_redo_source));
    REQUIRE(session.document_.mesh_patches()[3].replay(second_redo_source));

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 4U);
    REQUIRE(loaded.value().mesh_patches().size() == 4U);
    REQUIRE(loaded.value().mesh_patches()[0].replay(first_source));
    REQUIRE(loaded.value().mesh_patches()[1].replay(second_source));
    REQUIRE(loaded.value().mesh_patches()[2].replay(first_redo_source));
    REQUIRE(loaded.value().mesh_patches()[3].replay(second_redo_source));
    REQUIRE(loaded.value().meshes().at(*first.mesh_asset).face_count() == 1U);
    REQUIRE(loaded.value().meshes().at(*second.mesh_asset).face_count() == 2U);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);
    REQUIRE(reopened.document_.topology_receipts().size() == 4U);
    REQUIRE(reopened.document_.mesh_patches().size() == 4U);
    REQUIRE(reopened.document_.meshes().at(*first.mesh_asset).face_count() == 1U);
    REQUIRE(reopened.document_.meshes().at(*second.mesh_asset).face_count() == 2U);
}

void application_refuses_multi_object_triangle_to_quad_on_shared_mesh_assets() {
    const auto fixture = make_application_tri_to_quad_fixture();
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Shared triangle pair", fixture.mesh}));
    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Shared triangle pair instance", {3.0, 0.0, 0.0}}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto instance = objects.back().object;
    REQUIRE(instance.mesh_asset == source.mesh_asset);
    const auto mesh_before = session.document_.meshes().at(*source.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{source.id, fixture.first_face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{source.id, fixture.second_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{instance.id, fixture.first_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{instance.id, fixture.second_face,
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    REQUIRE(before.selection.component_groups.size() == 2U);
    REQUIRE(before.selection.component_groups[0].faces.size() == 2U);
    REQUIRE(before.selection.component_groups[1].faces.size() == 2U);

    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.tri-to-quad", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).revision() == mesh_before.revision());
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).face_count() == 2U);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_object_triangle_to_quad_atomically_on_stale_or_incompatible_owner() {
    auto stale_first_fixture = make_application_tri_to_quad_fixture();
    auto stale_second_fixture = make_application_tri_to_quad_fixture(true);
    carto::application::ApplicationSession stale_session;
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::CreateMeshObjectAction{
            "First stale triangle pair", stale_first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::CreateMeshObjectAction{
            "Second stale triangle pair", stale_second_fixture.mesh}));
    const auto stale_objects = stale_session.snapshot().objects;
    const auto stale_first = stale_objects[0].object;
    const auto stale_second = stale_objects[1].object;
    REQUIRE(stale_first.mesh_asset.has_value() && stale_second.mesh_asset.has_value());
    const auto stale_first_before = stale_session.document_.meshes().at(*stale_first.mesh_asset);
    const auto stale_second_before = stale_session.document_.meshes().at(*stale_second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SelectFaceAction{stale_first.id, stale_first_fixture.first_face}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SelectFaceAction{stale_first.id, stale_first_fixture.second_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SelectFaceAction{stale_second.id, stale_second_fixture.first_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SelectFaceAction{stale_second.id, stale_second_fixture.second_face,
            carto::editor::SelectionOperation::add}));
    const auto stale_revision = stale_session.document_.replace_mesh_if_revision(
        *stale_second.mesh_asset, stale_second_before.revision(), stale_second_before);
    REQUIRE(stale_revision);
    const auto stale_before = stale_session.snapshot();
    const auto stale_rejected = carto::application::testing::dispatch(stale_session,
        carto::application::InvokeToolAction{"mesh.tri-to-quad", {}});
    REQUIRE(!stale_rejected);
    REQUIRE(stale_rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(stale_session.snapshot().project_revision == stale_before.project_revision);
    REQUIRE(stale_session.snapshot().undo_count == stale_before.undo_count);
    REQUIRE(stale_session.document_.meshes().at(*stale_first.mesh_asset).revision() ==
        stale_first_before.revision());
    REQUIRE(stale_session.document_.meshes().at(*stale_first.mesh_asset).face_count() == 2U);
    REQUIRE(stale_session.document_.meshes().at(*stale_second.mesh_asset).revision() ==
        stale_revision.value());
    REQUIRE(stale_session.document_.meshes().at(*stale_second.mesh_asset).face_count() == 3U);
    REQUIRE(stale_session.document_.topology_receipts().empty());

    auto valid_first_fixture = make_application_tri_to_quad_fixture();
    auto invalid_second_fixture = make_application_disconnected_triangles_fixture();
    carto::application::ApplicationSession invalid_session;
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::CreateMeshObjectAction{
            "Valid first triangle pair", valid_first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::CreateMeshObjectAction{
            "Disconnected later triangle pair", invalid_second_fixture.mesh}));
    const auto invalid_objects = invalid_session.snapshot().objects;
    const auto invalid_first = invalid_objects[0].object;
    const auto invalid_second = invalid_objects[1].object;
    REQUIRE(invalid_first.mesh_asset.has_value() && invalid_second.mesh_asset.has_value());
    const auto valid_mesh_before = invalid_session.document_.meshes().at(*invalid_first.mesh_asset);
    const auto disconnected_mesh_before =
        invalid_session.document_.meshes().at(*invalid_second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SelectFaceAction{invalid_first.id, valid_first_fixture.first_face}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SelectFaceAction{invalid_first.id, valid_first_fixture.second_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SelectFaceAction{invalid_second.id, invalid_second_fixture.first_face,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(invalid_session,
        carto::application::SelectFaceAction{invalid_second.id, invalid_second_fixture.second_face,
            carto::editor::SelectionOperation::add}));
    const auto invalid_before = invalid_session.snapshot();
    REQUIRE(invalid_before.selection.component_groups.size() == 2U);
    REQUIRE(invalid_before.selection.component_groups[0].faces.size() == 2U);
    REQUIRE(invalid_before.selection.component_groups[1].faces.size() == 2U);
    const auto invalid_rejected = carto::application::testing::dispatch(invalid_session,
        carto::application::InvokeToolAction{"mesh.tri-to-quad", {}});
    REQUIRE(!invalid_rejected);
    REQUIRE(invalid_rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(invalid_session.snapshot().project_revision == invalid_before.project_revision);
    REQUIRE(invalid_session.snapshot().undo_count == invalid_before.undo_count);
    REQUIRE(invalid_session.document_.meshes().at(*invalid_first.mesh_asset).revision() ==
        valid_mesh_before.revision());
    REQUIRE(invalid_session.document_.meshes().at(*invalid_first.mesh_asset).face_count() == 2U);
    REQUIRE(invalid_session.document_.meshes().at(*invalid_second.mesh_asset).revision() ==
        disconnected_mesh_before.revision());
    REQUIRE(invalid_session.document_.meshes().at(*invalid_second.mesh_asset).face_count() == 2U);
    REQUIRE(invalid_session.document_.topology_receipts().empty());
}

void application_insets_selected_faces_across_objects_atomically_and_persists_lineage() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-face-inset.carto";
    auto first_fixture = make_application_tri_to_quad_fixture();
    auto second_fixture = make_application_tri_to_quad_fixture(true);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First inset mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second inset mesh", second_fixture.mesh}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    REQUIRE(first_fixture.first_face != second_fixture.first_face);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, first_fixture.first_face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, second_fixture.first_face,
            carto::editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    REQUIRE(selected.selection.component_groups[0].faces.size() == 1U);
    REQUIRE(selected.selection.component_groups[1].faces.size() == 1U);

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.1;
    const auto inset = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.inset-face", arguments});
    REQUIRE(inset);
    REQUIRE(inset.value().affected.objects.size() == 2U);
    REQUIRE(inset.value().affected.objects[0] == first.id);
    REQUIRE(inset.value().affected.objects[1] == second.id);
    REQUIRE(inset.value().affected.faces.size() == 2U);
    REQUIRE(inset.value().affected.faces[0] == first_fixture.first_face);
    REQUIRE(inset.value().affected.faces[1] == second_fixture.first_face);
    REQUIRE(inset.value().parameters.has_value());
    REQUIRE(inset.value().parameters->distance.has_value());
    REQUIRE(*inset.value().parameters->distance == 0.1);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() == 5U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() == 6U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    const auto assert_receipt = [&](std::uint64_t mesh_asset, carto::geometry::FaceId face) {
        const auto& records = session.document_.topology_receipts();
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset](const auto& candidate) { return candidate.mesh_asset == mesh_asset; });
        REQUIRE(record != records.end());
        REQUIRE(record->receipt.removed_faces.size() == 1U);
        REQUIRE(record->receipt.removed_faces.front() == face);
        REQUIRE(record->receipt.created_vertices.size() == 3U);
        REQUIRE(record->receipt.created_faces.size() == 4U);
    };
    const auto assert_patch = [](
        const std::vector<carto::project::MeshPatchRecord>& records,
        std::uint64_t mesh_asset,
        const carto::geometry::EditableMesh& source,
        const carto::geometry::EditableMesh& actual,
        carto::geometry::FaceId face) {
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset, source_revision = source.revision()](const auto& candidate) {
                if (candidate.mesh_asset != mesh_asset) return false;
                const auto decoded = carto::geometry::FaceInsetPatch::deserialize(
                    candidate.patch_bytes);
                return decoded && decoded.value().expected_revision == source_revision;
            });
        REQUIRE(record != records.end());
        REQUIRE(record->validate());
        REQUIRE(!record->patch_digest.is_zero());
        REQUIRE(!record->source_mesh_digest.is_zero());
        REQUIRE(!record->result_mesh_digest.is_zero());
        REQUIRE(record->source_mesh_digest != record->result_mesh_digest);
        const auto patch = carto::geometry::FaceInsetPatch::deserialize(record->patch_bytes);
        REQUIRE(patch);
        REQUIRE(patch.value().expected_revision == source.revision());
        REQUIRE(patch.value().face == face);
        REQUIRE(patch.value().distance == 0.1);
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        REQUIRE(replayed.value().revision() == actual.revision());
        REQUIRE(replayed.value().vertex_count() == actual.vertex_count());
        REQUIRE(replayed.value().face_count() == actual.face_count());
    };
    assert_receipt(*first.mesh_asset, first_fixture.first_face);
    assert_receipt(*second.mesh_asset, second_fixture.first_face);
    const auto first_inset = session.document_.meshes().at(*first.mesh_asset);
    const auto second_inset = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_source,
        first_inset, first_fixture.first_face);
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_source,
        second_inset, second_fixture.first_face);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto assert_same_faces = [](
        const carto::geometry::EditableMesh& actual,
        const carto::geometry::EditableMesh& expected) {
        const auto actual_faces = actual.faces_sorted(100U);
        const auto expected_faces = expected.faces_sorted(100U);
        REQUIRE(actual_faces.size() == expected_faces.size());
        for (std::size_t index = 0U; index < expected_faces.size(); ++index) {
            REQUIRE(actual_faces[index].id == expected_faces[index].id);
            REQUIRE(actual_faces[index].vertices == expected_faces[index].vertices);
        }
    };
    assert_same_faces(session.document_.meshes().at(*first.mesh_asset), first_source);
    assert_same_faces(session.document_.meshes().at(*second.mesh_asset), second_source);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    assert_same_faces(session.document_.meshes().at(*first.mesh_asset), first_inset);
    assert_same_faces(session.document_.meshes().at(*second.mesh_asset), second_inset);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        session.document_.meshes().at(*first.mesh_asset), first_fixture.first_face);
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        session.document_.meshes().at(*second.mesh_asset), second_fixture.first_face);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 4U);
    REQUIRE(loaded.value().mesh_patches().size() == 4U);
    REQUIRE(loaded.value().meshes().at(*first.mesh_asset).face_count() == 5U);
    REQUIRE(loaded.value().meshes().at(*second.mesh_asset).face_count() == 6U);
    assert_patch(loaded.value().mesh_patches(), *first.mesh_asset, first_redo_source,
        loaded.value().meshes().at(*first.mesh_asset), first_fixture.first_face);
    assert_patch(loaded.value().mesh_patches(), *second.mesh_asset, second_redo_source,
        loaded.value().meshes().at(*second.mesh_asset), second_fixture.first_face);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);
    REQUIRE(reopened.document_.topology_receipts().size() == 4U);
    REQUIRE(reopened.document_.mesh_patches().size() == 4U);
    REQUIRE(reopened.document_.meshes().at(*first.mesh_asset).face_count() == 5U);
    REQUIRE(reopened.document_.meshes().at(*second.mesh_asset).face_count() == 6U);
    assert_patch(reopened.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        reopened.document_.meshes().at(*first.mesh_asset), first_fixture.first_face);
    assert_patch(reopened.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        reopened.document_.meshes().at(*second.mesh_asset), second_fixture.first_face);
}

void application_rejects_edge_bevel_wrong_mode_and_multi_selection() {
    carto::application::ApplicationSession wrong_mode;
    REQUIRE(carto::application::testing::dispatch(
        wrong_mode, carto::application::CreateBoxAction{"Wrong mode", {1.0, 1.0, 1.0}}));
    const auto wrong_object = wrong_mode.snapshot().objects.front().object.id;
    const auto wrong_before = wrong_mode.snapshot();
    REQUIRE(carto::application::testing::dispatch(
        wrong_mode, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(
        wrong_mode, carto::application::SelectFaceAction{
            wrong_object, carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments wrong_arguments;
    wrong_arguments.distance = 0.1;
    const auto wrong_result = carto::application::testing::dispatch(
        wrong_mode, carto::application::InvokeToolAction{
            "mesh.bevel-edge", wrong_arguments});
    REQUIRE(!wrong_result);
    REQUIRE(wrong_mode.snapshot().project_revision == wrong_before.project_revision);
    REQUIRE(wrong_mode.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 8U);

    carto::application::ApplicationSession multiple;
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::CreateBoxAction{"Multiple", {1.0, 1.0, 1.0}}));
    const auto multiple_snapshot = multiple.snapshot();
    const auto multiple_object = multiple_snapshot.objects.front().object.id;
    const auto triangle_edges = multiple_snapshot.viewport.scene.instances().front()
        .mesh->triangle_edges.front();
    REQUIRE(triangle_edges[0].has_value());
    REQUIRE(triangle_edges[1].has_value());
    REQUIRE(*triangle_edges[0] != *triangle_edges[1]);
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::SelectEdgeAction{
            multiple_object, *triangle_edges[0]}));
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::SelectEdgeAction{
            multiple_object, *triangle_edges[1], carto::editor::SelectionOperation::add}));
    const auto multiple_before = multiple.snapshot();
    carto::editor::ToolArguments multiple_arguments;
    multiple_arguments.distance = 0.1;
    const auto multiple_result = carto::application::testing::dispatch(
        multiple, carto::application::InvokeToolAction{
            "mesh.bevel-edge", multiple_arguments});
    REQUIRE(!multiple_result);
    REQUIRE(multiple.snapshot().project_revision == multiple_before.project_revision);
    REQUIRE(multiple.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 8U);
}

void application_routes_vertex_bevel_through_history_and_persistence() {
    TempDirectory temp;
    const auto path = temp.path() / "vertex-bevel-receipt.carto";
    auto tetrahedron = make_application_tetrahedron();
    REQUIRE(tetrahedron);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateMeshObjectAction{
            "Vertex bevel", std::move(tetrahedron.value())}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, carto::geometry::VertexId{1U}}));

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    arguments.segments = 3U;
    const auto beveled = carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(beveled);
    REQUIRE(session.document_.topology_receipts().size() == 1U);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(beveled.value().parameters.has_value());
    REQUIRE(beveled.value().parameters->distance.has_value());
    REQUIRE(*beveled.value().parameters->distance == 0.25);
    REQUIRE(beveled.value().parameters->segments.has_value());
    REQUIRE(*beveled.value().parameters->segments == 3U);
    REQUIRE(session.snapshot().selection.vertices.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 13U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.document_.topology_receipts().size() == 1U);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 13U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 2U);
    REQUIRE(loaded.value().mesh_patches().size() == 2U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.created_vertices.size() == 10U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.created_faces.size() == 18U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.removed_vertices.size() == 1U);

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 13U);
    REQUIRE(reopened.document_.topology_receipts().size() == 2U);
    REQUIRE(reopened.document_.mesh_patches().size() == 2U);
}

void application_bevels_selected_vertices_across_objects_atomically_and_persists_lineage() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-vertex-bevel.carto";
    auto first_fixture = make_application_tetrahedron();
    auto second_fixture = make_application_tetrahedron();
    REQUIRE(first_fixture && second_fixture);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First vertex bevel tetrahedron", std::move(first_fixture.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second vertex bevel tetrahedron", std::move(second_fixture.value())}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, carto::geometry::VertexId{1U},
            carto::editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    REQUIRE(selected.selection.component_groups[0].vertices.size() == 1U);
    REQUIRE(selected.selection.component_groups[1].vertices.size() == 1U);

    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    arguments.segments = 3U;
    const auto beveled = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(beveled);
    REQUIRE(beveled.value().affected.objects.size() == 2U);
    REQUIRE(beveled.value().affected.objects[0] == first.id);
    REQUIRE(beveled.value().affected.objects[1] == second.id);
    REQUIRE(beveled.value().affected.vertices.size() == 2U);
    REQUIRE(beveled.value().affected.vertices[0] == carto::geometry::VertexId{1U});
    REQUIRE(beveled.value().affected.vertices[1] == carto::geometry::VertexId{1U});
    REQUIRE(beveled.value().parameters.has_value());
    REQUIRE(beveled.value().parameters->distance.has_value());
    REQUIRE(*beveled.value().parameters->distance == 0.25);
    REQUIRE(beveled.value().parameters->segments.has_value());
    REQUIRE(*beveled.value().parameters->segments == 3U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 13U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 13U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto assert_receipt = [&](std::uint64_t mesh_asset) {
        const auto& records = session.document_.topology_receipts();
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset](const auto& candidate) { return candidate.mesh_asset == mesh_asset; });
        REQUIRE(record != records.end());
        REQUIRE(record->receipt.removed_vertices.size() == 1U);
        REQUIRE(record->receipt.removed_vertices.front() == carto::geometry::VertexId{1U});
        REQUIRE(record->receipt.created_vertices.size() == 10U);
        REQUIRE(record->receipt.created_faces.size() == 18U);
    };
    const auto assert_patch = [](const std::vector<carto::project::MeshPatchRecord>& records,
                                 std::uint64_t mesh_asset,
                                 const carto::geometry::EditableMesh& source,
                                 carto::geometry::VertexId vertex,
                                 const carto::geometry::EditableMesh& actual) {
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset, source_revision = source.revision()](const auto& candidate) {
                if (candidate.mesh_asset != mesh_asset) return false;
                const auto decoded = carto::geometry::BevelVertexPatch::deserialize(
                    candidate.patch_bytes);
                return decoded && decoded.value().expected_revision == source_revision;
            });
        REQUIRE(record != records.end());
        REQUIRE(record->validate());
        REQUIRE(!record->patch_digest.is_zero());
        REQUIRE(!record->source_mesh_digest.is_zero());
        REQUIRE(!record->result_mesh_digest.is_zero());
        REQUIRE(record->source_mesh_digest != record->result_mesh_digest);
        const auto decoded = carto::geometry::BevelVertexPatch::deserialize(record->patch_bytes);
        REQUIRE(decoded);
        REQUIRE(decoded.value().vertex == vertex);
        REQUIRE(decoded.value().width == 0.25);
        REQUIRE(decoded.value().segments == 3U);
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        REQUIRE(replayed.value().revision() == actual.revision());
        REQUIRE(replayed.value().vertex_count() == actual.vertex_count());
        REQUIRE(replayed.value().face_count() == actual.face_count());
    };
    assert_receipt(*first.mesh_asset);
    assert_receipt(*second.mesh_asset);
    const auto first_beveled = session.document_.meshes().at(*first.mesh_asset);
    const auto second_beveled = session.document_.meshes().at(*second.mesh_asset);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_source,
        carto::geometry::VertexId{1U}, first_beveled);
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_source,
        carto::geometry::VertexId{1U}, second_beveled);

    const auto assert_same_geometry = [](
        const carto::geometry::EditableMesh& actual,
        const carto::geometry::EditableMesh& expected) {
        const auto actual_vertices = actual.vertices_sorted();
        const auto expected_vertices = expected.vertices_sorted();
        REQUIRE(actual_vertices.size() == expected_vertices.size());
        for (std::size_t index = 0U; index < expected_vertices.size(); ++index) {
            REQUIRE(actual_vertices[index].id == expected_vertices[index].id);
            REQUIRE(actual_vertices[index].position.x == expected_vertices[index].position.x);
            REQUIRE(actual_vertices[index].position.y == expected_vertices[index].position.y);
            REQUIRE(actual_vertices[index].position.z == expected_vertices[index].position.z);
        }
        const auto actual_faces = actual.faces_sorted(100U);
        const auto expected_faces = expected.faces_sorted(100U);
        REQUIRE(actual_faces.size() == expected_faces.size());
        for (std::size_t index = 0U; index < expected_faces.size(); ++index) {
            REQUIRE(actual_faces[index].id == expected_faces[index].id);
            REQUIRE(actual_faces[index].vertices == expected_faces[index].vertices);
        }
    };
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    assert_same_geometry(session.document_.meshes().at(*first.mesh_asset), first_source);
    assert_same_geometry(session.document_.meshes().at(*second.mesh_asset), second_source);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    assert_same_geometry(session.document_.meshes().at(*first.mesh_asset), first_beveled);
    assert_same_geometry(session.document_.meshes().at(*second.mesh_asset), second_beveled);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        carto::geometry::VertexId{1U}, session.document_.meshes().at(*first.mesh_asset));
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        carto::geometry::VertexId{1U}, session.document_.meshes().at(*second.mesh_asset));

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 4U);
    REQUIRE(loaded.value().mesh_patches().size() == 4U);
    REQUIRE(loaded.value().meshes().at(*first.mesh_asset).vertex_count() == 13U);
    REQUIRE(loaded.value().meshes().at(*second.mesh_asset).vertex_count() == 13U);
    assert_patch(loaded.value().mesh_patches(), *first.mesh_asset, first_redo_source,
        carto::geometry::VertexId{1U}, loaded.value().meshes().at(*first.mesh_asset));
    assert_patch(loaded.value().mesh_patches(), *second.mesh_asset, second_redo_source,
        carto::geometry::VertexId{1U}, loaded.value().meshes().at(*second.mesh_asset));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);
    REQUIRE(reopened.document_.topology_receipts().size() == 4U);
    REQUIRE(reopened.document_.mesh_patches().size() == 4U);
    REQUIRE(reopened.document_.meshes().at(*first.mesh_asset).vertex_count() == 13U);
    REQUIRE(reopened.document_.meshes().at(*second.mesh_asset).vertex_count() == 13U);
    assert_patch(reopened.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        carto::geometry::VertexId{1U}, reopened.document_.meshes().at(*first.mesh_asset));
    assert_patch(reopened.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        carto::geometry::VertexId{1U}, reopened.document_.meshes().at(*second.mesh_asset));
}

void application_rejects_multi_object_vertex_bevel_on_shared_mesh_assets() {
    auto fixture = make_application_tetrahedron();
    REQUIRE(fixture);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Shared vertex bevel tetrahedron", std::move(fixture.value())}));
    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Shared vertex bevel instance", {3.0, 0.0, 0.0}}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto instance = objects.back().object;
    REQUIRE(instance.mesh_asset == source.mesh_asset);
    const auto mesh_before = session.document_.meshes().at(*source.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{source.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{instance.id, carto::geometry::VertexId{1U},
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).revision() == mesh_before.revision());
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).vertex_count() == 4U);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_single_selected_shared_vertex_bevel_atomically() {
    auto fixture = make_application_tetrahedron();
    REQUIRE(fixture);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Shared vertex bevel source", fixture.value()}));
    const auto source_object = session.snapshot().objects.front().object;
    REQUIRE(source_object.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source_object.id, "Unselected shared vertex bevel instance", {3.0, 0.0, 0.0}}));
    const auto instances = session.snapshot().objects;
    REQUIRE(instances.size() == 2U);
    REQUIRE(instances.front().object.mesh_asset == source_object.mesh_asset);
    const auto mesh_before = session.document_.meshes().at(*source_object.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{
            source_object.id, carto::geometry::VertexId{1U}}));
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source_object.mesh_asset).revision() ==
        mesh_before.revision());
    REQUIRE(session.document_.meshes().at(*source_object.mesh_asset).vertex_count() == 4U);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_object_vertex_bevel_on_stale_second_owner_atomically() {
    auto first_fixture = make_application_tetrahedron();
    auto second_fixture = make_application_tetrahedron();
    REQUIRE(first_fixture && second_fixture);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First stale vertex bevel tetrahedron", std::move(first_fixture.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second stale vertex bevel tetrahedron", std::move(second_fixture.value())}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_merge_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_merge_source = session.document_.meshes().at(*second.mesh_asset);
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, carto::geometry::VertexId{1U},
            carto::editor::SelectionOperation::add}));
    const auto stale_revision = session.document_.replace_mesh_if_revision(
        *second.mesh_asset, second_before.revision(), second_before);
    REQUIRE(stale_revision);
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 4U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == stale_revision.value());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 4U);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_object_vertex_bevel_atomically_on_later_open_mesh() {
    auto tetrahedron = make_application_tetrahedron();
    auto open_mesh = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(tetrahedron && open_mesh);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Valid first vertex bevel tetrahedron", std::move(tetrahedron.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Open later vertex bevel mesh", std::move(open_mesh.value())}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    const auto open_vertex = second_before.vertices_sorted().front().id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, open_vertex,
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() ==
        first_before.vertex_count());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == second_before.revision());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() ==
        second_before.vertex_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_vertex_bevel_wrong_mode_and_multi_selection() {
    auto tetrahedron = make_application_tetrahedron();
    REQUIRE(tetrahedron);
    carto::application::ApplicationSession wrong_mode;
    REQUIRE(carto::application::testing::dispatch(
        wrong_mode, carto::application::CreateMeshObjectAction{
            "Wrong vertex bevel mode", std::move(tetrahedron.value())}));
    const auto wrong_object = wrong_mode.snapshot().objects.front().object.id;
    const auto wrong_before = wrong_mode.snapshot();
    REQUIRE(carto::application::testing::dispatch(
        wrong_mode, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(
        wrong_mode, carto::application::SelectFaceAction{
            wrong_object, carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto wrong_result = carto::application::testing::dispatch(
        wrong_mode, carto::application::InvokeToolAction{
            "mesh.bevel-vertex", arguments});
    REQUIRE(!wrong_result);
    REQUIRE(wrong_mode.snapshot().project_revision == wrong_before.project_revision);
    REQUIRE(wrong_mode.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);

    auto multiple_tetrahedron = make_application_tetrahedron();
    REQUIRE(multiple_tetrahedron);
    carto::application::ApplicationSession multiple;
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::CreateMeshObjectAction{
            "Multiple vertex bevel", std::move(multiple_tetrahedron.value())}));
    const auto multiple_object = multiple.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::SelectVertexAction{
            multiple_object, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(
        multiple, carto::application::SelectVertexAction{
            multiple_object, carto::geometry::VertexId{2U},
            carto::editor::SelectionOperation::add}));
    const auto multiple_before = multiple.snapshot();
    const auto multiple_result = carto::application::testing::dispatch(
        multiple, carto::application::InvokeToolAction{
            "mesh.bevel-vertex", arguments});
    REQUIRE(!multiple_result);
    REQUIRE(multiple.snapshot().project_revision == multiple_before.project_revision);
    REQUIRE(multiple.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
}

void application_bevels_multiple_vertices_per_owner_with_replayable_history() {
    using namespace carto;
    TempDirectory temp;
    const auto path = temp.path() / "multi-vertex-bevel.carto";
    auto fixture = make_application_disconnected_tetrahedra(2U);
    REQUIRE(fixture);

    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Two disconnected tetrahedra", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_asset = *object.mesh_asset;
    const auto source = session.document_.meshes().at(mesh_asset);
    const std::array<geometry::VertexId, 2U> vertices{
        geometry::VertexId{1U}, geometry::VertexId{5U}};

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, vertices[1]}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, vertices[0], editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    REQUIRE(before.selection.component_groups.size() == 1U);
    REQUIRE(before.selection.component_groups.front().vertices.size() == vertices.size());

    editor::ToolArguments arguments;
    arguments.distance = 0.25;
    arguments.segments = 2U;
    const auto beveled = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(beveled);
    REQUIRE(beveled.value().affected.vertices.size() == vertices.size());
    REQUIRE(beveled.value().affected.vertices[0] == vertices[0]);
    REQUIRE(beveled.value().affected.vertices[1] == vertices[1]);
    REQUIRE(session.snapshot().undo_count == before.undo_count + 1U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_result = session.document_.meshes().at(mesh_asset);
    REQUIRE(first_result.vertex_count() > source.vertex_count());
    REQUIRE(first_result.face_count() > source.face_count());
    std::size_t receipt_vertex_count = source.vertex_count();
    for (const auto& record : session.document_.topology_receipts()) {
        if (record.mesh_asset != mesh_asset) continue;
        receipt_vertex_count += record.receipt.created_vertices.size();
        REQUIRE(record.receipt.removed_vertices.size() <= receipt_vertex_count);
        receipt_vertex_count -= record.receipt.removed_vertices.size();
    }
    REQUIRE(first_result.vertex_count() == receipt_vertex_count);

    const auto assert_patch_chain = [&](
        const std::vector<project::MeshPatchRecord>& records,
        const geometry::EditableMesh& initial,
        const geometry::EditableMesh& expected) {
        geometry::EditableMesh replay = initial;
        std::size_t applied = 0U;
        for (const auto& record : records) {
            if (record.mesh_asset != mesh_asset) continue;
            const auto patch = geometry::BevelVertexPatch::deserialize(record.patch_bytes);
            REQUIRE(patch);
            if (patch.value().expected_revision != replay.revision()) continue;
            REQUIRE(applied < vertices.size());
            REQUIRE(patch.value().vertex == vertices[applied]);
            REQUIRE(patch.value().width == 0.25);
            REQUIRE(patch.value().segments == 2U);
            REQUIRE(record.validate());
            const auto next = record.replay(replay);
            REQUIRE(next);
            replay = next.value();
            ++applied;
        }
        REQUIRE(applied == vertices.size());
        REQUIRE(replay.revision() == expected.revision());
        REQUIRE(replay.vertex_count() == expected.vertex_count());
        REQUIRE(replay.face_count() == expected.face_count());
        REQUIRE(replay.validate());
    };
    assert_patch_chain(session.document_.mesh_patches(), source, first_result);

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 2U);
    assert_patch_chain(reopened.value().mesh_patches(), source,
        reopened.value().meshes().at(mesh_asset));
    application::ApplicationSession reloaded;
    REQUIRE(application::testing::dispatch(reloaded,
        application::OpenProjectAction{path}));
    REQUIRE(reloaded.document_.mesh_patches().size() == 2U);
    REQUIRE(reloaded.document_.meshes().at(mesh_asset).vertex_count() ==
        first_result.vertex_count());

    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    const auto after_undo = session.document_.meshes().at(mesh_asset);
    REQUIRE(after_undo.vertex_count() == source.vertex_count());
    REQUIRE(after_undo.face_count() == source.face_count());
    const auto redo_source = after_undo;
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    const auto after_redo = session.document_.meshes().at(mesh_asset);
    REQUIRE(after_redo.vertex_count() == first_result.vertex_count());
    REQUIRE(after_redo.face_count() == first_result.face_count());
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    assert_patch_chain(session.document_.mesh_patches(), redo_source, after_redo);
    REQUIRE(session.document_.validate());

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 4U);
    assert_patch_chain(reopened.value().mesh_patches(), redo_source,
        reopened.value().meshes().at(mesh_asset));
}

void application_rejects_multi_vertex_bevel_after_late_boundary_vertex_atomically() {
    using namespace carto;
    auto fixture = make_application_disconnected_tetrahedra(1U, true);
    REQUIRE(fixture);
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Closed and open components", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_asset = *object.mesh_asset;
    const auto source = session.document_.meshes().at(mesh_asset);

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, geometry::VertexId{1U}}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, geometry::VertexId{5U},
            editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.snapshot().redo_count == before.redo_count);
    REQUIRE(session.document_.meshes().at(mesh_asset).revision() == source.revision());
    REQUIRE(session.document_.meshes().at(mesh_asset).vertex_count() == source.vertex_count());
    REQUIRE(session.document_.meshes().at(mesh_asset).face_count() == source.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_vertex_bevel_over_budget_before_mutation() {
    using namespace carto;
    constexpr std::size_t kSelectedVertices = 65U;
    geometry::EditableMesh fixture;
    constexpr std::size_t kTriangleCount = 22U;
    for (std::size_t index = 0U; index < kTriangleCount; ++index) {
        const double x = static_cast<double>(index) * 2.0;
        const auto a = fixture.add_vertex({x, 0.0, 0.0});
        const auto b = fixture.add_vertex({x + 1.0, 0.0, 0.0});
        const auto c = fixture.add_vertex({x, 1.0, 0.0});
        REQUIRE(a && b && c);
        REQUIRE(fixture.add_face({a.value(), b.value(), c.value()}));
    }
    REQUIRE(fixture.validate());
    const auto vertices = fixture.vertices_sorted();
    REQUIRE(vertices.size() == 66U);
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Over-budget disconnected triangles", fixture}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_asset = *object.mesh_asset;
    const auto source = session.document_.meshes().at(mesh_asset);
    const auto& bound_mesh = session.document_.meshes().at(mesh_asset);
    REQUIRE(session.selection_.set_mode(editor::SelectionMode::vertex));
    for (std::size_t index = 0U; index < kSelectedVertices; ++index) {
        const auto operation = index == 0U
            ? editor::SelectionOperation::replace
            : editor::SelectionOperation::add;
        REQUIRE(session.selection_.select_vertex(
            session.document_.scene(), object.id, bound_mesh, vertices[index].id, operation));
    }
    const auto before = session.snapshot();
    REQUIRE(before.selection.component_groups.front().vertices.size() == kSelectedVertices);
    editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(mesh_asset).revision() == source.revision());
    REQUIRE(session.document_.meshes().at(mesh_asset).vertex_count() == source.vertex_count());
    REQUIRE(session.document_.meshes().at(mesh_asset).face_count() == source.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_routes_imported_mesh_through_the_durable_command_boundary() {
    TempDirectory temp;
    auto mesh = carto::geometry::make_plane(2.0, 3.0);
    REQUIRE(mesh);

    const auto path = temp.path() / "imported.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Imported"}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateMeshObjectAction{
        "Imported mesh", std::move(mesh.value())}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{std::nullopt}));

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.viewport.error == std::nullopt);

    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 2U);
    REQUIRE(entries.value().back().event_type == "cartographer.application_action");
    const std::string payload(
        entries.value().back().payload.begin(), entries.value().back().payload.end());
    REQUIRE(payload.find("operation=Create Mesh Object") != std::string::npos);
    REQUIRE(journal.verify());
}

void application_duplicates_mesh_instances_through_durable_history() {
    TempDirectory temp;
    const auto path = temp.path() / "instanced-module.carto";

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Instanced module"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Module", {2.0, 2.0, 2.0}}));

    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    const auto before_rejections = session.snapshot();
    REQUIRE(!carto::application::testing::dispatch(
        session,
        carto::application::DuplicateMeshInstanceAction{
            carto::scene::ObjectId{999U}, "Missing", {1.0, 0.0, 0.0}}));
    REQUIRE(!carto::application::testing::dispatch(
        session,
        carto::application::DuplicateMeshInstanceAction{
            source.id,
            "Non-finite",
            {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}}));
    REQUIRE(session.snapshot().project_revision == before_rejections.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejections.undo_count);
    REQUIRE(session.snapshot().objects.size() == before_rejections.objects.size());

    const auto duplicated = carto::application::testing::dispatch(
        session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Module east", {4.0, 0.0, -1.0}});
    REQUIRE(duplicated);
    REQUIRE(duplicated.value().action == "Duplicate Mesh Instance");
    REQUIRE(duplicated.value().document_changed);
    REQUIRE(duplicated.value().affected.objects.size() == 1U);
    REQUIRE(duplicated.value().affected.objects.front() == source.id);
    REQUIRE(duplicated.value().parameters.has_value());
    REQUIRE(duplicated.value().parameters->position.has_value());
    REQUIRE(duplicated.value().parameters->position->x == 4.0);
    REQUIRE(duplicated.value().parameters->position->y == 0.0);
    REQUIRE(duplicated.value().parameters->position->z == -1.0);

    auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 2U);
    REQUIRE(snapshot.viewport.scene.size() == 2U);
    const auto duplicate_iterator = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(), [](const auto& view) {
            return view.object.name == "Module east";
        });
    REQUIRE(duplicate_iterator != snapshot.objects.end());
    const auto duplicate_id = duplicate_iterator->object.id;
    REQUIRE(duplicate_iterator->object.mesh_asset == source.mesh_asset);
    REQUIRE(duplicate_iterator->object.local_transform.translation.x == 4.0);
    REQUIRE(duplicate_iterator->object.local_transform.translation.y == 0.0);
    REQUIRE(duplicate_iterator->object.local_transform.translation.z == -1.0);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{std::nullopt}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().scene().size() == 2U);
    REQUIRE(loaded.value().meshes().size() == 1U);
    REQUIRE(loaded.value().scene().find(source.id));
    REQUIRE(loaded.value().scene().find(duplicate_id));
    REQUIRE(loaded.value().scene().find(duplicate_id)->mesh_asset == source.mesh_asset);

    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 3U);
    const std::string payload(
        entries.value().back().payload.begin(), entries.value().back().payload.end());
    REQUIRE(payload.find("operation=Duplicate Mesh Instance") != std::string::npos);
    REQUIRE(journal.verify());

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.size() == 1U);
    REQUIRE(session.snapshot().viewport.scene.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 2U);
    const auto restored = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(), [duplicate_id](const auto& view) {
            return view.object.id == duplicate_id;
        });
    REQUIRE(restored != snapshot.objects.end());
    REQUIRE(restored->object.mesh_asset == source.mesh_asset);
}

void application_duplicates_mesh_instance_batches_through_durable_history() {
    TempDirectory temp;
    const auto path = temp.path() / "instanced-batch.carto";

    auto document = carto::project::ProjectDocument::create("Instanced batch");
    REQUIRE(document);
    auto primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(primitive);

    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(make_test_mesh_object_command(
        document.value(), "Module", std::move(primitive.value()))));
    const auto source_id = document.value().scene().objects_sorted().front().id;
    const auto source = *document.value().scene().find(source_id);
    REQUIRE(source.mesh_asset.has_value());
    const auto shared_mesh_asset = *source.mesh_asset;

    const std::vector<carto::core::Vec3d> offsets{{4.0, 0.0, -1.0}, {0.0, 5.0, 2.5}};
    std::vector<carto::editor::MeshInstanceBatchInstance> placements;
    placements.push_back({"Module east", offsets.front()});
    placements.push_back({"Module north", offsets.back()});

    auto batch = make_test_mesh_instance_batch_command(
        document.value(), source_id, std::move(placements));
    REQUIRE(batch->label() == "Duplicate Mesh Instance Batch");
    REQUIRE(bus.execute(std::move(batch)));
    REQUIRE(bus.undo_count() == 2U);
    REQUIRE(document.value().scene().size() == 3U);
    REQUIRE(document.value().meshes().size() == 1U);

    const auto objects = document.value().scene().objects_sorted();
    const auto east = std::find_if(objects.begin(), objects.end(), [](const auto& object) {
        return object.name == "Module east";
    });
    const auto north = std::find_if(objects.begin(), objects.end(), [](const auto& object) {
        return object.name == "Module north";
    });
    REQUIRE(east != objects.end() && north != objects.end());
    const auto east_id = east->id;
    const auto north_id = north->id;
    REQUIRE(east_id != north_id);
    REQUIRE(east_id != source_id && north_id != source_id);
    REQUIRE(east_id.value < north_id.value);
    REQUIRE(east->mesh_asset.has_value() && north->mesh_asset.has_value());
    REQUIRE(*east->mesh_asset == shared_mesh_asset);
    REQUIRE(*north->mesh_asset == shared_mesh_asset);
    REQUIRE(!east->parent.has_value() && !north->parent.has_value());
    REQUIRE(east->visible && north->visible && !east->locked && !north->locked);
    REQUIRE(east->local_transform.translation.x ==
            source.local_transform.translation.x + offsets.front().x);
    REQUIRE(east->local_transform.translation.y ==
            source.local_transform.translation.y + offsets.front().y);
    REQUIRE(east->local_transform.translation.z ==
            source.local_transform.translation.z + offsets.front().z);
    REQUIRE(north->local_transform.translation.x ==
            source.local_transform.translation.x + offsets.back().x);
    REQUIRE(north->local_transform.translation.y ==
            source.local_transform.translation.y + offsets.back().y);
    REQUIRE(north->local_transform.translation.z ==
            source.local_transform.translation.z + offsets.back().z);
    REQUIRE(east->local_transform.rotation.w == source.local_transform.rotation.w);
    REQUIRE(east->local_transform.scale.x == source.local_transform.scale.x);
    const auto* source_after = document.value().scene().find(source_id);
    REQUIRE(source_after);
    REQUIRE(source_after->mesh_asset == source.mesh_asset);
    REQUIRE(source_after->local_transform.translation.x ==
            source.local_transform.translation.x);
    REQUIRE(document.value().validate());

    REQUIRE(bus.undo());
    REQUIRE(bus.undo_count() == 1U);
    REQUIRE(document.value().scene().size() == 1U);
    REQUIRE(document.value().meshes().size() == 1U);
    REQUIRE(document.value().scene().find(source_id));
    REQUIRE(*document.value().scene().find(source_id)->mesh_asset == shared_mesh_asset);
    REQUIRE(document.value().validate());
    REQUIRE(bus.redo());
    REQUIRE(document.value().scene().size() == 3U);
    const auto* east_restored = document.value().scene().find(east_id);
    const auto* north_restored = document.value().scene().find(north_id);
    REQUIRE(east_restored && north_restored);
    REQUIRE(*east_restored->mesh_asset == shared_mesh_asset);
    REQUIRE(*north_restored->mesh_asset == shared_mesh_asset);
    REQUIRE(east_restored->local_transform.translation.x ==
            source.local_transform.translation.x + offsets.front().x);
    REQUIRE(north_restored->local_transform.translation.y ==
            source.local_transform.translation.y + offsets.back().y);
    REQUIRE(document.value().validate());

    const auto persisted = carto::project::ProjectDocument::deserialize(
        document.value().serialize());
    REQUIRE(persisted);
    REQUIRE(persisted.value().scene().size() == 3U);
    REQUIRE(persisted.value().meshes().size() == 1U);
    const auto* persisted_east = persisted.value().scene().find(east_id);
    REQUIRE(persisted_east);
    REQUIRE(persisted_east->mesh_asset == east->mesh_asset);
    REQUIRE(persisted.value().validate());

    const auto saved = document.value().save_atomic(path);
    REQUIRE(saved);
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().scene().size() == 3U);
    REQUIRE(loaded.value().meshes().size() == 1U);
    const auto* loaded_east = loaded.value().scene().find(east_id);
    const auto* loaded_north = loaded.value().scene().find(north_id);
    REQUIRE(loaded_east && loaded_north);
    REQUIRE(*loaded_east->mesh_asset == shared_mesh_asset);
    REQUIRE(*loaded_north->mesh_asset == shared_mesh_asset);
    REQUIRE(loaded_east->local_transform.translation.z ==
            source.local_transform.translation.z + offsets.front().z);
    REQUIRE(loaded_north->local_transform.translation.y ==
            source.local_transform.translation.y + offsets.back().y);
    REQUIRE(loaded.value().validate());

    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const std::string payload_text = "operation=Duplicate Mesh Instance Batch";
    const auto* payload_bytes = reinterpret_cast<const std::uint8_t*>(payload_text.data());
    const auto appended = journal.append(carto::journal::JournalAppend{
        carto::core::Revision{},
        carto::core::Revision{}.next(),
        "cartographer.application_action",
        std::span<const std::uint8_t>(payload_bytes, payload_text.size()),
        std::nullopt,
    });
    REQUIRE(appended);
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);
    REQUIRE(entries.value().back().event_type == "cartographer.application_action");
    const std::string payload(
        entries.value().back().payload.begin(), entries.value().back().payload.end());
    REQUIRE(payload.find("operation=Duplicate Mesh Instance Batch") != std::string::npos);
    REQUIRE(journal.verify());
}

void application_rejects_invalid_mesh_instance_batches() {
    auto document = carto::project::ProjectDocument::create("Rejected batches");
    REQUIRE(document);
    auto primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(primitive);
    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(make_test_mesh_object_command(
        document.value(), "Module", std::move(primitive.value()))));
    const auto source_id = document.value().scene().objects_sorted().front().id;
    const auto before = document.value().serialize();

    const auto expect_rejected =
        [&document, &before](
            std::vector<carto::editor::MeshInstanceBatchInstance> instances,
            carto::scene::ObjectId source) {
            carto::editor::CommandBus rejected_bus;
            REQUIRE(!rejected_bus.execute(make_test_mesh_instance_batch_command(
                document.value(), source, std::move(instances))));
            REQUIRE(rejected_bus.undo_count() == 0U);
            REQUIRE(document.value().serialize() == before);
        };

    expect_rejected({}, source_id);
    expect_rejected({{"Copy", {1.0, 0.0, 0.0}}}, carto::scene::ObjectId{0U});
    expect_rejected({{"Copy", {1.0, 0.0, 0.0}}}, carto::scene::ObjectId{999U});
    expect_rejected(
        {{"Copy", {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}}}, source_id);
    expect_rejected(
        {{"Copy", {std::numeric_limits<double>::infinity(), 0.0, 0.0}}}, source_id);
    expect_rejected(
        {{"Copy",
          {carto::editor::kMaxMeshInstanceBatchOffsetMagnitude + 1.0, 0.0, 0.0}}},
        source_id);
    expect_rejected({{"", {1.0, 0.0, 0.0}}}, source_id);
    expect_rejected(
        {{"First", {1.0, 0.0, 0.0}},
         {"Second", {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}}},
        source_id);
    std::vector<carto::editor::MeshInstanceBatchInstance> overflow(
        carto::editor::kMaxMeshInstanceBatchInstances + 1U, {"Copy", {1.0, 0.0, 0.0}});
    expect_rejected(std::move(overflow), source_id);

    const auto expect_source_rejected = [](
                                            bool visible,
                                            bool locked,
                                            bool parented,
                                            bool has_mesh) {
        auto rejected_document = carto::project::ProjectDocument::create(
            "Rejected batch source");
        REQUIRE(rejected_document);
        std::optional<carto::scene::ObjectId> parent;
        if (parented) {
            const auto parent_id = rejected_document.value().create_object("Parent");
            REQUIRE(parent_id);
            parent = parent_id.value();
        }
        std::optional<std::uint64_t> mesh_asset;
        if (has_mesh) {
            auto plane = carto::geometry::make_plane(1.0, 1.0);
            REQUIRE(plane);
            const auto asset = rejected_document.value().add_mesh(std::move(plane.value()));
            REQUIRE(asset);
            mesh_asset = asset.value();
        }
        const carto::scene::ObjectId rejected_source_id{parented ? 2U : 1U};
        REQUIRE(rejected_document.value().insert_object(carto::scene::SceneObject{
            rejected_source_id,
            "Rejected source",
            carto::core::Transform::identity(),
            parent,
            mesh_asset,
            visible,
            locked}));
        carto::editor::CommandBus rejected_bus;
        REQUIRE(!rejected_bus.execute(make_test_mesh_instance_batch_command(
            rejected_document.value(),
            rejected_source_id,
            {{"Rejected copy", {1.0, 0.0, 0.0}}})));
        REQUIRE(rejected_bus.undo_count() == 0U);
        REQUIRE(rejected_document.value().scene().size() == (parented ? 2U : 1U));
    };
    expect_source_rejected(false, false, false, true);
    expect_source_rejected(true, true, false, true);
    expect_source_rejected(true, false, true, true);
    expect_source_rejected(true, false, false, false);
}

void application_routes_vertex_edit_and_rejects_stale_reselection() {
    TempDirectory temp;
    const auto path = temp.path() / "vertex-patch.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Box", {2.0, 2.0, 2.0}}));
    auto snapshot = session.snapshot();
    const auto object = snapshot.objects.front().object.id;
    const auto mesh_asset = snapshot.objects.front().object.mesh_asset;
    REQUIRE(mesh_asset.has_value());
    const auto source_mesh = session.document_.meshes().at(*mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1}}));

    const auto before_instances = session.snapshot().viewport.scene.instances();
    REQUIRE(before_instances.size() == 1U);
    const auto before_mesh = before_instances.front().mesh;
    const auto before_vertex = std::find(
        before_mesh->vertex_ids.begin(), before_mesh->vertex_ids.end(),
        carto::geometry::VertexId{1});
    REQUIRE(before_vertex != before_mesh->vertex_ids.end());
    const auto before_index = static_cast<std::size_t>(
        before_vertex - before_mesh->vertex_ids.begin());
    const auto target = before_mesh->positions[before_index] + carto::core::Vec3d{0.1, 0.0, 0.0};

    carto::editor::ToolArguments arguments;
    arguments.position = target;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.undo_count == 2U);
    const auto after_instances = snapshot.viewport.scene.instances();
    const auto after_vertex = std::find(
        after_instances.front().mesh->vertex_ids.begin(),
        after_instances.front().mesh->vertex_ids.end(),
        carto::geometry::VertexId{1});
    REQUIRE(after_vertex != after_instances.front().mesh->vertex_ids.end());
    const auto after_index = static_cast<std::size_t>(
        after_vertex - after_instances.front().mesh->vertex_ids.begin());
    REQUIRE(after_instances.front().mesh->positions[after_index].x == target.x);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded && loaded.value().mesh_patches().size() == 1U);
    const auto replayed = loaded.value().mesh_patches().front().replay(source_mesh);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() ==
            loaded.value().meshes().at(*mesh_asset).revision());

    auto stale_source = source_mesh;
    const auto unrelated_vertex = stale_source.vertices_sorted().back();
    REQUIRE(stale_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + carto::core::Vec3d{0.01, 0.0, 0.0}));
    REQUIRE(stale_source.restore_revision(source_mesh.revision()));
    const auto stale_replay = loaded.value().mesh_patches().front().replay(stale_source);
    REQUIRE(!stale_replay &&
        stale_replay.error().code == carto::core::ErrorCode::stale_data);

    std::string tampered_project = loaded.value().serialize();
    const std::string patch_digest = loaded.value().mesh_patches().front().patch_digest.hex();
    const auto digest_offset = tampered_project.find(patch_digest);
    REQUIRE(digest_offset != std::string::npos);
    tampered_project[digest_offset] = tampered_project[digest_offset] == '0' ? '1' : '0';
    REQUIRE(!carto::project::ProjectDocument::deserialize(tampered_project));
    std::string missing_patch_table = loaded.value().serialize();
    const auto patch_table_start = missing_patch_table.find("MESH_PATCHES 1\n");
    REQUIRE(patch_table_start != std::string::npos);
    const auto patch_record_start = missing_patch_table.find("MESH_PATCH ", patch_table_start);
    REQUIRE(patch_record_start != std::string::npos);
    const auto patch_table_end = missing_patch_table.find('\n', patch_record_start);
    REQUIRE(patch_table_end != std::string::npos);
    missing_patch_table.erase(patch_table_start, patch_table_end + 1U - patch_table_start);
    REQUIRE(!carto::project::ProjectDocument::deserialize(missing_patch_table));

    std::string duplicate_patch = loaded.value().serialize();
    const auto duplicate_table = duplicate_patch.find("MESH_PATCHES 1\n");
    REQUIRE(duplicate_table != std::string::npos);
    const auto duplicate_row = duplicate_patch.find("MESH_PATCH ", duplicate_table);
    REQUIRE(duplicate_row != std::string::npos);
    const auto duplicate_row_end = duplicate_patch.find('\n', duplicate_row);
    REQUIRE(duplicate_row_end != std::string::npos);
    duplicate_patch.replace(duplicate_table, std::string("MESH_PATCHES 1\n").size(),
        "MESH_PATCHES 2\n");
    duplicate_patch.insert(duplicate_row_end + 1U,
        duplicate_patch.substr(duplicate_row, duplicate_row_end + 1U - duplicate_row));
    REQUIRE(!carto::project::ProjectDocument::deserialize(duplicate_patch));

    REQUIRE(!carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(session.snapshot().undo_count == 2U);
    REQUIRE(!session.snapshot().problems.empty());
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    const auto redo_source = session.document_.meshes().at(*mesh_asset);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().redo_count == 0U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.mesh_patches().back().replay(redo_source));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded && loaded.value().mesh_patches().size() == 2U);
}

void application_authors_selected_face_custom_normals_with_revisioned_history() {
    TempDirectory temp;
    const auto path = temp.path() / "custom-face-normal.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Normal box", {2.0, 2.0, 2.0}}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 1U);
    const auto object = initial.objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto source_mesh = session.document_.meshes().at(*object.mesh_asset);
    const auto selected_face = source_mesh.faces_sorted().front().id;
    const auto source_compiled = source_mesh.compile();
    REQUIRE(source_compiled);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{object.id, selected_face}));

    carto::editor::ToolArguments invalid_arguments;
    invalid_arguments.normal = {};
    const auto before_rejection = session.snapshot();
    const auto invalid_normal = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.set-face-normal", invalid_arguments});
    REQUIRE(!invalid_normal);
    REQUIRE(invalid_normal.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejection.undo_count);

    const carto::core::Vec3d authored_input{0.0, 3.0, 4.0};
    const carto::core::Vec3d expected_normal{0.0, 0.6, 0.8};
    carto::editor::ToolArguments arguments;
    arguments.normal = authored_input;
    session.active_source_ = carto::application::OperationSource::ai_proposal;
    const auto ai_attempt = session.invoke_tool(
        carto::application::InvokeToolAction{"mesh.set-face-normal", arguments});
    session.active_source_ = carto::application::OperationSource::human;
    REQUIRE(!ai_attempt);
    REQUIRE(ai_attempt.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejection.undo_count);

    const auto applied = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.set-face-normal", arguments});
    REQUIRE(applied && applied.value().document_changed);
    REQUIRE(applied.value().parameters.has_value());
    REQUIRE(applied.value().parameters->normal.has_value());
    REQUIRE(applied.value().parameters->normal.value().y == authored_input.y);
    REQUIRE(applied.value().affected.faces ==
        std::vector<carto::geometry::FaceId>{selected_face});

    const auto& authored_mesh = session.document_.meshes().at(*object.mesh_asset);
    REQUIRE(authored_mesh.revision() > source_mesh.revision());
    const auto* normal_layer = authored_mesh.attributes().find("cartographer.normal0");
    REQUIRE(normal_layer);
    REQUIRE(normal_layer->descriptor.domain == carto::attributes::AttributeDomain::corner);
    REQUIRE(normal_layer->descriptor.type == carto::attributes::AttributeType::vec3);
    const auto topology = authored_mesh.topology();
    REQUIRE(topology);
    std::map<carto::geometry::VertexId, carto::core::Vec3d> source_normals;
    for (std::size_t index = 0U; index < source_compiled.value().vertex_ids.size(); ++index) {
        source_normals.emplace(
            source_compiled.value().vertex_ids[index], source_compiled.value().normals[index]);
    }
    std::size_t authored_corner_count = 0U;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto& corner = topology.value().corners[index];
        const auto* value = std::get_if<carto::core::Vec3d>(&normal_layer->values[index]);
        REQUIRE(value);
        if (corner.face == selected_face) {
            REQUIRE(value->x == expected_normal.x);
            REQUIRE(value->y == expected_normal.y);
            REQUIRE(value->z == expected_normal.z);
            ++authored_corner_count;
        } else {
            REQUIRE(value->x == source_normals.at(corner.vertex).x);
            REQUIRE(value->y == source_normals.at(corner.vertex).y);
            REQUIRE(value->z == source_normals.at(corner.vertex).z);
        }
    }
    REQUIRE(authored_corner_count == authored_mesh.find_face(selected_face)->vertices.size());

    const auto overlay_snapshot = session.snapshot();
    REQUIRE(overlay_snapshot.project_revision == applied.value().revision_after);
    REQUIRE(overlay_snapshot.viewport.attribute_overlays.size() == 1U);
    const auto& normal_overlay = overlay_snapshot.viewport.attribute_overlays.front();
    REQUIRE(normal_overlay.object == object.id);
    REQUIRE(normal_overlay.mesh_asset == *object.mesh_asset);
    REQUIRE(normal_overlay.source_revision == authored_mesh.revision());
    REQUIRE(normal_overlay.data);
    REQUIRE(normal_overlay.data->has_custom_normal_layer);
    REQUIRE(normal_overlay.data->corner_normals.size() == topology.value().corners.size());
    REQUIRE(!normal_overlay.data->corner_normal_scan_capped);
    REQUIRE(normal_overlay.data->evaluated_mesh_revision ==
        overlay_snapshot.viewport.scene.find(object.id)->mesh->source_revision);
    bool selected_normal_is_visible = false;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        if (topology.value().corners[index].face != selected_face) continue;
        const auto direction = (normal_overlay.data->corner_normals[index].end -
            normal_overlay.data->corner_normals[index].start).normalized();
        REQUIRE(std::abs(direction.x - expected_normal.x) < 1.0e-9);
        REQUIRE(std::abs(direction.y - expected_normal.y) < 1.0e-9);
        REQUIRE(std::abs(direction.z - expected_normal.z) < 1.0e-9);
        selected_normal_is_visible = true;
    }
    REQUIRE(selected_normal_is_visible);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{path}));
    auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    const auto* reopened_layer = reopened.value().meshes().at(*object.mesh_asset)
        .attributes().find("cartographer.normal0");
    REQUIRE(reopened_layer);
    REQUIRE(reopened_layer->values.size() == normal_layer->values.size());
    for (std::size_t index = 0U; index < normal_layer->values.size(); ++index) {
        const auto* saved = std::get_if<carto::core::Vec3d>(&reopened_layer->values[index]);
        const auto* current = std::get_if<carto::core::Vec3d>(&normal_layer->values[index]);
        REQUIRE(saved && current);
        REQUIRE(saved->x == current->x);
        REQUIRE(saved->y == current->y);
        REQUIRE(saved->z == current->z);
    }

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.document_.meshes().at(*object.mesh_asset)
        .attributes().find("cartographer.normal0") == nullptr);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.document_.meshes().at(*object.mesh_asset)
        .attributes().find("cartographer.normal0") != nullptr);
}

void application_authors_selected_edge_sharp_marks_with_revisioned_history() {
    TempDirectory temp;
    const auto path = temp.path() / "sharp-edge.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Sharp edge box", {2.0, 2.0, 2.0}}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 1U);
    const auto object = initial.objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto source_mesh = session.document_.meshes().at(*object.mesh_asset);
    const auto source_topology = source_mesh.topology();
    REQUIRE(source_topology);
    REQUIRE(source_topology.value().edges.size() >= 3U);
    const std::vector<carto::geometry::EdgeId> selected_edges{
        source_topology.value().edges[0].id,
        source_topology.value().edges[2].id};

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{object.id, selected_edges[0]}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{object.id, selected_edges[1],
            carto::editor::SelectionOperation::add}));

    const auto before_rejection = session.snapshot();
    const auto missing_value = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{
            "mesh.set-edge-sharp", carto::editor::ToolArguments{}});
    REQUIRE(!missing_value);
    REQUIRE(missing_value.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejection.undo_count);

    carto::editor::ToolArguments arguments;
    arguments.edge_sharp = true;
    session.active_source_ = carto::application::OperationSource::ai_proposal;
    const auto ai_attempt = session.invoke_tool(
        carto::application::InvokeToolAction{"mesh.set-edge-sharp", arguments});
    session.active_source_ = carto::application::OperationSource::human;
    REQUIRE(!ai_attempt);
    REQUIRE(ai_attempt.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejection.undo_count);

    const auto applied = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.set-edge-sharp", arguments});
    REQUIRE(applied && applied.value().document_changed);
    REQUIRE(applied.value().parameters.has_value());
    REQUIRE(applied.value().parameters->edge_sharp == std::optional<bool>{true});
    REQUIRE(applied.value().affected.edges == selected_edges);

    const auto& authored_mesh = session.document_.meshes().at(*object.mesh_asset);
    REQUIRE(authored_mesh.revision() > source_mesh.revision());
    const auto* sharp_layer = authored_mesh.attributes().find("cartographer.edge_sharp");
    REQUIRE(sharp_layer);
    REQUIRE(sharp_layer->descriptor.domain == carto::attributes::AttributeDomain::edge);
    REQUIRE(sharp_layer->descriptor.type == carto::attributes::AttributeType::boolean);
    REQUIRE(sharp_layer->values.size() == source_topology.value().edges.size());
    for (std::size_t index = 0U; index < source_topology.value().edges.size(); ++index) {
        const auto* value = std::get_if<bool>(&sharp_layer->values[index]);
        REQUIRE(value);
        const auto edge = source_topology.value().edges[index].id;
        REQUIRE(*value == (edge == selected_edges[0] || edge == selected_edges[1]));
    }

    const auto overlay_snapshot = session.snapshot();
    REQUIRE(overlay_snapshot.viewport.attribute_overlays.size() == 1U);
    const auto& sharp_overlay = overlay_snapshot.viewport.attribute_overlays.front();
    REQUIRE(sharp_overlay.object == object.id);
    REQUIRE(sharp_overlay.source_revision == authored_mesh.revision());
    REQUIRE(sharp_overlay.data);
    REQUIRE(sharp_overlay.data->has_sharp_edge_layer);
    REQUIRE(sharp_overlay.data->hard_edges.size() == selected_edges.size());
    REQUIRE(sharp_overlay.data->evaluated_mesh_revision ==
        overlay_snapshot.viewport.scene.find(object.id)->mesh->source_revision);
    REQUIRE(sharp_overlay.data->hard_edges[0].start.x ==
        source_mesh.find_vertex(source_topology.value().edges[0].first)->position.x);
    REQUIRE(sharp_overlay.data->hard_edges[0].end.y ==
        source_mesh.find_vertex(source_topology.value().edges[0].second)->position.y);
    REQUIRE(sharp_overlay.data->hard_edges[1].start.z ==
        source_mesh.find_vertex(source_topology.value().edges[2].first)->position.z);

    const auto stale_before = session.snapshot();
    const auto stale_selection = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.set-edge-sharp", arguments});
    REQUIRE(!stale_selection);
    REQUIRE(stale_selection.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == stale_before.project_revision);
    REQUIRE(session.snapshot().undo_count == stale_before.undo_count);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{object.id, selected_edges[0]}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{object.id, selected_edges[1],
            carto::editor::SelectionOperation::add}));
    const auto no_op_before = session.snapshot();
    const auto repeated_mark = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.set-edge-sharp", arguments});
    REQUIRE(!repeated_mark);
    REQUIRE(repeated_mark.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == no_op_before.project_revision);
    REQUIRE(session.snapshot().undo_count == no_op_before.undo_count);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{path}));
    auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    const auto* reopened_layer = reopened.value().meshes().at(*object.mesh_asset)
        .attributes().find("cartographer.edge_sharp");
    REQUIRE(reopened_layer);
    REQUIRE(reopened_layer->values.size() == sharp_layer->values.size());
    for (std::size_t index = 0U; index < sharp_layer->values.size(); ++index) {
        const auto* saved = std::get_if<bool>(&reopened_layer->values[index]);
        const auto* current = std::get_if<bool>(&sharp_layer->values[index]);
        REQUIRE(saved && current);
        REQUIRE(*saved == *current);
    }

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.document_.meshes().at(*object.mesh_asset)
        .attributes().find("cartographer.edge_sharp") == nullptr);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.document_.meshes().at(*object.mesh_asset)
        .attributes().find("cartographer.edge_sharp") != nullptr);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{object.id, selected_edges[0]}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{object.id, selected_edges[1],
            carto::editor::SelectionOperation::add}));
    arguments.edge_sharp = false;
    const auto cleared = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.set-edge-sharp", arguments});
    REQUIRE(cleared && cleared.value().document_changed);
    REQUIRE(cleared.value().parameters->edge_sharp == std::optional<bool>{false});
    const auto* cleared_layer = session.document_.meshes().at(*object.mesh_asset)
        .attributes().find("cartographer.edge_sharp");
    REQUIRE(cleared_layer);
    for (const auto edge : selected_edges) {
        const auto found = std::find_if(source_topology.value().edges.begin(),
            source_topology.value().edges.end(), [edge](const auto& record) {
                return record.id == edge;
            });
        REQUIRE(found != source_topology.value().edges.end());
        const auto index = static_cast<std::size_t>(
            std::distance(source_topology.value().edges.begin(), found));
        const auto* value = std::get_if<bool>(&cleared_layer->values[index]);
        REQUIRE(value && !*value);
    }
}

void application_sets_selected_vertex_positions_across_objects_atomically_and_persists_patches() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-vertex-position.carto";
    auto first_fixture = make_application_tetrahedron();
    auto second_fixture = make_application_tetrahedron();
    REQUIRE(first_fixture && second_fixture);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First position tetrahedron", std::move(first_fixture.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second position tetrahedron", std::move(second_fixture.value())}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);
    const carto::geometry::VertexId first_vertex{1U};
    const carto::geometry::VertexId second_vertex{2U};
    const auto* first_source_vertex = first_source.find_vertex(first_vertex);
    const auto* second_source_vertex = second_source.find_vertex(second_vertex);
    REQUIRE(first_source_vertex && second_source_vertex);
    REQUIRE(first_source_vertex->position.x != second_source_vertex->position.x);
    const carto::core::Vec3d target{0.25, 0.5, 0.75};

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, first_vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, second_vertex,
            carto::editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    REQUIRE(selected.selection.component_groups[0].object == first.id);
    REQUIRE(selected.selection.component_groups[0].vertices ==
        std::vector<carto::geometry::VertexId>{first_vertex});
    REQUIRE(selected.selection.component_groups[1].object == second.id);
    REQUIRE(selected.selection.component_groups[1].vertices ==
        std::vector<carto::geometry::VertexId>{second_vertex});

    carto::editor::ToolArguments arguments;
    arguments.position = target;
    const auto before = session.snapshot();
    const auto positioned = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.set-vertex-position", arguments});
    if (!positioned) {
        throw std::runtime_error("multi-object vertex positioning failed: " +
            positioned.error().message);
    }
    REQUIRE(positioned);
    REQUIRE((positioned.value().affected.objects ==
        std::vector<carto::scene::ObjectId>{first.id, second.id}));
    REQUIRE((positioned.value().affected.vertices ==
        std::vector<carto::geometry::VertexId>{first_vertex, second_vertex}));
    REQUIRE(session.snapshot().undo_count == before.undo_count + 1U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.topology_receipts().empty());

    const auto require_position = [](const carto::geometry::EditableMesh& mesh,
                                     carto::geometry::VertexId id,
                                     carto::core::Vec3d expected) {
        const auto* vertex = mesh.find_vertex(id);
        REQUIRE(vertex);
        REQUIRE(vertex->position.x == expected.x);
        REQUIRE(vertex->position.y == expected.y);
        REQUIRE(vertex->position.z == expected.z);
    };
    const auto verify_patch = [&](const std::vector<carto::project::MeshPatchRecord>& records,
                                  std::uint64_t mesh_asset,
                                  const carto::geometry::EditableMesh& source,
                                  carto::geometry::VertexId id) {
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset](const auto& candidate) { return candidate.mesh_asset == mesh_asset; });
        REQUIRE(record != records.end());
        const auto patch = carto::geometry::MeshPatch::deserialize(record->patch_bytes);
        REQUIRE(patch);
        REQUIRE(patch.value().expected_revision == source.revision());
        REQUIRE(patch.value().vertex_changes.size() == 1U);
        const auto& change = patch.value().vertex_changes.front();
        REQUIRE(change.id == id);
        const auto* source_vertex = source.find_vertex(id);
        REQUIRE(source_vertex);
        REQUIRE(change.before.x == source_vertex->position.x);
        REQUIRE(change.before.y == source_vertex->position.y);
        REQUIRE(change.before.z == source_vertex->position.z);
        REQUIRE(change.after.x == target.x);
        REQUIRE(change.after.y == target.y);
        REQUIRE(change.after.z == target.z);
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        require_position(replayed.value(), id, target);
    };
    verify_patch(session.document_.mesh_patches(), *first.mesh_asset, first_source, first_vertex);
    verify_patch(session.document_.mesh_patches(), *second.mesh_asset, second_source, second_vertex);
    require_position(session.document_.meshes().at(*first.mesh_asset), first_vertex, target);
    require_position(session.document_.meshes().at(*second.mesh_asset), second_vertex, target);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto saved = carto::project::ProjectDocument::load(path);
    REQUIRE(saved && saved.value().mesh_patches().size() == 2U);
    verify_patch(saved.value().mesh_patches(), *first.mesh_asset, first_source, first_vertex);
    verify_patch(saved.value().mesh_patches(), *second.mesh_asset, second_source, second_vertex);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);
    require_position(reopened.document_.meshes().at(*first.mesh_asset), first_vertex, target);
    require_position(reopened.document_.meshes().at(*second.mesh_asset), second_vertex, target);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    require_position(session.document_.meshes().at(*first.mesh_asset), first_vertex,
        first_source_vertex->position);
    require_position(session.document_.meshes().at(*second.mesh_asset), second_vertex,
        second_source_vertex->position);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    require_position(session.document_.meshes().at(*first.mesh_asset), first_vertex, target);
    require_position(session.document_.meshes().at(*second.mesh_asset), second_vertex, target);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto redone_saved = carto::project::ProjectDocument::load(path);
    REQUIRE(redone_saved && redone_saved.value().mesh_patches().size() == 4U);
}

void application_rejects_multi_object_vertex_position_for_shared_stale_and_invalid_owners() {
    {
        auto fixture = make_application_tetrahedron();
        REQUIRE(fixture);
        carto::application::ApplicationSession session;
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{
                "Shared position tetrahedron", std::move(fixture.value())}));
        const auto source = session.snapshot().objects.front().object;
        REQUIRE(source.mesh_asset.has_value());
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::DuplicateMeshInstanceAction{
                source.id, "Shared position instance", {3.0, 0.0, 0.0}}));
        const auto objects = session.snapshot().objects;
        REQUIRE(objects.size() == 2U);
        const auto instance = objects.back().object;
        REQUIRE(instance.mesh_asset == source.mesh_asset);
        const auto mesh_before = session.document_.meshes().at(*source.mesh_asset);
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{source.id, carto::geometry::VertexId{1U}}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{instance.id, carto::geometry::VertexId{2U},
                carto::editor::SelectionOperation::add}));
        const auto before = session.snapshot();
        carto::editor::ToolArguments arguments;
        arguments.position = {3.0, 3.0, 3.0};
        const auto rejected = carto::application::testing::dispatch(session,
            carto::application::InvokeToolAction{"mesh.set-vertex-position", arguments});
        REQUIRE(!rejected);
        REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.snapshot().undo_count == before.undo_count);
        REQUIRE(session.document_.meshes().at(*source.mesh_asset).revision() ==
            mesh_before.revision());
        REQUIRE(session.document_.meshes().at(*source.mesh_asset).find_vertex(
            carto::geometry::VertexId{1U})->position.x ==
            mesh_before.find_vertex(carto::geometry::VertexId{1U})->position.x);
        REQUIRE(session.document_.mesh_patches().empty());
        REQUIRE(session.document_.topology_receipts().empty());
    }

    {
        auto first_fixture = make_application_tetrahedron();
        auto second_fixture = make_application_tetrahedron();
        REQUIRE(first_fixture && second_fixture);
        carto::application::ApplicationSession session;
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{
                "First stale position tetrahedron", std::move(first_fixture.value())}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{
                "Second stale position tetrahedron", std::move(second_fixture.value())}));
        const auto objects = session.snapshot().objects;
        const auto first = objects[0].object;
        const auto second = objects[1].object;
        REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
        const auto first_before = session.document_.meshes().at(*first.mesh_asset);
        const auto second_before = session.document_.meshes().at(*second.mesh_asset);
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{first.id, carto::geometry::VertexId{1U}}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{second.id, carto::geometry::VertexId{2U},
                carto::editor::SelectionOperation::add}));
        const auto stale_revision = session.document_.replace_mesh_if_revision(
            *second.mesh_asset, second_before.revision(), second_before);
        REQUIRE(stale_revision);
        const auto before = session.snapshot();
        carto::editor::ToolArguments arguments;
        arguments.position = {0.25, 0.5, 0.75};
        const auto rejected = carto::application::testing::dispatch(session,
            carto::application::InvokeToolAction{"mesh.set-vertex-position", arguments});
        REQUIRE(!rejected);
        REQUIRE(rejected.error().code == carto::core::ErrorCode::stale_data);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.snapshot().undo_count == before.undo_count);
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() ==
            first_before.revision());
        REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() ==
            stale_revision.value());
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).find_vertex(
            carto::geometry::VertexId{1U})->position.x ==
            first_before.find_vertex(carto::geometry::VertexId{1U})->position.x);
        REQUIRE(session.document_.mesh_patches().empty());
    }

    {
        carto::geometry::EditableMesh first_mesh;
        const auto first_a = first_mesh.add_vertex({0.0, 0.0, 0.0});
        const auto first_b = first_mesh.add_vertex({2.0, 0.0, 0.0});
        const auto first_c = first_mesh.add_vertex({0.0, 2.0, 0.0});
        REQUIRE(first_a && first_b && first_c);
        REQUIRE(first_mesh.add_face({first_a.value(), first_b.value(), first_c.value()}));
        carto::geometry::EditableMesh second_mesh;
        const auto second_a = second_mesh.add_vertex({0.0, 0.0, 0.0});
        const auto second_b = second_mesh.add_vertex({1.0, 1.0, 1.0});
        const auto second_c = second_mesh.add_vertex({0.0, 1.0, 0.0});
        REQUIRE(second_a && second_b && second_c);
        REQUIRE(second_mesh.add_face({second_a.value(), second_b.value(), second_c.value()}));
        const carto::core::Vec3d target{1.0, 1.0, 1.0};

        auto first_probe = first_mesh;
        REQUIRE(first_probe.apply_patch(carto::geometry::MeshPatch{
            first_mesh.revision(),
            {{first_a.value(), {0.0, 0.0, 0.0}, target}}}));
        auto second_probe = second_mesh;
        const auto invalid_second_patch = second_probe.apply_patch(carto::geometry::MeshPatch{
            second_mesh.revision(),
            {{second_a.value(), {0.0, 0.0, 0.0}, target}}});
        REQUIRE(!invalid_second_patch);
        REQUIRE(invalid_second_patch.error().code == carto::core::ErrorCode::validation_failed);

        carto::application::ApplicationSession session;
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{"Valid first position mesh", first_mesh}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{"Invalid later position mesh", second_mesh}));
        const auto objects = session.snapshot().objects;
        const auto first = objects[0].object;
        const auto second = objects[1].object;
        REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
        const auto first_before = session.document_.meshes().at(*first.mesh_asset);
        const auto second_before = session.document_.meshes().at(*second.mesh_asset);
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{first.id, first_a.value()}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{second.id, second_a.value(),
                carto::editor::SelectionOperation::add}));
        const auto before = session.snapshot();
        carto::editor::ToolArguments arguments;
        arguments.position = target;
        const auto rejected = carto::application::testing::dispatch(session,
            carto::application::InvokeToolAction{"mesh.set-vertex-position", arguments});
        REQUIRE(!rejected);
        REQUIRE(rejected.error().code == carto::core::ErrorCode::validation_failed);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.snapshot().undo_count == before.undo_count);
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() ==
            first_before.revision());
        REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() ==
            second_before.revision());
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).find_vertex(first_a.value())->
            position.x == first_before.find_vertex(first_a.value())->position.x);
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).find_vertex(first_a.value())->
            position.y == first_before.find_vertex(first_a.value())->position.y);
        REQUIRE(session.document_.meshes().at(*second.mesh_asset).find_vertex(second_a.value())->
            position.x == second_before.find_vertex(second_a.value())->position.x);
        REQUIRE(session.document_.meshes().at(*second.mesh_asset).find_vertex(second_a.value())->
            position.y == second_before.find_vertex(second_a.value())->position.y);
        REQUIRE(session.document_.mesh_patches().empty());
        REQUIRE(session.document_.topology_receipts().empty());
    }
}

void application_slides_selected_vertices_across_objects_with_owner_local_support_and_history() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-vertex-slide.carto";
    auto first_fixture = make_application_tetrahedron();
    auto second_fixture = make_application_tetrahedron();
    REQUIRE(first_fixture && second_fixture);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First vertex slide tetrahedron", std::move(first_fixture.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second vertex slide tetrahedron", std::move(second_fixture.value())}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);
    const carto::geometry::VertexId first_vertex{1U};
    const carto::geometry::VertexId second_vertex{2U};

    const auto reference_slide = [](carto::geometry::EditableMesh mesh,
                                    carto::geometry::VertexId vertex,
                                    double factor) {
        const auto topology = mesh.topology();
        REQUIRE(topology);
        std::optional<carto::geometry::EdgeId> support;
        for (const auto& edge : topology.value().edges) {
            if ((edge.first == vertex || edge.second == vertex) &&
                (!support.has_value() || edge.id < *support)) {
                support = edge.id;
            }
        }
        REQUIRE(support.has_value());
        REQUIRE(mesh.slide_vertex(vertex, *support, factor));
        return std::make_pair(std::move(mesh), *support);
    };
    constexpr double factor = 0.25;
    const auto [first_expected, first_support] = reference_slide(first_source, first_vertex, factor);
    const auto [second_expected, second_support] = reference_slide(second_source, second_vertex, factor);
    REQUIRE(first_support != second_support);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, first_vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, second_vertex,
            carto::editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    REQUIRE(selected.selection.component_groups[0].object == first.id);
    REQUIRE(selected.selection.component_groups[0].vertices.front() == first_vertex);
    REQUIRE(selected.selection.component_groups[1].object == second.id);
    REQUIRE(selected.selection.component_groups[1].vertices.front() == second_vertex);
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.factor = factor;
    const auto slided = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.slide-vertex", arguments});
    if (!slided) {
        throw std::runtime_error("multi-object vertex slide failed: " + slided.error().message);
    }
    REQUIRE(slided);
    REQUIRE((slided.value().affected.objects ==
        std::vector<carto::scene::ObjectId>{first.id, second.id}));
    REQUIRE((slided.value().affected.vertices ==
        std::vector<carto::geometry::VertexId>{first_vertex, second_vertex}));
    REQUIRE(slided.value().parameters.has_value());
    REQUIRE(slided.value().parameters->factor.has_value());
    REQUIRE(*slided.value().parameters->factor == factor);
    REQUIRE(!slided.value().parameters->support_edge.has_value());
    REQUIRE(session.snapshot().undo_count == before.undo_count + 1U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.topology_receipts().empty());

    const auto assert_same_positions = [](const carto::geometry::EditableMesh& actual,
                                          const carto::geometry::EditableMesh& expected) {
        const auto actual_vertices = actual.vertices_sorted();
        const auto expected_vertices = expected.vertices_sorted();
        REQUIRE(actual_vertices.size() == expected_vertices.size());
        for (std::size_t index = 0U; index < expected_vertices.size(); ++index) {
            REQUIRE(actual_vertices[index].id == expected_vertices[index].id);
            REQUIRE(actual_vertices[index].position.x == expected_vertices[index].position.x);
            REQUIRE(actual_vertices[index].position.y == expected_vertices[index].position.y);
            REQUIRE(actual_vertices[index].position.z == expected_vertices[index].position.z);
        }
    };
    const auto verify_replay = [&](const std::vector<carto::project::MeshPatchRecord>& records,
                                   std::uint64_t asset,
                                   const carto::geometry::EditableMesh& source,
                                   const carto::geometry::EditableMesh& expected) {
        const auto record = std::find_if(records.rbegin(), records.rend(),
            [asset](const auto& candidate) { return candidate.mesh_asset == asset; });
        REQUIRE(record != records.rend());
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        assert_same_positions(replayed.value(), expected);
    };
    const auto first_slided = session.document_.meshes().at(*first.mesh_asset);
    const auto second_slided = session.document_.meshes().at(*second.mesh_asset);
    assert_same_positions(first_slided, first_expected);
    assert_same_positions(second_slided, second_expected);
    REQUIRE(first_slided.find_vertex(first_vertex)->position.x !=
        first_source.find_vertex(first_vertex)->position.x ||
        first_slided.find_vertex(first_vertex)->position.y !=
        first_source.find_vertex(first_vertex)->position.y ||
        first_slided.find_vertex(first_vertex)->position.z !=
        first_source.find_vertex(first_vertex)->position.z);
    REQUIRE(second_slided.find_vertex(second_vertex)->position.x !=
        second_source.find_vertex(second_vertex)->position.x ||
        second_slided.find_vertex(second_vertex)->position.y !=
        second_source.find_vertex(second_vertex)->position.y ||
        second_slided.find_vertex(second_vertex)->position.z !=
        second_source.find_vertex(second_vertex)->position.z);
    verify_replay(session.document_.mesh_patches(), *first.mesh_asset, first_source, first_slided);
    verify_replay(session.document_.mesh_patches(), *second.mesh_asset, second_source, second_slided);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    auto saved = carto::project::ProjectDocument::load(path);
    REQUIRE(saved && saved.value().mesh_patches().size() == 2U);
    verify_replay(saved.value().mesh_patches(), *first.mesh_asset, first_source, first_slided);
    verify_replay(saved.value().mesh_patches(), *second.mesh_asset, second_source, second_slided);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);
    assert_same_positions(reopened.document_.meshes().at(*first.mesh_asset), first_slided);
    assert_same_positions(reopened.document_.meshes().at(*second.mesh_asset), second_slided);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    assert_same_positions(session.document_.meshes().at(*first.mesh_asset), first_source);
    assert_same_positions(session.document_.meshes().at(*second.mesh_asset), second_source);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    assert_same_positions(session.document_.meshes().at(*first.mesh_asset), first_slided);
    assert_same_positions(session.document_.meshes().at(*second.mesh_asset), second_slided);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    verify_replay(session.document_.mesh_patches(), *first.mesh_asset,
        first_redo_source, first_slided);
    verify_replay(session.document_.mesh_patches(), *second.mesh_asset,
        second_redo_source, second_slided);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    saved = carto::project::ProjectDocument::load(path);
    REQUIRE(saved && saved.value().mesh_patches().size() == 4U);
    verify_replay(saved.value().mesh_patches(), *first.mesh_asset,
        first_redo_source, first_slided);
    verify_replay(saved.value().mesh_patches(), *second.mesh_asset,
        second_redo_source, second_slided);
}

void application_slides_multiple_vertices_per_owner_with_replayable_history() {
    using namespace carto;
    TempDirectory temp;
    const auto path = temp.path() / "multi-vertex-slide.carto";
    auto fixture = make_application_disconnected_tetrahedra(2U);
    REQUIRE(fixture);
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Disconnected slide vertices", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_asset = *object.mesh_asset;
    const auto source = session.document_.meshes().at(mesh_asset);
    const std::array<geometry::VertexId, 2U> selected{geometry::VertexId{1U}, geometry::VertexId{5U}};
    auto expected = source;
    const auto topology = source.topology();
    REQUIRE(topology);
    const std::set<geometry::VertexId> selected_set(selected.begin(), selected.end());
    for (const auto vertex : selected) {
        std::optional<geometry::EdgeId> support;
        for (const auto& edge : topology.value().edges) {
            if (edge.first != vertex && edge.second != vertex) continue;
            const auto other = edge.first == vertex ? edge.second : edge.first;
            if (!selected_set.contains(other) && (!support || edge.id < *support)) {
                support = edge.id;
            }
        }
        REQUIRE(support.has_value());
        REQUIRE(expected.slide_vertex(vertex, *support, 0.25));
    }

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    // Deliberately select in reverse ID order: patch order must remain canonical.
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, selected[1]}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, selected[0], editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    editor::ToolArguments arguments;
    arguments.factor = 0.25;
    const auto slided = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(slided);
    REQUIRE(slided.value().affected.vertices.size() == selected.size());
    REQUIRE(session.snapshot().undo_count == before.undo_count + 1U);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(session.document_.topology_receipts().empty());

    const auto same_positions = [](const geometry::EditableMesh& actual,
                                   const geometry::EditableMesh& target) {
        const auto actual_vertices = actual.vertices_sorted();
        const auto target_vertices = target.vertices_sorted();
        REQUIRE(actual_vertices.size() == target_vertices.size());
        for (std::size_t index = 0U; index < target_vertices.size(); ++index) {
            REQUIRE(actual_vertices[index].id == target_vertices[index].id);
            REQUIRE(actual_vertices[index].position.x == target_vertices[index].position.x);
            REQUIRE(actual_vertices[index].position.y == target_vertices[index].position.y);
            REQUIRE(actual_vertices[index].position.z == target_vertices[index].position.z);
        }
    };
    const auto edited = session.document_.meshes().at(mesh_asset);
    same_positions(edited, expected);
    const auto verify_last_patch = [&](const std::vector<project::MeshPatchRecord>& records,
                                       const geometry::EditableMesh& patch_source,
                                       const geometry::EditableMesh& patch_target) {
        const auto record = std::find_if(records.rbegin(), records.rend(),
            [mesh_asset](const auto& candidate) { return candidate.mesh_asset == mesh_asset; });
        REQUIRE(record != records.rend());
        const auto patch = geometry::MeshPatch::deserialize(record->patch_bytes);
        REQUIRE(patch);
        REQUIRE(patch.value().expected_revision == patch_source.revision());
        REQUIRE(patch.value().vertex_changes.size() == selected.size());
        REQUIRE(patch.value().vertex_changes[0].id == selected[0]);
        REQUIRE(patch.value().vertex_changes[1].id == selected[1]);
        const auto replayed = record->replay(patch_source);
        REQUIRE(replayed);
        same_positions(replayed.value(), patch_target);
    };
    verify_last_patch(session.document_.mesh_patches(), source, edited);
    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    auto reopened_document = project::ProjectDocument::load(path);
    REQUIRE(reopened_document);
    verify_last_patch(reopened_document.value().mesh_patches(), source,
        reopened_document.value().meshes().at(mesh_asset));

    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    same_positions(session.document_.meshes().at(mesh_asset), source);
    const auto redo_source = session.document_.meshes().at(mesh_asset);
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    const auto redone = session.document_.meshes().at(mesh_asset);
    same_positions(redone, expected);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    verify_last_patch(session.document_.mesh_patches(), redo_source, redone);
    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    reopened_document = project::ProjectDocument::load(path);
    REQUIRE(reopened_document);
    verify_last_patch(reopened_document.value().mesh_patches(), redo_source,
        reopened_document.value().meshes().at(mesh_asset));
    application::ApplicationSession reopened_session;
    REQUIRE(application::testing::dispatch(reopened_session,
        application::OpenProjectAction{path}));
    same_positions(reopened_session.document_.meshes().at(mesh_asset), redone);
}

void application_rejects_multi_vertex_slide_edges_and_late_failure_atomically() {
    using namespace carto;
    auto fixture = make_application_tetrahedron();
    REQUIRE(fixture);
    const auto isolated_vertex = fixture.value().add_vertex({9.0, 9.0, 9.0});
    REQUIRE(isolated_vertex);
    REQUIRE(fixture.value().validate());
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Invalid multi-slide boundary", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_asset = *object.mesh_asset;
    const auto source = session.document_.meshes().at(mesh_asset);
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, geometry::VertexId{1U}}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, geometry::VertexId{2U},
            editor::SelectionOperation::add}));
    auto before = session.snapshot();
    const std::string unchanged = session.document_.serialize();
    editor::ToolArguments arguments;
    arguments.factor = 0.25;
    arguments.support_edge = geometry::EdgeId{1U};
    auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == unchanged);

    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, geometry::VertexId{1U}}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{object.id, isolated_vertex.value(),
            editor::SelectionOperation::add}));
    before = session.snapshot();
    arguments.support_edge.reset();
    rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::not_found);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == unchanged);
    REQUIRE(session.document_.meshes().at(mesh_asset).revision() == source.revision());
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
}

void application_rejects_single_selected_shared_vertex_slide_atomically() {
    using namespace carto;
    auto fixture = make_application_tetrahedron();
    REQUIRE(fixture);
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Shared single-owner slide source", fixture.value()}));
    const auto source_object = session.snapshot().objects.front().object;
    REQUIRE(source_object.mesh_asset.has_value());
    REQUIRE(application::testing::dispatch(session,
        application::DuplicateMeshInstanceAction{
            source_object.id, "Unselected shared slide instance", {3.0, 0.0, 0.0}}));
    const auto mesh_asset = *source_object.mesh_asset;
    const auto source = session.document_.meshes().at(mesh_asset);
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectVertexAction{source_object.id, geometry::VertexId{1U}}));
    const auto before = session.snapshot();
    const std::string unchanged = session.document_.serialize();
    editor::ToolArguments arguments;
    arguments.factor = 0.25;
    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == unchanged);
    REQUIRE(session.document_.meshes().at(mesh_asset).revision() == source.revision());
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
}

void application_rejects_multi_vertex_slide_over_budget_before_mutation() {
    using namespace carto;
    constexpr std::size_t kSelectedVertices = 65U;
    geometry::EditableMesh fixture;
    constexpr std::size_t kTriangleCount = 22U;
    for (std::size_t index = 0U; index < kTriangleCount; ++index) {
        const double x = static_cast<double>(index) * 2.0;
        const auto a = fixture.add_vertex({x, 0.0, 0.0});
        const auto b = fixture.add_vertex({x + 1.0, 0.0, 0.0});
        const auto c = fixture.add_vertex({x, 1.0, 0.0});
        REQUIRE(a && b && c);
        REQUIRE(fixture.add_face({a.value(), b.value(), c.value()}));
    }
    REQUIRE(fixture.validate());
    const auto vertices = fixture.vertices_sorted();
    REQUIRE(vertices.size() == 66U);
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Over-budget vertex slide", fixture}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_asset = *object.mesh_asset;
    const auto& bound_mesh = session.document_.meshes().at(mesh_asset);
    REQUIRE(session.selection_.set_mode(editor::SelectionMode::vertex));
    for (std::size_t index = 0U; index < kSelectedVertices; ++index) {
        const auto operation = index == 0U
            ? editor::SelectionOperation::replace
            : editor::SelectionOperation::add;
        REQUIRE(session.selection_.select_vertex(session.document_.scene(), object.id,
            bound_mesh, vertices[index].id, operation));
    }
    const auto before = session.snapshot();
    const std::string unchanged = session.document_.serialize();
    editor::ToolArguments arguments;
    arguments.factor = 0.25;
    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == unchanged);
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
}

void application_rejects_multi_object_vertex_slide_with_ambiguous_explicit_support_edge() {
    auto first_fixture = make_application_tetrahedron();
    auto second_fixture = make_application_tetrahedron();
    REQUIRE(first_fixture && second_fixture);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First explicit slide tetrahedron", std::move(first_fixture.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second explicit slide tetrahedron", std::move(second_fixture.value())}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, carto::geometry::VertexId{2U},
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.factor = 0.25;
    arguments.support_edge = carto::geometry::EdgeId{1U};
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == second_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).find_vertex(
        carto::geometry::VertexId{1U})->position.x ==
        first_before.find_vertex(carto::geometry::VertexId{1U})->position.x);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).find_vertex(
        carto::geometry::VertexId{2U})->position.x ==
        second_before.find_vertex(carto::geometry::VertexId{2U})->position.x);
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
}

void application_rejects_multi_object_vertex_slide_on_shared_mesh_assets() {
    auto fixture = make_application_tetrahedron();
    REQUIRE(fixture);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Shared vertex slide tetrahedron", std::move(fixture.value())}));
    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Shared vertex slide instance", {3.0, 0.0, 0.0}}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto instance = objects.back().object;
    REQUIRE(instance.mesh_asset == source.mesh_asset);
    const auto mesh_before = session.document_.meshes().at(*source.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{source.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{instance.id, carto::geometry::VertexId{2U},
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.factor = 0.25;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).revision() == mesh_before.revision());
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).find_vertex(
        carto::geometry::VertexId{1U})->position.x ==
        mesh_before.find_vertex(carto::geometry::VertexId{1U})->position.x);
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
}

void application_rejects_multi_object_vertex_slide_on_stale_second_owner_atomically() {
    auto first_fixture = make_application_tetrahedron();
    auto second_fixture = make_application_tetrahedron();
    REQUIRE(first_fixture && second_fixture);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First stale vertex slide tetrahedron", std::move(first_fixture.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second stale vertex slide tetrahedron", std::move(second_fixture.value())}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, carto::geometry::VertexId{2U},
            carto::editor::SelectionOperation::add}));
    const auto stale_revision = session.document_.replace_mesh_if_revision(
        *second.mesh_asset, second_before.revision(), second_before);
    REQUIRE(stale_revision);
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.factor = 0.25;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == stale_revision.value());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).find_vertex(
        carto::geometry::VertexId{1U})->position.x ==
        first_before.find_vertex(carto::geometry::VertexId{1U})->position.x);
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
}

void application_rejects_multi_object_vertex_slide_atomically_when_later_owner_has_no_edge() {
    auto first_fixture = make_application_tetrahedron();
    auto second_fixture = make_application_tetrahedron();
    REQUIRE(first_fixture && second_fixture);
    auto& later_mesh = second_fixture.value();
    const auto isolated_vertex = later_mesh.add_vertex({9.0, 9.0, 9.0});
    REQUIRE(isolated_vertex);
    REQUIRE(later_mesh.validate());

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Valid first slide tetrahedron", std::move(first_fixture.value())}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Isolated later slide vertex", std::move(later_mesh)}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    const auto first_topology = first_before.topology();
    REQUIRE(first_topology);
    const auto first_support = std::find_if(first_topology.value().edges.begin(),
        first_topology.value().edges.end(), [](const auto& edge) {
            return edge.first == carto::geometry::VertexId{1U} ||
                edge.second == carto::geometry::VertexId{1U};
        });
    REQUIRE(first_support != first_topology.value().edges.end());
    auto first_probe = first_before;
    REQUIRE(first_probe.slide_vertex(carto::geometry::VertexId{1U}, first_support->id, 0.25));
    const auto second_topology = second_before.topology();
    REQUIRE(second_topology);
    REQUIRE(std::none_of(second_topology.value().edges.begin(), second_topology.value().edges.end(),
        [isolated_vertex = isolated_vertex.value()](const auto& edge) {
            return edge.first == isolated_vertex || edge.second == isolated_vertex;
        }));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{first.id, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{second.id, isolated_vertex.value(),
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    REQUIRE(session.document_.mesh_patches().empty());
    const std::string serialized_before = session.document_.serialize();
    carto::editor::ToolArguments arguments;
    arguments.factor = 0.25;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.slide-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == second_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).find_vertex(
        carto::geometry::VertexId{1U})->position.x ==
        first_before.find_vertex(carto::geometry::VertexId{1U})->position.x);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).find_vertex(
        carto::geometry::VertexId{1U})->position.y ==
        first_before.find_vertex(carto::geometry::VertexId{1U})->position.y);
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.serialize() == serialized_before);
}

void project_face_deletion_patch_is_stale_safe_and_replayable() {
    using namespace carto;
    project::ProjectDocument document;
    auto mesh = geometry::make_plane(2.0, 2.0);
    REQUIRE(mesh);
    const auto asset = document.add_mesh(mesh.value());
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const auto face = source.faces_sorted().front().id;
    const std::string before_rejections = document.serialize();

    const auto stale = document.apply_face_deletion_patch(asset.value(),
        geometry::FaceDeletionPatch{source.revision().next(), face, true});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    REQUIRE(document.serialize() == before_rejections);

    const auto missing = document.apply_face_deletion_patch(asset.value(),
        geometry::FaceDeletionPatch{source.revision(), geometry::FaceId{999U}, true});
    REQUIRE(!missing && missing.error().code == core::ErrorCode::not_found);
    REQUIRE(document.serialize() == before_rejections);

    REQUIRE(document.apply_face_deletion_patch(asset.value(),
        geometry::FaceDeletionPatch{source.revision(), face, true}));
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto replayed = document.mesh_patches().front().replay(source);
    REQUIRE(replayed && replayed.value().faces_sorted().empty());

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto reopened_replay = reopened.value().mesh_patches().front().replay(source);
    REQUIRE(reopened_replay && reopened_replay.value().faces_sorted().empty());
}

void project_edge_split_patch_is_stale_safe_and_replayable() {
    using namespace carto;
    project::ProjectDocument document;
    auto mesh = make_application_tetrahedron();
    REQUIRE(mesh);
    const auto asset = document.add_mesh(mesh.value());
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const auto edge = source.edges_sorted(1U).front().id;
    const std::string before_rejections = document.serialize();

    const auto stale = document.apply_edge_split_patch(asset.value(),
        geometry::EdgeSplitPatch{source.revision().next(), edge, 0.375});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    REQUIRE(document.serialize() == before_rejections);

    const auto missing = document.apply_edge_split_patch(asset.value(),
        geometry::EdgeSplitPatch{source.revision(), geometry::EdgeId{999U}, 0.375});
    REQUIRE(!missing && missing.error().code == core::ErrorCode::not_found);
    REQUIRE(document.serialize() == before_rejections);

    const auto invalid_factor = document.apply_edge_split_patch(asset.value(),
        geometry::EdgeSplitPatch{source.revision(), edge, 1.0});
    REQUIRE(!invalid_factor && invalid_factor.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(document.serialize() == before_rejections);

    REQUIRE(document.apply_edge_split_patch(asset.value(),
        geometry::EdgeSplitPatch{source.revision(), edge, 0.375}));
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto replayed = document.mesh_patches().front().replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertices_sorted().size() == source.vertices_sorted().size() + 1U);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto reopened_replay = reopened.value().mesh_patches().front().replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_loop_cut_patch_is_stale_safe_and_replayable() {
    using namespace carto;
    auto fixture = make_application_loop_cut_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture.mesh);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const std::string before_rejections = document.serialize();

    const geometry::LoopCutPatch zero_edge{source.revision(), geometry::EdgeId{}};
    REQUIRE(!zero_edge.validate());
    const auto zero_edge_rejected = document.apply_loop_cut_patch(asset.value(), zero_edge);
    REQUIRE(!zero_edge_rejected &&
        zero_edge_rejected.error().code == core::ErrorCode::invalid_argument);
    const auto stale = document.apply_loop_cut_patch(asset.value(),
        geometry::LoopCutPatch{source.revision().next(), fixture.seed_edge});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto missing = document.apply_loop_cut_patch(asset.value(),
        geometry::LoopCutPatch{source.revision(), geometry::EdgeId{999999U}});
    REQUIRE(!missing && missing.error().code == core::ErrorCode::not_found);
    REQUIRE(document.serialize() == before_rejections);

    REQUIRE(document.apply_loop_cut_patch(asset.value(),
        geometry::LoopCutPatch{source.revision(), fixture.seed_edge}));
    REQUIRE(document.mesh_patches().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::LoopCutPatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().edge == fixture.seed_edge);
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.01, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_knife_cut_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    geometry::EditableMesh fixture;
    const auto a = fixture.add_vertex({0.0, 0.0, 0.0});
    const auto b = fixture.add_vertex({2.0, 0.0, 0.0});
    const auto c = fixture.add_vertex({2.0, 2.0, 0.0});
    const auto d = fixture.add_vertex({0.0, 2.0, 0.0});
    REQUIRE(a && b && c && d);
    const auto face = fixture.add_face({a.value(), b.value(), c.value(), d.value()});
    REQUIRE(face);

    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const std::vector<core::Vec3d> path{
        {0.0, 0.0, 0.0}, {0.75, 1.0, 0.0}, {2.0, 2.0, 0.0}};
    const geometry::KnifeCutPatch patch{source.revision(), face.value(), path};
    const std::string before_rejections = document.serialize();

    const auto missing_asset = document.apply_knife_cut_patch(999999U, patch);
    REQUIRE(!missing_asset && missing_asset.error().code == core::ErrorCode::not_found);
    const auto stale = document.apply_knife_cut_patch(asset.value(),
        geometry::KnifeCutPatch{source.revision().next(), face.value(), path});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto missing_face = document.apply_knife_cut_patch(asset.value(),
        geometry::KnifeCutPatch{
            source.revision(), geometry::FaceId{999999U}, path});
    REQUIRE(!missing_face && missing_face.error().code == core::ErrorCode::not_found);
    const auto non_finite = document.apply_knife_cut_patch(asset.value(),
        geometry::KnifeCutPatch{source.revision(), face.value(),
            {{0.0, 0.0, 0.0},
             {std::numeric_limits<double>::infinity(), 1.0, 0.0},
             {2.0, 2.0, 0.0}}});
    REQUIRE(!non_finite && non_finite.error().code == core::ErrorCode::invalid_argument);
    const auto outside_face = document.apply_knife_cut_patch(asset.value(),
        geometry::KnifeCutPatch{source.revision(), face.value(),
            {{0.0, 0.0, 0.0}, {3.0, 1.0, 0.0}, {2.0, 2.0, 0.0}}});
    REQUIRE(!outside_face);
    REQUIRE(document.serialize() == before_rejections);
    REQUIRE(document.mesh_patches().empty());
    REQUIRE(document.topology_receipts().empty());

    REQUIRE(document.apply_knife_cut_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::KnifeCutPatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().face == face.value());
    REQUIRE(decoded_patch.value().path.size() == path.size());
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().face_count() == 2U);

    const std::string before_duplicate = document.serialize();
    const auto duplicate_source_patch = document.apply_knife_cut_patch(asset.value(), patch);
    REQUIRE(!duplicate_source_patch &&
        duplicate_source_patch.error().code == core::ErrorCode::invalid_state);
    REQUIRE(document.serialize() == before_duplicate);

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.01, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().face_count() == 2U);
}

void project_knife_cut_patch_transfers_edge_endpoint_attributes_and_replays() {
    using namespace carto;
    geometry::EditableMesh fixture;
    const auto a = fixture.add_vertex({0.0, 0.0, 0.0});
    const auto b = fixture.add_vertex({2.0, 0.0, 0.0});
    const auto c = fixture.add_vertex({2.0, 2.0, 0.0});
    const auto d = fixture.add_vertex({0.0, 2.0, 0.0});
    REQUIRE(a && b && c && d);
    const auto face = fixture.add_face({a.value(), b.value(), c.value(), d.value()});
    REQUIRE(face);
    const auto topology = fixture.topology();
    REQUIRE(topology);
    std::vector<attributes::AttributeValue> vertex_values;
    for (const auto& vertex : fixture.vertices_sorted()) {
        vertex_values.emplace_back(vertex.position.x + 10.0 * vertex.position.y);
    }
    auto corners = topology.value().corners;
    std::sort(corners.begin(), corners.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::vector<attributes::AttributeValue> uv_values;
    for (const auto& corner : corners) {
        const auto* vertex = fixture.find_vertex(corner.vertex);
        REQUIRE(vertex != nullptr);
        uv_values.emplace_back(core::Vec2d{vertex->position.x, vertex->position.y});
    }
    attributes::AttributeSet payload({
        {attributes::AttributeDomain::vertex, fixture.vertex_count()},
        {attributes::AttributeDomain::corner, corners.size()}});
    REQUIRE(payload.add_layer({
        {"vertex_signal", attributes::AttributeDomain::vertex, attributes::AttributeType::float64},
        std::move(vertex_values)}));
    REQUIRE(payload.add_layer({
        {"uv0", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        std::move(uv_values)}));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({1U, "UV0", "uv0", true, true, false, 1U}));
    REQUIRE(fixture.set_attribute_payload(std::move(payload), std::move(uv_sets)));

    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::KnifeCutPatch patch{
        source.revision(), face.value(),
        {{0.5, 0.0, 0.0}, {1.0, 0.75, 0.0}, {2.0, 0.5, 0.0}}};
    const auto decoded_edge_patch = geometry::KnifeCutPatch::deserialize(patch.serialize());
    REQUIRE(decoded_edge_patch);
    REQUIRE(decoded_edge_patch.value().path.size() == patch.path.size());
    REQUIRE(decoded_edge_patch.value().path.front().x == patch.path.front().x);
    REQUIRE(decoded_edge_patch.value().path.back().x == patch.path.back().x);
    const auto applied = document.apply_knife_cut_patch(asset.value(), patch);
    if (!applied) std::cerr << "attribute project knife refused: " <<
        applied.error().message << '\n';
    REQUIRE(applied);
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto& receipt = document.topology_receipts().front();
    REQUIRE(receipt.receipt.vertex_origins.size() == 3U);

    const auto& after = document.meshes().at(asset.value());
    REQUIRE(after.uv_sets().find(1U) != nullptr);
    const auto* transferred_vertices = after.attributes().find("vertex_signal");
    const auto* transferred_uvs = after.attributes().find("uv0");
    REQUIRE(transferred_vertices && transferred_uvs);
    std::size_t weighted_endpoints = 0U;
    const auto after_vertices = after.vertices_sorted();
    for (const auto& vertex : after_vertices) {
        const auto origin = receipt.receipt.vertex_origins.find(vertex.id);
        if (origin == receipt.receipt.vertex_origins.end()) continue;
        const auto value_index = static_cast<std::size_t>(std::distance(
            after_vertices.begin(), std::find_if(after_vertices.begin(), after_vertices.end(),
                [&](const auto& item) { return item.id == vertex.id; })));
        const double signal = std::get<double>(transferred_vertices->values[value_index]);
        if (origin->second.kind == geometry::OriginKind::interpolated_from) {
            REQUIRE(std::abs(signal - (vertex.position.x + 10.0 * vertex.position.y)) < 1e-12);
            ++weighted_endpoints;
        } else {
            REQUIRE(origin->second.kind == geometry::OriginKind::generated_from_face);
            REQUIRE(signal == 0.0);
        }
    }
    REQUIRE(weighted_endpoints == 2U);
    const auto after_topology = after.topology();
    REQUIRE(after_topology);
    auto after_corners = after_topology.value().corners;
    std::sort(after_corners.begin(), after_corners.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    for (std::size_t index = 0U; index < after_corners.size(); ++index) {
        const auto& corner = after_corners[index];
        const auto origin = receipt.receipt.corner_origins.find(corner.id);
        if (origin == receipt.receipt.corner_origins.end() ||
            origin->second.kind != geometry::OriginKind::interpolated_from) continue;
        const auto* vertex = after.find_vertex(corner.vertex);
        REQUIRE(vertex != nullptr);
        const auto uv = std::get<core::Vec2d>(transferred_uvs->values[index]);
        REQUIRE(std::abs(uv.x - vertex->position.x) < 1e-12);
        REQUIRE(std::abs(uv.y - vertex->position.y) < 1e-12);
    }

    const auto& record = document.mesh_patches().front();
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().attributes().find("vertex_signal") != nullptr);
    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto reopened_replay = reopened.value().mesh_patches().front().replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().attributes().find("uv0") != nullptr);
}

void application_knife_cut_tool_is_face_bound_and_repeatably_persistent() {
    using namespace carto;
    TempDirectory temp;
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(
        session, application::NewProjectAction{"Viewport Knife Cut"}));

    geometry::EditableMesh fixture;
    const auto a = fixture.add_vertex({0.0, 0.0, 0.0});
    const auto b = fixture.add_vertex({2.0, 0.0, 0.0});
    const auto c = fixture.add_vertex({2.0, 0.0, 2.0});
    const auto d = fixture.add_vertex({0.0, 0.0, 2.0});
    REQUIRE(a && b && c && d);
    const auto face = fixture.add_face({a.value(), b.value(), c.value(), d.value()});
    REQUIRE(face);
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Knife fixture", fixture}));

    auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    const scene::ObjectId object = snapshot.objects.front().object.id;
    REQUIRE(application::testing::dispatch(session,
        application::SelectObjectAction{object}));
    const core::Revision before_wrong_mode = session.snapshot().project_revision;
    editor::ToolArguments arguments;
    arguments.knife_path = {{0.0, 0.0, 0.0}, {1.0, 0.0, 1.0}, {2.0, 0.0, 2.0}};
    const auto wrong_mode = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.knife-cut", arguments});
    REQUIRE(!wrong_mode);
    REQUIRE(session.snapshot().project_revision == before_wrong_mode);

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectFaceAction{object, face.value()}));
    const core::Revision before_invalid_cut = session.snapshot().project_revision;
    arguments.knife_path = {{0.0, 0.0, 0.0}, {3.0, 0.0, 1.0}, {2.0, 0.0, 2.0}};
    const auto invalid_cut = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.knife-cut", arguments});
    REQUIRE(!invalid_cut);
    REQUIRE(session.snapshot().project_revision == before_invalid_cut);

    arguments.knife_path = {{0.5, 0.0, 0.0}, {1.0, 0.0, 1.0}, {2.0, 0.0, 1.0}};
    session.active_source_ = application::OperationSource::ai_proposal;
    const auto ai_attempt = session.invoke_tool(
        application::InvokeToolAction{"mesh.knife-cut", arguments});
    session.active_source_ = application::OperationSource::human;
    REQUIRE(!ai_attempt);
    REQUIRE(ai_attempt.error().code == core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before_invalid_cut);
    arguments.knife_path = {{0.5, 0.0, 0.0}, {1.0, 0.0, 1.0}, {2.0, 0.0, 1.0}};
    REQUIRE(application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.knife-cut", arguments}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision > before_invalid_cut);
    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    const auto path = temp.path() / "viewport-knife-cut.carto";
    REQUIRE(application::testing::dispatch(session, application::SaveProjectAction{path}));
    const auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 2U);
    REQUIRE(reopened.value().topology_receipts().size() == 2U);
    const auto& record = reopened.value().mesh_patches().back();
    REQUIRE(record.validate());
    const auto patch = geometry::KnifeCutPatch::deserialize(record.patch_bytes);
    REQUIRE(patch);
    REQUIRE(patch.value().face == face.value());
    REQUIRE(patch.value().path.size() == 3U);
    auto source_mesh = fixture;
    REQUIRE(source_mesh.restore_revision(patch.value().expected_revision));
    const auto replayed = record.replay(source_mesh);
    REQUIRE(replayed);
    REQUIRE(replayed.value().face_count() == 2U);
}

void project_bevel_edge_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    auto source_result = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(source_result);
    project::ProjectDocument document;
    const auto asset = document.add_mesh(source_result.value());
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const auto edge = source.edges_sorted(1U).front().id;
    const std::string before_rejections = document.serialize();

    const auto stale = document.apply_bevel_edge_patch(asset.value(),
        geometry::BevelEdgePatch{source.revision().next(), edge, 0.1, 2U});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto missing = document.apply_bevel_edge_patch(asset.value(),
        geometry::BevelEdgePatch{source.revision(), geometry::EdgeId{999999U}, 0.1, 2U});
    REQUIRE(!missing && missing.error().code == core::ErrorCode::not_found);
    const auto invalid_segments = document.apply_bevel_edge_patch(asset.value(),
        geometry::BevelEdgePatch{source.revision(), edge, 0.1, 65U});
    REQUIRE(!invalid_segments &&
        invalid_segments.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(document.serialize() == before_rejections);

    const geometry::BevelEdgePatch patch{source.revision(), edge, 0.1, 2U};
    REQUIRE(document.apply_bevel_edge_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::BevelEdgePatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().edge == edge);
    REQUIRE(decoded_patch.value().width == patch.width);
    REQUIRE(decoded_patch.value().segments == patch.segments);
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertex_count() ==
        document.meshes().at(asset.value()).vertex_count());
    REQUIRE(replayed.value().face_count() ==
        document.meshes().at(asset.value()).face_count());

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().front();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_bevel_vertex_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    auto source_result = make_application_square_pyramid();
    REQUIRE(source_result);
    project::ProjectDocument document;
    const auto asset = document.add_mesh(source_result.value());
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::VertexId vertex{1U};
    const std::string before_rejections = document.serialize();

    const auto stale = document.apply_bevel_vertex_patch(asset.value(),
        geometry::BevelVertexPatch{source.revision().next(), vertex, 0.25, 3U});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto missing = document.apply_bevel_vertex_patch(asset.value(),
        geometry::BevelVertexPatch{source.revision(), geometry::VertexId{999999U}, 0.25, 3U});
    REQUIRE(!missing && missing.error().code == core::ErrorCode::not_found);
    const auto invalid_width = document.apply_bevel_vertex_patch(asset.value(),
        geometry::BevelVertexPatch{source.revision(), vertex, 0.0, 3U});
    REQUIRE(!invalid_width &&
        invalid_width.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(document.serialize() == before_rejections);

    const geometry::BevelVertexPatch patch{source.revision(), vertex, 0.25, 3U};
    REQUIRE(document.apply_bevel_vertex_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::BevelVertexPatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().vertex == vertex);
    REQUIRE(decoded_patch.value().width == patch.width);
    REQUIRE(decoded_patch.value().segments == patch.segments);
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertex_count() ==
        document.meshes().at(asset.value()).vertex_count());
    REQUIRE(replayed.value().face_count() ==
        document.meshes().at(asset.value()).face_count());

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_face_extrusion_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    auto fixture = make_application_dissolve_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture.mesh);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::FaceId face{1U};
    const std::string before_rejections = document.serialize();

    const auto stale = document.apply_face_extrusion_patch(asset.value(),
        geometry::FaceExtrusionPatch{source.revision().next(), face, 0.25});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto missing = document.apply_face_extrusion_patch(asset.value(),
        geometry::FaceExtrusionPatch{source.revision(), geometry::FaceId{999999U}, 0.25});
    REQUIRE(!missing && missing.error().code == core::ErrorCode::not_found);
    const auto invalid_distance = document.apply_face_extrusion_patch(asset.value(),
        geometry::FaceExtrusionPatch{source.revision(), face, 0.0});
    REQUIRE(!invalid_distance &&
        invalid_distance.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(document.serialize() == before_rejections);

    const geometry::FaceExtrusionPatch patch{source.revision(), face, 0.25};
    REQUIRE(document.apply_face_extrusion_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::FaceExtrusionPatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().face == face);
    REQUIRE(decoded_patch.value().distance == patch.distance);
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertex_count() ==
        document.meshes().at(asset.value()).vertex_count());
    REQUIRE(replayed.value().face_count() == document.meshes().at(asset.value()).face_count());

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_face_inset_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    auto fixture = make_application_tri_to_quad_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture.mesh);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::FaceId face = fixture.first_face;
    const std::string before_rejections = document.serialize();

    const auto missing_asset = document.apply_face_inset_patch(999999U,
        geometry::FaceInsetPatch{source.revision(), face, 0.1});
    REQUIRE(!missing_asset && missing_asset.error().code == core::ErrorCode::not_found);
    const auto stale = document.apply_face_inset_patch(asset.value(),
        geometry::FaceInsetPatch{source.revision().next(), face, 0.1});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto missing_face = document.apply_face_inset_patch(asset.value(),
        geometry::FaceInsetPatch{source.revision(), geometry::FaceId{999999U}, 0.1});
    REQUIRE(!missing_face && missing_face.error().code == core::ErrorCode::not_found);
    const auto invalid_distance = document.apply_face_inset_patch(asset.value(),
        geometry::FaceInsetPatch{source.revision(), face, 0.0});
    REQUIRE(!invalid_distance &&
        invalid_distance.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(document.serialize() == before_rejections);

    const geometry::FaceInsetPatch patch{source.revision(), face, 0.1};
    REQUIRE(document.apply_face_inset_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::FaceInsetPatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().face == face);
    REQUIRE(decoded_patch.value().distance == patch.distance);
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertex_count() ==
        document.meshes().at(asset.value()).vertex_count());
    REQUIRE(replayed.value().face_count() ==
        document.meshes().at(asset.value()).face_count());

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_face_poke_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    auto fixture = make_application_dissolve_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture.mesh);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::FaceId face{1U};
    const std::string before_rejections = document.serialize();

    const auto missing_asset = document.apply_face_poke_patch(999999U,
        geometry::FacePokePatch{source.revision(), face});
    REQUIRE(!missing_asset && missing_asset.error().code == core::ErrorCode::not_found);
    const auto stale = document.apply_face_poke_patch(asset.value(),
        geometry::FacePokePatch{source.revision().next(), face});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto missing_face = document.apply_face_poke_patch(asset.value(),
        geometry::FacePokePatch{source.revision(), geometry::FaceId{999999U}});
    REQUIRE(!missing_face && missing_face.error().code == core::ErrorCode::not_found);
    REQUIRE(document.serialize() == before_rejections);

    const geometry::FacePokePatch patch{source.revision(), face};
    REQUIRE(document.apply_face_poke_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::FacePokePatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().face == face);
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertex_count() ==
        document.meshes().at(asset.value()).vertex_count());
    REQUIRE(replayed.value().face_count() ==
        document.meshes().at(asset.value()).face_count());

    const std::string before_duplicate = document.serialize();
    const auto duplicate_source_patch = document.apply_face_poke_patch(asset.value(), patch);
    REQUIRE(!duplicate_source_patch &&
        duplicate_source_patch.error().code == core::ErrorCode::invalid_state);
    REQUIRE(document.serialize() == before_duplicate);

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_tri_to_quad_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    const auto fixture = make_application_tri_to_quad_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture.mesh);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::TriToQuadPatch patch{
        source.revision(), fixture.first_face, fixture.second_face};
    const std::string before_rejections = document.serialize();

    const auto missing_asset = document.apply_tri_to_quad_patch(999999U, patch);
    REQUIRE(!missing_asset && missing_asset.error().code == core::ErrorCode::not_found);
    const auto stale = document.apply_tri_to_quad_patch(asset.value(),
        geometry::TriToQuadPatch{
            source.revision().next(), fixture.first_face, fixture.second_face});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto same_face = document.apply_tri_to_quad_patch(asset.value(),
        geometry::TriToQuadPatch{source.revision(), fixture.first_face, fixture.first_face});
    REQUIRE(!same_face && same_face.error().code == core::ErrorCode::invalid_argument);
    const auto missing_face = document.apply_tri_to_quad_patch(asset.value(),
        geometry::TriToQuadPatch{
            source.revision(), fixture.first_face, geometry::FaceId{999999U}});
    REQUIRE(!missing_face && missing_face.error().code == core::ErrorCode::not_found);
    REQUIRE(document.serialize() == before_rejections);
    REQUIRE(document.mesh_patches().empty());
    REQUIRE(document.topology_receipts().empty());

    REQUIRE(document.apply_tri_to_quad_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::TriToQuadPatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE((decoded_patch.value().first_face ==
        std::min(fixture.first_face, fixture.second_face)));
    REQUIRE((decoded_patch.value().second_face ==
        std::max(fixture.first_face, fixture.second_face)));
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().face_count() == document.meshes().at(asset.value()).face_count());

    const std::string before_duplicate = document.serialize();
    const auto duplicate_source_patch = document.apply_tri_to_quad_patch(asset.value(), patch);
    REQUIRE(!duplicate_source_patch &&
        duplicate_source_patch.error().code == core::ErrorCode::invalid_state);
    REQUIRE(document.serialize() == before_duplicate);

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    const auto reopened_replay = reopened_record.replay(source);
    REQUIRE(reopened_replay);
    REQUIRE(reopened_replay.value().revision() ==
        reopened.value().meshes().at(asset.value()).revision());
}

void project_edge_dissolve_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    const auto fixture = make_application_dissolve_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture.mesh);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::EdgeDissolvePatch patch{
        source.revision(), {fixture.internal_edge}};
    const std::string before_rejections = document.serialize();

    const auto missing_asset = document.apply_edge_dissolve_patch(999999U, patch);
    REQUIRE(!missing_asset && missing_asset.error().code == core::ErrorCode::not_found);
    const auto stale = document.apply_edge_dissolve_patch(asset.value(),
        geometry::EdgeDissolvePatch{
            source.revision().next(), {fixture.internal_edge}});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto empty = document.apply_edge_dissolve_patch(asset.value(),
        geometry::EdgeDissolvePatch{source.revision(), {}});
    REQUIRE(!empty && empty.error().code == core::ErrorCode::invalid_argument);
    const auto missing_edge = document.apply_edge_dissolve_patch(asset.value(),
        geometry::EdgeDissolvePatch{source.revision(), {geometry::EdgeId{999999U}}});
    REQUIRE(!missing_edge && missing_edge.error().code == core::ErrorCode::not_found);
    const auto boundary_edge = document.apply_edge_dissolve_patch(asset.value(),
        geometry::EdgeDissolvePatch{source.revision(), {fixture.boundary_edge}});
    REQUIRE(!boundary_edge && boundary_edge.error().code == core::ErrorCode::unsupported);
    REQUIRE(document.serialize() == before_rejections);
    REQUIRE(document.mesh_patches().empty());
    REQUIRE(document.topology_receipts().empty());

    REQUIRE(document.apply_edge_dissolve_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::EdgeDissolvePatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().edges == std::vector<geometry::EdgeId>{fixture.internal_edge});
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().face_count() == document.meshes().at(asset.value()).face_count());

    const std::string before_duplicate = document.serialize();
    const auto duplicate_source_patch = document.apply_edge_dissolve_patch(asset.value(), patch);
    REQUIRE(!duplicate_source_patch &&
        duplicate_source_patch.error().code == core::ErrorCode::invalid_state);
    REQUIRE(document.serialize() == before_duplicate);

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    REQUIRE(reopened_record.replay(source));
}

void application_mesh_cleanup_reviews_preserve_authority_and_durable_lineage() {
    using namespace carto;
    TempDirectory temp;
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session, application::CreateMeshObjectAction{
        "Cleanup seam", make_application_merge_distance_fixture()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(application::testing::dispatch(session, application::SetSelectionModeAction{
        editor::SelectionMode::vertex}));
    REQUIRE(application::testing::dispatch(session, application::SelectVertexAction{
        object.id, geometry::VertexId{1U}}));
    REQUIRE(application::testing::dispatch(session, application::SelectVertexAction{
        object.id, geometry::VertexId{5U}, editor::SelectionOperation::add}));
    auto moved_transform = object.local_transform;
    moved_transform.translation.x = 2.0;
    REQUIRE(application::testing::dispatch(session, application::SetObjectTransformAction{
        object.id, moved_transform, std::nullopt}));
    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    const auto path = temp.path() / "reviewed-cleanup.carto";
    REQUIRE(application::testing::dispatch(session, application::SaveProjectAction{path}));
    journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries_before = journal.read_all();
    REQUIRE(entries_before);
    const auto before = session.document_.serialize();
    const auto before_snapshot = session.snapshot();
    application::MeshCleanupOptions options;
    options.merge_distance = 1.0e-4;
    const std::vector<geometry::VertexId> candidates{{5U}, {2U}, {4U}, {1U}};
    auto review = session.review_mesh_cleanup(object.id, candidates, options);
    REQUIRE(review && review.value().active());
    REQUIRE((review.value().candidates() == std::vector<geometry::VertexId>{{1U}, {2U}, {4U}, {5U}}));
    REQUIRE(review.value().receipt().merged_vertices.at(geometry::VertexId{5U}) == geometry::VertexId{1U});
    REQUIRE(review.value().receipt().merged_vertices.at(geometry::VertexId{4U}) == geometry::VertexId{2U});
    REQUIRE((review.value().modified_faces() == std::vector<geometry::FaceId>{{2U}}));
    REQUIRE(session.document_.serialize() == before);
    REQUIRE(session.snapshot().undo_count == before_snapshot.undo_count);
    REQUIRE(journal.read_all().value().size() == entries_before.value().size());
    for (const auto* reason : {"", "   ", "reason\nwith control"}) {
        REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(session, review.value(), reason));
        REQUIRE(review.value().active() && session.document_.serialize() == before);
    }
    session.active_source_ = application::OperationSource::ai_proposal;
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(session, review.value(), "AI cannot approve"));
    session.active_source_ = application::OperationSource::human;
    REQUIRE(review.value().active() && session.document_.serialize() == before);

    // Force append failure after an otherwise admissible proposal. The same
    // token remains usable once the journal configuration is restored.
    journal::JournalLimits limits;
    limits.max_payload_bytes = 1U;
    session.journal_.emplace(journal.path(), limits);
    const auto selection_before_failure = session.selection_.component_groups();
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(session, review.value(), "Join duplicate seam"));
    REQUIRE(review.value().active() && session.document_.serialize() == before);
    REQUIRE(session.snapshot().undo_count == before_snapshot.undo_count);
    REQUIRE(session.snapshot().redo_count == before_snapshot.redo_count);
    REQUIRE(session.selection_.component_groups() == selection_before_failure);
    REQUIRE(journal.read_all().value().size() == entries_before.value().size());
    session.journal_.emplace(journal.path());

    const auto expected_digest = review.value().result_digest();
    auto moved = std::move(review.value());
    REQUIRE(!review.value().active() && moved.active());
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(session, review.value(), "Moved token"));
    const auto committed = application::HumanApplicationAccess::commit_mesh_cleanup(session, moved, "Join duplicate seam");
    REQUIRE(committed && !moved.active());
    REQUIRE(committed.value().source == application::OperationSource::human);
    REQUIRE(session.snapshot().undo_count == before_snapshot.undo_count + 1U);
    REQUIRE(session.snapshot().redo_count == 0U);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(session.document_.topology_receipts().size() == 1U);
    REQUIRE(application::canonical_mesh_geometry_digest(session.document_.meshes().at(*object.mesh_asset)) == expected_digest);
    const auto entries = journal.read_all();
    REQUIRE(entries && entries.value().size() == entries_before.value().size() + 1U);
    const std::string payload(entries.value().back().payload.begin(), entries.value().back().payload.end());
    REQUIRE(payload.find("Join duplicate seam") != std::string::npos);
    REQUIRE(payload.find(expected_digest.hex()) != std::string::npos);
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(session, moved, "Repeated token"));
    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).vertex_count() == 6U);
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    REQUIRE(application::canonical_mesh_geometry_digest(session.document_.meshes().at(*object.mesh_asset)) == expected_digest);
    REQUIRE(application::testing::dispatch(session, application::SaveProjectAction{}));
    const auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened && reopened.value().mesh_patches().size() == 1U);
    REQUIRE(application::canonical_mesh_geometry_digest(reopened.value().meshes().at(*object.mesh_asset)) == expected_digest);
}

void application_mesh_cleanup_refuses_stale_foreign_and_overbudget_reviews() {
    using namespace carto;
    static_assert(!std::is_constructible_v<application::ApplicationAction, application::MeshCleanupReview>);
    application::MeshCleanupOptions options;
    options.merge_distance = 1.0e-4;
    const std::vector<geometry::VertexId> candidates{{1U}, {2U}, {4U}, {5U}};
    const auto populate = [](application::ApplicationSession& session) {
        REQUIRE(application::testing::dispatch(session, application::CreateMeshObjectAction{
            "Cleanup seam", make_application_merge_distance_fixture()}));
        return session.snapshot().objects.front().object.id;
    };
    application::ApplicationSession session;
    const auto object = populate(session);
    const auto before = session.document_.serialize();
    auto limited = options;
    limited.max_candidates = 2U;
    REQUIRE(!session.review_mesh_cleanup(object, candidates, limited));
    limited = options;
    limited.max_merged_vertices = 1U;
    REQUIRE(!session.review_mesh_cleanup(object, candidates, limited));
    limited = options;
    limited.merge_distance = std::numeric_limits<double>::infinity();
    REQUIRE(!session.review_mesh_cleanup(object, candidates, limited));
    REQUIRE(!session.review_mesh_cleanup(object, {{1U}, {1U}}, options));
    REQUIRE(!session.review_mesh_cleanup(object, {{1U}, {99999U}}, options));
    REQUIRE(session.document_.serialize() == before);
    auto review = session.review_mesh_cleanup(object, candidates, options);
    REQUIRE(review);
    application::ApplicationSession foreign;
    populate(foreign);
    const auto foreign_before = foreign.document_.serialize();
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(foreign, review.value(), "Wrong session"));
    REQUIRE(foreign.document_.serialize() == foreign_before && review.value().active());
    auto cancelled = session.review_mesh_cleanup(object, candidates, options);
    REQUIRE(cancelled);
    cancelled.value().cancel();
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(session, cancelled.value(), "Cancelled"));
    REQUIRE(application::testing::dispatch(session, application::CreatePlaneAction{"Concurrent change"}));
    const auto changed = session.document_.serialize();
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(session, review.value(), "Stale review"));
    REQUIRE(session.document_.serialize() == changed);

    std::optional<application::ApplicationSession> reused(std::in_place);
    const auto first_id = populate(*reused);
    auto expired = reused->review_mesh_cleanup(first_id, candidates, options);
    REQUIRE(expired);
    const auto* address = &*reused;
    reused.reset();
    reused.emplace();
    REQUIRE(&*reused == address);
    populate(*reused);
    const auto reused_before = reused->document_.serialize();
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(*reused, expired.value(), "Expired session"));
    REQUIRE(reused->document_.serialize() == reused_before);
    application::ApplicationSession identityless;
    populate(identityless);
    identityless.cleanup_review_identity_.reset(); // adversarial moved-from identity
    REQUIRE(!expired.value().session_identity_.lock());
    REQUIRE(expired.value().project_generation() == identityless.project_generation_);
    REQUIRE(expired.value().project_revision() == identityless.document_.revision());
    const auto identityless_before = identityless.document_.serialize();
    REQUIRE(!identityless.review_mesh_cleanup(
        identityless.snapshot().objects.front().object.id, candidates, options));
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(
        identityless, expired.value(), "Two absent identities cannot authorize"));
    REQUIRE(identityless.document_.serialize() == identityless_before && expired.value().active());

    auto generation_review = foreign.review_mesh_cleanup(
        foreign.snapshot().objects.front().object.id, candidates, options);
    REQUIRE(generation_review);
    ++foreign.project_generation_; // simulate a project replacement at the same revision
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(
        foreign, generation_review.value(), "Old project generation"));
    REQUIRE(foreign.document_.serialize() == foreign_before);

    application::ApplicationSession shared;
    const auto shared_id = populate(shared);
    auto shared_review = shared.review_mesh_cleanup(shared_id, candidates, options);
    REQUIRE(shared_review);
    REQUIRE(application::testing::dispatch(shared, application::DuplicateMeshInstanceAction{
        shared_id, "Shared instance"}));
    REQUIRE(!shared.review_mesh_cleanup(shared_id, candidates, options));
    const auto shared_before = shared.document_.serialize();
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(shared, shared_review.value(), "Shared after review"));
    REQUIRE(shared.document_.serialize() == shared_before);
    REQUIRE(application::testing::dispatch(shared, application::SetSelectionModeAction{
        editor::SelectionMode::vertex}));
    REQUIRE(application::testing::dispatch(shared, application::SelectVertexAction{
        shared_id, geometry::VertexId{1U}}));
    REQUIRE(application::testing::dispatch(shared, application::SelectVertexAction{
        shared_id, geometry::VertexId{5U}, editor::SelectionOperation::add}));
    editor::ToolArguments merge_arguments;
    merge_arguments.distance = options.merge_distance;
    const auto shared_history = shared.snapshot().undo_count;
    REQUIRE(!application::testing::dispatch(shared, application::InvokeToolAction{
        "mesh.merge-by-distance", merge_arguments}));
    REQUIRE(shared.document_.serialize() == shared_before);
    REQUIRE(shared.snapshot().undo_count == shared_history);
    auto locked_review = foreign.review_mesh_cleanup(
        foreign.snapshot().objects.front().object.id, candidates, options);
    REQUIRE(locked_review);
    auto* locked = const_cast<scene::SceneObject*>(foreign.document_.scene().find(foreign.snapshot().objects.front().object.id));
    locked->locked = true; // adversarial fixture: no revision change
    const auto locked_before = foreign.document_.serialize();
    REQUIRE(!foreign.review_mesh_cleanup(locked->id, candidates, options));
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(
        foreign, locked_review.value(), "Locked after review"));
    REQUIRE(foreign.document_.serialize() == locked_before);
    locked->locked = false;
    auto hidden_review = foreign.review_mesh_cleanup(locked->id, candidates, options);
    REQUIRE(hidden_review);
    locked->visible = false; // adversarial fixture: no revision change
    const auto hidden_before = foreign.document_.serialize();
    REQUIRE(!foreign.review_mesh_cleanup(locked->id, candidates, options));
    REQUIRE(!application::HumanApplicationAccess::commit_mesh_cleanup(
        foreign, hidden_review.value(), "Hidden after review"));
    REQUIRE(foreign.document_.serialize() == hidden_before && hidden_review.value().active());

    application::ApplicationSession drifted;
    const auto drifted_id = populate(drifted);
    auto drifted_review = drifted.review_mesh_cleanup(drifted_id, candidates, options);
    REQUIRE(drifted_review);
    auto& drifted_mesh = const_cast<geometry::EditableMesh&>(drifted.document_.meshes().at(
        *drifted.document_.scene().find(drifted_id)->mesh_asset));
    const auto old_revision = drifted_mesh.revision();
    REQUIRE(drifted_mesh.set_vertex_position(geometry::VertexId{6U}, {0.5, -2.0, 0.0}));
    REQUIRE(drifted_mesh.restore_revision(old_revision)); // adversarial same-revision source drift
    const auto drifted_before = drifted.document_.serialize();
    REQUIRE(drifted.review_mesh_cleanup(drifted_id, candidates, options));
    const auto drifted_commit = application::HumanApplicationAccess::commit_mesh_cleanup(
        drifted, drifted_review.value(), "Changed source geometry");
    REQUIRE(!drifted_commit && drifted_commit.error().code == core::ErrorCode::stale_data);
    REQUIRE(drifted.document_.serialize() == drifted_before && drifted_review.value().active());

    auto attributed = make_application_merge_distance_fixture();
    attributes::AttributeSet payload({{attributes::AttributeDomain::vertex, attributed.vertex_count()}});
    REQUIRE(payload.add_layer({{"keep", attributes::AttributeDomain::vertex,
        attributes::AttributeType::int32}, std::vector<attributes::AttributeValue>(
            attributed.vertex_count(), std::int32_t{7})}));
    REQUIRE(attributed.set_attribute_payload(std::move(payload), {}));
    application::ApplicationSession attributed_session;
    REQUIRE(application::testing::dispatch(attributed_session, application::CreateMeshObjectAction{
        "Attributed cleanup source", std::move(attributed)}));
    const auto attributed_before = attributed_session.document_.serialize();
    REQUIRE(!attributed_session.review_mesh_cleanup(
        attributed_session.snapshot().objects.front().object.id, candidates, options));
    REQUIRE(attributed_session.document_.serialize() == attributed_before);

    application::ApplicationSession oversized;
    geometry::EditableMesh large;
    std::vector<geometry::Vertex> vertices;
    vertices.reserve(application::MeshCleanupOptions::kMaxMeshElements + 1U);
    for (std::size_t index = 0U; index <= application::MeshCleanupOptions::kMaxMeshElements; ++index) {
        vertices.push_back({geometry::VertexId{index + 1U}, {static_cast<double>(index), 0.0, 0.0}});
    }
    REQUIRE(large.insert_bulk(std::move(vertices), {}));
    REQUIRE(application::testing::dispatch(oversized, application::CreateMeshObjectAction{
        "Over budget cleanup", std::move(large)}));
    const auto over_before = oversized.document_.serialize();
    REQUIRE(!oversized.review_mesh_cleanup(oversized.snapshot().objects.front().object.id,
        {{1U}, {2U}}, options));
    REQUIRE(oversized.document_.serialize() == over_before);
}

void application_mesh_cleanup_enforces_face_removal_and_corner_budgets() {
    using namespace carto;
    geometry::EditableMesh triangle;
    REQUIRE(triangle.insert_bulk({{{1U}, {0.0, 0.0, 0.0}},
        {{2U}, {1.0, 0.0, 0.0}}, {{3U}, {0.0, 1.0, 0.0}}},
        {{{1U}, {{1U}, {2U}, {3U}}}}));
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session, application::CreateMeshObjectAction{
        "Face removal budget", std::move(triangle)}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto before = session.document_.serialize();
    application::MeshCleanupOptions options;
    options.merge_distance = 1.01;
    options.max_removed_faces = 0U;
    const auto refused = session.review_mesh_cleanup(object, {{1U}, {2U}}, options);
    REQUIRE(!refused && refused.error().code == core::ErrorCode::unsupported);
    options.max_removed_faces = 1U;
    const auto accepted = session.review_mesh_cleanup(object, {{1U}, {2U}}, options);
    REQUIRE(accepted);
    REQUIRE((accepted.value().receipt().removed_faces == std::vector<geometry::FaceId>{{1U}}));
    REQUIRE(session.document_.serialize() == before);

    // V/E/F fit under the cap; corners alone put this valid triangle fan over it.
    constexpr std::size_t ring_count = 15000U;
    std::vector<geometry::Vertex> vertices{{{1U}, {0.0, 0.0, 0.0}}};
    std::vector<geometry::Face> faces;
    vertices.reserve(ring_count + 1U);
    faces.reserve(ring_count);
    for (std::size_t index = 0U; index < ring_count; ++index) {
        const double angle = 6.283185307179586 * static_cast<double>(index) / ring_count;
        vertices.push_back({geometry::VertexId{index + 2U}, {std::cos(angle), std::sin(angle), 0.0}});
        faces.push_back({geometry::FaceId{index + 1U}, {{1U},
            geometry::VertexId{index + 2U}, geometry::VertexId{(index + 1U) % ring_count + 2U}}});
    }
    geometry::EditableMesh fan;
    REQUIRE(fan.insert_bulk(std::move(vertices), std::move(faces)));
    const auto subtotal = fan.vertex_count() + fan.edge_count() + fan.face_count();
    REQUIRE(subtotal < application::MeshCleanupOptions::kMaxMeshElements);
    REQUIRE(subtotal + fan.corners_sorted(application::MeshCleanupOptions::kMaxMeshElements).size() >
        application::MeshCleanupOptions::kMaxMeshElements);
    application::ApplicationSession corner_limited;
    REQUIRE(application::testing::dispatch(corner_limited, application::CreateMeshObjectAction{
        "Corner budget", std::move(fan)}));
    const auto corner_before = corner_limited.document_.serialize();
    const auto corner_refusal = corner_limited.review_mesh_cleanup(
        corner_limited.snapshot().objects.front().object.id, {{2U}, {3U}}, options);
    REQUIRE(!corner_refusal && corner_refusal.error().code == core::ErrorCode::unsupported);
    REQUIRE(corner_refusal.error().message.find("corner budget") != std::string::npos);
    REQUIRE(corner_limited.document_.serialize() == corner_before);
}

void project_merge_by_distance_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    const geometry::EditableMesh fixture = make_application_merge_distance_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::MergeByDistancePatch patch{
        source.revision(),
        {geometry::VertexId{1U}, geometry::VertexId{2U},
            geometry::VertexId{4U}, geometry::VertexId{5U}},
        1e-4};
    const std::string before_rejections = document.serialize();

    const auto missing_asset = document.apply_merge_by_distance_patch(999999U, patch);
    REQUIRE(!missing_asset && missing_asset.error().code == core::ErrorCode::not_found);
    const auto stale = document.apply_merge_by_distance_patch(asset.value(),
        geometry::MergeByDistancePatch{
            source.revision().next(), patch.candidates, patch.distance});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto too_few = document.apply_merge_by_distance_patch(asset.value(),
        geometry::MergeByDistancePatch{
            source.revision(), {geometry::VertexId{1U}}, patch.distance});
    REQUIRE(!too_few && too_few.error().code == core::ErrorCode::invalid_argument);
    const auto duplicate = document.apply_merge_by_distance_patch(asset.value(),
        geometry::MergeByDistancePatch{
            source.revision(), {geometry::VertexId{1U}, geometry::VertexId{1U}},
            patch.distance});
    REQUIRE(!duplicate && duplicate.error().code == core::ErrorCode::invalid_argument);
    const auto missing_vertex = document.apply_merge_by_distance_patch(asset.value(),
        geometry::MergeByDistancePatch{
            source.revision(), {geometry::VertexId{1U}, geometry::VertexId{999999U}},
            patch.distance});
    REQUIRE(!missing_vertex && missing_vertex.error().code == core::ErrorCode::not_found);
    const auto invalid_distance = document.apply_merge_by_distance_patch(asset.value(),
        geometry::MergeByDistancePatch{source.revision(), patch.candidates, 0.0});
    REQUIRE(!invalid_distance &&
        invalid_distance.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(document.serialize() == before_rejections);
    REQUIRE(document.mesh_patches().empty());
    REQUIRE(document.topology_receipts().empty());

    REQUIRE(document.apply_merge_by_distance_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    REQUIRE(record.source_mesh_digest != record.result_mesh_digest);
    const auto decoded_patch = geometry::MergeByDistancePatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().distance == patch.distance);
    REQUIRE(decoded_patch.value().candidates == std::vector<geometry::VertexId>({
        geometry::VertexId{1U}, geometry::VertexId{2U},
        geometry::VertexId{4U}, geometry::VertexId{5U}}));
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertex_count() == document.meshes().at(asset.value()).vertex_count());

    const std::string before_duplicate = document.serialize();
    const auto duplicate_source_patch = document.apply_merge_by_distance_patch(
        asset.value(), patch);
    REQUIRE(!duplicate_source_patch &&
        duplicate_source_patch.error().code == core::ErrorCode::invalid_state);
    REQUIRE(document.serialize() == before_duplicate);

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    REQUIRE(reopened_record.replay(source));
}

void project_vertex_merge_patch_is_digest_bound_and_replayable() {
    using namespace carto;
    const geometry::EditableMesh fixture = make_application_weld_fixture();
    project::ProjectDocument document;
    const auto asset = document.add_mesh(fixture);
    REQUIRE(asset);
    const geometry::EditableMesh source = document.meshes().at(asset.value());
    const geometry::VertexMergePatch patch{
        source.revision(), geometry::VertexId{1U}, geometry::VertexId{2U},
        geometry::VertexMergeMode::center};
    const std::string before_rejections = document.serialize();

    const auto missing_asset = document.apply_vertex_merge_patch(999999U, patch);
    REQUIRE(!missing_asset && missing_asset.error().code == core::ErrorCode::not_found);
    const auto stale = document.apply_vertex_merge_patch(asset.value(),
        geometry::VertexMergePatch{
            source.revision().next(), patch.target, patch.source, patch.mode});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    const auto same_vertex = document.apply_vertex_merge_patch(asset.value(),
        geometry::VertexMergePatch{
            source.revision(), patch.target, patch.target, patch.mode});
    REQUIRE(!same_vertex &&
        same_vertex.error().code == core::ErrorCode::invalid_argument);
    const auto missing_vertex = document.apply_vertex_merge_patch(asset.value(),
        geometry::VertexMergePatch{
            source.revision(), patch.target, geometry::VertexId{999999U}, patch.mode});
    REQUIRE(!missing_vertex &&
        missing_vertex.error().code == core::ErrorCode::not_found);
    REQUIRE(document.serialize() == before_rejections);
    REQUIRE(document.mesh_patches().empty());
    REQUIRE(document.topology_receipts().empty());

    REQUIRE(document.apply_vertex_merge_patch(asset.value(), patch));
    REQUIRE(document.mesh_patches().size() == 1U);
    REQUIRE(document.topology_receipts().size() == 1U);
    const auto& record = document.mesh_patches().front();
    REQUIRE(record.validate());
    REQUIRE(!record.patch_digest.is_zero());
    REQUIRE(!record.source_mesh_digest.is_zero());
    REQUIRE(!record.result_mesh_digest.is_zero());
    const auto decoded_patch = geometry::VertexMergePatch::deserialize(record.patch_bytes);
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch.value().expected_revision == source.revision());
    REQUIRE(decoded_patch.value().target == patch.target);
    REQUIRE(decoded_patch.value().source == patch.source);
    REQUIRE(decoded_patch.value().mode == geometry::VertexMergeMode::center);
    const auto replayed = record.replay(source);
    REQUIRE(replayed);
    REQUIRE(replayed.value().revision() == document.meshes().at(asset.value()).revision());
    REQUIRE(replayed.value().vertex_count() == document.meshes().at(asset.value()).vertex_count());
    const auto* merged_target = replayed.value().find_vertex(patch.target);
    REQUIRE(merged_target != nullptr);
    REQUIRE(merged_target->position.x == 0.5);

    auto wrong_source = source;
    const auto unrelated_vertex = wrong_source.vertices_sorted().back();
    REQUIRE(wrong_source.set_vertex_position(unrelated_vertex.id,
        unrelated_vertex.position + core::Vec3d{0.001, 0.0, 0.0}));
    REQUIRE(wrong_source.restore_revision(source.revision()));
    const auto wrong_source_replay = record.replay(wrong_source);
    REQUIRE(!wrong_source_replay &&
        wrong_source_replay.error().code == core::ErrorCode::stale_data);

    auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 1U);
    REQUIRE(reopened.value().topology_receipts().size() == 1U);
    const auto& reopened_record = reopened.value().mesh_patches().front();
    REQUIRE(reopened_record.patch_digest == record.patch_digest);
    REQUIRE(reopened_record.source_mesh_digest == record.source_mesh_digest);
    REQUIRE(reopened_record.result_mesh_digest == record.result_mesh_digest);
    REQUIRE(reopened_record.replay(source));
}

void project_selected_edge_split_redo_records_a_replayable_patch() {
    TempDirectory temp;
    const auto path = temp.path() / "project-edge-split-patch.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Edge split", {1.0, 1.0, 1.0}}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 1U);
    const auto object = initial.objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto source = session.document_.meshes().at(*object.mesh_asset);
    const auto edge = source.edges_sorted(1U).front().id;

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{object.id, edge}));
    carto::editor::ToolArguments arguments;
    arguments.factor = 0.375;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.split-edge", arguments}));
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(session.document_.topology_receipts().size() == 1U);
    REQUIRE(session.document_.mesh_patches().back().replay(source));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded && loaded.value().mesh_patches().size() == 1U);
    REQUIRE(loaded.value().mesh_patches().front().replay(source));

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto redo_source = session.document_.meshes().at(*object.mesh_asset);
    REQUIRE(session.document_.mesh_patches().size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().back().replay(redo_source));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded && loaded.value().mesh_patches().size() == 2U);
    REQUIRE(loaded.value().mesh_patches().back().replay(redo_source));
}

void project_edge_split_across_objects_is_atomic_and_replayable() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-edge-split.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"First box", {1.0, 1.0, 1.0}}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Second box", {1.0, 1.0, 1.0}}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value());
    REQUIRE(second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);
    const auto first_edge = first_source.edges_sorted(1U).front().id;
    const auto second_edge = second_source.edges_sorted(1U).front().id;

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{first.id, first_edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{second.id, second_edge,
            carto::editor::SelectionOperation::add}));
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);

    carto::editor::ToolArguments arguments;
    arguments.factor = 0.375;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.split-edge", arguments}));
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches()[0].replay(first_source));
    REQUIRE(session.document_.mesh_patches()[1].replay(second_source));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertices_sorted().size() ==
        first_source.vertices_sorted().size() + 1U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertices_sorted().size() ==
        second_source.vertices_sorted().size() + 1U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded && loaded.value().mesh_patches().size() == 2U);
    REQUIRE(loaded.value().mesh_patches()[0].replay(first_source));
    REQUIRE(loaded.value().mesh_patches()[1].replay(second_source));

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches()[2].replay(first_redo_source));
    REQUIRE(session.document_.mesh_patches()[3].replay(second_redo_source));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded && loaded.value().mesh_patches().size() == 4U);
    REQUIRE(loaded.value().mesh_patches()[2].replay(first_redo_source));
    REQUIRE(loaded.value().mesh_patches()[3].replay(second_redo_source));
}

void project_edge_split_refuses_shared_mesh_and_stale_multi_object_selection() {
    using namespace carto;
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(
        session, application::CreateBoxAction{"Shared source", {1.0, 1.0, 1.0}}));
    const auto source_object = session.snapshot().objects.front().object;
    REQUIRE(source_object.mesh_asset.has_value());
    REQUIRE(application::testing::dispatch(session,
        application::DuplicateMeshInstanceAction{
            source_object.id, "Shared instance", {3.0, 0.0, 0.0}}));
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{
            "Independent instance", session.document_.meshes().at(*source_object.mesh_asset)}));
    const auto instances = session.snapshot();
    REQUIRE(instances.objects.size() == 3U);
    const auto instance = instances.objects[1].object;
    const auto independent = instances.objects[2].object;
    REQUIRE(instance.mesh_asset == source_object.mesh_asset);
    REQUIRE(independent.mesh_asset.has_value());
    REQUIRE(independent.mesh_asset != source_object.mesh_asset);
    const auto shared_source = session.document_.meshes().at(*source_object.mesh_asset);
    const auto shared_edge = shared_source.edges_sorted(1U).front().id;
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::edge}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectEdgeAction{source_object.id, shared_edge}));
    const auto before_shared_rejection = session.snapshot();
    editor::ToolArguments arguments;
    arguments.factor = 0.5;
    const auto shared_rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.split-edge", arguments});
    REQUIRE(!shared_rejected);
    REQUIRE(shared_rejected.error().code == core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before_shared_rejection.project_revision);
    REQUIRE(session.document_.mesh_patches().empty());

    const auto independent_mesh = session.document_.meshes().at(*independent.mesh_asset);
    const auto independent_edge = independent_mesh.edges_sorted(1U).front().id;
    REQUIRE(application::testing::dispatch(session,
        application::SelectEdgeAction{independent.id, independent_edge,
            editor::SelectionOperation::add}));
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);

    const auto stale_source = session.document_.meshes().at(*independent.mesh_asset);
    const auto stale_revision = stale_source.revision();
    REQUIRE(session.document_.replace_mesh_if_revision(
        *independent.mesh_asset, stale_revision, stale_source));
    const auto before_stale_rejection = session.snapshot();
    const auto stale_authoring = session.authoring_context();
    REQUIRE(!stale_authoring);
    REQUIRE(stale_authoring.error().code == core::ErrorCode::stale_data);
    const auto stale_rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.split-edge", arguments});
    REQUIRE(!stale_rejected);
    REQUIRE(stale_rejected.error().code == core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before_stale_rejection.project_revision);
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.meshes().at(*source_object.mesh_asset).vertices_sorted().size() ==
        shared_source.vertices_sorted().size());
    REQUIRE(session.document_.meshes().at(*independent.mesh_asset).vertices_sorted().size() ==
        stale_source.vertices_sorted().size());
}

void application_loop_cuts_selected_edges_across_objects_atomically_and_persists_receipts() {
    using namespace carto;
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-loop-cut.carto";
    auto first_fixture = make_application_loop_cut_fixture();
    auto second_fixture = make_application_loop_cut_fixture();
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{
            "First loop-cut strip", std::move(first_fixture.mesh)}));
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{
            "Second loop-cut strip", std::move(second_fixture.mesh)}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::edge}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectEdgeAction{first.id, first_fixture.seed_edge}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectEdgeAction{second.id, second_fixture.seed_edge,
            editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    REQUIRE(selected.selection.component_groups[0].object == first.id);
    REQUIRE(selected.selection.component_groups[0].edges ==
        std::vector<geometry::EdgeId>{first_fixture.seed_edge});
    REQUIRE(selected.selection.component_groups[1].object == second.id);
    REQUIRE(selected.selection.component_groups[1].edges ==
        std::vector<geometry::EdgeId>{second_fixture.seed_edge});

    const auto cut = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.loop-cut", {}});
    if (!cut) {
        throw std::runtime_error("multi-object loop cut failed: " + cut.error().message);
    }
    REQUIRE(cut);
    REQUIRE((cut.value().affected.objects == std::vector<scene::ObjectId>{first.id, second.id}));
    REQUIRE((cut.value().affected.edges ==
        std::vector<geometry::EdgeId>{first_fixture.seed_edge, second_fixture.seed_edge}));
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);

    const auto require_mesh_result = [](const geometry::EditableMesh& actual,
                                        const geometry::EditableMesh& source) {
        REQUIRE(actual.validate());
        REQUIRE(actual.vertex_count() == source.vertex_count() + 3U);
        REQUIRE(actual.face_count() == source.face_count() + 2U);
    };
    const auto require_receipt = [](const std::vector<project::TopologyReceiptRecord>& records,
                                    std::uint64_t asset,
                                    core::Revision before_revision) {
        const auto record = std::find_if(records.begin(), records.end(),
            [asset, before_revision](const auto& candidate) {
                return candidate.mesh_asset == asset &&
                    candidate.receipt.revision_before == before_revision;
            });
        REQUIRE(record != records.end());
        REQUIRE(record->validate());
        REQUIRE(record->receipt.revision_after == before_revision.next());
        REQUIRE(record->receipt.created_vertices.size() == 3U);
        REQUIRE(record->receipt.created_faces.size() == 2U);
        REQUIRE(record->receipt.removed_edges.size() == 3U);
        REQUIRE(record->receipt.vertex_origins.size() == 3U);
        REQUIRE(record->receipt.face_origins.size() == 2U);
    };
    const auto require_patch = [](const std::vector<project::MeshPatchRecord>& records,
                                  std::uint64_t asset,
                                  const geometry::EditableMesh& source,
                                  geometry::EdgeId seed_edge,
                                  const geometry::EditableMesh& actual) {
        const auto record = std::find_if(records.begin(), records.end(),
            [asset, source_revision = source.revision()](const auto& candidate) {
                if (candidate.mesh_asset != asset) return false;
                const auto patch = geometry::LoopCutPatch::deserialize(candidate.patch_bytes);
                return patch && patch.value().expected_revision == source_revision;
            });
        REQUIRE(record != records.end());
        REQUIRE(record->validate());
        REQUIRE(!record->patch_digest.is_zero());
        REQUIRE(!record->source_mesh_digest.is_zero());
        REQUIRE(!record->result_mesh_digest.is_zero());
        REQUIRE(record->source_mesh_digest != record->result_mesh_digest);
        const auto patch = geometry::LoopCutPatch::deserialize(record->patch_bytes);
        REQUIRE(patch);
        REQUIRE(patch.value().edge == seed_edge);
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        REQUIRE(replayed.value().revision() == actual.revision());
        REQUIRE(replayed.value().vertex_count() == actual.vertex_count());
        REQUIRE(replayed.value().face_count() == actual.face_count());
    };
    const auto first_cut = session.document_.meshes().at(*first.mesh_asset);
    const auto second_cut = session.document_.meshes().at(*second.mesh_asset);
    require_mesh_result(first_cut, first_source);
    require_mesh_result(second_cut, second_source);
    require_receipt(session.document_.topology_receipts(), *first.mesh_asset,
        first_source.revision());
    require_receipt(session.document_.topology_receipts(), *second.mesh_asset,
        second_source.revision());
    require_patch(session.document_.mesh_patches(), *first.mesh_asset, first_source,
        first_fixture.seed_edge, first_cut);
    require_patch(session.document_.mesh_patches(), *second.mesh_asset, second_source,
        second_fixture.seed_edge, second_cut);

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    auto saved = project::ProjectDocument::load(path);
    REQUIRE(saved && saved.value().topology_receipts().size() == 2U);
    REQUIRE(saved.value().mesh_patches().size() == 2U);
    require_receipt(saved.value().topology_receipts(), *first.mesh_asset, first_source.revision());
    require_receipt(saved.value().topology_receipts(), *second.mesh_asset, second_source.revision());
    require_patch(saved.value().mesh_patches(), *first.mesh_asset, first_source,
        first_fixture.seed_edge, saved.value().meshes().at(*first.mesh_asset));
    require_patch(saved.value().mesh_patches(), *second.mesh_asset, second_source,
        second_fixture.seed_edge, saved.value().meshes().at(*second.mesh_asset));
    application::ApplicationSession reopened;
    REQUIRE(application::testing::dispatch(reopened,
        application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);
    REQUIRE(reopened.document_.topology_receipts().size() == 2U);
    REQUIRE(reopened.document_.mesh_patches().size() == 2U);
    require_mesh_result(reopened.document_.meshes().at(*first.mesh_asset), first_source);
    require_mesh_result(reopened.document_.meshes().at(*second.mesh_asset), second_source);
    require_patch(reopened.document_.mesh_patches(), *first.mesh_asset, first_source,
        first_fixture.seed_edge, reopened.document_.meshes().at(*first.mesh_asset));
    require_patch(reopened.document_.mesh_patches(), *second.mesh_asset, second_source,
        second_fixture.seed_edge, reopened.document_.meshes().at(*second.mesh_asset));

    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() ==
        first_source.vertex_count());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() ==
        first_source.face_count());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() ==
        second_source.vertex_count());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() ==
        second_source.face_count());
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    require_mesh_result(session.document_.meshes().at(*first.mesh_asset), first_source);
    require_mesh_result(session.document_.meshes().at(*second.mesh_asset), second_source);
    require_receipt(session.document_.topology_receipts(), *first.mesh_asset,
        first_redo_source.revision());
    require_receipt(session.document_.topology_receipts(), *second.mesh_asset,
        second_redo_source.revision());
    require_patch(session.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        first_fixture.seed_edge, session.document_.meshes().at(*first.mesh_asset));
    require_patch(session.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        second_fixture.seed_edge, session.document_.meshes().at(*second.mesh_asset));

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    saved = project::ProjectDocument::load(path);
    REQUIRE(saved && saved.value().topology_receipts().size() == 4U);
    REQUIRE(saved.value().mesh_patches().size() == 4U);
    require_receipt(saved.value().topology_receipts(), *first.mesh_asset,
        first_redo_source.revision());
    require_receipt(saved.value().topology_receipts(), *second.mesh_asset,
        second_redo_source.revision());
    require_patch(saved.value().mesh_patches(), *first.mesh_asset, first_redo_source,
        first_fixture.seed_edge, saved.value().meshes().at(*first.mesh_asset));
    require_patch(saved.value().mesh_patches(), *second.mesh_asset, second_redo_source,
        second_fixture.seed_edge, saved.value().meshes().at(*second.mesh_asset));
}

void application_rejects_multi_object_loop_cut_shared_stale_and_invalid_later_owners() {
    using namespace carto;
    const auto select_edges = [](application::ApplicationSession& session,
                                 scene::ObjectId first, geometry::EdgeId first_edge,
                                 scene::ObjectId second, geometry::EdgeId second_edge) {
        REQUIRE(application::testing::dispatch(session,
            application::SetSelectionModeAction{editor::SelectionMode::edge}));
        REQUIRE(application::testing::dispatch(session,
            application::SelectEdgeAction{first, first_edge}));
        REQUIRE(application::testing::dispatch(session,
            application::SelectEdgeAction{second, second_edge,
                editor::SelectionOperation::add}));
        REQUIRE(session.snapshot().selection.component_groups.size() == 2U);
    };

    {
        auto fixture = make_application_loop_cut_fixture();
        application::ApplicationSession session;
        REQUIRE(application::testing::dispatch(session,
            application::CreateMeshObjectAction{
                "Shared loop-cut source", std::move(fixture.mesh)}));
        const auto source = session.snapshot().objects.front().object;
        REQUIRE(source.mesh_asset.has_value());
        REQUIRE(application::testing::dispatch(session,
            application::DuplicateMeshInstanceAction{
                source.id, "Shared loop-cut instance", {3.0, 0.0, 0.0}}));
        const auto objects = session.snapshot().objects;
        REQUIRE(objects.size() == 2U);
        const auto duplicate = objects[1].object;
        REQUIRE(duplicate.mesh_asset == source.mesh_asset);
        select_edges(session, source.id, fixture.seed_edge, duplicate.id, fixture.seed_edge);
        const auto before = session.snapshot();
        const std::string serialized_before = session.document_.serialize();
        const auto rejected = application::testing::dispatch(session,
            application::InvokeToolAction{"mesh.loop-cut", {}});
        REQUIRE(!rejected && rejected.error().code == core::ErrorCode::unsupported);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.document_.topology_receipts().empty());
        REQUIRE(session.document_.mesh_patches().empty());
        REQUIRE(session.document_.serialize() == serialized_before);
    }

    {
        auto first_fixture = make_application_loop_cut_fixture();
        auto second_fixture = make_application_loop_cut_fixture();
        application::ApplicationSession session;
        REQUIRE(application::testing::dispatch(session,
            application::CreateMeshObjectAction{
                "First stale loop-cut strip", std::move(first_fixture.mesh)}));
        REQUIRE(application::testing::dispatch(session,
            application::CreateMeshObjectAction{
                "Second stale loop-cut strip", std::move(second_fixture.mesh)}));
        const auto objects = session.snapshot().objects;
        const auto first = objects[0].object;
        const auto second = objects[1].object;
        REQUIRE(first.mesh_asset && second.mesh_asset);
        select_edges(session, first.id, first_fixture.seed_edge,
            second.id, second_fixture.seed_edge);
        const auto first_source = session.document_.meshes().at(*first.mesh_asset);
        const auto stale_source = session.document_.meshes().at(*second.mesh_asset);
        REQUIRE(session.document_.replace_mesh_if_revision(
            *second.mesh_asset, stale_source.revision(), stale_source));
        const auto before = session.snapshot();
        const std::string serialized_before = session.document_.serialize();
        const auto rejected = application::testing::dispatch(session,
            application::InvokeToolAction{"mesh.loop-cut", {}});
        REQUIRE(!rejected && rejected.error().code == core::ErrorCode::stale_data);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() ==
            first_source.revision());
        REQUIRE(session.document_.topology_receipts().empty());
        REQUIRE(session.document_.mesh_patches().empty());
        REQUIRE(session.document_.serialize() == serialized_before);
    }

    {
        auto first_fixture = make_application_loop_cut_fixture();
        auto later_fixture = make_application_weld_fixture();
        const auto later_edge = later_fixture.edges_sorted(1U).front().id;
        application::ApplicationSession session;
        REQUIRE(application::testing::dispatch(session,
            application::CreateMeshObjectAction{
                "Valid first loop-cut strip", std::move(first_fixture.mesh)}));
        REQUIRE(application::testing::dispatch(session,
            application::CreateMeshObjectAction{
                "Unsupported later triangle mesh", std::move(later_fixture)}));
        const auto objects = session.snapshot().objects;
        const auto first = objects[0].object;
        const auto second = objects[1].object;
        REQUIRE(first.mesh_asset && second.mesh_asset);
        select_edges(session, first.id, first_fixture.seed_edge, second.id, later_edge);
        const auto first_source = session.document_.meshes().at(*first.mesh_asset);
        const auto later_source = session.document_.meshes().at(*second.mesh_asset);
        const auto before = session.snapshot();
        const std::string serialized_before = session.document_.serialize();
        const auto rejected = application::testing::dispatch(session,
            application::InvokeToolAction{"mesh.loop-cut", {}});
        REQUIRE(!rejected && rejected.error().code == core::ErrorCode::unsupported);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() ==
            first_source.revision());
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() ==
            first_source.vertex_count());
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() ==
            first_source.face_count());
        REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() ==
            later_source.revision());
        REQUIRE(session.document_.topology_receipts().empty());
        REQUIRE(session.document_.mesh_patches().empty());
        REQUIRE(session.document_.serialize() == serialized_before);
    }
}

void checkpoint_rebase_preserves_persistent_mesh_patch_history() {
    using namespace carto;
    project::ProjectDocument base;
    auto mesh = make_application_tetrahedron();
    REQUIRE(mesh);
    const auto asset = base.add_mesh(mesh.value());
    REQUIRE(asset);
    const geometry::EditableMesh source = base.meshes().at(asset.value());
    const auto vertex = source.vertices_sorted().front();
    const geometry::MeshPatch patch{
        source.revision(),
        {{vertex.id, vertex.position,
          {vertex.position.x + 0.05, vertex.position.y, vertex.position.z}}},
    };

    project::ProjectDocument speculative = base;
    REQUIRE(speculative.apply_mesh_patch(asset.value(), patch));
    const project::ProjectDocument current = base;
    const std::vector<std::uint64_t> read_assets{asset.value()};
    const std::vector<std::uint64_t> write_assets{asset.value()};
    const auto merged = project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, current, speculative, {}, read_assets, {}, write_assets);
    REQUIRE(merged && merged.value().mesh_patches().size() == 1U);
    REQUIRE(merged.value().mesh_patches().front().replay(source));

    const auto unscoped_write = project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, current, speculative, {}, read_assets, {}, {});
    REQUIRE(!unscoped_write &&
        unscoped_write.error().code == core::ErrorCode::invalid_state);

    const std::uint64_t revision_delta =
        speculative.revision().value() - base.revision().value();
    const auto selected = project::ProjectDocument::select_agent_rebase_step_changes(
        base, speculative, {}, write_assets, revision_delta);
    REQUIRE(selected && selected.value().mesh_patches().size() == 1U);
    REQUIRE(selected.value().mesh_patches().front().replay(source));
}

void application_repeats_last_tool_against_current_document() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Repeat", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));

    carto::editor::ToolArguments arguments;
    arguments.position = {-0.25, -0.5, -0.5};
    REQUIRE(carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RepeatLastToolAction{}));
    const auto snapshot = session.snapshot();
    REQUIRE(snapshot.undo_count == 2U);
    REQUIRE(snapshot.redo_count == 0U);
    REQUIRE(snapshot.viewport.scene.instances().front().mesh->positions.front().x == -0.25);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    carto::editor::ToolArguments adjusted;
    adjusted.position = {-0.4, -0.5, -0.5};
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::AdjustLastToolAction{adjusted}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->positions.front().x == -0.4);
}

void application_rejects_repeat_without_prior_tool() {
    carto::application::ApplicationSession session;
    const auto repeated = carto::application::testing::dispatch(
        session, carto::application::RepeatLastToolAction{});
    REQUIRE(!repeated);
    REQUIRE(repeated.error().code == carto::core::ErrorCode::invalid_state);
    const auto adjusted = carto::application::testing::dispatch(
        session, carto::application::AdjustLastToolAction{carto::editor::ToolArguments{}});
    REQUIRE(!adjusted);
    REQUIRE(adjusted.error().code == carto::core::ErrorCode::invalid_state);
}

void application_rejects_repeat_when_saved_component_was_removed() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreatePlaneAction{"Removed repeat", 1.0, 1.0}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{object, carto::geometry::FaceId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.remove_orphaned_vertices = false;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.remove-face", arguments}));
    const auto before_repeat = session.snapshot();
    const auto repeated = carto::application::testing::dispatch(session,
        carto::application::RepeatLastToolAction{});
    REQUIRE(!repeated);
    REQUIRE(repeated.error().code == carto::core::ErrorCode::not_found);
    const auto after_repeat = session.snapshot();
    REQUIRE(after_repeat.project_revision == before_repeat.project_revision);
    REQUIRE(after_repeat.undo_count == before_repeat.undo_count);
    REQUIRE(after_repeat.selection.faces.empty());
}

void application_routes_component_selection_conversion_without_mutation() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"Plane", 2.0, 2.0}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, {1U}}));
    for (std::uint64_t vertex = 2U; vertex <= 4U; ++vertex) {
        REQUIRE(carto::application::testing::dispatch(
            session, carto::application::SelectVertexAction{
                object, {vertex}, carto::editor::SelectionOperation::add}));
    }

    const auto before = session.snapshot().project_revision;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ConvertSelectionAction{
            carto::editor::SelectionMode::face}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.mode == carto::editor::SelectionMode::face);
    REQUIRE(snapshot.selection.faces.size() == 1U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ConvertSelectionAction{
            carto::editor::SelectionMode::vertex}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.mode == carto::editor::SelectionMode::vertex);
    REQUIRE(snapshot.selection.vertices.size() == 4U);
}

void application_converts_component_selection_per_object_without_flattening_ids() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"First plane", 2.0, 2.0}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"Second plane", 2.0, 2.0}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first_object = initial.objects[0].object.id;
    const auto second_object = initial.objects[1].object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));

    for (std::uint64_t vertex = 1U; vertex <= 4U; ++vertex) {
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{
                first_object, carto::geometry::VertexId{vertex},
                vertex == 1U ? carto::editor::SelectionOperation::replace
                             : carto::editor::SelectionOperation::add}));
    }
    for (std::uint64_t vertex = 1U; vertex <= 4U; ++vertex) {
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{
                second_object, carto::geometry::VertexId{vertex},
                carto::editor::SelectionOperation::add}));
    }

    const auto project_revision = session.snapshot().project_revision;
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.selection.component_groups.size() == 2U);
    REQUIRE(!snapshot.selection.component_object.has_value());
    REQUIRE(snapshot.selection.vertices.empty());
    for (const auto& group : snapshot.selection.component_groups) {
        REQUIRE(group.vertices.size() == 4U);
    }
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::ConvertSelectionAction{carto::editor::SelectionMode::face}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == project_revision);
    REQUIRE(snapshot.selection.faces.empty());
    REQUIRE(snapshot.selection.component_groups.size() == 2U);
    for (const auto& group : snapshot.selection.component_groups) {
        REQUIRE(group.faces.size() == 1U);
    }
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::ConvertSelectionAction{carto::editor::SelectionMode::vertex}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == project_revision);
    REQUIRE(snapshot.selection.component_groups.size() == 2U);
    for (const auto& group : snapshot.selection.component_groups) {
        REQUIRE(group.vertices.size() == 4U);
    }
}

void application_routes_component_selection_expansion_without_mutation() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Expansion", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SelectVertexAction{object, carto::geometry::VertexId{1U}}));
    const auto before = session.snapshot().project_revision;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ExpandSelectionAction{
            carto::editor::SelectionExpansion::grow}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.vertices.size() == 4U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::ExpandSelectionAction{
            carto::editor::SelectionExpansion::invert}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == before);
    REQUIRE(snapshot.selection.vertices.size() == 4U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::object}));
    const auto rejected = carto::application::testing::dispatch(
        session, carto::application::ExpandSelectionAction{
            carto::editor::SelectionExpansion::linked});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before);
}

void application_expands_component_selection_per_object() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"First box", {1.0, 1.0, 1.0}}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Second box", {1.0, 1.0, 1.0}}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first_object = initial.objects[0].object.id;
    const auto second_object = initial.objects[1].object.id;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{
            first_object, carto::geometry::VertexId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{
            second_object, carto::geometry::VertexId{1U},
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot().project_revision;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::ExpandSelectionAction{carto::editor::SelectionExpansion::grow}));
    const auto expanded = session.snapshot();
    REQUIRE(expanded.project_revision == before);
    REQUIRE(expanded.selection.component_groups.size() == 2U);
    REQUIRE(expanded.selection.vertices.empty());
    for (const auto& group : expanded.selection.component_groups) {
        REQUIRE(group.vertices.size() == 4U);
    }
}

void application_deletes_selected_faces_across_objects_atomically() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-face-delete.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Multi-object face deletion"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"First plane", 2.0, 2.0}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"Second plane", 2.0, 2.0}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value());
    REQUIRE(second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{
            first.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{
            second.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    const auto first_source_mesh = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source_mesh = session.document_.meshes().at(*second.mesh_asset);

    carto::editor::ToolArguments arguments;
    arguments.remove_orphaned_vertices = false;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.remove-face", arguments}));
    const auto edited = session.snapshot();
    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().back().event_type == "cartographer.application_action");
    REQUIRE(entries.value().back().revision_before == selected.project_revision);
    REQUIRE(entries.value().back().revision_after == edited.project_revision);
    const std::string payload(
        entries.value().back().payload.begin(), entries.value().back().payload.end());
    REQUIRE(payload.find("operation=Invoke Tool: mesh.remove-face") != std::string::npos);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).faces_sorted().empty());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).faces_sorted().empty());
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 2U);
    REQUIRE(loaded.value().mesh_patches().size() == 2U);
    REQUIRE(loaded.value().meshes().at(*first.mesh_asset).faces_sorted().empty());
    REQUIRE(loaded.value().meshes().at(*second.mesh_asset).faces_sorted().empty());
    std::size_t first_patch_count = 0U;
    std::size_t second_patch_count = 0U;
    for (const auto& record : loaded.value().mesh_patches()) {
        if (record.mesh_asset == *first.mesh_asset) {
            ++first_patch_count;
            const auto replayed = record.replay(first_source_mesh);
            REQUIRE(replayed && replayed.value().faces_sorted().empty());
        } else if (record.mesh_asset == *second.mesh_asset) {
            ++second_patch_count;
            const auto replayed = record.replay(second_source_mesh);
            REQUIRE(replayed && replayed.value().faces_sorted().empty());
        }
    }
    REQUIRE(first_patch_count == 1U);
    REQUIRE(second_patch_count == 1U);
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 2U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).faces_sorted().size() == 1U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).faces_sorted().size() == 1U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).faces_sorted().empty());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).faces_sorted().empty());
    REQUIRE(session.document_.mesh_patches()[2].replay(first_redo_source));
    REQUIRE(session.document_.mesh_patches()[3].replay(second_redo_source));
}

void application_rejects_stale_multi_object_face_selection_atomically() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"First stale plane", 2.0, 2.0}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreatePlaneAction{"Second stale plane", 2.0, 2.0}));
    const auto initial = session.snapshot();
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value());
    REQUIRE(second.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{
            first.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{
            second.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));

    const auto& stale_mesh = session.document_.meshes().at(*second.mesh_asset);
    const auto vertex = stale_mesh.vertices_sorted().front();
    auto changed_position = vertex.position;
    changed_position.x += 0.01;
    REQUIRE(session.document_.apply_mesh_patch(*second.mesh_asset, carto::geometry::MeshPatch{
        stale_mesh.revision(),
        {carto::geometry::VertexChange{vertex.id, vertex.position, changed_position}},
    }));
    const auto before_rejection = session.snapshot();

    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.remove-face", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejection.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).faces_sorted().size() == 1U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).faces_sorted().size() == 1U);
}

void application_rejects_multi_object_face_edits_on_shared_mesh_assets() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Shared source", {2.0, 2.0, 2.0}}));
    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Shared instance", {3.0, 0.0, 0.0}}));
    const auto duplicated = session.snapshot();
    REQUIRE(duplicated.objects.size() == 2U);
    const auto instance = duplicated.objects.back().object;
    REQUIRE(instance.mesh_asset == source.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{
            source.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{
            instance.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();

    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.remove-face", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).faces_sorted().size() == 6U);
}

void application_dissolves_selected_edges_across_objects_atomically() {
    auto first_fixture = make_application_dissolve_fixture();
    auto second_fixture = make_application_dissolve_fixture();
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First dissolve mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second dissolve mesh", second_fixture.mesh}));
    const auto initial = session.snapshot();
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{first.id, first_fixture.internal_edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{second.id, second_fixture.internal_edge,
            carto::editor::SelectionOperation::add}));
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);
    const auto authoring = session.authoring_context();
    REQUIRE(authoring);
    REQUIRE(authoring.value().component_groups.size() == 2U);
    REQUIRE(!authoring.value().component_object.has_value());
    REQUIRE(authoring.value().edges.empty());
    const auto dissolved = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.dissolve-edge", {}});
    REQUIRE(dissolved);
    REQUIRE(dissolved.value().affected.objects.size() == 2U);
    REQUIRE(dissolved.value().affected.edges.size() == 2U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() == 3U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() == 3U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.mesh_patches()[0].replay(first_source));
    REQUIRE(session.document_.mesh_patches()[1].replay(second_source));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto first_restored_faces =
        session.document_.meshes().at(*first.mesh_asset).faces_sorted();
    const auto second_restored_faces =
        session.document_.meshes().at(*second.mesh_asset).faces_sorted();
    const auto first_source_faces = first_source.faces_sorted();
    const auto second_source_faces = second_source.faces_sorted();
    REQUIRE(first_restored_faces.size() == first_source_faces.size());
    REQUIRE(second_restored_faces.size() == second_source_faces.size());
    for (std::size_t index = 0U; index < first_source_faces.size(); ++index) {
        REQUIRE(first_restored_faces[index].id == first_source_faces[index].id);
        REQUIRE(first_restored_faces[index].vertices == first_source_faces[index].vertices);
        REQUIRE(second_restored_faces[index].id == second_source_faces[index].id);
        REQUIRE(second_restored_faces[index].vertices == second_source_faces[index].vertices);
    }
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() == 3U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() == 3U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    REQUIRE(session.document_.mesh_patches()[2].replay(first_redo_source));
    REQUIRE(session.document_.mesh_patches()[3].replay(second_redo_source));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto first_repeat_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_repeat_source = session.document_.meshes().at(*second.mesh_asset);
    const auto repeated = carto::application::testing::dispatch(
        session, carto::application::RepeatLastToolAction{});
    REQUIRE(repeated);
    REQUIRE(repeated.value().affected.objects.size() == 2U);
    REQUIRE(repeated.value().affected.edges.size() == 2U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() == 3U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() == 3U);
    REQUIRE(session.document_.topology_receipts().size() == 6U);
    REQUIRE(session.document_.mesh_patches().size() == 6U);
    REQUIRE(session.document_.mesh_patches()[4].replay(first_repeat_source));
    REQUIRE(session.document_.mesh_patches()[5].replay(second_repeat_source));
}

void application_rejects_multi_object_edge_dissolve_atomically() {
    auto first_fixture = make_application_dissolve_fixture();
    auto second_fixture = make_application_dissolve_fixture();
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First atomic mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second atomic mesh", second_fixture.mesh}));
    const auto initial = session.snapshot();
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{first.id, first_fixture.internal_edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{second.id, second_fixture.boundary_edge,
            carto::editor::SelectionOperation::add}));
    const auto before_rejection = session.snapshot();
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.dissolve-edge", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejection.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == second_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() == 4U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() == 4U);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);
}

void application_refuses_multi_object_edge_dissolve_on_shared_mesh_assets() {
    auto fixture = make_application_dissolve_fixture();
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Shared dissolve mesh", fixture.mesh}));
    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Shared dissolve instance", {3.0, 0.0, 0.0}}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto instance = objects.back().object;
    REQUIRE(instance.mesh_asset == source.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{source.id, fixture.internal_edge}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectEdgeAction{instance.id, fixture.internal_edge,
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.dissolve-edge", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).face_count() == 4U);
}

void application_pokes_selected_faces_across_objects_atomically() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-face-poke.carto";
    auto first_fixture = make_application_dissolve_fixture();
    auto second_fixture = make_application_dissolve_fixture();
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First poke mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second poke mesh", second_fixture.mesh}));
    const auto initial = session.snapshot();
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);
    const auto authoring = session.authoring_context();
    REQUIRE(authoring);
    REQUIRE(authoring.value().component_groups.size() == 2U);
    REQUIRE(!authoring.value().component_object.has_value());
    REQUIRE(authoring.value().faces.empty());

    const auto poked = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.poke-face", {}});
    REQUIRE(poked);
    REQUIRE(poked.value().affected.objects.size() == 2U);
    REQUIRE(poked.value().affected.faces.size() == 2U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 6U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 6U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    const auto assert_patch = [](
        const std::vector<carto::project::MeshPatchRecord>& records,
        std::uint64_t mesh_asset,
        const carto::geometry::EditableMesh& source,
        const carto::geometry::EditableMesh& actual) {
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset, source_revision = source.revision()](const auto& candidate) {
                if (candidate.mesh_asset != mesh_asset) return false;
                const auto decoded = carto::geometry::FacePokePatch::deserialize(
                    candidate.patch_bytes);
                return decoded && decoded.value().expected_revision == source_revision;
            });
        REQUIRE(record != records.end());
        REQUIRE(record->validate());
        REQUIRE(!record->patch_digest.is_zero());
        REQUIRE(!record->source_mesh_digest.is_zero());
        REQUIRE(!record->result_mesh_digest.is_zero());
        REQUIRE(record->source_mesh_digest != record->result_mesh_digest);
        const auto patch = carto::geometry::FacePokePatch::deserialize(record->patch_bytes);
        REQUIRE(patch);
        REQUIRE(patch.value().expected_revision == source.revision());
        REQUIRE(patch.value().face == carto::geometry::FaceId{1U});
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        REQUIRE(replayed.value().revision() == actual.revision());
        REQUIRE(replayed.value().vertex_count() == actual.vertex_count());
        REQUIRE(replayed.value().face_count() == actual.face_count());
    };
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_poked = session.document_.meshes().at(*first.mesh_asset);
    const auto second_poked = session.document_.meshes().at(*second.mesh_asset);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_source, first_poked);
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_source, second_poked);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 5U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 5U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 6U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 6U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        session.document_.meshes().at(*first.mesh_asset));
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        session.document_.meshes().at(*second.mesh_asset));

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 4U);
    REQUIRE(loaded.value().mesh_patches().size() == 4U);
    assert_patch(loaded.value().mesh_patches(), *first.mesh_asset, first_redo_source,
        loaded.value().meshes().at(*first.mesh_asset));
    assert_patch(loaded.value().mesh_patches(), *second.mesh_asset, second_redo_source,
        loaded.value().meshes().at(*second.mesh_asset));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(reopened,
        carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.document_.topology_receipts().size() == 4U);
    REQUIRE(reopened.document_.mesh_patches().size() == 4U);
    assert_patch(reopened.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        reopened.document_.meshes().at(*first.mesh_asset));
    assert_patch(reopened.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        reopened.document_.meshes().at(*second.mesh_asset));
}

void application_pokes_multiple_faces_per_owner_atomically() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-face-per-owner-poke.carto";
    auto fixture = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(fixture);
    const auto source_faces = fixture.value().faces_sorted();
    REQUIRE(source_faces.size() == 6U);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First multi-face poke", fixture.value()}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second multi-face poke", fixture.value()}));
    const auto initial = session.snapshot();
    REQUIRE(initial.objects.size() == 2U);
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, source_faces[0].id}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, source_faces[1].id,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, source_faces[0].id,
            carto::editor::SelectionOperation::add}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, source_faces[1].id,
            carto::editor::SelectionOperation::add}));
    const auto selected = session.snapshot();
    REQUIRE(selected.selection.component_groups.size() == 2U);
    REQUIRE(selected.selection.component_groups[0].faces.size() == 2U);
    REQUIRE(selected.selection.component_groups[1].faces.size() == 2U);
    const auto undo_count_before = selected.undo_count;

    const auto poked = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.poke-face", {}});
    REQUIRE(poked);
    REQUIRE(poked.value().affected.objects.size() == 2U);
    REQUIRE(poked.value().affected.faces.size() == 4U);
    REQUIRE(session.snapshot().undo_count == undo_count_before + 1U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    const auto first_poked = session.document_.meshes().at(*first.mesh_asset);
    const auto second_poked = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(first_poked.vertex_count() == first_source.vertex_count() + 2U);
    REQUIRE(second_poked.vertex_count() == second_source.vertex_count() + 2U);
    REQUIRE(first_poked.face_count() == first_source.face_count() + 6U);
    REQUIRE(second_poked.face_count() == second_source.face_count() + 6U);

    const auto assert_patch_chain = [](
        const std::vector<carto::project::MeshPatchRecord>& all_records,
        std::uint64_t mesh_asset,
        const carto::geometry::EditableMesh& source,
        const carto::geometry::EditableMesh& actual,
        const std::array<carto::geometry::FaceId, 2U>& expected_faces) {
        std::vector<const carto::project::MeshPatchRecord*> records;
        for (const auto& record : all_records) {
            if (record.mesh_asset == mesh_asset) records.push_back(&record);
        }
        REQUIRE(records.size() == expected_faces.size());
        auto replay = source;
        for (std::size_t index = 0U; index < records.size(); ++index) {
            const auto* record = records[index];
            REQUIRE(record->validate());
            const auto patch = carto::geometry::FacePokePatch::deserialize(record->patch_bytes);
            REQUIRE(patch);
            REQUIRE(patch.value().expected_revision == replay.revision());
            REQUIRE(patch.value().face == expected_faces[index]);
            const auto next = record->replay(replay);
            REQUIRE(next);
            replay = next.value();
        }
        REQUIRE(replay.revision() == actual.revision());
        REQUIRE(replay.vertex_count() == actual.vertex_count());
        REQUIRE(replay.face_count() == actual.face_count());
        REQUIRE(replay.validate());
    };
    const std::array<carto::geometry::FaceId, 2U> selected_faces{
        source_faces[0].id, source_faces[1].id};
    assert_patch_chain(session.document_.mesh_patches(), *first.mesh_asset,
        first_source, first_poked, selected_faces);
    assert_patch_chain(session.document_.mesh_patches(), *second.mesh_asset,
        second_source, second_poked, selected_faces);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    auto reopened_document = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened_document);
    REQUIRE(reopened_document.value().mesh_patches().size() == 4U);
    REQUIRE(reopened_document.value().topology_receipts().size() == 4U);
    assert_patch_chain(reopened_document.value().mesh_patches(), *first.mesh_asset,
        first_source, reopened_document.value().meshes().at(*first.mesh_asset), selected_faces);
    assert_patch_chain(reopened_document.value().mesh_patches(), *second.mesh_asset,
        second_source, reopened_document.value().meshes().at(*second.mesh_asset), selected_faces);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() ==
        first_source.vertex_count());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() ==
        second_source.vertex_count());
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() ==
        first_source.face_count() + 6U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() ==
        second_source.face_count() + 6U);
    REQUIRE(session.document_.topology_receipts().size() == 8U);
    REQUIRE(session.document_.mesh_patches().size() == 8U);
    REQUIRE(session.document_.validate());
    const auto verify_redo_records = [&](
        std::uint64_t mesh_asset,
        const carto::geometry::EditableMesh& redo_source,
        const carto::geometry::EditableMesh& actual) {
        std::size_t replayed_count = 0U;
        auto replay = redo_source;
        for (const auto& record : session.document_.mesh_patches()) {
            if (record.mesh_asset != mesh_asset) continue;
            const auto patch = carto::geometry::FacePokePatch::deserialize(record.patch_bytes);
            REQUIRE(patch);
            if (patch.value().expected_revision != replay.revision()) continue;
            const auto next = record.replay(replay);
            REQUIRE(next);
            replay = next.value();
            ++replayed_count;
        }
        REQUIRE(replayed_count == 2U);
        REQUIRE(replay.revision() == actual.revision());
        REQUIRE(replay.vertex_count() == actual.vertex_count());
        REQUIRE(replay.face_count() == actual.face_count());
        REQUIRE(replay.validate());
    };
    verify_redo_records(*first.mesh_asset, first_redo_source,
        session.document_.meshes().at(*first.mesh_asset));
    verify_redo_records(*second.mesh_asset, second_redo_source,
        session.document_.meshes().at(*second.mesh_asset));

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto final_reopen = carto::project::ProjectDocument::load(path);
    REQUIRE(final_reopen);
    REQUIRE(final_reopen.value().mesh_patches().size() == 8U);
    REQUIRE(final_reopen.value().topology_receipts().size() == 8U);
}

void application_multi_face_poke_refuses_a_later_exhausted_revision_atomically() {
    auto fixture = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(fixture);
    REQUIRE(fixture.value().restore_revision(carto::core::Revision{
        carto::core::Revision::max_value() - 2U}));
    const auto faces = fixture.value().faces_sorted();
    REQUIRE(faces.size() == 6U);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Exhausted multi-face poke", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{object.id, faces[0].id}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{object.id, faces[1].id,
            carto::editor::SelectionOperation::add}));
    const auto before_snapshot = session.snapshot();
    const std::string before_document = session.document_.serialize();
    const auto before_mesh = session.document_.meshes().at(*object.mesh_asset);

    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.poke-face", {}});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == before_snapshot.project_revision);
    REQUIRE(session.snapshot().undo_count == before_snapshot.undo_count);
    REQUIRE(session.document_.serialize() == before_document);
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).revision() == before_mesh.revision());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).vertex_count() ==
        before_mesh.vertex_count());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).face_count() == before_mesh.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_merges_selected_vertices_across_objects_atomically() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "First grouped weld", make_application_weld_fixture()}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Second grouped weld", make_application_weld_fixture()}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_merge_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_merge_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    for (const auto object : {first.id, second.id}) {
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{1U},
                object == first.id ? carto::editor::SelectionOperation::replace
                                   : carto::editor::SelectionOperation::add}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{2U},
                carto::editor::SelectionOperation::add}));
    }
    const auto merged = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.merge-vertices", {}});
    REQUIRE(merged);
    REQUIRE(merged.value().affected.objects.size() == 2U);
    REQUIRE(merged.value().affected.vertices.size() == 4U);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 3U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 3U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    REQUIRE(session.document_.mesh_patches()[0].replay(first_merge_source));
    REQUIRE(session.document_.mesh_patches()[1].replay(second_merge_source));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 4U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 4U);
    const auto first_merge_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_merge_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 3U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 3U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    REQUIRE(session.document_.mesh_patches()[2].replay(first_merge_redo_source));
    REQUIRE(session.document_.mesh_patches()[3].replay(second_merge_redo_source));

    {
        TempDirectory temp;
        const auto path = temp.path() / "per-owner-last-selected-weld.carto";
        auto first_fixture = make_application_tetrahedron();
        auto second_fixture = make_application_tetrahedron();
        REQUIRE(first_fixture && second_fixture);
        carto::application::ApplicationSession last_session;
        REQUIRE(carto::application::testing::dispatch(last_session,
            carto::application::CreateMeshObjectAction{
                "First last-selected tetrahedron", std::move(first_fixture.value())}));
        REQUIRE(carto::application::testing::dispatch(last_session,
            carto::application::CreateMeshObjectAction{
                "Second last-selected tetrahedron", std::move(second_fixture.value())}));
        const auto last_objects = last_session.snapshot().objects;
        REQUIRE(last_objects.size() == 2U);
        const auto last_first = last_objects[0].object;
        const auto last_second = last_objects[1].object;
        REQUIRE(last_first.mesh_asset.has_value() && last_second.mesh_asset.has_value());
        REQUIRE(last_first.mesh_asset != last_second.mesh_asset);
        const auto first_source = last_session.document_.meshes().at(*last_first.mesh_asset);
        const auto second_source = last_session.document_.meshes().at(*last_second.mesh_asset);

        REQUIRE(carto::application::testing::dispatch(last_session,
            carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
        // The last pick survives independently within each owner's group.
        REQUIRE(carto::application::testing::dispatch(last_session,
            carto::application::SelectVertexAction{
                last_first.id, carto::geometry::VertexId{2U}}));
        REQUIRE(carto::application::testing::dispatch(last_session,
            carto::application::SelectVertexAction{
                last_first.id, carto::geometry::VertexId{1U},
                carto::editor::SelectionOperation::add}));
        REQUIRE(carto::application::testing::dispatch(last_session,
            carto::application::SelectVertexAction{
                last_second.id, carto::geometry::VertexId{1U},
                carto::editor::SelectionOperation::add}));
        REQUIRE(carto::application::testing::dispatch(last_session,
            carto::application::SelectVertexAction{
                last_second.id, carto::geometry::VertexId{2U},
                carto::editor::SelectionOperation::add}));
        const auto groups = last_session.snapshot().selection.component_groups;
        REQUIRE(groups.size() == 2U);
        REQUIRE(groups[0].object == last_first.id);
        REQUIRE((groups[0].vertex_selection_order ==
            std::vector<carto::geometry::VertexId>{
                carto::geometry::VertexId{2U}, carto::geometry::VertexId{1U}}));
        REQUIRE(groups[1].object == last_second.id);
        REQUIRE((groups[1].vertex_selection_order ==
            std::vector<carto::geometry::VertexId>{
                carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U}}));

        carto::editor::ToolArguments last_arguments;
        last_arguments.merge_mode = carto::geometry::VertexMergeMode::last_selected;
        const auto last_merged = carto::application::testing::dispatch(last_session,
            carto::application::InvokeToolAction{"mesh.merge-vertices", last_arguments});
        REQUIRE(last_merged);
        REQUIRE(last_merged.value().affected.objects.size() == 2U);
        REQUIRE(last_merged.value().affected.objects[0] == last_first.id);
        REQUIRE(last_merged.value().affected.objects[1] == last_second.id);
        REQUIRE((last_merged.value().affected.vertices ==
            std::vector<carto::geometry::VertexId>{
                carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U},
                carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U}}));
        REQUIRE(last_session.document_.topology_receipts().size() == 2U);
        REQUIRE(last_session.document_.mesh_patches().size() == 2U);
        REQUIRE(last_session.document_.mesh_patches()[0].replay(first_source));
        REQUIRE(last_session.document_.mesh_patches()[1].replay(second_source));
        const auto assert_last_receipt = [&](std::uint64_t mesh_asset,
                                             carto::geometry::VertexId source,
                                             carto::geometry::VertexId target) {
            const auto& receipts = last_session.document_.topology_receipts();
            const auto receipt = std::find_if(receipts.begin(), receipts.end(),
                [mesh_asset](const auto& candidate) { return candidate.mesh_asset == mesh_asset; });
            REQUIRE(receipt != receipts.end());
            REQUIRE(receipt->receipt.merged_vertices.size() == 1U);
            REQUIRE(receipt->receipt.merged_vertices.at(source) == target);
            REQUIRE(receipt->receipt.removed_vertices.size() == 1U);
            REQUIRE(receipt->receipt.removed_vertices.front() == source);
        };
        assert_last_receipt(*last_first.mesh_asset,
            carto::geometry::VertexId{2U}, carto::geometry::VertexId{1U});
        assert_last_receipt(*last_second.mesh_asset,
            carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U});
        const auto first_merged = last_session.document_.meshes().at(*last_first.mesh_asset);
        const auto second_merged = last_session.document_.meshes().at(*last_second.mesh_asset);
        REQUIRE(first_merged.vertex_count() == 3U);
        REQUIRE(first_merged.find_vertex(carto::geometry::VertexId{1U}) != nullptr);
        REQUIRE(first_merged.find_vertex(carto::geometry::VertexId{2U}) == nullptr);
        REQUIRE(first_merged.find_vertex(carto::geometry::VertexId{1U})->position.x == 0.0);
        REQUIRE(second_merged.vertex_count() == 3U);
        REQUIRE(second_merged.find_vertex(carto::geometry::VertexId{1U}) == nullptr);
        REQUIRE(second_merged.find_vertex(carto::geometry::VertexId{2U}) != nullptr);
        REQUIRE(second_merged.find_vertex(carto::geometry::VertexId{2U})->position.x == 2.0);

        REQUIRE(carto::application::testing::dispatch(
            last_session, carto::application::UndoAction{}));
        REQUIRE(last_session.document_.meshes().at(*last_first.mesh_asset).vertex_count() == 4U);
        REQUIRE(last_session.document_.meshes().at(*last_second.mesh_asset).vertex_count() == 4U);
        REQUIRE(last_session.document_.meshes().at(*last_first.mesh_asset).face_count() == 4U);
        REQUIRE(last_session.document_.meshes().at(*last_second.mesh_asset).face_count() == 4U);
        const auto first_redo_source =
            last_session.document_.meshes().at(*last_first.mesh_asset);
        const auto second_redo_source =
            last_session.document_.meshes().at(*last_second.mesh_asset);
        REQUIRE(carto::application::testing::dispatch(
            last_session, carto::application::RedoAction{}));
        REQUIRE(last_session.document_.meshes().at(*last_first.mesh_asset).vertex_count() == 3U);
        REQUIRE(last_session.document_.meshes().at(*last_second.mesh_asset).vertex_count() == 3U);
        REQUIRE(last_session.document_.topology_receipts().size() == 4U);
        REQUIRE(last_session.document_.mesh_patches().size() == 4U);
        REQUIRE(last_session.document_.mesh_patches()[2].replay(first_redo_source));
        REQUIRE(last_session.document_.mesh_patches()[3].replay(second_redo_source));
        REQUIRE(carto::application::testing::dispatch(
            last_session, carto::application::SaveProjectAction{path}));
        const auto loaded = carto::project::ProjectDocument::load(path);
        REQUIRE(loaded);
        REQUIRE(loaded.value().topology_receipts().size() == 4U);
        REQUIRE(loaded.value().mesh_patches().size() == 4U);
        REQUIRE(loaded.value().mesh_patches()[2].replay(first_redo_source));
        REQUIRE(loaded.value().mesh_patches()[3].replay(second_redo_source));
        carto::application::ApplicationSession reopened;
        REQUIRE(carto::application::testing::dispatch(
            reopened, carto::application::OpenProjectAction{path}));
        REQUIRE(reopened.snapshot().objects.size() == 2U);
        REQUIRE(reopened.document_.topology_receipts().size() == 4U);
        REQUIRE(reopened.document_.mesh_patches().size() == 4U);
        REQUIRE(reopened.document_.mesh_patches()[2].replay(first_redo_source));
        REQUIRE(reopened.document_.mesh_patches()[3].replay(second_redo_source));
        REQUIRE(reopened.document_.meshes().at(*last_first.mesh_asset).vertex_count() == 3U);
        REQUIRE(reopened.document_.meshes().at(*last_second.mesh_asset).vertex_count() == 3U);
        REQUIRE(reopened.document_.meshes().at(*last_first.mesh_asset).find_vertex(
            carto::geometry::VertexId{1U}) != nullptr);
        REQUIRE(reopened.document_.meshes().at(*last_second.mesh_asset).find_vertex(
            carto::geometry::VertexId{2U}) != nullptr);
        static_cast<void>(first_source);
        static_cast<void>(second_source);
    }

    TempDirectory distance_temp;
    const auto distance_path = distance_temp.path() / "multi-object-merge-by-distance.carto";
    carto::application::ApplicationSession distance_session;
    REQUIRE(carto::application::testing::dispatch(distance_session,
        carto::application::CreateMeshObjectAction{
            "First seam weld", make_application_merge_distance_fixture()}));
    REQUIRE(carto::application::testing::dispatch(distance_session,
        carto::application::CreateMeshObjectAction{
            "Second seam weld", make_application_merge_distance_fixture()}));
    const auto distance_objects = distance_session.snapshot().objects;
    const auto first_seam = distance_objects[0].object;
    const auto second_seam = distance_objects[1].object;
    REQUIRE(first_seam.mesh_asset.has_value() && second_seam.mesh_asset.has_value());
    const auto first_distance_source =
        distance_session.document_.meshes().at(*first_seam.mesh_asset);
    const auto second_distance_source =
        distance_session.document_.meshes().at(*second_seam.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(distance_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    for (const auto object : {first_seam.id, second_seam.id}) {
        for (const std::uint64_t vertex : {1U, 2U, 4U, 5U}) {
            const auto operation = object == first_seam.id && vertex == 1U
                ? carto::editor::SelectionOperation::replace
                : carto::editor::SelectionOperation::add;
            REQUIRE(carto::application::testing::dispatch(distance_session,
                carto::application::SelectVertexAction{
                    object, carto::geometry::VertexId{vertex}, operation}));
        }
    }
    carto::editor::ToolArguments distance_arguments;
    distance_arguments.distance = 1e-4;
    const auto distance_merged = carto::application::testing::dispatch(distance_session,
        carto::application::InvokeToolAction{
            "mesh.merge-by-distance", distance_arguments});
    REQUIRE(distance_merged);
    REQUIRE(distance_merged.value().affected.objects.size() == 2U);
    REQUIRE(distance_merged.value().affected.vertices.size() == 8U);
    REQUIRE(distance_session.document_.meshes().at(*first_seam.mesh_asset).vertex_count() == 4U);
    REQUIRE(distance_session.document_.meshes().at(*second_seam.mesh_asset).vertex_count() == 4U);
    REQUIRE(distance_session.document_.topology_receipts().size() == 2U);
    REQUIRE(distance_session.document_.mesh_patches().size() == 2U);
    REQUIRE(distance_session.document_.mesh_patches()[0].replay(first_distance_source));
    REQUIRE(distance_session.document_.mesh_patches()[1].replay(second_distance_source));
    REQUIRE(carto::application::testing::dispatch(
        distance_session, carto::application::UndoAction{}));
    REQUIRE(distance_session.document_.meshes().at(*first_seam.mesh_asset).vertex_count() == 6U);
    REQUIRE(distance_session.document_.meshes().at(*second_seam.mesh_asset).vertex_count() == 6U);
    const auto first_redo_source = distance_session.document_.meshes().at(*first_seam.mesh_asset);
    const auto second_redo_source = distance_session.document_.meshes().at(*second_seam.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(
        distance_session, carto::application::RedoAction{}));
    REQUIRE(distance_session.document_.meshes().at(*first_seam.mesh_asset).vertex_count() == 4U);
    REQUIRE(distance_session.document_.meshes().at(*second_seam.mesh_asset).vertex_count() == 4U);
    REQUIRE(distance_session.document_.topology_receipts().size() == 4U);
    REQUIRE(distance_session.document_.mesh_patches().size() == 4U);
    REQUIRE(distance_session.document_.mesh_patches()[2].replay(first_redo_source));
    REQUIRE(distance_session.document_.mesh_patches()[3].replay(second_redo_source));
    REQUIRE(carto::application::testing::dispatch(
        distance_session, carto::application::SaveProjectAction{distance_path}));
    const auto distance_loaded = carto::project::ProjectDocument::load(distance_path);
    REQUIRE(distance_loaded);
    REQUIRE(distance_loaded.value().topology_receipts().size() == 4U);
    REQUIRE(distance_loaded.value().mesh_patches().size() == 4U);
    REQUIRE(distance_loaded.value().mesh_patches()[0].replay(first_distance_source));
    REQUIRE(distance_loaded.value().mesh_patches()[1].replay(second_distance_source));
    REQUIRE(distance_loaded.value().mesh_patches()[2].replay(first_redo_source));
    REQUIRE(distance_loaded.value().mesh_patches()[3].replay(second_redo_source));
    carto::application::ApplicationSession distance_reopened;
    REQUIRE(carto::application::testing::dispatch(distance_reopened,
        carto::application::OpenProjectAction{distance_path}));
    REQUIRE(distance_reopened.document_.mesh_patches().size() == 4U);
    REQUIRE(distance_reopened.document_.topology_receipts().size() == 4U);

    auto unsupported_mesh = make_application_weld_fixture();
    const auto topology = unsupported_mesh.topology();
    REQUIRE(topology);
    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::vertex, unsupported_mesh.vertex_count()},
        {carto::attributes::AttributeDomain::edge, topology.value().edges.size()},
        {carto::attributes::AttributeDomain::face, unsupported_mesh.face_count()},
        {carto::attributes::AttributeDomain::corner, topology.value().corners.size()}});
    std::vector<carto::attributes::AttributeValue> edge_tags(
        topology.value().edges.size(), std::int32_t{7});
    REQUIRE(attributes.add_layer({
        {"edge_tag", carto::attributes::AttributeDomain::edge,
            carto::attributes::AttributeType::int32}, std::move(edge_tags)}));
    REQUIRE(unsupported_mesh.set_attribute_payload(std::move(attributes), {}));
    carto::application::ApplicationSession failing_session;
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::CreateMeshObjectAction{
            "First atomic weld", make_application_weld_fixture()}));
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::CreateMeshObjectAction{
            "Second atomic weld", std::move(unsupported_mesh)}));
    const auto failing_objects = failing_session.snapshot().objects;
    const auto first_failing = failing_objects[0].object;
    const auto second_failing = failing_objects[1].object;
    REQUIRE(first_failing.mesh_asset.has_value() && second_failing.mesh_asset.has_value());
    const auto first_before = failing_session.document_.meshes().at(*first_failing.mesh_asset);
    const auto second_before = failing_session.document_.meshes().at(*second_failing.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    for (const auto object : {first_failing.id, second_failing.id}) {
        REQUIRE(carto::application::testing::dispatch(failing_session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{1U},
                object == first_failing.id ? carto::editor::SelectionOperation::replace
                                           : carto::editor::SelectionOperation::add}));
        REQUIRE(carto::application::testing::dispatch(failing_session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{2U},
                carto::editor::SelectionOperation::add}));
    }
    const auto before_rejection = failing_session.snapshot();
    const auto rejected = carto::application::testing::dispatch(failing_session,
        carto::application::InvokeToolAction{"mesh.merge-vertices", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(failing_session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(failing_session.snapshot().undo_count == before_rejection.undo_count);
    REQUIRE(failing_session.document_.meshes().at(*first_failing.mesh_asset).revision() ==
        first_before.revision());
    REQUIRE(failing_session.document_.meshes().at(*second_failing.mesh_asset).revision() ==
        second_before.revision());
    REQUIRE(failing_session.document_.meshes().at(*first_failing.mesh_asset).vertex_count() == 4U);
    REQUIRE(failing_session.document_.meshes().at(*second_failing.mesh_asset).vertex_count() == 4U);
    REQUIRE(failing_session.document_.topology_receipts().empty());

    carto::application::ApplicationSession shared_session;
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::CreateMeshObjectAction{
            "Shared grouped weld", make_application_weld_fixture()}));
    const auto shared_source = shared_session.snapshot().objects.front().object;
    REQUIRE(shared_source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::DuplicateMeshInstanceAction{
            shared_source.id, "Shared grouped weld instance", {3.0, 0.0, 0.0}}));
    const auto shared_objects = shared_session.snapshot().objects;
    REQUIRE(shared_objects.size() == 2U);
    const auto shared_instance = shared_objects.back().object;
    REQUIRE(shared_instance.mesh_asset == shared_source.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    for (const auto object : {shared_source.id, shared_instance.id}) {
        REQUIRE(carto::application::testing::dispatch(shared_session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{1U},
                object == shared_source.id ? carto::editor::SelectionOperation::replace
                                           : carto::editor::SelectionOperation::add}));
        REQUIRE(carto::application::testing::dispatch(shared_session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{2U},
                carto::editor::SelectionOperation::add}));
    }
    const auto shared_mesh_before =
        shared_session.document_.meshes().at(*shared_source.mesh_asset);
    const auto shared_before = shared_session.snapshot();
    const auto shared_rejected = carto::application::testing::dispatch(shared_session,
        carto::application::InvokeToolAction{"mesh.merge-vertices", {}});
    REQUIRE(!shared_rejected);
    REQUIRE(shared_rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(shared_session.snapshot().project_revision == shared_before.project_revision);
    REQUIRE(shared_session.snapshot().undo_count == shared_before.undo_count);
    REQUIRE(shared_session.document_.meshes().at(*shared_source.mesh_asset).revision() ==
        shared_mesh_before.revision());
    REQUIRE(shared_session.document_.topology_receipts().empty());

    carto::application::ApplicationSession stale_session;
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::CreateMeshObjectAction{
            "First stale grouped weld", make_application_weld_fixture()}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::CreateMeshObjectAction{
            "Second stale grouped weld", make_application_weld_fixture()}));
    const auto stale_objects = stale_session.snapshot().objects;
    const auto stale_first = stale_objects[0].object;
    const auto stale_second = stale_objects[1].object;
    REQUIRE(stale_first.mesh_asset.has_value() && stale_second.mesh_asset.has_value());
    const auto stale_first_before = stale_session.document_.meshes().at(*stale_first.mesh_asset);
    const auto stale_second_before = stale_session.document_.meshes().at(*stale_second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
    for (const auto object : {stale_first.id, stale_second.id}) {
        REQUIRE(carto::application::testing::dispatch(stale_session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{1U},
                object == stale_first.id ? carto::editor::SelectionOperation::replace
                                         : carto::editor::SelectionOperation::add}));
        REQUIRE(carto::application::testing::dispatch(stale_session,
            carto::application::SelectVertexAction{
                object, carto::geometry::VertexId{2U},
                carto::editor::SelectionOperation::add}));
    }
    const auto stale_revision = stale_session.document_.replace_mesh_if_revision(
        *stale_second.mesh_asset, stale_second_before.revision(), stale_second_before);
    REQUIRE(stale_revision);
    const auto stale_before = stale_session.snapshot();
    const auto stale_rejected = carto::application::testing::dispatch(stale_session,
        carto::application::InvokeToolAction{"mesh.merge-vertices", {}});
    REQUIRE(!stale_rejected);
    REQUIRE(stale_rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(stale_session.snapshot().project_revision == stale_before.project_revision);
    REQUIRE(stale_session.snapshot().undo_count == stale_before.undo_count);
    REQUIRE(stale_session.document_.meshes().at(*stale_first.mesh_asset).revision() ==
        stale_first_before.revision());
    REQUIRE(stale_session.document_.meshes().at(*stale_second.mesh_asset).revision() ==
        stale_revision.value());
    REQUIRE(stale_session.document_.topology_receipts().empty());
}

void application_extrudes_selected_faces_across_objects_atomically() {
    TempDirectory temp;
    const auto path = temp.path() / "multi-object-face-extrusion.carto";
    auto first_fixture = make_application_dissolve_fixture();
    auto second_fixture = make_application_dissolve_fixture();
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First extrusion mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second extrusion mesh", second_fixture.mesh}));
    const auto initial = session.snapshot();
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto extruded = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.extrude-face", arguments});
    REQUIRE(extruded);
    REQUIRE(extruded.value().affected.objects.size() == 2U);
    REQUIRE(extruded.value().affected.faces.size() == 2U);
    const auto first_extruded_vertices =
        session.document_.meshes().at(*first.mesh_asset).vertex_count();
    const auto second_extruded_vertices =
        session.document_.meshes().at(*second.mesh_asset).vertex_count();
    REQUIRE(first_extruded_vertices > 5U);
    REQUIRE(second_extruded_vertices > 5U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto assert_patch = [](const std::vector<carto::project::MeshPatchRecord>& records,
                                 std::uint64_t mesh_asset,
                                 const carto::geometry::EditableMesh& source,
                                 const carto::geometry::EditableMesh& actual) {
        const auto record = std::find_if(records.begin(), records.end(),
            [mesh_asset, source_revision = source.revision()](const auto& candidate) {
                if (candidate.mesh_asset != mesh_asset) return false;
                const auto decoded = carto::geometry::FaceExtrusionPatch::deserialize(
                    candidate.patch_bytes);
                return decoded && decoded.value().expected_revision == source_revision;
            });
        REQUIRE(record != records.end());
        REQUIRE(record->validate());
        REQUIRE(!record->patch_digest.is_zero());
        REQUIRE(!record->source_mesh_digest.is_zero());
        REQUIRE(!record->result_mesh_digest.is_zero());
        REQUIRE(record->source_mesh_digest != record->result_mesh_digest);
        const auto patch = carto::geometry::FaceExtrusionPatch::deserialize(record->patch_bytes);
        REQUIRE(patch);
        REQUIRE(patch.value().face == carto::geometry::FaceId{1U});
        REQUIRE(patch.value().distance == 0.25);
        const auto replayed = record->replay(source);
        REQUIRE(replayed);
        REQUIRE(replayed.value().revision() == actual.revision());
        REQUIRE(replayed.value().vertex_count() == actual.vertex_count());
        REQUIRE(replayed.value().face_count() == actual.face_count());
    };
    const auto first_after_initial = session.document_.meshes().at(*first.mesh_asset);
    const auto second_after_initial = session.document_.meshes().at(*second.mesh_asset);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_source,
        first_after_initial);
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_source,
        second_after_initial);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 5U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 5U);
    REQUIRE(session.document_.topology_receipts().size() == 2U);
    REQUIRE(session.document_.mesh_patches().size() == 2U);
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() ==
        first_extruded_vertices);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() ==
        second_extruded_vertices);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);
    assert_patch(session.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        session.document_.meshes().at(*first.mesh_asset));
    assert_patch(session.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        session.document_.meshes().at(*second.mesh_asset));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{path}));
    auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 4U);
    REQUIRE(loaded.value().mesh_patches().size() == 4U);
    assert_patch(loaded.value().mesh_patches(), *first.mesh_asset, first_redo_source,
        loaded.value().meshes().at(*first.mesh_asset));
    assert_patch(loaded.value().mesh_patches(), *second.mesh_asset, second_redo_source,
        loaded.value().meshes().at(*second.mesh_asset));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(reopened,
        carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.document_.topology_receipts().size() == 4U);
    REQUIRE(reopened.document_.mesh_patches().size() == 4U);
    assert_patch(reopened.document_.mesh_patches(), *first.mesh_asset, first_redo_source,
        reopened.document_.meshes().at(*first.mesh_asset));
    assert_patch(reopened.document_.mesh_patches(), *second.mesh_asset, second_redo_source,
        reopened.document_.meshes().at(*second.mesh_asset));

    auto first_failing_fixture = make_application_dissolve_fixture();
    auto second_failing_fixture = make_application_dissolve_fixture();
    const auto failing_topology = second_failing_fixture.mesh.topology();
    REQUIRE(failing_topology);
    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::vertex, second_failing_fixture.mesh.vertex_count()},
        {carto::attributes::AttributeDomain::edge, failing_topology.value().edges.size()},
        {carto::attributes::AttributeDomain::face, second_failing_fixture.mesh.face_count()},
        {carto::attributes::AttributeDomain::corner, failing_topology.value().corners.size()}});
    std::vector<carto::attributes::AttributeValue> regions(
        second_failing_fixture.mesh.face_count(), std::uint32_t{4U});
    REQUIRE(attributes.add_layer({
        {"region", carto::attributes::AttributeDomain::face,
            carto::attributes::AttributeType::uint32}, std::move(regions)}));
    REQUIRE(second_failing_fixture.mesh.set_attribute_payload(std::move(attributes), {}));
    carto::application::ApplicationSession failing_session;
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::CreateMeshObjectAction{
            "First atomic extrusion mesh", first_failing_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::CreateMeshObjectAction{
            "Second atomic extrusion mesh", second_failing_fixture.mesh}));
    const auto failing_objects = failing_session.snapshot().objects;
    const auto first_failing = failing_objects[0].object;
    const auto second_failing = failing_objects[1].object;
    REQUIRE(first_failing.mesh_asset.has_value() && second_failing.mesh_asset.has_value());
    const auto first_failing_before =
        failing_session.document_.meshes().at(*first_failing.mesh_asset);
    const auto second_failing_before =
        failing_session.document_.meshes().at(*second_failing.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::SelectFaceAction{
            first_failing.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(failing_session,
        carto::application::SelectFaceAction{
            second_failing.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    const auto before_rejection = failing_session.snapshot();
    const auto rejected = carto::application::testing::dispatch(failing_session,
        carto::application::InvokeToolAction{"mesh.extrude-face", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(failing_session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(failing_session.snapshot().undo_count == before_rejection.undo_count);
    REQUIRE(failing_session.document_.meshes().at(*first_failing.mesh_asset).revision() ==
        first_failing_before.revision());
    REQUIRE(failing_session.document_.meshes().at(*second_failing.mesh_asset).revision() ==
        second_failing_before.revision());
    REQUIRE(failing_session.document_.meshes().at(*first_failing.mesh_asset).vertex_count() == 5U);
    REQUIRE(failing_session.document_.meshes().at(*second_failing.mesh_asset).vertex_count() == 5U);
    REQUIRE(failing_session.document_.topology_receipts().empty());
    REQUIRE(failing_session.document_.mesh_patches().empty());
}

void application_refuses_multi_object_face_extrusion_on_shared_or_stale_owners() {
    {
        auto fixture = make_application_dissolve_fixture();
        carto::application::ApplicationSession session;
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{
                "Shared face extrusion source", fixture.mesh}));
        const auto source = session.snapshot().objects.front().object;
        REQUIRE(source.mesh_asset.has_value());
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::DuplicateMeshInstanceAction{
                source.id, "Shared face extrusion instance", {3.0, 0.0, 0.0}}));
        const auto objects = session.snapshot().objects;
        REQUIRE(objects.size() == 2U);
        const auto instance = objects.back().object;
        REQUIRE(instance.mesh_asset == source.mesh_asset);
        const auto mesh_before = session.document_.meshes().at(*source.mesh_asset);
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectFaceAction{source.id, carto::geometry::FaceId{1U}}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectFaceAction{instance.id, carto::geometry::FaceId{1U},
                carto::editor::SelectionOperation::add}));
        const auto before = session.snapshot();
        const std::string serialized_before = session.document_.serialize();
        carto::editor::ToolArguments arguments;
        arguments.distance = 0.25;
        const auto rejected = carto::application::testing::dispatch(session,
            carto::application::InvokeToolAction{"mesh.extrude-face", arguments});
        REQUIRE(!rejected && rejected.error().code == carto::core::ErrorCode::unsupported);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.document_.meshes().at(*source.mesh_asset).revision() ==
            mesh_before.revision());
        REQUIRE(session.document_.topology_receipts().empty());
        REQUIRE(session.document_.mesh_patches().empty());
        REQUIRE(session.document_.serialize() == serialized_before);
    }

    {
        const auto first_fixture = make_application_dissolve_fixture();
        const auto second_fixture = make_application_dissolve_fixture();
        carto::application::ApplicationSession session;
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{
                "First stale face extrusion mesh", first_fixture.mesh}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateMeshObjectAction{
                "Second stale face extrusion mesh", second_fixture.mesh}));
        const auto objects = session.snapshot().objects;
        const auto first = objects[0].object;
        const auto second = objects[1].object;
        REQUIRE(first.mesh_asset && second.mesh_asset);
        const auto first_before = session.document_.meshes().at(*first.mesh_asset);
        const auto second_before = session.document_.meshes().at(*second.mesh_asset);
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectFaceAction{first.id, carto::geometry::FaceId{1U}}));
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SelectFaceAction{second.id, carto::geometry::FaceId{1U},
                carto::editor::SelectionOperation::add}));
        const auto stale_revision = session.document_.replace_mesh_if_revision(
            *second.mesh_asset, second_before.revision(), second_before);
        REQUIRE(stale_revision);
        const auto before = session.snapshot();
        const std::string serialized_before = session.document_.serialize();
        carto::editor::ToolArguments arguments;
        arguments.distance = 0.25;
        const auto rejected = carto::application::testing::dispatch(session,
            carto::application::InvokeToolAction{"mesh.extrude-face", arguments});
        REQUIRE(!rejected && rejected.error().code == carto::core::ErrorCode::stale_data);
        REQUIRE(session.snapshot().project_revision == before.project_revision);
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() ==
            first_before.revision());
        REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() ==
            first_before.vertex_count());
        REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() ==
            stale_revision.value());
        REQUIRE(session.document_.topology_receipts().empty());
        REQUIRE(session.document_.mesh_patches().empty());
        REQUIRE(session.document_.serialize() == serialized_before);
    }
}

void application_refuses_multi_object_inset_on_shared_mesh_assets() {
    auto fixture = make_application_tri_to_quad_fixture();
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Shared inset mesh", fixture.mesh}));
    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Shared inset instance", {3.0, 0.0, 0.0}}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto instance = objects.back().object;
    REQUIRE(instance.mesh_asset == source.mesh_asset);
    const auto mesh_before = session.document_.meshes().at(*source.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{source.id, fixture.first_face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{instance.id, fixture.first_face,
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.inset-face", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).revision() == mesh_before.revision());
    REQUIRE(session.document_.meshes().at(*source.mesh_asset).face_count() == 2U);
    REQUIRE(session.document_.topology_receipts().empty());
}

void application_rejects_multi_object_inset_on_stale_second_owner_atomically() {
    auto first_fixture = make_application_tri_to_quad_fixture();
    auto second_fixture = make_application_tri_to_quad_fixture(true);
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First stale inset mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second stale inset mesh", second_fixture.mesh}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, first_fixture.first_face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, second_fixture.first_face,
            carto::editor::SelectionOperation::add}));
    const auto stale_revision = session.document_.replace_mesh_if_revision(
        *second.mesh_asset, second_before.revision(), second_before);
    REQUIRE(stale_revision);
    const auto before = session.snapshot();
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.inset-face", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() == 2U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == stale_revision.value());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() == 3U);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_object_inset_atomically_on_unsupported_payload() {
    auto first_fixture = make_application_dissolve_fixture();
    auto second_fixture = make_application_dissolve_fixture();
    const auto topology = second_fixture.mesh.topology();
    REQUIRE(topology);
    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::vertex, second_fixture.mesh.vertex_count()},
        {carto::attributes::AttributeDomain::edge, topology.value().edges.size()},
        {carto::attributes::AttributeDomain::face, second_fixture.mesh.face_count()},
        {carto::attributes::AttributeDomain::corner, topology.value().corners.size()}});
    std::vector<carto::attributes::AttributeValue> regions(
        second_fixture.mesh.face_count(), std::uint32_t{2U});
    REQUIRE(attributes.add_layer({
        {"region", carto::attributes::AttributeDomain::face,
            carto::attributes::AttributeType::uint32}, std::move(regions)}));
    REQUIRE(second_fixture.mesh.set_attribute_payload(std::move(attributes), {}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First atomic inset mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second atomic inset mesh", second_fixture.mesh}));
    const auto objects = session.snapshot().objects;
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    carto::editor::ToolArguments arguments;
    arguments.distance = 0.1;
    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.inset-face", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).face_count() ==
        first_before.face_count());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == second_before.revision());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).face_count() ==
        second_before.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_object_face_poke_atomically_on_unsupported_payload() {
    auto first_fixture = make_application_dissolve_fixture();
    auto second_fixture = make_application_dissolve_fixture();
    const auto topology = second_fixture.mesh.topology();
    REQUIRE(topology);
    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::vertex, second_fixture.mesh.vertex_count()},
        {carto::attributes::AttributeDomain::edge, topology.value().edges.size()},
        {carto::attributes::AttributeDomain::face, second_fixture.mesh.face_count()},
        {carto::attributes::AttributeDomain::corner, topology.value().corners.size()}});
    std::vector<carto::attributes::AttributeValue> regions(
        second_fixture.mesh.face_count(), std::uint32_t{9U});
    REQUIRE(attributes.add_layer({
        {"region", carto::attributes::AttributeDomain::face,
            carto::attributes::AttributeType::uint32}, std::move(regions)}));
    REQUIRE(second_fixture.mesh.set_attribute_payload(std::move(attributes), {}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"First atomic poke mesh", first_fixture.mesh}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{"Second atomic poke mesh", second_fixture.mesh}));
    const auto initial = session.snapshot();
    const auto first = initial.objects[0].object;
    const auto second = initial.objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    const auto first_before = session.document_.meshes().at(*first.mesh_asset);
    const auto second_before = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{first.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectFaceAction{second.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    const auto before_rejection = session.snapshot();
    const std::string serialized_before = session.document_.serialize();

    const auto rejected = carto::application::testing::dispatch(session,
        carto::application::InvokeToolAction{"mesh.poke-face", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(session.snapshot().project_revision == before_rejection.project_revision);
    REQUIRE(session.snapshot().undo_count == before_rejection.undo_count);
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).revision() == first_before.revision());
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).revision() == second_before.revision());
    REQUIRE(session.document_.meshes().at(*first.mesh_asset).vertex_count() == 5U);
    REQUIRE(session.document_.meshes().at(*second.mesh_asset).vertex_count() == 5U);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.serialize() == serialized_before);
    REQUIRE(session.snapshot().selection.component_groups.size() == 2U);
}

void application_refuses_multi_object_face_poke_on_stale_or_shared_owners() {
    auto fixture = make_application_dissolve_fixture();
    carto::application::ApplicationSession stale_session;
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::CreateMeshObjectAction{"Stale poke owner", fixture.mesh}));
    auto stale_owner = stale_session.snapshot().objects.front().object;
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(stale_session,
        carto::application::SelectFaceAction{stale_owner.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(stale_session, carto::application::UndoAction{}));
    const auto stale_before = stale_session.snapshot();
    const auto stale_rejected = carto::application::testing::dispatch(stale_session,
        carto::application::InvokeToolAction{"mesh.poke-face", {}});
    REQUIRE(!stale_rejected);
    REQUIRE(stale_rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(stale_session.snapshot().project_revision == stale_before.project_revision);
    REQUIRE(stale_session.document_.topology_receipts().empty());
    REQUIRE(stale_session.document_.mesh_patches().empty());

    carto::application::ApplicationSession shared_session;
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::CreateMeshObjectAction{"Shared poke mesh", fixture.mesh}));
    const auto source = shared_session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::DuplicateMeshInstanceAction{
            source.id, "Shared poke instance", {3.0, 0.0, 0.0}}));
    const auto objects = shared_session.snapshot().objects;
    const auto instance = objects.back().object;
    REQUIRE(instance.mesh_asset == source.mesh_asset);
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::SelectFaceAction{source.id, carto::geometry::FaceId{1U}}));
    REQUIRE(carto::application::testing::dispatch(shared_session,
        carto::application::SelectFaceAction{instance.id, carto::geometry::FaceId{1U},
            carto::editor::SelectionOperation::add}));
    const auto shared_before = shared_session.snapshot();
    const auto shared_rejected = carto::application::testing::dispatch(shared_session,
        carto::application::InvokeToolAction{"mesh.poke-face", {}});
    REQUIRE(!shared_rejected);
    REQUIRE(shared_rejected.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(shared_session.snapshot().project_revision == shared_before.project_revision);
    REQUIRE(shared_session.document_.meshes().at(*source.mesh_asset).vertex_count() == 5U);
    REQUIRE(shared_session.document_.topology_receipts().empty());
    REQUIRE(shared_session.document_.mesh_patches().empty());
}

void application_routes_persistent_edge_selection_without_mutation() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Edge selection", {2.0, 2.0, 2.0}}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    const auto object = snapshot.objects.front().object.id;
    const auto instances = snapshot.viewport.scene.instances();
    REQUIRE(instances.size() == 1U);
    REQUIRE(instances.front().mesh->triangle_edges.size() ==
            instances.front().mesh->triangle_faces.size());
    REQUIRE(instances.front().mesh->triangle_edges.front().front().has_value());
    const auto edge = *instances.front().mesh->triangle_edges.front().front();
    const auto revision = snapshot.project_revision;

    REQUIRE(carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::edge}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SelectEdgeAction{object, edge}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.project_revision == revision);
    REQUIRE(snapshot.selection.mode == carto::editor::SelectionMode::edge);
    REQUIRE(snapshot.selection.edges.size() == 1U);
    REQUIRE(snapshot.selection.edges.front() == edge);

    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SelectEdgeAction{
        object, carto::geometry::EdgeId{999}}));
    const auto after_failure = session.snapshot();
    REQUIRE(after_failure.project_revision == revision);
    REQUIRE(after_failure.selection.edges.size() == 1U);
    REQUIRE(after_failure.selection.edges.front() == edge);
}

void application_rejects_bad_selection_without_mutating_history() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Box", {1.0, 1.0, 1.0}}));
    const auto before = session.snapshot();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SelectFaceAction{
        carto::scene::ObjectId{999}, carto::geometry::FaceId{1}}));
    const auto after = session.snapshot();
    REQUIRE(after.undo_count == before.undo_count);
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(!after.problems.empty());

    REQUIRE(!carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "missing.tool", {}}));
    REQUIRE(session.snapshot().undo_count == before.undo_count);

    const auto before_mode = session.snapshot().selection.mode;
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetSelectionModeAction{
        static_cast<carto::editor::SelectionMode>(99)}));
    REQUIRE(session.snapshot().selection.mode == before_mode);
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SelectObjectAction{
        carto::scene::ObjectId{1},
        static_cast<carto::editor::SelectionOperation>(99)}));
    REQUIRE(session.snapshot().selection.objects.empty());
}

void application_requires_explicit_discard_for_project_replacement() {
    TempDirectory temp;
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(source, carto::application::NewProjectAction{"Saved source"}));
    REQUIRE(carto::application::testing::dispatch(source, carto::application::CreatePlaneAction{
        "Saved source", 2.0, 2.0}));
    const auto source_path = temp.path() / "source.carto";
    REQUIRE(carto::application::testing::dispatch(source, carto::application::SaveProjectAction{source_path}));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Unsaved", {1.0, 1.0, 1.0}}));
    const auto before = session.snapshot();
    REQUIRE(before.dirty);
    REQUIRE(!session.can_close());
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Replacement"}));
    REQUIRE(session.snapshot().project_name == before.project_name);
    REQUIRE(session.snapshot().objects.size() == before.objects.size());

    const auto generation = before.project_generation;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Replacement", true}));
    const auto after = session.snapshot();
    REQUIRE(after.project_name == "Replacement");
    REQUIRE(after.objects.empty());
    REQUIRE(!after.dirty);
    REQUIRE(after.project_generation != generation);
    REQUIRE(session.can_close());

    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Dirty before open", {1.0, 1.0, 1.0}}));
    const auto before_open = session.snapshot();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::OpenProjectAction{source_path}));
    REQUIRE(session.snapshot().project_name == before_open.project_name);
    REQUIRE(session.snapshot().objects.size() == before_open.objects.size());
    REQUIRE(carto::application::testing::dispatch(session, carto::application::OpenProjectAction{source_path, true}));
    REQUIRE(session.snapshot().project_name == "Saved source");
    REQUIRE(!session.snapshot().dirty);
}

void application_bounds_failure_diagnostics() {
    carto::application::ApplicationSession session;
    for (std::size_t index = 0U; index < 256U; ++index) {
        REQUIRE(!carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
            "missing.tool", {}}));
    }
    const auto snapshot = session.snapshot();
    REQUIRE(!snapshot.problems.empty());
    REQUIRE(snapshot.problems.size() <= 128U);
}

void application_validates_workspace_layouts() {
    carto::application::ApplicationSession session;
    const auto before = session.snapshot().workspace.visible_panes;

    carto::application::WorkspaceState duplicate;
    duplicate.visible_panes = {
        carto::application::Pane::viewport,
        carto::application::Pane::viewport,
    };
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{duplicate}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState empty;
    empty.visible_panes.clear();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{empty}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState unknown;
    unknown.visible_panes = {
        carto::application::Pane::viewport,
        static_cast<carto::application::Pane>(99),
    };
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{unknown}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState no_viewport;
    no_viewport.visible_panes = {carto::application::Pane::outliner};
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SetWorkspaceAction{no_viewport}));
    REQUIRE(session.snapshot().workspace.visible_panes == before);

    carto::application::WorkspaceState compact;
    compact.visible_panes = {
        carto::application::Pane::viewport,
        carto::application::Pane::inspector,
    };
    const auto compact_receipt = carto::application::testing::dispatch(session,
        carto::application::SetWorkspaceAction{compact});
    REQUIRE(compact_receipt);
    REQUIRE(compact_receipt.value().action == "Set Workspace");
    REQUIRE(!compact_receipt.value().document_changed);
    REQUIRE(session.snapshot().workspace.visible_panes == compact.visible_panes);
    REQUIRE(session.snapshot().problems.size() == 4U);
}

void application_open_failure_preserves_current_project_and_save_is_atomic() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreatePlaneAction{
        "Plane", 2.0, 3.0}));
    const auto current = session.snapshot();
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::OpenProjectAction{
        temp.path() / "missing.carto"}));
    REQUIRE(session.snapshot().objects.size() == current.objects.size());
    REQUIRE(session.snapshot().project_revision == current.project_revision);

    const auto path = temp.path() / "saved.carto";
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));
    REQUIRE(!session.snapshot().dirty);
    REQUIRE(std::filesystem::is_regular_file(path));

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().objects.size() == 1U);
    REQUIRE(!reopened.snapshot().dirty);
}

void application_journals_committed_mutations_and_rolls_back_failed_append() {
    TempDirectory temp;
    const auto path = temp.path() / "journaled.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Journaled"}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));

    carto::journal::Journal journal(journal_path);
    auto baseline = journal.read_all();
    REQUIRE(baseline);
    REQUIRE(baseline.value().size() == 1U);
    REQUIRE(baseline.value().front().event_type == "cartographer.snapshot");

    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Box", {1.0, 1.0, 1.0}}));
    auto after_create = session.snapshot();
    REQUIRE(after_create.objects.size() == 1U);
    REQUIRE(after_create.undo_count == 1U);

    auto after_event = journal.read_all();
    REQUIRE(after_event);
    REQUIRE(after_event.value().size() == 2U);
    REQUIRE(after_event.value().back().event_type == "cartographer.application_action");
    REQUIRE(after_event.value().back().revision_after == after_create.project_revision);
    const std::string mutation_payload(
        after_event.value().back().payload.begin(), after_event.value().back().payload.end());
    REQUIRE(mutation_payload.find("CARTOGRAPHER_APPLICATION_MUTATION_V1") == 0U);
    REQUIRE(mutation_payload.find(
                "CARTOGRAPHER_PROJECT " +
                std::to_string(carto::project::ProjectDocument::kSchemaVersion)) !=
            std::string::npos);
    REQUIRE(mutation_payload.find(
                "AUTHORING \"cartographer.authoring\" \"meters\" \"right_handed_y_up\"") !=
            std::string::npos);
    REQUIRE(journal.verify());

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    auto after_undo = journal.read_all();
    REQUIRE(after_undo);
    REQUIRE(after_undo.value().size() == 3U);
    REQUIRE(after_undo.value().back().event_type == "cartographer.application_action");

    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    auto after_redo = journal.read_all();
    REQUIRE(after_redo);
    REQUIRE(after_redo.value().size() == 4U);

    const auto before_failed_append = session.snapshot();
    REQUIRE(std::filesystem::remove(journal_path));
    REQUIRE(std::filesystem::create_directory(journal_path));
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::CreatePlaneAction{
        "Rejected", 1.0, 1.0}));
    const auto after_failed_append = session.snapshot();
    REQUIRE(after_failed_append.project_revision == before_failed_append.project_revision);
    REQUIRE(after_failed_append.objects.size() == before_failed_append.objects.size());
    REQUIRE(after_failed_append.undo_count == before_failed_append.undo_count);
    REQUIRE(std::filesystem::remove_all(journal_path) == 1U);
}

void application_rejects_journal_revision_drift_before_open() {
    TempDirectory temp;
    const auto path = temp.path() / "drifted.carto";
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(source, carto::application::NewProjectAction{"Drifted"}));
    REQUIRE(carto::application::testing::dispatch(source, carto::application::SaveProjectAction{path}));

    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    carto::journal::Journal journal(journal_path);
    const auto current = journal.current_revision();
    REQUIRE(current);
    const std::array<std::uint8_t, 1> payload{0x01U};
    REQUIRE(journal.append(carto::journal::JournalAppend{
        current.value(), current.value().next(), "test.drift", payload, 1U}));

    carto::application::ApplicationSession target;
    REQUIRE(!carto::application::testing::dispatch(target, carto::application::OpenProjectAction{path}));
    REQUIRE(target.snapshot().project_name != "Drifted");
    REQUIRE(!target.snapshot().problems.empty());
}

void application_rolls_back_preexisting_journal_baseline_on_save_failure() {
    TempDirectory temp;
    const auto path = temp.path() / "blocked.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    const std::string header = "CARTOGRAPHER_JOURNAL_V1\n";
    {
        std::ofstream seed(journal_path, std::ios::binary | std::ios::trunc);
        seed << header;
    }
    REQUIRE(std::filesystem::create_directory(path));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::NewProjectAction{"Save rollback"}));
    REQUIRE(!carto::application::testing::dispatch(session, carto::application::SaveProjectAction{path}));

    std::ifstream restored(journal_path, std::ios::binary);
    const std::string contents(
        std::istreambuf_iterator<char>{restored}, std::istreambuf_iterator<char>{});
    REQUIRE(contents == header);
    carto::journal::Journal journal(journal_path);
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().empty());
}

void application_rejects_semantically_wrong_journal_tail() {
    TempDirectory temp;
    const auto path = temp.path() / "semantic-drift.carto";
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(source, carto::application::NewProjectAction{"Semantic drift"}));
    REQUIRE(carto::application::testing::dispatch(source, carto::application::SaveProjectAction{path}));

    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    carto::journal::Journal journal(journal_path);
    const auto baseline = journal.read_all();
    REQUIRE(baseline);
    REQUIRE(baseline.value().size() == 1U);
    const std::string baseline_payload(
        baseline.value().front().payload.begin(), baseline.value().front().payload.end());
    const std::size_t marker = baseline_payload.find("document_bytes=");
    REQUIRE(marker != std::string::npos);
    const std::size_t document_start = baseline_payload.find('\n', marker);
    REQUIRE(document_start != std::string::npos);
    std::string revision_two = baseline_payload.substr(document_start + 1U);
    const std::string revision_marker = "REVISION 1\n";
    const std::size_t revision = revision_two.find(revision_marker);
    REQUIRE(revision != std::string::npos);
    revision_two.replace(revision, revision_marker.size(), "REVISION 2\n");

    const std::string fake_payload =
        "CARTOGRAPHER_PROJECT_SNAPSHOT_V1\n" +
        std::string("document_bytes=") + std::to_string(revision_two.size()) + "\n" +
        revision_two;
    REQUIRE(journal.append(carto::journal::JournalAppend{
        carto::core::Revision{1U}, carto::core::Revision{2U}, "test.fake_snapshot",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(fake_payload.data()), fake_payload.size()),
        2U}));
    std::ofstream replace(path, std::ios::binary | std::ios::trunc);
    replace << revision_two;
    replace.close();
    REQUIRE(replace);

    carto::application::ApplicationSession target;
    const auto opened = carto::application::testing::dispatch(target, carto::application::OpenProjectAction{path});
    REQUIRE(!opened);
    REQUIRE(opened.error().code == carto::core::ErrorCode::validation_failed);
}

void application_inspects_recovery_without_mutation() {
    TempDirectory temp;
    const auto path = temp.path() / "inspect.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");

    const auto unavailable = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(unavailable);
    REQUIRE(unavailable.value().state ==
            carto::application::RecoveryInspectionState::unavailable);
    REQUIRE(unavailable.value().journal_entry_count == 0U);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Inspection"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto clean_before = session.snapshot();
    const auto clean = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(clean);
    REQUIRE(clean.value().state == carto::application::RecoveryInspectionState::clean);
    REQUIRE(clean.value().project_revision == clean_before.project_revision);
    REQUIRE(clean.value().durable_revision == clean_before.project_revision);
    REQUIRE(clean.value().journal_entry_count == 1U);
    REQUIRE(clean.value().journal_entries.front().event_type == "cartographer.snapshot");

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Pending", {1.0, 1.0, 1.0}}));
    const auto pending_before = session.snapshot();
    const auto pending = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(pending);
    REQUIRE(pending.value().state == carto::application::RecoveryInspectionState::pending);
    REQUIRE(pending.value().project_revision == clean_before.project_revision);
    REQUIRE(pending.value().durable_revision == pending_before.project_revision);
    REQUIRE(pending.value().journal_entry_count == 2U);
    REQUIRE(std::filesystem::is_regular_file(path));
    REQUIRE(std::filesystem::is_regular_file(journal_path));
    REQUIRE(session.snapshot().project_revision == pending_before.project_revision);

    REQUIRE(std::filesystem::remove(path));
    const auto missing_project = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(missing_project);
    REQUIRE(missing_project.value().state ==
            carto::application::RecoveryInspectionState::pending);
    REQUIRE(missing_project.value().diagnostic.has_value());
    REQUIRE(missing_project.value().diagnostic->code == carto::core::ErrorCode::not_found);
}

void application_recovery_inspection_serializes_with_project_writers() {
    TempDirectory temp;
    const auto path = temp.path() / "inspection-lock.carto";

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Inspection lock"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::thread worker;
    bool lock_acquired = false;
    bool started_seen = false;
    bool blocked_while_held = false;
    {
        carto::project::FileLock held(path);
        lock_acquired = static_cast<bool>(held.acquire());
        if (lock_acquired) {
            worker = std::thread([&] {
                started.store(true, std::memory_order_release);
                const auto result =
                    carto::application::ApplicationSession::inspect_recovery(path);
                succeeded.store(static_cast<bool>(result), std::memory_order_release);
                completed.store(true, std::memory_order_release);
            });
            for (unsigned attempt = 0U;
                 attempt < 1000U && !started.load(std::memory_order_acquire); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started_seen = started.load(std::memory_order_acquire);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            blocked_while_held = !completed.load(std::memory_order_acquire);
        }
    }
    if (worker.joinable()) worker.join();

    REQUIRE(lock_acquired);
    REQUIRE(started_seen);
    REQUIRE(blocked_while_held);
    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(succeeded.load(std::memory_order_acquire));
}

void application_blocks_malformed_recovery_evidence() {
    TempDirectory temp;
    const auto path = temp.path() / "malformed.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    {
        std::ofstream project(path, std::ios::binary | std::ios::trunc);
        project << "not a cartographer project\n";
        std::ofstream journal(journal_path, std::ios::binary | std::ios::trunc);
        journal << "CARTOGRAPHER_JOURNAL_V1\nmalformed\n";
    }

    const auto inspected = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(inspected);
    REQUIRE(inspected.value().state ==
            carto::application::RecoveryInspectionState::blocked);
    REQUIRE(inspected.value().diagnostic.has_value());
    REQUIRE(inspected.value().diagnostic->code == carto::core::ErrorCode::validation_failed);
    REQUIRE(inspected.value().journal_entry_count == 0U);

    auto forged = inspected.value();
    forged.state = carto::application::RecoveryInspectionState::clean;
    forged.diagnostic.reset();
    forged.durable_revision = carto::core::Revision{1U};
    REQUIRE(!forged.validate());
}

void application_requires_explicit_recovery_and_rebinds_saved_state() {
    TempDirectory temp;
    const auto path = temp.path() / "recover.carto";
    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Recoverable"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::CreateBoxAction{"Recovered box", {1.0, 1.0, 1.0}}));

    carto::application::ApplicationSession target;
    const auto recovered = carto::application::testing::dispatch(
        target, carto::application::RecoverProjectAction{path});
    REQUIRE(recovered);
    REQUIRE(recovered.value().source == carto::application::OperationSource::recovery);
    REQUIRE(recovered.value().document_changed);
    REQUIRE(target.snapshot().objects.size() == 1U);
    REQUIRE(!target.snapshot().dirty);
    REQUIRE(target.snapshot().project_path == path);

    const auto clean = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(clean);
    REQUIRE(clean.value().state == carto::application::RecoveryInspectionState::clean);

    carto::application::ApplicationSession dirty;
    REQUIRE(carto::application::testing::dispatch(
        dirty, carto::application::CreatePlaneAction{"Unsaved", 1.0, 1.0}));
    const auto before = dirty.snapshot();
    const auto rejected = carto::application::testing::dispatch(
        dirty, carto::application::RecoverProjectAction{path});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(dirty.snapshot().project_revision == before.project_revision);
    REQUIRE(dirty.snapshot().objects.size() == before.objects.size());
}

void application_recovery_fails_closed_without_mutating_session_or_evidence() {
    TempDirectory temp;

    const auto unsupported_path = temp.path() / "unsupported-tail.carto";
    carto::application::ApplicationSession unsupported_source;
    REQUIRE(carto::application::testing::dispatch(
        unsupported_source, carto::application::NewProjectAction{"Unsupported tail"}));
    REQUIRE(carto::application::testing::dispatch(
        unsupported_source, carto::application::SaveProjectAction{unsupported_path}));
    const auto unsupported_journal_path =
        std::filesystem::path(unsupported_path.string() + ".journal");
    carto::journal::Journal unsupported_journal(unsupported_journal_path);
    const std::string unsupported_payload = "CARTOGRAPHER_UNKNOWN_RECOVERY_V1\n";
    REQUIRE(unsupported_journal.append(carto::journal::JournalAppend{
        carto::core::Revision{1U}, carto::core::Revision{2U}, "test.unsupported_recovery",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(unsupported_payload.data()),
            unsupported_payload.size()),
        2U}));
    const auto unsupported_entries_before = unsupported_journal.read_all();
    REQUIRE(unsupported_entries_before);
    REQUIRE(unsupported_entries_before.value().size() == 2U);

    carto::application::ApplicationSession unsupported_target;
    const auto unsupported_before = unsupported_target.snapshot();
    const auto unsupported_recovery = carto::application::testing::dispatch(
        unsupported_target, carto::application::RecoverProjectAction{unsupported_path});
    REQUIRE(!unsupported_recovery);
    REQUIRE(unsupported_target.snapshot().project_revision ==
            unsupported_before.project_revision);
    REQUIRE(unsupported_target.snapshot().objects.size() == unsupported_before.objects.size());
    REQUIRE(unsupported_target.snapshot().project_path == unsupported_before.project_path);
    const auto unsupported_entries_after = unsupported_journal.read_all();
    REQUIRE(unsupported_entries_after);
    REQUIRE(unsupported_entries_after.value().size() ==
            unsupported_entries_before.value().size());
    REQUIRE(unsupported_journal.verify());

    const auto save_failure_path = temp.path() / "atomic-save-failure.carto";
    carto::application::ApplicationSession save_failure_source;
    REQUIRE(carto::application::testing::dispatch(
        save_failure_source, carto::application::NewProjectAction{"Atomic recovery"}));
    REQUIRE(carto::application::testing::dispatch(
        save_failure_source, carto::application::SaveProjectAction{save_failure_path}));
    REQUIRE(carto::application::testing::dispatch(
        save_failure_source, carto::application::CreateBoxAction{"Recovered", {1.0, 1.0, 1.0}}));
    const auto save_failure_journal_path =
        std::filesystem::path(save_failure_path.string() + ".journal");
    carto::journal::Journal save_failure_journal(save_failure_journal_path);
    const auto save_failure_entries_before = save_failure_journal.read_all();
    REQUIRE(save_failure_entries_before);
    REQUIRE(save_failure_entries_before.value().size() == 2U);
    REQUIRE(std::filesystem::remove(save_failure_path));
    REQUIRE(std::filesystem::create_directory(save_failure_path));

    carto::application::ApplicationSession save_failure_target;
    const auto save_failure_before = save_failure_target.snapshot();
    const auto save_failure_recovery = carto::application::testing::dispatch(
        save_failure_target, carto::application::RecoverProjectAction{save_failure_path});
    REQUIRE(!save_failure_recovery);
    REQUIRE(save_failure_target.snapshot().project_revision ==
            save_failure_before.project_revision);
    REQUIRE(save_failure_target.snapshot().objects.size() == save_failure_before.objects.size());
    REQUIRE(!save_failure_target.snapshot().project_path.has_value());
    REQUIRE(std::filesystem::is_directory(save_failure_path));
    const auto save_failure_entries_after = save_failure_journal.read_all();
    REQUIRE(save_failure_entries_after);
    REQUIRE(save_failure_entries_after.value().size() ==
            save_failure_entries_before.value().size());
    REQUIRE(save_failure_journal.verify());
}

void application_opens_pending_projects_with_automatic_recovery() {
    TempDirectory temp;
    const auto path = temp.path() / "automatic-recovery.carto";

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Recovered automatically"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::CreateBoxAction{"Recovered box", {1.0, 1.0, 1.0}}));

    const auto pending = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(pending);
    REQUIRE(pending.value().state == carto::application::RecoveryInspectionState::pending);
    REQUIRE(pending.value().journal_entry_count == 2U);

    // Opt-out keeps the existing refusal when the durable journal is ahead.
    carto::application::ApplicationSession opted_out;
    const auto refused = carto::application::testing::dispatch(
        opted_out, carto::application::OpenProjectAction{path});
    REQUIRE(!refused);
    REQUIRE(refused.error().code == carto::core::ErrorCode::stale_data);

    carto::application::ApplicationSession automatic;
    const auto opened = carto::application::testing::dispatch(
        automatic,
        carto::application::OpenProjectAction{
            path, false, carto::application::OpenProjectOptions{true}});
    REQUIRE(opened);
    REQUIRE(opened.value().action == "Open Project");
    REQUIRE(opened.value().source == carto::application::OperationSource::recovery);
    REQUIRE(opened.value().document_changed);
    REQUIRE(opened.value().automatic_recovery_applied);
    const auto snapshot = automatic.snapshot();
    REQUIRE(!snapshot.dirty);
    REQUIRE(snapshot.project_path == path);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.objects.front().object.name == "Recovered box");

    const auto clean = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(clean);
    REQUIRE(clean.value().state == carto::application::RecoveryInspectionState::clean);

    // Automatic and explicit recovery replay the same durable evidence and
    // must converge on a canonically identical document.
    const auto automatic_document = carto::project::ProjectDocument::load(path);
    REQUIRE(automatic_document);
    const std::string automatic_serialized = automatic_document.value().serialize();
    const auto automatic_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(automatic_serialized.data()),
        automatic_serialized.size()));

    carto::application::ApplicationSession human;
    const auto recovered = carto::application::testing::dispatch(
        human, carto::application::RecoverProjectAction{path});
    REQUIRE(recovered);
    REQUIRE(recovered.value().source == carto::application::OperationSource::recovery);
    const auto human_document = carto::project::ProjectDocument::load(path);
    REQUIRE(human_document);
    const std::string human_serialized = human_document.value().serialize();
    const auto human_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(human_serialized.data()),
        human_serialized.size()));
    REQUIRE(automatic_digest == human_digest);
}

void application_auto_recovery_recovers_last_verified_snapshot_across_truncation() {
    TempDirectory temp;
    const auto path = temp.path() / "truncated-recovery.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Truncated recovery"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::CreateBoxAction{"First", {1.0, 1.0, 1.0}}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::CreateBoxAction{"Second", {2.0, 2.0, 2.0}}));

    // Crash at a record boundary: keep the header, baseline snapshot, and the
    // first mutation; the second mutation never became durable.
    std::filesystem::resize_file(
        journal_path, journal_prefix_bytes_through_record(journal_path, 2U));

    const auto pending = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(pending);
    REQUIRE(pending.value().state == carto::application::RecoveryInspectionState::pending);
    REQUIRE(pending.value().journal_entry_count == 2U);

    carto::application::ApplicationSession automatic;
    const auto opened = carto::application::testing::dispatch(
        automatic,
        carto::application::OpenProjectAction{
            path, false, carto::application::OpenProjectOptions{true}});
    REQUIRE(opened);
    REQUIRE(opened.value().automatic_recovery_applied);
    const auto snapshot = automatic.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.objects.front().object.name == "First");
    REQUIRE(!snapshot.dirty);
    const auto automatic_document = carto::project::ProjectDocument::load(path);
    REQUIRE(automatic_document);
    const std::string automatic_serialized = automatic_document.value().serialize();

    carto::application::ApplicationSession human;
    const auto recovered = carto::application::testing::dispatch(
        human, carto::application::RecoverProjectAction{path});
    REQUIRE(recovered);
    const auto human_document = carto::project::ProjectDocument::load(path);
    REQUIRE(human_document);
    REQUIRE(human_document.value().serialize() == automatic_serialized);

    carto::journal::Journal journal(journal_path);
    REQUIRE(journal.verify());
    const auto clean = carto::application::ApplicationSession::inspect_recovery(path);
    REQUIRE(clean);
    REQUIRE(clean.value().state == carto::application::RecoveryInspectionState::clean);
}

void application_auto_recovery_fails_closed_on_torn_journals_and_dirty_sessions() {
    TempDirectory temp;
    const auto torn_path = temp.path() / "torn-recovery.carto";
    const auto torn_journal_path =
        std::filesystem::path(torn_path.string() + ".journal");

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Torn recovery"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{torn_path}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::CreateBoxAction{"Unsaved", {1.0, 1.0, 1.0}}));

    const auto original_size = std::filesystem::file_size(torn_journal_path);
    std::filesystem::resize_file(torn_journal_path, original_size - 13U);

    const auto blocked = carto::application::ApplicationSession::inspect_recovery(torn_path);
    REQUIRE(blocked);
    REQUIRE(blocked.value().state == carto::application::RecoveryInspectionState::blocked);
    REQUIRE(blocked.value().diagnostic.has_value());

    carto::application::ApplicationSession target;
    const auto before = target.snapshot();
    const auto opened = carto::application::testing::dispatch(
        target,
        carto::application::OpenProjectAction{
            torn_path, false, carto::application::OpenProjectOptions{true}});
    REQUIRE(!opened);
    REQUIRE(target.snapshot().project_revision == before.project_revision);
    REQUIRE(target.snapshot().objects.size() == before.objects.size());
    REQUIRE(!target.snapshot().project_path.has_value());
    REQUIRE(std::filesystem::file_size(torn_journal_path) == original_size - 13U);

    // A dirty session is refused even with discard confirmation: automatic
    // recovery never discards unsaved human work.
    carto::application::ApplicationSession dirty;
    REQUIRE(carto::application::testing::dispatch(
        dirty, carto::application::CreatePlaneAction{"Unsaved plane", 1.0, 1.0}));
    const auto dirty_before = dirty.snapshot();
    const auto refused = carto::application::testing::dispatch(
        dirty,
        carto::application::OpenProjectAction{
            torn_path, true, carto::application::OpenProjectOptions{true}});
    REQUIRE(!refused);
    REQUIRE(refused.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(dirty.snapshot().project_revision == dirty_before.project_revision);
    REQUIRE(dirty.snapshot().objects.size() == dirty_before.objects.size());
    REQUIRE(dirty.snapshot().dirty);
}

void application_auto_recovery_serializes_with_project_writers() {
    TempDirectory temp;
    const auto path = temp.path() / "auto-recovery-lock.carto";

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Automatic recovery lock"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::CreateBoxAction{"Pending", {1.0, 1.0, 1.0}}));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::atomic<bool> applied{false};
    std::thread worker;
    bool lock_acquired = false;
    bool started_seen = false;
    bool blocked_while_held = false;
    {
        carto::project::FileLock held(path);
        lock_acquired = static_cast<bool>(held.acquire());
        if (lock_acquired) {
            worker = std::thread([&] {
                started.store(true, std::memory_order_release);
                carto::application::ApplicationSession target;
                const auto result = carto::application::testing::dispatch(
                    target,
                    carto::application::OpenProjectAction{
                        path, false, carto::application::OpenProjectOptions{true}});
                const bool ok = static_cast<bool>(result);
                succeeded.store(ok, std::memory_order_release);
                applied.store(
                    ok && result.value().automatic_recovery_applied,
                    std::memory_order_release);
                completed.store(true, std::memory_order_release);
            });
            for (unsigned attempt = 0U;
                 attempt < 1000U && !started.load(std::memory_order_acquire); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started_seen = started.load(std::memory_order_acquire);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            blocked_while_held = !completed.load(std::memory_order_acquire);
        }
    }
    if (worker.joinable()) worker.join();

    REQUIRE(lock_acquired);
    REQUIRE(started_seen);
    REQUIRE(blocked_while_held);
    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(succeeded.load(std::memory_order_acquire));
    REQUIRE(applied.load(std::memory_order_acquire));
}

void application_duplicates_mesh_instance_batches_through_session_action() {
    TempDirectory temp;
    const auto path = temp.path() / "session-batch.carto";

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Session batch"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::CreateBoxAction{"Module", {2.0, 2.0, 2.0}}));

    const auto source = session.snapshot().objects.front().object;
    REQUIRE(source.mesh_asset.has_value());
    const auto batch = carto::application::testing::dispatch(
        session,
        carto::application::DuplicateMeshInstanceBatchAction{
            source.id,
            {{"Module east", {4.0, 0.0, -1.0}}, {"Module north", {0.0, 5.0, 2.5}}}});
    REQUIRE(batch);
    REQUIRE(batch.value().action == "Duplicate Mesh Instance Batch");
    REQUIRE(batch.value().document_changed);
    REQUIRE(batch.value().affected.objects.size() == 1U);
    REQUIRE(batch.value().affected.objects.front() == source.id);

    auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 3U);
    const auto east = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(),
        [](const auto& view) { return view.object.name == "Module east"; });
    const auto north = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(),
        [](const auto& view) { return view.object.name == "Module north"; });
    REQUIRE(east != snapshot.objects.end());
    REQUIRE(north != snapshot.objects.end());
    REQUIRE(east->object.mesh_asset == source.mesh_asset);
    REQUIRE(north->object.mesh_asset == source.mesh_asset);
    REQUIRE(east->object.local_transform.translation.x == 4.0);
    REQUIRE(east->object.local_transform.translation.z == -1.0);
    REQUIRE(north->object.local_transform.translation.y == 5.0);

    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 3U);
    const std::string payload(
        entries.value().back().payload.begin(), entries.value().back().payload.end());
    REQUIRE(payload.find("operation=Duplicate Mesh Instance Batch") != std::string::npos);
    REQUIRE(journal.verify());

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 3U);
    const auto restored = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(),
        [id = east->object.id](const auto& view) { return view.object.id == id; });
    REQUIRE(restored != snapshot.objects.end());
    REQUIRE(restored->object.mesh_asset == source.mesh_asset);
}

void application_mutation_serializes_with_project_recovery_writer_lease() {
    TempDirectory temp;
    const auto path = temp.path() / "mutation-lock.carto";

    carto::application::ApplicationSession source;
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::NewProjectAction{"Mutation lock"}));
    REQUIRE(carto::application::testing::dispatch(
        source, carto::application::SaveProjectAction{path}));

    carto::application::ApplicationSession writer;
    REQUIRE(carto::application::testing::dispatch(
        writer, carto::application::OpenProjectAction{path}));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    std::thread worker;
    bool lock_acquired = false;
    bool started_seen = false;
    bool blocked_while_held = false;
    {
        carto::project::FileLock held(path);
        lock_acquired = static_cast<bool>(held.acquire());
        if (lock_acquired) {
            worker = std::thread([&] {
                started.store(true, std::memory_order_release);
                const auto result = carto::application::testing::dispatch(
                    writer, carto::application::CreateBoxAction{
                        "Serialized", {1.0, 1.0, 1.0}});
                succeeded.store(static_cast<bool>(result), std::memory_order_release);
                completed.store(true, std::memory_order_release);
            });
            for (unsigned attempt = 0U;
                 attempt < 1000U && !started.load(std::memory_order_acquire); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started_seen = started.load(std::memory_order_acquire);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            blocked_while_held = !completed.load(std::memory_order_acquire);
        }
    }
    if (worker.joinable()) worker.join();

    REQUIRE(lock_acquired);
    REQUIRE(started_seen);
    REQUIRE(blocked_while_held);
    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(succeeded.load(std::memory_order_acquire));
    REQUIRE(writer.snapshot().project_revision == carto::core::Revision{4U});
    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 2U);
    REQUIRE(journal.verify());
}

void application_routes_spatial_field_creation_and_paint_through_revisioned_history() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    const auto initial_path = temp.path() / "field-application.carto";
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Field authoring"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{initial_path}));
    const carto::application::CreateSpatialFieldAction create{
        "field.workflow.moisture", carto::spatial_field::FieldSemantic::moisture,
        {0.0, 0.0}, 1.0, 8U, 8U, 0.0, 1.0, 0.0};
    REQUIRE(carto::application::testing::dispatch(session, create));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.spatial_fields.size() == 1U);
    REQUIRE(snapshot.spatial_fields.front().source_revision == carto::core::Revision{1U});
    const auto initial_preview = session.spatial_field_preview("field.workflow.moisture");
    REQUIRE(initial_preview && initial_preview.value().preview_width == 8U &&
            initial_preview.value().preview_height == 8U &&
            initial_preview.value().values.size() == 64U &&
            initial_preview.value().source_revision == carto::core::Revision{1U});
    REQUIRE(!session.spatial_field_preview("field.workflow.missing"));

    carto::spatial_field::FieldBrush brush;
    brush.expected_revision = carto::core::Revision{1U};
    brush.center_uv = {3.5, 3.5};
    brush.radius = 3.0;
    brush.value = 0.8;
    brush.strength = 1.0;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::PaintSpatialFieldAction{
            "field.workflow.moisture", brush}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.spatial_fields.front().source_revision == carto::core::Revision{2U});
    const auto painted_preview = session.spatial_field_preview("field.workflow.moisture");
    REQUIRE(painted_preview &&
            painted_preview.value().source_revision == carto::core::Revision{2U} &&
            std::any_of(painted_preview.value().values.begin(),
                        painted_preview.value().values.end(),
                        [](double value) { return value > 0.0; }));
    const auto after_paint_revision = snapshot.project_revision;
    carto::journal::Journal initial_journal(
        std::filesystem::path(initial_path.string() + ".journal"));
    const auto mutation_events = initial_journal.read_all();
    REQUIRE(mutation_events && mutation_events.value().size() == 3U);
    REQUIRE(std::string(mutation_events.value()[1U].payload.begin(),
                        mutation_events.value()[1U].payload.end()).find(
                            "Create Spatial Field") != std::string::npos);
    REQUIRE(std::string(mutation_events.value()[2U].payload.begin(),
                        mutation_events.value()[2U].payload.end()).find(
                            "Paint Spatial Field") != std::string::npos);

    REQUIRE(!carto::application::testing::dispatch(
        session, carto::application::PaintSpatialFieldAction{
            "field.workflow.moisture", brush}));
    REQUIRE(session.snapshot().project_revision == after_paint_revision);
    const auto events_after_stale_paint = initial_journal.read_all();
    REQUIRE(events_after_stale_paint && events_after_stale_paint.value().size() == 3U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.spatial_fields.front().source_revision == carto::core::Revision{3U});
    const auto restored_path = temp.path() / "field-undo.carto";
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{restored_path}));
    auto restored_project = carto::project::ProjectDocument::load(restored_path);
    REQUIRE(restored_project);
    const auto& restored_values = restored_project.value().spatial_fields().at(
        "field.workflow.moisture").values();
    REQUIRE(std::all_of(restored_values.begin(), restored_values.end(),
        [](double value) { return value == 0.0; }));

    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.spatial_fields.front().source_revision == carto::core::Revision{4U});
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().spatial_fields.empty());
    const auto deleted_path = temp.path() / "field-deleted.carto";
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{deleted_path}));
    const auto deleted_project = carto::project::ProjectDocument::load(deleted_path);
    REQUIRE(deleted_project);
    REQUIRE(deleted_project.value().serialize().find(
        "SPATIAL_FIELD_LINEAGE_RECORD \"field.workflow.moisture\" 5") !=
            std::string::npos);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().spatial_fields.front().source_revision == carto::core::Revision{6U});
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().spatial_fields.front().source_revision == carto::core::Revision{7U});
}

void application_builds_read_only_terrain_previews_from_current_project_sources() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Terrain preview"}));
    const std::string identity = carto::terrain::terrain_tile_identity({-1, 2});
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            identity, carto::spatial_field::FieldSemantic::terrain_elevation,
            {-12.0, 24.0}, 1.0, 7U, 7U, -10.0, 10.0, 0.0}));
    const auto before = session.snapshot();
    const auto product = session.terrain_tile_preview(identity, 2U, 25.0, 0.5);
    REQUIRE(product && product.value().validate());
    REQUIRE(product.value().source_revision() == carto::core::Revision{1U});
    REQUIRE(product.value().vertices().size() == 16U);
    REQUIRE(product.value().indices().size() / 3U == 18U);
    REQUIRE(std::all_of(product.value().wet().begin(), product.value().wet().end(),
        [](std::uint8_t value) { return value == 1U; }));
    const auto after = session.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.spatial_fields.front().source_revision ==
        before.spatial_fields.front().source_revision);
    REQUIRE(!session.terrain_tile_preview("terrain.tile.x-9.z-9"));
    REQUIRE(!session.terrain_tile_preview("not-a-terrain-tile"));
}

void application_builds_read_only_source_bound_wind_vector_previews() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Wind vector preview"}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            "field.wind.u", carto::spatial_field::FieldSemantic::wind_u,
            {-8.0, 12.0}, 2.0, 20U, 12U, -10.0, 10.0, 2.5}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            "field.wind.v", carto::spatial_field::FieldSemantic::wind_v,
            {-8.0, 12.0}, 2.0, 20U, 12U, -10.0, 10.0, -1.25}));
    const auto before = session.snapshot();
    const auto preview = session.spatial_vector_field_preview("field.wind.u", "field.wind.v");
    REQUIRE(preview && preview.value().validate() &&
            preview.value().samples.size() == 16U * 12U);
    REQUIRE(preview.value().u_component.digest == before.spatial_fields[0U].digest &&
            preview.value().v_component.digest == before.spatial_fields[1U].digest);
    REQUIRE(preview.value().samples.front().vector_uv.x == 2.5 &&
            preview.value().samples.front().vector_uv.y == -1.25);
    const auto after = session.snapshot();
    REQUIRE(after.project_revision == before.project_revision &&
            after.spatial_fields[0U].source_revision == before.spatial_fields[0U].source_revision &&
            after.spatial_fields[1U].source_revision == before.spatial_fields[1U].source_revision);
    REQUIRE(!session.spatial_vector_field_preview("field.wind.missing", "field.wind.v"));
    REQUIRE(!session.spatial_vector_field_preview("field.wind.u", "field.wind.u"));
    REQUIRE(!session.spatial_vector_field_preview("field.wind.u", "field.wind.v", 1U));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            "field.wind.misaligned", carto::spatial_field::FieldSemantic::wind_v,
            {-6.0, 12.0}, 2.0, 20U, 12U, -10.0, 10.0, 0.0}));
    REQUIRE(!session.spatial_vector_field_preview("field.wind.u", "field.wind.misaligned"));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            "field.wind.wrong", carto::spatial_field::FieldSemantic::moisture,
            {-8.0, 12.0}, 2.0, 20U, 12U, -10.0, 10.0, 0.0}));
    REQUIRE(!session.spatial_vector_field_preview("field.wind.wrong", "field.wind.v"));

    auto paint = carto::spatial_field::FieldBrush{};
    paint.expected_revision = carto::core::Revision{1U};
    paint.center_uv = {-8.0, 12.0};
    paint.radius = 2.0;
    paint.value = 3.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::PaintSpatialFieldAction{"field.wind.u", paint}));
    const auto refreshed = session.spatial_vector_field_preview(
        "field.wind.u", "field.wind.v");
    REQUIRE(refreshed && refreshed.value().u_component.source_revision ==
            carto::core::Revision{2U} &&
            refreshed.value().u_component.digest != preview.value().u_component.digest &&
            preview.value().u_component.source_revision == carto::core::Revision{1U});
}

void application_authors_revision_bound_roads_through_history_and_journal() {
    TempDirectory temp;
    const auto path = temp.path() / "road-authoring.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Road authoring"}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    constexpr std::string_view centerline_identity = "road.test.centerline";
    constexpr std::string_view road_identity = "road.test.source";
    const auto build_action = [&](carto::core::Revision expected_project_revision,
                                  carto::core::Revision centerline_revision,
                                  carto::core::Revision road_revision,
                                  double offset) {
        carto::geometry::CurvePath centerline;
        centerline.source_identity = std::string(centerline_identity);
        centerline.source_revision = centerline_revision;
        centerline.segments.push_back({
            {0.0, offset, 0.0}, {1.0, offset, 0.0},
            {2.0, offset, 1.0}, {3.0, offset, 1.0}});
        centerline.segments.push_back({
            {3.0, offset, 1.0}, {4.0, offset, 1.0},
            {5.0, offset, 2.0}, {6.0, offset, 2.0}});
        carto::project::RoadSource road;
        road.source_identity = std::string(road_identity);
        road.source_revision = road_revision;
        road.centerline_identity = std::string(centerline_identity);
        road.centerline_revision = centerline_revision;
        road.width_meters = 5.0;
        road.shoulder_meters = 0.5;
        road.drainage_intent = carto::project::RoadDrainageIntent::swale_crossing;
        return carto::application::SetRoadAuthoringAction{
            expected_project_revision, std::move(centerline), std::move(road)};
    };

    auto snapshot = session.snapshot();
    const auto centerline_revision = session.next_curve_path_revision(centerline_identity);
    const auto road_revision = session.next_road_source_revision(road_identity);
    REQUIRE(centerline_revision && road_revision);
    const auto created = carto::application::testing::dispatch(session, build_action(
        snapshot.project_revision, centerline_revision.value(), road_revision.value(), 0.0));
    REQUIRE(created && created.value().revision_after == snapshot.project_revision.next());
    snapshot = session.snapshot();
    REQUIRE(snapshot.curve_paths.size() == 1U && snapshot.road_sources.size() == 1U);
    REQUIRE(snapshot.curve_paths.front().source_revision == carto::core::Revision{1U});
    REQUIRE(snapshot.curve_paths.front().segment_count == 2U);
    REQUIRE(snapshot.road_sources.front().source_revision == carto::core::Revision{1U});
    REQUIRE(snapshot.road_sources.front().centerline_revision ==
        snapshot.curve_paths.front().source_revision);

    const auto before_stale = session.snapshot();
    const auto stale = carto::application::testing::dispatch(session, build_action(
        carto::core::Revision{}, carto::core::Revision{2U},
        carto::core::Revision{2U}, 2.0));
    REQUIRE(!stale);
    REQUIRE(session.snapshot().project_revision == before_stale.project_revision);
    REQUIRE(session.snapshot().undo_count == before_stale.undo_count);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().curve_paths.empty() && session.snapshot().road_sources.empty());
    REQUIRE(session.next_curve_path_revision(centerline_identity).value() ==
        carto::core::Revision{2U});
    REQUIRE(session.next_road_source_revision(road_identity).value() ==
        carto::core::Revision{2U});
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.curve_paths.front().source_revision == carto::core::Revision{2U});
    REQUIRE(snapshot.road_sources.front().source_revision == carto::core::Revision{2U});

    const auto next_curve = session.next_curve_path_revision(centerline_identity);
    const auto next_road = session.next_road_source_revision(road_identity);
    REQUIRE(next_curve && next_road);
    REQUIRE(carto::application::testing::dispatch(session, build_action(
        snapshot.project_revision, next_curve.value(), next_road.value(), 2.0)));
    auto updated_path = session.curve_path_source(centerline_identity);
    REQUIRE(updated_path && updated_path.value().source_revision == carto::core::Revision{3U});
    REQUIRE(updated_path.value().segments.size() == 2U);
    REQUIRE(updated_path.value().segments.front().p0.y == 2.0);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    updated_path = session.curve_path_source(centerline_identity);
    REQUIRE(updated_path && updated_path.value().source_revision == carto::core::Revision{4U});
    REQUIRE(updated_path.value().segments.front().p0.y == 0.0);
    REQUIRE(session.snapshot().road_sources.front().source_revision ==
        carto::core::Revision{4U});
    REQUIRE(session.snapshot().road_sources.front().centerline_revision ==
        updated_path.value().source_revision);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    updated_path = session.curve_path_source(centerline_identity);
    REQUIRE(updated_path && updated_path.value().source_revision == carto::core::Revision{5U});
    REQUIRE(updated_path.value().segments.front().p0.y == 2.0);
    REQUIRE(session.snapshot().road_sources.front().source_revision ==
        carto::core::Revision{5U});

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened && reopened.value().curve_paths().at(
        std::string(centerline_identity)).source_revision == carto::core::Revision{5U});
    REQUIRE(reopened.value().curve_paths().at(
        std::string(centerline_identity)).segments.size() == 2U);
    REQUIRE(reopened.value().road_sources().at(std::string(road_identity)).
        centerline_revision == carto::core::Revision{5U});
    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto events = journal.read_all();
    REQUIRE(events && events.value().size() >= 7U);
    REQUIRE(std::any_of(events.value().begin(), events.value().end(), [](const auto& event) {
        return std::string(event.payload.begin(), event.payload.end()).find(
            "Set Road Authoring") != std::string::npos;
    }));
}

void application_builds_read_only_revision_bound_road_surface_previews() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Road surface preview"}));
    carto::geometry::CurvePath centerline;
    centerline.source_identity = "road.preview.centerline";
    centerline.source_revision = carto::core::Revision{1U};
    centerline.segments.push_back({
        {0.0, 0.0, 0.0}, {10.0 / 3.0, 0.0, 0.0},
        {20.0 / 3.0, 0.0, 0.0}, {10.0, 0.0, 0.0}});
    carto::project::RoadSource road;
    road.source_identity = "road.preview.source";
    road.source_revision = carto::core::Revision{1U};
    road.centerline_identity = centerline.source_identity;
    road.centerline_revision = centerline.source_revision;
    road.width_meters = 6.0;
    road.shoulder_meters = 1.0;
    road.minimum_curve_radius_meters = 20.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetRoadAuthoringAction{
            session.snapshot().project_revision, centerline, road}));

    const auto before = session.snapshot();
    REQUIRE(before.road_sources.size() == 1U && before.curve_paths.size() == 1U);
    const auto preview = session.road_surface_preview(road.source_identity, 8U);
    REQUIRE(preview && preview.value().mesh.validate());
    REQUIRE(preview.value().road_revision == before.road_sources.front().source_revision &&
            preview.value().road_digest == before.road_sources.front().digest &&
            preview.value().centerline_digest == before.curve_paths.front().digest &&
            preview.value().project_generation == before.project_generation);
    REQUIRE(preview.value().matches(before.project_generation,
            before.road_sources.front(), before.curve_paths.front()));
    REQUIRE(!preview.value().matches(before.project_generation + 1U,
            before.road_sources.front(), before.curve_paths.front()));
    REQUIRE(preview.value().mesh.centerline_identity == centerline.source_identity &&
            preview.value().mesh.centerline_revision == centerline.source_revision &&
            preview.value().mesh.road_triangle_count() == 16U);
    const auto after_preview = session.snapshot();
    REQUIRE(after_preview.project_revision == before.project_revision &&
            after_preview.undo_count == before.undo_count &&
            after_preview.redo_count == before.redo_count &&
            after_preview.dirty == before.dirty);
    REQUIRE(!session.road_surface_preview("road.preview.missing"));

    centerline.source_revision = carto::core::Revision{2U};
    road.source_revision = carto::core::Revision{2U};
    road.centerline_revision = centerline.source_revision;
    road.width_meters = 8.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetRoadAuthoringAction{
            after_preview.project_revision, centerline, road}));
    const auto updated = session.snapshot();
    REQUIRE(updated.road_sources.front().digest != preview.value().road_digest &&
            updated.curve_paths.front().digest != preview.value().centerline_digest &&
            updated.road_sources.front().source_revision != preview.value().road_revision);
    REQUIRE(!preview.value().matches(updated.project_generation,
            updated.road_sources.front(), updated.curve_paths.front()));
    const auto refreshed = session.road_surface_preview(road.source_identity, 8U);
    REQUIRE(refreshed && refreshed.value().road_digest == updated.road_sources.front().digest &&
            refreshed.value().centerline_digest == updated.curve_paths.front().digest &&
            refreshed.value().mesh.road_width_meters == 8.0 &&
            refreshed.value().matches(updated.project_generation,
                updated.road_sources.front(), updated.curve_paths.front()));
}

void application_builds_bounded_source_bound_road_terrain_cut_fill_previews() {
    using carto::application::RoadTerrainCoordinateFrame;
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Road terrain preview"}));

    carto::geometry::CurvePath centerline;
    centerline.source_identity = "road.terrain.preview.centerline";
    centerline.source_revision = carto::core::Revision{1U};
    centerline.segments.push_back({
        {0.0, -0.6, 0.0}, {10.0 / 3.0, -0.2, 0.0},
        {20.0 / 3.0, 0.2, 0.0}, {10.0, 0.6, 0.0}});
    carto::project::RoadSource road;
    road.source_identity = "road.terrain.preview.source";
    road.source_revision = carto::core::Revision{1U};
    road.centerline_identity = centerline.source_identity;
    road.centerline_revision = centerline.source_revision;
    road.width_meters = 4.0;
    road.shoulder_meters = 0.5;
    road.maximum_grade_ratio = 0.25;
    road.minimum_curve_radius_meters = 10.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetRoadAuthoringAction{
            session.snapshot().project_revision, centerline, road}));

    constexpr std::string_view terrain_identity = "terrain.tile.x0.z0";
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            std::string(terrain_identity),
            carto::spatial_field::FieldSemantic::terrain_elevation,
            {-5.0, -5.0}, 1.0, 21U, 11U, -10.0, 10.0, 0.0}));

    const auto source_snapshot = session.snapshot();
    REQUIRE(source_snapshot.road_sources.size() == 1U &&
            source_snapshot.curve_paths.size() == 1U &&
            source_snapshot.spatial_fields.size() == 1U);
    const auto& terrain_source = source_snapshot.spatial_fields.front();
    const auto preview = session.road_terrain_preview(
        road.source_identity, terrain_identity,
        RoadTerrainCoordinateFrame::world_xz_elevation_y, 8U);
    REQUIRE(preview && preview.value().validate());
    REQUIRE(preview.value().coordinate_frame ==
            RoadTerrainCoordinateFrame::world_xz_elevation_y);
    REQUIRE(preview.value().cut_volume_cubic_meters > 0.0);
    REQUIRE(preview.value().fill_volume_cubic_meters > 0.0);
    REQUIRE(!preview.value().cells.empty());
    REQUIRE(preview.value().candidate_grid_tests >= preview.value().cells.size());
    REQUIRE(preview.value().candidate_grid_tests <=
            carto::application::RoadTerrainPreview::kMaxCandidateGridTests);
    REQUIRE(preview.value().matches(source_snapshot.project_generation,
        source_snapshot.road_sources.front(), source_snapshot.curve_paths.front(),
        terrain_source));
    REQUIRE(preview.value().matches_sources(source_snapshot.project_generation,
        source_snapshot.road_sources.front(), source_snapshot.curve_paths.front(),
        terrain_source));
    REQUIRE(!preview.value().matches(source_snapshot.project_generation + 1U,
        source_snapshot.road_sources.front(), source_snapshot.curve_paths.front(),
        terrain_source));
    REQUIRE(!preview.value().matches_sources(source_snapshot.project_generation + 1U,
        source_snapshot.road_sources.front(), source_snapshot.curve_paths.front(),
        terrain_source));
    for (const auto& cell : preview.value().cells) {
        REQUIRE(cell.position.x == preview.value().terrain_origin_xz.x +
            static_cast<double>(cell.u) * preview.value().terrain_cell_size_meters);
        REQUIRE(cell.position.z == preview.value().terrain_origin_xz.y +
            static_cast<double>(cell.v) * preview.value().terrain_cell_size_meters);
        REQUIRE(cell.position.y == cell.terrain_elevation_meters);
    }
    const auto repeated = session.road_terrain_preview(
        road.source_identity, terrain_identity,
        RoadTerrainCoordinateFrame::world_xz_elevation_y, 8U);
    REQUIRE(repeated && repeated.value().preview_digest == preview.value().preview_digest);

    const auto after_preview = session.snapshot();
    REQUIRE(after_preview.project_revision == source_snapshot.project_revision);
    REQUIRE(after_preview.undo_count == source_snapshot.undo_count);
    REQUIRE(after_preview.redo_count == source_snapshot.redo_count);
    REQUIRE(after_preview.dirty == source_snapshot.dirty);
    REQUIRE(!session.road_terrain_preview("road.terrain.missing", terrain_identity,
        RoadTerrainCoordinateFrame::world_xz_elevation_y));
    REQUIRE(!session.road_terrain_preview(road.source_identity, "terrain.tile.missing",
        RoadTerrainCoordinateFrame::world_xz_elevation_y));
    const auto unsupported_frame = session.road_terrain_preview(
        road.source_identity, terrain_identity,
        static_cast<RoadTerrainCoordinateFrame>(255U));
    REQUIRE(!unsupported_frame && unsupported_frame.error().code ==
            carto::core::ErrorCode::invalid_argument);
    REQUIRE(!session.road_terrain_preview(road.source_identity, terrain_identity,
        RoadTerrainCoordinateFrame::world_xz_elevation_y, 0U));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            "terrain.tile.x1.z0", carto::spatial_field::FieldSemantic::generic,
            {-5.0, -5.0}, 1.0, 21U, 11U, -10.0, 10.0, 0.0}));
    const auto wrong_semantic = session.road_terrain_preview(
        road.source_identity, "terrain.tile.x1.z0",
        RoadTerrainCoordinateFrame::world_xz_elevation_y);
    REQUIRE(!wrong_semantic && wrong_semantic.error().code ==
            carto::core::ErrorCode::validation_failed);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSpatialFieldAction{
            "terrain.tile.x2.z0", carto::spatial_field::FieldSemantic::terrain_elevation,
            {1.0, -1.0}, 1.0, 3U, 3U, -10.0, 10.0, 0.0}));
    const auto uncovered = session.road_terrain_preview(
        road.source_identity, "terrain.tile.x2.z0",
        RoadTerrainCoordinateFrame::world_xz_elevation_y);
    REQUIRE(!uncovered && uncovered.error().code ==
            carto::core::ErrorCode::validation_failed);

    carto::spatial_field::FieldBrush brush;
    brush.expected_revision = terrain_source.source_revision;
    brush.center_uv = {0.0, 0.0};
    brush.radius = 1.5;
    brush.value = 1.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::PaintSpatialFieldAction{
            std::string(terrain_identity), brush}));
    const auto refreshed_snapshot = session.snapshot();
    const auto refreshed_terrain = std::find_if(
        refreshed_snapshot.spatial_fields.begin(), refreshed_snapshot.spatial_fields.end(),
        [terrain_identity](const auto& source) {
            return source.identity == terrain_identity;
        });
    REQUIRE(refreshed_terrain != refreshed_snapshot.spatial_fields.end());
    REQUIRE(refreshed_terrain->source_revision != terrain_source.source_revision);
    REQUIRE(!preview.value().matches(refreshed_snapshot.project_generation,
        refreshed_snapshot.road_sources.front(), refreshed_snapshot.curve_paths.front(),
        *refreshed_terrain));
    REQUIRE(!preview.value().matches_sources(refreshed_snapshot.project_generation,
        refreshed_snapshot.road_sources.front(), refreshed_snapshot.curve_paths.front(),
        *refreshed_terrain));
    const auto refreshed = session.road_terrain_preview(
        road.source_identity, terrain_identity,
        RoadTerrainCoordinateFrame::world_xz_elevation_y, 8U);
    REQUIRE(refreshed && refreshed.value().preview_digest != preview.value().preview_digest);
    REQUIRE(refreshed.value().matches(refreshed_snapshot.project_generation,
        refreshed_snapshot.road_sources.front(), refreshed_snapshot.curve_paths.front(),
        *refreshed_terrain));
}

void application_commits_revision_bound_road_mesh_through_history_and_reopen() {
    TempDirectory temp;
    const auto path = temp.path() / "committed-road-surface.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Committed road surface"}));

    carto::geometry::CurvePath centerline;
    centerline.source_identity = "road.commit.centerline";
    centerline.source_revision = carto::core::Revision{1U};
    centerline.segments.push_back({
        {0.0, 0.0, 0.0}, {10.0 / 3.0, 0.0, 0.0},
        {20.0 / 3.0, 0.0, 0.0}, {10.0, 0.0, 0.0}});
    carto::project::RoadSource road;
    road.source_identity = "road.commit.source";
    road.source_revision = carto::core::Revision{1U};
    road.centerline_identity = centerline.source_identity;
    road.centerline_revision = centerline.source_revision;
    road.width_meters = 6.0;
    road.shoulder_meters = 1.0;
    road.minimum_curve_radius_meters = 20.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetRoadAuthoringAction{
            session.snapshot().project_revision, centerline, road}));

    const auto source_snapshot = session.snapshot();
    const auto preview = session.road_surface_preview(road.source_identity, 8U);
    REQUIRE(preview && preview.value().matches(source_snapshot.project_generation,
        source_snapshot.road_sources.front(), source_snapshot.curve_paths.front()));
    const auto load_saved_project = [&path]() {
        auto loaded = carto::project::ProjectDocument::load(path);
        REQUIRE(loaded);
        return std::move(loaded).value();
    };
    const auto action_for = [&preview, &source_snapshot]() {
        return carto::application::CreateRoadSurfaceInstanceAction{
            source_snapshot.project_generation,
            source_snapshot.project_revision,
            preview.value().road_identity,
            preview.value().road_revision,
            preview.value().road_digest,
            preview.value().mesh.centerline_identity,
            preview.value().mesh.centerline_revision,
            preview.value().centerline_digest,
            preview.value().mesh.samples_per_segment};
    };

    auto stale_action = action_for();
    stale_action.expected_centerline_revision = carto::core::Revision{2U};
    const auto stale_result = carto::application::testing::dispatch(session, stale_action);
    REQUIRE(!stale_result && stale_result.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == source_snapshot.project_revision &&
            session.snapshot().undo_count == source_snapshot.undo_count &&
            session.snapshot().objects.empty());

    const auto committed_result =
        carto::application::testing::dispatch(session, action_for());
    if (!committed_result) {
        std::cerr << "road surface commit: " << committed_result.error().message << '\n';
    }
    REQUIRE(committed_result);
    auto committed = session.snapshot();
    REQUIRE(committed.objects.size() == 1U &&
            committed.objects.front().object.mesh_asset.has_value() &&
            committed.undo_count == source_snapshot.undo_count + 1U);
    const auto object_id = committed.objects.front().object.id;
    const auto mesh_asset = *committed.objects.front().object.mesh_asset;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));

    const auto reopened = load_saved_project();
    REQUIRE(reopened.road_surface_instance_receipts().size() == 1U);
    const auto& receipt = reopened.road_surface_instance_receipts().at(object_id);
    REQUIRE(receipt.mesh_asset == mesh_asset &&
            receipt.road_identity == road.source_identity &&
            receipt.road_revision == road.source_revision &&
            receipt.centerline_identity == centerline.source_identity &&
            receipt.centerline_revision == centerline.source_revision &&
            receipt.samples_per_segment == 8U &&
            receipt.road_digest == road.source_digest() &&
            receipt.generated_mesh_digest ==
                carto::application::canonical_mesh_geometry_digest(
                    reopened.meshes().at(mesh_asset)));
    const auto& committed_mesh = reopened.meshes().at(mesh_asset);
    REQUIRE(committed_mesh.face_count() == 24U &&
            committed_mesh.attributes().find(
                carto::geometry::kRoadSurfaceMaterialSlotLayerIdentity) != nullptr &&
            committed_mesh.attributes().find(
                carto::geometry::kRoadSurfaceUvLayerIdentity) != nullptr);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.empty());
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto reopened_after_undo = load_saved_project();
    REQUIRE(reopened_after_undo.road_surface_instance_receipts().empty());

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().objects.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto reopened_after_redo = load_saved_project();
    REQUIRE(reopened_after_redo.road_surface_instance_receipts().size() == 1U &&
            reopened_after_redo.road_surface_instance_receipts().contains(object_id));

    auto revised_centerline = centerline;
    revised_centerline.source_revision = carto::core::Revision{2U};
    auto revised_road = road;
    revised_road.source_revision = carto::core::Revision{2U};
    revised_road.centerline_revision = revised_centerline.source_revision;
    revised_road.width_meters = 8.0;
    const auto before_source_revision = session.snapshot();
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetRoadAuthoringAction{
            before_source_revision.project_revision,
            revised_centerline, revised_road}));
    const auto before_stale_commit = session.snapshot();
    auto old_preview_action = action_for();
    old_preview_action.expected_project_generation = before_stale_commit.project_generation;
    old_preview_action.expected_project_revision = before_stale_commit.project_revision;
    const auto old_preview_commit = carto::application::testing::dispatch(
        session, old_preview_action);
    REQUIRE(!old_preview_commit &&
            old_preview_commit.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before_stale_commit.project_revision &&
            session.snapshot().undo_count == before_stale_commit.undo_count &&
            session.snapshot().objects.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto reopened_after_source_revision = load_saved_project();
    REQUIRE(reopened_after_source_revision.road_surface_instance_receipts().size() == 1U &&
            reopened_after_source_revision.road_surface_instance_receipts().at(object_id).road_revision ==
                carto::core::Revision{1U} &&
            reopened_after_source_revision.road_sources().at(road.source_identity).source_revision ==
                carto::core::Revision{2U});
}

void application_authors_revision_bound_sweeps_through_history() {
    TempDirectory temp;
    const auto project_path = temp.path() / "sweep-authoring.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Sweep authoring"}));

    carto::geometry::CurvePath centerline;
    centerline.source_identity = "sweep.authoring.centerline";
    centerline.source_revision = carto::core::Revision{1U};
    centerline.segments.push_back({
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0}, {3.0, 0.0, 0.0}});
    carto::project::RoadSource road;
    road.source_identity = "sweep.authoring.path-owner";
    road.source_revision = carto::core::Revision{1U};
    road.centerline_identity = centerline.source_identity;
    road.centerline_revision = centerline.source_revision;
    road.width_meters = 4.0;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetRoadAuthoringAction{
            session.snapshot().project_revision, centerline, road}));
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{project_path}));
    const auto journal_path = std::filesystem::path(project_path.string() + ".journal");
    carto::journal::Journal journal(journal_path);
    const auto baseline_journal = journal.read_all();
    REQUIRE(baseline_journal && baseline_journal.value().size() == 1U &&
            baseline_journal.value().front().event_type == "cartographer.snapshot");

    carto::geometry::SweepSource source;
    source.source_identity = "sweep.authoring.pipe";
    source.centerline_identity = centerline.source_identity;
    source.centerline_revision = centerline.source_revision;
    source.reference_up = {0.0, 1.0, 0.0};
    source.samples_per_segment = 4U;
    source.profile.reserve(8U);
    constexpr double kTau = 6.283185307179586476925286766559;
    for (std::size_t index = 0U; index < 8U; ++index) {
        const double angle = kTau * static_cast<double>(index) / 8.0;
        source.profile.push_back({0.1 * std::cos(angle), 0.1 * std::sin(angle)});
    }
    const auto next_source_revision = session.next_sweep_source_revision(
        source.source_identity);
    REQUIRE(next_source_revision);
    source.source_revision = next_source_revision.value();

    const auto expect_rejected_without_mutation = [&session, &journal](
        const carto::geometry::SweepSource& rejected_source,
        carto::core::ErrorCode expected_error,
        carto::application::OperationSource operation_source) {
        const auto before = session.snapshot();
        const auto document_before = session.document_.serialize();
        const auto journal_before = journal.read_all();
        REQUIRE(journal_before);
        const carto::application::SetSweepSourceAction action{
            before.project_revision, rejected_source};
        const auto result = [&]() {
            if (operation_source == carto::application::OperationSource::human) {
                return carto::application::testing::dispatch(session, action);
            }
            const auto previous_source = session.active_source_;
            session.active_source_ = operation_source;
            auto rejected = session.set_sweep_source(action);
            session.active_source_ = previous_source;
            return rejected;
        }();
        REQUIRE(!result && result.error().code == expected_error);
        const auto after = session.snapshot();
        REQUIRE(after.project_revision == before.project_revision &&
                after.project_generation == before.project_generation &&
                after.undo_count == before.undo_count &&
                after.redo_count == before.redo_count && after.dirty == before.dirty &&
                after.sweep_sources.size() == before.sweep_sources.size());
        REQUIRE(session.document_.serialize() == document_before);
        const auto journal_after = journal.read_all();
        REQUIRE(journal_after && journal_after.value().size() == journal_before.value().size());
        REQUIRE(journal_after.value().back().entry_hash ==
                journal_before.value().back().entry_hash);
        REQUIRE(journal.verify());
    };

    auto invalid_recipe = source;
    invalid_recipe.profile.front().x = std::numeric_limits<double>::quiet_NaN();
    expect_rejected_without_mutation(
        invalid_recipe, carto::core::ErrorCode::validation_failed,
        carto::application::OperationSource::human);
    auto stale_centerline = source;
    stale_centerline.centerline_revision = carto::core::Revision{2U};
    expect_rejected_without_mutation(
        stale_centerline, carto::core::ErrorCode::stale_data,
        carto::application::OperationSource::human);
    expect_rejected_without_mutation(
        source, carto::core::ErrorCode::unsupported,
        carto::application::OperationSource::agent_workbench);
    REQUIRE(session.next_sweep_source_revision(source.source_identity).value() ==
            carto::core::Revision{1U});

    const auto before_create = session.snapshot();
    const auto created = carto::application::testing::dispatch(session,
        carto::application::SetSweepSourceAction{
            before_create.project_revision, source});
    REQUIRE(created);
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.sweep_sources.size() == 1U &&
            snapshot.sweep_sources.front().source.source_revision ==
                carto::core::Revision{1U});
    auto journal_after_create = journal.read_all();
    REQUIRE(journal_after_create && journal_after_create.value().size() == 2U &&
            journal_after_create.value().back().event_type ==
                "cartographer.application_action" &&
            journal_after_create.value().back().revision_after == snapshot.project_revision);
    const std::string create_payload(
        journal_after_create.value().back().payload.begin(),
        journal_after_create.value().back().payload.end());
    REQUIRE(create_payload.find("CARTOGRAPHER_SWEEP_SOURCE 1") != std::string::npos &&
            create_payload.find(source.source_identity) != std::string::npos &&
            journal.verify());

    const auto journal_before_preview = journal_after_create.value().back().entry_hash;
    const auto preview = session.sweep_source_preview(source.source_identity);
    REQUIRE(preview && preview.value().mesh.validate());
    REQUIRE(preview.value().source_identity == source.source_identity &&
            preview.value().source_revision == carto::core::Revision{1U} &&
            preview.value().centerline_identity == centerline.source_identity &&
            preview.value().centerline_revision == centerline.source_revision);
    const auto after_preview = session.snapshot();
    REQUIRE(after_preview.project_revision == snapshot.project_revision &&
            after_preview.undo_count == snapshot.undo_count &&
            after_preview.dirty == snapshot.dirty);
    journal_after_create = journal.read_all();
    REQUIRE(journal_after_create && journal_after_create.value().size() == 2U &&
            journal_after_create.value().back().entry_hash == journal_before_preview);
    REQUIRE(!session.sweep_source_preview("sweep.authoring.missing"));

    const auto before_stale_action = session.snapshot();
    const auto stale_document_before = session.document_.serialize();
    const auto stale = carto::application::testing::dispatch(session,
        carto::application::SetSweepSourceAction{before_create.project_revision, source});
    REQUIRE(!stale && stale.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before_stale_action.project_revision &&
            session.snapshot().undo_count == before_stale_action.undo_count &&
            session.document_.serialize() == stale_document_before);
    journal_after_create = journal.read_all();
    REQUIRE(journal_after_create && journal_after_create.value().size() == 2U &&
            journal_after_create.value().back().entry_hash == journal_before_preview);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().sweep_sources.empty() &&
            session.next_sweep_source_revision(source.source_identity).value() ==
                carto::core::Revision{2U});
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.sweep_sources.size() == 1U &&
            snapshot.sweep_sources.front().source.source_revision ==
                carto::core::Revision{2U});

    auto edited_source = snapshot.sweep_sources.front().source;
    edited_source.end_scale = 0.5;
    edited_source.source_revision = session.next_sweep_source_revision(
        edited_source.source_identity).value();
    const auto old_digest = snapshot.sweep_sources.front().digest;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSweepSourceAction{
            snapshot.project_revision, edited_source}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.sweep_sources.front().digest != old_digest);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.sweep_sources.front().source.end_scale == 1.0 &&
            snapshot.sweep_sources.front().source.source_revision ==
                carto::core::Revision{4U});
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.sweep_sources.front().source.end_scale == 0.5 &&
            snapshot.sweep_sources.front().source.source_revision ==
                carto::core::Revision{5U});

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    const auto reopened_snapshot = reopened.snapshot();
    REQUIRE(reopened_snapshot.sweep_sources.size() == 1U &&
            reopened_snapshot.sweep_sources.front().source.source_revision ==
                carto::core::Revision{5U} &&
            reopened_snapshot.sweep_sources.front().source.end_scale == 0.5 &&
            reopened_snapshot.curve_paths.size() == 1U);
}

carto::biological::BiologicalAuthoringDocument application_biological_source(
    std::uint64_t revision, double radius) {
    const auto digest = [](std::string_view value) {
        return carto::assets::sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    };
    carto::biological::BiologicalAuthoringDocument source;
    source.source = carto::biological::AuthoringSourceIdentity{
        "fixtures/application-biological", 8811U, carto::core::Revision{revision},
        digest("application-biological-source-" + std::to_string(revision))};
    source.organism_family = "coarse-proxy-fixture";
    const auto inserted = source.insert_structure(carto::biological::StructureRecord{
        .id = carto::biological::StructureId{1U},
        .kind = carto::biological::StructureKind::organism,
        .parent = std::nullopt,
        .semantic_type = "organism.proxy",
        .coordinate_frame = "organism.local",
        .kinematic_joint = std::nullopt,
        .kinematic_graph_digest = std::nullopt,
        .geometry_digest = std::nullopt,
        .proxy_surface = carto::biological::EllipsoidProxyProfile{
            .center = {0.0, 0.0, 0.0}, .radii = {radius, 1.0, 0.5},
            .longitude_segments = 12U, .latitude_segments = 6U,
        },
    });
    REQUIRE(inserted);
    REQUIRE(source.validate());
    return source;
}

void application_routes_biological_authoring_and_proxy_preview_through_history() {
    TempDirectory temp;
    const auto project_path = temp.path() / "biological-authoring.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::NewProjectAction{"Biological authoring"}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));

    auto snapshot = session.snapshot();
    const auto initial_source = application_biological_source(7U, 0.75);
    const carto::project::BiologicalAuthoringKey key{
        initial_source.source.source_namespace, initial_source.source.asset_id};
    const auto committed = carto::application::testing::dispatch(session,
        carto::application::SetBiologicalAuthoringAction{
            snapshot.project_revision, initial_source});
    REQUIRE(committed && committed.value().document_changed);
    snapshot = session.snapshot();
    REQUIRE(snapshot.biological_sources.size() == 1U &&
        snapshot.biological_sources.front().key == key);
    const auto next_revision = session.next_biological_authoring_revision(key);
    REQUIRE(next_revision && next_revision.value() == carto::core::Revision{8U});

    const auto source = session.biological_authoring_source(key, snapshot.project_revision);
    REQUIRE(source && source.value().source.revision == carto::core::Revision{7U});
    const auto preview = session.biological_proxy_preview(
        key, snapshot.project_revision, carto::biological::StructureId{1U});
    REQUIRE(preview && preview.value().mesh.vertex_count() == 62U &&
        preview.value().validate_against(source.value()));
    REQUIRE(session.snapshot().project_revision == snapshot.project_revision &&
        session.snapshot().objects.empty());
    const auto stale_project = carto::application::testing::dispatch(session,
        carto::application::CreateBiologicalProxyInstanceAction{
            carto::core::Revision{snapshot.project_revision.value() + 1U}, key,
            source.value().source.revision, preview.value().structure,
            snapshot.biological_sources.front().payload_digest,
            preview.value().product_digest});
    REQUIRE(!stale_project && session.snapshot().objects.empty());
    const auto stale_source = carto::application::testing::dispatch(session,
        carto::application::CreateBiologicalProxyInstanceAction{
            snapshot.project_revision, key,
            carto::core::Revision{source.value().source.revision.value() + 1U},
            preview.value().structure,
            snapshot.biological_sources.front().payload_digest,
            preview.value().product_digest});
    REQUIRE(!stale_source && session.snapshot().objects.empty());
    auto wrong_source_digest = snapshot.biological_sources.front().payload_digest;
    wrong_source_digest.bytes[0] ^= 0x01U;
    const auto stale_payload = carto::application::testing::dispatch(session,
        carto::application::CreateBiologicalProxyInstanceAction{
            snapshot.project_revision, key, source.value().source.revision,
            preview.value().structure, wrong_source_digest,
            preview.value().product_digest});
    REQUIRE(!stale_payload && session.snapshot().objects.empty());
    const auto rejected_product = [&]() {
        auto wrong_digest = preview.value().product_digest;
        wrong_digest.bytes[0] ^= 0x01U;
        return carto::application::testing::dispatch(session,
            carto::application::CreateBiologicalProxyInstanceAction{
                snapshot.project_revision, key, source.value().source.revision,
                preview.value().structure,
                snapshot.biological_sources.front().payload_digest, wrong_digest});
    }();
    REQUIRE(!rejected_product && session.snapshot().project_revision ==
        snapshot.project_revision && session.snapshot().objects.empty());
    const auto materialized = carto::application::testing::dispatch(session,
        carto::application::CreateBiologicalProxyInstanceAction{
            snapshot.project_revision, key, source.value().source.revision,
            preview.value().structure,
            snapshot.biological_sources.front().payload_digest,
            preview.value().product_digest});
    REQUIRE(materialized && materialized.value().document_changed);
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U &&
        snapshot.objects.front().object.mesh_asset.has_value());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    auto materialized_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(materialized_project &&
        materialized_project.value().biological_proxy_instance_receipts().size() == 1U);
    const auto instance_receipt =
        materialized_project.value().biological_proxy_instance_receipts().begin()->second;
    REQUIRE(instance_receipt.validate() && instance_receipt.source_namespace == key.first &&
        instance_receipt.source_asset_id == key.second &&
        instance_receipt.source_revision == source.value().source.revision &&
        instance_receipt.structure_id == preview.value().structure.value &&
        instance_receipt.product_digest == preview.value().product_digest &&
        instance_receipt.mesh_asset == *snapshot.objects.front().object.mesh_asset);
    const std::string serialized_materialized = materialized_project.value().serialize();
    auto corrupted_product = serialized_materialized;
    const auto receipt_marker = corrupted_product.find(
        "BIOLOGICAL_PROXY_INSTANCE_RECEIPT ");
    const auto product_digest_offset = corrupted_product.find(
        instance_receipt.product_digest.hex(), receipt_marker);
    REQUIRE(receipt_marker != std::string::npos &&
        product_digest_offset != std::string::npos);
    corrupted_product.replace(product_digest_offset,
        instance_receipt.product_digest.hex().size(),
        carto::assets::Sha256Digest{}.hex());
    REQUIRE(!carto::project::ProjectDocument::deserialize(corrupted_product));
    auto missing_receipt_table = serialized_materialized;
    const auto receipt_table_offset = missing_receipt_table.rfind(
        "BIOLOGICAL_PROXY_INSTANCE_RECEIPTS ");
    REQUIRE(receipt_table_offset != std::string::npos);
    missing_receipt_table.resize(receipt_table_offset);
    missing_receipt_table += "END\n";
    REQUIRE(!carto::project::ProjectDocument::deserialize(missing_receipt_table));

    const auto duplicate = carto::application::testing::dispatch(session,
        carto::application::DuplicateMeshInstanceAction{
            snapshot.objects.front().object.id, "Biological proxy duplicate",
            {1.0, 0.0, 0.0}});
    REQUIRE(duplicate);
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 2U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    auto duplicated_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(duplicated_project &&
        duplicated_project.value().biological_proxy_instance_receipts().size() == 2U);
    const auto shared_asset = *snapshot.objects.front().object.mesh_asset;
    REQUIRE(std::all_of(snapshot.objects.begin(), snapshot.objects.end(),
        [shared_asset](const auto& object) {
            return object.object.mesh_asset == std::optional<std::uint64_t>{shared_asset};
        }));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.snapshot().objects.size() == 2U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.empty());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto undone_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(undone_project && undone_project.value().biological_proxy_instance_receipts().empty());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto redone_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(redone_project &&
        redone_project.value().biological_proxy_instance_receipts().size() == 1U);
    const auto same_geometry = [](const carto::biological::ProxyMeshProduct& left,
                                   const carto::biological::ProxyMeshProduct& right) {
        const auto left_vertices = left.mesh.vertices_sorted();
        const auto right_vertices = right.mesh.vertices_sorted();
        if (left_vertices.size() != right_vertices.size()) return false;
        for (std::size_t index = 0U; index < left_vertices.size(); ++index) {
            if (left_vertices[index].position.x != right_vertices[index].position.x ||
                left_vertices[index].position.y != right_vertices[index].position.y ||
                left_vertices[index].position.z != right_vertices[index].position.z) {
                return false;
            }
        }
        return left.mesh.faces_sorted().size() == right.mesh.faces_sorted().size();
    };

    auto updated_source = application_biological_source(8U, 1.0);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetBiologicalAuthoringAction{
            snapshot.project_revision, updated_source}));
    const auto updated_snapshot = session.snapshot();
    const auto stale_preview = session.biological_proxy_preview(
        key, snapshot.project_revision, carto::biological::StructureId{1U});
    REQUIRE(!stale_preview && stale_preview.error().code ==
        carto::core::ErrorCode::stale_data);
    const auto updated_preview = session.biological_proxy_preview(
        key, updated_snapshot.project_revision, carto::biological::StructureId{1U});
    REQUIRE(updated_preview && updated_preview.value().product_digest !=
        preview.value().product_digest && !same_geometry(updated_preview.value(), preview.value()));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    auto undone_snapshot = session.snapshot();
    const auto restored = session.biological_authoring_source(
        key, undone_snapshot.project_revision);
    REQUIRE(restored && restored.value().source.revision == carto::core::Revision{9U});
    const auto restored_preview = session.biological_proxy_preview(
        key, undone_snapshot.project_revision, carto::biological::StructureId{1U});
    REQUIRE(restored_preview && restored_preview.value().product_digest !=
        preview.value().product_digest && same_geometry(restored_preview.value(), preview.value()));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    const auto redone_snapshot = session.snapshot();
    const auto redone_source = session.biological_authoring_source(
        key, redone_snapshot.project_revision);
    REQUIRE(redone_source && redone_source.value().source.revision ==
        carto::core::Revision{10U});
    const auto redone_preview = session.biological_proxy_preview(
        key, redone_snapshot.project_revision, carto::biological::StructureId{1U});
    REQUIRE(redone_preview && redone_preview.value().product_digest !=
        updated_preview.value().product_digest &&
        same_geometry(redone_preview.value(), updated_preview.value()));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    const auto persisted = carto::biological::BiologicalAuthoringDocument::deserialize(
        reopened.value().biological_authoring_sources().at(key).bytes);
    REQUIRE(persisted && persisted.value().source.revision == carto::core::Revision{10U});
    REQUIRE(reopened.value().biological_proxy_instance_receipts().size() == 1U);
    const auto& reopened_receipt =
        reopened.value().biological_proxy_instance_receipts().begin()->second;
    REQUIRE(reopened_receipt.validate() && reopened_receipt.source_namespace == key.first &&
        reopened_receipt.source_asset_id == key.second &&
        reopened_receipt.source_revision == carto::core::Revision{7U} &&
        reopened_receipt.structure_id == preview.value().structure.value &&
        reopened_receipt.product_digest == preview.value().product_digest);

    const auto& base_mesh = reopened.value().meshes().at(reopened_receipt.mesh_asset);
    const auto faces = base_mesh.faces_sorted();
    REQUIRE(!faces.empty() && faces.front().vertices.size() >= 3U);
    const carto::production::SculptSurfaceAnchor surface_anchor{
        faces.front().id,
        {faces.front().vertices[0], faces.front().vertices[1], faces.front().vertices[2]},
        {0.25, 0.25, 0.5}};
    const auto surface_sample = carto::production::make_sculpt_surface_sample(
        base_mesh, surface_anchor);
    REQUIRE(surface_sample);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::vertex}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SelectVertexAction{
            reopened_receipt.object, surface_anchor.vertices[0]}));
    const auto pre_sculpt = session.snapshot();
    const auto* pre_sculpt_instance = pre_sculpt.viewport.scene.find(reopened_receipt.object);
    REQUIRE(pre_sculpt_instance && pre_sculpt_instance->mesh);
    const auto original_positions = pre_sculpt_instance->mesh->positions;
    const auto sculpted = carto::application::testing::dispatch(session,
        carto::application::AppendSculptSurfaceStrokeAction{
            pre_sculpt.project_revision, reopened_receipt.object,
            reopened_receipt.mesh_asset, carto::production::SculptStrokeMode::add,
            0.75, 0.05, {surface_sample.value()}});
    REQUIRE(sculpted && sculpted.value().document_changed);
    const auto post_sculpt = session.snapshot();
    const auto* post_sculpt_instance = post_sculpt.viewport.scene.find(reopened_receipt.object);
    REQUIRE(post_sculpt_instance && post_sculpt_instance->mesh &&
        post_sculpt.sculpt_layers.size() == 1U);
    REQUIRE(post_sculpt_instance->mesh->positions.size() == original_positions.size());
    bool sculpt_changed_viewport = false;
    for (std::size_t index = 0U; index < original_positions.size(); ++index) {
        const auto& before = original_positions[index];
        const auto& after = post_sculpt_instance->mesh->positions[index];
        sculpt_changed_viewport = sculpt_changed_viewport || before.x != after.x ||
            before.y != after.y || before.z != after.z;
    }
    REQUIRE(!sculpt_changed_viewport);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto sculpted_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(sculpted_project && sculpted_project.value().sculpt_stacks().contains(
        reopened_receipt.mesh_asset));
    REQUIRE(sculpted_project.value().biological_proxy_instance_receipts().contains(
        reopened_receipt.object));
    const auto stored_stack = carto::production::SculptStack::deserialize(
        sculpted_project.value().sculpt_stacks().at(reopened_receipt.mesh_asset).bytes);
    REQUIRE(stored_stack);

    carto::journal::Journal journal(
        std::filesystem::path(project_path.string() + ".journal"));
    const auto events = journal.read_all();
    REQUIRE(events && std::any_of(events.value().begin(), events.value().end(),
        [](const auto& event) {
            const std::string payload(event.payload.begin(), event.payload.end());
            return payload.find("Set Biological Authoring Source") != std::string::npos;
        }));
}

void application_authors_and_materializes_revision_bound_sign_text() {
    TempDirectory temp;
    const auto project_path = temp.path() / "sign-text.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::NewProjectAction{"Sign authoring"}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));

    carto::geometry::SignTextSource source;
    source.source_identity = "fixture.sign.motel8";
    source.source_revision = carto::core::Revision{1U};
    source.text = "MOTEL 8\nVACANCY";
    source.board_height = 1.5;
    source.glyph_height = 0.5;
    source.line_spacing = 0.08;
    REQUIRE(source.validate());
    auto snapshot = session.snapshot();
    const auto source_saved = carto::application::testing::dispatch(session,
        carto::application::SetSignTextSourceAction{snapshot.project_revision, source});
    if (!source_saved) std::cerr << "sign source diagnostic: " << source_saved.error().message << '\n';
    REQUIRE(source_saved);
    snapshot = session.snapshot();
    REQUIRE(snapshot.sign_text_sources.size() == 1U &&
        snapshot.sign_text_sources.front().source.source_revision ==
            carto::core::Revision{1U});

    const auto stale_project = carto::application::testing::dispatch(session,
        carto::application::CreateSignTextObjectAction{
            carto::core::Revision{snapshot.project_revision.value() - 1U},
            source.source_identity, source.source_revision});
    REQUIRE(!stale_project &&
        stale_project.error().code == carto::core::ErrorCode::stale_data);
    const auto stale_source = carto::application::testing::dispatch(session,
        carto::application::CreateSignTextObjectAction{
            snapshot.project_revision, source.source_identity, carto::core::Revision{2U}});
    REQUIRE(!stale_source && stale_source.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().objects.empty());

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSignTextObjectAction{
            snapshot.project_revision, source.source_identity, source.source_revision}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U && snapshot.viewport.scene.instances().size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened && reopened.value().sign_text_instance_receipts().size() == 1U);
    const auto object_id = snapshot.objects.front().object.id;
    const auto& receipt = reopened.value().sign_text_instance_receipts().at(object_id);
    REQUIRE(receipt.source_identity == source.source_identity &&
        receipt.source_revision == source.source_revision &&
        receipt.source_snapshot == source.serialize() && receipt.validate());

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().objects.empty());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened_after_undo = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened_after_undo &&
        reopened_after_undo.value().sign_text_instance_receipts().empty() &&
        reopened_after_undo.value().sign_text_sources().size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.snapshot().objects.front().object.id == object_id);
    REQUIRE(session.snapshot().sign_text_sources.front().source.source_revision ==
        carto::core::Revision{1U});

    source.source_revision = session.next_sign_text_source_revision(
        source.source_identity).value();
    source.text = "MOTEL 9\nVACANCY";
    snapshot = session.snapshot();
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSignTextSourceAction{snapshot.project_revision, source}));
    REQUIRE(session.snapshot().sign_text_sources.front().source.source_revision ==
        carto::core::Revision{2U});
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened_after_update = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened_after_update && reopened_after_update.value().validate() &&
        reopened_after_update.value().sign_text_instance_receipts().at(object_id).source_snapshot !=
            reopened_after_update.value().sign_text_sources().at(source.source_identity).serialize());

    carto::geometry::SignTextSource price_board;
    price_board.source_identity = "fixture.sign.pine-ridge-prices";
    price_board.source_revision = carto::core::Revision{1U};
    price_board.text = "PINE RIDGE\nGAS $3.99\nDIESEL $4.29";
    price_board.board_width = 5.8;
    price_board.board_height = 2.0;
    price_board.glyph_height = 0.42;
    price_board.border = 0.15;
    price_board.line_spacing = 0.08;
    REQUIRE(price_board.validate());
    snapshot = session.snapshot();
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSignTextSourceAction{snapshot.project_revision, price_board}));
    snapshot = session.snapshot();
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSignTextObjectAction{
            snapshot.project_revision, price_board.source_identity,
            price_board.source_revision}));

    const auto add_sign_fixture = [&](const carto::geometry::SignTextSource& fixture) {
        auto current = session.snapshot();
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::SetSignTextSourceAction{current.project_revision, fixture}));
        current = session.snapshot();
        REQUIRE(carto::application::testing::dispatch(session,
            carto::application::CreateSignTextObjectAction{
                current.project_revision, fixture.source_identity, fixture.source_revision}));
    };
    carto::geometry::SignTextSource room_number;
    room_number.source_identity = "fixture.sign.room-number-plate";
    room_number.source_revision = carto::core::Revision{1U};
    room_number.text = "ROOM 204";
    room_number.board_width = 2.8;
    room_number.board_height = 0.6;
    room_number.border = 0.08;
    room_number.glyph_height = 0.3;
    REQUIRE(room_number.validate());
    add_sign_fixture(room_number);

    carto::geometry::SignTextSource church_board;
    church_board.source_identity = "fixture.sign.church-message-board";
    church_board.source_revision = carto::core::Revision{1U};
    church_board.text = "SUNDAY SERVICE\n10:30 AM";
    church_board.board_width = 4.5;
    church_board.board_height = 1.5;
    church_board.border = 0.1;
    church_board.glyph_height = 0.3;
    church_board.line_spacing = 0.08;
    REQUIRE(church_board.validate());
    add_sign_fixture(church_board);

    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 4U && snapshot.viewport.scene.instances().size() == 4U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened_fixtures = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened_fixtures && reopened_fixtures.value().validate() &&
        reopened_fixtures.value().sign_text_sources().size() == 4U &&
        reopened_fixtures.value().sign_text_instance_receipts().size() == 4U);
    const auto& motel_receipt =
        reopened_fixtures.value().sign_text_instance_receipts().at(object_id);
    const auto& receipts = reopened_fixtures.value().sign_text_instance_receipts();
    const auto price_receipt_entry = std::find_if(receipts.begin(), receipts.end(),
        [&](const auto& entry) {
            return entry.second.source_identity == price_board.source_identity;
        });
    REQUIRE(price_receipt_entry != receipts.end());
    const auto& price_receipt = price_receipt_entry->second;
    const auto find_receipt = [&](std::string_view identity) {
        return std::find_if(receipts.begin(), receipts.end(), [&](const auto& entry) {
            return entry.second.source_identity == identity;
        });
    };
    const auto room_receipt_entry = find_receipt(room_number.source_identity);
    const auto church_receipt_entry = find_receipt(church_board.source_identity);
    REQUIRE(motel_receipt.source_identity == "fixture.sign.motel8" &&
        motel_receipt.source_revision == carto::core::Revision{1U} &&
        motel_receipt.source_snapshot.find("MOTEL 8") != std::string::npos &&
        price_receipt.source_identity == price_board.source_identity &&
        price_receipt.source_revision == price_board.source_revision &&
        price_receipt.source_snapshot == price_board.serialize() &&
        room_receipt_entry != receipts.end() &&
        room_receipt_entry->second.source_snapshot == room_number.serialize() &&
        church_receipt_entry != receipts.end() &&
        church_receipt_entry->second.source_snapshot == church_board.serialize() &&
        motel_receipt.validate() && price_receipt.validate());
}

void application_assigns_sign_panel_and_lettering_materials_through_carto_history() {
    TempDirectory temp;
    const auto project_path = temp.path() / "sign-material-regions.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));

    carto::geometry::SignTextSource source;
    source.source_identity = "fixture.sign.material-regions";
    source.source_revision = carto::core::Revision{1U};
    source.text = "MOTEL 8\nVACANCY";
    REQUIRE(source.validate());
    auto snapshot = session.snapshot();
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetSignTextSourceAction{snapshot.project_revision, source}));
    snapshot = session.snapshot();
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateSignTextObjectAction{
            snapshot.project_revision, source.source_identity, source.source_revision}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U &&
        snapshot.objects.front().object.mesh_asset.has_value());
    const std::uint64_t mesh_asset = *snapshot.objects.front().object.mesh_asset;

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            snapshot.project_revision, "Sign panel enamel", {}}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.materials.size() == 1U);
    const carto::render::MaterialId panel_material = snapshot.materials.front().id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            snapshot.project_revision, "Raised lettering", {}}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.materials.size() == 2U);
    const carto::render::MaterialId lettering_material = snapshot.materials.back().id;
    REQUIRE(panel_material != lettering_material);

    const carto::project::MaterialRegionKey panel_region{
        mesh_asset, carto::geometry::kSignTextPanelMaterialSlot};
    const carto::project::MaterialRegionKey lettering_region{
        mesh_asset, carto::geometry::kSignTextLetteringMaterialSlot};
    const auto before_assignment = session.snapshot();
    const auto missing_region = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_assignment.project_revision,
            {mesh_asset, 999U}, panel_material, std::nullopt});
    REQUIRE(!missing_region &&
        missing_region.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(session.snapshot().project_revision == before_assignment.project_revision &&
        session.snapshot().material_assignments.empty());

    const auto panel_assigned = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_assignment.project_revision, panel_region, panel_material, std::nullopt});
    REQUIRE(panel_assigned && panel_assigned.value().document_changed);
    const auto stale_lettering = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_assignment.project_revision, lettering_region,
            lettering_material, std::nullopt});
    REQUIRE(!stale_lettering &&
        stale_lettering.error().code == carto::core::ErrorCode::stale_data);

    const auto current = session.snapshot();
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            current.project_revision, lettering_region,
            lettering_material, std::nullopt}));
    const auto assigned = session.snapshot();
    REQUIRE(assigned.material_assignments.size() == 2U);
    REQUIRE(std::any_of(assigned.material_assignments.begin(),
        assigned.material_assignments.end(), [&](const auto& value) {
            return value.key == panel_region && value.material == panel_material;
        }));
    REQUIRE(std::any_of(assigned.material_assignments.begin(),
        assigned.material_assignments.end(), [&](const auto& value) {
            return value.key == lettering_region && value.material == lettering_material;
        }));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened && reopened.value().validate());
    const auto& assignments = reopened.value().material_catalog().assignments();
    REQUIRE(assignments.at(panel_region).material == panel_material);
    REQUIRE(assignments.at(lettering_region).material == lettering_material);
    const auto& mesh = reopened.value().meshes().at(mesh_asset);
    const auto* slots = mesh.attributes().find("cartographer.material_slot");
    REQUIRE(slots != nullptr &&
        slots->descriptor.domain == carto::attributes::AttributeDomain::face &&
        slots->descriptor.type == carto::attributes::AttributeType::uint32 &&
        slots->values.size() == mesh.face_count());
    std::size_t panel_faces = 0U;
    std::size_t lettering_faces = 0U;
    for (const auto& value : slots->values) {
        const std::uint32_t slot = std::get<std::uint32_t>(value);
        if (slot == carto::geometry::kSignTextPanelMaterialSlot) ++panel_faces;
        if (slot == carto::geometry::kSignTextLetteringMaterialSlot) ++lettering_faces;
    }
    REQUIRE(panel_faces > 0U && lettering_faces > 0U &&
        panel_faces + lettering_faces == slots->values.size());
    REQUIRE(reopened.value().sign_text_instance_receipts().size() == 1U &&
        reopened.value().sign_text_instance_receipts().at(
            assigned.objects.front().object.id).source_snapshot == source.serialize());

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    auto after_undo = session.snapshot();
    REQUIRE(after_undo.material_assignments.size() == 1U &&
        after_undo.material_assignments.front().key == panel_region);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.snapshot().material_assignments.size() == 2U);

    carto::journal::Journal journal(
        std::filesystem::path(project_path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    const auto assignment_receipts = std::count_if(
        entries.value().begin(), entries.value().end(), [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return payload.find("Set Material Region Assignment") != std::string::npos;
        });
    REQUIRE(assignment_receipts == 2);
}

void application_routes_source_reimport_through_human_history() {
    carto::application::ApplicationSession session;
    TempDirectory directory;
    const auto project_path = directory.path() / "source-assets.carto";
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));
    const auto bytes_digest = [](std::string_view value) {
        return carto::assets::sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    };
    carto::assets::SourceImportRecord source;
    source.identity = "asset.oak";
    source.relative_source_path = "Assets/trees/oak.obj";
    source.source_generation = carto::core::Revision{1U};
    source.kind = carto::assets::SourceAssetKind::mesh;
    source.source_digest = bytes_digest("source-v1");
    source.importer_identity = "carto.import.obj";
    source.importer_version = 1U;
    source.artifact_identity = "mesh.oak";
    source.artifact_digest = bytes_digest("mesh-v1");
    source.domain_digests = {{
        carto::assets::SourceAssetDomain::geometry, bytes_digest("mesh-v1")}};

    const auto plan = session.preview_source_reimport({source});
    REQUIRE(static_cast<bool>(plan));
    const auto imported = carto::application::testing::dispatch(session,
        carto::application::ApplySourceReimportPlanAction{plan.value()});
    REQUIRE(imported && imported.value().document_changed);
    const auto after_import = session.snapshot();
    REQUIRE(after_import.source_assets.size() == 1U &&
        after_import.source_assets.front().identity == source.identity &&
        after_import.source_assets.front().relative_source_path == source.relative_source_path &&
        after_import.source_assets.front().source_generation == carto::core::Revision{1U});
    const auto no_change_plan = session.preview_source_reimport({source});
    REQUIRE(no_change_plan && no_change_plan.value().entries.front().change ==
        carto::assets::SourceReimportChange::unchanged);
    const auto recovery = carto::application::ApplicationSession::inspect_recovery(project_path);
    REQUIRE(recovery && recovery.value().state ==
        carto::application::RecoveryInspectionState::pending);
    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries && !journal_entries.value().empty());
    const std::string journal_payload(journal_entries.value().back().payload.begin(),
        journal_entries.value().back().payload.end());
    REQUIRE(journal_payload.find("Apply Source Reimport ") != std::string::npos &&
        journal_payload.find("SOURCE_ASSET_REGISTRY_HEX") != std::string::npos);

    const auto stale_plan = carto::application::testing::dispatch(session,
        carto::application::ApplySourceReimportPlanAction{plan.value()});
    REQUIRE(!stale_plan && stale_plan.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().source_assets.size() == 1U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    const auto after_undo = session.snapshot();
    REQUIRE(after_undo.source_assets.empty() && after_undo.source_asset_tombstone_count == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    const auto after_redo = session.snapshot();
    REQUIRE(after_redo.source_assets.size() == 1U &&
        after_redo.source_assets.front().source_generation == carto::core::Revision{2U});
    const auto stale_no_change = carto::application::testing::dispatch(session,
        carto::application::ApplySourceReimportPlanAction{no_change_plan.value()});
    REQUIRE(!stale_no_change &&
        stale_no_change.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened && reopened.value().source_asset_registry().records().at(source.identity)
        .source_generation == carto::core::Revision{2U});
}

void application_authors_pbr_materials_through_revisioned_journaled_history() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    const auto project_path = temp.path() / "materials.carto";
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));

    carto::render::StandardMaterial initial;
    initial.base_color_factor = {0.18, 0.22, 0.25, 1.0};
    initial.metallic = 0.62;
    initial.roughness = 0.71;
    const auto before_create = session.snapshot();
    const auto created = carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            before_create.project_revision, "Galvanized steel", initial});
    REQUIRE(created && created.value().document_changed &&
        created.value().action == "Create Material");
    auto after_create = session.snapshot();
    REQUIRE(after_create.materials.size() == 1U);
    REQUIRE(after_create.materials.front().id == 1U);
    REQUIRE(after_create.materials.front().revision == carto::core::Revision{1U});
    REQUIRE(after_create.materials.front().definition.roughness == 0.71);

    const auto stale_create = carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            before_create.project_revision, "Stale material", {}});
    REQUIRE(!stale_create && stale_create.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().materials.size() == 1U);

    auto edited = after_create.materials.front().definition;
    edited.base_color_factor = {0.42, 0.45, 0.48, 1.0};
    edited.metallic = 0.92;
    edited.roughness = 0.3;
    const auto updated = carto::application::testing::dispatch(session,
        carto::application::UpdateMaterialAction{
            after_create.project_revision, 1U, carto::core::Revision{1U}, edited});
    REQUIRE(updated && updated.value().document_changed &&
        updated.value().action == "Update Material");
    auto after_update = session.snapshot();
    REQUIRE(after_update.materials.front().revision == carto::core::Revision{2U});
    REQUIRE(after_update.materials.front().definition.metallic == 0.92);
    REQUIRE(after_update.materials.front().definition.roughness == 0.3);

    const auto stale_material = carto::application::testing::dispatch(session,
        carto::application::UpdateMaterialAction{
            after_update.project_revision, 1U, carto::core::Revision{1U}, initial});
    REQUIRE(!stale_material &&
        stale_material.error().code == carto::core::ErrorCode::stale_data);
    auto invalid = edited;
    invalid.roughness = 1.5;
    const auto invalid_edit = carto::application::testing::dispatch(session,
        carto::application::UpdateMaterialAction{
            after_update.project_revision, 1U, carto::core::Revision{2U}, invalid});
    REQUIRE(!invalid_edit);
    REQUIRE(session.snapshot().materials.front().revision == carto::core::Revision{2U});

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    auto after_undo = session.snapshot();
    REQUIRE(after_undo.materials.front().revision == carto::core::Revision{1U});
    REQUIRE(after_undo.materials.front().definition.roughness == 0.71);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    auto after_redo = session.snapshot();
    REQUIRE(after_redo.materials.front().revision == carto::core::Revision{2U});
    REQUIRE(after_redo.materials.front().definition.roughness == 0.3);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened && reopened.value().material_catalog().materials().size() == 1U);
    REQUIRE(reopened.value().material_catalog().materials().at(1U).revision ==
        carto::core::Revision{2U});
    REQUIRE(reopened.value().material_catalog().materials().at(1U).definition.roughness == 0.3);

    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries && entries.value().size() >= 4U);
    const auto contains_payload = [&entries](std::string_view needle) {
        return std::any_of(entries.value().begin(), entries.value().end(),
            [needle](const auto& entry) {
                const std::string payload(entry.payload.begin(), entry.payload.end());
                return payload.find(needle) != std::string::npos;
            });
    };
    REQUIRE(contains_payload("Create Material"));
    REQUIRE(contains_payload("Update Material"));
    REQUIRE(contains_payload("Undo"));
    REQUIRE(contains_payload("Redo"));
}

void application_authors_material_graphs_through_digest_guarded_history() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    const auto project_path = temp.path() / "material-graphs.carto";
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));

    carto::project::MaterialGraph graph;
    graph.identity = "materials/pine-ridge/painted-metal";
    graph.revision = carto::core::Revision{1U};
    carto::project::MaterialGraphNode color;
    color.id = 1U;
    color.kind = carto::project::MaterialGraphNodeKind::constant_color;
    color.value_type = carto::project::MaterialGraphValueType::color;
    color.constants = {0.28, 0.32, 0.36, 1.0};
    carto::project::MaterialGraphNode alpha;
    alpha.id = 2U;
    alpha.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    alpha.value_type = carto::project::MaterialGraphValueType::scalar;
    alpha.constants[0] = 1.0;
    carto::project::MaterialGraphNode roughness;
    roughness.id = 3U;
    roughness.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    roughness.value_type = carto::project::MaterialGraphValueType::scalar;
    roughness.constants[0] = 0.63;
    carto::project::MaterialGraphNode metallic;
    metallic.id = 4U;
    metallic.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    metallic.value_type = carto::project::MaterialGraphValueType::scalar;
    metallic.constants[0] = 0.78;
    carto::project::MaterialGraphNode unsupported;
    unsupported.id = 5U;
    unsupported.kind = carto::project::MaterialGraphNodeKind::unsupported;
    unsupported.value_type = carto::project::MaterialGraphValueType::scalar;
    unsupported.unsupported_identity = "donor-noise-node";
    graph.nodes = {color, alpha, roughness, metallic, unsupported};
    graph.outputs.base_color = color.id;
    graph.outputs.alpha = alpha.id;
    graph.outputs.roughness = roughness.id;
    graph.outputs.metallic = metallic.id;

    const auto before_create = session.snapshot();
    const auto created = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            before_create.project_revision, std::nullopt, std::nullopt, graph});
    REQUIRE(created && created.value().document_changed &&
        created.value().action == "Set Material Graph");
    auto after_create = session.snapshot();
    REQUIRE(after_create.material_graphs.size() == 1U);
    REQUIRE(after_create.material_graphs.front().identity == graph.identity);
    REQUIRE(after_create.material_graphs.front().node_count == graph.nodes.size());
    auto source = session.material_graph_source(graph.identity);
    REQUIRE(source && source.value().revision == carto::core::Revision{1U});
    auto compile_report = carto::project::compile_material_graph(source.value());
    REQUIRE(compile_report && !compile_report.value().supported &&
        compile_report.value().diagnostics.size() == 1U &&
        compile_report.value().diagnostics.front().node_id == unsupported.id);

    carto::render::StandardMaterial material_definition;
    const auto material_created = carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            after_create.project_revision, "Paint source", material_definition});
    REQUIRE(material_created);
    after_create = session.snapshot();
    REQUIRE(after_create.materials.size() == 1U &&
        after_create.materials.front().revision == carto::core::Revision{1U});

    const auto original_digest = source.value().canonical_digest();
    REQUIRE(original_digest);
    auto stale_binding_digest = original_digest.value();
    stale_binding_digest.bytes[0] ^= 0xffU;
    const auto stale_binding = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphBindingAction{
            after_create.project_revision, after_create.materials.front().id,
            after_create.materials.front().revision, graph.identity, source.value().revision,
            stale_binding_digest});
    REQUIRE(!stale_binding &&
        stale_binding.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().material_graph_bindings.empty());
    const auto bound = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphBindingAction{
            after_create.project_revision, after_create.materials.front().id,
            after_create.materials.front().revision, graph.identity, source.value().revision,
            original_digest.value()});
    REQUIRE(bound && bound.value().action == "Bind Material Graph");
    after_create = session.snapshot();
    REQUIRE(after_create.material_graph_bindings.size() == 1U &&
        after_create.material_graph_bindings.front().source_current &&
        after_create.material_graph_bindings.front().source_revision ==
            source.value().revision &&
        after_create.materials.front().revision == carto::core::Revision{2U});

    auto updated = source.value();
    const auto color_node = std::find_if(updated.nodes.begin(), updated.nodes.end(),
        [](const auto& node) { return node.id == 1U; });
    REQUIRE(color_node != updated.nodes.end());
    color_node->constants[0] = 0.44;
    updated.revision = updated.revision.next();

    const auto stale_project = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            before_create.project_revision, source.value().revision,
            original_digest.value(), updated});
    REQUIRE(!stale_project &&
        stale_project.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().material_graphs.front().source_revision ==
        carto::core::Revision{1U});

    auto stale_source_digest = original_digest.value();
    stale_source_digest.bytes[0] ^= 0xffU;
    const auto stale_source = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            after_create.project_revision, source.value().revision,
            stale_source_digest, updated});
    REQUIRE(!stale_source &&
        stale_source.error().code == carto::core::ErrorCode::stale_data);

    auto wrong_revision = updated;
    wrong_revision.revision = carto::core::Revision{9U};
    const auto invalid_revision = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            after_create.project_revision, source.value().revision,
            original_digest.value(), wrong_revision});
    REQUIRE(!invalid_revision &&
        invalid_revision.error().code == carto::core::ErrorCode::invalid_argument);

    const auto applied = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            after_create.project_revision, source.value().revision,
            original_digest.value(), updated});
    REQUIRE(applied && applied.value().document_changed);
    auto after_update = session.snapshot();
    REQUIRE(after_update.material_graphs.front().source_revision ==
        carto::core::Revision{2U});
    REQUIRE(after_update.material_graph_bindings.size() == 1U &&
        !after_update.material_graph_bindings.front().source_current &&
        after_update.material_graph_bindings.front().source_revision ==
            carto::core::Revision{1U});
    source = session.material_graph_source(graph.identity);
    REQUIRE(source && source.value().nodes.front().constants[0] == 0.44);
    compile_report = carto::project::compile_material_graph(source.value());
    REQUIRE(compile_report && !compile_report.value().supported &&
        compile_report.value().diagnostics.size() == 1U);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    const auto& reopened_graph = reopened.value().material_catalog().material_graphs().at(
        graph.identity);
    REQUIRE(reopened_graph.revision == carto::core::Revision{2U});
    REQUIRE(reopened_graph.nodes.front().constants[0] == 0.44);
    REQUIRE(reopened_graph.nodes.back().unsupported_identity == "donor-noise-node");

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    source = session.material_graph_source(graph.identity);
    REQUIRE(source && source.value().revision == carto::core::Revision{1U} &&
        source.value().nodes.front().constants[0] == 0.28);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    source = session.material_graph_source(graph.identity);
    REQUIRE(source && source.value().revision == carto::core::Revision{2U} &&
        source.value().nodes.front().constants[0] == 0.44);

    const auto topology_base_digest = source.value().canonical_digest();
    REQUIRE(topology_base_digest);
    auto topology_graph = source.value();
    carto::project::MaterialGraphNode color_add;
    color_add.id = 6U;
    color_add.kind = carto::project::MaterialGraphNodeKind::add;
    color_add.value_type = carto::project::MaterialGraphValueType::color;
    color_add.inputs = {1U, 1U};
    topology_graph.nodes.push_back(color_add);
    topology_graph.outputs.base_color = color_add.id;
    topology_graph.revision = topology_graph.revision.next();
    const auto before_topology_commit = session.snapshot();
    const auto topology_applied = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            before_topology_commit.project_revision, source.value().revision,
            topology_base_digest.value(), topology_graph});
    REQUIRE(topology_applied);
    source = session.material_graph_source(graph.identity);
    REQUIRE((source && source.value().revision == carto::core::Revision{3U} &&
        source.value().outputs.base_color == color_add.id &&
        source.value().nodes.back().inputs == std::vector<std::uint64_t>{1U, 1U}));

    const auto latest_graph_digest = source.value().canonical_digest();
    REQUIRE(latest_graph_digest);
    const auto before_rebind = session.snapshot();
    const auto rebound = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphBindingAction{
            before_rebind.project_revision, before_rebind.materials.front().id,
            before_rebind.materials.front().revision, graph.identity, source.value().revision,
            latest_graph_digest.value()});
    REQUIRE(rebound && rebound.value().action == "Bind Material Graph");
    auto after_rebind = session.snapshot();
    REQUIRE(after_rebind.material_graph_bindings.front().source_current);
    const auto unbound = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphBindingAction{
            after_rebind.project_revision, after_rebind.materials.front().id,
            after_rebind.materials.front().revision, std::nullopt, std::nullopt,
            std::nullopt});
    REQUIRE(unbound && unbound.value().action == "Unbind Material Graph");
    const auto after_unbind = session.snapshot();
    REQUIRE(after_unbind.material_graph_bindings.empty());
    const auto rebound_again = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphBindingAction{
            after_unbind.project_revision, after_unbind.materials.front().id,
            after_unbind.materials.front().revision, graph.identity, source.value().revision,
            latest_graph_digest.value()});
    REQUIRE(rebound_again && session.snapshot().material_graph_bindings.front().source_current);

    const auto topology_digest = source.value().canonical_digest();
    REQUIRE(topology_digest);
    auto cyclic_graph = source.value();
    const auto cycle_node = std::find_if(cyclic_graph.nodes.begin(), cyclic_graph.nodes.end(),
        [color_add](const auto& node) { return node.id == color_add.id; });
    REQUIRE(cycle_node != cyclic_graph.nodes.end());
    cycle_node->inputs = {color_add.id, 1U};
    cyclic_graph.revision = cyclic_graph.revision.next();
    const auto before_cycle_rejection = session.snapshot();
    const auto rejected_cycle = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            before_cycle_rejection.project_revision, source.value().revision,
            topology_digest.value(), cyclic_graph});
    REQUIRE(!rejected_cycle &&
        rejected_cycle.error().code == carto::core::ErrorCode::cycle_detected);
    REQUIRE(session.snapshot().project_revision == before_cycle_rejection.project_revision);
    REQUIRE(session.material_graph_source(graph.identity).value().revision ==
        carto::core::Revision{3U});

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened_topology = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened_topology);
    const auto& topology_reopened_graph =
        reopened_topology.value().material_catalog().material_graphs().at(graph.identity);
    REQUIRE(topology_reopened_graph.revision == carto::core::Revision{3U});
    REQUIRE(topology_reopened_graph.outputs.base_color == color_add.id);
    REQUIRE((topology_reopened_graph.nodes.back().inputs ==
        std::vector<std::uint64_t>{1U, 1U}));
    REQUIRE(reopened_topology.value().material_catalog().material_graph_bindings().at(1U)
        .source_revision == carto::core::Revision{3U});

    auto untracked_deletion = source.value();
    untracked_deletion.outputs.base_color = 1U;
    untracked_deletion.nodes.erase(std::remove_if(
        untracked_deletion.nodes.begin(), untracked_deletion.nodes.end(),
        [color_add](const auto& node) { return node.id == color_add.id; }),
        untracked_deletion.nodes.end());
    untracked_deletion.revision = untracked_deletion.revision.next();
    const auto before_untracked_delete = session.snapshot();
    const auto rejected_untracked_delete = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            before_untracked_delete.project_revision, source.value().revision,
            topology_digest.value(), untracked_deletion});
    REQUIRE(!rejected_untracked_delete &&
        rejected_untracked_delete.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before_untracked_delete.project_revision);

    auto deleted_graph = untracked_deletion;
    deleted_graph.retired_node_ids.push_back(color_add.id);
    const auto tracked_delete = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            before_untracked_delete.project_revision, source.value().revision,
            topology_digest.value(), deleted_graph});
    REQUIRE(tracked_delete);
    source = session.material_graph_source(graph.identity);
    REQUIRE(source && source.value().retired_node_ids ==
        std::vector<std::uint64_t>{color_add.id});

    auto reused_identity = source.value();
    carto::project::MaterialGraphNode reused_node;
    reused_node.id = color_add.id;
    reused_node.kind = carto::project::MaterialGraphNodeKind::add;
    reused_node.value_type = carto::project::MaterialGraphValueType::color;
    reused_node.inputs = {1U, 1U};
    reused_identity.nodes.push_back(reused_node);
    reused_identity.outputs.base_color = reused_node.id;
    reused_identity.revision = reused_identity.revision.next();
    REQUIRE(!reused_identity.validate());

    auto fresh_identity = source.value();
    auto fresh_node = reused_node;
    fresh_node.id = color_add.id + 1U;
    fresh_identity.nodes.push_back(fresh_node);
    fresh_identity.outputs.base_color = fresh_node.id;
    fresh_identity.revision = fresh_identity.revision.next();
    const auto current_graph_digest = source.value().canonical_digest();
    REQUIRE(current_graph_digest);
    const auto before_fresh_add = session.snapshot();
    const auto fresh_added = carto::application::testing::dispatch(session,
        carto::application::SetMaterialGraphAction{
            before_fresh_add.project_revision, source.value().revision,
            current_graph_digest.value(), fresh_identity});
    REQUIRE(fresh_added);
    REQUIRE(session.material_graph_source(graph.identity).value().retired_node_ids ==
        std::vector<std::uint64_t>{color_add.id});
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened_lineage = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened_lineage);
    const auto& lineage_graph = reopened_lineage.value().material_catalog()
        .material_graphs().at(graph.identity);
    REQUIRE(lineage_graph.revision == carto::core::Revision{5U});
    REQUIRE(lineage_graph.retired_node_ids == std::vector<std::uint64_t>{color_add.id});
    REQUIRE(std::any_of(lineage_graph.nodes.begin(), lineage_graph.nodes.end(),
        [fresh_node](const auto& node) { return node.id == fresh_node.id; }));

    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    const bool has_graph_receipt = std::any_of(
        entries.value().begin(), entries.value().end(), [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return payload.find("Set Material Graph") != std::string::npos;
        });
    REQUIRE(has_graph_receipt);
}

void application_assigns_material_regions_through_revisioned_history() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    const auto project_path = temp.path() / "material-regions.carto";
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));

    auto mesh = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(mesh);
    const auto faces = mesh.value().faces_sorted();
    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::face, faces.size()}});
    std::vector<carto::attributes::AttributeValue> regions(
        faces.size(), std::uint32_t{7U});
    REQUIRE(attributes.add_layer(carto::attributes::AttributeLayer{
        {"cartographer.material_slot", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::uint32}, std::move(regions)}));
    REQUIRE(mesh.value().set_attribute_payload(std::move(attributes), {}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateMeshObjectAction{
            "Authored regions", std::move(mesh.value())}));
    const auto mesh_snapshot = session.snapshot();
    REQUIRE(mesh_snapshot.objects.size() == 1U);
    REQUIRE(mesh_snapshot.objects.front().object.mesh_asset.has_value());
    const auto mesh_asset = *mesh_snapshot.objects.front().object.mesh_asset;

    auto first_source = carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            mesh_snapshot.project_revision, "Stone", {}});
    REQUIRE(first_source);
    const auto after_first_source = session.snapshot();
    auto second_source = carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            after_first_source.project_revision, "Metal", {}});
    REQUIRE(second_source);
    const auto before_assignment = session.snapshot();
    const carto::project::MaterialRegionKey region{mesh_asset, 7U};

    const auto missing_material = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_assignment.project_revision, region, 999U, std::nullopt});
    REQUIRE(!missing_material &&
        missing_material.error().code == carto::core::ErrorCode::not_found);
    const auto missing_region = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_assignment.project_revision,
            {mesh_asset, 999U}, 1U, std::nullopt});
    REQUIRE(!missing_region &&
        missing_region.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(session.snapshot().material_assignments.empty());
    REQUIRE(session.snapshot().project_revision == before_assignment.project_revision);

    const auto assigned = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_assignment.project_revision, region, 1U, std::nullopt});
    REQUIRE(assigned && assigned.value().document_changed &&
        assigned.value().action == "Set Material Region Assignment");
    const auto after_assignment = session.snapshot();
    REQUIRE(after_assignment.material_assignments.size() == 1U);
    REQUIRE(after_assignment.material_assignments.front().key == region);
    REQUIRE(after_assignment.material_assignments.front().material == 1U);

    const auto stale_assignment = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_assignment.project_revision, region, 2U, std::nullopt});
    REQUIRE(!stale_assignment &&
        stale_assignment.error().code == carto::core::ErrorCode::stale_data);

    const auto reassigned = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            after_assignment.project_revision, region, 2U, std::nullopt});
    REQUIRE(reassigned);
    const auto after_reassignment = session.snapshot();
    REQUIRE(after_reassignment.material_assignments.front().material == 2U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().material_catalog().assignments().at(region).material == 2U);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().material_assignments.front().material == 1U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.snapshot().material_assignments.front().material == 2U);

    const auto before_clear = session.snapshot();
    const auto cleared = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            before_clear.project_revision, region, std::nullopt, std::nullopt});
    REQUIRE(cleared && session.snapshot().material_assignments.empty());
    const auto duplicate_clear = carto::application::testing::dispatch(session,
        carto::application::SetMaterialRegionAssignmentAction{
            session.snapshot().project_revision, region, std::nullopt, std::nullopt});
    REQUIRE(!duplicate_clear &&
        duplicate_clear.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(session.snapshot().material_assignments.empty());

    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    const bool has_assignment_receipt = std::any_of(
        entries.value().begin(), entries.value().end(), [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return payload.find("Set Material Region Assignment") != std::string::npos;
        });
    REQUIRE(has_assignment_receipt);
}

void application_authors_presentation_catalog_through_revisioned_history() {
    TempDirectory temp;
    const auto project_path = temp.path() / "presentation-authoring.carto";
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::NewProjectAction{"Presentation authoring"}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Review camera", {1.0, 1.0, 1.0}}));
    const auto camera_object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Exterior sconce", {1.0, 1.0, 1.0}}));
    const auto light_object = session.snapshot().objects.back().object.id;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));

    carto::project::AuthoredCamera camera;
    camera.object = camera_object;
    camera.identity = "review.camera.motel-sign";
    camera.bookmark_identity = "motel-sign.approach";
    camera.reference_image_identity = "reference.sign-front";
    camera.projection = carto::project::CameraProjection::perspective;
    camera.review_purpose = carto::project::CameraReviewPurpose::silhouette_review;
    camera.vertical_fov_radians = 0.82;
    camera.near_plane = 0.05;
    camera.far_plane = 800.0;
    camera.active_for_review = true;
    camera.revision = carto::core::Revision{1U};

    carto::project::AuthoredLight light;
    light.object = light_object;
    light.identity = "fixture.exterior-sconce";
    light.semantic_class = "practical";
    light.fixture_owner = "motel-building";
    light.power_owner = "service-panel";
    light.switch_owner = "front-desk-switch";
    light.kind = carto::project::AuthoredLightKind::spot;
    light.linear_color = {1.0, 0.71, 0.42};
    light.intensity = 900.0;
    light.range = 18.0;
    light.inner_cone_radians = 0.24;
    light.outer_cone_radians = 0.52;
    light.color_temperature_kelvin = 2'700.0;
    light.normal_state = carto::project::LightNormalState::switchable;
    light.failure_state = carto::project::LightFailureState::intermittent;
    light.flicker_eligible = true;
    light.revision = carto::core::Revision{1U};

    const std::string reference_bytes = "application-reference-image";
    const auto reference_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(reference_bytes.data()),
        reference_bytes.size()));
    const auto make_reference_plane = [&](std::string identity,
            carto::project::ReferencePlaneView view,
            std::string view_set,
            std::optional<carto::scene::ObjectId> pinned_camera) {
        carto::project::ReferenceImagePlane plane;
        plane.identity = std::move(identity);
        plane.source_asset_identity = "source." + plane.identity;
        plane.source_locator = "References/" + plane.identity + ".png";
        plane.source_digest = reference_digest;
        plane.source_generation = carto::core::Revision{1U};
        plane.image_width_pixels = 64U;
        plane.image_height_pixels = 32U;
        plane.view = view;
        plane.view_set_identity = std::move(view_set);
        plane.pinned_camera = pinned_camera;
        plane.width_meters = 8.0;
        plane.opacity = 0.7;
        plane.visible = true;
        plane.locked = view == carto::project::ReferencePlaneView::front;
        plane.revision = carto::core::Revision{1U};
        return plane;
    };

    carto::project::PresentationCatalog initial_catalog;
    REQUIRE(initial_catalog.insert_camera(camera));
    REQUIRE(initial_catalog.insert_light(light));
    REQUIRE(initial_catalog.insert_reference_plane(make_reference_plane(
        "motel.front", carto::project::ReferencePlaneView::front, "motel.orthos", std::nullopt)));
    REQUIRE(initial_catalog.insert_reference_plane(make_reference_plane(
        "motel.side", carto::project::ReferencePlaneView::side, "motel.orthos", std::nullopt)));
    REQUIRE(initial_catalog.insert_reference_plane(make_reference_plane(
        "motel.top", carto::project::ReferencePlaneView::top, "motel.orthos", std::nullopt)));
    REQUIRE(initial_catalog.insert_reference_plane(make_reference_plane(
        "motel.camera", carto::project::ReferencePlaneView::camera_pinned, {}, camera_object)));
    const auto before = session.snapshot();
    const auto created = carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            before.project_generation, before.project_revision, initial_catalog});
    REQUIRE(created);
    const auto after_create = session.snapshot();
    REQUIRE(after_create.project_revision == before.project_revision.next());
    REQUIRE(after_create.presentation_catalog.cameras().size() == 1U);
    REQUIRE(after_create.presentation_catalog.lights().size() == 1U);
    REQUIRE(after_create.presentation_catalog.reference_planes().size() == 4U);
    REQUIRE(created.value().action == "Set Scene Presentation");

    auto moved_locked_plane = after_create.presentation_catalog.reference_planes().at("motel.front");
    moved_locked_plane.origin.x += 1.0;
    moved_locked_plane.revision = moved_locked_plane.revision.next();
    carto::project::PresentationCatalog locked_move_replacement;
    for (const auto& [object, value] : after_create.presentation_catalog.cameras()) {
        static_cast<void>(object);
        REQUIRE(locked_move_replacement.insert_camera(value));
    }
    for (const auto& [object, value] : after_create.presentation_catalog.lights()) {
        static_cast<void>(object);
        REQUIRE(locked_move_replacement.insert_light(value));
    }
    for (const auto& [identity, value] : after_create.presentation_catalog.reference_planes()) {
        REQUIRE(locked_move_replacement.insert_reference_plane(
            identity == "motel.front" ? moved_locked_plane : value));
    }
    const auto before_locked_move = session.snapshot();
    const auto rejected_locked_move = carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            before_locked_move.project_generation, before_locked_move.project_revision,
            std::move(locked_move_replacement)});
    REQUIRE(!rejected_locked_move && rejected_locked_move.error().code ==
        carto::core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before_locked_move.project_revision);
    REQUIRE(session.snapshot().undo_count == before_locked_move.undo_count);

    auto visible_locked_plane = after_create.presentation_catalog.reference_planes().at(
        "motel.front");
    visible_locked_plane.visible = false;
    visible_locked_plane.opacity = 0.45;
    visible_locked_plane.revision = visible_locked_plane.revision.next();
    carto::project::PresentationCatalog display_update;
    for (const auto& [object, value] : after_create.presentation_catalog.cameras()) {
        static_cast<void>(object);
        REQUIRE(display_update.insert_camera(value));
    }
    for (const auto& [object, value] : after_create.presentation_catalog.lights()) {
        static_cast<void>(object);
        REQUIRE(display_update.insert_light(value));
    }
    for (const auto& [identity, value] : after_create.presentation_catalog.reference_planes()) {
        REQUIRE(display_update.insert_reference_plane(
            identity == "motel.front" ? visible_locked_plane : value));
    }
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            before_locked_move.project_generation, before_locked_move.project_revision,
            std::move(display_update)}));
    REQUIRE(session.snapshot().presentation_catalog.reference_planes().at("motel.front")
        .opacity == 0.45);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().presentation_catalog.reference_planes().at("motel.front")
        .opacity == 0.7);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.snapshot().presentation_catalog.reference_planes().at("motel.front")
        .opacity == 0.45);

    const auto replace_camera = [&session](
        carto::project::AuthoredCamera replacement) {
        const auto base = session.snapshot();
        carto::project::PresentationCatalog candidate;
        bool replaced = false;
        for (const auto& [object, current] :
             base.presentation_catalog.cameras()) {
            if (object == replacement.object) {
                REQUIRE(candidate.insert_camera(replacement));
                replaced = true;
            } else {
                REQUIRE(candidate.insert_camera(current));
            }
        }
        if (!replaced) REQUIRE(candidate.insert_camera(replacement));
        for (const auto& [object, current] :
             base.presentation_catalog.lights()) {
            static_cast<void>(object);
            REQUIRE(candidate.insert_light(current));
        }
        for (const auto& [identity, current] :
             base.presentation_catalog.reference_planes()) {
            static_cast<void>(identity);
            REQUIRE(candidate.insert_reference_plane(current));
        }
        return candidate;
    };

    auto stale_record = camera;
    stale_record.vertical_fov_radians = 0.7;
    const auto snapshot_before_rejection = session.snapshot();
    const auto revision_before_rejection = snapshot_before_rejection.project_revision;
    const auto undo_before_rejection = snapshot_before_rejection.undo_count;
    const auto rejected_record_revision = carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            snapshot_before_rejection.project_generation, revision_before_rejection,
            replace_camera(stale_record)});
    REQUIRE(!rejected_record_revision &&
        rejected_record_revision.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == revision_before_rejection);
    REQUIRE(session.snapshot().undo_count == undo_before_rejection);

    auto replaced_identity = camera;
    replaced_identity.identity = "review.camera.replaced";
    replaced_identity.revision = replaced_identity.revision.next();
    const auto rejected_identity = carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            snapshot_before_rejection.project_generation, revision_before_rejection,
            replace_camera(replaced_identity)});
    REQUIRE(!rejected_identity && rejected_identity.error().code ==
        carto::core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == revision_before_rejection);

    auto revised_camera = camera;
    revised_camera.vertical_fov_radians = 0.7;
    revised_camera.revision = revised_camera.revision.next();
    const auto stale_project_revision = carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            before.project_generation, before.project_revision,
            replace_camera(revised_camera)});
    REQUIRE(!stale_project_revision &&
        stale_project_revision.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == revision_before_rejection);

    const auto updated = carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            snapshot_before_rejection.project_generation, revision_before_rejection,
            replace_camera(revised_camera)});
    REQUIRE(updated);
    REQUIRE(session.snapshot().presentation_catalog.cameras().at(camera_object)
        .revision == carto::core::Revision{2U});
    REQUIRE(session.snapshot().presentation_catalog.cameras().at(camera_object)
        .vertical_fov_radians == 0.7);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().presentation_catalog.cameras().at(camera_object)
        .revision == carto::core::Revision{1U});
    REQUIRE(session.snapshot().presentation_catalog.cameras().at(camera_object)
        .vertical_fov_radians == 0.82);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.snapshot().presentation_catalog.cameras().at(camera_object)
        .revision == carto::core::Revision{2U});

    auto second_camera = camera;
    second_camera.object = light_object;
    second_camera.identity = "review.camera.motel-exterior";
    second_camera.bookmark_identity = "motel.exterior.approach";
    second_camera.active_for_review = true;
    second_camera.revision = carto::core::Revision{1U};
    const auto after_fov_redo = session.snapshot();
    carto::project::PresentationCatalog camera_switch_catalog;
    for (const auto& [object, current] : after_fov_redo.presentation_catalog.cameras()) {
        auto value = current;
        if (value.active_for_review) {
            value.active_for_review = false;
            value.revision = value.revision.next();
        }
        REQUIRE(camera_switch_catalog.insert_camera(std::move(value)));
        static_cast<void>(object);
    }
    REQUIRE(camera_switch_catalog.insert_camera(second_camera));
    for (const auto& [object, current] : after_fov_redo.presentation_catalog.lights()) {
        REQUIRE(camera_switch_catalog.insert_light(current));
        static_cast<void>(object);
    }
    for (const auto& [identity, current] :
         after_fov_redo.presentation_catalog.reference_planes()) {
        REQUIRE(camera_switch_catalog.insert_reference_plane(current));
        static_cast<void>(identity);
    }
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            after_fov_redo.project_generation, after_fov_redo.project_revision,
            std::move(camera_switch_catalog)}));
    auto after_camera_switch = session.snapshot();
    REQUIRE(!after_camera_switch.presentation_catalog.cameras().at(camera_object)
        .active_for_review);
    REQUIRE(after_camera_switch.presentation_catalog.cameras().at(camera_object)
        .revision == carto::core::Revision{3U});
    REQUIRE(after_camera_switch.presentation_catalog.cameras().at(light_object)
        .active_for_review);
    REQUIRE(after_camera_switch.presentation_catalog.cameras().at(light_object)
        .revision == carto::core::Revision{1U});
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().presentation_catalog.cameras().at(camera_object)
        .active_for_review);
    REQUIRE(!session.snapshot().presentation_catalog.cameras().contains(light_object));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{std::nullopt}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().presentation_catalog().cameras().size() == 2U);
    REQUIRE(reopened.value().presentation_catalog().cameras().at(camera_object)
        .revision == carto::core::Revision{3U});
    REQUIRE(reopened.value().presentation_catalog().cameras().at(light_object)
        .revision == carto::core::Revision{1U});
    REQUIRE(reopened.value().presentation_catalog().lights().at(light_object)
        .fixture_owner == "motel-building");
    REQUIRE(reopened.value().presentation_catalog().reference_planes().size() == 4U);
    REQUIRE(reopened.value().presentation_catalog().reference_planes().at("motel.front")
        .opacity == 0.45);
    REQUIRE(!reopened.value().presentation_catalog().reference_planes().at("motel.front")
        .visible);

    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto entries = journal.read_all();
    REQUIRE(entries);
    const auto presentation_receipts = std::count_if(
        entries.value().begin(), entries.value().end(), [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return payload.find("Set Scene Presentation") != std::string::npos;
        });
    REQUIRE(presentation_receipts >= 2U);

    const auto old_generation = session.snapshot().project_generation;
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::NewProjectAction{"Replacement presentation project"}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Replacement target", {1.0, 1.0, 1.0}}));
    const auto replacement = session.snapshot();
    REQUIRE(replacement.project_generation != old_generation);
    carto::project::AuthoredCamera cross_project_camera;
    cross_project_camera.object = replacement.objects.front().object.id;
    cross_project_camera.identity = "review.camera.replacement";
    cross_project_camera.bookmark_identity = "replacement.approach";
    cross_project_camera.revision = carto::core::Revision{1U};
    carto::project::PresentationCatalog cross_project_catalog;
    REQUIRE(cross_project_catalog.insert_camera(cross_project_camera));
    const auto stale_generation = carto::application::testing::dispatch(session,
        carto::application::SetPresentationCatalogAction{
            old_generation, replacement.project_revision, cross_project_catalog});
    REQUIRE(!stale_generation &&
        stale_generation.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == replacement.project_revision);
    REQUIRE(session.snapshot().presentation_catalog.empty());
}

} // namespace

void application_rejects_late_invalid_face_poke_without_partial_mutation() {
    using namespace carto;
    geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    const auto p0 = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto p1 = mesh.add_vertex({4.0, 0.0, 0.0});
    const auto p2 = mesh.add_vertex({4.0, 1.0, 0.0});
    const auto p3 = mesh.add_vertex({3.0, 1.0, 0.0});
    const auto p4 = mesh.add_vertex({3.0, 2.0, 0.0});
    const auto p5 = mesh.add_vertex({2.0, 2.0, 0.0});
    REQUIRE(p0 && p1 && p2 && p3 && p4 && p5);
    REQUIRE(mesh.add_face({p0.value(), p1.value(), p2.value(), p3.value(),
        p4.value(), p5.value()}));
    REQUIRE(mesh.validate());

    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Late invalid poke", mesh}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const geometry::EditableMesh before_mesh =
        session.document_.meshes().at(*object.mesh_asset);
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectFaceAction{object.id, geometry::FaceId{1U}}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectFaceAction{object.id, geometry::FaceId{2U},
            editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    const std::string serialized_before = session.document_.serialize();

    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.poke-face", {}});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).revision() ==
        before_mesh.revision());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).vertex_count() ==
        before_mesh.vertex_count());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).face_count() ==
        before_mesh.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
    REQUIRE(session.document_.serialize() == serialized_before);
}

void application_extrudes_multiple_faces_per_owner_with_replayable_history() {
    using namespace carto;
    TempDirectory temp;
    const auto path = temp.path() / "multi-face-extrusion.carto";
    const auto fixture = make_application_dissolve_fixture();
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"First multi-face extrusion", fixture.mesh}));
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Second multi-face extrusion", fixture.mesh}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::face}));
    for (const scene::ObjectId owner : {first.id, second.id}) {
        REQUIRE(application::testing::dispatch(session,
            application::SelectFaceAction{owner, geometry::FaceId{1U},
                owner == first.id ? editor::SelectionOperation::replace
                                  : editor::SelectionOperation::add}));
        REQUIRE(application::testing::dispatch(session,
            application::SelectFaceAction{owner, geometry::FaceId{2U},
                editor::SelectionOperation::add}));
    }
    editor::ToolArguments arguments;
    arguments.distance = 0.25;
    const auto extruded = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.extrude-face", arguments});
    REQUIRE(extruded);
    REQUIRE(extruded.value().affected.objects.size() == 2U);
    REQUIRE(extruded.value().affected.faces.size() == 4U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);

    const std::array<geometry::FaceId, 2U> selected_faces{
        geometry::FaceId{1U}, geometry::FaceId{2U}};
    const auto assert_patch_chain = [](
        const std::vector<project::MeshPatchRecord>& records,
        std::uint64_t mesh_asset,
        const geometry::EditableMesh& source,
        const geometry::EditableMesh& actual,
        const std::array<geometry::FaceId, 2U>& expected_faces) {
        geometry::EditableMesh replay = source;
        std::size_t replayed = 0U;
        for (const auto& record : records) {
            if (record.mesh_asset != mesh_asset) continue;
            const auto patch = geometry::FaceExtrusionPatch::deserialize(record.patch_bytes);
            REQUIRE(patch);
            if (patch.value().expected_revision != replay.revision()) continue;
            REQUIRE(replayed < expected_faces.size());
            REQUIRE(patch.value().face == expected_faces[replayed]);
            REQUIRE(patch.value().distance == 0.25);
            const auto next = record.replay(replay);
            REQUIRE(next);
            replay = next.value();
            ++replayed;
        }
        REQUIRE(replayed == expected_faces.size());
        REQUIRE(replay.revision() == actual.revision());
        REQUIRE(replay.vertex_count() == actual.vertex_count());
        REQUIRE(replay.face_count() == actual.face_count());
        REQUIRE(replay.validate());
    };
    const auto first_edited = session.document_.meshes().at(*first.mesh_asset);
    const auto second_edited = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(first_edited.vertex_count() == first_source.vertex_count() + 6U);
    REQUIRE(second_edited.vertex_count() == second_source.vertex_count() + 6U);
    REQUIRE(first_edited.face_count() == first_source.face_count() + 6U);
    REQUIRE(second_edited.face_count() == second_source.face_count() + 6U);
    assert_patch_chain(session.document_.mesh_patches(), *first.mesh_asset,
        first_source, first_edited, selected_faces);
    assert_patch_chain(session.document_.mesh_patches(), *second.mesh_asset,
        second_source, second_edited, selected_faces);

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    const auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 4U);
    assert_patch_chain(reopened.value().mesh_patches(), *first.mesh_asset,
        first_source, reopened.value().meshes().at(*first.mesh_asset), selected_faces);
    assert_patch_chain(reopened.value().mesh_patches(), *second.mesh_asset,
        second_source, reopened.value().meshes().at(*second.mesh_asset), selected_faces);

    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(first_redo_source.face_count() == first_source.face_count());
    REQUIRE(second_redo_source.face_count() == second_source.face_count());
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    REQUIRE(session.document_.mesh_patches().size() == 8U);
    REQUIRE(session.document_.topology_receipts().size() == 8U);
    const auto first_redone = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redone = session.document_.meshes().at(*second.mesh_asset);
    assert_patch_chain(session.document_.mesh_patches(), *first.mesh_asset,
        first_redo_source, first_redone, selected_faces);
    assert_patch_chain(session.document_.mesh_patches(), *second.mesh_asset,
        second_redo_source, second_redone, selected_faces);
    REQUIRE(session.document_.validate());

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    const auto final_reopen = project::ProjectDocument::load(path);
    REQUIRE(final_reopen);
    assert_patch_chain(final_reopen.value().mesh_patches(), *first.mesh_asset,
        first_redo_source, final_reopen.value().meshes().at(*first.mesh_asset), selected_faces);
    assert_patch_chain(final_reopen.value().mesh_patches(), *second.mesh_asset,
        second_redo_source, final_reopen.value().meshes().at(*second.mesh_asset), selected_faces);
}

void application_rejects_multi_face_extrusion_revision_exhaustion_atomically() {
    using namespace carto;
    auto fixture = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(fixture);
    REQUIRE(fixture.value().restore_revision(core::Revision{
        core::Revision::max_value() - 2U}));
    const auto faces = fixture.value().faces_sorted();
    REQUIRE(faces.size() == 6U);
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Exhausted multi-face extrusion", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectFaceAction{object.id, faces[0].id}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectFaceAction{object.id, faces[1].id,
            editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    const auto before_mesh = session.document_.meshes().at(*object.mesh_asset);
    const std::string before_document = session.document_.serialize();
    editor::ToolArguments arguments;
    arguments.distance = 0.25;

    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.extrude-face", arguments});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == before_document);
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).revision() == before_mesh.revision());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).vertex_count() ==
        before_mesh.vertex_count());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).face_count() ==
        before_mesh.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_insets_multiple_faces_per_owner_with_replayable_history() {
    using namespace carto;
    TempDirectory temp;
    const auto path = temp.path() / "multi-face-inset.carto";
    const auto fixture = make_application_dissolve_fixture();
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"First multi-face inset", fixture.mesh}));
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Second multi-face inset", fixture.mesh}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::face}));
    for (const scene::ObjectId owner : {first.id, second.id}) {
        REQUIRE(application::testing::dispatch(session,
            application::SelectFaceAction{owner, geometry::FaceId{1U},
                owner == first.id ? editor::SelectionOperation::replace
                                  : editor::SelectionOperation::add}));
        REQUIRE(application::testing::dispatch(session,
            application::SelectFaceAction{owner, geometry::FaceId{2U},
                editor::SelectionOperation::add}));
    }
    editor::ToolArguments arguments;
    arguments.distance = 0.05;
    const auto inset = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.inset-face", arguments});
    REQUIRE(inset);
    REQUIRE(inset.value().affected.objects.size() == 2U);
    REQUIRE(inset.value().affected.faces.size() == 4U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);

    const std::array<geometry::FaceId, 2U> selected_faces{
        geometry::FaceId{1U}, geometry::FaceId{2U}};
    const auto assert_patch_chain = [](
        const std::vector<project::MeshPatchRecord>& records,
        std::uint64_t mesh_asset,
        const geometry::EditableMesh& source,
        const geometry::EditableMesh& actual,
        const std::array<geometry::FaceId, 2U>& expected_faces) {
        geometry::EditableMesh replay = source;
        std::size_t replayed = 0U;
        for (const auto& record : records) {
            if (record.mesh_asset != mesh_asset) continue;
            const auto patch = geometry::FaceInsetPatch::deserialize(record.patch_bytes);
            REQUIRE(patch);
            if (patch.value().expected_revision != replay.revision()) continue;
            REQUIRE(replayed < expected_faces.size());
            REQUIRE(patch.value().face == expected_faces[replayed]);
            REQUIRE(patch.value().distance == 0.05);
            const auto next = record.replay(replay);
            REQUIRE(next);
            replay = next.value();
            ++replayed;
        }
        REQUIRE(replayed == expected_faces.size());
        REQUIRE(replay.revision() == actual.revision());
        REQUIRE(replay.vertex_count() == actual.vertex_count());
        REQUIRE(replay.face_count() == actual.face_count());
        REQUIRE(replay.validate());
    };
    const auto first_edited = session.document_.meshes().at(*first.mesh_asset);
    const auto second_edited = session.document_.meshes().at(*second.mesh_asset);
    assert_patch_chain(session.document_.mesh_patches(), *first.mesh_asset,
        first_source, first_edited, selected_faces);
    assert_patch_chain(session.document_.mesh_patches(), *second.mesh_asset,
        second_source, second_edited, selected_faces);

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    const auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 4U);
    assert_patch_chain(reopened.value().mesh_patches(), *first.mesh_asset,
        first_source, reopened.value().meshes().at(*first.mesh_asset), selected_faces);
    assert_patch_chain(reopened.value().mesh_patches(), *second.mesh_asset,
        second_source, reopened.value().meshes().at(*second.mesh_asset), selected_faces);

    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(first_redo_source.face_count() == first_source.face_count());
    REQUIRE(second_redo_source.face_count() == second_source.face_count());
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    REQUIRE(session.document_.mesh_patches().size() == 8U);
    REQUIRE(session.document_.topology_receipts().size() == 8U);
    const auto first_redone = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redone = session.document_.meshes().at(*second.mesh_asset);
    assert_patch_chain(session.document_.mesh_patches(), *first.mesh_asset,
        first_redo_source, first_redone, selected_faces);
    assert_patch_chain(session.document_.mesh_patches(), *second.mesh_asset,
        second_redo_source, second_redone, selected_faces);
    REQUIRE(session.document_.validate());

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    const auto final_reopen = project::ProjectDocument::load(path);
    REQUIRE(final_reopen);
    assert_patch_chain(final_reopen.value().mesh_patches(), *first.mesh_asset,
        first_redo_source, final_reopen.value().meshes().at(*first.mesh_asset), selected_faces);
    assert_patch_chain(final_reopen.value().mesh_patches(), *second.mesh_asset,
        second_redo_source, final_reopen.value().meshes().at(*second.mesh_asset), selected_faces);
}

void application_rejects_multi_face_inset_revision_exhaustion_atomically() {
    using namespace carto;
    auto fixture = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(fixture);
    REQUIRE(fixture.value().restore_revision(core::Revision{
        core::Revision::max_value() - 2U}));
    const auto faces = fixture.value().faces_sorted();
    REQUIRE(faces.size() == 6U);
    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Exhausted multi-face inset", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectFaceAction{object.id, faces[0].id}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectFaceAction{object.id, faces[1].id,
            editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    const auto before_mesh = session.document_.meshes().at(*object.mesh_asset);
    const std::string before_document = session.document_.serialize();
    editor::ToolArguments arguments;
    arguments.distance = 0.1;

    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.inset-face", arguments});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == before_document);
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).revision() == before_mesh.revision());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).vertex_count() ==
        before_mesh.vertex_count());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).face_count() ==
        before_mesh.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_bevels_multiple_edges_per_owner_with_replayable_history() {
    using namespace carto;
    TempDirectory temp;
    const auto path = temp.path() / "multi-edge-bevel.carto";
    auto fixture = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(fixture);
    const auto topology = fixture.value().topology();
    REQUIRE(topology);
    std::optional<std::array<geometry::EdgeId, 2U>> disjoint_edges;
    for (std::size_t first = 0U; first < topology.value().edges.size() && !disjoint_edges;
         ++first) {
        const auto& a = topology.value().edges[first];
        if (!a.second_half_edge.has_value()) continue;
        for (std::size_t second = first + 1U; second < topology.value().edges.size(); ++second) {
            const auto& b = topology.value().edges[second];
            if (!b.second_half_edge.has_value() || a.first == b.first || a.first == b.second ||
                a.second == b.first || a.second == b.second) {
                continue;
            }
            disjoint_edges = std::array<geometry::EdgeId, 2U>{a.id, b.id};
            break;
        }
    }
    REQUIRE(disjoint_edges.has_value());
    std::sort(disjoint_edges->begin(), disjoint_edges->end());

    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"First multi-edge bevel", fixture.value()}));
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Second multi-edge bevel", fixture.value()}));
    const auto objects = session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset.has_value() && second.mesh_asset.has_value());
    REQUIRE(first.mesh_asset != second.mesh_asset);
    const auto first_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_source = session.document_.meshes().at(*second.mesh_asset);

    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::edge}));
    const std::array<scene::ObjectId, 2U> owners{first.id, second.id};
    for (std::size_t owner_index = 0U; owner_index < owners.size(); ++owner_index) {
        for (std::size_t edge_index = 0U; edge_index < disjoint_edges->size(); ++edge_index) {
            const auto operation = owner_index == 0U && edge_index == 0U
                ? editor::SelectionOperation::replace : editor::SelectionOperation::add;
            REQUIRE(application::testing::dispatch(session,
                application::SelectEdgeAction{
                    owners[owner_index], (*disjoint_edges)[edge_index], operation}));
        }
    }
    const auto before = session.snapshot();
    editor::ToolArguments arguments;
    arguments.distance = 0.05;
    arguments.segments = 2U;
    const auto beveled = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(beveled);
    REQUIRE(beveled.value().affected.objects.size() == 2U);
    REQUIRE(beveled.value().affected.edges.size() == 4U);
    REQUIRE(beveled.value().parameters.has_value());
    REQUIRE(beveled.value().parameters->distance == 0.05);
    REQUIRE(beveled.value().parameters->segments == 2U);
    REQUIRE(session.snapshot().undo_count == before.undo_count + 1U);
    REQUIRE(session.document_.topology_receipts().size() == 4U);
    REQUIRE(session.document_.mesh_patches().size() == 4U);

    const auto assert_patch_chain = [](
        const std::vector<project::MeshPatchRecord>& records,
        std::uint64_t mesh_asset,
        const geometry::EditableMesh& source,
        const geometry::EditableMesh& actual,
        const std::array<geometry::EdgeId, 2U>& expected_edges) {
        geometry::EditableMesh replay = source;
        std::size_t replayed = 0U;
        for (const auto& record : records) {
            if (record.mesh_asset != mesh_asset) continue;
            const auto patch = geometry::BevelEdgePatch::deserialize(record.patch_bytes);
            REQUIRE(patch);
            if (patch.value().expected_revision != replay.revision()) continue;
            REQUIRE(replayed < expected_edges.size());
            REQUIRE(patch.value().edge == expected_edges[replayed]);
            REQUIRE(patch.value().width == 0.05);
            REQUIRE(patch.value().segments == 2U);
            const auto next = record.replay(replay);
            REQUIRE(next);
            replay = next.value();
            ++replayed;
        }
        REQUIRE(replayed == expected_edges.size());
        REQUIRE(replay.revision() == actual.revision());
        REQUIRE(replay.vertex_count() == actual.vertex_count());
        REQUIRE(replay.face_count() == actual.face_count());
        REQUIRE(replay.validate());
    };
    const auto first_edited = session.document_.meshes().at(*first.mesh_asset);
    const auto second_edited = session.document_.meshes().at(*second.mesh_asset);
    assert_patch_chain(session.document_.mesh_patches(), *first.mesh_asset,
        first_source, first_edited, *disjoint_edges);
    assert_patch_chain(session.document_.mesh_patches(), *second.mesh_asset,
        second_source, second_edited, *disjoint_edges);

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().mesh_patches().size() == 4U);
    assert_patch_chain(reopened.value().mesh_patches(), *first.mesh_asset,
        first_source, reopened.value().meshes().at(*first.mesh_asset), *disjoint_edges);
    assert_patch_chain(reopened.value().mesh_patches(), *second.mesh_asset,
        second_source, reopened.value().meshes().at(*second.mesh_asset), *disjoint_edges);

    REQUIRE(application::testing::dispatch(session, application::UndoAction{}));
    const auto first_redo_source = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redo_source = session.document_.meshes().at(*second.mesh_asset);
    REQUIRE(first_redo_source.face_count() == first_source.face_count());
    REQUIRE(second_redo_source.face_count() == second_source.face_count());
    REQUIRE(application::testing::dispatch(session, application::RedoAction{}));
    REQUIRE(session.document_.topology_receipts().size() == 8U);
    REQUIRE(session.document_.mesh_patches().size() == 8U);
    const auto first_redone = session.document_.meshes().at(*first.mesh_asset);
    const auto second_redone = session.document_.meshes().at(*second.mesh_asset);
    assert_patch_chain(session.document_.mesh_patches(), *first.mesh_asset,
        first_redo_source, first_redone, *disjoint_edges);
    assert_patch_chain(session.document_.mesh_patches(), *second.mesh_asset,
        second_redo_source, second_redone, *disjoint_edges);
    REQUIRE(session.document_.validate());

    REQUIRE(application::testing::dispatch(session,
        application::SaveProjectAction{path}));
    reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    assert_patch_chain(reopened.value().mesh_patches(), *first.mesh_asset,
        first_redo_source, reopened.value().meshes().at(*first.mesh_asset), *disjoint_edges);
    assert_patch_chain(reopened.value().mesh_patches(), *second.mesh_asset,
        second_redo_source, reopened.value().meshes().at(*second.mesh_asset), *disjoint_edges);
}

void application_refuses_multi_edge_bevel_after_late_boundary_edge_atomically() {
    using namespace carto;
    auto fixture = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(fixture);
    const auto a = fixture.value().add_vertex({3.0, 0.0, 0.0});
    const auto b = fixture.value().add_vertex({4.0, 0.0, 0.0});
    const auto c = fixture.value().add_vertex({3.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(fixture.value().add_face({a.value(), b.value(), c.value()}));
    REQUIRE(fixture.value().validate());
    const auto topology = fixture.value().topology();
    REQUIRE(topology);
    const auto valid_edge = std::find_if(topology.value().edges.begin(),
        topology.value().edges.end(), [](const auto& edge) {
            return edge.second_half_edge.has_value();
        });
    const auto boundary_edge = std::find_if(topology.value().edges.begin(),
        topology.value().edges.end(), [](const auto& edge) {
            return !edge.second_half_edge.has_value();
        });
    REQUIRE(valid_edge != topology.value().edges.end());
    REQUIRE(boundary_edge != topology.value().edges.end());
    REQUIRE(valid_edge->id < boundary_edge->id);

    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Late invalid edge bevel", fixture.value()}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_before = session.document_.meshes().at(*object.mesh_asset);
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::edge}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectEdgeAction{object.id, valid_edge->id}));
    REQUIRE(application::testing::dispatch(session,
        application::SelectEdgeAction{object.id, boundary_edge->id,
            editor::SelectionOperation::add}));
    const auto before = session.snapshot();
    const std::string document_before = session.document_.serialize();
    editor::ToolArguments arguments;
    arguments.distance = 0.05;

    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::unsupported);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == document_before);
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).revision() == mesh_before.revision());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).vertex_count() ==
        mesh_before.vertex_count());
    REQUIRE(session.document_.meshes().at(*object.mesh_asset).face_count() ==
        mesh_before.face_count());
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_rejects_multi_edge_bevel_over_command_budget_without_mutation() {
    using namespace carto;
    geometry::EditableMesh mesh;
    constexpr std::size_t kTriangleCount = 22U;
    for (std::size_t index = 0U; index < kTriangleCount; ++index) {
        const double x = static_cast<double>(index) * 2.0;
        const auto a = mesh.add_vertex({x, 0.0, 0.0});
        const auto b = mesh.add_vertex({x + 1.0, 0.0, 0.0});
        const auto c = mesh.add_vertex({x, 1.0, 0.0});
        REQUIRE(a && b && c);
        REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    }
    REQUIRE(mesh.validate());
    const auto edges = mesh.edges_sorted(100U);
    REQUIRE(edges.size() == 66U);

    application::ApplicationSession session;
    REQUIRE(application::testing::dispatch(session,
        application::CreateMeshObjectAction{"Over-budget edge bevel", mesh}));
    const auto object = session.snapshot().objects.front().object;
    REQUIRE(object.mesh_asset.has_value());
    REQUIRE(application::testing::dispatch(session,
        application::SetSelectionModeAction{editor::SelectionMode::edge}));
    for (std::size_t index = 0U; index < edges.size(); ++index) {
        REQUIRE(application::testing::dispatch(session,
            application::SelectEdgeAction{object.id, edges[index].id,
                index == 0U ? editor::SelectionOperation::replace
                            : editor::SelectionOperation::add}));
    }
    const auto before = session.snapshot();
    const std::string document_before = session.document_.serialize();
    editor::ToolArguments arguments;
    arguments.distance = 0.05;

    const auto rejected = application::testing::dispatch(session,
        application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().undo_count == before.undo_count);
    REQUIRE(session.document_.serialize() == document_before);
    REQUIRE(session.document_.topology_receipts().empty());
    REQUIRE(session.document_.mesh_patches().empty());
}

void application_places_decals_from_a_current_surface_through_human_history() {
    carto::application::ApplicationSession session;
    TempDirectory directory;
    const auto project_path = directory.path() / "decal-placement.carto";
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::NewProjectAction{"Decal placement"}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::CreateBoxAction{"Target", {2.0, 2.0, 2.0}}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    const auto target = snapshot.objects.front().object.id;
    const auto material_created = carto::application::testing::dispatch(session,
        carto::application::CreateMaterialAction{
            snapshot.project_revision, "Decal material", carto::render::StandardMaterial{}});
    REQUIRE(material_created && material_created.value().document_changed);
    snapshot = session.snapshot();
    REQUIRE(snapshot.materials.size() == 1U);

    const auto target_mesh = snapshot.objects.front().object.mesh_asset;
    REQUIRE(target_mesh.has_value());
    const auto mesh = session.document_.meshes().find(*target_mesh);
    REQUIRE(mesh != session.document_.meshes().end());
    const auto before_draft = session.snapshot();
    const auto invalid_material = session.make_decal_placement(
        target, std::numeric_limits<std::uint64_t>::max(), {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5, 0.5, 0.1, 0.0,
        before_draft.project_revision);
    REQUIRE(!invalid_material && invalid_material.error().code ==
        carto::core::ErrorCode::not_found);
    const auto stale_draft = session.make_decal_placement(
        target, snapshot.materials.front().id, {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5, 0.5, 0.1, 0.0,
        before_draft.project_revision.next());
    REQUIRE(!stale_draft && stale_draft.error().code ==
        carto::core::ErrorCode::stale_data);
    const auto invalid_extent = session.make_decal_placement(
        target, snapshot.materials.front().id, {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.0, 0.5, 0.1, 0.0,
        before_draft.project_revision);
    REQUIRE(!invalid_extent);
    const auto draft = session.make_decal_placement(
        target, snapshot.materials.front().id, {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5, 0.5, 0.1, 0.0,
        before_draft.project_revision);
    REQUIRE(draft);
    REQUIRE(draft.value().target_mesh_asset == *target_mesh &&
        draft.value().target_mesh_revision == mesh->second.revision() &&
        draft.value().target_mesh_digest ==
            carto::application::canonical_mesh_geometry_digest(mesh->second));
    const auto exact_source_mesh = session.decal_placement_source_mesh(
        draft.value(), before_draft.project_revision);
    REQUIRE(exact_source_mesh && exact_source_mesh.value().valid() &&
        exact_source_mesh.value().source_revision == draft.value().target_mesh_revision);
    auto wrong_source_digest = draft.value();
    wrong_source_digest.target_mesh_digest.bytes[0U] ^= 1U;
    const auto stale_source_mesh = session.decal_placement_source_mesh(
        wrong_source_digest, before_draft.project_revision);
    REQUIRE(!stale_source_mesh && stale_source_mesh.error().code ==
        carto::core::ErrorCode::stale_data);
    const auto stale_project_mesh = session.decal_placement_source_mesh(
        draft.value(), before_draft.project_revision.next());
    REQUIRE(!stale_project_mesh && stale_project_mesh.error().code ==
        carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before_draft.project_revision &&
        session.snapshot().undo_count == before_draft.undo_count &&
        session.snapshot().decal_placements.empty());

    carto::application::SetDecalPlacementAction action;
    action.expected_project_revision = before_draft.project_revision;
    action.expected_placement_revision = carto::core::Revision{};
    action.placement = draft.value();
    const auto decal_journal_path = std::filesystem::path(project_path.string() + ".journal");
    carto::journal::Journal journal(decal_journal_path);
    const auto journal_bytes = [&]() {
        std::ifstream input(decal_journal_path, std::ios::binary);
        REQUIRE(input.good());
        return std::string(std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{});
    };
    const auto expect_rejected_without_mutation = [&](
        const carto::application::SetDecalPlacementAction& rejected_action,
        carto::core::ErrorCode expected_error,
        carto::application::OperationSource source =
            carto::application::OperationSource::human) {
        const auto before = session.snapshot();
        const auto document_before = session.document_.serialize();
        const auto entries_before = journal.read_all();
        REQUIRE(entries_before);
        const auto journal_before = journal_bytes();
        const auto rejected = [&]() {
            if (source == carto::application::OperationSource::human) {
                return carto::application::testing::dispatch(session, rejected_action);
            }
            const auto previous_source = session.active_source_;
            session.active_source_ = source;
            auto result = session.set_decal_placement(rejected_action);
            session.active_source_ = previous_source;
            return result;
        }();
        REQUIRE(!rejected && rejected.error().code == expected_error);
        const auto after = session.snapshot();
        REQUIRE(after.project_revision == before.project_revision &&
            after.undo_count == before.undo_count &&
            after.redo_count == before.redo_count && after.dirty == before.dirty);
        REQUIRE(session.document_.serialize() == document_before);
        const auto entries_after = journal.read_all();
        REQUIRE(entries_after && entries_after.value().size() == entries_before.value().size());
        REQUIRE(journal_bytes() == journal_before);
    };

    auto invalid_catalog_action = action;
    invalid_catalog_action.placement.material_id =
        std::numeric_limits<std::uint64_t>::max();
    expect_rejected_without_mutation(invalid_catalog_action, carto::core::ErrorCode::not_found);

    auto invalid_action = action;
    invalid_action.expected_project_revision = before_draft.project_revision.next();
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);
    invalid_action = action;
    invalid_action.placement.target_mesh_asset = std::numeric_limits<std::uint64_t>::max();
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::not_found);
    invalid_action = action;
    invalid_action.placement.target_mesh_revision = action.placement.target_mesh_revision.next();
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);
    invalid_action = action;
    invalid_action.placement.target_mesh_digest.bytes[0U] ^= 1U;
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);
    invalid_action = action;
    invalid_action.expected_placement_revision = carto::core::Revision{1U};
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);
    invalid_action = action;
    invalid_action.placement.revision = carto::core::Revision{2U};
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);
    invalid_action = action;
    invalid_action.placement.revision = carto::core::Revision{};
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);

    for (const auto source : {carto::application::OperationSource::ai_proposal,
             carto::application::OperationSource::agent_workbench,
             carto::application::OperationSource::plugin,
             carto::application::OperationSource::recovery}) {
        expect_rejected_without_mutation(action, carto::core::ErrorCode::unsupported, source);
    }

    const auto placed = carto::application::testing::dispatch(session, action);
    REQUIRE(placed && placed.value().document_changed);
    auto after_place = session.snapshot();
    REQUIRE(after_place.decal_placements.size() == 1U);
    const auto encoded_draft = carto::production::serialize_decal_placement(draft.value());
    REQUIRE(encoded_draft);
    const auto encoded_snapshot = carto::production::serialize_decal_placement(
        after_place.decal_placements.front().placement);
    REQUIRE(encoded_snapshot);
    REQUIRE(encoded_snapshot.value() == encoded_draft.value() &&
        after_place.undo_count == before_draft.undo_count + 1U);
    expect_rejected_without_mutation(action, carto::core::ErrorCode::stale_data);

    auto edit_action = action;
    edit_action.expected_project_revision = after_place.project_revision;
    edit_action.expected_placement_revision = action.placement.revision;
    edit_action.placement.revision = action.placement.revision.next();
    edit_action.placement.width = 0.75;
    invalid_action = edit_action;
    invalid_action.expected_placement_revision = carto::core::Revision{};
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);
    invalid_action = edit_action;
    invalid_action.expected_placement_revision = edit_action.placement.revision;
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::stale_data);
    invalid_action = edit_action;
    invalid_action.placement.revision = action.placement.revision;
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::invalid_argument);
    invalid_action = edit_action;
    invalid_action.placement.revision = edit_action.placement.revision.next();
    expect_rejected_without_mutation(invalid_action, carto::core::ErrorCode::invalid_argument);

    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::UndoAction{}));
    REQUIRE(session.snapshot().decal_placements.empty());
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::RedoAction{}));
    REQUIRE(session.snapshot().decal_placements.size() == 1U);
    REQUIRE(carto::application::testing::dispatch(session,
        carto::application::SaveProjectAction{project_path}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    const auto persisted = carto::production::load_decal_placement(
        reopened.value(), draft.value().id);
    REQUIRE(persisted);
    const auto encoded_persisted = carto::production::serialize_decal_placement(
        persisted.value());
    REQUIRE(encoded_persisted && encoded_persisted.value() == encoded_draft.value());

    const auto entries = journal.read_all();
    REQUIRE(entries);
    const auto decal_events = std::count_if(entries.value().begin(), entries.value().end(),
        [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return payload.find("Set Decal Placement") != std::string::npos;
        });
    REQUIRE(decal_events == 1);
}

int main() {
    try {
        application_mesh_geometry_digest_is_canonical_and_revision_independent();
        application_routes_authoring_through_snapshot_and_history();
        application_synthetic_room_snap_gesture_commits_undoes_and_rejects_stale_source();
        application_reuses_revision_keyed_viewport_meshes_without_cross_project_staleness();
        application_builds_read_only_terrain_previews_from_current_project_sources();
        application_builds_read_only_source_bound_wind_vector_previews();
        application_builds_read_only_revision_bound_road_surface_previews();
        application_builds_bounded_source_bound_road_terrain_cut_fill_previews();
        application_commits_revision_bound_road_mesh_through_history_and_reopen();
        application_authors_revision_bound_sweeps_through_history();
        application_routes_biological_authoring_and_proxy_preview_through_history();
        application_authors_and_materializes_revision_bound_sign_text();
        application_assigns_sign_panel_and_lettering_materials_through_carto_history();
        application_persists_project_topology_receipts_through_save_and_reopen();
        application_persists_vertex_merge_lineage_through_save_and_reopen();
        application_persists_center_vertex_merge_position_through_save_and_reopen();
        application_merges_uv_attributes_and_persists_payload_through_reopen();
        application_refuses_unmapped_edge_attributes_atomically_during_weld();
        application_routes_face_poke_through_history_and_persists_lineage();
        application_routes_edge_bevel_through_history_and_persistence();
        application_bevels_selected_edges_across_objects_atomically_and_persists_lineage();
        application_rejects_multi_object_bevel_on_shared_mesh_assets_atomically();
        application_rejects_multi_object_bevel_on_stale_or_invalid_second_owner_atomically();
        application_converts_triangle_pairs_across_objects_atomically_and_persists_lineage();
        application_refuses_multi_object_triangle_to_quad_on_shared_mesh_assets();
        application_rejects_multi_object_triangle_to_quad_atomically_on_stale_or_incompatible_owner();
        application_rejects_edge_bevel_wrong_mode_and_multi_selection();
        application_routes_vertex_bevel_through_history_and_persistence();
        application_bevels_selected_vertices_across_objects_atomically_and_persists_lineage();
        application_rejects_multi_object_vertex_bevel_on_shared_mesh_assets();
        application_rejects_single_selected_shared_vertex_bevel_atomically();
        application_rejects_multi_object_vertex_bevel_on_stale_second_owner_atomically();
        application_rejects_multi_object_vertex_bevel_atomically_on_later_open_mesh();
        application_rejects_vertex_bevel_wrong_mode_and_multi_selection();
        application_bevels_multiple_vertices_per_owner_with_replayable_history();
        application_rejects_multi_vertex_bevel_after_late_boundary_vertex_atomically();
        application_rejects_multi_vertex_bevel_over_budget_before_mutation();
        application_routes_imported_mesh_through_the_durable_command_boundary();
        application_duplicates_mesh_instances_through_durable_history();
        application_duplicates_mesh_instance_batches_through_durable_history();
        application_duplicates_mesh_instance_batches_through_session_action();
        application_rejects_invalid_mesh_instance_batches();
        application_routes_vertex_edit_and_rejects_stale_reselection();
        application_authors_selected_face_custom_normals_with_revisioned_history();
        application_authors_selected_edge_sharp_marks_with_revisioned_history();
        application_sets_selected_vertex_positions_across_objects_atomically_and_persists_patches();
        application_rejects_multi_object_vertex_position_for_shared_stale_and_invalid_owners();
        application_slides_selected_vertices_across_objects_with_owner_local_support_and_history();
        application_slides_multiple_vertices_per_owner_with_replayable_history();
        application_rejects_multi_vertex_slide_edges_and_late_failure_atomically();
        application_rejects_single_selected_shared_vertex_slide_atomically();
        application_rejects_multi_vertex_slide_over_budget_before_mutation();
        application_rejects_multi_object_vertex_slide_with_ambiguous_explicit_support_edge();
        application_rejects_multi_object_vertex_slide_on_shared_mesh_assets();
        application_rejects_multi_object_vertex_slide_on_stale_second_owner_atomically();
        application_rejects_multi_object_vertex_slide_atomically_when_later_owner_has_no_edge();
        project_face_deletion_patch_is_stale_safe_and_replayable();
        project_edge_split_patch_is_stale_safe_and_replayable();
        project_loop_cut_patch_is_stale_safe_and_replayable();
        project_knife_cut_patch_is_digest_bound_and_replayable();
        project_knife_cut_patch_transfers_edge_endpoint_attributes_and_replays();
        application_knife_cut_tool_is_face_bound_and_repeatably_persistent();
        project_bevel_edge_patch_is_digest_bound_and_replayable();
        project_bevel_vertex_patch_is_digest_bound_and_replayable();
        project_face_extrusion_patch_is_digest_bound_and_replayable();
        project_face_inset_patch_is_digest_bound_and_replayable();
        project_face_poke_patch_is_digest_bound_and_replayable();
        project_tri_to_quad_patch_is_digest_bound_and_replayable();
        project_edge_dissolve_patch_is_digest_bound_and_replayable();
        project_merge_by_distance_patch_is_digest_bound_and_replayable();
        application_mesh_cleanup_reviews_preserve_authority_and_durable_lineage();
        application_mesh_cleanup_refuses_stale_foreign_and_overbudget_reviews();
        application_mesh_cleanup_enforces_face_removal_and_corner_budgets();
        project_vertex_merge_patch_is_digest_bound_and_replayable();
        project_selected_edge_split_redo_records_a_replayable_patch();
        project_edge_split_across_objects_is_atomic_and_replayable();
        project_edge_split_refuses_shared_mesh_and_stale_multi_object_selection();
        application_loop_cuts_selected_edges_across_objects_atomically_and_persists_receipts();
        application_rejects_multi_object_loop_cut_shared_stale_and_invalid_later_owners();
        checkpoint_rebase_preserves_persistent_mesh_patch_history();
        application_repeats_last_tool_against_current_document();
        application_rejects_repeat_without_prior_tool();
        application_rejects_repeat_when_saved_component_was_removed();
        application_routes_component_selection_conversion_without_mutation();
        application_converts_component_selection_per_object_without_flattening_ids();
        application_routes_component_selection_expansion_without_mutation();
        application_expands_component_selection_per_object();
        application_deletes_selected_faces_across_objects_atomically();
        application_rejects_stale_multi_object_face_selection_atomically();
        application_rejects_multi_object_face_edits_on_shared_mesh_assets();
        application_dissolves_selected_edges_across_objects_atomically();
        application_rejects_multi_object_edge_dissolve_atomically();
        application_refuses_multi_object_edge_dissolve_on_shared_mesh_assets();
        application_pokes_selected_faces_across_objects_atomically();
        application_merges_selected_vertices_across_objects_atomically();
        application_extrudes_selected_faces_across_objects_atomically();
        application_extrudes_multiple_faces_per_owner_with_replayable_history();
        application_rejects_multi_face_extrusion_revision_exhaustion_atomically();
        application_insets_multiple_faces_per_owner_with_replayable_history();
        application_rejects_multi_face_inset_revision_exhaustion_atomically();
        application_bevels_multiple_edges_per_owner_with_replayable_history();
        application_refuses_multi_edge_bevel_after_late_boundary_edge_atomically();
        application_rejects_multi_edge_bevel_over_command_budget_without_mutation();
        application_refuses_multi_object_face_extrusion_on_shared_or_stale_owners();
        application_insets_selected_faces_across_objects_atomically_and_persists_lineage();
        application_refuses_multi_object_inset_on_shared_mesh_assets();
        application_rejects_multi_object_inset_on_stale_second_owner_atomically();
        application_rejects_multi_object_inset_atomically_on_unsupported_payload();
        application_pokes_multiple_faces_per_owner_atomically();
        application_multi_face_poke_refuses_a_later_exhausted_revision_atomically();
        application_rejects_late_invalid_face_poke_without_partial_mutation();
        application_rejects_multi_object_face_poke_atomically_on_unsupported_payload();
        application_refuses_multi_object_face_poke_on_stale_or_shared_owners();
        application_routes_persistent_edge_selection_without_mutation();
        application_rejects_bad_selection_without_mutating_history();
        application_requires_explicit_discard_for_project_replacement();
        application_bounds_failure_diagnostics();
        application_validates_workspace_layouts();
        application_open_failure_preserves_current_project_and_save_is_atomic();
        application_journals_committed_mutations_and_rolls_back_failed_append();
        application_rejects_journal_revision_drift_before_open();
        application_rolls_back_preexisting_journal_baseline_on_save_failure();
        application_rejects_semantically_wrong_journal_tail();
        application_inspects_recovery_without_mutation();
        application_recovery_inspection_serializes_with_project_writers();
        application_blocks_malformed_recovery_evidence();
        application_requires_explicit_recovery_and_rebinds_saved_state();
        application_recovery_fails_closed_without_mutating_session_or_evidence();
        application_opens_pending_projects_with_automatic_recovery();
        application_auto_recovery_recovers_last_verified_snapshot_across_truncation();
        application_auto_recovery_fails_closed_on_torn_journals_and_dirty_sessions();
        application_auto_recovery_serializes_with_project_writers();
        application_mutation_serializes_with_project_recovery_writer_lease();
        application_routes_spatial_field_creation_and_paint_through_revisioned_history();
        application_authors_revision_bound_roads_through_history_and_journal();
        application_routes_source_reimport_through_human_history();
        application_authors_pbr_materials_through_revisioned_journaled_history();
        application_authors_material_graphs_through_digest_guarded_history();
        application_assigns_material_regions_through_revisioned_history();
        application_authors_presentation_catalog_through_revisioned_history();
        application_places_decals_from_a_current_surface_through_human_history();
    } catch (const std::exception& error) {
        return (void(std::cerr << "FAIL " << error.what() << '\n'), 1);
    }
    return 0;
}
