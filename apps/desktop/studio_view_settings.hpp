#pragma once

// Presentation-only viewport view settings for the Cartographer studio dock.
// These helpers own view kind / color / spatial-representation selection and
// per-view camera-pose retention. They are UI state, never project or scene
// truth: nothing here touches .carto authoring data, selection, or the command
// path, so switching views cannot reset the scene or the selection.
//
// Rendering integration contract (all modes reuse existing camera types):
//   - perspective_3d renders through carto::editor::EditorCamera (perspective).
//   - top_plan_xz renders through carto::editor::WorkSurfaceCamera (world X/Z,
//     matching the existing studio plan view; view depth is world height Y).
//   - front_xy / right_zy render through carto::editor::EditorCamera with
//     EditorProjection::orthographic and the preset orientations from
//     viewport_view_axes(); no separate renderer mode is introduced.

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/editor/editor_camera.hpp>
#include <carto/editor/viewport_interaction.hpp>
#include <carto/geometry/compiled_mesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::studio {

enum class ViewportViewKind : std::uint8_t {
    perspective_3d,
    top_plan_xz,
    front_xy,
    right_zy,
};

enum class ViewportColorMode : std::uint8_t {
    full_color,
    monochrome,
};

// Shaded uses the existing material/lighting draw. Depth and structure are
// honest CPU re-projections of the same displayed geometry; they never claim
// GPU texture or depth-buffer readback the studio does not perform.
enum class ViewportSpatialMode : std::uint8_t {
    shaded,
    depth,
    structure,
};

[[nodiscard]] inline std::string_view view_kind_label(ViewportViewKind kind) noexcept {
    switch (kind) {
    case ViewportViewKind::perspective_3d: return "3D Perspective";
    case ViewportViewKind::top_plan_xz: return "Top (Plan X/Z)";
    case ViewportViewKind::front_xy: return "Front (X/Y)";
    case ViewportViewKind::right_zy: return "Right (Z/Y)";
    }
    return "Unknown view";
}

[[nodiscard]] inline std::string_view color_mode_label(ViewportColorMode mode) noexcept {
    switch (mode) {
    case ViewportColorMode::full_color: return "Full color";
    case ViewportColorMode::monochrome: return "Black & white";
    }
    return "Unknown color mode";
}

[[nodiscard]] inline std::string_view spatial_mode_label(ViewportSpatialMode mode) noexcept {
    switch (mode) {
    case ViewportSpatialMode::shaded: return "Shaded";
    case ViewportSpatialMode::depth: return "Depth";
    case ViewportSpatialMode::structure: return "Structure";
    }
    return "Unknown spatial mode";
}

// --- Menu ordering contract -----------------------------------------------
// The ImGui combo item strings below and the index mapping helpers are the
// single source of truth for menu order. They live next to the enums so a UI
// reorder cannot silently transpose modes (the spatial list is deliberately
// ordered shaded/depth/structure to match the enum, not alphabetically).
// Tests split the item strings and pin them against the index helpers.
inline constexpr char kViewportViewKindMenuItems[] =
    "3D Perspective\0Top (Plan X/Z)\0Front (X/Y)\0Right (Z/Y)\0";
inline constexpr char kViewportColorModeMenuItems[] =
    "Full color\0Black & white\0";
inline constexpr char kViewportSpatialModeMenuItems[] =
    "Shaded\0Depth\0Structure\0";

