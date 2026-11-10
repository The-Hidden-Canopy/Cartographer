#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "application_access.hpp"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <carto/attributes/set.hpp>
#include <carto/geometry/primitives.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace carto;
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
template <typename T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
void okay(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
class TempDirectory {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("carto-multiface-test-" + std::to_string(stamp));
        check(std::filesystem::create_directory(path), "test directory must be new");
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};
core::Result<application::DispatchReceipt> dispatch(application::ApplicationSession& session,
                                                     const application::ApplicationAction& action) {
    return application::testing::dispatch(session, action);
}
geometry::EditableMesh many_triangles(std::size_t count) {
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    vertices.reserve(count * 3U); faces.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto id = static_cast<std::uint64_t>(i * 3U + 1U);
        const double x = static_cast<double>(i) * 3.0;
        vertices.push_back({geometry::VertexId{id}, {x, 0, 0}});
        vertices.push_back({geometry::VertexId{id + 1U}, {x + 1, 0, 0}});
        vertices.push_back({geometry::VertexId{id + 2U}, {x, 1, 0}});
        faces.push_back({geometry::FaceId{static_cast<std::uint64_t>(i + 1U)},
                         {geometry::VertexId{id}, geometry::VertexId{id + 1U}, geometry::VertexId{id + 2U}}});
    }
    geometry::EditableMesh mesh;
    okay(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    return mesh;
}
void direct_deletion_and_bound() {
    auto mesh = take(geometry::make_box({2, 2, 2}));
    editor::SelectionState selection;
    okay(selection.set_mode(editor::SelectionMode::face));
    okay(selection.select_face(mesh, geometry::FaceId{1}));
    okay(selection.select_face(mesh, geometry::FaceId{2}, editor::SelectionOperation::add));
    editor::DeleteSelectedFaceCommand command(selection, mesh, false);
    okay(command.execute());
    check(mesh.face_count() == 4U && mesh.vertex_count() == 8U, "direct batch removes both faces");
    okay(command.undo());
    check(mesh.face_count() == 6U && mesh.vertex_count() == 8U, "direct batch undo restores faces");
    okay(command.execute());
    check(mesh.face_count() == 4U, "direct batch redo replays captured selection");

    auto large = many_triangles(editor::kMaxSelectedFacesPerDeletion + 1U);
    editor::SelectionState all;
    okay(all.set_mode(editor::SelectionMode::face));
    for (const auto& face : large.faces_sorted()) okay(all.select_face(large, face.id, editor::SelectionOperation::add));
    const auto revision = large.revision();
    editor::DeleteSelectedFaceCommand too_many(all, large, true);
    const auto rejected = too_many.execute();
    check(!rejected && rejected.error().code == core::ErrorCode::unsupported &&
          large.revision() == revision && large.face_count() == editor::kMaxSelectedFacesPerDeletion + 1U,
          "over-budget direct batch leaves mesh untouched");
}
void project_batch_roundtrip(const std::filesystem::path& path) {
    auto session = std::make_unique<application::ApplicationSession>();
    check(bool(dispatch(*session, application::NewProjectAction{"Multi face"})), "new project");
    check(bool(dispatch(*session, application::SaveProjectAction{path})), "save project");
    check(bool(dispatch(*session, application::CreateBoxAction{"A", {2,2,2}})), "create first box");
    check(bool(dispatch(*session, application::CreateBoxAction{"B", {2,2,2}})), "create second box");
    const auto objects = session->snapshot().objects;
    check(objects.size() == 2U && objects[0].object.mesh_asset && objects[1].object.mesh_asset, "box assets exist");
    const auto a = objects[0].object; const auto b = objects[1].object;
    const auto source_a = session->document_.meshes().at(*a.mesh_asset);
    const auto source_b = session->document_.meshes().at(*b.mesh_asset);
    check(bool(dispatch(*session, application::SetSelectionModeAction{editor::SelectionMode::face})), "face mode");
    check(bool(dispatch(*session, application::SelectFaceAction{a.id, geometry::FaceId{1}})), "select A1");
    check(bool(dispatch(*session, application::SelectFaceAction{a.id, geometry::FaceId{2}, editor::SelectionOperation::add})), "select A2");
    check(bool(dispatch(*session, application::SelectFaceAction{b.id, geometry::FaceId{1}, editor::SelectionOperation::add})), "select B1");
    editor::ToolArguments args; args.remove_orphaned_vertices = false;
    check(bool(dispatch(*session, application::InvokeToolAction{"mesh.remove-face", args})), "delete grouped faces");
    check(session->document_.meshes().at(*a.mesh_asset).face_count() == 4U &&
          session->document_.meshes().at(*b.mesh_asset).face_count() == 5U, "all selected faces removed");
    const auto& patches = session->document_.mesh_patches();
    check(patches.size() == 3U && session->document_.topology_receipts().size() == 3U, "one durable patch per face");
    auto replay_a = take(patches[0].replay(source_a));
    replay_a = take(patches[1].replay(replay_a));
    auto replay_b = take(patches[2].replay(source_b));
    check(replay_a.face_count() == 4U && replay_b.face_count() == 5U, "patches replay in order");
    check(bool(dispatch(*session, application::UndoAction{})), "undo batch");
    check(session->document_.meshes().at(*a.mesh_asset).face_count() == 6U &&
          session->document_.meshes().at(*b.mesh_asset).face_count() == 6U, "undo restores all owners");
    check(bool(dispatch(*session, application::RedoAction{})), "redo batch");
    check(session->document_.meshes().at(*a.mesh_asset).face_count() == 4U &&
          session->document_.meshes().at(*b.mesh_asset).face_count() == 5U &&
          session->document_.mesh_patches().size() == 6U, "redo persists every patch");
    check(bool(dispatch(*session, application::SaveProjectAction{path})), "save result");
    const auto loaded = take(project::ProjectDocument::load(path));
    check(loaded.meshes().at(*a.mesh_asset).face_count() == 4U &&
          loaded.meshes().at(*b.mesh_asset).face_count() == 5U && loaded.mesh_patches().size() == 6U,
          "save and reopen preserve batch and lineage");
    check(bool(dispatch(*session, application::UndoAction{})), "undo before stale redo");
    const auto& mesh_b = session->document_.meshes().at(*b.mesh_asset);
    const auto vertex = mesh_b.vertices_sorted().front();
    auto moved = vertex.position;
    moved.x += 0.01;
    check(bool(session->document_.apply_mesh_patch(*b.mesh_asset,
        geometry::MeshPatch{mesh_b.revision(), {{vertex.id, vertex.position, moved}}})), "change second owner");
    const auto before_rejected_redo = session->document_.serialize();
    const auto redo_count = session->snapshot().redo_count;
    const auto rejected = dispatch(*session, application::RedoAction{});
    check(!rejected && rejected.error().code == core::ErrorCode::stale_data &&
          session->document_.serialize() == before_rejected_redo &&
          session->snapshot().redo_count == redo_count,
          "stale second owner rejects redo without publishing the first owner's changes");
}
void project_orphan_cleanup() {
    auto session = std::make_unique<application::ApplicationSession>();
    check(bool(dispatch(*session, application::CreateBoxAction{"Box", {2,2,2}})), "create box");
    const auto object = session->snapshot().objects.front().object;
    check(object.mesh_asset.has_value(), "box asset exists");
    check(bool(dispatch(*session, application::SetSelectionModeAction{editor::SelectionMode::face})), "face mode");
    for (std::uint64_t id = 1; id <= 6; ++id)
        check(bool(dispatch(*session, application::SelectFaceAction{object.id, geometry::FaceId{id},
            editor::SelectionOperation::add})), "select box face");
    editor::ToolArguments args; args.remove_orphaned_vertices = true;
    check(bool(dispatch(*session, application::InvokeToolAction{"mesh.remove-face", args})), "delete all faces");
    check(session->document_.meshes().at(*object.mesh_asset).face_count() == 0 &&
          session->document_.meshes().at(*object.mesh_asset).vertex_count() == 0, "orphaned vertices removed");
    check(bool(dispatch(*session, application::UndoAction{})) &&
          session->document_.meshes().at(*object.mesh_asset).vertex_count() == 8U, "undo restores orphaned vertices");
    check(bool(dispatch(*session, application::RedoAction{})) &&
          session->document_.meshes().at(*object.mesh_asset).vertex_count() == 0U, "redo removes orphans again");
}
void project_failure_is_atomic() {
    auto session = std::make_unique<application::ApplicationSession>();
    check(bool(dispatch(*session, application::CreateBoxAction{"First", {2,2,2}})), "create first box");
    auto mesh = take(geometry::make_box({2,2,2}));
    const auto topology = take(mesh.topology());
    attributes::AttributeSet attrs({{attributes::AttributeDomain::vertex, mesh.vertex_count()},
        {attributes::AttributeDomain::edge, topology.edges.size()},
        {attributes::AttributeDomain::face, mesh.face_count()},
        {attributes::AttributeDomain::corner, topology.corners.size()}});
    std::vector<attributes::AttributeValue> weights(mesh.vertex_count(), attributes::AttributeValue{0.5F});
    okay(attrs.add_layer({{"weight", attributes::AttributeDomain::vertex, attributes::AttributeType::float32}, std::move(weights)}));
    okay(mesh.set_attribute_payload(std::move(attrs), {}));
    check(bool(dispatch(*session, application::CreateMeshObjectAction{"Attributed", mesh})), "create attributed mesh");
    const auto objects = session->snapshot().objects;
    const auto a = objects[0].object; const auto b = objects[1].object;
    check(bool(dispatch(*session, application::SetSelectionModeAction{editor::SelectionMode::face})), "face mode");
    check(bool(dispatch(*session, application::SelectFaceAction{a.id, geometry::FaceId{1}})), "select A1");
    check(bool(dispatch(*session, application::SelectFaceAction{a.id, geometry::FaceId{2}, editor::SelectionOperation::add})), "select A2");
    check(bool(dispatch(*session, application::SelectFaceAction{b.id, geometry::FaceId{1}, editor::SelectionOperation::add})), "select attributed B1");
    const auto before = session->document_.serialize(); const auto snapshot = session->snapshot();
    const auto rejected = dispatch(*session, application::InvokeToolAction{"mesh.remove-face", {}});
    check(!rejected && session->document_.serialize() == before && session->snapshot().undo_count == snapshot.undo_count,
          "unsupported later owner leaves the whole batch unchanged");
}
void project_batch_bound() {
    auto session = std::make_unique<application::ApplicationSession>();
    auto mesh = many_triangles(editor::kMaxSelectedFacesPerDeletion + 1U);
    check(bool(dispatch(*session, application::CreateMeshObjectAction{"Many faces", mesh})), "create many-face object");
    const auto snapshot = session->snapshot(); const auto object = snapshot.objects.front().object;
    check(object.mesh_asset.has_value(), "many-face asset exists");
    check(bool(dispatch(*session, application::SetSelectionModeAction{editor::SelectionMode::face})), "face mode");
    std::vector<geometry::FaceId> faces;
    for (const auto& face : mesh.faces_sorted()) faces.push_back(face.id);
    const auto selected = session->snapshot();
    check(bool(dispatch(*session, application::SelectFaceBatchAction{selected.project_generation,
        selected.project_revision, session->document_.meshes().at(*object.mesh_asset).revision(), object.id, faces})),
        "select oversized batch");
    const auto before = session->document_.serialize(); const auto undo_count = session->snapshot().undo_count;
    const auto rejected = dispatch(*session, application::InvokeToolAction{"mesh.remove-face", {}});
    check(!rejected && rejected.error().code == core::ErrorCode::unsupported &&
          session->document_.serialize() == before && session->snapshot().undo_count == undo_count,
          "project batch limit rejects before mutation");
}
} // namespace
int main() {
    try {
        TempDirectory temp;
        direct_deletion_and_bound();
        project_batch_roundtrip(temp.path / "multi-face.carto");
        project_orphan_cleanup();
        project_failure_is_atomic();
        project_batch_bound();
        std::cout << "multi-face deletion, atomicity, history and persistence passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
