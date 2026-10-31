#include "reference_image_decoder.hpp"

#include <carto/assets/texture_image_decoder.hpp>

#include <utility>

namespace carto::desktop {

core::Result<assets::DecodedRgba8Texture> decode_reference_image_pixels(
    std::vector<std::uint8_t> encoded_source,
    const assets::Sha256Digest& expected_digest,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    const assets::TextureSourceLimits& limits) {
    const auto admission = assets::inspect_texture_source(
        encoded_source, {}, limits);
    if (!admission) {
        return core::Result<assets::DecodedRgba8Texture>::failure(
            admission.error().with_context("reference-image source admission"));
    }
    if (admission.value().source.digest != expected_digest ||
        admission.value().width != expected_width ||
        admission.value().height != expected_height) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "reference-image pixels do not match the current source binding"));
    }
    if (admission.value().has_color_encoding_metadata) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "reference-image color-encoding metadata is unsupported by this preview path"));
    }
    return assets::decode_texture_image_rgba8(std::move(encoded_source),
        expected_digest, expected_width, expected_height, limits);
}

} // namespace carto::desktop
