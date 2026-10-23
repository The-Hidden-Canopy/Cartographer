#include <carto/face/lipsync.hpp>

#include <carto/face/phoneme.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::face {
namespace {

constexpr std::string_view kTimelineMagic = "CARTO_FACE_PHONEME_TIMELINE";
constexpr std::string_view kAlignmentMagic = "CARTO_FACE_SPEECH_ALIGNMENT";
constexpr std::string_view kEvaluationMagic = "CARTO_FACE_LIPSYNC_EVALUATION";

using detail::invalid;
using detail::parse_error;
using detail::safe_token;
using detail::validation;
using detail::version_error;

core::Result<std::uint64_t> read_count(std::istringstream& input, const char* label) {
    const auto token = detail::read_token(input, label);
    if (!token) return core::Result<std::uint64_t>::failure(token.error());
    std::uint64_t count = 0U;
    const auto parsed = std::from_chars(
        token.value().data(), token.value().data() + token.value().size(), count);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != token.value().data() + token.value().size()) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(label) + " count is invalid"));
    }
    return core::Result<std::uint64_t>::success(count);
}

core::Result<std::int64_t> read_int64(std::istringstream& input, const char* label) {
    const auto token = detail::read_token(input, label);
    if (!token) return core::Result<std::int64_t>::failure(token.error());
    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        token.value().data(), token.value().data() + token.value().size(), value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != token.value().data() + token.value().size()) {
        return core::Result<std::int64_t>::failure(parse_error(
            std::string(label) + " integer is invalid"));
    }
    return core::Result<std::int64_t>::success(value);
}

void write_time(std::ostringstream& output, character::RationalTime time) {
    output << time.tick << ' ' << time.ticks_per_second;
}

core::Result<character::RationalTime> read_time(
    std::istringstream& input,
    const char* label) {
    const auto tick = read_int64(input, label);
    if (!tick) return core::Result<character::RationalTime>::failure(tick.error());
    const auto timebase = read_count(input, label);
    if (!timebase) {
        return core::Result<character::RationalTime>::failure(timebase.error());
    }
    if (timebase.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<character::RationalTime>::failure(parse_error(
            std::string(label) + " timebase exceeds the admissible range"));
    }
    character::RationalTime time{
        tick.value(), static_cast<std::uint32_t>(timebase.value())};
    if (auto result = time.validate(); !result) {
        return core::Result<character::RationalTime>::failure(result.error());
    }
    return core::Result<character::RationalTime>::success(time);
}

void write_segments(std::ostringstream& output, std::string_view tag,
                    const std::vector<PhonemeSegment>& segments) {
    output << tag << ' ' << segments.size() << '\n';
    for (const PhonemeSegment& segment : segments) {
        output << "SEGMENT ";
        write_time(output, segment.start);
        output << ' ';
        write_time(output, segment.end);
        output << ' ' << detail::json_string(segment.phoneme) << '\n';
    }
}

core::Result<std::vector<PhonemeSegment>> read_segments(
    std::istringstream& input,
    std::uint64_t count) {
    if (count > PhonemeTimeline::kMaxSegments) {
        return core::Result<std::vector<PhonemeSegment>>::failure(parse_error(
            "phoneme segment count exceeds its bound"));
    }
    std::vector<PhonemeSegment> segments;
    segments.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        const auto tag = detail::read_token(input, "SEGMENT");
        if (!tag) {
            return core::Result<std::vector<PhonemeSegment>>::failure(tag.error());
        }
        if (tag.value() != "SEGMENT") {
            return core::Result<std::vector<PhonemeSegment>>::failure(parse_error(
                "phoneme segment framing is invalid"));
        }
        const auto start = read_time(input, "SEGMENT start");
        if (!start) {
            return core::Result<std::vector<PhonemeSegment>>::failure(start.error());
        }
        const auto end = read_time(input, "SEGMENT end");
        if (!end) {
            return core::Result<std::vector<PhonemeSegment>>::failure(end.error());
        }
        const auto phoneme = detail::read_quoted(
            input, "SEGMENT phoneme", PhonemeSegment::kMaxPhonemeBytes);
        if (!phoneme) {
            return core::Result<std::vector<PhonemeSegment>>::failure(phoneme.error());
        }
        PhonemeSegment segment;
        segment.start = start.value();
        segment.end = end.value();
        segment.phoneme = phoneme.value();
        if (auto result = segment.validate(); !result) {
            return core::Result<std::vector<PhonemeSegment>>::failure(result.error());
        }
        segments.push_back(std::move(segment));
    }
    return core::Result<std::vector<PhonemeSegment>>::success(std::move(segments));
}

