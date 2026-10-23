#pragma once

#include <carto/assets/texture_source.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace carto::assets {

enum class TextureChannel : std::uint8_t { red, green, blue, alpha };

struct TextureChannelInput final {
    // First implementation accepts exact, uncompressed true-color TGA bytes.
    std::span<const std::uint8_t> encoded_tga;
    TextureChannel channel = TextureChannel::red;
};

// Array order is the destination order R, G, B, A. Repeated inputs are
// allowed only when the recipe explicitly selects them.
struct Rgba8ChannelPackRecipe final {
    std::array<TextureChannelInput, 4U> outputs;
};

struct PackedTextureChannelSource final {
    BlobRef source;
    TextureChannel channel = TextureChannel::red;

    [[nodiscard]] constexpr bool operator==(
        const PackedTextureChannelSource& other) const noexcept {
        return source.digest == other.source.digest && source.bytes == other.source.bytes &&
            source.media_type == other.source.media_type && channel == other.channel;
    }
};

struct Rgba8ChannelPack final {
    static constexpr std::uint64_t kMaxPixels = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaxOutputBytes = 256ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaxAggregateInputBytes = 256ULL * 1024ULL * 1024ULL;

    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::array<PackedTextureChannelSource, 4U> sources;
    std::vector<std::uint8_t> rgba;
    Sha256Digest artifact_digest;

    [[nodiscard]] core::Result<void> validate_against(
        const Rgba8ChannelPackRecipe& recipe,
        const TextureSourceLimits& limits = {}) const;
};

// Packs only after every source container and decoded pixel payload is
// validated. Dimensions must match exactly. Selecting alpha requires an
// authored 8-bit TGA alpha channel; the decoder's opaque fill for RGB inputs
// is never treated as authored alpha.
[[nodiscard]] core::Result<Rgba8ChannelPack> pack_rgba8_channels(
    const Rgba8ChannelPackRecipe& recipe,
    const TextureSourceLimits& limits = {});

} // namespace carto::assets
