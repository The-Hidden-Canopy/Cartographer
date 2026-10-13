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
    camera_does_not_author_scene_mutation();
    std::cout << "editor camera tests passed\n";
    return EXIT_SUCCESS;
}
