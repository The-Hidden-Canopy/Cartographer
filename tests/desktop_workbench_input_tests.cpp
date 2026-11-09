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

struct DockingLayoutFrame {
    ImGuiID root = 0U;
    std::array<ImGuiID, 5U> panel_nodes{};
    ImVec2 floating_position{};
};

DockingLayoutFrame draw_docking_layout_frame(
    carto::studio::DockingWorkspace& workspace,
    ImVec2 first_use_floating_position) {
    ImGui::NewFrame();
    workspace.begin_root();
    ImGuiWindow* host = ImGui::FindWindowByName(
        "Cartographer Studio Dock Host##host");
    DockingLayoutFrame result;
    result.root = host == nullptr ? 0U : host->GetID("CartographerStudioDockV1");
    const std::array<const char*, 5U> names{
        "Viewport", "Scene", "Inspector", "Assets", "History"};
    for (std::size_t index = 0U; index < names.size(); ++index) {
        if (ImGui::Begin(names[index])) ImGui::TextUnformatted("Dock test panel");
        const ImGuiWindow* panel = ImGui::GetCurrentWindowRead();
        result.panel_nodes[index] = panel != nullptr && panel->DockNode != nullptr
            ? panel->DockNode->ID : 0U;
        ImGui::End();
    }
    ImGui::SetNextWindowPos(first_use_floating_position, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(180.0F, 110.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Floating Dock Persistence Probe");
    result.floating_position = ImGui::GetWindowPos();
    ImGui::TextUnformatted("Floating instrument");
    ImGui::End();
    ImGui::Render();
    return result;
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

    const ImVec2 hidden_title{100.0F, -100.0F};
    const ImVec2 recovered_title =
        carto::studio::DockingWorkspace::recover_window_position(
            hidden_title, window_size, monitors, 2);
    require(recovered_title.x == 100.0F && recovered_title.y == 0.0F,
        "a visible body must not leave the title bar above every monitor");

    const ImVec2 oversized_hidden_title =
        carto::studio::DockingWorkspace::recover_window_position(
            hidden_title, ImVec2(2200.0F, 1400.0F), monitors, 2);
    require(oversized_hidden_title.y == 0.0F,
        "an oversized window must also recover its top drag strip");

    ImGuiPlatformMonitor stacked_monitors[2]{};
    stacked_monitors[0] = monitors[0];
    stacked_monitors[1].WorkPos = {0.0F, -1024.0F};
    stacked_monitors[1].WorkSize = {1280.0F, 1024.0F};
    const ImVec2 spanning{100.0F, -100.0F};
    const ImVec2 kept_spanning =
        carto::studio::DockingWorkspace::recover_window_position(
            spanning, window_size, stacked_monitors, 2);
    require(kept_spanning.x == spanning.x && kept_spanning.y == spanning.y,
        "a drag strip on another connected monitor must retain its placement");

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

    visible.Pos = {20.0F, -100.0F};
    recovered_platform_position_calls = 0;
    carto::studio::DockingWorkspace::recover_offscreen_platform_windows();
    require(visible.Pos.x == 20.0F && visible.Pos.y == 0.0F &&
            recovered_platform_position_calls == 1 &&
            recovered_platform_viewport == &visible,
        "a visible window body must not hide its platform title bar offscreen");
    carto::studio::DockingWorkspace::recover_offscreen_platform_windows();
    require(recovered_platform_position_calls == 1,
        "a recovered platform window must not be moved again every frame");

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

void require_docking_mouse_snap() {
    ImGuiContext* const previous_context = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    require(context != nullptr,
        "a separate ImGui context must be available for pointer-driven dock acceptance");
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(960.0F, 640.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(io.Fonts->Build(),
        "the pointer-driven docking test font atlas must build");

    struct PanelState {
        ImGuiID scene_node = 0U;
        ImGuiID inspector_node = 0U;
        ImRect scene_tab{};
        ImRect inspector_tab{};
    };
    carto::studio::DockingWorkspace workspace;
    bool mouse_down = false;
    const auto frame = [&](ImVec2 position, bool next_mouse_down) {
        io.AddMousePosEvent(position.x, position.y);
        if (mouse_down != next_mouse_down) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, next_mouse_down);
            mouse_down = next_mouse_down;
        }
        ImGui::NewFrame();
        workspace.begin_root();
        PanelState state;
        for (const char* name : {"Viewport", "Scene", "Inspector", "Assets", "History"}) {
            if (ImGui::Begin(name)) ImGui::TextUnformatted("Dock interaction probe");
            const ImGuiWindow* const panel = ImGui::GetCurrentWindowRead();
            if (panel != nullptr && panel->DockNode != nullptr) {
                const ImGuiID node = panel->DockNode->ID;
                ImRect tab_rect{};
                if (panel->DockNode->TabBar != nullptr) {
                    const ImGuiTabBar* const tab_bar = panel->DockNode->TabBar;
                    for (const ImGuiTabItem& tab : tab_bar->Tabs) {
                        if (tab.ID != panel->TabId) continue;
                        tab_rect = ImRect(
                            ImVec2(tab_bar->BarRect.Min.x + tab.Offset,
                                   tab_bar->BarRect.Min.y),
                            ImVec2(tab_bar->BarRect.Min.x + tab.Offset + tab.Width,
                                   tab_bar->BarRect.Max.y));
                        break;
                    }
                }
                if (std::string_view(name) == "Scene") {
                    state.scene_node = node;
                    state.scene_tab = tab_rect;
                } else if (std::string_view(name) == "Inspector") {
                    state.inspector_node = node;
                    state.inspector_tab = tab_rect;
                }
            }
            ImGui::End();
        }
        ImGui::Render();
        return state;
    };

    static_cast<void>(frame(ImVec2(-100.0F, -100.0F), false));
    const PanelState initial = frame(ImVec2(-100.0F, -100.0F), false);
    PanelState state = initial;
    require(state.scene_node != 0U && state.inspector_node != 0U &&
            state.scene_node != state.inspector_node &&
            state.scene_tab.GetWidth() > 0.0F && state.inspector_tab.GetWidth() > 0.0F,
        "the initial layout must expose separate draggable Scene and Inspector tabs");

    const ImVec2 inspector_tab_center(
        (state.inspector_tab.Min.x + state.inspector_tab.Max.x) * 0.5F,
        (state.inspector_tab.Min.y + state.inspector_tab.Max.y) * 0.5F);
    const ImVec2 scene_drop_target(
        (state.scene_tab.Min.x + state.scene_tab.Max.x) * 0.5F,
        (state.scene_tab.Min.y + state.scene_tab.Max.y) * 0.5F);
    static_cast<void>(frame(inspector_tab_center, false));
    static_cast<void>(frame(inspector_tab_center, true));
    static_cast<void>(frame(ImVec2(
        (inspector_tab_center.x + scene_drop_target.x) * 0.5F,
        (inspector_tab_center.y + scene_drop_target.y) * 0.5F), true));
    static_cast<void>(frame(scene_drop_target, true));
    static_cast<void>(frame(scene_drop_target, true));
    static_cast<void>(frame(scene_drop_target, false));
    state = frame(scene_drop_target, false);
    require(state.scene_node != 0U && state.inspector_node == state.scene_node,
        "dragging the Inspector tab onto Scene must dock both panes into one tab group");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous_context);
}

struct ArrangementGeometry {
    ImGuiID root = 0U;
    ImGuiID viewport_node = 0U;
    ImGuiID scene_node = 0U;
    ImGuiID inspector_node = 0U;
    ImGuiID assets_node = 0U;
    ImVec2 viewport_pos{};
    ImVec2 scene_pos{};
    ImVec2 inspector_pos{};
    ImVec2 assets_pos{};
    float viewport_share = 0.0F;
    bool viewport_central = false;
};

ArrangementGeometry draw_arrangement_frame(carto::studio::DockingWorkspace& workspace) {
    ImGui::NewFrame();
    workspace.begin_root();
    ImGuiWindow* host = ImGui::FindWindowByName(
        "Cartographer Studio Dock Host##host");
    ArrangementGeometry geometry;
    geometry.root = host == nullptr ? 0U : host->GetID("CartographerStudioDockV1");
    for (const char* name : {"Viewport", "Scene", "Inspector", "Assets", "History"}) {
        if (ImGui::Begin(name)) ImGui::TextUnformatted("Arrangement probe");
        ImGui::End();
    }
    ImGui::Render();

    const ImGuiDockNode* const root = ImGui::DockBuilderGetNode(geometry.root);
    const ImGuiWindow* const viewport = ImGui::FindWindowByName("Viewport");
    const ImGuiWindow* const scene = ImGui::FindWindowByName("Scene");
    const ImGuiWindow* const inspector = ImGui::FindWindowByName("Inspector");
    const ImGuiWindow* const assets = ImGui::FindWindowByName("Assets");
    const ImGuiWindow* const history = ImGui::FindWindowByName("History");
    require(root != nullptr && viewport != nullptr && scene != nullptr &&
            inspector != nullptr && assets != nullptr && history != nullptr &&
            viewport->DockNode != nullptr && scene->DockNode != nullptr &&
            inspector->DockNode != nullptr && assets->DockNode != nullptr &&
            history->DockNode != nullptr,
        "every arrangement probe panel must stay assigned to the shared dock tree");
    geometry.viewport_node = viewport->DockNode->ID;
    geometry.scene_node = scene->DockNode->ID;
    geometry.inspector_node = inspector->DockNode->ID;
    geometry.assets_node = assets->DockNode->ID;
    geometry.viewport_pos = viewport->DockNode->Pos;
    geometry.scene_pos = scene->DockNode->Pos;
    geometry.inspector_pos = inspector->DockNode->Pos;
    geometry.assets_pos = assets->DockNode->Pos;
    geometry.viewport_central = viewport->DockNode->IsCentralNode();
    const float root_area = root->Size.x * root->Size.y;
    require(root_area > 0.0F && std::isfinite(root_area),
        "the shared dock root must have a measurable surface");
    geometry.viewport_share =
        viewport->DockNode->Size.x * viewport->DockNode->Size.y / root_area;
    require(history->DockNode == assets->DockNode,
        "Assets and History must remain tabbed together in every arrangement");
    return geometry;
}

void require_workspace_arrangement_switching() {
    ImGuiContext* const previous_context = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    require(context != nullptr,
        "a separate ImGui context must be available for arrangement acceptance");
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 800.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(io.Fonts->Build(), "the arrangement test font atlas must build");

    using Arrangement = carto::studio::WorkspaceArrangement;
    carto::studio::DockingWorkspace workspace;
    require(workspace.arrangement() == Arrangement::studio,
        "the default arrangement must remain Studio for saved-layout compatibility");
    static_cast<void>(draw_arrangement_frame(workspace));
    const ArrangementGeometry studio = draw_arrangement_frame(workspace);
    require(studio.viewport_central,
        "Studio must keep the viewport in the central dock node");
    require(studio.inspector_node != studio.scene_node &&
            studio.scene_pos.x < studio.viewport_pos.x &&
            studio.inspector_pos.x > studio.viewport_pos.x &&
            studio.assets_pos.y > studio.viewport_pos.y,
        "Studio must keep Scene left, Inspector right, and Assets/History below");

    workspace.request_arrangement(Arrangement::scene);
    require(workspace.arrangement() == Arrangement::scene,
        "request_arrangement must record the artist's selection");
    static_cast<void>(draw_arrangement_frame(workspace));
    const ArrangementGeometry scene = draw_arrangement_frame(workspace);
    require(scene.viewport_central &&
            scene.scene_pos.x < scene.viewport_pos.x &&
            scene.inspector_pos.x > scene.viewport_pos.x &&
            scene.inspector_node != scene.scene_node,
        "Scene must keep the same panel topology with Scene left and Inspector right");
    require(scene.viewport_share > studio.viewport_share + 0.05F,
        "Scene must give the viewport a substantially larger share than Studio");

    workspace.request_arrangement(Arrangement::review);
    require(workspace.focus_mode() == false,
        "arrangement switching must not enter focus mode by itself");
    static_cast<void>(draw_arrangement_frame(workspace));
    const ArrangementGeometry review = draw_arrangement_frame(workspace);
    require(review.viewport_central,
        "Review must keep the viewport in the central dock node");
    require(review.inspector_node == review.scene_node &&
            review.assets_node == review.scene_node &&
            review.scene_pos.x < review.viewport_pos.x,
        "Review must fold Scene, Inspector, and ledger tabs into one left tab group");
    require(review.viewport_share > scene.viewport_share + 0.05F,
        "Review must give the viewport a substantially larger share than Scene");

    // Re-selecting the active arrangement must be a no-op so customization
    // within the current arrangement is never reset by re-selection.
    const ArrangementGeometry customized = review;
    workspace.request_arrangement(Arrangement::review);
    const ArrangementGeometry reselected = draw_arrangement_frame(workspace);
    require(reselected.scene_node == customized.scene_node &&
            reselected.inspector_node == customized.inspector_node &&
            reselected.viewport_node == customized.viewport_node,
        "re-selecting the active arrangement must not rebuild the dock tree");

    // An explicit switch away and back may rebuild to that arrangement's
    // authored defaults, discarding in-arrangement customization.
    workspace.request_arrangement(Arrangement::scene);
    static_cast<void>(draw_arrangement_frame(workspace));
    static_cast<void>(draw_arrangement_frame(workspace));
    workspace.request_arrangement(Arrangement::review);
    static_cast<void>(draw_arrangement_frame(workspace));
    const ArrangementGeometry restored_review = draw_arrangement_frame(workspace);
    require(restored_review.inspector_node == restored_review.scene_node &&
            restored_review.scene_node != 0U,
        "switching back to Review must rebuild its authored tab-fold topology");

    // Arrangement switching must survive focus mode: the topology stays alive
    // underneath and is rebuilt when the chrome returns.
    workspace.toggle_focus();
    workspace.request_arrangement(Arrangement::scene);
    require(workspace.focus_mode(),
        "an arrangement request must not clear focus mode");
    static_cast<void>(draw_arrangement_frame(workspace));
    require(workspace.focus_mode(),
        "arrangement frames must remain in focus mode until the artist exits it");
    workspace.toggle_focus();
    static_cast<void>(draw_arrangement_frame(workspace));
    const ArrangementGeometry focused_scene = draw_arrangement_frame(workspace);
    require(!workspace.focus_mode() && focused_scene.viewport_central &&
            focused_scene.inspector_pos.x > focused_scene.viewport_pos.x,
        "the arrangement requested during focus mode must be applied when chrome returns");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous_context);
}

