#include <carto/pose/pose_library.hpp>

#include <carto/pose/rotation.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace carto::pose {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic missing(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > PoseLibrary::kMaxNameLength) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

core::Vec3d lerp(const core::Vec3d& from, const core::Vec3d& to, double t) {
    return from + (to - from) * t;
}

core::Transform rest_or_override(
    const kinematic::KinematicGraph& graph,
    const LocalPose& pose,
    kinematic::JointId joint) {
    const auto found = pose.find(joint);
    if (found != pose.end()) return found->second;
    const kinematic::KinematicNode* node = graph.find(joint);
    return node == nullptr ? core::Transform::identity() : node->rest;
}

core::Transform reflect(const core::Transform& transform, PoseMirrorAxis axis) {
    core::Transform mirrored = transform;
    core::Quaternion& q = mirrored.rotation;
    switch (axis) {
    case PoseMirrorAxis::x:
        mirrored.translation.x = -mirrored.translation.x;
        q = {q.x, -q.y, -q.z, q.w};
        break;
    case PoseMirrorAxis::y:
        mirrored.translation.y = -mirrored.translation.y;
        q = {-q.x, q.y, -q.z, q.w};
        break;
    case PoseMirrorAxis::z:
        mirrored.translation.z = -mirrored.translation.z;
        q = {-q.x, -q.y, q.z, q.w};
        break;
    }
    return mirrored;
}

} // namespace

core::Result<void> PoseLibrary::add(std::string name, LocalPose pose) {
    if (!valid_name(name)) {
        return core::Result<void>::failure(invalid(
            "pose library names are 1..64 characters of [A-Za-z0-9_.-]"));
    }
    if (poses_.contains(name)) {
        return core::Result<void>::failure(invalid("pose library name already exists"));
    }
    if (poses_.size() >= kMaxPoses) {
        return core::Result<void>::failure(invalid("pose library is at its capacity"));
    }
    if (pose.empty() || pose.size() > kMaxJointsPerPose) {
        return core::Result<void>::failure(invalid(
            "a stored pose must name a bounded non-empty joint set"));
    }
    for (const auto& [joint, transform] : pose) {
        if (!joint || !transform.finite()) {
            return core::Result<void>::failure(invalid(
                "a stored pose requires non-zero joints and finite transforms"));
        }
    }
    poses_.emplace(std::move(name), std::move(pose));
    return core::Result<void>::success();
}

core::Result<void> PoseLibrary::remove(std::string_view name) {
    const auto found = poses_.find(name);
    if (found == poses_.end()) {
        return core::Result<void>::failure(missing("pose library has no such pose"));
    }
    poses_.erase(found);
    return core::Result<void>::success();
}

const LocalPose* PoseLibrary::find(std::string_view name) const noexcept {
    const auto found = poses_.find(name);
    return found == poses_.end() ? nullptr : &found->second;
}