[[nodiscard]] inline constexpr std::optional<ViewportViewKind>
viewport_view_kind_from_menu_index(std::size_t index) noexcept {
    switch (index) {
    case 0U: return ViewportViewKind::perspective_3d;
    case 1U: return ViewportViewKind::top_plan_xz;
    case 2U: return ViewportViewKind::front_xy;
    case 3U: return ViewportViewKind::right_zy;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr std::optional<ViewportColorMode>
viewport_color_mode_from_menu_index(std::size_t index) noexcept {
    switch (index) {
    case 0U: return ViewportColorMode::full_color;
    case 1U: return ViewportColorMode::monochrome;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr std::optional<ViewportSpatialMode>
viewport_spatial_mode_from_menu_index(std::size_t index) noexcept {
    switch (index) {
    case 0U: return ViewportSpatialMode::shaded;
    case 1U: return ViewportSpatialMode::depth;
    case 2U: return ViewportSpatialMode::structure;
    }
    return std::nullopt;
}

// Only the perspective 3D view may orbit. The fixed orthographic presets
// (Front/Right) keep their authored orientation: rotating them would silently
// change a bounded 2D projection. UI callers hide or disable the orbit
// control for fixed views instead of routing input through it.
[[nodiscard]] inline constexpr bool view_kind_allows_orbit(
    ViewportViewKind kind) noexcept {
    return kind == ViewportViewKind::perspective_3d;
}

// World-space meaning of one 2D viewport for a view kind. All vectors are
// unit length: screen x grows along screen_right, screen y grows along
// screen_up, and view_direction points from the viewer into the scene.
struct ViewportViewAxes final {
    core::Vec3d screen_right{1.0, 0.0, 0.0};
    core::Vec3d screen_up{0.0, 1.0, 0.0};
    core::Vec3d view_direction{0.0, 0.0, -1.0};
};

[[nodiscard]] inline core::Result<ViewportViewAxes> viewport_view_axes(
    ViewportViewKind kind) noexcept {
    using Result = core::Result<ViewportViewAxes>;
    switch (kind) {
    case ViewportViewKind::perspective_3d:
        // Orbit camera: the operator owns orientation, so only the default
        // frame is described here.
        return Result::success({{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
            {0.0, 0.0, -1.0}});
    case ViewportViewKind::top_plan_xz:
        // Plan view maps world X to screen x and world Z to screen y with the
        // y axis flipped (WorkSurfaceCamera::world_to_screen), so +screen up
        // is -Z. Depth is world height Y.
        return Result::success({{1.0, 0.0, 0.0}, {0.0, 0.0, -1.0},
            {0.0, -1.0, 0.0}});
    case ViewportViewKind::front_xy:
        // Orthographic preset: yaw 0, pitch 0 looks along -Z with +X right.
        return Result::success({{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
            {0.0, 0.0, -1.0}});
    case ViewportViewKind::right_zy:
        // Orthographic preset: yaw -PI/2 looks along -X with -Z right and
        // +Y up, so the horizontal axis is world Z.
        return Result::success({{0.0, 0.0, -1.0}, {0.0, 1.0, 0.0},
            {-1.0, 0.0, 0.0}});
    }
    return Result::failure({core::ErrorCode::invalid_argument,
        "viewport view kind is not a supported value"});
}

// Applies the fixed orthographic orientation for plan-like 2D views. Returns
// false (camera unchanged) for the perspective kind or invalid input.
[[nodiscard]] inline bool apply_orthographic_view_orientation(
    ViewportViewKind kind,
    editor::EditorCamera& camera) noexcept {
    if (kind == ViewportViewKind::perspective_3d || kind == ViewportViewKind::top_plan_xz ||
        !camera.valid()) {
        return false;
    }
    if (kind == ViewportViewKind::front_xy) {
        return camera.set_orientation(0.0, 0.0);
    }
    return camera.set_orientation(-1.5707963267948966, 0.0); // -PI/2
}

// Per-view camera pose retention. Editor-based views persist the exact
// EditorCamera state string (see EditorCamera::serialize); the plan view
// persists its 2D target and view height. A default-constructed pose means
// "no saved pose"; consumers fall back to the live camera.
struct ViewportViewPose final {
    std::optional<std::string> editor_camera_state;
    std::optional<core::Vec2d> plan_target;
    std::optional<double> plan_view_height;

    [[nodiscard]] bool empty() const noexcept {
        return !editor_camera_state.has_value() && !plan_target.has_value() &&
            !plan_view_height.has_value();
    }
};

[[nodiscard]] inline core::Result<ViewportViewPose> capture_viewport_view_pose(
    ViewportViewKind kind,
    const editor::EditorCamera& editor_camera,
    const editor::WorkSurfaceCamera& plan_camera) {
    using Result = core::Result<ViewportViewPose>;
    ViewportViewPose pose;
    if (kind == ViewportViewKind::top_plan_xz) {
        if (!plan_camera.valid()) {
            return Result::failure({core::ErrorCode::invalid_state,
                "plan camera state is non-finite or outside its bounded domain"});
        }
        pose.plan_target = plan_camera.target();
        pose.plan_view_height = plan_camera.view_height();
        return Result::success(std::move(pose));
    }
    if (!editor_camera.valid()) {
        return Result::failure({core::ErrorCode::invalid_state,
            "editor camera state is non-finite or outside its bounded domain"});
    }
    pose.editor_camera_state = editor_camera.serialize();
    return Result::success(std::move(pose));
}

// Restores a captured pose into the live UI cameras. Only the two camera
// objects are written; scene and selection state are untouched, so switching
// views never resets the scene or the selection. The plan pose is restored
// through WorkSurfaceCamera::frame with an exact-span rectangle, so it is
// exact for the viewport extent passed by the caller.
[[nodiscard]] inline core::Result<void> apply_viewport_view_pose(
    ViewportViewKind kind,
    const ViewportViewPose& pose,
    editor::EditorCamera& editor_camera,
    editor::WorkSurfaceCamera& plan_camera,
    core::Vec2d viewport_extent) {
    using Result = core::Result<void>;
    if (kind == ViewportViewKind::top_plan_xz) {
        if (!pose.plan_target.has_value() || !pose.plan_view_height.has_value()) {
            return Result::failure({core::ErrorCode::not_found,
                "saved plan view pose is incomplete"});
        }
        const core::Vec2d target = *pose.plan_target;
        const double view_height = *pose.plan_view_height;
        if (!target.finite() || !std::isfinite(view_height) ||
            view_height < editor::WorkSurfaceCamera::kMinViewHeight ||
            view_height > editor::WorkSurfaceCamera::kMaxViewHeight) {
            return Result::failure({core::ErrorCode::invalid_argument,
                "saved plan view pose is non-finite or outside its bounded domain"});
        }
        if (!viewport_extent.finite() || viewport_extent.x <= 0.0 ||
            viewport_extent.y <= 0.0 || viewport_extent.x > 1.0e7 ||
            viewport_extent.y > 1.0e7) {
            return Result::failure({core::ErrorCode::invalid_argument,
                "plan pose restoration requires a finite positive bounded viewport"});
        }
        const double aspect = viewport_extent.x / viewport_extent.y;
        const core::Vec2d world_extent{view_height * aspect, view_height};
        if (!world_extent.finite()) {
            return Result::failure({core::ErrorCode::invalid_argument,
                "plan pose restoration produced non-finite world extents"});
        }
        if (!plan_camera.frame(target, world_extent, viewport_extent, 1.0)) {
            return Result::failure({core::ErrorCode::invalid_state,
                "plan camera rejected its saved pose"});
        }
        return Result::success();
    }
    if (!pose.editor_camera_state.has_value() || pose.editor_camera_state->empty()) {
        return Result::failure({core::ErrorCode::not_found,
            "saved editor view pose is missing"});
    }
    const auto restored = editor::EditorCamera::deserialize(*pose.editor_camera_state);
    if (!restored.has_value()) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "saved editor view pose could not be decoded"});
    }
    editor_camera = *restored;
    if (kind != ViewportViewKind::perspective_3d) {
        // Fixed 2D views always render through the orthographic path; the
        // pose span is preserved by set_projection.
        editor_camera.set_projection(editor::EditorProjection::orthographic);
        if (!apply_orthographic_view_orientation(kind, editor_camera) ||
            !editor_camera.valid()) {
            return Result::failure({core::ErrorCode::invalid_state,
                "saved editor view pose failed orthographic orientation"});
        }
    }
    return Result::success();
}

// ---------------------------------------------------------------------------
// Display mapping
// ---------------------------------------------------------------------------

inline constexpr double kViewportColorComponentMax = 1.0;
inline constexpr double kViewportColorComponentTolerance = 1.0e-9;

// Maps an actual displayed material RGB (linear 0..1, same values the studio
// feeds to its ImGui draw lists) to the RGB the view should draw. Monochrome
// is a real transform of the material color (Rec. 709 luma); it never
// replaces the material with a constant or a texture lookup.
[[nodiscard]] inline core::Result<core::Vec3d> map_viewport_color(
    core::Vec3d material_rgb,
    ViewportColorMode mode) noexcept {
    using Result = core::Result<core::Vec3d>;
    if (!material_rgb.finite() ||
        material_rgb.x < -kViewportColorComponentTolerance ||
        material_rgb.y < -kViewportColorComponentTolerance ||
        material_rgb.z < -kViewportColorComponentTolerance ||
        material_rgb.x > kViewportColorComponentMax + kViewportColorComponentTolerance ||
        material_rgb.y > kViewportColorComponentMax + kViewportColorComponentTolerance ||
        material_rgb.z > kViewportColorComponentMax + kViewportColorComponentTolerance) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "material RGB must be finite and within the displayed 0..1 range"});
    }
    if (mode == ViewportColorMode::full_color) {
        return Result::success(material_rgb);
    }
    const double clamped_r = std::clamp(material_rgb.x, 0.0, kViewportColorComponentMax);
    const double clamped_g = std::clamp(material_rgb.y, 0.0, kViewportColorComponentMax);
    const double clamped_b = std::clamp(material_rgb.z, 0.0, kViewportColorComponentMax);
    const double luma = 0.2126 * clamped_r + 0.7152 * clamped_g + 0.0722 * clamped_b;
    if (!std::isfinite(luma)) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "monochrome mapping produced a non-finite luma"});
    }
    const double gray = std::clamp(luma, 0.0, kViewportColorComponentMax);
    return Result::success({gray, gray, gray});
}

