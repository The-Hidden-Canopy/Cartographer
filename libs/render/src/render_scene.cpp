#include <carto/render/render_scene.hpp>

#include <utility>

namespace carto::render {

namespace {

core::Vec3d transform_point(const core::Transform& transform, core::Vec3d point) {
    return transform.translation +
        transform.rotation.rotate(core::componentwise_multiply(transform.scale, point));
}

} // namespace

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
    for (const auto point : instance.mesh->positions) {
        if (!transform_point(instance.world_transform, point).finite()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "render instance produces a non-finite world position"));
        }
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

std::vector<RenderInstance> RenderScene::instances() const {
    std::vector<RenderInstance> result;
    result.reserve(instances_.size());
    for (const auto& [object, instance] : instances_) {
        static_cast<void>(object);
        result.push_back(instance);
    }
    return result;
}

} // namespace carto::render
