#include <carto/scene/scene.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace carto::scene {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic missing(std::string message) {
    return Diagnostic(ErrorCode::not_found, std::move(message));
}

} // namespace

core::Result<ObjectId> Scene::create_object(std::string name, core::Transform local_transform) {
    if (name.empty()) {
        return core::Result<ObjectId>::failure(invalid("scene object name must not be empty"));
    }
    if (!local_transform.finite()) {
        return core::Result<ObjectId>::failure(
            invalid("scene object transform must contain finite values"));
    }
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<ObjectId>::failure(
            Diagnostic(ErrorCode::invalid_state, "scene object id space is exhausted"));
    }

    const ObjectId id{next_id_++};
    objects_.emplace(
        id,
        SceneObject{id, std::move(name), local_transform, std::nullopt, std::nullopt, true, false});
    bump_revision();
    return core::Result<ObjectId>::success(id);
}

core::Result<void> Scene::insert_object(SceneObject object) {
    if (!object.id || object.name.empty()) {
        return core::Result<void>::failure(
            invalid("inserted scene object requires a non-zero id and non-empty name"));
    }
    if (!object.local_transform.finite()) {
        return core::Result<void>::failure(
            invalid("inserted scene object transform must contain finite values"));
    }
    if (objects_.contains(object.id)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "duplicate scene object id"));
    }
    if (object.parent.has_value() && !objects_.contains(*object.parent)) {
        return core::Result<void>::failure(missing("inserted object parent does not exist"));
    }
    if (object.parent.has_value() && *object.parent == object.id) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::cycle_detected, "scene object cannot parent itself"));
    }

    const ObjectId inserted_id = object.id;
    objects_.emplace(inserted_id, std::move(object));
    if (inserted_id.value < std::numeric_limits<std::uint64_t>::max()) {
        next_id_ = std::max(next_id_, inserted_id.value + 1U);
    }
    if (auto validation = validate(); !validation) {
        objects_.erase(inserted_id);
        return validation;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> Scene::rename_object(ObjectId id, std::string name) {
    if (name.empty()) {
        return core::Result<void>::failure(invalid("scene object name must not be empty"));
    }
    auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return core::Result<void>::failure(missing("cannot rename missing scene object"));
    }
    iterator->second.name = std::move(name);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> Scene::set_parent(ObjectId child, std::optional<ObjectId> parent) {
    auto child_iterator = objects_.find(child);
    if (child_iterator == objects_.end()) {
        return core::Result<void>::failure(missing("cannot parent missing scene object"));
    }
    if (parent.has_value() && !objects_.contains(*parent)) {
        return core::Result<void>::failure(missing("scene parent does not exist"));
    }
    if (parent.has_value() && would_create_cycle(child, *parent)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::cycle_detected, "scene hierarchy cycle rejected"));
    }
    child_iterator->second.parent = parent;
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> Scene::set_local_transform(ObjectId id, core::Transform transform) {
    if (!transform.finite()) {
        return core::Result<void>::failure(
            invalid("scene object transform must contain finite values"));
    }
    auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return core::Result<void>::failure(missing("cannot transform missing scene object"));
    }
    iterator->second.local_transform = transform;
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> Scene::attach_mesh(ObjectId id, std::optional<std::uint64_t> mesh_asset) {
    auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return core::Result<void>::failure(missing("cannot attach mesh to missing scene object"));
    }
    if (mesh_asset.has_value() && *mesh_asset == 0) {
        return core::Result<void>::failure(invalid("mesh asset id must be non-zero"));
    }
    iterator->second.mesh_asset = mesh_asset;
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> Scene::remove_object(ObjectId id) {
    auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return core::Result<void>::failure(missing("cannot remove missing scene object"));
    }
    const bool has_children = std::any_of(
        objects_.begin(),
        objects_.end(),
        [id](const auto& item) { return item.second.parent.has_value() && *item.second.parent == id; });
    if (has_children) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "removing a parent with children requires an explicit child policy"));
    }
    objects_.erase(iterator);
    bump_revision();
    return core::Result<void>::success();
}

const SceneObject* Scene::find(ObjectId id) const noexcept {
    const auto iterator = objects_.find(id);
    return iterator == objects_.end() ? nullptr : &iterator->second;
}

std::vector<SceneObject> Scene::objects_sorted() const {
    std::vector<SceneObject> result;
    result.reserve(objects_.size());
    for (const auto& [id, object] : objects_) {
        static_cast<void>(id);
        result.push_back(object);
    }
    return result;
}

core::Result<core::Transform> Scene::world_transform(ObjectId id) const {
    if (!objects_.contains(id)) {
        return core::Result<core::Transform>::failure(missing("cannot resolve missing scene object"));
    }
    std::map<ObjectId, bool> visiting;
    return resolve_world(id, visiting);
}

core::Result<core::Transform> Scene::resolve_world(
    ObjectId id,
    std::map<ObjectId, bool>& visiting) const {
    const auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return core::Result<core::Transform>::failure(missing("scene hierarchy contains a dangling parent"));
    }
    if (visiting[id]) {
        return core::Result<core::Transform>::failure(
            Diagnostic(ErrorCode::cycle_detected, "scene hierarchy cycle encountered during evaluation"));
    }
    visiting[id] = true;
    const SceneObject& object = iterator->second;
    if (!object.parent.has_value()) {
        visiting[id] = false;
        return core::Result<core::Transform>::success(object.local_transform);
    }
    auto parent_result = resolve_world(*object.parent, visiting);
    if (!parent_result) {
        return parent_result;
    }
    visiting[id] = false;
    return core::Result<core::Transform>::success(parent_result.value().combine(object.local_transform));
}

core::Result<void> Scene::validate() const {
    for (const auto& [id, object] : objects_) {
        if (!id || object.id != id) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::validation_failed, "scene object map key does not match object id"));
        }
        if (object.name.empty() || !object.local_transform.finite()) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::validation_failed, "scene object contains invalid authoring data"));
        }
        if (object.parent.has_value() && !objects_.contains(*object.parent)) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::validation_failed, "scene object contains a dangling parent"));
        }
        if (object.parent.has_value() && *object.parent == id) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::cycle_detected, "scene object self-parenting is invalid"));
        }
    }

    for (const auto& [id, object] : objects_) {
        static_cast<void>(object);
        std::map<ObjectId, bool> visiting;
        if (auto result = resolve_world(id, visiting); !result) {
            return core::Result<void>::failure(result.error());
        }
    }
    return core::Result<void>::success();
}

bool Scene::would_create_cycle(ObjectId child, ObjectId parent) const {
    ObjectId cursor = parent;
    while (cursor) {
        if (cursor == child) {
            return true;
        }
        const auto iterator = objects_.find(cursor);
        if (iterator == objects_.end() || !iterator->second.parent.has_value()) {
            return false;
        }
        cursor = *iterator->second.parent;
    }
    return false;
}

void Scene::clear() noexcept {
    objects_.clear();
    next_id_ = 1;
    bump_revision();
}

} // namespace carto::scene
