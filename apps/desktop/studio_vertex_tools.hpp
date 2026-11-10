#pragma once

#include <carto/application/application.hpp>
#include <carto/editor/command_bus.hpp>

#include <limits>

namespace carto::studio {

struct VertexToolAvailability {
    std::size_t selected_vertices = 0U;
    bool single = false;
    bool bevel = false;
};

inline VertexToolAvailability vertex_tool_availability(const application::SelectionView& selection) {
    VertexToolAvailability result;
    if (selection.mode != editor::SelectionMode::vertex || selection.component_groups.empty())
        return result;
    for (const auto& group : selection.component_groups) {
        if (group.vertices.empty()) return {};
        if (group.vertices.size() > std::numeric_limits<std::size_t>::max() - result.selected_vertices)
            return {};
        result.selected_vertices += group.vertices.size();
    }
    result.single = result.selected_vertices == 1U && selection.component_groups.size() == 1U;
    result.bevel = result.selected_vertices <= editor::kMaxSelectedVerticesPerBevel;
    return result;
}

} // namespace carto::studio