void write_evaluation_payload(std::ostringstream& output,
                              const LipsyncEvaluation& evaluation) {
    output << "SAMPLES " << evaluation.samples.size() << '\n';
    for (const VisemeSample& sample : evaluation.samples) {
        output << "SAMPLE ";
        write_time(output, sample.time);
        output << " WEIGHTS " << sample.weights.size();
        for (const auto& [viseme, weight] : sample.weights) {
            output << ' ' << detail::json_string(viseme) << ' ';
            detail::write_double(output, weight);
        }
        output << '\n';
    }
    output << "UNMAPPED " << evaluation.unmapped_phonemes.size() << '\n';
    for (const std::string& phoneme : evaluation.unmapped_phonemes) {
        output << "UNMAPPED_PHONEME " << detail::json_string(phoneme) << '\n';
    }
}

core::Result<LipsyncEvaluation> read_evaluation_payload(
    std::istringstream& input,
    bool expect_end) {
    LipsyncEvaluation evaluation;
    const auto sample_tag = detail::read_token(input, "SAMPLES");
    if (!sample_tag) return core::Result<LipsyncEvaluation>::failure(sample_tag.error());
    if (sample_tag.value() != "SAMPLES") {
        return core::Result<LipsyncEvaluation>::failure(parse_error(
            "lipsync evaluation framing is invalid: SAMPLES"));
    }
    const auto sample_count = read_count(input, "SAMPLES");
    if (!sample_count) {
        return core::Result<LipsyncEvaluation>::failure(sample_count.error());
    }
    if (sample_count.value() > LipsyncEvaluation::kMaxSamples) {
        return core::Result<LipsyncEvaluation>::failure(parse_error(
            "lipsync evaluation sample count exceeds its bound"));
    }
    for (std::uint64_t index = 0U; index < sample_count.value(); ++index) {
        const auto tag = detail::read_token(input, "SAMPLE");
        if (!tag) return core::Result<LipsyncEvaluation>::failure(tag.error());
        if (tag.value() != "SAMPLE") {
            return core::Result<LipsyncEvaluation>::failure(parse_error(
                "lipsync evaluation framing is invalid: SAMPLE"));
        }
        VisemeSample sample;
        const auto time = read_time(input, "SAMPLE time");
        if (!time) return core::Result<LipsyncEvaluation>::failure(time.error());
        sample.time = time.value();
        const auto weights_tag = detail::read_token(input, "WEIGHTS");
        if (!weights_tag) {
            return core::Result<LipsyncEvaluation>::failure(weights_tag.error());
        }
        if (weights_tag.value() != "WEIGHTS") {
            return core::Result<LipsyncEvaluation>::failure(parse_error(
                "lipsync evaluation framing is invalid: WEIGHTS"));
        }
        const auto weight_count = read_count(input, "WEIGHTS");
        if (!weight_count) {
            return core::Result<LipsyncEvaluation>::failure(weight_count.error());
        }
        if (weight_count.value() > PhonemeInventory::kMaxPhonemes) {
            return core::Result<LipsyncEvaluation>::failure(parse_error(
                "lipsync evaluation weight count exceeds its bound"));
        }
        for (std::uint64_t weight = 0U; weight < weight_count.value(); ++weight) {
            const auto viseme = detail::read_quoted(
                input, "WEIGHTS viseme", VisemeMapping::kMaxVisemeBytes);
            if (!viseme) {
                return core::Result<LipsyncEvaluation>::failure(viseme.error());
            }
            const auto value_token = detail::read_token(input, "WEIGHTS value");
            if (!value_token) {
                return core::Result<LipsyncEvaluation>::failure(value_token.error());
            }
            const auto value = detail::parse_double_token(
                value_token.value(), "WEIGHTS value");
            if (!value) return core::Result<LipsyncEvaluation>::failure(value.error());
            sample.weights.emplace_back(viseme.value(), value.value());
        }
        if (auto result = sample.validate(); !result) {
            return core::Result<LipsyncEvaluation>::failure(result.error());
        }
        evaluation.samples.push_back(std::move(sample));
    }
    const auto unmapped_tag = detail::read_token(input, "UNMAPPED");
    if (!unmapped_tag) {
        return core::Result<LipsyncEvaluation>::failure(unmapped_tag.error());
    }
    if (unmapped_tag.value() != "UNMAPPED") {
        return core::Result<LipsyncEvaluation>::failure(parse_error(
            "lipsync evaluation framing is invalid: UNMAPPED"));
    }
    const auto unmapped_count = read_count(input, "UNMAPPED");
    if (!unmapped_count) {
        return core::Result<LipsyncEvaluation>::failure(unmapped_count.error());
    }
    if (unmapped_count.value() > LipsyncEvaluation::kMaxUnmappedPhonemes) {
        return core::Result<LipsyncEvaluation>::failure(parse_error(
            "lipsync evaluation unmapped count exceeds its bound"));
    }
    for (std::uint64_t index = 0U; index < unmapped_count.value(); ++index) {
        const auto tag = detail::read_token(input, "UNMAPPED_PHONEME");
        if (!tag) return core::Result<LipsyncEvaluation>::failure(tag.error());
        if (tag.value() != "UNMAPPED_PHONEME") {
            return core::Result<LipsyncEvaluation>::failure(parse_error(
                "lipsync evaluation framing is invalid: UNMAPPED_PHONEME"));
        }
        const auto phoneme = detail::read_quoted(
            input, "UNMAPPED_PHONEME", PhonemeInventory::kMaxPhonemeBytes);
        if (!phoneme) return core::Result<LipsyncEvaluation>::failure(phoneme.error());
        evaluation.unmapped_phonemes.push_back(phoneme.value());
    }
    if (expect_end) {
        if (auto result = detail::expect_end(input); !result) {
            return core::Result<LipsyncEvaluation>::failure(result.error());
        }
    }
    if (auto result = evaluation.validate(); !result) {
        return core::Result<LipsyncEvaluation>::failure(result.error());
    }
    return core::Result<LipsyncEvaluation>::success(std::move(evaluation));
}

} // namespace

