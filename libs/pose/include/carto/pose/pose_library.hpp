#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/time.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/pose/fk.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace carto::pose {

// Pose library, pose mirroring, and motion paths (ch13 "pose library, motion
// path, IK/FK matching and curve UX"). All three are pure CPU-reference
// helpers over LocalPose: applying a stored pose returns a new LocalPose for
// the caller to key or discard, and a motion path is derived display data,
// never authored truth.

// Named stored local poses. Names are 1..64 characters of [A-Za-z0-9_.-] and
// unique; entries are ordered by name so serialization is canonical.
class PoseLibrary final {
public:
    static constexpr std::size_t kMaxPoses = 1'024U;
    static constexpr std::size_t kMaxJointsPerPose = 4'096U;
    static constexpr std::size_t kMaxNameLength = 64U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    // Stores a pose; refuses an invalid or duplicate name, an empty or
    // oversized pose, or a non-finite transform.
    [[nodiscard]] core::Result<void> add(std::string name, LocalPose pose);
    [[nodiscard]] core::Result<void> remove(std::string_view name);
    [[nodiscard]] const LocalPose* find(std::string_view name) const noexcept;
    [[nodiscard]] const std::map<std::string, LocalPose, std::less<>>& entries() const noexcept {
        return poses_;
    }
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<PoseLibrary> deserialize(std::string_view text);
    [[nodiscard]] assets::Sha256Digest canonical_digest() const;

private:
    std::map<std::string, LocalPose, std::less<>> poses_;
};

struct PoseApplyRequest {
    std::string name;
    // Blend from the current pose (0) to the stored pose (1), inside [0, 1].
    double weight = 1.0;
    // Restricts the blend to these joints; nullopt applies every joint the
    // stored pose names.
    std::optional<std::set<kinematic::JointId>> joint_mask;
};

// Blends a stored pose into `current` (rest locals for joints `current` does
// not override): translation and scale lerp, rotation nlerp. Refuses an
// unknown pose name, a stored joint absent from the graph, or a weight
// outside [0, 1].
[[nodiscard]] core::Result<LocalPose> apply_library_pose(
    const kinematic::KinematicGraph& graph,
    const LocalPose& current,
    const PoseLibrary& library,
    const PoseApplyRequest& request);

enum class PoseMirrorAxis : std::uint8_t { x, y, z };

// Mirrors a local pose across the plane normal to `axis`: each joint's local
// transform is reflected (translation component negated, rotation reflected
// as a pseudovector) and moved to its paired joint. Joints without a pair
// mirror onto themselves. Valid for rigs whose rest pose is symmetric under
// the same reflection. Refuses an asymmetric pairing or a paired joint absent
// from the graph.
[[nodiscard]] core::Result<LocalPose> mirror_pose(
    const kinematic::KinematicGraph& graph,
    const LocalPose& pose,
    const std::map<kinematic::JointId, kinematic::JointId>& joint_pairs,
    PoseMirrorAxis axis);

struct MotionPathSample {
    character::RationalTime time;
    core::Vec3d position;
};

struct MotionPath {
    static constexpr std::size_t kMaxSamples = 10'000U;

    kinematic::JointId joint;
    std::vector<MotionPathSample> samples;
    // Total polyline length through the samples.
    double length = 0.0;
};

// Samples the world-space position of `joint` from `start` to `end`
// inclusive every `step_ticks` on one timebase. `sample_pose` supplies the
// local pose at a time (typically the evaluated animation layers). Refuses a
// mixed timebase, a reversed range, a non-positive step, more than
// MotionPath::kMaxSamples samples, or any failed pose sample.
[[nodiscard]] core::Result<MotionPath> compute_motion_path(
    const kinematic::KinematicGraph& graph,
    kinematic::JointId joint,
    character::RationalTime start,
    character::RationalTime end,
    std::int64_t step_ticks,
    const std::function<core::Result<LocalPose>(character::RationalTime)>& sample_pose);

} // namespace carto::pose
