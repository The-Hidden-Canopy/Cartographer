#include <carto/assets/blob_store.hpp>
#include <carto/assets/texture_image_decoder.hpp>
#include <carto/assets/texture_source.hpp>
#include <carto/d3d12/scene_renderer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <locale>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::d3d12 {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::uint64_t kMaxEnvironmentImageBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxEnvironmentImagePixels = 16'777'216ULL;
constexpr std::uint64_t kMaxSpecularEnvironmentPixels = 4'194'304ULL;
constexpr std::uint32_t kMaxEnvironmentImageDimension = 16'384U;
constexpr std::size_t kMaxRadianceHeaderBytes = 64U * 1024U;
constexpr double kMaxRadianceHeaderScale = 1.0e12;

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

double decode_channel(std::uint8_t encoded, assets::ColorSpace color_space) {
    const double value = static_cast<double>(encoded) / 255.0;
    if (color_space == assets::ColorSpace::linear) return value;
    if (value <= 0.04045) return value / 12.92;
    return std::pow((value + 0.055) / 1.055, 2.4);
}

bool radiance_signature(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::string_view radiance = "#?RADIANCE";
    constexpr std::string_view rgbe = "#?RGBE";
    const auto starts_with = [bytes](std::string_view prefix) {
        return bytes.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), bytes.begin(),
                [](char expected, std::uint8_t actual) {
                    return static_cast<std::uint8_t>(expected) == actual;
                });
    };
    return starts_with(radiance) || starts_with(rgbe);
}

bool read_line(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::string_view& line) noexcept {
    if (cursor >= bytes.size()) return false;
    const std::size_t begin = cursor;
    while (cursor < bytes.size() && bytes[cursor] != static_cast<std::uint8_t>('\n')) {
        if (cursor - begin >= kMaxRadianceHeaderBytes) return false;
        ++cursor;
    }
    if (cursor >= bytes.size()) return false;
    std::size_t end = cursor++;
    if (end > begin && bytes[end - 1U] == static_cast<std::uint8_t>('\r')) --end;
    line = std::string_view(
        reinterpret_cast<const char*>(bytes.data() + begin), end - begin);
    return true;
}

bool parse_positive_finite(std::string_view text, double& value) noexcept {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        std::isfinite(value) && value > 0.0;
}

core::Result<std::uint32_t> parse_dimension(std::string_view token) {
    std::uint32_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value == 0U) {
        return core::Result<std::uint32_t>::failure(invalid(
            "Radiance environment resolution contains an invalid dimension"));
    }
    return core::Result<std::uint32_t>::success(value);
}

void accumulate_radiance_sample(
    std::array<core::Vec3d, 9U>& coefficients,
    double sin_theta,
    double cos_theta,
    double pixel_solid_angle,
    const std::array<double, 2U>& longitude_direction,
    core::Vec3d radiance) noexcept {
    const double direction_x = sin_theta * longitude_direction[0U];
    const double direction_y = cos_theta;
    const double direction_z = sin_theta * longitude_direction[1U];
    const std::array<double, 9U> basis{
        0.282095,
        0.488603 * direction_y,
        0.488603 * direction_z,
        0.488603 * direction_x,
        1.092548 * direction_x * direction_y,
        1.092548 * direction_y * direction_z,
        0.315392 * (3.0 * direction_z * direction_z - 1.0),
        1.092548 * direction_x * direction_z,
        0.546274 * (direction_x * direction_x - direction_y * direction_y)};
    for (std::size_t coefficient = 0U; coefficient < basis.size(); ++coefficient) {
        const double weight = basis[coefficient] * pixel_solid_angle;
        coefficients[coefficient].x += radiance.x * weight;
        coefficients[coefficient].y += radiance.y * weight;
        coefficients[coefficient].z += radiance.z * weight;
    }
}

std::uint32_t reverse_bits(std::uint32_t value) noexcept {
    value = (value << 16U) | (value >> 16U);
    value = ((value & 0x55555555U) << 1U) | ((value >> 1U) & 0x55555555U);
    value = ((value & 0x33333333U) << 2U) | ((value >> 2U) & 0x33333333U);
    value = ((value & 0x0f0f0f0fU) << 4U) | ((value >> 4U) & 0x0f0f0f0fU);
    value = ((value & 0x00ff00ffU) << 8U) | ((value >> 8U) & 0x00ff00ffU);
    return value;
}

