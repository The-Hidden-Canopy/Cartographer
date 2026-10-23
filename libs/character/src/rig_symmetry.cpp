#include <carto/character/rig_symmetry.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic missing(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

// Symmetric joint pairing: left->right and right->left. Refuses a joint used
// by two pairs.
core::Result<std::map<kinematic::JointId, kinematic::JointId>> pairing_of(
    const kinematic::KinematicGraph& graph,
    std::span<const SymmetryPair> pairs) {
    if (pairs.size() > RigDefinition::kMaxSymmetryPairs) {
        return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::failure(
            invalid("rig symmetry exceeds its pair bound"));
    }
    std::map<kinematic::JointId, kinematic::JointId> mirror;
    for (const SymmetryPair& pair : pairs) {
        if (auto result = pair.validate(); !result) {
            return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::failure(
                result.error());
        }
        if (graph.find(pair.left_joint) == nullptr || graph.find(pair.right_joint) == nullptr) {
            return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::failure(
                missing("rig symmetry pair names a joint absent from the graph"));
        }
        if (!mirror.emplace(pair.left_joint, pair.right_joint).second ||
            !mirror.emplace(pair.right_joint, pair.left_joint).second) {
            return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::failure(
                invalid("a joint may belong to only one symmetry pair"));
        }
    }
    return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::success(
        std::move(mirror));
}

kinematic::JointId mirrored_joint(
    const std::map<kinematic::JointId, kinematic::JointId>& mirror,
    kinematic::JointId joint) {
    const auto found = mirror.find(joint);
    return found == mirror.end() ? joint : found->second;
}

bool parents_mirror(
    const std::map<kinematic::JointId, kinematic::JointId>& mirror,
    const kinematic::KinematicNode& source,
    const kinematic::KinematicNode& destination) {
    if (source.parent.has_value() != destination.parent.has_value()) return false;
    if (!source.parent.has_value()) return true;
    return mirrored_joint(mirror, *source.parent) == *destination.parent;
}

double rotation_difference(const core::Quaternion& first, const core::Quaternion& second) {
    const core::Quaternion a = first.normalized();
    const core::Quaternion b = second.normalized();
    const double dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0 * std::acos(std::clamp(dot, 0.0, 1.0));
}

// Symmetric control pairing: left->right and right->left. Refuses a control
// used by two pairs. Bounded by the graph's own control capacity.
core::Result<std::map<ControlId, ControlId>> control_pairing_of(
    const ControlGraph& graph,
    std::span<const ControlSymmetryPair> pairs) {
    if (pairs.size() > ControlGraph::kMaxControls) {
        return core::Result<std::map<ControlId, ControlId>>::failure(
            invalid("control symmetry exceeds its pair bound"));
    }
    std::map<ControlId, ControlId> mirror;
    for (const ControlSymmetryPair& pair : pairs) {
        if (auto result = pair.validate(); !result) {
            return core::Result<std::map<ControlId, ControlId>>::failure(
                result.error());
        }
        if (graph.find(pair.left_control) == nullptr ||
            graph.find(pair.right_control) == nullptr) {
            return core::Result<std::map<ControlId, ControlId>>::failure(
                missing("control symmetry pair names a control absent from the graph"));
        }
        if (!mirror.emplace(pair.left_control, pair.right_control).second ||
            !mirror.emplace(pair.right_control, pair.left_control).second) {
            return core::Result<std::map<ControlId, ControlId>>::failure(
                invalid("a control may belong to only one symmetry pair"));
        }
    }
    return core::Result<std::map<ControlId, ControlId>>::success(
        std::move(mirror));
}

ControlId mirrored_control(
    const std::map<ControlId, ControlId>& mirror,
    ControlId control) {
    const auto found = mirror.find(control);
    return found == mirror.end() ? control : found->second;
}

bool same_vec3(const core::Vec3d& first, const core::Vec3d& second) {
    return first.x == second.x && first.y == second.y && first.z == second.z;
}

bool same_quaternion(const core::Quaternion& first, const core::Quaternion& second) {
    return first.x == second.x && first.y == second.y &&
        first.z == second.z && first.w == second.w;
}

