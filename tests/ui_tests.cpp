#include <carto/ui/ui.hpp>
#include <carto/journal/journal.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

bool same_positions(
    const std::vector<carto::core::Vec3d>& left,
    const std::vector<carto::core::Vec3d>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].x != right[index].x || left[index].y != right[index].y ||
            left[index].z != right[index].z) {
            return false;
        }
    }
    return true;
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-ui-tests-" + std::to_string(stamp));
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

void preferences_validate_shell_invariants() {
    carto::ui::UiPreferences preferences;
    REQUIRE(preferences.validate());

    preferences.visible_panels.clear();
    REQUIRE(!preferences.validate());

    preferences = {};
    preferences.visible_panels.push_back(carto::ui::Panel::viewport);
    preferences.visible_panels.push_back(carto::ui::Panel::viewport);
    REQUIRE(!preferences.validate());

    preferences = {};
    preferences.visible_panels.erase(
        std::find(preferences.visible_panels.begin(), preferences.visible_panels.end(),
                  carto::ui::Panel::viewport));
    REQUIRE(!preferences.validate());
}

void ui_routes_authoritative_actions_and_operation_lineage() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}}));
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");
    REQUIRE(snapshot.operations.size() == 1U);
    REQUIRE(snapshot.operations.back().operation_id == 1U);
    REQUIRE(snapshot.operations.back().revision_before == carto::core::Revision{});
    REQUIRE(snapshot.operations.back().revision_after > snapshot.operations.back().revision_before);
    REQUIRE(snapshot.operations.back().category == carto::ui::OperationCategory::committed_command);
    REQUIRE(snapshot.operations.back().action == "Create Box");
    REQUIRE(snapshot.operations.back().document_changed);

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.snapshot().operations.size() == 1U);

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.operations.size() == 3U);
    REQUIRE(snapshot.operations[1].category == carto::ui::OperationCategory::undo);
    REQUIRE(snapshot.operations[2].category == carto::ui::OperationCategory::redo);

    const auto object = snapshot.objects.front().object.id;
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::vertex_mode));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.position = {-1.25, -1.0, -1.0};
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::repeat_last_tool));
    REQUIRE(ui.snapshot().operations.back().action == "Invoke Tool: mesh.set-vertex-position");
}

void ui_routes_spatial_fields_through_the_human_command_boundary() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const carto::application::CreateSpatialFieldAction create{
        "field.ui.moisture", carto::spatial_field::FieldSemantic::moisture,
        {0.0, 0.0}, 1.0, 4U, 4U, 0.0, 1.0, 0.0};
    REQUIRE(!ui.submit_ai_proposal(create));
    REQUIRE(session.snapshot().spatial_fields.empty());
    REQUIRE(ui.dispatch(create));
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.spatial_fields.size() == 1U);
    REQUIRE(snapshot.spatial_fields.front().semantic ==
        carto::spatial_field::FieldSemantic::moisture);
    REQUIRE(snapshot.operations.back().category ==
        carto::ui::OperationCategory::committed_command);
    REQUIRE(snapshot.operations.back().action == "Create Spatial Field");

    carto::spatial_field::FieldBrush brush;
    brush.expected_revision = snapshot.spatial_fields.front().source_revision;
    brush.center_uv = {1.5, 1.5};
    brush.radius = 2.0;
    brush.value = 0.6;
    brush.strength = 1.0;
    REQUIRE(ui.dispatch(carto::application::PaintSpatialFieldAction{
        "field.ui.moisture", brush}));
    snapshot = ui.snapshot();
    const auto painted_revision = snapshot.spatial_fields.front().source_revision;
    if (painted_revision != carto::core::Revision{2U}) {
        throw TestFailure("spatial field paint expected source revision 2, found " +
            std::to_string(painted_revision.value()));
    }
    REQUIRE(snapshot.operations.back().action == "Paint Spatial Field");
}

void ui_distinguishes_unsaved_from_saved_state() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);

    auto snapshot = ui.snapshot();
    REQUIRE(!snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");

    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Unsaved"}));
    snapshot = ui.snapshot();
    REQUIRE(!snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");

    REQUIRE(!ui.dispatch(carto::application::SaveProjectAction{
        temp.path() / "missing-parent" / "failed.carto"}));
    snapshot = ui.snapshot();
    REQUIRE(!snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");

    const auto path = temp.path() / "saved.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::saved);
    REQUIRE(snapshot.project_status_text == "Saved");

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Modified", {1.0, 1.0, 1.0}}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.project_path.has_value());
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::modified);
    REQUIRE(snapshot.project_status_text == "Modified");
    REQUIRE(!ui.dispatch(carto::application::SaveProjectAction{
        temp.path() / "missing-parent" / "failed-again.carto"}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.project_path.has_value());
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::modified);
    REQUIRE(snapshot.project_status_text == "Modified");
}

void ui_rejects_unadmitted_ai_proposals() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const auto before = session.snapshot();

    const auto rejected = ui.submit_ai_proposal(
        carto::application::CreateBoxAction{"AI Box", {1.0, 1.0, 1.0}});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().objects.empty());
    REQUIRE(ui.snapshot().operations.empty());
    REQUIRE(!ui.snapshot().ui_problems.empty());

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Human Box", {1.0, 1.0, 1.0}}));
    REQUIRE(session.snapshot().objects.size() == 1U);
    REQUIRE(ui.snapshot().operations.size() == 1U);
}

void ui_routes_edge_selection_without_authoring_mutation() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto before = ui.snapshot();
    REQUIRE(before.objects.size() == 1U);
    const auto object = before.objects.front().object.id;
    const auto instances = before.viewport.scene.instances();
    REQUIRE(instances.size() == 1U);
    REQUIRE(instances.front().mesh->triangle_edges.front().front().has_value());
    const auto edge = *instances.front().mesh->triangle_edges.front().front();
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::edge_mode));
    REQUIRE(ui.dispatch(carto::application::SelectEdgeAction{object, edge}));
    const auto after = ui.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.objects.size() == before.objects.size());
    REQUIRE(after.selection.mode == carto::editor::SelectionMode::edge);
    REQUIRE(after.selection.edges.size() == 1U);
    REQUIRE(after.selection.edges.front() == edge);
    REQUIRE(after.ui_problems.empty());
}