// Real view depth for one world point, using the exact projection path the
// studio already renders with. Convention matches SculptViewportTriangle:
// plan views report world height Y; editor views report the negated positive
// camera-space view depth (points behind the camera fail closed).
[[nodiscard]] inline core::Result<double> projected_view_depth(
    ViewportViewKind kind,
    const editor::EditorCamera& editor_camera,
    const editor::WorkSurfaceCamera& plan_camera,
    core::Vec3d world_point) {
    using Result = core::Result<double>;
    if (!world_point.finite()) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "view depth input point is non-finite"});
    }
    if (kind == ViewportViewKind::top_plan_xz) {
        if (!plan_camera.valid()) {
            return Result::failure({core::ErrorCode::invalid_state,
                "plan camera state is non-finite or outside its bounded domain"});
        }
        return Result::success(world_point.y);
    }
    if (!editor_camera.valid()) {
        return Result::failure({core::ErrorCode::invalid_state,
            "editor camera state is non-finite or outside its bounded domain"});
    }
    const double fallback_extent = 1024.0;
    const auto marker = editor_camera.project_marker(world_point,
        {fallback_extent, fallback_extent});
    if (!marker) {
        return Result::failure(marker.error());
    }
    if (!marker.value().in_front) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "view depth is undefined for a point behind the camera"});
    }
    return Result::success(-marker.value().view_depth);
}

