#include <carto/assets/texture_source.hpp>
#include <carto/assets/texture_mip.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <span>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_le32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_cstring(std::vector<std::uint8_t>& bytes, std::string_view value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0U);
}

std::uint32_t png_crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void append_png_chunk(
    std::vector<std::uint8_t>& bytes,
    std::string_view type,
    std::span<const std::uint8_t> payload) {
    require(type.size() == 4U, "PNG test chunk type is four bytes");
    append_be32(bytes, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crc_begin = bytes.size();
    bytes.insert(bytes.end(), type.begin(), type.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    append_be32(bytes, png_crc32(std::span<const std::uint8_t>{bytes}.subspan(crc_begin)));
}

std::vector<std::uint8_t> png_fixture() {
    std::vector<std::uint8_t> bytes{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    std::array<std::uint8_t, 13U> ihdr{};
    ihdr[3U] = 2U;
    ihdr[7U] = 1U;
    ihdr[8U] = 8U;
    ihdr[9U] = 6U;
    append_png_chunk(bytes, "IHDR", ihdr);
    constexpr std::array<std::uint8_t, 1U> idat{0U};
    append_png_chunk(bytes, "IDAT", idat);
    append_png_chunk(bytes, "IEND", std::span<const std::uint8_t>{});
    return bytes;
}

std::vector<std::uint8_t> jpeg_fixture() {
    return {
        0xffU, 0xd8U,
        0xffU, 0xc0U, 0x00U, 0x11U,
        0x08U, 0x00U, 0x01U, 0x00U, 0x02U, 0x03U,
        0x01U, 0x11U, 0x00U,
        0x02U, 0x11U, 0x00U,
        0x03U, 0x11U, 0x00U,
        0xffU, 0xd9U,
    };
}

std::vector<std::uint8_t> tga_fixture() {
    std::vector<std::uint8_t> bytes(18U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = 2U;
    bytes[14U] = 1U;
    bytes[16U] = 24U;
    bytes.insert(bytes.end(), {0U, 0U, 255U, 0U, 255U, 0U});
    return bytes;
}

std::vector<std::uint8_t> rgba_tga_fixture(
    std::uint16_t width,
    std::uint16_t height,
    std::span<const std::uint8_t> rgba) {
    require(rgba.size() == static_cast<std::size_t>(width) * height * 4U,
            "RGBA TGA fixture has the declared pixel count");
    std::vector<std::uint8_t> bytes(18U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = static_cast<std::uint8_t>(width & 0xffU);
    bytes[13U] = static_cast<std::uint8_t>(width >> 8U);
    bytes[14U] = static_cast<std::uint8_t>(height & 0xffU);
    bytes[15U] = static_cast<std::uint8_t>(height >> 8U);
    bytes[16U] = 32U;
    bytes[17U] = 0x28U;
    for (std::size_t offset = 0U; offset < rgba.size(); offset += 4U) {
        bytes.insert(bytes.end(), {
            rgba[offset + 2U], rgba[offset + 1U], rgba[offset], rgba[offset + 3U]});
    }
    return bytes;
}

std::vector<std::uint8_t> dds_fixture() {
    std::vector<std::uint8_t> bytes(128U, 0U);
    bytes[0U] = 'D'; bytes[1U] = 'D'; bytes[2U] = 'S'; bytes[3U] = ' ';
    const auto write = [&bytes](std::size_t offset, std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            bytes[offset++] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
        }
    };
    write(4U, 124U);
    write(12U, 4U);
    write(16U, 4U);
    write(28U, 1U);
    write(76U, 32U);
    write(80U, 0x41U);
    write(88U, 32U);
    bytes.push_back(0U);
    return bytes;
}

std::vector<std::uint8_t> exr_fixture() {
    std::vector<std::uint8_t> bytes;
    append_le32(bytes, 0x01312f76U);
    append_le32(bytes, 2U);

    append_cstring(bytes, "dataWindow");
    append_cstring(bytes, "box2i");
    append_le32(bytes, 16U);
    append_le32(bytes, 0U);
    append_le32(bytes, 0U);
    append_le32(bytes, 1U);
    append_le32(bytes, 0U);

    std::vector<std::uint8_t> channels;
    for (const std::string_view name : {"R", "G", "B"}) {
        append_cstring(channels, name);
        append_le32(channels, 1U);
        channels.insert(channels.end(), {0U, 0U, 0U, 0U});
        append_le32(channels, 1U);
        append_le32(channels, 1U);
    }
    channels.push_back(0U);
    append_cstring(bytes, "channels");
    append_cstring(bytes, "chlist");
    append_le32(bytes, static_cast<std::uint32_t>(channels.size()));
    bytes.insert(bytes.end(), channels.begin(), channels.end());
    bytes.push_back(0U);
    bytes.push_back(0U);
    return bytes;
}

void supported_sources_are_digest_bound_without_decode_claims() {
    using namespace carto::assets;
    const struct Case {
        std::vector<std::uint8_t> bytes;
        TextureSourceFormat format;
        const char* media_type;
        std::uint32_t width;
        std::uint32_t height;
    } cases[] = {
        {png_fixture(), TextureSourceFormat::png, "image/png", 2U, 1U},
        {jpeg_fixture(), TextureSourceFormat::jpeg, "image/jpeg", 2U, 1U},
        {tga_fixture(), TextureSourceFormat::tga, "image/x-tga", 2U, 1U},
        {dds_fixture(), TextureSourceFormat::dds, "image/vnd-ms.dds", 4U, 4U},
        {exr_fixture(), TextureSourceFormat::exr, "image/x-exr", 2U, 1U},
    };
    for (const auto& test : cases) {
        const auto first = inspect_texture_source(test.bytes, test.media_type);
        const auto second = inspect_texture_source(test.bytes, test.media_type);
        require(first && second, "supported texture source is structurally admitted");
        require(first.value().format == test.format &&
                    first.value().stage == TextureSourceStage::container_inspected &&
                    first.value().width == test.width && first.value().height == test.height &&
                    first.value().source.bytes == test.bytes.size() &&
                    first.value().source.digest == second.value().source.digest &&
                    first.value().source.media_type == test.media_type,
                "texture admission records deterministic bounded source evidence");
        require(static_cast<bool>(first.value().validate()),
                "texture admission validates without claiming decoded pixels");
    }
}

void malformed_and_mislabeled_sources_fail_closed() {
    using namespace carto::assets;
    const auto png = png_fixture();
    require(!inspect_texture_source({}, "image/png"), "empty texture source is rejected");
    require(!inspect_texture_source(png, "image/jpeg"),
            "declared media type cannot disagree with inspected bytes");
    require(!inspect_texture_source(png, "image/png\nprivate"),
            "unsafe declared media type is rejected");

    auto bad_crc = png;
    bad_crc[24U] ^= 1U;
    require(!inspect_texture_source(bad_crc), "PNG CRC corruption is rejected");
    auto trailing_png = png;
    trailing_png.push_back(0U);
    require(!inspect_texture_source(trailing_png), "PNG polyglot trailing data is rejected");

    auto jpeg = jpeg_fixture();
    jpeg.pop_back();
    require(!inspect_texture_source(jpeg), "JPEG without a complete EOI is rejected");
    auto tga = tga_fixture();
    tga.resize(18U);
    require(!inspect_texture_source(tga), "truncated TGA pixel payload is rejected");
    auto dds = dds_fixture();
    dds.resize(128U);
    require(!inspect_texture_source(dds), "DDS without image payload is rejected");
    auto exr = exr_fixture();
    exr.resize(8U);
    require(!inspect_texture_source(exr), "EXR without bounded attributes is rejected");
    const std::array<std::uint8_t, 32U> random{};
    require(!inspect_texture_source(random), "unknown zero-filled source is rejected");

    const TextureSourceLimits tiny{
        .max_source_bytes = 1024U,
        .max_dimension = 1U,
        .max_pixels = 1U,
        .max_mip_levels = 1U,
    };
    require(!inspect_texture_source(png, "image/png", tiny),
            "source dimensions beyond configured admission limits are rejected");

    auto inspected = inspect_texture_source(png);
    require(static_cast<bool>(inspected), "decoded-stage adversarial fixture begins valid");
    inspected.value().stage = TextureSourceStage::decoded;
    inspected.value().channels = 0U;
    require(!inspected.value().validate(),
            "container inspection cannot be relabeled as decoded evidence without channels");

    inspected = inspect_texture_source(png);
    require(static_cast<bool>(inspected), "unknown-format adversarial fixture begins valid");
    inspected.value().format = static_cast<TextureSourceFormat>(255U);
    inspected.value().source.media_type = "application/octet-stream";
    require(!inspected.value().validate(),
            "an unknown texture format cannot validate through the generic media type");
}

void caller_limits_cannot_disable_texture_safety_bounds() {
    using namespace carto::assets;

    const TextureSourceLimits absolute_limits{
        .max_source_bytes = kTextureSourceAbsoluteMaxBytes,
        .max_dimension = kTextureAbsoluteMaxDimension,
        .max_pixels = kTextureAbsoluteMaxPixels,
        .max_mip_levels = kTextureAbsoluteMaxMipLevels,
    };
    require(static_cast<bool>(absolute_limits.validate()),
            "documented absolute texture limits remain admissible");

    auto unbounded = absolute_limits;
    ++unbounded.max_source_bytes;
    require(!unbounded.validate(),
            "callers cannot relax the absolute source-byte ceiling");
    unbounded = absolute_limits;
    ++unbounded.max_dimension;
    require(!unbounded.validate(),
            "callers cannot relax the absolute dimension ceiling");
    unbounded = absolute_limits;
    ++unbounded.max_pixels;
    require(!unbounded.validate(),
            "callers cannot relax the absolute decoded-pixel ceiling");
    unbounded = absolute_limits;
    ++unbounded.max_mip_levels;
    require(!unbounded.validate(),
            "callers cannot relax the absolute mip-level ceiling");
}

void tga_decode_advances_evidence_and_canonicalizes_orientation() {
    using namespace carto::assets;
    const auto source = tga_fixture();
    const auto decoded = decode_tga_rgba8(source);
    require(decoded && decoded.value().admission.stage == TextureSourceStage::decoded &&
                decoded.value().admission.source.digest ==
                    inspect_texture_source(source).value().source.digest,
            "TGA decode advances evidence while preserving source identity");
    const std::vector<std::uint8_t> expected{
        255U, 0U, 0U, 255U,
        0U, 255U, 0U, 255U,
    };
    require(decoded.value().pixels == expected,
            "TGA BGR pixels decode to canonical RGBA8 ordering");

    std::vector<std::uint8_t> oriented(18U, 0U);
    oriented[2U] = 2U;
    oriented[12U] = 2U;
    oriented[14U] = 2U;
    oriented[16U] = 32U;
    oriented[17U] = 0x38U;
    // Right-to-left, top-to-bottom file order: green, red, white, blue.
    oriented.insert(oriented.end(), {
        0U, 255U, 0U, 64U,
        0U, 0U, 255U, 128U,
        255U, 255U, 255U, 192U,
        255U, 0U, 0U, 255U,
    });
    const auto canonical = decode_tga_rgba8(oriented);
    const std::vector<std::uint8_t> canonical_expected{
        255U, 0U, 0U, 128U,
        0U, 255U, 0U, 64U,
        0U, 0U, 255U, 255U,
        255U, 255U, 255U, 192U,
    };
    require(canonical && canonical.value().pixels == canonical_expected,
            "TGA origin flags and authored alpha canonicalize without ambiguity");

    std::vector<std::uint8_t> rle(18U, 0U);
    rle[2U] = 10U;
    rle[12U] = 2U;
    rle[14U] = 2U;
    rle[16U] = 24U;
    rle[17U] = 0x20U;
    rle.insert(rle.end(), {0x83U, 32U, 64U, 128U});
    const auto rle_decoded = decode_tga_rgba8(rle);
    const std::vector<std::uint8_t> rle_expected{
        128U, 64U, 32U, 255U,
        128U, 64U, 32U, 255U,
        128U, 64U, 32U, 255U,
        128U, 64U, 32U, 255U,
    };
    require(rle_decoded && rle_decoded.value().admission.compressed &&
                rle_decoded.value().pixels == rle_expected,
            "TGA RLE packets decode to the exact canonical pixel count");

    for (std::size_t size = 0U; size < rle.size(); ++size) {
        require(!decode_tga_rgba8(std::span<const std::uint8_t>{rle}.first(size)),
                "every truncated RLE prefix fails closed");
    }
    for (std::size_t index = 0U; index < rle.size(); ++index) {
        auto mutated = rle;
        mutated[index] ^= 0x5aU;
        const auto mutation_result = decode_tga_rgba8(mutated);
        require(!mutation_result || static_cast<bool>(mutation_result.value().validate()),
                "single-byte RLE mutations either reject or produce validated evidence");
    }

    auto malformed_decoded = decoded.value();
    malformed_decoded.pixels.pop_back();
    require(!malformed_decoded.validate(),
            "decoded RGBA8 evidence rejects a mismatched byte count");
    require(!decode_tga_rgba8(png_fixture()),
            "TGA decoder rejects a different admitted container format");
}

void mip_generation_is_semantic_and_color_space_correct() {
    using namespace carto::assets;
    const std::array<std::uint8_t, 8U> black_white{
        0U, 0U, 0U, 255U,
        255U, 255U, 255U, 255U,
    };
    const auto decoded = decode_tga_rgba8(rgba_tga_fixture(2U, 1U, black_white));
    require(static_cast<bool>(decoded), "mip color fixture decodes");

    const auto srgb = generate_rgba8_mips(decoded.value(), {
        .semantic = TextureSemantic::base_color,
        .color_space = ColorSpace::srgb,
    });
    require(srgb && srgb.value().levels.size() == 2U &&
                srgb.value().levels.back().pixels[0U] == 188U &&
                srgb.value().levels.back().pixels[1U] == 188U &&
                srgb.value().levels.back().pixels[2U] == 188U &&
                srgb.value().levels.back().pixels[3U] == 255U,
            "sRGB channels filter in linear light while alpha stays linear");

    const auto linear = generate_rgba8_mips(decoded.value(), {
        .semantic = TextureSemantic::data,
        .color_space = ColorSpace::linear,
    });
    require(linear && linear.value().levels.back().pixels[0U] == 128U,
            "linear data channels use an arithmetic box filter");
    require(linear.value().resident_bytes() == 12U,
            "mip resident byte count includes every canonical level");
    require(!generate_rgba8_mips(decoded.value(), {
                .semantic = TextureSemantic::base_color,
                .color_space = ColorSpace::linear,
            }),
            "semantic/color-space disagreement fails closed");
}

void mip_generation_preserves_normals_alpha_and_odd_edges() {
    using namespace carto::assets;
    const std::array<std::uint8_t, 16U> normals{
        255U, 128U, 128U, 255U,
        128U, 255U, 128U, 255U,
        128U, 128U, 255U, 255U,
        128U, 128U, 255U, 255U,
    };
    const auto decoded_normals = decode_tga_rgba8(rgba_tga_fixture(2U, 2U, normals));
    const auto normal_mips = generate_rgba8_mips(decoded_normals.value(), {
        .semantic = TextureSemantic::normal,
        .color_space = ColorSpace::linear,
    });
    require(static_cast<bool>(normal_mips), "normal-map mip generation succeeds");
    const auto& normal = normal_mips.value().levels.back().pixels;
    const double nx = normal[0U] / 127.5 - 1.0;
    const double ny = normal[1U] / 127.5 - 1.0;
    const double nz = normal[2U] / 127.5 - 1.0;
    require(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0) < 0.015,
            "filtered normal is renormalized rather than darkened");

    const std::array<std::uint8_t, 16U> cutout{
        255U, 255U, 255U, 255U,
        255U, 255U, 255U, 0U,
        255U, 255U, 255U, 0U,
        255U, 255U, 255U, 0U,
    };
    const auto decoded_cutout = decode_tga_rgba8(rgba_tga_fixture(2U, 2U, cutout));
    const auto ordinary = generate_rgba8_mips(decoded_cutout.value(), {
        .semantic = TextureSemantic::mask,
        .color_space = ColorSpace::linear,
    });
    const auto preserved = generate_rgba8_mips(decoded_cutout.value(), {
        .semantic = TextureSemantic::mask,
        .color_space = ColorSpace::linear,
        .preserve_alpha_coverage = true,
        .alpha_cutoff = 128U,
    });
    require(ordinary && ordinary.value().levels.back().pixels[3U] == 64U &&
                preserved && preserved.value().levels.back().pixels[3U] == 128U,
            "optional alpha coverage retains a sparse cutout at terminal mip");
    require(!generate_rgba8_mips(decoded_cutout.value(), {
                .semantic = TextureSemantic::mask,
                .color_space = ColorSpace::linear,
                .preserve_alpha_coverage = true,
                .alpha_cutoff = 0U,
            }),
            "coverage preservation rejects an ambiguous zero cutoff");

    const std::array<std::uint8_t, 12U> odd_row{
        0U, 0U, 0U, 255U,
        0U, 0U, 0U, 255U,
        255U, 255U, 255U, 255U,
    };
    const auto decoded_odd = decode_tga_rgba8(rgba_tga_fixture(3U, 1U, odd_row));
    const auto odd_mips = generate_rgba8_mips(decoded_odd.value(), {
        .semantic = TextureSemantic::data,
        .color_space = ColorSpace::linear,
    });
    require(odd_mips && odd_mips.value().levels.back().pixels[0U] == 85U,
            "odd extents account for the terminal source texel");
}

void malformed_mip_chains_fail_closed() {
    using namespace carto::assets;
    const std::array<std::uint8_t, 16U> pixels{
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
        9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U,
    };
    const auto decoded = decode_tga_rgba8(rgba_tga_fixture(2U, 2U, pixels));
    const auto generated = generate_rgba8_mips(decoded.value(), {
        .semantic = TextureSemantic::data,
        .color_space = ColorSpace::linear,
    });
    require(generated && generated.value().validate(),
            "generated canonical mip chain validates");

    auto wrong_bytes = generated.value();
    wrong_bytes.levels.front().pixels.pop_back();
    require(!wrong_bytes.validate(), "mip chain rejects a truncated level");
    auto missing_terminal = generated.value();
    missing_terminal.levels.pop_back();
    require(!missing_terminal.validate(), "mip chain requires a terminal texel");
    auto duplicate_terminal = generated.value();
    duplicate_terminal.levels.push_back(duplicate_terminal.levels.back());
    require(!duplicate_terminal.validate(), "mip chain rejects duplicate terminal levels");
    auto forged_space = generated.value();
    forged_space.color_space = ColorSpace::srgb;
    require(!forged_space.validate(), "mip chain rejects forged semantic color space");

    const auto regenerated = generate_rgba8_mips(decoded.value(), {
        .semantic = TextureSemantic::data,
        .color_space = ColorSpace::linear,
    });
    require(regenerated &&
                regenerated.value().artifact_digest == generated.value().artifact_digest &&
                !generated.value().artifact_digest.is_zero(),
            "identical mip inputs produce a stable non-zero artifact digest");
    const auto serialized = serialize_rgba8_mip_chain(generated.value());
    require(static_cast<bool>(serialized), "canonical mip artifact serializes");
    const auto reopened = deserialize_rgba8_mip_chain(serialized.value());
    require(reopened && reopened.value().artifact_digest == generated.value().artifact_digest &&
                reopened.value().source.digest == generated.value().source.digest &&
                reopened.value().levels.size() == generated.value().levels.size() &&
                reopened.value().levels.front().pixels == generated.value().levels.front().pixels,
            "mip artifact survives a digest-verified save/reopen round trip");
    TextureSourceLimits base_extent_limits;
    base_extent_limits.max_pixels = 4U;
    const auto reopened_with_base_extent_limit =
        deserialize_rgba8_mip_chain(serialized.value(), base_extent_limits);
    require(reopened_with_base_extent_limit &&
                reopened_with_base_extent_limit.value().resident_bytes() == 20U,
            "mip decode applies max_pixels to the base extent rather than the full chain");

    auto tampered = serialized.value();
    tampered[tampered.size() / 2U] ^= 1U;
    require(!deserialize_rgba8_mip_chain(tampered),
            "tampered mip artifact is rejected by structure or digest evidence");
    auto truncated = serialized.value();
    truncated.pop_back();
    require(!deserialize_rgba8_mip_chain(truncated),
            "truncated mip artifact fails closed");
    auto trailing = serialized.value();
    trailing.push_back(0U);
    require(!deserialize_rgba8_mip_chain(trailing),
            "mip artifact rejects unbound trailing bytes");
    auto wrong_magic = serialized.value();
    wrong_magic.front() = 'X';
    require(!deserialize_rgba8_mip_chain(wrong_magic),
            "mip artifact rejects an unknown container identity");
}

} // namespace

int main() {
    supported_sources_are_digest_bound_without_decode_claims();
    malformed_and_mislabeled_sources_fail_closed();
    caller_limits_cannot_disable_texture_safety_bounds();
    tga_decode_advances_evidence_and_canonicalizes_orientation();
    mip_generation_is_semantic_and_color_space_correct();
    mip_generation_preserves_normals_alpha_and_odd_edges();
    malformed_mip_chains_fail_closed();
    std::cout << "texture source tests passed\n";
    return EXIT_SUCCESS;
}