bool same_control(const ControlDefinition& first, const ControlDefinition& second) {
    if (first.id != second.id || first.shape != second.shape ||
        first.allowed_axes != second.allowed_axes ||
        first.follow_space != second.follow_space ||
        first.control_set != second.control_set ||
        first.affected_joints != second.affected_joints) {
        return false;
    }
    if (!same_vec3(first.transform.translation, second.transform.translation) ||
        !same_quaternion(first.transform.rotation, second.transform.rotation) ||
        !same_vec3(first.transform.scale, second.transform.scale)) {
        return false;
    }
    for (std::size_t axis = 0U; axis < kAxisCount; ++axis) {
        if (first.axis_minimum[axis] != second.axis_minimum[axis] ||
            first.axis_maximum[axis] != second.axis_maximum[axis]) {
            return false;
        }
    }
    return true;
}

} // namespace

core::Result<void> SymmetryTolerance::validate() const {
    if (!std::isfinite(translation) || translation < 0.0 ||
        !std::isfinite(rotation) || rotation < 0.0) {
        return core::Result<void>::failure(invalid(
            "symmetry tolerances must be finite and non-negative"));
    }
    return core::Result<void>::success();
}

core::Transform reflect_local_transform(
    const core::Transform& transform,
    SymmetryAxis axis) noexcept {
    core::Transform mirrored = transform;
    core::Quaternion& q = mirrored.rotation;
    switch (axis) {
    case SymmetryAxis::x:
        mirrored.translation.x = -mirrored.translation.x;
        q = {q.x, -q.y, -q.z, q.w};
        break;
    case SymmetryAxis::y:
        mirrored.translation.y = -mirrored.translation.y;
        q = {-q.x, q.y, -q.z, q.w};
        break;
    case SymmetryAxis::z:
        mirrored.translation.z = -mirrored.translation.z;
        q = {-q.x, -q.y, q.z, q.w};
        break;
    }
    return mirrored;
}

core::Result<SymmetryReport> analyze_rig_symmetry(
    const kinematic::KinematicGraph& graph,
    std::span<const SymmetryPair> pairs,
    SymmetryAxis axis,
    const SymmetryTolerance& tolerance) {
    if (auto result = tolerance.validate(); !result) {
        return core::Result<SymmetryReport>::failure(result.error());
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<SymmetryReport>::failure(result.error());
    }
    const auto mirror = pairing_of(graph, pairs);
    if (!mirror) return core::Result<SymmetryReport>::failure(mirror.error());
    SymmetryReport report;
    report.pair_count = pairs.size();
    for (const SymmetryPair& pair : pairs) {
        const kinematic::KinematicNode& left = *graph.find(pair.left_joint);
        const kinematic::KinematicNode& right = *graph.find(pair.right_joint);
        const core::Transform expected = reflect_local_transform(left.rest, axis);
        SymmetryDivergence divergence;
        divergence.left = pair.left_joint;
        divergence.right = pair.right_joint;
        divergence.translation_error = (right.rest.translation - expected.translation).length();
        divergence.rotation_error = rotation_difference(right.rest.rotation, expected.rotation);
        divergence.parent_mismatch = !parents_mirror(mirror.value(), left, right);
        if (divergence.parent_mismatch ||
            divergence.translation_error > tolerance.translation ||
            divergence.rotation_error > tolerance.rotation) {
            report.divergent.push_back(divergence);
        }
    }
    return core::Result<SymmetryReport>::success(std::move(report));
}

