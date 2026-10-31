#include <carto/assets/texture_image_decoder.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using namespace carto::assets;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

// These explicitly encode grayscale zero-coefficient blocks, not invented decoder output.
Bytes image(unsigned width, bool progressive = false, bool restarts = false, bool stuffed = false) {
    Bytes b{0xffU, 0xd8U, 0xffU, 0xdbU, 0U, 67U, 0U};
    b.insert(b.end(), 64U, 1U);
    b.insert(b.end(), {0xffU, static_cast<std::uint8_t>(progressive ? 0xc2U : 0xc0U),
        0U, 11U, 8U, 0U, 1U, 0U, static_cast<std::uint8_t>(width), 1U, 1U, 0x11U, 0U});
    if (stuffed) {
        // DC codes: 0 -> category 0, 10 -> category 11. AC code 0000 -> EOB.
        b.insert(b.end(), {0xffU, 0xc4U, 0U, 39U, 0U, 1U, 1U});
        b.insert(b.end(), 14U, 0U);
        b.insert(b.end(), {0U, 11U, 0x10U, 0U, 0U, 0U, 1U});
        b.insert(b.end(), 12U, 0U);
        b.push_back(0U);
    } else {
        b.insert(b.end(), {0xffU, 0xc4U, 0U, 38U,
            0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
            0x10U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});
    }
    if (restarts) b.insert(b.end(), {0xffU, 0xddU, 0U, 4U, 0U, 1U});
    b.insert(b.end(), {0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U,
        static_cast<std::uint8_t>(progressive ? 0U : 63U), 0U});
    if (progressive) {
        b.insert(b.end(), {0x7fU, 0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 1U, 63U, 0U, 0x7fU});
    } else if (restarts) {
        b.insert(b.end(), {0x3fU, 0xffU, 0xd0U, 0x3fU});
    } else if (stuffed) {
        b.insert(b.end(), {0x05U, 0xffU, 0U, 0xc3U});
    } else {
        b.push_back(width > 8U ? 0x0fU : 0x3fU);
    }
    b.insert(b.end(), {0xffU, 0xd9U});
    return b;
}

Bytes progressive_refinement() {
    Bytes b{0xffU, 0xd8U, 0xffU, 0xdbU, 0U, 67U, 0U};
    b.insert(b.end(), 64U, 1U);
    b.insert(b.end(), {0xffU, 0xc2U, 0U, 11U, 8U, 0U, 1U, 0U, 1U, 1U, 1U, 0x11U, 0U,
        0xffU, 0xc4U, 0U, 39U,
        0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0x10U, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0x01U, 0x00U,
        // DC first at Al=1, then refinement Ah=1/Al=0.
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U, 0U, 1U, 0x7fU,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U, 0U, 0x10U, 0x7fU,
        // AC coefficient 1 first appears at Al=1; then EOB plus correction bit.
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 1U, 1U, 1U, 0x7fU,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 1U, 1U, 0x10U, 0xbfU,
        0xffU, 0xd9U});
    return b;
}

Bytes progressive_restart() {
    auto b = image(16U, true);
    b.resize(b.size() - 24U); // replace both scans and EOI
    b.insert(b.end(), {0xffU, 0xddU, 0U, 4U, 0U, 1U,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U, 0U, 0U,
        0x7fU, 0xffU, 0xd0U, 0x7fU,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 1U, 63U, 0U,
        0x7fU, 0xffU, 0xd0U, 0x7fU, 0xffU, 0xd9U});
    return b;
}

Bytes progressive_eob_run() {
    auto b = image(16U, true);
    b[b.size() - 25U] = 0x10U; // AC symbol: EOB run with one extra bit
    b[b.size() - 14U] = 0x3fU; // two DC blocks: 00 + padding
    b[b.size() - 3U] = 0x3fU; // AC EOB-run code 0, extra bit 0: two blocks
    return b;
}

Bytes progressive_refinement_eob_run() {
    auto b = progressive_refinement();
    b.resize(b.size() - 46U);
    b[79U] = 16U; // SOF width, two grayscale blocks
    b.back() = 0x10U; // second AC code is an EOB run
    b.insert(b.end(), {
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U, 0U, 0U, 0x3fU,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 1U, 1U, 1U, 0x5fU,
        0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 1U, 1U, 0x10U, 0x97U,
        0xffU, 0xd9U});
    return b;
}