void require_arrangement_layout_path_mapping() {
    using Arrangement = carto::studio::WorkspaceArrangement;
    const std::filesystem::path directory =
        std::filesystem::path("prefs") / "desktop-test";
    const auto studio_path =
        carto::studio::DockingWorkspace::arrangement_layout_path(
            directory, Arrangement::studio);
    require(studio_path == directory / "workspace-docking.ini",
        "Studio must keep the exact legacy docking INI path for compatibility");
    require(studio_path.filename() ==
            std::filesystem::path(carto::studio::DockingWorkspace::kLegacyLayoutFileName),
        "Studio must resolve to the legacy layout file name");

    const auto scene_path =
        carto::studio::DockingWorkspace::arrangement_layout_path(
            directory, Arrangement::scene);
    const auto review_path =
        carto::studio::DockingWorkspace::arrangement_layout_path(
            directory, Arrangement::review);
    require(scene_path != studio_path && review_path != studio_path &&
            scene_path != review_path,
        "each arrangement must persist to its own bounded file");
    require(scene_path.parent_path() == directory &&
            review_path.parent_path() == directory,
        "per-arrangement layout files must stay beside the legacy Studio file");
    require(scene_path.filename().string().size() <= 40U &&
            review_path.filename().string().size() <= 40U,
        "per-arrangement layout file names must remain bounded");

    const auto scene_temporary =
        carto::studio::DockingWorkspace::layout_temporary_path(scene_path, 4100U, 0U);
    require(scene_temporary.parent_path() == directory,
        "arrangement temp writes must remain beside the destination file");

    require(carto::studio::DockingWorkspace::classify_arrangement("Studio") ==
                Arrangement::studio &&
            carto::studio::DockingWorkspace::classify_arrangement("Scene") ==
                Arrangement::scene &&
            carto::studio::DockingWorkspace::classify_arrangement("Review") ==
                Arrangement::review,
        "arrangement labels must classify to their persisted layouts");
    require(!carto::studio::DockingWorkspace::classify_arrangement("scene").has_value() &&
            !carto::studio::DockingWorkspace::classify_arrangement("").has_value() &&
            !carto::studio::DockingWorkspace::classify_arrangement("Studio ").has_value(),
        "unknown arrangement labels must be rejected instead of mapping to a layout");
    for (std::size_t index = 0U;
         index < carto::studio::DockingWorkspace::kArrangementCount; ++index) {
        const auto arrangement = static_cast<Arrangement>(index);
        require(carto::studio::DockingWorkspace::arrangement_name(arrangement).size() > 0U,
            "every arrangement must expose a non-empty artist-facing label");
    }
}