void ui_preserves_required_viewport_and_supports_mode_theme_density() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(!ui.set_panel_visible(carto::ui::Panel::viewport, false));
    REQUIRE(ui.set_density(carto::ui::Density::compact));
    REQUIRE(ui.set_theme(carto::ui::Theme::dark));
    const auto dark_snapshot = ui.snapshot();
    REQUIRE(dark_snapshot.theme == carto::ui::Theme::dark);
    REQUIRE(dark_snapshot.colors.canvas_0.red < dark_snapshot.colors.text_primary.red);
    REQUIRE(ui.set_theme(carto::ui::Theme::light));
    const auto light_snapshot = ui.snapshot();
    REQUIRE(light_snapshot.theme == carto::ui::Theme::light);
    REQUIRE(light_snapshot.colors.canvas_0.red > light_snapshot.colors.text_primary.red);
    REQUIRE(dark_snapshot.colors.canvas_0.red != light_snapshot.colors.canvas_0.red);
    REQUIRE(ui.set_theme(carto::ui::Theme::high_contrast));
    REQUIRE(ui.set_operator_mode(carto::ui::OperatorMode::ai_first));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::ai));
    const auto snapshot = ui.snapshot();
    REQUIRE(snapshot.density == carto::ui::Density::compact);
    REQUIRE(snapshot.theme == carto::ui::Theme::high_contrast);
    REQUIRE(snapshot.operator_mode == carto::ui::OperatorMode::ai_first);
    REQUIRE(snapshot.workspace_kind == carto::ui::Workspace::ai);
    REQUIRE(snapshot.ai_available);
    REQUIRE(snapshot.ai_auto_approve);
    REQUIRE(snapshot.ai_status.find("Native planner") != std::string::npos);
    REQUIRE(std::find(snapshot.visible_panels.begin(), snapshot.visible_panels.end(),
                      carto::ui::Panel::viewport) != snapshot.visible_panels.end());
}

void cartographer_brand_palette_is_stable() {
    const auto dark = carto::ui::color_tokens(carto::ui::Theme::dark);
    REQUIRE(dark.canvas_0.red == 0.031F);
    REQUIRE(dark.canvas_0.green == 0.059F);
    REQUIRE(dark.canvas_0.blue == 0.078F);
    REQUIRE(dark.accent_primary.red == 0.184F);
    REQUIRE(dark.accent_primary.green == 0.510F);
    REQUIRE(dark.accent_primary.blue == 0.965F);
    REQUIRE(dark.accent_secondary.red == 0.831F);
    REQUIRE(dark.accent_secondary.green == 0.651F);
    REQUIRE(dark.accent_secondary.blue == 0.471F);

    const auto light = carto::ui::color_tokens(carto::ui::Theme::light);
    REQUIRE(light.canvas_0.red == 0.941F);
    REQUIRE(light.canvas_0.green == 0.953F);
    REQUIRE(light.canvas_0.blue == 0.965F);
    REQUIRE(light.accent_primary.blue > light.accent_primary.red);
    REQUIRE(light.accent_secondary.red > light.accent_secondary.blue);
}

void ui_persists_preferences_and_resets_corrupt_state() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.set_density(carto::ui::Density::touch));
    REQUIRE(ui.set_theme(carto::ui::Theme::light));
    REQUIRE(ui.set_bottom_panel(carto::ui::BottomPanel::problems));
    REQUIRE(ui.set_ai_auto_approve(false));
    const auto path = temp.path() / "workspace.prefs";
    REQUIRE(ui.save_preferences(path));

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened(reopened_session);
    REQUIRE(reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().density == carto::ui::Density::touch);
    REQUIRE(reopened.snapshot().theme == carto::ui::Theme::light);
    REQUIRE(reopened.snapshot().bottom_panel == carto::ui::BottomPanel::problems);
    REQUIRE(!reopened.snapshot().ai_auto_approve);
    REQUIRE(reopened.set_panel_visible(carto::ui::Panel::scene, false));
    auto reopened_application_snapshot = reopened_session.snapshot();
    REQUIRE(std::find(reopened_application_snapshot.workspace.visible_panes.begin(),
                      reopened_application_snapshot.workspace.visible_panes.end(),
                      carto::application::Pane::outliner) ==
            reopened_application_snapshot.workspace.visible_panes.end());

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "header=CARTOGRAPHER_UI_PREFS_V1\n"
                  "operator_mode=person-first\n"
                  "workspace=model\n"
                  "density=standard\n"
                  "theme=dark\n"
                  "bottom_panel=operations\n"
                  "panels=scene,viewport,inspector,operations,problems\n";
    }
    REQUIRE(reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().ai_auto_approve);

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "header=CARTOGRAPHER_UI_PREFS_V1\nworkspace=unknown\n";
    }
    REQUIRE(!reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().workspace_kind == carto::ui::Workspace::model);
    REQUIRE(reopened.snapshot().density == carto::ui::Density::standard);
    reopened_application_snapshot = reopened_session.snapshot();
    REQUIRE(std::find(reopened_application_snapshot.workspace.visible_panes.begin(),
                      reopened_application_snapshot.workspace.visible_panes.end(),
                      carto::application::Pane::outliner) !=
            reopened_application_snapshot.workspace.visible_panes.end());
    REQUIRE(!reopened.snapshot().ui_problems.empty());

    {
        std::ofstream oversized(path, std::ios::binary | std::ios::trunc);
        oversized << std::string(17U * 1024U, 'x');
    }
    REQUIRE(!reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().workspace_kind == carto::ui::Workspace::model);
    REQUIRE(reopened.snapshot().density == carto::ui::Density::standard);
    REQUIRE(reopened.snapshot().theme == carto::ui::Theme::dark);
    REQUIRE(!reopened.snapshot().ui_problems.empty());
}

