#include "../apps/desktop/studio_style.hpp"
#include "../apps/desktop/studio_draft.hpp"
#include "../apps/desktop/studio_chrome.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Context final {
public:
    Context() : context_(ImGui::CreateContext()) {
        require(context_ != nullptr, "headless ImGui context must initialize");
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {640.0F, 480.0F};
        io.DeltaTime = 1.0F / 60.0F;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        require(io.Fonts->Build(), "headless font atlas must build");
    }
    ~Context() { ImGui::DestroyContext(context_); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

private:
    ImGuiContext* context_;
};

carto::ui::UiSnapshot appearance(carto::ui::Theme theme, carto::ui::Density density) {
    carto::ui::UiSnapshot snapshot;
    snapshot.theme = theme;
    snapshot.density = density;
    snapshot.colors = carto::ui::color_tokens(theme);
    return snapshot;
}

bool same(ImVec4 left, ImVec4 right) {
    return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
}

bool same(ImVec2 left, ImVec2 right) {
    return left.x == right.x && left.y == right.y;
}

bool matches(ImVec4 value, carto::ui::UiColor token) {
    return same(value, ImVec4{token.red, token.green, token.blue, token.alpha});
}

double luminance(ImVec4 color) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.x) + 0.7152 * linear(color.y) + 0.0722 * linear(color.z);
}

