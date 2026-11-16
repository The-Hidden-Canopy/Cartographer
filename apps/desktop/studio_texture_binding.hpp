#pragma once

#include <carto/render/material.hpp>
#include <imgui.h>

namespace carto::studio {
// Edits only the local material draft. The owner commits via ApplicationAction.
inline bool draw_texture_mapping(render::TextureRef& ref, bool color_map) {
    if (!ref || !ImGui::TreeNode("Mapping and channels")) return false;
    bool changed = false;
    changed |= ImGui::InputScalar("UV set ID", ImGuiDataType_U64, &ref.uv_set);
    changed |= ImGui::InputScalar("Sampler ID (0 = default)", ImGuiDataType_U64, &ref.sampler);
    double offset[]{ref.transform.offset.x, ref.transform.offset.y};
    if (ImGui::InputScalarN("UV offset", ImGuiDataType_Double, offset, 2)) {
        ref.transform.offset = {offset[0], offset[1]}; changed = true;
    }
    double scale[]{ref.transform.scale.x, ref.transform.scale.y};
    if (ImGui::InputScalarN("UV scale", ImGuiDataType_Double, scale, 2)) {
        ref.transform.scale = {scale[0], scale[1]}; changed = true;
    }
    double pivot[]{ref.transform.pivot.x, ref.transform.pivot.y};
    if (ImGui::InputScalarN("UV pivot", ImGuiDataType_Double, pivot, 2)) {
        ref.transform.pivot = {pivot[0], pivot[1]}; changed = true;
    }
    changed |= ImGui::InputDouble("UV rotation (radians)", &ref.transform.rotation, 0.01, 0.1, "%.4f");
    int interpretation = static_cast<int>(ref.interpretation);
    if (ImGui::Combo("Color space", &interpretation,
            color_map ? "Slot default\0Linear\0sRGB\0" : "Slot default (linear)\0Linear\0")) {
        ref.interpretation = static_cast<render::TextureInterpretation>(interpretation); changed = true;
    }
    if (!color_map && ref.interpretation == render::TextureInterpretation::srgb)
        ImGui::TextWrapped("This imported data-map sRGB override is unsupported. Choose Linear before rendering.");
    constexpr const char* channel_names = "R\0G\0B\0A\0One minus R\0One minus G\0One minus B\0One minus A\0Zero\0One\0";
    constexpr const char* labels[]{"Output R", "Output G", "Output B", "Output A"};
    for (unsigned i = 0; i < 4; ++i) {
        int channel = static_cast<int>(ref.channels.rgba[i]);
        if (ImGui::Combo(labels[i], &channel, channel_names)) {
            ref.channels.rgba[i] = static_cast<render::TextureChannel>(channel); changed = true;
        }
    }
    ImGui::TextWrapped("One UV identity per rendered material. All referenced UV sets and samplers must exist. Export reports unsupported color-space and normal-frame conversions.");
    ImGui::TreePop();
    return changed;
}
} // namespace carto::studio
