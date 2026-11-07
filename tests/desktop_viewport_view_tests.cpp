#include "studio_view_settings.hpp"

#include <carto/geometry/compiled_mesh.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

using carto::studio::ViewportColorMode;
using carto::studio::ViewportSpatialCapability;
using carto::studio::ViewportSpatialMode;
using carto::studio::ViewportViewKind;
using carto::studio::ViewportViewSettings;

constexpr double kEpsilon = 1.0e-9;

void require_near(double actual, double expected, double epsilon = 1.0e-9) {
    if (std::abs(actual - expected) > epsilon) {
        throw std::runtime_error("value mismatch: " + std::to_string(actual) +
            " vs " + std::to_string(expected));
    }
}

// --- View kinds, axes, labels ----------------------------------------------

void view_labels_cover_every_kind() {
    using carto::studio::view_kind_label;
    REQUIRE(view_kind_label(ViewportViewKind::perspective_3d) == "3D Perspective");
    REQUIRE(view_kind_label(ViewportViewKind::top_plan_xz) == "Top (Plan X/Z)");
    REQUIRE(view_kind_label(ViewportViewKind::front_xy) == "Front (X/Y)");
    REQUIRE(view_kind_label(ViewportViewKind::right_zy) == "Right (Z/Y)");
    REQUIRE(carto::studio::color_mode_label(ViewportColorMode::monochrome) ==
        "Black & white");
    REQUIRE(carto::studio::spatial_mode_label(ViewportSpatialMode::structure) ==
        "Structure");
}

void view_axes_match_projection_paths() {
    const auto plan = carto::studio::viewport_view_axes(ViewportViewKind::top_plan_xz);
    REQUIRE(plan.has_value());
    // Plan view: world X right, world Z up-screen flipped, height Y depth.
    REQUIRE(plan.value().screen_right.x == 1.0);
    REQUIRE(plan.value().screen_up.z == -1.0);
    REQUIRE(plan.value().view_direction.y == -1.0);

    const auto front = carto::studio::viewport_view_axes(ViewportViewKind::front_xy);
    REQUIRE(front.has_value());
    REQUIRE(front.value().view_direction.z == -1.0);

    const auto right = carto::studio::viewport_view_axes(ViewportViewKind::right_zy);
    REQUIRE(right.has_value());
    REQUIRE(right.value().screen_right.z == -1.0); // horizontal axis is world Z
    REQUIRE(right.value().screen_up.y == 1.0);
    REQUIRE(right.value().view_direction.x == -1.0);
}

void orthographic_presets_apply_existing_camera() {
    carto::editor::EditorCamera camera;
    camera.set_projection(carto::editor::EditorProjection::orthographic);
    REQUIRE(camera.projection() == carto::editor::EditorProjection::orthographic);
    REQUIRE(carto::studio::apply_orthographic_view_orientation(
        ViewportViewKind::front_xy, camera));
    require_near(camera.yaw(), 0.0);
    require_near(camera.pitch(), 0.0);

    REQUIRE(carto::studio::apply_orthographic_view_orientation(
        ViewportViewKind::right_zy, camera));
    require_near(camera.yaw(), -1.5707963267948966);
    require_near(camera.pitch(), 0.0);

    // Perspective and plan kinds never touch the editor camera.
    carto::editor::EditorCamera untouched;
    const auto before = untouched.serialize();
    REQUIRE(!carto::studio::apply_orthographic_view_orientation(
        ViewportViewKind::perspective_3d, untouched));
    REQUIRE(!carto::studio::apply_orthographic_view_orientation(
        ViewportViewKind::top_plan_xz, untouched));
    REQUIRE(untouched.serialize() == before);
}

// --- Camera pose retention ---------------------------------------------------

