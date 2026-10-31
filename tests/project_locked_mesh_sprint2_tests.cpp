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

#include <carto/geometry/mesh.hpp>
#include <carto/project/project.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
using namespace carto;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::current_path() /
        ("carto-locks-sprint2-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "journal is readable");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

geometry::EditableMesh triangle() {
    geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    require(a && b && c, "vertices created");
    require(static_cast<bool>(mesh.add_face({a.value(), b.value(), c.value()})), "face created");
    return mesh;
}

void direct_project_boundary(bool locked_owner, bool locked_sibling) {
    project::ProjectDocument document;
    const auto asset = document.add_mesh(triangle());
    require(static_cast<bool>(asset), "mesh created");
    const auto first = document.create_object("selected");
    require(first && document.attach_mesh(first.value(), asset.value()), "owner attached");
    if (locked_sibling) {
        const auto sibling = document.create_object("sibling");
        require(sibling && document.attach_mesh(sibling.value(), asset.value()), "sibling attached");
        const_cast<scene::SceneObject*>(document.scene().find(sibling.value()))->locked = true;
    }
    if (locked_owner) {
        const_cast<scene::SceneObject*>(document.scene().find(first.value()))->locked = true;
    }
    const auto& source = document.meshes().at(asset.value());
    const geometry::MeshPatch patch{source.revision(),
        {{geometry::VertexId{1U}, source.find_vertex(geometry::VertexId{1U})->position,
          {0.0, 0.0, 0.25}}}};
    auto changed = source;
    require(static_cast<bool>(changed.set_vertex_position(geometry::VertexId{1U}, {0.0, 0.0, 0.25})),
        "replacement fixture created");
    const std::string before = document.serialize();
    const auto vertex = document.apply_mesh_patch(asset.value(), patch);
    const auto face = document.apply_face_extrusion_patch(asset.value(),
        geometry::FaceExtrusionPatch{source.revision(), geometry::FaceId{1U}, 0.25});
    const auto replacement = document.replace_mesh_if_revision(asset.value(), source.revision(), changed);
    require(!vertex && !face && !replacement &&
            vertex.error().code == core::ErrorCode::invalid_state &&
            face.error().code == core::ErrorCode::invalid_state &&
            replacement.error().code == core::ErrorCode::invalid_state,
        "locked owner or shared sibling blocks patch, topology and replacement");
    require(document.serialize() == before, "locked refusals preserve complete project bytes");
}

void checkpoint_rebase_observes_current_locks() {
    project::ProjectDocument base;
    const auto asset = base.add_mesh(triangle());
    const auto object = base.create_object("owner");
    require(asset && object && base.attach_mesh(object.value(), asset.value()),
        "rebase fixture is attached");
    project::ProjectDocument speculative = base;
    const auto& source = base.meshes().at(asset.value());
    const geometry::MeshPatch patch{source.revision(),
        {{geometry::VertexId{1U}, source.find_vertex(geometry::VertexId{1U})->position,
          {0.0, 0.0, 0.25}}}};
    require(static_cast<bool>(speculative.apply_mesh_patch(asset.value(), patch)),
        "unlocked speculative mesh edit succeeds");
    const auto merged = project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, base, speculative, {}, {asset.value()}, {}, {asset.value()});
    require(static_cast<bool>(merged), "unlocked mesh rebase succeeds");

    project::ProjectDocument current = base;
    const_cast<scene::SceneObject*>(current.scene().find(object.value()))->locked = true;
    require(static_cast<bool>(current.set_name("Current")), "current revision advances");
    const auto current_bytes = current.serialize();
    const auto locked_merge = project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, current, speculative, {}, {asset.value()}, {}, {asset.value()});
    require(!locked_merge && locked_merge.error().code == core::ErrorCode::invalid_state &&
            current.serialize() == current_bytes,
        "rebase refuses a lock acquired after speculation without changing current");

    project::ProjectDocument locked_base = base;
    const_cast<scene::SceneObject*>(locked_base.scene().find(object.value()))->locked = true;
    const auto locked_bytes = locked_base.serialize();
    const auto selection = project::ProjectDocument::select_agent_rebase_step_changes(
        locked_base, speculative, {}, {asset.value()}, 1U);
    require(!selection && selection.error().code == core::ErrorCode::invalid_state &&
            locked_base.serialize() == locked_bytes,
        "step selection refuses to copy mesh data over a locked instance");
}

