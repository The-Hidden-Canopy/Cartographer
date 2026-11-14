#pragma once

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace carto::assets {
// Strict scene-linear RGB PFM input. Values are float32, not normalized bytes.
// This is a building block only; project material binding is not yet wired to it.
struct LinearFloatImage {
    std::uint32_t width{}, height{};
    std::vector<float> rgba;
};
inline LinearFloatImage decode_linear_pfm(std::span<const std::uint8_t> bytes) {
    constexpr std::uint64_t max_pixels = 4096ULL * 4096ULL;
    std::size_t cursor = 0;
    auto line = [&]() -> std::string_view {
        const auto begin = cursor;
        while (cursor < bytes.size() && bytes[cursor] != '\n' && cursor-begin <= 64) ++cursor;
        if (cursor == bytes.size() || cursor-begin > 64) throw std::runtime_error("invalid PFM header");
        auto result = std::string_view(reinterpret_cast<const char*>(bytes.data()+begin), cursor-begin);
        ++cursor; return result;
    };
    if (line() != "PF") throw std::runtime_error("expected RGB PFM");
    auto dimensions = line(); auto split = dimensions.find(' ');
    if (split == dimensions.npos) throw std::runtime_error("invalid PFM dimensions");
    LinearFloatImage out;
    auto parse = [](std::string_view s, std::uint32_t& value) {
        const auto result = std::from_chars(s.data(), s.data()+s.size(), value);
        return result.ec == std::errc{} && result.ptr == s.data()+s.size();
    };
    if (!parse(dimensions.substr(0,split), out.width) || !parse(dimensions.substr(split+1), out.height) ||
        !out.width || !out.height || out.width > 4096 || out.height > 4096 ||
        std::uint64_t(out.width)*out.height > max_pixels) throw std::runtime_error("PFM dimensions exceed bounds");
    // Deliberately restrict the admitted contract to unit-scale little-endian.
    if (line() != "-1.0") throw std::runtime_error("PFM requires little-endian unit scale");
    const auto pixels = std::uint64_t(out.width)*out.height;
    if (bytes.size()-cursor != pixels*12U) throw std::runtime_error("PFM payload length mismatch");
    out.rgba.resize(static_cast<std::size_t>(pixels)*4U);
    for (std::uint32_t file_y=0; file_y<out.height; ++file_y)
        for (std::uint32_t x=0; x<out.width; ++x) {
            const auto destination=(std::size_t(out.height-1-file_y)*out.width+x)*4U;
            for (unsigned c=0;c<3;++c) {
                std::uint32_t bits=0; for(unsigned b=0;b<4;++b) bits|=std::uint32_t(bytes[cursor++])<<(b*8);
                const float v=std::bit_cast<float>(bits);
                if (!std::isfinite(v)) throw std::runtime_error("PFM contains nonfinite values");
                out.rgba[destination+c]=v;
            }
            out.rgba[destination+3]=1.0f;
        }
    return out;
}
} // namespace carto::assets
