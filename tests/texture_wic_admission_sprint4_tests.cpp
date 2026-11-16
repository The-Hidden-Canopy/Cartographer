#include <carto/assets/texture_image_decoder.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {
using namespace carto::assets;
using carto::core::ErrorCode;
using Bytes = std::vector<std::uint8_t>;
using Decode = carto::core::Result<DecodedRgba8Texture>;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

// Reuses the one-block grayscale fixture from texture_source_tests.cpp and
// the SOF2 scan layout from texture_coverage_sprint3_tests.cpp.
Bytes jpeg(std::uint8_t sof = 0xc0U, std::uint8_t precision = 8U, std::uint8_t width = 1U) {
    Bytes b{0xffU, 0xd8U, 0xffU, 0xdbU, 0U, 67U, 0U};
    b.insert(b.end(), 64U, 1U);
    b.insert(b.end(), {0xffU, sof, 0U, 11U, precision, 0U, 1U, 0U, width,
        1U, 1U, 0x11U, 0U, 0xffU, 0xc4U, 0U, 38U,
        0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0x10U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U,
        static_cast<std::uint8_t>(sof == 0xc2U ? 0U : 63U), 0U});
    if (sof == 0xc2U) {
        b.insert(b.end(), {0x7fU, 0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 1U, 63U, 0U, 0x7fU});
    } else {
        b.push_back(0x3fU);
    }
    b.insert(b.end(), {0xffU, 0xd9U});
    return b;
}

void add_app(Bytes& b, std::uint8_t marker, std::span<const std::uint8_t> payload,
             bool after_scan = false) {
    const unsigned size = static_cast<unsigned>(payload.size()) + 2U;
    Bytes segment{0xffU, marker, static_cast<std::uint8_t>(size >> 8U), static_cast<std::uint8_t>(size)};
    segment.insert(segment.end(), payload.begin(), payload.end());
    b.insert(after_scan ? b.end() - 2 : b.begin() + 2, segment.begin(), segment.end());
}

Bytes exif_orientation(std::uint8_t value) {
    return {'E','x','i','f',0U,0U, 'I','I',42U,0U,8U,0U,0U,0U,
        1U,0U, 0x12U,0x01U,3U,0U,1U,0U,0U,0U,value,0U,0U,0U, 0U,0U,0U,0U};
}

Bytes exif_color_space() {
    return {'E','x','i','f',0U,0U, 'I','I',42U,0U,8U,0U,0U,0U,
        1U,0U, 0x69U,0x87U,4U,0U,1U,0U,0U,0U,26U,0U,0U,0U, 0U,0U,0U,0U,
        1U,0U, 0x01U,0xa0U,3U,0U,1U,0U,0U,0U,1U,0U,0U,0U, 0U,0U,0U,0U};
}

std::uint32_t crc32(std::span<const std::uint8_t> b) {
    std::uint32_t crc = 0xffffffffU;
    for (auto x : b) { crc ^= x; for (unsigned i = 0; i < 8U; ++i)
        crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U))); }
    return ~crc;
}
void be32(Bytes& b, std::uint32_t x) {
    b.insert(b.end(), {static_cast<std::uint8_t>(x >> 24U), static_cast<std::uint8_t>(x >> 16U),
        static_cast<std::uint8_t>(x >> 8U), static_cast<std::uint8_t>(x)});
}
void chunk(Bytes& b, std::string_view type, std::span<const std::uint8_t> payload) {
    be32(b, static_cast<std::uint32_t>(payload.size()));
    const auto start = b.size();
    b.insert(b.end(), type.begin(), type.end()); b.insert(b.end(), payload.begin(), payload.end());
    be32(b, crc32(std::span<const std::uint8_t>{b}.subspan(start)));
}
Bytes png(bool embedded_srgb) {
    Bytes b{0x89U,'P','N','G',13U,10U,26U,10U};
    const Bytes ihdr{0U,0U,0U,1U,0U,0U,0U,1U,8U,6U,0U,0U,0U};
    const Bytes idat{0x78U,1U,1U,5U,0U,0xfaU,0xffU,0U,0xffU,0U,0U,0xffU,5U,0U,1U,0xffU};
    chunk(b, "IHDR", ihdr);
    if (embedded_srgb) { const Bytes intent{0U}; chunk(b, "sRGB", intent); }
    chunk(b, "IDAT", idat); chunk(b, "IEND", {});
    return b;
}

