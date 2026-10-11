#pragma once

#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::temporal {

enum class LoopPolicy : std::uint8_t {
    play_once,
    loop,
    ping_pong,
};

enum class SequencePurpose : std::uint8_t {
    gameplay,
    presentation,
};

[[nodiscard]] std::string_view loop_policy_name(LoopPolicy policy) noexcept;
[[nodiscard]] std::string_view sequence_purpose_name(
    SequencePurpose purpose) noexcept;

// Digest-bound reference establishing exact-revision correspondence: the
// stable identity selects the record, the digest binds the precise revision
// that correspondence was authored against.
struct ChannelRef {
    ChannelId channel;
    assets::Sha256Digest channel_digest;

    [[nodiscard]] bool operator==(const ChannelRef&) const noexcept = default;
};

struct ClipRef {
    ClipId clip;
    assets::Sha256Digest clip_digest;

    [[nodiscard]] bool operator==(const ClipRef&) const noexcept = default;
};

// A named, revisioned grouping of channel references. The ClipId is stable
// across revisions: revision changes re-version the clip, they never reissue
// its identity, so downstream references survive reimport.
class MotionClip final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxChannelsPerClip = 4'096U;
    static constexpr std::size_t kMaxClipNameBytes = 256U;

    ClipId id;
    std::string clip_name;
    core::Revision revision = core::Revision{1U};
    double duration = 0.0;
    std::vector<ChannelRef> channels;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<MotionClip> deserialize(std::string_view text);
};

// Neutral, versioned, digest-bound sequence record: ordered clip and channel
// references, authoring-time event markers, and a loop policy. Reference
// order is authoring playback order and is content; duplicate references are
// rejected. Which clip plays next, crossfade behavior, and all runtime state
// remain VANTA and game runtime authority. A sequence may be designated as a
// presentation sequence (cutscene/weathering showcase) without changing its
// record shape.
class MotionSequence final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxClipsPerSequence = 4'096U;
    static constexpr std::size_t kMaxChannelsPerSequence = 4'096U;
    static constexpr std::size_t kMaxEventMarkersPerSequence = 4'096U;
    static constexpr std::size_t kMaxSequenceNameBytes = 256U;

    SequenceId id;
    std::string sequence_name;
    core::Revision revision = core::Revision{1U};
    double duration = 0.0;
    LoopPolicy loop = LoopPolicy::play_once;
    SequencePurpose purpose = SequencePurpose::gameplay;
    std::vector<ClipRef> clips;
    std::vector<ChannelRef> channels;
    std::vector<EventMarker> event_markers;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<MotionSequence> deserialize(
        std::string_view text);
};

} // namespace carto::temporal
