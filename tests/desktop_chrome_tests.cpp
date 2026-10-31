#include "../apps/desktop/studio_chrome.hpp"

#include <array>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw std::runtime_error(std::string("requirement failed: ") +   \
            #condition);                                                                 \
    } while (false)

bool near(float first, float second, float tolerance = 0.01F) {
    return std::abs(first - second) <= tolerance;
}

void require_finite_rect(const carto::studio::StudioChromeRect& rect) {
    REQUIRE(std::isfinite(rect.origin.x) && std::isfinite(rect.origin.y));
    REQUIRE(std::isfinite(rect.extent.x) && std::isfinite(rect.extent.y));
    REQUIRE(rect.extent.x >= 1.0F && rect.extent.y >= 0.0F);
    REQUIRE(std::isfinite(rect.origin.x + rect.extent.x));
    REQUIRE(std::isfinite(rect.origin.y + rect.extent.y));
}

void metrics_respond_to_density_and_actual_font_pressure() {
    using namespace carto;
    const auto compact = studio::studio_chrome_metrics(1600.0F, ui::Density::compact, 16.0F);
    const auto standard = studio::studio_chrome_metrics(1600.0F, ui::Density::standard, 16.0F);
    const auto touch = studio::studio_chrome_metrics(1600.0F, ui::Density::touch, 16.0F);
    REQUIRE(!compact.stacked && !standard.stacked && !touch.stacked);
    REQUIRE(compact.control_height < standard.control_height);
    REQUIRE(standard.control_height < touch.control_height);
    REQUIRE(compact.padding < standard.padding && standard.padding < touch.padding);
    REQUIRE(!studio::studio_chrome_metrics(1280.0F, ui::Density::standard, 16.0F).stacked);
    REQUIRE(studio::studio_chrome_metrics(1279.0F, ui::Density::standard, 16.0F).stacked);
    REQUIRE(studio::studio_chrome_metrics(1280.0F, ui::Density::touch, 16.0F).stacked);
    REQUIRE(!studio::studio_chrome_metrics(1440.0F, ui::Density::touch, 16.0F).stacked);
    const auto enlarged = studio::studio_chrome_metrics(1600.0F, ui::Density::standard, 32.0F);
    REQUIRE(enlarged.stacked && enlarged.control_height >= 32.0F + 2.0F * enlarged.padding);
    REQUIRE(enlarged.footer_height >= 32.0F + 2.0F * enlarged.padding);
    REQUIRE(!studio::studio_chrome_metrics(2560.0F, ui::Density::standard, 32.0F).stacked);
    const auto unknown = studio::studio_chrome_metrics(1600.0F,
        static_cast<ui::Density>(999), 16.0F);
    REQUIRE(unknown.header_height == standard.header_height);
    REQUIRE(unknown.padding == standard.padding && unknown.stacked == standard.stacked);
}

void desktop_chrome_partitions_without_overlap_and_focus_restores_full_extent() {
    using namespace carto;
    const std::array sizes{ImVec2{720.0F, 520.0F}, ImVec2{960.0F, 640.0F},
        ImVec2{1280.0F, 720.0F}, ImVec2{1920.0F, 1080.0F}};
    for (const auto size : sizes) {
        for (const auto density : {ui::Density::compact, ui::Density::standard, ui::Density::touch}) {
            for (const float font : {8.0F, 13.0F, 16.0F, 24.0F, 40.0F, 64.0F}) {
                const auto metrics = studio::studio_chrome_metrics(size.x, density, font);
                const ImVec2 origin{-1920.0F, 32.0F};
                const auto layout = studio::studio_work_area(origin, size, metrics);
                require_finite_rect(layout.header);
                require_finite_rect(layout.work_area);
                require_finite_rect(layout.footer);
                REQUIRE(layout.header.origin.x == origin.x && layout.header.origin.y == origin.y);
                REQUIRE(near(layout.work_area.origin.y, origin.y + metrics.header_height));
                REQUIRE(near(layout.header.extent.y, metrics.header_height));
                REQUIRE(near(layout.footer.extent.y, metrics.footer_height));
                REQUIRE(near(layout.footer.origin.y, layout.work_area.origin.y +
                    layout.work_area.extent.y));
                REQUIRE(near(layout.footer.origin.y + layout.footer.extent.y, origin.y + size.y));
                REQUIRE(layout.work_area.extent.x == size.x);
                REQUIRE(layout.work_area.extent.y >= studio::kStudioMinimumWorkHeight);
                const auto focus = studio::studio_work_area(origin, size, metrics, true);
                REQUIRE(focus.header.extent.y == 0.0F && focus.footer.extent.y == 0.0F);
                REQUIRE(focus.work_area.origin.x == origin.x && focus.work_area.origin.y == origin.y);
                REQUIRE(focus.work_area.extent.x == size.x && focus.work_area.extent.y == size.y);
            }
        }
    }
}