core::Result<void> PhonemeSegment::validate() const {
    if (auto result = start.validate(); !result) return result;
    if (auto result = end.validate(); !result) return result;
    if (!character::same_timebase(start, end)) {
        return core::Result<void>::failure(invalid(
            "phoneme segment endpoints must share one timebase"));
    }
    if (character::compare(start, end) != std::strong_ordering::less) {
        return core::Result<void>::failure(invalid(
            "phoneme segment end must be strictly after its start"));
    }
    if (!safe_token(phoneme, kMaxPhonemeBytes)) {
        return core::Result<void>::failure(invalid(
            "phoneme segment token is not a bounded safe token"));
    }
    return core::Result<void>::success();
}

core::Result<void> PhonemeTimeline::insert(PhonemeSegment segment) {
    if (auto result = segment.validate(); !result) return result;
    if (segments_.size() >= kMaxSegments) {
        return core::Result<void>::failure(invalid(
            "phoneme timeline exceeds its segment bound"));
    }
    const auto position = std::lower_bound(
        segments_.begin(), segments_.end(), segment,
        [](const PhonemeSegment& left, const PhonemeSegment& right) {
            return character::compare(left.start, right.start) ==
                std::strong_ordering::less;
        });
    if (position != segments_.end() &&
        character::compare(position->start, segment.end) == std::strong_ordering::less) {
        return core::Result<void>::failure(invalid(
            "phoneme timeline segments must not overlap"));
    }
    if (position != segments_.begin()) {
        const auto previous = position - 1;
        if (character::compare(previous->end, segment.start) ==
            std::strong_ordering::greater) {
            return core::Result<void>::failure(invalid(
                "phoneme timeline segments must not overlap"));
        }
    }
    segments_.insert(position, std::move(segment));
    return core::Result<void>::success();
}

