#include <carto/render/render_scene.hpp>

#include <utility>

namespace carto::render {

core::Result<void> RenderScene::upsert(RenderInstance instance) {
    if (!instance.object || !instance.mesh) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_argument, "render instance requires an object and compiled mesh"));
    }
    if (!instance.world_transform.finite()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_argument, "render instance transform must be finite"));
    }
    if (!instance.mesh->valid()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed, "render instance mesh is invalid"));
    }
    if (instance.source_revision != instance.mesh->source_revision) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::stale_data, "render instance source revision is stale"));
    }
    instances_[instance.object] = std::move(instance);
    return core::Result<void>::success();
}

core::Result<void> RenderScene::remove(scene::ObjectId object) {
    if (instances_.erase(object) == 0U) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "render instance does not exist"));
    }
    return core::Result<void>::success();
}

const RenderInstance* RenderScene::find(scene::ObjectId object) const noexcept {
    const auto iterator = instances_.find(object);
    return iterator == instances_.end() ? nullptr : &iterator->second;
}

} // namespace carto::render