void workbench_preferences_round_trip_and_fail_closed() {
    TempDirectory temp;
    const auto path = temp.path() / "workbench.prefs";
    carto::ui::WorkbenchPreferences preferences;
    preferences.ledger_collapsed = true;
    preferences.flow_use_counts[2] = 7U;
    preferences.last_flow_index = 2;
    auto& scene = preferences.instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene));
    scene.visible = true;
    scene.positioned = true;
    scene.x = 142;
    scene.y = 156;
    scene.width = 286;
    scene.height = 330;
    REQUIRE(carto::ui::save_workbench_preferences(path, preferences));

    const auto loaded = carto::ui::load_workbench_preferences(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().ledger_collapsed);
    REQUIRE(loaded.value().flow_use_counts[2] == 7U);
    REQUIRE(loaded.value().last_flow_index == 2);
    REQUIRE(loaded.value().instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).visible);
    REQUIRE(loaded.value().instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).x == 142);
    REQUIRE(loaded.value().instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).width == 286);

    const std::string before_failed_save = [&path] {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }();
    auto invalid_preferences = preferences;
    invalid_preferences.instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).width = 20;
    REQUIRE(!carto::ui::save_workbench_preferences(path, invalid_preferences));
    std::ifstream after_failed_save_stream(path, std::ios::binary);
    const std::string after_failed_save(
        std::istreambuf_iterator<char>(after_failed_save_stream), {});
    REQUIRE(after_failed_save == before_failed_save);

    auto invalid_memory = preferences;
    invalid_memory.last_flow_index = 4;
    REQUIRE(!carto::ui::save_workbench_preferences(path, invalid_memory));
    std::ifstream after_invalid_memory_stream(path, std::ios::binary);
    const std::string after_invalid_memory(
        std::istreambuf_iterator<char>(after_invalid_memory_stream), {});
    REQUIRE(after_invalid_memory == before_failed_save);

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "header=CARTOGRAPHER_WORKBENCH_PREFS_V1\n"
                  "ledger_collapsed=0\n";
        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto instrument = static_cast<carto::ui::WorkbenchInstrument>(index);
            legacy << carto::ui::workbench_instrument_name(instrument)
                   << "=0,0,0,0,0,0\n";
        }
    }
    const auto legacy_loaded = carto::ui::load_workbench_preferences(path);
    REQUIRE(legacy_loaded);
    REQUIRE(legacy_loaded.value().last_flow_index == -1);
    REQUIRE(std::all_of(legacy_loaded.value().flow_use_counts.begin(),
                        legacy_loaded.value().flow_use_counts.end(),
                        [](std::uint32_t uses) { return uses == 0U; }));

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "header=CARTOGRAPHER_WORKBENCH_PREFS_V1\n"
                   "ledger_collapsed=0\nscene=1,1,142,156,286\n";
    }
    REQUIRE(!carto::ui::load_workbench_preferences(path));
}

void ui_connects_surface_fidelity_preview_without_authoring_mutation() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);

    REQUIRE(!ui.compile_surface_preview(6U));
    REQUIRE(ui.snapshot().surface_fidelity.status ==
            carto::ui::SurfaceFidelityStatus::unavailable);

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Surface box", {2.0, 2.0, 2.0}}));
    const auto before = ui.snapshot();
    REQUIRE(before.objects.size() == 1U);
    const auto object = before.objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    const auto selected = ui.snapshot();
    const auto revision = selected.project_revision;
    const auto bounded = ui.compile_surface_preview(
        carto::ui::kMaxSurfacePreviewTriangles + 1U,
        carto::surface::RepresentationMode::fidelity_closure);
    REQUIRE(!bounded);
    REQUIRE(bounded.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(ui.snapshot().surface_fidelity.status ==
            carto::ui::SurfaceFidelityStatus::unavailable);
    REQUIRE(ui.snapshot().operations.size() == 1U);
    REQUIRE(ui.compile_surface_preview(
        6U, carto::surface::RepresentationMode::fidelity_closure));

    auto preview = ui.snapshot().surface_fidelity;
    REQUIRE(preview.status == carto::ui::SurfaceFidelityStatus::ready);
    REQUIRE(preview.admitted);
    REQUIRE(preview.object.has_value() && *preview.object == object);
    REQUIRE(preview.source_triangles == 12U);
    REQUIRE(preview.target_triangles == 6U);
    REQUIRE(preview.mode == carto::surface::RepresentationMode::fidelity_closure);
    REQUIRE(preview.compiled != nullptr);
    REQUIRE(preview.compiled->geometry.triangle_count() == 6U);
    REQUIRE(!preview.domain_digest.empty());
    REQUIRE(!preview.representation_digest.empty());
    REQUIRE(preview.validate());
    REQUIRE(ui.snapshot().project_revision == revision);
    REQUIRE(ui.snapshot().operations.size() == 1U);

    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        object, carto::core::Transform{{1.0, 0.0, 0.0}, carto::core::Quaternion::identity(),
                                       {1.0, 1.0, 1.0}}, std::nullopt}));
    preview = ui.snapshot().surface_fidelity;
    REQUIRE(preview.status == carto::ui::SurfaceFidelityStatus::stale);
    REQUIRE(!preview.admitted);

    REQUIRE(ui.clear_surface_preview());
    REQUIRE(ui.snapshot().surface_fidelity.status == carto::ui::SurfaceFidelityStatus::idle);
}

