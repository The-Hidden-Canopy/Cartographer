#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/assets/source_registry.hpp>
#include <carto/core/revision.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/engineering/authoring.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/geometry/curve_path.hpp>
#include <carto/geometry/sweep.hpp>
#include <carto/project/file_lock.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/project/presentation_catalog.hpp>
#include <carto/project/road_source.hpp>
#include <carto/spatial_field/scalar_field.hpp>
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
class SetProjectMeshCommand;
class SlideProjectSelectedVertexCommand;
class BevelProjectSelectedEdgeCommand;
class BevelProjectSelectedVertexCommand;
class SplitProjectSelectedEdgeCommand;
class TriToQuadProjectSelectedFacesCommand;
class SetProjectEngineeringAuthoringCommand;
class SetProjectConditionAuthoringCommand;
class SetProjectMaterialCatalogCommand;
class SetProjectSpatialFieldCommand;
class SetProjectRoadAuthoringCommand;
class SetProjectSweepSourceCommand;
class ApplySourceReimportPlanCommand;
class ImportSourceMeshCommand;
class ReplaceProjectDocumentCommand;
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

// Opaque revision-bound DV012 engineering authoring bytes. The project layer
// stores the exact validated authoring document and digest; engineering
// semantics remain behind the carto_engineering boundary.
struct SerializedEngineeringAuthoring {
    std::string bytes;
    assets::Sha256Digest digest;

    [[nodiscard]] bool operator==(const SerializedEngineeringAuthoring&) const noexcept = default;
};

// Opaque revision-bound CONDITION source-authoring bytes. The project layer
// preserves the validated public contract and digest; CONDITION runtime
// behavior remains outside this checkout.
struct SerializedConditionAuthoring {
    std::string bytes;
    assets::Sha256Digest digest;

    [[nodiscard]] bool operator==(const SerializedConditionAuthoring&) const noexcept = default;
};

