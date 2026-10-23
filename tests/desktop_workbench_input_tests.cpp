#include <carto/ui/ui.hpp>

#include <imgui.h>

#include "../apps/desktop/carto_studio_dock.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

ImGuiViewport* recovered_platform_viewport = nullptr;
ImVec2 recovered_platform_position{};
int recovered_platform_position_calls = 0;

void record_platform_window_position(ImGuiViewport* viewport, ImVec2 position) {
    recovered_platform_viewport = viewport;
    recovered_platform_position = position;
    ++recovered_platform_position_calls;
}

struct GripFrame {
    ImVec2 minimum{};
    ImVec2 maximum{};
    ImVec2 move_minimum{};
    ImVec2 move_maximum{};
    float available_width = 0.0F;
    bool hovered = false;
    bool active = false;
    bool activated = false;
    bool visible = false;
    bool pressed = false;
    bool move_visible = false;
    bool move_pressed = false;
    bool stacked = false;
};

GripFrame draw_grip_frame(ImVec2 pointer, bool primary_down,
                          float window_width = 260.0F) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, primary_down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(window_width, 140.0F), ImGuiCond_Always);
    ImGui::Begin("Workbench instrument test", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetCursorPos(ImVec2(8.0F, 30.0F));
    const float available_width = ImGui::GetContentRegionAvail().x;
    const bool stacked = carto::ui::workbench_instrument_drag_controls_stacked(
        available_width);
    const float move_width = stacked
        ? available_width
        : std::min(68.0F, std::max(44.0F, available_width * 0.30F));
    const float grip_width = stacked
        ? available_width
        : static_cast<float>(carto::ui::workbench_instrument_drag_grip_width(
            available_width - move_width - ImGui::GetStyle().ItemSpacing.x));
    const std::string grip_label =
        std::string(carto::ui::workbench_instrument_drag_grip_label(grip_width)) +
        "##instrument_move_grip";
    const bool pressed = ImGui::Button(grip_label.c_str(), ImVec2(
        grip_width,
        static_cast<float>(carto::ui::kWorkbenchInstrumentDragGripHeight)));
    const ImVec2 grip_minimum = ImGui::GetItemRectMin();
    const ImVec2 grip_maximum = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    const bool visible = ImGui::IsItemVisible();
    if (!stacked) ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
    const bool move_pressed = ImGui::Button("MOVE##instrument_move_mode", ImVec2(
        move_width,
        static_cast<float>(carto::ui::kWorkbenchInstrumentDragGripHeight)));
    const ImVec2 move_minimum = ImGui::GetItemRectMin();
    const ImVec2 move_maximum = ImGui::GetItemRectMax();
    const GripFrame result{
        grip_minimum, grip_maximum, move_minimum, move_maximum, available_width,
        hovered, active, activated, visible, pressed, ImGui::IsItemVisible(), move_pressed,
        stacked};
    ImGui::End();
    ImGui::Render();
    return result;
}

ImVec2 draw_fixed_titlebar_frame(ImVec2 pointer, bool primary_down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, primary_down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(80.0F, 60.0F), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(260.0F, 140.0F), ImGuiCond_Once);
    ImGui::Begin("Movable workbench instrument", nullptr,
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 position = ImGui::GetWindowPos();
    ImGui::End();
    ImGui::Render();
    return position;
}

struct NativeTitlebarFrame {
    ImVec2 position{};
    bool moving = false;
    bool pointer_dragging = false;
    bool released = false;
};

NativeTitlebarFrame draw_movable_titlebar_frame(ImVec2 pointer, bool primary_down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, primary_down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(500.0F, 400.0F), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(260.0F, 140.0F), ImGuiCond_Once);
    ImGui::Begin("Snap-aware floating instrument", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const NativeTitlebarFrame frame{
        ImGui::GetWindowPos(),
        GImGui->MovingWindow == window,
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0F),
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)};
    ImGui::End();
    ImGui::Render();
    return frame;
}

