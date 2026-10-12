#include <carto/temporal/root_motion.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace carto::temporal {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_ROOT_MOTION_PROPOSAL";

bool valid_hint(RootMotionHint hint) {
    switch (hint) {
    case RootMotionHint::extract_and_strip:
    case RootMotionHint::extract_and_retain:
        return true;
    }
    return false;
}

} // namespace

std::string_view root_motion_hint_name(RootMotionHint hint) noexcept {
    switch (hint) {
    case RootMotionHint::extract_and_strip: return "extract-and-strip";
    case RootMotionHint::extract_and_retain: return "extract-and-retain";
    }
    return "unknown";
}

core::Result<void> RootMotionProposal::validate() const {
    if (!source_channel || source_revision.exhausted() || !valid_hint(hint) ||
        curve.empty() || curve.size() > kMaxKeysPerCurve) {
        return core::Result<void>::failure(detail::invalid(
            "root motion proposal source, revision, hint, or curve is invalid"));
    }
    for (std::size_t index = 0U; index < curve.size(); ++index) {
        const auto& key = curve[index];
        if (!std::isfinite(key.time) || !key.translation.finite() ||
            !key.rotation.finite() || !key.rotation.normalizable()) {
            return core::Result<void>::failure(detail::invalid(
                "root motion proposal curve contains a non-finite key"));
        }
        if (index > 0U && !(curve[index - 1U].time < key.time)) {
            return core::Result<void>::failure(detail::validation(
                "root motion proposal curve times must be strictly increasing"));
        }
    }
    return core::Result<void>::success();
}

std::string RootMotionProposal::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "PROPOSAL " << source_channel.value << ' ' << source_revision.value()
           << ' ' << static_cast<unsigned>(hint) << ' ' << curve.size() << '\n';
    for (const auto& key : curve) {
        output << "ROOT_KEY " << key.time << ' ';
        detail::write_vec3(output, key.translation);
        output << ' ';
        detail::write_quaternion(output, key.rotation);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> RootMotionProposal::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<RootMotionProposal> RootMotionProposal::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RootMotionProposal>::failure(detail::parse_error(
            "root motion proposal text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<RootMotionProposal>::failure(detail::parse_error(
            "root motion proposal framing is invalid"));
    }
    if (!detail::read_integer(input, version)) {
        return core::Result<RootMotionProposal>::failure(detail::parse_error(
            "root motion proposal schema version is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RootMotionProposal>::failure(detail::version_error(
            "root motion proposal schema version is not supported"));
    }

    RootMotionProposal proposal;
    std::uint64_t revision = 0U;
    unsigned hint = 0U;
    std::size_t key_count = 0U;
    if (!detail::require_token(input, "PROPOSAL") ||
        !detail::read_integer(input, proposal.source_channel.value) ||
        !detail::read_integer(input, revision) ||
        !detail::read_integer(input, hint) ||
        !detail::read_count(input, kMaxKeysPerCurve, key_count)) {
        return core::Result<RootMotionProposal>::failure(detail::parse_error(
            "root motion proposal header is malformed"));
    }
    proposal.source_revision = core::Revision{revision};
    proposal.hint = static_cast<RootMotionHint>(hint);
    proposal.curve.reserve(key_count);
    for (std::size_t index = 0U; index < key_count; ++index) {
        RootMotionKey key;
        if (!detail::require_token(input, "ROOT_KEY") ||
            !detail::read_double(input, key.time) ||
            !detail::read_vec3(input, key.translation) ||
            !detail::read_quaternion(input, key.rotation)) {
            return core::Result<RootMotionProposal>::failure(detail::parse_error(
                "root motion proposal curve key is malformed"));
        }
        proposal.curve.push_back(key);
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<RootMotionProposal>::failure(detail::parse_error(
            "root motion proposal is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RootMotionProposal>::failure(detail::parse_error(
            "root motion proposal contains trailing unbound data"));
    }
    if (auto result = proposal.validate(); !result) {
        return core::Result<RootMotionProposal>::failure(result.error());
    }
    return core::Result<RootMotionProposal>::success(std::move(proposal));
}

core::Result<RootMotionProposal> propose_root_motion(
    const TemporalChannel& channel, RootMotionHint hint) {
    if (auto result = channel.validate(); !result) {
        return core::Result<RootMotionProposal>::failure(result.error());
    }
    if (channel.value_kind != ChannelValueKind::transform) {
        return core::Result<RootMotionProposal>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "root motion proposals require a transform channel"));
    }
    RootMotionProposal proposal;
    proposal.source_channel = channel.id;
    proposal.source_revision = channel.revision;
    proposal.hint = hint;
    proposal.curve.reserve(channel.transform_keyframes.size());
    for (const auto& keyframe : channel.transform_keyframes) {
        RootMotionKey key;
        key.time = keyframe.time;
        key.translation = keyframe.value.translation;
        key.rotation = keyframe.value.rotation.normalized();
        proposal.curve.push_back(key);
    }
    if (auto result = proposal.validate(); !result) {
        return core::Result<RootMotionProposal>::failure(result.error());
    }
    return core::Result<RootMotionProposal>::success(std::move(proposal));
}

} // namespace carto::temporal
