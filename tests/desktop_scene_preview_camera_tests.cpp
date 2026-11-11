#include "project_document_access.hpp"
#include "studio_scene_preview_camera.hpp"
#include "studio_view_settings.hpp"
#include "studio_viewport_capture.hpp"

#include <carto/geometry/primitives.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
using namespace carto;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void near(double actual, double expected) {
    require(std::abs(actual - expected) < 1.0e-8, "camera projection differs");
}

project::ProjectDocument fixture() {
    auto document = take(project::ProjectDocument::create("Synthetic preview camera fixture"));
    auto mesh = take(geometry::make_box({2.0, 2.0, 2.0}));
    attributes::AttributeSet payload({
        {attributes::AttributeDomain::face, mesh.faces_sorted().size()}});
    take(payload.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
        attributes::AttributeType::uint32}, std::vector<attributes::AttributeValue>(
            mesh.faces_sorted().size(), std::uint32_t{1U})}));
    take(mesh.set_attribute_payload(std::move(payload), {}));
    auto access = project::testing::access(document);
    const auto mesh_id = take(access.add_mesh(std::move(mesh)));
    const auto object = take(access.create_object("Preview cube"));
    take(access.attach_mesh(object, mesh_id));
    project::MaterialCatalog catalog;
    take(catalog.insert_material({1U, "Fixture material", {}, core::Revision{1U}}));
    take(catalog.insert_assignment({{mesh_id, 1U}, 1U, {}}));
    take(document.set_material_catalog(std::move(catalog)));
    // Exercise the persisted representation, not an unsaved parallel scene.
    return take(project::ProjectDocument::deserialize(document.serialize()));
}

core::Vec2d project_preview(const d3d12::SceneRenderCamera& camera,
                           core::Vec3d point, core::Vec2d extent) {
    const auto forward = (camera.target - camera.position).normalized();
    const auto right = core::cross(forward, camera.up.normalized()).normalized();
    const auto up = core::cross(right, forward).normalized();
    const auto delta = point - camera.position;
    const auto depth = core::dot(delta, forward);
    require(depth > camera.near_plane && depth < camera.far_plane,
        "fixture is outside preview clipping range");
    const double span = camera.projection == d3d12::SceneRenderProjection::orthographic
        ? camera.orthographic_height : 2.0 * depth * std::tan(camera.vertical_fov * 0.5);
    return {extent.x * 0.5 + core::dot(delta, right) * extent.y / span,
        extent.y * 0.5 - core::dot(delta, up) * extent.y / span};
}

void named_views_reach_saved_scene_requests() {
    const auto document = fixture();
    const auto original = document.serialize();
    for (const auto kind : {studio::ViewportViewKind::perspective_3d,
                            studio::ViewportViewKind::front_xy,
                            studio::ViewportViewKind::right_zy}) {
        for (const core::Vec2d extent : {core::Vec2d{800.0, 600.0},
                                       core::Vec2d{400.0, 800.0}}) {
            editor::EditorCamera navigation;
            if (kind != studio::ViewportViewKind::perspective_3d) {
                navigation.set_projection(editor::EditorProjection::orthographic);
                require(studio::apply_orthographic_view_orientation(kind, navigation),
                    "named orientation unavailable");
            } else {
                require(navigation.set_orientation(0.72, 0.43), "perspective orientation");
            }
            require(navigation.frame({}, 50.0, extent.x / extent.y), "large-scene frame");
            const auto state = navigation.serialize();
            const auto camera = take(studio::scene_preview_camera_from_editor_camera(navigation, 1.5));
            require(camera.projection == (kind == studio::ViewportViewKind::perspective_3d
                ? d3d12::SceneRenderProjection::perspective
                : d3d12::SceneRenderProjection::orthographic), "named projection lost");
            near(camera.orthographic_height, navigation.orthographic_height());
            near(camera.exposure_ev, 1.5);
            const auto live = take(studio::authoring_view_from_editor_camera(
                navigation, extent.x / extent.y));
            const auto axes = navigation.axes();
            for (const auto point : {navigation.target(),
                    navigation.target() + axes.right * 20.0 + axes.up * 10.0,
                    navigation.target() - axes.forward * 40.0}) {
                const auto marker = take(navigation.project_marker(point, extent));
                require(marker.in_front, "near-side geometry must remain visible");
                const auto pixel = project_preview(camera, point, extent);
                const auto live_pixel = take(live.project(point,
                    static_cast<std::uint32_t>(extent.x), static_cast<std::uint32_t>(extent.y)));
                near(pixel.x, marker.screen_position.x);
                near(pixel.y, marker.screen_position.y);
                near(pixel.x, live_pixel.x);
                near(pixel.y, live_pixel.y);
            }
            const auto request = take(d3d12::prepare_project_scene_render("camera-integration",
                document, camera, static_cast<std::uint32_t>(extent.x),
                static_cast<std::uint32_t>(extent.y)));
            take(request.validate());
            const auto digest = take(request.digest());
            auto restored = editor::EditorCamera::deserialize(state);
            require(restored.has_value(), "persisted navigation state reopens");
            auto repeated = request;
            repeated.camera = take(studio::scene_preview_camera_from_editor_camera(*restored, 1.5));
            require(take(repeated.digest()) == digest, "repeated capture identity differs");
            require(restored->dolly(1.0), "zoom changes named view");
            repeated.camera = take(studio::scene_preview_camera_from_editor_camera(*restored, 1.5));
            require(take(repeated.digest()) != digest, "zoom reused stale capture identity");
            require(navigation.serialize() == state && document.serialize() == original,
                "capture mutated navigation or authored project");
        }
    }
}

