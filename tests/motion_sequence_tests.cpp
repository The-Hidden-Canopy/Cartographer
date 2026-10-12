#include <carto/temporal/root_motion.hpp>
#include <carto/temporal/sequence.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace carto;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest_of(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

temporal::MotionClip make_clip() {
    temporal::MotionClip clip;
    clip.id = temporal::ClipId{41U};
    clip.clip_name = "lift-cycle";
    clip.revision = core::Revision{2U};
    clip.duration = 2.0;
    clip.channels = {
        {temporal::ChannelId{7U}, digest_of("channel:7:rev3")},
        {temporal::ChannelId{9U}, digest_of("channel:9:rev1")},
    };
    return clip;
}

temporal::MotionSequence make_sequence() {
    temporal::MotionSequence sequence;
    sequence.id = temporal::SequenceId{100U};
    sequence.sequence_name = "lift-presentation";
    sequence.revision = core::Revision{1U};
    sequence.duration = 6.0;
    sequence.loop = temporal::LoopPolicy::loop;
    sequence.purpose = temporal::SequencePurpose::presentation;
    sequence.clips = {{temporal::ClipId{41U}, digest_of("clip:41:rev2")}};
    sequence.channels = {{temporal::ChannelId{9U}, digest_of("channel:9:rev1")}};
    sequence.event_markers = {{temporal::EventId{55U}, 1.25, "presentation-begin"}};
    return sequence;
}

void sequences_round_trip_with_stable_digests() {
    const temporal::MotionClip clip = make_clip();
    require(static_cast<bool>(clip.validate()), "canonical clip validates");
    const std::string clip_text = clip.serialize();
    const auto clip_decoded = temporal::MotionClip::deserialize(clip_text);
    require(static_cast<bool>(clip_decoded), "clip deserializes");
    require(clip_decoded.value().serialize() == clip_text,
        "clip serialization is canonical");
    require(clip_decoded.value().canonical_digest().value() ==
            clip.canonical_digest().value(),
        "round trip preserves the clip digest");

    const temporal::MotionSequence sequence = make_sequence();
    require(static_cast<bool>(sequence.validate()), "canonical sequence validates");
    const std::string text = sequence.serialize();
    const auto decoded = temporal::MotionSequence::deserialize(text);
    require(static_cast<bool>(decoded), "sequence deserializes");
    require(decoded.value().id == sequence.id, "round trip preserves sequence identity");
    require(decoded.value().serialize() == text, "sequence serialization is canonical");
    require(decoded.value().canonical_digest().value() ==
            sequence.canonical_digest().value(),
        "round trip preserves the sequence digest");
    require(decoded.value().purpose == temporal::SequencePurpose::presentation,
        "presentation purpose survives the round trip");
}

void sequences_validate_ordering_and_loop_policy() {
    const temporal::MotionSequence sequence = make_sequence();

    temporal::MotionSequence duplicate_clip = sequence;
    duplicate_clip.clips.push_back(sequence.clips.front());
    require(!duplicate_clip.validate(), "duplicate clip references fail validation");

    temporal::MotionSequence duplicate_channel = sequence;
    duplicate_channel.channels.push_back(sequence.channels.front());
    require(!duplicate_channel.validate(),
        "duplicate channel references fail validation");

    temporal::MotionSequence zero_digest = sequence;
    zero_digest.clips.front().clip_digest = assets::Sha256Digest{};
    require(!zero_digest.validate(), "references without digests fail validation");

    temporal::MotionSequence empty = sequence;
    empty.clips.clear();
    empty.channels.clear();
    require(!empty.validate(), "sequences without references fail validation");

    temporal::MotionSequence late_marker = sequence;
    late_marker.event_markers.front().time = 7.0;
    require(!late_marker.validate(), "markers past the sequence duration fail");

    temporal::MotionSequence bad_loop = sequence;
    bad_loop.loop = static_cast<temporal::LoopPolicy>(99U);
    require(!bad_loop.validate(), "unknown loop policies fail validation");

    temporal::MotionSequence bad_purpose = sequence;
    bad_purpose.purpose = static_cast<temporal::SequencePurpose>(99U);
    require(!bad_purpose.validate(), "unknown purposes fail validation");

    temporal::MotionSequence reordered = sequence;
    reordered.clips = {
        {temporal::ClipId{43U}, digest_of("clip:43:rev1")},
        {temporal::ClipId{41U}, digest_of("clip:41:rev2")},
    };
    require(static_cast<bool>(reordered.validate()),
        "reordered clip references remain valid");
    require(reordered.canonical_digest().value() != sequence.canonical_digest().value(),
        "clip playback order is content and changes the digest");
}