void ui_imports_mesh_through_provider_and_authoring_boundaries() {
    TempDirectory temp;
    const auto project_path = temp.path() / "wired-import.carto";
    const auto obj_path = temp.path() / "triangle.obj";
    const auto ply_path = temp.path() / "triangle.ply";
    const auto stl_path = temp.path() / "triangle.stl";
    {
        std::ofstream obj(obj_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(obj));
        obj << "v 0 0 0\n"
               "v 1 0 0\n"
               "v 0 1 0\n"
               "f 1 2 3\n";
    }
    {
        std::ofstream ply(ply_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(ply));
        ply << "ply\n"
               "format ascii 1.0\n"
               "element vertex 3\n"
               "property float x\n"
               "property float y\n"
               "property float z\n"
               "element face 1\n"
               "property list uchar int vertex_indices\n"
               "end_header\n"
               "0 0 0\n"
               "1 0 0\n"
               "0 1 0\n"
               "3 0 1 2\n";
    }
    {
        std::ofstream stl(stl_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(stl));
        stl << "solid triangle\n"
               "facet normal 0 0 1\n"
               "outer loop\n"
               "vertex 0 0 0\n"
               "vertex 1 0 0\n"
               "vertex 0 1 0\n"
               "endloop\n"
               "endfacet\n"
               "endsolid triangle\n";
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto revision_before = ui.snapshot().project_revision;

    const auto imported = ui.import_mesh(obj_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(imported);
    REQUIRE(imported.value().object_name == "triangle");
    REQUIRE(imported.value().vertices == 3U);
    REQUIRE(imported.value().faces == 1U);
    REQUIRE(imported.value().triangles == 1U);
    REQUIRE(imported.value().dispatch.action == "Create Mesh Object");
    REQUIRE(imported.value().dispatch.document_changed);
    REQUIRE(imported.value().dispatch.source == carto::application::OperationSource::human);

    const auto snapshot = ui.snapshot();
    REQUIRE(snapshot.project_revision > revision_before);
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.objects.front().object.name == "triangle");
    REQUIRE(snapshot.objects.front().object.mesh_asset.has_value());
    REQUIRE(snapshot.viewport.scene.instances().size() == 1U);
    REQUIRE(!snapshot.operations.empty());
    REQUIRE(snapshot.operations.back().action == "Create Mesh Object");

    const auto recovery = carto::application::ApplicationSession::inspect_recovery(project_path);
    REQUIRE(recovery);
    REQUIRE(recovery.value().state == carto::application::RecoveryInspectionState::pending);
    REQUIRE(!recovery.value().journal_entries.empty());
    REQUIRE(recovery.value().journal_entries.back().event_type ==
            "cartographer.application_action");
    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries);
    REQUIRE(!journal_entries.value().empty());
    const std::string journal_payload(
        journal_entries.value().back().payload.begin(),
        journal_entries.value().back().payload.end());
    REQUIRE(journal_payload.find("operation=Create Mesh Object") != std::string::npos);

    const auto imported_ply = ui.import_mesh(ply_path, carto::ui::MeshImportFormat::ply);
    REQUIRE(imported_ply);
    REQUIRE(imported_ply.value().vertices == 3U);
    REQUIRE(imported_ply.value().faces == 1U);
    REQUIRE(imported_ply.value().triangles == 1U);
    const auto imported_stl = ui.import_mesh(stl_path, carto::ui::MeshImportFormat::stl);
    REQUIRE(imported_stl);
    REQUIRE(imported_stl.value().vertices == 3U);
    REQUIRE(imported_stl.value().faces == 1U);
    REQUIRE(imported_stl.value().triangles == 1U);
    REQUIRE(ui.snapshot().objects.size() == 3U);

    const auto stable_revision = ui.snapshot().project_revision;
    const auto stable_operations = ui.snapshot().operations.size();
    const auto missing = ui.import_mesh(
        temp.path() / "missing.obj", carto::ui::MeshImportFormat::obj);
    REQUIRE(!missing);
    REQUIRE(ui.snapshot().project_revision == stable_revision);
    REQUIRE(ui.snapshot().objects.size() == 3U);
    REQUIRE(ui.snapshot().operations.size() == stable_operations);
    REQUIRE(!ui.snapshot().ui_problems.empty());

    const auto unsupported = ui.import_mesh(
        obj_path, static_cast<carto::ui::MeshImportFormat>(99));
    REQUIRE(!unsupported);
    REQUIRE(unsupported.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(ui.snapshot().project_revision == stable_revision);

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::save));
    REQUIRE(!ui.snapshot().dirty);
}

void ui_connects_local_market_boundary_and_fails_closed() {
    TempDirectory temp;
    const auto package = temp.path() / "package";
    const auto library = temp.path() / "library";
    std::filesystem::create_directories(package);
    {
        std::ofstream payload(package / "geometry.bin", std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(payload));
        payload << "cartographer-data-only-payload";
    }

    carto::market::MarketPackageManifest manifest;
    manifest.package_id = "sample.geometry";
    manifest.listing_id = "sample-listing";
    manifest.listing_version_id = "1.0.0";
    manifest.publisher_id = "local-publisher";
    manifest.license_id = "cc0-1.0";
    manifest.entrypoints.push_back({"asset", "geometry.bin"});
    const auto content_digest = carto::market::MarketPackageValidator::directory_payload_digest(package);
    REQUIRE(content_digest);
    manifest.content_digest = content_digest.value();
    {
        std::ofstream descriptor(package / "manifest.json", std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(descriptor));
        descriptor << manifest.serialize();
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.snapshot().market.status == carto::ui::MarketStatus::unconfigured);
    REQUIRE(ui.configure_market_library(library));
    REQUIRE(ui.snapshot().market.status == carto::ui::MarketStatus::empty);
    REQUIRE(ui.snapshot().market.packages.empty());
    const auto refresh_command = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "market.refresh"; });
    REQUIRE(refresh_command != ui.snapshot().commands.end());
    REQUIRE(refresh_command->supported);

    const auto installed = ui.install_market_directory(package);
    REQUIRE(installed);
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.market.status == carto::ui::MarketStatus::ready);
    REQUIRE(snapshot.market.packages.size() == 1U);
    REQUIRE(snapshot.market.packages.front().package_id == "sample.geometry");
    REQUIRE(snapshot.market.packages.front().listing_version_id == "1.0.0");
    REQUIRE(!snapshot.market.packages.front().publication_receipt_bound);
    REQUIRE(snapshot.market.packages.front().package_digest.starts_with("sha256:"));

    const auto installed_payload = installed.value().install_root / "geometry.bin";
    {
        std::ofstream tampered(installed_payload, std::ios::binary | std::ios::app);
        REQUIRE(static_cast<bool>(tampered));
        tampered << "tampered";
    }
    const auto refresh = ui.refresh_market_library();
    REQUIRE(!refresh);
    REQUIRE(ui.snapshot().market.status == carto::ui::MarketStatus::error);
    REQUIRE(ui.snapshot().market.packages.empty());
}

void ui_failed_shortcuts_and_actions_remain_bounded() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(!ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(!ui.handle_shortcut(carto::ui::Shortcut::redo));
    REQUIRE(!ui.handle_shortcut(carto::ui::Shortcut::save));
    REQUIRE(!ui.dispatch(carto::application::SelectObjectAction{carto::scene::ObjectId{999U}}));
    REQUIRE(ui.snapshot().operations.empty());
    REQUIRE(!ui.snapshot().problems.empty());
}