// Validates finite depth-mapping bounds and normalizes a view depth into
// [0, 1]: 0 at near_depth, 1 at far_depth, clamped in between. Both bounds
// must be finite with far strictly greater than near.
[[nodiscard]] inline core::Result<double> normalize_viewport_depth(
    double view_depth,
    double near_depth,
    double far_depth) noexcept {
    using Result = core::Result<double>;
    if (!std::isfinite(view_depth) || !std::isfinite(near_depth) ||
        !std::isfinite(far_depth) || !(near_depth < far_depth)) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "depth normalization requires a finite depth and ordered finite bounds"});
    }
    const double span = far_depth - near_depth;
    if (!std::isfinite(span) || span <= 0.0) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "depth normalization bounds do not form a positive finite span"});
    }
    return Result::success(
        std::clamp((view_depth - near_depth) / span, 0.0, 1.0));
}

// ---------------------------------------------------------------------------
// Structure representation (actual mesh topology, not a depth buffer)
// ---------------------------------------------------------------------------

struct ViewportStructureEdge final {
    core::Vec3d first{};
    core::Vec3d second{};
};

inline constexpr std::size_t kMaxViewportStructureVertices = 1'000'000U;

// Collects the unique display edges of one compiled mesh from its real
// topology: triangle_edges identity when present (shared across triangles),
// otherwise the unordered vertex-index pair. The result is intended for line
// drawing in structure mode; it carries no depth claim of any kind.
[[nodiscard]] inline core::Result<std::vector<ViewportStructureEdge>>
collect_viewport_structure_edges(const geometry::CompiledMesh& mesh) {
    using Result = core::Result<std::vector<ViewportStructureEdge>>;
    if (!mesh.valid()) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "structure edges require a valid compiled mesh"});
    }
    if (mesh.positions.size() > kMaxViewportStructureVertices) {
        return Result::failure({core::ErrorCode::unsupported,
            "structure edges exceed the bounded vertex budget"});
    }
    struct VertexPairKey final {
        std::uint32_t first = 0U;
        std::uint32_t second = 0U;
        [[nodiscard]] constexpr bool operator<(const VertexPairKey& other) const noexcept {
            return first != other.first ? first < other.first : second < other.second;
        }
    };
    std::vector<ViewportStructureEdge> edges;
    std::map<geometry::EdgeId, std::size_t> edge_ids;
    std::map<VertexPairKey, std::size_t> vertex_pairs;
    const std::size_t triangle_count = mesh.triangle_faces.size();
    for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
        for (std::uint32_t corner = 0U; corner < 3U; ++corner) {
            const std::uint32_t a = mesh.indices[triangle * 3U + (corner + 1U) % 3U];
            const std::uint32_t b = mesh.indices[triangle * 3U + (corner + 2U) % 3U];
            const auto position_pair = [&mesh, a, b]() -> ViewportStructureEdge {
                return {mesh.positions[a], mesh.positions[b]};
            };
            const auto remember = [&edges](const std::size_t slot,
                                          const ViewportStructureEdge& edge) {
                if (slot == edges.size()) edges.push_back(edge);
            };
            if (const auto edge_id = mesh.triangle_edges[triangle][corner];
                edge_id.has_value()) {
                const auto [iterator, inserted] = edge_ids.emplace(
                    *edge_id, edges.size());
                if (inserted) remember(iterator->second, position_pair());
                continue;
            }
            const VertexPairKey key{std::min(a, b), std::max(a, b)};
            const auto [iterator, inserted] = vertex_pairs.emplace(
                key, edges.size());
            if (inserted) remember(iterator->second, position_pair());
        }
    }
    return Result::success(std::move(edges));
}

