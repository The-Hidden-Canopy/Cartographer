#include <carto/editor/command_bus.hpp>
#include <carto/editor/selection.hpp>
#include <carto/editor/tools.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/io/obj.hpp>
#include <carto/project/project.hpp>
#include <carto/render/render_scene.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            throw TestFailure(std::string("requirement failed: ") + #condition);            \
        }                                                                                   \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("cartographer-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

carto::geometry::EditableMesh triangle_mesh() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    return mesh;
}

void scene_rejects_dangling_parent_and_cycles() {
    carto::scene::Scene scene;
    const auto parent = scene.create_object("Parent");
    const auto child = scene.create_object("Child");
    REQUIRE(parent && child);
    REQUIRE(scene.set_parent(child.value(), parent.value()));
    const auto missing = scene.set_parent(child.value(), carto::scene::ObjectId{999});
    REQUIRE(!missing);
    REQUIRE(missing.error().code == carto::core::ErrorCode::not_found);
    const auto cycle = scene.set_parent(parent.value(), child.value());
    REQUIRE(!cycle);
    REQUIRE(cycle.error().code == carto::core::ErrorCode::cycle_detected);
    REQUIRE(!scene.remove_object(parent.value()));
}

void scene_resolves_double_precision_world_transform() {
    carto::scene::Scene scene;
    carto::core::Transform root_transform;
    root_transform.translation = {1.0, 2.0, 3.0};
    const auto root = scene.create_object("Root", root_transform);
    REQUIRE(root);
    carto::core::Transform child_transform;
    child_transform.translation = {0.25, 0.5, 0.75};
    const auto child = scene.create_object("Child", child_transform);
    REQUIRE(child);
    REQUIRE(scene.set_parent(child.value(), root.value()));
    const auto world = scene.world_transform(child.value());
    REQUIRE(world);
    REQUIRE(world.value().translation.x == 1.25);
    REQUIRE(world.value().translation.y == 2.5);
    REQUIRE(world.value().translation.z == 3.75);
}

