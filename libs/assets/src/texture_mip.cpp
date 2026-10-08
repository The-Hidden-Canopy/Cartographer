#include <carto/assets/texture_mip.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace carto::assets {
namespace {

constexpr std::array<std::uint8_t, 12U> kMipMagic{
    'C', 'A', 'R', 'T', 'O', '_', 'M', 'I', 'P', 'S', 0U, 0U};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

bool read_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::uint32_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(value)) return false;
    value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[cursor++]) << shift;
    }
    return true;
}

bool read_u64(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::uint64_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(value)) return false;
    value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[cursor++]) << shift;
    }
    return true;
}

bool safe_text(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x21U && byte <= 0x7eU;
        });
}

double srgb_to_linear(double value) {
    return value <= 0.04045
        ? value / 12.92
        : std::pow((value + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.0031308
        ? value * 12.92
        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

std::uint8_t quantize(double value) {
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(value * 255.0), 0L, 255L));
}

std::size_t alpha_coverage(
    const Rgba8MipLevel& level,
    std::uint8_t cutoff) {
    std::size_t result = 0U;
    for (std::size_t offset = 3U; offset < level.pixels.size(); offset += 4U) {
        if (level.pixels[offset] >= cutoff) ++result;
    }
    return result;
}

void preserve_coverage(
    Rgba8MipLevel& level,
    std::size_t source_covered,
    std::size_t source_pixels,
    std::uint8_t cutoff) {
    const std::size_t destination_pixels =
        static_cast<std::size_t>(level.width) * level.height;
    std::size_t desired = static_cast<std::size_t>(std::llround(
        static_cast<long double>(source_covered) * destination_pixels /
        static_cast<long double>(source_pixels)));
    if (source_covered > 0U && desired == 0U) desired = 1U;
    if (source_covered < source_pixels && desired == destination_pixels &&
        destination_pixels > 1U) {
        --desired;
    }
    desired = std::min(desired, destination_pixels);
    if (alpha_coverage(level, cutoff) == desired) return;

    std::vector<std::size_t> order(destination_pixels);
    std::iota(order.begin(), order.end(), 0U);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return level.pixels[left * 4U + 3U] > level.pixels[right * 4U + 3U];
    });
    for (std::size_t rank = 0U; rank < order.size(); ++rank) {
        std::uint8_t& alpha = level.pixels[order[rank] * 4U + 3U];
        if (rank < desired && alpha < cutoff) alpha = cutoff;
        if (rank >= desired && alpha >= cutoff) {
            alpha = cutoff == 0U ? 0U : static_cast<std::uint8_t>(cutoff - 1U);
        }
    }
}

Rgba8MipLevel downsample(
    const Rgba8MipLevel& source,
    TextureSemantic semantic,
    ColorSpace color_space) {
    Rgba8MipLevel output;
    output.width = std::max(1U, source.width / 2U);
    output.height = std::max(1U, source.height / 2U);
    output.pixels.resize(
        static_cast<std::size_t>(output.width) * output.height * 4U);

    for (std::uint32_t y = 0U; y < output.height; ++y) {
        for (std::uint32_t x = 0U; x < output.width; ++x) {
            std::array<double, 4U> sum{};
            std::uint32_t samples = 0U;
            const std::uint32_t source_y_begin = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(y) * source.height / output.height);
            const std::uint32_t source_y_end = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(y + 1U) * source.height / output.height);
            const std::uint32_t source_x_begin = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(x) * source.width / output.width);
            const std::uint32_t source_x_end = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(x + 1U) * source.width / output.width);
            for (std::uint32_t source_y = source_y_begin;
                 source_y < source_y_end;
                 ++source_y) {
                for (std::uint32_t source_x = source_x_begin;
                     source_x < source_x_end;
                     ++source_x) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(source_y) * source.width + source_x) * 4U;
                    if (semantic == TextureSemantic::normal) {
                        sum[0U] += source.pixels[offset] / 127.5 - 1.0;
                        sum[1U] += source.pixels[offset + 1U] / 127.5 - 1.0;
                        sum[2U] += source.pixels[offset + 2U] / 127.5 - 1.0;
                    } else {
                        for (std::size_t channel = 0U; channel < 3U; ++channel) {
                            const double encoded = source.pixels[offset + channel] / 255.0;
                            sum[channel] += color_space == ColorSpace::srgb
                                ? srgb_to_linear(encoded)
                                : encoded;
                        }
                    }
                    sum[3U] += source.pixels[offset + 3U] / 255.0;
                    ++samples;
                }
            }
            const std::size_t destination =
                (static_cast<std::size_t>(y) * output.width + x) * 4U;
            if (semantic == TextureSemantic::normal) {
                const double length = std::hypot(std::hypot(sum[0U], sum[1U]), sum[2U]);
                if (std::isfinite(length) && length > 1e-12) {
                    output.pixels[destination] = quantize(sum[0U] / length * 0.5 + 0.5);
                    output.pixels[destination + 1U] = quantize(sum[1U] / length * 0.5 + 0.5);
                    output.pixels[destination + 2U] = quantize(sum[2U] / length * 0.5 + 0.5);
                } else {
                    output.pixels[destination] = 128U;
                    output.pixels[destination + 1U] = 128U;
                    output.pixels[destination + 2U] = 255U;
                }
            } else {
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    const double averaged = sum[channel] / samples;
                    output.pixels[destination + channel] = quantize(
                        color_space == ColorSpace::srgb
                            ? linear_to_srgb(averaged)
                            : averaged);
                }
            }
            output.pixels[destination + 3U] = quantize(sum[3U] / samples);
        }
    }
    return output;
}

