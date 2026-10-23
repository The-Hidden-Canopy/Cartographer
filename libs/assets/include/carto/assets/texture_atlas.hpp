#pragma once

#include <carto/assets/texture_source.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::assets {

// Each assignment places one complete source image at an authored top-left
// pixel coordinate. The first implementation deliberately does not resize,
// rotate, pad, or repack sources; unsupported layout intent stays explicit.
struct TextureAtlasAssignment final {
    std::string source_identity;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
};

struct TextureAtlasSource final {
    std::string identity;
    std::span<const std::uint8_t> encoded_tga;
};

struct ResolvedTextureAtlasAssignment final {
    std::string source_identity;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    Sha256Digest source_digest;

    [[nodiscard]] bool operator==(
        const ResolvedTextureAtlasAssignment&) const = default;
};

struct TextureAtlasRecipe final {
    static constexpr std::size_t kMaxAssignments = 4'096U;
    static constexpr std::uint32_t kMaxDimension = 16'384U;
    static constexpr std::uint64_t kMaxPixels = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaxOutputBytes = 256ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaxAggregateSourceBytes =
        256ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kMaxIdentityBytes = 128U;

    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    // Every unassigned pixel uses this explicit authored value.
    std::array<std::uint8_t, 4U> background_rgba{0U, 0U, 0U, 0U};
    std::vector<TextureAtlasAssignment> assignments;

    [[nodiscard]] core::Result<void> validate(
        const TextureSourceLimits& limits = {}) const;
};

struct TextureAtlasProduct final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::array<std::uint8_t, 4U> background_rgba{0U, 0U, 0U, 0U};
    std::vector<ResolvedTextureAtlasAssignment> assignments;
    // Canonical tightly packed, top-left-origin RGBA8 pixels.
    std::vector<std::uint8_t> rgba;
    Sha256Digest artifact_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> export_tga() const;
    [[nodiscard]] core::Result<void> validate_against(
        const TextureAtlasRecipe& recipe,
        std::span<const TextureAtlasSource> sources,
        const TextureSourceLimits& limits = {}) const;
};

// Compiles an exact, non-overlapping authored layout from uncompressed
// true-color TGA inputs. Every supplied source must be assigned exactly once.
// The receipt binds layout, source digests, background pixels, and output data.
[[nodiscard]] core::Result<TextureAtlasProduct> build_texture_atlas(
    const TextureAtlasRecipe& recipe,
    std::span<const TextureAtlasSource> sources,
    const TextureSourceLimits& limits = {});

} // namespace carto::assets
