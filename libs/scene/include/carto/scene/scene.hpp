#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace carto::scene {

struct ObjectId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const ObjectId&) const noexcept = default;
};

struct SceneObject {
    ObjectId id;
    std::string name;
    core::Transform local_transform = core::Transform::identity();
    std::optional<ObjectId> parent;
    std::optional<std::uint64_t> mesh_asset;
    bool visible = true;
    bool locked = false;
};

class Scene {
public:
    Scene() = default;

    [[nodiscard]] core::Result<ObjectId> create_object(
        std::string name,
        core::Transform local_transform = core::Transform::identity());

    [[nodiscard]] core::Result<void> insert_object(SceneObject object);
    [[nodiscard]] core::Result<void> rename_object(ObjectId id, std::string name);
    [[nodiscard]] core::Result<void> set_parent(ObjectId child, std::optional<ObjectId> parent);
    [[nodiscard]] core::Result<void> set_local_transform(ObjectId id, core::Transform transform);
    [[nodiscard]] core::Result<void> attach_mesh(ObjectId id, std::optional<std::uint64_t> mesh_asset);
    [[nodiscard]] core::Result<void> remove_object(ObjectId id);

    [[nodiscard]] const SceneObject* find(ObjectId id) const noexcept;
    [[nodiscard]] std::vector<SceneObject> objects_sorted() const;
    [[nodiscard]] core::Result<core::Transform> world_transform(ObjectId id) const;
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }
    [[nodiscard]] core::Result<void> clear();

    // Used by higher-level atomic document transactions. All scene state is
    // exchanged without changing either scene's revision.
    void swap(Scene& other) noexcept;

private:
    [[nodiscard]] bool would_create_cycle(ObjectId child, ObjectId parent) const;
    [[nodiscard]] core::Result<core::Transform> resolve_world(
        ObjectId id,
        std::map<ObjectId, bool>& visiting) const;
    void bump_revision() noexcept { revision_ = revision_.next(); }

    std::map<ObjectId, SceneObject> objects_;
    std::map<ObjectId, std::set<ObjectId>> children_;
    std::map<ObjectId, std::size_t> hierarchy_depths_;
    std::uint64_t next_id_ = 1;
    core::Revision revision_{};
};

} // namespace carto::scene
