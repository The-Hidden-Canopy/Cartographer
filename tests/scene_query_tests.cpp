#include <carto/agent/workbench.hpp>
#include <carto/agent/scene_query.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/application/application.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

carto::geometry::EditableMesh make_condition_technical_mesh(bool include_lightmap_candidate = true) {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}}}));

    attributes::AttributeSet payload({
        {attributes::AttributeDomain::corner, 3U},
        {attributes::AttributeDomain::face, 1U},
    });
    REQUIRE(payload.add_layer(attributes::AttributeLayer{
        {"uv.render.corners", attributes::AttributeDomain::corner,
            attributes::AttributeType::vec2},
        {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0}, core::Vec2d{0.0, 1.0}}}));
    if (include_lightmap_candidate) {
        REQUIRE(payload.add_layer(attributes::AttributeLayer{
            {"uv.lightmap.corners", attributes::AttributeDomain::corner,
                attributes::AttributeType::vec2},
            {core::Vec2d{0.0, 0.0}, core::Vec2d{0.5, 0.0}, core::Vec2d{0.0, 0.5}}}));
    }
    REQUIRE(payload.add_layer(attributes::AttributeLayer{
        {"cartographer.material_slot", attributes::AttributeDomain::face,
            attributes::AttributeType::uint32},
        {std::uint32_t{1U}}}));

    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
        1U, "Render UV", "uv.render.corners", true, true, false, 1U}));
    if (include_lightmap_candidate) {
        REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
            2U, "Lightmap UV", "uv.lightmap.corners", false, false, true, 1U}));
    }
    REQUIRE(mesh.set_attribute_payload(std::move(payload), std::move(uv_sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::scene::ObjectId create_condition_mesh_target(
    carto::application::ApplicationSession& session,
    bool assign_material,
    bool include_lightmap_candidate = true) {
    using namespace carto;
    REQUIRE(application::HumanApplicationAccess::dispatch(
        session, application::CreateMeshObjectAction{
            "CONDITION technical target", make_condition_technical_mesh(
                include_lightmap_candidate)}));
    auto snapshot = session.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.objects.front().object.mesh_asset.has_value());
    const auto object = snapshot.objects.front().object.id;
    const auto mesh_asset = *snapshot.objects.front().object.mesh_asset;
    REQUIRE(application::HumanApplicationAccess::dispatch(
        session, application::CreateMaterialAction{
            snapshot.project_revision, "Unassigned preview material", {}}));
    snapshot = session.snapshot();
    REQUIRE(snapshot.materials.size() == 1U);
    if (assign_material) {
        REQUIRE(application::HumanApplicationAccess::dispatch(
            session, application::SetMaterialRegionAssignmentAction{
                snapshot.project_revision, {mesh_asset, 1U},
                snapshot.materials.front().id, std::nullopt}));
    }
    return object;
}

void import_condition_fixture_for(
    carto::application::ApplicationSession& session,
    carto::scene::ObjectId object) {
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    fixture.value().project_revision = session.snapshot().project_revision.next();
    fixture.value().assets.front().authored_object_id = object.value;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportConditionAuthoringAction{
            fixture.value().serialize()}));
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
    REQUIRE(tag_fact->source_id == 1U);
    REQUIRE(tag_fact->source_revision == "fixture-1");
    REQUIRE(tag_fact->source_current);
    REQUIRE(tag_fact->source_reviewed);
    REQUIRE(tag_fact->protected_features ==
        std::vector<std::string>({"entry-door", "silhouette"}));

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
    REQUIRE(familiarity->source_id == 1U);
    REQUIRE(familiarity->source_revision == "fixture-1");
    REQUIRE(familiarity->source_current);
    REQUIRE(familiarity->source_reviewed);

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

    carto::application::ApplicationSession unreviewed_session;
    const auto unreviewed_objects = create_boxes(unreviewed_session, {"A", "B", "C"});
    auto unreviewed_fixture = carto::condition::make_condition_fixture();
    REQUIRE(unreviewed_fixture);
    unreviewed_fixture.value().project_revision =
        unreviewed_session.snapshot().project_revision.next();
    unreviewed_fixture.value().sources.front().current = false;
    unreviewed_fixture.value().sources.front().reviewed = false;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        unreviewed_session, carto::application::ImportConditionAuthoringAction{
            unreviewed_fixture.value().serialize()}));
    auto unreviewed_task = begin_task(unreviewed_session, unreviewed_objects);
    auto unreviewed_tag = carto::agent::SceneQuery::execute(unreviewed_task, {
        carto::agent::ConditionAssetsByTagQuery{"safe-place"}, 8U});
    REQUIRE(unreviewed_tag);
    REQUIRE(unreviewed_tag.value().rows.size() == 1U);
    const auto* unreviewed_fact = std::get_if<carto::agent::ConditionAssetTagFact>(
        &unreviewed_tag.value().rows.front());
    REQUIRE(unreviewed_fact != nullptr);
    REQUIRE(!unreviewed_fact->source_current);
    REQUIRE(!unreviewed_fact->source_reviewed);
    REQUIRE(unreviewed_fact->source_revision == "fixture-1");

    carto::agent::AgentWorkEnvelope small_semantic_result;
    small_semantic_result.max_query_result_bytes = 1U;
    auto bounded_tag_task = begin_task(
        current_session, current_objects, small_semantic_result);
    auto bounded_tag = carto::agent::SceneQuery::execute(bounded_tag_task, {
        carto::agent::ConditionAssetsByTagQuery{"safe-place"}, 8U});
    REQUIRE(bounded_tag);
    REQUIRE(bounded_tag.value().truncated);
    REQUIRE(bounded_tag.value().rows.empty());

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

    carto::application::ApplicationSession bounded_tag_session;
    const auto bounded_tag_objects = create_boxes(bounded_tag_session, {"A", "B", "C"});
    auto bounded_tag_fixture = carto::condition::make_condition_fixture();
    REQUIRE(bounded_tag_fixture);
    bounded_tag_fixture.value().project_revision =
        bounded_tag_session.snapshot().project_revision.next();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        bounded_tag_session, carto::application::ImportConditionAuthoringAction{
            bounded_tag_fixture.value().serialize()}));
    carto::agent::AgentWorkEnvelope bounded_tag_envelope;
    bounded_tag_envelope.max_semantic_records_examined = 3U;
    auto bounded_tag_budget_task = begin_task(
        bounded_tag_session, bounded_tag_objects, bounded_tag_envelope);
    auto bounded_tag_query = carto::agent::SceneQuery::execute(bounded_tag_budget_task, {
        carto::agent::ConditionAssetsByTagQuery{"safe-place"}, 8U});
    REQUIRE(bounded_tag_query);
    REQUIRE(bounded_tag_query.value().truncated);
    REQUIRE(bounded_tag_query.value().rows.empty());
    REQUIRE(bounded_tag_query.value().semantic_records_examined == 3U);
    REQUIRE(bounded_tag_budget_task.snapshot().query_budget.semantic_records_examined == 3U);

    carto::application::ApplicationSession bounded_familiarity_session;
    const auto bounded_familiarity_objects = create_boxes(
        bounded_familiarity_session, {"A", "B", "C"});
    auto bounded_familiarity_fixture = carto::condition::make_condition_fixture();
    REQUIRE(bounded_familiarity_fixture);
    bounded_familiarity_fixture.value().project_revision =
        bounded_familiarity_session.snapshot().project_revision.next();
    auto second_familiarity = bounded_familiarity_fixture.value().familiarity.front();
    second_familiarity.id = 2U;
    second_familiarity.semantic_id = "SAFEPLACE.MOTEL8.SIGN.ALTERNATE";
    bounded_familiarity_fixture.value().familiarity.push_back(second_familiarity);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        bounded_familiarity_session, carto::application::ImportConditionAuthoringAction{
            bounded_familiarity_fixture.value().serialize()}));
    carto::agent::AgentWorkEnvelope bounded_familiarity_envelope;
    bounded_familiarity_envelope.max_semantic_records_examined = 5U;
    auto bounded_familiarity_task = begin_task(
        bounded_familiarity_session, bounded_familiarity_objects,
        bounded_familiarity_envelope);
    auto bounded_familiarity_query = carto::agent::SceneQuery::execute(
        bounded_familiarity_task, {
            carto::agent::FamiliarityRecordsQuery{std::nullopt}, 8U});
    REQUIRE(bounded_familiarity_query);
    REQUIRE(bounded_familiarity_query.value().truncated);
    REQUIRE(bounded_familiarity_query.value().rows.size() == 1U);
    REQUIRE(bounded_familiarity_query.value().semantic_records_examined == 5U);
    REQUIRE(bounded_familiarity_task.snapshot().query_budget.semantic_records_examined == 5U);
}

