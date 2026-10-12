#include <carto/temporal/channel.hpp>
#include <carto/temporal/events.hpp>
#include <carto/temporal/mechanical.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace carto;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

temporal::TemporalChannel make_scalar_channel() {
    temporal::TemporalChannel channel;
    channel.id = temporal::ChannelId{7U};
    channel.channel_name = "lift-height";
    channel.revision = core::Revision{3U};
    channel.target = {temporal::TargetBindingKind::rigid_object, "mechanism.lift.arm"};
    channel.value_kind = temporal::ChannelValueKind::scalar;
    channel.duration = 2.0;
    channel.scalar_keyframes = {{0.0, 0.0}, {1.0, 4.0}, {2.0, 0.0}};
    channel.event_markers = {
        {temporal::EventId{11U}, 0.5, "lift-start"},
        {temporal::EventId{12U}, 1.5, "lift-peak"},
    };
    return channel;
}

void valid_channels_round_trip_with_stable_digests() {
    const temporal::TemporalChannel channel = make_scalar_channel();
    require(static_cast<bool>(channel.validate()),
        "canonical scalar channel validates");

    const std::string text = channel.serialize();
    const auto decoded = temporal::TemporalChannel::deserialize(text);
    require(static_cast<bool>(decoded), "channel deserializes");
    require(decoded.value().id == channel.id, "round trip preserves channel identity");
    require(decoded.value().serialize() == text, "channel serialization is canonical");
    require(decoded.value().canonical_digest().value() == channel.canonical_digest().value(),
        "round trip preserves the channel digest");

    const temporal::TemporalChannel rebuilt = make_scalar_channel();
    require(rebuilt.canonical_digest().value() == channel.canonical_digest().value(),
        "independently authored identical channels share one digest");

    temporal::TemporalChannel renamed = channel;
    renamed.channel_name = "lift-height-renamed";
    require(renamed.canonical_digest().value() != channel.canonical_digest().value(),
        "digest binds the channel name");
}

void malformed_channels_fail_closed() {
    const temporal::TemporalChannel channel = make_scalar_channel();
    const std::string encoded = channel.serialize();

    require(!temporal::TemporalChannel::deserialize(encoded + "trailing"),
        "channel parser rejects trailing unbound data");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_TEMPORAL_CHANNEL 1"}.size(),
        "CARTOGRAPHER_TEMPORAL_CHANNEL 99");
    const auto future_result = temporal::TemporalChannel::deserialize(future);
    require(!future_result, "future channel versions fail");
    require(future_result.error().code == core::ErrorCode::version_mismatch,
        "future channel versions fail with version_mismatch");

    require(!temporal::TemporalChannel::deserialize(
                "CARTOGRAPHER_TEMPORAL_CHANNEL 1\n"
                "CHANNEL 1 1 \"c\" 1.0 1 \"t\" 0 70000 0 0\n"),
        "hostile keyframe counts are rejected before allocation");

    temporal::TemporalChannel unsorted = channel;
    std::swap(unsorted.scalar_keyframes[0U], unsorted.scalar_keyframes[2U]);
    require(!unsorted.validate(), "unsorted keyframes fail validation");

    temporal::TemporalChannel duplicate_time = channel;
    duplicate_time.scalar_keyframes[1U].time = 0.0;
    require(!duplicate_time.validate(), "duplicate keyframe times fail validation");

    temporal::TemporalChannel out_of_range = channel;
    out_of_range.scalar_keyframes[2U].time = 3.0;
    require(!out_of_range.validate(), "keyframes past the duration fail validation");

    temporal::TemporalChannel non_finite = channel;
    non_finite.scalar_keyframes[1U].value =
        std::numeric_limits<double>::quiet_NaN();
    require(!non_finite.validate(), "non-finite keyframe values fail validation");

    temporal::TemporalChannel duplicate_markers = channel;
    duplicate_markers.event_markers[1U].id = duplicate_markers.event_markers[0U].id;
    require(!duplicate_markers.validate(), "duplicate marker identities fail validation");

    temporal::TemporalChannel late_marker = channel;
    late_marker.event_markers[1U].time = 2.5;
    require(!late_marker.validate(), "markers past the duration fail validation");

    temporal::TemporalChannel wrong_payload = channel;
    wrong_payload.value_kind = temporal::ChannelValueKind::transform;
    require(!wrong_payload.validate(), "value kind must agree with its keyframe payload");

    temporal::TemporalChannel empty = channel;
    empty.scalar_keyframes.clear();
    require(!empty.validate(), "channels without keyframes fail validation");
}