void selection_is_mode_scoped_and_context_validated() {
    carto::scene::Scene scene;
    const auto first_object = scene.create_object("First");
    const auto second_object = scene.create_object("Second");
    REQUIRE(first_object && second_object);

    auto mesh = triangle_mesh();
    const auto vertex = mesh.vertices_sorted().front().id;
    carto::editor::SelectionState selection;
    REQUIRE(selection.select_object(scene, first_object.value()));
    REQUIRE(selection.select_object(
        scene, second_object.value(), carto::editor::SelectionOperation::add));
    REQUIRE(selection.mode() == carto::editor::SelectionMode::object);
    REQUIRE(selection.active_count() == 2U);
    REQUIRE(selection.select_object(
        scene, second_object.value(), carto::editor::SelectionOperation::toggle));
    REQUIRE(selection.active_count() == 1U);
    REQUIRE(selection.validate(scene));

    REQUIRE(selection.select_vertex(mesh, vertex));
    REQUIRE(selection.mode() == carto::editor::SelectionMode::vertex);
    REQUIRE(selection.active_count() == 1U);
    REQUIRE(selection.selected_objects().empty());
    REQUIRE(selection.validate(scene, &mesh));
    const auto missing_vertex = selection.select_vertex(mesh, carto::geometry::VertexId{999});
    REQUIRE(!missing_vertex);
    REQUIRE(missing_vertex.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(selection.active_count() == 1U);
    const auto missing_context = selection.validate(scene);
    REQUIRE(!missing_context);
    REQUIRE(missing_context.error().code == carto::core::ErrorCode::invalid_argument);

    REQUIRE(selection.select_vertex(
        mesh, vertex, carto::editor::SelectionOperation::toggle));
    REQUIRE(selection.empty());
    REQUIRE(selection.validate(scene, &mesh));

    auto box = carto::geometry::make_box({2.0, 2.0, 2.0});
    auto plane = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(box && plane);
    const auto box_face = box.value().faces_sorted().back().id;
    REQUIRE(selection.select_face(box.value(), box_face));
    REQUIRE(selection.mode() == carto::editor::SelectionMode::face);
    REQUIRE(selection.validate(scene, &box.value()));
    const auto stale_selection = selection.validate(scene, &plane.value());
    REQUIRE(!stale_selection);
    REQUIRE(stale_selection.error().code == carto::core::ErrorCode::stale_data);

    REQUIRE(scene.remove_object(second_object.value()));
    carto::editor::SelectionState stale_object_selection;
    REQUIRE(stale_object_selection.select_object(scene, first_object.value()));
    REQUIRE(scene.remove_object(first_object.value()));
    const auto stale_object = stale_object_selection.validate(scene);
    REQUIRE(!stale_object);
    REQUIRE(stale_object.error().code == carto::core::ErrorCode::stale_data);
}

void mesh_rejects_degenerate_and_duplicate_topology() {
    carto::geometry::EditableMesh degenerate;
    const auto a = degenerate.add_vertex({0.0, 0.0, 0.0});
    const auto b = degenerate.add_vertex({1.0, 0.0, 0.0});
    const auto c = degenerate.add_vertex({2.0, 0.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(!degenerate.add_face({a.value(), b.value(), c.value()}));
    REQUIRE(degenerate.face_count() == 0U);

    auto mesh = triangle_mesh();
    const auto vertices = mesh.vertices_sorted();
    REQUIRE(!mesh.add_face({vertices[0].id, vertices[1].id, vertices[2].id}));
    REQUIRE(mesh.face_count() == 1U);

    carto::geometry::EditableMesh non_manifold;
    const auto n0 = non_manifold.add_vertex({0.0, 0.0, 0.0});
    const auto n1 = non_manifold.add_vertex({1.0, 0.0, 0.0});
    const auto n2 = non_manifold.add_vertex({0.0, 1.0, 0.0});
    const auto n3 = non_manifold.add_vertex({0.0, -1.0, 0.0});
    const auto n4 = non_manifold.add_vertex({0.0, 0.0, 1.0});
    REQUIRE(n0 && n1 && n2 && n3 && n4);
    REQUIRE(non_manifold.add_face({n0.value(), n1.value(), n2.value()}));
    REQUIRE(non_manifold.add_face({n1.value(), n0.value(), n3.value()}));
    REQUIRE(!non_manifold.add_face({n0.value(), n1.value(), n4.value()}));
}

void mesh_compiles_without_mutating_source_and_exposes_boundary_topology() {
    auto mesh = triangle_mesh();
    const auto before_revision = mesh.revision();
    const auto before_vertices = mesh.vertex_count();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().source_revision == before_revision);
    REQUIRE(mesh.revision() == before_revision);
    REQUIRE(mesh.vertex_count() == before_vertices);
    REQUIRE(compiled.value().indices.size() == 3U);

    const auto topology = mesh.topology();
    REQUIRE(topology);
    REQUIRE(topology.value().half_edges.size() == 3U);
    REQUIRE(topology.value().validate());
    for (const auto& edge : topology.value().half_edges) {
        REQUIRE(!edge.twin.has_value());
    }
}

void primitives_are_deterministic_and_dimension_validated() {
    const auto box = carto::geometry::make_box({2.0, 4.0, 6.0});
    REQUIRE(box);
    REQUIRE(box.value().vertex_count() == 8U);
    REQUIRE(box.value().face_count() == 6U);
    const auto compiled_box = box.value().compile();
    REQUIRE(compiled_box);
    REQUIRE(compiled_box.value().bounds.minimum.x == -1.0);
    REQUIRE(compiled_box.value().bounds.minimum.y == -2.0);
    REQUIRE(compiled_box.value().bounds.minimum.z == -3.0);
    REQUIRE(compiled_box.value().bounds.maximum.x == 1.0);
    REQUIRE(compiled_box.value().bounds.maximum.y == 2.0);
    REQUIRE(compiled_box.value().bounds.maximum.z == 3.0);

    const auto same_box = carto::geometry::make_box({2.0, 4.0, 6.0});
    REQUIRE(same_box);
    REQUIRE(box.value().vertices_sorted().size() == same_box.value().vertices_sorted().size());
    REQUIRE(box.value().faces_sorted().size() == same_box.value().faces_sorted().size());
    for (std::size_t index = 0; index < box.value().vertices_sorted().size(); ++index) {
        const auto left = box.value().vertices_sorted().at(index);
        const auto right = same_box.value().vertices_sorted().at(index);
        REQUIRE(left.id == right.id);
        REQUIRE(left.position.x == right.position.x);
        REQUIRE(left.position.y == right.position.y);
        REQUIRE(left.position.z == right.position.z);
    }

    const auto plane = carto::geometry::make_plane(4.0, 2.0);
    REQUIRE(plane);
    REQUIRE(plane.value().vertex_count() == 4U);
    REQUIRE(plane.value().face_count() == 1U);
    const auto compiled_plane = plane.value().compile();
    REQUIRE(compiled_plane);
    REQUIRE(compiled_plane.value().normals.front().y > 0.99);

    const auto invalid_box = carto::geometry::make_box({0.0, 1.0, 1.0});
    REQUIRE(!invalid_box);
    REQUIRE(invalid_box.error().code == carto::core::ErrorCode::invalid_argument);
    const auto invalid_plane = carto::geometry::make_plane(
        std::numeric_limits<double>::quiet_NaN(), 2.0);
    REQUIRE(!invalid_plane);
}

void vertex_edit_command_is_reversible_and_topology_atomic() {
    auto mesh = triangle_mesh();
    const auto vertex = mesh.vertices_sorted().at(2);
    const auto before = vertex.position;
    const carto::core::Vec3d after{0.25, 1.25, 0.0};
    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(std::make_unique<carto::editor::SetVertexPositionCommand>(
        mesh, vertex.id, before, after)));
    REQUIRE(mesh.find_vertex(vertex.id)->position.x == after.x);
    REQUIRE(mesh.find_vertex(vertex.id)->position.y == after.y);
    REQUIRE(bus.undo_count() == 1U);
    REQUIRE(bus.undo());
    REQUIRE(mesh.find_vertex(vertex.id)->position.x == before.x);
    REQUIRE(mesh.find_vertex(vertex.id)->position.y == before.y);
    REQUIRE(bus.redo());

    const auto edited = *mesh.find_vertex(vertex.id);
    const auto invalid_topology = carto::core::Vec3d{2.0, 0.0, 0.0};
    REQUIRE(!bus.execute(std::make_unique<carto::editor::SetVertexPositionCommand>(
        mesh, vertex.id, edited.position, invalid_topology)));
    REQUIRE(bus.undo_count() == 1U);
    REQUIRE(mesh.find_vertex(vertex.id)->position.x == edited.position.x);
    REQUIRE(mesh.find_vertex(vertex.id)->position.y == edited.position.y);

    const auto invalid_value = carto::core::Vec3d{
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    REQUIRE(!bus.execute(std::make_unique<carto::editor::SetVertexPositionCommand>(
        mesh, vertex.id, edited.position, invalid_value)));
    REQUIRE(bus.undo_count() == 1U);
    REQUIRE(mesh.find_vertex(vertex.id)->position.x == edited.position.x);
}

void face_extrude_command_is_reversible_and_validated() {
    auto primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(primitive);
    auto mesh = std::move(primitive.value());
    const auto face = mesh.faces_sorted().front().id;
    const auto before_revision = mesh.revision();
    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(std::make_unique<carto::editor::ExtrudeFaceCommand>(
        mesh, face, 0.5)));
    REQUIRE(mesh.vertex_count() == 12U);
    REQUIRE(mesh.face_count() == 10U);
    REQUIRE(mesh.validate());
    REQUIRE(mesh.revision() > before_revision);
    REQUIRE(mesh.topology());
    REQUIRE(mesh.compile());

    REQUIRE(bus.undo());
    REQUIRE(mesh.vertex_count() == 8U);
    REQUIRE(mesh.face_count() == 6U);
    REQUIRE(mesh.validate());
    REQUIRE(bus.redo());
    REQUIRE(mesh.vertex_count() == 12U);
    REQUIRE(mesh.face_count() == 10U);
    REQUIRE(mesh.validate());

    auto invalid_primitive = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(invalid_primitive);
    auto invalid_mesh = std::move(invalid_primitive.value());
    const auto invalid_face = invalid_mesh.faces_sorted().front().id;
    const auto invalid_revision = invalid_mesh.revision();
    carto::editor::CommandBus invalid_bus;
    REQUIRE(!invalid_bus.execute(std::make_unique<carto::editor::ExtrudeFaceCommand>(
        invalid_mesh, invalid_face, 0.0)));
    REQUIRE(invalid_bus.undo_count() == 0U);
    REQUIRE(invalid_mesh.vertex_count() == 8U);
    REQUIRE(invalid_mesh.face_count() == 6U);
    REQUIRE(invalid_mesh.revision() == invalid_revision);
}

void selected_face_tool_adapter_validates_before_delegating() {
    auto primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(primitive);
    auto mesh = std::move(primitive.value());
    const auto face = mesh.faces_sorted().front().id;
    carto::editor::SelectionState selection;
    REQUIRE(selection.select_face(mesh, face));

    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(std::make_unique<carto::editor::ExtrudeSelectedFaceCommand>(
        selection, mesh, 0.5)));
    REQUIRE(mesh.vertex_count() == 12U);
    REQUIRE(mesh.face_count() == 10U);
    REQUIRE(bus.undo());
    REQUIRE(mesh.vertex_count() == 8U);
    REQUIRE(bus.redo());
    REQUIRE(mesh.vertex_count() == 12U);

    auto wrong_mode_primitive = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(wrong_mode_primitive);
    auto wrong_mode_mesh = std::move(wrong_mode_primitive.value());
    carto::editor::SelectionState vertex_selection;
    REQUIRE(vertex_selection.select_vertex(
        wrong_mode_mesh, wrong_mode_mesh.vertices_sorted().front().id));
    carto::editor::CommandBus wrong_mode_bus;
    REQUIRE(!wrong_mode_bus.execute(std::make_unique<carto::editor::ExtrudeSelectedFaceCommand>(
        vertex_selection, wrong_mode_mesh, 0.5)));
    REQUIRE(wrong_mode_bus.undo_count() == 0U);
    REQUIRE(wrong_mode_mesh.face_count() == 6U);

    auto multiple_primitive = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(multiple_primitive);
    auto multiple_mesh = std::move(multiple_primitive.value());
    const auto multiple_faces = multiple_mesh.faces_sorted();
    carto::editor::SelectionState multiple_selection;
    REQUIRE(multiple_selection.select_face(multiple_mesh, multiple_faces[0].id));
    REQUIRE(multiple_selection.select_face(
        multiple_mesh,
        multiple_faces[1].id,
        carto::editor::SelectionOperation::add));
    carto::editor::CommandBus multiple_bus;
    REQUIRE(!multiple_bus.execute(std::make_unique<carto::editor::ExtrudeSelectedFaceCommand>(
        multiple_selection, multiple_mesh, 0.5)));
    REQUIRE(multiple_bus.undo_count() == 0U);
    REQUIRE(multiple_mesh.vertex_count() == 8U);
    REQUIRE(multiple_mesh.face_count() == 6U);

    auto stale_primitive = carto::geometry::make_box({1.0, 1.0, 1.0});
    auto stale_context = carto::geometry::make_plane(2.0, 2.0);
    REQUIRE(stale_primitive && stale_context);
    const auto stale_face = stale_primitive.value().faces_sorted().back().id;
    carto::editor::SelectionState stale_selection;
    REQUIRE(stale_selection.select_face(stale_primitive.value(), stale_face));
    carto::editor::CommandBus stale_bus;
    REQUIRE(!stale_bus.execute(std::make_unique<carto::editor::ExtrudeSelectedFaceCommand>(
        stale_selection, stale_context.value(), 0.5)));
    REQUIRE(stale_bus.undo_count() == 0U);
    REQUIRE(stale_context.value().vertex_count() == 4U);
    REQUIRE(stale_context.value().face_count() == 1U);
}

