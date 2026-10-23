#pragma once

#include <carto/character/motion_document.hpp>
#include <carto/character/time.hpp>
#include <carto/kinematic/identity.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace carto::motion {

// One explicit authored correspondence from a source skeleton joint to a
// destination skeleton joint. There is deliberately no fuzzy matching here:
// automation may propose entries, but only validated explicit entries enter
// the map.
struct JointMapEntry {
    kinematic::JointId source;
    kinematic::JointId target;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const JointMapEntry&) const noexcept = default;
    [[nodiscard]] bool operator<(const JointMapEntry& other) const noexcept {
        return std::tie(source.value, target.value) <
            std::tie(other.source.value, other.target.value);
    }
};

// The validated skeleton A->B mapping. Entries are sorted by source then
// target; duplicate sources and duplicate targets are validation errors, so a
// missing or duplicated target is a named failure, never a silent joint
// substitution.
class RetargetMap final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxEntries = 65'536U;

    std::vector<JointMapEntry> entries;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] const JointMapEntry* find_source(kinematic::JointId joint) const noexcept;
    [[nodiscard]] const JointMapEntry* find_target(kinematic::JointId joint) const noexcept;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<RetargetMap> deserialize(std::string_view text);
};

// Retarget behaviour knobs. translation_scale compensates rest-proportion
// differences on the translation delta (1.0 = same proportions). root selects
// the joint treated as the motion root; with proposal_only the root's
// retargeted transform is emitted into the proposal record only and never
// enters the destination pose, with authored it stays in the destination
// pose, and none rejects the retarget if a root is set.
struct RetargetPolicy {
    double translation_scale = 1.0;
    kinematic::JointId root;
    character::RootMotionPolicy root_motion = character::RootMotionPolicy::proposal_only;

    [[nodiscard]] core::Result<void> validate() const;
};

// One sampled local pose on either skeleton, keyed by joint id. The map
// ordering is the deterministic iteration order of every retarget walk.
struct PoseSample {
    static constexpr std::size_t kMaxJoints = 65'536U;

    character::RationalTime time{0, 0};
    std::map<kinematic::JointId, core::Transform> locals;

    [[nodiscard]] core::Result<void> validate() const;
};

// Root motion extracted from a proposal_only retarget. This record proposes;
// admission of the displacement is a downstream game-runtime decision and
// mutates nothing in Cartographer records.
struct RootMotionProposal {
    static constexpr std::size_t kMaxKeys = 65'536U;

    std::vector<character::RationalTime> times;
    std::vector<core::Vec3d> translations;
    std::vector<core::Quaternion> rotations;

    [[nodiscard]] core::Result<void> validate() const;
};

struct RetargetedPose {
    PoseSample destination;
    std::optional<RootMotionProposal> root_proposal;
};

// Evidence for one retarget operation. Unmapped joints are reported on both
// sides, sorted, and are never synthesized. Residuals are round-trip
// deviations: retargeting back through the inverse rest compensation must
// recover the source local pose, and the maximum angular/positional
// deviation across mapped joints is recorded. The digests pin the exact
// source, destination, map and policy content the receipt was issued
// against; the receipt digest is refreshed, never hand-assigned.
struct RetargetReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxJoints = 65'536U;

    std::size_t mapped_joint_count = 0U;
    std::vector<kinematic::JointId> unmapped_source_joints;
    std::vector<kinematic::JointId> unmapped_target_joints;
    // Radians and source-length units; zero when no mapped joint deviates.
    double max_orientation_residual = 0.0;
    double max_position_residual = 0.0;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest destination_digest;
    assets::Sha256Digest map_digest;
    assets::Sha256Digest policy_digest;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct RetargetResult {
    RetargetedPose pose;
    RetargetReceipt receipt;
};

// Retargets one sampled source local pose onto the destination skeleton with
// rest-pose delta compensation: destination local = destination rest * the
// transported source delta, where translations are scaled by the policy and
// rotated into the destination rest frame. Rest data comes from each side's
// JointCorrespondence authored rests, so the same receipts that pin joint
// identity pin retarget evidence. With RootMotionPolicy::proposal_only the
// source pose object is untouched and the root lands only in the proposal.
[[nodiscard]] core::Result<RetargetResult> retarget_pose(
    const kinematic::JointCorrespondence& source,
    const kinematic::JointCorrespondence& destination,
    const RetargetMap& map,
    const RetargetPolicy& policy,
    const PoseSample& sample);

} // namespace carto::motion