// ---------------------------------------------------------------------------
// Capability labels
// ---------------------------------------------------------------------------

// Honest availability of a spatial representation for the studio's real
// rendering path. Depth and structure are CPU re-projections of the displayed
// geometry and are always available for a live viewport; shaded GPU capture
// previews are a separate authored-camera feature and are never implied here.
enum class ViewportSpatialCapability : std::uint8_t {
    available,
    unavailable_no_projected_geometry,
    unsupported_gpu_depth_readback,
    unsupported_gpu_texture_readback,
};

[[nodiscard]] inline std::string_view spatial_capability_label(
    ViewportSpatialCapability capability) noexcept {
    switch (capability) {
    case ViewportSpatialCapability::available:
        return "Available (CPU projected geometry)";
    case ViewportSpatialCapability::unavailable_no_projected_geometry:
        return "Unavailable: no projected scene geometry";
    case ViewportSpatialCapability::unsupported_gpu_depth_readback:
        return "Unsupported: GPU depth readback is not implemented";
    case ViewportSpatialCapability::unsupported_gpu_texture_readback:
        return "Unsupported: GPU texture readback is not implemented";
    }
    return "Unknown capability";
}

// Resolves the honest capability for one spatial mode. `projected_geometry`
// is true when the viewport currently holds a projected mesh snapshot;
// `requesting_gpu_readback` must be true only when a caller explicitly asks
// for a GPU-derived mode, which this studio does not provide.
[[nodiscard]] inline ViewportSpatialCapability viewport_spatial_capability(
    ViewportSpatialMode mode,
    bool projected_geometry,
    bool requesting_gpu_readback = false) noexcept {
    if (requesting_gpu_readback) {
        return mode == ViewportSpatialMode::depth
            ? ViewportSpatialCapability::unsupported_gpu_depth_readback
            : ViewportSpatialCapability::unsupported_gpu_texture_readback;
    }
    if (!projected_geometry &&
        mode != ViewportSpatialMode::shaded) {
        return ViewportSpatialCapability::unavailable_no_projected_geometry;
    }
    return ViewportSpatialCapability::available;
}

// ---------------------------------------------------------------------------
// Bounded view-settings codec (presentation only, never .carto truth)
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kViewportViewSettingsCodecVersion = 1U;
inline constexpr std::size_t kMaxViewportViewSettingsCodecBytes = 4096U;
inline constexpr std::size_t kMaxViewportViewSettingsLines = 32U;

