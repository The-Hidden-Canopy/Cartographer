#include <carto/editor/observation.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/spatial_field/scalar_field.hpp>
#include <carto/terrain/terrain_tile.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
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

carto::geometry::CompiledMesh vertical_triangle() {
    carto::geometry::EditableMesh mesh;
    const auto first = mesh.add_vertex({0.0, -1.0, -1.0});
    const auto second = mesh.add_vertex({0.0, 1.0, -1.0});
    const auto third = mesh.add_vertex({0.0, 0.0, 1.0});
    REQUIRE(first && second && third);
    REQUIRE(mesh.add_face({first.value(), second.value(), third.value()}));
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    return compiled.value();
}

void add_occluder(
    carto::render::RenderScene& scene,
    carto::scene::ObjectId object,
    std::uint64_t asset,
    const carto::geometry::CompiledMesh& source,
    double x) {
    auto mesh = std::make_shared<const carto::geometry::CompiledMesh>(source);
    auto transform = carto::core::Transform::identity();
    transform.translation.x = x;
    REQUIRE(scene.upsert({object, std::move(mesh), transform, source.source_revision, asset}));
}

carto::editor::PointSightlineQuery query() {
    return {{-5.0, 0.0, 0.0}, {5.0, 0.0, 0.0},
        carto::core::Revision{14U}, std::nullopt, std::nullopt};
}

carto::editor::RouteVisibilityQuery route_query() {
    return {{{-5.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}, {},
        {5.0, 0.0, 0.0}, carto::core::Revision{14U}, std::nullopt};
}

carto::editor::LandmarkViewshedQuery viewshed_query() {
    return {{-5.0, 0.0, 0.0}, std::nullopt, 2U, 2U,
        {{5.0, 0.0, -0.2}, {5.0, 0.0, 0.2},
         {5.0, 3.0, -0.2}, {5.0, 3.0, 0.2}},
        carto::core::Revision{14U}};
}

void point_sightline_reports_nearest_blocker_and_is_deterministic() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene scene;
    add_occluder(scene, carto::scene::ObjectId{20U}, 120U, mesh, 2.0);
    add_occluder(scene, carto::scene::ObjectId{10U}, 110U, mesh, -1.0);
    const auto before = scene.instances();

    const auto first = carto::editor::evaluate_point_sightline(scene, query());
    const auto second = carto::editor::evaluate_point_sightline(scene, query());
    REQUIRE(first && second);
    REQUIRE(!first.value().visible);
    REQUIRE(first.value().blocker_object == carto::scene::ObjectId{10U});
    REQUIRE(first.value().blocker_mesh_asset == 110U);
    REQUIRE(first.value().blocker_face.has_value());
    REQUIRE(std::abs(first.value().blocker_distance - 4.0) < 1e-12);
    REQUIRE(first.value().triangles_tested == 2U);
    REQUIRE(first.value().validate(query(), {}, carto::core::Revision{14U}));
    REQUIRE(first.value().blocker_object == second.value().blocker_object);
    REQUIRE(first.value().blocker_face == second.value().blocker_face);
    REQUIRE(scene.instances().size() == before.size());
    const auto* retained = scene.find(carto::scene::ObjectId{10U});
    REQUIRE(retained != nullptr);
    REQUIRE(retained->mesh->source_revision == before.front().mesh->source_revision);
    REQUIRE(retained->mesh->positions.size() == before.front().mesh->positions.size());
    REQUIRE(retained->mesh->positions.front().x == before.front().mesh->positions.front().x);
    REQUIRE(retained->world_transform.translation.x == before.front().world_transform.translation.x);

    carto::render::RenderScene tied_scene;
    add_occluder(tied_scene, carto::scene::ObjectId{20U}, 220U, mesh, 0.0);
    add_occluder(tied_scene, carto::scene::ObjectId{10U}, 110U, mesh, 0.0);
    const auto tied = carto::editor::evaluate_point_sightline(tied_scene, query());
    REQUIRE(tied);
    REQUIRE(tied.value().blocker_object == carto::scene::ObjectId{10U});
}