void perspective_pose_round_trips() {
    carto::editor::EditorCamera camera;
    carto::editor::WorkSurfaceCamera plan_camera;
    REQUIRE(camera.orbit(40.0, -15.0));
    REQUIRE(camera.pan(12.0, -6.0, 720.0));
    REQUIRE(camera.dolly(3.0));

    const auto pose = carto::studio::capture_viewport_view_pose(
        ViewportViewKind::perspective_3d, camera, plan_camera);
    REQUIRE(pose.has_value());
    REQUIRE(pose.value().editor_camera_state.has_value());

    carto::editor::EditorCamera live;
    REQUIRE(live.orbit(500.0, 200.0));
    const auto applied = carto::studio::apply_viewport_view_pose(
        ViewportViewKind::perspective_3d, pose.value(), live,
        plan_camera, {800.0, 600.0});
    REQUIRE(applied.has_value());
    REQUIRE(live.serialize() == camera.serialize());
}

void orthographic_pose_restores_through_projection_switch() {
    carto::editor::EditorCamera saved;
    saved.set_projection(carto::editor::EditorProjection::orthographic);
    REQUIRE(saved.set_orientation(-1.5707963267948966, 0.0)); // right Z/Y preset
    REQUIRE(saved.frame({1.0, 2.0, 3.0}, 4.0, 1.5));

    const auto pose = carto::studio::capture_viewport_view_pose(
        ViewportViewKind::right_zy, saved, carto::editor::WorkSurfaceCamera{});
    REQUIRE(pose.has_value());

    carto::editor::EditorCamera live; // starts perspective at the origin
    carto::editor::WorkSurfaceCamera plan_camera;
    const auto applied = carto::studio::apply_viewport_view_pose(
        ViewportViewKind::right_zy, pose.value(), live,
        plan_camera, {800.0, 600.0});
    REQUIRE(applied.has_value());
    REQUIRE(live.projection() == carto::editor::EditorProjection::orthographic);
    require_near(live.yaw(), -1.5707963267948966);
    require_near(live.target().x, 1.0);
    require_near(live.target().y, 2.0);
    require_near(live.target().z, 3.0);
}

void plan_pose_round_trips_through_frame() {
    carto::editor::WorkSurfaceCamera saved;
    REQUIRE(saved.zoom_at(2.0, {400.0, 300.0}, {0.0, 0.0}, {800.0, 600.0}));
    REQUIRE(saved.pan_pixels({37.0, -11.0}, 600.0));

    carto::editor::EditorCamera editor_camera;
    const auto pose = carto::studio::capture_viewport_view_pose(
        ViewportViewKind::top_plan_xz, editor_camera, saved);
    REQUIRE(pose.has_value());
    REQUIRE(pose.value().plan_target.has_value());
    REQUIRE(pose.value().plan_view_height.has_value());

    carto::editor::WorkSurfaceCamera live;
    const auto applied = carto::studio::apply_viewport_view_pose(
        ViewportViewKind::top_plan_xz, pose.value(),
        editor_camera, live, {800.0, 600.0});
    REQUIRE(applied.has_value());
    require_near(live.target().x, saved.target().x, 1.0e-6);
    require_near(live.target().y, saved.target().y, 1.0e-6);
    require_near(live.view_height(), saved.view_height(), 1.0e-6);
}

void invalid_poses_fail_closed() {
    carto::studio::ViewportViewPose pose;
    carto::editor::EditorCamera editor;
    carto::editor::WorkSurfaceCamera plan;

    // Missing editor pose.
    REQUIRE(!carto::studio::apply_viewport_view_pose(
        ViewportViewKind::perspective_3d, pose, editor, plan, {800.0, 600.0}));

    // Undecodable editor pose.
    pose.editor_camera_state = "not a camera";
    const auto bad_editor = carto::studio::apply_viewport_view_pose(
        ViewportViewKind::front_xy, pose, editor, plan, {800.0, 600.0});
    REQUIRE(!bad_editor);

    // Incomplete and out-of-bounds plan poses.
    carto::studio::ViewportViewPose plan_pose;
    plan_pose.plan_target = carto::core::Vec2d{1.0, 2.0};
    REQUIRE(!carto::studio::apply_viewport_view_pose(
        ViewportViewKind::top_plan_xz, plan_pose, editor, plan, {800.0, 600.0}));
    plan_pose.plan_view_height = 0.0; // below WorkSurfaceCamera::kMinViewHeight
    REQUIRE(!carto::studio::apply_viewport_view_pose(
        ViewportViewKind::top_plan_xz, plan_pose, editor, plan, {800.0, 600.0}));
    plan_pose.plan_view_height =
        std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!carto::studio::apply_viewport_view_pose(
        ViewportViewKind::top_plan_xz, plan_pose, editor, plan, {800.0, 600.0}));

    // Plan restoration rejects a zero-sized viewport.
    carto::studio::ViewportViewPose valid_plan;
    valid_plan.plan_target = carto::core::Vec2d{1.0, 2.0};
    valid_plan.plan_view_height = 10.0;
    REQUIRE(!carto::studio::apply_viewport_view_pose(
        ViewportViewKind::top_plan_xz, valid_plan, editor, plan, {0.0, 600.0}));
}

