#pragma once

#include <carto/assets/blob_store.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::assets {

using TextureAssetId = std::uint64_t;

enum class TextureDimension {
    texture_2d,
    texture_2d_array,
    cube,
};

enum class TextureSemantic {
    base_color,
    normal,
    metallic_roughness,
    occlusion,
    emissive,
    height,
    mask,
    hdri,
    data,
};

enum class ColorSpace {
    linear,
    srgb,
};

enum class PixelFormat {
    unknown,
    r8,
    rg8,
    rgba8,
    rgba16_float,
    rgba32_float,
    bc5,
    bc6h,
    bc7,
};

struct MipLevel {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint64_t bytes = 0U;
};

[[nodiscard]] constexpr ColorSpace expected_color_space(TextureSemantic semantic) noexcept {
    switch (semantic) {
    case TextureSemantic::base_color:
    case TextureSemantic::emissive:
        return ColorSpace::srgb;
    case TextureSemantic::normal:
    case TextureSemantic::metallic_roughness:
    case TextureSemantic::occlusion:
    case TextureSemantic::height:
    case TextureSemantic::mask:
    case TextureSemantic::hdri:
    case TextureSemantic::data:
        return ColorSpace::linear;
    }
    return ColorSpace::linear;
}

struct TextureAsset {
    TextureAssetId id = 0U;
    std::string name;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    TextureDimension dimension = TextureDimension::texture_2d;
    TextureSemantic semantic = TextureSemantic::data;
    ColorSpace color_space = ColorSpace::linear;
    PixelFormat source_format = PixelFormat::unknown;
    std::vector<MipLevel> mips;
    std::optional<BlobRef> source_blob;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::assets
