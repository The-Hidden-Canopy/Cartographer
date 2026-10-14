#include <carto/agent/scene_query.hpp>
#include <carto/agent/workbench.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

namespace carto::agent {
namespace {

constexpr std::size_t kMaximumQueryResults = 512U;
constexpr std::size_t kMaximumIdentityBatch = 128U;
constexpr std::size_t kMaximumScopeObjects = 512U;
constexpr std::size_t kMaximumQueryCount = 256U;
constexpr std::size_t kMaximumObjectsExamined = 1'000'000U;
constexpr std::size_t kMaximumTopologyElements = 4'000'000U;
constexpr std::size_t kMaximumSemanticRecords = 262'144U;
constexpr std::size_t kMaximumQueryBytes = 64U * 1024U * 1024U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic denied(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic missing(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool in_scope(
    scene::ObjectId object,
    const std::vector<scene::ObjectId>& allowed_scope) noexcept {
    return allowed_scope.empty() ||
        std::find(allowed_scope.begin(), allowed_scope.end(), object) != allowed_scope.end();
}

core::Result<void> validate_scope(const std::vector<scene::ObjectId>& scope) {
    if (scope.empty()) {
        return core::Result<void>::failure(invalid(
            "scene queries require an explicit nonempty object scope"));
    }
    if (scope.size() > kMaximumScopeObjects) {
        return core::Result<void>::failure(invalid("scene query scope exceeds its object limit"));
    }
    std::set<scene::ObjectId> unique;
    for (const auto object : scope) {
        if (!object || !unique.insert(object).second) {
            return core::Result<void>::failure(invalid(
                "scene query scope contains a zero or duplicate object identity"));
        }
    }
    return core::Result<void>::success();
}

core::Vec3d transform_point(const core::Transform& transform, core::Vec3d point) noexcept {
    return transform.translation + transform.rotation.rotate(
        core::componentwise_multiply(transform.scale, point));
}

std::size_t encoded_size(const SceneQueryRow& row) noexcept {
    return sizeof(SceneQueryRow) + std::visit([](const auto& fact) -> std::size_t {
        using Fact = std::decay_t<decltype(fact)>;
        if constexpr (std::is_same_v<Fact, SceneNamedObjectFact>) {
            return fact.stable_name.size();
        } else if constexpr (std::is_same_v<Fact, FamiliarityRecordFact>) {
            return fact.semantic_id.size();
        }
        return 0U;
    }, row);
}

bool append_row(
    SceneQueryResult& result,
    SceneQueryBudget& budget,
    std::size_t result_limit,
    SceneQueryRow row) {
    if (result.rows.size() >= result_limit) {
        result.truncated = true;
        return false;
    }
    const std::size_t bytes = encoded_size(row);
    if (bytes > budget.max_result_bytes - budget.result_bytes) {
        result.truncated = true;
        return false;
    }
    result.rows.push_back(std::move(row));
    budget.result_bytes += bytes;
    result.result_bytes += bytes;
    return true;
}

struct CandidateObjects {
    std::vector<scene::SceneObject> objects;
    bool truncated = false;
};

core::Result<CandidateObjects> candidate_objects(
    const project::ProjectDocument& document,
    SceneQueryBudget& budget,
    const std::vector<scene::ObjectId>& allowed_scope,
    std::optional<scene::ObjectId> only_object,
    SceneQueryResult& result) {
    CandidateObjects candidates;
    const auto& scene = document.scene();
    const std::size_t remaining = budget.max_objects_examined - budget.objects_examined;
    if (only_object.has_value()) {
        if (!in_scope(*only_object, allowed_scope)) {
            return core::Result<CandidateObjects>::failure(denied(
                "scene query object is outside the declared task scope"));
        }
        if (remaining == 0U) {
            candidates.truncated = true;
            result.truncated = true;
            return core::Result<CandidateObjects>::success(std::move(candidates));
        }
        ++budget.objects_examined;
        result.objects_examined += 1U;
        if (const auto* object = scene.find(*only_object); object != nullptr) {
            candidates.objects.push_back(*object);
        } else {
            result.diagnostics.push_back(missing(
                "scene query object identity is absent at the captured revision"));
        }
        return core::Result<CandidateObjects>::success(std::move(candidates));
    }

    if (!allowed_scope.empty()) {
        std::vector<scene::ObjectId> ordered_scope = allowed_scope;
        std::sort(ordered_scope.begin(), ordered_scope.end());
        if (ordered_scope.size() > remaining) {
            ordered_scope.resize(remaining);
            candidates.truncated = true;
            result.truncated = true;
        }
        candidates.objects.reserve(ordered_scope.size());
        for (const auto object_id : ordered_scope) {
            ++budget.objects_examined;
            ++result.objects_examined;
            if (const auto* object = scene.find(object_id); object != nullptr) {
                candidates.objects.push_back(*object);
            } else {
                return core::Result<CandidateObjects>::failure(stale(
                    "a scoped scene identity is absent at the captured revision"));
            }
        }
        return core::Result<CandidateObjects>::success(std::move(candidates));
    }

    if (remaining == 0U) {
        candidates.truncated = scene.size() != 0U;
        result.truncated = candidates.truncated;
        return core::Result<CandidateObjects>::success(std::move(candidates));
    }
    const std::size_t bounded_limit = std::min(scene.size(), remaining);
    candidates.objects = scene.objects_sorted(bounded_limit);
    budget.objects_examined += candidates.objects.size();
    result.objects_examined += candidates.objects.size();
    candidates.truncated = candidates.objects.size() < scene.size();
    result.truncated = result.truncated || candidates.truncated;
    return core::Result<CandidateObjects>::success(std::move(candidates));
}

core::Result<std::pair<core::Vec3d, core::Vec3d>> world_face_geometry(
    const geometry::EditableMesh& mesh,
    const geometry::Face& face,
    const core::Transform& transform) {
    if (face.vertices.size() < 3U) {
        return core::Result<std::pair<core::Vec3d, core::Vec3d>>::failure(
            invalid("scene query face has fewer than three vertices"));
    }
    std::vector<core::Vec3d> points;
    points.reserve(face.vertices.size());
    for (const auto vertex_id : face.vertices) {
        const auto* vertex = mesh.find_vertex(vertex_id);
        if (vertex == nullptr) {
            return core::Result<std::pair<core::Vec3d, core::Vec3d>>::failure(
                invalid("scene query face references a missing vertex"));
        }
        points.push_back(transform_point(transform, vertex->position));
    }
    core::Vec3d normal{};
    core::Vec3d centroid{};
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const core::Vec3d current = points[index];
        const core::Vec3d next = points[(index + 1U) % points.size()];
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
        centroid = centroid + current;
    }
    normal = normal.normalized();
    centroid = centroid * (1.0 / static_cast<double>(points.size()));
    if (!normal.finite() || !centroid.finite()) {
        return core::Result<std::pair<core::Vec3d, core::Vec3d>>::failure(
            invalid("scene query face has no stable world-space normal"));
    }
    return core::Result<std::pair<core::Vec3d, core::Vec3d>>::success({centroid, normal});
}

} // namespace

core::Result<SceneQueryResult> SceneQuery::execute(
    AgentTask& task,
    const SceneQueryRequest& request) {
    if (!task.speculative_ || task.status_ == TaskStatus::accepted ||
        task.status_ == TaskStatus::rejected) {
        return core::Result<SceneQueryResult>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "scene queries are unavailable after an agent task is accepted or rejected"));
    }
    task.cached_snapshot_.reset();
    return SceneQuery::execute(
        task.scene_query_document(), request, task.query_budget_,
        task.plan_.objective.scope_objects);
}

