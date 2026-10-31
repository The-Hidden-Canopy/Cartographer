#include "../apps/desktop/studio_draft.hpp"

#include <carto/ui/ui.hpp>
#include <carto/journal/journal.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {

#define REQUIRE(condition) \
    do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

using namespace carto;

std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

geometry::EditableMesh triangle(double x) {
    geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({x, 0.0, 0.0});
    const auto b = mesh.add_vertex({x + 1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({x, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    return mesh;
}

struct Fixture {
    application::ApplicationSession session;
    ui::UiController controller{session};
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path directory = root / ("inspector-wiring-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "drafts.carto";
    scene::ObjectId first;
    scene::ObjectId second;

    Fixture() {
        REQUIRE(std::filesystem::create_directory(directory));
        REQUIRE(controller.dispatch(application::NewProjectAction{"Inspector wiring"}));
        REQUIRE(controller.dispatch(application::CreateMeshObjectAction{
            "First", triangle(0.0), std::nullopt, std::nullopt}));
        REQUIRE(controller.dispatch(application::CreateMeshObjectAction{
            "Second", triangle(10.0), std::nullopt, std::nullopt}));
        first = session.snapshot().objects.front().object.id;
        second = session.snapshot().objects.back().object.id;
        save();
    }
    ~Fixture() {
        // Delete only this fixture's explicitly created child of the test cwd.
        if (directory.parent_path() == root && directory.filename().string().starts_with("inspector-wiring-")) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    }
    void save() { REQUIRE(controller.dispatch(application::SaveProjectAction{path})); }
    std::filesystem::path journal_path() const { return std::filesystem::path(path.string() + ".journal"); }
    std::size_t events() const {
        const auto entries = journal::Journal(journal_path()).read_all();
        REQUIRE(entries);
        return entries.value().size();
    }
    void select_vertex(scene::ObjectId owner) {
        REQUIRE(controller.dispatch(application::SelectVertexAction{owner, geometry::VertexId{1U}}));
    }
    template <typename Operation>
    void refused(Operation operation) {
        save();
        const auto before = controller.snapshot();
        const auto document = bytes(path);
        const auto journal = bytes(journal_path());
        REQUIRE(!operation());
        const auto after = controller.snapshot();
        REQUIRE(after.project_generation == before.project_generation);
        REQUIRE(after.project_revision == before.project_revision && after.dirty == before.dirty);
        REQUIRE(after.undo_count == before.undo_count && after.redo_count == before.redo_count);
        REQUIRE(after.selection.objects == before.selection.objects);
        REQUIRE(after.selection.component_groups == before.selection.component_groups);
        REQUIRE(after.operations.size() == before.operations.size());
        REQUIRE(bytes(journal_path()) == journal);
        save();
        REQUIRE(bytes(path) == document && bytes(journal_path()) == journal);
    }
};

void drafts_keep_edited_values_stale_and_bind_owner_and_generation() {
    studio::InspectorDraft<core::Vec3d> draft;
    studio::InspectorDraftContext context{1U, core::Revision{5U}, scene::ObjectId{1U}, geometry::VertexId{1U}};
    REQUIRE(!draft.checked_value(context));
    draft.observe(context, {1.0, 2.0, 3.0});
    REQUIRE(draft.checked_value(context));
    draft.value().x = 9.0;
    draft.mark_edited();
    const auto edited_at = context;
    context.revision = context.revision.next();
    draft.observe(context, {4.0, 5.0, 6.0});
    REQUIRE(draft.stale() && draft.value().x == 9.0);
    REQUIRE(!draft.checked_value(context) && !draft.checked_value(edited_at));
    draft.observe(context, {4.0, 5.0, 6.0});
    REQUIRE(draft.stale() && draft.value().x == 9.0);
    draft.reset(context, {4.0, 5.0, 6.0});
    REQUIRE(!draft.stale() && draft.checked_value(context).value().x == 4.0);
    // Clean drafts follow undo/redo or other source edits automatically.
    context.revision = context.revision.next();
    draft.observe(context, {7.0, 8.0, 9.0});
    REQUIRE(draft.checked_value(context).value().x == 7.0);
    draft.value().x = 99.0;
    draft.mark_edited();
    context.object = scene::ObjectId{2U}; // Same vertex ID in another mesh.
    draft.observe(context, {20.0, 0.0, 0.0});
    REQUIRE(!draft.stale() && draft.checked_value(context).value().x == 20.0);
    draft.value().x = 99.0;
    draft.mark_edited();
    ++context.generation; // Same object/vertex/revision in a replacement project.
    draft.observe(context, {30.0, 0.0, 0.0});
    REQUIRE(draft.checked_value(context).value().x == 30.0);
    draft.clear();
    REQUIRE(!draft.checked_value(context));
}

void transform_inspector_rejects_stale_scope_and_preserves_human_history() {
    Fixture fixture;
    auto& ui = fixture.controller;
    REQUIRE(ui.dispatch(application::SelectObjectAction{fixture.first}));
    const auto captured = ui.snapshot();
    auto transform = core::Transform::identity();
    transform.translation.x = 2.0;
    const application::SetObjectTransformAction action{fixture.first, transform, captured.project_revision};
    REQUIRE(ui.dispatch(application::SelectObjectAction{fixture.second}));
    fixture.refused([&] { return ui.apply_inspector_transform(action, captured.project_generation); });
    REQUIRE(ui.dispatch(application::SelectObjectAction{fixture.first}));
    fixture.refused([&] { return ui.apply_inspector_transform(action, captured.project_generation + 1U); });
    auto invalid = action;
    invalid.expected_project_revision.reset();
    fixture.refused([&] { return ui.apply_inspector_transform(invalid, captured.project_generation); });
    invalid = action;
    invalid.transform.translation.x = std::numeric_limits<double>::infinity();
    fixture.refused([&] { return ui.apply_inspector_transform(invalid, captured.project_generation); });
    fixture.refused([&] { return ui.submit_ai_proposal(action); });
    const auto count = fixture.events();
    const auto receipt = ui.apply_inspector_transform(action, captured.project_generation);
    REQUIRE(receipt && receipt.value().document_changed && receipt.value().source == application::OperationSource::human);
    REQUIRE(fixture.events() == count + 1U);
    REQUIRE(ui.snapshot().operations.back().action == receipt.value().action);
    REQUIRE(ui.snapshot().undo_count == captured.undo_count + 1U);
    REQUIRE(ui.snapshot().objects.front().object.local_transform.translation.x == 2.0);
    fixture.refused([&] { return ui.apply_inspector_transform(action, captured.project_generation); });
    REQUIRE(ui.dispatch(application::UndoAction{}));
    REQUIRE(ui.snapshot().objects.front().object.local_transform.translation.x == 0.0);
    REQUIRE(ui.dispatch(application::RedoAction{}));
    REQUIRE(ui.snapshot().objects.front().object.local_transform.translation.x == 2.0);
    fixture.save();
    const auto saved = ui.snapshot();
    auto saved_action = action;
    saved_action.expected_project_revision = saved.project_revision;
    REQUIRE(ui.dispatch(application::OpenProjectAction{fixture.path}));
    REQUIRE(ui.snapshot().project_revision == saved.project_revision);
    REQUIRE(ui.dispatch(application::SelectObjectAction{fixture.first}));
    fixture.refused([&] { return ui.apply_inspector_transform(saved_action, saved.project_generation); });
    REQUIRE(ui.snapshot().objects.front().object.local_transform.translation.x == 2.0);
}

void vertex_inspector_binds_exact_selection_and_rechecks_live_revision() {
    Fixture fixture;
    auto& ui = fixture.controller;
    fixture.select_vertex(fixture.first);
    const auto captured = ui.snapshot();
    const auto apply = [&](scene::ObjectId owner, std::uint64_t generation, core::Revision revision) {
        return ui.apply_inspector_vertex_position(owner, geometry::VertexId{1U}, {-0.25, 0.0, 0.1}, generation, revision);
    };
    fixture.select_vertex(fixture.second);
    fixture.refused([&] { return apply(fixture.first, captured.project_generation, captured.project_revision); });
    fixture.select_vertex(fixture.first);
    REQUIRE(ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::object}));
    fixture.refused([&] { return apply(fixture.first, captured.project_generation, captured.project_revision); });
    fixture.select_vertex(fixture.first);
    fixture.refused([&] { return apply(fixture.first, captured.project_generation + 1U, captured.project_revision); });
    REQUIRE(ui.dispatch(application::SelectVertexAction{fixture.first, geometry::VertexId{2U}, editor::SelectionOperation::add}));
    fixture.refused([&] { return apply(fixture.first, captured.project_generation, captured.project_revision); });
    fixture.select_vertex(fixture.first);
    fixture.refused([&] { return ui.apply_inspector_vertex_position(fixture.first, geometry::VertexId{1U},
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, captured.project_generation, captured.project_revision); });
    editor::ToolArguments raw_arguments;
    raw_arguments.position = {-0.25, 0.0, 0.1};
    fixture.refused([&] { return ui.submit_ai_proposal(application::InvokeToolAction{
        "mesh.set-vertex-position", raw_arguments}); });
    const auto count = fixture.events();
    const auto receipt = apply(fixture.first, captured.project_generation, captured.project_revision);
    REQUIRE(receipt && receipt.value().document_changed && receipt.value().source == application::OperationSource::human);
    REQUIRE(fixture.events() == count + 1U);
    REQUIRE(ui.snapshot().operations.back().action == receipt.value().action);
    REQUIRE(ui.snapshot().undo_count == captured.undo_count + 1U);
    auto current = ui.snapshot();
    REQUIRE(current.viewport.scene.find(fixture.first)->mesh->positions.front().x == -0.25);
    fixture.refused([&] { return apply(fixture.first, captured.project_generation, captured.project_revision); });
    REQUIRE(ui.dispatch(application::UndoAction{}));
    REQUIRE(ui.snapshot().viewport.scene.find(fixture.first)->mesh->positions.front().x == 0.0);
    REQUIRE(ui.dispatch(application::RedoAction{}));
    REQUIRE(ui.snapshot().viewport.scene.find(fixture.first)->mesh->positions.front().x == -0.25);
    fixture.save();
    const auto saved = ui.snapshot();
    REQUIRE(ui.dispatch(application::OpenProjectAction{fixture.path}));
    fixture.select_vertex(fixture.first);
    fixture.refused([&] { return apply(fixture.first, saved.project_generation, saved.project_revision); });
    REQUIRE(ui.snapshot().viewport.scene.find(fixture.first)->mesh->positions.front().x == -0.25);
}

} // namespace

int main() {
    try {
        drafts_keep_edited_values_stale_and_bind_owner_and_generation();
        transform_inspector_rejects_stale_scope_and_preserves_human_history();
        vertex_inspector_binds_exact_selection_and_rechecks_live_revision();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
