#pragma once

#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace carto::temporal {

// Fixed sample grid: sample_count samples evenly spaced over the channel
// duration, both endpoints included. The grid is resolution only; it carries
// no runtime policy.
struct SampleGrid {
    static constexpr std::uint32_t kMaxSamplesPerGrid = 65'536U;

    std::uint32_t sample_count = 2U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ScalarSample {
    std::uint32_t index = 0U;
    double time = 0.0;
    double value = 0.0;
};

struct TransformSample {
    std::uint32_t index = 0U;
    double time = 0.0;
    core::Transform value = core::Transform::identity();
};

// Deterministic CPU reference sample set. The receipt digest binds channel
// identity, channel content, grid resolution, and every sample value stored
// in ascending index order; identical channel and grid inputs produce
// identical digests on every platform. Native backend sampling evidence must
// reconcile against this reference, never replace it silently.
class SampleSet final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::uint32_t kMaxSamplesPerSet = SampleGrid::kMaxSamplesPerGrid;

    ChannelId channel_id;
    assets::Sha256Digest channel_digest;
    ChannelValueKind value_kind = ChannelValueKind::scalar;
    std::uint32_t sample_count = 0U;
    std::vector<ScalarSample> scalar_samples;
    std::vector<TransformSample> transform_samples;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<SampleSet> deserialize(std::string_view text);
};

// Reference sampling: scalar channels linearly interpolate; transforms
// interpolate translation and scale linearly and rotation by normalized lerp
// on the shortest path. Values clamp to the outer keyframes.
[[nodiscard]] core::Result<SampleSet> sample_channel(
    const TemporalChannel& channel, const SampleGrid& grid);

} // namespace carto::temporal
