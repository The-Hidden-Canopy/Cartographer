#pragma once

#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace carto::temporal {

enum class RootMotionHint : std::uint8_t {
    // The curve is proposed for extraction and the authored root translation
    // is expected to be removed from clip playback by the runtime.
    extract_and_strip,
    // The curve is proposed for extraction while the authored root is kept.
    extract_and_retain,
};

[[nodiscard]] std::string_view root_motion_hint_name(RootMotionHint hint) noexcept;

struct RootMotionKey {
    double time = 0.0;
    core::Vec3d translation{};
    core::Quaternion rotation = core::Quaternion::identity();
};

// Proposal record only. Extracting, stripping, or consuming root motion is a
// downstream consumer/game runtime decision; this record grants no mutation authority over
// the source channel or any runtime state. Applying a proposal therefore
// mutates nothing in Cartographer records.
class RootMotionProposal final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxKeysPerCurve = 65'536U;

    ChannelId source_channel;
    core::Revision source_revision = core::Revision{1U};
    RootMotionHint hint = RootMotionHint::extract_and_retain;
    std::vector<RootMotionKey> curve;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<RootMotionProposal> deserialize(
        std::string_view text);
};

// Derives a root-motion proposal from a transform channel without mutating
// it. The proposal curve carries translation and normalized rotation only;
// scale is not root motion.
[[nodiscard]] core::Result<RootMotionProposal> propose_root_motion(
    const TemporalChannel& channel, RootMotionHint hint);

} // namespace carto::temporal