void point_sightline_respects_endpoint_exclusions_and_world_transforms() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene scene;
    add_occluder(scene, carto::scene::ObjectId{8U}, 80U, mesh, 0.0);
    auto request = query();
    request.target_object = carto::scene::ObjectId{8U};
    const auto excluded = carto::editor::evaluate_point_sightline(scene, request);
    REQUIRE(excluded && excluded.value().visible);
    REQUIRE(!excluded.value().blocker_object.has_value());

    request.target_object.reset();
    auto transformed = carto::core::Transform::identity();
    transformed.translation.x = 1.0;
    transformed.scale.y = 2.0;
    carto::render::RenderScene transformed_scene;
    auto shared_mesh = std::make_shared<const carto::geometry::CompiledMesh>(mesh);
    REQUIRE(transformed_scene.upsert({carto::scene::ObjectId{9U}, shared_mesh,
        transformed, mesh.source_revision, 90U}));
    const auto blocked = carto::editor::evaluate_point_sightline(transformed_scene, request);
    REQUIRE(blocked && !blocked.value().visible);
    REQUIRE(blocked.value().blocker_object == carto::scene::ObjectId{9U});
    REQUIRE(std::abs(blocked.value().blocker_distance - 6.0) < 1e-12);
}

void point_sightline_fails_closed_on_stale_or_unbound_occluders() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene stale_scene;
    auto mutable_mesh = std::make_shared<carto::geometry::CompiledMesh>(mesh);
    REQUIRE(stale_scene.upsert({carto::scene::ObjectId{3U}, mutable_mesh,
        carto::core::Transform::identity(), mesh.source_revision, 30U}));
    mutable_mesh->source_revision = mesh.source_revision.next();
    const auto stale = carto::editor::evaluate_point_sightline(stale_scene, query());
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);

    carto::render::RenderScene unbound_scene;
    REQUIRE(unbound_scene.upsert({carto::scene::ObjectId{4U},
        std::make_shared<const carto::geometry::CompiledMesh>(mesh),
        carto::core::Transform::identity(), mesh.source_revision, std::nullopt}));
    const auto unbound = carto::editor::evaluate_point_sightline(unbound_scene, query());
    REQUIRE(!unbound);
    REQUIRE(unbound.error().code == carto::core::ErrorCode::validation_failed);

    const auto stale_result = carto::editor::evaluate_point_sightline(
        carto::render::RenderScene{}, query());
    REQUIRE(stale_result);
    REQUIRE(!stale_result.value().validate(query(), {}, carto::core::Revision{15U}));
}

void point_sightline_rejects_invalid_inputs_and_budget_overflow() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene scene;
    add_occluder(scene, carto::scene::ObjectId{5U}, 50U, mesh, 0.0);

    auto non_finite = query();
    non_finite.target.x = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!carto::editor::evaluate_point_sightline(scene, non_finite));
    auto degenerate = query();
    degenerate.target = degenerate.observer;
    REQUIRE(!carto::editor::evaluate_point_sightline(scene, degenerate));

    carto::editor::PointSightlineOptions too_small;
    too_small.max_triangles = 0U;
    REQUIRE(!too_small.validate());
    too_small.max_triangles = 0U + 1U;
    const auto one_triangle_budget = carto::editor::evaluate_point_sightline(
        scene, query(), too_small);
    REQUIRE(one_triangle_budget);

    carto::render::RenderScene two_triangles;
    auto quad = mesh;
    quad.triangle_faces.push_back(mesh.triangle_faces.front());
    quad.indices.insert(quad.indices.end(), mesh.indices.begin(), mesh.indices.end());
    quad.triangle_edges.push_back(mesh.triangle_edges.front());
    auto quad_mesh = std::make_shared<const carto::geometry::CompiledMesh>(quad);
    REQUIRE(two_triangles.upsert({carto::scene::ObjectId{6U}, quad_mesh,
        carto::core::Transform::identity(), quad.source_revision, 60U}));
    const auto over_budget = carto::editor::evaluate_point_sightline(
        two_triangles, query(), too_small);
    REQUIRE(!over_budget);
    REQUIRE(over_budget.error().code == carto::core::ErrorCode::validation_failed);

    carto::editor::PointSightlineOptions too_many_occluders;
    too_many_occluders.max_occluders = 1U;
    carto::render::RenderScene two_occluders;
    add_occluder(two_occluders, carto::scene::ObjectId{7U}, 70U, mesh, 0.0);
    add_occluder(two_occluders, carto::scene::ObjectId{8U}, 80U, mesh, 1.0);
    REQUIRE(!carto::editor::evaluate_point_sightline(
        two_occluders, query(), too_many_occluders));
}

