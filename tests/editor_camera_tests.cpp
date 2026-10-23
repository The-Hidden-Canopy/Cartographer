#include <carto/editor/editor_camera.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

using carto::core::Vec3d;
using carto::editor::EditorCamera;
using carto::editor::EditorProjection;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double left, double right, double epsilon = 1.0e-10) {
    return std::abs(left - right) <= epsilon;
}

bool same_vec(Vec3d left, Vec3d right) {
    return near(left.x, right.x) && near(left.y, right.y) &&
        near(left.z, right.z);
}

void default_camera_has_stable_orthonormal_axes() {
    EditorCamera camera;
    require(camera.valid() && camera.validate(), "default editor camera is valid");
    const auto axes = camera.axes();
    require(near(axes.right.length(), 1.0) && near(axes.up.length(), 1.0) &&
            near(axes.forward.length(), 1.0),
        "camera axes are normalized");
    require(std::abs(carto::core::dot(axes.right, axes.up)) < 1.0e-10 &&
            std::abs(carto::core::dot(axes.right, axes.forward)) < 1.0e-10 &&
            std::abs(carto::core::dot(axes.up, axes.forward)) < 1.0e-10,
        "camera axes are mutually perpendicular");
    require(camera.position().finite(), "camera position is finite");
}

void navigation_is_bounded_and_deterministic() {
    EditorCamera camera;
    const auto before = camera.serialize();
    require(camera.orbit(120.0, -40.0), "orbit accepts finite pixel deltas");
    require(camera.pan(40.0, -20.0, 800.0), "pan accepts a valid viewport");
    require(camera.dolly(2.0), "dolly accepts finite steps");
    require(camera.fly({1.0, 2.0, 3.0}, 0.5), "perspective fly accepts local motion");
    require(camera.valid() && camera.serialize() != before,
        "valid navigation changes only bounded camera state");

    const auto stable = camera.serialize();
    require(!camera.pan(0.0, 0.0, 0.0), "zero-height viewport is rejected");
    require(camera.serialize() == stable, "rejected pan leaves camera unchanged");
    require(!camera.orbit(std::numeric_limits<double>::infinity(), 0.0),
        "non-finite orbit input is rejected");
    require(camera.serialize() == stable, "rejected orbit leaves camera unchanged");
}

void projection_switch_preserves_target_span() {
    EditorCamera camera;
    const double perspective_span =
        2.0 * camera.distance() * std::tan(camera.vertical_fov() * 0.5);
    camera.set_projection(EditorProjection::orthographic);
    require(camera.projection() == EditorProjection::orthographic &&
            near(camera.orthographic_height(), perspective_span),
        "orthographic switch preserves view span");
    camera.set_projection(EditorProjection::perspective);
    const double restored_span =
        2.0 * camera.distance() * std::tan(camera.vertical_fov() * 0.5);
    require(near(restored_span, perspective_span),
        "perspective switch restores view span");
}

void frame_and_serialization_preserve_editor_state() {
    EditorCamera camera;
    require(camera.frame({4.0, -2.0, 1.0}, 3.0, 16.0 / 9.0),
        "perspective frame accepts a finite sphere");
    require(camera.target().x == 4.0 && camera.target().y == -2.0 &&
            camera.target().z == 1.0 && camera.distance() > 3.0,
        "perspective frame binds target and fits radius");

    const auto serialized = camera.serialize();
    const auto reopened = EditorCamera::deserialize(serialized);
    require(reopened.has_value() && reopened->serialize() == serialized,
        "camera state round-trips deterministically");

    camera.set_projection(EditorProjection::orthographic);
    require(camera.frame({0.0, 0.0, 0.0}, 2.0, 0.5),
        "orthographic frame accepts a narrow aspect");
    require(near(camera.orthographic_height(), 8.0),
        "orthographic frame fits both viewport dimensions");
    require(!EditorCamera::deserialize("persp nan 0 0 10 0 0 2 0.7"),
        "non-finite serialized camera state is rejected");
    require(!EditorCamera::deserialize("persp 0 0 0 10 0 0 2 0.7 extra"),
        "trailing serialized camera state is rejected");
}