void require_arrangement_layout_restart() {
    ImGuiContext* previous_context = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    require(context != nullptr,
        "a separate ImGui context must be available for arrangement persistence");
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 800.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(io.Fonts->Build(),
        "the arrangement persistence font atlas must build");

    // Artist works in the Scene arrangement and customizes it by folding the
    // Inspector into the Scene node; the customized topology is persisted.
    using Arrangement = carto::studio::WorkspaceArrangement;
    carto::studio::DockingWorkspace workspace;
    workspace.request_arrangement(Arrangement::scene);
    static_cast<void>(draw_arrangement_frame(workspace));
    const ArrangementGeometry scene = draw_arrangement_frame(workspace);
    ImGui::DockBuilderDockWindow("Inspector", scene.scene_node);
    ImGui::DockBuilderFinish(scene.root);
    const ArrangementGeometry customized = draw_arrangement_frame(workspace);
    require(customized.inspector_node == customized.scene_node,
        "the customized Scene arrangement must tab Inspector with Scene");
    std::size_t persisted_size = 0U;
    const char* persisted_data = ImGui::SaveIniSettingsToMemory(&persisted_size);
    require(persisted_data != nullptr && persisted_size > 0U,
        "a customized arrangement must produce restart settings");
    const std::string persisted_settings(persisted_data, persisted_size);

    // Fresh session: the workspace still defaults to Studio, and the saved
    // Scene-arrangement customization is loaded. Startup must not overwrite
    // the artist's saved topology with any arrangement's authored defaults.
    ImGui::DestroyContext(context);
    context = ImGui::CreateContext();
    require(context != nullptr,
        "a fresh ImGui context must be available for arrangement restart");
    ImGuiIO& restored_io = ImGui::GetIO();
    restored_io.DisplaySize = ImVec2(1280.0F, 800.0F);
    restored_io.DeltaTime = 1.0F / 60.0F;
    restored_io.IniFilename = nullptr;
    restored_io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(restored_io.Fonts->Build(),
        "the arrangement restart font atlas must build");
    ImGui::LoadIniSettingsFromMemory(
        persisted_settings.data(), persisted_settings.size());
    carto::studio::DockingWorkspace restored;
    restored.note_saved_layout_loaded();
    static_cast<void>(draw_arrangement_frame(restored));
    const ArrangementGeometry restarted = draw_arrangement_frame(restored);
    require(restarted.inspector_node == restarted.scene_node &&
            restarted.scene_pos.x < restarted.viewport_pos.x,
        "startup must restore the saved Scene-arrangement customization instead of Studio defaults");
    require(restarted.viewport_share > 0.4F,
        "the restored arrangement must keep a viewport-dominant surface");
    const ArrangementGeometry kept = restarted;
    static_cast<void>(draw_arrangement_frame(restored));
    const ArrangementGeometry kept_again = draw_arrangement_frame(restored);
    require(kept_again.scene_node == kept.scene_node &&
            kept_again.inspector_node == kept.inspector_node,
        "frames after a loaded arrangement must not rebuild the saved topology");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous_context);
}