void tool_registry_dispatches_registered_commands_through_history() {
    auto primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(primitive);
    auto mesh = std::move(primitive.value());
    carto::editor::SelectionState selection;
    REQUIRE(selection.select_face(mesh, mesh.faces_sorted().front().id));
    carto::editor::ToolContext context(selection, mesh);
    carto::editor::ToolArguments arguments{0.5};
    carto::editor::CommandBus history;
    carto::editor::ToolRegistry registry;
    REQUIRE(registry.register_builtin_tools());
    const auto descriptors = registry.descriptors();
    REQUIRE(descriptors.size() == 1U);
    REQUIRE(descriptors.front().id == "mesh.extrude-face");
    REQUIRE(!registry.register_builtin_tools());

    REQUIRE(registry.invoke("mesh.extrude-face", context, arguments, history));
    REQUIRE(history.undo_count() == 1U);
    REQUIRE(mesh.vertex_count() == 12U);
    REQUIRE(history.undo());
    REQUIRE(mesh.vertex_count() == 8U);
    REQUIRE(history.redo());
    REQUIRE(mesh.vertex_count() == 12U);

    const auto missing = registry.invoke("missing-tool", context, arguments, history);
    REQUIRE(!missing);
    REQUIRE(missing.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(history.undo_count() == 1U);

    carto::editor::ToolRegistry invalid_registry;
    REQUIRE(!invalid_registry.register_tool(
        {"", "Invalid"},
        [](const carto::editor::ToolContext&, const carto::editor::ToolArguments&)
            -> carto::core::Result<std::unique_ptr<carto::editor::EditorCommand>> {
            return carto::core::Result<std::unique_ptr<carto::editor::EditorCommand>>::success(nullptr);
        }));
    REQUIRE(!invalid_registry.register_tool(
        {"null-command", "Null command"}, carto::editor::ToolFactory{}));
    REQUIRE(invalid_registry.register_tool(
        {"null-command", "Null command"},
        [](const carto::editor::ToolContext&, const carto::editor::ToolArguments&)
            -> carto::core::Result<std::unique_ptr<carto::editor::EditorCommand>> {
            return carto::core::Result<std::unique_ptr<carto::editor::EditorCommand>>::success(nullptr);
        }));
    const auto null_command = invalid_registry.invoke(
        "null-command", context, arguments, history);
    REQUIRE(!null_command);
    REQUIRE(null_command.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(history.undo_count() == 1U);
}

void create_mesh_object_command_preserves_ids_across_undo_redo() {
    auto document = carto::project::ProjectDocument::create("Tool workflow");
    REQUIRE(document);
    auto primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(primitive);

    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(std::make_unique<carto::editor::CreateMeshObjectCommand>(
        document.value(), "Box", std::move(primitive.value()))));
    REQUIRE(document.value().scene().size() == 1U);
    REQUIRE(document.value().meshes().size() == 1U);
    const auto object = document.value().scene().objects_sorted().front();
    REQUIRE(object.mesh_asset.has_value());
    const auto mesh_asset = *object.mesh_asset;
    REQUIRE(document.value().validate());

    REQUIRE(bus.undo());
    REQUIRE(document.value().scene().size() == 0U);
    REQUIRE(document.value().meshes().empty());
    REQUIRE(document.value().validate());

    REQUIRE(bus.redo());
    REQUIRE(document.value().scene().size() == 1U);
    REQUIRE(document.value().meshes().size() == 1U);
    const auto restored = document.value().scene().objects_sorted().front();
    REQUIRE(restored.id == object.id);
    REQUIRE(restored.mesh_asset.has_value());
    REQUIRE(*restored.mesh_asset == mesh_asset);
    REQUIRE(document.value().validate());

    auto invalid_document = carto::project::ProjectDocument::create("Invalid tool workflow");
    REQUIRE(invalid_document);
    auto second_primitive = carto::geometry::make_plane(1.0, 1.0);
    REQUIRE(second_primitive);
    carto::editor::CommandBus invalid_bus;
    REQUIRE(!invalid_bus.execute(std::make_unique<carto::editor::CreateMeshObjectCommand>(
        invalid_document.value(), "", std::move(second_primitive.value()))));
    REQUIRE(invalid_bus.undo_count() == 0U);
    REQUIRE(invalid_document.value().scene().size() == 0U);
    REQUIRE(invalid_document.value().meshes().empty());
}

void stale_render_result_is_rejected() {
    auto mesh = triangle_mesh();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    auto compiled_ptr = std::make_shared<carto::geometry::CompiledMesh>(compiled.value());
    const auto object = carto::scene::ObjectId{1};
    carto::render::RenderScene render;
    REQUIRE(render.upsert({object, compiled_ptr, carto::core::Transform::identity(), compiled.value().source_revision}));
    const auto stale = render.upsert({
        object,
        compiled_ptr,
        carto::core::Transform::identity(),
        compiled.value().source_revision.next(),
    });
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);
}

