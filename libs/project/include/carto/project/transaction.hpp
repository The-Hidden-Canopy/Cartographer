#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace carto::project {

struct TransactionReceipt {
    core::Revision revision_before;
    core::Revision revision_after;
    std::string actor;
    std::string operation;
    assets::Sha256Digest document_digest;
};

// Stages document mutations and publishes them only after a matching journal
// append succeeds. ProjectDocument's state-mutating primitives are private to
// this boundary, the application session, and its trusted editor commands.
class ProjectTransaction {
public:
    [[nodiscard]] static core::Result<ProjectTransaction> begin(
        ProjectDocument& document,
        journal::Journal& journal,
        std::string actor);

    ProjectTransaction(ProjectTransaction&&) noexcept = default;
    ProjectTransaction& operator=(ProjectTransaction&&) = delete;
    ProjectTransaction(const ProjectTransaction&) = delete;
    ProjectTransaction& operator=(const ProjectTransaction&) = delete;

    [[nodiscard]] core::Result<scene::ObjectId> create_object(
        std::string name,
        core::Transform transform = core::Transform::identity());
    [[nodiscard]] core::Result<void> insert_object(scene::SceneObject object);
    [[nodiscard]] core::Result<void> remove_object(scene::ObjectId object);
    [[nodiscard]] core::Result<std::uint64_t> add_mesh(geometry::EditableMesh mesh);
    [[nodiscard]] core::Result<void> insert_mesh(
        std::uint64_t mesh_asset,
        geometry::EditableMesh mesh);
    [[nodiscard]] core::Result<void> replace_mesh(
        std::uint64_t mesh_asset,
        geometry::EditableMesh mesh,
        std::optional<geometry::TopologyEditReceipt> receipt = std::nullopt);
    [[nodiscard]] core::Result<void> remove_mesh(std::uint64_t mesh_asset);
    [[nodiscard]] core::Result<void> attach_mesh(
        scene::ObjectId object,
        std::uint64_t mesh_asset);
    [[nodiscard]] core::Result<void> set_evaluation_graph_digest(
        std::optional<assets::Sha256Digest> digest);
    [[nodiscard]] core::Result<void> set_scientific_model(
        scientific::ScientificModel model);
    [[nodiscard]] core::Result<void> set_world_model(world::WorldModel model);
    [[nodiscard]] core::Result<void> set_material_catalog(MaterialCatalog catalog);
    [[nodiscard]] core::Result<void> set_object_transform(
        scene::ObjectId object,
        core::Transform transform);

    [[nodiscard]] core::Result<TransactionReceipt> commit(
        std::string operation = "project.transaction");
    [[nodiscard]] core::Result<void> rollback() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] core::Revision revision_before() const noexcept { return expected_revision_; }
    [[nodiscard]] core::Revision staged_revision() const noexcept { return staged_.revision(); }
    [[nodiscard]] const ProjectDocument& staged_document() const noexcept { return staged_; }

private:
    ProjectTransaction(
        ProjectDocument& document,
        ProjectDocument staged,
        journal::Journal& journal,
        std::string actor)
        : document_(document),
          staged_(std::move(staged)),
          expected_revision_(document.revision()),
          journal_(journal),
          actor_(std::move(actor)) {}

    [[nodiscard]] core::Result<void> ensure_active() const;
    [[nodiscard]] static core::Result<void> validate_actor(std::string_view actor);
    [[nodiscard]] static core::Result<void> validate_operation(std::string_view operation);
    [[nodiscard]] std::string journal_payload(
        std::string_view operation,
        std::string_view serialized_document) const;

    ProjectDocument& document_;
    ProjectDocument staged_;
    core::Revision expected_revision_;
    journal::Journal& journal_;
    std::string actor_;
    bool active_ = true;
};

} // namespace carto::project
