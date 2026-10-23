#pragma once

// Cartographer viewport-first workspace adapter.
// Requires Dear ImGui v1.93.1 WIP docking, upstream commit
// d88f238d1d363ef387382b2e29ad12cf96f826ac (see CARTOGRAPHER_PIN).
// Its Win32/Vulkan backends own platform windows and swapchains; this adapter
// owns only layout and presentation. Project/mesh authority remains in carto_ui.

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#ifndef IMGUI_HAS_DOCK
#error "Cartographer Studio requires the pinned Dear ImGui docking build. See docs/CODE_HANDOFF.md"
#endif

namespace carto::studio {

inline void apply_vanta_studio_theme(bool dark) noexcept {
    if (!dark) return;
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 4.0F;
    s.ChildRounding = 4.0F;
    s.FrameRounding = 4.0F;
    s.PopupRounding = 4.0F;
    s.TabRounding = 3.0F;
    s.GrabRounding = 3.0F;
    s.WindowBorderSize = 1.0F;
    s.ChildBorderSize = 1.0F;
    s.FrameBorderSize = 1.0F;
    s.WindowPadding = {9.0F, 8.0F};
    s.FramePadding = {9.0F, 6.0F};
    s.ItemSpacing = {7.0F, 6.0F};
    s.ItemInnerSpacing = {6.0F, 5.0F};
    s.ScrollbarSize = 9.0F;
    s.Colors[ImGuiCol_WindowBg] = {0.038F, 0.058F, 0.078F, 0.99F};
    s.Colors[ImGuiCol_ChildBg] = {0.044F, 0.069F, 0.086F, 1.0F};
    s.Colors[ImGuiCol_PopupBg] = {0.055F, 0.077F, 0.097F, 0.99F};
    s.Colors[ImGuiCol_MenuBarBg] = {0.038F, 0.058F, 0.078F, 1.0F};
    s.Colors[ImGuiCol_TitleBg] = {0.045F, 0.064F, 0.081F, 1.0F};
    s.Colors[ImGuiCol_TitleBgActive] = {0.059F, 0.090F, 0.112F, 1.0F};
    s.Colors[ImGuiCol_Border] = {0.135F, 0.196F, 0.226F, 0.85F};
    s.Colors[ImGuiCol_Separator] = {0.132F, 0.193F, 0.219F, 0.72F};
    s.Colors[ImGuiCol_FrameBg] = {0.067F, 0.097F, 0.122F, 1.0F};
    s.Colors[ImGuiCol_FrameBgHovered] = {0.094F, 0.150F, 0.177F, 1.0F};
    s.Colors[ImGuiCol_FrameBgActive] = {0.085F, 0.184F, 0.224F, 1.0F};
    s.Colors[ImGuiCol_Button] = {0.068F, 0.109F, 0.135F, 1.0F};
    s.Colors[ImGuiCol_ButtonHovered] = {0.085F, 0.180F, 0.219F, 1.0F};
    s.Colors[ImGuiCol_ButtonActive] = {0.073F, 0.250F, 0.301F, 1.0F};
    s.Colors[ImGuiCol_Header] = {0.067F, 0.118F, 0.145F, 1.0F};
    s.Colors[ImGuiCol_HeaderHovered] = {0.079F, 0.166F, 0.205F, 1.0F};
    s.Colors[ImGuiCol_HeaderActive] = {0.076F, 0.219F, 0.273F, 1.0F};
    s.Colors[ImGuiCol_Tab] = {0.054F, 0.080F, 0.098F, 1.0F};
    s.Colors[ImGuiCol_TabHovered] = {0.075F, 0.175F, 0.216F, 1.0F};
    s.Colors[ImGuiCol_TabSelected] = {0.074F, 0.181F, 0.224F, 1.0F};
    s.Colors[ImGuiCol_DockingPreview] = {0.16F, 0.70F, 0.84F, 0.55F};
    s.Colors[ImGuiCol_DockingEmptyBg] = {0.034F, 0.052F, 0.070F, 1.0F};
    s.Colors[ImGuiCol_CheckMark] = {0.35F, 0.83F, 0.89F, 1.0F};
    s.Colors[ImGuiCol_SliderGrab] = {0.23F, 0.67F, 0.82F, 1.0F};
    s.Colors[ImGuiCol_SliderGrabActive] = {0.44F, 0.86F, 0.96F, 1.0F};
}

class DockingWorkspace final {
public:
    enum class LayoutPresence : std::uint8_t { missing, present, rejected };
    enum class NativeCaptureState : std::uint8_t { none, owned, lost };