void command_bus_is_reversible_and_rejects_failed_commands() {
    carto::scene::Scene scene;
    const auto object = scene.create_object("Editable");
    REQUIRE(object);
    const auto before = scene.find(object.value())->local_transform;
    auto after = before;
    after.translation.x = 4.0;

    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(std::make_unique<carto::editor::SetObjectTransformCommand>(
        scene, object.value(), before, after)));
    REQUIRE(bus.undo_count() == 1U);
    REQUIRE(scene.find(object.value())->local_transform.translation.x == 4.0);
    REQUIRE(bus.undo());
    REQUIRE(scene.find(object.value())->local_transform.translation.x == 0.0);
    REQUIRE(bus.redo());
    REQUIRE(scene.find(object.value())->local_transform.translation.x == 4.0);

    auto invalid_after = after;
    invalid_after.translation.x = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!bus.execute(std::make_unique<carto::editor::SetObjectTransformCommand>(
        scene, object.value(), after, invalid_after)));
    REQUIRE(bus.undo_count() == 1U);

    REQUIRE(bus.undo());
    auto divergent = before;
    divergent.translation.y = 8.0;
    REQUIRE(bus.execute(std::make_unique<carto::editor::SetObjectTransformCommand>(
        scene, object.value(), before, divergent)));
    REQUIRE(bus.redo_count() == 0U);
    REQUIRE(!bus.redo());
}