void ui_command_palette_is_bounded_and_explicit_about_deferred_features() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::toggle_command_palette));
    REQUIRE(ui.snapshot().command_palette_open);
    REQUIRE(ui.set_command_palette_query("face"));
    REQUIRE(ui.snapshot().command_palette_query == "face");
    const std::string oversized(1025U, 'x');
    REQUIRE(!ui.set_command_palette_query(oversized));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::close_overlay));
    REQUIRE(!ui.snapshot().command_palette_open);
    const auto edge = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "mode.edge"; });
    REQUIRE(edge != ui.snapshot().commands.end());
    REQUIRE(edge->supported);
    const auto repeat = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "edit.repeat-last"; });
    REQUIRE(repeat != ui.snapshot().commands.end());
    REQUIRE(repeat->shortcut == "Ctrl+R");
    const auto import_mesh = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "file.import-mesh"; });
    REQUIRE(import_mesh != ui.snapshot().commands.end());
    REQUIRE(import_mesh->supported);
    REQUIRE(import_mesh->shortcut == "Ctrl+I");
    const auto duplicate = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "edit.duplicate"; });
    REQUIRE(duplicate != ui.snapshot().commands.end());
    REQUIRE(duplicate->supported);
    REQUIRE(duplicate->shortcut == "Ctrl+D");
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::edge_mode));
    REQUIRE(ui.snapshot().selection.mode == carto::editor::SelectionMode::edge);
    REQUIRE(ui.snapshot().project_revision == carto::core::Revision{});
}

void native_key_contract_maps_core_control_without_imgui() {
    using carto::ui::NativeKey;
    using carto::ui::NativeKeyEvent;
    using carto::ui::Shortcut;

    const auto save = carto::ui::shortcut_for_native_key({
        NativeKey::s, carto::ui::kNativeModifierControl, true, false});
    REQUIRE(save.has_value() && *save == Shortcut::save);
    const auto undo_repeat = carto::ui::shortcut_for_native_key({
        NativeKey::z, carto::ui::kNativeModifierControl, true, true});
    REQUIRE(undo_repeat.has_value() && *undo_repeat == Shortcut::undo);
    const auto repeat_last = carto::ui::shortcut_for_native_key({
        NativeKey::r, carto::ui::kNativeModifierControl, true, false});
    REQUIRE(repeat_last.has_value() && *repeat_last == Shortcut::repeat_last_tool);
    const auto face = carto::ui::shortcut_for_native_key({
        NativeKey::digit_4, 0U, true, false});
    REQUIRE(face.has_value() && *face == Shortcut::face_mode);
    const auto escape = carto::ui::shortcut_for_native_key({
        NativeKey::escape, 0U, true, false});
    REQUIRE(escape.has_value() && *escape == Shortcut::close_overlay);
    REQUIRE(!carto::ui::shortcut_for_native_key({
        NativeKey::s, 0U, true, false}).has_value());
    REQUIRE(!carto::ui::shortcut_for_native_key({
        NativeKey::digit_1, carto::ui::kNativeModifierShift, true, false}).has_value());
    REQUIRE(!carto::ui::shortcut_for_native_key(
        NativeKeyEvent{NativeKey::k, carto::ui::kNativeModifierControl, false, false}).has_value());

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.handle_shortcut(*face));
    REQUIRE(ui.snapshot().selection.mode == carto::editor::SelectionMode::face);

    using carto::ui::NativePointerButton;
    using carto::ui::NativePointerEvent;
    const auto replace = carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240, 0U, true});
    REQUIRE(replace.has_value() && *replace == carto::editor::SelectionOperation::replace);
    const auto add = carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240, carto::ui::kNativeModifierShift, true});
    REQUIRE(add.has_value() && *add == carto::editor::SelectionOperation::add);
    const auto toggle = carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240, carto::ui::kNativeModifierControl, true});
    REQUIRE(toggle.has_value() && *toggle == carto::editor::SelectionOperation::toggle);
    REQUIRE(!carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::secondary, 120, 240, 0U, true}).has_value());
    REQUIRE(!carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240,
        static_cast<std::uint8_t>(carto::ui::kNativeModifierShift |
                                   carto::ui::kNativeModifierControl), true}).has_value());
    REQUIRE(!carto::ui::selection_operation_for_native_pointer(
        NativePointerEvent{NativePointerButton::primary, 120, 240, 0U, false}).has_value());
}

void workspace_registry_is_canonical_and_fail_closed() {
    const auto& registry = carto::ui::workspace_registry();
    REQUIRE(registry.size() == 10U);
    std::vector<carto::ui::Workspace> seen;
    for (const auto& definition : registry) {
        REQUIRE(definition.validate());
        REQUIRE(std::find(seen.begin(), seen.end(), definition.workspace) == seen.end());
        seen.push_back(definition.workspace);
        const auto resolved = carto::ui::workspace_definition(definition.workspace);
        REQUIRE(resolved);
        REQUIRE(resolved.value().id == definition.id);
        REQUIRE(resolved.value().label == definition.label);
    }
    REQUIRE(seen.size() == registry.size());

    const auto model = carto::ui::workspace_definition(carto::ui::Workspace::model);
    REQUIRE(model && model.value().available);
    const auto ai = carto::ui::workspace_definition(carto::ui::Workspace::ai);
    REQUIRE(ai && ai.value().available);
    const auto sculpt = carto::ui::workspace_definition(carto::ui::Workspace::sculpt);
    REQUIRE(sculpt && sculpt.value().available);
    const auto engineering = carto::ui::workspace_definition(
        carto::ui::Workspace::engineering);
    REQUIRE(engineering && engineering.value().available);
    const auto condition = carto::ui::workspace_definition(
        carto::ui::Workspace::condition);
    REQUIRE(condition && condition.value().available);
    const auto future = carto::ui::workspace_definition(carto::ui::Workspace::cad);
    REQUIRE(future && !future.value().available);
    REQUIRE(!future.value().unavailable_reason.empty());
    REQUIRE(!carto::ui::workspace_definition(static_cast<carto::ui::Workspace>(99)));

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.set_panel_visible(carto::ui::Panel::assets, true));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::ai));
    const auto ai_snapshot = ui.snapshot();
    REQUIRE(ai_snapshot.workspace_kind == carto::ui::Workspace::ai);
    REQUIRE(ai_snapshot.bottom_panel == carto::ui::BottomPanel::graph);
    REQUIRE(std::find(ai_snapshot.visible_panels.begin(), ai_snapshot.visible_panels.end(),
                      carto::ui::Panel::graph) != ai_snapshot.visible_panels.end());
    REQUIRE(std::find(ai_snapshot.visible_panels.begin(), ai_snapshot.visible_panels.end(),
                      carto::ui::Panel::assets) == ai_snapshot.visible_panels.end());

    const auto unavailable = ui.set_workspace(carto::ui::Workspace::cad);
    REQUIRE(!unavailable);
    REQUIRE(unavailable.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(ui.snapshot().workspace_kind == carto::ui::Workspace::ai);

    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));
    const auto sculpt_snapshot = ui.snapshot();
    REQUIRE(sculpt_snapshot.workspace_kind == carto::ui::Workspace::sculpt);
    REQUIRE(sculpt_snapshot.bottom_panel == carto::ui::BottomPanel::operations);

    REQUIRE(ui.set_workspace(carto::ui::Workspace::engineering));
    const auto engineering_snapshot = ui.snapshot();
    REQUIRE(engineering_snapshot.workspace_kind == carto::ui::Workspace::engineering);
    REQUIRE(engineering_snapshot.bottom_panel == carto::ui::BottomPanel::problems);
    REQUIRE(std::find(engineering_snapshot.visible_panels.begin(),
                      engineering_snapshot.visible_panels.end(),
                      carto::ui::Panel::operations) != engineering_snapshot.visible_panels.end());

    REQUIRE(ui.set_workspace(carto::ui::Workspace::condition));
    const auto condition_snapshot = ui.snapshot();
    REQUIRE(condition_snapshot.workspace_kind == carto::ui::Workspace::condition);
    REQUIRE(condition_snapshot.bottom_panel == carto::ui::BottomPanel::problems);
    REQUIRE(std::find(condition_snapshot.visible_panels.begin(),
                      condition_snapshot.visible_panels.end(),
                      carto::ui::Panel::operations) != condition_snapshot.visible_panels.end());
}