class ProjectDocument {
public:
    static constexpr std::uint32_t kSchemaVersion = 22;
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
    [[nodiscard]] const std::map<std::string, geometry::CurvePath>& curve_paths() const noexcept {
        return curve_paths_;
    }
    [[nodiscard]] const std::map<std::string, RoadSource>& road_sources() const noexcept {
        return road_sources_;
    }
    [[nodiscard]] const std::map<std::string, geometry::SweepSource>&
    sweep_sources() const noexcept { return sweep_sources_; }
    [[nodiscard]] const std::map<std::string, spatial_field::ScalarField2D>&
    spatial_fields() const noexcept { return spatial_fields_; }
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
    [[nodiscard]] const PresentationCatalog& presentation_catalog() const noexcept {
        return presentation_catalog_;
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
    [[nodiscard]] const std::optional<SerializedEngineeringAuthoring>&
    engineering_authoring() const noexcept {
        return engineering_authoring_;
    }
    [[nodiscard]] const std::optional<SerializedConditionAuthoring>&
    condition_authoring() const noexcept {
        return condition_authoring_;
    }
    [[nodiscard]] const assets::ProjectAssetRegistry& source_asset_registry() const noexcept {
        return source_asset_registry_;
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
    friend class ::carto::editor::SetProjectMeshCommand;
    friend class ::carto::editor::SlideProjectSelectedVertexCommand;
    friend class ::carto::editor::BevelProjectSelectedEdgeCommand;
    friend class ::carto::editor::BevelProjectSelectedVertexCommand;
    friend class ::carto::editor::SplitProjectSelectedEdgeCommand;
    friend class ::carto::editor::TriToQuadProjectSelectedFacesCommand;
    friend class ::carto::editor::SetProjectEngineeringAuthoringCommand;
    friend class ::carto::editor::SetProjectConditionAuthoringCommand;
    friend class ::carto::editor::SetProjectMaterialCatalogCommand;
    friend class ::carto::editor::SetProjectSpatialFieldCommand;
    friend class ::carto::editor::SetProjectRoadAuthoringCommand;
    friend class ::carto::editor::SetProjectSweepSourceCommand;
    friend class ::carto::editor::ApplySourceReimportPlanCommand;
    friend class ::carto::editor::ImportSourceMeshCommand;
    friend class ::carto::editor::ReplaceProjectDocumentCommand;
    friend class ::carto::project::ProjectTransaction;

    ProjectDocument& operator=(const ProjectDocument&) = default;
    ProjectDocument& operator=(ProjectDocument&&) = default;

    [[nodiscard]] static core::Result<ProjectDocument> merge_disjoint_scene_mesh_changes(
        const ProjectDocument& base,
        const ProjectDocument& current,
        const ProjectDocument& speculative,
        const std::vector<scene::ObjectId>& read_objects,
        const std::vector<std::uint64_t>& read_mesh_assets,
        const std::vector<scene::ObjectId>& write_objects,
        const std::vector<std::uint64_t>& write_mesh_assets,
        const std::vector<MaterialRegionKey>& read_material_regions = {},
        const std::vector<MaterialRegionKey>& write_material_regions = {});
    [[nodiscard]] static core::Result<ProjectDocument> select_agent_rebase_step_changes(
        const ProjectDocument& base,
        const ProjectDocument& speculative,
        const std::vector<scene::ObjectId>& write_objects,
        const std::vector<std::uint64_t>& write_mesh_assets,
        std::uint64_t revision_delta);

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
    [[nodiscard]] core::Result<void> set_presentation_catalog(PresentationCatalog catalog);
    [[nodiscard]] core::Result<void> set_curve_path(geometry::CurvePath path);
    [[nodiscard]] core::Result<void> erase_curve_path_if_revision(
        std::string_view identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_road_source(RoadSource road);
    [[nodiscard]] core::Result<void> erase_road_source_if_revision(
        std::string_view identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_sweep_source(geometry::SweepSource source);
    [[nodiscard]] core::Result<void> erase_sweep_source_if_revision(
        std::string_view identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_spatial_field(
        spatial_field::ScalarField2D field);
    [[nodiscard]] core::Result<void> erase_spatial_field_if_revision(
        std::string_view identity,
        core::Revision expected_source_revision);
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
    [[nodiscard]] core::Result<void> set_engineering_authoring(
        std::string serialized_authoring,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> erase_engineering_authoring_if_revision(
        core::Revision expected_revision);
    [[nodiscard]] core::Result<void> set_condition_authoring(
        std::string serialized_authoring,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> erase_condition_authoring_if_revision(
        core::Revision expected_revision);
    [[nodiscard]] core::Result<void> apply_source_reimport_plan(
        const assets::SourceReimportPlan& plan);
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
    std::map<std::string, geometry::CurvePath> curve_paths_;
    // Retained through deletion so undo/redo cannot reuse a stale revision.
    std::map<std::string, core::Revision> curve_path_lineage_;
    std::map<std::string, RoadSource> road_sources_;
    // Road intent has an independent lineage from its centerline source.
    std::map<std::string, core::Revision> road_source_lineage_;
    std::map<std::string, geometry::SweepSource> sweep_sources_;
    // Retained through deletion so sweep recipe IDs cannot reset their lineage.
    std::map<std::string, core::Revision> sweep_source_lineage_;
    std::map<std::string, spatial_field::ScalarField2D> spatial_fields_;
    // Includes active fields and tombstones, preventing identity reuse from
    // resetting source lineage after a governed undo/delete.
    std::map<std::string, core::Revision> spatial_field_lineage_;
    std::vector<TopologyReceiptRecord> topology_receipts_;
    scientific::ScientificModel scientific_model_;
    world::WorldModel world_model_;
    MaterialCatalog material_catalog_;
    PresentationCatalog presentation_catalog_;
    std::optional<std::string> semantic_sidecar_bytes_;
    std::optional<assets::Sha256Digest> semantic_sidecar_digest_;
    std::map<std::uint64_t, SerializedSculptStack> sculpt_stacks_;
    std::map<std::uint64_t, SerializedGarmentAsset> garment_assets_;
    std::map<std::uint64_t, SerializedClothRestPoseReceipt> cloth_rest_pose_receipts_;
    std::optional<SerializedEngineeringAuthoring> engineering_authoring_;
    std::optional<SerializedConditionAuthoring> condition_authoring_;
    assets::ProjectAssetRegistry source_asset_registry_;
    std::optional<assets::Sha256Digest> evaluation_graph_digest_;
    std::uint64_t next_mesh_id_ = 1;
    core::Revision revision_{};
};

} // namespace carto::project