void require_arrangement_fallback_preserves_guard_state() {
    ImGuiContext* const previous_context = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    require(context != nullptr,
        "a separate ImGui context must be available for arrangement fallback acceptance");
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 800.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(io.Fonts->Build(),
        "the arrangement fallback font atlas must build");

    using Arrangement = carto::studio::WorkspaceArrangement;
    carto::studio::DockingWorkspace workspace;
    workspace.toggle_focus();
    static_cast<void>(draw_arrangement_frame(workspace));
    static_cast<void>(draw_arrangement_frame(workspace));

    // Simulate the app rejecting a corrupt layout file for the current
    // arrangement. The corrupt-file guard must survive a defaults fallback:
    // shutdown saves would otherwise silently overwrite the rejected bytes.
    workspace.note_saved_layout_rejected();
    require(!workspace.layout_save_allowed(),
        "a rejected layout must block saves before the fallback");
    workspace.request_arrangement_defaults(Arrangement::scene);
    require(workspace.arrangement() == Arrangement::scene,
        "the fallback must record the arrangement it rebuilds");
    require(workspace.focus_mode(),
        "the arrangement defaults fallback must preserve focus mode");
    require(!workspace.layout_save_allowed(),
        "the arrangement defaults fallback must preserve a pending rejection so the corrupt file is not overwritten");
    static_cast<void>(draw_arrangement_frame(workspace));
    require(workspace.focus_mode(),
        "fallback frames must remain in focus mode until the artist exits it");
    // Geometry assertions need the chrome back: while focus mode is active the
    // dockspace runs KeepAliveOnly and marks no central node.
    workspace.toggle_focus();
    const ArrangementGeometry rebuilt = draw_arrangement_frame(workspace);
    require(rebuilt.viewport_central &&
            rebuilt.scene_pos.x < rebuilt.viewport_pos.x &&
            rebuilt.inspector_pos.x > rebuilt.viewport_pos.x,
        "the fallback must rebuild the Scene arrangement's authored topology");

    // The explicit Reset workspace layout action keeps its deliberate
    // semantics: it clears focus and the rejection guard, making saves
    // allowed again (the artist has chosen to discard the corrupt file).
    workspace.toggle_focus();
    workspace.request_reset();
    require(!workspace.focus_mode() && workspace.layout_save_allowed(),
        "an explicit reset must clear focus and the rejection guard");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous_context);
}

