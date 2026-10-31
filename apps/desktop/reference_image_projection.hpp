#pragma once

#include <carto/core/result.hpp>
#include <carto/project/presentation_catalog.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace carto::desktop {

struct ClippedReferenceImageVertex final {
    core::Vec3d world_position;
    core::Vec2d uv;
    double camera_depth = 0.0;
};

// Clips the world-space image quad against the perspective near plane. The
// signed camera depth convention is negative in front of the camera. UVs are
// interpolated along the same edge as the world position so partial quads keep
// their exact image mapping.
[[nodiscard]] inline core::Result<std::vector<ClippedReferenceImageVertex>>
clip_reference_image_plane_to_near(
    const project::ReferenceImagePlaneCorners& corners,
    const std::array<double, 4U>& camera_depths,
    double near_clip_depth) {
    using Result = core::Result<std::vector<ClippedReferenceImageVertex>>;
    if (!std::isfinite(near_clip_depth) || near_clip_depth >= 0.0) {
        return Result::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "reference-image near clip must be a finite front-of-camera depth"));
    }
    const std::array<core::Vec3d, 4U> positions{
        corners.upper_left, corners.upper_right,
        corners.lower_right, corners.lower_left};
    constexpr std::array<core::Vec2d, 4U> uvs{
        core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
        core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}};
    std::array<ClippedReferenceImageVertex, 4U> input{};
    for (std::size_t index = 0U; index < input.size(); ++index) {
        if (!positions[index].finite() || !std::isfinite(camera_depths[index])) {
            return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "reference-image quad has a non-finite corner or camera depth"));
        }
        input[index] = {positions[index], uvs[index], camera_depths[index]};
    }

    const auto inside = [near_clip_depth](
        const ClippedReferenceImageVertex& vertex) noexcept {
        return vertex.camera_depth <= near_clip_depth;
    };
    const auto same_vertex = [](const ClippedReferenceImageVertex& left,
                                const ClippedReferenceImageVertex& right) noexcept {
        constexpr double epsilon = 1.0e-14;
        return std::abs(left.world_position.x - right.world_position.x) <= epsilon &&
            std::abs(left.world_position.y - right.world_position.y) <= epsilon &&
            std::abs(left.world_position.z - right.world_position.z) <= epsilon &&
            std::abs(left.uv.x - right.uv.x) <= epsilon &&
            std::abs(left.uv.y - right.uv.y) <= epsilon &&
            std::abs(left.camera_depth - right.camera_depth) <= epsilon;
    };
    std::vector<ClippedReferenceImageVertex> clipped;
    clipped.reserve(6U);
    const auto append = [&clipped, &same_vertex](
        ClippedReferenceImageVertex vertex) {
        if (!clipped.empty() && same_vertex(clipped.back(), vertex)) return true;
        if (clipped.size() >= 6U) return false;
        clipped.push_back(std::move(vertex));
        return true;
    };
    ClippedReferenceImageVertex previous = input.back();
    bool previous_inside = inside(previous);
    for (const auto& current : input) {
        const bool current_inside = inside(current);
        if (previous_inside != current_inside) {
            const double denominator = current.camera_depth - previous.camera_depth;
            if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-15) {
                return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "reference-image near-plane edge cannot be interpolated safely"));
            }
            const double factor = std::clamp(
                (near_clip_depth - previous.camera_depth) / denominator, 0.0, 1.0);
            ClippedReferenceImageVertex intersection{
                previous.world_position +
                    (current.world_position - previous.world_position) * factor,
                {previous.uv.x + (current.uv.x - previous.uv.x) * factor,
                 previous.uv.y + (current.uv.y - previous.uv.y) * factor},
                near_clip_depth};
            if (!intersection.world_position.finite() || !intersection.uv.finite() ||
                !append(intersection)) {
                return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "reference-image near-plane intersection is invalid or exceeds its bound"));
            }
        }
        if (current_inside && !append(current)) {
            return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "reference-image near-plane polygon exceeds its vertex bound"));
        }
        previous = current;
        previous_inside = current_inside;
    }
    if (clipped.size() > 1U && same_vertex(clipped.front(), clipped.back())) {
        clipped.pop_back();
    }
    return Result::success(std::move(clipped));
}

// Resolves a world-space reference plane only for a viewport mode whose
// projection is implemented. Camera-pinned planes require the exact authored
// camera and scene transform supplied to the helper below.
[[nodiscard]] inline core::Result<project::ReferenceImagePlaneCorners>
reference_image_viewport_corners(
    const project::ReferenceImagePlane& plane,
    bool perspective_view) {
    if (auto valid = plane.validate(); !valid) {
        return core::Result<project::ReferenceImagePlaneCorners>::failure(
            valid.error().with_context("reference-image viewport projection"));
    }
    if (perspective_view) {
        if (plane.view == project::ReferencePlaneView::camera_pinned) {
            return core::Result<project::ReferenceImagePlaneCorners>::failure(
                core::Diagnostic(core::ErrorCode::unsupported,
                    "camera-pinned reference-image projection requires its authored camera transform"));
        }
    } else {
        const auto plan = project::reference_image_plane_xz_corners(plane);
        if (!plan) {
            return core::Result<project::ReferenceImagePlaneCorners>::failure(
                plan.error().with_context("reference-image plan viewport projection"));
        }
    }
    return project::reference_image_plane_corners(plane);
}

// Schema-v3 camera-local placement is transformed from the exact authored
// scene object. Legacy world-space pins use the ordinary world-plane resolver.
// Camera scale does not alter the image quad.
[[nodiscard]] inline core::Result<project::ReferenceImagePlaneCorners>
reference_image_camera_pinned_corners(
    const project::ReferenceImagePlane& plane,
    const project::AuthoredCamera& camera,
    const core::Transform& camera_world_transform) {
    if (auto valid = plane.validate(); !valid) {
        return core::Result<project::ReferenceImagePlaneCorners>::failure(
            valid.error().with_context("camera-pinned reference-image projection"));
    }
    if (auto valid = camera.validate(); !valid) {
        return core::Result<project::ReferenceImagePlaneCorners>::failure(
            valid.error().with_context("camera-pinned reference-image camera"));
    }
    if (plane.view != project::ReferencePlaneView::camera_pinned ||
        plane.pinned_camera != camera.object) {
        return core::Result<project::ReferenceImagePlaneCorners>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "reference image is not pinned to the supplied authored camera"));
    }
    if (!plane.camera_local_placement) {
        return core::Result<project::ReferenceImagePlaneCorners>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                "legacy camera-pinned reference image retains world-space placement"));
    }
    if (!camera_world_transform.finite()) {
        return core::Result<project::ReferenceImagePlaneCorners>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "authored camera world transform is not finite"));
    }

    // Camera-local axes use +X right, +Y up, and -Z forward. Camera scale does
    // not scale optical axes or the authored physical width of the image plane.
    const auto rotation = camera_world_transform.rotation.normalized();
    auto world_plane = plane;
    world_plane.origin = camera_world_transform.translation +
        rotation.rotate(plane.origin);
    world_plane.normal = rotation.rotate(plane.normal).normalized();
    world_plane.up = rotation.rotate(plane.up).normalized();
    return project::reference_image_plane_corners(world_plane);
}

} // namespace carto::desktop