void condition_inventory_is_scoped_factual_and_budgeted() {
    carto::application::ApplicationSession session;
    const auto objects = create_boxes(session, {"Exterior", "Sign", "Room"});
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    fixture.value().project_revision = session.snapshot().project_revision.next();
    fixture.value().region_kits.front().asset_ids = {1U, 2U};
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportConditionAuthoringAction{
            fixture.value().serialize()}));
    const auto imported_revision = session.snapshot().project_revision;
    auto task = begin_task(session, objects);

    auto inventory = carto::agent::SceneQuery::execute(task, {
        carto::agent::ConditionAssetInventoryQuery{std::nullopt}, 8U});
    REQUIRE(inventory);
    REQUIRE(inventory.value().rows.size() == 3U);
    REQUIRE(inventory.value().source_revision == imported_revision);
    REQUIRE(!inventory.value().truncated);
    const auto fact_for = [&](std::uint64_t asset_id)
            -> const carto::agent::ConditionAssetInventoryFact* {
        for (const auto& row : inventory.value().rows) {
            const auto* fact = std::get_if<carto::agent::ConditionAssetInventoryFact>(&row);
            if (fact != nullptr && fact->asset == asset_id) return fact;
        }
        return nullptr;
    };
    const auto* exterior = fact_for(1U);
    const auto* sign = fact_for(2U);
    const auto* room = fact_for(3U);
    REQUIRE(exterior != nullptr);
    REQUIRE(sign != nullptr);
    REQUIRE(room != nullptr);
    REQUIRE(exterior->mesh_validation ==
        carto::agent::ConditionTechnicalCheckStatus::passed);
    REQUIRE(exterior->uv_set_presence ==
        carto::agent::ConditionTechnicalCheckStatus::failed);
    REQUIRE(exterior->material_region_assignments ==
        carto::agent::ConditionTechnicalCheckStatus::failed);
    REQUIRE(exterior->technical_checks_complete);
    REQUIRE(exterior->authored_object == objects[0U]);
    REQUIRE(exterior->source_id == 1U);
    REQUIRE(exterior->source_revision == "fixture-1");
    REQUIRE(exterior->source_current);
    REQUIRE(exterior->source_reviewed);
    REQUIRE(exterior->asset_class == carto::condition::AssetClass::architecture);
    REQUIRE(exterior->fidelity == carto::condition::FidelityClass::familiarity_critical);
    REQUIRE(exterior->protected_features ==
        std::vector<std::string>({"entry-door", "silhouette"}));
    REQUIRE(exterior->safe_place_profiles == 1U);
    REQUIRE(exterior->familiarity_records == 0U);
    REQUIRE(exterior->view_conditions == 0U);
    REQUIRE(exterior->variant_candidates == 1U);
    REQUIRE(exterior->temporal_packages == 2U);
    REQUIRE(exterior->surface_states == 1U);
    REQUIRE(exterior->interaction_anchors == 1U);
    REQUIRE(exterior->collection_memberships == 1U);
    REQUIRE(exterior->prefab_memberships == 0U);
    REQUIRE(exterior->region_kit_memberships == 1U);
    REQUIRE(exterior->related_records_complete);
    const auto inventory_audit = carto::agent::audit_condition_inventory(inventory.value());
    REQUIRE(inventory_audit.technical_inspection_complete);
    REQUIRE(inventory_audit.mesh_valid_assets == 3U);
    REQUIRE(inventory_audit.uv_set_presence_passed_assets == 0U);
    REQUIRE(inventory_audit.material_region_assignments_passed_assets == 0U);
    REQUIRE(!carto::agent::ConditionEvidenceAudit::production_readiness_established);
    REQUIRE(sign->authored_object == objects[1U]);
    REQUIRE(sign->asset_class == carto::condition::AssetClass::prop);
    REQUIRE(sign->familiarity_records == 1U);
    REQUIRE(sign->view_conditions == 1U);
    REQUIRE(sign->variant_candidates == 1U);
    REQUIRE(sign->collection_memberships == 1U);
    REQUIRE(sign->region_kit_memberships == 1U);
    REQUIRE(sign->related_records_complete);
    REQUIRE(room->authored_object == objects[2U]);
    REQUIRE(room->interaction_anchors == 1U);
    REQUIRE(room->collection_memberships == 1U);
    REQUIRE(room->prefab_memberships == 1U);
    REQUIRE(room->region_kit_memberships == 1U);
    REQUIRE(room->related_records_complete);

    auto targeted = carto::agent::SceneQuery::execute(task, {
        carto::agent::ConditionAssetInventoryQuery{objects[1U]}, 8U});
    REQUIRE(targeted);
    REQUIRE(targeted.value().rows.size() == 1U);
    const auto* targeted_fact = std::get_if<carto::agent::ConditionAssetInventoryFact>(
        &targeted.value().rows.front());
    REQUIRE(targeted_fact != nullptr);
    REQUIRE(targeted_fact->asset == 2U);
    REQUIRE(session.snapshot().project_revision == imported_revision);

    auto outside_task = begin_task(session, {objects[2U]});
    auto outside = carto::agent::SceneQuery::execute(outside_task, {
        carto::agent::ConditionAssetInventoryQuery{objects[0U]}, 8U});
    REQUIRE(!outside);
    REQUIRE(outside.error().code == carto::core::ErrorCode::invalid_state);

    auto zero_object_task = begin_task(session, objects);
    auto zero_object = carto::agent::SceneQuery::execute(zero_object_task, {
        carto::agent::ConditionAssetInventoryQuery{carto::scene::ObjectId{}}, 8U});
    REQUIRE(!zero_object);
    REQUIRE(zero_object.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(zero_object_task.snapshot().query_budget.queries_used == 0U);

    carto::application::ApplicationSession stale_lineage_session;
    const auto stale_lineage_objects = create_boxes(
        stale_lineage_session, {"Exterior", "Sign", "Room"});
    auto stale_lineage_fixture = carto::condition::make_condition_fixture();
    REQUIRE(stale_lineage_fixture);
    stale_lineage_fixture.value().project_revision =
        stale_lineage_session.snapshot().project_revision.next();
    stale_lineage_fixture.value().assets.front().source_revision = "fixture-old";
    stale_lineage_fixture.value().assets[1U].source_digest =
        stale_lineage_fixture.value().sources.front().import_settings_digest;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        stale_lineage_session, carto::application::ImportConditionAuthoringAction{
            stale_lineage_fixture.value().serialize()}));
    auto stale_lineage_task = begin_task(stale_lineage_session, stale_lineage_objects);
    auto stale_lineage = carto::agent::SceneQuery::execute(stale_lineage_task, {
        carto::agent::ConditionAssetInventoryQuery{std::nullopt}, 8U});
    REQUIRE(stale_lineage);
    REQUIRE(stale_lineage.value().rows.size() == 3U);
    const auto stale_fact_for = [&](std::uint64_t asset_id)
            -> const carto::agent::ConditionAssetInventoryFact* {
        for (const auto& row : stale_lineage.value().rows) {
            const auto* fact = std::get_if<carto::agent::ConditionAssetInventoryFact>(&row);
            if (fact != nullptr && fact->asset == asset_id) return fact;
        }
        return nullptr;
    };
    REQUIRE(stale_fact_for(1U) != nullptr);
    REQUIRE(!stale_fact_for(1U)->source_current);
    REQUIRE(stale_fact_for(1U)->source_reviewed);
    REQUIRE(stale_fact_for(2U) != nullptr);
    REQUIRE(!stale_fact_for(2U)->source_current);
    REQUIRE(stale_fact_for(2U)->source_reviewed);
    REQUIRE(stale_fact_for(3U) != nullptr);
    REQUIRE(stale_fact_for(3U)->source_current);
    const auto stale_audit = carto::agent::audit_condition_inventory(stale_lineage.value());
    REQUIRE(!stale_audit.source_evidence_complete);
    REQUIRE(stale_audit.technical_inspection_complete);
    REQUIRE(!carto::agent::ConditionEvidenceAudit::production_readiness_established);

    carto::application::ApplicationSession stale_revision_session;
    const auto stale_revision_objects = create_boxes(
        stale_revision_session, {"Exterior", "Sign", "Room"});
    auto stale_revision_fixture = carto::condition::make_condition_fixture();
    REQUIRE(stale_revision_fixture);
    stale_revision_fixture.value().project_revision =
        stale_revision_session.snapshot().project_revision;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        stale_revision_session, carto::application::ImportConditionAuthoringAction{
            stale_revision_fixture.value().serialize()}));
    auto stale_revision_task = begin_task(stale_revision_session, stale_revision_objects);
    auto stale_revision = carto::agent::SceneQuery::execute(stale_revision_task, {
        carto::agent::ConditionAssetInventoryQuery{std::nullopt}, 8U});
    REQUIRE(!stale_revision);
    REQUIRE(stale_revision.error().code == carto::core::ErrorCode::stale_data);

    carto::application::ApplicationSession bounded_session;
    const auto bounded_objects = create_boxes(bounded_session, {"Exterior", "Sign", "Room"});
    auto bounded_fixture = carto::condition::make_condition_fixture();
    REQUIRE(bounded_fixture);
    bounded_fixture.value().project_revision = bounded_session.snapshot().project_revision.next();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        bounded_session, carto::application::ImportConditionAuthoringAction{
            bounded_fixture.value().serialize()}));
    carto::agent::AgentWorkEnvelope envelope;
    envelope.max_semantic_records_examined = 3U;
    auto bounded_task = begin_task(bounded_session, bounded_objects, envelope);
    auto bounded = carto::agent::SceneQuery::execute(bounded_task, {
        carto::agent::ConditionAssetInventoryQuery{std::nullopt}, 8U});
    REQUIRE(bounded);
    REQUIRE(bounded.value().truncated);
    REQUIRE(bounded.value().semantic_records_examined == 3U);
    REQUIRE(bounded_task.snapshot().query_budget.semantic_records_examined == 3U);
    REQUIRE(bounded.value().rows.empty());

    carto::agent::AgentWorkEnvelope related_envelope;
    related_envelope.max_semantic_records_examined = 4U;
    auto related_task = begin_task(bounded_session, bounded_objects, related_envelope);
    auto related_limited = carto::agent::SceneQuery::execute(related_task, {
        carto::agent::ConditionAssetInventoryQuery{std::nullopt}, 8U});
    REQUIRE(related_limited);
    REQUIRE(related_limited.value().truncated);
    REQUIRE(related_limited.value().semantic_records_examined == 4U);
    REQUIRE(related_task.snapshot().query_budget.semantic_records_examined == 4U);
    REQUIRE(related_limited.value().rows.size() == 1U);
    const auto* related_fact = std::get_if<carto::agent::ConditionAssetInventoryFact>(
        &related_limited.value().rows.front());
    REQUIRE(related_fact != nullptr);
    REQUIRE(related_fact->asset == 1U);
    REQUIRE(!related_fact->related_records_complete);
    REQUIRE(bounded_session.snapshot().project_revision == bounded_task.snapshot().base_revision);
}