std::array<double, 3U> sample_linear_panorama(
    std::span<const std::array<float, 3U>> pixels,
    std::uint32_t width,
    std::uint32_t height,
    core::Vec3d direction) noexcept {
    const double longitude = 0.5 + std::atan2(direction.x, direction.z) /
        (2.0 * kPi);
    const double u = longitude - std::floor(longitude);
    const double v = std::acos(std::clamp(direction.y, -1.0, 1.0)) / kPi;
    const double pixel_x = u * width - 0.5;
    const double pixel_y = v * height - 0.5;
    const double floor_x = std::floor(pixel_x);
    const double floor_y = std::floor(pixel_y);
    const auto wrap_x = [width](std::int64_t x) {
        const std::int64_t signed_width = static_cast<std::int64_t>(width);
        return static_cast<std::uint32_t>((x % signed_width + signed_width) % signed_width);
    };
    const auto clamp_y = [height](std::int64_t y) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            y, 0, static_cast<std::int64_t>(height) - 1));
    };
    const auto x0 = wrap_x(static_cast<std::int64_t>(floor_x));
    const auto x1 = wrap_x(static_cast<std::int64_t>(floor_x) + 1);
    const auto y0 = clamp_y(static_cast<std::int64_t>(floor_y));
    const auto y1 = clamp_y(static_cast<std::int64_t>(floor_y) + 1);
    const double blend_x = pixel_x - floor_x;
    const double blend_y = pixel_y - floor_y;
    const auto at = [pixels, width](std::uint32_t x, std::uint32_t y,
                                    std::size_t channel) {
        return static_cast<double>(pixels[static_cast<std::size_t>(y) * width + x][channel]);
    };
    std::array<double, 3U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        const double top = at(x0, y0, channel) * (1.0 - blend_x) +
            at(x1, y0, channel) * blend_x;
        const double bottom = at(x0, y1, channel) * (1.0 - blend_x) +
            at(x1, y1, channel) * blend_x;
        result[channel] = top * (1.0 - blend_y) + bottom * blend_y;
    }
    return result;
}

