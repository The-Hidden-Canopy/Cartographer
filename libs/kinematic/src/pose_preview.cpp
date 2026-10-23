#include <carto/kinematic/pose_preview.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace carto::kinematic {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

} // namespace

core::Result<HingePosePreview> preview_hinge_pose(
    const KinematicGraph& graph,
    const JointLimitSet& limits,
    JointId joint,
    double normalized_travel) {
    if (!std::isfinite(normalized_travel) || normalized_travel < 0.0 ||
        normalized_travel > 1.0) {
        return core::Result<HingePosePreview>::failure(invalid(
            "hinge preview travel must be finite and within [0, 1]"));
    }
    if (auto valid = graph.validate(); !valid) {
        return core::Result<HingePosePreview>::failure(valid.error().with_context(
            "hinge pose preview graph"));
    }
    if (auto valid = limits.validate_for_graph(graph); !valid) {
        return core::Result<HingePosePreview>::failure(valid.error().with_context(
            "hinge pose preview limits"));
    }
    const auto* node = graph.find(joint);
    if (node == nullptr) {
        return core::Result<HingePosePreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "hinge pose preview joint does not exist"));
    }
    if (node->kind != JointKind::hinge) {
        return core::Result<HingePosePreview>::failure(unsupported(
            "editor pose preview currently supports authored hinge joints only"));
    }
    const auto* limit = limits.find(joint);
    if (limit == nullptr || !limit->angular_enabled) {
        return core::Result<HingePosePreview>::failure(unsupported(
            "hinge pose preview requires authored angular travel limits"));
    }
    if (limit->angular_min.y != 0.0 || limit->angular_max.y != 0.0 ||
        limit->angular_min.z != 0.0 || limit->angular_max.z != 0.0) {
        return core::Result<HingePosePreview>::failure(unsupported(
            "editor hinge preview currently uses the authored joint-local X axis"));
    }

    std::vector<const KinematicNode*> ancestry;
    const KinematicNode* ancestor = node;
    while (ancestor != nullptr) {
        ancestry.push_back(ancestor);
        ancestor = ancestor->parent.has_value() ? graph.find(*ancestor->parent) : nullptr;
    }
    std::reverse(ancestry.begin(), ancestry.end());
    core::Transform joint_frame = core::Transform::identity();
    for (const auto* part : ancestry) joint_frame = joint_frame.combine(part->rest);
    const core::Vec3d axis = joint_frame.rotation.rotate({1.0, 0.0, 0.0}).normalized();
    if (!joint_frame.finite() || !axis.finite()) {
        return core::Result<HingePosePreview>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "hinge pose preview produced a non-finite joint frame"));
    }
    const double angle = limit->angular_min.x +
        (limit->angular_max.x - limit->angular_min.x) * normalized_travel;
    return core::Result<HingePosePreview>::success(
        HingePosePreview{joint, joint_frame.translation, axis, angle});
}

