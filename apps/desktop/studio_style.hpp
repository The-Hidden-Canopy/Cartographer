#pragma once

#include <carto/ui/ui.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace carto::studio {

namespace style_detail {

inline ImVec4 color(ui::UiColor value, float opacity = 1.0F) noexcept {
    return {value.red, value.green, value.blue, value.alpha * opacity};
}

inline float positive_scale(float value) noexcept {
    return std::isfinite(value) && value > 0.0F ? value : 1.0F;
}

} // namespace style_detail

// Apply between frames. The snapshot owns appearance preferences; this adapter
// owns only ImGui presentation and retains the font/DPI scales set by the host.
inline void apply_studio_style(const ui::UiSnapshot& snapshot) noexcept {
    const auto& previous = ImGui::GetStyle();
    ImGuiStyle style;
    style.FontSizeBase = std::isfinite(previous.FontSizeBase) && previous.FontSizeBase >= 0.0F
        ? previous.FontSizeBase : 0.0F;
    style.FontScaleMain = style_detail::positive_scale(previous.FontScaleMain);
    style.FontScaleDpi = style_detail::positive_scale(previous.FontScaleDpi);
    style.Alpha = 1.0F;
    style.DisabledAlpha = 0.65F;
    style.WindowRounding = 6.0F;
    style.ChildRounding = 4.0F;
    style.PopupRounding = 6.0F;
    style.FrameRounding = 4.0F;
    style.GrabRounding = 3.0F;
    style.ScrollbarRounding = 6.0F;
    style.TabRounding = 8.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 0.0F;
    style.TabBarOverlineSize = 3.0F;
    style.WindowTitleAlign = {0.0F, 0.5F};
    style.ButtonTextAlign = {0.5F, 0.5F};
    style.SelectableTextAlign = {0.0F, 0.5F};
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextAlign = {0.0F, 0.5F};
    style.TreeLinesSize = 1.0F;
    style.TreeLinesRounding = 2.0F;
    style.DragDropTargetBorderSize = 2.0F;
    style.DragDropTargetRounding = 4.0F;

    switch (snapshot.density) {
    case ui::Density::compact:
        style.WindowPadding = {10.0F, 8.0F};
        style.FramePadding = {8.0F, 4.0F};
        style.ItemSpacing = {6.0F, 5.0F};
        style.ItemInnerSpacing = {4.0F, 4.0F};
        style.CellPadding = {6.0F, 4.0F};
        style.ScrollbarSize = 12.0F;
        style.GrabMinSize = 12.0F;
        style.WindowBorderHoverPadding = 4.0F;
        style.DockingSeparatorSize = 3.0F;
        style.TouchExtraPadding = {0.0F, 0.0F};
        break;
    case ui::Density::touch:
        style.WindowPadding = {18.0F, 16.0F};
        style.FramePadding = {16.0F, 16.0F};
        // Keep default-sized controls reachable when the host reduces text scale.
        style.FramePadding.y = std::max(style.FramePadding.y,
            (44.0F - (style.FontSizeBase > 0.0F ? style.FontSizeBase : 13.0F) *
                style.FontScaleMain * style.FontScaleDpi) * 0.5F);
        style.ItemSpacing = {12.0F, 12.0F};
        style.ItemInnerSpacing = {8.0F, 8.0F};
        style.CellPadding = {12.0F, 10.0F};
        style.ScrollbarSize = 24.0F;
        style.GrabMinSize = 24.0F;
        style.WindowBorderHoverPadding = 8.0F;
        style.DockingSeparatorSize = 8.0F;
        style.TouchExtraPadding = {2.0F, 2.0F};
        break;
    case ui::Density::standard:
    default:
        style.WindowPadding = {14.0F, 12.0F};
        style.FramePadding = {10.0F, 7.0F};
        style.ItemSpacing = {8.0F, 8.0F};
        style.ItemInnerSpacing = {6.0F, 5.0F};
        style.CellPadding = {8.0F, 6.0F};
        style.ScrollbarSize = 16.0F;
        style.GrabMinSize = 16.0F;
        style.WindowBorderHoverPadding = 5.0F;
        style.DockingSeparatorSize = 4.0F;
        style.TouchExtraPadding = {0.0F, 0.0F};
        break;
    }
    style.IndentSpacing = style.FramePadding.x * 2.0F;
    style.ColumnsMinSpacing = style.FramePadding.x + 4.0F;
    style.SeparatorTextPadding = {style.FramePadding.x, style.FramePadding.y};

    const auto& c = snapshot.colors;
    using style_detail::color;
    style.Colors[ImGuiCol_Text] = color(c.text_primary);
    style.Colors[ImGuiCol_TextDisabled] = color(c.text_disabled);
    style.Colors[ImGuiCol_WindowBg] = color(c.canvas_0);
    style.Colors[ImGuiCol_ChildBg] = color(c.panel_0);
    style.Colors[ImGuiCol_PopupBg] = color(c.panel_1);
    style.Colors[ImGuiCol_Border] = color(c.border_soft);
    style.Colors[ImGuiCol_BorderShadow] = color(c.canvas_0, 0.0F);
    style.Colors[ImGuiCol_FrameBg] = color(c.panel_1);
    style.Colors[ImGuiCol_FrameBgHovered] = color(c.panel_hover);
    style.Colors[ImGuiCol_FrameBgActive] = color(c.panel_selected);
    style.Colors[ImGuiCol_TitleBg] = color(c.canvas_1);
    style.Colors[ImGuiCol_TitleBgActive] = color(c.panel_0);
    style.Colors[ImGuiCol_TitleBgCollapsed] = color(c.canvas_0);
    style.Colors[ImGuiCol_MenuBarBg] = color(c.panel_0);
    style.Colors[ImGuiCol_ScrollbarBg] = color(c.canvas_0);
    style.Colors[ImGuiCol_ScrollbarGrab] = color(c.border_soft);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = color(c.border_focus);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = color(c.accent_primary);
    style.Colors[ImGuiCol_CheckMark] = color(c.accent_primary);
    style.Colors[ImGuiCol_CheckboxSelectedBg] = color(c.panel_selected);
    style.Colors[ImGuiCol_SliderGrab] = color(c.accent_primary);
    style.Colors[ImGuiCol_SliderGrabActive] = color(c.accent_secondary);
    style.Colors[ImGuiCol_Button] = color(c.panel_1);
    style.Colors[ImGuiCol_ButtonHovered] = color(c.panel_hover);
    style.Colors[ImGuiCol_ButtonActive] = color(c.panel_selected);
    style.Colors[ImGuiCol_Header] = color(c.panel_0);
    style.Colors[ImGuiCol_HeaderHovered] = color(c.panel_hover);
    style.Colors[ImGuiCol_HeaderActive] = color(c.panel_selected);
    style.Colors[ImGuiCol_Separator] = color(c.border_soft);
    style.Colors[ImGuiCol_SeparatorHovered] = color(c.border_focus);
    style.Colors[ImGuiCol_SeparatorActive] = color(c.accent_primary);
    style.Colors[ImGuiCol_ResizeGrip] = color(c.border_soft);
    style.Colors[ImGuiCol_ResizeGripHovered] = color(c.border_focus);
    style.Colors[ImGuiCol_ResizeGripActive] = color(c.accent_primary);
    style.Colors[ImGuiCol_InputTextCursor] = color(c.text_primary);
    style.Colors[ImGuiCol_TabHovered] = color(c.panel_hover);
    style.Colors[ImGuiCol_Tab] = color(c.panel_0);
    style.Colors[ImGuiCol_TabSelected] = color(c.panel_selected);
    style.Colors[ImGuiCol_TabSelectedOverline] = color(c.accent_primary);
    style.Colors[ImGuiCol_TabDimmed] = color(c.canvas_1);
    style.Colors[ImGuiCol_TabDimmedSelected] = color(c.panel_1);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = color(c.border_focus);
    style.Colors[ImGuiCol_DockingPreview] = color(c.accent_primary, 0.55F);
    style.Colors[ImGuiCol_DockingEmptyBg] = color(c.canvas_0);
    style.Colors[ImGuiCol_PlotLines] = color(c.accent_primary);
    style.Colors[ImGuiCol_PlotLinesHovered] = color(c.accent_secondary);
    style.Colors[ImGuiCol_PlotHistogram] = color(c.accent_secondary);
    style.Colors[ImGuiCol_PlotHistogramHovered] = color(c.semantic_warning);
    style.Colors[ImGuiCol_TableHeaderBg] = color(c.panel_1);
    style.Colors[ImGuiCol_TableBorderStrong] = color(c.border_soft);
    style.Colors[ImGuiCol_TableBorderLight] = color(c.border_soft,
        snapshot.theme == ui::Theme::high_contrast ? 1.0F : 0.65F);
    style.Colors[ImGuiCol_TableRowBg] = color(c.panel_0);
    style.Colors[ImGuiCol_TableRowBgAlt] = color(c.panel_1);
    style.Colors[ImGuiCol_TextLink] = color(c.accent_primary);
    style.Colors[ImGuiCol_TextSelectedBg] = color(c.panel_selected);
    style.Colors[ImGuiCol_TreeLines] = color(c.border_soft);
    style.Colors[ImGuiCol_DragDropTarget] = color(c.border_focus);
    style.Colors[ImGuiCol_DragDropTargetBg] = color(c.panel_selected);
    style.Colors[ImGuiCol_UnsavedMarker] = color(c.modified);
    style.Colors[ImGuiCol_NavCursor] = color(c.border_focus);
    style.Colors[ImGuiCol_NavWindowingHighlight] = color(c.border_focus);
    style.Colors[ImGuiCol_NavWindowingDimBg] = color(c.canvas_0, 0.60F);
    style.Colors[ImGuiCol_ModalWindowDimBg] = color(c.canvas_0, 0.72F);
    ImGui::GetStyle() = style;
}

} // namespace carto::studio