core::Result<void> PhonemeTimeline::validate() const {
    if (segments_.size() > kMaxSegments) {
        return core::Result<void>::failure(validation(
            "phoneme timeline exceeds its segment bound"));
    }
    for (const PhonemeSegment& segment : segments_) {
        if (auto result = segment.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < segments_.size(); ++index) {
        if (character::compare(segments_[index - 1U].start, segments_[index].start) !=
                std::strong_ordering::less ||
            character::compare(segments_[index - 1U].end, segments_[index].start) ==
                std::strong_ordering::greater) {
            return core::Result<void>::failure(validation(
                "phoneme timeline must be sorted, non-overlapping, one timebase"));
        }
    }
    return core::Result<void>::success();
}

std::string PhonemeTimeline::serialize() const {
    std::ostringstream output;
    output << kTimelineMagic << ' ' << 1U << '\n';
    write_segments(output, "SEGMENTS", segments_);
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PhonemeTimeline::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<PhonemeTimeline> PhonemeTimeline::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PhonemeTimeline>::failure(parse_error(
            "phoneme timeline text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kTimelineMagic, 1U); !result) {
        return core::Result<PhonemeTimeline>::failure(result.error());
    }
    const auto tag = detail::read_token(input, "SEGMENTS");
    if (!tag) return core::Result<PhonemeTimeline>::failure(tag.error());
    if (tag.value() != "SEGMENTS") {
        return core::Result<PhonemeTimeline>::failure(parse_error(
            "phoneme timeline framing is invalid: SEGMENTS"));
    }
    const auto count = read_count(input, "SEGMENTS");
    if (!count) return core::Result<PhonemeTimeline>::failure(count.error());
    const auto segments = read_segments(input, count.value());
    if (!segments) return core::Result<PhonemeTimeline>::failure(segments.error());
    if (auto result = detail::expect_end(input); !result) {
        return core::Result<PhonemeTimeline>::failure(result.error());
    }
    PhonemeTimeline timeline;
    timeline.segments_ = segments.value();
    if (auto result = timeline.validate(); !result) {
        return core::Result<PhonemeTimeline>::failure(result.error());
    }
    return core::Result<PhonemeTimeline>::success(std::move(timeline));
}

core::Result<void> SpeechAlignment::validate() const {
    if (schema_version != kSchemaVersion) {
        return core::Result<void>::failure(version_error(
            "speech alignment schema version is not supported"));
    }
    if (audio_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "speech alignment audio digest must be nonzero"));
    }
    if (ticks_per_second == 0U) {
        return core::Result<void>::failure(invalid(
            "speech alignment requires a positive ticks-per-second"));
    }
    if (!safe_token(language, kMaxLanguageBytes)) {
        return core::Result<void>::failure(invalid(
            "speech alignment language is not a bounded safe token"));
    }
    if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
        return core::Result<void>::failure(invalid(
            "speech alignment confidence must be finite and within [0, 1]"));
    }
    if (auto result = timeline.validate(); !result) return result;
    for (const PhonemeSegment& segment : timeline.segments()) {
        if (segment.start.ticks_per_second != ticks_per_second) {
            return core::Result<void>::failure(validation(
                "speech alignment segments must share the declared timebase"));
        }
    }
    return core::Result<void>::success();
}