void route_visibility_reports_revision_bound_landmark_samples() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene scene;
    add_occluder(scene, carto::scene::ObjectId{7U}, 70U, mesh, 0.0);
    const auto query_value = route_query();

    const auto result = carto::editor::evaluate_route_visibility(scene, query_value);
    REQUIRE(result);
    REQUIRE(result.value().samples.size() == 3U);
    REQUIRE(result.value().samples[0U].visible == false);
    REQUIRE(result.value().samples[1U].visible == false);
    REQUIRE(result.value().samples[2U].visible);
    REQUIRE(result.value().cumulative_route_distance.size() == 3U);
    REQUIRE(result.value().cumulative_route_distance[0U] == 0.0);
    REQUIRE(result.value().cumulative_route_distance[1U] == 4.0);
    REQUIRE(result.value().cumulative_route_distance[2U] == 6.0);
    REQUIRE(result.value().route_length == 6.0);
    REQUIRE(result.value().triangles_tested == 3U);
    REQUIRE(result.value().validate(query_value, {}, carto::core::Revision{14U}));

    auto invalid_receipt = result.value();
    invalid_receipt.cumulative_route_distance[1U] += 0.25;
    REQUIRE(!invalid_receipt.validate(query_value, {}, carto::core::Revision{14U}));
    REQUIRE(!result.value().validate(
        query_value, {}, carto::core::Revision{15U}));
}

void route_visibility_honors_per_station_exclusions_and_fails_closed() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene scene;
    add_occluder(scene, carto::scene::ObjectId{7U}, 70U, mesh, 0.0);

    auto query_value = route_query();
    query_value.observer_objects = {
        carto::scene::ObjectId{7U}, std::nullopt, std::nullopt};
    const auto selectively_excluded =
        carto::editor::evaluate_route_visibility(scene, query_value);
    REQUIRE(selectively_excluded);
    REQUIRE(selectively_excluded.value().samples[0U].visible);
    REQUIRE(!selectively_excluded.value().samples[1U].visible);

    query_value.observer_objects.clear();
    query_value.target_object = carto::scene::ObjectId{7U};
    const auto target_excluded =
        carto::editor::evaluate_route_visibility(scene, query_value);
    REQUIRE(target_excluded);
    for (const auto& sample : target_excluded.value().samples) REQUIRE(sample.visible);

    query_value = route_query();
    query_value.observer_objects = {std::nullopt};
    REQUIRE(!carto::editor::evaluate_route_visibility(scene, query_value));
    query_value = route_query();
    query_value.observer_positions[1U] = query_value.observer_positions[0U];
    REQUIRE(!carto::editor::evaluate_route_visibility(scene, query_value));

    auto too_few_samples = carto::editor::RouteVisibilityOptions{};
    too_few_samples.max_samples = 2U;
    REQUIRE(!carto::editor::evaluate_route_visibility(
        scene, route_query(), too_few_samples));

    auto aggregate_limit = carto::editor::RouteVisibilityOptions{};
    aggregate_limit.max_total_triangles = 2U;
    const auto over_budget = carto::editor::evaluate_route_visibility(
        scene, route_query(), aggregate_limit);
    REQUIRE(!over_budget);
    REQUIRE(over_budget.error().code == carto::core::ErrorCode::validation_failed);
}

