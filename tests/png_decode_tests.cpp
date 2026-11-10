#include <algorithm>
#include <array>
#include <carto/assets/texture_mip.hpp>
#include <carto/assets/texture_source.hpp>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
void check(bool good, const char *message) {
    if (!good) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void be32(Bytes &data, std::uint32_t v) {
    for (int shift : {24, 16, 8, 0})
        data.push_back(std::uint8_t(v >> shift));
}
void chunk(Bytes &png, const char *name, const Bytes &data) {
    be32(png, std::uint32_t(data.size()));
    const auto begin = png.size();
    png.insert(png.end(), name, name + 4);
    png.insert(png.end(), data.begin(), data.end());
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t p = begin; p < png.size(); ++p) {
        crc ^= png[p];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    be32(png, ~crc);
}
// Independent fixture encoder: one stored DEFLATE block, no third-party corpus.
Bytes png(std::uint32_t w, std::uint32_t h, unsigned depth, unsigned color, const Bytes &raw,
          bool adam7 = false, bool animated = false, const Bytes &palette = {},
          const Bytes &transparency = {}) {
    check(raw.size() < 65536, "bounded test encoder");
    Bytes out{137, 80, 78, 71, 13, 10, 26, 10}, header;
    be32(header, w);
    be32(header, h);
    header.insert(header.end(),
                  {std::uint8_t(depth), std::uint8_t(color), 0, 0, std::uint8_t(adam7)});
    chunk(out, "IHDR", header);
    if (animated)
        chunk(out, "acTL", {0, 0, 0, 1, 0, 0, 0, 0});
    if (!palette.empty())
        chunk(out, "PLTE", palette);
    if (!transparency.empty())
        chunk(out, "tRNS", transparency);
    const auto length = std::uint16_t(raw.size()), inverse = std::uint16_t(~length);
    Bytes z{0x78,
            0x01,
            0x01,
            std::uint8_t(length),
            std::uint8_t(length >> 8),
            std::uint8_t(inverse),
            std::uint8_t(inverse >> 8)};
    z.insert(z.end(), raw.begin(), raw.end());
    std::uint32_t a = 1, b = 0;
    for (auto v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16U) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}
void expect(const Bytes &source, const Bytes &expected) {
    const auto decoded = carto::assets::decode_png_rgba8(source);
    if (!decoded)
        std::cerr << decoded.error().message << '\n';
    check(bool(decoded), "PNG pixel decode must succeed");
    check(decoded.value().pixels == expected, "PNG pixels must match independent corpus exactly");
    check(decoded.value().admission.source.digest == carto::assets::sha256(source) &&
              decoded.value().admission.stage == carto::assets::TextureSourceStage::decoded,
          "PNG decoded pixels retain exact source evidence");
}
} // namespace
int main() {
#ifdef _WIN32
    using namespace carto::assets;
    Bytes pixels, filtered;
    for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < 2; ++x)
            pixels.insert(pixels.end(),
                          {std::uint8_t(11 + x * 73), std::uint8_t(7 + y * 31),
                           std::uint8_t(251 - x * 41 - y * 3), std::uint8_t(1 + y * 57)});
    auto at = [&](int x, int y) {
        return x < 0 || y < 0 ? 0 : int(pixels[std::size_t(y) * 8 + std::size_t(x)]);
    };
    for (int y = 0; y < 5; ++y) {
        filtered.push_back(std::uint8_t(y));
        for (int x = 0; x < 8; ++x) {
            const int a = at(x - 4, y), b = at(x, y - 1), c = at(x - 4, y - 1), p = a + b - c;
            const auto paeth =
                std::abs(p - a) <= std::abs(p - b) && std::abs(p - a) <= std::abs(p - c)
                    ? a
                    : (std::abs(p - b) <= std::abs(p - c) ? b : c);
            const int predictor = y == 0   ? 0
                                  : y == 1 ? a
                                  : y == 2 ? b
                                  : y == 3 ? (a + b) / 2
                                           : paeth;
            filtered.push_back(std::uint8_t(at(x, y) - predictor));
        }
    }
    const auto source = png(2, 5, 8, 6, filtered);
    expect(source, pixels); // all five row filters and straight alpha (including alpha=1)
    expect(png(2, 1, 8, 2, {0, 255, 0, 0, 0, 255, 0}), {255, 0, 0, 255, 0, 255, 0, 255});
    expect(png(2, 1, 8, 0, {0, 0, 255}), {0, 0, 0, 255, 255, 255, 255, 255});
    expect(png(2, 1, 8, 4, {0, 255, 128, 64, 255}), {255, 255, 255, 128, 64, 64, 64, 255});
    expect(png(2, 1, 16, 0, {0, 255, 255, 0, 0}), {255, 255, 255, 255, 0, 0, 0, 255});
    expect(png(2, 1, 1, 0, {0, 0x80}), {255, 255, 255, 255, 0, 0, 0, 255});
    expect(png(2, 1, 8, 3, {0, 0, 1}, false, false, {255, 0, 0, 0, 0, 255}, {0, 128}),
           {255, 0, 0, 0, 0, 0, 255, 128});
    expect(png(2, 2, 8, 6,
               {0, 255, 0, 0, 255, 0, 0, 255, 0, 128, 0, 0, 0, 255, 255, 255, 255, 255, 64}, true),
           {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 255, 255, 255, 255, 64});
    for (std::size_t n = 0; n < source.size(); ++n)
        check(!decode_png_rgba8(std::span(source).first(n)), "truncated PNG must fail");
    auto bad = source;
    bad[bad.size() / 2] ^= 1;
    check(!decode_png_rgba8(bad), "CRC-corrupt PNG must fail");
    bad = source;
    bad.push_back(0);
    check(!decode_png_rgba8(bad), "trailing PNG data must fail");
    TextureSourceLimits limits;
    limits.max_pixels = 1;
    check(!decode_png_rgba8(source, limits), "PNG dimension/pixel budget precedes native decode");
    check(!decode_png_rgba8(png(1, 1, 8, 6, {0, 0, 0, 0, 255}, false, true)),
          "APNG must fail visibly");
    const auto decoded =
        decode_texture_rgba8(png(2, 1, 8, 6, {0, 0, 0, 0, 255, 255, 255, 255, 255}));
    check(bool(decoded), "unified decoder dispatches PNG to pixel execution");
    const auto srgb =
        generate_rgba8_mips(decoded.value(), {TextureSemantic::base_color, ColorSpace::srgb});
    const auto data =
        generate_rgba8_mips(decoded.value(), {TextureSemantic::data, ColorSpace::linear});
    check(srgb && data && srgb.value().levels.back().pixels[0] == 188 &&
              data.value().levels.back().pixels[0] == 128,
          "decoded PNG reuses semantic mip generation without silently changing source transfer");
    std::cout << "PNG decode: filters 0-4, RGB/gray/alpha/palette, 1/8/16-bit, Adam7, "
                 "budgets/corruption and semantic mips passed\n";
#else
    check(!carto::assets::decode_png_rgba8(png(1, 1, 8, 6, {0, 0, 0, 0, 255})),
          "build without PNG adapter must fail visibly");
    std::cout << "PNG native adapter unavailable; unsupported path checked\n";
#endif
}
