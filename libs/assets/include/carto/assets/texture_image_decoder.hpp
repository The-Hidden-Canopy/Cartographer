#pragma once

#include <carto/assets/texture_source.hpp>

#include <cstdint>
#include <vector>

namespace carto::assets {

// Windows Imaging Component decoder used by native render adapters. The
// expected source binding is checked before pixel data is returned. TGA is
// also accepted here so callers have one digest-bound decode entry point.
[[nodiscard]] core::Result<DecodedRgba8Texture> decode_texture_image_rgba8(
    std::vector<std::uint8_t> encoded_source,
    const Sha256Digest& expected_digest,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    const TextureSourceLimits& limits = {});

} // namespace carto::assets
