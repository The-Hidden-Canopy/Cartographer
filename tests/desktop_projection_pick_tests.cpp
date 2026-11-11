#include "studio_view_settings.hpp"
#include "studio_viewport_capture.hpp"
#include <carto/ui/ui.hpp>

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
void near(double actual, double expected) {
    require(std::abs(actual - expected) < 1.0e-9, "projected pick differs from source");
}
void near(core::Vec3d actual, core::Vec3d expected) {
    near(actual.x, expected.x); near(actual.y, expected.y); near(actual.z, expected.z);
}

void named_orthographic_picks_preserve_source_positions_and_visibility() {
    for (const auto kind : {studio::ViewportViewKind::front_xy, studio::ViewportViewKind::right_zy}) {
        editor::EditorCamera camera;
        camera.set_projection(editor::EditorProjection::orthographic);
        require(studio::apply_orthographic_view_orientation(kind, camera), "named orientation");
        require(camera.frame({}, 5.0, 4.0 / 3.0), "named frame");
        const auto state = camera.serialize();
        const auto axes = camera.axes();
        const core::Vec2d extent{800.0, 600.0};
        ui::SculptViewportTriangle triangle;
        triangle.object = scene::ObjectId{1U};
        triangle.face = geometry::FaceId{1U};
        triangle.vertices = {geometry::VertexId{1U}, geometry::VertexId{2U}, geometry::VertexId{3U}};
        const std::array<core::Vec2d, 3U> offsets{{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}}};
        const std::array<double, 3U> depths{5.0, 10.0, 20.0};
        for (std::size_t i = 0U; i < 3U; ++i) {
            triangle.local_positions[i] = camera.position() + axes.right * offsets[i].x +
                axes.up * offsets[i].y + axes.forward * depths[i];
            const auto marker = take(camera.project_marker(triangle.local_positions[i], extent));
            require(marker.in_front && marker.inside_viewport, "fixture must be visible");
            triangle.screen_positions[i] = marker.screen_position;
            triangle.camera_depth[i] = -marker.view_depth;
        }
        triangle.perspective_correct = studio::viewport_uses_perspective_interpolation(true, camera);
        require(!triangle.perspective_correct, "named orthographic view must use affine weights");
        const std::array<double, 3U> weights{0.6, 0.2, 0.2};
        core::Vec2d screen{};
        core::Vec3d source{};
        for (std::size_t i = 0U; i < 3U; ++i) {
            screen = screen + triangle.screen_positions[i] * weights[i];
            source = source + triangle.local_positions[i] * weights[i];
        }
        const auto hit = take(ui::resolve_frontmost_viewport_surface(
            std::span<const ui::SculptViewportTriangle>{&triangle, 1U}, screen));
        require(hit.status == ui::ProjectedSurfaceHitStatus::hit, "orthographic source hit");
        near(hit.sample.position, source);
        near(hit.camera_depth, -9.0);
        require(hit.sample.surface_anchor.has_value(), "source anchor retained");
        for (std::size_t i = 0U; i < 3U; ++i)
            near(hit.sample.surface_anchor->barycentric[i], weights[i]);

        auto blocker = triangle;
        blocker.object = scene::ObjectId{2U};
        blocker.face = geometry::FaceId{2U};
        blocker.camera_depth = {-7.5, -7.5, -7.5};
        for (std::size_t i = 0U; i < 3U; ++i)
            blocker.local_positions[i] = camera.position() + axes.right * offsets[i].x +
                axes.up * offsets[i].y + axes.forward * 7.5;
        std::array<ui::SculptViewportTriangle, 2U> scene{triangle, blocker};
        const auto frontmost = take(ui::resolve_frontmost_viewport_surface(scene, screen));
        require(frontmost.object == blocker.object, "nearer surface must block the sloped source");
        const auto index = take(ui::build_projected_surface_depth_index(scene, {}, extent));
        const auto indexed = take(ui::resolve_frontmost_viewport_depth(scene, index, screen, 2U));
        require(indexed.camera_depth.has_value(), "indexed depth is available");
        near(*indexed.camera_depth, frontmost.camera_depth);

        // Reproduce the former desktop flag: reciprocal weights falsely put
        // the sloped source in front and move its immutable source anchor.
        scene.front().perspective_correct = true;
        const auto incorrect = take(ui::resolve_frontmost_viewport_surface(scene, screen));
        require(incorrect.object == triangle.object, "fixture must expose the old ordering bug");
        require((incorrect.sample.position - source).length() > 0.1,
            "fixture must expose the old source-anchor displacement");

        // A clipped screen corner still maps back to the original source IDs.
        auto clipped = triangle;
        clipped.screen_positions[0] = (triangle.screen_positions[0] + triangle.screen_positions[2]) * 0.5;
        clipped.camera_depth[0] = (triangle.camera_depth[0] + triangle.camera_depth[2]) * 0.5;
        clipped.screen_source_weights[0] = {0.5, 0.0, 0.5};
        const auto centroid = (clipped.screen_positions[0] + clipped.screen_positions[1] +
            clipped.screen_positions[2]) * (1.0 / 3.0);
        const auto clipped_hit = take(ui::resolve_frontmost_viewport_surface(
            std::span<const ui::SculptViewportTriangle>{&clipped, 1U}, centroid));
        require(clipped_hit.sample.surface_anchor.has_value(), "clipped source anchor retained");
        const std::array<double, 3U> expected{1.0 / 6.0, 1.0 / 3.0, 0.5};
        for (std::size_t i = 0U; i < 3U; ++i)
            near(clipped_hit.sample.surface_anchor->barycentric[i], expected[i]);
        require(camera.serialize() == state, "picking cannot mutate navigation state");
    }
}