double contrast(ImVec4 foreground, ImVec4 background) {
    foreground = {
        foreground.x * foreground.w + background.x * (1.0F - foreground.w),
        foreground.y * foreground.w + background.y * (1.0F - foreground.w),
        foreground.z * foreground.w + background.z * (1.0F - foreground.w), 1.0F};
    const double first = luminance(foreground);
    const double second = luminance(background);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

void studio_style_preserves_theme_authority_and_readable_controls() {
    Context context;
    for (const auto theme : {carto::ui::Theme::dark, carto::ui::Theme::light,
                              carto::ui::Theme::high_contrast}) {
        const auto snapshot = appearance(theme, carto::ui::Density::standard);
        carto::studio::apply_studio_style(snapshot);
        const auto& style = ImGui::GetStyle();
        require(matches(style.Colors[ImGuiCol_Text], snapshot.colors.text_primary),
            "native reading text must use the shared theme");
        require(matches(style.Colors[ImGuiCol_WindowBg], snapshot.colors.canvas_0),
            "native backgrounds must not replace the shared theme");
        require(matches(style.Colors[ImGuiCol_NavCursor], snapshot.colors.border_focus),
            "keyboard focus must use the shared focus token");
        require(matches(style.Colors[ImGuiCol_TabSelectedOverline], snapshot.colors.accent_primary),
            "selected tabs need a visible shared interaction accent");
        require(matches(style.Colors[ImGuiCol_TableHeaderBg], snapshot.colors.panel_1),
            "table headers must follow the selected theme");
        require(matches(style.Colors[ImGuiCol_DockingEmptyBg], snapshot.colors.canvas_0),
            "empty docking areas must switch theme with the rest of the shell");
        const double minimum = theme == carto::ui::Theme::high_contrast ? 7.0 : 4.5;
        for (const auto background : {ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg,
                                      ImGuiCol_FrameBg, ImGuiCol_ButtonHovered, ImGuiCol_TableHeaderBg}) {
            require(contrast(style.Colors[ImGuiCol_Text], style.Colors[background]) >= minimum,
                "reading text must retain contrast across windows and interactive surfaces");
        }
        require(contrast(style.Colors[ImGuiCol_NavCursor], style.Colors[ImGuiCol_WindowBg]) >= 3.0,
            "keyboard focus must remain distinguishable from the window surface");
        require(style.Colors[ImGuiCol_NavCursor].w == 1.0F,
            "keyboard focus must not be lost to decorative transparency");
        require(snapshot.theme == theme && snapshot.density == carto::ui::Density::standard,
            "applying native appearance must not rewrite preferences");
    }
}

void studio_style_initializes_every_color_from_shared_tokens() {
    Context context;
    auto snapshot = appearance(carto::ui::Theme::dark, carto::ui::Density::standard);
    std::array<carto::ui::UiColor*, 20U> tokens{
        &snapshot.colors.canvas_0, &snapshot.colors.canvas_1,
        &snapshot.colors.panel_0, &snapshot.colors.panel_1,
        &snapshot.colors.panel_hover, &snapshot.colors.panel_selected,
        &snapshot.colors.text_primary, &snapshot.colors.text_secondary,
        &snapshot.colors.text_disabled, &snapshot.colors.border_soft,
        &snapshot.colors.border_focus, &snapshot.colors.accent_primary,
        &snapshot.colors.accent_secondary, &snapshot.colors.semantic_success,
        &snapshot.colors.semantic_warning, &snapshot.colors.semantic_error,
        &snapshot.colors.semantic_info, &snapshot.colors.provisional,
        &snapshot.colors.stale, &snapshot.colors.modified};
    for (std::size_t index = 0U; index < tokens.size(); ++index) {
        const float value = static_cast<float>(index + 1U) / 24.0F;
        *tokens[index] = {value, value * 0.6F, value * 0.4F, 1.0F};
    }
    for (auto& color : ImGui::GetStyle().Colors) color = {-9.0F, -8.0F, -7.0F, -6.0F};
    carto::studio::apply_studio_style(snapshot);
    for (const auto color : ImGui::GetStyle().Colors) {
        require(std::isfinite(color.w) && color.w >= 0.0F && color.w <= 1.0F,
            "every native color must have a valid initialized opacity");
        require(std::any_of(tokens.begin(), tokens.end(), [color](const auto* token) {
            return color.x == token->red && color.y == token->green && color.z == token->blue;
        }), "every native color must derive from the shared palette, including new ImGui roles");
    }
}

void studio_style_switches_are_reversible_and_ignore_prior_widget_style() {
    Context context;
    auto& style = ImGui::GetStyle();
    style.FontSizeBase = 15.0F;
    style.FontScaleMain = 1.25F;
    style.FontScaleDpi = 1.75F;
    const auto baseline = appearance(carto::ui::Theme::dark, carto::ui::Density::standard);
    carto::studio::apply_studio_style(baseline);
    const ImGuiStyle expected = style;
    carto::studio::apply_studio_style(appearance(
        carto::ui::Theme::high_contrast, carto::ui::Density::touch));
    carto::studio::apply_studio_style(appearance(
        carto::ui::Theme::light, carto::ui::Density::compact));
    style.Alpha = 0.01F;
    style.DisabledAlpha = 0.0F;
    style.WindowMinSize = {-100.0F, -100.0F};
    style.WindowPadding = {std::numeric_limits<float>::quiet_NaN(), -100.0F};
    style.FramePadding = {-100.0F, 1000.0F};
    style.TabRounding = 80.0F;
    style.CellPadding = {1000.0F, 1000.0F};
    style.DockingSeparatorSize = -20.0F;
    style.TouchExtraPadding = {40.0F, 40.0F};
    style.AntiAliasedFill = false;
    style.HoverDelayNormal = 40.0F;
    for (auto& color : style.Colors) color = {-1.0F, -1.0F, -1.0F, -1.0F};
    const auto io_flags = ImGui::GetIO().ConfigFlags;
    carto::studio::apply_studio_style(baseline);
    for (int index = 0; index < ImGuiCol_COUNT; ++index) {
        require(same(style.Colors[index], expected.Colors[index]),
            "switching themes must restore all colors without prior-theme residue");
    }
    require(same(style.WindowPadding, expected.WindowPadding) &&
        same(style.FramePadding, expected.FramePadding) && same(style.CellPadding, expected.CellPadding) &&
        same(style.WindowMinSize, expected.WindowMinSize) &&
        same(style.TouchExtraPadding, expected.TouchExtraPadding) &&
        style.Alpha == expected.Alpha && style.DisabledAlpha == expected.DisabledAlpha &&
        style.TabRounding == expected.TabRounding &&
        style.DockingSeparatorSize == expected.DockingSeparatorSize &&
        style.AntiAliasedFill == expected.AntiAliasedFill &&
        style.HoverDelayNormal == expected.HoverDelayNormal,
        "native metrics and behavior must initialize independently of previous widget style");
    require(style.FontSizeBase == 15.0F && style.FontScaleMain == 1.25F && style.FontScaleDpi == 1.75F,
        "theme switching must preserve host font size, user scaling, and monitor DPI");
    require(ImGui::GetIO().ConfigFlags == io_flags,
        "appearance must not change keyboard navigation or other input authority");
}

struct ButtonFrame {
    ImVec2 minimum;
    ImVec2 maximum;
    bool pressed = false;
};

ButtonFrame draw_button_frame(ImVec2 pointer, bool down) {
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0F, 0.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({640.0F, 480.0F}, ImGuiCond_Always);
    ImGui::Begin("Density controls", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ButtonFrame result;
    result.pressed = ImGui::Button("Apply reviewed source");
    result.minimum = ImGui::GetItemRectMin();
    result.maximum = ImGui::GetItemRectMax();
    ImGui::End();
    ImGui::Render();
    return result;
}

void studio_density_changes_real_control_geometry_and_touch_hit_targets() {
    Context context;
    std::array<float, 3U> heights{};
    const std::array densities{carto::ui::Density::compact,
        carto::ui::Density::standard, carto::ui::Density::touch};
    for (std::size_t index = 0U; index < densities.size(); ++index) {
        carto::studio::apply_studio_style(appearance(carto::ui::Theme::dark, densities[index]));
        const auto frame = draw_button_frame({600.0F, 450.0F}, false);
        heights[index] = frame.maximum.y - frame.minimum.y;
    }
    require(heights[0U] < heights[1U] && heights[1U] < heights[2U],
        "compact, standard, and touch preferences must change real button geometry");
    require(heights[2U] >= 44.0F && ImGui::GetStyle().ScrollbarSize >= 24.0F &&
        ImGui::GetStyle().GrabMinSize >= 24.0F,
        "touch controls must have usable button, scrollbar, and slider target sizes");
    const auto touch = draw_button_frame({600.0F, 450.0F}, false);
    const ImVec2 center{(touch.minimum.x + touch.maximum.x) * 0.5F,
        (touch.minimum.y + touch.maximum.y) * 0.5F};
    static_cast<void>(draw_button_frame(center, false));
    static_cast<void>(draw_button_frame(center, true));
    const auto released = draw_button_frame(center, false);
    require(released.pressed, "the enlarged touch target must activate through normal pointer input");
    ImGui::GetStyle().FontScaleMain = 0.5F;
    carto::studio::apply_studio_style(appearance(
        carto::ui::Theme::dark, carto::ui::Density::touch));
    const auto small_text = draw_button_frame({600.0F, 450.0F}, false);
    require(small_text.maximum.y - small_text.minimum.y >= 44.0F,
        "smaller host text scaling must not shrink touch controls below their minimum target");
    ImGui::GetStyle().FontScaleMain = 1.0F;
    carto::studio::apply_studio_style(appearance(
        carto::ui::Theme::high_contrast, carto::ui::Density::compact));
    const auto compact = draw_button_frame({600.0F, 450.0F}, false);
    require(compact.maximum.y - compact.minimum.y == heights[0U] &&
        same(ImGui::GetStyle().TouchExtraPadding, ImVec2{0.0F, 0.0F}),
        "leaving touch density must restore compact geometry and non-overlapping hit bounds");
}

struct ToolbarRect {
    ImVec2 minimum;
    ImVec2 maximum;
};

struct ToolbarFrame {
    std::array<ToolbarRect, 6U> controls;
    float content_left = 0.0F;
    float content_right = 0.0F;
};

ToolbarFrame draw_toolbar_frame(float width) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = {1024.0F, 768.0F};
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0F, 0.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({width, 480.0F}, ImGuiCond_Always);
    ImGui::Begin("Toolbar wrapping", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ToolbarFrame result;
    result.content_left = ImGui::GetCursorScreenPos().x;
    result.content_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const auto record = [&result](std::size_t index) {
        result.controls[index] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    };
    const auto button_width = [](const char* label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0F;
    };
    ImGui::BeginDisabled();
    static_cast<void>(ImGui::Button("Undo"));
    ImGui::EndDisabled();
    record(0U);
    carto::studio::studio_continue_row(button_width("Redo"));
    ImGui::BeginDisabled();
    static_cast<void>(ImGui::Button("Redo"));
    ImGui::EndDisabled();
    record(1U);
    const float mode_width = std::max(110.0F, ImGui::GetFontSize() * 7.5F);
    carto::studio::studio_continue_row(mode_width);
    int mode = 0;
    const char* modes[] = {"Object", "Vertex", "Edge", "Face"};
    ImGui::SetNextItemWidth(mode_width);
    static_cast<void>(ImGui::Combo("##SelectionMode", &mode, modes, 4));
    record(2U);
    constexpr std::array labels{"Move", "Modify", "More"};
    for (std::size_t index = 0U; index < labels.size(); ++index) {
        carto::studio::studio_continue_row(button_width(labels[index]));
        static_cast<void>(ImGui::Button(labels[index]));
        record(index + 3U);
    }
    ImGui::End();
    ImGui::Render();
    return result;
}

void studio_toolbar_wraps_real_controls_within_narrow_content_bounds() {
    Context context;
    for (const auto density : {carto::ui::Density::compact, carto::ui::Density::standard,
                                carto::ui::Density::touch}) {
        carto::studio::apply_studio_style(appearance(carto::ui::Theme::dark, density));
        for (const float width : {220.0F, 300.0F, 720.0F}) {
            const auto frame = draw_toolbar_frame(width);
            std::size_t wrapped = 0U;
            for (std::size_t index = 0U; index < frame.controls.size(); ++index) {
                const auto& control = frame.controls[index];
                require(control.minimum.x >= frame.content_left - 0.01F &&
                    control.maximum.x <= frame.content_right + 0.01F,
                    "every actual toolbar button and mode combo must fit its content bounds");
                if (index > 0U && control.minimum.y > frame.controls[index - 1U].minimum.y + 0.01F) {
                    ++wrapped;
                }
            }
            require(width < 720.0F ? wrapped > 0U : wrapped == 0U,
                "narrow toolbars must wrap while a sufficiently wide toolbar stays on one row");
        }
    }
}

void studio_toolbar_invalid_or_oversized_width_keeps_the_next_row_cursor() {
    Context context;
    carto::studio::apply_studio_style(appearance(
        carto::ui::Theme::dark, carto::ui::Density::standard));
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0F, 0.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({220.0F, 300.0F}, ImGuiCond_Always);
    ImGui::Begin("Invalid toolbar widths", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    static_cast<void>(ImGui::Button("Undo"));
    const ImVec2 next_row = ImGui::GetCursorScreenPos();
    for (const float width : {0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::max(), 1000.0F}) {
        carto::studio::studio_continue_row(width);
        require(same(ImGui::GetCursorScreenPos(), next_row),
            "invalid or non-fitting widths must not issue SameLine or move the cursor");
    }
    ImGui::End();
    ImGui::Render();
}

void inspector_widget_buffers_do_not_follow_another_owner_or_revision() {
    Context context;
    ImGui::NewFrame();
    ImGui::Begin("Inspector widget binding test");
    using carto::studio::InspectorDraftContext;
    InspectorDraftContext source{1U, carto::core::Revision{3U},
        carto::scene::ObjectId{1U}, carto::geometry::VertexId{1U}};
    const auto field_id = [](InspectorDraftContext binding) {
        const auto scope = carto::studio::inspector_draft_widget_id(binding);
        ImGui::PushID(scope.c_str());
        const ImGuiID id = ImGui::GetID("Position X");
        ImGui::PopID();
        return id;
    };
    const ImGuiID original = field_id(source);
    require(field_id(source) == original, "same-source text editing must retain its widget identity");
    auto different = source;
    different.object.value += (1ULL << 32U);
    require(field_id(different) != original, "full-width owner IDs must isolate active text buffers");
    different = source;
    ++different.vertex.value;
    require(field_id(different) != original, "another vertex must not inherit a focused position buffer");
    different = source;
    different.revision = different.revision.next();
    require(field_id(different) != original, "a new source revision must reset active text ownership");
    different = source;
    ++different.generation;
    require(field_id(different) != original, "same-ID replacement projects need fresh text buffers");
    ImGui::End();
    ImGui::Render();
}

} // namespace

int main() {
    try {
        studio_style_preserves_theme_authority_and_readable_controls();
        studio_style_initializes_every_color_from_shared_tokens();
        studio_style_switches_are_reversible_and_ignore_prior_widget_style();
        studio_density_changes_real_control_geometry_and_touch_hit_targets();
        studio_toolbar_wraps_real_controls_within_narrow_content_bounds();
        studio_toolbar_invalid_or_oversized_width_keeps_the_next_row_cursor();
        inspector_widget_buffers_do_not_follow_another_owner_or_revision();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