bool decode(const Bytes& b, unsigned width, unsigned height = 1U) {
    return static_cast<bool>(decode_texture_image_rgba8(b, sha256(b), width, height));
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (auto byte : bytes) {
        crc ^= byte;
        for (unsigned i = 0U; i < 8U; ++i)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void be32(Bytes& b, std::uint32_t n) {
    b.insert(b.end(), {static_cast<std::uint8_t>(n >> 24U), static_cast<std::uint8_t>(n >> 16U),
        static_cast<std::uint8_t>(n >> 8U), static_cast<std::uint8_t>(n)});
}

void chunk(Bytes& b, const char* type, std::span<const std::uint8_t> payload) {
    be32(b, static_cast<std::uint32_t>(payload.size()));
    const auto begin = b.size();
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), payload.begin(), payload.end());
    be32(b, crc32(std::span<const std::uint8_t>{b}.subspan(begin)));
}

Bytes png() {
    Bytes b{0x89U, 'P', 'N', 'G', 13U, 10U, 26U, 10U};
    const Bytes ihdr{0U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 8U, 6U, 0U, 0U, 0U};
    const Bytes data{0x78U, 1U, 1U, 5U, 0U, 0xfaU, 0xffU, 0U,
        0xffU, 0U, 0U, 0xffU, 5U, 0U, 1U, 0xffU};
    chunk(b, "IHDR", ihdr); chunk(b, "IDAT", data); chunk(b, "IEND", {});
    return b;
}

