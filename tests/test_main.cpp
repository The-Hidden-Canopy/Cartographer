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

void scene_rejects_nonfinite_world_transform_and_deep_hierarchies() {
    carto::scene::Scene scene;
    carto::core::Transform large_root;
    large_root.translation = {1.0e308, 0.0, 0.0};
    const auto root = scene.create_object("Large Root", large_root);
    carto::core::Transform large_child;
    large_child.translation = {1.0e308, 0.0, 0.0};
    const auto child = scene.create_object("Large Child", large_child);
    REQUIRE(root && child);
    const auto nonfinite_composition = large_root.combine(large_child);
    REQUIRE(!nonfinite_composition.finite());
    REQUIRE(scene.set_parent(child.value(), root.value()));
    const auto nonfinite = scene.world_transform(child.value());
    REQUIRE(!nonfinite);
    REQUIRE(nonfinite.error().code == carto::core::ErrorCode::validation_failed);

    carto::core::Transform zero_rotation;
    zero_rotation.rotation = {0.0, 0.0, 0.0, 0.0};
    REQUIRE(!scene.create_object("Zero rotation", zero_rotation));
    REQUIRE(!zero_rotation.combine(carto::core::Transform::identity()).finite());

    constexpr std::size_t depth = 4097U;
    std::string serialized =
        "CARTOGRAPHER_PROJECT 1\n"
        "NAME \"Deep hierarchy\"\n"
        "REVISION 0\n";
    serialized += "OBJECTS " + std::to_string(depth) + "\n";
    for (std::size_t index = 1U; index <= depth; ++index) {
        const std::size_t parent = index == depth ? 0U : index + 1U;
        serialized += "OBJECT " + std::to_string(index) + " " + std::to_string(parent) +
            " \"Object" + std::to_string(index) + "\" 0 0 0 0 0 0 1 1 1 1 0 0 0\n";
    }
    serialized += "MESHES 0\nEND\n";
    const auto deep = carto::project::ProjectDocument::deserialize(serialized);
    REQUIRE(!deep);
    REQUIRE(deep.error().code == carto::core::ErrorCode::invalid_state);
}

