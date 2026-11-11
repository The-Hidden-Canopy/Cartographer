#include "studio_viewshed.hpp"
#include "studio_scene_camera.hpp"

#include <stdexcept>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace {

#define REQUIRE(expression) do { \
    if (!(expression)) throw std::runtime_error( \
        std::string("requirement failed: ") + #expression); \
} while (false)

void editor_camera_matches_authoring_projection() {
    using namespace carto;
    for (const auto projection : {editor::EditorProjection::perspective,
                                  editor::EditorProjection::orthographic}) {
        for (const auto [yaw, pitch] : std::array{
                 std::pair{0.0, 0.0}, std::pair{0.72, 0.43}, std::pair{-1.31, -0.71}}) {
            editor::EditorCamera camera;
            REQUIRE(camera.set_orientation(yaw, pitch));
            camera.set_projection(projection);
            REQUIRE(camera.frame({2.0, -1.0, 3.0}, 4.0, 4.0 / 3.0));
            const auto view = studio::authoring_view_from_editor_camera(camera, 4.0 / 3.0);
            REQUIRE(view);
            const auto axes = camera.axes();
            for (const auto point : {camera.target(),
                    camera.target() + axes.right * 1.2 + axes.up * 0.7,
                    camera.target() - axes.right * 0.9 - axes.up * 0.4 + axes.forward}) {
                const auto marker = camera.project_marker(point, {800.0, 600.0});
                const auto rendered = view.value().project(point, 800U, 600U);
                REQUIRE(marker && rendered && marker.value().in_front);
                REQUIRE(std::abs(marker.value().screen_position.x - rendered.value().x) < 1.0e-8);
                REQUIRE(std::abs(marker.value().screen_position.y - rendered.value().y) < 1.0e-8);
                REQUIRE(std::abs(marker.value().view_depth - rendered.value().z) < 1.0e-8);
            }
            REQUIRE(!studio::authoring_view_from_editor_camera(camera, 0.0));
            REQUIRE(!studio::authoring_view_from_editor_camera(camera, std::numeric_limits<double>::infinity()));
        }
    }
    editor::EditorCamera huge;
    REQUIRE(huge.frame({}, 5.0e7, 1.0));
    REQUIRE(!studio::authoring_view_from_editor_camera(huge, 1.0));
}

void plan_camera_matches_authoring_projection_without_a_gpu() {
    using namespace carto;
    editor::WorkSurfaceCamera camera;
    const core::Vec2d extent{401.0, 799.0};
    REQUIRE(camera.frame({3.0, -4.0}, {20.0, 10.0}, extent));
    const auto view = studio::authoring_view_from_plan_camera(camera, {}, extent.x / extent.y);
    REQUIRE(view);
    REQUIRE(view.value().projection == project::CameraProjection::orthographic);
    REQUIRE(view.value().aspect_ratio == extent.x / extent.y);
    for (const core::Vec2d point : {core::Vec2d{3.0, -4.0},
                                   core::Vec2d{8.0, -1.0}, core::Vec2d{-4.0, -8.0}}) {
        const auto expected = camera.world_to_screen(point, {}, extent);
        const auto actual = view.value().project({point.x, 0.0, point.y}, 401U, 799U);
        REQUIRE(expected && actual);
        REQUIRE(std::abs(expected.value().x - actual.value().x) < 1.0e-8);
        REQUIRE(std::abs(expected.value().y - actual.value().y) < 1.0e-8);
    }
    REQUIRE(!studio::authoring_view_from_plan_camera(camera, {}, 0.0));
}

void explicit_viewshed_observer_preserves_finite_world_coordinates() {
    carto::application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 5U;
    const carto::core::Vec3d explicit_position{2.5, 4.0, -8.25};
    const auto resolved = carto::studio::resolve_viewshed_observer(
        snapshot, {}, explicit_position);
    REQUIRE(resolved);
    REQUIRE(resolved.value().position.x == explicit_position.x);
    REQUIRE(resolved.value().position.y == explicit_position.y);
    REQUIRE(resolved.value().position.z == explicit_position.z);
    REQUIRE(!resolved.value().object.has_value());
    REQUIRE(!carto::studio::resolve_viewshed_observer(snapshot, {},
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}));
}

void bound_viewshed_observer_uses_current_world_origin() {
    carto::application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 8U;
    carto::application::ObjectView view;
    view.object.id = carto::scene::ObjectId{42U};
    view.world_transform.translation = {10.0, 2.0, -3.0};
    snapshot.objects.push_back(view);

    const carto::studio::ViewshedObserverBinding binding{
        carto::scene::ObjectId{42U}, snapshot.project_generation};
    const auto resolved = carto::studio::resolve_viewshed_observer(
        snapshot, binding, {999.0, 999.0, 999.0});
    REQUIRE(resolved);
    REQUIRE(resolved.value().position.x == 10.0);
    REQUIRE(resolved.value().position.y == 2.0);
    REQUIRE(resolved.value().position.z == -3.0);
    REQUIRE(resolved.value().object == binding.object);
    REQUIRE(carto::studio::viewshed_observer_matches(
        resolved, {10.0, 2.0, -3.0}, binding.object));
    REQUIRE(!carto::studio::viewshed_observer_matches(
        resolved, {10.0, 2.0, -3.1}, binding.object));
    REQUIRE(!carto::studio::viewshed_observer_matches(
        resolved, {10.0, 2.0, -3.0}, std::nullopt));
}

