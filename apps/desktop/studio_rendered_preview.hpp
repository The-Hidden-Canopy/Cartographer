#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace carto::studio {

// A single, unwrapped row keeps the viewport extent stable while an
// asynchronous result moves between preparing, ready and error states.
inline void draw_rendered_preview_status(std::string_view diagnostic, bool displayed = false) {
    const auto end = diagnostic.find_first_of("\r\n");
    const auto first_line = diagnostic.substr(0, std::min<std::size_t>(
        end == std::string_view::npos ? diagnostic.size() : end, 256U));
    ImGui::TextUnformatted("Rendered preview:");
    ImGui::SameLine();
    if (first_line.empty()) {
        ImGui::TextUnformatted(displayed ? "Ready" : "Waiting for preview");
    } else {
        ImGui::TextUnformatted(first_line.data(), first_line.data() + first_line.size());
    }
    if (ImGui::IsItemHovered() && diagnostic.size() > first_line.size()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(diagnostic.data(), diagnostic.data() + diagnostic.size());
        ImGui::EndTooltip();
    }
}

struct PreviewReturnFrame {
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    float x = 0, y = 0, width = 0, height = 0;
    float window_x = 0, window_y = 0, window_width = 0, window_height = 0;

    [[nodiscard]] bool valid() const noexcept {
        return std::isfinite(x) && std::isfinite(y) &&
            std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0 &&
            std::isfinite(window_x) && std::isfinite(window_y) &&
            std::isfinite(window_width) && std::isfinite(window_height) &&
            window_width > 0 && window_height > 0;
    }
    [[nodiscard]] bool operator==(const PreviewReturnFrame&) const = default;
};

// Only a successfully presented live-canvas frame can authorize an explicit
// return to editing. An unavailable, changed, detached or unpresented frame
// invalidates the receipt; rendered-preview pixels never authorize picking.
class PreviewReturnGate {
public:
    void clear() noexcept { presented_.reset(); }
    void cancel() noexcept { clear(); pending_ = false; requested_source_.reset(); }
    void observe(bool presented, bool preview_enabled,
                 const std::optional<PreviewReturnFrame>& candidate) noexcept {
        if (pending_ && candidate && requested_source_ &&
            *requested_source_ != std::pair{candidate->generation, candidate->revision}) {
            cancel();
        }
        if (!presented || preview_enabled || !candidate || !candidate->valid()) {
            clear();
            return;
        }
        presented_ = candidate;
    }
    [[nodiscard]] bool may_resume(bool preview_enabled, bool gesture_active,
                                  const PreviewReturnFrame& current) const noexcept {
        return !preview_enabled && !gesture_active && current.valid() &&
            presented_ && *presented_ == current;
    }
    [[nodiscard]] bool request_resume(bool preview_enabled, bool gesture_active,
                                      const PreviewReturnFrame& current) noexcept {
        if (pending_ || !may_resume(preview_enabled, gesture_active, current)) return false;
        pending_ = true;
        requested_source_ = std::pair{current.generation, current.revision};
        clear();
        return true;
    }
    // Switching preview off is itself an explicit request to return. Keep the
    // lock until a final-layout canvas from this exact source is presented.
    void request_mode_exit(std::uint64_t generation, std::uint64_t revision) noexcept {
        clear();
        pending_ = true;
        requested_source_ = std::pair{generation, revision};
    }
    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] bool finish_resume(bool final_layout_presented, bool preview_enabled,
                                     bool gesture_active, const PreviewReturnFrame& current) noexcept {
        if (!pending_ || !requested_source_ ||
            *requested_source_ != std::pair{current.generation, current.revision} ||
            !final_layout_presented ||
            !may_resume(preview_enabled, gesture_active, current)) return false;
        cancel();
        return true;
    }
private:
    std::optional<PreviewReturnFrame> presented_;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> requested_source_;
    bool pending_ = false;
};

} // namespace carto::studio
