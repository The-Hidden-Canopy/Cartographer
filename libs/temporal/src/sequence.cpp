#include <carto/temporal/sequence.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace carto::temporal {
namespace {

constexpr std::string_view kClipMagic = "CARTOGRAPHER_MOTION_CLIP";
constexpr std::string_view kSequenceMagic = "CARTOGRAPHER_MOTION_SEQUENCE";

bool valid_loop_policy(LoopPolicy policy) {
    switch (policy) {
    case LoopPolicy::play_once:
    case LoopPolicy::loop:
    case LoopPolicy::ping_pong:
        return true;
    }
    return false;
}

bool valid_sequence_purpose(SequencePurpose purpose) {
    switch (purpose) {
    case SequencePurpose::gameplay:
    case SequencePurpose::presentation:
        return true;
    }
    return false;
}

bool markers_canonical(const std::vector<EventMarker>& markers, double duration) {
    const auto order = [](const EventMarker& left, const EventMarker& right) {
        return std::tie(left.time, left.id.value) < std::tie(right.time, right.id.value);
    };
    if (!std::is_sorted(markers.begin(), markers.end(), order)) return false;
    for (std::size_t index = 0U; index < markers.size(); ++index) {
        if (!markers[index].id || !std::isfinite(markers[index].time) ||
            markers[index].time < 0.0 || markers[index].time > duration) {
            return false;
        }
        if (index > 0U && markers[index - 1U].id == markers[index].id) return false;
    }
    return true;
}

bool clip_refs_unique(const std::vector<ClipRef>& refs) {
    for (std::size_t index = 0U; index < refs.size(); ++index) {
        if (!refs[index].clip || refs[index].clip_digest.is_zero()) return false;
        for (std::size_t other = index + 1U; other < refs.size(); ++other) {
            if (refs[index].clip == refs[other].clip) return false;
        }
    }
    return true;
}

bool channel_refs_unique(const std::vector<ChannelRef>& refs) {
    for (std::size_t index = 0U; index < refs.size(); ++index) {
        if (!refs[index].channel || refs[index].channel_digest.is_zero()) return false;
        for (std::size_t other = index + 1U; other < refs.size(); ++other) {
            if (refs[index].channel == refs[other].channel) return false;
        }
    }
    return true;
}

} // namespace

std::string_view loop_policy_name(LoopPolicy policy) noexcept {
    switch (policy) {
    case LoopPolicy::play_once: return "play-once";
    case LoopPolicy::loop: return "loop";
    case LoopPolicy::ping_pong: return "ping-pong";
    }
    return "unknown";
}

std::string_view sequence_purpose_name(SequencePurpose purpose) noexcept {
    switch (purpose) {
    case SequencePurpose::gameplay: return "gameplay";
    case SequencePurpose::presentation: return "presentation";
    }
    return "unknown";
}

core::Result<void> MotionClip::validate() const {
    if (!id || !detail::safe_text(clip_name, kMaxClipNameBytes) ||
        revision.exhausted() || !std::isfinite(duration) || duration <= 0.0 ||
        channels.empty() || channels.size() > kMaxChannelsPerClip) {
        return core::Result<void>::failure(detail::invalid(
            "motion clip identity, name, revision, duration, or channel set is invalid"));
    }
    if (!channel_refs_unique(channels)) {
        return core::Result<void>::failure(detail::validation(
            "motion clip channel references must be unique and digest-bound"));
    }
    return core::Result<void>::success();
}