void require_arrangement_switch_clears_rejection_for_healthy_target() {
    // Switching away from a rejected arrangement to one with no usable saved
    // layout must clear the stale rejection: the new arrangement has no
    // corrupt file to preserve, and blocking saves would prevent it from ever
    // persisting. This is the request_arrangement normal branch the app uses
    // for missing target layouts.
    carto::studio::DockingWorkspace workspace;
    workspace.note_saved_layout_rejected();
    require(!workspace.layout_save_allowed(),
        "a rejected layout must block saves before switching");
    workspace.request_arrangement(carto::studio::WorkspaceArrangement::review);
    require(workspace.arrangement() == carto::studio::WorkspaceArrangement::review,
        "switching must record the target arrangement");
    require(workspace.layout_save_allowed(),
        "switching to a layout with no corrupt file must clear the stale rejection so the target can be saved");
}

// Mirrors the shell's Viewport binding: p_open is the persisted work-surface
// pane flag. Closing (tab or native OS window) hides the window without
// touching the dock tree, and restoring re-docks it to the same node.
void require_viewport_close_restore_keeps_dock_identity() {
    ImGuiContext* const previous_context = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    require(context != nullptr,
        "a separate ImGui context must be available for viewport close/restore acceptance");
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 800.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(io.Fonts->Build(),
        "the viewport close/restore font atlas must build");

    carto::studio::DockingWorkspace workspace;
    bool work_surface_visible = true;
    const ImGuiWindow* viewport = nullptr;
    const ImGuiWindow* scene = nullptr;
    const auto frame = [&]() {
        ImGui::NewFrame();
        workspace.begin_root();
        viewport = nullptr;
        if (work_surface_visible) {
            if (ImGui::Begin("Viewport", &work_surface_visible,
                    ImGuiWindowFlags_NoScrollbar)) {
                ImGui::TextUnformatted("Viewport probe");
            }
            ImGui::End();
            viewport = ImGui::FindWindowByName("Viewport");
        }
        if (ImGui::Begin("Scene")) ImGui::TextUnformatted("Scene probe");
        ImGui::End();
        scene = ImGui::FindWindowByName("Scene");
        ImGui::Render();
    };
    frame();
    frame();
    require(viewport != nullptr && viewport->DockNode != nullptr,
        "the viewport must dock into the shared tree");
    require(scene != nullptr && scene->DockNode != nullptr,
        "the scene probe must dock into the shared tree");
    const ImGuiID viewport_node = viewport->DockNode->ID;
    const ImGuiID scene_node = scene->DockNode->ID;

    // Closing the tab (or the torn-out native window) drives p_open false,
    // exactly as the shell's bound Begin reports it.
    work_surface_visible = false;
    frame();
    require(viewport == nullptr,
        "a hidden work surface must not draw the viewport window");
    require(scene != nullptr && scene->DockNode != nullptr &&
            scene->DockNode->ID == scene_node,
        "hiding the viewport must leave the rest of the dock tree intact");

    // Restore through the same persisted flag View > Work surface toggles.
    work_surface_visible = true;
    frame();
    frame();
    const ImGuiWindow* restored = ImGui::FindWindowByName("Viewport");
    require(restored != nullptr && restored->DockNode != nullptr &&
            restored->DockNode->ID == viewport_node,
        "restoring the work surface must re-dock Viewport to its previous node");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous_context);
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

    const ImGuiWindow* scene_window = ImGui::FindWindowByName("Scene");
    require(scene_window != nullptr && scene_window->DockNode != nullptr,
        "the persistence probe requires the initialized Scene dock node");
    ImGui::DockBuilderDockWindow("Inspector", scene_window->DockNode->ID);
    ImGui::DockBuilderFinish(dock_id);
    static_cast<void>(draw_docking_frame(docking));
    static_cast<void>(draw_docking_frame(docking));
    const ImGuiWindow* const tabbed_scene = ImGui::FindWindowByName("Scene");
    const ImGuiWindow* const tabbed_inspector = ImGui::FindWindowByName("Inspector");
    require(tabbed_scene != nullptr && tabbed_inspector != nullptr &&
            tabbed_scene->DockNode != nullptr &&
            tabbed_scene->DockNode == tabbed_inspector->DockNode,
        "a user-created instrument tab grouping must be represented in the dock tree");

    const DockingLayoutFrame first_saved_layout = draw_docking_layout_frame(
        docking, ImVec2(460.0F, 290.0F));
    const DockingLayoutFrame saved_layout_frame = draw_docking_layout_frame(
        docking, ImVec2(460.0F, 290.0F));
    require(first_saved_layout.root == dock_id && saved_layout_frame.root == dock_id &&
            saved_layout_frame.panel_nodes[1] != 0U &&
            saved_layout_frame.panel_nodes[1] == saved_layout_frame.panel_nodes[2],
        "the custom tab group and dock root must remain live before persistence");
    require(std::abs(saved_layout_frame.floating_position.x - 460.0F) < 0.01F &&
            std::abs(saved_layout_frame.floating_position.y - 290.0F) < 0.01F,
        "the floating instrument must begin at its requested first-use position");
    std::size_t persisted_settings_size = 0U;
    const char* const persisted_settings_data =
        ImGui::SaveIniSettingsToMemory(&persisted_settings_size);
    require(persisted_settings_data != nullptr && persisted_settings_size > 0U,
        "a customized docking and floating layout must produce settings for restart");
    const std::string persisted_settings(
        persisted_settings_data, persisted_settings_size);

    require_docking_monitor_recovery();
    require_live_docking_monitor_recovery();
    require_unique_docking_layout_write_paths();
    require_docking_layout_load_fail_closed();
    require_docking_capture_loss_is_distinguished();
    require_docking_mouse_snap();
    require_workspace_arrangement_switching();
    require_arrangement_layout_path_mapping();
    require_arrangement_layout_restart();
    require_arrangement_fallback_preserves_guard_state();
    require_arrangement_switch_clears_rejection_for_healthy_target();
    require_viewport_close_restore_keeps_dock_identity();

    ImGui::DestroyContext(context);

    ImGuiContext* restored_context = ImGui::CreateContext();
    require(restored_context != nullptr,
        "a fresh ImGui context must be available for layout restart acceptance");
    ImGuiIO& restored_io = ImGui::GetIO();
    restored_io.DisplaySize = ImVec2(640.0F, 480.0F);
    restored_io.DeltaTime = 1.0F / 60.0F;
    restored_io.IniFilename = nullptr;
    restored_io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    require(restored_io.Fonts->Build(),
        "the restarted layout test font atlas must build");
    ImGui::LoadIniSettingsFromMemory(
        persisted_settings.data(), persisted_settings.size());
    carto::studio::DockingWorkspace restored_docking;
    restored_docking.note_saved_layout_loaded();
    static_cast<void>(draw_docking_layout_frame(
        restored_docking, ImVec2(20.0F, 24.0F)));
    const DockingLayoutFrame restored_layout = draw_docking_layout_frame(
        restored_docking, ImVec2(20.0F, 24.0F));
    require(restored_layout.root == dock_id &&
            restored_layout.panel_nodes[1] != 0U &&
            restored_layout.panel_nodes[1] == restored_layout.panel_nodes[2],
        "a fresh session must restore the user-created Scene/Inspector tab group");
    require(std::abs(restored_layout.floating_position.x -
                     saved_layout_frame.floating_position.x) < 0.01F &&
            std::abs(restored_layout.floating_position.y -
                     saved_layout_frame.floating_position.y) < 0.01F,
        "a fresh session must restore the floating instrument position instead of its default");
    ImGui::DestroyContext(restored_context);
    return 0;
}