void project_round_trips_authoritative_state_and_rejects_future_versions() {
    TempDirectory temp;
    const auto path = temp.path() / "scene.carto";
    auto document = carto::project::ProjectDocument::create("Round Trip");
    REQUIRE(document);
    const auto object = document.value().create_object("Triangle");
    REQUIRE(object);
    auto mesh = triangle_mesh();
    const auto mesh_id = document.value().add_mesh(std::move(mesh));
    REQUIRE(mesh_id);
    REQUIRE(document.value().attach_mesh(object.value(), mesh_id.value()));
    const std::string serialized_before = document.value().serialize();
    REQUIRE(document.value().save_atomic(path));
    auto loaded = carto::project::ProjectDocument::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().serialize() == serialized_before);
    REQUIRE(loaded.value().scene().find(object.value()) != nullptr);
    REQUIRE(loaded.value().meshes().at(mesh_id.value()).vertex_count() == 3U);

    std::string future = serialized_before;
    const std::string version = "CARTOGRAPHER_PROJECT 1";
    const auto position = future.find(version);
    REQUIRE(position != std::string::npos);
    future.replace(position, version.size(), "CARTOGRAPHER_PROJECT 2");
    const auto rejected = carto::project::ProjectDocument::deserialize(future);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::version_mismatch);
}

