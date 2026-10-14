#include <carto/condition/wave_import.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace carto::condition {

namespace {

constexpr std::size_t kMaxWaveChunks = 4096U;
constexpr std::uint32_t kMinimumSampleRate = 8000U;
constexpr std::uint32_t kMaximumSampleRate = 192000U;
constexpr std::uint64_t kMaximumDurationSeconds = 30U * 60U;
constexpr std::size_t kMaximumAuditionBytes = 16U * 1024U * 1024U;

core::Diagnostic malformed(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool matches(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::string_view value) noexcept {
    if (offset > bytes.size() || value.size() > bytes.size() - offset) return false;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(value[index])) return false;
    }
    return true;
}

std::optional<std::uint16_t> read_u16(
    std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 2U) return std::nullopt;
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::optional<std::uint32_t> read_u32(
    std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return std::nullopt;
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::optional<std::uint64_t> read_u64(
    std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 8U) return std::nullopt;
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

double decode_sample(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    const WaveClipInspection& format) {
    if (format.encoding == WaveSampleEncoding::ieee_float) {
        if (format.bits_per_sample == 32U) {
            const auto bits = read_u32(bytes, offset);
            return bits ? std::clamp(static_cast<double>(std::bit_cast<float>(*bits)), -1.0, 1.0)
                        : 0.0;
        }
        const auto bits = read_u64(bytes, offset);
        return bits ? std::clamp(std::bit_cast<double>(*bits), -1.0, 1.0) : 0.0;
    }
    switch (format.bits_per_sample) {
    case 8U:
        return (static_cast<double>(bytes[offset]) - 128.0) / 128.0;
    case 16U: {
        const auto bits = read_u16(bytes, offset);
        return bits ? static_cast<double>(std::bit_cast<std::int16_t>(*bits)) / 32768.0 : 0.0;
    }
    case 24U: {
        const std::uint32_t raw = static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
            (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U);
        const std::int32_t signed_value = (raw & 0x800000U) != 0U
            ? static_cast<std::int32_t>(raw | 0xff000000U)
            : static_cast<std::int32_t>(raw);
        return static_cast<double>(signed_value) / 8388608.0;
    }
    case 32U: {
        const auto bits = read_u32(bytes, offset);
        return bits ? static_cast<double>(std::bit_cast<std::int32_t>(*bits)) /
            2147483648.0 : 0.0;
    }
    default:
        return 0.0;
    }
}

void append_pcm16(std::vector<std::uint8_t>& output, double sample) {
    const auto bounded = std::clamp(sample, -1.0, 1.0);
    const auto integer = static_cast<std::int16_t>(std::lround(bounded * 32767.0));
    append_u16(output, std::bit_cast<std::uint16_t>(integer));
}

} // namespace

double WaveClipInspection::duration_seconds() const noexcept {
    return sample_rate == 0U
        ? 0.0
        : static_cast<double>(frame_count) / static_cast<double>(sample_rate);
}

std::string_view WaveClipInspection::encoding_name() const noexcept {
    switch (encoding) {
    case WaveSampleEncoding::pcm_integer: return "PCM";
    case WaveSampleEncoding::ieee_float: return "IEEE float";
    }
    return "unknown";
}