core::Result<void> validate_structure(
    const Rgba8MipChain& chain,
    const TextureSourceLimits& limits) {
    if (auto result = limits.validate(); !result) return result;
    if (chain.artifact_version != kRgba8MipArtifactVersion ||
        chain.algorithm != kRgba8MipAlgorithm ||
        !safe_text(chain.algorithm, 64U) ||
        chain.source.bytes == 0U || chain.source.bytes > limits.max_source_bytes ||
        chain.source.digest.is_zero() || !safe_text(chain.source.media_type, 127U) ||
        chain.levels.empty() || chain.levels.size() > limits.max_mip_levels ||
        chain.color_space != expected_color_space(chain.semantic)) {
        return core::Result<void>::failure(validation(
            "RGBA8 mip chain source, algorithm, semantic, or level identity is invalid"));
    }
    switch (chain.semantic) {
    case TextureSemantic::base_color:
    case TextureSemantic::normal:
    case TextureSemantic::metallic_roughness:
    case TextureSemantic::occlusion:
    case TextureSemantic::emissive:
    case TextureSemantic::height:
    case TextureSemantic::mask:
    case TextureSemantic::hdri:
    case TextureSemantic::data:
        break;
    default:
        return core::Result<void>::failure(validation(
            "RGBA8 mip chain contains an unknown semantic"));
    }
    std::uint32_t expected_width = chain.levels.front().width;
    std::uint32_t expected_height = chain.levels.front().height;
    if (expected_width == 0U || expected_height == 0U ||
        expected_width > limits.max_dimension || expected_height > limits.max_dimension ||
        static_cast<std::uint64_t>(expected_width) * expected_height > limits.max_pixels) {
        return core::Result<void>::failure(validation(
            "RGBA8 mip chain base extent exceeds its bounds"));
    }
    std::uint64_t resident_bytes = 0U;
    for (std::size_t index = 0U; index < chain.levels.size(); ++index) {
        const Rgba8MipLevel& level = chain.levels[index];
        const std::uint64_t expected_bytes =
            static_cast<std::uint64_t>(expected_width) * expected_height * 4U;
        if (expected_bytes > kMaxRgba8MipArtifactBytes ||
            level.width != expected_width || level.height != expected_height ||
            level.pixels.size() != expected_bytes ||
            resident_bytes > kMaxRgba8MipArtifactBytes - expected_bytes) {
            return core::Result<void>::failure(validation(
                "RGBA8 mip level dimensions or byte count are non-canonical"));
        }
        resident_bytes += expected_bytes;
        if (index + 1U < chain.levels.size()) {
            if (expected_width == 1U && expected_height == 1U) {
                return core::Result<void>::failure(validation(
                    "RGBA8 mip chain contains levels after the terminal texel"));
            }
            expected_width = std::max(1U, expected_width / 2U);
            expected_height = std::max(1U, expected_height / 2U);
        }
    }
    if (chain.levels.back().width != 1U || chain.levels.back().height != 1U) {
        return core::Result<void>::failure(validation(
            "RGBA8 mip chain must terminate at one texel"));
    }
    return core::Result<void>::success();
}