// --- Color mapping -----------------------------------------------------------

void color_mapping_transforms_actual_material_rgb() {
    const auto full = carto::studio::map_viewport_color(
        {0.25, 0.5, 0.75}, ViewportColorMode::full_color);
    REQUIRE(full.has_value());
    require_near(full.value().x, 0.25);
    require_near(full.value().y, 0.5);
    require_near(full.value().z, 0.75);

    // Rec. 709 luma of pure green.
    const auto green = carto::studio::map_viewport_color(
        {0.0, 1.0, 0.0}, ViewportColorMode::monochrome);
    REQUIRE(green.has_value());
    require_near(green.value().x, 0.7152);
    require_near(green.value().y, 0.7152);
    require_near(green.value().z, 0.7152);

    // A mid-gray stays equal in all channels.
    const auto gray = carto::studio::map_viewport_color(
        {0.4, 0.4, 0.4}, ViewportColorMode::monochrome);
    REQUIRE(gray.has_value());
    require_near(gray.value().x, gray.value().y);
    require_near(gray.value().y, gray.value().z);

    // Invalid displayed RGB is rejected, not silently remapped.
    REQUIRE(!carto::studio::map_viewport_color(
        {1.5, 0.0, 0.0}, ViewportColorMode::monochrome));
    REQUIRE(!carto::studio::map_viewport_color(
        {-0.1, 0.0, 0.0}, ViewportColorMode::monochrome));
    REQUIRE(!carto::studio::map_viewport_color(
        {std::numeric_limits<double>::infinity(), 0.0, 0.0},
        ViewportColorMode::full_color));
}

// --- Depth from real projected geometry ---------------------------------------

void plan_depth_is_world_height() {
    const auto depth = carto::studio::projected_view_depth(
        ViewportViewKind::top_plan_xz, carto::editor::EditorCamera{},
        carto::editor::WorkSurfaceCamera{}, {3.0, 7.5, -2.0});
    REQUIRE(depth.has_value());
    require_near(depth.value(), 7.5);
}