ImVec2 draw_captured_instrument_frame(
    ImVec2 position, ImVec2 pointer, bool primary_down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, primary_down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260.0F, 140.0F), ImGuiCond_Always);
    ImGui::Begin("Captured workbench instrument", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 actual_position = ImGui::GetWindowPos();
    ImGui::End();
    ImGui::Render();
    return actual_position;
}

void require(bool condition, const char* message);

ImGuiID draw_docking_frame(carto::studio::DockingWorkspace& workspace) {
    ImGui::NewFrame();
    workspace.begin_root();
    ImGuiWindow* host = ImGui::FindWindowByName(
        "Cartographer Studio Dock Host##host");
    const ImGuiID dock_id = host == nullptr
        ? 0U : host->GetID("CartographerStudioDockV1");
    for (const char* name : {"Viewport", "Scene", "Inspector", "Assets", "History"}) {
        if (ImGui::Begin(name)) ImGui::TextUnformatted("Dock test panel");
        ImGui::End();
    }
    ImGui::Render();
    return dock_id;
}

void require_initial_dock_layout(ImGuiID dock_id) {
    const ImGuiDockNode* const root = ImGui::DockBuilderGetNode(dock_id);
    require(dock_id != 0U && root != nullptr,
        "the workspace host must retain a dock-space root");
    for (const char* name : {"Viewport", "Scene", "Inspector", "Assets", "History"}) {
        const ImGuiWindow* panel = ImGui::FindWindowByName(name);
        require(panel != nullptr && panel->DockNode != nullptr,
            "every initial workbench panel must be assigned to the dock tree");
    }

    const ImGuiWindow* const viewport = ImGui::FindWindowByName("Viewport");
    const ImGuiWindow* const scene = ImGui::FindWindowByName("Scene");
    const ImGuiWindow* const inspector = ImGui::FindWindowByName("Inspector");
    const ImGuiWindow* const assets = ImGui::FindWindowByName("Assets");
    const ImGuiWindow* const history = ImGui::FindWindowByName("History");
    require(viewport->DockNode->IsCentralNode(),
        "the viewport must occupy the central work-surface node");
    require(scene->DockNode != viewport->DockNode &&
            inspector->DockNode != viewport->DockNode &&
            assets->DockNode != viewport->DockNode,
        "scene, inspector, and ledger tabs must remain outside the central viewport node");
    require(scene->DockNode->Pos.x < viewport->DockNode->Pos.x &&
            inspector->DockNode->Pos.x > viewport->DockNode->Pos.x &&
            assets->DockNode->Pos.y > viewport->DockNode->Pos.y,
        "the initial layout must place Scene left, Inspector right, and Assets/History below");
    require(assets->DockNode == history->DockNode,
        "Assets and History must start as tabs in the same bottom dock node");
    require(root->IsSplitNode(),
        "the initial layout must retain a split workbench rather than a single stacked node");
}

