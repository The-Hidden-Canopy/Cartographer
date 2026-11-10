#pragma once
#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <span>
#include <vector>

namespace carto::assets {
struct HdrEnvironmentImage {
    Sha256Digest source_digest;
    std::uint32_t source_width = 0, source_height = 0;
    std::uint32_t width = 0, height = 0;
    std::vector<core::Vec3d> linear_radiance;
};
// Bounded Radiance RGBE lat-long import, including planar RLE and axis flips.
// EXPOSURE/COLORCORR are removed, primaries adapted to linear Rec.709/D65.
// The persisted lighting grid is explicitly solid-angle filtered to <=128x64.
// Unsupported encodings, out-of-gamut radiance and malformed input fail atomically.
[[nodiscard]] core::Result<HdrEnvironmentImage> decode_hdr_environment(
    std::span<const std::uint8_t> bytes);
} // namespace carto::assets