void pick_rays_match_projection_conventions() {
    EditorCamera camera;
    const auto axes = camera.axes();
    const auto viewport = carto::core::Vec2d{800.0, 600.0};

    const auto centre = camera.ray_for_pixel({400.0, 300.0}, viewport);
    require(centre.has_value(), "centre pixel ray is accepted");
    require(same_vec(centre.value().origin, camera.position()),
        "perspective ray origin is the camera position");
    require(near(carto::core::dot(centre.value().direction, axes.forward),
            1.0, 1.0e-12),
        "centre pixel ray matches the camera forward axis");
    require(near(centre.value().direction.length(), 1.0),
        "perspective ray direction is normalized");

    const auto repeated = camera.ray_for_pixel({400.0, 300.0}, viewport);
    require(same_vec(repeated.value().origin, centre.value().origin) &&
            same_vec(repeated.value().direction, centre.value().direction),
        "pick ray generation is deterministic");

    const auto corner = camera.ray_for_pixel({800.0, 0.0}, viewport);
    require(corner.has_value(), "corner pixel ray is accepted");
    const double aspect = viewport.x / viewport.y;
    const double tan_half = std::tan(camera.vertical_fov() * 0.5);
    const auto expected = (axes.forward + axes.right * (aspect * tan_half) +
                           axes.up * tan_half)
                              .normalized();
    require(near(corner.value().direction.x, expected.x) &&
            near(corner.value().direction.y, expected.y) &&
            near(corner.value().direction.z, expected.z),
        "corner pixel ray spans the FOV cone");

    camera.set_projection(EditorProjection::orthographic);
    const auto ortho_centre = camera.ray_for_pixel({400.0, 300.0}, viewport);
    require(ortho_centre.has_value(), "orthographic centre ray is accepted");
    require(same_vec(ortho_centre.value().origin, camera.position() +
            axes.forward * camera.distance()),
        "orthographic centre ray origin sits on the image plane");
    require(near(carto::core::dot(ortho_centre.value().direction, axes.forward),
            1.0),
        "orthographic centre ray direction is the forward axis");

    const auto ortho_corner = camera.ray_for_pixel({0.0, 0.0}, viewport);
    const double span = camera.orthographic_height();
    const auto expected_origin = camera.target() -
        axes.right * (span * 0.5 * aspect) + axes.up * (span * 0.5);
    require(ortho_corner.has_value() &&
            near(ortho_corner.value().origin.x, expected_origin.x) &&
            near(ortho_corner.value().origin.y, expected_origin.y) &&
            near(ortho_corner.value().origin.z, expected_origin.z),
        "orthographic corner ray offsets on the image plane");
    require(same_vec(ortho_corner.value().direction,
            ortho_centre.value().direction),
        "orthographic rays are parallel");

    const auto zero_viewport = camera.ray_for_pixel({0.0, 0.0}, {0.0, 600.0});
    require(!zero_viewport && zero_viewport.error().code ==
            carto::core::ErrorCode::invalid_argument,
        "zero-sized viewport fails closed");
    const auto nan_pixel = camera.ray_for_pixel(
        {std::numeric_limits<double>::quiet_NaN(), 0.0}, viewport);
    require(!nan_pixel && nan_pixel.error().code ==
            carto::core::ErrorCode::invalid_argument,
        "non-finite pixel fails closed");
    const auto nan_viewport = camera.ray_for_pixel({0.0, 0.0},
        {std::numeric_limits<double>::infinity(), 600.0});
    require(!nan_viewport, "non-finite viewport fails closed");
}

void projected_markers_are_viewport_local_and_fail_closed() {
    EditorCamera camera;
    const auto centre = camera.project_marker({0.0, 0.0, 0.0}, {800.0, 600.0});
    require(centre && centre.value().in_front && centre.value().inside_viewport,
        "perspective marker at target projects inside viewport");
    require(near(centre.value().screen_position.x, 400.0) &&
            near(centre.value().screen_position.y, 300.0),
        "perspective target projects to viewport center");
    const auto behind = camera.project_marker({0.0, 0.0, 20.0}, {800.0, 600.0});
    require(behind && !behind.value().in_front,
        "perspective marker behind the camera is identified");
    const auto outside = camera.project_marker({100.0, 0.0, 0.0}, {800.0, 600.0});
    require(outside && outside.value().in_front && !outside.value().inside_viewport,
        "front-facing marker outside viewport remains explicitly outside");

    camera.set_projection(EditorProjection::orthographic);
    const auto orthographic = camera.project_marker({0.0, 0.0, 0.0}, {800.0, 600.0});
    require(orthographic && orthographic.value().inside_viewport &&
            near(orthographic.value().screen_position.x, 400.0) &&
            near(orthographic.value().screen_position.y, 300.0),
        "orthographic marker shares the viewport center convention");
    require(!camera.project_marker({0.0, 0.0, 0.0}, {0.0, 600.0}),
        "zero-sized viewport is rejected");
    require(!camera.project_marker(
                {std::numeric_limits<double>::infinity(), 0.0, 0.0}, {800.0, 600.0}),
        "non-finite marker world position is rejected");
}

void camera_does_not_author_scene_mutation() {
    EditorCamera camera;
    const auto before = camera.serialize();
    camera.set_projection(EditorProjection::orthographic);
    require(camera.fly({1.0, 0.0, 0.0}, 1.0) == false,
        "orthographic camera refuses fly state rather than inventing behavior");
    require(camera.serialize() != before,
        "projection is UI state and remains independently serializable");
}

} // namespace

int main() {
    default_camera_has_stable_orthonormal_axes();
    navigation_is_bounded_and_deterministic();
    projection_switch_preserves_target_span();
    frame_and_serialization_preserve_editor_state();
    pick_rays_match_projection_conventions();
    projected_markers_are_viewport_local_and_fail_closed();
    camera_does_not_author_scene_mutation();
    std::cout << "editor camera tests passed\n";
    return EXIT_SUCCESS;
}