std::string PoseLibrary::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_POSE_LIBRARY 1\n" << std::setprecision(17);
    output << "POSES " << poses_.size() << '\n';
    for (const auto& [name, pose] : poses_) {
        output << "POSE " << name << ' ' << pose.size() << '\n';
        for (const auto& [joint, transform] : pose) {
            const core::Quaternion& q = transform.rotation;
            output << "JOINT " << joint.value << ' '
                   << transform.translation.x << ' ' << transform.translation.y << ' '
                   << transform.translation.z << ' ' << q.x << ' ' << q.y << ' ' << q.z
                   << ' ' << q.w << ' ' << transform.scale.x << ' ' << transform.scale.y
                   << ' ' << transform.scale.z << '\n';
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<PoseLibrary> PoseLibrary::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PoseLibrary>::failure(parse_error(
            "pose library text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != "CARTOGRAPHER_POSE_LIBRARY" ||
        !(input >> version) || version != 1U) {
        return core::Result<PoseLibrary>::failure(parse_error(
            "pose library framing is invalid"));
    }
    std::string token;
    std::uint64_t pose_count = 0U;
    if (!(input >> token) || token != "POSES" || !(input >> pose_count) ||
        pose_count > kMaxPoses) {
        return core::Result<PoseLibrary>::failure(parse_error(
            "pose library pose count is invalid"));
    }
    PoseLibrary library;
    for (std::uint64_t index = 0U; index < pose_count; ++index) {
        std::string name;
        std::uint64_t joint_count = 0U;
        if (!(input >> token) || token != "POSE" || !(input >> name) ||
            !(input >> joint_count) || joint_count == 0U || joint_count > kMaxJointsPerPose ||
            !valid_name(name)) {
            return core::Result<PoseLibrary>::failure(parse_error(
                "pose library entry name or joint count is invalid"));
        }
        LocalPose pose;
        for (std::uint64_t joint_index = 0U; joint_index < joint_count; ++joint_index) {
            std::uint64_t joint_value = 0U;
            core::Transform transform;
            if (!(input >> token) || token != "JOINT" || !(input >> joint_value) ||
                !(input >> transform.translation.x >> transform.translation.y >>
                  transform.translation.z >> transform.rotation.x >> transform.rotation.y >>
                  transform.rotation.z >> transform.rotation.w >> transform.scale.x >>
                  transform.scale.y >> transform.scale.z)) {
                return core::Result<PoseLibrary>::failure(parse_error(
                    "pose library joint record is malformed"));
            }
            if (!pose.emplace(kinematic::JointId{joint_value}, transform).second) {
                return core::Result<PoseLibrary>::failure(parse_error(
                    "pose library entry repeats a joint"));
            }
        }
        if (auto result = library.add(std::move(name), std::move(pose)); !result) {
            return core::Result<PoseLibrary>::failure(result.error());
        }
    }
    if (!(input >> token) || token != "END") {
        return core::Result<PoseLibrary>::failure(parse_error(
            "pose library is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PoseLibrary>::failure(parse_error(
            "pose library contains unexpected data after END"));
    }
    return core::Result<PoseLibrary>::success(std::move(library));
}

assets::Sha256Digest PoseLibrary::canonical_digest() const {
    const std::string text = serialize();
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

core::Result<LocalPose> apply_library_pose(
    const kinematic::KinematicGraph& graph,
    const LocalPose& current,
    const PoseLibrary& library,
    const PoseApplyRequest& request) {
    if (!std::isfinite(request.weight) || request.weight < 0.0 || request.weight > 1.0) {
        return core::Result<LocalPose>::failure(invalid(
            "pose blend weight must be inside [0, 1]"));
    }
    const LocalPose* stored = library.find(request.name);
    if (stored == nullptr) {
        return core::Result<LocalPose>::failure(missing("pose library has no such pose"));
    }
    for (const auto& [joint, transform] : *stored) {
        static_cast<void>(transform);
        if (graph.find(joint) == nullptr) {
            return core::Result<LocalPose>::failure(missing(
                "stored pose names a joint absent from the graph")
                .with_context("joint=" + std::to_string(joint.value)));
        }
    }
    if (request.joint_mask.has_value()) {
        for (const kinematic::JointId joint : *request.joint_mask) {
            if (graph.find(joint) == nullptr) {
                return core::Result<LocalPose>::failure(missing(
                    "pose blend mask names a joint absent from the graph"));
            }
        }
    }
    LocalPose blended = current;
    for (const auto& [joint, target] : *stored) {
        if (request.joint_mask.has_value() && !request.joint_mask->contains(joint)) continue;
        const core::Transform from = rest_or_override(graph, current, joint);
        core::Transform result;
        result.translation = lerp(from.translation, target.translation, request.weight);
        result.rotation = nlerp(from.rotation, target.rotation, request.weight);
        result.scale = lerp(from.scale, target.scale, request.weight);
        if (!result.finite()) {
            return core::Result<LocalPose>::failure(invalid(
                "pose blend produced a non-finite transform"));
        }
        blended[joint] = result;
    }
    return core::Result<LocalPose>::success(std::move(blended));
}

core::Result<LocalPose> mirror_pose(
    const kinematic::KinematicGraph& graph,
    const LocalPose& pose,
    const std::map<kinematic::JointId, kinematic::JointId>& joint_pairs,
    PoseMirrorAxis axis) {
    if (axis != PoseMirrorAxis::x && axis != PoseMirrorAxis::y && axis != PoseMirrorAxis::z) {
        return core::Result<LocalPose>::failure(invalid("pose mirror axis is not supported"));
    }
    for (const auto& [joint, partner] : joint_pairs) {
        const auto back = joint_pairs.find(partner);
        if (!joint || !partner || joint == partner || back == joint_pairs.end() ||
            back->second != joint) {
            return core::Result<LocalPose>::failure(invalid(
                "pose mirror joint pairs must be symmetric"));
        }
        if (graph.find(joint) == nullptr) {
            return core::Result<LocalPose>::failure(missing(
                "pose mirror pair names a joint absent from the graph"));
        }
    }
    const auto partner_of = [&joint_pairs](kinematic::JointId joint) {
        const auto found = joint_pairs.find(joint);
        return found == joint_pairs.end() ? joint : found->second;
    };
    // Every joint touched on either side of a pair is mirrored so a pose
    // that overrides only the left arm also resets the right arm to rest.
    std::set<kinematic::JointId> touched;
    for (const auto& [joint, transform] : pose) {
        static_cast<void>(transform);
        if (graph.find(joint) == nullptr) {
            return core::Result<LocalPose>::failure(missing(
                "mirrored pose names a joint absent from the graph"));
        }
        touched.insert(joint);
        touched.insert(partner_of(joint));
    }
    LocalPose mirrored;
    for (const kinematic::JointId joint : touched) {
        const kinematic::JointId source = partner_of(joint);
        mirrored[joint] = reflect(rest_or_override(graph, pose, source), axis);
    }
    return core::Result<LocalPose>::success(std::move(mirrored));
}

core::Result<MotionPath> compute_motion_path(
    const kinematic::KinematicGraph& graph,
    kinematic::JointId joint,
    character::RationalTime start,
    character::RationalTime end,
    std::int64_t step_ticks,
    const std::function<core::Result<LocalPose>(character::RationalTime)>& sample_pose) {
    if (auto result = start.validate(); !result) {
        return core::Result<MotionPath>::failure(result.error());
    }
    if (auto result = end.validate(); !result) {
        return core::Result<MotionPath>::failure(result.error());
    }
    if (start.ticks_per_second != end.ticks_per_second) {
        return core::Result<MotionPath>::failure(invalid(
            "motion path range must use one timebase"));
    }
    if (end.tick < start.tick || step_ticks <= 0) {
        return core::Result<MotionPath>::failure(invalid(
            "motion path requires an ordered range and a positive step"));
    }
    if (!sample_pose) {
        return core::Result<MotionPath>::failure(invalid(
            "motion path requires a pose sampler"));
    }
    if (graph.find(joint) == nullptr) {
        return core::Result<MotionPath>::failure(missing(
            "motion path joint is absent from the graph"));
    }
    const double span = static_cast<double>(end.tick) - static_cast<double>(start.tick);
    const double count = std::floor(span / static_cast<double>(step_ticks)) + 1.0;
    if (count > static_cast<double>(MotionPath::kMaxSamples)) {
        return core::Result<MotionPath>::failure(invalid(
            "motion path exceeds its sample bound"));
    }
    MotionPath path;
    path.joint = joint;
    path.samples.reserve(static_cast<std::size_t>(count));
    for (std::int64_t tick = start.tick; tick <= end.tick; tick += step_ticks) {
        const character::RationalTime time{tick, start.ticks_per_second};
        auto locals = sample_pose(time);
        if (!locals) {
            return core::Result<MotionPath>::failure(
                locals.error().with_context("tick=" + std::to_string(tick)));
        }
        auto posed = evaluate_fk(graph, locals.value());
        if (!posed) return core::Result<MotionPath>::failure(posed.error());
        const core::Vec3d position = posed.value().world.at(joint).translation;
        if (!path.samples.empty()) {
            path.length += (position - path.samples.back().position).length();
        }
        path.samples.push_back(MotionPathSample{time, position});
        if (end.tick - tick < step_ticks) break;
    }
    return core::Result<MotionPath>::success(std::move(path));
}

} // namespace carto::pose
