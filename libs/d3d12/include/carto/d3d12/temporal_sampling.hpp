#pragma once

#include <carto/core/math.hpp>

#include <cstdint>

namespace carto::d3d12 {

inline constexpr std::uint32_t temporal_sample_limit = 16U;
inline constexpr std::uint64_t temporal_history_texture_budget_bytes =
    256ULL * 1024ULL * 1024ULL;

[[nodiscard]] inline double temporal_halton_sample(
    std::uint32_t index,
    std::uint32_t base) noexcept {
    if (base < 2U) return 0.0;
    double fraction = 1.0;
    double value = 0.0;
    while (index != 0U) {
        fraction /= static_cast<double>(base);
        value += fraction * static_cast<double>(index % base);
        index /= base;
    }
    return value;
}

// Output-pixel offsets for deterministic, bounded projection sampling.
[[nodiscard]] inline core::Vec2d temporal_sample_jitter(
    std::uint32_t completed_samples) noexcept {
    if (completed_samples == 0U) return {};
    return {temporal_halton_sample(completed_samples, 2U) - 0.5,
        temporal_halton_sample(completed_samples, 3U) - 0.5};
}

[[nodiscard]] inline core::Vec2d temporal_history_offset(
    core::Vec2d previous_jitter,
    core::Vec2d current_jitter) noexcept {
    return {previous_jitter.x - current_jitter.x,
        previous_jitter.y - current_jitter.y};
}

[[nodiscard]] inline bool temporal_history_textures_fit_budget(
    std::uint32_t width,
    std::uint32_t height) noexcept {
    constexpr std::uint64_t bytes_per_pixel_for_two_textures =
        2ULL * 4ULL * sizeof(float);
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    return width != 0U && height != 0U &&
        pixels <= temporal_history_texture_budget_bytes /
            bytes_per_pixel_for_two_textures;
}

} // namespace carto::d3d12