core::Result<std::map<JointId, core::Transform>> preview_chain_poses(
    const KinematicGraph& graph,
    const JointLimitSet& limits,
    const std::map<JointId, double>& normalized_travel) {
    for (const auto& [joint, travel] : normalized_travel) {
        static_cast<void>(joint);
        if (!std::isfinite(travel) || travel < 0.0 || travel > 1.0) {
            return core::Result<std::map<JointId, core::Transform>>::failure(
                invalid(
                    "chain pose preview travel must be finite and within [0, 1]"));
        }
    }
    if (auto valid = graph.validate(); !valid) {
        return core::Result<std::map<JointId, core::Transform>>::failure(
            valid.error().with_context("chain pose preview graph"));
    }
    if (auto valid = limits.validate_for_graph(graph); !valid) {
        return core::Result<std::map<JointId, core::Transform>>::failure(
            valid.error().with_context("chain pose preview limits"));
    }
    for (const auto& [joint, travel] : normalized_travel) {
        static_cast<void>(travel);
        if (graph.find(joint) == nullptr) {
            return core::Result<std::map<JointId, core::Transform>>::failure(
                core::Diagnostic(core::ErrorCode::not_found,
                    "chain pose preview control references a joint missing "
                    "from the graph"));
        }
    }

    const KinematicNode* root = nullptr;
    std::map<JointId, std::vector<JointId>> children;
    for (const auto& [identity, node] : graph.nodes()) {
        if (!node.parent.has_value()) {
            root = &node;
        } else {
            children[*node.parent].push_back(identity);
        }
    }
    if (root == nullptr) {
        return core::Result<std::map<JointId, core::Transform>>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "chain pose preview graph has no root"));
    }

    auto travel_for = [&normalized_travel](JointId joint) {
        const auto found = normalized_travel.find(joint);
        return found == normalized_travel.end() ? 0.0 : found->second;
    };
    auto motion_for = [&limits, &travel_for](const KinematicNode& node)
        -> core::Result<core::Transform> {
        const double travel = travel_for(node.id);
        const auto* limit = limits.find(node.id);
        switch (node.kind) {
        case JointKind::fixed:
            return core::Result<core::Transform>::success(
                core::Transform::identity());
        case JointKind::hinge: {
            if (limit == nullptr || !limit->angular_enabled) {
                return core::Result<core::Transform>::failure(unsupported(
                    "chain pose preview hinge requires authored angular "
                    "travel limits"));
            }
            if (limit->angular_min.y != 0.0 || limit->angular_max.y != 0.0 ||
                limit->angular_min.z != 0.0 || limit->angular_max.z != 0.0) {
                return core::Result<core::Transform>::failure(unsupported(
                    "chain pose preview hinge currently uses the authored "
                    "joint-local X axis"));
            }
            const double half =
                (limit->angular_min.x +
                 (limit->angular_max.x - limit->angular_min.x) * travel) *
                0.5;
            core::Transform motion;
            motion.rotation = {std::sin(half), 0.0, 0.0, std::cos(half)};
            return core::Result<core::Transform>::success(motion);
        }
        case JointKind::slider: {
            if (limit == nullptr || !limit->linear_enabled) {
                return core::Result<core::Transform>::failure(unsupported(
                    "chain pose preview slider requires authored linear "
                    "travel limits"));
            }
            core::Transform motion;
            motion.translation = limit->linear_min +
                (limit->linear_max - limit->linear_min) * travel;
            return core::Result<core::Transform>::success(motion);
        }
        case JointKind::ball:
            return core::Result<core::Transform>::failure(unsupported(
                "chain pose preview does not invent an orientation "
                "convention for ball joints"));
        }
        return core::Result<core::Transform>::failure(unsupported(
            "chain pose preview joint kind is not supported"));
    };

    std::map<JointId, core::Transform> poses;
    std::vector<JointId> pending{root->id};
    while (!pending.empty()) {
        const JointId current = pending.front();
        pending.erase(pending.begin());
        const KinematicNode& node = *graph.find(current);
        core::Transform pose = node.parent.has_value()
            ? poses.at(*node.parent).combine(node.rest)
            : node.rest;
        if (auto motion = motion_for(node); !motion) {
            return core::Result<std::map<JointId, core::Transform>>::failure(
                motion.error());
        } else {
            pose = pose.combine(motion.value());
        }
        if (!pose.finite() || !pose.rotation.normalizable()) {
            return core::Result<std::map<JointId, core::Transform>>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "chain pose preview produced a non-finite joint frame"));
        }
        poses.emplace(current, pose);
        for (const JointId child : children[current]) pending.push_back(child);
    }
    if (poses.size() != graph.nodes().size()) {
        return core::Result<std::map<JointId, core::Transform>>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "chain pose preview graph is not a single rooted tree"));
    }
    return core::Result<std::map<JointId, core::Transform>>::success(
        std::move(poses));
}

} // namespace carto::kinematic
