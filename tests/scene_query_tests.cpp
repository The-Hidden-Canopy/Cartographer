#include <carto/agent/workbench.hpp>
#include <carto/agent/scene_query.hpp>
#include <carto/application/application.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition) do { \
    if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
} while (false)

std::vector<carto::scene::ObjectId> create_boxes(
    carto::application::ApplicationSession& session,
    const std::vector<std::string>& names) {
    for (const auto& name : names) {
        const auto result = carto::application::HumanApplicationAccess::dispatch(
            session, carto::application::CreateBoxAction{name, {1.0, 1.0, 1.0}});
        REQUIRE(result);
    }
    std::vector<carto::scene::ObjectId> objects;
    for (const auto& view : session.snapshot().objects) objects.push_back(view.object.id);
    return objects;
}

carto::agent::AgentTask begin_task(
    carto::application::ApplicationSession& session,
    const std::vector<carto::scene::ObjectId>& scope,
    carto::agent::AgentWorkEnvelope envelope = {}) {
    const auto snapshot = session.snapshot();
    envelope.scope_objects = scope;
    envelope.max_objects_touched = std::max<std::size_t>(scope.size(), 1U);
    carto::agent::AgentPlan plan{
        "scene-query-task",
        {"scene-query-objective", "Find an authored target", snapshot.project_revision,
         scope, {}, {}, {}},
        {{"planned-edit", {scope.front(), carto::geometry::FaceId{1U}}, "inset face 0.1"}}};
    auto task = carto::agent::AgentTask::begin(session, std::move(plan), std::move(envelope));
    REQUIRE(task);
    return std::move(task.value());
}

void identity_queries_are_revision_bound_and_scope_limited() {
    carto::application::ApplicationSession session;
    const auto objects = create_boxes(session, {"East Wall", "Protected Sign"});
    const auto base_revision = session.snapshot().project_revision;

    carto::project::ProjectDocument unscoped_document;
    carto::agent::SceneQueryBudget unscoped_budget;
    auto unscoped = carto::agent::SceneQuery::execute(
        unscoped_document,
        {carto::agent::ObjectsByStableNameQuery{"East Wall"}, 8U},
        unscoped_budget,
        {});
    REQUIRE(!unscoped);
    REQUIRE(unscoped.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(unscoped_budget.queries_used == 0U);

    auto task = begin_task(session, {objects.front()});

    auto by_name = carto::agent::SceneQuery::execute(task, {carto::agent::ObjectsByStableNameQuery{"East Wall"}, 8U});
    REQUIRE(by_name);
    REQUIRE(by_name.value().source_revision == base_revision);
    REQUIRE(by_name.value().rows.size() == 1U);
    const auto* named = std::get_if<carto::agent::SceneNamedObjectFact>(
        &by_name.value().rows.front());
    REQUIRE(named != nullptr);
    REQUIRE(named->object == objects.front());
    REQUIRE(named->stable_name == "East Wall");

    auto hidden_name = carto::agent::SceneQuery::execute(task, {
        carto::agent::ObjectsByStableNameQuery{"Protected Sign"}, 8U});
    REQUIRE(hidden_name);
    REQUIRE(hidden_name.value().rows.empty());

    auto outside_scope = carto::agent::SceneQuery::execute(task, {carto::agent::ObjectsByIdQuery{{objects.back()}}, 8U});
    REQUIRE(!outside_scope);
    REQUIRE(session.snapshot().project_revision == base_revision);
    REQUIRE(task.snapshot().query_budget.queries_used == 3U);
}

void spatial_orientation_and_boundary_queries_return_measured_facts() {
    carto::application::ApplicationSession session;
    const auto objects = create_boxes(session, {"Near", "Far"});
    carto::core::Transform far_transform = carto::core::Transform::identity();
    far_transform.translation = {10.0, 0.0, 0.0};
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetObjectTransformAction{
            objects[1U], far_transform, std::nullopt}));
    const auto scope = session.snapshot().project_revision;
    auto task = begin_task(session, objects);

    auto near = carto::agent::SceneQuery::execute(task, {
        carto::agent::ObjectsWithinDistanceQuery{{0.0, 0.0, 0.0}, 2.0}, 8U});
    REQUIRE(near);
    REQUIRE(near.value().source_revision == scope);
    REQUIRE(near.value().rows.size() == 1U);
    const auto* spatial = std::get_if<carto::agent::SceneSpatialObjectFact>(
        &near.value().rows.front());
    REQUIRE(spatial != nullptr);
    REQUIRE(spatial->object == objects.front());
    REQUIRE(std::abs(spatial->distance) < 1e-12);

    auto oriented = carto::agent::SceneQuery::execute(task, {
        carto::agent::FacesByOrientationQuery{{0.0, 0.0, 1.0}, 0.99, objects.front()}, 8U});
    REQUIRE(oriented);
    REQUIRE(!oriented.value().rows.empty());
    for (const auto& row : oriented.value().rows) {
        const auto* face = std::get_if<carto::agent::SceneFaceOrientationFact>(&row);
        REQUIRE(face != nullptr);
        REQUIRE(face->object == objects.front());
        REQUIRE(face->alignment >= 0.99);
        REQUIRE(std::abs(face->world_normal.length() - 1.0) < 1e-10);
    }

    carto::application::ApplicationSession plane_session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        plane_session, carto::application::CreatePlaneAction{"Open Surface", 2.0, 2.0}));
    const auto plane_object = plane_session.snapshot().objects.front().object.id;
    auto plane_task = begin_task(plane_session, {plane_object});
    auto boundary = carto::agent::SceneQuery::execute(plane_task, {carto::agent::BoundaryEdgesQuery{plane_object}, 16U});
    REQUIRE(boundary);
    REQUIRE(boundary.value().rows.size() == 4U);
    for (const auto& row : boundary.value().rows) {
        REQUIRE(std::holds_alternative<carto::agent::SceneBoundaryEdgeFact>(row));
    }
    REQUIRE(plane_session.snapshot().project_revision == plane_task.snapshot().base_revision);
}