void malformed_metrics_and_small_extents_remain_bounded() {
    using namespace carto;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::array values{nan, infinity, -infinity, -1.0F, 0.0F,
        std::numeric_limits<float>::denorm_min(), 40.0F,
        std::numeric_limits<float>::max()};
    for (const float width : values) {
        for (const float font : values) {
            const auto metrics = studio::studio_chrome_metrics(width, ui::Density::touch, font);
            REQUIRE(std::isfinite(metrics.header_height) && metrics.header_height > 0.0F);
            REQUIRE(std::isfinite(metrics.footer_height) && metrics.footer_height > 0.0F);
            REQUIRE(std::isfinite(metrics.control_height) && metrics.control_height > 0.0F);
            for (const float height : values) {
                const auto layout = studio::studio_work_area({width, -width}, {width, height}, metrics);
                require_finite_rect(layout.header);
                require_finite_rect(layout.work_area);
                require_finite_rect(layout.footer);
                REQUIRE(layout.work_area.extent.y >= 1.0F);
                REQUIRE(layout.header.extent.y + layout.work_area.extent.y +
                    layout.footer.extent.y <= studio::kStudioMaximumExtent + 1.0F);
            }
        }
    }
    auto forged = studio::studio_chrome_metrics(720.0F, ui::Density::touch, 64.0F);
    forged.header_height = std::numeric_limits<float>::max();
    forged.footer_height = std::numeric_limits<float>::max();
    const auto fitted = studio::studio_work_area({}, {720.0F, 120.0F}, forged);
    REQUIRE(near(fitted.header.extent.y + fitted.footer.extent.y, 24.0F));
    REQUIRE(near(fitted.work_area.extent.y, studio::kStudioMinimumWorkHeight));
    const auto tiny = studio::studio_work_area({}, {8.0F, 12.0F}, forged);
    REQUIRE(tiny.header.extent.y == 0.0F && tiny.footer.extent.y == 0.0F);
    REQUIRE(tiny.work_area.extent.x == 8.0F && tiny.work_area.extent.y == 12.0F);
    forged.header_height = nan;
    forged.footer_height = -infinity;
    const auto invalid_chrome = studio::studio_work_area({}, {720.0F, 520.0F}, forged);
    REQUIRE(invalid_chrome.work_area.extent.y == 520.0F);
}

void actual_imgui_controls_fit_the_same_reserved_header_and_footer() {
    using namespace carto;
    struct Context {
        ImGuiContext* value = ImGui::CreateContext();
        ~Context() { ImGui::DestroyContext(value); }
    } context;
    REQUIRE(context.value != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.0F / 60.0F;
    REQUIRE(io.Fonts->Build());
    for (const ImVec2 size : {ImVec2{720.0F, 520.0F}, ImVec2{1920.0F, 1080.0F}}) {
        for (const auto density : {ui::Density::compact, ui::Density::standard, ui::Density::touch}) {
            io.DisplaySize = size;
            ImGui::NewFrame();
            const auto metrics = studio::studio_chrome_metrics(size.x, density, ImGui::GetFontSize());
            const auto layout = studio::studio_work_area({}, size, metrics);
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {metrics.padding, metrics.padding});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0F, 1.0F});
            ImGui::SetNextWindowPos(layout.header.origin, ImGuiCond_Always);
            ImGui::SetNextWindowSize(layout.header.extent, ImGuiCond_Always);
            ImGui::Begin("Studio header metrics test", nullptr, flags);
            REQUIRE(near(ImGui::GetWindowSize().x, layout.header.extent.x));
            REQUIRE(near(ImGui::GetWindowSize().y, layout.header.extent.y));
            ImGui::SetCursorPos({metrics.padding, metrics.padding});
            static_cast<void>(ImGui::Button("Project", {120.0F, metrics.control_height}));
            if (metrics.stacked) {
                ImGui::SetCursorPos({metrics.padding,
                    metrics.padding + metrics.control_height + metrics.gap});
            } else {
                ImGui::SameLine(0.0F, metrics.gap);
            }
            static_cast<void>(ImGui::Button("Workspace", {200.0F, metrics.control_height}));
            const auto header_control_end = ImGui::GetItemRectMax();
            REQUIRE(header_control_end.x <= layout.header.origin.x + layout.header.extent.x);
            REQUIRE(header_control_end.y <= layout.header.origin.y + layout.header.extent.y);
            ImGui::End();
            ImGui::SetNextWindowPos(layout.footer.origin, ImGuiCond_Always);
            ImGui::SetNextWindowSize(layout.footer.extent, ImGuiCond_Always);
            ImGui::Begin("Studio footer metrics test", nullptr, flags);
            REQUIRE(near(ImGui::GetWindowSize().x, layout.footer.extent.x));
            REQUIRE(near(ImGui::GetWindowSize().y, layout.footer.extent.y));
            ImGui::SetCursorPos({metrics.padding, metrics.padding});
            ImGui::TextUnformatted("Saved / revision 7");
            REQUIRE(ImGui::GetItemRectMax().y <= layout.footer.origin.y + layout.footer.extent.y);
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::Render();
        }
    }
}