void sculpt_viewport_sampling_preserves_frontmost_surface_and_path_spacing() {
    using carto::core::Vec2d;
    using carto::core::Vec3d;
    using carto::geometry::FaceId;
    using carto::geometry::VertexId;
    using carto::scene::ObjectId;
    using carto::ui::SculptViewportTriangle;

    const ObjectId object{17U};
    const auto make_triangle = [](ObjectId owner, FaceId face, std::uint64_t first_id,
                                  double depth) {
        return SculptViewportTriangle{
            owner,
            face,
            {Vec2d{0.0, 0.0}, Vec2d{10.0, 0.0}, Vec2d{0.0, 10.0}},
            {VertexId{first_id}, VertexId{first_id + 1U}, VertexId{first_id + 2U}},
            {Vec3d{0.0, depth, 0.0}, Vec3d{10.0, depth, 0.0}, Vec3d{0.0, depth, 10.0}},
            {depth, depth, depth}};
    };
    std::vector<SculptViewportTriangle> triangles{
        make_triangle(object, FaceId{1U}, 1U, 2.0),
        make_triangle(object, FaceId{2U}, 4U, 1.0),
        make_triangle(ObjectId{18U}, FaceId{3U}, 7U, 9.0)};

    const auto hit = carto::ui::sample_sculpt_viewport_surface(
        triangles, object, Vec2d{2.0, 2.0});
    REQUIRE(hit);
    REQUIRE(hit.value().surface_anchor.has_value());
    REQUIRE(hit.value().surface_anchor->face == FaceId{1U});
    REQUIRE(hit.value().position.x == 2.0);
    REQUIRE(hit.value().position.y == 2.0);
    REQUIRE(hit.value().position.z == 2.0);
    REQUIRE(hit.value().surface_anchor->validate());

    const std::vector<Vec2d> direct_path{{1.0, 1.0}, {7.0, 1.0}};
    const std::vector<Vec2d> segmented_path{{1.0, 1.0}, {3.0, 1.0}, {7.0, 1.0}};
    const auto direct = carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, direct_path, 2.0);
    const auto segmented = carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, segmented_path, 2.0);
    REQUIRE(direct);
    REQUIRE(segmented);
    REQUIRE(direct.value() == segmented.value());
    REQUIRE(direct.value().size() == 4U);
    for (const auto& sample : direct.value()) {
        REQUIRE(sample.surface_anchor.has_value());
        REQUIRE(sample.surface_anchor->face == FaceId{1U});
        REQUIRE(sample.position.y == 2.0);
    }

    std::reverse(triangles.begin(), triangles.end());
    const auto reordered_hit = carto::ui::sample_sculpt_viewport_surface(
        triangles, object, Vec2d{2.0, 2.0});
    REQUIRE(reordered_hit);
    REQUIRE(reordered_hit.value() == hit.value());

    const auto miss = carto::ui::sample_sculpt_viewport_surface(
        triangles, object, Vec2d{12.0, 12.0});
    REQUIRE(!miss);
    REQUIRE(miss.error().code == carto::core::ErrorCode::not_found);
    const std::vector<Vec2d> leaving_surface{{1.0, 1.0}, {12.0, 12.0}};
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, leaving_surface, 2.0));
    const std::vector<Vec2d> non_finite_path{{1.0, 1.0},
        {std::numeric_limits<double>::quiet_NaN(), 2.0}};
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, non_finite_path, 2.0));
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, direct_path, 0.0));
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, direct_path, 1.0e-12));
}

