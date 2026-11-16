#pragma once

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace carto::studio {

struct PreviewRenderExtent {
    std::uint32_t width = 0, height = 0;
    bool capped = false;
};

// Two samples per canvas axis retain thin silhouette detail that misses a
// single pixel-center sample. Temporal reprojection alone adds no samples.
// Keep the existing renderer's pixel and dimension budgets authoritative.
inline std::optional<PreviewRenderExtent> preview_render_extent(
        double width, double height, int scale_index,
        std::uint64_t max_pixels, std::uint32_t max_dimension) {
    constexpr std::array scales{2.0, 1.0, .5, .25};
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        !std::isfinite(width / height) || width / height <= 0 || scale_index < 0 ||
        static_cast<std::size_t>(scale_index) >= scales.size() || !max_pixels || !max_dimension)
        return std::nullopt;
    const double scale = scales[static_cast<std::size_t>(scale_index)];
    const double budget = static_cast<double>(max_pixels);
    const double requested_width = std::clamp(width * scale, 1.0, budget);
    const double requested_height = std::clamp(height * scale, 1.0, budget);
    const double limit = std::min(1.0,
        std::min(std::sqrt(budget / (requested_width * requested_height)),
            double(max_dimension) / std::max(requested_width, requested_height)));
    PreviewRenderExtent result;
    result.width = static_cast<std::uint32_t>(std::max(1.0, std::floor(requested_width * limit)));
    result.height = static_cast<std::uint32_t>(std::max(1.0, std::floor(requested_height * limit)));
    if (std::uint64_t(result.width) * result.height > max_pixels)
        result.width = static_cast<std::uint32_t>(max_pixels / result.height);
    result.capped = limit < 1.0 || width * scale > budget || height * scale > budget;
    return result;
}

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