void curve_route_visibility_samples_and_binds_the_exact_authored_source() {
    using namespace carto;
    geometry::CurvePath curve;
    curve.source_identity = "carto.test.curved-route";
    curve.source_revision = core::Revision{7U};
    curve.segments = {
        {{-4.0, 0.0, -2.0}, {-2.0, 0.0, 4.0}, {0.0, 0.0, 4.0},
         {2.0, 0.0, -2.0}},
        {{2.0, 0.0, -2.0}, {4.0, 0.0, -6.0}, {6.0, 0.0, -6.0},
         {8.0, 0.0, -2.0}},
    };
    constexpr core::Revision project_revision{31U};
    const auto query_value = editor::build_curve_route_visibility_query(
        curve, {10.0, 0.0, 0.0}, project_revision, 4U,
        scene::ObjectId{90U}, scene::ObjectId{91U});
    REQUIRE(query_value);
    REQUIRE(query_value.value().curve_identity == curve.source_identity);
    REQUIRE(query_value.value().curve_revision == curve.source_revision);
    REQUIRE(!query_value.value().curve_digest.is_zero());
    REQUIRE(query_value.value().route.observer_positions.size() == 9U);
    REQUIRE(query_value.value().route.observer_objects.size() == 9U);
    REQUIRE(query_value.value().route.observer_objects.front() == scene::ObjectId{90U});
    REQUIRE(query_value.value().route.target_object == scene::ObjectId{91U});
    REQUIRE(query_value.value().route.observer_positions[2U].x == -1.0);
    REQUIRE(query_value.value().route.observer_positions[2U].z == 2.5);
    REQUIRE(query_value.value().validate(curve, project_revision));

    render::RenderScene scene_snapshot;
    const auto first = editor::evaluate_curve_route_visibility(
        scene_snapshot, curve, query_value.value(), project_revision);
    const auto repeated = editor::evaluate_curve_route_visibility(
        scene_snapshot, curve, query_value.value(), project_revision);
    REQUIRE(first && repeated);
    REQUIRE(first.value().visibility.samples.size() == 9U);
    REQUIRE(first.value().visibility.route_length > 0.0);
    REQUIRE(first.value().validate(curve, project_revision));
    REQUIRE(repeated.value().visibility.cumulative_route_distance ==
        first.value().visibility.cumulative_route_distance);
    for (std::size_t index = 0U; index < first.value().visibility.samples.size(); ++index) {
        REQUIRE(repeated.value().visibility.samples[index].visible ==
            first.value().visibility.samples[index].visible);
        REQUIRE(repeated.value().visibility.samples[index].blocker_object ==
            first.value().visibility.samples[index].blocker_object);
    }
    REQUIRE(scene_snapshot.instances().empty());

    REQUIRE(!query_value.value().validate(curve, project_revision.next()));
    auto stale_identity = query_value.value();
    stale_identity.curve_identity = "carto.test.other-route";
    REQUIRE(!stale_identity.validate(curve, project_revision));
    auto altered_digest = query_value.value();
    altered_digest.curve_digest = {};
    REQUIRE(!altered_digest.validate(curve, project_revision));
    auto altered_station = query_value.value();
    altered_station.route.observer_positions[2U].z += 0.01;
    REQUIRE(!altered_station.validate(curve, project_revision));
    auto altered_exclusion = query_value.value();
    altered_exclusion.route.observer_objects[0U] = scene::ObjectId{92U};
    REQUIRE(!altered_exclusion.validate(curve, project_revision));
    auto altered_target = query_value.value();
    altered_target.route.target_object = scene::ObjectId{92U};
    REQUIRE(!altered_target.validate(curve, project_revision));

    auto revised_curve = curve;
    revised_curve.source_revision = curve.source_revision.next();
    REQUIRE(!query_value.value().validate(revised_curve, project_revision));
    auto edited_curve = curve;
    edited_curve.segments.front().p1.z += 0.25;
    REQUIRE(!query_value.value().validate(edited_curve, project_revision));

    auto too_few_samples = editor::RouteVisibilityOptions{};
    too_few_samples.max_samples = 8U;
    REQUIRE(!editor::build_curve_route_visibility_query(
        curve, {10.0, 0.0, 0.0}, project_revision, 4U,
        std::nullopt, std::nullopt, too_few_samples));
    REQUIRE(!editor::build_curve_route_visibility_query(
        curve, {10.0, 0.0, 0.0}, project_revision, 0U));
    REQUIRE(!editor::build_curve_route_visibility_query(
        curve, {10.0, 0.0, 0.0}, project_revision,
        geometry::CurvePath::kMaxSamplesPerSegment + 1U));
    REQUIRE(!editor::build_curve_route_visibility_query(
        curve, {std::numeric_limits<double>::infinity(), 0.0, 0.0},
        project_revision, 4U));
}

