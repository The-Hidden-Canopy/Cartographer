#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/editor/snap_service.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/render/render_scene.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace carto::editor {

// A viewport hit is a read-only identity result. It is safe to pass from a
// CPU or GPU picker to a modal tool because the source revision is part of the
// value and must be checked again before authoring mutation.
struct ViewportHit final {
    scene::ObjectId object;
    std::optional<geometry::VertexId> vertex;
    std::optional<geometry::EdgeId> edge;
    std::optional<geometry::FaceId> face;
    core::Vec2d screen_position{};
    core::Vec3d world_position{};
    core::Vec3d world_normal{0.0, 0.0, 1.0};
    double depth = 0.0;
    bool front_facing = true;
    bool occluded = false;
    core::Revision source_revision;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class ViewportPickTarget {
    nearest_visible,
    object,
    component,
};

struct ViewportPickSettings final {
    ViewportPickTarget target = ViewportPickTarget::nearest_visible;
    bool select_through = false;
    bool include_backfaces = false;
    std::optional<double> minimum_depth;
    std::optional<double> maximum_depth;

    [[nodiscard]] core::Result<void> validate() const;
};

// CPU reference selection over the same revision-bound hit records consumed
// by a native identity picker. The caller supplies the hit order-independent
// of backend; the result is deterministic and never mutates authoring state.
[[nodiscard]] core::Result<std::optional<ViewportHit>> pick_viewport_hit(
    const std::vector<ViewportHit>& hits,
    core::Revision source_revision,
    const ViewportPickSettings& settings = {});

struct ViewportSelectionBox final {
    core::Vec2d first{};
    core::Vec2d second{};
    bool crossing = false;

    [[nodiscard]] core::Result<void> validate() const;
};

// Returns one deterministic hit per object in a box. Hits are currently point
// samples, so crossing and containment have the same point-in-box semantics;
// a higher-level shell can provide richer projected extents before dispatch.
// `select_through` keeps occluded component/object identities instead of
// reducing to the nearest visible hit.
[[nodiscard]] core::Result<std::vector<ViewportHit>> select_viewport_box(
    const std::vector<ViewportHit>& hits,
    core::Revision source_revision,
    const ViewportSelectionBox& box,
    const ViewportPickSettings& settings = {});

enum class ViewportInteractionPhase {
    idle,
    captured,
    dragging,
    completed,
    cancelled,
};

struct ViewportPointerSample final {
    core::Vec2d screen_position{};
    std::uint8_t modifiers = 0U;
    bool primary_button = true;
    core::Revision source_revision;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ViewportInteractionSnapshot final {
    ViewportInteractionPhase phase = ViewportInteractionPhase::idle;
    core::Revision source_revision;
    ViewportPointerSample start;
    ViewportPointerSample current;

    [[nodiscard]] core::Result<void> validate() const;
};

// A small backend-neutral drag-capture state machine. It owns no project
// references, emits no mutation, and is intentionally strict about revision
// changes so a document replacement cannot leave a modal tool operating on
// stale viewport coordinates.
class ViewportInteractionSession final {
public:
    [[nodiscard]] core::Result<void> begin(const ViewportPointerSample& sample);
    [[nodiscard]] core::Result<void> update(const ViewportPointerSample& sample);
    [[nodiscard]] core::Result<ViewportInteractionSnapshot> complete(
        const ViewportPointerSample& sample);
    [[nodiscard]] core::Result<void> cancel();

    [[nodiscard]] ViewportInteractionPhase phase() const noexcept { return state_.phase; }
    [[nodiscard]] bool active() const noexcept {
        return state_.phase == ViewportInteractionPhase::captured ||
            state_.phase == ViewportInteractionPhase::dragging;
    }
    [[nodiscard]] const ViewportInteractionSnapshot& snapshot() const noexcept { return state_; }

private:
    [[nodiscard]] core::Result<void> require_active(
        const ViewportPointerSample& sample) const;
    [[nodiscard]] core::Result<void> require_revision(
        const ViewportPointerSample& sample) const;

