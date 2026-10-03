#include <carto/ui/ui.hpp>

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

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
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
    REQUIRE(registry.size() == 8U);
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
}

} // namespace

int main() {
    try {
        preferences_validate_shell_invariants();
        ui_routes_authoritative_actions_and_operation_lineage();
        ui_distinguishes_unsaved_from_saved_state();
        ui_rejects_unadmitted_ai_proposals();
        ui_routes_edge_selection_without_authoring_mutation();
        ui_preserves_required_viewport_and_supports_mode_theme_density();
        ui_persists_preferences_and_resets_corrupt_state();
        workbench_preferences_round_trip_and_fail_closed();
        ui_failed_shortcuts_and_actions_remain_bounded();
        ui_command_palette_is_bounded_and_explicit_about_deferred_features();
        native_key_contract_maps_core_control_without_imgui();
        workspace_registry_is_canonical_and_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
