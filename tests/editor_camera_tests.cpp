#include <carto/editor/editor_camera.hpp>
#include "../apps/desktop/studio_viewport_capture.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

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

void named_orientation_is_bounded_and_fail_closed() {
    EditorCamera camera;
    require(camera.set_orientation(0.75, -1.2),
        "named orientation accepts bounded orbit angles");
    require(camera.yaw() == 0.75 && camera.pitch() == -1.2,
        "named orientation updates only the camera orientation");
    const auto before = camera.serialize();
    require(!camera.set_orientation(0.0, EditorCamera::kMaxPitch + 0.01),
        "named orientation rejects a pitch beyond the pole bound");
    require(!camera.set_orientation(std::numeric_limits<double>::infinity(), 0.0),
        "named orientation rejects a non-finite yaw");
    require(camera.serialize() == before,
        "rejected named orientation leaves camera state unchanged");
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

void viewport_capture_rejects_camera_changes() {
    using carto::editor::WorkSurfaceCamera;
    EditorCamera perspective;
    WorkSurfaceCamera plan;
    require(carto::studio::viewport_camera_state_matches(
                true, perspective, plan, true, perspective, plan),
        "capture accepts an unchanged perspective or plan camera snapshot");

    EditorCamera moved_perspective = perspective;
    require(moved_perspective.orbit(12.0, -4.0), "camera fixture can orbit");
    require(!carto::studio::viewport_camera_state_matches(
                true, moved_perspective, plan, true, perspective, plan),
        "perspective camera movement invalidates an active capture");

    WorkSurfaceCamera moved_plan = plan;
    require(moved_plan.pan_pixels({12.0, -4.0}, 600.0), "plan fixture can pan");
    require(!carto::studio::viewport_camera_state_matches(
                false, perspective, moved_plan, false, perspective, plan),
        "plan camera movement invalidates an active capture");
    require(!carto::studio::viewport_camera_state_matches(
                true, perspective, plan, false, perspective, plan),
        "switching projection modes invalidates an active capture");
}

void saved_scene_preview_binding_refuses_stale_context() {
    using carto::core::Revision;
    using carto::core::Vec2d;
    const carto::studio::SavedScenePreviewBinding capture{
        7U, Revision{21U}, 4U, 9U, "camera-state-v1", {800.0, 450.0}};
    const auto matches = [&](bool project_available, bool project_clean,
                             bool perspective_camera, bool live_project_source,
                             std::uint64_t generation, Revision revision,
                             std::string_view camera_state, Vec2d extent) {
        return carto::studio::saved_scene_preview_binding_matches(
            capture, project_available, project_clean, perspective_camera,
            live_project_source, generation, revision, camera_state, extent, 4U, 9U);
    };
    require(matches(true, true, true, true, 7U, Revision{21U},
                    "camera-state-v1", {800.0, 450.0}),
        "saved preview accepts the exact clean project, camera, and viewport binding");
    require(!carto::studio::saved_scene_preview_binding_matches(
                capture, true, true, true, true, 7U, Revision{21U},
                "camera-state-v1", {800.0, 450.0}, 5U, 9U),
        "saved preview refuses a changed environment selection");
    require(!carto::studio::saved_scene_preview_binding_matches(
                capture, true, true, true, true, 7U, Revision{21U},
                "camera-state-v1", {800.0, 450.0}, 4U, 10U),
        "saved preview refuses changed transient lighting or exposure settings");
    require(!matches(false, true, true, true, 7U, Revision{21U},
                     "camera-state-v1", {800.0, 450.0}),
        "saved preview refuses a missing project");
    require(!matches(true, false, true, true, 7U, Revision{21U},
                     "camera-state-v1", {800.0, 450.0}),
        "saved preview refuses dirty project state");
    require(!matches(true, true, false, true, 7U, Revision{21U},
                     "camera-state-v1", {800.0, 450.0}),
        "saved preview refuses a non-perspective camera");
    require(!matches(true, true, true, false, 7U, Revision{21U},
                     "camera-state-v1", {800.0, 450.0}),
        "saved preview refuses an agent or otherwise non-live source");
    require(!matches(true, true, true, true, 8U, Revision{21U},
                     "camera-state-v1", {800.0, 450.0}),
        "saved preview refuses another project generation");
    require(!matches(true, true, true, true, 7U, Revision{22U},
                     "camera-state-v1", {800.0, 450.0}),
        "saved preview refuses a stale project revision");
    require(!matches(true, true, true, true, 7U, Revision{21U},
                     "camera-state-v2", {800.0, 450.0}),
        "saved preview refuses camera changes");
    require(!matches(true, true, true, true, 7U, Revision{21U},
                     "camera-state-v1", {801.0, 450.0}),
        "saved preview refuses viewport resize");
    require(!matches(true, true, true, true, 7U, Revision{21U},
                     "camera-state-v1", {0.0, 450.0}),
        "saved preview refuses a zero-sized viewport");
    require(!matches(true, true, true, true, 7U, Revision{21U},
                     "camera-state-v1",
                     {std::numeric_limits<double>::infinity(), 450.0}),
        "saved preview refuses a non-finite viewport extent");
    require(!matches(true, true, true, true, 7U, Revision{21U},
                     "", {800.0, 450.0}),
        "saved preview refuses an unbound camera identity");
}

void saved_scene_preview_render_settings_require_exact_current_inputs() {
    const carto::studio::SavedScenePreviewRenderSettings captured{
        {0.3, 0.7, 1.0}, {3.0, 2.0, 1.0}, 0.5};
    require(carto::studio::saved_scene_preview_render_settings_match(
                captured, captured),
        "saved preview accepts exact captured lighting and exposure");
    require(!carto::studio::saved_scene_preview_render_settings_match(
                captured, {{0.3, 0.7, 0.9}, {3.0, 2.0, 1.0}, 0.5}),
        "saved preview refuses changed light direction when its generation aliases");
    require(!carto::studio::saved_scene_preview_render_settings_match(
                captured, {{0.3, 0.7, 1.0}, {3.0, 2.1, 1.0}, 0.5}),
        "saved preview refuses changed light radiance when its generation aliases");
    require(!carto::studio::saved_scene_preview_render_settings_match(
                captured, {{0.3, 0.7, 1.0}, {3.0, 2.0, 1.0}, 0.6}),
        "saved preview refuses changed exposure when its generation aliases");
    require(!carto::studio::saved_scene_preview_render_settings_match(
                captured, {{0.3, 0.7, 1.0}, {3.0, 2.0, 1.0},
                    std::numeric_limits<double>::quiet_NaN()}),
        "saved preview refuses non-finite current render settings");
    const Vec3d invalid_direction{
        std::numeric_limits<double>::infinity(), 0.7, 1.0};
    require(!carto::studio::saved_scene_preview_render_settings_match(
                {invalid_direction, captured.key_light_radiance, captured.exposure_ev},
                captured),
        "saved preview refuses non-finite captured render settings");
}

void perspective_stroke_sampling_is_spatially_bounded() {
    const auto normal = carto::studio::perspective_stroke_segment_intervals(
        {0.0, 0.0}, {2.1, 0.0}, 10U);
    require(normal && normal.value() == 3U,
        "segment sampling uses intervals no wider than one screen pixel");

    const auto maximum = carto::studio::perspective_stroke_segment_intervals(
        {0.0, 0.0}, {64.0, 0.0}, 1U);
    require(maximum && maximum.value() ==
            carto::studio::kMaxPerspectiveStrokeSegmentSamples,
        "segment at the sample bound is admitted");
    require(!carto::studio::perspective_stroke_segment_intervals(
                {0.0, 0.0}, {65.0, 0.0}, 1U),
        "segment beyond the sample bound is refused");

    const auto work_limited = carto::studio::perspective_stroke_segment_intervals(
        {0.0, 0.0}, {20.0, 0.0}, 50'000U);
    require(work_limited && work_limited.value() == 20U,
        "triangle-check budget can reduce the allowed segment length");
    require(!carto::studio::perspective_stroke_segment_intervals(
                {0.0, 0.0}, {21.0, 0.0}, 50'000U),
        "segment beyond the aggregate triangle-check budget is refused");
    require(!carto::studio::perspective_stroke_segment_intervals(
                {0.0, 0.0}, {1.0, 0.0}, 0U),
        "nonzero segment without visibility geometry is refused");
    require(!carto::studio::perspective_stroke_segment_intervals(
                {0.0, 0.0}, {std::numeric_limits<double>::infinity(), 0.0}, 1U),
        "non-finite segment coordinates are refused");
}

void perspective_edge_visibility_uses_reciprocal_depth() {
    const auto depth = carto::studio::perspective_edge_camera_depth(-10.0, -20.0, 0.5);
    require(depth && near(depth.value(), -13.333333333333334),
        "edge depth uses perspective-correct reciprocal interpolation");
    const auto occluded = carto::studio::perspective_edge_is_frontmost(
        depth.value(), -8.0);
    const auto visible = carto::studio::perspective_edge_is_frontmost(
        depth.value(), -14.0);
    require(occluded && !occluded.value(),
        "nearer surface blocks an edge behind it");
    require(visible && visible.value(),
        "edge in front of the sampled surface remains selectable");
    const auto overlay_visible = carto::studio::perspective_depth_is_frontmost(
        -7.0, -8.0);
    const auto overlay_occluded = carto::studio::perspective_depth_is_frontmost(
        -9.0, -8.0);
    require(overlay_visible && overlay_visible.value(),
        "perspective overlay in front of scene depth remains visible");
    require(overlay_occluded && !overlay_occluded.value(),
        "perspective overlay behind scene depth is hidden");
    require(!carto::studio::perspective_depth_is_frontmost(
                0.0, -8.0),
        "overlay points on or behind the camera are refused");
    require(!carto::studio::perspective_edge_camera_depth(-10.0, 0.0, 0.5),
        "zero/behind-camera endpoint depth is refused");
}

void perspective_workspace_disables_legacy_screen_picking() {
    require(!carto::studio::use_legacy_screen_pick(true, true),
        "perspective workspace cannot bypass the depth-aware pointer route");
    require(carto::studio::use_legacy_screen_pick(true, false),
        "plan workspace keeps the established screen-space route");
    require(carto::studio::use_legacy_screen_pick(false, true),
        "other desktop surfaces retain their independent pointer handling");
}

void loop_cut_pick_context_preserves_cancel_and_retry_boundaries() {
    using carto::studio::LoopCutPickDisposition;
    carto::studio::LoopCutPickContext context{
        true, true, true, true, true, true, false, true, true};
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::admit,
        "current unmodified model edge click reaches target admission");

    context.primary_press = false;
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::ignore,
        "non-primary and release events do not consume loop-cut input");
    context.primary_press = true;

    context.workspace_surface = false;
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::cancel,
        "leaving the workspace cancels the armed modeling pick");
    context.workspace_surface = true;
    context.interactive_hit_cache = false;
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::keep_armed,
        "a temporarily unavailable viewport keeps the pick armed");
    context.interactive_hit_cache = true;
    context.current_snapshot = false;
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::cancel,
        "a stale project snapshot cancels the armed pick");
    context.current_snapshot = true;
    context.speculative_preview = true;
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::keep_armed,
        "speculative preview refuses the click without changing project state");
    context.speculative_preview = false;
    context.unmodified_primary_click = false;
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::keep_armed,
        "modified selection clicks are refused while the tool remains armed");
    context.unmodified_primary_click = true;
    context.point_in_viewport = false;
    require(carto::studio::decide_loop_cut_pick_context(context).disposition ==
            LoopCutPickDisposition::keep_armed,
        "outside-viewport clicks do not dispatch a loop cut");
}

