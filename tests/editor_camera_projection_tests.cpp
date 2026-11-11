#include <carto/editor/editor_camera.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using carto::core::Vec2d;
using carto::core::Vec3d;
using carto::editor::EditorCamera;
using carto::editor::EditorProjection;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(Vec3d a, Vec3d b) { return (a - b).length() < 1.0e-8; }

void orthographic_frame_keeps_the_whole_sphere_in_front() {
    for (const double aspect : {0.5, 1.0, 2.0}) {
        EditorCamera camera;
        camera.set_projection(EditorProjection::orthographic);
        require(camera.set_orientation(0.7, -0.5), "oblique camera orientation accepted");
        const Vec3d centre{20.0, 30.0, -40.0};
        constexpr double radius = 50.0;
        require(camera.frame(centre, radius, aspect), "large orthographic frame accepted");
        require(camera.distance() > radius, "orthographic frame places the eye outside the sphere");
        const auto axes = camera.axes();
        const Vec2d extent{600.0 * aspect, 600.0};
        for (const auto point : {centre - axes.forward * radius,
                 centre + axes.forward * radius,
                 centre + axes.right * (radius * 0.99),
                 centre - axes.right * (radius * 0.99),
                 centre + axes.up * (radius * 0.99),
                 centre - axes.up * (radius * 0.99)}) {
            const auto marker = camera.project_marker(point, extent);
            require(marker && marker.value().in_front && marker.value().inside_viewport,
                "framed sphere extremities remain visible in both viewport dimensions");
        }
        const auto stable = camera.serialize();
        require(camera.frame(centre, radius, aspect) && camera.serialize() == stable,
            "repeated orthographic framing is deterministic");
        require(!camera.frame(centre, std::numeric_limits<double>::max(), aspect) &&
                camera.serialize() == stable,
            "unrepresentable frame fails without changing camera state");
        const auto restored = EditorCamera::deserialize(stable);
        require(restored && restored->serialize() == stable,
            "framed eye distance survives local camera persistence");
        const double distant_eye = camera.distance();
        require(camera.frame(centre, 1.0, aspect) && camera.distance() == distant_eye,
            "framing a smaller sphere does not pull an already safe eye forward");
    }
}

void orthographic_rays_reach_both_sides_of_the_target() {
    for (const auto orientation : {Vec2d{0.0, 0.0}, Vec2d{0.7, -0.5}}) {
        EditorCamera camera;
        camera.set_projection(EditorProjection::orthographic);
        require(camera.set_orientation(orientation.x, orientation.y), "ray orientation accepted");
        require(camera.frame({2.0, 3.0, 4.0}, 5.0, 4.0 / 3.0), "ray frame accepted");
        const auto axes = camera.axes();
        const Vec2d extent{800.0, 600.0};
        for (const double depth_offset : {-4.0, 0.0, 4.0}) {
            const auto point = camera.target() + axes.right * 1.0 + axes.up * 0.5 +
                axes.forward * depth_offset;
            const auto marker = camera.project_marker(point, extent);
            require(marker && marker.value().in_front, "ray target is visible");
            const auto ray = camera.ray_for_pixel(marker.value().screen_position, extent);
            require(bool(ray), "orthographic ray accepted");
            const double depth = carto::core::dot(point - ray.value().origin, ray.value().direction);
            require(depth > 0.0, "visible points on either side of the orbit target lie ahead of the ray");
            require(near(ray.value().origin + ray.value().direction * depth, point),
                "projected pixel ray reconstructs the exact world point");
            require(std::abs(depth - marker.value().view_depth) < 1.0e-8,
                "ray distance and marker view depth use the same camera plane");
        }
    }
}

void perspective_projection_retains_its_ray_and_frame_contract() {
    EditorCamera camera;
    require(camera.frame({2.0, 3.0, 4.0}, 50.0, 0.5), "perspective frame accepted");
    require(camera.distance() > 50.0, "perspective framing still keeps the eye outside the sphere");
    const auto ray = camera.ray_for_pixel({150.0, 300.0}, {300.0, 600.0});
    require(ray && near(ray.value().origin, camera.position()) &&
            near(ray.value().direction, camera.axes().forward),
        "perspective centre ray keeps its existing eye origin and forward direction");
    const auto stable = camera.serialize();
    require(!camera.frame({}, 50.0, 0.0) && camera.serialize() == stable,
        "invalid perspective framing remains atomic");
}
} // namespace

int main() {
    orthographic_frame_keeps_the_whole_sphere_in_front();
    orthographic_rays_reach_both_sides_of_the_target();
    perspective_projection_retains_its_ray_and_frame_contract();
    std::cout << "editor camera projection tests passed\n";
}
