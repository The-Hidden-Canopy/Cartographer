#pragma once

#include <carto/application/application.hpp>
#include <carto/editor/command_bus.hpp>

#include <limits>

namespace carto::studio {

inline constexpr std::size_t kMaxInteractiveFaceBatch = editor::kMaxSelectedFacesPerDeletion;

struct FaceToolAvailability {
    std::size_t selected_faces = 0U;
    std::size_t selected_objects = 0U;
    bool individual = false;
    bool remove = false;
    bool single = false;
    bool triangle_pair = false;
};

inline FaceToolAvailability face_tool_availability(const application::SelectionView& selection) {
    FaceToolAvailability result;
    if (selection.mode != editor::SelectionMode::face || selection.component_groups.empty())
        return result;
    for (const auto& group : selection.component_groups) {
        if (group.faces.empty()) return {};
        if (group.faces.size() > std::numeric_limits<std::size_t>::max() - result.selected_faces)
            return {};
        result.selected_faces += group.faces.size();
    }
    result.selected_objects = selection.component_groups.size();
    result.individual = result.selected_faces <= kMaxInteractiveFaceBatch;
    result.remove = result.selected_faces <= editor::kMaxSelectedFacesPerDeletion;
    result.single = result.selected_faces == 1U && result.selected_objects == 1U;
    result.triangle_pair = result.selected_faces == 2U && result.selected_objects == 1U;
    return result;
}

} // namespace carto::studio
