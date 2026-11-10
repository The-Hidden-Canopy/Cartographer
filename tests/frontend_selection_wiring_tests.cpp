// Expose application state for read-only assertions in this test translation
// unit; every exercised operation still uses the public UI entry point.
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

#include "project_document_access.hpp"
#include "../apps/desktop/studio_edge_tools.hpp"
#include "../apps/desktop/studio_face_tools.hpp"
#include "../apps/desktop/studio_vertex_tools.hpp"
#include <carto/ui/ui.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

carto::geometry::EditableMesh quad() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value(), d.value()}));
    return mesh;
}

carto::geometry::EditableMesh tetrahedron() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({2.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 2.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 0.0, 2.0});
    REQUIRE(a && b && c && d);
    REQUIRE(mesh.add_face({a.value(), c.value(), b.value()}));
    REQUIRE(mesh.add_face({a.value(), b.value(), d.value()}));
    REQUIRE(mesh.add_face({a.value(), d.value(), c.value()}));
    REQUIRE(mesh.add_face({b.value(), c.value(), d.value()}));
    return mesh;
}

struct Truth {
    carto::application::ApplicationSnapshot snapshot;
    std::string document;
    std::string journal;
    std::size_t events = 0U;
    std::size_t operations = 0U;
};

class Fixture {
public:
    Fixture() : ui(session) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::current_path() /
            ("cartographer-frontend-selection-" + std::to_string(stamp));
        std::filesystem::create_directories(directory);
        path = directory / "selection.carto";
        REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Selection wiring", false}));
        REQUIRE(ui.dispatch(carto::application::CreateMeshObjectAction{
            "Quad", quad(), std::nullopt, std::nullopt}));
        REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
        source = session.snapshot().objects.front().object.id;
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    Truth truth() const {
        carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
        const auto events = journal.read_all();
        REQUIRE(events);
        return {session.snapshot(), session.document_.serialize(), read_bytes(journal.path()),
            events.value().size(), ui.snapshot().operations.size()};
    }
    void unchanged(const Truth& before, bool selection_unchanged = false) const {
        const auto after = truth();
        REQUIRE(after.document == before.document && after.journal == before.journal);
        REQUIRE(after.events == before.events && after.operations == before.operations);
        REQUIRE(after.snapshot.project_revision == before.snapshot.project_revision);
        REQUIRE(after.snapshot.undo_count == before.snapshot.undo_count);
        REQUIRE(after.snapshot.redo_count == before.snapshot.redo_count);
        REQUIRE(after.snapshot.dirty == before.snapshot.dirty);
        if (selection_unchanged) {
            REQUIRE(after.snapshot.selection.mode == before.snapshot.selection.mode);
            REQUIRE(after.snapshot.selection.objects == before.snapshot.selection.objects);
            REQUIRE(after.snapshot.selection.component_groups ==
                before.snapshot.selection.component_groups);
        }
    }
    carto::core::Result<carto::application::DispatchReceipt> run(
        std::string_view command, carto::geometry::VertexId goal = {}) {
        const auto current = session.snapshot();
        return ui.run_selection_command(command, current.project_generation,
            current.project_revision, goal, goal ? std::optional{source} : std::nullopt);
    }
    void select_vertex(std::uint64_t id) {
        REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
            source, carto::geometry::VertexId{id}, carto::editor::SelectionOperation::replace}));
    }
    bool offered(std::string_view id) const {
        const auto commands = ui.snapshot().commands;
        const auto command = std::find_if(commands.begin(), commands.end(),
            [id](const auto& candidate) { return candidate.id == id; });
        REQUIRE(command != commands.end());
        return command->supported;
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui;
    std::filesystem::path directory;
    std::filesystem::path path;
    carto::scene::ObjectId source;
};

void shared_selection_commands_convert_and_expand_without_authoring() {
    using namespace carto;
    Fixture fixture;
    REQUIRE(!fixture.offered(ui::selection_commands::to_vertex));
    REQUIRE(fixture.ui.dispatch(application::SelectFaceAction{
        fixture.source, geometry::FaceId{1U}, editor::SelectionOperation::replace}));
    REQUIRE(fixture.offered(ui::selection_commands::to_edge));
    REQUIRE(!fixture.offered(ui::selection_commands::loop));
    auto before = fixture.truth();
    const auto converted = fixture.run(ui::selection_commands::to_edge);
    REQUIRE(converted && !converted.value().document_changed);
    REQUIRE(fixture.session.snapshot().selection.edges.size() == 4U);
    REQUIRE(fixture.offered(ui::selection_commands::loop));
    fixture.unchanged(before);
    REQUIRE(fixture.run(ui::selection_commands::to_vertex));
    REQUIRE(fixture.session.snapshot().selection.vertices.size() == 4U);
    REQUIRE(!fixture.offered(ui::selection_commands::boundary));
    fixture.unchanged(before);

    fixture.select_vertex(1U);
    before = fixture.truth();
    REQUIRE(fixture.run(ui::selection_commands::grow));
    REQUIRE((fixture.session.snapshot().selection.vertices ==
        std::vector<geometry::VertexId>{{1U}, {2U}, {4U}}));
    fixture.unchanged(before);
    REQUIRE(fixture.run(ui::selection_commands::linked));
    REQUIRE(fixture.session.snapshot().selection.vertices.size() == 4U);
    fixture.unchanged(before);

    // Numeric mode shortcuts continue to clear selection instead of converting.
    REQUIRE(fixture.ui.handle_shortcut(ui::Shortcut::face_mode));
    REQUIRE(fixture.session.snapshot().selection.faces.empty());
    REQUIRE(fixture.session.snapshot().selection.vertices.empty());
    fixture.unchanged(before);
}