core::Result<WaveClipInspection> inspect_wave_clip(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12U || bytes.size() > kMaxWaveClipBytes) {
        return core::Result<WaveClipInspection>::failure(malformed(
            "WAVE input size is outside the supported range"));
    }
    if (!matches(bytes, 0U, "RIFF") || !matches(bytes, 8U, "WAVE")) {
        return core::Result<WaveClipInspection>::failure(unsupported(
            "only little-endian RIFF/WAVE input is supported"));
    }
    const auto riff_size = read_u32(bytes, 4U);
    if (!riff_size || static_cast<std::uint64_t>(*riff_size) + 8U != bytes.size()) {
        return core::Result<WaveClipInspection>::failure(malformed(
            "RIFF size does not match the complete input"));
    }

    std::optional<WaveClipInspection> format;
    std::optional<std::pair<std::size_t, std::size_t>> audio_data;
    std::size_t offset = 12U;
    std::size_t chunk_count = 0U;
    while (offset < bytes.size()) {
        if (++chunk_count > kMaxWaveChunks || bytes.size() - offset < 8U) {
            return core::Result<WaveClipInspection>::failure(malformed(
                "WAVE chunk table is truncated or exceeds its bound"));
        }
        const auto chunk_size_value = read_u32(bytes, offset + 4U);
        if (!chunk_size_value) return core::Result<WaveClipInspection>::failure(malformed(
            "WAVE chunk size is truncated"));
        const std::size_t chunk_size = static_cast<std::size_t>(*chunk_size_value);
        const std::size_t payload = offset + 8U;
        if (chunk_size > bytes.size() - payload) {
            return core::Result<WaveClipInspection>::failure(malformed(
                "WAVE chunk extends beyond the input"));
        }
        const std::size_t padded_size = chunk_size + (chunk_size & 1U);
        if (padded_size > bytes.size() - payload) {
            return core::Result<WaveClipInspection>::failure(malformed(
                "WAVE odd-sized chunk is missing its pad byte"));
        }

        if (matches(bytes, offset, "fmt ")) {
            if (format.has_value() || (chunk_size != 16U && chunk_size != 18U)) {
                return core::Result<WaveClipInspection>::failure(malformed(
                    "WAVE must contain one bounded canonical format chunk"));
            }
            const auto tag = read_u16(bytes, payload);
            const auto channels = read_u16(bytes, payload + 2U);
            const auto sample_rate = read_u32(bytes, payload + 4U);
            const auto byte_rate = read_u32(bytes, payload + 8U);
            const auto block_align = read_u16(bytes, payload + 12U);
            const auto bits = read_u16(bytes, payload + 14U);
            if (!tag || !channels || !sample_rate || !byte_rate || !block_align || !bits) {
                return core::Result<WaveClipInspection>::failure(malformed(
                    "WAVE format chunk is incomplete"));
            }
            if (*tag != 1U && *tag != 3U) {
                return core::Result<WaveClipInspection>::failure(unsupported(
                    "compressed or extensible WAVE formats are not supported"));
            }
            if (chunk_size == 18U) {
                const auto extension_size = read_u16(bytes, payload + 16U);
                if (!extension_size || *extension_size != 0U) {
                    return core::Result<WaveClipInspection>::failure(unsupported(
                        "WAVE format extensions are not supported"));
                }
            }
            const bool pcm_depth = *bits == 8U || *bits == 16U ||
                *bits == 24U || *bits == 32U;
            const bool float_depth = *bits == 32U || *bits == 64U;
            if (*channels == 0U || *channels > 8U ||
                *sample_rate < kMinimumSampleRate || *sample_rate > kMaximumSampleRate ||
                (*tag == 1U && !pcm_depth) || (*tag == 3U && !float_depth)) {
                return core::Result<WaveClipInspection>::failure(unsupported(
                    "WAVE channel count, sample rate, or sample encoding is unsupported"));
            }
            const std::uint64_t expected_align =
                static_cast<std::uint64_t>(*channels) * (*bits / 8U);
            const std::uint64_t expected_rate =
                static_cast<std::uint64_t>(*sample_rate) * expected_align;
            if (expected_align == 0U || expected_align > std::numeric_limits<std::uint16_t>::max() ||
                expected_rate > std::numeric_limits<std::uint32_t>::max() ||
                *block_align != expected_align || *byte_rate != expected_rate) {
                return core::Result<WaveClipInspection>::failure(malformed(
                    "WAVE byte-rate or block-alignment fields are inconsistent"));
            }
            format = WaveClipInspection{
                *tag == 1U ? WaveSampleEncoding::pcm_integer : WaveSampleEncoding::ieee_float,
                *channels, *bits, *sample_rate, 0U, 0U};
        } else if (matches(bytes, offset, "data")) {
            if (audio_data.has_value() || chunk_size == 0U) {
                return core::Result<WaveClipInspection>::failure(malformed(
                    "WAVE must contain one non-empty data chunk"));
            }
            audio_data = std::pair{payload, chunk_size};
        }
        offset = payload + padded_size;
    }
    if (offset != bytes.size() || !format || !audio_data) {
        return core::Result<WaveClipInspection>::failure(malformed(
            "WAVE is missing a format or audio-data chunk"));
    }

    const std::uint64_t block_align = static_cast<std::uint64_t>(format->channels) *
        (format->bits_per_sample / 8U);
    const std::uint64_t audio_bytes = audio_data->second;
    if (audio_bytes > kMaxWaveClipBytes || block_align == 0U || audio_bytes % block_align != 0U) {
        return core::Result<WaveClipInspection>::failure(malformed(
            "WAVE audio data is empty, oversized, or not frame aligned"));
    }
    format->frame_count = audio_bytes / block_align;
    format->audio_bytes = audio_bytes;
    if (format->frame_count == 0U ||
        format->frame_count / format->sample_rate > kMaximumDurationSeconds ||
        (format->frame_count / format->sample_rate == kMaximumDurationSeconds &&
            format->frame_count % format->sample_rate != 0U)) {
        return core::Result<WaveClipInspection>::failure(unsupported(
            "WAVE clip duration exceeds the 30-minute authoring limit"));
    }

    if (format->encoding == WaveSampleEncoding::ieee_float) {
        const std::size_t width = format->bits_per_sample / 8U;
        for (std::size_t sample_offset = audio_data->first;
             sample_offset < audio_data->first + audio_data->second;
             sample_offset += width) {
            bool finite = false;
            if (width == 4U) {
                const auto sample_bits = read_u32(bytes, sample_offset);
                if (!sample_bits) return core::Result<WaveClipInspection>::failure(malformed(
                    "WAVE floating-point sample is truncated"));
                finite = std::isfinite(std::bit_cast<float>(*sample_bits));
            } else {
                const auto sample_bits = read_u64(bytes, sample_offset);
                if (!sample_bits) return core::Result<WaveClipInspection>::failure(malformed(
                    "WAVE floating-point sample is truncated"));
                finite = std::isfinite(std::bit_cast<double>(*sample_bits));
            }
            if (!finite) return core::Result<WaveClipInspection>::failure(malformed(
                "WAVE floating-point samples contain a non-finite value"));
        }
    }
    return core::Result<WaveClipInspection>::success(*format);
}

