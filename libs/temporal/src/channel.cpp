#include <carto/temporal/channel.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <tuple>

namespace carto::temporal {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_TEMPORAL_CHANNEL";

bool valid_target_kind(TargetBindingKind kind) {
    switch (kind) {
    case TargetBindingKind::none:
    case TargetBindingKind::rigid_object:
    case TargetBindingKind::mechanical_subassembly:
    case TargetBindingKind::skinned_mesh:
    case TargetBindingKind::camera:
    case TargetBindingKind::light:
        return true;
    }
    return false;
}

bool valid_value_kind(ChannelValueKind kind) {
    switch (kind) {
    case ChannelValueKind::scalar:
    case ChannelValueKind::transform:
        return true;
    }
    return false;
}

// Canonical order is strictly increasing time with every value finite and
// every time inside the closed [0, duration] interval.
template <typename Keyframe, typename ValueFinite>
bool keyframes_canonical(
    const std::vector<Keyframe>& keyframes,
    double duration,
    ValueFinite value_finite) {
    for (std::size_t index = 0U; index < keyframes.size(); ++index) {
        const auto& key = keyframes[index];
        if (!std::isfinite(key.time) || key.time < 0.0 || key.time > duration ||
            !value_finite(key)) {
            return false;
        }
        if (index > 0U && !(keyframes[index - 1U].time < key.time)) return false;
    }
    return true;
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

} // namespace

std::string_view target_binding_kind_name(TargetBindingKind kind) noexcept {
    switch (kind) {
    case TargetBindingKind::none: return "none";
    case TargetBindingKind::rigid_object: return "rigid-object";
    case TargetBindingKind::mechanical_subassembly: return "mechanical-subassembly";
    case TargetBindingKind::skinned_mesh: return "skinned-mesh";
    case TargetBindingKind::camera: return "camera";
    case TargetBindingKind::light: return "light";
    }
    return "unknown";
}

std::string_view channel_value_kind_name(ChannelValueKind kind) noexcept {
    switch (kind) {
    case ChannelValueKind::scalar: return "scalar";
    case ChannelValueKind::transform: return "transform";
    }
    return "unknown";
}

core::Result<void> TargetBinding::validate() const {
    if (!valid_target_kind(kind)) {
        return core::Result<void>::failure(
            detail::invalid("temporal target binding kind is invalid"));
    }
    if (kind == TargetBindingKind::none) {
        if (!stable_id.empty()) {
            return core::Result<void>::failure(detail::validation(
                "temporal target binding kind 'none' cannot carry a stable identity"));
        }
        return core::Result<void>::success();
    }
    if (!detail::safe_text(stable_id, TemporalChannel::kMaxStableIdBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "temporal target binding stable identity is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> EventMarker::validate() const {
    if (!id || !std::isfinite(time) ||
        !detail::safe_text(name, TemporalChannel::kMaxEventNameBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "temporal event marker identity, time, or name is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> TemporalChannel::validate() const {
    if (!id || !detail::safe_text(channel_name, kMaxChannelNameBytes) ||
        revision.exhausted() || !std::isfinite(duration) || duration <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "temporal channel identity, name, revision, or duration is invalid"));
    }
    if (auto result = target.validate(); !result) return result;
    if (!valid_value_kind(value_kind)) {
        return core::Result<void>::failure(
            detail::invalid("temporal channel value kind is invalid"));
    }
    if (scalar_keyframes.size() > kMaxKeyframesPerChannel ||
        transform_keyframes.size() > kMaxKeyframesPerChannel) {
        return core::Result<void>::failure(detail::validation(
            "temporal channel exceeds its bounded keyframe capacity"));
    }
    if (scalar_keyframes.empty() && transform_keyframes.empty()) {
        return core::Result<void>::failure(detail::validation(
            "temporal channel must contain at least one keyframe"));
    }
    const auto scalar_finite = [](const ScalarKeyframe& key) {
        return std::isfinite(key.value);
    };
    const auto transform_finite = [](const TransformKeyframe& key) {
        return key.value.finite();
    };
    if (!keyframes_canonical(scalar_keyframes, duration, scalar_finite)) {
        return core::Result<void>::failure(detail::validation(
            "temporal channel scalar keyframes must be strictly ordered, finite, "
            "and inside the channel duration"));
    }
    if (!keyframes_canonical(transform_keyframes, duration, transform_finite)) {
        return core::Result<void>::failure(detail::validation(
            "temporal channel transform keyframes must be strictly ordered, finite, "
            "and inside the channel duration"));
    }
    switch (value_kind) {
    case ChannelValueKind::scalar:
        if (!transform_keyframes.empty()) {
            return core::Result<void>::failure(detail::validation(
                "temporal channel value kind disagrees with its keyframe payload"));
        }
        break;
    case ChannelValueKind::transform:
        if (!scalar_keyframes.empty()) {
            return core::Result<void>::failure(detail::validation(
                "temporal channel value kind disagrees with its keyframe payload"));
        }
        break;
    }
    if (event_markers.size() > kMaxEventMarkersPerChannel) {
        return core::Result<void>::failure(detail::validation(
            "temporal channel exceeds its bounded event marker capacity"));
    }
    for (const auto& marker : event_markers) {
        if (auto result = marker.validate(); !result) return result;
    }
    if (!markers_canonical(event_markers, duration)) {
        return core::Result<void>::failure(detail::validation(
            "temporal channel event markers must be ordered by time then identity, "
            "unique in identity, and inside the channel duration"));
    }
    return core::Result<void>::success();
}

std::string TemporalChannel::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "CHANNEL " << id.value << ' ' << revision.value() << ' '
           << std::quoted(channel_name) << ' ' << duration << ' '
           << static_cast<unsigned>(target.kind) << ' ' << std::quoted(target.stable_id)
           << ' ' << static_cast<unsigned>(value_kind) << ' '
           << scalar_keyframes.size() << ' ' << transform_keyframes.size() << ' '
           << event_markers.size() << '\n';
    for (const auto& key : scalar_keyframes) {
        output << "SCALAR_KEY " << key.time << ' ' << key.value << '\n';
    }
    for (const auto& key : transform_keyframes) {
        output << "TRANSFORM_KEY " << key.time << ' ';
        detail::write_transform(output, key.value);
        output << '\n';
    }
    for (const auto& marker : event_markers) {
        output << "EVENT " << marker.id.value << ' ' << marker.time << ' '
               << std::quoted(marker.name) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> TemporalChannel::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<TemporalChannel> TemporalChannel::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<TemporalChannel>::failure(detail::parse_error(
            "temporal channel text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<TemporalChannel>::failure(detail::parse_error(
            "temporal channel framing is invalid"));
    }
    if (!detail::read_integer(input, version)) {
        return core::Result<TemporalChannel>::failure(detail::parse_error(
            "temporal channel schema version is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<TemporalChannel>::failure(detail::version_error(
            "temporal channel schema version is not supported"));
    }

    TemporalChannel channel;
    std::uint64_t revision = 0U;
    std::size_t scalar_count = 0U;
    std::size_t transform_count = 0U;
    std::size_t marker_count = 0U;
    unsigned target_kind = 0U;
    unsigned value_kind = 0U;
    if (!detail::require_token(input, "CHANNEL") ||
        !detail::read_integer(input, channel.id.value) ||
        !detail::read_integer(input, revision) ||
        !(input >> std::quoted(channel.channel_name)) ||
        !detail::read_double(input, channel.duration) ||
        !detail::read_integer(input, target_kind) ||
        !(input >> std::quoted(channel.target.stable_id)) ||
        !detail::read_integer(input, value_kind) ||
        !detail::read_count(input, kMaxKeyframesPerChannel, scalar_count) ||
        !detail::read_count(input, kMaxKeyframesPerChannel, transform_count) ||
        !detail::read_count(input, kMaxEventMarkersPerChannel, marker_count)) {
        return core::Result<TemporalChannel>::failure(detail::parse_error(
            "temporal channel header is malformed"));
    }
    channel.revision = core::Revision{revision};
    channel.target.kind = static_cast<TargetBindingKind>(target_kind);
    channel.value_kind = static_cast<ChannelValueKind>(value_kind);
    channel.scalar_keyframes.reserve(scalar_count);
    channel.transform_keyframes.reserve(transform_count);
    channel.event_markers.reserve(marker_count);
    for (std::size_t index = 0U; index < scalar_count; ++index) {
        ScalarKeyframe key;
        if (!detail::require_token(input, "SCALAR_KEY") ||
            !detail::read_double(input, key.time) ||
            !detail::read_double(input, key.value)) {
            return core::Result<TemporalChannel>::failure(detail::parse_error(
                "temporal channel scalar keyframe is malformed"));
        }
        channel.scalar_keyframes.push_back(key);
    }
    for (std::size_t index = 0U; index < transform_count; ++index) {
        TransformKeyframe key;
        if (!detail::require_token(input, "TRANSFORM_KEY") ||
            !detail::read_double(input, key.time) ||
            !detail::read_transform(input, key.value)) {
            return core::Result<TemporalChannel>::failure(detail::parse_error(
                "temporal channel transform keyframe is malformed"));
        }
        channel.transform_keyframes.push_back(key);
    }
    for (std::size_t index = 0U; index < marker_count; ++index) {
        EventMarker marker;
        if (!detail::require_token(input, "EVENT") ||
            !detail::read_integer(input, marker.id.value) ||
            !detail::read_double(input, marker.time) ||
            !(input >> std::quoted(marker.name))) {
            return core::Result<TemporalChannel>::failure(detail::parse_error(
                "temporal channel event marker is malformed"));
        }
        channel.event_markers.push_back(std::move(marker));
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<TemporalChannel>::failure(detail::parse_error(
            "temporal channel is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<TemporalChannel>::failure(detail::parse_error(
            "temporal channel contains trailing unbound data"));
    }
    if (auto result = channel.validate(); !result) {
        return core::Result<TemporalChannel>::failure(result.error());
    }
    return core::Result<TemporalChannel>::success(std::move(channel));
}

} // namespace carto::temporal
