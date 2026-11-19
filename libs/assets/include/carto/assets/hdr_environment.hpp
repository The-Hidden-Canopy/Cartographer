#pragma once
#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <cstddef>
#include <span>
#include <vector>

namespace carto::assets {
enum class HdrLightingQuality { standard, high };
inline constexpr std::size_t kMaxHdrEnvironmentSourceBytes = 64U * 1024U * 1024U;

struct HdrEnvironmentImage {
    Sha256Digest source_digest;
    std::uint32_t source_width = 0, source_height = 0;
    std::uint32_t width = 0, height = 0;
    std::vector<core::Vec3d> linear_radiance;
};
// Bounded Radiance RGBE lat-long import, including planar RLE and axis flips.
// Sources are limited to 4096x2048 and 64 MiB encoded; sources above 2048x1024
// are accumulated into the lighting grid without a full decoded source image.
// EXPOSURE/COLORCORR are removed, primaries adapted to linear Rec.709/D65.
// The persisted lighting grid is solid-angle filtered to <=256x128 by default,
// or <=512x256 when high quality is explicitly requested.
// Unsupported encodings, out-of-gamut radiance and malformed input fail atomically.
[[nodiscard]] core::Result<HdrEnvironmentImage> decode_hdr_environment(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] core::Result<HdrEnvironmentImage> decode_hdr_environment_with_quality(
    std::span<const std::uint8_t> bytes, HdrLightingQuality quality);
} // namespace carto::assets
