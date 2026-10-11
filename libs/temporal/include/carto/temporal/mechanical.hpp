#pragma once

#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::temporal {

// Descriptor-level rigid consumer: a named mechanism that temporal channels
// may drive. Skinned targets are rejected at this layer so mechanical
// animation never synthesizes a fake skinning path; skin-targeted channels
// are refused before any runtime representation exists.
class RigidConsumerDescriptor final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxTargetsPerConsumer = 1'024U;
    static constexpr std::size_t kMaxConsumerNameBytes = 256U;

    std::string consumer_name;
    std::vector<TargetBinding> targets;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<RigidConsumerDescriptor> deserialize(
        std::string_view text);
};

// Descriptor-level gate: a channel may drive rigid-only consumers only when
// its target is not skin-targeted.
[[nodiscard]] core::Result<void> validate_rigid_channel(
    const TemporalChannel& channel);

// Validates that a channel may drive a rigid consumer and that the consumer
// descriptor actually offers the channel's target binding.
[[nodiscard]] core::Result<void> bind_channel_to_rigid_consumer(
    const TemporalChannel& channel, const RigidConsumerDescriptor& consumer);

} // namespace carto::temporal