void perspective_depth_uses_camera_space_view_depth() {
    carto::editor::EditorCamera camera; // target origin, distance 10 on +Z
    const auto front = carto::studio::projected_view_depth(
        ViewportViewKind::perspective_3d, camera,
        carto::editor::WorkSurfaceCamera{}, {0.0, 0.0, 0.0});
    REQUIRE(front.has_value());
    require_near(front.value(), -10.0); // negated positive view depth

    const auto behind = carto::studio::projected_view_depth(
        ViewportViewKind::perspective_3d, camera,
        carto::editor::WorkSurfaceCamera{}, {0.0, 0.0, 20.0});
    REQUIRE(!behind);

    const auto non_finite = carto::studio::projected_view_depth(
        ViewportViewKind::perspective_3d, camera,
        carto::editor::WorkSurfaceCamera{},
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    REQUIRE(!non_finite);
}

void orthographic_depth_matches_camera_axes() {
    carto::editor::EditorCamera camera;
    camera.set_projection(carto::editor::EditorProjection::orthographic);
    REQUIRE(carto::studio::apply_orthographic_view_orientation(
        ViewportViewKind::front_xy, camera));
    const auto depth = carto::studio::projected_view_depth(
        ViewportViewKind::front_xy, camera, carto::editor::WorkSurfaceCamera{},
        {1.0, 2.0, 3.0});
    REQUIRE(depth.has_value());
    // Camera-space depth is negative in front of the camera, matching the
    // SculptViewportTriangle convention used by the studio hit cache.
    REQUIRE(depth.value() < 0.0);
}

void depth_normalization_is_bounded() {
    const auto middle = carto::studio::normalize_viewport_depth(5.0, 0.0, 10.0);
    REQUIRE(middle.has_value());
    require_near(middle.value(), 0.5);
    require_near(carto::studio::normalize_viewport_depth(-3.0, 0.0, 10.0).value(), 0.0);
    require_near(carto::studio::normalize_viewport_depth(30.0, 0.0, 10.0).value(), 1.0);

    REQUIRE(!carto::studio::normalize_viewport_depth(
        5.0, 10.0, 0.0)); // inverted bounds
    REQUIRE(!carto::studio::normalize_viewport_depth(
        5.0, 4.0, 4.0)); // empty span
    REQUIRE(!carto::studio::normalize_viewport_depth(
        std::numeric_limits<double>::quiet_NaN(), 0.0, 10.0));
    REQUIRE(!carto::studio::normalize_viewport_depth(
        5.0, 0.0, std::numeric_limits<double>::infinity()));
}

// --- Structure edges from real topology --------------------------------------

carto::geometry::CompiledMesh make_quad_mesh(bool with_edge_ids) {
    carto::geometry::CompiledMesh mesh;
    mesh.positions = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}};
    mesh.normals = {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0},
        {0.0, 0.0, 1.0}};
    mesh.indices = {0U, 1U, 2U, 0U, 2U, 3U};
    mesh.vertex_ids = {carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U},
        carto::geometry::VertexId{3U}, carto::geometry::VertexId{4U}};
    mesh.triangle_faces = {carto::geometry::FaceId{1U}, carto::geometry::FaceId{2U}};
    if (with_edge_ids) {
        // tri0 (v0,v1,v2): opposite-corner edges; the diagonal (v0,v2) shares
        // EdgeId 20 with tri1.
        mesh.triangle_edges = {{
            {{carto::geometry::EdgeId{10U}, carto::geometry::EdgeId{20U},
                carto::geometry::EdgeId{30U}}},
            {{carto::geometry::EdgeId{40U}, carto::geometry::EdgeId{50U},
                carto::geometry::EdgeId{20U}}},
        }};
    } else {
        mesh.triangle_edges = {{
            {{std::nullopt, std::nullopt, std::nullopt}},
            {{std::nullopt, std::nullopt, std::nullopt}},
        }};
    }
    mesh.bounds = carto::core::Bounds3d{{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}};
    mesh.source_revision = carto::core::Revision{1U};
    return mesh;
}

void structure_edges_dedupe_real_topology() {
    const auto edges = carto::studio::collect_viewport_structure_edges(
        make_quad_mesh(true));
    REQUIRE(edges.has_value());
    // Quad: 4 boundary edges + 1 shared diagonal, collected exactly once.
    REQUIRE(edges.value().size() == 5U);

    const auto fallback = carto::studio::collect_viewport_structure_edges(
        make_quad_mesh(false));
    REQUIRE(fallback.has_value());
    REQUIRE(fallback.value().size() == 5U);

    // Every reported edge endpoints come from actual vertex positions.
    bool found_diagonal = false;
    for (const auto& edge : edges.value()) {
        const bool is_unit = (edge.first - edge.second).length_squared() > 0.5;
        REQUIRE(is_unit);
        const bool is_diagonal =
            (edge.first.x == 0.0 && edge.first.y == 0.0 && edge.second.x == 1.0 &&
                edge.second.y == 1.0) ||
            (edge.second.x == 0.0 && edge.second.y == 0.0 && edge.first.x == 1.0 &&
                edge.first.y == 1.0);
        found_diagonal = found_diagonal || is_diagonal;
    }
    REQUIRE(found_diagonal);

    auto invalid = make_quad_mesh(true);
    invalid.indices[0U] = 99U; // out-of-range index makes the mesh invalid
    REQUIRE(!carto::studio::collect_viewport_structure_edges(invalid));
}