void loop_cut_pick_target_refuses_occluded_and_invalid_edges() {
    using carto::studio::LoopCutEdgePickStatus;
    using carto::studio::LoopCutPickDisposition;
    carto::studio::LoopCutPickTarget target{
        LoopCutEdgePickStatus::hit, true, true, false, true};
    const auto occluded = carto::studio::decide_loop_cut_pick_target(target);
    require(occluded.disposition == LoopCutPickDisposition::keep_armed &&
            occluded.message.find("frontmost") != std::string_view::npos,
        "an occluded perspective edge cannot dispatch loop cut and preserves retry");

    const std::string_view depth_failure = "depth sample unavailable";
    const auto incomplete_depth = carto::studio::decide_loop_cut_pick_target(
        target, depth_failure);
    require(incomplete_depth.disposition == LoopCutPickDisposition::keep_armed &&
            incomplete_depth.message == depth_failure,
        "incomplete depth evidence preserves the production diagnostic");

    target.frontmost = true;
    require(carto::studio::decide_loop_cut_pick_target(target).disposition ==
            LoopCutPickDisposition::admit,
        "frontmost exact edge with eligible owner is admitted");
    target.edge_source_found = false;
    require(carto::studio::decide_loop_cut_pick_target(target).disposition ==
            LoopCutPickDisposition::cancel,
        "missing perspective source edge cancels instead of guessing identity");
    target.edge_source_found = true;
    target.owner_eligible = false;
    require(carto::studio::decide_loop_cut_pick_target(target).disposition ==
            LoopCutPickDisposition::keep_armed,
        "hidden, locked, or non-mesh owners cannot dispatch loop cut");

    target.status = LoopCutEdgePickStatus::miss;
    target.owner_eligible = true;
    require(carto::studio::decide_loop_cut_pick_target(target).disposition ==
            LoopCutPickDisposition::keep_armed,
        "a miss remains retryable");
    target.status = LoopCutEdgePickStatus::ambiguous;
    require(carto::studio::decide_loop_cut_pick_target(target).disposition ==
            LoopCutPickDisposition::keep_armed,
        "ambiguous edges remain retryable and do not select arbitrarily");
    target.status = LoopCutEdgePickStatus::invalid;
    require(carto::studio::decide_loop_cut_pick_target(target).disposition ==
            LoopCutPickDisposition::cancel,
        "invalid hit evidence cancels the armed pick");
}

} // namespace

int main() {
    default_camera_has_stable_orthonormal_axes();
    navigation_is_bounded_and_deterministic();
    named_orientation_is_bounded_and_fail_closed();
    projection_switch_preserves_target_span();
    frame_and_serialization_preserve_editor_state();
    pick_rays_match_projection_conventions();
    projected_markers_are_viewport_local_and_fail_closed();
    camera_does_not_author_scene_mutation();
    viewport_capture_rejects_camera_changes();
    saved_scene_preview_binding_refuses_stale_context();
    saved_scene_preview_render_settings_require_exact_current_inputs();
    perspective_stroke_sampling_is_spatially_bounded();
    perspective_edge_visibility_uses_reciprocal_depth();
    perspective_workspace_disables_legacy_screen_picking();
    loop_cut_pick_context_preserves_cancel_and_retry_boundaries();
    loop_cut_pick_target_refuses_occluded_and_invalid_edges();
    std::cout << "editor camera tests passed\n";
    return EXIT_SUCCESS;
}