core::Result<SceneRenderEnvironmentPrefilteredMipChain>
build_ggx_environment_mips(
    assets::Sha256Digest source_digest,
    assets::ColorSpace source_color_space,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::array<float, 3U>> base_pixels) {
    using Output = core::Result<SceneRenderEnvironmentPrefilteredMipChain>;
    constexpr std::size_t kGgxSampleCount =
        kSceneRenderEnvironmentPrefilterSampleCount;
    constexpr std::uint64_t kMipPayloadLimit = 64ULL * 1024ULL * 1024ULL;
    if (source_digest.is_zero() || width < 16U || height < 8U ||
        static_cast<std::uint64_t>(width) != static_cast<std::uint64_t>(height) * 2U ||
        static_cast<std::uint64_t>(width) * height > kMaxSpecularEnvironmentPixels ||
        base_pixels.size() != static_cast<std::size_t>(width) * height ||
        (source_color_space != assets::ColorSpace::linear &&
         source_color_space != assets::ColorSpace::srgb)) {
        return Output::failure(invalid(
            "GGX environment source identity, color space, or dimensions are invalid"));
    }
    std::vector<std::array<std::uint32_t, 2U>> dimensions;
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    std::uint64_t total_pixels = 0U;
    while (true) {
        const std::uint64_t level_pixels =
            static_cast<std::uint64_t>(level_width) * level_height;
        if (level_pixels > (kMipPayloadLimit / sizeof(std::array<float, 3U>)) -
                total_pixels) {
            return Output::failure(invalid(
                "GGX environment mip payload exceeds 64 MiB"));
        }
        total_pixels += level_pixels;
        dimensions.push_back({level_width, level_height});
        if (level_width == 1U && level_height == 1U) break;
        level_width = std::max(1U, level_width / 2U);
        level_height = std::max(1U, level_height / 2U);
    }

    std::array<std::array<double, 2U>, kGgxSampleCount> samples{};
    for (std::uint32_t sample = 0U; sample < kGgxSampleCount; ++sample) {
        samples[sample] = {
            static_cast<double>(sample) / static_cast<double>(kGgxSampleCount),
            static_cast<double>(reverse_bits(sample)) / 4294967296.0};
    }

    SceneRenderEnvironmentPrefilteredMipChain chain;
    chain.source_digest = source_digest;
    chain.source_color_space = source_color_space;
    chain.levels.reserve(dimensions.size());
    chain.levels.push_back({width, height, std::move(base_pixels)});
    const auto& source_pixels = chain.levels.front().linear_rgb;
    for (std::size_t level_index = 1U; level_index < dimensions.size(); ++level_index) {
        const std::uint32_t destination_width = dimensions[level_index][0U];
        const std::uint32_t destination_height = dimensions[level_index][1U];
        SceneRenderEnvironmentMipLevel destination;
        destination.width = destination_width;
        destination.height = destination_height;
        destination.linear_rgb.resize(
            static_cast<std::size_t>(destination_width) * destination_height);
        const double roughness = static_cast<double>(level_index) /
            static_cast<double>(dimensions.size() - 1U);
        const double alpha = roughness * roughness;
        const double alpha_squared = alpha * alpha;
        std::array<std::array<double, 3U>, kGgxSampleCount> local_half_vectors{};
        for (std::size_t sample_index = 0U; sample_index < samples.size(); ++sample_index) {
            const double sample_phi = 2.0 * kPi * samples[sample_index][0U];
            const double denominator = 1.0 +
                (alpha_squared - 1.0) * samples[sample_index][1U];
            const double cos_half = std::sqrt(std::clamp(
                (1.0 - samples[sample_index][1U]) / denominator, 0.0, 1.0));
            const double sin_half = std::sqrt(std::max(0.0, 1.0 - cos_half * cos_half));
            local_half_vectors[sample_index] = {
                std::cos(sample_phi) * sin_half,
                std::sin(sample_phi) * sin_half,
                cos_half};
        }
        for (std::uint32_t y = 0U; y < destination_height; ++y) {
            const double theta = kPi *
                (static_cast<double>(y) + 0.5) / destination_height;
            const double sin_theta = std::sin(theta);
            const double cos_theta = std::cos(theta);
            for (std::uint32_t x = 0U; x < destination_width; ++x) {
                const double phi = 2.0 * kPi *
                    ((static_cast<double>(x) + 0.5) / destination_width - 0.5);
                const core::Vec3d normal{
                    sin_theta * std::sin(phi), cos_theta,
                    sin_theta * std::cos(phi)};
                const core::Vec3d axis = std::abs(normal.y) < 0.999
                    ? core::Vec3d{0.0, 1.0, 0.0}
                    : core::Vec3d{1.0, 0.0, 0.0};
                const core::Vec3d tangent = core::cross(axis, normal).normalized();
                const core::Vec3d bitangent = core::cross(normal, tangent);
                std::array<double, 3U> weighted_radiance{};
                double weight_sum = 0.0;
                for (const auto& sample : local_half_vectors) {
                    const core::Vec3d half_vector{
                        tangent.x * sample[0U] + bitangent.x * sample[1U] + normal.x * sample[2U],
                        tangent.y * sample[0U] + bitangent.y * sample[1U] + normal.y * sample[2U],
                        tangent.z * sample[0U] + bitangent.z * sample[1U] + normal.z * sample[2U]};
                    const double view_dot_half = std::max(0.0,
                        normal.x * half_vector.x + normal.y * half_vector.y +
                        normal.z * half_vector.z);
                    const core::Vec3d light{
                        2.0 * view_dot_half * half_vector.x - normal.x,
                        2.0 * view_dot_half * half_vector.y - normal.y,
                        2.0 * view_dot_half * half_vector.z - normal.z};
                    const double normal_dot_light = std::max(0.0,
                        normal.x * light.x + normal.y * light.y + normal.z * light.z);
                    if (normal_dot_light <= 0.0) continue;
                    const auto radiance = sample_linear_panorama(
                        source_pixels, width, height, light);
                    for (std::size_t channel = 0U; channel < weighted_radiance.size(); ++channel)
                        weighted_radiance[channel] += radiance[channel] * normal_dot_light;
                    weight_sum += normal_dot_light;
                }
                auto& output = destination.linear_rgb[
                    static_cast<std::size_t>(y) * destination_width + x];
                if (weight_sum <= 1.0e-12) {
                    const auto fallback = sample_linear_panorama(
                        source_pixels, width, height, normal);
                    for (std::size_t channel = 0U; channel < output.size(); ++channel)
                        output[channel] = static_cast<float>(fallback[channel]);
                } else {
                    for (std::size_t channel = 0U; channel < output.size(); ++channel)
                        output[channel] = static_cast<float>(weighted_radiance[channel] /
                            weight_sum);
                }
            }
        }
        chain.levels.push_back(std::move(destination));
    }
    if (auto refreshed = chain.refresh_digest(); !refreshed)
        return Output::failure(refreshed.error());
    return Output::success(std::move(chain));
}