void query_and_scan_budgets_truncate_or_fail_closed() {
    carto::application::ApplicationSession session;
    const auto objects = create_boxes(session, {"Duplicate", "Duplicate"});
    carto::agent::AgentWorkEnvelope envelope;
    envelope.max_scene_queries = 1U;
    envelope.max_objects_examined = 4U;
    auto task = begin_task(session, objects, envelope);

    auto truncated = carto::agent::SceneQuery::execute(task, {
        carto::agent::ObjectsByStableNameQuery{"Duplicate"}, 1U});
    REQUIRE(truncated);
    REQUIRE(truncated.value().truncated);
    REQUIRE(truncated.value().rows.size() == 1U);
    REQUIRE(truncated.value().objects_examined == 2U);
    REQUIRE(task.snapshot().query_budget.objects_examined == 2U);

    auto over_budget = carto::agent::SceneQuery::execute(task, {
        carto::agent::ObjectsByIdQuery{{objects.front()}}, 1U});
    REQUIRE(!over_budget);
    REQUIRE(task.snapshot().query_budget.queries_used == 1U);

    carto::application::ApplicationSession invalid_session;
    const auto one = create_boxes(invalid_session, {"One"});
    auto invalid_task = begin_task(invalid_session, one);
    auto invalid_request = carto::agent::SceneQuery::execute(invalid_task, {
        carto::agent::ObjectsWithinDistanceQuery{
            {0.0, 0.0, 0.0}, std::numeric_limits<double>::quiet_NaN()}, 4U});
    REQUIRE(!invalid_request);
    REQUIRE(invalid_task.snapshot().query_budget.queries_used == 0U);

    carto::application::ApplicationSession bounded_scan_session;
    const auto scan_objects = create_boxes(bounded_scan_session, {"First", "Second"});
    carto::agent::AgentWorkEnvelope scan_envelope;
    scan_envelope.max_objects_examined = 1U;
    auto scan_task = begin_task(bounded_scan_session, scan_objects, scan_envelope);
    auto bounded_scan = carto::agent::SceneQuery::execute(scan_task, {
        carto::agent::ObjectsByStableNameQuery{"First"}, 8U});
    REQUIRE(bounded_scan);
    REQUIRE(bounded_scan.value().truncated);
    REQUIRE(bounded_scan.value().objects_examined == 1U);

    carto::application::ApplicationSession bounded_bytes_session;
    const auto byte_objects = create_boxes(bounded_bytes_session, {"Byte Bound"});
    carto::agent::AgentWorkEnvelope byte_envelope;
    byte_envelope.max_query_result_bytes = 1U;
    auto byte_task = begin_task(bounded_bytes_session, byte_objects, byte_envelope);
    auto bounded_bytes = carto::agent::SceneQuery::execute(byte_task, {
        carto::agent::ObjectsByIdQuery{{byte_objects.front()}}, 4U});
    REQUIRE(bounded_bytes);
    REQUIRE(bounded_bytes.value().truncated);
    REQUIRE(bounded_bytes.value().rows.empty());
    REQUIRE(bounded_bytes.value().result_bytes == 0U);

    carto::application::ApplicationSession bounded_topology_session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        bounded_topology_session, carto::application::CreatePlaneAction{"Open Surface", 2.0, 2.0}));
    const auto topology_object = bounded_topology_session.snapshot().objects.front().object.id;
    carto::agent::AgentWorkEnvelope topology_envelope;
    topology_envelope.max_topology_elements_examined = 2U;
    auto topology_task = begin_task(bounded_topology_session, {topology_object}, topology_envelope);
    auto bounded_topology = carto::agent::SceneQuery::execute(topology_task, {
        carto::agent::BoundaryEdgesQuery{topology_object}, 8U});
    REQUIRE(bounded_topology);
    REQUIRE(bounded_topology.value().truncated);
    REQUIRE(bounded_topology.value().rows.size() == 2U);
    REQUIRE(bounded_topology.value().topology_elements_examined == 2U);
}

