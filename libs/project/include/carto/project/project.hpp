#pragma once

#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/file_lock.hpp>
#include <carto/scene/scene.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace carto::application {
class ApplicationSession;
}

namespace carto::project {

struct AssetReference {
    std::string relative_path;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SaveReceipt {
    std::filesystem::path path;
    std::uint64_t bytes_written = 0;
    std::uint32_t schema_version = 0;
    core::Revision source_revision;
};

class ProjectDocument {
public:
    static constexpr std::uint32_t kSchemaVersion = 1;
    static constexpr std::string_view kMagic = "CARTOGRAPHER_PROJECT";

    ProjectDocument() = default;

    [[nodiscard]] static core::Result<ProjectDocument> create(std::string name);
    [[nodiscard]] static core::Result<ProjectDocument> load(const std::filesystem::path& path);
    [[nodiscard]] static core::Result<ProjectDocument> deserialize(std::string_view text);

    [[nodiscard]] core::Result<scene::ObjectId> create_object(
        std::string name,
        core::Transform transform = core::Transform::identity());
    [[nodiscard]] core::Result<void> insert_object(scene::SceneObject object);
    [[nodiscard]] core::Result<void> remove_object(scene::ObjectId object);
    [[nodiscard]] core::Result<std::uint64_t> add_mesh(geometry::EditableMesh mesh);
    [[nodiscard]] core::Result<void> insert_mesh(
        std::uint64_t mesh_asset,
        geometry::EditableMesh mesh);
    // Replaces a validated asset and advances its source revision.
    [[nodiscard]] core::Result<void> replace_mesh(
        std::uint64_t mesh_asset,
        geometry::EditableMesh mesh);
    // Replaces only when the caller still owns the expected source revision.
    [[nodiscard]] core::Result<core::Revision> replace_mesh_if_revision(
        std::uint64_t mesh_asset,
        core::Revision expected_revision,
        geometry::EditableMesh mesh);
    [[nodiscard]] core::Result<void> remove_mesh(std::uint64_t mesh_asset);
    [[nodiscard]] core::Result<void> attach_mesh(
        scene::ObjectId object,
        std::uint64_t mesh_asset);
    [[nodiscard]] core::Result<void> set_object_transform(
        scene::ObjectId object,
        core::Transform transform);
    [[nodiscard]] core::Result<core::Revision> set_object_transform_if_revision(
        scene::ObjectId object,
        core::Revision expected_revision,
        core::Transform transform);

    [[nodiscard]] core::Result<SaveReceipt> save_atomic(const std::filesystem::path& path) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<void> validate() const;

    // Exchanges the complete document state without advancing either
    // revision. ProjectTransaction uses this after its journal append is
    // durable, making the in-memory publish step non-throwing.
    void swap(ProjectDocument& other) noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const scene::Scene& scene() const noexcept { return scene_; }
    [[nodiscard]] const std::map<std::uint64_t, geometry::EditableMesh>& meshes() const noexcept {
        return meshes_;
    }
    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint32_t schema_version() const noexcept { return kSchemaVersion; }

private:
    friend class ::carto::application::ApplicationSession;

    [[nodiscard]] core::Result<void> set_name(std::string name);
    [[nodiscard]] core::Result<SaveReceipt> save_atomic_unlocked(
        const std::filesystem::path& path) const;
    void bump_revision() noexcept { revision_ = revision_.next(); }

    std::string name_ = "Untitled";
    scene::Scene scene_;
    std::map<std::uint64_t, geometry::EditableMesh> meshes_;
    std::uint64_t next_mesh_id_ = 1;
    core::Revision revision_{};
};

} // namespace carto::project
