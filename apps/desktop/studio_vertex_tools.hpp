#pragma once

#include <carto/application/application.hpp>
#include <carto/editor/command_bus.hpp>

#include <algorithm>
#include <limits>

namespace carto::studio {

struct VertexToolAvailability {
    std::size_t selected_vertices = 0U;
    bool single = false;
    bool bevel = false;
    bool merge_pair = false;
    bool merge_last_selected = false;
    bool merge_by_distance = false;
};

inline VertexToolAvailability vertex_tool_availability(const application::SelectionView& selection) {
    VertexToolAvailability result;
    if (selection.mode != editor::SelectionMode::vertex || selection.component_groups.empty())
        return result;
    bool two_per_object = true;
    bool at_least_two_per_object = true;
    bool ordered_pair_per_object = true;
    for (const auto& group : selection.component_groups) {
        if (group.vertices.empty()) return {};
        if (group.vertices.size() > std::numeric_limits<std::size_t>::max() - result.selected_vertices)
            return {};
        result.selected_vertices += group.vertices.size();
        two_per_object = two_per_object && group.vertices.size() == 2U;
        ordered_pair_per_object = ordered_pair_per_object &&
            group.vertex_selection_order.size() == 2U &&
            group.vertex_selection_order[0] != group.vertex_selection_order[1] &&
            std::find(group.vertices.begin(), group.vertices.end(), group.vertex_selection_order[0]) != group.vertices.end() &&
            std::find(group.vertices.begin(), group.vertices.end(), group.vertex_selection_order[1]) != group.vertices.end();
        at_least_two_per_object = at_least_two_per_object && group.vertices.size() >= 2U;
    }
    result.single = result.selected_vertices == 1U && selection.component_groups.size() == 1U;
    result.bevel = result.selected_vertices <= editor::kMaxSelectedVerticesPerBevel;
    result.merge_pair = two_per_object;
    result.merge_last_selected = two_per_object && ordered_pair_per_object;
    result.merge_by_distance = at_least_two_per_object;
    return result;
}

inline application::InvokeToolAction merge_at_last_selected_action() {
    editor::ToolArguments arguments;
    arguments.merge_mode = geometry::VertexMergeMode::last_selected;
    return {"mesh.merge-vertices", arguments};
}

} // namespace carto::studio
