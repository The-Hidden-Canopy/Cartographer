#include <carto/temporal/sampling.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

using namespace carto;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void require_near(double left, double right, const char* message) {
    if (!(std::abs(left - right) <= 1e-12)) {
        std::cerr << "FAIL: " << message << " (" << left << " vs " << right << ")\n";
        std::exit(EXIT_FAILURE);
    }
}

temporal::TemporalChannel make_scalar_channel() {
    temporal::TemporalChannel channel;
    channel.id = temporal::ChannelId{7U};
    channel.channel_name = "beacon-intensity";
    channel.revision = core::Revision{1U};
    channel.target = {temporal::TargetBindingKind::light, "scene.beacon"};
    channel.value_kind = temporal::ChannelValueKind::scalar;
    channel.duration = 1.0;
    channel.scalar_keyframes = {{0.0, 0.0}, {0.5, 2.0}, {1.0, 4.0}};
    return channel;
}

temporal::TemporalChannel make_transform_channel() {
    temporal::TemporalChannel channel;
    channel.id = temporal::ChannelId{9U};
    channel.channel_name = "camera-dolly";
    channel.revision = core::Revision{1U};
    channel.target = {temporal::TargetBindingKind::camera, "scene.dolly"};
    channel.value_kind = temporal::ChannelValueKind::transform;
    channel.duration = 1.0;
    channel.transform_keyframes = {
        {0.0, core::Transform::identity()},
        {1.0, core::Transform{{2.0, 4.0, 0.0}, core::Quaternion::identity(),
                              {3.0, 1.0, 1.0}}},
    };
    return channel;
}

void sampling_digests_are_deterministic_and_digest_bound() {
    const temporal::TemporalChannel channel = make_scalar_channel();
    const temporal::SampleGrid grid{3U};

    const auto first = temporal::sample_channel(channel, grid);
    require(static_cast<bool>(first), "reference sampling succeeds");
    const auto second = temporal::sample_channel(channel, grid);
    require(static_cast<bool>(second), "reference sampling succeeds again");
    require(first.value().canonical_digest().value() ==
            second.value().canonical_digest().value(),
        "identical channel and grid produce identical receipt digests");
    require(first.value().channel_digest == channel.canonical_digest().value(),
        "receipts bind the exact channel digest");

    const std::string text = first.value().serialize();
    const auto decoded = temporal::SampleSet::deserialize(text);
    require(static_cast<bool>(decoded), "sample set deserializes");
    require(decoded.value().serialize() == text, "sample set serialization is canonical");
    require(decoded.value().canonical_digest().value() ==
            first.value().canonical_digest().value(),
        "receipt digest survives the round trip");

    const temporal::SampleGrid finer{5U};
    const auto denser = temporal::sample_channel(channel, finer);
    require(static_cast<bool>(denser), "denser sampling succeeds");
    require(denser.value().canonical_digest().value() !=
            first.value().canonical_digest().value(),
        "receipt digests bind the sample grid resolution");

    temporal::TemporalChannel renamed = channel;
    renamed.channel_name = "beacon-intensity-renamed";
    const auto renamed_samples = temporal::sample_channel(renamed, grid);
    require(static_cast<bool>(renamed_samples), "renamed channel still samples");
    require(renamed_samples.value().canonical_digest().value() !=
            first.value().canonical_digest().value(),
        "receipt digests bind channel content");
}

