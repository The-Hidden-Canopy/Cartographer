#pragma once

#include <carto/assets/texture.hpp>
#include <carto/assets/texture_source.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::assets {

inline constexpr std::uint32_t kRgba8MipArtifactVersion = 1U;
inline constexpr std::string_view kRgba8MipAlgorithm =
    "carto.rgba8-mip.area-box.v1";
inline constexpr std::uint64_t kMaxRgba8MipArtifactBytes =
    1024ULL * 1024ULL * 1024ULL;

struct Rgba8MipLevel {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> pixels;
};

struct Rgba8MipChain {
    std::uint32_t artifact_version = kRgba8MipArtifactVersion;
    std::string algorithm{kRgba8MipAlgorithm};
    BlobRef source;
    TextureSemantic semantic = TextureSemantic::data;
    ColorSpace color_space = ColorSpace::linear;
    std::vector<Rgba8MipLevel> levels;
    Sha256Digest artifact_digest;

    [[nodiscard]] core::Result<void> validate(
        const TextureSourceLimits& limits = {}) const;
    [[nodiscard]] core::Result<void> refresh_digest(
        const TextureSourceLimits& limits = {});
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept;
};

struct Rgba8MipOptions {
    TextureSemantic semantic = TextureSemantic::data;
    ColorSpace color_space = ColorSpace::linear;
    bool preserve_alpha_coverage = false;
    std::uint8_t alpha_cutoff = 128U;
};

// Produces a complete deterministic mip chain. sRGB RGB channels are filtered
// in linear light, normal maps are decoded/averaged/renormalized, and alpha is
// always filtered linearly. Coverage preservation is explicit and optional.
[[nodiscard]] core::Result<Rgba8MipChain> generate_rgba8_mips(
    const DecodedRgba8Texture& decoded,
    const Rgba8MipOptions& options,
    const TextureSourceLimits& limits = {});

// Canonical binary persistence for a derived mip artifact. The trailing digest
// authenticates all metadata and pixels, including source identity and the
// algorithm version. Unknown versions, trailing bytes, and digest mismatches
// fail closed.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize_rgba8_mip_chain(
    const Rgba8MipChain& chain,
    const TextureSourceLimits& limits = {});
[[nodiscard]] core::Result<Rgba8MipChain> deserialize_rgba8_mip_chain(
    std::span<const std::uint8_t> bytes,
    const TextureSourceLimits& limits = {});

} // namespace carto::assets
