#pragma once

#include <carto/assets/texture_source.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::assets {

enum class TexturePaintTarget : std::uint8_t {
    red = 0U,
    green = 1U,
    blue = 2U,
    alpha = 3U,
    mask = 4U,
};

enum class TexturePaintMode : std::uint8_t {
    paint = 0U,
    erase = 1U,
};

struct TexturePaintStamp final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint16_t radius = 1U;
    std::uint8_t value = 0U;
    std::uint8_t opacity = 255U;
    TexturePaintMode mode = TexturePaintMode::paint;

    [[nodiscard]] bool operator==(const TexturePaintStamp&) const = default;
};

struct TexturePaintLayer final {
    std::string identity;
    TexturePaintTarget target = TexturePaintTarget::red;
    // Mask layers start from this explicit value; channel layers require 0
    // because their initial pixels come from the bound source texture.
    std::uint8_t initial_value = 0U;

    [[nodiscard]] bool operator==(const TexturePaintLayer&) const = default;
};

struct TexturePaintCommand final {
    std::string layer_identity;
    TexturePaintStamp stamp;

    [[nodiscard]] bool operator==(const TexturePaintCommand&) const = default;
};

// Encoded pixels remain in the caller's content-addressed source store. Paint
// documents bind identity, revision, size, extent, and digest, then require the
// exact source again on reopen or compile.
struct TexturePaintSource final {
    std::string identity;
    core::Revision revision;
    std::span<const std::uint8_t> encoded_tga;
};

struct TextureMaskLayerProduct final {
    std::string identity;
    // Canonical row-major R8 mask pixels.
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool operator==(const TextureMaskLayerProduct&) const = default;
};

class TexturePaintDocument final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxLayers = 128U;
    static constexpr std::size_t kMaxCommands = 65'536U;
    static constexpr std::uint32_t kMaxDimension = 16'384U;
    static constexpr std::uint64_t kMaxPixels = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaxSourceBytes = 256ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaxAggregateOutputBytes =
        256ULL * 1024ULL * 1024ULL;
    static constexpr std::uint16_t kMaxBrushRadius = 512U;
    static constexpr std::uint64_t kMaxBrushWorkPixels = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    TexturePaintDocument(const TexturePaintDocument&) = default;
    TexturePaintDocument& operator=(const TexturePaintDocument&) = default;
    TexturePaintDocument(TexturePaintDocument&&) noexcept = default;
    TexturePaintDocument& operator=(TexturePaintDocument&&) noexcept = default;

    [[nodiscard]] static core::Result<TexturePaintDocument> create(
        std::string identity,
        const TexturePaintSource& source,
        const TextureSourceLimits& limits = {});

    [[nodiscard]] const std::string& identity() const noexcept { return identity_; }
    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const std::string& source_identity() const noexcept {
        return source_identity_;
    }
    [[nodiscard]] core::Revision source_revision() const noexcept {
        return source_revision_;
    }
    [[nodiscard]] const Sha256Digest& source_digest() const noexcept {
        return source_digest_;
    }
    [[nodiscard]] std::uint64_t source_bytes() const noexcept { return source_bytes_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::size_t history_cursor() const noexcept { return history_cursor_; }
    [[nodiscard]] const std::vector<TexturePaintLayer>& layers() const noexcept {
        return layers_;
    }
    [[nodiscard]] const std::vector<TexturePaintCommand>& commands() const noexcept {
        return commands_;
    }

    [[nodiscard]] core::Result<core::Revision> add_layer(
        core::Revision expected_revision,
        std::string layer_identity,
        TexturePaintTarget target,
        std::uint8_t initial_value);
    [[nodiscard]] core::Result<core::Revision> apply_stamp(
        core::Revision expected_revision,
        std::string_view layer_identity,
        TexturePaintStamp stamp);
    [[nodiscard]] core::Result<core::Revision> undo(core::Revision expected_revision);
    [[nodiscard]] core::Result<core::Revision> redo(core::Revision expected_revision);

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_source(
        const TexturePaintSource& source,
        const TextureSourceLimits& limits = {}) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<TexturePaintDocument> deserialize(
        std::string_view text);

private:
    TexturePaintDocument() = default;

    std::string identity_;
    core::Revision revision_;
    std::string source_identity_;
    core::Revision source_revision_;
    Sha256Digest source_digest_;
    std::uint64_t source_bytes_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::vector<TexturePaintLayer> layers_;
    std::vector<TexturePaintCommand> commands_;
    std::size_t history_cursor_ = 0U;
    std::uint64_t history_work_pixels_ = 0U;
};

struct TexturePaintProduct final {
    std::string document_identity;
    core::Revision document_revision;
    std::string source_identity;
    core::Revision source_revision;
    Sha256Digest source_digest;
    Sha256Digest document_digest;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    // Canonical tightly packed, top-left-origin RGBA8 base plus color-channel edits.
    std::vector<std::uint8_t> rgba;
    std::vector<TextureMaskLayerProduct> masks;
    Sha256Digest artifact_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> export_tga() const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> export_mask_tga(
        std::string_view layer_identity) const;
    [[nodiscard]] core::Result<void> validate_against(
        const TexturePaintDocument& document,
        const TexturePaintSource& source,
        const TextureSourceLimits& limits = {}) const;
};

// Applies bounded integer-pixel brush history over the exact source. RGB/A
// layers edit one source channel; mask layers emit independent R8 planes.
[[nodiscard]] core::Result<TexturePaintProduct> compile_texture_paint(
    const TexturePaintDocument& document,
    const TexturePaintSource& source,
    const TextureSourceLimits& limits = {});

} // namespace carto::assets
