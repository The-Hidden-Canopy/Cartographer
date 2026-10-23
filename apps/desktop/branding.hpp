#pragma once

#include <imgui.h>

#include <array>

namespace carto::desktop_branding {

// The mark is drawn from primitives instead of loading the full brand sheet
// at runtime. That keeps the native shell crisp at any DPI and makes the
// identity available before the renderer has loaded project assets.
inline void draw_mark(
    ImDrawList* draw_list,
    ImVec2 center,
    float size,
    ImU32 slate,
    ImU32 stone,
    ImU32 ice,
    ImU32 blue,
    ImU32 bronze) {
    if (draw_list == nullptr || size <= 0.0F) return;

    const auto point = [center, size](float x, float y) {
        return ImVec2(center.x + x * size, center.y + y * size);
    };

    const float compass_width = size < 40.0F ? 1.0F : 1.35F;
    draw_list->AddCircle(center, size * 0.49F, bronze, 40, compass_width);
    draw_list->AddLine(point(-0.58F, 0.0F), point(-0.42F, 0.0F), bronze, compass_width);
    draw_list->AddLine(point(0.42F, 0.0F), point(0.58F, 0.0F), bronze, compass_width);
    draw_list->AddLine(point(0.0F, -0.58F), point(0.0F, -0.42F), bronze, compass_width);
    draw_list->AddLine(point(0.0F, 0.42F), point(0.0F, 0.58F), bronze, compass_width);
    draw_list->AddTriangleFilled(
        point(0.0F, -0.61F), point(-0.035F, -0.49F), point(0.035F, -0.49F), bronze);
    draw_list->AddTriangleFilled(
        point(0.0F, 0.61F), point(-0.035F, 0.49F), point(0.035F, 0.49F), bronze);

    const std::array<ImVec2, 6> mark{
        point(0.37F, -0.20F),
        point(0.0F, -0.48F),
        point(-0.39F, -0.22F),
        point(-0.39F, 0.22F),
        point(0.0F, 0.48F),
        point(0.37F, 0.20F),
    };
    std::array<ImVec2, 6> shadow = mark;
    for (auto& vertex : shadow) {
        vertex.x += size * 0.028F;
        vertex.y += size * 0.034F;
    }
    draw_list->AddPolyline(
        shadow.data(), static_cast<int>(shadow.size()), slate, ImDrawFlags_None, size * 0.245F);
    draw_list->AddPolyline(
        mark.data(), static_cast<int>(mark.size()), stone, ImDrawFlags_None, size * 0.215F);

    const std::array<ImVec2, 3> upper{mark[0], mark[1], mark[2]};
    const std::array<ImVec2, 4> lower{mark[2], mark[3], mark[4], mark[5]};
    draw_list->AddPolyline(
        upper.data(), static_cast<int>(upper.size()), ice, ImDrawFlags_None, size * 0.145F);
    draw_list->AddPolyline(
        lower.data(), static_cast<int>(lower.size()), slate, ImDrawFlags_None, size * 0.135F);

    const std::array<ImVec2, 4> cube_top{
        point(0.0F, -0.17F), point(0.145F, -0.085F),
        point(0.0F, 0.0F), point(-0.145F, -0.085F),
    };
    const std::array<ImVec2, 4> cube_left{
        point(-0.145F, -0.085F), point(0.0F, 0.0F),
        point(0.0F, 0.17F), point(-0.145F, 0.085F),
    };
    const std::array<ImVec2, 4> cube_right{
        point(0.0F, 0.0F), point(0.145F, -0.085F),
        point(0.145F, 0.085F), point(0.0F, 0.17F),
    };
    draw_list->AddConvexPolyFilled(
        cube_left.data(), static_cast<int>(cube_left.size()), stone);
    draw_list->AddConvexPolyFilled(
        cube_right.data(), static_cast<int>(cube_right.size()), slate);
    draw_list->AddConvexPolyFilled(
        cube_top.data(), static_cast<int>(cube_top.size()), ice);
    draw_list->AddPolyline(
        cube_top.data(), static_cast<int>(cube_top.size()), blue, ImDrawFlags_Closed, 0.8F);
}

} // namespace carto::desktop_branding