void landmark_viewshed_preserves_row_major_visibility_and_receipts() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene scene;
    add_occluder(scene, carto::scene::ObjectId{7U}, 70U, mesh, 0.0);
    const auto query_value = viewshed_query();

    const auto result = carto::editor::evaluate_landmark_viewshed(scene, query_value);
    REQUIRE(result);
    REQUIRE(result.value().width == 2U && result.value().height == 2U);
    REQUIRE(result.value().cells.size() == 4U);
    REQUIRE(!result.value().cells[0U].visible);
    REQUIRE(!result.value().cells[1U].visible);
    REQUIRE(result.value().cells[2U].visible);
    REQUIRE(result.value().cells[3U].visible);
    REQUIRE(result.value().visible_cells == 2U);
    REQUIRE(result.value().triangles_tested == 4U);
    REQUIRE(result.value().validate(query_value, {}, carto::core::Revision{14U}));

    auto tampered_receipt = result.value();
    tampered_receipt.visible_cells = 4U;
    REQUIRE(!tampered_receipt.validate(query_value, {}, carto::core::Revision{14U}));
    REQUIRE(!result.value().validate(
        query_value, {}, carto::core::Revision{15U}));
}

void landmark_viewshed_rejects_bad_grids_and_aggregate_overflow() {
    const auto mesh = vertical_triangle();
    carto::render::RenderScene scene;
    add_occluder(scene, carto::scene::ObjectId{7U}, 70U, mesh, 0.0);

    auto malformed = viewshed_query();
    malformed.width = 3U;
    REQUIRE(!carto::editor::evaluate_landmark_viewshed(scene, malformed));
    malformed = viewshed_query();
    malformed.target_positions[2U].z = std::numeric_limits<double>::infinity();
    REQUIRE(!carto::editor::evaluate_landmark_viewshed(scene, malformed));

    auto too_small = carto::editor::LandmarkViewshedOptions{};
    too_small.max_cells = 3U;
    REQUIRE(!too_small.validate());
    too_small.max_cells = 4U;
    too_small.max_total_triangles = 3U;
    const auto over_budget = carto::editor::evaluate_landmark_viewshed(
        scene, viewshed_query(), too_small);
    REQUIRE(!over_budget);
    REQUIRE(over_budget.error().code == carto::core::ErrorCode::validation_failed);

    auto excluded_query = viewshed_query();
    excluded_query.observer_object = carto::scene::ObjectId{7U};
    const auto excluded = carto::editor::evaluate_landmark_viewshed(scene, excluded_query);
    REQUIRE(excluded);
    REQUIRE(excluded.value().visible_cells == 4U);
}

