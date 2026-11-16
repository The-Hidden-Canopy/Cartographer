#include <carto/assets/texture_image_decoder.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace carto::assets;
using Bytes = std::vector<std::uint8_t>;
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

// One grayscale DCT block; the large-category form exercises byte stuffing.
Bytes jpeg(std::uint8_t sof, std::uint8_t precision, bool large_categories = false,
           std::uint8_t dc_category = 15U, std::uint8_t ac_category = 14U) {
    Bytes bytes{0xffU, 0xd8U, 0xffU, 0xdbU, 0U, 67U, 0U};
    bytes.insert(bytes.end(), 64U, 1U);
    bytes.insert(bytes.end(), {0xffU, sof, 0U, 11U, precision, 0U, 1U, 0U, 1U,
        1U, 1U, 0x11U, 0U});
    bytes.insert(bytes.end(), {0xffU, 0xc4U, 0U, static_cast<std::uint8_t>(large_categories ? 39U : 38U),
        0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        static_cast<std::uint8_t>(large_categories ? dc_category : 0U),
        0x10U, 1U, static_cast<std::uint8_t>(large_categories ? 1U : 0U),
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        static_cast<std::uint8_t>(large_categories ? ac_category : 0U)});
    if (large_categories) bytes.push_back(0U);
    const bool progressive = sof == 0xc2U;
    bytes.insert(bytes.end(), {0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U, 0U,
        static_cast<std::uint8_t>(progressive ? 0U : 63U), 0U});
    if (large_categories) {
        bytes.insert(bytes.end(), {0x7fU, 0xffU, 0U, 0x7fU, 0xffU, 0U, 0x7fU});
    } else if (progressive) {
        bytes.insert(bytes.end(), {0x7fU, 0xffU, 0xdaU, 0U, 8U, 1U, 1U, 0U,
            1U, 63U, 0U, 0x7fU});
    } else {
        bytes.push_back(0x3fU);
    }
    bytes.insert(bytes.end(), {0xffU, 0xd9U});
    return bytes;
}

bool is_entropy_rejection(const carto::core::Result<DecodedRgba8Texture>& result) {
    if (result) return false;
    const auto& contexts = result.error().context;
    return std::find(contexts.begin(), contexts.end(), "JPEG Huffman entropy admission") != contexts.end();
}

void scenarios() {
    const auto sequential8 = jpeg(0xc1U, 8U);
    const auto sequential12 = jpeg(0xc1U, 12U);
    const auto progressive12 = jpeg(0xc2U, 12U);
    const auto boundaries12 = jpeg(0xc1U, 12U, true);
    for (const auto* bytes : {&sequential8, &sequential12, &progressive12, &boundaries12}) {
        const auto source = inspect_texture_source(*bytes, "image/jpeg");
        require(source && source.value().source.digest == sha256(*bytes),
                "JPEG source inspection preserves its exact digest");
#ifdef _WIN32
        const auto result = decode_texture_image_rgba8(*bytes, sha256(*bytes), 1U, 1U);
        require(!is_entropy_rejection(result), "valid category stream passes Huffman admission");
        if (result) require(result.value().admission.source.digest == sha256(*bytes),
                            "any returned pixels remain bound to the exact source");
#endif
    }
    auto broken_sequential = sequential12;
    broken_sequential[broken_sequential.size() - 3U] = 0x80U;
    auto broken_progressive = progressive12;
    broken_progressive[broken_progressive.size() - 3U] = 0x80U;
    const auto oversized_dc = jpeg(0xc1U, 8U, true, 12U, 10U);
    const auto oversized_ac = jpeg(0xc1U, 12U, true, 15U, 15U);
    for (const auto* bytes : std::array<const Bytes*, 4U>{&broken_sequential, &broken_progressive, &oversized_dc, &oversized_ac}) {
        require(static_cast<bool>(inspect_texture_source(*bytes)), "corrupt entropy retains bounded marker structure");
#ifdef _WIN32
        require(is_entropy_rejection(decode_texture_image_rgba8(*bytes, sha256(*bytes), 1U, 1U)),
                "invalid covered entropy is refused before WIC");
#endif
    }
    const auto unchecked_arithmetic = jpeg(0xc9U, 8U);
    auto unchecked_precision = jpeg(0xc2U, 10U);
    unchecked_precision[unchecked_precision.size() - 3U] = 0x80U;
    for (const auto* bytes : std::array<const Bytes*, 2U>{&unchecked_arithmetic, &unchecked_precision}) {
        require(static_cast<bool>(inspect_texture_source(*bytes)), "unchecked process has an inspected container");
#ifdef _WIN32
        const auto result = decode_texture_image_rgba8(*bytes, sha256(*bytes), 1U, 1U);
        require(!is_entropy_rejection(result), "unchecked route delegates to WIC without a false checked claim");
#endif
    }
    auto truncated = sequential12;
    truncated.pop_back();
    require(!decode_texture_image_rgba8(truncated, sha256(truncated), 1U, 1U), "truncated source is refused");
    const auto wrong_digest = sha256(Bytes{});
    require(!decode_texture_image_rgba8(sequential12, wrong_digest, 1U, 1U) &&
            !decode_texture_image_rgba8(sequential12, sha256(sequential12), 2U, 1U), "stale binding refuses");
    TextureSourceLimits limits;
    limits.max_source_bytes = sequential12.size() - 1U;
    require(!decode_texture_image_rgba8(sequential12, sha256(sequential12), 1U, 1U, limits), "source byte budget refuses");
}
}

int main() { scenarios(); return EXIT_SUCCESS; }