void navigation_width_is_bounded_and_preserves_content_space() {
    using carto::studio::studio_navigation_width;
    REQUIRE(near(studio_navigation_width(720.0F), 160.0F));
    REQUIRE(near(studio_navigation_width(960.0F), 192.0F));
    REQUIRE(near(studio_navigation_width(1440.0F), 240.0F));
    for (const float width : {113.0F, 160.0F, 200.0F, 320.0F, 720.0F, 1440.0F}) {
        const float rail = studio_navigation_width(width);
        REQUIRE(rail >= 1.0F && rail <= 240.0F);
        REQUIRE(width - rail - 16.0F >= 96.0F);
    }
    for (const float width : {std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
             -100.0F, 0.0F, std::numeric_limits<float>::denorm_min(),
             std::numeric_limits<float>::max()}) {
        const float rail = studio_navigation_width(width);
        REQUIRE(std::isfinite(rail) && rail >= 1.0F && rail <= 240.0F);
    }
}

void navigation_and_settings_body_remain_visible_in_real_child_layout() {
    struct Context {
        ImGuiContext* value = ImGui::CreateContext();
        ~Context() { ImGui::DestroyContext(value); }
    } context;
    REQUIRE(context.value != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.0F / 60.0F;
    REQUIRE(io.Fonts->Build());
    for (const float width : {720.0F, 960.0F, 1440.0F}) {
        for (int frame = 0; frame < 2; ++frame) {
            io.DisplaySize = {width, 520.0F};
            ImGui::NewFrame();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0F, 8.0F});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0F, 1.0F});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {16.0F, 6.0F});
            ImGui::SetNextWindowPos({}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({width, 520.0F}, ImGuiCond_Always);
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
            ImGui::Begin("Studio nonworkspace layout test", nullptr, flags);
            ImGui::BeginChild("ShellBody", {}, false);
            const ImVec2 host_origin = ImGui::GetCursorScreenPos();
            const float available_width = ImGui::GetContentRegionAvail().x;
            const bool navigation_visible = ImGui::BeginChild("ProductRail",
                {carto::studio::studio_navigation_width(available_width), 0.0F}, true);
            const ImVec2 navigation_origin = ImGui::GetWindowPos();
            const ImVec2 navigation_extent = ImGui::GetWindowSize();
            for (const char* label : {"Workspace", "Project", "Market", "Diagnostics"}) {
                static_cast<void>(ImGui::Selectable(label, false, 0, {0.0F, 32.0F}));
            }
            static_cast<void>(ImGui::Selectable("Settings", true, 0, {0.0F, 32.0F}));
            const bool settings_visible = ImGui::IsItemVisible();
            ImGui::EndChild();
            ImGui::SameLine();
            const bool body_visible = ImGui::BeginChild("SurfaceHost", {}, false);
            const ImVec2 body_origin = ImGui::GetWindowPos();
            const ImVec2 body_extent = ImGui::GetWindowSize();
            ImGui::TextUnformatted("Settings");
            static_cast<void>(ImGui::Button("Density"));
            const bool body_control_visible = ImGui::IsItemVisible();
            REQUIRE(navigation_visible && settings_visible && body_visible && body_control_visible);
            REQUIRE(navigation_extent.x >= 1.0F && body_extent.x >= 96.0F);
            REQUIRE(navigation_origin.x + navigation_extent.x <= body_origin.x);
            REQUIRE(body_origin.x + body_extent.x <= host_origin.x + available_width + 0.5F);
            REQUIRE(body_origin.x < host_origin.x + available_width);
            ImGui::EndChild();
            ImGui::EndChild();
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::Render();
        }
    }
}

} // namespace

int main() {
    try {
        metrics_respond_to_density_and_actual_font_pressure();
        desktop_chrome_partitions_without_overlap_and_focus_restores_full_extent();
        malformed_metrics_and_small_extents_remain_bounded();
        actual_imgui_controls_fit_the_same_reserved_header_and_footer();
        navigation_width_is_bounded_and_preserves_content_space();
        navigation_and_settings_body_remain_visible_in_real_child_layout();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
