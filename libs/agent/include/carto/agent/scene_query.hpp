#pragma once

#include <carto/agent/scene_query_budget.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace carto::condition {
enum class AssetClass : std::uint8_t;
enum class FidelityClass : std::uint8_t;
}

namespace carto::project {
class ProjectDocument;
}

namespace carto::application {
class ApplicationSession;
}

namespace carto::agent {

class AgentTask;

struct ObjectsByIdQuery {
    std::vector<scene::ObjectId> objects;
};

struct ObjectsByStableNameQuery {
    std::string exact_name;
};

struct ObjectsByMeshAssetQuery {
    std::uint64_t mesh_asset = 0U;
};

struct ObjectsWithinDistanceQuery {
    core::Vec3d origin;
    double radius = 0.0;
};

struct FacesByOrientationQuery {
    core::Vec3d direction;
    double minimum_alignment = 0.9;
    std::optional<scene::ObjectId> object;
};

struct BoundaryEdgesQuery {
    std::optional<scene::ObjectId> object;
};

struct ConditionAssetsByTagQuery {
    std::string exact_tag;
};

struct ConditionAssetInventoryQuery {
    std::optional<scene::ObjectId> object;
};

struct FamiliarityRecordsQuery {
    std::optional<std::string> exact_semantic_id;
};

using SceneQueryPredicate = std::variant<
    ObjectsByIdQuery,
    ObjectsByStableNameQuery,
    ObjectsByMeshAssetQuery,
    ObjectsWithinDistanceQuery,
    FacesByOrientationQuery,
    BoundaryEdgesQuery,
    ConditionAssetsByTagQuery,
    ConditionAssetInventoryQuery,
    FamiliarityRecordsQuery>;

struct SceneQueryRequest {
    SceneQueryPredicate predicate;
    std::size_t limit = 64U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SceneObjectIdentityFact {
    scene::ObjectId object;
};

struct SceneNamedObjectFact {
    scene::ObjectId object;
    std::string stable_name;
};

struct SceneSpatialObjectFact {
    scene::ObjectId object;
    core::Vec3d world_origin;
    double distance = 0.0;
};

struct SceneFaceOrientationFact {
    scene::ObjectId object;
    std::uint64_t mesh_asset = 0U;
    geometry::FaceId face;
    core::Vec3d world_centroid;
    core::Vec3d world_normal;
    double alignment = 0.0;
};

struct SceneBoundaryEdgeFact {
    scene::ObjectId object;
    std::uint64_t mesh_asset = 0U;
    geometry::EdgeId edge;
    geometry::VertexId first;
    geometry::VertexId second;
};

struct ConditionAssetTagFact {
    std::uint64_t asset = 0U;
    scene::ObjectId authored_object;
    condition::AssetClass asset_class{};
    condition::FidelityClass fidelity{};
    std::uint64_t source_id = 0U;
    std::string source_revision;
    bool source_current = false;
    bool source_reviewed = false;
    std::vector<std::string> protected_features;
};

enum class ConditionTechnicalCheckStatus : std::uint8_t {
    passed,
    failed,
    incomplete,
    unavailable,
    not_evaluated,
};

[[nodiscard]] std::string_view condition_technical_check_status_name(
    ConditionTechnicalCheckStatus status) noexcept;

struct ConditionAssetInventoryFact {
    std::uint64_t asset = 0U;
    scene::ObjectId authored_object;
    condition::AssetClass asset_class{};
    condition::FidelityClass fidelity{};
    std::uint64_t source_id = 0U;
    std::string source_revision;
    bool source_current = false;
    bool source_reviewed = false;
    std::vector<std::string> protected_features;
    std::size_t safe_place_profiles = 0U;
    std::size_t familiarity_records = 0U;
    std::size_t view_conditions = 0U;
    std::size_t variant_candidates = 0U;
    std::size_t temporal_packages = 0U;
    std::size_t surface_states = 0U;
    std::size_t interaction_anchors = 0U;
    std::size_t collection_memberships = 0U;
    std::size_t prefab_memberships = 0U;
    std::size_t region_kit_memberships = 0U;
    bool related_records_complete = false;
    // These checks report only what the project currently stores. They do not
    // establish full production or CONDITION readiness.
    std::uint64_t mesh_asset = 0U;
    ConditionTechnicalCheckStatus mesh_validation =
        ConditionTechnicalCheckStatus::not_evaluated;
    ConditionTechnicalCheckStatus uv_set_presence =
        ConditionTechnicalCheckStatus::not_evaluated;
    ConditionTechnicalCheckStatus material_region_assignments =
        ConditionTechnicalCheckStatus::not_evaluated;
    std::size_t mesh_vertices = 0U;
    std::size_t mesh_edges = 0U;
    std::size_t mesh_faces = 0U;
    std::size_t uv_set_count = 0U;
    std::uint64_t active_render_uv_set = 0U;
    std::size_t lightmap_candidate_uv_sets = 0U;
    std::size_t material_region_count = 0U;
    std::size_t resolved_material_region_count = 0U;
    bool technical_checks_complete = false;
};

struct FamiliarityRecordFact {
    std::uint64_t record = 0U;
    std::uint64_t asset = 0U;
    scene::ObjectId authored_object;
    std::string semantic_id;
    double importance = 0.0;
    bool may_change_physically = false;
    std::uint64_t source_id = 0U;
    std::string source_revision;
    bool source_current = false;
    bool source_reviewed = false;
};

using SceneQueryRow = std::variant<
    SceneObjectIdentityFact,
    SceneNamedObjectFact,
    SceneSpatialObjectFact,
    SceneFaceOrientationFact,
    SceneBoundaryEdgeFact,
    ConditionAssetTagFact,
    ConditionAssetInventoryFact,
    FamiliarityRecordFact>;

struct SceneQueryResult {
    core::Revision source_revision;
    std::vector<SceneQueryRow> rows;
    std::vector<core::Diagnostic> diagnostics;
    std::size_t objects_examined = 0U;
    std::size_t topology_elements_examined = 0U;
    std::size_t semantic_records_examined = 0U;
    std::size_t result_bytes = 0U;
    bool truncated = false;
};

enum class ConditionEvidenceStatus : std::uint8_t {
    verified,
    needs_review,
    incomplete,
    unavailable,
};

struct ConditionAssetEvidenceAudit {
    std::uint64_t asset = 0U;
    scene::ObjectId authored_object;
    condition::AssetClass asset_class{};
    condition::FidelityClass fidelity{};
    ConditionEvidenceStatus source_status = ConditionEvidenceStatus::unavailable;
    ConditionEvidenceStatus metadata_scan_status = ConditionEvidenceStatus::unavailable;
};

// A conservative summary of the bounded inventory query. "verified" applies
// only to source lineage or scan completeness; it never means CONDITION-ready.
struct ConditionEvidenceAudit {
    core::Revision source_revision;
    std::vector<ConditionAssetEvidenceAudit> assets;
    std::size_t current_reviewed_sources = 0U;
    std::size_t complete_metadata_scans = 0U;
    std::size_t complete_technical_inspections = 0U;
    std::size_t mesh_valid_assets = 0U;
    std::size_t uv_set_presence_passed_assets = 0U;
    std::size_t material_region_assignments_passed_assets = 0U;
    std::size_t unrecognized_rows = 0U;
    bool query_complete = false;
    bool source_evidence_complete = false;
    bool metadata_scan_complete = false;
    bool technical_inspection_complete = false;
    // Collision, traversal, tangents, bakes, source-digest correspondence,
    // and domain-specific CONDITION policy are not evaluated by this query.
    static constexpr bool production_readiness_established = false;
};

[[nodiscard]] ConditionEvidenceAudit audit_condition_inventory(
    const SceneQueryResult& inventory);

enum class ConditionReadinessGateStatus : std::uint8_t {
    passed,
    failed,
    incomplete,
    unavailable,
    not_evaluated,
};

struct ConditionReadinessGate {
    std::string id;
    std::string label;
    ConditionReadinessGateStatus status = ConditionReadinessGateStatus::not_evaluated;
    std::string evidence;
    std::vector<std::uint64_t> scoped_assets;
    std::vector<scene::ObjectId> scoped_objects;
    std::vector<std::uint64_t> affected_assets;
    std::vector<scene::ObjectId> affected_objects;
};

// Gate results summarize only facts present in a complete, revision-bound
// inventory. Missing domain validators remain explicit and this report never
// establishes overall CONDITION or production readiness. Scoped identities
// describe the observed query; affected identities name only rows that failed,
// were unavailable, or were incomplete at the asset level.
struct ConditionReadinessReport {
    ConditionEvidenceAudit inventory;
    std::vector<ConditionReadinessGate> gates;
    bool all_observed_checks_passed = false;
    static constexpr bool production_readiness_established = false;
};

[[nodiscard]] ConditionReadinessReport audit_condition_readiness(
    const SceneQueryResult& inventory);
[[nodiscard]] std::string_view condition_readiness_gate_status_name(
    ConditionReadinessGateStatus status) noexcept;

// SceneQuery is a bounded read-only vocabulary. It returns copied facts and
// stable identities; it never exposes a project, scene, or mesh reference.
class SceneQuery final {
public:
    [[nodiscard]] static core::Result<SceneQueryResult> execute(
        AgentTask& task,
        const SceneQueryRequest& request);
    [[nodiscard]] static core::Result<SceneQueryResult> execute(
        const ::carto::application::ApplicationSession& source,
        const SceneQueryRequest& request,
        SceneQueryBudget& budget,
        const std::vector<scene::ObjectId>& allowed_scope,
        core::Revision expected_source_revision);
    [[nodiscard]] static core::Result<SceneQueryResult> execute(
        const project::ProjectDocument& document,
        const SceneQueryRequest& request,
        SceneQueryBudget& budget,
        const std::vector<scene::ObjectId>& allowed_scope);
};

} // namespace carto::agent