    [[nodiscard]] static NativeCaptureState classify_native_capture(
        bool capture_expected, bool capture_owned) noexcept {
        if (!capture_expected) return NativeCaptureState::none;
        return capture_owned ? NativeCaptureState::owned : NativeCaptureState::lost;
    }

    [[nodiscard]] static LayoutPresence classify_layout_presence(
        bool exists, const std::error_code& error) noexcept {
        if (error) return LayoutPresence::rejected;
        return exists ? LayoutPresence::present : LayoutPresence::missing;
    }

    [[nodiscard]] static std::filesystem::path layout_temporary_path(
        const std::filesystem::path& destination,
        std::uint32_t process_id,
        std::uint64_t write_sequence) {
        auto temporary = destination;
        temporary += ".tmp." + std::to_string(process_id) + "." +
                     std::to_string(write_sequence);
        return temporary;
    }

    void note_saved_layout_loaded() noexcept {
        loaded_existing_layout_ = true;
        layout_load_rejected_ = false;
    }
    void note_saved_layout_rejected() noexcept {
        loaded_existing_layout_ = false;
        layout_load_rejected_ = true;
    }
    [[nodiscard]] bool layout_save_allowed() const noexcept {
        return !layout_load_rejected_;
    }
    void request_reset() noexcept {
        rebuild_ = true;
        loaded_existing_layout_ = false;
        layout_load_rejected_ = false;
        focus_mode_ = false;
    }
    void toggle_focus() noexcept { focus_mode_ = !focus_mode_; }
    [[nodiscard]] bool focus_mode() const noexcept { return focus_mode_; }

    // Keep detached windows recoverable when a monitor disappears. A window
    // already showing a useful portion on any connected work area is left
    // untouched; otherwise clamp it to the nearest current monitor, retaining
    // at least a small draggable strip. This changes presentation only.
    [[nodiscard]] static ImVec2 recover_window_position(
        ImVec2 position,
        ImVec2 size,
        const ImGuiPlatformMonitor* monitors,
        int monitor_count,
        float minimum_visible_width = 48.0F,
        float minimum_visible_height = 32.0F) noexcept {
        if (monitors == nullptr || monitor_count <= 0 ||
            !std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(size.x) || !std::isfinite(size.y) ||
            size.x <= 0.0F || size.y <= 0.0F ||
            !std::isfinite(minimum_visible_width) ||
            !std::isfinite(minimum_visible_height) ||
            minimum_visible_width <= 0.0F || minimum_visible_height <= 0.0F) {
            return position;
        }

        const float right = position.x + size.x;
        const float bottom = position.y + size.y;
        if (!std::isfinite(right) || !std::isfinite(bottom)) return position;

        int nearest = -1;
        double nearest_distance_squared = std::numeric_limits<double>::infinity();
        const double center_x = static_cast<double>(position.x) + size.x * 0.5;
        const double center_y = static_cast<double>(position.y) + size.y * 0.5;
        for (int index = 0; index < monitor_count; ++index) {
            const auto& monitor = monitors[index];
            const ImVec2 area_position = monitor.WorkPos;
            const ImVec2 area_size = monitor.WorkSize;
            if (!std::isfinite(area_position.x) || !std::isfinite(area_position.y) ||
                !std::isfinite(area_size.x) || !std::isfinite(area_size.y) ||
                area_size.x <= 0.0F || area_size.y <= 0.0F) {
                continue;
            }
            const float area_right = area_position.x + area_size.x;
            const float area_bottom = area_position.y + area_size.y;
            if (!std::isfinite(area_right) || !std::isfinite(area_bottom)) continue;

            const float overlap_x = std::max(0.0F,
                std::min(right, area_right) - std::max(position.x, area_position.x));
            const float overlap_y = std::max(0.0F,
                std::min(bottom, area_bottom) - std::max(position.y, area_position.y));
            const float visible_x = std::min({minimum_visible_width, size.x, area_size.x});
            const float visible_y = std::min({minimum_visible_height, size.y, area_size.y});
            if (overlap_x >= visible_x && overlap_y >= visible_y) return position;

            const double dx = center_x < area_position.x
                ? static_cast<double>(area_position.x) - center_x
                : center_x > area_right ? center_x - area_right : 0.0;
            const double dy = center_y < area_position.y
                ? static_cast<double>(area_position.y) - center_y
                : center_y > area_bottom ? center_y - area_bottom : 0.0;
            const double distance_squared = dx * dx + dy * dy;
            if (distance_squared < nearest_distance_squared) {
                nearest = index;
                nearest_distance_squared = distance_squared;
            }
        }
        if (nearest < 0) return position;

        const auto& monitor = monitors[nearest];
        const float visible_x = std::min({
            minimum_visible_width, size.x, monitor.WorkSize.x});
        const float visible_y = std::min({
            minimum_visible_height, size.y, monitor.WorkSize.y});
        const float minimum_x = size.x <= monitor.WorkSize.x
            ? monitor.WorkPos.x
            : monitor.WorkPos.x - size.x + visible_x;
        const float maximum_x = size.x <= monitor.WorkSize.x
            ? monitor.WorkPos.x + monitor.WorkSize.x - size.x
            : monitor.WorkPos.x + monitor.WorkSize.x - visible_x;
        const float minimum_y = size.y <= monitor.WorkSize.y
            ? monitor.WorkPos.y
            : monitor.WorkPos.y - size.y + visible_y;
        const float maximum_y = size.y <= monitor.WorkSize.y
            ? monitor.WorkPos.y + monitor.WorkSize.y - size.y
            : monitor.WorkPos.y + monitor.WorkSize.y - visible_y;
        return {std::clamp(position.x, minimum_x, maximum_x),
                std::clamp(position.y, minimum_y, maximum_y)};
    }

