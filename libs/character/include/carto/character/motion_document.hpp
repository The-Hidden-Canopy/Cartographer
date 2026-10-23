#pragma once

#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace carto::character {

// Digest-bound references into the temporal authoring layer. The digest pins
// the exact clip/channel/event content this document was authored against.
struct ClipRef {
    temporal::ClipId clip;
    assets::Sha256Digest digest;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator<(const ClipRef& other) const noexcept {
        return std::tie(clip.value, digest) < std::tie(other.clip.value, other.digest);
    }
};

struct ChannelRef {
    temporal::ChannelId channel;
    assets::Sha256Digest digest;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator<(const ChannelRef& other) const noexcept {
        return std::tie(channel.value, digest) < std::tie(other.channel.value, other.digest);
    }
};

struct SequenceRef {
    temporal::SequenceId sequence;
    assets::Sha256Digest digest;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator<(const SequenceRef& other) const noexcept {
        return std::tie(sequence.value, digest) < std::tie(other.sequence.value, other.digest);
    }
};

struct EventRef {
    temporal::EventId event;
    assets::Sha256Digest digest;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator<(const EventRef& other) const noexcept {
        return std::tie(event.value, digest) < std::tie(other.event.value, other.digest);
    }
};

// A joint or control this document is allowed to address, exactly one set.
struct MotionTargetRef {
    kinematic::JointId joint;
    ControlId control;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const MotionTargetRef&) const noexcept = default;
    [[nodiscard]] bool operator<(const MotionTargetRef&) const noexcept;
};

enum class RootMotionPolicy : std::uint8_t {
    // Root motion is authored truth and ships in prepared products.
    authored,
    // Root motion is a proposal only; admission requires a human decision.
    proposal_only,
    // The document carries no root motion.
    none,
};

// Explicit policy for curves that address a target absent from the document:
// fail closed, or skip the offender and record a skip receipt downstream.
enum class MissingTargetHandling : std::uint8_t {
    fail,
    skip_with_receipt,
};

[[nodiscard]] std::string_view root_motion_policy_name(RootMotionPolicy policy) noexcept;
[[nodiscard]] std::string_view missing_target_handling_name(
    MissingTargetHandling handling) noexcept;

// The authored motion document for one character: a rational timebase,
// digest-bound temporal references, the joint/control targets it may address,
// and explicit root-motion and missing-target policies. Evaluated poses and
// playback clocks are derived runtime state and are deliberately absent.
struct MotionDocument {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxTemporalRefs = 4'096U;
    static constexpr std::size_t kMaxTargets = 4'096U;
    static constexpr std::uint32_t kMaxTicksPerSecond = 1'000'000U;

    MotionDocumentId id;
    // Rational timebase policy for all authored time in this document.
    std::uint32_t timebase_ticks_per_second = 0U;
    std::vector<ClipRef> clips;
    std::vector<ChannelRef> channels;
    std::vector<SequenceRef> sequences;
    std::vector<EventRef> events;
    // Addressable targets, sorted and unique.
    std::vector<MotionTargetRef> targets;
    RootMotionPolicy root_motion = RootMotionPolicy::none;
    MissingTargetHandling missing_target = MissingTargetHandling::fail;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
