#pragma once

#include <carto/character/time.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::face {

class VisemeMapping;

// One aligned phoneme interval on the rational timeline. End must be strictly
// after start under character::compare and both must share one timebase.
struct PhonemeSegment {
    static constexpr std::size_t kMaxPhonemeBytes = 16U;

    character::RationalTime start{};
    character::RationalTime end{};
    std::string phoneme;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const PhonemeSegment& other) const noexcept {
        return start.tick == other.start.tick &&
            start.ticks_per_second == other.start.ticks_per_second &&
            end.tick == other.end.tick &&
            end.ticks_per_second == other.end.ticks_per_second &&
            phoneme == other.phoneme;
    }
};

// Bounded, canonically ordered phoneme timeline: sorted by start time,
// non-overlapping, one timebase. This is timing and identity metadata only;
// it carries no audio content.
class PhonemeTimeline final {
public:
    static constexpr std::size_t kMaxSegments = 4'096U;
    static constexpr std::size_t kMaxSerializedBytes = 256U * 1024U;

    [[nodiscard]] core::Result<void> insert(PhonemeSegment segment);
    [[nodiscard]] const std::vector<PhonemeSegment>& segments() const noexcept {
        return segments_;
    }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PhonemeTimeline> deserialize(
        std::string_view text);

private:
    friend struct SpeechAlignment;
    std::vector<PhonemeSegment> segments_;
};

// Speech performance source: the digest, timebase, language and confidence
// metadata of one speech alignment, with its phoneme timeline. The exact
// audio content enters only as audio_digest; raw audio bytes and any
// rights-bearing payload live outside this record, so an audio edit
// invalidates exactly the dependent alignment/viseme products.
struct SpeechAlignment {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxLanguageBytes = 32U;
    static constexpr std::size_t kMaxSerializedBytes = 256U * 1024U;

    std::uint32_t schema_version = kSchemaVersion;
    assets::Sha256Digest audio_digest{};
    std::uint32_t ticks_per_second = 0U; // rational timebase of the timeline
    std::string language;
    double confidence = 0.0; // finite, within [0, 1]
    PhonemeTimeline timeline;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<SpeechAlignment> deserialize(
        std::string_view text);
};

// Viseme weights at one rational sample time. `weights` is sorted by viseme
// name, values are finite and within (0, kMaxVisemeWeight]; an empty table is
// the explicit rest pose.
struct VisemeSample {
    character::RationalTime time{};
    std::vector<std::pair<std::string, double>> weights;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const VisemeSample& other) const noexcept {
        return time.tick == other.time.tick &&
            time.ticks_per_second == other.time.ticks_per_second &&
            weights == other.weights;
    }
};

// Lipsync evaluation result: one sample per requested sample time (sorted by
// time), plus the sorted unique set of phonemes that had no mapping entry.
// Unmapped phonemes produce the rest pose at their sample times; they are
// reported, never faked.
struct LipsyncEvaluation {
    static constexpr std::size_t kMaxSamples = 4'096U;
    static constexpr std::size_t kMaxUnmappedPhonemes = 512U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;

    std::vector<VisemeSample> samples;
    std::vector<std::string> unmapped_phonemes; // sorted, unique

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<LipsyncEvaluation> deserialize(
        std::string_view text);
};

// Constant-hold lipsync evaluation, documented contract:
// - a sample takes the active segment's mapped viseme at its mapping weight;
// - between segments all weights are zero (the explicit rest pose);
// - no coarticulation smoothing is applied at this tier; if a consumer
//   crossfades visemes it does so downstream with its own declared policy;
// - sample times must share the alignment timebase and be non-decreasing;
// - phonemes with no mapping entry are reported in unmapped_phonemes and
//   produce the rest pose at their sample times.
[[nodiscard]] core::Result<LipsyncEvaluation> evaluate_lipsync(
    const SpeechAlignment& alignment,
    const VisemeMapping& mapping,
    std::span<const character::RationalTime> sample_times);

} // namespace carto::face
