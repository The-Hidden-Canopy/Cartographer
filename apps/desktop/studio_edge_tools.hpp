#pragma once

#include <carto/application/application.hpp>
#include <carto/editor/command_bus.hpp>

#include <limits>

namespace carto::studio {

struct EdgeToolAvailability {
    std::size_t selected_edges = 0U;
    bool split = false;
    bool bevel = false;
    bool dissolve = false;
};

inline EdgeToolAvailability edge_tool_availability(const application::SelectionView& selection) {
    EdgeToolAvailability result;
    if (selection.mode != editor::SelectionMode::edge || selection.component_groups.empty()) {
        return result;
    }
    bool one_per_object = true;
    bool within_dissolve_limit = true;
    for (const auto& group : selection.component_groups) {
        if (group.edges.empty()) return {};
        if (group.edges.size() > std::numeric_limits<std::size_t>::max() - result.selected_edges)
            return {};
        result.selected_edges += group.edges.size();
        one_per_object = one_per_object && group.edges.size() == 1U;
        within_dissolve_limit = within_dissolve_limit &&
            group.edges.size() <= geometry::EditableMesh::kMaxDissolveEdges &&
            group.edges.size() <= geometry::EdgeDissolvePatch::kMaxEdges;
    }
    result.split = one_per_object;
    result.bevel = result.selected_edges <= editor::kMaxSelectedEdgesPerBevel;
    result.dissolve = within_dissolve_limit;
    return result;
}

} // namespace carto::studio
