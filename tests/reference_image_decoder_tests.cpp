#include "reference_image_decoder.hpp"

#include <carto/assets/texture_source.hpp>
#include <carto/assets/texture_image_decoder.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
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

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
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
    append_be32(bytes, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crc_begin = bytes.size();
    bytes.insert(bytes.end(), type.begin(), type.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    append_be32(bytes, crc32(std::span<const std::uint8_t>{bytes}.subspan(crc_begin)));
}

std::vector<std::uint8_t> png_fixture(bool valid_deflate = true,
    std::size_t split_at = 0U, bool interleaved_text = false,
    bool trailing_text = false, bool include_srgb = false) {
    std::vector<std::uint8_t> bytes{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    constexpr std::array<std::uint8_t, 13U> header{
        0U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 8U, 6U, 0U, 0U, 0U};
    append_png_chunk(bytes, "IHDR", header);
    constexpr std::array<std::uint8_t, 1U> srgb_intent{0U};
    if (include_srgb) append_png_chunk(bytes, "sRGB", srgb_intent);

    std::vector<std::uint8_t> compressed{0x78U, 0x01U};
    if (valid_deflate) {
        constexpr std::array<std::uint8_t, 5U> scanline{0U, 0x12U, 0x34U, 0x56U, 0xffU};
        compressed.insert(compressed.end(), {0x01U, 0x05U, 0U, 0xfaU, 0xffU});
        compressed.insert(compressed.end(), scanline.begin(), scanline.end());
        std::uint32_t first = 1U;
        std::uint32_t second = 0U;
        for (const auto byte : scanline) {
            first = (first + byte) % 65521U;
            second = (second + first) % 65521U;
        }
        append_be32(compressed, (second << 16U) | first);
    } else {
        // BTYPE=3 is reserved by DEFLATE, while the PNG chunk framing remains valid.
        compressed.insert(compressed.end(), {0x07U, 0U, 0U, 0xffU, 0xffU});
    }
    constexpr std::array<std::uint8_t, 3U> text{'n', 0U, 'v'};
    if (split_at != 0U) {
        require(split_at < compressed.size(), "split image-data fixture is bounded");
        append_png_chunk(bytes, "IDAT", std::span<const std::uint8_t>{compressed}.first(split_at));
        if (interleaved_text) append_png_chunk(bytes, "tEXt", text);
        append_png_chunk(bytes, "IDAT", std::span<const std::uint8_t>{compressed}.subspan(split_at));
    } else {
        append_png_chunk(bytes, "IDAT", compressed);
    }
    if (trailing_text) append_png_chunk(bytes, "tEXt", text);
    append_png_chunk(bytes, "IEND", std::span<const std::uint8_t>{});
    return bytes;
}

std::vector<std::uint8_t> png_with_zlib_payload(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> bytes{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    constexpr std::array<std::uint8_t, 13U> header{
        0U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 8U, 6U, 0U, 0U, 0U};
    append_png_chunk(bytes, "IHDR", header);
    append_png_chunk(bytes, "IDAT", payload);
    append_png_chunk(bytes, "IEND", std::span<const std::uint8_t>{});
    return bytes;
}

void append_adler32(std::vector<std::uint8_t>& bytes,
                    std::span<const std::uint8_t> payload) {
    std::uint32_t first = 1U;
    std::uint32_t second = 0U;
    for (const auto byte : payload) {
        first = (first + byte) % 65521U;
        second = (second + first) % 65521U;
    }
    append_be32(bytes, (second << 16U) | first);
}

std::vector<std::uint8_t> stored_zlib_stream(std::span<const std::uint8_t> payload) {
    require(payload.size() <= 65'535U, "stored DEFLATE fixture fits one block");
    const auto length = static_cast<std::uint16_t>(payload.size());
    const auto inverse = static_cast<std::uint16_t>(~length);
    std::vector<std::uint8_t> bytes{0x78U, 0x01U, 0x01U,
        static_cast<std::uint8_t>(length & 0xffU),
        static_cast<std::uint8_t>(length >> 8U),
        static_cast<std::uint8_t>(inverse & 0xffU),
        static_cast<std::uint8_t>(inverse >> 8U)};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    append_adler32(bytes, payload);
    return bytes;
}

class DeflateBitWriter final {
public:
    explicit DeflateBitWriter(std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    void write_bits(std::uint32_t value, unsigned count) {
        for (unsigned index = 0U; index < count; ++index) {
            current_byte_ |= static_cast<std::uint8_t>(
                ((value >> index) & 1U) << bits_in_byte_);
            if (++bits_in_byte_ == 8U) flush_byte();
        }
    }

    void write_huffman_code(std::uint32_t code, unsigned count) {
        for (unsigned index = count; index != 0U; --index) {
            write_bits((code >> (index - 1U)) & 1U, 1U);
        }
    }

    void finish() {
        if (bits_in_byte_ != 0U) flush_byte();
    }

private:
    void flush_byte() {
        bytes_.push_back(current_byte_);
        current_byte_ = 0U;
        bits_in_byte_ = 0U;
    }

    std::vector<std::uint8_t>& bytes_;
    std::uint8_t current_byte_ = 0U;
    unsigned bits_in_byte_ = 0U;
};

void write_fixed_literal(DeflateBitWriter& bits, std::uint16_t symbol) {
    if (symbol <= 143U) {
        bits.write_huffman_code(0x30U + symbol, 8U);
    } else if (symbol <= 255U) {
        bits.write_huffman_code(0x190U + symbol - 144U, 9U);
    } else if (symbol <= 279U) {
        bits.write_huffman_code(symbol - 256U, 7U);
    } else {
        bits.write_huffman_code(0xc0U + symbol - 280U, 8U);
    }
}

std::vector<std::uint8_t> fixed_huffman_zlib_stream(
    std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> bytes{0x78U, 0x01U};
    DeflateBitWriter bits(bytes);
    bits.write_bits(1U, 1U);
    bits.write_bits(1U, 2U);
    for (const auto byte : payload) write_fixed_literal(bits, byte);
    write_fixed_literal(bits, 256U);
    bits.finish();
    append_adler32(bytes, payload);
    return bytes;
}

std::vector<std::uint8_t> fixed_huffman_backreference_zlib_stream() {
    constexpr std::array<std::uint8_t, 5U> payload{0U, 0U, 0U, 0U, 0U};
    std::vector<std::uint8_t> bytes{0x78U, 0x01U};
    DeflateBitWriter bits(bytes);
    bits.write_bits(1U, 1U);
    bits.write_bits(1U, 2U);
    write_fixed_literal(bits, 0U);
    write_fixed_literal(bits, 258U); // length 4
    bits.write_huffman_code(0U, 5U); // distance 1
    write_fixed_literal(bits, 256U);
    bits.finish();
    append_adler32(bytes, payload);
    return bytes;
}

std::vector<std::uint8_t> incomplete_dynamic_huffman_zlib_stream(
    std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, 257U> literal_lengths{};
    for (const auto byte : payload) literal_lengths[byte] = 3U;
    literal_lengths[256U] = 3U;

    std::array<std::uint8_t, 258U> combined_lengths{};
    std::copy(literal_lengths.begin(), literal_lengths.end(), combined_lengths.begin());
    combined_lengths[257U] = 1U;

    constexpr std::array<std::uint8_t, 19U> code_order{
        16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U,
        11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U};
    std::array<std::uint8_t, 19U> code_lengths{};
    for (const auto symbol : {0U, 1U, 3U, 18U}) code_lengths[symbol] = 2U;

    std::vector<std::uint8_t> bytes{0x78U, 0x01U};
    DeflateBitWriter bits(bytes);
    bits.write_bits(1U, 1U);
    bits.write_bits(2U, 2U);
    bits.write_bits(0U, 5U);
    bits.write_bits(0U, 5U);
    bits.write_bits(14U, 4U);
    for (std::size_t index = 0U; index < 18U; ++index) {
        bits.write_bits(code_lengths[code_order[index]], 3U);
    }

    // The code-length tree has four two-bit codes: 0=00, 1=01, 3=10, 18=11.
    for (std::size_t index = 0U; index < combined_lengths.size();) {
        const std::uint8_t length = combined_lengths[index];
        if (length != 0U) {
            const std::uint32_t code = length == 1U ? 1U : 2U;
            bits.write_huffman_code(code, 2U);
            ++index;
            continue;
        }
        std::size_t run = 1U;
        while (index + run < combined_lengths.size() &&
            combined_lengths[index + run] == 0U) ++run;
        while (run >= 11U) {
            const std::size_t repeated = std::min<std::size_t>(run, 138U);
            bits.write_huffman_code(3U, 2U);
            bits.write_bits(static_cast<std::uint32_t>(repeated - 11U), 7U);
            index += repeated;
            run -= repeated;
        }
        while (run != 0U) {
            bits.write_huffman_code(0U, 2U);
            ++index;
            --run;
        }
    }

    std::array<std::uint32_t, 257U> literal_codes{};
    std::uint32_t next_code = 0U;
    for (std::size_t symbol = 0U; symbol < literal_lengths.size(); ++symbol) {
        if (literal_lengths[symbol] == 3U) literal_codes[symbol] = next_code++;
    }
    for (const auto byte : payload) bits.write_huffman_code(literal_codes[byte], 3U);
    bits.write_huffman_code(literal_codes[256U], 3U);
    bits.finish();
    append_adler32(bytes, payload);
    return bytes;
}

std::vector<std::uint8_t> incomplete_dynamic_huffman_backreference_zlib_stream() {
    constexpr std::array<std::uint8_t, 5U> payload{0U, 0U, 0U, 0U, 0U};
    std::array<std::uint8_t, 259U> literal_lengths{};
    literal_lengths[0U] = 2U;
    literal_lengths[256U] = 2U;
    literal_lengths[258U] = 2U;
    std::array<std::uint8_t, 260U> combined_lengths{};
    std::copy(literal_lengths.begin(), literal_lengths.end(), combined_lengths.begin());
    combined_lengths[259U] = 1U;

    constexpr std::array<std::uint8_t, 19U> code_order{
        16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U,
        11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U};
    std::array<std::uint8_t, 19U> code_lengths{};
    for (const auto symbol : {0U, 1U, 2U, 18U}) code_lengths[symbol] = 2U;

    std::vector<std::uint8_t> bytes{0x78U, 0x01U};
    DeflateBitWriter bits(bytes);
    bits.write_bits(1U, 1U);
    bits.write_bits(2U, 2U);
    bits.write_bits(2U, 5U); // 259 literal/length symbols
    bits.write_bits(0U, 5U); // one distance symbol
    bits.write_bits(14U, 4U); // 18 code-length-code lengths
    for (std::size_t index = 0U; index < 18U; ++index) {
        bits.write_bits(code_lengths[code_order[index]], 3U);
    }

    // The code-length tree has four two-bit codes: 0=00, 1=01, 2=10, 18=11.
    for (std::size_t index = 0U; index < combined_lengths.size();) {
        const std::uint8_t length = combined_lengths[index];
        if (length != 0U) {
            bits.write_huffman_code(length == 1U ? 1U : 2U, 2U);
            ++index;
            continue;
        }
        std::size_t run = 1U;
        while (index + run < combined_lengths.size() &&
            combined_lengths[index + run] == 0U) ++run;
        while (run >= 11U) {
            const std::size_t repeated = std::min<std::size_t>(run, 138U);
            bits.write_huffman_code(3U, 2U);
            bits.write_bits(static_cast<std::uint32_t>(repeated - 11U), 7U);
            index += repeated;
            run -= repeated;
        }
        while (run != 0U) {
            bits.write_huffman_code(0U, 2U);
            ++index;
            --run;
        }
    }

    // Literal/length codes 0, 256, and 258 canonically map to 00, 01, 10.
    bits.write_huffman_code(0U, 2U);
    bits.write_huffman_code(2U, 2U); // symbol 258: length 4
    bits.write_huffman_code(0U, 1U); // distance symbol 0: distance 1
    bits.write_huffman_code(1U, 2U); // end-of-block symbol 256
    bits.finish();
    append_adler32(bytes, payload);
    return bytes;
}

std::vector<std::uint8_t> jpeg_fixture_with_icc_profile() {
    std::vector<std::uint8_t> bytes{0xffU, 0xd8U};
    constexpr std::array<std::uint8_t, 12U> signature{
        'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0U};
    bytes.insert(bytes.end(), {0xffU, 0xe2U, 0x00U, 0x10U});
    bytes.insert(bytes.end(), signature.begin(), signature.end());
    bytes.insert(bytes.end(), {1U, 1U});
    bytes.insert(bytes.end(), {
        0xffU, 0xc0U, 0x00U, 0x11U,
        0x08U, 0x00U, 0x01U, 0x00U, 0x02U, 0x03U,
        0x01U, 0x11U, 0x00U,
        0x02U, 0x11U, 0x00U,
        0x03U, 0x11U, 0x00U,
        0xffU, 0xdaU, 0x00U, 0x0cU,
        0x03U, 0x01U, 0x00U, 0x02U, 0x11U, 0x03U, 0x11U,
        0x00U, 0x3fU, 0x00U, 0x00U,
        0xffU, 0xd9U});
    return bytes;
}

std::vector<std::uint8_t> jpeg_fixture_with_post_scan_icc_profile() {
    auto bytes = jpeg_fixture_with_icc_profile();
    const std::vector<std::uint8_t> icc_segment(bytes.begin() + 2, bytes.begin() + 20);
    bytes.erase(bytes.begin() + 2, bytes.begin() + 20);
    bytes.insert(bytes.end() - 2, icc_segment.begin(), icc_segment.end());
    return bytes;
}

std::vector<std::uint8_t> jpeg_fixture_with_exif_color_space(
    bool include_color_space,
    bool after_scan = false,
    bool include_gamma = false,
    bool big_endian = false) {
    auto bytes = jpeg_fixture_with_icc_profile();
    bytes.erase(bytes.begin() + 2, bytes.begin() + 20);
    std::vector<std::uint8_t> payload{'E', 'x', 'i', 'f', 0U, 0U};
    const auto append_tiff_u16 = [&payload, big_endian](std::uint16_t value) {
        if (big_endian) {
            payload.push_back(static_cast<std::uint8_t>(value >> 8U));
            payload.push_back(static_cast<std::uint8_t>(value & 0xffU));
        } else {
            payload.push_back(static_cast<std::uint8_t>(value & 0xffU));
            payload.push_back(static_cast<std::uint8_t>(value >> 8U));
        }
    };
    const auto append_tiff_u32 = [&payload, big_endian](std::uint32_t value) {
        for (unsigned index = 0U; index < 4U; ++index) {
            const unsigned shift = big_endian ? (3U - index) * 8U : index * 8U;
            payload.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    };
    payload.push_back(big_endian ? 'M' : 'I');
    payload.push_back(big_endian ? 'M' : 'I');
    append_tiff_u16(42U);
    append_tiff_u32(8U);
    append_tiff_u16(1U);
    append_tiff_u16(0x8769U);
    append_tiff_u16(4U);
    append_tiff_u32(1U);
    append_tiff_u32(26U);
    append_tiff_u32(0U);
    const std::uint16_t exif_entry_count =
        static_cast<std::uint16_t>((include_color_space ? 1U : 0U) +
            (include_gamma ? 1U : 0U));
    append_tiff_u16(exif_entry_count);
    if (include_color_space) {
        append_tiff_u16(0xa001U);
        append_tiff_u16(3U);
        append_tiff_u32(1U);
        append_tiff_u16(1U);
        append_tiff_u16(0U);
    }
    if (include_gamma) {
        const std::uint32_t gamma_offset = static_cast<std::uint32_t>(
            payload.size() + 12U + 4U - 6U);
        append_tiff_u16(0xa500U);
        append_tiff_u16(5U);
        append_tiff_u32(1U);
        append_tiff_u32(gamma_offset);
    }
    append_tiff_u32(0U);
    if (include_gamma) {
        append_tiff_u32(22U);
        append_tiff_u32(10U);
    }
    const auto segment_length = static_cast<std::uint16_t>(payload.size() + 2U);
    std::vector<std::uint8_t> segment{
        0xffU, 0xe1U,
        static_cast<std::uint8_t>(segment_length >> 8U),
        static_cast<std::uint8_t>(segment_length & 0xffU)};
    segment.insert(segment.end(), payload.begin(), payload.end());
    const auto insertion = after_scan ? bytes.end() - 2 : bytes.begin() + 2;
    bytes.insert(insertion, segment.begin(), segment.end());
    return bytes;
}

void png_pixels_decode_to_digest_bound_rgba8() {
    using namespace carto::assets;
    auto encoded = png_fixture();
    const auto admitted = inspect_texture_source(encoded, "image/png");
    require(static_cast<bool>(admitted), "valid PNG fixture passes source admission");

    const auto decoded = carto::desktop::decode_reference_image_pixels(
        encoded, admitted.value().source.digest, 1U, 1U,
        TextureSourceLimits{.max_pixels = 16'777'216ULL});
    require(decoded && decoded.value().validate(),
        "valid PNG is decoded as bounded RGBA8 pixels");
    require(decoded.value().admission.format == TextureSourceFormat::png &&
            decoded.value().admission.source.digest == admitted.value().source.digest &&
            decoded.value().pixels == std::vector<std::uint8_t>{0x12U, 0x34U, 0x56U, 0xffU},
        "decoded pixels preserve source identity and channel order");
}

void stale_bindings_and_bad_pixel_streams_fail_closed() {
    using namespace carto::assets;
    auto encoded = png_fixture();
    const auto admitted = inspect_texture_source(encoded, "image/png");
    require(static_cast<bool>(admitted), "adversarial PNG fixture passes container admission");

    require(!carto::desktop::decode_reference_image_pixels(
            encoded, Sha256Digest{}, 1U, 1U),
        "stale source digest blocks pixel decode");
    require(!carto::desktop::decode_reference_image_pixels(
            encoded, admitted.value().source.digest, 2U, 1U),
        "changed plane dimensions block pixel decode");

    auto malformed = png_fixture(false);
    const auto malformed_admission = inspect_texture_source(malformed, "image/png");
    require(static_cast<bool>(malformed_admission),
        "malformed pixel stream can still have valid bounded container framing");
    require(!carto::desktop::decode_reference_image_pixels(
            std::move(malformed), malformed_admission.value().source.digest, 1U, 1U),
        "invalid compressed pixel stream is refused without fallback pixels");
}

void png_deflate_stream_is_verified_before_codec_decode() {
    using namespace carto::assets;
    constexpr std::array<std::uint8_t, 5U> scanline{
        0U, 0x12U, 0x34U, 0x56U, 0xffU};
    const auto decode_zlib = [](std::vector<std::uint8_t> zlib) {
        auto encoded = png_with_zlib_payload(zlib);
        const auto admitted = inspect_texture_source(encoded, "image/png");
        require(static_cast<bool>(admitted),
            "DEFLATE adversarial fixture retains valid PNG chunk framing");
        return carto::desktop::decode_reference_image_pixels(
            std::move(encoded), admitted.value().source.digest, 1U, 1U);
    };
    const auto require_refused = [&decode_zlib](
        std::vector<std::uint8_t> zlib, const char* message) {
        require(!decode_zlib(std::move(zlib)), message);
    };

    for (const auto& zlib : {
            stored_zlib_stream(scanline),
            fixed_huffman_zlib_stream(scanline),
            std::vector<std::uint8_t>{
                0x78U, 0x01U, 0x05U, 0xc0U, 0x81U, 0x09U, 0x00U, 0x00U, 0x00U, 0x83U,
                0xa0U, 0x6eU, 0xe8U, 0x96U, 0xddU, 0xb2U, 0xffU, 0x3fU, 0xd9U, 0xc4U,
                0x74U, 0x07U, 0x02U, 0x94U, 0x01U, 0x9cU},
            std::vector<std::uint8_t>{
                0x78U, 0x01U, 0x04U, 0xc0U, 0x01U, 0x05U, 0x00U, 0x00U, 0x00U, 0x00U,
                0xa0U, 0xffU, 0xafU, 0x4bU, 0x00U, 0x05U, 0x00U, 0xfaU, 0xffU, 0x00U,
                0x12U, 0x34U, 0x56U, 0xffU, 0x02U, 0x94U, 0x01U, 0x9cU}}) {
        const auto decoded = decode_zlib(zlib);
        if (!decoded) {
            std::cerr << "DEFLATE fixture block kind " << ((zlib[2U] >> 1U) & 3U)
                      << ": " << decoded.error().message << '\n';
        }
        if (decoded && decoded.value().pixels !=
                std::vector<std::uint8_t>{0x12U, 0x34U, 0x56U, 0xffU}) {
            std::cerr << "DEFLATE fixture block kind " << ((zlib[2U] >> 1U) & 3U) << " pixels:";
            for (const auto byte : decoded.value().pixels) std::cerr << ' ' << static_cast<unsigned>(byte);
            std::cerr << '\n';
        }
        require(decoded && decoded.value().pixels ==
                std::vector<std::uint8_t>{0x12U, 0x34U, 0x56U, 0xffU},
            "stored, fixed, dynamic and legal one-symbol blocks preserve exact admitted pixels");
    }

    const std::vector<std::uint8_t> black_pixel{0U, 0U, 0U, 0U};
    const auto fixed_backreference = decode_zlib(
        fixed_huffman_backreference_zlib_stream());
    require(fixed_backreference && fixed_backreference.value().pixels == black_pixel,
        "fixed-Huffman length-distance pairs expand within the bounded history");
    // Complete literal trees independently checked with stdlib zlib; keep
    // the original incomplete-tree generators below as refusal fixtures.
    const auto dynamic_backreference = decode_zlib(
        std::vector<std::uint8_t>{
                0x78U, 0x01U, 0x15U, 0xc0U, 0x01U, 0x09U, 0x00U, 0x00U, 0x00U, 0x80U,
                0x20U, 0xffU, 0xafU, 0x8eU, 0xb4U, 0x00U, 0x00U, 0x05U, 0x00U, 0x01U});
    require(dynamic_backreference && dynamic_backreference.value().pixels == black_pixel,
        "dynamic-Huffman length-distance pairs expand within the bounded history");

    require_refused(incomplete_dynamic_huffman_zlib_stream(scanline),
        "incomplete multi-symbol dynamic literal tree is refused without codec fallback");
    require_refused(incomplete_dynamic_huffman_backreference_zlib_stream(),
        "incomplete dynamic backreference literal tree is refused without codec fallback");

    auto truncated_block = stored_zlib_stream(scanline);
    truncated_block.resize(7U + scanline.size() - 1U);
    require_refused(std::move(truncated_block),
        "truncated DEFLATE block is refused before codec recovery");

    auto truncated_trailer = stored_zlib_stream(scanline);
    truncated_trailer.pop_back();
    require_refused(std::move(truncated_trailer),
        "truncated zlib checksum is refused before codec recovery");

    auto bad_adler = stored_zlib_stream(scanline);
    bad_adler.back() ^= 1U;
    require_refused(std::move(bad_adler),
        "incorrect zlib Adler-32 is refused before codec recovery");

    auto trailing_data = stored_zlib_stream(scanline);
    trailing_data.push_back(0U);
    require_refused(std::move(trailing_data),
        "bytes after the zlib trailer are refused");

    constexpr std::array<std::uint8_t, 4U> short_scanline{
        0U, 0x12U, 0x34U, 0x56U};
    require_refused(stored_zlib_stream(short_scanline),
        "short decompressed scanline is refused");

    constexpr std::array<std::uint8_t, 6U> long_scanline{
        0U, 0x12U, 0x34U, 0x56U, 0xffU, 0U};
    require_refused(stored_zlib_stream(long_scanline),
        "overlong decompressed scanline is refused");

    constexpr std::array<std::uint8_t, 5U> invalid_filter_scanline{
        5U, 0x12U, 0x34U, 0x56U, 0xffU};
    require_refused(stored_zlib_stream(invalid_filter_scanline),
        "reserved PNG scanline filter is refused");
}

void image_data_ordering_is_checked_after_the_complete_prefix() {
    using namespace carto::assets;
    for (const auto split_at : {1U, 2U, 3U, 8U}) {
        const auto valid = png_fixture(true, split_at, false, true);
        const auto admitted = inspect_texture_source(valid, "image/png");
        require(static_cast<bool>(admitted), "split PNG fixture has valid container framing");
        const auto decoded = carto::desktop::decode_reference_image_pixels(
            valid, admitted.value().source.digest, 1U, 1U);
        require(decoded && decoded.value().pixels ==
                std::vector<std::uint8_t>{0x12U, 0x34U, 0x56U, 0xffU},
            "contiguous image-data chunks and legal trailing metadata preserve exact pixels");

        const auto interleaved = png_fixture(true, split_at, true);
        const auto interleaved_source = inspect_texture_source(interleaved, "image/png");
        require(!interleaved_source && interleaved_source.error().message.find("noncontiguous") !=
                std::string::npos, "source inspection refuses noncontiguous image-data chunks");
        const auto digest = sha256(std::span<const std::uint8_t>{interleaved});
        const auto refused = carto::desktop::decode_reference_image_pixels(
            interleaved, digest, 1U, 1U);
        require(!refused && refused.error().code == interleaved_source.error().code &&
                refused.error().message == interleaved_source.error().message,
            "reference preview preserves source ordering refusal before codec recovery");
        const auto generic_refused = decode_texture_image_rgba8(interleaved, digest, 1U, 1U);
        require(!generic_refused && generic_refused.error().code == interleaved_source.error().code &&
                generic_refused.error().message == interleaved_source.error().message,
            "shared material decoder preserves source ordering refusal before codec recovery");
    }
}

void reference_preview_refuses_unhandled_color_encoding_metadata() {
    using namespace carto::assets;
    auto png = png_fixture(true, 0U, false, false, true);
    const auto png_admission = inspect_texture_source(png, "image/png");
    require(png_admission && png_admission.value().has_color_encoding_metadata,
        "PNG sRGB metadata is visible in source admission");
    const auto png_refused = carto::desktop::decode_reference_image_pixels(
        png, png_admission.value().source.digest, 1U, 1U);
    require(!png_refused && png_refused.error().code == carto::core::ErrorCode::unsupported,
        "reference preview refuses color metadata it does not convert");
    const auto generic_png_refused = decode_texture_image_rgba8(
        png, png_admission.value().source.digest, 1U, 1U);
    require(!generic_png_refused &&
            generic_png_refused.error().code == carto::core::ErrorCode::unsupported,
        "shared texture decoder refuses PNG metadata before saved-material decode");
    const auto generic_stale = decode_texture_image_rgba8(
        png, Sha256Digest{}, 1U, 1U);
    require(!generic_stale &&
            generic_stale.error().code == carto::core::ErrorCode::stale_data,
        "shared texture decoder checks source binding before metadata policy");

    auto jpeg = jpeg_fixture_with_icc_profile();
    const auto jpeg_admission = inspect_texture_source(jpeg, "image/jpeg");
    require(jpeg_admission && jpeg_admission.value().has_color_encoding_metadata,
        "JPEG ICC APP2 metadata is visible in source admission");
    const auto jpeg_refused = carto::desktop::decode_reference_image_pixels(
        jpeg, jpeg_admission.value().source.digest, 2U, 1U);
    require(!jpeg_refused && jpeg_refused.error().code == carto::core::ErrorCode::unsupported,
        "reference preview refuses embedded JPEG ICC profiles");
    const auto generic_jpeg_refused = decode_texture_image_rgba8(
        jpeg, jpeg_admission.value().source.digest, 2U, 1U);
    require(!generic_jpeg_refused &&
            generic_jpeg_refused.error().code == carto::core::ErrorCode::unsupported,
        "shared texture decoder refuses JPEG profiles before saved-material decode");

    const auto post_scan_jpeg = jpeg_fixture_with_post_scan_icc_profile();
    const auto post_scan_admission = inspect_texture_source(post_scan_jpeg, "image/jpeg");
    require(post_scan_admission && post_scan_admission.value().has_color_encoding_metadata,
        "JPEG ICC APP2 metadata between scan data and EOI is surfaced");
    const auto post_scan_refused = carto::desktop::decode_reference_image_pixels(
        post_scan_jpeg, post_scan_admission.value().source.digest, 2U, 1U);
    require(!post_scan_refused &&
            post_scan_refused.error().code == carto::core::ErrorCode::unsupported,
        "post-scan JPEG ICC profile is refused before codec pixel conversion");

    auto nested_marker_jpeg = jpeg_fixture_with_icc_profile();
    nested_marker_jpeg.erase(nested_marker_jpeg.begin() + 2,
        nested_marker_jpeg.begin() + 20);
    nested_marker_jpeg.erase(nested_marker_jpeg.end() - 2, nested_marker_jpeg.end());
    std::vector<std::uint8_t> nested_icc_bytes{
        0xffU, 0xe2U, 0x00U, 0x10U};
    constexpr std::array<std::uint8_t, 12U> nested_icc_signature{
        'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0U};
    nested_icc_bytes.insert(nested_icc_bytes.end(), nested_icc_signature.begin(),
        nested_icc_signature.end());
    nested_icc_bytes.insert(nested_icc_bytes.end(), {1U, 1U});
    const auto app1_length = static_cast<std::uint8_t>(nested_icc_bytes.size() + 2U);
    nested_marker_jpeg.insert(nested_marker_jpeg.end(),
        {0xffU, 0xe1U, 0x00U, app1_length});
    nested_marker_jpeg.insert(nested_marker_jpeg.end(),
        nested_icc_bytes.begin(), nested_icc_bytes.end());
    nested_marker_jpeg.insert(nested_marker_jpeg.end(), {0xffU, 0xd9U});
    const auto nested_admission = inspect_texture_source(nested_marker_jpeg, "image/jpeg");
    require(nested_admission && !nested_admission.value().has_color_encoding_metadata,
        "post-scan marker parsing does not treat ICC-like bytes inside another segment as metadata");

    const auto exif_color_space = jpeg_fixture_with_exif_color_space(true);
    const auto exif_admission = inspect_texture_source(exif_color_space, "image/jpeg");
    require(exif_admission && exif_admission.value().has_color_encoding_metadata,
        "JPEG EXIF ColorSpace in the Exif IFD is surfaced for caller policy");
    const auto exif_refused = carto::desktop::decode_reference_image_pixels(
        exif_color_space, exif_admission.value().source.digest, 2U, 1U);
    require(!exif_refused && exif_refused.error().code == carto::core::ErrorCode::unsupported,
        "reference preview refuses EXIF color-space metadata it does not transform");
    const auto exif_generic_refused = decode_texture_image_rgba8(
        exif_color_space, exif_admission.value().source.digest, 2U, 1U);
    require(!exif_generic_refused &&
            exif_generic_refused.error().code == carto::core::ErrorCode::unsupported,
        "shared texture decoder refuses EXIF color metadata before saved-material use");

    const auto exif_without_color_space = jpeg_fixture_with_exif_color_space(false);
    const auto exif_without_color_admission = inspect_texture_source(
        exif_without_color_space, "image/jpeg");
    require(exif_without_color_admission &&
            !exif_without_color_admission.value().has_color_encoding_metadata,
        "ordinary EXIF attributes do not imply an unsupported color transform");

    const auto exif_gamma = jpeg_fixture_with_exif_color_space(false, false, true);
    const auto exif_gamma_admission = inspect_texture_source(exif_gamma, "image/jpeg");
    require(exif_gamma_admission &&
            exif_gamma_admission.value().has_color_encoding_metadata,
        "JPEG EXIF Gamma in the Exif IFD is surfaced for caller policy");

    const auto big_endian_exif = jpeg_fixture_with_exif_color_space(
        true, false, false, true);
    const auto big_endian_exif_admission = inspect_texture_source(
        big_endian_exif, "image/jpeg");
    require(big_endian_exif_admission &&
            big_endian_exif_admission.value().has_color_encoding_metadata,
        "big-endian EXIF TIFF tags are parsed without losing color metadata");

    const auto post_scan_exif = jpeg_fixture_with_exif_color_space(true, true);
    const auto post_scan_exif_admission = inspect_texture_source(post_scan_exif, "image/jpeg");
    require(post_scan_exif_admission &&
            post_scan_exif_admission.value().has_color_encoding_metadata,
        "JPEG EXIF color metadata between scans is surfaced");

    auto malformed_exif = exif_color_space;
    malformed_exif[30U] = 0U;
    malformed_exif[31U] = 0U;
    malformed_exif[32U] = 0U;
    malformed_exif[33U] = 0U;
    require(!inspect_texture_source(malformed_exif, "image/jpeg"),
        "malformed EXIF IFD pointers cannot hide color metadata from admission");

    const auto stale = carto::desktop::decode_reference_image_pixels(
        png, Sha256Digest{}, 1U, 1U);
    require(!stale && stale.error().code == carto::core::ErrorCode::stale_data,
        "stale source binding takes precedence over color-metadata refusal");
}

} // namespace

int main() {
    png_pixels_decode_to_digest_bound_rgba8();
    stale_bindings_and_bad_pixel_streams_fail_closed();
    png_deflate_stream_is_verified_before_codec_decode();
    image_data_ordering_is_checked_after_the_complete_prefix();
    reference_preview_refuses_unhandled_color_encoding_metadata();
    return EXIT_SUCCESS;
}
