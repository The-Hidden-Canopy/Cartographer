#include <carto/temporal/sampling.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace carto::temporal {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_TEMPORAL_SAMPLE_SET";

double interpolate_scalar(const TemporalChannel& channel, double time) {
    const auto& keys = channel.scalar_keyframes;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](double value, const ScalarKeyframe& key) { return value < key.time; });
    const auto& first = *(upper - 1);
    const auto& second = *upper;
    const double span = second.time - first.time;
    const double fraction = (time - first.time) / span;
    return first.value + (second.value - first.value) * fraction;
}

core::Transform interpolate_transform(const TemporalChannel& channel, double time) {
    const auto& keys = channel.transform_keyframes;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](double value, const TransformKeyframe& key) { return value < key.time; });
    const auto& first = *(upper - 1);
    const auto& second = *upper;
    const double span = second.time - first.time;
    const double fraction = (time - first.time) / span;

    core::Quaternion first_rotation = first.value.rotation.normalized();
    core::Quaternion second_rotation = second.value.rotation.normalized();
    double alignment = first_rotation.x * second_rotation.x +
        first_rotation.y * second_rotation.y + first_rotation.z * second_rotation.z +
        first_rotation.w * second_rotation.w;
    if (alignment < 0.0) {
        second_rotation = {-second_rotation.x, -second_rotation.y, -second_rotation.z,
                           -second_rotation.w};
        alignment = -alignment;
    }
    const core::Quaternion mixed{
        first_rotation.x + (second_rotation.x - first_rotation.x) * fraction,
        first_rotation.y + (second_rotation.y - first_rotation.y) * fraction,
        first_rotation.z + (second_rotation.z - first_rotation.z) * fraction,
        first_rotation.w + (second_rotation.w - first_rotation.w) * fraction,
    };

    core::Transform result;
    result.translation = {
        first.value.translation.x +
            (second.value.translation.x - first.value.translation.x) * fraction,
        first.value.translation.y +
            (second.value.translation.y - first.value.translation.y) * fraction,
        first.value.translation.z +
            (second.value.translation.z - first.value.translation.z) * fraction,
    };
    result.rotation = mixed.normalized();
    result.scale = {
        first.value.scale.x + (second.value.scale.x - first.value.scale.x) * fraction,
        first.value.scale.y + (second.value.scale.y - first.value.scale.y) * fraction,
        first.value.scale.z + (second.value.scale.z - first.value.scale.z) * fraction,
    };
    return result;
}

} // namespace

core::Result<void> SampleGrid::validate() const {
    if (sample_count < 2U || sample_count > kMaxSamplesPerGrid) {
        return core::Result<void>::failure(detail::invalid(
            "sample grid resolution is outside its bounded range"));
    }
    return core::Result<void>::success();
}

core::Result<void> SampleSet::validate() const {
    if (!channel_id || channel_digest.is_zero() ||
        sample_count < 2U || sample_count > kMaxSamplesPerSet ||
        scalar_samples.size() > kMaxSamplesPerSet ||
        transform_samples.size() > kMaxSamplesPerSet) {
        return core::Result<void>::failure(detail::invalid(
            "sample set identity, digest, resolution, or bounded size is invalid"));
    }
    switch (value_kind) {
    case ChannelValueKind::scalar:
        if (scalar_samples.size() != sample_count || !transform_samples.empty()) {
            return core::Result<void>::failure(detail::validation(
                "sample set scalar payload disagrees with its resolution"));
        }
        for (std::size_t index = 0U; index < scalar_samples.size(); ++index) {
            const auto& sample = scalar_samples[index];
            if (sample.index != index || !std::isfinite(sample.time) ||
                !std::isfinite(sample.value) ||
                (index > 0U && !(scalar_samples[index - 1U].time < sample.time))) {
                return core::Result<void>::failure(detail::validation(
                    "sample set scalar samples must be ordered, indexed, and finite"));
            }
        }
        break;
    case ChannelValueKind::transform:
        if (transform_samples.size() != sample_count || !scalar_samples.empty()) {
            return core::Result<void>::failure(detail::validation(
                "sample set transform payload disagrees with its resolution"));
        }
        for (std::size_t index = 0U; index < transform_samples.size(); ++index) {
            const auto& sample = transform_samples[index];
            if (sample.index != index || !std::isfinite(sample.time) ||
                !sample.value.finite() ||
                (index > 0U && !(transform_samples[index - 1U].time < sample.time))) {
                return core::Result<void>::failure(detail::validation(
                    "sample set transform samples must be ordered, indexed, and finite"));
            }
        }
        break;
    }
    return core::Result<void>::success();
}

