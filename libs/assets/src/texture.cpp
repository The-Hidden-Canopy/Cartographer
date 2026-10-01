#include <carto/assets/texture.hpp>

#include <algorithm>
#include <utility>

namespace carto::assets {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

core::Result<void> TextureAsset::validate() const {
    if (id == 0U || name.empty() || width == 0U || height == 0U ||
        source_format == PixelFormat::unknown || mips.empty()) {
        return core::Result<void>::failure(
            invalid("texture asset identity, extent, source format, and mip data are required"));
    }
    if (color_space != expected_color_space(semantic)) {
        return core::Result<void>::failure(
            validation("texture asset color space does not match its semantic"));
    }
    if (dimension == TextureDimension::cube && width != height) {
        return core::Result<void>::failure(
            validation("cube texture assets require a square source extent"));
    }

    std::uint32_t previous_width = width;
    std::uint32_t previous_height = height;
    for (std::size_t index = 0U; index < mips.size(); ++index) {
        const MipLevel& mip = mips[index];
        if (mip.width == 0U || mip.height == 0U || mip.bytes == 0U) {
            return core::Result<void>::failure(
                invalid("texture asset mip levels require non-zero dimensions and bytes"));
        }
        if (index == 0U && (mip.width != width || mip.height != height)) {
            return core::Result<void>::failure(
                validation("texture asset base mip does not match the source extent"));
        }
        if (index > 0U && (mip.width > previous_width || mip.height > previous_height)) {
            return core::Result<void>::failure(
                validation("texture asset mip dimensions must not increase"));
        }
        previous_width = mip.width;
        previous_height = mip.height;
    }
    if (source_blob.has_value() &&
        (source_blob->bytes == 0U || source_blob->digest.is_zero() ||
         source_blob->media_type.empty())) {
        return core::Result<void>::failure(
            invalid("texture asset source blob metadata is incomplete"));
    }
    return core::Result<void>::success();
}

} // namespace carto::assets