void shared_shortest_path_keeps_backend_ties_and_refuses_bad_context() {
    using namespace carto;
    Fixture fixture;
    auto before = fixture.truth();
    REQUIRE(!fixture.run(ui::selection_commands::grow));
    fixture.unchanged(before, true);
    REQUIRE(!fixture.run(ui::selection_commands::shortest_path, geometry::VertexId{3U}));
    fixture.unchanged(before, true);
    REQUIRE(fixture.ui.dispatch(application::SelectFaceAction{
        fixture.source, geometry::FaceId{1U}, editor::SelectionOperation::replace}));
    before = fixture.truth();
    REQUIRE(!fixture.run(ui::selection_commands::shortest_path, geometry::VertexId{3U}));
    fixture.unchanged(before, true);
    fixture.select_vertex(1U);
    before = fixture.truth();
    REQUIRE(!fixture.run(ui::selection_commands::shortest_path, geometry::VertexId{999U}));
    fixture.unchanged(before, true);
    REQUIRE(!fixture.run(ui::selection_commands::shortest_path));
    fixture.unchanged(before, true);
    REQUIRE(!fixture.run("selection.unknown"));
    fixture.unchanged(before, true);
    REQUIRE(fixture.offered(ui::selection_commands::shortest_path));
    REQUIRE(fixture.run(ui::selection_commands::shortest_path, geometry::VertexId{3U}));
    const auto path = fixture.session.snapshot().selection.component_groups.front();
    // Opposite corners have two equal-hop routes; the backend's stable-ID tie
    // chooses vertex 2, and the UI must preserve that exact ordered result.
    REQUIRE((path.vertex_selection_order ==
        std::vector<geometry::VertexId>{{1U}, {2U}, {3U}}));
    fixture.unchanged(before);

    const auto captured = fixture.session.snapshot();
    REQUIRE(fixture.ui.dispatch(application::CreateBoxAction{"Later edit", {1.0, 1.0, 1.0}}));
    before = fixture.truth();
    const auto stale_revision = fixture.ui.run_selection_command(ui::selection_commands::grow,
        captured.project_generation, captured.project_revision);
    REQUIRE(!stale_revision && stale_revision.error().code == core::ErrorCode::stale_data);
    fixture.unchanged(before, true);
    const auto stale_generation = fixture.ui.run_selection_command(ui::selection_commands::grow,
        before.snapshot.project_generation + 1U, before.snapshot.project_revision);
    REQUIRE(!stale_generation && stale_generation.error().code == core::ErrorCode::stale_data);
    fixture.unchanged(before, true);
}