core::Result<RigMirrorResult> mirror_rig_rest(
    const kinematic::KinematicGraph& graph,
    std::span<const SymmetryPair> pairs,
    SymmetryAxis axis,
    SymmetryDirection direction) {
    if (axis != SymmetryAxis::x && axis != SymmetryAxis::y && axis != SymmetryAxis::z) {
        return core::Result<RigMirrorResult>::failure(invalid("symmetry axis is not supported"));
    }
    if (direction != SymmetryDirection::left_to_right &&
        direction != SymmetryDirection::right_to_left) {
        return core::Result<RigMirrorResult>::failure(invalid(
            "symmetry direction is not supported"));
    }
    if (pairs.empty()) {
        return core::Result<RigMirrorResult>::failure(invalid(
            "rig mirror requires at least one symmetry pair"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<RigMirrorResult>::failure(result.error());
    }
    const auto mirror = pairing_of(graph, pairs);
    if (!mirror) return core::Result<RigMirrorResult>::failure(mirror.error());

    std::map<kinematic::JointId, core::Transform> rewritten;
    for (const SymmetryPair& pair : pairs) {
        const bool forward = direction == SymmetryDirection::left_to_right;
        const kinematic::JointId source_id = forward ? pair.left_joint : pair.right_joint;
        const kinematic::JointId destination_id = forward ? pair.right_joint : pair.left_joint;
        const kinematic::KinematicNode& source = *graph.find(source_id);
        const kinematic::KinematicNode& destination = *graph.find(destination_id);
        if (!parents_mirror(mirror.value(), source, destination)) {
            return core::Result<RigMirrorResult>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "rig mirror destination parent is not the mirror of its source parent")
                .with_context("joint=" + std::to_string(destination_id.value)));
        }
        rewritten.emplace(destination_id, reflect_local_transform(source.rest, axis));
    }

    kinematic::KinematicGraph result;
    std::size_t changed = 0U;
    for (const auto& [id, node] : graph.nodes()) {
        kinematic::KinematicNode copy = node;
        const auto replacement = rewritten.find(id);
        if (replacement != rewritten.end()) {
            const core::Transform& next = replacement->second;
            const core::Quaternion& q = next.rotation;
            const core::Quaternion& r = copy.rest.rotation;
            if ((next.translation - copy.rest.translation).length() > 0.0 ||
                q.x != r.x || q.y != r.y || q.z != r.z || q.w != r.w ||
                (next.scale - copy.rest.scale).length() > 0.0) {
                ++changed;
            }
            copy.rest = next;
        }
        if (auto inserted = result.insert_node(std::move(copy)); !inserted) {
            return core::Result<RigMirrorResult>::failure(inserted.error());
        }
    }
    if (auto valid = result.validate(); !valid) {
        return core::Result<RigMirrorResult>::failure(valid.error());
    }
    const auto source_digest = graph.canonical_digest();
    const auto result_digest = result.canonical_digest();
    if (!source_digest) return core::Result<RigMirrorResult>::failure(source_digest.error());
    if (!result_digest) return core::Result<RigMirrorResult>::failure(result_digest.error());

    RigMirrorResult mirrored;
    mirrored.receipt.axis = axis;
    mirrored.receipt.direction = direction;
    mirrored.receipt.source_graph_digest = source_digest.value();
    mirrored.receipt.result_graph_digest = result_digest.value();
    mirrored.receipt.changed_joint_count = changed;
    mirrored.graph = std::move(result);
    return core::Result<RigMirrorResult>::success(std::move(mirrored));
}

