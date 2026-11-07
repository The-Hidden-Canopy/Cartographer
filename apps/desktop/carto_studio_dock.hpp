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
#include <optional>
#include <string>
#include <string_view>

#ifndef IMGUI_HAS_DOCK
#error "Cartographer Studio requires the pinned Dear ImGui docking build. See docs/CODE_HANDOFF.md"
#endif

namespace carto::studio {

// Named workspace arrangements share the same dock root identity and the same
// panel windows as the legacy Studio layout; only split topology and
// proportions differ. Studio keeps the exact legacy proportions (Scene 17%,
// Inspector 22%, bottom 21%) and the legacy persisted file so existing users
// keep their saved layout untouched; Scene and Review give the viewport a
// substantially larger share for viewport-first authoring.
enum class WorkspaceArrangement : std::uint8_t { studio, scene, review };

class DockingWorkspace final {
public:
    static constexpr std::size_t kArrangementCount = 3U;
    // The legacy Studio persisted layout file; its name and location must not
    // change so previously saved workspaces keep loading.
    static constexpr std::string_view kLegacyLayoutFileName = "workspace-docking.ini";

    [[nodiscard]] static constexpr std::string_view arrangement_name(
        WorkspaceArrangement arrangement) noexcept {
        switch (arrangement) {
        case WorkspaceArrangement::scene: return "Scene";
        case WorkspaceArrangement::review: return "Review";
        case WorkspaceArrangement::studio: default: return "Studio";
        }
    }

    // Bounded input mapping from a user-visible arrangement label. Names that
    // do not exactly match a known arrangement are rejected.
    [[nodiscard]] static constexpr std::optional<WorkspaceArrangement>
    classify_arrangement(std::string_view name) noexcept {
        for (std::size_t index = 0U; index < kArrangementCount; ++index) {
            const auto arrangement = static_cast<WorkspaceArrangement>(index);
            if (arrangement_name(arrangement) == name) return arrangement;
        }
        return std::nullopt;
    }

    // Maps an arrangement to its bounded persisted INI file beside the legacy
    // Studio file. Studio resolves to the exact legacy path for compatibility;
    // every other arrangement gets its own sibling file so an artist can
    // customize and later return to each arrangement independently.
    [[nodiscard]] static std::filesystem::path arrangement_layout_path(
        const std::filesystem::path& directory,
        WorkspaceArrangement arrangement) {
        switch (arrangement) {
        case WorkspaceArrangement::scene:
            return directory / "workspace-docking-scene.ini";
        case WorkspaceArrangement::review:
            return directory / "workspace-docking-review.ini";
        case WorkspaceArrangement::studio: default:
            return directory / std::string(kLegacyLayoutFileName);
        }
    }

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
    [[nodiscard]] WorkspaceArrangement arrangement() const noexcept { return arrangement_; }
    // An explicit arrangement switch rebuilds the dock tree to that
    // arrangement's proportions on the next frame. Re-selecting the active
    // arrangement is a no-op so artist customization within the current
    // arrangement is never reset by re-selection. A switch never clears focus
    // mode and never touches project/scene/camera authority. Loading a saved
    // layout keeps the saved topology: startup only builds an arrangement when
    // no usable layout was restored.
    void request_arrangement(WorkspaceArrangement arrangement) noexcept {
        if (arrangement_ == arrangement) return;
        arrangement_ = arrangement;
        rebuild_ = true;
        loaded_existing_layout_ = false;
        layout_load_rejected_ = false;
    }
    // Rebuilds the given arrangement's authored defaults on the next frame
    // without clearing focus mode or a pending layout-load rejection. Unlike
    // request_arrangement() this entry fires even when the arrangement is
    // already selected, and unlike request_reset() it is not an explicit
    // artist reset: the corrupt-file guard (layout_save_allowed) and focus
    // state are preserved so a rejected layout file is never overwritten as a
    // side effect. The app uses this when the target arrangement has no usable
    // saved layout — its file is missing (first use, save allowed) or rejected
    // (preserved on disk until the artist explicitly resets the workspace).
    void request_arrangement_defaults(WorkspaceArrangement arrangement) noexcept {
        arrangement_ = arrangement;
        rebuild_ = true;
        loaded_existing_layout_ = false;
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

    // Chrome extents come from the same layout used by the native header/footer.
    // The viewport can always be torn out of its dock node into a native HWND.
    void begin_root(float mode_bar_height = 44.0F, float status_bar_height = 22.0F) {
        ImGuiViewport* primary = ImGui::GetMainViewport();
        if (primary == nullptr) return;
        const float reserved = focus_mode_ ? 0.0F : mode_bar_height;
        const float footer = focus_mode_ ? 0.0F : status_bar_height;
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
        switch (arrangement_) {
        case WorkspaceArrangement::scene:
            build_arrangement(root, 0.13F, 0.18F, 0.14F);
            break;
        case WorkspaceArrangement::review:
            // Review folds Scene, Inspector, Assets, and History into one tab
            // column on the left so the viewport takes the whole remaining
            // ~86% of the surface. There is no right node and no bottom node.
            build_arrangement(root, 0.14F, 0.0F, 0.0F, true);
            break;
        case WorkspaceArrangement::studio: default:
            // Legacy compatibility baseline: Scene 17%, Inspector 22%,
            // Assets/History 21% below; the viewport keeps ~51% of the surface.
            build_arrangement(root, 0.17F, 0.22F, 0.21F);
            break;
        }
        ImGui::DockBuilderFinish(root);
    }

    // Same panels, same root identity, same tear-off paths for every
    // arrangement; only the splits differ. When fold_inspector_left is set the
    // Inspector is tabbed with Scene instead of receiving its own right node,
    // and bottom_fraction is applied to the remaining center column.
    static void build_arrangement(ImGuiID root, float left_fraction,
                                  float right_fraction, float bottom_fraction,
                                  bool fold_inspector_left = false) {
        ImGuiID center = root;
        const ImGuiID left = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Left, left_fraction, nullptr, &center);
        ImGuiID right = 0U;
        if (!fold_inspector_left) {
            right = ImGui::DockBuilderSplitNode(
                center, ImGuiDir_Right, right_fraction, nullptr, &center);
        }
        if (bottom_fraction > 0.0F) {
            const ImGuiID bottom = ImGui::DockBuilderSplitNode(
                center, ImGuiDir_Down, bottom_fraction, nullptr, &center);
            ImGui::DockBuilderDockWindow("Assets", bottom);
            ImGui::DockBuilderDockWindow("History", bottom);
        } else {
            ImGui::DockBuilderDockWindow("Assets", left);
            ImGui::DockBuilderDockWindow("History", left);
        }
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Scene", left);
        ImGui::DockBuilderDockWindow("Inspector",
            fold_inspector_left || right == 0U ? left : right);
    }
    bool initialized_{false};
    bool loaded_existing_layout_{false};
    bool layout_load_rejected_{false};
    bool rebuild_{false};
    bool focus_mode_{false};
    WorkspaceArrangement arrangement_{WorkspaceArrangement::studio};
};

} // namespace carto::studio