    static void recover_offscreen_platform_windows() noexcept {
        ImGuiIO& io = ImGui::GetIO();
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) return;
        ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        const ImGuiViewport* main = ImGui::GetMainViewport();
        if (main == nullptr || platform.Monitors.Size == 0) return;
        for (int index = 0; index < platform.Viewports.Size; ++index) {
            ImGuiViewport* viewport = platform.Viewports[index];
            if (viewport == nullptr || viewport->ID == main->ID) continue;
            const ImVec2 recovered = recover_window_position(
                viewport->Pos, viewport->Size,
                platform.Monitors.Data, platform.Monitors.Size);
            if (recovered.x == viewport->Pos.x && recovered.y == viewport->Pos.y) continue;
            viewport->Pos = recovered;
            if (viewport->PlatformHandle != nullptr &&
                platform.Platform_SetWindowPos != nullptr) {
                platform.Platform_SetWindowPos(viewport, recovered);
            }
        }
    }

    [[nodiscard]] static bool platform_windows_enabled() noexcept {
        const auto& io = ImGui::GetIO();
        constexpr int needed = ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports;
        return (io.BackendFlags & needed) == needed &&
               (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    }

    // A 44 px mode bar is reserved immediately below ImGui's standard main menu.
    // The viewport can always be torn out of its dock node into a native HWND.
    void begin_root(float mode_bar_height = 44.0F) {
        ImGuiViewport* primary = ImGui::GetMainViewport();
        if (primary == nullptr) return;
        const float reserved = focus_mode_ ? 0.0F : mode_bar_height;
        const float footer = focus_mode_ ? 0.0F : 22.0F;
        const ImVec2 position{primary->WorkPos.x, primary->WorkPos.y + reserved};
        const ImVec2 extent{
            std::max(1.0F, primary->WorkSize.x),
            std::max(1.0F, primary->WorkSize.y - reserved - footer)};
        ImGui::SetNextWindowViewport(primary->ID);
        ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(extent, ImGuiCond_Always);
        const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        const bool visible = ImGui::Begin("Cartographer Studio Dock Host##host", nullptr, flags);
        ImGui::PopStyleVar();
        const ImGuiID root = ImGui::GetID("CartographerStudioDockV1");
        // Keep the dock tree alive even when the solo viewport covers it.
        if (!initialized_ || rebuild_) {
            if (rebuild_ || !loaded_existing_layout_) build_initial_layout(root, extent);
            initialized_ = true;
            rebuild_ = false;
        }
        const ImGuiDockNodeFlags dock_flags = focus_mode_
            ? ImGuiDockNodeFlags_KeepAliveOnly
            : ImGuiDockNodeFlags_PassthruCentralNode;
        if (visible || focus_mode_) ImGui::DockSpace(root, {0.0F, 0.0F}, dock_flags);
        ImGui::End();
    }

    void begin_solo_viewport() const {
        ImGuiViewport* primary = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(primary->ID);
        ImGui::SetNextWindowPos(primary->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(primary->WorkSize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("Cartographer Viewport Focus##focus", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
    }

private:
    void build_initial_layout(ImGuiID root, ImVec2 extent) {
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, extent);
        ImGuiID center = root;
        const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17F, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22F, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.21F, nullptr, &center);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Scene", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderDockWindow("History", bottom);
        ImGui::DockBuilderFinish(root);
    }
    bool initialized_{false};
    bool loaded_existing_layout_{false};
    bool layout_load_rejected_{false};
    bool rebuild_{false};
    bool focus_mode_{false};
};

} // namespace carto::studio