void clip_identity_is_stable_across_revision() {
    const temporal::MotionClip original = make_clip();
    const assets::Sha256Digest original_digest = original.canonical_digest().value();

    temporal::MotionClip revised = original;
    revised.revision = original.revision.next();
    require(revised.id == original.id,
        "revision changes never reissue clip identity");
    require(revised.revision.value() == original.revision.value() + 1U,
        "revision advances by one");
    require(revised.canonical_digest().value() != original_digest,
        "revision changes re-version the clip digest");

    const auto reimported = temporal::MotionClip::deserialize(original.serialize());
    require(static_cast<bool>(reimported), "clip reimport succeeds");
    require(reimported.value().id == original.id,
        "reimport preserves clip identity across revision");
    require(reimported.value().canonical_digest().value() == original_digest,
        "reimport preserves the exact clip revision digest");

    temporal::MotionSequence sequence = make_sequence();
    sequence.clips = {{original.id, original_digest}};
    require(static_cast<bool>(sequence.validate()),
        "sequences bind clip identity plus exact revision digest");
    sequence.clips = {{original.id, revised.canonical_digest().value()}};
    require(static_cast<bool>(sequence.validate()),
        "sequences rebind cleanly to the revised clip digest");
}

void malformed_sequences_fail_closed() {
    const temporal::MotionSequence sequence = make_sequence();
    const std::string encoded = sequence.serialize();

    require(!temporal::MotionSequence::deserialize(encoded + "trailing"),
        "sequence parser rejects trailing unbound data");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_MOTION_SEQUENCE 1"}.size(),
        "CARTOGRAPHER_MOTION_SEQUENCE 99");
    const auto future_result = temporal::MotionSequence::deserialize(future);
    require(!future_result, "future sequence versions fail");
    require(future_result.error().code == core::ErrorCode::version_mismatch,
        "future sequence versions fail with version_mismatch");

    require(!temporal::MotionSequence::deserialize(
                "CARTOGRAPHER_MOTION_SEQUENCE 1\n"
                "SEQUENCE 1 1 \"s\" 1.0 0 0 70000 0 0\n"),
        "hostile clip counts are rejected before allocation");

    const temporal::MotionClip clip = make_clip();
    const std::string clip_text = clip.serialize();
    require(!temporal::MotionClip::deserialize(clip_text + "trailing"),
        "clip parser rejects trailing unbound data");
}

void root_motion_proposals_never_mutate_source() {
    temporal::TemporalChannel channel;
    channel.id = temporal::ChannelId{7U};
    channel.channel_name = "root-motion-source";
    channel.revision = core::Revision{4U};
    channel.target = {temporal::TargetBindingKind::rigid_object, "walker.root"};
    channel.value_kind = temporal::ChannelValueKind::transform;
    channel.duration = 1.0;
    channel.transform_keyframes = {
        {0.0, core::Transform::identity()},
        {1.0, core::Transform{{2.0, 0.0, 0.0}, core::Quaternion::identity(),
                              {1.0, 1.0, 1.0}}},
    };
    require(static_cast<bool>(channel.validate()), "transform channel validates");

    const auto channel_digest = channel.canonical_digest().value();
    const auto before_keys = channel.transform_keyframes.size();

    const auto proposal = temporal::propose_root_motion(
        channel, temporal::RootMotionHint::extract_and_strip);
    require(static_cast<bool>(proposal), "transform channel yields a proposal");
    require(proposal.value().source_channel == channel.id,
        "proposal names its source channel");
    require(proposal.value().source_revision == channel.revision,
        "proposal binds the source revision");
    require(proposal.value().curve.size() == before_keys,
        "proposal curve mirrors the source keyframes");
    require(proposal.value().curve.front().translation.x == 0.0 &&
            proposal.value().curve.back().translation.x == 2.0,
        "proposal curve carries the authored root translation");

    require(channel.canonical_digest().value() == channel_digest,
        "proposing root motion mutates nothing in the source record");
    require(channel.transform_keyframes.size() == before_keys &&
            channel.transform_keyframes.back().value.translation.x == 2.0,
        "source keyframes are untouched after extraction");

    const std::string text = proposal.value().serialize();
    const auto decoded = temporal::RootMotionProposal::deserialize(text);
    require(static_cast<bool>(decoded), "proposal deserializes");
    require(decoded.value().serialize() == text, "proposal serialization is canonical");
    require(decoded.value().canonical_digest().value() ==
            proposal.value().canonical_digest().value(),
        "proposal digest survives the round trip");

    temporal::TemporalChannel scalar = channel;
    scalar.value_kind = temporal::ChannelValueKind::scalar;
    scalar.transform_keyframes.clear();
    scalar.scalar_keyframes = {{0.0, 1.0}};
    const auto scalar_proposal = temporal::propose_root_motion(
        scalar, temporal::RootMotionHint::extract_and_retain);
    require(!scalar_proposal, "scalar channels cannot propose root motion");
    require(scalar_proposal.error().code == core::ErrorCode::unsupported,
        "scalar root motion proposals report unsupported");

    std::string future = text;
    future.replace(0U, std::string{"CARTOGRAPHER_ROOT_MOTION_PROPOSAL 1"}.size(),
        "CARTOGRAPHER_ROOT_MOTION_PROPOSAL 99");
    require(!temporal::RootMotionProposal::deserialize(future),
        "future proposal versions fail");
    require(!temporal::RootMotionProposal::deserialize(text + "trailing"),
        "proposal parser rejects trailing unbound data");
}

} // namespace

int main() {
    sequences_round_trip_with_stable_digests();
    sequences_validate_ordering_and_loop_policy();
    clip_identity_is_stable_across_revision();
    malformed_sequences_fail_closed();
    root_motion_proposals_never_mutate_source();
    std::cout << "motion sequence tests passed\n";
    return EXIT_SUCCESS;
}