// --- Capability labels --------------------------------------------------------

void unsupported_modes_are_labeled_honestly() {
    REQUIRE(carto::studio::viewport_spatial_capability(
        ViewportSpatialMode::depth, true, false) ==
        ViewportSpatialCapability::available);
    REQUIRE(carto::studio::viewport_spatial_capability(
        ViewportSpatialMode::structure, false, false) ==
        ViewportSpatialCapability::unavailable_no_projected_geometry);
    // Shaded rendering does not depend on projected geometry snapshots.
    REQUIRE(carto::studio::viewport_spatial_capability(
        ViewportSpatialMode::shaded, false, false) ==
        ViewportSpatialCapability::available);

    const auto depth_readback = carto::studio::viewport_spatial_capability(
        ViewportSpatialMode::depth, true, true);
    REQUIRE(depth_readback ==
        ViewportSpatialCapability::unsupported_gpu_depth_readback);
    REQUIRE(carto::studio::spatial_capability_label(depth_readback) ==
        "Unsupported: GPU depth readback is not implemented");
    const auto texture_readback = carto::studio::viewport_spatial_capability(
        ViewportSpatialMode::shaded, true, true);
    REQUIRE(texture_readback ==
        ViewportSpatialCapability::unsupported_gpu_texture_readback);
}

// --- Codec --------------------------------------------------------------------

void codec_round_trips_settings() {
    ViewportViewSettings settings;
    settings.view = ViewportViewKind::right_zy;
    settings.color = ViewportColorMode::monochrome;
    settings.spatial = ViewportSpatialMode::depth;

    carto::editor::EditorCamera camera;
    carto::editor::WorkSurfaceCamera plan_camera;
    REQUIRE(camera.orbit(25.0, -10.0));
    const auto pose = carto::studio::capture_viewport_view_pose(
        ViewportViewKind::right_zy, camera, plan_camera);
    REQUIRE(pose.has_value());
    settings.pose = pose.value();

    const std::string encoded =
        carto::studio::encode_viewport_view_settings(settings);
    const auto decoded = carto::studio::decode_viewport_view_settings(encoded);
    REQUIRE(decoded.has_value());
    const auto& restored = decoded.value().settings;
    REQUIRE(restored.view == ViewportViewKind::right_zy);
    REQUIRE(restored.color == ViewportColorMode::monochrome);
    REQUIRE(restored.spatial == ViewportSpatialMode::depth);
    REQUIRE(restored.pose.editor_camera_state.has_value());
    const auto restored_camera =
        carto::editor::EditorCamera::deserialize(*restored.pose.editor_camera_state);
    REQUIRE(restored_camera.has_value());
    REQUIRE(restored_camera.value().serialize() == camera.serialize());
}

void codec_rejects_malformed_payloads() {
    using carto::studio::decode_viewport_view_settings;
    REQUIRE(!decode_viewport_view_settings("")); // empty
    REQUIRE(!decode_viewport_view_settings("version=2\n")); // unknown version
    REQUIRE(!decode_viewport_view_settings("view=perspective_3d\nversion=1\n"));
    REQUIRE(!decode_viewport_view_settings("version=1\nview=bogus\n"));
    REQUIRE(!decode_viewport_view_settings("version=1\ncolor=sepia\n"));
    REQUIRE(!decode_viewport_view_settings("version=1\nspatial=volumetric\n"));
    REQUIRE(!decode_viewport_view_settings("version=1\nmystery=1\n")); // unknown key
    REQUIRE(!decode_viewport_view_settings("version=1\nversion=1\n")); // duplicate
    REQUIRE(!decode_viewport_view_settings("version=1\nview=\n")); // empty value
    REQUIRE(!decode_viewport_view_settings("version=1\nnot-a-field\n")); // malformed
    REQUIRE(!decode_viewport_view_settings("version=1\neditor_camera=bogus\n"));
    REQUIRE(!decode_viewport_view_settings("version=1\nplan_pose=1 2\n"));
    REQUIRE(!decode_viewport_view_settings("version=1\nplan_pose=1 2 0\n"));
    REQUIRE(!decode_viewport_view_settings(
        "version=1\nplan_pose=1 2 3 4\n")); // trailing token

    const auto versioned = decode_viewport_view_settings("version=9\n");
    REQUIRE(!versioned);
    REQUIRE(versioned.error().code == carto::core::ErrorCode::version_mismatch);
}

