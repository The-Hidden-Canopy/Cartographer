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
#include <variant>
#include <vector>

namespace carto::condition {
enum class AssetClass : std::uint8_t;
enum class FidelityClass : std::uint8_t;
}

namespace carto::project {
class ProjectDocument;
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

// SceneQuery is a bounded read-only vocabulary. It returns copied facts and
// stable identities; it never exposes a project, scene, or mesh reference.
class SceneQuery final {
public:
    [[nodiscard]] static core::Result<SceneQueryResult> execute(
        AgentTask& task,
        const SceneQueryRequest& request);
    [[nodiscard]] static core::Result<SceneQueryResult> execute(
        const project::ProjectDocument& document,
        const SceneQueryRequest& request,
        SceneQueryBudget& budget,
        const std::vector<scene::ObjectId>& allowed_scope);
};

} // namespace carto::agent