void require_docking_monitor_recovery() {
    ImGuiPlatformMonitor monitors[2]{};
    monitors[0].WorkPos = ImVec2(0.0F, 0.0F);
    monitors[0].WorkSize = ImVec2(1920.0F, 1080.0F);
    monitors[1].WorkPos = ImVec2(-1280.0F, 0.0F);
    monitors[1].WorkSize = ImVec2(1280.0F, 1024.0F);

    const ImVec2 negative_origin{-1100.0F, 120.0F};
    const ImVec2 window_size{640.0F, 420.0F};
    const ImVec2 on_negative_monitor =
        carto::studio::DockingWorkspace::recover_window_position(
            negative_origin, window_size, monitors, 2);
    require(on_negative_monitor.x == negative_origin.x &&
            on_negative_monitor.y == negative_origin.y,
        "a viewport on a negative-origin monitor must not be relocated");

    const ImVec2 sufficiently_visible{1870.0F, 240.0F};
    const ImVec2 kept_visible =
        carto::studio::DockingWorkspace::recover_window_position(
            sufficiently_visible, window_size, monitors, 2);
    require(kept_visible.x == sufficiently_visible.x &&
            kept_visible.y == sufficiently_visible.y,
        "a detached viewport with a useful visible strip must keep its placement");

    const ImVec2 barely_visible{1876.0F, 1400.0F};
    const ImVec2 recovered =
        carto::studio::DockingWorkspace::recover_window_position(
            barely_visible, window_size, monitors, 2);
    require(recovered.x == 1280.0F && recovered.y == 660.0F,
        "a window that fits must be recovered fully inside its nearest monitor");

    const ImVec2 oversized =
        carto::studio::DockingWorkspace::recover_window_position(
            barely_visible, ImVec2(2200.0F, 1400.0F), monitors, 2);
    require(oversized.x == 1872.0F && oversized.y == 1048.0F,
        "an oversized viewport must retain a draggable strip without inventing a smaller size");

    const ImVec2 no_monitor =
        carto::studio::DockingWorkspace::recover_window_position(
            barely_visible, window_size, nullptr, 0);
    require(no_monitor.x == barely_visible.x && no_monitor.y == barely_visible.y,
        "missing monitor data must preserve the saved placement instead of inventing bounds");

    ImGuiPlatformMonitor invalid_monitor{};
    const ImVec2 invalid_bounds =
        carto::studio::DockingWorkspace::recover_window_position(
            barely_visible, window_size, &invalid_monitor, 1);
    require(invalid_bounds.x == barely_visible.x && invalid_bounds.y == barely_visible.y,
        "malformed monitor bounds must be ignored safely");

    const ImVec2 malformed_position{
        std::numeric_limits<float>::quiet_NaN(), barely_visible.y};
    const ImVec2 malformed =
        carto::studio::DockingWorkspace::recover_window_position(
            malformed_position, window_size, monitors, 2);
    require(std::isnan(malformed.x) && malformed.y == malformed_position.y,
        "non-finite saved coordinates must be rejected without further corruption");
}

void require_live_docking_monitor_recovery() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    ImGuiViewport* const main = ImGui::GetMainViewport();
    require(main != nullptr, "the live recovery test requires ImGui's main viewport");

    ImGuiViewport visible{};
    visible.ID = 0xCA470001U;
    visible.Pos = {20.0F, 30.0F};
    visible.Size = {640.0F, 420.0F};
    visible.PlatformHandle = &visible;
    ImGuiViewport offscreen{};
    offscreen.ID = 0xCA470002U;
    offscreen.Pos = {4000.0F, 4000.0F};
    offscreen.Size = {640.0F, 420.0F};
    offscreen.PlatformHandle = &offscreen;

    ImGuiPlatformMonitor monitor{};
    monitor.WorkPos = {0.0F, 0.0F};
    monitor.WorkSize = {1920.0F, 1080.0F};
    platform.Viewports.clear();
    platform.Viewports.push_back(main);
    platform.Viewports.push_back(&visible);
    platform.Viewports.push_back(&offscreen);
    platform.Monitors.clear();
    platform.Monitors.push_back(monitor);
    platform.Platform_SetWindowPos = &record_platform_window_position;
    recovered_platform_viewport = nullptr;
    recovered_platform_position = {};
    recovered_platform_position_calls = 0;

    const ImGuiConfigFlags original_flags = io.ConfigFlags;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    carto::studio::DockingWorkspace::recover_offscreen_platform_windows();
    require(offscreen.Pos.x == 4000.0F && offscreen.Pos.y == 4000.0F &&
            recovered_platform_position_calls == 0,
        "monitor recovery must remain inert when native platform windows are disabled");

    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    carto::studio::DockingWorkspace::recover_offscreen_platform_windows();
    require(visible.Pos.x == 20.0F && visible.Pos.y == 30.0F,
        "a still-visible native viewport must not be repositioned");
    require(offscreen.Pos.x == 1280.0F && offscreen.Pos.y == 660.0F,
        "an offscreen native viewport must be recovered using the live monitor bounds");
    require(recovered_platform_position_calls == 1 &&
            recovered_platform_viewport == &offscreen &&
            recovered_platform_position.x == offscreen.Pos.x &&
            recovered_platform_position.y == offscreen.Pos.y,
        "recovery must update only the detached OS window through its platform callback");

    io.ConfigFlags = original_flags;
    platform.Platform_SetWindowPos = nullptr;
    platform.Viewports.clear();
    platform.Monitors.clear();
}

