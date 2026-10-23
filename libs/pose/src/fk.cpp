#include <carto/pose/fk.hpp>

#include <carto/pose/rotation.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace carto::pose {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Result<core::Vec3d> axis_unit_vector(character::Axis axis) {
    switch (axis) {
    case character::Axis::positive_x: return core::Result<core::Vec3d>::success({1.0, 0.0, 0.0});
    case character::Axis::negative_x: return core::Result<core::Vec3d>::success({-1.0, 0.0, 0.0});
    case character::Axis::positive_y: return core::Result<core::Vec3d>::success({0.0, 1.0, 0.0});
    case character::Axis::negative_y: return core::Result<core::Vec3d>::success({0.0, -1.0, 0.0});
    case character::Axis::positive_z: return core::Result<core::Vec3d>::success({0.0, 0.0, 1.0});
    case character::Axis::negative_z: return core::Result<core::Vec3d>::success({0.0, 0.0, -1.0});
    }
    return core::Result<core::Vec3d>::failure(invalid("coordinate axis is invalid"));
}

// Quaternion whose basis columns are (right, up, forward), via the classic
// trace branch; deterministic for a valid orthonormal basis.
core::Quaternion rotation_from_basis(core::Vec3d right, core::Vec3d up, core::Vec3d forward) {
    const double trace = right.x + up.y + forward.z;
    core::Quaternion q;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q = {(up.z - forward.y) / s, (forward.x - right.z) / s,
             (right.y - up.x) / s, s * 0.25};
    } else if (right.x > up.y && right.x > forward.z) {
        const double s = std::sqrt(1.0 + right.x - up.y - forward.z) * 2.0;
        q = {s * 0.25, (up.x + right.y) / s, (forward.x + right.z) / s,
             (up.z - forward.y) / s};
    } else if (up.y > forward.z) {
        const double s = std::sqrt(1.0 + up.y - right.x - forward.z) * 2.0;
        q = {(up.x + right.y) / s, s * 0.25, (forward.y + up.z) / s,
             (forward.x - right.z) / s};
    } else {
        const double s = std::sqrt(1.0 + forward.z - right.x - up.y) * 2.0;
        q = {(forward.x + right.z) / s, (forward.y + up.z) / s, s * 0.25,
             (right.y - up.x) / s};
    }
    const core::Quaternion normalized = q.normalized();
    return normalized.w >= 0.0
        ? normalized
        : core::Quaternion{-normalized.x, -normalized.y, -normalized.z, -normalized.w};
}

} // namespace

core::Result<core::Transform> policy_frame_transform(
    const character::CoordinatePolicy& policy) {
    if (auto result = policy.validate(); !result) {
        return core::Result<core::Transform>::failure(result.error());
    }
    if (!std::isfinite(policy.meters_per_unit) || policy.meters_per_unit <= 0.0) {
        return core::Result<core::Transform>::failure(invalid(
            "coordinate policy meters-per-unit must be finite and positive"));
    }
    const auto up = axis_unit_vector(policy.up);
    if (!up) return core::Result<core::Transform>::failure(up.error());
    const auto forward = axis_unit_vector(policy.forward);
    if (!forward) return core::Result<core::Transform>::failure(forward.error());
    const double alignment = dot(up.value(), forward.value());
    if (std::abs(alignment) > 1.0 - 1e-9) {
        return core::Result<core::Transform>::failure(validation(
            "coordinate policy up and forward axes must not be collinear"));
    }
    // Right-handed: right = up x forward (matches +Y up, +Z forward -> +X
    // right). Left-handed negates it.
    core::Vec3d right = cross(up.value(), forward.value());
    const bool left_handed = policy.handedness == character::Handedness::left;
    if (left_handed) {
        right = right * -1.0;
    }
    right = right.normalized();
    if (!right.finite()) {
        return core::Result<core::Transform>::failure(validation(
            "coordinate policy basis is degenerate"));
    }
    const double scale = policy.meters_per_unit;
    // A negated-right basis is a reflection, not a rotation: feeding it to
    // the quaternion trace branch silently collapses to the identity and the
    // left-handed frame would equal the right-handed one. Encode the flip as
    // a 180-degree turn about the up axis instead, and fold the matching
    // forward negation into the scale so transported points still land on
    // the declared axes (right negated, up and forward preserved).
    core::Quaternion frame_rotation =
        rotation_from_basis(right, up.value(), forward.value());
    core::Vec3d frame_scale{scale, scale, scale};
    if (left_handed) {
        frame_rotation =
            (axis_angle(up.value(), 3.14159265358979323846) * frame_rotation)
                .normalized();
        if (frame_rotation.w < 0.0) {
            frame_rotation = core::Quaternion{-frame_rotation.x, -frame_rotation.y,
                -frame_rotation.z, -frame_rotation.w};
        }
        frame_scale.z = -scale;
    }
    return core::Result<core::Transform>::success(core::Transform{
        {0.0, 0.0, 0.0},
        frame_rotation,
        frame_scale,
    });
}

