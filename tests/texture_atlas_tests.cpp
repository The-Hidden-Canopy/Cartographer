#include <carto/assets/texture_atlas.hpp>

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
    std::span<const std::array<std::uint8_t, 4U>> rgba,
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

void authored_layout_exports_deterministically_and_binds_inputs() {
    const auto horizontal = tga(2U, 1U,
        std::array<std::array<std::uint8_t, 4U>, 2U>{{
            {{10U, 20U, 30U, 40U}}, {{50U, 60U, 70U, 80U}}}});
    const auto vertical = tga(1U, 2U,
        std::array<std::array<std::uint8_t, 4U>, 2U>{{
            {{90U, 100U, 110U, 120U}}, {{130U, 140U, 150U, 160U}}}});
    const std::array sources{
        TextureAtlasSource{"trim.horizontal", horizontal},
        TextureAtlasSource{"trim.vertical", vertical},
    };
    const TextureAtlasRecipe recipe{
        .width = 3U,
        .height = 2U,
        .background_rgba = {1U, 2U, 3U, 4U},
        .assignments = {
            TextureAtlasAssignment{"trim.horizontal", 0U, 1U},
            TextureAtlasAssignment{"trim.vertical", 2U, 0U},
        },
    };

    const auto product = build_texture_atlas(recipe, sources);
    if (!product) {
        std::cerr << "atlas error: " << product.error().message << '\n';
        for (const auto& context : product.error().context) {
            std::cerr << "  " << context << '\n';
        }
    }
    require(static_cast<bool>(product), "valid explicit atlas layout compiles");
    require(product.value().rgba == std::vector<std::uint8_t>({
        1U, 2U, 3U, 4U, 1U, 2U, 3U, 4U, 90U, 100U, 110U, 120U,
        10U, 20U, 30U, 40U, 50U, 60U, 70U, 80U, 130U, 140U, 150U, 160U}),
        "authored pixel coordinates preserve source order and explicit background");
    require(product.value().assignments.size() == 2U &&
        product.value().assignments[0U].source_digest ==
            inspect_texture_source(horizontal, "image/x-tga").value().source.digest,
        "resolved assignment records the exact source digest and dimensions");
    require(static_cast<bool>(product.value().validate_against(recipe, sources)),
        "product validates against exact current layout and source bytes");
    const auto repeated = build_texture_atlas(recipe, sources);
    require(repeated && repeated.value().artifact_digest == product.value().artifact_digest,
        "same layout and bytes produce a stable artifact digest");

    const auto exported = product.value().export_tga();
    require(static_cast<bool>(exported), "validated product exports a TGA image");
    const auto decoded = decode_tga_rgba8(exported.value());
    require(decoded && decoded.value().pixels == product.value().rgba,
        "export has explicit top-left origin and round-trips exact RGBA pixels");

    auto changed = horizontal;
    changed.back() ^= 1U;
    const std::array changed_sources{
        TextureAtlasSource{"trim.horizontal", changed},
        TextureAtlasSource{"trim.vertical", vertical},
    };
    require(!product.value().validate_against(recipe, changed_sources),
        "changed source bytes stale the previous atlas receipt");
    auto moved_recipe = recipe;
    moved_recipe.assignments[0U].x = 1U;
    require(!product.value().validate_against(moved_recipe, sources),
        "changed authored layout stales the previous atlas receipt");
}