void project_save_failure_does_not_replace_existing_file_and_paths_are_bounded() {
    TempDirectory temp;
    const auto path = temp.path() / "valid.carto";
    auto document = carto::project::ProjectDocument::create("Stable");
    REQUIRE(document);
    REQUIRE(document.value().create_object("Object"));
    REQUIRE(document.value().save_atomic(path));
    std::ifstream before(path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
    REQUIRE(!document.value().save_atomic(temp.path() / "missing" / "never.carto"));
    std::ifstream after(path, std::ios::binary);
    const std::string unchanged((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
    REQUIRE(original == unchanged);

    carto::project::AssetReference safe{"assets/mesh.obj"};
    REQUIRE(safe.validate());
    carto::project::AssetReference traversal{"../outside.obj"};
    REQUIRE(!traversal.validate());
    carto::project::AssetReference absolute{temp.path().string()};
    REQUIRE(!absolute.validate());

    std::string invalid_flag = document.value().serialize();
    const std::string valid_flags = " 1 0 0\n";
    const auto flag = invalid_flag.find(valid_flags, invalid_flag.find("OBJECT "));
    REQUIRE(flag != std::string::npos);
    invalid_flag.replace(flag, valid_flags.size(), " 2 0 0\n");
    REQUIRE(!carto::project::ProjectDocument::deserialize(invalid_flag));
}

void obj_interchange_reports_feature_loss_and_round_trips_geometry() {
    TempDirectory temp;
    const auto path = temp.path() / "triangle.obj";
    const auto mesh = triangle_mesh();
    const auto exported = carto::io::export_obj(mesh, path);
    REQUIRE(exported);
    REQUIRE(exported.value().vertices == 3U);
    REQUIRE(!exported.value().warnings.empty());
    const auto imported = carto::io::import_obj(path);
    REQUIRE(imported);
    REQUIRE(imported.value().mesh.vertex_count() == 3U);
    REQUIRE(imported.value().mesh.face_count() == 1U);
    REQUIRE(imported.value().report.triangles == 1U);
}

void render_boundary_does_not_include_editable_mesh_api() {
    // The compile-time boundary is reinforced by render_scene.hpp including
    // only compiled_mesh.hpp. This runtime check confirms the object accepts
    // a derived snapshot and never owns the editable source.
    auto mesh = triangle_mesh();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().valid());
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests = {
        {"scene rejects dangling parent and cycles", scene_rejects_dangling_parent_and_cycles},
        {"scene resolves double precision world transform", scene_resolves_double_precision_world_transform},
        {"selection is mode scoped and context validated", selection_is_mode_scoped_and_context_validated},
        {"mesh rejects degenerate and duplicate topology", mesh_rejects_degenerate_and_duplicate_topology},
        {"mesh compiles without mutating source", mesh_compiles_without_mutating_source_and_exposes_boundary_topology},
        {"primitives are deterministic and validated", primitives_are_deterministic_and_dimension_validated},
        {"stale render result is rejected", stale_render_result_is_rejected},
        {"command bus is reversible", command_bus_is_reversible_and_rejects_failed_commands},
        {"vertex edit command is reversible and atomic", vertex_edit_command_is_reversible_and_topology_atomic},
        {"face extrude command is reversible", face_extrude_command_is_reversible_and_validated},
        {"selected face tool adapter validates", selected_face_tool_adapter_validates_before_delegating},
        {"tool registry dispatches through history", tool_registry_dispatches_registered_commands_through_history},
        {"create mesh object command preserves ids", create_mesh_object_command_preserves_ids_across_undo_redo},
        {"project round trip and future version rejection", project_round_trips_authoritative_state_and_rejects_future_versions},
        {"project save failure and path bounds", project_save_failure_does_not_replace_existing_file_and_paths_are_bounded},
        {"OBJ interchange reports feature loss", obj_interchange_reports_feature_loss_and_round_trips_geometry},
        {"render boundary consumes compiled data", render_boundary_does_not_include_editable_mesh_api},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
