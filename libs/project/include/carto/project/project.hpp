#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/file_lock.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/scientific/model.hpp>
#include <carto/scene/scene.hpp>
#include <carto/world/model.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::application {
class ApplicationSession;
}

namespace carto::editor {
class CreateMeshObjectCommand;
class DuplicateProjectMeshInstanceCommand;
class DeleteProjectSelectedFaceCommand;
class DissolveProjectSelectedEdgeCommand;
class ExtrudeProjectSelectedFaceCommand;
class InsetProjectSelectedFaceCommand;
class MergeProjectSelectedVerticesCommand;
class PokeProjectSelectedFaceCommand;
class SetProjectObjectTransformCommand;
class SetProjectSculptStackCommand;
class SetProjectSelectedVertexPositionCommand;
class SlideProjectSelectedVertexCommand;
class BevelProjectSelectedEdgeCommand;
class BevelProjectSelectedVertexCommand;
class SplitProjectSelectedEdgeCommand;
class TriToQuadProjectSelectedFacesCommand;
}

namespace carto::project {

class ProjectTransaction;

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

// A durable lineage record for a project-bound topology edit. The receipt is
// historical metadata: it may describe an earlier mesh revision after
// undo/redo has advanced the current revision, but it never substitutes for
// the authoritative mesh snapshot.
struct TopologyReceiptRecord {
    std::uint64_t mesh_asset = 0;
    geometry::TopologyEditReceipt receipt;

    [[nodiscard]] core::Result<void> validate() const;
};

// Opaque revision-bound SculptStack source bytes. The project layer preserves
// the serialized stack without depending on the sculpt compiler.
struct SerializedSculptStack {
    std::string bytes;
    assets::Sha256Digest digest;

    [[nodiscard]] bool operator==(const SerializedSculptStack&) const noexcept = default;
};

// Opaque revision-bound GarmentAsset source bytes. The project layer stores
// the canonical source record without depending on cloth compilation.
struct SerializedGarmentAsset {
    std::string bytes;
    assets::Sha256Digest digest;

    [[nodiscard]] bool operator==(const SerializedGarmentAsset&) const noexcept = default;
};

// Opaque derived cloth-rest-pose receipt bytes attached to the mesh they
// certify. The project layer preserves provenance without depending on the
// cloth compiler.
struct SerializedClothRestPoseReceipt {
    std::string bytes;
    assets::Sha256Digest digest;

    [[nodiscard]] bool operator==(const SerializedClothRestPoseReceipt&) const noexcept = default;
};

class ProjectDocument {
public:
    static constexpr std::uint32_t kSchemaVersion = 13;
    static constexpr std::uint32_t kMinimumReadableSchemaVersion = 1;
    static constexpr std::string_view kMagic = "CARTOGRAPHER_PROJECT";
    static constexpr std::string_view kAuthoringFormat = "cartographer.authoring";
    static constexpr std::string_view kAuthoringUnits = "meters";
    static constexpr std::string_view kAuthoringCoordinateSystem = "right_handed_y_up";

    ProjectDocument();
    ProjectDocument(const ProjectDocument&) = default;
    ProjectDocument(ProjectDocument&&) = default;

    [[nodiscard]] static core::Result<ProjectDocument> create(std::string name);
    [[nodiscard]] static core::Result<ProjectDocument> load(const std::filesystem::path& path);
    [[nodiscard]] static core::Result<ProjectDocument> deserialize(std::string_view text);

    [[nodiscard]] core::Result<SaveReceipt> save_atomic(const std::filesystem::path& path) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& source_namespace() const noexcept {
        return source_namespace_;
    }
    [[nodiscard]] const scene::Scene& scene() const noexcept { return scene_; }
    [[nodiscard]] const std::map<std::uint64_t, geometry::EditableMesh>& meshes() const noexcept {
        return meshes_;
    }
    [[nodiscard]] const std::optional<assets::Sha256Digest>& evaluation_graph_digest() const noexcept {
        return evaluation_graph_digest_;
    }
    [[nodiscard]] const std::vector<TopologyReceiptRecord>& topology_receipts() const noexcept {
        return topology_receipts_;
    }
    [[nodiscard]] const scientific::ScientificModel& scientific_model() const noexcept {
        return scientific_model_;
    }
    [[nodiscard]] const world::WorldModel& world_model() const noexcept {
        return world_model_;
    }
    [[nodiscard]] const MaterialCatalog& material_catalog() const noexcept {
        return material_catalog_;
    }
    // Opaque serialized semantic sidecar bytes and the digest that binds
    // them. ProjectDocument stores the bytes verbatim and never parses them;
    // semantic meaning stays behind the carto_semantic boundary.
    [[nodiscard]] const std::optional<std::string>& semantic_sidecar_bytes() const noexcept {
        return semantic_sidecar_bytes_;
    }
    [[nodiscard]] const std::optional<assets::Sha256Digest>& semantic_sidecar_digest() const noexcept {
        return semantic_sidecar_digest_;
    }
    [[nodiscard]] const std::map<std::uint64_t, SerializedSculptStack>& sculpt_stacks() const noexcept {
        return sculpt_stacks_;
    }
    [[nodiscard]] const std::map<std::uint64_t, SerializedGarmentAsset>& garment_assets() const noexcept {
        return garment_assets_;
    }
    [[nodiscard]] const std::map<std::uint64_t, SerializedClothRestPoseReceipt>&
    cloth_rest_pose_receipts() const noexcept {
        return cloth_rest_pose_receipts_;
    }
    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint32_t schema_version() const noexcept { return kSchemaVersion; }

private:
    friend class ::carto::application::ApplicationSession;
    friend class ::carto::editor::CreateMeshObjectCommand;
    friend class ::carto::editor::DuplicateProjectMeshInstanceCommand;
    friend class ::carto::editor::DeleteProjectSelectedFaceCommand;
    friend class ::carto::editor::DissolveProjectSelectedEdgeCommand;
    friend class ::carto::editor::ExtrudeProjectSelectedFaceCommand;
    friend class ::carto::editor::InsetProjectSelectedFaceCommand;
    friend class ::carto::editor::MergeProjectSelectedVerticesCommand;
    friend class ::carto::editor::PokeProjectSelectedFaceCommand;
    friend class ::carto::editor::SetProjectObjectTransformCommand;
    friend class ::carto::editor::SetProjectSculptStackCommand;
    friend class ::carto::editor::SetProjectSelectedVertexPositionCommand;
    friend class ::carto::editor::SlideProjectSelectedVertexCommand;
    friend class ::carto::editor::BevelProjectSelectedEdgeCommand;
    friend class ::carto::editor::BevelProjectSelectedVertexCommand;
    friend class ::carto::editor::SplitProjectSelectedEdgeCommand;
    friend class ::carto::editor::TriToQuadProjectSelectedFacesCommand;
    friend class ::carto::project::ProjectTransaction;