core::Result<FkPose> evaluate_fk(
    const kinematic::KinematicGraph& graph,
    const LocalPose& locals,
    const std::optional<character::CoordinatePolicy>& policy) {
    if (auto result = graph.validate(); !result) {
        return core::Result<FkPose>::failure(result.error().with_context(
            "forward kinematics requires a valid kinematic graph"));
    }
    for (const auto& [joint, transform] : locals) {
        if (!joint || graph.find(joint) == nullptr) {
            return core::Result<FkPose>::failure(invalid(
                "local pose override references a joint outside the graph"));
        }
        if (!transform.finite()) {
            return core::Result<FkPose>::failure(invalid(
                "local pose override transform is not finite"));
        }
    }
    core::Transform root_frame = core::Transform::identity();
    if (policy.has_value()) {
        auto built = policy_frame_transform(policy.value());
        if (!built) return core::Result<FkPose>::failure(built.error());
        root_frame = built.value();
    }

    FkPose pose;
    pose.root_frame = root_frame;
    // Compose root-first via memoized recursion over the validated tree;
    // std::map iteration order keeps the composed output deterministically
    // sorted by joint id regardless of insertion history.
    std::function<core::Result<const core::Transform*>(kinematic::JointId)> compose;
    compose = [&](kinematic::JointId joint) -> core::Result<const core::Transform*> {
        const auto cached = pose.world.find(joint);
        if (cached != pose.world.end()) {
            return core::Result<const core::Transform*>::success(&cached->second);
        }
        const kinematic::KinematicNode* node = graph.find(joint);
        if (node == nullptr) {
            return core::Result<const core::Transform*>::failure(invalid(
                "kinematic hierarchy references a missing node"));
        }
        const auto local_override = locals.find(joint);
        const core::Transform& local = local_override != locals.end()
            ? local_override->second
            : node->rest;
        if (node->parent.has_value()) {
            auto parent_world = compose(node->parent.value());
            if (!parent_world) return core::Result<const core::Transform*>::failure(parent_world.error());
            const core::Transform world = parent_world.value()->combine(local);
            const auto inserted = pose.world.emplace(joint, world);
            return core::Result<const core::Transform*>::success(&inserted.first->second);
        }
        const core::Transform world = root_frame.combine(local);
        const auto inserted = pose.world.emplace(joint, world);
        return core::Result<const core::Transform*>::success(&inserted.first->second);
    };
    for (const auto& [joint, node] : graph.nodes()) {
        static_cast<void>(node);
        if (auto result = compose(joint); !result) {
            return core::Result<FkPose>::failure(result.error());
        }
    }
    return core::Result<FkPose>::success(std::move(pose));
}

core::Result<core::Transform> local_from_world(
    const core::Transform& parent_world,
    const core::Transform& world) {
    if (!parent_world.finite() || !world.finite()) {
        return core::Result<core::Transform>::failure(invalid(
            "world transforms must be finite to extract a local pose"));
    }
    const core::Vec3d parent_scale = parent_world.scale;
    if (std::abs(parent_scale.x - parent_scale.y) > 1e-12 ||
        std::abs(parent_scale.y - parent_scale.z) > 1e-12) {
        return core::Result<core::Transform>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "local extraction from a non-uniform-scale parent is unsupported"));
    }
    if (std::abs(parent_scale.x) <= 1e-12) {
        return core::Result<core::Transform>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "local extraction from a zero-scale parent is unsupported"));
    }
    const double inverse_scale = 1.0 / parent_scale.x;
    const core::Quaternion parent_rotation = parent_world.rotation.normalized();
    const core::Quaternion parent_inverse = inverse(parent_rotation);
    const core::Vec3d local_translation = parent_inverse.rotate(
        (world.translation - parent_world.translation) * inverse_scale);
    const core::Quaternion local_rotation =
        (parent_inverse * world.rotation.normalized()).normalized();
    return core::Result<core::Transform>::success(core::Transform{
        local_translation,
        local_rotation,
        world.scale * inverse_scale,
    });
}

} // namespace carto::pose
