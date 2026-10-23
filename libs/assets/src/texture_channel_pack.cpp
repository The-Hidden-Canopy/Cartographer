#include <carto/assets/texture_channel_pack.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace carto::assets {
namespace {

constexpr std::array<std::uint8_t, 27U> kArtifactMagic{
    'C', 'A', 'R', 'T', 'O', '_', 'R', 'G', 'B', 'A', '8', '_',
    'C', 'H', 'A', 'N', 'N', 'E', 'L', '_', 'P', 'A', 'C', 'K', '_', '1', 0U};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

std::optional<std::uint8_t> channel_index(TextureChannel channel) noexcept {
    switch (channel) {
    case TextureChannel::red: return 0U;
    case TextureChannel::green: return 1U;
    case TextureChannel::blue: return 2U;
    case TextureChannel::alpha: return 3U;
    }
    return std::nullopt;
}

TextureSourceLimits bounded_limits(const TextureSourceLimits& requested) {
    auto limits = requested;
    limits.max_source_bytes = std::min(
        limits.max_source_bytes, Rgba8ChannelPack::kMaxAggregateInputBytes);
    limits.max_dimension = std::min(limits.max_dimension, 16'384U);
    limits.max_pixels = std::min(limits.max_pixels, Rgba8ChannelPack::kMaxPixels);
    limits.max_mip_levels = std::min(limits.max_mip_levels, 1U);
    return limits;
}

void append_u64_le(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

void append_u32_le(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

std::vector<std::uint8_t> canonical_artifact(const Rgba8ChannelPack& packed) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kArtifactMagic.size() + 8U + 4U * 41U + packed.rgba.size());
    bytes.insert(bytes.end(), kArtifactMagic.begin(), kArtifactMagic.end());
    append_u32_le(bytes, packed.width);
    append_u32_le(bytes, packed.height);
    for (const auto& source : packed.sources) {
        bytes.insert(bytes.end(), source.source.digest.bytes.begin(),
            source.source.digest.bytes.end());
        append_u64_le(bytes, source.source.bytes);
        bytes.push_back(static_cast<std::uint8_t>(source.channel));
    }
    bytes.insert(bytes.end(), packed.rgba.begin(), packed.rgba.end());
    return bytes;
}

core::Result<Rgba8ChannelPack> compile(
    const Rgba8ChannelPackRecipe& recipe,
    const TextureSourceLimits& requested_limits) {
    if (auto result = requested_limits.validate(); !result) {
        return core::Result<Rgba8ChannelPack>::failure(result.error());
    }
    const auto limits = bounded_limits(requested_limits);
    if (auto result = limits.validate(); !result) {
        return core::Result<Rgba8ChannelPack>::failure(result.error());
    }

    std::array<TextureSourceAdmission, 4U> admissions;
    std::uint64_t total_input_bytes = 0U;
    for (std::size_t output = 0U; output < recipe.outputs.size(); ++output) {
        const auto& input = recipe.outputs[output];
        if (!channel_index(input.channel)) {
            return core::Result<Rgba8ChannelPack>::failure(invalid(
                "channel pack recipe contains an unknown source channel"));
        }
        if (input.encoded_tga.size() >
            Rgba8ChannelPack::kMaxAggregateInputBytes - total_input_bytes) {
            return core::Result<Rgba8ChannelPack>::failure(invalid(
                "channel pack aggregate input byte budget is exceeded"));
        }
        total_input_bytes += input.encoded_tga.size();
        auto inspected = inspect_texture_source(input.encoded_tga, "image/x-tga", limits);
        if (!inspected) return core::Result<Rgba8ChannelPack>::failure(inspected.error());
        if (inspected.value().format != TextureSourceFormat::tga ||
            input.encoded_tga.size() < 18U || input.encoded_tga[2U] != 2U) {
            return core::Result<Rgba8ChannelPack>::failure(unsupported(
                "channel packing currently requires uncompressed true-color TGA sources"));
        }
        const std::uint64_t source_pixel_bytes = inspected.value().bit_depth / 8U;
        const std::uint64_t exact_size = 18U + input.encoded_tga[0U] +
            static_cast<std::uint64_t>(inspected.value().width) *
                inspected.value().height * source_pixel_bytes;
        if (exact_size != input.encoded_tga.size()) {
            return core::Result<Rgba8ChannelPack>::failure(invalid(
                "TGA source contains trailing or incomplete pixel data"));
        }
        if (input.channel == TextureChannel::alpha &&
            (inspected.value().bit_depth != 32U ||
                (input.encoded_tga[17U] & 0x0fU) != 8U)) {
            return core::Result<Rgba8ChannelPack>::failure(unsupported(
                "alpha selection requires an authored 8-bit TGA alpha channel"));
        }
        admissions[output] = inspected.value();
        if (output != 0U &&
            (admissions[output].width != admissions[0U].width ||
                admissions[output].height != admissions[0U].height)) {
            return core::Result<Rgba8ChannelPack>::failure(invalid(
                "channel pack source dimensions must match exactly"));
        }
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(admissions[0U].width) * admissions[0U].height;
    if (pixel_count == 0U || pixel_count > limits.max_pixels ||
        pixel_count * 4U > Rgba8ChannelPack::kMaxOutputBytes ||
        pixel_count > std::numeric_limits<std::size_t>::max() / 4U) {
        return core::Result<Rgba8ChannelPack>::failure(invalid(
            "channel pack output exceeds its pixel or byte budget"));
    }

    Rgba8ChannelPack result{
        .width = admissions[0U].width,
        .height = admissions[0U].height,
        .sources = {},
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(pixel_count) * 4U),
        .artifact_digest = {},
    };
    for (std::size_t output = 0U; output < recipe.outputs.size(); ++output) {
        const auto& input = recipe.outputs[output];
        result.sources[output] = PackedTextureChannelSource{
            .source = admissions[output].source,
            .channel = input.channel,
        };
        auto decoded = decode_tga_rgba8(input.encoded_tga, limits);
        if (!decoded) return core::Result<Rgba8ChannelPack>::failure(decoded.error());
        if (auto result_check = decoded.value().validate(limits); !result_check) {
            return core::Result<Rgba8ChannelPack>::failure(result_check.error());
        }
        const auto source_component = channel_index(input.channel).value();
        for (std::size_t pixel = 0U; pixel < static_cast<std::size_t>(pixel_count); ++pixel) {
            result.rgba[pixel * 4U + output] =
                decoded.value().pixels[pixel * 4U + source_component];
        }
    }
    const auto canonical = canonical_artifact(result);
    result.artifact_digest = sha256(canonical);
    return core::Result<Rgba8ChannelPack>::success(std::move(result));
}

} // namespace

core::Result<Rgba8ChannelPack> pack_rgba8_channels(
    const Rgba8ChannelPackRecipe& recipe, const TextureSourceLimits& limits) {
    return compile(recipe, limits);
}

core::Result<void> Rgba8ChannelPack::validate_against(
    const Rgba8ChannelPackRecipe& recipe, const TextureSourceLimits& limits) const {
    const auto expected = compile(recipe, limits);
    if (!expected) return core::Result<void>::failure(expected.error());
    if (width != expected.value().width || height != expected.value().height ||
        sources != expected.value().sources || rgba != expected.value().rgba ||
        artifact_digest != expected.value().artifact_digest) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "channel pack artifact differs from the exact current recipe or inputs"));
    }
    return core::Result<void>::success();
}

} // namespace carto::assets