std::vector<std::uint8_t> canonical_payload(const Rgba8MipChain& chain) {
    std::vector<std::uint8_t> output;
    output.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(chain.resident_bytes() + 256U,
                                std::numeric_limits<std::size_t>::max())));
    output.insert(output.end(), kMipMagic.begin(), kMipMagic.end());
    append_u32(output, chain.artifact_version);
    append_u32(output, static_cast<std::uint32_t>(chain.algorithm.size()));
    output.insert(output.end(), chain.algorithm.begin(), chain.algorithm.end());
    output.insert(
        output.end(), chain.source.digest.bytes.begin(), chain.source.digest.bytes.end());
    append_u64(output, chain.source.bytes);
    append_u32(output, static_cast<std::uint32_t>(chain.source.media_type.size()));
    output.insert(
        output.end(), chain.source.media_type.begin(), chain.source.media_type.end());
    append_u32(output, static_cast<std::uint32_t>(chain.semantic));
    append_u32(output, static_cast<std::uint32_t>(chain.color_space));
    append_u32(output, static_cast<std::uint32_t>(chain.levels.size()));
    for (const Rgba8MipLevel& level : chain.levels) {
        append_u32(output, level.width);
        append_u32(output, level.height);
        append_u64(output, static_cast<std::uint64_t>(level.pixels.size()));
        output.insert(output.end(), level.pixels.begin(), level.pixels.end());
    }
    return output;
}

} // namespace

core::Result<void> Rgba8MipChain::validate(
    const TextureSourceLimits& limits) const {
    if (auto result = validate_structure(*this, limits); !result) return result;
    if (artifact_digest.is_zero() || artifact_digest != sha256(canonical_payload(*this))) {
        return core::Result<void>::failure(validation(
            "RGBA8 mip chain artifact digest does not match its canonical payload"));
    }
    return core::Result<void>::success();
}

core::Result<void> Rgba8MipChain::refresh_digest(
    const TextureSourceLimits& limits) {
    if (auto result = validate_structure(*this, limits); !result) return result;
    artifact_digest = sha256(canonical_payload(*this));
    return core::Result<void>::success();
}

std::uint64_t Rgba8MipChain::resident_bytes() const noexcept {
    std::uint64_t result = 0U;
    for (const Rgba8MipLevel& level : levels) {
        result += level.pixels.size();
    }
    return result;
}

core::Result<Rgba8MipChain> generate_rgba8_mips(
    const DecodedRgba8Texture& decoded,
    const Rgba8MipOptions& options,
    const TextureSourceLimits& limits) {
    if (auto result = decoded.validate(limits); !result) {
        return core::Result<Rgba8MipChain>::failure(result.error());
    }
    if (options.color_space != expected_color_space(options.semantic)) {
        return core::Result<Rgba8MipChain>::failure(validation(
            "mip-generation color space does not match the texture semantic"));
    }
    if (options.preserve_alpha_coverage && options.alpha_cutoff == 0U) {
        return core::Result<Rgba8MipChain>::failure(invalid(
            "alpha-coverage preservation requires a non-zero cutoff"));
    }

    Rgba8MipChain result;
    result.source = decoded.admission.source;
    result.semantic = options.semantic;
    result.color_space = options.color_space;
    result.levels.push_back({
        decoded.admission.width,
        decoded.admission.height,
        decoded.pixels,
    });
    const std::size_t source_pixels =
        static_cast<std::size_t>(decoded.admission.width) * decoded.admission.height;
    const std::size_t source_covered = options.preserve_alpha_coverage
        ? alpha_coverage(result.levels.front(), options.alpha_cutoff)
        : 0U;
    while (result.levels.back().width != 1U || result.levels.back().height != 1U) {
        if (result.levels.size() >= limits.max_mip_levels) {
            return core::Result<Rgba8MipChain>::failure(validation(
                "complete RGBA8 mip chain exceeds the configured level bound"));
        }
        Rgba8MipLevel level = downsample(
            result.levels.back(), options.semantic, options.color_space);
        if (options.preserve_alpha_coverage) {
            preserve_coverage(
                level, source_covered, source_pixels, options.alpha_cutoff);
        }
        result.levels.push_back(std::move(level));
    }
    if (auto digest_result = result.refresh_digest(limits); !digest_result) {
        return core::Result<Rgba8MipChain>::failure(digest_result.error());
    }
    if (auto validation_result = result.validate(limits); !validation_result) {
        return core::Result<Rgba8MipChain>::failure(validation_result.error());
    }
    return core::Result<Rgba8MipChain>::success(std::move(result));
}