core::Result<SceneRenderEnvironment> decode_radiance_hdr(
    std::span<const std::uint8_t> encoded) {
    using Output = core::Result<SceneRenderEnvironment>;
    std::size_t cursor = 0U;
    std::string_view line;
    if (!read_line(encoded, cursor, line) ||
        (line != "#?RADIANCE" && line != "#?RGBE")) {
        return Output::failure(invalid("Radiance environment header is malformed"));
    }

    bool format_seen = false;
    double exposure = 1.0;
    core::Vec3d color_correction{1.0, 1.0, 1.0};
    std::size_t header_bytes = 0U;
    bool header_ended = false;
    while (cursor < encoded.size()) {
        const std::size_t line_start = cursor;
        if (!read_line(encoded, cursor, line)) break;
        header_bytes += cursor - line_start;
        if (header_bytes > kMaxRadianceHeaderBytes) {
            return Output::failure(invalid("Radiance environment header exceeds 64 KiB"));
        }
        if (line.empty()) {
            header_ended = true;
            break;
        }
        if (line == "FORMAT=32-bit_rle_rgbe") {
            if (format_seen) {
                return Output::failure(invalid("Radiance environment repeats its pixel format"));
            }
            format_seen = true;
            continue;
        }
        constexpr std::string_view exposure_prefix = "EXPOSURE=";
        if (line.starts_with(exposure_prefix)) {
            double multiplier = 0.0;
            if (!parse_positive_finite(line.substr(exposure_prefix.size()), multiplier) ||
                multiplier > kMaxRadianceHeaderScale ||
                exposure > kMaxRadianceHeaderScale / multiplier) {
                return Output::failure(invalid(
                    "Radiance environment exposure is invalid or exceeds its bound"));
            }
            exposure *= multiplier;
            if (!std::isfinite(exposure) || exposure <= 0.0) {
                return Output::failure(invalid(
                    "Radiance environment exposure underflows its supported range"));
            }
            continue;
        }
        constexpr std::string_view color_prefix = "COLORCORR=";
        if (line.starts_with(color_prefix)) {
            std::istringstream values{std::string(line.substr(color_prefix.size()))};
            values.imbue(std::locale::classic());
            core::Vec3d correction;
            std::string trailing;
            if (!(values >> correction.x >> correction.y >> correction.z) ||
                (values >> trailing) || !correction.finite() ||
                correction.x <= 0.0 || correction.y <= 0.0 || correction.z <= 0.0 ||
                correction.x > kMaxRadianceHeaderScale ||
                correction.y > kMaxRadianceHeaderScale ||
                correction.z > kMaxRadianceHeaderScale ||
                color_correction.x > kMaxRadianceHeaderScale / correction.x ||
                color_correction.y > kMaxRadianceHeaderScale / correction.y ||
                color_correction.z > kMaxRadianceHeaderScale / correction.z) {
                return Output::failure(invalid(
                    "Radiance environment color correction is invalid or exceeds its bound"));
            }
            color_correction.x *= correction.x;
            color_correction.y *= correction.y;
            color_correction.z *= correction.z;
            if (!color_correction.finite() || color_correction.x <= 0.0 ||
                color_correction.y <= 0.0 || color_correction.z <= 0.0) {
                return Output::failure(invalid(
                    "Radiance environment color correction underflows its supported range"));
            }
            continue;
        }
        constexpr std::string_view gamma_prefix = "GAMMA=";
        if (line.starts_with(gamma_prefix)) {
            double gamma = 0.0;
            if (!parse_positive_finite(line.substr(gamma_prefix.size()), gamma) || gamma != 1.0) {
                return Output::failure(invalid(
                    "Radiance environment supports only linear GAMMA=1"));
            }
            continue;
        }
        constexpr std::string_view pixel_aspect_prefix = "PIXASPECT=";
        if (line.starts_with(pixel_aspect_prefix)) {
            double aspect = 0.0;
            if (!parse_positive_finite(line.substr(pixel_aspect_prefix.size()), aspect) ||
                aspect != 1.0) {
                return Output::failure(invalid(
                    "Radiance environment requires PIXASPECT=1"));
            }
            continue;
        }
        if (line.starts_with("PRIMARIES=")) {
            return Output::failure(invalid(
                "Radiance environment explicit primaries metadata is not supported"));
        }
        if (line.starts_with("SOFTWARE=") || line.starts_with("VIEW=") ||
            line.starts_with("HOST=") || line.starts_with("DESCRIPTION=") ||
            line.starts_with("DATE=") || line.starts_with("COMMAND=") ||
            line.starts_with("#")) {
            continue;
        }
        return Output::failure(invalid(
            "Radiance environment contains an unsupported header field"));
    }
    if (!header_ended || !format_seen || !read_line(encoded, cursor, line)) {
        return Output::failure(invalid(
            "Radiance environment requires a supported format, blank line, and resolution"));
    }

    std::istringstream resolution{std::string(line)};
    resolution.imbue(std::locale::classic());
    std::string y_axis;
    std::string y_token;
    std::string x_axis;
    std::string x_token;
    std::string trailing;
    if (!(resolution >> y_axis >> y_token >> x_axis >> x_token) ||
        (resolution >> trailing) || y_axis.size() != 2U || x_axis.size() != 2U ||
        (y_axis[0U] != '+' && y_axis[0U] != '-') || y_axis[1U] != 'Y' ||
        (x_axis[0U] != '+' && x_axis[0U] != '-') || x_axis[1U] != 'X') {
        return Output::failure(invalid(
            "Radiance environment resolution must use Y then X axes"));
    }
    const auto height = parse_dimension(y_token);
    const auto width = parse_dimension(x_token);
    if (!height || !width) {
        return Output::failure(!height ? height.error() : width.error());
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width.value()) * height.value();
    if (width.value() > kMaxEnvironmentImageDimension ||
        height.value() > kMaxEnvironmentImageDimension ||
        pixels > kMaxEnvironmentImagePixels ||
        static_cast<std::uint64_t>(width.value()) !=
            static_cast<std::uint64_t>(height.value()) * 2U) {
        return Output::failure(invalid(
            "Radiance environment dimensions must be bounded and 2:1"));
    }
    const bool hdr_specular_eligible = pixels <= kMaxSpecularEnvironmentPixels &&
        width.value() >= 16U && height.value() >= 8U;
    bool hdr_specular_valid = hdr_specular_eligible;
    std::vector<std::array<float, 3U>> hdr_specular_base;
    if (hdr_specular_eligible)
        hdr_specular_base.resize(static_cast<std::size_t>(pixels));

    const double longitude_step = 2.0 * kPi / static_cast<double>(width.value());
    std::vector<std::array<double, 2U>> longitude_directions(width.value());
    for (std::uint32_t x = 0U; x < width.value(); ++x) {
        const double phi = 2.0 * kPi *
            ((static_cast<double>(x) + 0.5) / static_cast<double>(width.value()) - 0.5);
        longitude_directions[x] = {std::sin(phi), std::cos(phi)};
    }
    std::array<core::Vec3d, 9U> coefficients{};
    std::vector<std::array<std::uint8_t, 4U>> scanline(width.value());
    // Modern RLE is identified by its width-tagged scanline marker. Otherwise
    // Radiance uses the legacy image-wide stream of RGBE literals and repeats.
    const bool modern_rle = width.value() >= 8U && encoded.size() - cursor >= 4U &&
        encoded[cursor] == 2U && encoded[cursor + 1U] == 2U &&
        (encoded[cursor + 2U] & 0x80U) == 0U &&
        ((static_cast<std::uint32_t>(encoded[cursor + 2U]) << 8U) |
            encoded[cursor + 3U]) == width.value();
    std::array<std::uint8_t, 4U> legacy_previous{};
    bool legacy_has_previous = false;
    std::uint64_t legacy_repeat_remaining = 0U;
    std::uint32_t legacy_repeat_shift = 0U;
    std::uint64_t legacy_pixels_decoded = 0U;
    for (std::uint32_t row = 0U; row < height.value(); ++row) {
        if (modern_rle) {
            if (encoded.size() - cursor < 4U || encoded[cursor] != 2U ||
                encoded[cursor + 1U] != 2U || (encoded[cursor + 2U] & 0x80U) != 0U) {
                return Output::failure(invalid(
                    "Radiance environment scanline is not bounded RGBE RLE"));
            }
            const std::uint32_t encoded_width =
                (static_cast<std::uint32_t>(encoded[cursor + 2U]) << 8U) |
                encoded[cursor + 3U];
            cursor += 4U;
            if (encoded_width != width.value()) {
                return Output::failure(invalid(
                    "Radiance environment scanline width does not match its header"));
            }
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                std::size_t x = 0U;
                while (x < width.value()) {
                    if (cursor >= encoded.size()) {
                        return Output::failure(invalid(
                            "Radiance environment scanline data is truncated"));
                    }
                    const std::uint8_t packet = encoded[cursor++];
                    if (packet == 0U) {
                        return Output::failure(invalid(
                            "Radiance environment scanline contains a zero-length packet"));
                    }
                    if (packet > 128U) {
                        const std::size_t run = static_cast<std::size_t>(packet - 128U);
                        if (run > width.value() - x || cursor >= encoded.size()) {
                            return Output::failure(invalid(
                                "Radiance environment run exceeds its scanline"));
                        }
                        const std::uint8_t value = encoded[cursor++];
                        for (std::size_t index = 0U; index < run; ++index) {
                            scanline[x + index][channel] = value;
                        }
                        x += run;
                    } else {
                        const std::size_t literal = packet;
                        if (literal > width.value() - x ||
                            encoded.size() - cursor < literal) {
                            return Output::failure(invalid(
                                "Radiance environment literal exceeds its scanline"));
                        }
                        for (std::size_t index = 0U; index < literal; ++index) {
                            scanline[x + index][channel] = encoded[cursor + index];
                        }
                        cursor += literal;
                        x += literal;
                    }
                }
            }
        } else {
            std::size_t x = 0U;
            while (x < width.value()) {
                if (legacy_repeat_remaining != 0U) {
                    scanline[x++] = legacy_previous;
                    --legacy_repeat_remaining;
                    ++legacy_pixels_decoded;
                    continue;
                }
                if (encoded.size() - cursor < 4U) {
                    return Output::failure(invalid(
                        "Radiance environment legacy RGBE data is truncated"));
                }
                const std::array<std::uint8_t, 4U> pixel{
                    encoded[cursor], encoded[cursor + 1U],
                    encoded[cursor + 2U], encoded[cursor + 3U]};
                cursor += 4U;
                if (pixel[0U] == 1U && pixel[1U] == 1U && pixel[2U] == 1U) {
                    if (!legacy_has_previous || legacy_repeat_shift >= 32U) {
                        return Output::failure(invalid(
                            "Radiance environment legacy RGBE run has no valid previous pixel"));
                    }
                    const std::uint64_t run =
                        static_cast<std::uint64_t>(pixel[3U]) << legacy_repeat_shift;
                    const std::uint64_t remaining_pixels = pixels - legacy_pixels_decoded;
                    if (run > remaining_pixels) {
                        return Output::failure(invalid(
                            "Radiance environment legacy RGBE run exceeds the image"));
                    }
                    legacy_repeat_shift += 8U;
                    legacy_repeat_remaining = run;
                    if (legacy_repeat_remaining == 0U) continue;
                    scanline[x++] = legacy_previous;
                    --legacy_repeat_remaining;
                    ++legacy_pixels_decoded;
                } else {
                    legacy_previous = pixel;
                    legacy_has_previous = true;
                    legacy_repeat_shift = 0U;
                    scanline[x++] = pixel;
                    ++legacy_pixels_decoded;
                }
            }
        }
        const std::uint32_t y = y_axis[0U] == '-' ? row : height.value() - 1U - row;
        const double theta_top = kPi * static_cast<double>(y) /
            static_cast<double>(height.value());
        const double theta_bottom = kPi * static_cast<double>(y + 1U) /
            static_cast<double>(height.value());
        const double theta = (theta_top + theta_bottom) * 0.5;
        const double sin_theta = std::sin(theta);
        const double cos_theta = std::cos(theta);
        const double pixel_solid_angle = longitude_step *
            (std::cos(theta_top) - std::cos(theta_bottom));
        for (std::uint32_t sample_x = 0U; sample_x < width.value(); ++sample_x) {
            const std::uint32_t x = x_axis[0U] == '+'
                ? sample_x : width.value() - 1U - sample_x;
            const auto& rgbe = scanline[sample_x];
            core::Vec3d radiance{};
            if (rgbe[3U] != 0U) {
                const double scale = std::ldexp(
                    1.0, static_cast<int>(rgbe[3U]) - 136) * exposure;
                radiance = {
                    static_cast<double>(rgbe[0U]) * scale * color_correction.x,
                    static_cast<double>(rgbe[1U]) * scale * color_correction.y,
                    static_cast<double>(rgbe[2U]) * scale * color_correction.z};
            }
            if (!radiance.finite()) {
                return Output::failure(invalid(
                    "Radiance environment decodes to a non-finite radiance value"));
            }
            accumulate_radiance_sample(coefficients, sin_theta, cos_theta,
                pixel_solid_angle, longitude_directions[x], radiance);
            if (hdr_specular_valid) {
                if (radiance.x > 1.0e12 || radiance.y > 1.0e12 || radiance.z > 1.0e12 ||
                    radiance.x > std::numeric_limits<float>::max() ||
                    radiance.y > std::numeric_limits<float>::max() ||
                    radiance.z > std::numeric_limits<float>::max()) {
                    hdr_specular_valid = false;
                } else {
                    hdr_specular_base[static_cast<std::size_t>(y) * width.value() + x] = {
                        static_cast<float>(radiance.x), static_cast<float>(radiance.y),
                        static_cast<float>(radiance.z)};
                }
            }
        }
    }
    if (cursor != encoded.size()) {
        return Output::failure(invalid(
            "Radiance environment contains trailing pixel data"));
    }

    SceneRenderEnvironment environment;
    environment.source_digest = assets::sha256(encoded);
    environment.source_color_space = assets::ColorSpace::linear;
    for (std::size_t coefficient = 0U; coefficient < coefficients.size(); ++coefficient) {
        const double lambert_over_pi = coefficient == 0U ? 1.0 :
            coefficient < 4U ? 2.0 / 3.0 : 0.25;
        environment.diffuse_irradiance_over_pi_sh[coefficient] =
            coefficients[coefficient] * lambert_over_pi;
    }
    if (hdr_specular_valid) {
        const auto hdr_mips = build_ggx_environment_mips(environment.source_digest,
            assets::ColorSpace::linear, width.value(), height.value(),
            std::move(hdr_specular_base));
        if (!hdr_mips) return Output::failure(hdr_mips.error().with_context(
            "GGX-prefiltered HDR environment generation"));
        environment.specular_environment_mips = hdr_mips.value();
    }
    if (auto valid = environment.validate(); !valid) {
        return Output::failure(valid.error());
    }
    return Output::success(std::move(environment));
}

} // namespace