bool entropy_error(const Decode& result) {
    if (result) return false;
    const auto& ctx = result.error().context;
    return std::find(ctx.begin(), ctx.end(), "JPEG Huffman entropy admission") != ctx.end();
}

void require_binding(const Decode& result, const Bytes& source, unsigned width, unsigned height) {
    const auto inspected = inspect_texture_source(source);
    require(inspected && result && result.value().admission.source.media_type == inspected.value().source.media_type &&
        result.value().admission.source.digest == sha256(source) &&
        result.value().admission.source.bytes == source.size() &&
        result.value().admission.width == width && result.value().admission.height == height &&
        result.value().admission.stage == TextureSourceStage::decoded &&
        result.value().pixels.size() == static_cast<std::size_t>(width) * height * 4U,
        "decoded RGBA and dimensions remain bound to exact source bytes");
}

void fixtures() {
    const auto baseline = jpeg();
    const auto progressive = jpeg(0xc2U);
    const auto png_bytes = png(false);
#ifdef _WIN32
    for (const auto* b : {&baseline, &progressive}) {
        const auto result = decode_texture_image_rgba8(*b, sha256(*b), 1U, 1U);
        require_binding(result, *b, 1U, 1U);
        require(result.value().pixels == Bytes({128U,128U,128U,255U}),
                "WIC returns the expected grayscale pixel");
    }
    const auto decoded_png = decode_texture_image_rgba8(png_bytes, sha256(png_bytes), 1U, 1U);
    require_binding(decoded_png, png_bytes, 1U, 1U);
    require(decoded_png.value().pixels == Bytes({255U,0U,0U,255U}), "WIC returns exact PNG RGBA");
    for (const auto& b : {jpeg(0xc1U,8U), jpeg(0xc1U,12U), jpeg(0xc2U,12U)}) {
        const auto result = decode_texture_image_rgba8(b, sha256(b), 1U, 1U);
        require(!entropy_error(result), "variant did not report an entropy rejection");
        if (result) {
            require_binding(result, b, 1U, 1U);
            std::cout << "OBSERVE WIC accepted SOF=" << static_cast<unsigned>(b[72U])
                      << " precision=" << static_cast<unsigned>(b[75U]) << " RGBA=";
            for (auto p : result.value().pixels) std::cout << static_cast<unsigned>(p) << ',';
            std::cout << '\n';
        } else {
            std::cout << "OBSERVE decoder refused SOF=" << static_cast<unsigned>(b[72U])
                      << " precision=" << static_cast<unsigned>(b[75U]) << ": " << result.error().message << '\n';
        }
    }
#else
    require(!decode_texture_image_rgba8(baseline, sha256(baseline), 1U, 1U),
            "JPEG WIC decoding is unavailable outside Windows");
#endif
    auto invalid = baseline;
    invalid[invalid.size()-3U] = 0x80U;
    auto truncated_scan = jpeg(0xc0U, 8U, 16U); // one encoded block for two declared blocks
    auto invalid_progressive = progressive; invalid_progressive[invalid_progressive.size()-3U] = 0x80U;
    auto truncated_progressive = jpeg(0xc2U, 8U, 16U);
    require(inspect_texture_source(invalid) && inspect_texture_source(truncated_scan) &&
            inspect_texture_source(invalid_progressive) && inspect_texture_source(truncated_progressive),
            "entropy adversaries retain structurally admitted source markers");
#ifdef _WIN32
    require(entropy_error(decode_texture_image_rgba8(invalid, sha256(invalid), 1U, 1U)) &&
            entropy_error(decode_texture_image_rgba8(truncated_scan, sha256(truncated_scan), 16U, 1U)) &&
            entropy_error(decode_texture_image_rgba8(invalid_progressive, sha256(invalid_progressive), 1U, 1U)) &&
            entropy_error(decode_texture_image_rgba8(truncated_progressive, sha256(truncated_progressive), 16U, 1U)),
            "malformed and incomplete Huffman streams refuse before WIC");
#endif
    auto truncated_eoi = baseline; truncated_eoi.pop_back();
    require(!decode_texture_image_rgba8(truncated_eoi, sha256(truncated_eoi), 1U, 1U), "truncated EOI refuses");
    const auto stale_digest = decode_texture_image_rgba8(baseline, sha256(std::span<const std::uint8_t>{}), 1U, 1U);
    const auto stale_extent = decode_texture_image_rgba8(baseline, sha256(baseline), 2U, 1U);
    require(!stale_digest && stale_digest.error().code == ErrorCode::stale_data &&
            !stale_extent && stale_extent.error().code == ErrorCode::stale_data,
            "stale digest and extent are refused before pixel publication");
    TextureSourceLimits limits; limits.max_source_bytes = baseline.size() - 1U;
    require(!decode_texture_image_rgba8(baseline, sha256(baseline), 1U, 1U, limits), "byte budget refuses");
    limits.max_source_bytes = 1024U; limits.max_dimension = 1U;
    require(!decode_texture_image_rgba8(truncated_scan, sha256(truncated_scan), 16U, 1U, limits), "extent budget refuses");

    const Bytes icc{'I','C','C','_','P','R','O','F','I','L','E',0U,1U,1U};
    auto jpeg_icc = baseline; add_app(jpeg_icc, 0xe2U, icc);
    auto postscan_icc = baseline; add_app(postscan_icc, 0xe2U, icc, true);
    auto jpeg_exif_color = baseline; add_app(jpeg_exif_color, 0xe1U, exif_color_space());
    const auto png_srgb = png(true);
    for (const auto* b : std::array<const Bytes*,4U>{&jpeg_icc,&postscan_icc,&jpeg_exif_color,&png_srgb}) {
        const auto inspected = inspect_texture_source(*b);
        require(inspected && inspected.value().has_color_encoding_metadata &&
                inspected.value().source.digest == sha256(*b), "profile metadata is source-bound and flagged");
        const auto result = decode_texture_image_rgba8(*b, sha256(*b), 1U, 1U);
        require(!result && result.error().code == ErrorCode::unsupported, "unmanaged color metadata is refused");
    }
    const auto stale_profile = decode_texture_image_rgba8(jpeg_icc, sha256(baseline), 1U, 1U);
    require(!stale_profile && stale_profile.error().code == ErrorCode::stale_data,
            "stale source binding is refused before color metadata handling");
    auto exif_bad = baseline; add_app(exif_bad, 0xe1U, Bytes{'E','x','i','f',0U,0U,'I'});
    require(!inspect_texture_source(exif_bad), "truncated EXIF header is refused");

    auto oriented = jpeg(0xc0U,8U,2U); add_app(oriented, 0xe1U, exif_orientation(6U));
    const auto oriented_source = inspect_texture_source(oriented);
    require(oriented_source && !oriented_source.value().has_color_encoding_metadata &&
            oriented_source.value().width == 2U && oriented_source.value().height == 1U,
            "orientation tag remains descriptive metadata in current source inspection");
#ifdef _WIN32
    const auto oriented_result = decode_texture_image_rgba8(oriented, sha256(oriented), 2U, 1U);
    if (oriented_result) {
        require_binding(oriented_result, oriented, 2U, 1U);
        std::cout << "OBSERVE EXIF orientation=6 returned source-sized RGBA; normalization remains unspecified\n";
    } else {
        std::cout << "OBSERVE decoder refused EXIF orientation=6: " << oriented_result.error().message << '\n';
    }
#endif
    Bytes tga(18U,0U); tga[2U]=2U; tga[12U]=2U; tga[14U]=2U; tga[16U]=32U; tga[17U]=0x38U;
    tga.insert(tga.end(), {0U,255U,0U,64U, 0U,0U,255U,128U,
        255U,255U,255U,192U, 255U,0U,0U,255U});
    const auto decoded_tga = decode_texture_image_rgba8(tga, sha256(tga), 2U, 2U);
    require_binding(decoded_tga, tga, 2U, 2U);
    require(decoded_tga.value().pixels == Bytes({255U,0U,0U,128U, 0U,255U,0U,64U,
        0U,0U,255U,255U, 255U,255U,255U,192U}), "TGA origin flags yield canonical top-left RGBA");
}
}

int main() { fixtures(); return EXIT_SUCCESS; }