void sculpt_surface_strokes_commit_anchored_samples_through_human_commands() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Sculpt surface stroke test"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Surface mesh", {2.0, 2.0, 2.0}}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));

    const auto initial = ui.snapshot();
    REQUIRE(initial.objects.size() == 1U);
    const carto::scene::ObjectId object = initial.objects.front().object.id;
    const std::uint64_t mesh_asset = *initial.objects.front().object.mesh_asset;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));
    const auto selected = ui.snapshot();
    const auto* instance = selected.viewport.scene.find(object);
    REQUIRE(instance != nullptr);
    REQUIRE(instance->mesh != nullptr);
    const auto source_positions = instance->mesh->positions;

    std::vector<carto::ui::SculptViewportTriangle> triangles;
    for (std::size_t index = 0U; index + 2U < instance->mesh->indices.size(); index += 3U) {
        const std::array<std::uint32_t, 3> corners{
            instance->mesh->indices[index],
            instance->mesh->indices[index + 1U],
            instance->mesh->indices[index + 2U]};
        carto::ui::SculptViewportTriangle triangle;
        triangle.object = object;
        triangle.face = instance->mesh->triangle_faces[index / 3U];
        for (std::size_t corner = 0U; corner < corners.size(); ++corner) {
            const std::uint32_t source_index = corners[corner];
            triangle.vertices[corner] = instance->mesh->vertex_ids[source_index];
            triangle.local_positions[corner] = instance->mesh->positions[source_index];
            triangle.screen_positions[corner] = {
                triangle.local_positions[corner].x,
                triangle.local_positions[corner].z};
            triangle.camera_depth[corner] = triangle.local_positions[corner].y;
        }
        triangles.push_back(std::move(triangle));
    }
    const std::vector<carto::core::Vec2d> cursor_path{
        {-0.6, 0.0}, {-0.1, 0.0}, {0.6, 0.0}};
    const auto samples = carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, cursor_path, 0.2);
    REQUIRE(samples);
    REQUIRE(samples.value().size() >= 4U);
    auto authored_mesh = carto::geometry::make_box(carto::core::Vec3d{2.0, 2.0, 2.0});
    REQUIRE(authored_mesh);
    for (const auto& sample : samples.value()) {
        REQUIRE(sample.surface_anchor.has_value());
        const auto resolved = carto::production::resolve_sculpt_surface_anchor(
            authored_mesh.value(), *sample.surface_anchor);
        REQUIRE(resolved);
        REQUIRE(sample.position.x == resolved.value().x);
        REQUIRE(sample.position.y == resolved.value().y);
        REQUIRE(sample.position.z == resolved.value().z);
        REQUIRE(sample.position.y == 1.0);
    }

    const auto revision = selected.project_revision;
    const auto missing_anchor_samples = [&samples]() {
        auto result = samples.value();
        result.front().surface_anchor.reset();
        return result;
    }();
    const auto missing_anchor = ui.dispatch(
        carto::application::AppendSculptSurfaceStrokeAction{
            revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
            2.5, 0.05, missing_anchor_samples});
    REQUIRE(!missing_anchor);
    REQUIRE(ui.snapshot().project_revision == revision);
    REQUIRE(ui.snapshot().sculpt_layers.empty());

    auto wrong_source_samples = samples.value();
    wrong_source_samples.front().position.x += 0.01;
    const auto stale_anchor = ui.dispatch(
        carto::application::AppendSculptSurfaceStrokeAction{
            revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
            2.5, 0.05, wrong_source_samples});
    REQUIRE(!stale_anchor);
    REQUIRE(stale_anchor.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == revision);

    const auto committed = ui.dispatch(carto::application::AppendSculptSurfaceStrokeAction{
        revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
        2.5, 0.05, samples.value()});
    REQUIRE(committed);
    REQUIRE(committed.value().action == "Add Sculpt Surface Stroke");
    REQUIRE(committed.value().document_changed);
    REQUIRE(!ui.snapshot().operations.empty());
    REQUIRE(ui.snapshot().operations.back().action == "Add Sculpt Surface Stroke");
    const auto sculpted = ui.snapshot();
    const auto* sculpted_instance = sculpted.viewport.scene.find(object);
    REQUIRE(sculpted_instance != nullptr);
    REQUIRE(sculpted_instance->mesh != nullptr);
    REQUIRE(sculpted_instance->mesh->positions.size() == source_positions.size());
    REQUIRE(!same_positions(sculpted_instance->mesh->positions, source_positions));
    const auto authored_revision = ui.snapshot().project_revision;

    const auto ai_mutation = ui.submit_ai_proposal(
        carto::application::AppendSculptSurfaceStrokeAction{
            authored_revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
            2.5, 0.05, samples.value()});
    REQUIRE(!ai_mutation);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto stored = ui.snapshot();
    REQUIRE(stored.sculpt_layers.size() == 1U);
    REQUIRE(stored.sculpt_layers.front().stroke_count == 1U);
    const auto stale = ui.dispatch(carto::application::AppendSculptSurfaceStrokeAction{
        revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
        2.5, 0.05, samples.value()});
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto path = temp.path() / "surface-sculpt.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    const auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    const auto decoded = carto::production::SculptStack::deserialize(
        reopened.value().sculpt_stacks().at(mesh_asset).bytes);
    REQUIRE(decoded);
    const auto& stroke = decoded.value().layers.front().strokes.front();
    REQUIRE(stroke.samples == samples.value());
    const auto evaluated = carto::production::compile_sculpt_stack(
        decoded.value(), reopened.value().meshes().at(mesh_asset));
    REQUIRE(evaluated);
    REQUIRE(evaluated.value().maximum_displacement > 0.0);
}