core::Result<SceneRenderEnvironment> load_scene_render_environment_equirectangular(
    const std::filesystem::path& path,
    assets::ColorSpace color_space) {
    using Output = core::Result<SceneRenderEnvironment>;
    if (color_space != assets::ColorSpace::linear &&
        color_space != assets::ColorSpace::srgb) {
        return Output::failure(invalid(
            "environment image color space must be explicitly linear or sRGB"));
    }

    assets::TextureSourceLimits limits;
    limits.max_source_bytes = kMaxEnvironmentImageBytes;
    limits.max_dimension = kMaxEnvironmentImageDimension;
    limits.max_pixels = kMaxEnvironmentImagePixels;

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return Output::failure({core::ErrorCode::io_error,
            "environment image is missing or unavailable"});
    }
    const auto file_size = input.tellg();
    if (file_size <= 0 ||
        file_size > static_cast<std::streamoff>(limits.max_source_bytes)) {
        return Output::failure({core::ErrorCode::io_error,
            "environment image is missing, empty, or exceeds 256 MiB"});
    }
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(file_size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(encoded.data()),
        static_cast<std::streamsize>(encoded.size()));
    if (!input) {
        return Output::failure({core::ErrorCode::io_error,
            "environment image read failed"});
    }

    const std::span<const std::uint8_t> encoded_span{encoded.data(), encoded.size()};
    if (radiance_signature(encoded_span)) {
        if (color_space != assets::ColorSpace::linear) {
            return Output::failure(invalid(
                "Radiance RGBE environments are linear; request color space must be linear"));
        }
        return decode_radiance_hdr(encoded_span);
    }

    const auto admission = assets::inspect_texture_source(encoded, {}, limits);
    if (!admission) return Output::failure(admission.error().with_context(
        "environment image admission"));
    if (admission.value().has_color_encoding_metadata) {
        return Output::failure({core::ErrorCode::unsupported,
            "environment panorama color metadata is unsupported by the explicit color-space conversion path"});
    }
    if (static_cast<std::uint64_t>(admission.value().width) !=
        static_cast<std::uint64_t>(admission.value().height) * 2U) {
        return Output::failure(invalid(
            "environment image must be a 2:1 equirectangular panorama"));
    }
    const auto source_digest = admission.value().source.digest;
    const auto decoded = assets::decode_texture_image_rgba8(std::move(encoded),
        source_digest, admission.value().width, admission.value().height, limits);
    if (!decoded) return Output::failure(decoded.error().with_context(
        "environment image pixel decode"));

    const std::uint32_t width = decoded.value().admission.width;
    const std::uint32_t height = decoded.value().admission.height;
    const std::uint64_t panorama_pixels =
        static_cast<std::uint64_t>(width) * height;
    const bool specular_eligible = panorama_pixels <= kMaxSpecularEnvironmentPixels &&
        width >= 16U && height >= 8U;
    std::vector<std::array<float, 3U>> specular_base;
    if (specular_eligible)
        specular_base.resize(static_cast<std::size_t>(panorama_pixels));
    const double longitude_step = 2.0 * kPi / static_cast<double>(width);
    std::array<double, 256U> linear_channels{};
    for (std::size_t channel = 0U; channel < linear_channels.size(); ++channel) {
        linear_channels[channel] = decode_channel(
            static_cast<std::uint8_t>(channel), color_space);
    }
    std::vector<std::array<double, 2U>> longitude_directions(width);
    for (std::uint32_t x = 0U; x < width; ++x) {
        const double phi = 2.0 * kPi *
            ((static_cast<double>(x) + 0.5) / static_cast<double>(width) - 0.5);
        longitude_directions[x] = {std::sin(phi), std::cos(phi)};
    }
    std::array<core::Vec3d, 9U> radiance_coefficients{};
    const auto& pixels = decoded.value().pixels;
    for (std::uint32_t y = 0U; y < height; ++y) {
        const double theta_top = kPi * static_cast<double>(y) /
            static_cast<double>(height);
        const double theta_bottom = kPi * static_cast<double>(y + 1U) /
            static_cast<double>(height);
        const double theta = (theta_top + theta_bottom) * 0.5;
        const double sin_theta = std::sin(theta);
        const double cos_theta = std::cos(theta);
        const double pixel_solid_angle = longitude_step *
            (std::cos(theta_top) - std::cos(theta_bottom));
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t pixel_offset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            if (pixels[pixel_offset + 3U] != 255U) {
                return Output::failure(invalid(
                    "environment panorama must have fully opaque pixels"));
            }
            const core::Vec3d radiance{
                linear_channels[pixels[pixel_offset]],
                linear_channels[pixels[pixel_offset + 1U]],
                linear_channels[pixel_offset + 2U]};
            if (specular_eligible) {
                specular_base[static_cast<std::size_t>(y) * width + x] = {
                    static_cast<float>(radiance.x), static_cast<float>(radiance.y),
                    static_cast<float>(radiance.z)};
            }
            accumulate_radiance_sample(radiance_coefficients, sin_theta, cos_theta,
                pixel_solid_angle, longitude_directions[x], radiance);
        }
    }

    SceneRenderEnvironment environment;
    environment.source_digest = source_digest;
    environment.source_color_space = color_space;
    for (std::size_t coefficient = 0U; coefficient < radiance_coefficients.size();
         ++coefficient) {
        const double lambert_over_pi = coefficient == 0U ? 1.0 :
            coefficient < 4U ? 2.0 / 3.0 : 0.25;
        environment.diffuse_irradiance_over_pi_sh[coefficient] =
            radiance_coefficients[coefficient] * lambert_over_pi;
    }
    if (specular_eligible) {
        const auto mips = build_ggx_environment_mips(source_digest, color_space,
            width, height, std::move(specular_base));
        if (!mips) return Output::failure(mips.error().with_context(
            "GGX-prefiltered environment generation"));
        environment.specular_environment_mips = mips.value();
    }
    if (auto valid = environment.validate(); !valid) {
        return Output::failure(valid.error());
    }
    return Output::success(std::move(environment));
}

} // namespace carto::d3d12