std::string SpeechAlignment::serialize() const {
    std::ostringstream output;
    output << kAlignmentMagic << ' ' << kSchemaVersion << '\n';
    output << "AUDIO_SHA256 " << detail::json_string(audio_digest.hex()) << '\n';
    output << "TICKS_PER_SECOND " << ticks_per_second << '\n';
    output << "LANGUAGE " << detail::json_string(language) << '\n';
    output << "CONFIDENCE ";
    detail::write_double(output, confidence);
    output << '\n';
    write_segments(output, "SEGMENTS", timeline.segments());
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> SpeechAlignment::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<SpeechAlignment> SpeechAlignment::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kAlignmentMagic, kSchemaVersion); !result) {
        return core::Result<SpeechAlignment>::failure(result.error());
    }
    SpeechAlignment alignment;
    const auto digest_tag = detail::read_token(input, "AUDIO_SHA256");
    if (!digest_tag) return core::Result<SpeechAlignment>::failure(digest_tag.error());
    if (digest_tag.value() != "AUDIO_SHA256") {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment framing is invalid: AUDIO_SHA256"));
    }
    const auto digest_hex = detail::read_quoted(input, "AUDIO_SHA256", 64U);
    if (!digest_hex) return core::Result<SpeechAlignment>::failure(digest_hex.error());
    const auto digest = assets::Sha256Digest::from_hex(digest_hex.value());
    if (!digest) {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment audio digest is malformed"));
    }
    alignment.audio_digest = digest.value();
    const auto timebase_tag = detail::read_token(input, "TICKS_PER_SECOND");
    if (!timebase_tag) {
        return core::Result<SpeechAlignment>::failure(timebase_tag.error());
    }
    if (timebase_tag.value() != "TICKS_PER_SECOND") {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment framing is invalid: TICKS_PER_SECOND"));
    }
    const auto timebase = read_count(input, "TICKS_PER_SECOND");
    if (!timebase) return core::Result<SpeechAlignment>::failure(timebase.error());
    if (timebase.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment timebase exceeds the admissible range"));
    }
    alignment.ticks_per_second = static_cast<std::uint32_t>(timebase.value());
    const auto language_tag = detail::read_token(input, "LANGUAGE");
    if (!language_tag) {
        return core::Result<SpeechAlignment>::failure(language_tag.error());
    }
    if (language_tag.value() != "LANGUAGE") {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment framing is invalid: LANGUAGE"));
    }
    const auto language = detail::read_quoted(input, "LANGUAGE", kMaxLanguageBytes);
    if (!language) return core::Result<SpeechAlignment>::failure(language.error());
    alignment.language = language.value();
    const auto confidence_tag = detail::read_token(input, "CONFIDENCE");
    if (!confidence_tag) {
        return core::Result<SpeechAlignment>::failure(confidence_tag.error());
    }
    if (confidence_tag.value() != "CONFIDENCE") {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment framing is invalid: CONFIDENCE"));
    }
    const auto confidence_token = detail::read_token(input, "CONFIDENCE");
    if (!confidence_token) {
        return core::Result<SpeechAlignment>::failure(confidence_token.error());
    }
    const auto confidence = detail::parse_double_token(
        confidence_token.value(), "CONFIDENCE");
    if (!confidence) return core::Result<SpeechAlignment>::failure(confidence.error());
    alignment.confidence = confidence.value();
    const auto segments_tag = detail::read_token(input, "SEGMENTS");
    if (!segments_tag) {
        return core::Result<SpeechAlignment>::failure(segments_tag.error());
    }
    if (segments_tag.value() != "SEGMENTS") {
        return core::Result<SpeechAlignment>::failure(parse_error(
            "speech alignment framing is invalid: SEGMENTS"));
    }
    const auto count = read_count(input, "SEGMENTS");
    if (!count) return core::Result<SpeechAlignment>::failure(count.error());
    const auto segments = read_segments(input, count.value());
    if (!segments) return core::Result<SpeechAlignment>::failure(segments.error());
    alignment.timeline.segments_ = segments.value();
    if (auto result = detail::expect_end(input); !result) {
        return core::Result<SpeechAlignment>::failure(result.error());
    }
    if (auto result = alignment.validate(); !result) {
        return core::Result<SpeechAlignment>::failure(result.error());
    }
    return core::Result<SpeechAlignment>::success(std::move(alignment));
}

core::Result<void> VisemeSample::validate() const {
    if (auto result = time.validate(); !result) return result;
    for (const auto& [viseme, weight] : weights) {
        if (!safe_token(viseme, VisemeMapping::kMaxVisemeBytes)) {
            return core::Result<void>::failure(validation(
                "viseme sample name is not a bounded safe token"));
        }
        if (!std::isfinite(weight) || weight <= 0.0 || weight > kMaxVisemeWeight) {
            return core::Result<void>::failure(validation(
                "viseme sample weight must be finite and within (0, kMaxVisemeWeight]"));
        }
    }
    const auto by_name = [](const std::pair<std::string, double>& left,
                            const std::pair<std::string, double>& right) {
        return left.first < right.first;
    };
    if (!std::is_sorted(weights.begin(), weights.end(), by_name) ||
        std::adjacent_find(weights.begin(), weights.end(),
            [](const std::pair<std::string, double>& left,
               const std::pair<std::string, double>& right) {
                return left.first == right.first;
            }) != weights.end()) {
        return core::Result<void>::failure(validation(
            "viseme sample weights must be sorted and unique by viseme name"));
    }
    return core::Result<void>::success();
}