void captured_named_view_invalidates_on_navigation_and_source_changes() {
    application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 7U;
    snapshot.project_revision = core::Revision{12U};
    editor::EditorCamera navigation;
    navigation.set_projection(editor::EditorProjection::orthographic);
    require(studio::apply_orthographic_view_orientation(studio::ViewportViewKind::front_xy,
        navigation), "front orientation");
    require(navigation.frame({}, 50.0, 4.0 / 3.0), "front frame");
    const auto bind = [&](const editor::EditorCamera& camera) {
        return take(studio::scene_preview_camera_source_binding(snapshot, {},
            snapshot.project_generation, true, camera, 0.0)).state;
    };
    const studio::SavedScenePreviewBinding captured{7U, core::Revision{12U}, 2U, 3U,
        bind(navigation), {800.0, 600.0}};
    const auto matches = [&](const editor::EditorCamera& camera, bool clean = true,
                             bool live = true, std::uint64_t generation = 7U,
                             core::Vec2d extent = {800.0, 600.0}) {
        return studio::saved_scene_preview_binding_matches(captured, true, clean, true,
            live, generation, core::Revision{12U}, bind(camera), extent, 2U, 3U);
    };
    require(matches(navigation), "front capture binding should match");
    auto changed = navigation;
    require(changed.pan(10.0, 5.0, 600.0) && !matches(changed), "pan must invalidate");
    changed = navigation;
    require(changed.dolly(1.0) && !matches(changed), "zoom must invalidate");
    changed = navigation;
    require(studio::apply_orthographic_view_orientation(studio::ViewportViewKind::right_zy,
        changed) && !matches(changed), "named-view switch must invalidate");
    changed = navigation;
    changed.set_projection(editor::EditorProjection::perspective);
    require(!matches(changed), "projection switch must invalidate");
    require(!matches(navigation, false) && !matches(navigation, true, false) &&
        !matches(navigation, true, true, 8U) &&
        !matches(navigation, true, true, 7U, {801.0, 600.0}),
        "dirty, speculative, replaced, or resized view must invalidate");
    require(!studio::scene_preview_camera_source_binding(snapshot, {}, 7U,
        false, navigation, 0.0),
        "absent editor camera must not admit the separate plan view");
}

void authored_camera_selection_is_independent_of_navigation() {
    application::ApplicationSnapshot snapshot;
    snapshot.project_generation = 7U;
    project::AuthoredCamera authored;
    authored.object = scene::ObjectId{42U};
    authored.identity = "fixture-saved-camera";
    authored.bookmark_identity = "fixture-saved-bookmark";
    authored.revision = core::Revision{1U};
    authored.projection = project::CameraProjection::orthographic;
    take(snapshot.presentation_catalog.insert_camera(authored));
    application::ObjectView object;
    object.object.id = authored.object;
    snapshot.objects.push_back(object);
    const auto narrow = editor::EditorCamera::deserialize("persp 0 0 0 10 0 0 2 0.005");
    require(narrow.has_value(), "narrow navigation camera");
    std::string expected;
    for (const bool active : {false, true}) {
        const auto selected = take(studio::scene_preview_camera_source_binding(snapshot,
            authored.object, 7U, active, *narrow, 0.0));
        require(selected.authored_object == authored.object, "authored selection lost");
        if (expected.empty()) expected = selected.state;
        else require(selected.state == expected, "navigation activation changed authored binding");
        require(!studio::scene_preview_camera_source_binding(snapshot,
            authored.object, 6U, active, *narrow, 0.0), "stale authored selection admitted");
        require(!studio::scene_preview_camera_source_binding(snapshot,
            scene::ObjectId{43U}, 7U, active, *narrow, 0.0), "missing authored camera admitted");
    }
    require(!studio::scene_preview_camera_source_binding(snapshot, {}, 7U, true, *narrow, 0.0),
        "unsupported editor source was admitted after clearing authored selection");
}

void unsupported_camera_values_fail_before_rendering() {
    editor::EditorCamera camera;
    const auto narrow = editor::EditorCamera::deserialize("persp 0 0 0 10 0 0 2 0.005");
    require(narrow.has_value(), "narrow FOV is valid editor state");
    require(!studio::scene_preview_camera_from_editor_camera(*narrow, 0.0),
        "unsupported perspective FOV accepted");
    const auto minimum = editor::EditorCamera::deserialize("persp 0 0 0 10 0 0 2 0.01");
    require(minimum.has_value() &&
        studio::scene_preview_camera_from_editor_camera(*minimum, 0.0).has_value(),
        "renderer minimum perspective FOV refused");
    const auto narrow_ortho = editor::EditorCamera::deserialize("ortho 0 0 0 10 0 0 2 0.005");
    require(narrow_ortho.has_value() &&
        studio::scene_preview_camera_from_editor_camera(*narrow_ortho, 0.0).has_value(),
        "unused perspective FOV must not refuse orthographic rendering");
    for (const double exposure : {21.0, -21.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()})
        require(!studio::scene_preview_camera_from_editor_camera(camera, exposure),
            "invalid exposure accepted");
    require(camera.frame({}, 5.0e7, 1.0), "editor permits wider source camera range");
    require(!studio::scene_preview_camera_from_editor_camera(camera, 0.0),
        "unrepresentable renderer camera accepted");
}
} // namespace

int main() {
    named_views_reach_saved_scene_requests();
    captured_named_view_invalidates_on_navigation_and_source_changes();
    authored_camera_selection_is_independent_of_navigation();
    unsupported_camera_values_fail_before_rendering();
}