    ViewportInteractionSnapshot state_;
};

enum class ViewportTranslateAxis {
    world_x,
    world_z,
};

// Revision-bound preview for the native 2D work-surface translation handles.
// Until parent-local axis conversion exists, callers restrict it to root
// objects. It computes a candidate only; callers commit through the application
// command/journal path after rechecking the captured revision.
class ViewportTransformSession final {
public:
    [[nodiscard]] core::Result<void> begin(
        scene::ObjectId object,
        core::Transform source_transform,
        core::Revision source_revision,
        ViewportTranslateAxis axis,
        core::Vec2d pointer_start,
        double pixels_per_world_unit);
    [[nodiscard]] core::Result<core::Transform> preview(
        core::Vec2d pointer,
        core::Revision current_revision) const;
    [[nodiscard]] core::Result<core::Transform> complete(
        core::Vec2d pointer,
        core::Revision current_revision);
    [[nodiscard]] core::Result<void> cancel();

    [[nodiscard]] bool active() const noexcept { return state_.has_value(); }
    [[nodiscard]] scene::ObjectId object() const noexcept {
        return state_.has_value() ? state_->object : scene::ObjectId{};
    }
    [[nodiscard]] core::Revision source_revision() const noexcept {
        return state_.has_value() ? state_->source_revision : core::Revision{};
    }

private:
    struct State final {
        scene::ObjectId object;
        core::Transform source_transform;
        core::Revision source_revision;
        ViewportTranslateAxis axis = ViewportTranslateAxis::world_x;
        core::Vec2d pointer_start;
        double pixels_per_world_unit = 0.0;
    };

    std::optional<State> state_;
};

struct ViewportTransformSnapResult final {
    core::Transform transform = core::Transform::identity();
    std::optional<SnapCandidate> candidate;
};

// Applies the native plan-view snapping policy to a revision-bound preview.
// Ctrl/grid has precedence over Shift/geometry. Geometry targets are queried
// from the compiled viewport scene and may exclude the object being moved.
// This is still preview state; the caller must submit the final transform
// through the application command path.
[[nodiscard]] core::Result<ViewportTransformSnapResult>
snap_viewport_transform_preview(
    core::Transform preview,
    ViewportTranslateAxis axis,
    const render::RenderScene& scene,
    core::Revision source_revision,
    bool grid_snap,
    bool geometry_snap,
    SnapSession& session,
    std::optional<scene::ObjectId> excluded_object = std::nullopt);

// UI-only orthographic camera for Cartographer's current X/Z plan view. It
// does not represent a scene camera and cannot mutate project state.
class WorkSurfaceCamera final {
public:
    static constexpr double kMinViewHeight = 1.0e-6;
    static constexpr double kMaxViewHeight = 1.0e9;

    [[nodiscard]] core::Vec2d target() const noexcept { return target_; }
    [[nodiscard]] double view_height() const noexcept { return view_height_; }
    [[nodiscard]] bool valid() const noexcept;

    // Fits world extents into a viewport with bounded padding. Invalid input
    // leaves the current view unchanged.
    [[nodiscard]] bool frame(
        core::Vec2d centre,
        core::Vec2d world_extent,
        core::Vec2d viewport_extent,
        double padding = 1.12) noexcept;
    // A canvas drag moves the view with the pointer, not the model.
    [[nodiscard]] bool pan_pixels(
        core::Vec2d delta_pixels, double viewport_height_pixels) noexcept;
    // Positive steps zoom in while preserving the world point under the
    // pointer. Pointer and viewport coordinates are in the same screen space.
    [[nodiscard]] bool zoom_at(
        double steps,
        core::Vec2d pointer,
        core::Vec2d viewport_origin,
        core::Vec2d viewport_extent) noexcept;
    [[nodiscard]] core::Result<core::Vec2d> world_to_screen(
        core::Vec2d world,
        core::Vec2d viewport_origin,
        core::Vec2d viewport_extent) const;
    [[nodiscard]] core::Result<core::Vec2d> screen_to_world(
        core::Vec2d screen,
        core::Vec2d viewport_origin,
        core::Vec2d viewport_extent) const;

private:
    core::Vec2d target_{}; // X/Z represented as x/y.
    double view_height_ = 10.0;
};

} // namespace carto::editor
