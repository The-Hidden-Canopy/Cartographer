#include "application_access.hpp"

#include <carto/attributes/set.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/assets/source_registry.hpp>
#include <carto/journal/journal.hpp>
#include <carto/editor/viewport_interaction.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

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
    arguments.segments = 1U;
    const auto beveled = carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{"mesh.bevel-edge", arguments});
    REQUIRE(beveled);
    REQUIRE(beveled.value().parameters.has_value());
    REQUIRE(beveled.value().parameters->distance.has_value());
    REQUIRE(*beveled.value().parameters->distance == 0.1);
    REQUIRE(beveled.value().parameters->segments.has_value());
    REQUIRE(*beveled.value().parameters->segments == 1U);
    REQUIRE(session.snapshot().selection.edges.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 12U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 8U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 12U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.created_vertices.size() == 4U);

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 12U);
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
    arguments.segments = 1U;
    const auto beveled = carto::application::testing::dispatch(
        session, carto::application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(beveled);
    REQUIRE(beveled.value().parameters.has_value());
    REQUIRE(beveled.value().parameters->distance.has_value());
    REQUIRE(*beveled.value().parameters->distance == 0.25);
    REQUIRE(beveled.value().parameters->segments.has_value());
    REQUIRE(*beveled.value().parameters->segments == 1U);
    REQUIRE(session.snapshot().selection.vertices.empty());
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 6U);

    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 4U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 6U);

    REQUIRE(carto::application::testing::dispatch(
        session, carto::application::SaveProjectAction{path}));
    const auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().topology_receipts().size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.created_vertices.size() == 3U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.created_faces.size() == 1U);
    REQUIRE(loaded.value().topology_receipts().front().receipt.removed_vertices.size() == 1U);

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::testing::dispatch(
        reopened, carto::application::OpenProjectAction{path}));
    REQUIRE(reopened.snapshot().viewport.scene.instances().front().mesh->vertex_ids.size() == 6U);
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

void application_routes_vertex_edit_and_rejects_stale_reselection() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::testing::dispatch(session, carto::application::CreateBoxAction{
        "Box", {2.0, 2.0, 2.0}}));
    auto snapshot = session.snapshot();
    const auto object = snapshot.objects.front().object.id;
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

    REQUIRE(!carto::application::testing::dispatch(session, carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(session.snapshot().undo_count == 2U);
    REQUIRE(!session.snapshot().problems.empty());
    REQUIRE(carto::application::testing::dispatch(session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().redo_count == 1U);
    REQUIRE(carto::application::testing::dispatch(session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().redo_count == 0U);
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
    const auto before_create = session.snapshot();
    const auto created = carto::application::testing::dispatch(session,
        carto::application::SetSweepSourceAction{
            before_create.project_revision, source});
    REQUIRE(created);
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.sweep_sources.size() == 1U &&
            snapshot.sweep_sources.front().source.source_revision ==
                carto::core::Revision{1U});

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
    REQUIRE(!session.sweep_source_preview("sweep.authoring.missing"));

    const auto stale = carto::application::testing::dispatch(session,
        carto::application::SetSweepSourceAction{before_create.project_revision, source});
    REQUIRE(!stale && session.snapshot().project_revision == snapshot.project_revision);

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
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened && reopened.value().sweep_sources().at(source.source_identity).
            source_revision == carto::core::Revision{5U});
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

} // namespace

int main() {
    try {
        application_mesh_geometry_digest_is_canonical_and_revision_independent();
        application_routes_authoring_through_snapshot_and_history();
        application_synthetic_room_snap_gesture_commits_undoes_and_rejects_stale_source();
        application_reuses_revision_keyed_viewport_meshes_without_cross_project_staleness();
        application_builds_read_only_terrain_previews_from_current_project_sources();
        application_builds_read_only_source_bound_wind_vector_previews();
        application_builds_read_only_revision_bound_road_surface_previews();
        application_authors_revision_bound_sweeps_through_history();
        application_persists_project_topology_receipts_through_save_and_reopen();
        application_persists_vertex_merge_lineage_through_save_and_reopen();
        application_routes_face_poke_through_history_and_persists_lineage();
        application_routes_edge_bevel_through_history_and_persistence();
        application_rejects_edge_bevel_wrong_mode_and_multi_selection();
        application_routes_vertex_bevel_through_history_and_persistence();
        application_rejects_vertex_bevel_wrong_mode_and_multi_selection();
        application_routes_imported_mesh_through_the_durable_command_boundary();
        application_duplicates_mesh_instances_through_durable_history();
        application_routes_vertex_edit_and_rejects_stale_reselection();
        application_repeats_last_tool_against_current_document();
        application_rejects_repeat_without_prior_tool();
        application_rejects_repeat_when_saved_component_was_removed();
        application_routes_component_selection_conversion_without_mutation();
        application_routes_component_selection_expansion_without_mutation();
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
        application_mutation_serializes_with_project_recovery_writer_lease();
        application_routes_spatial_field_creation_and_paint_through_revisioned_history();
        application_authors_revision_bound_roads_through_history_and_journal();
        application_routes_source_reimport_through_human_history();
        application_authors_pbr_materials_through_revisioned_journaled_history();
        application_authors_material_graphs_through_digest_guarded_history();
        application_assigns_material_regions_through_revisioned_history();
    } catch (const std::exception& error) {
        return (void(std::cerr << "FAIL " << error.what() << '\n'), 1);
    }
    return 0;
}