void sampling_interpolates_reference_values() {
    const temporal::TemporalChannel scalar = make_scalar_channel();
    const auto samples = temporal::sample_channel(scalar, temporal::SampleGrid{3U});
    require(static_cast<bool>(samples), "scalar sampling succeeds");
    require(samples.value().scalar_samples.size() == 3U,
        "scalar sample count matches the grid");
    require_near(samples.value().scalar_samples[0U].value, 0.0,
        "first sample hits the first keyframe");
    require_near(samples.value().scalar_samples[1U].time, 0.5,
        "sample times land on the fixed grid");
    require_near(samples.value().scalar_samples[1U].value, 2.0,
        "midpoint sample interpolates linearly");
    require_near(samples.value().scalar_samples[2U].value, 4.0,
        "last sample hits the last keyframe");

    const temporal::TemporalChannel transform = make_transform_channel();
    const auto transformed =
        temporal::sample_channel(transform, temporal::SampleGrid{3U});
    require(static_cast<bool>(transformed), "transform sampling succeeds");
    require(transformed.value().transform_samples.size() == 3U,
        "transform sample count matches the grid");
    require_near(transformed.value().transform_samples[1U].value.translation.x, 1.0,
        "midpoint translation interpolates linearly");
    require_near(transformed.value().transform_samples[1U].value.translation.y, 2.0,
        "midpoint translation interpolates linearly on every axis");
    require_near(transformed.value().transform_samples[1U].value.scale.x, 2.0,
        "midpoint scale interpolates linearly");
    require_near(transformed.value().transform_samples[1U].value.rotation.w, 1.0,
        "identity rotation stays identity through nlerp");
    require_near(transformed.value().transform_samples[2U].value.translation.x, 2.0,
        "final transform sample hits the last keyframe");

    const auto clamped = temporal::sample_channel(transform, temporal::SampleGrid{2U});
    require(static_cast<bool>(clamped), "endpoint sampling succeeds");
    require_near(clamped.value().transform_samples[0U].value.translation.x, 0.0,
        "grid endpoints include the channel start");
    require_near(clamped.value().transform_samples[1U].value.translation.x, 2.0,
        "grid endpoints include the channel end");

    require(!temporal::sample_channel(transform, temporal::SampleGrid{1U}),
        "degenerate grids fail closed");
    require(!temporal::sample_channel(transform, temporal::SampleGrid{0U}),
        "empty grids fail closed");
}

void malformed_sample_sets_fail_closed() {
    const temporal::TemporalChannel channel = make_scalar_channel();
    const auto samples = temporal::sample_channel(channel, temporal::SampleGrid{3U});
    require(static_cast<bool>(samples), "reference sampling succeeds");
    const std::string encoded = samples.value().serialize();

    require(!temporal::SampleSet::deserialize(encoded + "trailing"),
        "sample set parser rejects trailing unbound data");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_TEMPORAL_SAMPLE_SET 1"}.size(),
        "CARTOGRAPHER_TEMPORAL_SAMPLE_SET 99");
    const auto future_result = temporal::SampleSet::deserialize(future);
    require(!future_result, "future sample set versions fail");
    require(future_result.error().code == core::ErrorCode::version_mismatch,
        "future sample set versions fail with version_mismatch");

    require(!temporal::SampleSet::deserialize(
                "CARTOGRAPHER_TEMPORAL_SAMPLE_SET 1\n"
                "SAMPLE_SET 1 "
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "
                "0 3 70000 0\n"),
        "hostile sample counts are rejected before allocation");

    temporal::SampleSet scrambled = samples.value();
    std::swap(scrambled.scalar_samples[0U], scrambled.scalar_samples[1U]);
    require(!scrambled.validate(), "out-of-order samples fail validation");

    temporal::SampleSet bad_index = samples.value();
    bad_index.scalar_samples[0U].index = 9U;
    require(!bad_index.validate(), "non-sequential sample indices fail validation");

    temporal::SampleSet count_mismatch = samples.value();
    count_mismatch.sample_count = 4U;
    require(!count_mismatch.validate(),
        "sample count must agree with the stored payload");
}

} // namespace

int main() {
    sampling_digests_are_deterministic_and_digest_bound();
    sampling_interpolates_reference_values();
    malformed_sample_sets_fail_closed();
    std::cout << "temporal sampling tests passed\n";
    return EXIT_SUCCESS;
}