std::string MotionClip::serialize() const {
    std::ostringstream output;
    output << kClipMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "CLIP " << id.value << ' ' << revision.value() << ' '
           << std::quoted(clip_name) << ' ' << duration << ' ' << channels.size()
           << '\n';
    for (const auto& ref : channels) {
        output << "CHANNEL_REF " << ref.channel.value << ' ' << ref.channel_digest.hex()
               << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> MotionClip::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<MotionClip> MotionClip::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<MotionClip>::failure(detail::parse_error(
            "motion clip text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kClipMagic) {
        return core::Result<MotionClip>::failure(detail::parse_error(
            "motion clip framing is invalid"));
    }
    if (!detail::read_integer(input, version)) {
        return core::Result<MotionClip>::failure(detail::parse_error(
            "motion clip schema version is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<MotionClip>::failure(detail::version_error(
            "motion clip schema version is not supported"));
    }

    MotionClip clip;
    std::uint64_t revision = 0U;
    std::size_t channel_count = 0U;
    if (!detail::require_token(input, "CLIP") ||
        !detail::read_integer(input, clip.id.value) ||
        !detail::read_integer(input, revision) ||
        !(input >> std::quoted(clip.clip_name)) ||
        !detail::read_double(input, clip.duration) ||
        !detail::read_count(input, kMaxChannelsPerClip, channel_count)) {
        return core::Result<MotionClip>::failure(detail::parse_error(
            "motion clip header is malformed"));
    }
    clip.revision = core::Revision{revision};
    clip.channels.reserve(channel_count);
    for (std::size_t index = 0U; index < channel_count; ++index) {
        ChannelRef ref;
        std::string digest;
        if (!detail::require_token(input, "CHANNEL_REF") ||
            !detail::read_integer(input, ref.channel.value) ||
            !(input >> digest)) {
            return core::Result<MotionClip>::failure(detail::parse_error(
                "motion clip channel reference is malformed"));
        }
        const auto parsed = assets::Sha256Digest::from_hex(digest);
        if (!parsed) {
            return core::Result<MotionClip>::failure(detail::parse_error(
                "motion clip channel reference digest is malformed"));
        }
        ref.channel_digest = parsed.value();
        clip.channels.push_back(ref);
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<MotionClip>::failure(detail::parse_error(
            "motion clip is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MotionClip>::failure(detail::parse_error(
            "motion clip contains trailing unbound data"));
    }
    if (auto result = clip.validate(); !result) {
        return core::Result<MotionClip>::failure(result.error());
    }
    return core::Result<MotionClip>::success(std::move(clip));
}

core::Result<void> MotionSequence::validate() const {
    if (!id || !detail::safe_text(sequence_name, kMaxSequenceNameBytes) ||
        revision.exhausted() || !std::isfinite(duration) || duration <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "motion sequence identity, name, revision, or duration is invalid"));
    }
    if (!valid_loop_policy(loop)) {
        return core::Result<void>::failure(
            detail::invalid("motion sequence loop policy is invalid"));
    }
    if (!valid_sequence_purpose(purpose)) {
        return core::Result<void>::failure(
            detail::invalid("motion sequence purpose is invalid"));
    }
    if (clips.empty() && channels.empty()) {
        return core::Result<void>::failure(detail::validation(
            "motion sequence must reference at least one clip or channel"));
    }
    if (clips.size() > kMaxClipsPerSequence || channels.size() > kMaxChannelsPerSequence ||
        event_markers.size() > kMaxEventMarkersPerSequence) {
        return core::Result<void>::failure(detail::validation(
            "motion sequence exceeds its bounded reference or marker capacity"));
    }
    if (!clip_refs_unique(clips)) {
        return core::Result<void>::failure(detail::validation(
            "motion sequence clip references must be unique and digest-bound"));
    }
    if (!channel_refs_unique(channels)) {
        return core::Result<void>::failure(detail::validation(
            "motion sequence channel references must be unique and digest-bound"));
    }
    for (const auto& marker : event_markers) {
        if (auto result = marker.validate(); !result) return result;
    }
    if (!markers_canonical(event_markers, duration)) {
        return core::Result<void>::failure(detail::validation(
            "motion sequence event markers must be ordered by time then identity, "
            "unique in identity, and inside the sequence duration"));
    }
    return core::Result<void>::success();
}