core::Result<RigControlMirrorResult> mirror_rig_controls(
    const kinematic::KinematicGraph& graph,
    const ControlGraph& control_graph,
    std::span<const SymmetryPair> joint_pairs,
    std::span<const ControlSymmetryPair> control_pairs,
    SymmetryAxis axis,
    SymmetryDirection direction) {
    if (axis != SymmetryAxis::x && axis != SymmetryAxis::y && axis != SymmetryAxis::z) {
        return core::Result<RigControlMirrorResult>::failure(
            invalid("symmetry axis is not supported"));
    }
    if (direction != SymmetryDirection::left_to_right &&
        direction != SymmetryDirection::right_to_left) {
        return core::Result<RigControlMirrorResult>::failure(invalid(
            "symmetry direction is not supported"));
    }
    if (joint_pairs.empty()) {
        return core::Result<RigControlMirrorResult>::failure(invalid(
            "control mirror requires at least one joint symmetry pair"));
    }
    if (control_pairs.empty()) {
        return core::Result<RigControlMirrorResult>::failure(invalid(
            "control mirror requires at least one control symmetry pair"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<RigControlMirrorResult>::failure(result.error());
    }
    if (auto result = control_graph.validate(); !result) {
        return core::Result<RigControlMirrorResult>::failure(result.error());
    }
    const auto joint_mirror = pairing_of(graph, joint_pairs);
    if (!joint_mirror) {
        return core::Result<RigControlMirrorResult>::failure(joint_mirror.error());
    }
    const auto control_mirror = control_pairing_of(control_graph, control_pairs);
    if (!control_mirror) {
        return core::Result<RigControlMirrorResult>::failure(control_mirror.error());
    }

    std::map<ControlId, ControlDefinition> rewritten;
    for (const ControlSymmetryPair& pair : control_pairs) {
        const bool forward = direction == SymmetryDirection::left_to_right;
        const ControlId source_id = forward ? pair.left_control : pair.right_control;
        const ControlId destination_id = forward ? pair.right_control : pair.left_control;
        const ControlDefinition& source = *control_graph.find(source_id);
        ControlDefinition copy = source;
        copy.id = destination_id;
        for (kinematic::JointId& joint : copy.affected_joints) {
            if (joint_mirror.value().find(joint) == joint_mirror.value().end()) {
                return core::Result<RigControlMirrorResult>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "mirrored control affects a joint outside the rig symmetry map")
                    .with_context("control=" + std::to_string(destination_id.value))
                    .with_context("joint=" + std::to_string(joint.value)));
            }
            joint = mirrored_joint(joint_mirror.value(), joint);
        }
        std::sort(copy.affected_joints.begin(), copy.affected_joints.end());
        copy.affected_joints.erase(
            std::unique(copy.affected_joints.begin(), copy.affected_joints.end()),
            copy.affected_joints.end());
        if (copy.follow_space.has_value()) {
            copy.follow_space = mirrored_control(control_mirror.value(), *copy.follow_space);
        }
        rewritten.emplace(destination_id, std::move(copy));
    }

    kinematic::KinematicGraph result_graph;
    for (const auto& [id, node] : graph.nodes()) {
        static_cast<void>(id);
        if (auto inserted = result_graph.insert_node(node); !inserted) {
            return core::Result<RigControlMirrorResult>::failure(inserted.error());
        }
    }
    if (auto valid = result_graph.validate(); !valid) {
        return core::Result<RigControlMirrorResult>::failure(valid.error());
    }

    ControlGraph result_controls;
    std::size_t changed = 0U;
    for (const auto& [id, control] : control_graph.nodes()) {
        static_cast<void>(id);
        ControlDefinition copy = control;
        const auto replacement = rewritten.find(control.id);
        if (replacement != rewritten.end()) {
            if (!same_control(copy, replacement->second)) ++changed;
            copy = replacement->second;
        }
        if (auto inserted = result_controls.insert(std::move(copy)); !inserted) {
            return core::Result<RigControlMirrorResult>::failure(inserted.error());
        }
    }
    for (const ControlEdge& edge : control_graph.edges()) {
        if (auto inserted = result_controls.insert_edge(edge); !inserted) {
            return core::Result<RigControlMirrorResult>::failure(inserted.error());
        }
    }
    if (auto valid = result_controls.validate(); !valid) {
        // Includes the remapped follow-space acyclicity check.
        return core::Result<RigControlMirrorResult>::failure(valid.error());
    }

    const auto source_digest = graph.canonical_digest();
    if (!source_digest) {
        return core::Result<RigControlMirrorResult>::failure(source_digest.error());
    }
    const auto result_digest = result_graph.canonical_digest();
    if (!result_digest) {
        return core::Result<RigControlMirrorResult>::failure(result_digest.error());
    }
    const auto source_control_digest = control_graph.canonical_digest();
    if (!source_control_digest) {
        return core::Result<RigControlMirrorResult>::failure(source_control_digest.error());
    }
    const auto result_control_digest = result_controls.canonical_digest();
    if (!result_control_digest) {
        return core::Result<RigControlMirrorResult>::failure(result_control_digest.error());
    }

    RigControlMirrorResult mirrored;
    mirrored.receipt.axis = axis;
    mirrored.receipt.direction = direction;
    mirrored.receipt.source_graph_digest = source_digest.value();
    mirrored.receipt.result_graph_digest = result_digest.value();
    mirrored.receipt.source_control_graph_digest = source_control_digest.value();
    mirrored.receipt.result_control_graph_digest = result_control_digest.value();
    mirrored.receipt.changed_control_count = changed;
    mirrored.graph = std::move(result_graph);
    mirrored.control_graph = std::move(result_controls);
    return core::Result<RigControlMirrorResult>::success(std::move(mirrored));
}

} // namespace carto::character
