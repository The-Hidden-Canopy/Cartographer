#include <carto/assets/texture_channel_pack.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace carto;
using namespace carto::assets;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::uint8_t> tga(
    std::uint16_t width,
    std::uint16_t height,
    const std::vector<std::array<std::uint8_t, 4U>>& rgba,
    bool alpha = true) {
    std::vector<std::uint8_t> bytes(18U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = static_cast<std::uint8_t>(width & 0xffU);
    bytes[13U] = static_cast<std::uint8_t>(width >> 8U);
    bytes[14U] = static_cast<std::uint8_t>(height & 0xffU);
    bytes[15U] = static_cast<std::uint8_t>(height >> 8U);
    bytes[16U] = alpha ? 32U : 24U;
    bytes[17U] = static_cast<std::uint8_t>(0x20U | (alpha ? 0x08U : 0U));
    for (const auto& pixel : rgba) {
        bytes.push_back(pixel[2U]);
        bytes.push_back(pixel[1U]);
        bytes.push_back(pixel[0U]);
        if (alpha) bytes.push_back(pixel[3U]);
    }
    return bytes;
}

Rgba8ChannelPackRecipe recipe_for(std::span<const std::uint8_t> bytes) {
    return Rgba8ChannelPackRecipe{{
        TextureChannelInput{bytes, TextureChannel::blue},
        TextureChannelInput{bytes, TextureChannel::red},
        TextureChannelInput{bytes, TextureChannel::alpha},
        TextureChannelInput{bytes, TextureChannel::green},
    }};
}

void explicit_typed_channels_pack_deterministically_and_validate() {
    const auto source = tga(2U, 1U, {{{10U, 20U, 30U, 40U}, {50U, 60U, 70U, 80U}}});
    const auto recipe = recipe_for(source);
    const auto packed = pack_rgba8_channels(recipe);
    require(static_cast<bool>(packed), "valid explicit source channel recipe packs");
    require(packed.value().width == 2U && packed.value().height == 1U,
        "output extent matches validated source extent");
    require(packed.value().rgba == std::vector<std::uint8_t>({
                30U, 10U, 40U, 20U, 70U, 50U, 80U, 60U}),
        "destination RGBA order follows explicit channel mapping");
    require(!packed.value().artifact_digest.is_zero(), "artifact digest is present");
    require(static_cast<bool>(packed.value().validate_against(recipe)),
        "artifact validates against exact recipe and source bytes");
    const auto repeated = pack_rgba8_channels(recipe);
    require(repeated && repeated.value().artifact_digest == packed.value().artifact_digest,
        "identical recipe and source produce identical digest");

    auto changed_source = source;
    changed_source.back() ^= 1U;
    const auto changed_recipe = recipe_for(changed_source);
    require(!packed.value().validate_against(changed_recipe),
        "changed input bytes invalidate a prior pack artifact");
}

void invalid_channels_extents_alpha_and_payloads_fail_closed() {
    const auto rgba = tga(2U, 1U, {{{1U, 2U, 3U, 4U}, {5U, 6U, 7U, 8U}}});
    auto invalid_channel = recipe_for(rgba);
    invalid_channel.outputs[0U].channel = static_cast<TextureChannel>(99U);
    require(!pack_rgba8_channels(invalid_channel), "unknown enum channel is refused");

    auto mismatch = recipe_for(rgba);
    const auto other_extent = tga(1U, 1U, {{{1U, 2U, 3U, 4U}}});
    mismatch.outputs[3U].encoded_tga = other_extent;
    require(!pack_rgba8_channels(mismatch), "mismatched source dimensions are refused");

    const auto rgb = tga(2U, 1U, {{{1U, 2U, 3U, 255U}, {4U, 5U, 6U, 255U}}}, false);
    auto unauthored_alpha = recipe_for(rgba);
    unauthored_alpha.outputs[2U] = TextureChannelInput{rgb, TextureChannel::alpha};
    require(!pack_rgba8_channels(unauthored_alpha),
        "decoder-generated opaque alpha is not accepted as authored alpha");

    auto malformed = recipe_for(rgba);
    auto truncated = rgba;
    truncated.pop_back();
    malformed.outputs[0U].encoded_tga = truncated;
    require(!pack_rgba8_channels(malformed), "truncated source payload is refused");
    auto trailing = rgba;
    trailing.push_back(0U);
    malformed.outputs[0U].encoded_tga = trailing;
    require(!pack_rgba8_channels(malformed), "uninterpreted trailing bytes are refused");

    TextureSourceLimits tight;
    tight.max_pixels = 1U;
    require(!pack_rgba8_channels(recipe_for(rgba), tight),
        "caller source pixel budget is enforced");
}

} // namespace

int main() {
    explicit_typed_channels_pack_deterministically_and_validate();
    invalid_channels_extents_alpha_and_payloads_fail_closed();
    std::cout << "PASS explicit texture channel packing boundaries\n";
    return EXIT_SUCCESS;
}
