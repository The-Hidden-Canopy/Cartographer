#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/character/autorig_graph.hpp>
#include <carto/character/definition.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/pose/ik_fk_authoring.hpp>
#include <carto/weights/transfer.hpp>

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
    [[nodiscard]] core::Result<void> set_presentation_catalog(PresentationCatalog catalog);
    [[nodiscard]] core::Result<void> set_biological_authoring(
        biological::BiologicalAuthoringDocument source);
    [[nodiscard]] core::Result<void> erase_biological_authoring(
        std::string_view source_namespace,
        std::uint64_t asset_id,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_curve_path(geometry::CurvePath path);
    [[nodiscard]] core::Result<void> erase_curve_path(
        std::string identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_road_source(RoadSource road);
    [[nodiscard]] core::Result<void> erase_road_source(
        std::string identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_sweep_source(geometry::SweepSource source);
    [[nodiscard]] core::Result<void> erase_sweep_source(
        std::string identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_sign_text_source(geometry::SignTextSource source);
    [[nodiscard]] core::Result<void> erase_sign_text_source(
        std::string identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_spatial_field(
        spatial_field::ScalarField2D field);
    [[nodiscard]] core::Result<void> erase_spatial_field(
        std::string identity,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> set_semantic_sidecar(
        std::string serialized_sidecar,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> set_sculpt_stack(
        std::uint64_t mesh_asset,
        std::string serialized_stack,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> set_garment_asset(
        std::uint64_t mesh_asset,
        std::string serialized_asset,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> insert_rig_module_template(
        std::string serialized_template);
    [[nodiscard]] core::Result<core::Revision> replace_rig_module_template_if_revision(
        std::string identity,
        core::Revision expected_revision,
        assets::Sha256Digest expected_digest,
        std::string serialized_template);
    [[nodiscard]] core::Result<core::Revision> set_decal_placement(
        std::uint64_t placement_id,
        core::Revision expected_revision,
        std::string serialized_placement,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<core::Revision> set_pose_library(
        std::string identity,
        core::Revision expected_revision,
        std::string serialized_library,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<core::Revision> set_kinematic_graph(
        std::string identity,
        core::Revision expected_revision,
        std::string serialized_graph,
        assets::Sha256Digest digest);
    // Performs live source/review checks and the weight transfer before staging
    // one bundle; commit publishes it with the project journal. IK/FK switch
    // authoring is not represented by this record. Callers must provide the
    // observed source namespace and project revision as well as the row
    // revision (source API change); branch/session identity remains caller-owned.
    // These checks assume the existing single-threaded transaction model and
    // do not provide synchronization or cross-thread atomicity.
    [[nodiscard]] core::Result<core::Revision> accept_autorig(
        std::string identity, std::string_view expected_source_namespace,
        core::Revision expected_project_revision,
        core::Revision expected_record_revision,
        const character::AutorigAnalysisRequest& request,
        const character::AutorigCandidateSet& candidates,
        const character::AutorigReviewAcceptance& acceptance,
        const std::vector<character::AutorigFinalNode>& nodes,
        const character::RigDefinition& rig,
        const kinematic::JointLimitSet& limits,
        const kinematic::SkinBinding& source_binding,
        const character::SkinWeightLayer& source_layer,
        const weights::WeightCorrespondence& correspondence,
        const weights::JointTransferMap& joint_map,
        const weights::WeightTransferOptions& options,
        const weights::WeightLocks* locks = nullptr);
    // Caller must bind the observed source namespace and project revision as well as the record
    // revision. Existing chain identities cannot be removed here. This is a
    // source API change: callers must supply the additional project pin.
    [[nodiscard]] core::Result<core::Revision> configure_autorig_ik_fk(
        std::string identity, std::string_view expected_source_namespace,
        core::Revision expected_project_revision,
        core::Revision expected_record_revision,
        const std::vector<pose::IkFkAuthoring>& configurations);
    [[nodiscard]] core::Result<void> append_weight_edit_receipt(
        std::string serialized_receipt,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> append_curve_edit_receipt(
        std::string serialized_receipt,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> set_cloth_rest_pose_receipt(
        std::uint64_t mesh_asset,
        std::string serialized_receipt,
        assets::Sha256Digest digest);
    [[nodiscard]] core::Result<void> apply_source_reimport_plan(
        const assets::SourceReimportPlan& plan);
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
