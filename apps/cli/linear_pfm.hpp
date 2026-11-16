#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// PFM stores scene-linear RGB as IEEE-754 float32, bottom row first. A
// negative scale declares little-endian bytes; alpha is deliberately omitted.
template <typename Image>
std::vector<std::uint8_t> encode_linear_pfm(const Image& image) {
    if (image.linear_hdr_rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4U)
        throw std::runtime_error("invalid linear HDR capture dimensions");
    const auto header = std::string("PF\n") + std::to_string(image.width) + " " +
        std::to_string(image.height) + "\n-1.0\n";
    std::vector<std::uint8_t> output(header.begin(), header.end());
    for (std::uint32_t y = image.height; y > 0U; --y)
        for (std::uint32_t x = 0U; x < image.width; ++x)
            for (unsigned channel = 0U; channel < 3U; ++channel) {
                const auto offset = (static_cast<std::size_t>(y - 1U) * image.width + x) * 4U + channel;
                const float value = image.linear_hdr_rgba[offset];
                if (!std::isfinite(value)) throw std::runtime_error("nonfinite HDR capture");
                const auto bits = std::bit_cast<std::uint32_t>(value);
                for (unsigned shift = 0U; shift < 32U; shift += 8U)
                    output.push_back(static_cast<std::uint8_t>(bits >> shift));
            }
    return output;
}