struct ViewportViewSettings final {
    ViewportViewKind view = ViewportViewKind::perspective_3d;
    ViewportColorMode color = ViewportColorMode::full_color;
    ViewportSpatialMode spatial = ViewportSpatialMode::shaded;
    ViewportViewPose pose;
};

[[nodiscard]] inline bool parse_viewport_view_kind(
    std::string_view text, ViewportViewKind& kind) noexcept {
    if (text == "perspective_3d") { kind = ViewportViewKind::perspective_3d; return true; }
    if (text == "top_plan_xz") { kind = ViewportViewKind::top_plan_xz; return true; }
    if (text == "front_xy") { kind = ViewportViewKind::front_xy; return true; }
    if (text == "right_zy") { kind = ViewportViewKind::right_zy; return true; }
    return false;
}

[[nodiscard]] inline bool parse_viewport_color_mode(
    std::string_view text, ViewportColorMode& mode) noexcept {
    if (text == "full_color") { mode = ViewportColorMode::full_color; return true; }
    if (text == "monochrome") { mode = ViewportColorMode::monochrome; return true; }
    return false;
}

[[nodiscard]] inline bool parse_viewport_spatial_mode(
    std::string_view text, ViewportSpatialMode& mode) noexcept {
    if (text == "shaded") { mode = ViewportSpatialMode::shaded; return true; }
    if (text == "depth") { mode = ViewportSpatialMode::depth; return true; }
    if (text == "structure") { mode = ViewportSpatialMode::structure; return true; }
    return false;
}

[[nodiscard]] inline std::string_view viewport_view_kind_token(
    ViewportViewKind kind) noexcept {
    switch (kind) {
    case ViewportViewKind::perspective_3d: return "perspective_3d";
    case ViewportViewKind::top_plan_xz: return "top_plan_xz";
    case ViewportViewKind::front_xy: return "front_xy";
    case ViewportViewKind::right_zy: return "right_zy";
    }
    return "perspective_3d";
}

[[nodiscard]] inline std::string_view viewport_color_mode_token(
    ViewportColorMode mode) noexcept {
    switch (mode) {
    case ViewportColorMode::full_color: return "full_color";
    case ViewportColorMode::monochrome: return "monochrome";
    }
    return "full_color";
}

[[nodiscard]] inline std::string_view viewport_spatial_mode_token(
    ViewportSpatialMode mode) noexcept {
    switch (mode) {
    case ViewportSpatialMode::shaded: return "shaded";
    case ViewportSpatialMode::depth: return "depth";
    case ViewportSpatialMode::structure: return "structure";
    }
    return "shaded";
}

// Encodes view settings as a bounded, versioned, line-oriented key=value
// text block for local UI persistence alongside the dock INI state.
[[nodiscard]] inline std::string encode_viewport_view_settings(
    const ViewportViewSettings& settings) {
    std::string output;
    const auto append = [&output](std::string_view key, std::string_view value) {
        output += key;
        output += '=';
        output += value;
        output += '\n';
    };
    append("version", "1");
    append("view", viewport_view_kind_token(settings.view));
    append("color", viewport_color_mode_token(settings.color));
    append("spatial", viewport_spatial_mode_token(settings.spatial));
    if (settings.pose.editor_camera_state.has_value()) {
        append("editor_camera", *settings.pose.editor_camera_state);
    }
    if (settings.pose.plan_target.has_value() && settings.pose.plan_view_height.has_value()) {
        std::string plan;
        plan += std::to_string(settings.pose.plan_target->x);
        plan += ' ';
        plan += std::to_string(settings.pose.plan_target->y);
        plan += ' ';
        plan += std::to_string(*settings.pose.plan_view_height);
        append("plan_pose", plan);
    }
    return output;
}

struct ViewportViewSettingsDecodeReport final {
    ViewportViewSettings settings;
    // Absent optional fields keep their defaults; malformed content is a hard
    // failure, so `recovered` is true only when a versioned payload carried
    // no usable fields at all (e.g. header plus empty body).
    bool recovered_with_defaults = false;
};

