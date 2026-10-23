#include <carto/assets/texture_atlas.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::assets {
namespace {

constexpr std::string_view kArtifactMagic = "CARTO_TEXTURE_ATLAS_RGBA8_V1";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

TextureSourceLimits bounded_limits(const TextureSourceLimits& requested) {
    auto limits = requested;
    limits.max_source_bytes = std::min(
        limits.max_source_bytes, TextureAtlasRecipe::kMaxAggregateSourceBytes);
    limits.max_dimension = std::min(
        limits.max_dimension, TextureAtlasRecipe::kMaxDimension);
    limits.max_pixels = std::min(limits.max_pixels, TextureAtlasRecipe::kMaxPixels);
    limits.max_mip_levels = std::min(limits.max_mip_levels, 1U);
    return limits;
}

bool valid_identity(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > TextureAtlasRecipe::kMaxIdentityBytes) {
        return false;
    }
    return std::all_of(identity.begin(), identity.end(), [](unsigned char value) {
        return value >= 0x21U && value <= 0x7eU;
    });
}

bool rectangles_overlap(
    std::uint32_t ax,
    std::uint32_t ay,
    std::uint32_t aw,
    std::uint32_t ah,
    std::uint32_t bx,
    std::uint32_t by,
    std::uint32_t bw,
    std::uint32_t bh) noexcept {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

void append_u32_le(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

void append_text(std::vector<std::uint8_t>& output, std::string_view value) {
    append_u32_le(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> canonical_artifact(const TextureAtlasProduct& product) {
    std::vector<std::uint8_t> bytes;
    std::size_t reserve = kArtifactMagic.size() + 4U * 3U +
        product.background_rgba.size() + product.rgba.size();
    for (const auto& assignment : product.assignments) {
        reserve += 4U + assignment.source_identity.size() + 4U * 4U + 32U;
    }
    bytes.reserve(reserve);
    bytes.insert(bytes.end(), kArtifactMagic.begin(), kArtifactMagic.end());
    bytes.push_back(0U);
    append_u32_le(bytes, product.width);
    append_u32_le(bytes, product.height);
    bytes.insert(bytes.end(), product.background_rgba.begin(),
        product.background_rgba.end());
    append_u32_le(bytes, static_cast<std::uint32_t>(product.assignments.size()));
    for (const auto& assignment : product.assignments) {
        append_text(bytes, assignment.source_identity);
        append_u32_le(bytes, assignment.x);
        append_u32_le(bytes, assignment.y);
        append_u32_le(bytes, assignment.width);
        append_u32_le(bytes, assignment.height);
        bytes.insert(bytes.end(), assignment.source_digest.bytes.begin(),
            assignment.source_digest.bytes.end());
    }
    bytes.insert(bytes.end(), product.rgba.begin(), product.rgba.end());
    return bytes;
}

core::Result<void> validate_atlas_extent(
    std::uint32_t width,
    std::uint32_t height,
    const TextureSourceLimits& limits) {
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (width == 0U || height == 0U || width > limits.max_dimension ||
        height > limits.max_dimension || pixels > limits.max_pixels ||
        pixels > TextureAtlasRecipe::kMaxPixels ||
        pixels > std::numeric_limits<std::size_t>::max() / 4U ||
        pixels * 4U > TextureAtlasRecipe::kMaxOutputBytes) {
        return core::Result<void>::failure(invalid(
            "atlas extent exceeds its non-zero dimension, pixel, or output byte budget"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_resolved_assignments(
    std::span<const ResolvedTextureAtlasAssignment> assignments,
    std::uint32_t atlas_width,
    std::uint32_t atlas_height) {
    std::set<std::string, std::less<>> identities;
    for (std::size_t index = 0U; index < assignments.size(); ++index) {
        const auto& current = assignments[index];
        if (!valid_identity(current.source_identity) ||
            !identities.insert(current.source_identity).second ||
            current.width == 0U || current.height == 0U ||
            current.x > atlas_width || current.y > atlas_height ||
            current.width > atlas_width - current.x ||
            current.height > atlas_height - current.y ||
            current.source_digest.is_zero()) {
            return core::Result<void>::failure(invalid(
                "atlas assignment has an invalid identity, digest, extent, or boundary"));
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            const auto& other = assignments[previous];
            if (rectangles_overlap(current.x, current.y, current.width, current.height,
                    other.x, other.y, other.width, other.height)) {
                return core::Result<void>::failure(invalid(
                    "atlas assignments overlap; explicit layouts may not overwrite pixels"));
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> TextureAtlasRecipe::validate(
    const TextureSourceLimits& requested_limits) const {
    if (auto result = requested_limits.validate(); !result) {
        return result;
    }
    const auto limits = bounded_limits(requested_limits);
    if (auto result = limits.validate(); !result) {
        return result;
    }
    if (auto result = validate_atlas_extent(width, height, limits); !result) {
        return result;
    }
    if (assignments.size() > kMaxAssignments) {
        return core::Result<void>::failure(invalid(
            "atlas assignment count exceeds its bounded limit"));
    }
    if (assignments.empty()) {
        return core::Result<void>::failure(invalid(
            "atlas recipe requires at least one explicit source assignment"));
    }
    std::set<std::string, std::less<>> identities;
    for (const auto& assignment : assignments) {
        if (!valid_identity(assignment.source_identity) ||
            !identities.insert(assignment.source_identity).second) {
            return core::Result<void>::failure(invalid(
                "atlas recipe contains an empty, malformed, or duplicate source identity"));
        }
    }
    return core::Result<void>::success();
}

core::Result<TextureAtlasProduct> build_texture_atlas(
    const TextureAtlasRecipe& recipe,
    std::span<const TextureAtlasSource> sources,
    const TextureSourceLimits& requested_limits) {
    if (auto result = requested_limits.validate(); !result) {
        return core::Result<TextureAtlasProduct>::failure(result.error());
    }
    const auto limits = bounded_limits(requested_limits);
    if (auto result = recipe.validate(limits); !result) {
        return core::Result<TextureAtlasProduct>::failure(result.error());
    }
    if (sources.size() != recipe.assignments.size()) {
        return core::Result<TextureAtlasProduct>::failure(invalid(
            "each supplied atlas source must have exactly one authored assignment"));
    }

    std::map<std::string, const TextureAtlasSource*, std::less<>> source_by_identity;
    std::uint64_t aggregate_source_bytes = 0U;
    for (const auto& source : sources) {
        if (!valid_identity(source.identity) ||
            !source_by_identity.emplace(source.identity, &source).second) {
            return core::Result<TextureAtlasProduct>::failure(invalid(
                "atlas source identities must be valid and unique"));
        }
        if (source.encoded_tga.size() >
            TextureAtlasRecipe::kMaxAggregateSourceBytes - aggregate_source_bytes) {
            return core::Result<TextureAtlasProduct>::failure(invalid(
                "atlas aggregate source byte budget is exceeded"));
        }
        aggregate_source_bytes += source.encoded_tga.size();
    }

    TextureAtlasProduct product{
        .width = recipe.width,
        .height = recipe.height,
        .background_rgba = recipe.background_rgba,
        .assignments = {},
        .rgba = {},
        .artifact_digest = {},
    };
    std::uint64_t atlas_bytes = 0U;
    for (const auto& authored : recipe.assignments) {
        const auto found = source_by_identity.find(authored.source_identity);
        if (found == source_by_identity.end()) {
            return core::Result<TextureAtlasProduct>::failure(invalid(
                "atlas assignment references a missing source identity"));
        }
        const auto& source = *found->second;
        auto inspected = inspect_texture_source(source.encoded_tga, "image/x-tga", limits);
        if (!inspected) {
            return core::Result<TextureAtlasProduct>::failure(inspected.error().with_context(
                "atlas source=" + authored.source_identity));
        }
        if (source.encoded_tga.size() < 18U) {
            return core::Result<TextureAtlasProduct>::failure(invalid(
                "atlas TGA source is shorter than its fixed header"));
        }
        if (source.encoded_tga[2U] != 2U) {
            return core::Result<TextureAtlasProduct>::failure(unsupported(
                "atlas compilation currently accepts uncompressed true-color TGA image type 2"));
        }
        if (inspected.value().bit_depth != 24U && inspected.value().bit_depth != 32U) {
            return core::Result<TextureAtlasProduct>::failure(unsupported(
                "atlas compilation requires 24/32-bit TGA, got bit depth " +
                std::to_string(inspected.value().bit_depth)));
        }
        const std::uint64_t source_pixel_bytes = inspected.value().bit_depth / 8U;
        const std::uint64_t expected_source_bytes = 18U + source.encoded_tga[0U] +
            static_cast<std::uint64_t>(inspected.value().width) *
                inspected.value().height * source_pixel_bytes;
        if (expected_source_bytes != source.encoded_tga.size()) {
            return core::Result<TextureAtlasProduct>::failure(invalid(
                "atlas TGA source contains trailing or incomplete pixel data"));
        }
        auto decoded = decode_tga_rgba8(source.encoded_tga, limits);
        if (!decoded) {
            return core::Result<TextureAtlasProduct>::failure(decoded.error().with_context(
                "atlas source=" + authored.source_identity));
        }
        const auto& admission = decoded.value().admission;
        if (authored.x > recipe.width || authored.y > recipe.height ||
            admission.width > recipe.width - authored.x ||
            admission.height > recipe.height - authored.y) {
            return core::Result<TextureAtlasProduct>::failure(invalid(
                "atlas source placement extends beyond the explicit atlas extent"));
        }
        if (admission.source.bytes >
            TextureAtlasRecipe::kMaxAggregateSourceBytes - atlas_bytes) {
            return core::Result<TextureAtlasProduct>::failure(invalid(
                "atlas aggregate admitted source byte budget is exceeded"));
        }
        atlas_bytes += admission.source.bytes;
        product.assignments.push_back(ResolvedTextureAtlasAssignment{
            .source_identity = authored.source_identity,
            .x = authored.x,
            .y = authored.y,
            .width = admission.width,
            .height = admission.height,
            .source_digest = admission.source.digest,
        });
    }
    if (auto result = validate_resolved_assignments(
            product.assignments, recipe.width, recipe.height); !result) {
        return core::Result<TextureAtlasProduct>::failure(result.error());
    }

    const std::uint64_t pixel_count = static_cast<std::uint64_t>(recipe.width) * recipe.height;
    product.rgba.resize(static_cast<std::size_t>(pixel_count) * 4U);
    for (std::size_t pixel = 0U; pixel < static_cast<std::size_t>(pixel_count); ++pixel) {
        std::copy(product.background_rgba.begin(), product.background_rgba.end(),
            product.rgba.begin() + static_cast<std::ptrdiff_t>(pixel * 4U));
    }
    for (std::size_t index = 0U; index < recipe.assignments.size(); ++index) {
        const auto& authored = recipe.assignments[index];
        const auto& resolved = product.assignments[index];
        const auto& source = *source_by_identity.at(authored.source_identity);
        auto decoded = decode_tga_rgba8(source.encoded_tga, limits);
        if (!decoded) {
            return core::Result<TextureAtlasProduct>::failure(decoded.error());
        }
        for (std::uint32_t row = 0U; row < resolved.height; ++row) {
            const std::size_t source_offset =
                static_cast<std::size_t>(row) * resolved.width * 4U;
            const std::size_t destination_offset =
                (static_cast<std::size_t>(resolved.y + row) * recipe.width + resolved.x) * 4U;
            std::copy_n(decoded.value().pixels.begin() +
                    static_cast<std::ptrdiff_t>(source_offset),
                static_cast<std::size_t>(resolved.width) * 4U,
                product.rgba.begin() + static_cast<std::ptrdiff_t>(destination_offset));
        }
    }
    product.artifact_digest = sha256(canonical_artifact(product));
    if (auto result = product.validate(); !result) {
        return core::Result<TextureAtlasProduct>::failure(result.error());
    }
    return core::Result<TextureAtlasProduct>::success(std::move(product));
}

core::Result<void> TextureAtlasProduct::validate() const {
    if (width == 0U || height == 0U || width > TextureAtlasRecipe::kMaxDimension ||
        height > TextureAtlasRecipe::kMaxDimension) {
        return core::Result<void>::failure(invalid("texture atlas product extent is invalid"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > TextureAtlasRecipe::kMaxPixels ||
        pixels > std::numeric_limits<std::size_t>::max() / 4U ||
        pixels * 4U > TextureAtlasRecipe::kMaxOutputBytes ||
        rgba.size() != static_cast<std::size_t>(pixels) * 4U ||
        assignments.size() > TextureAtlasRecipe::kMaxAssignments) {
        return core::Result<void>::failure(invalid(
            "texture atlas product exceeds its pixel, assignment, or byte budget"));
    }
    if (auto result = validate_resolved_assignments(assignments, width, height); !result) {
        return result;
    }
    if (artifact_digest.is_zero() || sha256(canonical_artifact(*this)) != artifact_digest) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "texture atlas product digest does not match its receipt and output pixels"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> TextureAtlasProduct::export_tga() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> output(18U, 0U);
    output[2U] = 2U;
    output[12U] = static_cast<std::uint8_t>(width & 0xffU);
    output[13U] = static_cast<std::uint8_t>((width >> 8U) & 0xffU);
    output[14U] = static_cast<std::uint8_t>(height & 0xffU);
    output[15U] = static_cast<std::uint8_t>((height >> 8U) & 0xffU);
    output[16U] = 32U;
    output[17U] = 0x28U; // top-left origin and authored 8-bit alpha
    output.reserve(18U + rgba.size());
    for (std::size_t pixel = 0U; pixel < rgba.size(); pixel += 4U) {
        output.push_back(rgba[pixel + 2U]);
        output.push_back(rgba[pixel + 1U]);
        output.push_back(rgba[pixel]);
        output.push_back(rgba[pixel + 3U]);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(output));
}

core::Result<void> TextureAtlasProduct::validate_against(
    const TextureAtlasRecipe& recipe,
    std::span<const TextureAtlasSource> sources,
    const TextureSourceLimits& limits) const {
    const auto expected = build_texture_atlas(recipe, sources, limits);
    if (!expected) {
        return core::Result<void>::failure(expected.error());
    }
    if (width != expected.value().width || height != expected.value().height ||
        background_rgba != expected.value().background_rgba ||
        assignments != expected.value().assignments || rgba != expected.value().rgba ||
        artifact_digest != expected.value().artifact_digest) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "texture atlas product differs from its exact layout, assignments, or inputs"));
    }
    return core::Result<void>::success();
}

} // namespace carto::assets