void codec_enforces_size_limits() {
    std::string oversized = "version=1\n";
    oversized.append(carto::studio::kMaxViewportViewSettingsCodecBytes + 1U, 'x');
    const auto result =
        carto::studio::decode_viewport_view_settings(oversized);
    REQUIRE(!result);
    REQUIRE(result.error().code == carto::core::ErrorCode::unsupported);

    std::string many_lines;
    for (std::size_t index = 0U; index < 64U; ++index) {
        many_lines += "version=1\n";
    }
    REQUIRE(!carto::studio::decode_viewport_view_settings(many_lines));
}

void codec_defaults_and_recovery_wrapper() {
    const auto minimal = carto::studio::decode_viewport_view_settings("version=1\n");
    REQUIRE(minimal.has_value());
    REQUIRE(minimal.value().recovered_with_defaults);
    REQUIRE(minimal.value().settings.view == ViewportViewKind::perspective_3d);
    REQUIRE(minimal.value().settings.color == ViewportColorMode::full_color);
    REQUIRE(minimal.value().settings.spatial == ViewportSpatialMode::shaded);
    REQUIRE(minimal.value().settings.pose.empty());

    bool used_defaults = false;
    const auto recovered = carto::studio::viewport_view_settings_or_default(
        "version=1\nview=nonsense\n", used_defaults);
    REQUIRE(used_defaults);
    REQUIRE(recovered.view == ViewportViewKind::perspective_3d);

    const auto valid = carto::studio::viewport_view_settings_or_default(
        "version=1\nview=top_plan_xz\n", used_defaults);
    REQUIRE(!used_defaults);
    REQUIRE(valid.view == ViewportViewKind::top_plan_xz);
}

// Splits an ImGui zero-separated combo item string into its items.
std::vector<std::string_view> split_menu_items(const char* items) {
    std::vector<std::string_view> out;
    const char* cursor = items;
    while (*cursor != '\0') {
        const std::string_view item(cursor);
        out.push_back(item);
        cursor += item.size() + 1U;
    }
    return out;
}