core::Result<void> LipsyncEvaluation::validate() const {
    if (samples.size() > kMaxSamples) {
        return core::Result<void>::failure(validation(
            "lipsync evaluation exceeds its sample bound"));
    }
    for (const VisemeSample& sample : samples) {
        if (auto result = sample.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < samples.size(); ++index) {
        if (character::compare(samples[index - 1U].time, samples[index].time) !=
            std::strong_ordering::less) {
            return core::Result<void>::failure(validation(
                "lipsync evaluation samples must be sorted and unique by time"));
        }
    }
    if (unmapped_phonemes.size() > kMaxUnmappedPhonemes) {
        return core::Result<void>::failure(validation(
            "lipsync evaluation exceeds its unmapped phoneme bound"));
    }
    for (const std::string& phoneme : unmapped_phonemes) {
        if (!safe_token(phoneme, PhonemeInventory::kMaxPhonemeBytes)) {
            return core::Result<void>::failure(validation(
                "unmapped phoneme is not a bounded safe token"));
        }
    }
    if (!std::is_sorted(unmapped_phonemes.begin(), unmapped_phonemes.end()) ||
        std::adjacent_find(unmapped_phonemes.begin(), unmapped_phonemes.end()) !=
            unmapped_phonemes.end()) {
        return core::Result<void>::failure(validation(
            "unmapped phonemes must be sorted and unique"));
    }
    return core::Result<void>::success();
}

std::string LipsyncEvaluation::serialize() const {
    std::ostringstream output;
    output << kEvaluationMagic << ' ' << 1U << '\n';
    write_evaluation_payload(output, *this);
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> LipsyncEvaluation::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<LipsyncEvaluation> LipsyncEvaluation::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<LipsyncEvaluation>::failure(parse_error(
            "lipsync evaluation text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kEvaluationMagic, 1U); !result) {
        return core::Result<LipsyncEvaluation>::failure(result.error());
    }
    return read_evaluation_payload(input, true);
}

core::Result<LipsyncEvaluation> evaluate_lipsync(
    const SpeechAlignment& alignment,
    const VisemeMapping& mapping,
    std::span<const character::RationalTime> sample_times) {
    if (auto result = alignment.validate(); !result) {
        return core::Result<LipsyncEvaluation>::failure(result.error());
    }
    if (auto result = mapping.validate(); !result) {
        return core::Result<LipsyncEvaluation>::failure(result.error());
    }
    if (sample_times.size() > LipsyncEvaluation::kMaxSamples) {
        return core::Result<LipsyncEvaluation>::failure(invalid(
            "lipsync sample request exceeds its sample bound"));
    }
    for (const character::RationalTime time : sample_times) {
        if (auto result = time.validate(); !result) {
            return core::Result<LipsyncEvaluation>::failure(result.error());
        }
        if (time.ticks_per_second != alignment.ticks_per_second) {
            return core::Result<LipsyncEvaluation>::failure(invalid(
                "lipsync sample times must share the alignment timebase"));
        }
    }
    for (std::size_t index = 1U; index < sample_times.size(); ++index) {
        if (character::compare(sample_times[index - 1U], sample_times[index]) !=
            std::strong_ordering::less) {
            return core::Result<LipsyncEvaluation>::failure(invalid(
                "lipsync sample times must be strictly increasing"));
        }
    }
    LipsyncEvaluation evaluation;
    std::set<std::string> unmapped;
    const std::vector<PhonemeSegment>& segments = alignment.timeline.segments();
    std::size_t cursor = 0U;
    for (const character::RationalTime time : sample_times) {
        // Samples are sorted and segments are sorted and non-overlapping, so
        // the active segment cursor only advances; no unordered iteration ever
        // enters a canonical output. Segments are half-open [start, end).
        while (cursor < segments.size() &&
               character::compare(segments[cursor].end, time) !=
                   std::strong_ordering::greater) {
            ++cursor;
        }
        VisemeSample sample;
        sample.time = time;
        if (cursor < segments.size() &&
            character::compare(segments[cursor].start, time) !=
                std::strong_ordering::greater &&
            character::compare(time, segments[cursor].end) ==
                std::strong_ordering::less) {
            const auto lookup = mapping.map(segments[cursor].phoneme);
            if (lookup) {
                sample.weights.emplace_back(
                    std::string(lookup.value().viseme), lookup.value().weight);
            } else {
                unmapped.insert(segments[cursor].phoneme);
            }
        }
        evaluation.samples.push_back(std::move(sample));
    }
    evaluation.unmapped_phonemes.assign(unmapped.begin(), unmapped.end());
    if (auto result = evaluation.validate(); !result) {
        return core::Result<LipsyncEvaluation>::failure(result.error());
    }
    return core::Result<LipsyncEvaluation>::success(std::move(evaluation));
}

} // namespace carto::face
