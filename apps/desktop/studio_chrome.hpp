#pragma once

#include <carto/ui/ui.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace carto::studio {

inline constexpr float kStudioMinimumWorkHeight = 96.0F;
inline constexpr float kStudioMaximumExtent = 1'000'000.0F;

// After an item ImGui has already advanced its cursor to the next row. Test
// against that item's actual right edge, not the next row's available width.
inline void studio_continue_row(float next_control_width) {
    if (!std::isfinite(next_control_width) || next_control_width <= 0.0F) return;
    const float content_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float next_left = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
    if (next_left + next_control_width <= content_right) ImGui::SameLine();
}

struct StudioChromeMetrics {
    float header_height = 0.0F;
    float footer_height = 0.0F;
    float control_height = 0.0F;
    float padding = 0.0F;
    float gap = 0.0F;
    bool stacked = true;
};

struct StudioChromeRect {
    ImVec2 origin{};
    ImVec2 extent{};
};

struct StudioChromeLayout {
    StudioChromeRect header;
    StudioChromeRect work_area;
    StudioChromeRect footer;
};

namespace detail {

inline float studio_finite_clamp(
    float value, float minimum, float maximum, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

} // namespace detail

// BeginChild treats zero width as "fill the remaining row". Keep the rail
// positive and reserve a 96px body plus a gutter of up to 16px when possible.
[[nodiscard]] inline float studio_navigation_width(float available_width) noexcept {
    const float width = detail::studio_finite_clamp(
        available_width, 1.0F, kStudioMaximumExtent, 1.0F);
    const float desired = std::clamp(width * 0.20F, 160.0F, 240.0F);
    return std::min(desired, std::max(1.0F, width - 96.0F - 16.0F));
}

// Consume the actual ImGui font size, after font/DPI scaling. Padding is used
// on both sides of each chrome window; a stacked header contains two control
// rows separated by gap. Control-label widths remain the native caller's job.
[[nodiscard]] inline StudioChromeMetrics studio_chrome_metrics(
    float available_width, ui::Density density, float font_size) noexcept {
    const float width = detail::studio_finite_clamp(
        available_width, 0.0F, kStudioMaximumExtent, 0.0F);
    const float font = font_size > 0.0F
        ? detail::studio_finite_clamp(font_size, 8.0F, 64.0F, 16.0F)
        : 16.0F;
    float padding = 8.0F;
    float gap = 6.0F;
    float minimum_control = 32.0F;
    float minimum_footer = 24.0F;
    float wide_threshold = 1280.0F;
    switch (density) {
    case ui::Density::compact:
        padding = 6.0F;
        gap = 5.0F;
        minimum_control = 28.0F;
        minimum_footer = 20.0F;
        wide_threshold = 1152.0F;
        break;
    case ui::Density::touch:
        padding = 10.0F;
        gap = 8.0F;
        minimum_control = 44.0F;
        minimum_footer = 28.0F;
        wide_threshold = 1440.0F;
        break;
    case ui::Density::standard:
    default:
        break;
    }
    wide_threshold *= std::max(1.0F, font / 16.0F);
    const bool stacked = width < wide_threshold;
    const float control = std::ceil(std::max(minimum_control, font + 2.0F * padding));
    return {
        (stacked ? 2.0F * control + gap : control) + 2.0F * padding,
        std::ceil(std::max(minimum_footer, font + 2.0F * padding)),
        control, padding, gap, stacked};
}

// Header, dock host and footer must all use this same partition. At desktop
// sizes metrics are unchanged. On smaller or malformed extents, chrome yields
// space to the work area instead of extending beyond the containing viewport.
// Focus mode reserves no chrome. Origin may be negative on another monitor.
[[nodiscard]] inline StudioChromeLayout studio_work_area(
    ImVec2 origin,
    ImVec2 extent,
    const StudioChromeMetrics& metrics,
    bool focus = false) noexcept {
    origin.x = detail::studio_finite_clamp(
        origin.x, -kStudioMaximumExtent, kStudioMaximumExtent, 0.0F);
    origin.y = detail::studio_finite_clamp(
        origin.y, -kStudioMaximumExtent, kStudioMaximumExtent, 0.0F);
    extent.x = detail::studio_finite_clamp(extent.x, 1.0F, kStudioMaximumExtent, 1.0F);
    extent.y = detail::studio_finite_clamp(extent.y, 1.0F, kStudioMaximumExtent, 1.0F);
    float header = focus ? 0.0F : detail::studio_finite_clamp(
        metrics.header_height, 0.0F, 4096.0F, 0.0F);
    float footer = focus ? 0.0F : detail::studio_finite_clamp(
        metrics.footer_height, 0.0F, 4096.0F, 0.0F);
    const float chrome_budget = extent.y - std::min(extent.y, kStudioMinimumWorkHeight);
    const float requested_chrome = header + footer;
    if (requested_chrome > chrome_budget && requested_chrome > 0.0F) {
        header *= chrome_budget / requested_chrome;
        footer = chrome_budget - header;
    }
    const float work_height = std::max(1.0F, extent.y - header - footer);
    const ImVec2 work_origin{origin.x, origin.y + header};
    const ImVec2 footer_origin{origin.x, work_origin.y + work_height};
    return {
        {origin, {extent.x, header}},
        {work_origin, {extent.x, work_height}},
        {footer_origin, {extent.x, footer}}};
}

} // namespace carto::studio
