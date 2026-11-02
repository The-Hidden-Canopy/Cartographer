#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace carto::render::visual {

// This is the existing v1 RGB8 comparison contract, factored for reuse by
// future fixtures. It does not capture pixels or attest native presentation.
struct BaselineSpec {
    std::string fixture, backend, adapter_name;
    std::uint32_t vendor_id = 0, device_id = 0;
    std::string feature_level, driver_label;
    std::uint32_t width = 0, height = 0;
    std::uint8_t max_channel_delta = 0;
    std::size_t max_changed_pixels = 0;
};

inline std::string metadata_v1(const BaselineSpec& spec) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "format=carto-visual-baseline-v1\n"
           << "fixture=" << spec.fixture << '\n'
           << "backend=" << spec.backend << '\n'
           << "adapter_name=" << spec.adapter_name << '\n'
           << "vendor_id=" << spec.vendor_id << '\n'
           << "device_id=" << spec.device_id << '\n'
           << "feature_level=" << spec.feature_level << '\n'
           << "driver_label=" << spec.driver_label << '\n'
           << "dimensions=" << spec.width << 'x' << spec.height << '\n'
           << "pixel_encoding=rgb8-channel-round-nearest\n"
           << "max_channel_delta=" << static_cast<unsigned>(spec.max_channel_delta) << '\n'
           << "max_changed_pixels=" << spec.max_changed_pixels << '\n';
    return output.str();
}

struct PixelDiff {
    std::size_t changed_pixels = 0;
    std::uint8_t max_observed_channel_delta = 0;
};

// One changed pixel means at least one channel exceeds the allowed delta.
// The caller supplies the work budget; dimensions and exact RGB byte counts are
// checked before reading. This function never changes pixels or baselines.
inline std::optional<PixelDiff> compare_rgb8(
    std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected,
    std::uint32_t width, std::uint32_t height, std::uint8_t allowed_delta,
    std::size_t pixel_budget) {
    if (width == 0 || height == 0) return std::nullopt;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > pixel_budget || pixels > std::numeric_limits<std::size_t>::max() / 3U)
        return std::nullopt;
    const auto bytes = static_cast<std::size_t>(pixels) * 3U;
    if (actual.size() != bytes || expected.size() != bytes) return std::nullopt;
    PixelDiff diff;
    for (std::size_t pixel = 0; pixel < bytes; pixel += 3U) {
        bool changed = false;
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const int a = actual[pixel + channel];
            const int b = expected[pixel + channel];
            const auto delta = static_cast<std::uint8_t>(a > b ? a - b : b - a);
            diff.max_observed_channel_delta = std::max(diff.max_observed_channel_delta, delta);
            changed = changed || delta > allowed_delta;
        }
        if (changed) ++diff.changed_pixels;
    }
    return diff;
}

enum class ComparisonStatus { match, invalid_spec, metadata_mismatch, invalid_pixels, pixel_difference };
struct Comparison {
    ComparisonStatus status = ComparisonStatus::invalid_pixels;
    PixelDiff pixels;
};

// Git can materialize a tracked LF text baseline as CRLF on Windows. Treat
// only that newline encoding as equivalent; all field bytes remain exact.
inline bool metadata_matches_v1(std::string_view recorded, std::string_view canonical) noexcept {
    std::size_t index = 0;
    for (const char expected : canonical) {
        if (expected == '\n' && index < recorded.size() && recorded[index] == '\r') ++index;
        if (index == recorded.size() || recorded[index] != expected) return false;
        ++index;
    }
    return index == recorded.size();
}

inline Comparison compare_baseline(const BaselineSpec& spec,
    std::string_view recorded_metadata, std::span<const std::uint8_t> actual,
    std::span<const std::uint8_t> expected, std::size_t pixel_budget) {
    const auto safe_field = [](std::string_view value) {
        return value.find_first_of(std::string_view{"\r\n\0", 3}) == std::string_view::npos;
    };
    if (spec.fixture.empty() || spec.backend.empty() || !safe_field(spec.fixture) ||
        !safe_field(spec.backend) || !safe_field(spec.adapter_name) ||
        !safe_field(spec.feature_level) || !safe_field(spec.driver_label))
        return {ComparisonStatus::invalid_spec, {}};
    if (!metadata_matches_v1(recorded_metadata, metadata_v1(spec)))
        return {ComparisonStatus::metadata_mismatch, {}};
    const auto pixels = compare_rgb8(actual, expected, spec.width, spec.height,
        spec.max_channel_delta, pixel_budget);
    if (!pixels) return {ComparisonStatus::invalid_pixels, {}};
    return {pixels->changed_pixels <= spec.max_changed_pixels
                ? ComparisonStatus::match : ComparisonStatus::pixel_difference, *pixels};
}
} // namespace carto::render::visual
