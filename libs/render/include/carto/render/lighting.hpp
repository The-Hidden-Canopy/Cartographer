#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <span>

namespace carto::render {

struct ShadowKernel {
    std::uint32_t radius = 1U;
    float depth_bias = 0.001F;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ShadowEvaluation {
    float visibility = 0.0F;
    std::uint32_t taps = 0U;
};

// Deterministic CPU reference for percentage-closer filtering. The map is a
// tightly packed single-channel depth image in [0, 1]. Out-of-bounds taps use
// edge clamping. This establishes sampling and bias semantics without claiming
// native shadow-map execution.
[[nodiscard]] core::Result<ShadowEvaluation> evaluate_shadow_reference(
    std::span<const float> depth_map,
    std::uint32_t width,
    std::uint32_t height,
    float u,
    float v,
    float receiver_depth,
    ShadowKernel kernel = {});

} // namespace carto::render