core::Result<std::vector<std::uint8_t>> make_wave_audition_excerpt(
    std::span<const std::uint8_t> bytes,
    double maximum_duration_seconds) {
    const auto inspected = inspect_wave_clip(bytes);
    if (!inspected) return core::Result<std::vector<std::uint8_t>>::failure(
        inspected.error().with_context("WAVE audition input"));
    if (!std::isfinite(maximum_duration_seconds) || maximum_duration_seconds <= 0.0 ||
        maximum_duration_seconds > 10.0) {
        return core::Result<std::vector<std::uint8_t>>::failure(malformed(
            "WAVE audition duration must be finite and within ten seconds"));
    }

    std::optional<std::pair<std::size_t, std::size_t>> audio_data;
    std::size_t offset = 12U;
    while (offset < bytes.size()) {
        const auto size = read_u32(bytes, offset + 4U);
        if (!size) return core::Result<std::vector<std::uint8_t>>::failure(malformed(
            "WAVE audition chunk table is truncated"));
        const std::size_t payload = offset + 8U;
        if (matches(bytes, offset, "data")) {
            audio_data = std::pair{payload, static_cast<std::size_t>(*size)};
            break;
        }
        offset = payload + static_cast<std::size_t>(*size) + (*size & 1U);
    }
    if (!audio_data.has_value()) {
        return core::Result<std::vector<std::uint8_t>>::failure(malformed(
            "WAVE audition requires its validated data chunk"));
    }

    const auto& format = inspected.value();
    const std::uint64_t requested_frames = static_cast<std::uint64_t>(std::floor(
        maximum_duration_seconds * static_cast<double>(format.sample_rate)));
    const std::uint64_t frames = std::min(format.frame_count,
        std::max<std::uint64_t>(1U, requested_frames));
    const std::uint16_t output_channels = format.channels == 2U ? 2U : 1U;
    const std::uint64_t data_bytes = frames * output_channels * sizeof(std::int16_t);
    if (data_bytes + 36U > std::numeric_limits<std::uint32_t>::max() ||
        data_bytes + 44U > kMaximumAuditionBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(unsupported(
            "decoded WAVE audition excerpt exceeds its transient memory limit"));
    }
    const std::uint16_t output_block_align = static_cast<std::uint16_t>(
        output_channels * sizeof(std::int16_t));
    const std::uint64_t output_byte_rate =
        static_cast<std::uint64_t>(format.sample_rate) * output_block_align;
    if (output_byte_rate > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::vector<std::uint8_t>>::failure(unsupported(
            "decoded WAVE audition byte rate exceeds the canonical PCM limit"));
    }

    std::vector<std::uint8_t> output;
    output.reserve(static_cast<std::size_t>(data_bytes + 44U));
    output.insert(output.end(), {'R','I','F','F'});
    append_u32(output, static_cast<std::uint32_t>(data_bytes + 36U));
    output.insert(output.end(), {'W','A','V','E','f','m','t',' '});
    append_u32(output, 16U);
    append_u16(output, 1U);
    append_u16(output, output_channels);
    append_u32(output, format.sample_rate);
    append_u32(output, static_cast<std::uint32_t>(output_byte_rate));
    append_u16(output, output_block_align);
    append_u16(output, 16U);
    output.insert(output.end(), {'d','a','t','a'});
    append_u32(output, static_cast<std::uint32_t>(data_bytes));

    const std::size_t input_width = format.bits_per_sample / 8U;
    const std::size_t input_block_align =
        static_cast<std::size_t>(format.channels) * input_width;
    for (std::uint64_t frame = 0U; frame < frames; ++frame) {
        const std::size_t frame_offset = audio_data->first +
            static_cast<std::size_t>(frame) * input_block_align;
        if (output_channels == 2U) {
            append_pcm16(output, decode_sample(bytes, frame_offset, format));
            append_pcm16(output, decode_sample(bytes, frame_offset + input_width, format));
        } else {
            double mixed = 0.0;
            for (std::uint16_t channel = 0U; channel < format.channels; ++channel) {
                mixed += decode_sample(bytes,
                    frame_offset + static_cast<std::size_t>(channel) * input_width, format);
            }
            append_pcm16(output, mixed / static_cast<double>(format.channels));
        }
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(output));
}

} // namespace carto::condition