void unrelated_locked_instance_does_not_block_unlocked_mesh() {
    project::ProjectDocument document;
    const auto first_asset = document.add_mesh(triangle());
    const auto second_asset = document.add_mesh(triangle());
    const auto first = document.create_object("editable");
    const auto second = document.create_object("locked other");
    require(first_asset && second_asset && first && second &&
            document.attach_mesh(first.value(), first_asset.value()) &&
            document.attach_mesh(second.value(), second_asset.value()), "two independent meshes attached");
    const_cast<scene::SceneObject*>(document.scene().find(second.value()))->locked = true;
    const auto& mesh = document.meshes().at(first_asset.value());
    const geometry::MeshPatch patch{mesh.revision(),
        {{geometry::VertexId{1U}, mesh.find_vertex(geometry::VertexId{1U})->position,
          {0.0, 0.0, 0.25}}}};
    const auto other_before = project::canonical_mesh_snapshot_digest(document.meshes().at(second_asset.value()));
    require(static_cast<bool>(document.apply_mesh_patch(first_asset.value(), patch)) &&
            project::canonical_mesh_snapshot_digest(document.meshes().at(second_asset.value())) == other_before,
        "an unrelated locked mesh does not block an unlocked mesh edit");
}

void application_lock_and_history(bool shared_sibling, bool face_tool) {
    TempDirectory temp;
    const auto path = temp.path / "history.carto";
    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    application::ApplicationSession session;
    require(static_cast<bool>(application::testing::dispatch(session, application::NewProjectAction{"Locks"})),
        "project created");
    require(static_cast<bool>(application::testing::dispatch(session, application::SaveProjectAction{path})),
        "journal enabled");
    require(static_cast<bool>(application::testing::dispatch(session,
        application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}})), "box created");
    const auto selected = session.snapshot().objects.front().object.id;
    scene::ObjectId lock_id = selected;
    if (shared_sibling) {
        require(static_cast<bool>(application::testing::dispatch(session,
            application::DuplicateMeshInstanceAction{selected, "Sibling"})), "shared sibling created");
        const auto objects = session.snapshot().objects;
        require(objects.size() == 2U && objects[0].object.mesh_asset == objects[1].object.mesh_asset,
            "sibling uses same asset");
        lock_id = objects[0].object.id == selected ? objects[1].object.id : objects[0].object.id;
    }
    require(static_cast<bool>(application::testing::dispatch(session,
        application::SetSelectionModeAction{face_tool ? editor::SelectionMode::face : editor::SelectionMode::vertex})),
        "component mode selected");
    if (face_tool) {
        require(static_cast<bool>(application::testing::dispatch(session,
            application::SelectFaceAction{selected, geometry::FaceId{1U}})), "face selected");
    } else {
        require(static_cast<bool>(application::testing::dispatch(session,
            application::SelectVertexAction{selected, geometry::VertexId{1U}})), "vertex selected");
    }
    editor::ToolArguments arguments;
    arguments.distance = 0.25;
    arguments.position = {0.125, 0.125, 0.125};
    const application::InvokeToolAction action{
        face_tool ? "mesh.extrude-face" : "mesh.set-vertex-position", arguments};
    auto* locked = const_cast<scene::SceneObject*>(session.document_.scene().find(lock_id));
    require(locked != nullptr, "lock target exists");
    locked->locked = true; // adversarial lock change after selection
    const auto before = session.document_.serialize();
    const auto before_snapshot = session.snapshot();
    const auto before_journal = bytes(journal_path);
    const auto refused = application::testing::dispatch(session, action);
    require(!refused, "locked component edit refused");
    require(session.document_.serialize() == before &&
            session.snapshot().undo_count == before_snapshot.undo_count &&
            session.snapshot().redo_count == before_snapshot.redo_count &&
            bytes(journal_path) == before_journal,
        "refusal preserves project, history and journal");
    locked = const_cast<scene::SceneObject*>(session.document_.scene().find(lock_id));
    require(locked != nullptr, "lock fixture reacquires the current document object");
    locked->locked = false;
    require(static_cast<bool>(application::testing::dispatch(session, action)), "unlocked edit succeeds");
    const auto mesh_asset = *session.document_.scene().find(selected)->mesh_asset;
    const auto edited_count = session.document_.meshes().at(mesh_asset).vertex_count();
    const auto edited_faces = session.document_.meshes().at(mesh_asset).face_count();
    const auto edited_position = session.document_.meshes().at(mesh_asset)
        .find_vertex(geometry::VertexId{1U})->position;
    for (int i = 0; i < 2; ++i) {
        require(static_cast<bool>(application::testing::dispatch(session, application::UndoAction{})), "undo succeeds");
        require(static_cast<bool>(application::testing::dispatch(session, application::RedoAction{})), "redo succeeds");
        const auto& mesh = session.document_.meshes().at(mesh_asset);
        const auto position = mesh.find_vertex(geometry::VertexId{1U})->position;
        require(mesh.vertex_count() == edited_count && mesh.face_count() == edited_faces &&
                position.x == edited_position.x && position.y == edited_position.y &&
                position.z == edited_position.z, "redo restores edited geometry");
    }
    locked = const_cast<scene::SceneObject*>(session.document_.scene().find(lock_id));
    require(locked != nullptr, "lock target exists before undo");
    locked->locked = true;
    const auto undo_before = session.document_.serialize();
    const auto undo_snapshot = session.snapshot();
    const auto undo_journal = bytes(journal_path);
    require(!application::testing::dispatch(session, application::UndoAction{}),
        "undo refuses a newly locked instance");
    require(session.document_.serialize() == undo_before &&
            session.snapshot().undo_count == undo_snapshot.undo_count &&
            session.snapshot().redo_count == undo_snapshot.redo_count &&
            bytes(journal_path) == undo_journal, "failed undo preserves document, history and journal");
    locked = const_cast<scene::SceneObject*>(session.document_.scene().find(lock_id));
    require(locked != nullptr, "lock fixture reacquires the current document object");
    locked->locked = false;
    require(static_cast<bool>(application::testing::dispatch(session, application::UndoAction{})), "undo after unlock");
    locked = const_cast<scene::SceneObject*>(session.document_.scene().find(lock_id));
    require(locked != nullptr, "lock target still exists");
    locked->locked = true;
    const auto redo_before = session.document_.serialize();
    const auto redo_snapshot = session.snapshot();
    const auto redo_journal = bytes(journal_path);
    require(!application::testing::dispatch(session, application::RedoAction{}), "redo refuses newly locked instance");
    require(session.document_.serialize() == redo_before &&
            session.snapshot().undo_count == redo_snapshot.undo_count &&
            session.snapshot().redo_count == redo_snapshot.redo_count &&
            bytes(journal_path) == redo_journal, "failed redo preserves document, history and journal");
    locked = const_cast<scene::SceneObject*>(session.document_.scene().find(lock_id));
    require(locked != nullptr, "lock fixture reacquires the current document object");
    locked->locked = false;
    require(static_cast<bool>(application::testing::dispatch(session, application::RedoAction{})),
        "redo remains available after unlocking");
}
} // namespace

int main() {
    direct_project_boundary(true, false);
    direct_project_boundary(false, true);
    checkpoint_rebase_observes_current_locks();
    unrelated_locked_instance_does_not_block_unlocked_mesh();
    application_lock_and_history(false, false);
    application_lock_and_history(true, false);
    application_lock_and_history(false, true);
    application_lock_and_history(true, true);
    std::cout << "project locked mesh sprint2 tests passed\n";
    return EXIT_SUCCESS;
}