    ProjectDocument& operator=(const ProjectDocument&) = default;
    ProjectDocument& operator=(ProjectDocument&&) = default;

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
        geometry::EditableMesh mesh,
        std::optional<geometry::TopologyEditReceipt> receipt = std::nullopt);
    [[nodiscard]] core::Result<void> remove_mesh(std::uint64_t mesh_asset);
    [[nodiscard]] core::Result<void> attach_mesh(
        scene::ObjectId object,
        std::uint64_t mesh_asset);
    // Binds a content-addressed evaluation graph snapshot to the same
    // revisioned document state that is journaled and checkpointed.
    [[nodiscard]] core::Result<void> set_evaluation_graph_digest(
        std::optional<assets::Sha256Digest> digest);
    [[nodiscard]] core::Result<void> set_scientific_model(
        scientific::ScientificModel model);
    [[nodiscard]] core::Result<void> set_world_model(world::WorldModel model);
    [[nodiscard]] core::Result<void> set_material_catalog(MaterialCatalog catalog);
    // Binds one opaque serialized semantic sidecar and its digest. The digest
    // must equal the SHA-256 of the exact bytes; mismatches fail closed.
    [[nodiscard]] core::Result<void> set_semantic_sidecar(
        std::string serialized_sidecar,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> set_sculpt_stack(
        std::uint64_t mesh_asset,
        std::string serialized_stack,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> erase_sculpt_stack_if_revision(
        std::uint64_t mesh_asset,
        core::Revision expected_revision);
    [[nodiscard]] core::Result<void> set_garment_asset(
        std::uint64_t mesh_asset,
        std::string serialized_asset,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> set_cloth_rest_pose_receipt(
        std::uint64_t mesh_asset,
        std::string serialized_receipt,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> set_object_transform(
        scene::ObjectId object,
        core::Transform transform);
    [[nodiscard]] core::Result<core::Revision> set_object_transform_if_revision(
        scene::ObjectId object,
        core::Revision expected_revision,
        core::Transform transform);

    // Exchanges the complete document state without advancing either
    // revision. ProjectTransaction uses this only after its journal append is
    // durable, making the in-memory publish step non-throwing.
    void swap(ProjectDocument& other) noexcept;

    [[nodiscard]] core::Result<void> set_name(std::string name);
    [[nodiscard]] core::Result<SaveReceipt> save_atomic_unlocked(
        const std::filesystem::path& path) const;
    void bump_revision() noexcept { revision_ = revision_.next(); }

    std::string name_ = "Untitled";
    std::string source_namespace_;
    scene::Scene scene_;
    std::map<std::uint64_t, geometry::EditableMesh> meshes_;
    std::vector<TopologyReceiptRecord> topology_receipts_;
    scientific::ScientificModel scientific_model_;
    world::WorldModel world_model_;
    MaterialCatalog material_catalog_;
    std::optional<std::string> semantic_sidecar_bytes_;
    std::optional<assets::Sha256Digest> semantic_sidecar_digest_;
    std::map<std::uint64_t, SerializedSculptStack> sculpt_stacks_;
    std::map<std::uint64_t, SerializedGarmentAsset> garment_assets_;
    std::map<std::uint64_t, SerializedClothRestPoseReceipt> cloth_rest_pose_receipts_;
    std::optional<assets::Sha256Digest> evaluation_graph_digest_;
    std::uint64_t next_mesh_id_ = 1;
    core::Revision revision_{};
};

} // namespace carto::project