core::Result<void> SceneQueryRequest::validate() const {
    if (limit == 0U || limit > kMaximumQueryResults) {
        return core::Result<void>::failure(invalid("scene query result limit is invalid"));
    }
    return std::visit([](const auto& query) -> core::Result<void> {
        using Query = std::decay_t<decltype(query)>;
        if constexpr (std::is_same_v<Query, ObjectsByIdQuery>) {
            if (query.objects.empty() || query.objects.size() > kMaximumIdentityBatch) {
                return core::Result<void>::failure(invalid(
                    "object identity query batch is empty or too large"));
            }
            std::set<scene::ObjectId> unique;
            for (const auto object : query.objects) {
                if (!object || !unique.insert(object).second) {
                    return core::Result<void>::failure(invalid(
                        "object identity query contains a zero or duplicate identity"));
                }
            }
        } else if constexpr (std::is_same_v<Query, ObjectsByStableNameQuery>) {
            if (query.exact_name.empty() || query.exact_name.size() > 256U) {
                return core::Result<void>::failure(invalid(
                    "stable-name query must contain 1 to 256 bytes"));
            }
        } else if constexpr (std::is_same_v<Query, ObjectsByMeshAssetQuery>) {
            if (query.mesh_asset == 0U) {
                return core::Result<void>::failure(invalid("mesh asset identity is zero"));
            }
        } else if constexpr (std::is_same_v<Query, ObjectsWithinDistanceQuery>) {
            if (!query.origin.finite() || !std::isfinite(query.radius) || query.radius < 0.0) {
                return core::Result<void>::failure(invalid(
                    "spatial query origin or radius is invalid"));
            }
        } else if constexpr (std::is_same_v<Query, FacesByOrientationQuery>) {
            if (!query.direction.finite() || !std::isfinite(query.direction.length()) ||
                query.direction.length() <= 1e-12 ||
                !std::isfinite(query.minimum_alignment) ||
                query.minimum_alignment < -1.0 || query.minimum_alignment > 1.0 ||
                (query.object.has_value() && !*query.object)) {
                return core::Result<void>::failure(invalid(
                    "face orientation query direction, threshold, or object is invalid"));
            }
        } else if constexpr (std::is_same_v<Query, BoundaryEdgesQuery>) {
            if (query.object.has_value() && !*query.object) {
                return core::Result<void>::failure(invalid(
                    "boundary edge query object identity is zero"));
            }
        } else if constexpr (std::is_same_v<Query, ConditionAssetsByTagQuery>) {
            if (query.exact_tag.empty() || query.exact_tag.size() > 256U) {
                return core::Result<void>::failure(invalid(
                    "CONDITION tag query must contain 1 to 256 bytes"));
            }
        } else if constexpr (std::is_same_v<Query, FamiliarityRecordsQuery>) {
            if (query.exact_semantic_id.has_value() &&
                (query.exact_semantic_id->empty() || query.exact_semantic_id->size() > 256U)) {
                return core::Result<void>::failure(invalid(
                    "CONDITION semantic identity must contain 1 to 256 bytes"));
            }
        }
        return core::Result<void>::success();
    }, predicate);
}

