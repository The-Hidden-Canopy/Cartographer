#include <carto/motion/retarget.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace carto::motion {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

// Inverse of a normalizable quaternion: conjugate, renormalized. The rest
// and sample rotations are validated normalizable before any math here.
core::Quaternion inverse(const core::Quaternion& q) {
    const core::Quaternion n = q.normalized();
    return core::Quaternion{-n.x, -n.y, -n.z, n.w};
}

// Shortest-path angular deviation in radians between two normalizable
// quaternions, symmetric under q ~ -q.
double rotation_residual(core::Quaternion left, core::Quaternion right) {
    const core::Quaternion a = left.normalized();
    const core::Quaternion b = right.normalized();
    const double dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0 * std::acos(std::min(1.0, dot));
}

std::string serialize_joint_id(kinematic::JointId joint) {
    return std::to_string(joint.value);
}

} // namespace

core::Result<void> JointMapEntry::validate() const {
    if (!source || !target) {
        return core::Result<void>::failure(invalid(
            "retarget map entry requires a source and a destination joint"));
    }
    return core::Result<void>::success();
}

core::Result<void> RetargetMap::validate() const {
    if (entries.empty() || entries.size() > kMaxEntries) {
        return core::Result<void>::failure(validation(
            "retarget map requires between one and its bounded capacity of entries"));
    }
    for (const JointMapEntry& entry : entries) {
        if (auto result = entry.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < entries.size(); ++index) {
        if (!(entries[index - 1U] < entries[index])) {
            return core::Result<void>::failure(validation(
                "retarget map entries must be sorted and unique"));
        }
        if (entries[index - 1U].source.value == entries[index].source.value) {
            return core::Result<void>::failure(validation(
                "retarget map sources must be unique; duplicated sources are a named error"));
        }
    }
    std::set<std::uint64_t> targets;
    for (const JointMapEntry& entry : entries) {
        if (!targets.insert(entry.target.value).second) {
            return core::Result<void>::failure(validation(
                "retarget map targets must be unique; duplicated targets are a named error"));
        }
    }
    return core::Result<void>::success();
}

const JointMapEntry* RetargetMap::find_source(kinematic::JointId joint) const noexcept {
    const auto found = std::lower_bound(
        entries.begin(), entries.end(), joint,
        [](const JointMapEntry& entry, kinematic::JointId wanted) {
            return entry.source.value < wanted.value;
        });
    if (found == entries.end() || found->source.value != joint.value) return nullptr;
    return &*found;
}

const JointMapEntry* RetargetMap::find_target(kinematic::JointId joint) const noexcept {
    for (const JointMapEntry& entry : entries) {
        if (entry.target.value == joint.value) return &entry;
    }
    return nullptr;
}

std::string RetargetMap::serialize() const {
    std::ostringstream output;
    output << "CARTO_RETARGET_MAP 1\n";
    output << "ENTRIES " << entries.size() << '\n';
    for (const JointMapEntry& entry : entries) {
        output << "MAP " << entry.source.value << ' ' << entry.target.value << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> RetargetMap::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<RetargetMap> RetargetMap::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RetargetMap>::failure(validation(
            "retarget map text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_RETARGET_MAP" || version != kSchemaVersion) {
        return core::Result<RetargetMap>::failure(validation(
            "retarget map framing or schema version is invalid"));
    }
    std::string token;
    std::uint64_t entry_count = 0U;
    if (!(input >> token) || token != "ENTRIES" || !(input >> entry_count) ||
        entry_count == 0U || entry_count > kMaxEntries) {
        return core::Result<RetargetMap>::failure(validation(
            "retarget map entry count is malformed or exceeds its bound"));
    }
    RetargetMap map;
    map.entries.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0U; index < entry_count; ++index) {
        JointMapEntry entry;
        if (!(input >> token) || token != "MAP" ||
            !(input >> entry.source.value >> entry.target.value)) {
            return core::Result<RetargetMap>::failure(validation(
                "retarget map entry payload is malformed"));
        }
        map.entries.push_back(entry);
    }
    if (!(input >> token) || token != "END") {
        return core::Result<RetargetMap>::failure(validation(
            "retarget map terminator is missing"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RetargetMap>::failure(validation(
            "retarget map text contains trailing unbound data"));
    }
    if (auto result = map.validate(); !result) {
        return core::Result<RetargetMap>::failure(result.error());
    }
    return core::Result<RetargetMap>::success(std::move(map));
}

core::Result<void> RetargetPolicy::validate() const {
    if (!std::isfinite(translation_scale) || translation_scale <= 0.0) {
        return core::Result<void>::failure(invalid(
            "retarget translation scale must be finite and positive"));
    }
    if (root && root_motion == character::RootMotionPolicy::none) {
        return core::Result<void>::failure(invalid(
            "a retarget root joint requires an authored or proposal-only policy"));
    }
    return core::Result<void>::success();
}

core::Result<void> PoseSample::validate() const {
    if (auto result = time.validate(); !result) return result;
    if (locals.empty() || locals.size() > kMaxJoints) {
        return core::Result<void>::failure(validation(
            "pose sample requires between one and its bounded capacity of joints"));
    }
    for (const auto& [joint, transform] : locals) {
        if (!joint) {
            return core::Result<void>::failure(invalid(
                "pose sample carries an invalid joint identity"));
        }
        if (!transform.finite()) {
            return core::Result<void>::failure(invalid(
                "pose sample transforms must be finite"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> RootMotionProposal::validate() const {
    if (times.size() != translations.size() || times.size() != rotations.size()) {
        return core::Result<void>::failure(validation(
            "root motion proposal keys must align across time and value arrays"));
    }
    if (times.empty() || times.size() > kMaxKeys) {
        return core::Result<void>::failure(validation(
            "root motion proposal requires between one and its bounded capacity of keys"));
    }
    for (const character::RationalTime& time : times) {
        if (auto result = time.validate(); !result) return result;
    }
    for (const core::Vec3d& translation : translations) {
        if (!translation.finite()) {
            return core::Result<void>::failure(invalid(
                "root motion proposal translations must be finite"));
        }
    }
    for (const core::Quaternion& rotation : rotations) {
        if (!rotation.normalizable()) {
            return core::Result<void>::failure(invalid(
                "root motion proposal rotations must be normalizable"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> RetargetReceipt::validate() const {
    if (mapped_joint_count == 0U || mapped_joint_count > kMaxJoints) {
        return core::Result<void>::failure(validation(
            "retarget receipt mapped joint count is out of range"));
    }
    auto validate_joint_list = [](const std::vector<kinematic::JointId>& joints,
                                  const char* label) {
        if (joints.size() > kMaxJoints) {
            return core::Result<void>::failure(validation(
                std::string(label) + " exceeds its bounded capacity"));
        }
        for (const kinematic::JointId joint : joints) {
            if (!joint) {
                return core::Result<void>::failure(invalid(
                    std::string(label) + " carries an invalid joint identity"));
            }
        }
        for (std::size_t index = 1U; index < joints.size(); ++index) {
            if (!(joints[index - 1U] < joints[index])) {
                return core::Result<void>::failure(validation(
                    std::string(label) + " must be sorted and unique"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_joint_list(unmapped_source_joints, "unmapped source joints");
        !result) {
        return result;
    }
    if (auto result = validate_joint_list(unmapped_target_joints, "unmapped target joints");
        !result) {
        return result;
    }
    if (!std::isfinite(max_orientation_residual) || max_orientation_residual < 0.0 ||
        !std::isfinite(max_position_residual) || max_position_residual < 0.0) {
        return core::Result<void>::failure(invalid(
            "retarget residuals must be finite and non-negative"));
    }
    if (source_digest.is_zero() || destination_digest.is_zero() ||
        map_digest.is_zero() || policy_digest.is_zero() || receipt_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "retarget receipt requires every evidence digest"));
    }
    return core::Result<void>::success();
}

std::string RetargetReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << "CARTO_RETARGET_RECEIPT 1\n";
    output << "MAPPED " << mapped_joint_count << '\n';
    output << "UNMAPPED_SOURCE " << unmapped_source_joints.size() << '\n';
    for (const kinematic::JointId joint : unmapped_source_joints) {
        output << "US " << serialize_joint_id(joint) << '\n';
    }
    output << "UNMAPPED_TARGET " << unmapped_target_joints.size() << '\n';
    for (const kinematic::JointId joint : unmapped_target_joints) {
        output << "UT " << serialize_joint_id(joint) << '\n';
    }
    output << std::setprecision(17);
    output << "RESIDUAL " << max_orientation_residual << ' '
           << max_position_residual << '\n';
    output << "SOURCE " << source_digest.hex() << '\n';
    output << "DESTINATION " << destination_digest.hex() << '\n';
    output << "MAP " << map_digest.hex() << '\n';
    output << "POLICY " << policy_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<void> RetargetReceipt::refresh_digest() {
    receipt_digest = digest_of(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<RetargetResult> retarget_pose(
    const kinematic::JointCorrespondence& source,
    const kinematic::JointCorrespondence& destination,
    const RetargetMap& map,
    const RetargetPolicy& policy,
    const PoseSample& sample) {
    if (auto result = map.validate(); !result) {
        return core::Result<RetargetResult>::failure(result.error());
    }
    if (auto result = policy.validate(); !result) {
        return core::Result<RetargetResult>::failure(result.error());
    }
    if (auto result = sample.validate(); !result) {
        return core::Result<RetargetResult>::failure(result.error());
    }
    const auto source_digest = source.canonical_digest();
    if (!source_digest) {
        return core::Result<RetargetResult>::failure(source_digest.error());
    }
    const auto destination_digest = destination.canonical_digest();
    if (!destination_digest) {
        return core::Result<RetargetResult>::failure(destination_digest.error());
    }
    const auto map_digest = map.canonical_digest();
    if (!map_digest) {
        return core::Result<RetargetResult>::failure(map_digest.error());
    }
    std::ostringstream policy_text;
    policy_text << "CARTO_RETARGET_POLICY 1\n";
    policy_text << std::setprecision(17);
    policy_text << "SCALE " << policy.translation_scale << '\n';
    policy_text << "ROOT " << policy.root.value << '\n';
    policy_text << "ROOT_MOTION "
                << character::root_motion_policy_name(policy.root_motion) << '\n';
    policy_text << "END\n";
    const assets::Sha256Digest policy_digest = digest_of(policy_text.str());

    const JointMapEntry* root_entry =
        policy.root ? map.find_source(policy.root) : nullptr;
    if (policy.root && !root_entry) {
        return core::Result<RetargetResult>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "retarget root joint has no mapping entry"));
    }

    RetargetResult result;
    result.pose.destination.time = sample.time;
    double max_orientation = 0.0;
    double max_position = 0.0;
    bool root_proposed = false;
    for (const JointMapEntry& entry : map.entries) {
        const kinematic::JointIdentityRecord* source_joint = source.find(entry.source);
        if (!source_joint) {
            return core::Result<RetargetResult>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "source joint is absent from the source correspondence"));
        }
        const kinematic::JointIdentityRecord* destination_joint =
            destination.find(entry.target);
        if (!destination_joint) {
            return core::Result<RetargetResult>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "destination joint is absent from the destination correspondence"));
        }
        const auto source_pose = sample.locals.find(entry.source);
        if (source_pose == sample.locals.end()) {
            return core::Result<RetargetResult>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "source pose sample is missing a mapped joint"));
        }
        const core::Transform& source_rest = source_joint->authored_rest;
        const core::Transform& destination_rest = destination_joint->authored_rest;
        const core::Transform& source_local = source_pose->second;

        // Rest-pose delta compensation: transport the source local delta into
        // the destination rest frame. Rotations compose exactly through the
        // conjugate; translations are rotated into the destination frame and
        // scaled by the rest-proportion factor.
        const core::Quaternion rest_transport =
            (destination_rest.rotation.normalized() * inverse(source_rest.rotation))
                .normalized();
        core::Transform destination_local;
        destination_local.rotation =
            (destination_rest.rotation.normalized() *
                (inverse(source_rest.rotation) * source_local.rotation.normalized()))
                .normalized();
        destination_local.translation = destination_rest.translation +
            rest_transport.rotate(source_local.translation - source_rest.translation) *
                policy.translation_scale;
        destination_local.scale = source_local.scale;

        const bool is_root = root_entry != nullptr && entry.source.value == policy.root.value;
        if (is_root && policy.root_motion ==
                character::RootMotionPolicy::proposal_only) {
            // Proposal only: the source sample is untouched and the root
            // enters the proposal record, never the destination pose.
            if (!result.pose.root_proposal.has_value()) {
                result.pose.root_proposal = RootMotionProposal{};
            }
            RootMotionProposal& proposal = *result.pose.root_proposal;
            proposal.times.push_back(sample.time);
            proposal.translations.push_back(destination_local.translation);
            proposal.rotations.push_back(destination_local.rotation.normalized());
            root_proposed = true;
        } else {
            result.pose.destination.locals.emplace(entry.target, destination_local);
        }

        // Round-trip residual: invert the compensation on the destination
        // pose and measure how far it lands from the source sample.
        const core::Quaternion recovered_rotation =
            (source_rest.rotation.normalized() *
                (inverse(destination_rest.rotation) * destination_local.rotation))
                .normalized();
        max_orientation = std::max(
            max_orientation, rotation_residual(recovered_rotation, source_local.rotation));
        const core::Vec3d recovered_translation =
            source_rest.translation +
            inverse(rest_transport)
                .rotate((destination_local.translation - destination_rest.translation) *
                    (1.0 / policy.translation_scale));
        max_position = std::max(
            max_position,
            (recovered_translation - source_local.translation).length());
    }

    std::set<kinematic::JointId> mapped_sources;
    std::set<kinematic::JointId> mapped_targets;
    for (const JointMapEntry& entry : map.entries) {
        mapped_sources.insert(entry.source);
        mapped_targets.insert(entry.target);
    }
    for (const auto& [joint, record] : source.joints()) {
        (void)record;
        if (!mapped_sources.contains(joint)) {
            result.receipt.unmapped_source_joints.push_back(joint);
        }
    }
    for (const auto& [joint, record] : destination.joints()) {
        (void)record;
        if (!mapped_targets.contains(joint)) {
            result.receipt.unmapped_target_joints.push_back(joint);
        }
    }

    result.receipt.mapped_joint_count = map.entries.size();
    result.receipt.max_orientation_residual = max_orientation;
    result.receipt.max_position_residual = max_position;
    result.receipt.source_digest = source_digest.value();
    result.receipt.destination_digest = destination_digest.value();
    result.receipt.map_digest = map_digest.value();
    result.receipt.policy_digest = policy_digest;
    if (policy.root && !root_proposed &&
        policy.root_motion == character::RootMotionPolicy::proposal_only) {
        return core::Result<RetargetResult>::failure(validation(
            "proposal-only retarget produced no root motion proposal"));
    }
    if (auto refreshed = result.receipt.refresh_digest(); !refreshed) {
        return core::Result<RetargetResult>::failure(refreshed.error());
    }
    if (auto valid = result.receipt.validate(); !valid) {
        return core::Result<RetargetResult>::failure(valid.error());
    }
    return core::Result<RetargetResult>::success(std::move(result));
}

} // namespace carto::motion