void invalid_assignment_and_payload_boundaries_fail_closed() {
    const auto pixel = tga(1U, 1U,
        std::array<std::array<std::uint8_t, 4U>, 1U>{{{{10U, 20U, 30U, 40U}}}});
    const std::array sources{TextureAtlasSource{"trim.one", pixel}};
    const TextureAtlasRecipe valid{
        .width = 2U,
        .height = 1U,
        .background_rgba = {0U, 0U, 0U, 0U},
        .assignments = {TextureAtlasAssignment{"trim.one", 0U, 0U}},
    };
    require(static_cast<bool>(build_texture_atlas(valid, sources)),
        "boundary cases start from a valid atlas");

    auto duplicate = valid;
    duplicate.assignments.push_back(TextureAtlasAssignment{"trim.one", 1U, 0U});
    require(!build_texture_atlas(duplicate,
        std::array{TextureAtlasSource{"trim.one", pixel},
            TextureAtlasSource{"trim.two", pixel}}),
        "duplicate assignment identities are refused before overwrite");

    auto missing = valid;
    missing.assignments[0U].source_identity = "trim.missing";
    require(!build_texture_atlas(missing, sources),
        "assignment to absent source is refused");
    require(!build_texture_atlas(valid, std::span<const TextureAtlasSource>{}),
        "unassigned layout cannot silently omit a source");

    auto overlap = valid;
    overlap.assignments.push_back(TextureAtlasAssignment{"trim.two", 0U, 0U});
    const std::array two_sources{
        TextureAtlasSource{"trim.one", pixel}, TextureAtlasSource{"trim.two", pixel}};
    require(!build_texture_atlas(overlap, two_sources),
        "overlapping explicit placements are refused rather than resolved by order");

    auto out_of_bounds = valid;
    out_of_bounds.assignments[0U].x = 2U;
    require(!build_texture_atlas(out_of_bounds, sources),
        "source extent crossing atlas boundary is refused");
    auto zero_extent = valid;
    zero_extent.width = 0U;
    require(!build_texture_atlas(zero_extent, sources),
        "zero atlas extent is refused");
    auto empty_layout = valid;
    empty_layout.assignments.clear();
    require(!build_texture_atlas(empty_layout, std::span<const TextureAtlasSource>{}),
        "empty layout cannot be presented as a compiled trim atlas");

    const TextureAtlasRecipe two_assignments{
        .width = 2U,
        .height = 1U,
        .background_rgba = {0U, 0U, 0U, 0U},
        .assignments = {
            TextureAtlasAssignment{"trim.one", 0U, 0U},
            TextureAtlasAssignment{"trim.two", 1U, 0U},
        },
    };
    const std::array duplicate_sources{
        TextureAtlasSource{"trim.one", pixel}, TextureAtlasSource{"trim.one", pixel}};
    require(!build_texture_atlas(two_assignments, duplicate_sources),
        "duplicate supplied source identities are refused");

    auto trailing = pixel;
    trailing.push_back(0U);
    const std::array trailing_source{TextureAtlasSource{"trim.one", trailing}};
    require(!build_texture_atlas(valid, trailing_source),
        "uninterpreted source bytes are not accepted as live pixels");
    auto truncated = pixel;
    truncated.pop_back();
    const std::array truncated_source{TextureAtlasSource{"trim.one", truncated}};
    require(!build_texture_atlas(valid, truncated_source),
        "truncated source pixels are refused");
    auto rle = pixel;
    rle[2U] = 10U;
    const std::array rle_source{TextureAtlasSource{"trim.one", rle}};
    require(!build_texture_atlas(valid, rle_source),
        "unsupported run-length TGA layout is refused explicitly");

    TextureSourceLimits tight;
    tight.max_pixels = 1U;
    auto larger = valid;
    larger.width = 2U;
    require(!build_texture_atlas(larger, sources, tight),
        "caller atlas pixel budget is enforced");
}

void tampered_products_cannot_validate_or_export() {
    const auto pixel = tga(1U, 1U,
        std::array<std::array<std::uint8_t, 4U>, 1U>{{{{10U, 20U, 30U, 40U}}}});
    const std::array sources{TextureAtlasSource{"trim.one", pixel}};
    const TextureAtlasRecipe recipe{
        .width = 1U,
        .height = 1U,
        .background_rgba = {0U, 0U, 0U, 0U},
        .assignments = {TextureAtlasAssignment{"trim.one", 0U, 0U}},
    };
    auto product = build_texture_atlas(recipe, sources);
    require(static_cast<bool>(product), "valid atlas product created");
    product.value().rgba[0U] ^= 0xffU;
    require(!product.value().validate(), "tampered product pixels invalidate receipt digest");
    require(!product.value().export_tga(), "tampered product cannot be exported");
}

} // namespace

int main() {
    authored_layout_exports_deterministically_and_binds_inputs();
    invalid_assignment_and_payload_boundaries_fail_closed();
    tampered_products_cannot_validate_or_export();
    std::cout << "PASS deterministic texture atlas authoring boundaries\n";
    return EXIT_SUCCESS;
}