std::string SampleSet::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "SAMPLE_SET " << channel_id.value << ' ' << channel_digest.hex() << ' '
           << static_cast<unsigned>(value_kind) << ' ' << sample_count << ' '
           << scalar_samples.size() << ' ' << transform_samples.size() << '\n';
    for (const auto& sample : scalar_samples) {
        output << "SCALAR_SAMPLE " << sample.index << ' ' << sample.time << ' '
               << sample.value << '\n';
    }
    for (const auto& sample : transform_samples) {
        output << "TRANSFORM_SAMPLE " << sample.index << ' ' << sample.time << ' ';
        detail::write_transform(output, sample.value);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> SampleSet::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<SampleSet> SampleSet::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SampleSet>::failure(detail::parse_error(
            "sample set text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<SampleSet>::failure(detail::parse_error(
            "sample set framing is invalid"));
    }
    if (!detail::read_integer(input, version)) {
        return core::Result<SampleSet>::failure(detail::parse_error(
            "sample set schema version is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<SampleSet>::failure(detail::version_error(
            "sample set schema version is not supported"));
    }

    SampleSet set;
    std::string digest;
    unsigned value_kind = 0U;
    std::size_t scalar_count = 0U;
    std::size_t transform_count = 0U;
    if (!detail::require_token(input, "SAMPLE_SET") ||
        !detail::read_integer(input, set.channel_id.value) || !(input >> digest) ||
        !detail::read_integer(input, value_kind) ||
        !detail::read_integer(input, set.sample_count) ||
        !detail::read_count(input, kMaxSamplesPerSet, scalar_count) ||
        !detail::read_count(input, kMaxSamplesPerSet, transform_count)) {
        return core::Result<SampleSet>::failure(detail::parse_error(
            "sample set header is malformed"));
    }
    const auto parsed = assets::Sha256Digest::from_hex(digest);
    if (!parsed) {
        return core::Result<SampleSet>::failure(detail::parse_error(
            "sample set channel digest is malformed"));
    }
    set.channel_digest = parsed.value();
    set.value_kind = static_cast<ChannelValueKind>(value_kind);
    set.scalar_samples.reserve(scalar_count);
    set.transform_samples.reserve(transform_count);
    for (std::size_t index = 0U; index < scalar_count; ++index) {
        ScalarSample sample;
        if (!detail::require_token(input, "SCALAR_SAMPLE") ||
            !detail::read_integer(input, sample.index) ||
            !detail::read_double(input, sample.time) ||
            !detail::read_double(input, sample.value)) {
            return core::Result<SampleSet>::failure(detail::parse_error(
                "sample set scalar sample is malformed"));
        }
        set.scalar_samples.push_back(sample);
    }
    for (std::size_t index = 0U; index < transform_count; ++index) {
        TransformSample sample;
        if (!detail::require_token(input, "TRANSFORM_SAMPLE") ||
            !detail::read_integer(input, sample.index) ||
            !detail::read_double(input, sample.time) ||
            !detail::read_transform(input, sample.value)) {
            return core::Result<SampleSet>::failure(detail::parse_error(
                "sample set transform sample is malformed"));
        }
        set.transform_samples.push_back(std::move(sample));
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<SampleSet>::failure(detail::parse_error(
            "sample set is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SampleSet>::failure(detail::parse_error(
            "sample set contains trailing unbound data"));
    }
    if (auto result = set.validate(); !result) {
        return core::Result<SampleSet>::failure(result.error());
    }
    return core::Result<SampleSet>::success(std::move(set));
}

core::Result<SampleSet> sample_channel(
    const TemporalChannel& channel, const SampleGrid& grid) {
    if (auto result = channel.validate(); !result) {
        return core::Result<SampleSet>::failure(result.error());
    }
    if (auto result = grid.validate(); !result) {
        return core::Result<SampleSet>::failure(result.error());
    }
    const auto digest = channel.canonical_digest();
    if (!digest) {
        return core::Result<SampleSet>::failure(digest.error());
    }

    SampleSet set;
    set.channel_id = channel.id;
    set.channel_digest = digest.value();
    set.value_kind = channel.value_kind;
    set.sample_count = grid.sample_count;
    const double denominator = static_cast<double>(grid.sample_count - 1U);
    for (std::uint32_t index = 0U; index < grid.sample_count; ++index) {
        const double time =
            channel.duration * (static_cast<double>(index) / denominator);
        switch (channel.value_kind) {
        case ChannelValueKind::scalar:
            set.scalar_samples.push_back(
                ScalarSample{index, time, interpolate_scalar(channel, time)});
            break;
        case ChannelValueKind::transform:
            set.transform_samples.push_back(
                TransformSample{index, time, interpolate_transform(channel, time)});
            break;
        }
    }
    if (auto result = set.validate(); !result) {
        return core::Result<SampleSet>::failure(result.error());
    }
    return core::Result<SampleSet>::success(std::move(set));
}

} // namespace carto::temporal