void bound_viewshed_observer_rejects_stale_missing_and_invalid_sources() {
    carto::application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 3U;
    carto::application::ObjectView view;
    view.object.id = carto::scene::ObjectId{7U};
    snapshot.objects.push_back(view);

    const carto::studio::ViewshedObserverBinding stale_generation{
        carto::scene::ObjectId{7U}, 2U};
    const auto stale = carto::studio::resolve_viewshed_observer(
        snapshot, stale_generation, {});
    REQUIRE(!stale && stale.error().code == carto::core::ErrorCode::stale_data);

    const carto::studio::ViewshedObserverBinding missing_object{
        carto::scene::ObjectId{8U}, snapshot.project_generation};
    const auto missing = carto::studio::resolve_viewshed_observer(
        snapshot, missing_object, {});
    REQUIRE(!missing && missing.error().code == carto::core::ErrorCode::stale_data);

    view.world_transform.translation.x = std::numeric_limits<double>::infinity();
    snapshot.objects.front() = view;
    const carto::studio::ViewshedObserverBinding invalid_transform{
        carto::scene::ObjectId{7U}, snapshot.project_generation};
    const auto invalid = carto::studio::resolve_viewshed_observer(
        snapshot, invalid_transform, {});
    REQUIRE(!invalid && invalid.error().code == carto::core::ErrorCode::validation_failed);
}

void scene_preview_camera_binding_uses_explicit_current_authored_source() {
    carto::application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 7U;
    snapshot.project_revision = carto::core::Revision{12U};
    carto::project::AuthoredCamera camera;
    camera.object = carto::scene::ObjectId{42U};
    camera.identity = "render-camera";
    camera.bookmark_identity = "render-camera-bookmark";
    camera.revision = carto::core::Revision{3U};
    REQUIRE(snapshot.presentation_catalog.insert_camera(camera));
    carto::application::ObjectView object;
    object.object.id = camera.object;
    object.world_transform.translation = {3.0, 4.0, 5.0};
    object.world_transform.rotation = {0.0, 0.5, 0.0, 0.8660254037844386};
    snapshot.objects.push_back(object);

    const auto binding = carto::studio::saved_scene_preview_camera_binding(
        snapshot, camera.object, snapshot.project_generation, {});
    REQUIRE(binding);
    REQUIRE(binding.value().authored_object == camera.object);
    REQUIRE(binding.value().authored_revision == camera.revision);
    REQUIRE(binding.value().source_label == camera.identity);
    REQUIRE(binding.value().state.find("authored:") == 0U);
    const auto repeated = carto::studio::saved_scene_preview_camera_binding(
        snapshot, camera.object, snapshot.project_generation, {});
    REQUIRE(repeated && repeated.value().state == binding.value().state);
}

void scene_preview_camera_binding_refuses_stale_missing_and_accepts_orthographic_sources() {
    carto::application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 9U;
    carto::project::AuthoredCamera camera;
    camera.object = carto::scene::ObjectId{5U};
    camera.identity = "review-camera";
    camera.bookmark_identity = "review-bookmark";
    camera.revision = carto::core::Revision{2U};
    REQUIRE(snapshot.presentation_catalog.insert_camera(camera));
    carto::application::ObjectView object;
    object.object.id = camera.object;
    snapshot.objects.push_back(object);

    const auto stale = carto::studio::saved_scene_preview_camera_binding(
        snapshot, camera.object, snapshot.project_generation - 1U, {});
    REQUIRE(!stale && stale.error().code == carto::core::ErrorCode::stale_data);
    const auto missing = carto::studio::saved_scene_preview_camera_binding(
        snapshot, carto::scene::ObjectId{6U}, snapshot.project_generation, {});
    REQUIRE(!missing && missing.error().code == carto::core::ErrorCode::stale_data);

    carto::application::ApplicationSnapshot orthographic_snapshot;
    orthographic_snapshot.project_generation = snapshot.project_generation;
    camera.projection = carto::project::CameraProjection::orthographic;
    camera.orthographic_height = 7.5;
    REQUIRE(orthographic_snapshot.presentation_catalog.insert_camera(camera));
    orthographic_snapshot.objects.push_back(object);
    const auto orthographic = carto::studio::saved_scene_preview_camera_binding(
        orthographic_snapshot, camera.object,
        orthographic_snapshot.project_generation, {});
    REQUIRE(orthographic);
    REQUIRE(orthographic.value().state.find("orthographic") != std::string::npos);
    REQUIRE(orthographic.value().state.find("7.5") != std::string::npos);

    carto::application::ApplicationSnapshot unbounded_snapshot;
    unbounded_snapshot.project_generation = snapshot.project_generation;
    camera.projection = carto::project::CameraProjection::perspective;
    camera.far_plane = 100'000'001.0;
    REQUIRE(unbounded_snapshot.presentation_catalog.insert_camera(camera));
    unbounded_snapshot.objects.push_back(object);
    const auto unbounded = carto::studio::saved_scene_preview_camera_binding(
        unbounded_snapshot, camera.object, unbounded_snapshot.project_generation, {});
    REQUIRE(!unbounded &&
        unbounded.error().code == carto::core::ErrorCode::unsupported);

    carto::application::ApplicationSnapshot nonfinite_snapshot;
    nonfinite_snapshot.project_generation = snapshot.project_generation;
    camera.far_plane = 10'000.0;
    REQUIRE(nonfinite_snapshot.presentation_catalog.insert_camera(camera));
    object.world_transform.translation.x =
        std::numeric_limits<double>::infinity();
    nonfinite_snapshot.objects.push_back(object);
    const auto nonfinite = carto::studio::saved_scene_preview_camera_binding(
        nonfinite_snapshot, camera.object,
        nonfinite_snapshot.project_generation, {});
    REQUIRE(!nonfinite && nonfinite.error().code == carto::core::ErrorCode::stale_data);
}

