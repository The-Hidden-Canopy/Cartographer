#pragma once

#include <carto/assets/texture_source.hpp>

#include <cstdint>
#include <vector>

namespace carto::desktop {

// The caller supplies verified project-local bytes and the plane's expected
// binding. The desktop codec path accepts PNG, JPEG, and TGA and remains
// bounded by the same dimensions and pixel limits used for source admission.
[[nodiscard]] core::Result<assets::DecodedRgba8Texture>
decode_reference_image_pixels(
    std::vector<std::uint8_t> encoded_source,
    const assets::Sha256Digest& expected_digest,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    const assets::TextureSourceLimits& limits = {});

} // namespace carto::desktop