void condition_inventory_reports_bounded_technical_checks_without_readiness() {
    using namespace carto;
    application::ApplicationSession assigned_session;
    const auto assigned_object = create_condition_mesh_target(assigned_session, true);
    import_condition_fixture_for(assigned_session, assigned_object);
    const auto captured_revision = assigned_session.snapshot().project_revision;
    auto assigned_task = begin_task(assigned_session, {assigned_object});
    auto assigned = agent::SceneQuery::execute(assigned_task, {
        agent::ConditionAssetInventoryQuery{assigned_object}, 8U});
    REQUIRE(assigned);
    REQUIRE(!assigned.value().truncated);
    REQUIRE(assigned.value().rows.size() == 1U);
    const auto* assigned_fact = std::get_if<agent::ConditionAssetInventoryFact>(
        &assigned.value().rows.front());
    REQUIRE(assigned_fact != nullptr);
    REQUIRE(assigned_fact->mesh_validation == agent::ConditionTechnicalCheckStatus::passed);
    REQUIRE(assigned_fact->mesh_vertices == 3U);
    REQUIRE(assigned_fact->mesh_edges == 3U);
    REQUIRE(assigned_fact->mesh_faces == 1U);
    REQUIRE(assigned_fact->uv_set_presence == agent::ConditionTechnicalCheckStatus::passed);
    REQUIRE(assigned_fact->uv_set_count == 2U);
    REQUIRE(assigned_fact->active_render_uv_set == 1U);
    REQUIRE(assigned_fact->lightmap_candidate_uv_sets == 1U);
    REQUIRE(assigned_fact->material_region_assignments ==
        agent::ConditionTechnicalCheckStatus::passed);
    REQUIRE(assigned_fact->material_region_count == 1U);
    REQUIRE(assigned_fact->resolved_material_region_count == 1U);
    REQUIRE(assigned_fact->technical_checks_complete);
    const auto assigned_audit = agent::audit_condition_inventory(assigned.value());
    REQUIRE(assigned_audit.technical_inspection_complete);
    REQUIRE(assigned_audit.mesh_valid_assets == 1U);
    REQUIRE(assigned_audit.uv_set_presence_passed_assets == 1U);
    REQUIRE(assigned_audit.material_region_assignments_passed_assets == 1U);
    REQUIRE(!agent::ConditionEvidenceAudit::production_readiness_established);
    REQUIRE(assigned_session.snapshot().project_revision == captured_revision);

    application::ApplicationSession unassigned_session;
    const auto unassigned_object = create_condition_mesh_target(unassigned_session, false);
    import_condition_fixture_for(unassigned_session, unassigned_object);
    auto unassigned_task = begin_task(unassigned_session, {unassigned_object});
    auto unassigned = agent::SceneQuery::execute(unassigned_task, {
        agent::ConditionAssetInventoryQuery{unassigned_object}, 8U});
    REQUIRE(unassigned);
    REQUIRE(!unassigned.value().truncated);
    const auto* unassigned_fact = std::get_if<agent::ConditionAssetInventoryFact>(
        &unassigned.value().rows.front());
    REQUIRE(unassigned_fact != nullptr);
    REQUIRE(unassigned_fact->mesh_validation == agent::ConditionTechnicalCheckStatus::passed);
    REQUIRE(unassigned_fact->uv_set_presence == agent::ConditionTechnicalCheckStatus::passed);
    REQUIRE(unassigned_fact->material_region_assignments ==
        agent::ConditionTechnicalCheckStatus::failed);
    REQUIRE(unassigned_fact->material_region_count == 1U);
    REQUIRE(unassigned_fact->resolved_material_region_count == 0U);
    REQUIRE(unassigned_fact->technical_checks_complete);

    application::ApplicationSession missing_candidate_session;
    const auto missing_candidate_object = create_condition_mesh_target(
        missing_candidate_session, true, false);
    import_condition_fixture_for(missing_candidate_session, missing_candidate_object);
    auto missing_candidate_task = begin_task(
        missing_candidate_session, {missing_candidate_object});
    auto missing_candidate = agent::SceneQuery::execute(missing_candidate_task, {
        agent::ConditionAssetInventoryQuery{missing_candidate_object}, 8U});
    REQUIRE(missing_candidate);
    const auto* missing_candidate_fact = std::get_if<agent::ConditionAssetInventoryFact>(
        &missing_candidate.value().rows.front());
    REQUIRE(missing_candidate_fact != nullptr);
    REQUIRE(missing_candidate_fact->uv_set_count == 1U);
    REQUIRE(missing_candidate_fact->active_render_uv_set == 1U);
    REQUIRE(missing_candidate_fact->lightmap_candidate_uv_sets == 0U);
    REQUIRE(missing_candidate_fact->uv_set_presence ==
        agent::ConditionTechnicalCheckStatus::failed);

    application::ApplicationSession bounded_session;
    const auto bounded_object = create_condition_mesh_target(bounded_session, true);
    import_condition_fixture_for(bounded_session, bounded_object);
    agent::AgentWorkEnvelope bounded_envelope;
    bounded_envelope.max_topology_elements_examined = 1U;
    auto bounded_task = begin_task(bounded_session, {bounded_object}, bounded_envelope);
    auto bounded = agent::SceneQuery::execute(bounded_task, {
        agent::ConditionAssetInventoryQuery{bounded_object}, 8U});
    REQUIRE(bounded);
    REQUIRE(bounded.value().truncated);
    REQUIRE(bounded.value().rows.size() == 1U);
    const auto* bounded_fact = std::get_if<agent::ConditionAssetInventoryFact>(
        &bounded.value().rows.front());
    REQUIRE(bounded_fact != nullptr);
    REQUIRE(!bounded_fact->technical_checks_complete);
    REQUIRE(bounded_fact->mesh_validation == agent::ConditionTechnicalCheckStatus::incomplete);
    REQUIRE(bounded_fact->uv_set_presence == agent::ConditionTechnicalCheckStatus::incomplete);
    REQUIRE(bounded_fact->material_region_assignments ==
        agent::ConditionTechnicalCheckStatus::incomplete);
    const auto bounded_audit = agent::audit_condition_inventory(bounded.value());
    REQUIRE(!bounded_audit.technical_inspection_complete);
    REQUIRE(!agent::ConditionEvidenceAudit::production_readiness_established);
    REQUIRE(bounded_session.snapshot().project_revision == bounded_task.snapshot().base_revision);
}