void fixtures() {
    const auto baseline = image(1U);
    const auto progressive = image(1U, true);
    const auto restarted = image(16U, false, true);
    const auto stuffed = image(16U, false, false, true);
    const auto refined = progressive_refinement();
    const auto progressive_with_restart = progressive_restart();
    const auto eob_run = progressive_eob_run();
    const auto refinement_eob_run = progressive_refinement_eob_run();
    auto newly_significant = refined;
    newly_significant[newly_significant.size() - 14U] = 0xbfU; // initial AC EOB
    newly_significant[newly_significant.size() - 3U] = 0x7fU; // refinement inserts coefficient
    for (const auto* source : std::array<const Bytes*, 9>{&baseline, &progressive, &restarted, &stuffed,
                               &refined, &progressive_with_restart, &eob_run,
                               &newly_significant, &refinement_eob_run}) {
        require(static_cast<bool>(inspect_texture_source(*source)), "valid JPEG fixture passes container admission");
    }
#ifdef _WIN32
    const auto decoded = decode_texture_image_rgba8(baseline, sha256(baseline), 1U, 1U);
    require(decoded && decoded.value().admission.source.digest == sha256(baseline) &&
            decoded.value().pixels == Bytes({128U, 128U, 128U, 255U}),
            "baseline exact pixels retain source digest");
    const auto progressive_decoded = decode_texture_image_rgba8(progressive, sha256(progressive), 1U, 1U);
    require(progressive_decoded && progressive_decoded.value().admission.source.digest == sha256(progressive) &&
            progressive_decoded.value().pixels == Bytes({128U, 128U, 128U, 255U}),
            "progressive pixel and exact source identity are preserved");
    require(decode(restarted, 16U), "valid baseline restart remains supported");
    require(decode(stuffed, 16U), "valid stuffed entropy remains supported");
    require(decode(refined, 1U), "progressive DC and AC refinement remains supported");
    require(decode(progressive_with_restart, 16U), "progressive restarts remain supported");
    require(decode(eob_run, 16U), "progressive EOB runs cover subsequent blocks");
    require(decode(newly_significant, 1U), "refinement may introduce a new coefficient");
    require(decode(refinement_eob_run, 16U), "refinement EOB run consumes existing coefficient bits");
#else
    require(!decode(baseline, 1U), "non-Windows JPEG pixel decode is unsupported");
#endif
    auto wrong_code = baseline;
    wrong_code[wrong_code.size() - 3U] = 0x80U; // no DC code starts with 1
    auto incomplete = image(16U);
    incomplete[incomplete.size() - 3U] = 0x3fU; // one coded block, two required
    auto wrong_restart = restarted;
    wrong_restart[wrong_restart.size() - 4U] = 0xd1U;
    auto missing_restart = restarted;
    missing_restart.erase(missing_restart.end() - 5, missing_restart.end() - 3);
    for (const auto* source : {&wrong_code, &incomplete, &wrong_restart, &missing_restart}) {
        require(static_cast<bool>(inspect_texture_source(*source)),
                "entropy adversary retains structurally admissible markers");
#ifdef _WIN32
        require(!decode(*source, source == &wrong_code ? 1U : 16U),
                "invalid baseline entropy refuses even with matching digest");
#endif
    }
    auto bad_progression = refined;
    bad_progression[bad_progression.size() - 4U] = 0x20U;
    auto bad_refinement = refined;
    bad_refinement[bad_refinement.size() - 3U] = 0xffU;
    bad_refinement.insert(bad_refinement.end() - 2, 0U); // stuffed undefined Huffman code
    auto bad_progressive_restart = progressive_with_restart;
    bad_progressive_restart[bad_progressive_restart.size() - 4U] = 0xd1U;
    auto eob_past_end = eob_run;
    eob_past_end[eob_past_end.size() - 3U] = 0x7fU; // EOB run of three, only two blocks
    auto eob_across_restart = progressive_with_restart;
    const std::array<std::uint8_t, 2U> dri_marker{0xffU, 0xddU};
    const auto dht_end = std::search(eob_across_restart.begin(), eob_across_restart.end(),
        dri_marker.begin(), dri_marker.end());
    require(dht_end != eob_across_restart.end(), "restart fixture has a DRI marker");
    *(dht_end - 1) = 0x10U; // AC EOB run symbol
    eob_across_restart[eob_across_restart.size() - 6U] = 0x3fU;
    auto incomplete_progressive = eob_run;
    incomplete_progressive[incomplete_progressive.size() - 14U] = 0x7fU;
    auto impossible_insertion = refined;
    impossible_insertion[impossible_insertion.size() - 3U] = 0x7fU;
    for (const auto* source : {&bad_progression, &bad_refinement, &bad_progressive_restart,
                               &eob_past_end, &eob_across_restart, &incomplete_progressive,
                               &impossible_insertion}) {
        require(static_cast<bool>(inspect_texture_source(*source)),
                "progressive adversary retains bounded marker structure");
#ifdef _WIN32
        require(!decode(*source, (source == &bad_progressive_restart ||
                                 source == &eob_past_end || source == &eob_across_restart ||
                                 source == &incomplete_progressive) ? 16U : 1U),
                "bad progressive progression, entropy or restart is refused");
#endif
    }
    auto truncated = baseline;
    truncated.pop_back();
    require(!inspect_texture_source(truncated) && !decode(truncated, 1U), "truncated EOI refuses");
    require(!decode_texture_image_rgba8(baseline, sha256(std::span<const std::uint8_t>{}), 1U, 1U) &&
            !decode_texture_image_rgba8(baseline, sha256(baseline), 2U, 1U), "stale identity refuses");
    TextureSourceLimits limits;
    limits.max_source_bytes = baseline.size() - 1U;
    require(!decode_texture_image_rgba8(baseline, sha256(baseline), 1U, 1U, limits), "source limit refuses");
    limits.max_source_bytes = 1024U;
    limits.max_dimension = 8U;
    require(!decode_texture_image_rgba8(restarted, sha256(restarted), 16U, 1U, limits),
            "dimension limit refuses before decode");
    const auto png_bytes = png();
    const auto png_result = decode_texture_image_rgba8(png_bytes, sha256(png_bytes), 1U, 1U);
#ifdef _WIN32
    require(png_result && png_result.value().pixels == Bytes({255U, 0U, 0U, 255U}) &&
            png_result.value().admission.source.digest == sha256(png_bytes),
            "PNG pixels and source identity are unchanged");
#else
    require(!png_result, "non-Windows PNG pixel decode is unsupported");
#endif
    Bytes tga(18U, 0U);
    tga[2U] = 2U; tga[12U] = 1U; tga[14U] = 1U; tga[16U] = 24U;
    tga.insert(tga.end(), {0U, 0U, 255U});
    const auto tga_result = decode_texture_image_rgba8(tga, sha256(tga), 1U, 1U);
    require(tga_result && tga_result.value().pixels == Bytes({255U, 0U, 0U, 255U}) &&
            tga_result.value().admission.source.digest == sha256(tga),
            "TGA pixels and source identity are unchanged");
}
}

int main() { fixtures(); return EXIT_SUCCESS; }