std::string MotionSequence::serialize() const {
    std::ostringstream output;
    output << kSequenceMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "SEQUENCE " << id.value << ' ' << revision.value() << ' '
           << std::quoted(sequence_name) << ' ' << duration << ' '
           << static_cast<unsigned>(loop) << ' ' << static_cast<unsigned>(purpose) << ' '
           << clips.size() << ' ' << channels.size() << ' ' << event_markers.size()
           << '\n';
    for (const auto& ref : clips) {
        output << "CLIP_REF " << ref.clip.value << ' ' << ref.clip_digest.hex() << '\n';
    }
    for (const auto& ref : channels) {
        output << "CHANNEL_REF " << ref.channel.value << ' ' << ref.channel_digest.hex()
               << '\n';
    }
    for (const auto& marker : event_markers) {
        output << "EVENT " << marker.id.value << ' ' << marker.time << ' '
               << std::quoted(marker.name) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> MotionSequence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<MotionSequence> MotionSequence::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<MotionSequence>::failure(detail::parse_error(
            "motion sequence text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kSequenceMagic) {
        return core::Result<MotionSequence>::failure(detail::parse_error(
            "motion sequence framing is invalid"));
    }
    if (!detail::read_integer(input, version)) {
        return core::Result<MotionSequence>::failure(detail::parse_error(
            "motion sequence schema version is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<MotionSequence>::failure(detail::version_error(
            "motion sequence schema version is not supported"));
    }

    MotionSequence sequence;
    std::uint64_t revision = 0U;
    unsigned loop = 0U;
    unsigned purpose = 0U;
    std::size_t clip_count = 0U;
    std::size_t channel_count = 0U;
    std::size_t marker_count = 0U;
    if (!detail::require_token(input, "SEQUENCE") ||
        !detail::read_integer(input, sequence.id.value) ||
        !detail::read_integer(input, revision) ||
        !(input >> std::quoted(sequence.sequence_name)) ||
        !detail::read_double(input, sequence.duration) ||
        !detail::read_integer(input, loop) ||
        !detail::read_integer(input, purpose) ||
        !detail::read_count(input, kMaxClipsPerSequence, clip_count) ||
        !detail::read_count(input, kMaxChannelsPerSequence, channel_count) ||
        !detail::read_count(input, kMaxEventMarkersPerSequence, marker_count)) {
        return core::Result<MotionSequence>::failure(detail::parse_error(
            "motion sequence header is malformed"));
    }
    sequence.revision = core::Revision{revision};
    sequence.loop = static_cast<LoopPolicy>(loop);
    sequence.purpose = static_cast<SequencePurpose>(purpose);
    sequence.clips.reserve(clip_count);
    sequence.channels.reserve(channel_count);
    sequence.event_markers.reserve(marker_count);
    for (std::size_t index = 0U; index < clip_count; ++index) {
        ClipRef ref;
        std::string digest;
        if (!detail::require_token(input, "CLIP_REF") ||
            !detail::read_integer(input, ref.clip.value) || !(input >> digest)) {
            return core::Result<MotionSequence>::failure(detail::parse_error(
                "motion sequence clip reference is malformed"));
        }
        const auto parsed = assets::Sha256Digest::from_hex(digest);
        if (!parsed) {
            return core::Result<MotionSequence>::failure(detail::parse_error(
                "motion sequence clip reference digest is malformed"));
        }
        ref.clip_digest = parsed.value();
        sequence.clips.push_back(ref);
    }
    for (std::size_t index = 0U; index < channel_count; ++index) {
        ChannelRef ref;
        std::string digest;
        if (!detail::require_token(input, "CHANNEL_REF") ||
            !detail::read_integer(input, ref.channel.value) || !(input >> digest)) {
            return core::Result<MotionSequence>::failure(detail::parse_error(
                "motion sequence channel reference is malformed"));
        }
        const auto parsed = assets::Sha256Digest::from_hex(digest);
        if (!parsed) {
            return core::Result<MotionSequence>::failure(detail::parse_error(
                "motion sequence channel reference digest is malformed"));
        }
        ref.channel_digest = parsed.value();
        sequence.channels.push_back(ref);
    }
    for (std::size_t index = 0U; index < marker_count; ++index) {
        EventMarker marker;
        if (!detail::require_token(input, "EVENT") ||
            !detail::read_integer(input, marker.id.value) ||
            !detail::read_double(input, marker.time) ||
            !(input >> std::quoted(marker.name))) {
            return core::Result<MotionSequence>::failure(detail::parse_error(
                "motion sequence event marker is malformed"));
        }
        sequence.event_markers.push_back(std::move(marker));
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<MotionSequence>::failure(detail::parse_error(
            "motion sequence is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MotionSequence>::failure(detail::parse_error(
            "motion sequence contains trailing unbound data"));
    }
    if (auto result = sequence.validate(); !result) {
        return core::Result<MotionSequence>::failure(result.error());
    }
    return core::Result<MotionSequence>::success(std::move(sequence));
}

} // namespace carto::temporal