void require_unique_docking_layout_write_paths() {
    const std::filesystem::path destination = "workspace-docking.ini";
    const auto first_instance =
        carto::studio::DockingWorkspace::layout_temporary_path(destination, 4100U, 0U);
    const auto second_instance =
        carto::studio::DockingWorkspace::layout_temporary_path(destination, 4101U, 0U);
    const auto second_write =
        carto::studio::DockingWorkspace::layout_temporary_path(destination, 4100U, 1U);
    require(first_instance != second_instance && first_instance != second_write &&
            second_instance != second_write,
        "concurrent instances and repeated writes must not share a docking temp path");
    require(first_instance.parent_path() == destination.parent_path() &&
            second_instance.parent_path() == destination.parent_path(),
        "atomic docking temp files must remain beside the destination");
}

void require_docking_layout_load_fail_closed() {
    using Presence = carto::studio::DockingWorkspace::LayoutPresence;
    const std::error_code no_error;
    const auto unreadable = std::make_error_code(std::errc::permission_denied);
    require(carto::studio::DockingWorkspace::classify_layout_presence(
                false, no_error) == Presence::missing,
        "only an error-free missing file may trigger the default layout");
    require(carto::studio::DockingWorkspace::classify_layout_presence(
                true, no_error) == Presence::present,
        "an error-free existing layout must be loaded");
    require(carto::studio::DockingWorkspace::classify_layout_presence(
                false, unreadable) == Presence::rejected &&
            carto::studio::DockingWorkspace::classify_layout_presence(
                true, unreadable) == Presence::rejected,
        "filesystem errors must be rejected regardless of the existence output");

    carto::studio::DockingWorkspace workspace;
    workspace.note_saved_layout_rejected();
    require(!workspace.layout_save_allowed(),
        "a rejected saved layout must not be overwritten automatically");
    workspace.request_reset();
    require(workspace.layout_save_allowed(),
        "an explicit layout reset must authorize replacing a rejected layout");
}