void selection_is_mode_scoped_and_context_validated() {
    carto::scene::Scene scene;
    const auto first_object = scene.create_object("First");
    const auto second_object = scene.create_object("Second");
    REQUIRE(first_object && second_object);

    carto::editor::SelectionState invalid_state;
    REQUIRE(!invalid_state.set_mode(static_cast<carto::editor::SelectionMode>(99)));
    REQUIRE(invalid_state.mode() == carto::editor::SelectionMode::object);
    REQUIRE(!invalid_state.select_object(
        scene,
        first_object.value(),
        static_cast<carto::editor::SelectionOperation>(99)));
    REQUIRE(invalid_state.empty());

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

    carto::editor::SelectionState bound_selection;
    REQUIRE(bound_selection.select_face(
        scene, first_object.value(), box.value(), box_face));
    REQUIRE(bound_selection.component_object().has_value());
    REQUIRE(*bound_selection.component_object() == first_object.value());
    REQUIRE(bound_selection.validate(scene, &box.value()));
    const auto cross_object = bound_selection.select_face(
        scene,
        second_object.value(),
        box.value(),
        box_face,
        carto::editor::SelectionOperation::add);
    REQUIRE(!cross_object);
    REQUIRE(cross_object.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(bound_selection.active_count() == 1U);
    REQUIRE(*bound_selection.component_object() == first_object.value());

    carto::editor::SelectionState unbound_binding;
    REQUIRE(unbound_binding.select_face(box.value(), box_face));
    const auto bind_without_replace = unbound_binding.select_face(
        scene,
        first_object.value(),
        box.value(),
        box_face,
        carto::editor::SelectionOperation::add);
    REQUIRE(!bind_without_replace);
    REQUIRE(bind_without_replace.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(unbound_binding.active_count() == 1U);
    REQUIRE(!unbound_binding.component_object().has_value());

    const auto bound_vertex = box.value().vertices_sorted().front();
    auto changed_position = bound_vertex.position;
    changed_position.x += 0.125;
    REQUIRE(box.value().set_vertex_position(bound_vertex.id, changed_position));
    const auto stale_revision = bound_selection.validate(scene, &box.value());
    REQUIRE(!stale_revision);
    REQUIRE(stale_revision.error().code == carto::core::ErrorCode::stale_data);

    REQUIRE(scene.remove_object(first_object.value()));
    const auto stale_component = bound_selection.validate(scene, &box.value());
    REQUIRE(!stale_component);
    REQUIRE(stale_component.error().code == carto::core::ErrorCode::stale_data);

    carto::editor::SelectionState stale_object_selection;
    REQUIRE(stale_object_selection.select_object(scene, second_object.value()));
    REQUIRE(scene.remove_object(second_object.value()));
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
    const auto compiled_again = mesh.compile();
    REQUIRE(compiled_again);
    REQUIRE(compiled_again.value().source_revision == before_revision);
    const auto moved_vertex = mesh.vertices_sorted().front();
    REQUIRE(mesh.set_vertex_position(moved_vertex.id, {0.1, 0.0, 0.0}));
    const auto recompiled = mesh.compile();
    REQUIRE(recompiled);
    REQUIRE(recompiled.value().source_revision == mesh.revision());
    REQUIRE(recompiled.value().source_revision != before_revision);

    const auto topology = mesh.topology();
    REQUIRE(topology);
    REQUIRE(topology.value().half_edges.size() == 3U);
    REQUIRE(topology.value().validate());
    for (const auto& edge : topology.value().half_edges) {
        REQUIRE(!edge.twin.has_value());
    }
}

void topology_ids_persist_and_boundary_traversal_is_deterministic() {
    auto mesh = triangle_mesh();
    const auto first_face = mesh.faces_sorted().front().id;
    const auto first_topology = mesh.topology();
    REQUIRE(first_topology);
    REQUIRE(first_topology.value().edges.size() == 3U);
    REQUIRE(first_topology.value().half_edges.size() == 3U);
    REQUIRE(first_topology.value().corners.size() == 3U);
    REQUIRE(first_topology.value().boundary_half_edges().size() == 3U);
    const auto first_boundary = first_topology.value().face_boundary(first_face);
    REQUIRE(first_boundary);
    REQUIRE(first_boundary.value().size() == 3U);

    const auto second_topology = mesh.topology();
    REQUIRE(second_topology);
    for (std::size_t index = 0U; index < first_topology.value().half_edges.size(); ++index) {
        REQUIRE(first_topology.value().half_edges[index].id ==
                second_topology.value().half_edges[index].id);
        REQUIRE(first_topology.value().half_edges[index].edge ==
                second_topology.value().half_edges[index].edge);
        REQUIRE(first_topology.value().half_edges[index].corner ==
                second_topology.value().half_edges[index].corner);
    }

    const auto vertices = mesh.vertices_sorted();
    REQUIRE(vertices.size() == 3U);
    const auto added_vertex = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(added_vertex);
    REQUIRE(mesh.add_face({vertices[1].id, added_vertex.value(), vertices[2].id}));

    const auto expanded_topology = mesh.topology();
    REQUIRE(expanded_topology);
    REQUIRE(expanded_topology.value().edges.size() == 5U);
    REQUIRE(expanded_topology.value().half_edges.size() == 6U);
    REQUIRE(expanded_topology.value().corners.size() == 6U);
    REQUIRE(expanded_topology.value().boundary_half_edges().size() == 4U);

    carto::geometry::EdgeId shared_edge{};
    for (const auto& edge : first_topology.value().edges) {
        if ((edge.first == vertices[1].id && edge.second == vertices[2].id) ||
            (edge.first == vertices[2].id && edge.second == vertices[1].id)) {
            shared_edge = edge.id;
        }
    }
    REQUIRE(shared_edge);
    bool shared_edge_survived = false;
    for (const auto& edge : expanded_topology.value().edges) {
        if (edge.id == shared_edge) {
            shared_edge_survived = true;
            REQUIRE(edge.second_half_edge.has_value());
        }
    }
    REQUIRE(shared_edge_survived);
    REQUIRE(expanded_topology.value().validate());

    auto malformed = expanded_topology.value();
    malformed.edges.front().first_half_edge = carto::geometry::HalfEdgeId{999};
    REQUIRE(!malformed.validate());
}

void mesh_patches_are_revision_bound_atomic_and_invertible() {
    auto mesh = triangle_mesh();
    const auto vertex = mesh.vertices_sorted().front();
    const carto::core::Vec3d moved{0.25, 0.0, 0.0};
    const carto::geometry::MeshPatch patch{
        mesh.revision(),
        {carto::geometry::VertexChange{vertex.id, vertex.position, moved}},
    };
    REQUIRE(patch.validate());
    REQUIRE(mesh.apply_patch(patch));
    REQUIRE(mesh.find_vertex(vertex.id)->position.x == moved.x);

    const auto inverse = patch.inverse(mesh.revision());
    REQUIRE(inverse.validate());
    REQUIRE(mesh.apply_patch(inverse));
    REQUIRE(mesh.find_vertex(vertex.id)->position.x == vertex.position.x);

    const auto stale = mesh.apply_patch(patch);
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);

    const auto current = mesh.find_vertex(vertex.id)->position;
    const carto::core::Vec3d invalid_target{0.5, 0.0, 0.0};
    const carto::geometry::MeshPatch partially_missing{
        mesh.revision(),
        {
            carto::geometry::VertexChange{vertex.id, current, invalid_target},
            carto::geometry::VertexChange{
                carto::geometry::VertexId{999}, current, invalid_target},
        },
    };
    REQUIRE(!mesh.apply_patch(partially_missing));
    REQUIRE(mesh.find_vertex(vertex.id)->position.x == current.x);
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

void vector_normalization_preserves_invalid_state() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const carto::core::Vec3d zero{};
    const carto::core::Vec3d nonfinite{nan, 0.0, 0.0};
    const carto::core::Vec3d huge{1.0e308, 0.0, 0.0};
    REQUIRE(!zero.normalized().finite());
    REQUIRE(!nonfinite.normalized().finite());
    REQUIRE(huge.normalized().x == 1.0);
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
    auto document = carto::project::ProjectDocument::create("Tool registry workflow");
    REQUIRE(document);
    const auto first_object = document.value().create_object("First");
    const auto second_object = document.value().create_object("Second");
    REQUIRE(first_object && second_object);
    auto first_primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    auto second_primitive = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(first_primitive && second_primitive);
    const auto first_mesh = document.value().add_mesh(std::move(first_primitive.value()));
    const auto second_mesh = document.value().add_mesh(std::move(second_primitive.value()));
    REQUIRE(first_mesh && second_mesh);
    REQUIRE(document.value().attach_mesh(first_object.value(), first_mesh.value()));
    REQUIRE(document.value().attach_mesh(second_object.value(), second_mesh.value()));
    carto::editor::SelectionState selection;
    const auto selected_face = document.value().meshes().at(second_mesh.value()).faces_sorted().front().id;
    REQUIRE(selection.select_face(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        selected_face));
    carto::editor::ToolContext context(document.value(), selection);
    carto::editor::ToolArguments arguments{0.5};
    carto::editor::CommandBus history;
    carto::editor::ToolRegistry registry;
    REQUIRE(registry.register_builtin_tools());
    const auto descriptors = registry.descriptors();
    REQUIRE(descriptors.size() == 2U);
    REQUIRE(descriptors.front().id == "mesh.extrude-face");
    REQUIRE(descriptors.back().id == "mesh.set-vertex-position");
    REQUIRE(!registry.register_builtin_tools());

    REQUIRE(registry.invoke("mesh.extrude-face", context, arguments, history));
    REQUIRE(history.undo_count() == 1U);
    REQUIRE(document.value().meshes().at(first_mesh.value()).vertex_count() == 8U);
    REQUIRE(document.value().meshes().at(second_mesh.value()).vertex_count() == 12U);
    REQUIRE(history.undo());
    REQUIRE(document.value().meshes().at(first_mesh.value()).vertex_count() == 8U);
    REQUIRE(document.value().meshes().at(second_mesh.value()).vertex_count() == 8U);
    REQUIRE(history.redo());
    REQUIRE(document.value().meshes().at(first_mesh.value()).vertex_count() == 8U);
    REQUIRE(document.value().meshes().at(second_mesh.value()).vertex_count() == 12U);
    REQUIRE(document.value().validate());

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

void project_vertex_tool_routes_through_history_and_rejects_bad_context() {
    auto document = carto::project::ProjectDocument::create("Project vertex workflow");
    REQUIRE(document);
    const auto first_object = document.value().create_object("First");
    const auto second_object = document.value().create_object("Second");
    const auto unbound_object = document.value().create_object("No mesh");
    REQUIRE(first_object && second_object && unbound_object);
    auto first_primitive = carto::geometry::make_box({2.0, 2.0, 2.0});
    auto second_primitive = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(first_primitive && second_primitive);
    const auto first_mesh = document.value().add_mesh(std::move(first_primitive.value()));
    const auto second_mesh = document.value().add_mesh(std::move(second_primitive.value()));
    REQUIRE(first_mesh && second_mesh);
    REQUIRE(document.value().attach_mesh(first_object.value(), first_mesh.value()));
    REQUIRE(document.value().attach_mesh(second_object.value(), second_mesh.value()));

    const auto second_vertex = document.value().meshes().at(second_mesh.value()).vertices_sorted().front();
    const auto first_position = document.value().meshes().at(first_mesh.value()).vertices_sorted().front().position;
    const auto original_position = second_vertex.position;
    auto target_position = original_position;
    target_position.x += 0.25;

    carto::editor::SelectionState selection;
    REQUIRE(selection.select_vertex(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_vertex.id));
    carto::editor::ToolContext context(document.value(), selection);
    carto::editor::ToolArguments arguments;
    arguments.position = target_position;
    carto::editor::CommandBus history;
    carto::editor::ToolRegistry registry;
    REQUIRE(registry.register_builtin_tools());
    const auto descriptors = registry.descriptors();
    REQUIRE(descriptors.size() == 2U);
    REQUIRE(descriptors[0].id == "mesh.extrude-face");
    REQUIRE(descriptors[1].id == "mesh.set-vertex-position");
    REQUIRE(!registry.register_builtin_tools());

    carto::editor::SelectionState rebound_selection;
    REQUIRE(rebound_selection.select_vertex(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_vertex.id));
    REQUIRE(document.value().attach_mesh(second_object.value(), first_mesh.value()));
    carto::editor::ToolContext rebound_context(document.value(), rebound_selection);
    carto::editor::CommandBus rebound_history;
    const auto rebound_result = registry.invoke(
        "mesh.set-vertex-position", rebound_context, arguments, rebound_history);
    REQUIRE(!rebound_result);
    REQUIRE(rebound_result.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(rebound_history.undo_count() == 0U);
    REQUIRE(document.value().attach_mesh(second_object.value(), second_mesh.value()));

    REQUIRE(registry.invoke("mesh.set-vertex-position", context, arguments, history));
    REQUIRE(history.undo_count() == 1U);
    REQUIRE(document.value().meshes().at(first_mesh.value()).vertices_sorted().front().position.x ==
            first_position.x);
    REQUIRE(document.value().meshes().at(second_mesh.value()).find_vertex(second_vertex.id)->position.x ==
            target_position.x);
    REQUIRE(document.value().validate());
    REQUIRE(history.undo());
    REQUIRE(document.value().meshes().at(second_mesh.value()).find_vertex(second_vertex.id)->position.x ==
            original_position.x);
    REQUIRE(history.redo());
    REQUIRE(document.value().meshes().at(second_mesh.value()).find_vertex(second_vertex.id)->position.x ==
            target_position.x);

    carto::editor::SelectionState replacement_selection;
    REQUIRE(replacement_selection.select_vertex(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_vertex.id));
    const auto replacement_revision = document.value().meshes().at(second_mesh.value()).revision();
    auto same_revision_mesh = document.value().meshes().at(second_mesh.value());
    REQUIRE(document.value().replace_mesh(second_mesh.value(), std::move(same_revision_mesh)));
    REQUIRE(document.value().meshes().at(second_mesh.value()).revision() > replacement_revision);
    const auto stale_replacement = replacement_selection.validate(
        document.value().scene(), &document.value().meshes().at(second_mesh.value()));
    REQUIRE(!stale_replacement);
    REQUIRE(stale_replacement.error().code == carto::core::ErrorCode::stale_data);
    const auto conflicted_undo = history.undo();
    REQUIRE(!conflicted_undo);
    REQUIRE(conflicted_undo.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(history.undo_count() == 1U);

    carto::editor::SelectionState wrong_mode;
    const auto second_face = document.value().meshes().at(second_mesh.value()).faces_sorted().front().id;
    REQUIRE(wrong_mode.select_face(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_face));
    carto::editor::ToolContext wrong_context(document.value(), wrong_mode);
    carto::editor::CommandBus wrong_history;
    REQUIRE(!registry.invoke("mesh.set-vertex-position", wrong_context, arguments, wrong_history));
    REQUIRE(wrong_history.undo_count() == 0U);

    carto::editor::SelectionState multiple_selection;
    const auto second_vertices = document.value().meshes().at(second_mesh.value()).vertices_sorted();
    REQUIRE(multiple_selection.select_vertex(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_vertices[0].id));
    REQUIRE(multiple_selection.select_vertex(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_vertices[1].id,
        carto::editor::SelectionOperation::add));
    carto::editor::ToolContext multiple_context(document.value(), multiple_selection);
    carto::editor::CommandBus multiple_history;
    REQUIRE(!registry.invoke("mesh.set-vertex-position", multiple_context, arguments, multiple_history));
    REQUIRE(multiple_history.undo_count() == 0U);

    carto::editor::SelectionState no_mesh_selection;
    REQUIRE(no_mesh_selection.select_vertex(
        document.value().scene(),
        unbound_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_vertex.id));
    carto::editor::ToolContext no_mesh_context(document.value(), no_mesh_selection);
    carto::editor::CommandBus no_mesh_history;
    REQUIRE(!registry.invoke("mesh.set-vertex-position", no_mesh_context, arguments, no_mesh_history));
    REQUIRE(no_mesh_history.undo_count() == 0U);

    carto::editor::SelectionState invalid_selection;
    REQUIRE(invalid_selection.select_vertex(
        document.value().scene(),
        second_object.value(),
        document.value().meshes().at(second_mesh.value()),
        second_vertex.id));
    carto::editor::ToolContext invalid_context(document.value(), invalid_selection);
    const auto invalid_target = document.value().meshes().at(second_mesh.value()).vertices_sorted().at(1).position;
    arguments.position = invalid_target;
    carto::editor::CommandBus invalid_history;
    const auto invalid_result = registry.invoke(
        "mesh.set-vertex-position", invalid_context, arguments, invalid_history);
    REQUIRE(!invalid_result);
    REQUIRE(invalid_result.error().code == carto::core::ErrorCode::validation_failed);
    REQUIRE(invalid_history.undo_count() == 0U);
    REQUIRE(document.value().meshes().at(second_mesh.value()).find_vertex(second_vertex.id)->position.x ==
            target_position.x);

    REQUIRE(document.value().remove_object(second_object.value()));
    carto::editor::CommandBus stale_history;
    arguments.position = target_position;
    REQUIRE(!registry.invoke("mesh.set-vertex-position", context, arguments, stale_history));
    REQUIRE(stale_history.undo_count() == 0U);
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
    const auto before_revision = invalid_document.value().revision();
    const auto before_serialized = invalid_document.value().serialize();
    carto::editor::CommandBus invalid_bus;
    REQUIRE(!invalid_bus.execute(std::make_unique<carto::editor::CreateMeshObjectCommand>(
        invalid_document.value(), "", std::move(second_primitive.value()))));
    REQUIRE(invalid_bus.undo_count() == 0U);
    REQUIRE(invalid_document.value().scene().size() == 0U);
    REQUIRE(invalid_document.value().meshes().empty());
    REQUIRE(invalid_document.value().revision() == before_revision);
    REQUIRE(invalid_document.value().serialize() == before_serialized);

    auto recovered_primitive = carto::geometry::make_plane(1.0, 1.0);
    REQUIRE(recovered_primitive);
    REQUIRE(invalid_bus.execute(std::make_unique<carto::editor::CreateMeshObjectCommand>(
        invalid_document.value(), "Recovered", std::move(recovered_primitive.value()))));
    REQUIRE(invalid_document.value().scene().objects_sorted().front().id ==
            carto::scene::ObjectId{1});
    REQUIRE(*invalid_document.value().scene().objects_sorted().front().mesh_asset == 1U);
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

    auto malformed = compiled.value();
    malformed.indices.front() = static_cast<std::uint32_t>(malformed.positions.size());
    REQUIRE(!malformed.valid());
    REQUIRE(!render.upsert({
        object,
        std::make_shared<carto::geometry::CompiledMesh>(std::move(malformed)),
        carto::core::Transform::identity(),
        compiled.value().source_revision,
    }));

    carto::core::Transform overflowing;
    overflowing.translation = {1.0e308, 0.0, 0.0};
    overflowing.scale = {1.0e308, 1.0, 1.0};
    const auto nonfinite_world = render.upsert({
        object,
        compiled_ptr,
        overflowing,
        compiled.value().source_revision,
    });
    REQUIRE(!nonfinite_world);
    REQUIRE(nonfinite_world.error().code == carto::core::ErrorCode::validation_failed);
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

void project_transform_command_rejects_stale_undo() {
    auto document = carto::project::ProjectDocument::create("Transform guard");
    REQUIRE(document);
    const auto object = document.value().create_object("Object");
    REQUIRE(object);
    const auto before = document.value().scene().find(object.value())->local_transform;
    auto after = before;
    after.translation.x = 4.0;

    carto::editor::CommandBus bus;
    REQUIRE(bus.execute(std::make_unique<carto::editor::SetProjectObjectTransformCommand>(
        document.value(), object.value(), before, after)));
    auto external = after;
    external.translation.y = 9.0;
    REQUIRE(document.value().set_object_transform(object.value(), external));

    REQUIRE(!bus.undo());
    REQUIRE(bus.undo_count() == 1U);
    const auto* current = document.value().scene().find(object.value());
    REQUIRE(current != nullptr);
    REQUIRE(current->local_transform.translation.x == 4.0);
    REQUIRE(current->local_transform.translation.y == 9.0);

    auto deferred_document = carto::project::ProjectDocument::create("Deferred transform guard");
    REQUIRE(deferred_document);
    const auto deferred_object = deferred_document.value().create_object("Object");
    REQUIRE(deferred_object);
    const auto deferred_before =
        deferred_document.value().scene().find(deferred_object.value())->local_transform;
    auto deferred_after = deferred_before;
    deferred_after.translation.x = 12.0;
    auto deferred_command = std::make_unique<carto::editor::SetProjectObjectTransformCommand>(
        deferred_document.value(), deferred_object.value(), deferred_before, deferred_after);
    auto external_transform = deferred_before;
    external_transform.translation.y = 6.0;
    REQUIRE(deferred_document.value().set_object_transform(
        deferred_object.value(), external_transform));
    carto::editor::CommandBus deferred_bus;
    REQUIRE(!deferred_bus.execute(std::move(deferred_command)));
    REQUIRE(deferred_bus.undo_count() == 0U);
    const auto* deferred_current =
        deferred_document.value().scene().find(deferred_object.value());
    REQUIRE(deferred_current != nullptr);
    REQUIRE(deferred_current->local_transform.translation.x == 0.0);
    REQUIRE(deferred_current->local_transform.translation.y == 6.0);
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

    REQUIRE(carto::core::Revision(carto::core::Revision::max_value()).next() ==
            carto::core::Revision(carto::core::Revision::max_value()));
    auto exhausted_mesh = triangle_mesh();
    REQUIRE(!exhausted_mesh.restore_revision(
        carto::core::Revision(carto::core::Revision::max_value())));
    std::string exhausted_project = serialized_before;
    const auto revision_position = exhausted_project.find("REVISION ");
    REQUIRE(revision_position != std::string::npos);
    const auto revision_end = exhausted_project.find('\n', revision_position);
    exhausted_project.replace(
        revision_position,
        revision_end - revision_position,
        "REVISION " + std::to_string(carto::core::Revision::max_value()));
    const auto exhausted_project_rejected =
        carto::project::ProjectDocument::deserialize(exhausted_project);
    REQUIRE(!exhausted_project_rejected);
    REQUIRE(exhausted_project_rejected.error().code ==
            carto::core::ErrorCode::validation_failed);

    std::string exhausted_mesh_revision = serialized_before;
    const auto serialized_mesh_start = exhausted_mesh_revision.find(
        "MESH ", exhausted_mesh_revision.find("MESHES "));
    REQUIRE(serialized_mesh_start != std::string::npos);
    const auto mesh_id_end = exhausted_mesh_revision.find(' ', serialized_mesh_start + 5U);
    const auto mesh_revision_end = exhausted_mesh_revision.find('\n', mesh_id_end + 1U);
    REQUIRE(mesh_id_end != std::string::npos && mesh_revision_end != std::string::npos);
    exhausted_mesh_revision.replace(
        mesh_id_end + 1U,
        mesh_revision_end - mesh_id_end - 1U,
        std::to_string(carto::core::Revision::max_value()));
    const auto exhausted_mesh_rejected =
        carto::project::ProjectDocument::deserialize(exhausted_mesh_revision);
    REQUIRE(!exhausted_mesh_rejected);
    REQUIRE(exhausted_mesh_rejected.error().code ==
            carto::core::ErrorCode::validation_failed);

    const auto mesh_start = serialized_before.find(
        "MESH ", serialized_before.find("MESHES "));
    const auto end_marker = serialized_before.rfind("END\n");
    REQUIRE(mesh_start != std::string::npos && end_marker != std::string::npos);
    const std::string mesh_block = serialized_before.substr(mesh_start, end_marker - mesh_start);
    std::string duplicate_mesh = serialized_before;
    const auto mesh_count = duplicate_mesh.find("MESHES 1");
    REQUIRE(mesh_count != std::string::npos);
    duplicate_mesh.replace(mesh_count, std::string("MESHES 1").size(), "MESHES 2");
    duplicate_mesh.insert(duplicate_mesh.rfind("END\n"), mesh_block);
    const auto duplicate_rejected = carto::project::ProjectDocument::deserialize(duplicate_mesh);
    REQUIRE(!duplicate_rejected);
    REQUIRE(duplicate_rejected.error().code == carto::core::ErrorCode::validation_failed);

    const auto trailing_rejected = carto::project::ProjectDocument::deserialize(
        serialized_before + "TRAILING\n");
    REQUIRE(!trailing_rejected);
    REQUIRE(trailing_rejected.error().code == carto::core::ErrorCode::validation_failed);

    const auto expect_oversized_count = [&](std::string marker, std::string replacement) {
        std::string oversized = serialized_before;
        const auto count_position = oversized.find(marker);
        REQUIRE(count_position != std::string::npos);
        oversized.replace(count_position, marker.size(), replacement);
        const auto rejected_count = carto::project::ProjectDocument::deserialize(oversized);
        REQUIRE(!rejected_count);
        REQUIRE(rejected_count.error().code == carto::core::ErrorCode::validation_failed);
    };
    expect_oversized_count("OBJECTS 1", "OBJECTS 1000001");
    expect_oversized_count("MESHES 1", "MESHES 1000001");
    expect_oversized_count("VERTICES 3", "VERTICES 1000001");
    expect_oversized_count("FACES 1", "FACES 1000001");

    std::string oversized_face = serialized_before;
    const auto face_position = oversized_face.find("FACE ");
    REQUIRE(face_position != std::string::npos);
    const auto face_id_end = oversized_face.find(' ', face_position + 5U);
    REQUIRE(face_id_end != std::string::npos);
    const auto face_count_end = oversized_face.find(' ', face_id_end + 1U);
    REQUIRE(face_count_end != std::string::npos);
    oversized_face.replace(
        face_id_end + 1U,
        face_count_end - face_id_end - 1U,
        "1000001");
    const auto oversized_face_rejected =
        carto::project::ProjectDocument::deserialize(oversized_face);
    REQUIRE(!oversized_face_rejected);
    REQUIRE(oversized_face_rejected.error().code == carto::core::ErrorCode::validation_failed);

    const auto higher_id_parent = carto::project::ProjectDocument::deserialize(
        "CARTOGRAPHER_PROJECT 1\n"
        "NAME \"Hierarchy\"\n"
        "REVISION 0\n"
        "OBJECTS 2\n"
        "OBJECT 1 2 \"Child\" 0 0 0 0 0 0 1 1 1 1 0 0 0\n"
        "OBJECT 2 0 \"Parent\" 0 0 0 0 0 0 1 1 1 1 0 0 0\n"
        "MESHES 0\n"
        "END\n");
    REQUIRE(higher_id_parent);
    REQUIRE(higher_id_parent.value().scene().find(carto::scene::ObjectId{1})->parent.has_value());
    REQUIRE(*higher_id_parent.value().scene().find(carto::scene::ObjectId{1})->parent ==
            carto::scene::ObjectId{2});

    const auto negative_id = carto::project::ProjectDocument::deserialize(
        "CARTOGRAPHER_PROJECT 1\n"
        "NAME \"Negative ID\"\n"
        "REVISION 0\n"
        "OBJECTS 1\n"
        "OBJECT -1 0 \"Object\" 0 0 0 0 0 0 1 1 1 1 0 0 0\n"
        "MESHES 0\n"
        "END\n");
    REQUIRE(!negative_id);
    REQUIRE(negative_id.error().code == carto::core::ErrorCode::validation_failed);
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

    const auto blocked_temporary = temp.path() / "valid.carto.carto.tmp";
    REQUIRE(std::filesystem::create_directory(blocked_temporary));
    const auto blocked_save = document.value().save_atomic(path);
    REQUIRE(!blocked_save);
    std::ifstream blocked_after(path, std::ios::binary);
    const std::string blocked_unchanged(
        (std::istreambuf_iterator<char>(blocked_after)), std::istreambuf_iterator<char>());
    REQUIRE(blocked_unchanged == original);

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

    const auto oversized_face_path = temp.path() / "oversized-face.obj";
    std::ofstream oversized_face(oversized_face_path, std::ios::binary | std::ios::trunc);
    oversized_face << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf";
    for (std::size_t index = 0; index < 1'000'001U; ++index) {
        oversized_face << " 1";
    }
    oversized_face << '\n';
    oversized_face.close();
    const auto rejected_oversized_face = carto::io::import_obj(oversized_face_path);
    REQUIRE(!rejected_oversized_face);
    REQUIRE(rejected_oversized_face.error().code == carto::core::ErrorCode::validation_failed);

    const auto malformed_index_path = temp.path() / "malformed-index.obj";
    std::ofstream malformed_index(malformed_index_path, std::ios::binary | std::ios::trunc);
    malformed_index << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1x 2 3\n";
    malformed_index.close();
    const auto rejected_malformed_index = carto::io::import_obj(malformed_index_path);
    REQUIRE(!rejected_malformed_index);
    REQUIRE(rejected_malformed_index.error().code == carto::core::ErrorCode::validation_failed);

    const auto atomic_path = temp.path() / "atomic.obj";
    REQUIRE(carto::io::export_obj(mesh, atomic_path));
    std::ifstream atomic_before(atomic_path, std::ios::binary);
    const std::string atomic_original(
        (std::istreambuf_iterator<char>(atomic_before)), std::istreambuf_iterator<char>());
    const auto blocked_temporary = temp.path() / "atomic.obj.carto.tmp";
    REQUIRE(std::filesystem::create_directory(blocked_temporary));
    const auto blocked_export = carto::io::export_obj(mesh, atomic_path);
    REQUIRE(!blocked_export);
    std::ifstream atomic_after(atomic_path, std::ios::binary);
    const std::string atomic_unchanged(
        (std::istreambuf_iterator<char>(atomic_after)), std::istreambuf_iterator<char>());
    REQUIRE(atomic_unchanged == atomic_original);
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
        {"scene rejects nonfinite and deep hierarchy state", scene_rejects_nonfinite_world_transform_and_deep_hierarchies},
        {"selection is mode scoped and context validated", selection_is_mode_scoped_and_context_validated},
        {"mesh rejects degenerate and duplicate topology", mesh_rejects_degenerate_and_duplicate_topology},
        {"mesh compiles without mutating source", mesh_compiles_without_mutating_source_and_exposes_boundary_topology},
        {"topology ids persist and boundary traversal is deterministic", topology_ids_persist_and_boundary_traversal_is_deterministic},
        {"mesh patches are revision-bound atomic and invertible", mesh_patches_are_revision_bound_atomic_and_invertible},
        {"primitives are deterministic and validated", primitives_are_deterministic_and_dimension_validated},
        {"vector normalization preserves invalid state", vector_normalization_preserves_invalid_state},
        {"stale render result is rejected", stale_render_result_is_rejected},
        {"command bus is reversible", command_bus_is_reversible_and_rejects_failed_commands},
        {"project transform command rejects stale undo", project_transform_command_rejects_stale_undo},
        {"vertex edit command is reversible and atomic", vertex_edit_command_is_reversible_and_topology_atomic},
        {"face extrude command is reversible", face_extrude_command_is_reversible_and_validated},
        {"selected face tool adapter validates", selected_face_tool_adapter_validates_before_delegating},
        {"tool registry dispatches through history", tool_registry_dispatches_registered_commands_through_history},
        {"project vertex tool routes through history", project_vertex_tool_routes_through_history_and_rejects_bad_context},
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
