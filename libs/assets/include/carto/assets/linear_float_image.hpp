#pragma once

#include <carto/assets/blob_store.hpp>
#include <algorithm>
#include <array>
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
// Material bindings use an explicit source semantic and retain float precision.
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
// Scene-linear float color artifact. The canonical digest binds
// dimensions, all float bits and source digest; mips are never quantized.
struct LinearFloatMipChain {
    Sha256Digest source_digest;
    std::vector<LinearFloatImage> levels;
    Sha256Digest artifact_digest;
};
inline Sha256Digest linear_float_mip_digest(const LinearFloatMipChain& chain) {
    std::vector<std::uint8_t> bytes{'C','A','R','T','O','F','3','2',1};
    const auto put = [&](std::uint32_t v) { for (unsigned s=0;s<32;s+=8) bytes.push_back(static_cast<std::uint8_t>(v>>s)); };
    bytes.insert(bytes.end(), chain.source_digest.bytes.begin(), chain.source_digest.bytes.end());
    put(static_cast<std::uint32_t>(chain.levels.size()));
    for (const auto& level : chain.levels) {
        put(level.width); put(level.height);
        for (const auto v : level.rgba) put(std::bit_cast<std::uint32_t>(v));
    }
    return sha256(bytes);
}
inline LinearFloatMipChain generate_linear_float_mips(LinearFloatImage image, Sha256Digest source) {
    if (source.is_zero() || !image.width || !image.height || image.width>4096 || image.height>4096 ||
        image.rgba.size()!=std::size_t(image.width)*image.height*4U)
        throw std::runtime_error("invalid float texture source");
    for (std::size_t i=0; i<image.rgba.size(); ++i) {
        const float v=image.rgba[i];
        if (!std::isfinite(v) || v<0.0f || (i%4U==3U && v>1.0f))
            throw std::runtime_error("invalid linear float color or alpha texel");
    }
    LinearFloatMipChain out; out.source_digest=source; out.levels.push_back(std::move(image));
    while (out.levels.back().width>1 || out.levels.back().height>1) {
        const auto& src=out.levels.back(); LinearFloatImage dst;
        dst.width=std::max(1U,src.width/2); dst.height=std::max(1U,src.height/2);
        dst.rgba.resize(std::size_t(dst.width)*dst.height*4U);
        for (std::uint32_t y=0;y<dst.height;++y) for (std::uint32_t x=0;x<dst.width;++x) {
            double sum[4]{}; unsigned n=0;
            const auto y0=std::uint64_t(y)*src.height/dst.height, y1=std::uint64_t(y+1)*src.height/dst.height;
            const auto x0=std::uint64_t(x)*src.width/dst.width, x1=std::uint64_t(x+1)*src.width/dst.width;
            for(auto sy=y0;sy<y1;++sy) for(auto sx=x0;sx<x1;++sx) {
                auto at=(sy*src.width+sx)*4U; for(unsigned c=0;c<4;++c) sum[c]+=src.rgba[at+c]; ++n;
            }
            auto at=(std::size_t(y)*dst.width+x)*4U;
            for(unsigned c=0;c<4;++c) dst.rgba[at+c]=static_cast<float>(sum[c]/n);
        }
        out.levels.push_back(std::move(dst));
    }
    out.artifact_digest=linear_float_mip_digest(out); return out;
}
inline bool validate_linear_float_mips(const LinearFloatMipChain& chain) {
    if(chain.source_digest.is_zero() || chain.levels.empty() || chain.levels.size()>13 || chain.artifact_digest.is_zero()) return false;
    auto w=chain.levels.front().width, h=chain.levels.front().height;
    if (!w || !h || w>4096 || h>4096) return false;
    for(std::size_t level_index=0; level_index<chain.levels.size(); ++level_index) {
        const auto& level=chain.levels[level_index];
        // A valid chain stops at its first 1x1 level; an extra terminal
        // subresource would exceed the GPU's legal mip count.
        if(w==1U && h==1U && level_index+1U!=chain.levels.size()) return false;
        if(level.width!=w || level.height!=h || level.rgba.size()!=std::size_t(w)*h*4U) return false;
        for(std::size_t i=0; i<level.rgba.size(); ++i) {
            const float v=level.rgba[i];
            if(!std::isfinite(v) || v<0.0f || (i%4U==3U && v>1.0f)) return false;
        }
        w=std::max(1U,w/2); h=std::max(1U,h/2);
    }
    return chain.levels.back().width==1 && chain.levels.back().height==1 && linear_float_mip_digest(chain)==chain.artifact_digest;
}
// Canonical, bounded persistence of derived float mips. The final SHA-256
// authenticates source identity, geometry and exact IEEE float bits.
inline std::vector<std::uint8_t> serialize_linear_float_mips(const LinearFloatMipChain& chain) {
    if (!validate_linear_float_mips(chain)) throw std::runtime_error("invalid float mip artifact");
    std::vector<std::uint8_t> out{'C','A','R','T','O','F','3','2',1};
    auto put=[&](std::uint32_t v){for(unsigned shift=0;shift<32;shift+=8) out.push_back(static_cast<std::uint8_t>(v>>shift));};
    out.insert(out.end(),chain.source_digest.bytes.begin(),chain.source_digest.bytes.end());
    put(static_cast<std::uint32_t>(chain.levels.size()));
    for(const auto& level:chain.levels){put(level.width);put(level.height);for(float v:level.rgba)put(std::bit_cast<std::uint32_t>(v));}
    out.insert(out.end(),chain.artifact_digest.bytes.begin(),chain.artifact_digest.bytes.end());
    return out;
}
inline LinearFloatMipChain deserialize_linear_float_mips(std::span<const std::uint8_t> bytes) {
    constexpr std::uint64_t max_bytes=512ULL*1024*1024;
    const std::array<std::uint8_t,9> magic{'C','A','R','T','O','F','3','2',1};
    if(bytes.size()<magic.size()+32+4+32 || bytes.size()>max_bytes || !std::equal(magic.begin(),magic.end(),bytes.begin()))
        throw std::runtime_error("invalid float mip artifact header");
    std::size_t cursor=magic.size(); LinearFloatMipChain out;
    std::copy_n(bytes.data()+cursor,32,out.source_digest.bytes.begin()); cursor+=32;
    auto get=[&](){if(bytes.size()-cursor<4)throw std::runtime_error("truncated float mip artifact");
        std::uint32_t v=0;for(unsigned b=0;b<4;++b)v|=std::uint32_t(bytes[cursor++])<<(b*8);return v;};
    auto count=get(); if(!count || count>13)throw std::runtime_error("invalid float mip count");
    for(std::uint32_t m=0;m<count;++m){
        LinearFloatImage level; level.width=get();level.height=get();
        if(!level.width || !level.height || level.width>4096 || level.height>4096) throw std::runtime_error("invalid float mip extent");
        const auto values=std::uint64_t(level.width)*level.height*4U;
        if(values>(bytes.size()-cursor)/4U || bytes.size()-cursor-values*4U<32U) throw std::runtime_error("truncated float mip pixels");
        level.rgba.reserve(static_cast<std::size_t>(values));for(std::uint64_t n=0;n<values;++n)level.rgba.push_back(std::bit_cast<float>(get()));
        out.levels.push_back(std::move(level));
    }
    if(bytes.size()-cursor!=32)throw std::runtime_error("float mip artifact trailing bytes");
    std::copy_n(bytes.data()+cursor,32,out.artifact_digest.bytes.begin());
    if(!validate_linear_float_mips(out))throw std::runtime_error("float mip artifact failed integrity validation");
    return out;
}
} // namespace carto::assets