core::Result<void> SceneQueryBudget::validate() const {
    if (max_queries == 0U || max_queries > kMaximumQueryCount ||
        max_objects_examined == 0U || max_objects_examined > kMaximumObjectsExamined ||
        max_topology_elements_examined == 0U ||
        max_topology_elements_examined > kMaximumTopologyElements ||
        max_semantic_records_examined == 0U ||
        max_semantic_records_examined > kMaximumSemanticRecords ||
        max_result_bytes == 0U || max_result_bytes > kMaximumQueryBytes) {
        return core::Result<void>::failure(invalid("scene query budget limits are invalid"));
    }
    if (queries_used > max_queries || objects_examined > max_objects_examined ||
        topology_elements_examined > max_topology_elements_examined ||
        semantic_records_examined > max_semantic_records_examined ||
        result_bytes > max_result_bytes) {
        return core::Result<void>::failure(invalid("scene query budget usage exceeds its limits"));
    }
    return core::Result<void>::success();
}

core::Result<SceneQueryResult> SceneQuery::execute(
    const project::ProjectDocument& document,
    const SceneQueryRequest& request,
    SceneQueryBudget& budget,
    const std::vector<scene::ObjectId>& allowed_scope) {
    if (auto result = request.validate(); !result) {
        return core::Result<SceneQueryResult>::failure(result.error());
    }
    if (auto result = budget.validate(); !result) {
        return core::Result<SceneQueryResult>::failure(result.error());
    }
    if (auto result = validate_scope(allowed_scope); !result) {
        return core::Result<SceneQueryResult>::failure(result.error());
    }
    if (budget.queries_used >= budget.max_queries) {
        return core::Result<SceneQueryResult>::failure(denied(
            "scene query count exceeds the task read budget"));
    }

    ++budget.queries_used;
    SceneQueryResult result;
    result.source_revision = document.revision();
    const auto fail = [&result](core::Diagnostic diagnostic) {
        static_cast<void>(result);
        return core::Result<SceneQueryResult>::failure(std::move(diagnostic));
    };

    const auto executed = std::visit([&](const auto& query) -> core::Result<void> {
        using Query = std::decay_t<decltype(query)>;
        if constexpr (std::is_same_v<Query, ObjectsByIdQuery>) {
            if (std::any_of(query.objects.begin(), query.objects.end(), [&](scene::ObjectId id) {
                    return !in_scope(id, allowed_scope);
                })) {
                return core::Result<void>::failure(denied(
                    "object identity query requests an object outside the declared task scope"));
            }
            std::vector<scene::ObjectId> ordered = query.objects;
            std::sort(ordered.begin(), ordered.end());
            for (const auto id : ordered) {
                if (budget.objects_examined >= budget.max_objects_examined) {
                    result.truncated = true;
                    break;
                }
                ++budget.objects_examined;
                ++result.objects_examined;
                if (document.scene().find(id) == nullptr) {
                    if (result.diagnostics.empty()) {
                        result.diagnostics.push_back(missing(
                            "an object identity is absent at the captured revision"));
                    }
                    continue;
                }
                if (!append_row(result, budget, request.limit, SceneObjectIdentityFact{id})) break;
            }
        } else if constexpr (std::is_same_v<Query, ObjectsByStableNameQuery>) {
            auto candidates = candidate_objects(
                document, budget, allowed_scope, std::nullopt, result);
            if (!candidates) return core::Result<void>::failure(candidates.error());
            for (const auto& object : candidates.value().objects) {
                if (object.name != query.exact_name) continue;
                if (!append_row(result, budget, request.limit,
                        SceneNamedObjectFact{object.id, object.name})) break;
            }
        } else if constexpr (std::is_same_v<Query, ObjectsByMeshAssetQuery>) {
            auto candidates = candidate_objects(
                document, budget, allowed_scope, std::nullopt, result);
            if (!candidates) return core::Result<void>::failure(candidates.error());
            for (const auto& object : candidates.value().objects) {
                if (object.mesh_asset != query.mesh_asset) continue;
                if (!append_row(result, budget, request.limit, SceneObjectIdentityFact{object.id})) break;
            }
        } else if constexpr (std::is_same_v<Query, ObjectsWithinDistanceQuery>) {
            auto candidates = candidate_objects(
                document, budget, allowed_scope, std::nullopt, result);
            if (!candidates) return core::Result<void>::failure(candidates.error());
            std::vector<SceneSpatialObjectFact> matches;
            std::size_t matched_count = 0U;
            for (const auto& object : candidates.value().objects) {
                auto transform = document.scene().world_transform(object.id);
                if (!transform) {
                    if (result.diagnostics.empty()) result.diagnostics.push_back(transform.error());
                    continue;
                }
                const core::Vec3d position = transform.value().translation;
                const double distance = (position - query.origin).length();
                if (!std::isfinite(distance) || distance > query.radius) continue;
                const SceneSpatialObjectFact candidate{object.id, position, distance};
                ++matched_count;
                const auto position_in_order = std::lower_bound(
                    matches.begin(), matches.end(), candidate, [](const auto& left, const auto& right) {
                        if (left.distance != right.distance) return left.distance < right.distance;
                        return left.object < right.object;
                    });
                if (matches.size() < request.limit || position_in_order != matches.end()) {
                    matches.insert(position_in_order, candidate);
                    if (matches.size() > request.limit) matches.pop_back();
                }
            }
            for (const auto& match : matches) {
                if (!append_row(result, budget, request.limit, match)) break;
            }
            if (matched_count > request.limit) result.truncated = true;
        } else if constexpr (std::is_same_v<Query, FacesByOrientationQuery>) {
            auto candidates = candidate_objects(
                document, budget, allowed_scope, query.object, result);
            if (!candidates) return core::Result<void>::failure(candidates.error());
            const core::Vec3d direction = query.direction.normalized();
            for (const auto& object : candidates.value().objects) {
                if (!object.mesh_asset.has_value()) continue;
                const auto mesh = document.meshes().find(*object.mesh_asset);
                if (mesh == document.meshes().end()) {
                    if (result.diagnostics.empty()) result.diagnostics.push_back(missing(
                        "a scoped object references a missing mesh asset"));
                    continue;
                }
                auto transform = document.scene().world_transform(object.id);
                if (!transform) return core::Result<void>::failure(transform.error());
                const std::size_t remaining = budget.max_topology_elements_examined -
                    budget.topology_elements_examined;
                if (remaining == 0U) {
                    result.truncated = true;
                    break;
                }
                const std::size_t face_limit = std::min(mesh->second.face_count(), remaining);
                const auto faces = mesh->second.faces_sorted(face_limit);
                if (faces.size() < mesh->second.face_count() && face_limit == remaining) {
                    result.truncated = true;
                }
                for (const auto& face : faces) {
                    const std::size_t face_cost = face.vertices.size() + 1U;
                    if (face_cost > budget.max_topology_elements_examined -
                            budget.topology_elements_examined) {
                        result.truncated = true;
                        break;
                    }
                    budget.topology_elements_examined += face_cost;
                    result.topology_elements_examined += face_cost;
                    auto geometry = world_face_geometry(mesh->second, face, transform.value());
                    if (!geometry) {
                        if (result.diagnostics.empty()) result.diagnostics.push_back(geometry.error());
                        continue;
                    }
                    const double alignment = core::dot(geometry.value().second, direction);
                    if (alignment < query.minimum_alignment) continue;
                    if (!append_row(result, budget, request.limit, SceneFaceOrientationFact{
                            object.id, *object.mesh_asset, face.id,
                            geometry.value().first, geometry.value().second, alignment})) break;
                }
                if (result.truncated) break;
            }
        } else if constexpr (std::is_same_v<Query, BoundaryEdgesQuery>) {
            auto candidates = candidate_objects(
                document, budget, allowed_scope, query.object, result);
            if (!candidates) return core::Result<void>::failure(candidates.error());
            for (const auto& object : candidates.value().objects) {
                if (!object.mesh_asset.has_value()) continue;
                const auto mesh = document.meshes().find(*object.mesh_asset);
                if (mesh == document.meshes().end()) {
                    if (result.diagnostics.empty()) result.diagnostics.push_back(missing(
                        "a scoped object references a missing mesh asset"));
                    continue;
                }
                const std::size_t remaining = budget.max_topology_elements_examined -
                    budget.topology_elements_examined;
                if (remaining == 0U) {
                    result.truncated = true;
                    break;
                }
                const std::size_t bounded_edges = std::min(mesh->second.edge_count(), remaining);
                const auto edges = mesh->second.edges_sorted(bounded_edges);
                for (const auto& edge : edges) {
                    ++budget.topology_elements_examined;
                    ++result.topology_elements_examined;
                    if (edge.second_half_edge.has_value()) continue;
                    if (!append_row(result, budget, request.limit, SceneBoundaryEdgeFact{
                            object.id, *object.mesh_asset, edge.id, edge.first, edge.second})) break;
                }
                if (bounded_edges < mesh->second.edge_count()) {
                    result.truncated = true;
                    break;
                }
            }
        } else if constexpr (std::is_same_v<Query, ConditionAssetsByTagQuery> ||
                             std::is_same_v<Query, FamiliarityRecordsQuery>) {
            const auto& serialized = document.condition_authoring();
            if (!serialized.has_value()) {
                result.diagnostics.push_back(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "CONDITION semantic query has no authored sidecar in this project"));
                return core::Result<void>::success();
            }
            auto authoring = condition::AuthoringDocument::deserialize(serialized->bytes);
            if (!authoring) {
                return core::Result<void>::failure(authoring.error().with_context(
                    "CONDITION SceneQuery"));
            }
            if (authoring.value().project_revision != document.revision()) {
                return core::Result<void>::failure(stale(
                    "CONDITION semantic source revision is stale for the project"));
            }
            std::size_t remaining = budget.max_semantic_records_examined -
                budget.semantic_records_examined;
            if constexpr (std::is_same_v<Query, ConditionAssetsByTagQuery>) {
                const std::size_t count = authoring.value().assets.size();
                const std::size_t examine = std::min(count, remaining);
                for (std::size_t index = 0U; index < examine; ++index) {
                    const auto& asset = authoring.value().assets[index];
                    ++budget.semantic_records_examined;
                    ++result.semantic_records_examined;
                    if (std::find(asset.tags.begin(), asset.tags.end(), query.exact_tag) ==
                            asset.tags.end()) continue;
                    const scene::ObjectId object{asset.authored_object_id};
                    if (!in_scope(object, allowed_scope)) continue;
                    if (!append_row(result, budget, request.limit, ConditionAssetTagFact{
                            asset.id, object, asset.asset_class, asset.fidelity})) break;
                }
                if (examine < count) result.truncated = true;
            } else {
                std::map<condition::Id, const condition::SourceAsset*> assets_by_id;
                const std::size_t asset_count = authoring.value().assets.size();
                const std::size_t assets_to_index = std::min(asset_count, remaining);
                for (std::size_t index = 0U; index < assets_to_index; ++index) {
                    const auto& asset = authoring.value().assets[index];
                    assets_by_id.emplace(asset.id, &asset);
                    ++budget.semantic_records_examined;
                    ++result.semantic_records_examined;
                }
                if (assets_to_index < asset_count) {
                    result.truncated = true;
                    return core::Result<void>::success();
                }
                remaining = budget.max_semantic_records_examined -
                    budget.semantic_records_examined;
                const std::size_t count = authoring.value().familiarity.size();
                const std::size_t examine = std::min(count, remaining);
                for (std::size_t index = 0U; index < examine; ++index) {
                    const auto& record = authoring.value().familiarity[index];
                    ++budget.semantic_records_examined;
                    ++result.semantic_records_examined;
                    if (query.exact_semantic_id.has_value() &&
                        record.semantic_id != *query.exact_semantic_id) continue;
                    const auto asset = assets_by_id.find(record.asset_id);
                    if (asset == assets_by_id.end()) {
                        return core::Result<void>::failure(invalid(
                            "CONDITION familiarity record references a missing asset"));
                    }
                    const scene::ObjectId object{asset->second->authored_object_id};
                    if (!in_scope(object, allowed_scope)) continue;
                    if (!append_row(result, budget, request.limit, FamiliarityRecordFact{
                            record.id, record.asset_id, object, record.semantic_id,
                            record.importance, record.may_change_physically})) break;
                }
                if (examine < count) result.truncated = true;
            }
        }
        return core::Result<void>::success();
    }, request.predicate);

    if (!executed) return fail(executed.error());
    for (const auto& diagnostic : result.diagnostics) {
        std::size_t bytes = sizeof(core::Diagnostic) + diagnostic.message.size();
        for (const auto& context : diagnostic.context) bytes += context.size();
        if (bytes > budget.max_result_bytes - budget.result_bytes) {
            return fail(denied("scene query diagnostics exceed the result-byte budget"));
        }
        budget.result_bytes += bytes;
        result.result_bytes += bytes;
    }
    return core::Result<SceneQueryResult>::success(std::move(result));
}

} // namespace carto::agent
