#include <carto/assets/texture_source.hpp>
#include <carto/assets/texture_image_decoder.hpp>
#include <carto/assets/texture_mip.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>
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

std::vector<std::uint8_t> png_fixture_with_chunks(
    std::uint8_t color_type,
    std::span<const std::pair<std::string_view, std::span<const std::uint8_t>>> before_idat,
    std::span<const std::pair<std::string_view, std::span<const std::uint8_t>>> after_idat = {}) {
    std::vector<std::uint8_t> bytes{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    std::array<std::uint8_t, 13U> ihdr{};
    ihdr[3U] = 2U;
    ihdr[7U] = 1U;
    ihdr[8U] = 8U;
    ihdr[9U] = color_type;
    append_png_chunk(bytes, "IHDR", ihdr);
    for (const auto& [type, payload] : before_idat) {
        append_png_chunk(bytes, type, payload);
    }
    constexpr std::array<std::uint8_t, 1U> idat{0U};
    append_png_chunk(bytes, "IDAT", idat);
    for (const auto& [type, payload] : after_idat) {
        append_png_chunk(bytes, type, payload);
    }
    append_png_chunk(bytes, "IEND", std::span<const std::uint8_t>{});
    return bytes;
}

std::vector<std::uint8_t> inspection_jpeg_fixture(bool include_scan = true) {
    std::vector<std::uint8_t> bytes{
        0xffU, 0xd8U,
        0xffU, 0xc0U, 0x00U, 0x11U,
        0x08U, 0x00U, 0x01U, 0x00U, 0x02U, 0x03U,
        0x01U, 0x11U, 0x00U,
        0x02U, 0x11U, 0x00U,
        0x03U, 0x11U, 0x00U,
    };
    if (include_scan) {
        bytes.insert(bytes.end(), {
            0xffU, 0xdaU, 0x00U, 0x0cU,
            0x03U, 0x01U, 0x00U, 0x02U, 0x11U, 0x03U, 0x11U,
            0x00U, 0x3fU, 0x00U, 0x00U,
        });
    }
    bytes.insert(bytes.end(), {0xffU, 0xd9U});
    return bytes;
}

std::vector<std::uint8_t> jpeg_fixture() {
    // One 8-bit grayscale block: zero DC difference and EOB, padded with ones.
    std::vector<std::uint8_t> bytes{0xffU, 0xd8U, 0xffU, 0xdbU, 0U, 67U, 0U};
    bytes.insert(bytes.end(), 64U, 1U);
    bytes.insert(bytes.end(), {
        0xffU, 0xc0U, 0U, 11U, 8U, 0U, 1U, 0U, 1U, 1U, 1U, 0x11U, 0U,
        0xffU, 0xc4U, 0U, 38U,
        0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0x10U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U, 63U, 0U, 0x3fU, 0xffU, 0xd9U,
    });
    return bytes;
}

std::vector<std::uint8_t> decodable_png_fixture() {
    std::vector<std::uint8_t> bytes{0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    std::array<std::uint8_t, 13U> ihdr{};
    ihdr[3U] = 1U;
    ihdr[7U] = 1U;
    ihdr[8U] = 8U;
    ihdr[9U] = 6U;
    append_png_chunk(bytes, "IHDR", ihdr);
    // Zlib stored block: filter byte followed by one opaque red RGBA pixel.
    constexpr std::array<std::uint8_t, 16U> idat{
        0x78U, 0x01U, 0x01U, 0x05U, 0x00U, 0xfaU, 0xffU, 0x00U,
        0xffU, 0x00U, 0x00U, 0xffU, 0x05U, 0x00U, 0x01U, 0xffU};
    append_png_chunk(bytes, "IDAT", idat);
    append_png_chunk(bytes, "IEND", std::span<const std::uint8_t>{});
    return bytes;
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

std::vector<std::uint8_t> tga_fixture_with_extension(bool has_color_metadata) {
    auto bytes = tga_fixture();
    const std::uint32_t extension_offset = static_cast<std::uint32_t>(bytes.size());
    const std::size_t extension_begin = bytes.size();
    bytes.resize(bytes.size() + 495U, 0U);
    bytes[extension_begin] = 0xefU;
    bytes[extension_begin + 1U] = 0x01U;
    if (has_color_metadata) {
        bytes[extension_begin + 478U] = 22U;
        bytes[extension_begin + 480U] = 10U;
    }
    append_le32(bytes, extension_offset);
    append_le32(bytes, 0U);
    constexpr std::array<std::uint8_t, 18U> signature{
        'T', 'R', 'U', 'E', 'V', 'I', 'S', 'I', 'O', 'N', '-',
        'X', 'F', 'I', 'L', 'E', '.', 0U};
    bytes.insert(bytes.end(), signature.begin(), signature.end());
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
        {jpeg_fixture(), TextureSourceFormat::jpeg, "image/jpeg", 1U, 1U},
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
    require(!inspect_texture_source(inspection_jpeg_fixture(false)),
            "JPEG frame headers without a scan are not admitted as image data");
    auto malformed_scan = inspection_jpeg_fixture();
    malformed_scan[24U] = 8U;
    require(!inspect_texture_source(malformed_scan),
            "JPEG scan component count must agree with its segment length");
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
}

void png_chunk_semantics_fail_closed_and_keep_legal_metadata() {
    using namespace carto::assets;
    constexpr std::array<std::uint8_t, 1U> payload{0U};
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        unknown_critical{{{"ABCD", payload}}};
    require(!inspect_texture_source(png_fixture_with_chunks(6U, unknown_critical)),
            "unknown critical PNG chunks are rejected before codec interpretation");

    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        invalid_reserved_bit{{{"abca", payload}}};
    require(!inspect_texture_source(png_fixture_with_chunks(6U, invalid_reserved_bit)),
            "PNG chunk types with a lowercase reserved bit are rejected");

    constexpr std::array<std::uint8_t, 8U> animation_control{
        0U, 0U, 0U, 2U, 0U, 0U, 0U, 0U};
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        animated_control{{{"acTL", animation_control}}};
    require(!inspect_texture_source(png_fixture_with_chunks(6U, animated_control)),
            "APNG animation control cannot be flattened into a static texture source");

    constexpr std::array<std::uint8_t, 26U> frame_control{
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 0U, 0U, 0U, 1U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 10U, 0U, 0U};
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        animated_frame{{{"fcTL", frame_control}}};
    require(!inspect_texture_source(png_fixture_with_chunks(6U, animated_frame)),
            "APNG frame controls cannot silently select a decoder-dependent frame");

    constexpr std::array<std::uint8_t, 5U> frame_data{0U, 0U, 0U, 1U, 0U};
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        animated_data{{{"fdAT", frame_data}}};
    require(!inspect_texture_source(png_fixture_with_chunks(6U, {}, animated_data)),
            "APNG frame data is refused when the source contract is single-frame");

    constexpr std::array<std::uint8_t, 3U> text_payload{'k', 0U, 'v'};
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 2U>
        interleaved_idat{{{"tEXt", text_payload}, {"IDAT", payload}}};
    require(!inspect_texture_source(png_fixture_with_chunks(6U, {}, interleaved_idat)),
            "PNG image-data chunks must remain contiguous");

    constexpr std::array<std::uint8_t, 3U> palette{0U, 0U, 0U};
    require(!inspect_texture_source(png_fixture_with_chunks(3U, {})),
            "indexed PNG sources require a palette before image data");
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        forbidden_palette{{{"PLTE", palette}}};
    require(!inspect_texture_source(png_fixture_with_chunks(0U, forbidden_palette)),
            "grayscale PNG sources cannot contain a palette chunk");
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 2U>
        duplicate_palette{{{"PLTE", palette}, {"PLTE", palette}}};
    require(!inspect_texture_source(png_fixture_with_chunks(3U, duplicate_palette)),
            "PNG sources cannot contain duplicate palettes");
    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        valid_palette{{{"PLTE", palette}}};
    require(static_cast<bool>(inspect_texture_source(
                png_fixture_with_chunks(3U, valid_palette))),
            "indexed PNG sources with a legal palette remain admitted");

    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        legal_text{{{"tEXt", text_payload}}};
    const auto text_source = inspect_texture_source(
        png_fixture_with_chunks(6U, {}, legal_text));
    require(static_cast<bool>(text_source),
            "legal ancillary metadata after contiguous image data remains admitted");
    require(!text_source.value().has_color_encoding_metadata,
            "descriptive text metadata is not classified as color encoding");

    const std::array<std::pair<std::string_view, std::span<const std::uint8_t>>, 1U>
        srgb_metadata{{{"sRGB", payload}}};
    const auto srgb_source = inspect_texture_source(
        png_fixture_with_chunks(6U, srgb_metadata));
    require(srgb_source && srgb_source.value().has_color_encoding_metadata,
            "PNG color encoding metadata is surfaced for caller policy");
}

void jpeg_marker_walk_fails_closed_and_preserves_binding() {
    using namespace carto::assets;
    const auto source = jpeg_fixture();
    const auto admission = inspect_texture_source(source, "image/jpeg");
    require(admission && admission.value().source.digest == sha256(source),
            "JPEG admission binds the exact source bytes");
    const auto decoded = decode_texture_image_rgba8(source, sha256(source), 1U, 1U);
#ifdef _WIN32
    require(decoded && decoded.value().admission.source.digest == sha256(source) &&
                decoded.value().pixels.size() == 4U &&
                decoded.value().pixels[0U] == 128U && decoded.value().pixels[1U] == 128U &&
                decoded.value().pixels[2U] == 128U && decoded.value().pixels[3U] == 255U,
            "valid grayscale JPEG decodes its authored pixel with the same source identity");
#else
    require(!decoded, "non-Windows JPEG decode remains explicitly unsupported");
#endif
    const auto wrong_digest = sha256(std::span<const std::uint8_t>{});
    require(!decode_texture_image_rgba8(source, wrong_digest, 1U, 1U) &&
                !decode_texture_image_rgba8(source, sha256(source), 2U, 1U),
            "stale digest and dimensions refuse before pixels are returned");

    const auto with_tail = [&](std::initializer_list<std::uint8_t> tail) {
        auto bytes = source;
        bytes.insert(bytes.end() - 2, tail.begin(), tail.end());
        return bytes;
    };
    const std::vector<std::vector<std::uint8_t>> malformed{
        with_tail({0xffU, 0xe1U, 0U, 1U}), // invalid segment length
        with_tail({0xffU, 0xe1U, 0U, 9U, 1U}), // segment exceeds terminal EOI
        with_tail({0xffU, 0xe1U, 0U}), // truncated length
        with_tail({0xffU, 0xd9U, 0x11U}), // premature EOI
        with_tail({0xffU, 0xd8U}), // nested SOI
        with_tail({0xffU, 0xc0U, 0U, 2U}), // duplicate frame
        with_tail({0xffU, 0xdaU, 0U, 8U, 1U, 2U, 0U, 0U, 63U, 0U, 0x3fU}),
        with_tail({0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U, 63U, 0U}), // empty scan
        with_tail({0xffU, 0xe0U, 0U, 2U, 0x11U}), // entropy outside a scan
        with_tail({0xffU, 0xe2U, 0U, 14U, 'I', 'C', 'C', '_', 'P', 'R', 'O', 'F',
                   'I', 'L', 'E', 0U, 0xffU, 0xe1U, 0U, 1U}), // bad after ICC
    };
    for (const auto& bytes : malformed) {
        require(!inspect_texture_source(bytes, "image/jpeg"),
                "malformed post-scan markers fail source admission");
        require(!decode_texture_image_rgba8(bytes, sha256(bytes), 1U, 1U),
                "malformed JPEG cannot return decoded pixels even with a matching digest");
    }
    auto no_scan = source;
    no_scan.erase(no_scan.end() - 13, no_scan.end() - 2);
    require(!inspect_texture_source(no_scan), "frame without a scan is refused");
    auto bad_first_selector = source;
    bad_first_selector[bad_first_selector.size() - 8U] = 2U;
    require(!inspect_texture_source(bad_first_selector) &&
                !decode_texture_image_rgba8(bad_first_selector, sha256(bad_first_selector), 1U, 1U),
            "unknown first scan component is refused by inspection and decode");
    auto stuffed = with_tail({0xffU, 0x00U, 0x3fU});
    require(static_cast<bool>(inspect_texture_source(stuffed)),
            "stuffed entropy bytes remain structurally admitted");
    auto two_scans = with_tail({0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U, 63U, 0U, 0x3fU});
    require(static_cast<bool>(inspect_texture_source(two_scans)),
            "bounded later scan headers are walked structurally");
    const auto icc = with_tail({0xffU, 0xe2U, 0U, 14U, 'I', 'C', 'C', '_', 'P', 'R', 'O', 'F',
                                'I', 'L', 'E', 0U});
    const auto profiled = inspect_texture_source(icc);
    require(profiled && profiled.value().has_color_encoding_metadata &&
                !decode_texture_image_rgba8(icc, sha256(icc), 1U, 1U),
            "post-scan color metadata remains identified and decode refuses it");
    TextureSourceLimits limits;
    limits.max_source_bytes = source.size() - 1U;
    require(!inspect_texture_source(source, "image/jpeg", limits) &&
                !decode_texture_image_rgba8(source, sha256(source), 1U, 1U, limits),
            "source byte limit is enforced by inspection and decode");

    const auto png = decodable_png_fixture();
    const auto png_decoded = decode_texture_image_rgba8(png, sha256(png), 1U, 1U);
#ifdef _WIN32
    require(png_decoded && png_decoded.value().admission.source.digest == sha256(png) &&
                png_decoded.value().pixels == std::vector<std::uint8_t>({255U, 0U, 0U, 255U}),
            "valid PNG still decodes exact source-bound pixels");
#else
    require(!png_decoded, "non-Windows PNG decode remains explicitly unsupported");
#endif
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

    auto malformed_decoded = decoded.value();
    malformed_decoded.pixels.pop_back();
    require(!malformed_decoded.validate(),
            "decoded RGBA8 evidence rejects a mismatched byte count");
    require(!decode_tga_rgba8(png_fixture()),
            "TGA decoder rejects a different admitted container format");
}

void tga_color_metadata_is_exposed_and_not_silently_decoded() {
    using namespace carto::assets;
    const auto descriptive = inspect_texture_source(
        tga_fixture_with_extension(false), "image/x-tga");
    require(descriptive && !descriptive.value().has_color_encoding_metadata,
        "TGA 2.0 descriptive extension without gamma or correction remains ordinary");

    const auto gamma_bytes = tga_fixture_with_extension(true);
    const auto gamma = inspect_texture_source(gamma_bytes, "image/x-tga");
    require(gamma && gamma.value().has_color_encoding_metadata,
        "TGA 2.0 gamma metadata is surfaced for caller policy");
    const auto decoded = decode_tga_rgba8(gamma_bytes);
    require(!decoded && decoded.error().code == carto::core::ErrorCode::unsupported,
        "TGA pixel decode refuses gamma metadata it cannot transform");
    const auto generic_decoded = decode_texture_image_rgba8(
        gamma_bytes, gamma.value().source.digest, 2U, 1U);
    require(!generic_decoded &&
            generic_decoded.error().code == carto::core::ErrorCode::unsupported,
        "shared RGBA8 decoder refuses TGA gamma metadata before texture use");

    auto invalid_offset = gamma_bytes;
    const std::size_t footer_offset = invalid_offset.size() - 26U;
    invalid_offset[footer_offset] = 1U;
    invalid_offset[footer_offset + 1U] = 0U;
    invalid_offset[footer_offset + 2U] = 0U;
    invalid_offset[footer_offset + 3U] = 0U;
    require(!inspect_texture_source(invalid_offset, "image/x-tga"),
        "TGA 2.0 extension area cannot overlap the image payload");
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
    png_chunk_semantics_fail_closed_and_keep_legal_metadata();
    jpeg_marker_walk_fails_closed_and_preserves_binding();
    tga_decode_advances_evidence_and_canonicalizes_orientation();
    tga_color_metadata_is_exposed_and_not_silently_decoded();
    mip_generation_is_semantic_and_color_space_correct();
    mip_generation_preserves_normals_alpha_and_odd_edges();
    malformed_mip_chains_fail_closed();
    std::cout << "texture source tests passed\n";
    return EXIT_SUCCESS;
}
