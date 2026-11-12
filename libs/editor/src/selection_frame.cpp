#include <carto/editor/selection_frame.hpp>

#include <map>
#include <set>

namespace carto::editor {
namespace {
core::Diagnostic invalid(const char* message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, message);
}
}
core::Result<core::Bounds3d> selected_render_bounds(
    const std::vector<scene::SceneObject>& objects,
    const std::vector<render::RenderInstance>& instances,
    const std::vector<scene::ObjectId>& selected_objects) {
    using Result = core::Result<core::Bounds3d>;
    if (selected_objects.empty()) return Result::failure(invalid("select an object to frame"));
    std::map<scene::ObjectId, const scene::SceneObject*> by_id;
    for (const auto& object : objects)
        if (!object.id || !by_id.emplace(object.id, &object).second)
            return Result::failure(invalid("scene snapshot has invalid or duplicate object identities"));
    std::set<scene::ObjectId> selected;
    for (const auto id : selected_objects) {
        if (!id || !by_id.contains(id)) return Result::failure(invalid("selected object is absent from the scene snapshot"));
        selected.insert(id);
    }
    core::Bounds3d bounds;
    std::set<scene::ObjectId> seen_instances;
    std::set<scene::ObjectId> covered_selection;
    for (const auto& instance : instances) {
        const auto object = by_id.find(instance.object);
        if (object == by_id.end() || !seen_instances.insert(instance.object).second)
            return Result::failure(invalid("viewport instance is missing or duplicated in the scene snapshot"));
        auto current = object->second;
        std::set<scene::ObjectId> visited;
        bool included = false;
        while (current) {
            if (!visited.insert(current->id).second)
                return Result::failure(invalid("scene snapshot hierarchy contains a cycle"));
            if (selected.contains(current->id)) {
                included = true;
                covered_selection.insert(current->id);
            }
            if (!current->parent) break;
            const auto parent = by_id.find(*current->parent);
            if (parent == by_id.end()) return Result::failure(invalid("scene snapshot hierarchy is incomplete"));
            current = parent->second;
        }
        if (!included) continue;
        if (!instance.mesh || !instance.mesh->valid() || !instance.world_transform.finite() ||
            instance.source_revision != instance.mesh->source_revision ||
            instance.mesh_asset != object->second->mesh_asset)
            return Result::failure(invalid("selected viewport geometry is unavailable or invalid"));
        for (const auto point : instance.mesh->positions) {
            const auto world = instance.world_transform.translation +
                instance.world_transform.rotation.rotate(core::componentwise_multiply(instance.world_transform.scale, point));
            if (!world.finite()) return Result::failure(invalid("selected viewport geometry overflows world coordinates"));
            bounds.include(world);
        }
    }
    if (covered_selection.size() != selected.size() || !bounds.valid())
        return Result::failure(invalid("one or more selected objects have no visible viewport geometry"));
    return Result::success(bounds);
}
} // namespace carto::editor