void shared_batch_duplicate_is_bounded_journaled_and_undoable() {
    using namespace carto;
    Fixture fixture;
    REQUIRE(fixture.ui.dispatch(application::SelectObjectAction{fixture.source,
        editor::SelectionOperation::replace}));
    REQUIRE(fixture.offered("edit.duplicate-batch"));
    const auto before = fixture.truth();
    const auto duplicate = [&](std::uint32_t copies, core::Vec3d step) {
        return fixture.ui.duplicate_mesh_instances(fixture.source, copies, step,
            before.snapshot.project_generation, before.snapshot.project_revision);
    };
    REQUIRE(!duplicate(0U, {1.0, 0.0, 0.0}));
    fixture.unchanged(before, true);
    REQUIRE(!duplicate(static_cast<std::uint32_t>(editor::kMaxMeshInstanceBatchInstances + 1U),
        {1.0, 0.0, 0.0}));
    fixture.unchanged(before, true);
    REQUIRE(!duplicate(2U, {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}));
    fixture.unchanged(before, true);
    REQUIRE(!duplicate(2U, {editor::kMaxMeshInstanceBatchOffsetMagnitude, 0.0, 0.0}));
    fixture.unchanged(before, true);
    const auto stale = fixture.ui.duplicate_mesh_instances(fixture.source, 2U, {1.0, 0.0, 0.0},
        before.snapshot.project_generation + 1U, before.snapshot.project_revision);
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    fixture.unchanged(before, true);

    const auto created = duplicate(3U, {1.0, 0.5, -1.0});
    REQUIRE(created && created.value().document_changed);
    const auto after = fixture.truth();
    REQUIRE(after.snapshot.objects.size() == 4U);
    REQUIRE(after.snapshot.undo_count == before.snapshot.undo_count + 1U);
    REQUIRE(after.operations == before.operations + 1U && after.events == before.events + 1U);
    for (std::size_t index = 1U; index < after.snapshot.objects.size(); ++index) {
        const auto& object = after.snapshot.objects[index].object;
        REQUIRE(object.mesh_asset == after.snapshot.objects.front().object.mesh_asset);
        REQUIRE(object.local_transform.translation.x == static_cast<double>(index));
        REQUIRE(object.local_transform.translation.y == static_cast<double>(index) * 0.5);
        REQUIRE(object.local_transform.translation.z == -static_cast<double>(index));
    }
    REQUIRE(fixture.ui.dispatch(application::UndoAction{}));
    REQUIRE(fixture.session.snapshot().objects.size() == 1U);
    REQUIRE(fixture.ui.dispatch(application::RedoAction{}));
    REQUIRE(fixture.session.snapshot().objects.size() == 4U);
    REQUIRE(fixture.ui.dispatch(application::SaveProjectAction{fixture.path}));
    const auto reopened = project::ProjectDocument::load(fixture.path);
    REQUIRE(reopened && reopened.value().scene().size() == 4U);
    fixture.select_vertex(1U);
    const auto wrong_mode = fixture.truth();
    REQUIRE(!fixture.ui.duplicate_mesh_instances(fixture.source, 2U, {1.0, 0.0, 0.0},
        wrong_mode.snapshot.project_generation, wrong_mode.snapshot.project_revision));
    fixture.unchanged(wrong_mode, true);
}

void batch_availability_matches_refusal_for_ineligible_authored_sources() {
    using namespace carto;
    enum class SourceKind { locked, hidden, parented, nonmesh };
    for (const auto kind : {SourceKind::locked, SourceKind::hidden,
             SourceKind::parented, SourceKind::nonmesh}) {
        Fixture fixture;
        auto authored = project::ProjectDocument::create("Batch eligibility fixture");
        REQUIRE(authored);
        auto author = project::testing::access(authored.value());
        scene::SceneObject source;
        source.id = scene::ObjectId{2U};
        source.name = "Ineligible source";
        source.locked = kind == SourceKind::locked;
        source.visible = kind != SourceKind::hidden;
        if (kind != SourceKind::nonmesh) {
            const auto mesh = author.add_mesh(quad());
            REQUIRE(mesh);
            source.mesh_asset = mesh.value();
        }
        if (kind == SourceKind::parented) {
            scene::SceneObject parent;
            parent.id = scene::ObjectId{1U};
            parent.name = "Parent";
            REQUIRE(author.insert_object(parent));
            source.parent = parent.id;
        }
        REQUIRE(author.insert_object(source));
        REQUIRE(authored.value().validate());
        fixture.path = fixture.directory / "ineligible.carto";
        REQUIRE(authored.value().save_atomic(fixture.path));
        // Admit the authored fixture through the normal application open path;
        // do not patch the live session to manufacture its eligibility state.
        REQUIRE(fixture.ui.dispatch(application::OpenProjectAction{fixture.path, true}));
        fixture.source = source.id;
        REQUIRE(fixture.ui.dispatch(application::SelectObjectAction{
            fixture.source, editor::SelectionOperation::replace}));
        const auto before = fixture.truth();
        REQUIRE(before.snapshot.selection.objects.size() == 1U);
        REQUIRE(!fixture.offered("edit.duplicate-batch"));
        const auto rejected = fixture.ui.duplicate_mesh_instances(
            fixture.source, 2U, {1.0, 0.0, 0.0},
            before.snapshot.project_generation, before.snapshot.project_revision);
        REQUIRE(!rejected && rejected.error().code == core::ErrorCode::invalid_state);
        fixture.unchanged(before, true);
    }
}

