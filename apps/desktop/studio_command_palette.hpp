#pragma once

#include <carto/ui/ui.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace carto::studio {

struct CommandPaletteState {
    bool was_open = false;
    std::optional<std::size_t> selected;
};

struct CommandPaletteResult {
    std::string query;
    std::optional<std::size_t> command;
    bool closed = false;
};

// Owns only the palette window. The caller must suppress underlying shortcuts
// while it is open and dispatch the returned command at most once.
inline CommandPaletteResult draw_command_palette(CommandPaletteState& state,
    bool visible, std::string_view search, const std::vector<ui::CommandView>& commands) {
    CommandPaletteResult result{std::string(search), std::nullopt, false};
    if (!visible) {
        state.was_open = false;
        return result;
    }
    const bool opening = !state.was_open;
    state.was_open = true;
    if (opening) state.selected.reset();
    bool open = true;
    if (!ImGui::Begin("Command Palette", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        result.closed = !open;
        if (!open) state.was_open = false;
        ImGui::End();
        return result;
    }
    std::array<char, 1025> query{};
    std::copy_n(search.data(), std::min(search.size(), query.size() - 1U), query.data());
    if (opening) ImGui::SetKeyboardFocusHere();
    const bool search_had_focus = ImGui::GetActiveID() == ImGui::GetID("Search");
    if (ImGui::InputText("Search", query.data(), query.size())) state.selected.reset();
    // InputText can deactivate on Enter/Escape during this frame.
    const bool search_owns_keys = search_had_focus || ImGui::IsItemActive();
    if (search_owns_keys) {
        // Reserve only our result-navigation keys. Otherwise ImGui's native
        // navigation can leave Search on Down before the next Enter frame.
        ImGui::SetKeyOwner(ImGuiKey_UpArrow, ImGui::GetID("Search"));
        ImGui::SetKeyOwner(ImGuiKey_DownArrow, ImGui::GetID("Search"));
    }
    result.query = query.data();
    const auto results = ui::command_palette_results(commands, result.query);
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    int direction = 0;
    if (focused && search_owns_keys && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) direction = 1;
    else if (focused && search_owns_keys && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) direction = -1;
    state.selected = ui::command_palette_next(commands, results, state.selected, direction);
    const bool dismiss = focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const bool invoke = !dismiss && focused && search_owns_keys &&
        ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (dismiss) open = false;
    if (results.empty()) ImGui::TextDisabled("No matching commands");
    for (std::size_t row = 0; open && row < results.size(); ++row) {
        const auto& command = commands[results[row]];
        if (!command.supported) {
            ImGui::TextDisabled("%s - unavailable: %s", command.label.c_str(), command.unavailable_reason.c_str());
            continue;
        }
        const bool selected = state.selected == row;
        const bool clicked = ImGui::Selectable((command.label +
            (command.shortcut.empty() ? "" : "    " + command.shortcut) + "##" + command.id).c_str(), selected);
        if (!search_owns_keys && ImGui::IsItemFocused()) state.selected = row;
        if (selected && direction != 0) ImGui::SetScrollHereY();
        if (clicked || (invoke && selected)) {
            result.command = results[row];
            open = false;
            break;
        }
    }
    result.closed = !open;
    if (!open) state.was_open = false;
    ImGui::End();
    return result;
}

} // namespace carto::studio