void rigid_targets_are_legal_and_skin_is_rejected() {
    temporal::RigidConsumerDescriptor consumer;
    consumer.consumer_name = "lift-mechanism";
    consumer.targets = {
        {temporal::TargetBindingKind::rigid_object, "mechanism.lift.arm"},
        {temporal::TargetBindingKind::mechanical_subassembly, "mechanism.lift.gear"},
    };
    require(static_cast<bool>(consumer.validate()),
        "rigid consumers accept rigid and mechanical targets");

    const std::string text = consumer.serialize();
    const auto decoded = temporal::RigidConsumerDescriptor::deserialize(text);
    require(static_cast<bool>(decoded), "rigid consumer deserializes");
    require(decoded.value().serialize() == text, "rigid consumer serialization is canonical");

    temporal::RigidConsumerDescriptor skinned = consumer;
    skinned.targets = {{temporal::TargetBindingKind::skinned_mesh, "character.torso"}};
    const auto skinned_result = skinned.validate();
    require(!skinned_result, "skinned targets fail on rigid consumers");
    require(skinned_result.error().code == core::ErrorCode::unsupported,
        "skinned rejection reports unsupported");

    const temporal::TemporalChannel channel = make_scalar_channel();
    require(static_cast<bool>(temporal::validate_rigid_channel(channel)),
        "rigid-bound channels are legal on rigid consumers");
    require(static_cast<bool>(temporal::bind_channel_to_rigid_consumer(channel, consumer)),
        "channel binds to the consumer offering its target");

    temporal::TemporalChannel skin_channel = channel;
    skin_channel.target = {temporal::TargetBindingKind::skinned_mesh, "character.torso"};
    const auto skin_channel_result = temporal::validate_rigid_channel(skin_channel);
    require(!skin_channel_result,
        "skin-targeted channels are rejected on rigid-only consumers");
    require(skin_channel_result.error().code == core::ErrorCode::unsupported,
        "skin-targeted channel rejection reports unsupported");
    require(!temporal::bind_channel_to_rigid_consumer(skin_channel, consumer),
        "skin-targeted channels cannot bind to rigid consumers");

    temporal::TemporalChannel unoffered = channel;
    unoffered.target = {temporal::TargetBindingKind::rigid_object, "mechanism.door"};
    require(!temporal::bind_channel_to_rigid_consumer(unoffered, consumer),
        "channels fail closed when the consumer does not offer their target");
}

void unbound_events_refuse_dispatch() {
    const temporal::TemporalChannel channel = make_scalar_channel();

    temporal::EventBindingTable table;
    require(static_cast<bool>(table.bind(temporal::EventId{11U}, "gameplay.lift.begin")),
        "explicit binding is accepted");
    require(!table.bind(temporal::EventId{11U}, "gameplay.lift.duplicate"),
        "duplicate marker binding fails closed");
    require(!table.bind(temporal::EventId{0U}, "gameplay.lift.zero"),
        "zero marker identity fails closed");

    const auto resolved = table.resolve(temporal::EventId{11U});
    require(static_cast<bool>(resolved), "bound markers resolve");
    require(resolved.value() == "gameplay.lift.begin", "binding name round trips");

    const auto missing = table.resolve(temporal::EventId{12U});
    require(!missing, "unbound markers refuse dispatch");
    require(missing.error().code == core::ErrorCode::not_found,
        "unbound markers report not_found");

    const auto verification = temporal::verify_channel_events(channel, table);
    require(!verification, "channels with unbound markers fail event verification");
    require(verification.error().code == core::ErrorCode::not_found,
        "event verification fails closed with not_found");
    require(!verification.error().context.empty(), "refusal names its context");

    require(static_cast<bool>(table.bind(temporal::EventId{12U}, "gameplay.lift.peak")),
        "second explicit binding is accepted");
    require(static_cast<bool>(temporal::verify_channel_events(channel, table)),
        "fully bound channels pass event verification");

    const std::string text = table.serialize();
    const auto decoded = temporal::EventBindingTable::deserialize(text);
    require(static_cast<bool>(decoded), "binding table deserializes");
    require(decoded.value().serialize() == text, "binding table serialization is canonical");
    require(!temporal::EventBindingTable::deserialize(text + "trailing"),
        "binding table parser rejects trailing unbound data");

    temporal::EventBindingTable empty_table;
    require(!temporal::verify_channel_events(channel, empty_table),
        "an empty binding table refuses every marker");
}

} // namespace

int main() {
    valid_channels_round_trip_with_stable_digests();
    malformed_channels_fail_closed();
    rigid_targets_are_legal_and_skin_is_rejected();
    unbound_events_refuse_dispatch();
    std::cout << "temporal channel tests passed\n";
    return EXIT_SUCCESS;
}