void multi_edge_tool_controls_match_authoring_commands() {
    using namespace carto;
    Fixture fixture;
    geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto center = mesh.add_vertex({0.5, 0.5, 0.0});
    REQUIRE(a && b && c && d && center);
    REQUIRE(mesh.add_face({a.value(), b.value(), center.value()}));
    REQUIRE(mesh.add_face({b.value(), c.value(), center.value()}));
    REQUIRE(mesh.add_face({c.value(), d.value(), center.value()}));
    REQUIRE(mesh.add_face({d.value(), a.value(), center.value()}));
    const auto topology = mesh.topology();
    REQUIRE(topology);
    std::vector<geometry::EdgeId> internal_edges;
    for (const auto& edge : topology.value().edges)
        if (edge.second_half_edge) internal_edges.push_back(edge.id);
    REQUIRE(internal_edges.size() >= 2U);
    REQUIRE(fixture.ui.dispatch(application::CreateMeshObjectAction{"Fan", std::move(mesh)}));
    const auto before_selection = fixture.session.snapshot();
    const auto object = before_selection.objects.back().object;
    REQUIRE(object.mesh_asset.has_value());
    REQUIRE(fixture.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::edge}));
    REQUIRE(fixture.ui.dispatch(application::SelectEdgeAction{object.id, internal_edges[0]}));
    auto availability = studio::edge_tool_availability(fixture.session.snapshot().selection);
    REQUIRE(availability.selected_edges == 1U && availability.split && availability.bevel && availability.dissolve);
    REQUIRE(fixture.ui.dispatch(application::SelectEdgeAction{
        object.id, internal_edges[1], editor::SelectionOperation::add}));
    availability = studio::edge_tool_availability(fixture.session.snapshot().selection);
    REQUIRE(availability.selected_edges == 2U && !availability.split && availability.bevel && availability.dissolve);
    const auto source_mesh = fixture.session.document_.meshes().at(*object.mesh_asset);
    const auto result = fixture.ui.dispatch(application::InvokeToolAction{"mesh.dissolve-edge", {}});
    REQUIRE(result && result.value().affected.edges.size() == 2U);
    REQUIRE(fixture.session.document_.meshes().at(*object.mesh_asset).face_count() == 2U);
    REQUIRE(fixture.session.document_.mesh_patches().back().replay(source_mesh));
    REQUIRE(fixture.ui.dispatch(application::UndoAction{}));
    REQUIRE(fixture.session.document_.meshes().at(*object.mesh_asset).face_count() == 4U);
    REQUIRE(fixture.ui.dispatch(application::RedoAction{}));
    REQUIRE(fixture.session.document_.meshes().at(*object.mesh_asset).face_count() == 2U);
    REQUIRE(fixture.ui.dispatch(application::SaveProjectAction{}));
    application::ApplicationSession reopened;
    REQUIRE(application::testing::dispatch(reopened, application::OpenProjectAction{fixture.path}));
    REQUIRE(reopened.document_.meshes().at(*object.mesh_asset).face_count() == 2U);

    Fixture multiple;
    REQUIRE(multiple.ui.dispatch(application::CreateMeshObjectAction{"Second Quad", quad()}));
    const auto objects = multiple.session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset && second.mesh_asset);
    const auto first_topology = multiple.session.document_.meshes().at(*first.mesh_asset).topology();
    const auto second_topology = multiple.session.document_.meshes().at(*second.mesh_asset).topology();
    REQUIRE(first_topology && second_topology);
    REQUIRE(multiple.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::edge}));
    REQUIRE(multiple.ui.dispatch(application::SelectEdgeAction{first.id, first_topology.value().edges.front().id}));
    REQUIRE(multiple.ui.dispatch(application::SelectEdgeAction{
        second.id, second_topology.value().edges.front().id, editor::SelectionOperation::add}));
    availability = studio::edge_tool_availability(multiple.session.snapshot().selection);
    REQUIRE(availability.selected_edges == 2U && availability.split);
    editor::ToolArguments split_arguments;
    split_arguments.factor = 0.5;
    const auto split = multiple.ui.dispatch(application::InvokeToolAction{"mesh.split-edge", split_arguments});
    REQUIRE(split && split.value().affected.objects.size() == 2U && split.value().affected.edges.size() == 2U);
    REQUIRE(multiple.session.document_.meshes().at(*first.mesh_asset).vertex_count() == 5U);
    REQUIRE(multiple.session.document_.meshes().at(*second.mesh_asset).vertex_count() == 5U);
    REQUIRE(multiple.ui.dispatch(application::UndoAction{}));
    REQUIRE(multiple.session.document_.meshes().at(*first.mesh_asset).vertex_count() == 4U);
    REQUIRE(multiple.session.document_.meshes().at(*second.mesh_asset).vertex_count() == 4U);
    REQUIRE(multiple.ui.dispatch(application::RedoAction{}));
    REQUIRE(multiple.session.document_.meshes().at(*first.mesh_asset).vertex_count() == 5U);
    REQUIRE(multiple.session.document_.meshes().at(*second.mesh_asset).vertex_count() == 5U);

    application::SelectionView selection;
    selection.mode = editor::SelectionMode::edge;
    selection.component_groups.resize(2U);
    selection.component_groups[0].edges = {geometry::EdgeId{1U}};
    selection.component_groups[1].edges = {geometry::EdgeId{2U}};
    availability = studio::edge_tool_availability(selection);
    REQUIRE(availability.selected_edges == 2U && availability.split && availability.bevel && availability.dissolve);
    selection.component_groups[1].edges.clear();
    availability = studio::edge_tool_availability(selection);
    REQUIRE(!availability.split && !availability.bevel && !availability.dissolve);
    selection.component_groups.resize(1U);
    selection.component_groups[0].edges.resize(editor::kMaxSelectedEdgesPerBevel + 1U);
    availability = studio::edge_tool_availability(selection);
    REQUIRE(!availability.split && !availability.bevel && availability.dissolve);
    selection.mode = editor::SelectionMode::face;
    availability = studio::edge_tool_availability(selection);
    REQUIRE(availability.selected_edges == 0U && !availability.split && !availability.bevel && !availability.dissolve);
}