void terrain_viewshed_grid_samples_the_bounded_triangulated_product() {
    using namespace carto;
    auto field = spatial_field::ScalarField2D::create(
        terrain::terrain_tile_identity({0, 0}),
        spatial_field::FieldSemantic::terrain_elevation,
        {-8.0, 4.0}, 2.0, 90U, 70U, -100.0, 100.0, 0.0);
    REQUIRE(field);
    const auto product = terrain::build_terrain_tile(field.value(), {0, 0});
    REQUIRE(product);
    const auto product_digest = product.value().source_digest();
    const auto grid = editor::build_terrain_landmark_viewshed_grid(
        product.value(), {0.0, 10.0, 0.0}, core::Revision{22U},
        scene::ObjectId{77U}, 20U);
    REQUIRE(grid);
    REQUIRE(grid.value().terrain_identity == product.value().source_identity());
    REQUIRE(grid.value().terrain_revision == product.value().source_revision());
    REQUIRE(grid.value().terrain_digest == product_digest);
    REQUIRE(grid.value().terrain_lod_step == product.value().lod_step());
    REQUIRE(grid.value().query.width == 20U);
    REQUIRE(grid.value().query.height == 20U);
    REQUIRE(grid.value().query.target_positions.size() == 400U);
    REQUIRE(grid.value().query.source_revision == core::Revision{22U});
    REQUIRE(grid.value().query.observer_object == scene::ObjectId{77U});
    const auto same_position = [](core::Vec3d left, core::Vec3d right) {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    };
    REQUIRE(same_position(grid.value().query.target_positions.front(),
        product.value().vertices().front().position));
    REQUIRE(same_position(grid.value().query.target_positions.back(),
        product.value().vertices().back().position));
    REQUIRE(grid.value().validate(product.value(), core::Revision{22U}));

    render::RenderScene empty_scene;
    const auto viewshed = editor::evaluate_landmark_viewshed(
        empty_scene, grid.value().query);
    REQUIRE(viewshed);
    REQUIRE(viewshed.value().cells.size() == 400U);
    REQUIRE(viewshed.value().visible_cells == 400U);
    REQUIRE(viewshed.value().validate(
        grid.value().query, editor::LandmarkViewshedOptions{}, core::Revision{22U}));

    REQUIRE(!grid.value().validate(product.value(), core::Revision{23U}));
    auto altered_grid = grid.value();
    altered_grid.query.target_positions.front().x += 0.125;
    const auto altered = altered_grid.validate(product.value(), core::Revision{22U});
    REQUIRE(!altered);
    REQUIRE(altered.error().code == core::ErrorCode::validation_failed);

    const auto oversized = editor::build_terrain_landmark_viewshed_grid(
        product.value(), {0.0, 10.0, 0.0}, core::Revision{22U},
        std::nullopt, editor::TerrainLandmarkViewshedGrid::kMaxAxis + 1U);
    REQUIRE(!oversized);
    const auto invalid_observer = editor::build_terrain_landmark_viewshed_grid(
        product.value(), {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
        core::Revision{22U});
    REQUIRE(!invalid_observer);

    auto changed_field = field.value();
    const auto edit = changed_field.paint(spatial_field::FieldBrush{
        core::Revision{1U}, {10.0, 20.0}, 3.0, 5.0, 1.0,
        spatial_field::BrushMode::replace});
    REQUIRE(edit);
    REQUIRE(edit.value().changed);
    const auto changed_product = terrain::build_terrain_tile(changed_field, {0, 0});
    REQUIRE(changed_product);
    const auto stale_source = grid.value().validate(
        changed_product.value(), core::Revision{22U});
    REQUIRE(!stale_source);
    REQUIRE(stale_source.error().code == core::ErrorCode::stale_data);
    REQUIRE(product.value().source_digest() == product_digest);

    const auto alternate_lod_product = terrain::build_terrain_tile(
        field.value(), {0, 0}, 2U);
    REQUIRE(alternate_lod_product);
    const auto stale_lod = grid.value().validate(
        alternate_lod_product.value(), core::Revision{22U});
    REQUIRE(!stale_lod);
    REQUIRE(stale_lod.error().code == core::ErrorCode::stale_data);

    auto peak_field = spatial_field::ScalarField2D::create(
        terrain::terrain_tile_identity({1, 1}),
        spatial_field::FieldSemantic::terrain_elevation,
        {10.0, -3.0}, 2.0, 4U, 4U, -20.0, 20.0, 0.0);
    REQUIRE(peak_field);
    const auto peak_edit = peak_field.value().paint(spatial_field::FieldBrush{
        core::Revision{1U}, {14.0, 1.0}, 1.0, 8.0, 1.0,
        spatial_field::BrushMode::replace});
    REQUIRE(peak_edit && peak_edit.value().changed);
    const auto coarse_peak_product = terrain::build_terrain_tile(
        peak_field.value(), {1, 1}, 2U);
    REQUIRE(coarse_peak_product);
    const auto interpolated_grid = editor::build_terrain_landmark_viewshed_grid(
        coarse_peak_product.value(), {10.0, 20.0, -3.0},
        core::Revision{23U}, std::nullopt, 3U);
    REQUIRE(interpolated_grid);
    REQUIRE(interpolated_grid.value().query.width == 3U);
    REQUIRE(interpolated_grid.value().query.height == 3U);
    const auto& interior_target = interpolated_grid.value().query.target_positions[4U];
    REQUIRE(interior_target.x == 13.0);
    REQUIRE(interior_target.z == 0.0);
    // The sampled point is halfway through a coarse mesh cell and lies in the
    // authored b-c diagonal. Its height must match the product triangles,
    // rather than bilinear interpolation over the four corners.
    REQUIRE(interior_target.y == 4.0);
    REQUIRE(interpolated_grid.value().terrain_lod_step == 2U);
    REQUIRE(interpolated_grid.value().validate(
        coarse_peak_product.value(), core::Revision{23U}));
}

void authoring_observation_projects_semantics_cone_and_visibility() {
    using namespace carto;
    const auto mesh = vertical_triangle();
    render::RenderScene scene;
    add_occluder(scene, scene::ObjectId{7U}, 70U, mesh, 0.0);

    editor::AuthoringObservationQuery query_value;
    query_value.observer = {-5.0, 0.0, 0.0};
    query_value.forward = {4.0, 0.0, 0.0};
    query_value.up = {0.0, 2.0, 0.0};
    query_value.horizontal_fov_radians = 1.0471975511965976;
    query_value.vertical_fov_radians = 0.6981317007977318;
    query_value.source_revision = core::Revision{14U};
    query_value.targets = {
        {101U, scene::ObjectId{101U}, {5.0, 0.0, 0.0},
            editor::ObservationTargetKind::landmark},
        {102U, scene::ObjectId{102U}, {5.0, 4.0, 0.0},
            editor::ObservationTargetKind::sign},
        {103U, scene::ObjectId{103U}, {500.0, 0.0, 0.0},
            editor::ObservationTargetKind::generic_object},
        {104U, scene::ObjectId{104U}, {5.0, 0.0, 0.0},
            editor::ObservationTargetKind::clutter},
        {105U, scene::ObjectId{105U}, {5.0, 0.0, 3.0},
            editor::ObservationTargetKind::interaction},
    };
    editor::AuthoringObservationOptions options;
    options.max_distance = 100.0;
    options.included_target_kinds = editor::kAllObservationTargetKinds &
        ~editor::observation_target_kind_bit(editor::ObservationTargetKind::clutter);

    const auto first = editor::evaluate_authoring_observation(scene, query_value, options);
    const auto repeated = editor::evaluate_authoring_observation(scene, query_value, options);
    REQUIRE(first && repeated);
    const auto& report = first.value();
    REQUIRE(report.targets.size() == 5U);
    REQUIRE(report.targets[0U].disposition == editor::ObservationDisposition::occluded);
    REQUIRE(report.targets[0U].sightline.has_value());
    REQUIRE(report.targets[1U].disposition ==
        editor::ObservationDisposition::outside_view_cone);
    REQUIRE(report.targets[2U].disposition == editor::ObservationDisposition::outside_range);
    REQUIRE(report.targets[3U].disposition == editor::ObservationDisposition::filtered_by_kind);
    REQUIRE(report.targets[4U].disposition == editor::ObservationDisposition::visible);
    REQUIRE(report.targets[4U].sightline.has_value());
    REQUIRE(report.visible_targets == 1U);
    REQUIRE(report.occluded_targets == 1U);
    REQUIRE(report.filtered_targets == 1U);
    REQUIRE(report.outside_range_targets == 1U);
    REQUIRE(report.outside_view_cone_targets == 1U);
    REQUIRE(report.triangles_tested == 2U);
    REQUIRE(report.forward.x == 1.0 && report.right.z == 1.0 && report.up.y == 1.0);
    REQUIRE(report.validate(query_value, options, core::Revision{14U}));
    REQUIRE(repeated.value().targets[0U].disposition == report.targets[0U].disposition);
    REQUIRE(repeated.value().targets[4U].disposition == report.targets[4U].disposition);

    auto tampered = report;
    tampered.targets[0U].target_identity = 999U;
    REQUIRE(!tampered.validate(query_value, options, core::Revision{14U}));
    REQUIRE(!report.validate(query_value, options, core::Revision{15U}));
}

void authoring_observation_rejects_invalid_context_and_aggregate_overflow() {
    using namespace carto;
    const auto mesh = vertical_triangle();
    render::RenderScene scene;
    add_occluder(scene, scene::ObjectId{7U}, 70U, mesh, 0.0);

    editor::AuthoringObservationQuery query_value;
    query_value.observer = {-5.0, 0.0, 0.0};
    query_value.forward = {1.0, 0.0, 0.0};
    query_value.up = {0.0, 1.0, 0.0};
    query_value.source_revision = core::Revision{14U};
    query_value.targets = {{101U, scene::ObjectId{101U}, {5.0, 0.0, 0.0},
        editor::ObservationTargetKind::landmark}};

    auto malformed = query_value;
    malformed.targets.front().identity = 0U;
    REQUIRE(!editor::evaluate_authoring_observation(scene, malformed));
    malformed = query_value;
    malformed.up = malformed.forward;
    REQUIRE(!editor::evaluate_authoring_observation(scene, malformed));
    malformed = query_value;
    malformed.targets.push_back(malformed.targets.front());
    REQUIRE(!editor::evaluate_authoring_observation(scene, malformed));

    auto too_small = editor::AuthoringObservationOptions{};
    too_small.max_total_triangles = 1U;
    auto two_targets = query_value;
    two_targets.targets.push_back({102U, scene::ObjectId{102U}, {5.0, 0.0, 0.0},
        editor::ObservationTargetKind::landmark});
    const auto over_budget = editor::evaluate_authoring_observation(
        scene, two_targets, too_small);
    REQUIRE(!over_budget);
    REQUIRE(over_budget.error().code == core::ErrorCode::validation_failed);

    auto malformed_options = editor::AuthoringObservationOptions{};
    malformed_options.included_target_kinds = 1U << 30U;
    REQUIRE(!malformed_options.validate());
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void(*)()>> tests = {
        {"point sightline reports nearest blocker deterministically",
         point_sightline_reports_nearest_blocker_and_is_deterministic},
        {"point sightline respects endpoint exclusions and transforms",
         point_sightline_respects_endpoint_exclusions_and_world_transforms},
        {"point sightline rejects stale and unbound inputs",
         point_sightline_fails_closed_on_stale_or_unbound_occluders},
        {"point sightline rejects invalid inputs and budget overflow",
         point_sightline_rejects_invalid_inputs_and_budget_overflow},
        {"route visibility reports revision-bound landmark samples",
         route_visibility_reports_revision_bound_landmark_samples},
        {"route visibility enforces station exclusions and aggregate bounds",
         route_visibility_honors_per_station_exclusions_and_fails_closed},
        {"curve route visibility is sampled and source-revision bound",
         curve_route_visibility_samples_and_binds_the_exact_authored_source},
        {"landmark viewshed preserves row-major visibility receipts",
         landmark_viewshed_preserves_row_major_visibility_and_receipts},
        {"landmark viewshed rejects malformed grids and aggregate overflow",
         landmark_viewshed_rejects_bad_grids_and_aggregate_overflow},
        {"terrain viewshed grid samples the bounded triangulated product",
         terrain_viewshed_grid_samples_the_bounded_triangulated_product},
        {"authoring observation projects semantic targets through cone and visibility",
         authoring_observation_projects_semantics_cone_and_visibility},
        {"authoring observation rejects invalid context and aggregate overflow",
         authoring_observation_rejects_invalid_context_and_aggregate_overflow},
    };
    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