void live_condition_inventory_is_revision_bound_and_read_only() {
    carto::application::ApplicationSession session;
    const auto objects = create_boxes(session, {"Selected", "Unselected"});
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    fixture.value().project_revision = session.snapshot().project_revision.next();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportConditionAuthoringAction{
            fixture.value().serialize()}));

    const auto captured_revision = session.snapshot().project_revision;
    carto::agent::SceneQueryBudget budget;
    auto inventory = carto::agent::SceneQuery::execute(session, {
        carto::agent::ConditionAssetInventoryQuery{std::nullopt}, 8U},
        budget, {objects.front()}, captured_revision);
    REQUIRE(inventory);
    REQUIRE(inventory.value().source_revision == captured_revision);
    REQUIRE(inventory.value().rows.size() == 1U);
    REQUIRE(budget.queries_used == 1U);
    REQUIRE(session.snapshot().project_revision == captured_revision);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{
            "Concurrent edit", {1.0, 1.0, 1.0}}));
    const auto changed_revision = session.snapshot().project_revision;
    auto stale = carto::agent::SceneQuery::execute(session, {
        carto::agent::ConditionAssetInventoryQuery{std::nullopt}, 8U},
        budget, {objects.front()}, captured_revision);
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(budget.queries_used == 1U);
    REQUIRE(session.snapshot().project_revision == changed_revision);
}

} // namespace

int main() {
    try {
        identity_queries_are_revision_bound_and_scope_limited();
        spatial_orientation_and_boundary_queries_return_measured_facts();
        query_and_scan_budgets_truncate_or_fail_closed();
        condition_semantics_require_current_explicit_source_data();
        condition_inventory_is_scoped_factual_and_budgeted();
        condition_inventory_reports_bounded_technical_checks_without_readiness();
        live_condition_inventory_is_revision_bound_and_read_only();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