void multi_face_tool_controls_match_authoring_commands() {
    using namespace carto;
    Fixture multiple;
    REQUIRE(multiple.ui.dispatch(application::CreateMeshObjectAction{"Second Quad", quad()}));
    const auto objects = multiple.session.snapshot().objects;
    REQUIRE(objects.size() == 2U);
    const auto first = objects[0].object;
    const auto second = objects[1].object;
    REQUIRE(first.mesh_asset && second.mesh_asset);
    const auto first_face = multiple.session.document_.meshes().at(*first.mesh_asset).faces_sorted().front().id;
    const auto second_face = multiple.session.document_.meshes().at(*second.mesh_asset).faces_sorted().front().id;
    REQUIRE(multiple.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(multiple.ui.dispatch(application::SelectFaceAction{first.id, first_face}));
    REQUIRE(multiple.ui.dispatch(application::SelectFaceAction{
        second.id, second_face, editor::SelectionOperation::add}));
    auto availability = studio::face_tool_availability(multiple.session.snapshot().selection);
    REQUIRE(availability.selected_faces == 2U && availability.selected_objects == 2U);
    REQUIRE(availability.individual && availability.remove && !availability.single && !availability.triangle_pair);
    editor::ToolArguments extrusion;
    extrusion.distance = 0.25;
    const auto extruded = multiple.ui.dispatch(application::InvokeToolAction{"mesh.extrude-face", extrusion});
    REQUIRE(extruded && extruded.value().affected.objects.size() == 2U && extruded.value().affected.faces.size() == 2U);
    REQUIRE(multiple.session.document_.meshes().at(*first.mesh_asset).face_count() == 5U);
    REQUIRE(multiple.session.document_.meshes().at(*second.mesh_asset).face_count() == 5U);
    REQUIRE(multiple.ui.dispatch(application::UndoAction{}));
    REQUIRE(multiple.session.document_.meshes().at(*first.mesh_asset).face_count() == 1U);
    REQUIRE(multiple.session.document_.meshes().at(*second.mesh_asset).face_count() == 1U);
    REQUIRE(multiple.ui.dispatch(application::RedoAction{}));
    REQUIRE(multiple.ui.dispatch(application::SaveProjectAction{}));
    application::ApplicationSession reopened;
    REQUIRE(application::testing::dispatch(reopened, application::OpenProjectAction{multiple.path}));
    REQUIRE(reopened.document_.meshes().at(*first.mesh_asset).face_count() == 5U);
    REQUIRE(reopened.document_.meshes().at(*second.mesh_asset).face_count() == 5U);

    Fixture same_owner;
    auto box = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto faces = box.value().faces_sorted();
    REQUIRE(same_owner.ui.dispatch(application::CreateMeshObjectAction{"Box", std::move(box.value())}));
    const auto box_object = same_owner.session.snapshot().objects.back().object;
    REQUIRE(box_object.mesh_asset);
    REQUIRE(same_owner.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(same_owner.ui.dispatch(application::SelectFaceAction{box_object.id, faces[0].id}));
    REQUIRE(same_owner.ui.dispatch(application::SelectFaceAction{
        box_object.id, faces[1].id, editor::SelectionOperation::add}));
    availability = studio::face_tool_availability(same_owner.session.snapshot().selection);
    REQUIRE(availability.selected_faces == 2U && availability.selected_objects == 1U);
    REQUIRE(availability.individual && availability.remove && availability.triangle_pair);
    REQUIRE(same_owner.ui.dispatch(application::InvokeToolAction{"mesh.poke-face", {}}));
    REQUIRE(same_owner.session.document_.meshes().at(*box_object.mesh_asset).face_count() == 12U);
    REQUIRE(same_owner.ui.dispatch(application::UndoAction{}));
    REQUIRE(same_owner.session.document_.meshes().at(*box_object.mesh_asset).face_count() == 6U);
    REQUIRE(same_owner.ui.dispatch(application::RedoAction{}));
    REQUIRE(same_owner.session.document_.meshes().at(*box_object.mesh_asset).face_count() == 12U);
    REQUIRE(same_owner.ui.dispatch(application::UndoAction{}));
    REQUIRE(same_owner.ui.dispatch(application::SelectFaceAction{box_object.id, faces[0].id}));
    REQUIRE(same_owner.ui.dispatch(application::SelectFaceAction{
        box_object.id, faces[1].id, editor::SelectionOperation::add}));
    editor::ToolArguments inset_arguments;
    inset_arguments.distance = 0.1;
    const auto inset = same_owner.ui.dispatch(application::InvokeToolAction{"mesh.inset-face", inset_arguments});
    REQUIRE(inset && inset.value().affected.faces.size() == 2U);
    REQUIRE(same_owner.session.document_.meshes().at(*box_object.mesh_asset).face_count() == 14U);
    REQUIRE(same_owner.ui.dispatch(application::UndoAction{}));
    REQUIRE(same_owner.session.document_.meshes().at(*box_object.mesh_asset).face_count() == 6U);
    REQUIRE(same_owner.ui.dispatch(application::RedoAction{}));
    REQUIRE(same_owner.ui.dispatch(application::SaveProjectAction{}));
    application::ApplicationSession inset_reopened;
    REQUIRE(application::testing::dispatch(inset_reopened, application::OpenProjectAction{same_owner.path}));
    REQUIRE(inset_reopened.document_.meshes().at(*box_object.mesh_asset).face_count() == 14U);

    Fixture deletion;
    REQUIRE(deletion.ui.dispatch(application::CreateMeshObjectAction{"Second Quad", quad()}));
    const auto delete_objects = deletion.session.snapshot().objects;
    const auto delete_first = delete_objects[0].object;
    const auto delete_second = delete_objects[1].object;
    REQUIRE(delete_first.mesh_asset && delete_second.mesh_asset);
    REQUIRE(deletion.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(deletion.ui.dispatch(application::SelectFaceAction{delete_first.id,
        deletion.session.document_.meshes().at(*delete_first.mesh_asset).faces_sorted().front().id}));
    REQUIRE(deletion.ui.dispatch(application::SelectFaceAction{delete_second.id,
        deletion.session.document_.meshes().at(*delete_second.mesh_asset).faces_sorted().front().id,
        editor::SelectionOperation::add}));
    const auto deleted = deletion.ui.dispatch(application::InvokeToolAction{"mesh.remove-face", {}});
    REQUIRE(deleted && deleted.value().affected.objects.size() == 2U && deleted.value().affected.faces.size() == 2U);
    REQUIRE(deletion.ui.dispatch(application::UndoAction{}));
    REQUIRE(deletion.session.document_.meshes().at(*delete_first.mesh_asset).face_count() == 1U);
    REQUIRE(deletion.session.document_.meshes().at(*delete_second.mesh_asset).face_count() == 1U);

    application::SelectionView selection;
    selection.mode = editor::SelectionMode::face;
    selection.component_groups.resize(1U);
    selection.component_groups[0].faces.resize(studio::kMaxInteractiveFaceBatch + 1U);
    availability = studio::face_tool_availability(selection);
    REQUIRE(availability.selected_faces == studio::kMaxInteractiveFaceBatch + 1U);
    REQUIRE(!availability.individual && !availability.remove && !availability.single && !availability.triangle_pair);
    selection.component_groups[0].faces.clear();
    availability = studio::face_tool_availability(selection);
    REQUIRE(availability.selected_faces == 0U && !availability.individual && !availability.remove);
    selection.component_groups[0].faces = {geometry::FaceId{1U}};
    selection.component_groups.resize(2U);
    availability = studio::face_tool_availability(selection);
    REQUIRE(!availability.individual && !availability.remove);
    selection.mode = editor::SelectionMode::edge;
    availability = studio::face_tool_availability(selection);
    REQUIRE(availability.selected_faces == 0U && !availability.individual && !availability.remove);
}

void multi_vertex_bevel_controls_use_atomic_project_history() {
    using namespace carto;
    Fixture fixture;
    REQUIRE(fixture.ui.dispatch(application::CreateMeshObjectAction{"Tetrahedron 1", tetrahedron()}));
    REQUIRE(fixture.ui.dispatch(application::CreateMeshObjectAction{"Tetrahedron 2", tetrahedron()}));
    const auto objects = fixture.session.snapshot().objects;
    REQUIRE(objects.size() == 3U);
    const auto first = objects[1].object;
    const auto second = objects[2].object;
    REQUIRE(first.mesh_asset && second.mesh_asset);
    REQUIRE(fixture.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(fixture.ui.dispatch(application::SelectVertexAction{first.id, geometry::VertexId{1U}}));
    auto availability = studio::vertex_tool_availability(fixture.session.snapshot().selection);
    REQUIRE(availability.selected_vertices == 1U && availability.single && availability.bevel);
    REQUIRE(fixture.ui.dispatch(application::SelectVertexAction{
        second.id, geometry::VertexId{1U}, editor::SelectionOperation::add}));
    availability = studio::vertex_tool_availability(fixture.session.snapshot().selection);
    REQUIRE(availability.selected_vertices == 2U && !availability.single && availability.bevel);
    editor::ToolArguments arguments;
    arguments.distance = 0.25;
    arguments.segments = 2U;
    const auto result = fixture.ui.dispatch(application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(result && result.value().affected.objects.size() == 2U);
    REQUIRE(result.value().affected.vertices.size() == 2U);
    const auto first_beveled = fixture.session.document_.meshes().at(*first.mesh_asset).vertex_count();
    const auto second_beveled = fixture.session.document_.meshes().at(*second.mesh_asset).vertex_count();
    REQUIRE(first_beveled > 4U && second_beveled > 4U);
    REQUIRE(fixture.ui.dispatch(application::UndoAction{}));
    REQUIRE(fixture.session.document_.meshes().at(*first.mesh_asset).vertex_count() == 4U);
    REQUIRE(fixture.session.document_.meshes().at(*second.mesh_asset).vertex_count() == 4U);
    REQUIRE(fixture.ui.dispatch(application::RedoAction{}));
    REQUIRE(fixture.session.document_.meshes().at(*first.mesh_asset).vertex_count() == first_beveled);
    REQUIRE(fixture.session.document_.meshes().at(*second.mesh_asset).vertex_count() == second_beveled);
    REQUIRE(fixture.ui.dispatch(application::SaveProjectAction{}));
    application::ApplicationSession reopened;
    REQUIRE(application::testing::dispatch(reopened, application::OpenProjectAction{fixture.path}));
    REQUIRE(reopened.document_.meshes().at(*first.mesh_asset).vertex_count() == first_beveled);
    REQUIRE(reopened.document_.meshes().at(*second.mesh_asset).vertex_count() == second_beveled);

    Fixture rejection;
    REQUIRE(rejection.ui.dispatch(application::CreateMeshObjectAction{"Closed tetrahedron", tetrahedron()}));
    REQUIRE(rejection.ui.dispatch(application::CreateMeshObjectAction{"Open quad", quad()}));
    const auto rejected_objects = rejection.session.snapshot().objects;
    const auto closed = rejected_objects[1].object;
    const auto open = rejected_objects[2].object;
    REQUIRE(closed.mesh_asset && open.mesh_asset);
    REQUIRE(rejection.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(rejection.ui.dispatch(application::SelectVertexAction{closed.id, geometry::VertexId{1U}}));
    REQUIRE(rejection.ui.dispatch(application::SelectVertexAction{
        open.id, geometry::VertexId{1U}, editor::SelectionOperation::add}));
    const auto before = rejection.session.snapshot();
    const auto before_document = rejection.session.document_.serialize();
    const auto rejected = rejection.ui.dispatch(application::InvokeToolAction{"mesh.bevel-vertex", arguments});
    REQUIRE(!rejected);
    REQUIRE(rejection.session.document_.serialize() == before_document);
    REQUIRE(rejection.session.snapshot().project_revision == before.project_revision);
    REQUIRE(rejection.session.snapshot().undo_count == before.undo_count);
    REQUIRE(rejection.session.document_.meshes().at(*closed.mesh_asset).vertex_count() == 4U);
    REQUIRE(rejection.session.document_.meshes().at(*open.mesh_asset).vertex_count() == 4U);

    application::SelectionView selection;
    selection.mode = editor::SelectionMode::vertex;
    selection.component_groups.resize(1U);
    selection.component_groups[0].vertices.resize(editor::kMaxSelectedVerticesPerBevel);
    availability = studio::vertex_tool_availability(selection);
    REQUIRE(availability.selected_vertices == editor::kMaxSelectedVerticesPerBevel && availability.bevel);
    selection.component_groups[0].vertices.emplace_back(geometry::VertexId{65U});
    availability = studio::vertex_tool_availability(selection);
    REQUIRE(!availability.bevel && !availability.single);
    selection.component_groups[0].vertices.clear();
    availability = studio::vertex_tool_availability(selection);
    REQUIRE(availability.selected_vertices == 0U && !availability.bevel);
    selection.component_groups[0].vertices = {geometry::VertexId{1U}};
    selection.component_groups.resize(2U);
    availability = studio::vertex_tool_availability(selection);
    REQUIRE(!availability.single && !availability.bevel);
    selection.mode = editor::SelectionMode::face;
    availability = studio::vertex_tool_availability(selection);
    REQUIRE(availability.selected_vertices == 0U && !availability.single && !availability.bevel);
}

void multi_object_vertex_merge_controls_match_per_object_cardinality() {
    using namespace carto;
    for (const bool by_distance : {false, true}) {
        for (const auto mode : {geometry::VertexMergeMode::target, geometry::VertexMergeMode::center}) {
            if (by_distance && mode == geometry::VertexMergeMode::center) continue;
            Fixture fixture;
            REQUIRE(fixture.ui.dispatch(application::CreateMeshObjectAction{"Second quad", quad()}));
            const auto objects = fixture.session.snapshot().objects;
            REQUIRE(objects.size() == 2U);
            const auto first = objects[0].object;
            const auto second = objects[1].object;
            REQUIRE(first.mesh_asset && second.mesh_asset);
            REQUIRE(fixture.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::vertex}));
            REQUIRE(fixture.ui.dispatch(application::SelectVertexAction{first.id, geometry::VertexId{1U}}));
            REQUIRE(fixture.ui.dispatch(application::SelectVertexAction{
                second.id, geometry::VertexId{1U}, editor::SelectionOperation::add}));
            auto availability = studio::vertex_tool_availability(fixture.session.snapshot().selection);
            REQUIRE(availability.selected_vertices == 2U && availability.bevel);
            REQUIRE(!availability.merge_pair && !availability.merge_by_distance);
            editor::ToolArguments arguments;
            arguments.merge_mode = mode;
            arguments.distance = 1.01;
            const auto command = by_distance ? "mesh.merge-by-distance" : "mesh.merge-vertices";
            auto before = fixture.truth();
            REQUIRE(!fixture.ui.dispatch(application::InvokeToolAction{command, arguments}));
            fixture.unchanged(before, true);

            REQUIRE(fixture.ui.dispatch(application::SelectVertexAction{
                first.id, geometry::VertexId{2U}, editor::SelectionOperation::add}));
            availability = studio::vertex_tool_availability(fixture.session.snapshot().selection);
            REQUIRE(availability.selected_vertices == 3U);
            REQUIRE(!availability.merge_pair && !availability.merge_by_distance);
            before = fixture.truth();
            REQUIRE(!fixture.ui.dispatch(application::InvokeToolAction{command, arguments}));
            fixture.unchanged(before, true);

            REQUIRE(fixture.ui.dispatch(application::SelectVertexAction{
                second.id, geometry::VertexId{2U}, editor::SelectionOperation::add}));
            availability = studio::vertex_tool_availability(fixture.session.snapshot().selection);
            REQUIRE(availability.selected_vertices == 4U && availability.merge_pair && availability.merge_by_distance);
            before = fixture.truth();
            const auto merged = fixture.ui.dispatch(application::InvokeToolAction{command, arguments});
            REQUIRE(merged && merged.value().affected.objects.size() == 2U);
            REQUIRE(fixture.session.snapshot().undo_count == before.snapshot.undo_count + 1U);
            REQUIRE(fixture.session.document_.meshes().at(*first.mesh_asset).vertex_count() == 3U);
            REQUIRE(fixture.session.document_.meshes().at(*second.mesh_asset).vertex_count() == 3U);
            REQUIRE(fixture.ui.dispatch(application::UndoAction{}));
            REQUIRE(fixture.session.document_.meshes().at(*first.mesh_asset).vertex_count() == 4U);
            REQUIRE(fixture.session.document_.meshes().at(*second.mesh_asset).vertex_count() == 4U);
            REQUIRE(fixture.ui.dispatch(application::RedoAction{}));
            REQUIRE(fixture.session.document_.meshes().at(*first.mesh_asset).vertex_count() == 3U);
            REQUIRE(fixture.session.document_.meshes().at(*second.mesh_asset).vertex_count() == 3U);
            REQUIRE(fixture.ui.dispatch(application::SaveProjectAction{}));
            application::ApplicationSession reopened;
            REQUIRE(application::testing::dispatch(reopened, application::OpenProjectAction{fixture.path}));
            REQUIRE(reopened.document_.meshes().at(*first.mesh_asset).vertex_count() == 3U);
            REQUIRE(reopened.document_.meshes().at(*second.mesh_asset).vertex_count() == 3U);
        }
    }

    application::SelectionView selection;
    selection.mode = editor::SelectionMode::vertex;
    REQUIRE(!studio::vertex_tool_availability(selection).merge_pair);
    REQUIRE(!studio::vertex_tool_availability(selection).merge_by_distance);
    selection.component_groups.resize(2U);
    selection.component_groups[0].vertices.resize(2U);
    selection.component_groups[1].vertices.resize(3U);
    auto availability = studio::vertex_tool_availability(selection);
    REQUIRE(!availability.merge_pair && availability.merge_by_distance);
    selection.component_groups[1].vertices.clear();
    availability = studio::vertex_tool_availability(selection);
    REQUIRE(!availability.merge_pair && !availability.merge_by_distance);
    selection.component_groups[1].vertices.resize(2U);
    selection.mode = editor::SelectionMode::face;
    availability = studio::vertex_tool_availability(selection);
    REQUIRE(!availability.merge_pair && !availability.merge_by_distance);
}

} // namespace

int main() {
    try {
        shared_selection_commands_convert_and_expand_without_authoring();
        shared_shortest_path_keeps_backend_ties_and_refuses_bad_context();
        shared_batch_duplicate_is_bounded_journaled_and_undoable();
        batch_availability_matches_refusal_for_ineligible_authored_sources();
        multi_edge_tool_controls_match_authoring_commands();
        multi_face_tool_controls_match_authoring_commands();
        multi_vertex_bevel_controls_use_atomic_project_history();
        multi_object_vertex_merge_controls_match_per_object_cardinality();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
