#include <carto/editor/selection_frame.hpp>
#include <carto/geometry/mesh.hpp>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

using namespace carto;
namespace {
void require(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
void selection_frames_only_current_visible_geometry() {
    geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0, 0, 0});
    const auto b = mesh.add_vertex({1, 0, 0});
    const auto c = mesh.add_vertex({0, 0, 2});
    require(a && b && c && mesh.add_face({a.value(), b.value(), c.value()}), "mesh fixture");
    auto compiled = mesh.compile();
    require(static_cast<bool>(compiled), "compile fixture");
    auto geometry = std::make_shared<const geometry::CompiledMesh>(compiled.value());
    std::vector<scene::SceneObject> objects{
        {{1}, "room", {}, std::nullopt, std::nullopt, true, false},
        {{2}, "mattress", {}, scene::ObjectId{1}, 10U, true, false},
        {{3}, "other", {}, std::nullopt, 10U, true, false},
        {{4}, "unresolved link", {}, scene::ObjectId{1}, std::nullopt, true, false}};
    core::Transform moved;
    moved.translation = {25, 0, 30};
    moved.scale = {2, 1, 3};
    core::Transform other;
    other.translation = {1000, 0, 1000};
    std::vector<render::RenderInstance> instances{
        {{2}, geometry, moved, geometry->source_revision, 10U},
        {{3}, geometry, other, geometry->source_revision, 10U}};
    auto single = editor::selected_render_bounds(objects, instances, {{2}});
    require(single && single.value().minimum.x == 25 && single.value().maximum.x == 27 &&
        single.value().minimum.z == 30 && single.value().maximum.z == 36,
        "frame uses selected transformed geometry, not distant scene objects");
    auto group = editor::selected_render_bounds(objects, instances, {{1}});
    require(group && group.value().minimum.x == single.value().minimum.x &&
        group.value().maximum.z == single.value().maximum.z, "group selection includes rendered descendants");
    require(!editor::selected_render_bounds(objects, instances, {{4}}), "unresolved linked part cannot frame unrelated geometry");
    require(!editor::selected_render_bounds(objects, instances, {{2}, {4}}),
        "partial multi-selection cannot hide an unresolved linked part");
    require(!editor::selected_render_bounds(objects, instances, {{999}}), "unknown selection refuses");
    require(!editor::selected_render_bounds(objects, instances, {}), "empty selection refuses");
    auto stale = instances;
    stale.front().source_revision = core::Revision{999};
    require(!editor::selected_render_bounds(objects, stale, {{2}}), "stale compiled mesh refuses");
    stale = instances;
    stale.front().world_transform.translation.x = std::numeric_limits<double>::infinity();
    require(!editor::selected_render_bounds(objects, stale, {{2}}), "non-finite transform refuses");
}
}
int main() { selection_frames_only_current_visible_geometry(); }