void scene_preview_camera_binding_keeps_viewport_mode_explicit() {
    carto::application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 4U;
    const auto viewport = carto::studio::saved_scene_preview_camera_binding(
        snapshot, std::nullopt, snapshot.project_generation, "editor-state-v1");
    REQUIRE(viewport);
    REQUIRE(viewport.value().state == "viewport:editor-state-v1");
    REQUIRE(!viewport.value().authored_object.has_value());
    REQUIRE(!carto::studio::saved_scene_preview_camera_binding(
        snapshot, std::nullopt, snapshot.project_generation, {}));
}

void viewshed_cell_footprints_preserve_projected_grid_geometry() {
    carto::editor::LandmarkViewshedQuery query;
    query.width = 3U;
    query.height = 2U;
    query.target_positions = {
        {0.0, 1.0, 0.0}, {2.0, 2.0, 0.0}, {5.0, 3.0, 0.0},
        {0.0, 1.5, 4.0}, {2.0, 2.5, 4.0}, {5.0, 3.5, 4.0}};
    const auto footprint = carto::studio::viewshed_cell_footprint(query, 0U, 1U);
    REQUIRE(footprint);
    REQUIRE(footprint.value().corners[0U].x == 1.0);
    REQUIRE(footprint.value().corners[0U].y == 2.0);
    REQUIRE(footprint.value().corners[0U].z == -2.0);
    REQUIRE(footprint.value().corners[1U].x == 3.0);
    REQUIRE(footprint.value().corners[1U].z == -2.0);
    REQUIRE(footprint.value().corners[2U].x == 3.0);
    REQUIRE(footprint.value().corners[2U].z == 2.0);
    REQUIRE(footprint.value().corners[3U].x == 1.0);
    REQUIRE(footprint.value().corners[3U].z == 2.0);
    const auto depth_samples =
        carto::studio::viewshed_cell_depth_samples(footprint.value());
    REQUIRE(depth_samples);
    REQUIRE(depth_samples.value().points.size() == 25U);
    REQUIRE(depth_samples.value().points.front().x == footprint.value().corners[0U].x);
    REQUIRE(depth_samples.value().points.front().y == footprint.value().corners[0U].y);
    REQUIRE(depth_samples.value().points.front().z == footprint.value().corners[0U].z);
    REQUIRE(depth_samples.value().points.back().x == footprint.value().corners[2U].x);
    REQUIRE(depth_samples.value().points.back().y == footprint.value().corners[2U].y);
    REQUIRE(depth_samples.value().points.back().z == footprint.value().corners[2U].z);
    REQUIRE(depth_samples.value().points[12U].x == 2.0);
    REQUIRE(depth_samples.value().points[12U].y == 2.0);
    REQUIRE(depth_samples.value().points[12U].z == 0.0);
    auto malformed_footprint = footprint.value();
    malformed_footprint.corners[3U].x =
        std::numeric_limits<double>::infinity();
    REQUIRE(!carto::studio::viewshed_cell_depth_samples(malformed_footprint));

    query.target_positions[1U].x = 0.0;
    REQUIRE(!carto::studio::viewshed_cell_footprint(query, 0U, 1U));
    query.target_positions.resize(5U);
    REQUIRE(!carto::studio::viewshed_cell_footprint(query, 0U, 0U));
    REQUIRE(!carto::studio::viewshed_cell_footprint(query, 2U, 0U));
}

} // namespace

int main() {
    editor_camera_matches_authoring_projection();
    plan_camera_matches_authoring_projection_without_a_gpu();
    explicit_viewshed_observer_preserves_finite_world_coordinates();
    bound_viewshed_observer_uses_current_world_origin();
    bound_viewshed_observer_rejects_stale_missing_and_invalid_sources();
    scene_preview_camera_binding_uses_explicit_current_authored_source();
    scene_preview_camera_binding_refuses_stale_missing_and_accepts_orthographic_sources();
    scene_preview_camera_binding_keeps_viewport_mode_explicit();
    viewshed_cell_footprints_preserve_projected_grid_geometry();
}