// Strict, bounded decoder. Rejects unknown versions, duplicate fields,
// unknown keys, malformed lines, and values outside finite camera bounds;
// absent fields fall back to the defaults in ViewportViewSettings.
[[nodiscard]] inline core::Result<ViewportViewSettingsDecodeReport>
decode_viewport_view_settings(std::string_view text) {
    using Result = core::Result<ViewportViewSettingsDecodeReport>;
    if (text.empty()) {
        return Result::failure({core::ErrorCode::invalid_argument,
            "view settings payload is empty"});
    }
    if (text.size() > kMaxViewportViewSettingsCodecBytes) {
        return Result::failure({core::ErrorCode::unsupported,
            "view settings payload exceeds its size bound"});
    }
    ViewportViewSettings settings;
    bool saw_version = false;
    bool saw_any_field = false;
    std::size_t lines = 0U;
    std::size_t cursor = 0U;
    const auto fail = [](std::string message) {
        return Result::failure({core::ErrorCode::invalid_argument, std::move(message)});
    };
    while (cursor < text.size()) {
        const std::size_t newline = text.find('\n', cursor);
        const std::size_t end = newline == std::string_view::npos
            ? text.size() : newline;
        const std::string_view line = text.substr(cursor, end - cursor);
        cursor = newline == std::string_view::npos ? text.size() : end + 1U;
        if (line.empty()) continue;
        if (++lines > kMaxViewportViewSettingsLines) {
            return Result::failure({core::ErrorCode::unsupported,
                "view settings payload exceeds its line bound"});
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos || separator == 0U) {
            return fail("view settings line is malformed");
        }
        const std::string_view key = line.substr(0U, separator);
        const std::string_view value = line.substr(separator + 1U);
        if (value.empty()) {
            return fail("view settings field has an empty value");
        }
        if (key == "version") {
            if (saw_version) {
                return fail("view settings version field is duplicated");
            }
            saw_version = true;
            if (value != "1") {
                return Result::failure({core::ErrorCode::version_mismatch,
                    "view settings payload has an unsupported version"});
            }
            continue;
        }
        if (!saw_version) {
            return fail("view settings version field must come first");
        }
        saw_any_field = true;
        if (key == "view") {
            if (!parse_viewport_view_kind(value, settings.view)) {
                return fail("view settings view value is unknown");
            }
            continue;
        }
        if (key == "color") {
            if (!parse_viewport_color_mode(value, settings.color)) {
                return fail("view settings color value is unknown");
            }
            continue;
        }
        if (key == "spatial") {
            if (!parse_viewport_spatial_mode(value, settings.spatial)) {
                return fail("view settings spatial value is unknown");
            }
            continue;
        }
        if (key == "editor_camera") {
            if (settings.pose.editor_camera_state.has_value()) {
                return fail("view settings editor_camera field is duplicated");
            }
            const auto camera = editor::EditorCamera::deserialize(value);
            if (!camera.has_value()) {
                return fail("view settings editor_camera pose is invalid");
            }
            settings.pose.editor_camera_state = std::string(value);
            continue;
        }
        if (key == "plan_pose") {
            if (settings.pose.plan_target.has_value()) {
                return fail("view settings plan_pose field is duplicated");
            }
            double x = 0.0;
            double y = 0.0;
            double height = 0.0;
            const std::string value_text(value);
            std::istringstream input(value_text);
            std::string extra;
            if (!(input >> x >> y >> height) || (input >> extra) ||
                !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(height) ||
                height < editor::WorkSurfaceCamera::kMinViewHeight ||
                height > editor::WorkSurfaceCamera::kMaxViewHeight) {
                return fail("view settings plan_pose value is malformed");
            }
            settings.pose.plan_target = core::Vec2d{x, y};
            settings.pose.plan_view_height = height;
            continue;
        }
        return fail("view settings field is unknown");
    }
    if (!saw_version) {
        return fail("view settings payload has no version field");
    }
    ViewportViewSettingsDecodeReport report;
    report.settings = std::move(settings);
    report.recovered_with_defaults = !saw_any_field;
    return Result::success(std::move(report));
}

// Convenience wrapper for local persistence: malformed or oversized payloads
// fall back to defaults with `used_defaults` set instead of failing closed;
// callers that need diagnostics should use decode_viewport_view_settings.
[[nodiscard]] inline ViewportViewSettings viewport_view_settings_or_default(
    std::string_view text,
    bool& used_defaults) noexcept {
    used_defaults = false;
    const auto decoded = decode_viewport_view_settings(text);
    if (!decoded) {
        used_defaults = true;
        return {};
    }
    if (decoded.value().recovered_with_defaults) used_defaults = true;
    return decoded.value().settings;
}

} // namespace carto::studio
