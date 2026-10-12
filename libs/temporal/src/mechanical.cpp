#include <carto/temporal/mechanical.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace carto::temporal {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_RIGID_CONSUMER";

} // namespace

core::Result<void> RigidConsumerDescriptor::validate() const {
    if (!detail::safe_text(consumer_name, kMaxConsumerNameBytes) ||
        targets.empty() || targets.size() > kMaxTargetsPerConsumer) {
        return core::Result<void>::failure(detail::invalid(
            "rigid consumer name or target set is invalid"));
    }
    for (const auto& target : targets) {
        if (auto result = target.validate(); !result) return result;
        if (target.kind == TargetBindingKind::none) {
            return core::Result<void>::failure(detail::invalid(
                "rigid consumer targets must be bound to a stable identity"));
        }
        if (target.kind == TargetBindingKind::skinned_mesh) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "rigid consumer rejects skinned targets; mechanical animation "
                "never synthesizes a skinning path"));
        }
    }
    const auto order = [](const TargetBinding& left, const TargetBinding& right) {
        return left.stable_id < right.stable_id;
    };
    if (!std::is_sorted(targets.begin(), targets.end(), order) ||
        std::adjacent_find(targets.begin(), targets.end(),
            [](const TargetBinding& left, const TargetBinding& right) {
                return left.stable_id == right.stable_id;
            }) != targets.end()) {
        return core::Result<void>::failure(detail::validation(
            "rigid consumer targets must be ordered and unique in stable identity"));
    }
    return core::Result<void>::success();
}

std::string RigidConsumerDescriptor::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "CONSUMER " << std::quoted(consumer_name) << ' ' << targets.size()
           << '\n';
    for (const auto& target : targets) {
        output << "TARGET " << static_cast<unsigned>(target.kind) << ' '
               << std::quoted(target.stable_id) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> RigidConsumerDescriptor::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<RigidConsumerDescriptor> RigidConsumerDescriptor::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RigidConsumerDescriptor>::failure(detail::parse_error(
            "rigid consumer text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<RigidConsumerDescriptor>::failure(detail::parse_error(
            "rigid consumer framing is invalid"));
    }
    if (!detail::read_integer(input, version)) {
        return core::Result<RigidConsumerDescriptor>::failure(detail::parse_error(
            "rigid consumer schema version is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RigidConsumerDescriptor>::failure(detail::version_error(
            "rigid consumer schema version is not supported"));
    }

    RigidConsumerDescriptor consumer;
    std::size_t target_count = 0U;
    if (!detail::require_token(input, "CONSUMER") ||
        !(input >> std::quoted(consumer.consumer_name)) ||
        !detail::read_count(input, kMaxTargetsPerConsumer, target_count)) {
        return core::Result<RigidConsumerDescriptor>::failure(detail::parse_error(
            "rigid consumer header is malformed"));
    }
    consumer.targets.reserve(target_count);
    for (std::size_t index = 0U; index < target_count; ++index) {
        TargetBinding target;
        unsigned kind = 0U;
        if (!detail::require_token(input, "TARGET") ||
            !detail::read_integer(input, kind) ||
            !(input >> std::quoted(target.stable_id))) {
            return core::Result<RigidConsumerDescriptor>::failure(detail::parse_error(
                "rigid consumer target is malformed"));
        }
        target.kind = static_cast<TargetBindingKind>(kind);
        consumer.targets.push_back(std::move(target));
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<RigidConsumerDescriptor>::failure(detail::parse_error(
            "rigid consumer is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RigidConsumerDescriptor>::failure(detail::parse_error(
            "rigid consumer contains trailing unbound data"));
    }
    if (auto result = consumer.validate(); !result) {
        return core::Result<RigidConsumerDescriptor>::failure(result.error());
    }
    return core::Result<RigidConsumerDescriptor>::success(std::move(consumer));
}

core::Result<void> validate_rigid_channel(const TemporalChannel& channel) {
    if (auto result = channel.validate(); !result) return result;
    if (channel.target.kind == TargetBindingKind::skinned_mesh) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "skin-targeted channels are rejected on rigid-only consumers "
            "at the descriptor level"));
    }
    if (channel.target.kind == TargetBindingKind::none) {
        return core::Result<void>::failure(detail::validation(
            "channel is not bound to a target and cannot drive any consumer"));
    }
    return core::Result<void>::success();
}

core::Result<void> bind_channel_to_rigid_consumer(
    const TemporalChannel& channel, const RigidConsumerDescriptor& consumer) {
    if (auto result = validate_rigid_channel(channel); !result) return result;
    if (auto result = consumer.validate(); !result) return result;
    const auto offered = std::find_if(
        consumer.targets.begin(), consumer.targets.end(),
        [&channel](const TargetBinding& target) {
            return target.kind == channel.target.kind &&
                target.stable_id == channel.target.stable_id;
        });
    if (offered == consumer.targets.end()) {
        return core::Result<void>::failure(detail::validation(
            "channel target is not offered by the rigid consumer descriptor"));
    }
    return core::Result<void>::success();
}

} // namespace carto::temporal