void menu_item_strings_match_enum_contract() {
    // Blocker regression: combo labels and enum mapping must stay in lockstep
    // so a UI reorder cannot transpose the actual display modes. The spatial
    // list is ordered shaded/depth/structure to match the enum.
    const auto kinds = split_menu_items(carto::studio::kViewportViewKindMenuItems);
    REQUIRE(kinds.size() == 4U);
    REQUIRE(kinds[0] == "3D Perspective");
    REQUIRE(kinds[1] == "Top (Plan X/Z)");
    REQUIRE(kinds[2] == "Front (X/Y)");
    REQUIRE(kinds[3] == "Right (Z/Y)");
    for (std::size_t index = 0U; index < kinds.size(); ++index) {
        const auto kind = carto::studio::viewport_view_kind_from_menu_index(index);
        REQUIRE(kind.has_value());
        REQUIRE(carto::studio::view_kind_label(*kind) == kinds[index]);
    }
    REQUIRE(!carto::studio::viewport_view_kind_from_menu_index(4U).has_value());

    const auto colors = split_menu_items(carto::studio::kViewportColorModeMenuItems);
    REQUIRE(colors.size() == 2U);
    REQUIRE(colors[0] == "Full color");
    REQUIRE(colors[1] == "Black & white");
    for (std::size_t index = 0U; index < colors.size(); ++index) {
        const auto mode = carto::studio::viewport_color_mode_from_menu_index(index);
        REQUIRE(mode.has_value());
        REQUIRE(carto::studio::color_mode_label(*mode) == colors[index]);
    }
    REQUIRE(!carto::studio::viewport_color_mode_from_menu_index(2U).has_value());

    const auto spatial = split_menu_items(carto::studio::kViewportSpatialModeMenuItems);
    REQUIRE(spatial.size() == 3U);
    REQUIRE(spatial[0] == "Shaded");
    REQUIRE(spatial[1] == "Depth");
    REQUIRE(spatial[2] == "Structure");
    for (std::size_t index = 0U; index < spatial.size(); ++index) {
        const auto mode = carto::studio::viewport_spatial_mode_from_menu_index(index);
        REQUIRE(mode.has_value());
        REQUIRE(carto::studio::spatial_mode_label(*mode) == spatial[index]);
    }
    REQUIRE(!carto::studio::viewport_spatial_mode_from_menu_index(3U).has_value());
    // The exact enum order the indices must resolve to:
    REQUIRE(*carto::studio::viewport_spatial_mode_from_menu_index(0U) ==
        ViewportSpatialMode::shaded);
    REQUIRE(*carto::studio::viewport_spatial_mode_from_menu_index(1U) ==
        ViewportSpatialMode::depth);
    REQUIRE(*carto::studio::viewport_spatial_mode_from_menu_index(2U) ==
        ViewportSpatialMode::structure);
}

void orbit_contract_for_view_kinds() {
    REQUIRE(carto::studio::view_kind_allows_orbit(ViewportViewKind::perspective_3d));
    REQUIRE(!carto::studio::view_kind_allows_orbit(ViewportViewKind::top_plan_xz));
    REQUIRE(!carto::studio::view_kind_allows_orbit(ViewportViewKind::front_xy));
    REQUIRE(!carto::studio::view_kind_allows_orbit(ViewportViewKind::right_zy));
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[fail] " << name << ": " << error.what() << '\n';
        }
    };

    run("view_labels_cover_every_kind", view_labels_cover_every_kind);
    run("view_axes_match_projection_paths", view_axes_match_projection_paths);
    run("orthographic_presets_apply_existing_camera",
        orthographic_presets_apply_existing_camera);
    run("perspective_pose_round_trips", perspective_pose_round_trips);
    run("orthographic_pose_restores_through_projection_switch",
        orthographic_pose_restores_through_projection_switch);
    run("plan_pose_round_trips_through_frame", plan_pose_round_trips_through_frame);
    run("invalid_poses_fail_closed", invalid_poses_fail_closed);
    run("color_mapping_transforms_actual_material_rgb",
        color_mapping_transforms_actual_material_rgb);
    run("plan_depth_is_world_height", plan_depth_is_world_height);
    run("perspective_depth_uses_camera_space_view_depth",
        perspective_depth_uses_camera_space_view_depth);
    run("orthographic_depth_matches_camera_axes", orthographic_depth_matches_camera_axes);
    run("depth_normalization_is_bounded", depth_normalization_is_bounded);
    run("structure_edges_dedupe_real_topology", structure_edges_dedupe_real_topology);
    run("unsupported_modes_are_labeled_honestly",
        unsupported_modes_are_labeled_honestly);
    run("codec_round_trips_settings", codec_round_trips_settings);
    run("codec_rejects_malformed_payloads", codec_rejects_malformed_payloads);
    run("codec_enforces_size_limits", codec_enforces_size_limits);
    run("codec_defaults_and_recovery_wrapper", codec_defaults_and_recovery_wrapper);
    run("menu_item_strings_match_enum_contract", menu_item_strings_match_enum_contract);
    run("orbit_contract_for_view_kinds", orbit_contract_for_view_kinds);

    if (failures != 0) {
        std::cout << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all desktop viewport view tests passed\n";
    return 0;
}