void edge_occlusion_uses_the_selected_projection() {
    const auto orthographic = take(studio::projected_edge_camera_depth(-10.0, -20.0, 0.5, false));
    const auto perspective = take(studio::projected_edge_camera_depth(-10.0, -20.0, 0.5, true));
    near(orthographic, -15.0);
    near(perspective, -40.0 / 3.0);
    require(!take(studio::perspective_edge_is_frontmost(orthographic, -14.0)),
        "orthographic edge behind a foreground surface must be refused");
    require(take(studio::perspective_edge_is_frontmost(perspective, -14.0)),
        "perspective edge must retain reciprocal visibility behavior");
    for (const bool perspective_correct : {false, true}) {
        near(take(studio::projected_edge_camera_depth(-10.0, -20.0, 0.0, perspective_correct)), -10.0);
        near(take(studio::projected_edge_camera_depth(-10.0, -20.0, 1.0, perspective_correct)), -20.0);
        require(!studio::projected_edge_camera_depth(-10.0, 0.0, 0.5, perspective_correct),
            "behind-camera edge refused");
        require(!studio::projected_edge_camera_depth(-10.0, -20.0, 1.1, perspective_correct),
            "out-of-range edge parameter refused");
        require(!studio::projected_edge_camera_depth(-10.0, -20.0,
            std::numeric_limits<double>::quiet_NaN(), perspective_correct), "NaN edge parameter refused");
    }
    editor::EditorCamera camera;
    require(studio::viewport_uses_perspective_interpolation(true, camera),
        "3D perspective correction remains enabled");
    require(!studio::viewport_uses_perspective_interpolation(false, camera),
        "inactive editor camera cannot change plan interpolation");
    camera.set_projection(editor::EditorProjection::orthographic);
    require(!studio::viewport_uses_perspective_interpolation(true, camera),
        "orthographic editor camera uses affine interpolation");
}
} // namespace

int main() {
    named_orthographic_picks_preserve_source_positions_and_visibility();
    edge_occlusion_uses_the_selected_projection();
}