core::Result<std::vector<std::uint8_t>> serialize_rgba8_mip_chain(
    const Rgba8MipChain& chain,
    const TextureSourceLimits& limits) {
    if (auto result = chain.validate(limits); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    auto bytes = canonical_payload(chain);
    if (bytes.size() > kMaxRgba8MipArtifactBytes - chain.artifact_digest.bytes.size()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "RGBA8 mip artifact exceeds its serialized byte bound"));
    }
    bytes.insert(
        bytes.end(), chain.artifact_digest.bytes.begin(), chain.artifact_digest.bytes.end());
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<Rgba8MipChain> deserialize_rgba8_mip_chain(
    std::span<const std::uint8_t> bytes,
    const TextureSourceLimits& limits) {
    if (auto result = limits.validate(); !result) {
        return core::Result<Rgba8MipChain>::failure(result.error());
    }
    if (bytes.size() < kMipMagic.size() + 4U + 32U ||
        bytes.size() > kMaxRgba8MipArtifactBytes ||
        !std::equal(kMipMagic.begin(), kMipMagic.end(), bytes.begin())) {
        return core::Result<Rgba8MipChain>::failure(invalid(
            "RGBA8 mip artifact framing is invalid"));
    }

    std::size_t cursor = kMipMagic.size();
    Rgba8MipChain chain;
    std::uint32_t algorithm_bytes = 0U;
    std::uint32_t media_type_bytes = 0U;
    std::uint32_t semantic = 0U;
    std::uint32_t color_space = 0U;
    std::uint32_t level_count = 0U;
    if (!read_u32(bytes, cursor, chain.artifact_version) ||
        !read_u32(bytes, cursor, algorithm_bytes) || algorithm_bytes == 0U ||
        algorithm_bytes > 64U || cursor > bytes.size() ||
        bytes.size() - cursor < algorithm_bytes) {
        return core::Result<Rgba8MipChain>::failure(invalid(
            "RGBA8 mip artifact algorithm record is truncated or unbounded"));
    }
    chain.algorithm.assign(
        reinterpret_cast<const char*>(bytes.data() + cursor), algorithm_bytes);
    cursor += algorithm_bytes;
    if (cursor > bytes.size() ||
        bytes.size() - cursor < chain.source.digest.bytes.size()) {
        return core::Result<Rgba8MipChain>::failure(invalid(
            "RGBA8 mip artifact source digest is truncated"));
    }
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
        chain.source.digest.bytes.size(), chain.source.digest.bytes.begin());
    cursor += chain.source.digest.bytes.size();
    if (!read_u64(bytes, cursor, chain.source.bytes) ||
        !read_u32(bytes, cursor, media_type_bytes) || media_type_bytes == 0U ||
        media_type_bytes > 127U || cursor > bytes.size() ||
        bytes.size() - cursor < media_type_bytes) {
        return core::Result<Rgba8MipChain>::failure(invalid(
            "RGBA8 mip artifact source identity is truncated or unbounded"));
    }
    chain.source.media_type.assign(
        reinterpret_cast<const char*>(bytes.data() + cursor), media_type_bytes);
    cursor += media_type_bytes;
    if (!read_u32(bytes, cursor, semantic) ||
        !read_u32(bytes, cursor, color_space) ||
        !read_u32(bytes, cursor, level_count) || level_count == 0U ||
        level_count > limits.max_mip_levels) {
        return core::Result<Rgba8MipChain>::failure(invalid(
            "RGBA8 mip artifact level header is invalid"));
    }
    chain.semantic = static_cast<TextureSemantic>(semantic);
    chain.color_space = static_cast<ColorSpace>(color_space);
    chain.levels.reserve(level_count);
    for (std::uint32_t index = 0U; index < level_count; ++index) {
        Rgba8MipLevel level;
        std::uint64_t pixel_bytes = 0U;
        std::uint64_t level_pixels = 0U;
        if (!read_u32(bytes, cursor, level.width) ||
            !read_u32(bytes, cursor, level.height) ||
            !read_u64(bytes, cursor, pixel_bytes) ||
            level.width == 0U || level.height == 0U ||
            level.width > limits.max_dimension || level.height > limits.max_dimension ||
            (level_pixels = static_cast<std::uint64_t>(level.width) * level.height) >
                limits.max_pixels ||
            pixel_bytes != level_pixels * 4U ||
            pixel_bytes > kMaxRgba8MipArtifactBytes || cursor > bytes.size() ||
            bytes.size() - cursor < pixel_bytes) {
            return core::Result<Rgba8MipChain>::failure(invalid(
                "RGBA8 mip artifact level is truncated or exceeds bounds"));
        }
        level.pixels.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor + pixel_bytes));
        cursor += static_cast<std::size_t>(pixel_bytes);
        chain.levels.push_back(std::move(level));
    }
    if (cursor > bytes.size() ||
        bytes.size() - cursor != chain.artifact_digest.bytes.size()) {
        return core::Result<Rgba8MipChain>::failure(invalid(
            "RGBA8 mip artifact has a truncated digest or trailing data"));
    }
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
        chain.artifact_digest.bytes.size(), chain.artifact_digest.bytes.begin());
    if (auto result = chain.validate(limits); !result) {
        return core::Result<Rgba8MipChain>::failure(result.error());
    }
    return core::Result<Rgba8MipChain>::success(std::move(chain));
}

} // namespace carto::assets
