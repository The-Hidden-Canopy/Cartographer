#pragma once

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace carto::condition {

inline constexpr std::size_t kMaxWaveClipBytes = 64U * 1024U * 1024U;
inline constexpr double kMaxWaveAuditionSeconds = 3.0;

enum class WaveSampleEncoding : std::uint8_t {
    pcm_integer,
    ieee_float,
};

struct WaveClipInspection {
    WaveSampleEncoding encoding = WaveSampleEncoding::pcm_integer;
    std::uint16_t channels = 0U;
    std::uint16_t bits_per_sample = 0U;
    std::uint32_t sample_rate = 0U;
    std::uint64_t frame_count = 0U;
    std::uint64_t audio_bytes = 0U;

    [[nodiscard]] double duration_seconds() const noexcept;
    [[nodiscard]] std::string_view encoding_name() const noexcept;
};

// Inspects only bounded, uncompressed RIFF/WAVE PCM or IEEE-float input. It
// does not retain samples, decode to a playback buffer, or create runtime state.
[[nodiscard]] core::Result<WaveClipInspection> inspect_wave_clip(
    std::span<const std::uint8_t> bytes);

// Decodes a bounded excerpt into canonical 16-bit PCM for transient audition.
// The caller owns the returned memory and must not persist it as project data.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> make_wave_audition_excerpt(
    std::span<const std::uint8_t> bytes,
    double maximum_duration_seconds = kMaxWaveAuditionSeconds);

} // namespace carto::condition
