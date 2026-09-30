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

// This header is test-only and must be included before the public project
// header. It exposes the model's private invariant helpers to this test
// translation unit without placing a test friend or a test macro in the
// installed production API.
#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include <carto/project/project.hpp>
#undef private
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif

#include <utility>

namespace carto::project::testing {

// Test-only access keeps project-model invariant tests able to construct
// documents without making the production mutation surface public again.
class ProjectDocumentAccess final {
public:
    explicit ProjectDocumentAccess(ProjectDocument& document) noexcept
        : document_(document) {}

    [[nodiscard]] core::Result<scene::ObjectId> create_object(
        std::string name,
        core::Transform transform = core::Transform::identity()) {
        return document_.create_object(std::move(name), transform);
    }

    [[nodiscard]] core::Result<void> insert_object(scene::SceneObject object) {
        return document_.insert_object(std::move(object));
    }

    [[nodiscard]] core::Result<void> remove_object(scene::ObjectId object) {
        return document_.remove_object(object);
    }

    [[nodiscard]] core::Result<std::uint64_t> add_mesh(geometry::EditableMesh mesh) {
        return document_.add_mesh(std::move(mesh));
    }

    [[nodiscard]] core::Result<void> insert_mesh(
        std::uint64_t mesh_asset,
        geometry::EditableMesh mesh) {
        return document_.insert_mesh(mesh_asset, std::move(mesh));
    }

    [[nodiscard]] core::Result<void> replace_mesh(
        std::uint64_t mesh_asset,
        geometry::EditableMesh mesh) {
        return document_.replace_mesh(mesh_asset, std::move(mesh));
    }

    [[nodiscard]] core::Result<core::Revision> replace_mesh_if_revision(
        std::uint64_t mesh_asset,
        core::Revision expected_revision,
        geometry::EditableMesh mesh) {
        return document_.replace_mesh_if_revision(
            mesh_asset, expected_revision, std::move(mesh));
    }

    [[nodiscard]] core::Result<void> remove_mesh(std::uint64_t mesh_asset) {
        return document_.remove_mesh(mesh_asset);
    }

    [[nodiscard]] core::Result<void> attach_mesh(
        scene::ObjectId object,
        std::uint64_t mesh_asset) {
        return document_.attach_mesh(object, mesh_asset);
    }

    [[nodiscard]] core::Result<void> set_object_transform(
        scene::ObjectId object,
        core::Transform transform) {
        return document_.set_object_transform(object, transform);
    }

    [[nodiscard]] core::Result<core::Revision> set_object_transform_if_revision(
        scene::ObjectId object,
        core::Revision expected_revision,
        core::Transform transform) {
        return document_.set_object_transform_if_revision(
            object, expected_revision, transform);
    }

    void swap(ProjectDocument& other) noexcept { document_.swap(other); }

private:
    ProjectDocument& document_;
};

[[nodiscard]] inline ProjectDocumentAccess access(ProjectDocument& document) noexcept {
    return ProjectDocumentAccess(document);
}

} // namespace carto::project::testing
