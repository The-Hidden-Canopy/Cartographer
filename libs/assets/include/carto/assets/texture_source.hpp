#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::assets {

enum class TextureSourceFormat {
    png,
    jpeg,
    tga,
    dds,
    exr,
};

enum class TextureSourceStage {
    // Container/header structure and source bytes were inspected and hashed.
    // Pixel decode, color conversion, and GPU upload are not implied.
    container_inspected,
    decoded,
};

[[nodiscard]] const char* texture_source_format_name(TextureSourceFormat format) noexcept;
[[nodiscard]] const char* texture_source_media_type(TextureSourceFormat format) noexcept;

inline constexpr std::uint64_t kTextureSourceAbsoluteMaxBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kTextureAbsoluteMaxDimension = 16'384U;
inline constexpr std::uint64_t kTextureAbsoluteMaxPixels = 268'435'456ULL;
inline constexpr std::uint32_t kTextureAbsoluteMaxMipLevels = 32U;

struct TextureSourceLimits {
    std::uint64_t max_source_bytes = kTextureSourceAbsoluteMaxBytes;
    std::uint32_t max_dimension = kTextureAbsoluteMaxDimension;
    std::uint64_t max_pixels = kTextureAbsoluteMaxPixels;
    std::uint32_t max_mip_levels = kTextureAbsoluteMaxMipLevels;

    [[nodiscard]] core::Result<void> validate() const;
};

// This is source-admission evidence, not decoded-image evidence. Unknown
// channel/bit-depth data in block-compressed DDS containers remains zero until
// an actual decoder resolves it.
struct TextureSourceAdmission {
    TextureSourceFormat format = TextureSourceFormat::png;
    TextureSourceStage stage = TextureSourceStage::container_inspected;
    BlobRef source;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t channels = 0U;
    std::uint32_t bit_depth = 0U;
    std::uint32_t mip_levels = 1U;
    bool compressed = false;
    bool high_dynamic_range = false;

    [[nodiscard]] core::Result<void> validate(
        const TextureSourceLimits& limits = {}) const;
};

// Detects and structurally inspects PNG, JPEG, TGA, DDS, or EXR source bytes,
// binds their SHA-256 digest, and rejects a mismatched declared media type.
// It does not claim to decode pixels.
[[nodiscard]] core::Result<TextureSourceAdmission> inspect_texture_source(
    std::span<const std::uint8_t> bytes,
    std::string_view declared_media_type = {},
    const TextureSourceLimits& limits = {});

struct DecodedRgba8Texture {
    TextureSourceAdmission admission;
    // Canonical tightly packed, top-left-origin RGBA8 pixels.
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] core::Result<void> validate(
        const TextureSourceLimits& limits = {}) const;
};

// Decodes bounded 24/32-bit true-color TGA, including RLE packets and both
// horizontal/vertical origin flags. Source bytes remain digest-bound in the
// decoded admission receipt.
[[nodiscard]] core::Result<DecodedRgba8Texture> decode_tga_rgba8(
    std::span<const std::uint8_t> bytes,
    const TextureSourceLimits& limits = {});

} // namespace carto::assets
