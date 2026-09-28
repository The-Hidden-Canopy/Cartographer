#include <carto/scene/scene.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

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

Diagnostic exhausted_revision() {
    return Diagnostic(ErrorCode::invalid_state, "scene revision space is exhausted");
}

constexpr std::size_t kMaxHierarchyDepth = 4096U;

} // namespace

core::Result<ObjectId> Scene::create_object(std::string name, core::Transform local_transform) {
    if (revision_.exhausted()) {
        return core::Result<ObjectId>::failure(exhausted_revision());
    }
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
    hierarchy_depths_[id] = 1U;
    bump_revision();
    return core::Result<ObjectId>::success(id);
}

core::Result<void> Scene::insert_object(SceneObject object) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    std::size_t depth = 1U;
    if (object.parent.has_value()) {
        const auto parent_depth = hierarchy_depths_.find(*object.parent);
        if (parent_depth == hierarchy_depths_.end() || parent_depth->second >= kMaxHierarchyDepth) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "scene hierarchy exceeds the maximum supported depth"));
        }
        depth = parent_depth->second + 1U;
    }

    const ObjectId inserted_id = object.id;
    const auto inserted_parent = object.parent;
    objects_.emplace(inserted_id, std::move(object));
    hierarchy_depths_[inserted_id] = depth;
    if (inserted_parent.has_value()) {
        children_[*inserted_parent].insert(inserted_id);
    }
    if (inserted_id.value < std::numeric_limits<std::uint64_t>::max()) {
        next_id_ = std::max(next_id_, inserted_id.value + 1U);
    }
    if (auto world = world_transform(inserted_id); !world) {
        if (inserted_parent.has_value()) {
            children_[*inserted_parent].erase(inserted_id);
            if (children_[*inserted_parent].empty()) {
                children_.erase(*inserted_parent);
            }
        }
        hierarchy_depths_.erase(inserted_id);
        objects_.erase(inserted_id);
        return core::Result<void>::failure(world.error());
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> Scene::rename_object(ObjectId id, std::string name) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    const auto previous_parent = child_iterator->second.parent;
    if (previous_parent == parent) {
        bump_revision();
        return core::Result<void>::success();
    }
    const auto child_depth_iterator = hierarchy_depths_.find(child);
    if (child_depth_iterator == hierarchy_depths_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "scene hierarchy depth index is incomplete"));
    }
    std::size_t new_depth = 1U;
    if (parent.has_value()) {
        const auto parent_depth = hierarchy_depths_.find(*parent);
        if (parent_depth == hierarchy_depths_.end() || parent_depth->second >= kMaxHierarchyDepth) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "scene hierarchy exceeds the maximum supported depth"));
        }
        new_depth = parent_depth->second + 1U;
    }
    const auto old_depth = child_depth_iterator->second;
    const auto depth_delta = static_cast<long long>(new_depth) - static_cast<long long>(old_depth);
    std::vector<ObjectId> subtree{child};
    for (std::size_t index = 0U; index < subtree.size(); ++index) {
        const auto descendants = children_.find(subtree[index]);
        if (descendants == children_.end()) {
            continue;
        }
        subtree.insert(subtree.end(), descendants->second.begin(), descendants->second.end());
    }
    for (const ObjectId object : subtree) {
        const auto current_depth = hierarchy_depths_.find(object);
        if (current_depth == hierarchy_depths_.end()) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::invalid_state, "scene hierarchy depth index is incomplete"));
        }
        const auto candidate_depth = static_cast<long long>(current_depth->second) + depth_delta;
        if (candidate_depth < 1 || candidate_depth > static_cast<long long>(kMaxHierarchyDepth)) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "scene hierarchy exceeds the maximum supported depth"));
        }
    }
    if (previous_parent.has_value()) {
        children_[*previous_parent].erase(child);
        if (children_[*previous_parent].empty()) {
            children_.erase(*previous_parent);
        }
    }
    if (parent.has_value()) {
        children_[*parent].insert(child);
    }
    child_iterator->second.parent = parent;
    for (const ObjectId object : subtree) {
        const auto current_depth = hierarchy_depths_.find(object);
        current_depth->second = static_cast<std::size_t>(
            static_cast<long long>(current_depth->second) + depth_delta);
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> Scene::set_local_transform(ObjectId id, core::Transform transform) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (iterator->second.parent.has_value()) {
        children_[*iterator->second.parent].erase(id);
        if (children_[*iterator->second.parent].empty()) {
            children_.erase(*iterator->second.parent);
        }
    }
    children_.erase(id);
    hierarchy_depths_.erase(id);
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
    std::vector<ObjectId> chain;
    ObjectId cursor = id;
    while (cursor) {
        if (chain.size() >= kMaxHierarchyDepth) {
            return core::Result<core::Transform>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "scene hierarchy exceeds the maximum supported depth"));
        }
        const auto iterator = objects_.find(cursor);
        if (iterator == objects_.end()) {
            return core::Result<core::Transform>::failure(
                missing("scene hierarchy contains a dangling parent"));
        }
        if (visiting[cursor]) {
            return core::Result<core::Transform>::failure(Diagnostic(
                ErrorCode::cycle_detected,
                "scene hierarchy cycle encountered during evaluation"));
        }
        visiting[cursor] = true;
        chain.push_back(cursor);
        if (!iterator->second.parent.has_value()) {
            break;
        }
        cursor = *iterator->second.parent;
    }

    core::Transform world = core::Transform::identity();
    for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator) {
        world = world.combine(objects_.at(*iterator).local_transform);
        if (!world.finite()) {
            return core::Result<core::Transform>::failure(Diagnostic(
                ErrorCode::validation_failed,
                "scene hierarchy produces a non-finite world transform"));
        }
    }
    for (const ObjectId object : chain) {
        visiting[object] = false;
    }
    return core::Result<core::Transform>::success(world);
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

core::Result<void> Scene::clear() {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    objects_.clear();
    children_.clear();
    hierarchy_depths_.clear();
    next_id_ = 1;
    bump_revision();
    return core::Result<void>::success();
}

} // namespace carto::scene