void require_docking_capture_loss_is_distinguished() {
    using State = carto::studio::DockingWorkspace::NativeCaptureState;
    require(carto::studio::DockingWorkspace::classify_native_capture(false, false) ==
                State::none &&
            carto::studio::DockingWorkspace::classify_native_capture(false, true) ==
                State::none,
        "an untracked capture must not trigger cancellation");
    require(carto::studio::DockingWorkspace::classify_native_capture(true, true) ==
                State::owned,
        "a still-owned capture must preserve the active gesture");
    require(carto::studio::DockingWorkspace::classify_native_capture(true, false) ==
                State::lost,
        "a tracked capture no longer owned by this process must be classified as lost");
}

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main() {
    ImGuiContext* context = ImGui::CreateContext();
    if (context == nullptr) return 2;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 480.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(io.Fonts->Build(), "ImGui test font atlas must build");

    const carto::ui::NativePointerEvent client_pointer{
        carto::ui::NativePointerButton::primary, 64, 96,
        carto::ui::kNativeModifierShift, true};
    const auto screen_pointer = carto::ui::native_pointer_event_to_screen_space(
        client_pointer, 1440, 80);
    require(screen_pointer.x == 1504 && screen_pointer.y == 176 &&
            screen_pointer.button == client_pointer.button &&
            screen_pointer.modifiers == client_pointer.modifiers &&
            screen_pointer.pressed == client_pointer.pressed,
        "client pointer translation must preserve event state and apply the desktop client origin");
    const auto saturated_pointer = carto::ui::native_pointer_event_to_screen_space(
        carto::ui::NativePointerEvent{
            carto::ui::NativePointerButton::primary,
            std::numeric_limits<std::int32_t>::max(),
            std::numeric_limits<std::int32_t>::min(), 0U, false},
        1, -1);
    require(saturated_pointer.x == std::numeric_limits<std::int32_t>::max() &&
            saturated_pointer.y == std::numeric_limits<std::int32_t>::min(),
        "screen coordinate translation must not overflow at integer boundaries");
    const std::array<carto::ui::WorkbenchInstrumentDragTarget, 1> client_drag_targets{{
        {carto::ui::WorkbenchInstrument::tools, 0, true,
         {48.0, 80.0}, {80.0, 112.0},
         {48.0, 40.0}, {104.0, 64.0}, false}}};
    require(carto::ui::topmost_workbench_drag_target(
                {static_cast<double>(client_pointer.x),
                 static_cast<double>(client_pointer.y)}, client_drag_targets) ==
            carto::ui::WorkbenchInstrument::tools,
        "client-local Win32 pointer coordinates must hit ImGui-local instrument grips when the app is offset");
    const std::array<carto::ui::WorkbenchInstrumentDragTarget, 1> client_titlebar_targets{{
        {carto::ui::WorkbenchInstrument::tools, 0, true,
         {48.0, 80.0}, {80.0, 112.0},
         {48.0, 40.0}, {104.0, 64.0}, true}}};
    require(carto::ui::topmost_workbench_drag_target(
                {70.0, 52.0}, client_titlebar_targets) ==
            carto::ui::WorkbenchInstrument::tools &&
            !carto::ui::topmost_workbench_drag_grip_target(
                {70.0, 52.0}, client_titlebar_targets).has_value(),
        "the native captured-drag path must leave titlebar movement to ImGui");
    require(carto::ui::topmost_workbench_drag_grip_target(
                {64.0, 96.0}, client_titlebar_targets) ==
            carto::ui::WorkbenchInstrument::tools,
        "the explicit visible grip must still start the native captured-drag path");
    const std::array<carto::ui::WorkbenchInstrumentDragTarget, 2> occluded_grips{{
        {carto::ui::WorkbenchInstrument::tools, 0, true,
         {48.0, 80.0}, {80.0, 112.0}, {48.0, 40.0}, {104.0, 64.0}, false,
         {40.0, 32.0}, {180.0, 180.0}, true},
        {carto::ui::WorkbenchInstrument::inspector, 1, true,
         {120.0, 80.0}, {160.0, 112.0}, {40.0, 40.0}, {180.0, 64.0}, true,
         {40.0, 32.0}, {180.0, 180.0}, true}}};
    require(!carto::ui::topmost_workbench_drag_grip_target(
                {64.0, 96.0}, occluded_grips).has_value() &&
            !carto::ui::topmost_workbench_drag_target(
                {64.0, 96.0}, occluded_grips).has_value(),
        "a lower instrument grip must not receive a drag through an occluding front instrument");
    auto frontmost_grip = occluded_grips;
    frontmost_grip[1].grip_minimum = {48.0, 80.0};
    frontmost_grip[1].grip_maximum = {80.0, 112.0};
    require(carto::ui::topmost_workbench_drag_grip_target(
                {64.0, 96.0}, frontmost_grip) ==
            carto::ui::WorkbenchInstrument::inspector,
        "overlapping grips must select the frontmost instrument, not the first enumerated one");
    require(!carto::ui::topmost_workbench_drag_target(
                {70.0, 52.0}, client_drag_targets).has_value(),
        "native titlebar movement must not be captured by the custom grip path");
    require(!carto::ui::topmost_workbench_drag_target(
                {110.0, 52.0}, client_drag_targets).has_value(),
        "the title-bar close-button zone must remain outside the drag target");
    require(!carto::ui::topmost_workbench_drag_target(
                {static_cast<double>(screen_pointer.x),
                 static_cast<double>(screen_pointer.y)}, client_drag_targets).has_value(),
        "desktop screen coordinates must not be fed to ImGui-local instrument hit targets");

    const GripFrame initial = draw_grip_frame(ImVec2(100.0F, 80.0F), false);
    const float rendered_width = initial.maximum.x - initial.minimum.x;
    require(initial.available_width > 1.0F && rendered_width > 100.0F,
        "visible drag grip must remain the primary control on a standard instrument");
    require(initial.maximum.y - initial.minimum.y >=
        static_cast<float>(carto::ui::kWorkbenchInstrumentDragGripHeight) - 0.01F,
        "drag grip must have a positive hit-test height");
    require(initial.visible,
        "drag grip must be a visible ImGui control, not only an invisible hit target");
    require(initial.move_visible && initial.move_maximum.x > initial.move_minimum.x,
        "each instrument must expose a separate visible move/placement control");
    require(std::string_view(carto::ui::workbench_instrument_drag_grip_label(rendered_width)) ==
            "DRAG TO MOVE · SNAP",
        "wide instruments must show the full drag and snap affordance");
    const ImVec2 edge_pointer(initial.maximum.x - 1.0F,
        (initial.minimum.y + initial.maximum.y) * 0.5F);
    const GripFrame hovered = draw_grip_frame(edge_pointer, false);
    require(hovered.hovered,
        "the far edge of the visible drag grip must be hoverable");
    const GripFrame held = draw_grip_frame(edge_pointer, true);
    require(held.hovered && held.active,
        "pressing the far edge of the visible drag grip must capture input");
    require(held.activated,
        "pressing the visible drag grip must produce the activation used to start a move");
    const GripFrame dragged = draw_grip_frame(ImVec2(
        edge_pointer.x + 36.0F, edge_pointer.y + 20.0F), true);
    require(dragged.active,
        "a captured drag grip must remain active after the pointer leaves its bounds");
    const GripFrame released = draw_grip_frame(
        ImVec2(edge_pointer.x + 36.0F, edge_pointer.y + 20.0F), false);
    require(!released.active,
        "releasing a dragged grip must clear ImGui's active mouse state");
    const GripFrame pressed_again = draw_grip_frame(edge_pointer, true);
    require(pressed_again.hovered && pressed_again.active && pressed_again.activated,
        "a grip must accept another drag after the previous release");
    const GripFrame click_release = draw_grip_frame(edge_pointer, false);
    require(click_release.pressed,
        "clicking the visible grip must remain a normal button release when no drag occurs");

    const GripFrame narrow = draw_grip_frame(ImVec2(80.0F, 80.0F), false, 140.0F);
    const float narrow_grip_width = narrow.maximum.x - narrow.minimum.x;
    require(narrow.visible && narrow.move_visible && narrow_grip_width >= 36.0F &&
            narrow.move_maximum.x > narrow.move_minimum.x,
        "narrow instruments must retain both operable drag and move controls");
    require(narrow.stacked && narrow.move_minimum.y > narrow.maximum.y,
        "narrow instruments must stack drag controls so neither affordance is clipped");
    require(std::string_view(carto::ui::workbench_instrument_drag_grip_label(
                narrow_grip_width)) == "DRAG",
        "narrow instruments must use a short visible grip label instead of clipping it");
    const ImVec2 move_button_center(
        (narrow.move_minimum.x + narrow.move_maximum.x) * 0.5F,
        (narrow.move_minimum.y + narrow.move_maximum.y) * 0.5F);
    static_cast<void>(draw_grip_frame(move_button_center, true, 140.0F));
    const GripFrame move_click = draw_grip_frame(move_button_center, false, 140.0F);
    require(move_click.move_pressed,
        "the dedicated MOVE button must enter placement through an ordinary click");

    const ImVec2 before_titlebar_drag = draw_fixed_titlebar_frame(
        ImVec2(500.0F, 400.0F), false);
    static_cast<void>(draw_fixed_titlebar_frame(ImVec2(120.0F, 68.0F), true));
    const ImVec2 during_titlebar_drag = draw_fixed_titlebar_frame(
        ImVec2(58.0F, 98.0F), true);
    static_cast<void>(draw_fixed_titlebar_frame(ImVec2(58.0F, 98.0F), false));
    require(std::abs(during_titlebar_drag.x - before_titlebar_drag.x) < 0.01F &&
        std::abs(during_titlebar_drag.y - before_titlebar_drag.y) < 0.01F,
        "instrument title bars must not race the single captured-drag owner");
    const auto titlebar_target = carto::ui::topmost_workbench_drag_target(
        {70.0, 52.0}, client_titlebar_targets);
    require(titlebar_target == carto::ui::WorkbenchInstrument::tools,
        "the visible title bar must enter the same native drag path as the grip");
    const auto titlebar_drag_position = carto::ui::workbench_dragged_position(
        {before_titlebar_drag.x, before_titlebar_drag.y}, {70.0, 52.0}, {12.0, 76.0});
    const auto titlebar_release = carto::ui::workbench_release_position(
        true, {12.0, 76.0}, {before_titlebar_drag.x, before_titlebar_drag.y});
    require(titlebar_release.x == 12.0 && titlebar_release.y == 76.0,
        "titlebar capture must settle from the actual native release point");
    const auto titlebar_snap = carto::ui::workbench_instrument_snap_position(
        titlebar_drag_position, {260.0, 140.0},
        {0.0, 0.0}, {640.0, 480.0},
        std::span<const carto::ui::WorkbenchSnapWindow>{}, 8.0);
    require(titlebar_snap.has_value() && titlebar_snap->x == 12.0 &&
        titlebar_snap->y == 84.0,
        "a moved titlebar must use captured movement and settle to the work-surface snap edge");
    static_cast<void>(draw_movable_titlebar_frame({510.0F, 410.0F}, false));
    const auto titlebar_click = draw_movable_titlebar_frame({510.0F, 410.0F}, true);
    const auto titlebar_click_release = draw_movable_titlebar_frame({510.0F, 410.0F}, false);
    require(!titlebar_click.pointer_dragging && titlebar_click_release.released &&
            !carto::ui::workbench_instrument_should_settle(
                false, titlebar_click.moving && titlebar_click.pointer_dragging,
                false, titlebar_click_release.released),
        "a titlebar click without movement must not trigger snap settlement");
    static_cast<void>(draw_movable_titlebar_frame({510.0F, 410.0F}, true));
    static_cast<void>(draw_movable_titlebar_frame({580.0F, 455.0F}, true));
    const auto titlebar_moved = draw_movable_titlebar_frame({580.0F, 455.0F}, true);
    const auto titlebar_released = draw_movable_titlebar_frame({580.0F, 455.0F}, false);
    require(titlebar_moved.moving,
        "ImGui must identify the floating instrument as the active moving window");
    require(titlebar_moved.pointer_dragging,
        "a titlebar press must cross ImGui's movement threshold before snap tracking begins");
    require(std::abs(titlebar_moved.position.x - titlebar_click.position.x) > 3.0F ||
            std::abs(titlebar_moved.position.y - titlebar_click.position.y) > 3.0F,
        "ImGui must update a floating instrument's titlebar position while dragging");
    require(titlebar_released.released &&
            carto::ui::workbench_instrument_should_settle(
                false, titlebar_moved.moving && titlebar_moved.pointer_dragging,
                false, titlebar_released.released),
        "a moved floating titlebar must enter Cartographer's snap settlement on release");
    require(carto::ui::workbench_instrument_should_settle(
                true, false, false, false),
        "a native captured release must settle even when ImGui does not own the mouse-up event");
    const std::array<carto::ui::WorkbenchSnapWindow, 1U> live_default_panel{{
        {{0.0, 0.0}, {240.0, 180.0}}}};
    const auto default_panel_snap = carto::ui::workbench_instrument_snap_position(
        {252.0, 96.0}, {180.0, 120.0}, {0.0, 0.0}, {640.0, 480.0},
        live_default_panel, 10.0);
    require(default_panel_snap.has_value() && default_panel_snap->x == 250.0 &&
        default_panel_snap->y == 96.0,
        "a live instrument at its unpersisted default position must be a reachable side-snap target");
    const ImVec2 after_outside_release = draw_fixed_titlebar_frame(
        ImVec2(620.0F, 460.0F), false);
    require(std::abs(after_outside_release.x - during_titlebar_drag.x) < 0.01F &&
        std::abs(after_outside_release.y - during_titlebar_drag.y) < 0.01F,
        "releasing a moved instrument outside its window must keep the moved position");

    const ImVec2 captured_origin(80.0F, 60.0F);
    const ImVec2 captured_pointer_origin(120.0F, 68.0F);
    const ImVec2 captured_pointer_current(180.0F, 98.0F);
    static_cast<void>(draw_captured_instrument_frame(
        captured_origin, captured_pointer_origin, false));
    const auto requested_position = carto::ui::workbench_dragged_position(
        {captured_origin.x, captured_origin.y},
        {captured_pointer_origin.x, captured_pointer_origin.y},
        {captured_pointer_current.x, captured_pointer_current.y});
    const ImVec2 requested(
        static_cast<float>(requested_position.x),
        static_cast<float>(requested_position.y));
    const ImVec2 captured_move = draw_captured_instrument_frame(
        requested, captured_pointer_current, true);
    require(std::abs(captured_move.x - requested.x) < 0.01F &&
        std::abs(captured_move.y - requested.y) < 0.01F,
        "native captured dragging must move titlebar/grip-owned NoMove instruments");
    const ImVec2 captured_release = draw_captured_instrument_frame(
        requested, captured_pointer_current, false);
    require(std::abs(captured_release.x - requested.x) < 0.01F &&
        std::abs(captured_release.y - requested.y) < 0.01F,
        "a captured instrument position must remain settled after release");

    carto::studio::DockingWorkspace docking;
    ImGuiID dock_id = draw_docking_frame(docking);
    dock_id = draw_docking_frame(docking);
    require_initial_dock_layout(dock_id);
    std::size_t saved_layout_size = 0U;
    const char* saved_layout = ImGui::SaveIniSettingsToMemory(&saved_layout_size);
    const std::string_view saved_layout_text(
        saved_layout == nullptr ? "" : saved_layout,
        saved_layout == nullptr ? 0U : saved_layout_size);
    require(saved_layout != nullptr && saved_layout_size > 0U &&
            saved_layout_text.find("DockId=") != std::string_view::npos,
        "the docked panel topology must serialize through ImGui's layout settings");
    docking.toggle_focus();
    require(docking.focus_mode(), "viewport focus mode must enter on toggle");
    const ImGuiID focused_dock_id = draw_docking_frame(docking);
    require(focused_dock_id == dock_id &&
            ImGui::DockBuilderGetNode(focused_dock_id) != nullptr,
        "viewport focus mode must keep the underlying dock topology alive");
    docking.request_reset();
    require(!docking.focus_mode(), "reset must leave viewport focus mode");
    dock_id = draw_docking_frame(docking);
    dock_id = draw_docking_frame(docking);
    require_initial_dock_layout(dock_id);
    require_docking_monitor_recovery();
    require_live_docking_monitor_recovery();
    require_unique_docking_layout_write_paths();
    require_docking_layout_load_fail_closed();
    require_docking_capture_loss_is_distinguished();

    ImGui::DestroyContext(context);
    return 0;
}