void condition_semantics_require_current_explicit_source_data() {
    carto::application::ApplicationSession missing_session;
    const auto no_sidecar_objects = create_boxes(missing_session, {"A", "B", "C"});
    auto missing_task = begin_task(missing_session, no_sidecar_objects);
    auto missing = carto::agent::SceneQuery::execute(missing_task, {
        carto::agent::FamiliarityRecordsQuery{std::nullopt}, 8U});
    REQUIRE(missing);
    REQUIRE(missing.value().rows.empty());
    REQUIRE(missing.value().diagnostics.size() == 1U);
    REQUIRE(missing.value().diagnostics.front().code == carto::core::ErrorCode::unsupported);

    carto::application::ApplicationSession current_session;
    const auto current_objects = create_boxes(current_session, {"A", "B", "C"});
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    fixture.value().project_revision = current_session.snapshot().project_revision.next();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        current_session, carto::application::ImportConditionAuthoringAction{
            fixture.value().serialize()}));
    REQUIRE(current_session.condition_authoring());
    REQUIRE(current_session.condition_authoring().value().project_revision ==
        current_session.snapshot().project_revision);
    auto current_task = begin_task(current_session, current_objects);
    auto tagged = carto::agent::SceneQuery::execute(current_task, {
        carto::agent::ConditionAssetsByTagQuery{"safe-place"}, 8U});
    REQUIRE(tagged);
    REQUIRE(tagged.value().rows.size() == 1U);
    const auto* tag_fact = std::get_if<carto::agent::ConditionAssetTagFact>(
        &tagged.value().rows.front());
    REQUIRE(tag_fact != nullptr);
    REQUIRE(tag_fact->asset == 1U);
    REQUIRE(tag_fact->authored_object == current_objects.front());

    auto familiar = carto::agent::SceneQuery::execute(current_task, {
        carto::agent::FamiliarityRecordsQuery{
            std::string("SAFEPLACE.MOTEL8.SIGN.MISSING_LETTER_R")}, 8U});
    REQUIRE(familiar);
    REQUIRE(familiar.value().rows.size() == 1U);
    const auto* familiarity = std::get_if<carto::agent::FamiliarityRecordFact>(
        &familiar.value().rows.front());
    REQUIRE(familiarity != nullptr);
    REQUIRE(familiarity->asset == 2U);
    REQUIRE(familiarity->authored_object == current_objects[1U]);
    REQUIRE(!familiarity->may_change_physically);

    auto out_of_scope_task = begin_task(current_session, {current_objects[2U]});
    auto out_of_scope_semantic = carto::agent::SceneQuery::execute(out_of_scope_task, {
        carto::agent::ConditionAssetsByTagQuery{"safe-place"}, 8U});
    REQUIRE(out_of_scope_semantic);
    REQUIRE(out_of_scope_semantic.value().rows.empty());

    carto::application::ApplicationSession stale_session;
    const auto stale_objects = create_boxes(stale_session, {"A", "B", "C"});
    auto stale_fixture = carto::condition::make_condition_fixture();
    REQUIRE(stale_fixture);
    stale_fixture.value().project_revision = stale_session.snapshot().project_revision;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        stale_session, carto::application::ImportConditionAuthoringAction{
            stale_fixture.value().serialize()}));
    auto stale_task = begin_task(stale_session, stale_objects);
    auto stale_query = carto::agent::SceneQuery::execute(stale_task, {
        carto::agent::FamiliarityRecordsQuery{std::nullopt}, 8U});
    REQUIRE(!stale_query);
    REQUIRE(stale_query.error().code == carto::core::ErrorCode::stale_data);

    carto::application::ApplicationSession bounded_semantics_session;
    const auto bounded_objects = create_boxes(bounded_semantics_session, {"A", "B", "C"});
    auto bounded_fixture = carto::condition::make_condition_fixture();
    REQUIRE(bounded_fixture);
    bounded_fixture.value().project_revision =
        bounded_semantics_session.snapshot().project_revision.next();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        bounded_semantics_session, carto::application::ImportConditionAuthoringAction{
            bounded_fixture.value().serialize()}));
    carto::agent::AgentWorkEnvelope semantic_envelope;
    semantic_envelope.max_semantic_records_examined = 2U;
    auto semantic_task = begin_task(
        bounded_semantics_session, bounded_objects, semantic_envelope);
    auto bounded_semantics = carto::agent::SceneQuery::execute(semantic_task, {
        carto::agent::FamiliarityRecordsQuery{std::nullopt}, 8U});
    REQUIRE(bounded_semantics);
    REQUIRE(bounded_semantics.value().truncated);
    REQUIRE(bounded_semantics.value().rows.empty());
    REQUIRE(bounded_semantics.value().semantic_records_examined == 2U);
}

} // namespace

int main() {
    try {
        identity_queries_are_revision_bound_and_scope_limited();
        spatial_orientation_and_boundary_queries_return_measured_facts();
        query_and_scan_budgets_truncate_or_fail_closed();
        condition_semantics_require_current_explicit_source_data();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