void sculpt_point_strokes_are_revision_bound_human_commands() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Sculpt workspace test"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Sculpt mesh", {2.0, 2.0, 2.0}}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));

    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    const carto::scene::ObjectId object = snapshot.objects.front().object.id;
    const std::uint64_t mesh_asset = *snapshot.objects.front().object.mesh_asset;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));

    const auto selected = ui.snapshot();
    const auto* source_instance = selected.viewport.scene.find(object);
    REQUIRE(source_instance != nullptr);
    REQUIRE(source_instance->mesh != nullptr);
    const auto source_positions = source_instance->mesh->positions;
    const auto original_revision = selected.project_revision;
    const auto invalid_mode = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, static_cast<carto::production::SculptStrokeMode>(99U), 0.5, 0.05});
    REQUIRE(!invalid_mode);
    REQUIRE(ui.snapshot().project_revision == original_revision);
    const auto invalid_radius = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, carto::production::SculptStrokeMode::add, -1.0, 0.05});
    REQUIRE(!invalid_radius);
    REQUIRE(ui.snapshot().project_revision == original_revision);

    const auto raw_ai = ui.submit_ai_proposal(
        carto::application::AppendSculptStrokeAction{
            original_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05});
    REQUIRE(!raw_ai);
    REQUIRE(ui.snapshot().project_revision == original_revision);

    const auto committed = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05});
    REQUIRE(committed);
    REQUIRE(committed.value().action == "Add Sculpt Stroke");
    REQUIRE(committed.value().document_changed);
    REQUIRE(committed.value().affected.objects == std::vector<carto::scene::ObjectId>{object});
    REQUIRE(committed.value().affected.vertices ==
        std::vector<carto::geometry::VertexId>{carto::geometry::VertexId{1U}});
    const auto authored_revision = ui.snapshot().project_revision;
    REQUIRE(authored_revision > original_revision);

    const auto stale = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05});
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto layer_snapshot = ui.snapshot();
    REQUIRE(layer_snapshot.sculpt_mesh_asset == mesh_asset);
    REQUIRE(layer_snapshot.sculpt_layers.size() == 1U);
    REQUIRE(!layer_snapshot.sculpt_layers.front().identity.empty());
    REQUIRE(layer_snapshot.sculpt_layers.front().visible);
    REQUIRE(layer_snapshot.sculpt_layers.front().stroke_count == 1U);
    const auto path = temp.path() / "sculpt.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));

    const auto wrong_mesh = ui.dispatch(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset + 1U,
            layer_snapshot.sculpt_layers.front().identity, false});
    REQUIRE(!wrong_mesh);
    REQUIRE(wrong_mesh.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto missing_layer = ui.dispatch(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset, "missing.layer", false});
    REQUIRE(!missing_layer);
    REQUIRE(missing_layer.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(ui.snapshot().project_revision == authored_revision);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);

    const auto ai_visibility = ui.submit_ai_proposal(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset,
            layer_snapshot.sculpt_layers.front().identity, false});
    REQUIRE(!ai_visibility);
    REQUIRE(ui.snapshot().project_revision == authored_revision);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);

    const auto hidden = ui.dispatch(carto::application::SetSculptLayerVisibilityAction{
        authored_revision, mesh_asset,
        layer_snapshot.sculpt_layers.front().identity, false});
    REQUIRE(hidden);
    REQUIRE(hidden.value().action == "Set Sculpt Layer Visibility");
    REQUIRE(hidden.value().document_changed);
    REQUIRE(ui.snapshot().operations.back().action == "Set Sculpt Layer Visibility");
    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries);
    REQUIRE(journal_entries.value().back().event_type == "cartographer.application_action");
    const std::string journal_payload(
        journal_entries.value().back().payload.begin(),
        journal_entries.value().back().payload.end());
    REQUIRE(journal_payload.find("operation=Set Sculpt Layer Visibility") != std::string::npos);
    const auto hidden_revision = ui.snapshot().project_revision;
    REQUIRE(!ui.snapshot().sculpt_layers.front().visible);
    const auto hidden_view = ui.snapshot();
    const auto* hidden_instance = hidden_view.viewport.scene.find(object);
    REQUIRE(hidden_instance != nullptr);
    REQUIRE(hidden_instance->mesh != nullptr);
    REQUIRE(same_positions(hidden_instance->mesh->positions, source_positions));

    const auto stale_visibility = ui.dispatch(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset,
            layer_snapshot.sculpt_layers.front().identity, true});
    REQUIRE(!stale_visibility);
    REQUIRE(stale_visibility.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == hidden_revision);
    REQUIRE(!ui.snapshot().sculpt_layers.front().visible);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().project_revision > authored_revision);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().project_revision > authored_revision);
    REQUIRE(!ui.snapshot().sculpt_layers.front().visible);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));

    const auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().sculpt_stacks().contains(mesh_asset));
    const auto& stored = reopened.value().sculpt_stacks().at(mesh_asset);
    const auto stack = carto::production::SculptStack::deserialize(stored.bytes);
    REQUIRE(stack);
    REQUIRE(stack.value().validate());
    REQUIRE(stack.value().layers.size() == 1U);
    REQUIRE(stack.value().layers.front().strokes.size() == 1U);
    REQUIRE(!stack.value().layers.front().visible);
    const auto mesh = reopened.value().meshes().find(mesh_asset);
    REQUIRE(mesh != reopened.value().meshes().end());
    const auto evaluation = carto::production::compile_sculpt_stack(stack.value(), mesh->second);
    REQUIRE(evaluation);
    REQUIRE(evaluation.value().maximum_displacement == 0.0);

    const auto enabled = ui.dispatch(carto::application::SetSculptLayerVisibilityAction{
        ui.snapshot().project_revision, mesh_asset,
        layer_snapshot.sculpt_layers.front().identity, true});
    REQUIRE(enabled);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);
    const auto enabled_view = ui.snapshot();
    const auto* enabled_instance = enabled_view.viewport.scene.find(object);
    REQUIRE(enabled_instance != nullptr);
    REQUIRE(enabled_instance->mesh != nullptr);
    REQUIRE(!same_positions(enabled_instance->mesh->positions, source_positions));
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    const auto reopened_enabled = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened_enabled);
    const auto enabled_stack = carto::production::SculptStack::deserialize(
        reopened_enabled.value().sculpt_stacks().at(mesh_asset).bytes);
    REQUIRE(enabled_stack);
    REQUIRE(enabled_stack.value().layers.front().visible);
    const auto enabled_mesh = reopened_enabled.value().meshes().find(mesh_asset);
    REQUIRE(enabled_mesh != reopened_enabled.value().meshes().end());
    const auto enabled_evaluation = carto::production::compile_sculpt_stack(
        enabled_stack.value(), enabled_mesh->second);
    REQUIRE(enabled_evaluation);
    REQUIRE(enabled_evaluation.value().maximum_displacement > 0.0);

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    const auto no_vertex = ui.dispatch(carto::application::AppendSculptStrokeAction{
        ui.snapshot().project_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05});
    REQUIRE(!no_vertex);
    REQUIRE(ui.snapshot().project_revision == reopened_enabled.value().revision());
}

} // namespace

int main() {
    try {
        preferences_validate_shell_invariants();
        ui_routes_authoritative_actions_and_operation_lineage();
        ui_routes_spatial_fields_through_the_human_command_boundary();
        ui_distinguishes_unsaved_from_saved_state();
        ui_rejects_unadmitted_ai_proposals();
        ui_routes_edge_selection_without_authoring_mutation();
        ui_preserves_required_viewport_and_supports_mode_theme_density();
        cartographer_brand_palette_is_stable();
        ui_persists_preferences_and_resets_corrupt_state();
        workbench_preferences_round_trip_and_fail_closed();
        ui_connects_surface_fidelity_preview_without_authoring_mutation();
        ui_imports_mesh_through_provider_and_authoring_boundaries();
        ui_connects_local_market_boundary_and_fails_closed();
        ui_failed_shortcuts_and_actions_remain_bounded();
        ui_command_palette_is_bounded_and_explicit_about_deferred_features();
        native_key_contract_maps_core_control_without_imgui();
        workspace_registry_is_canonical_and_fail_closed();
        sculpt_viewport_sampling_preserves_frontmost_surface_and_path_spacing();
        sculpt_surface_strokes_commit_anchored_samples_through_human_commands();
        sculpt_point_strokes_are_revision_bound_human_commands();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
