#include <carto/render/lighting.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace carto::render {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

core::Result<void> ShadowKernel::validate() const {
    if (radius > 4U || !std::isfinite(depth_bias) || depth_bias < 0.0F) {
        return core::Result<void>::failure(invalid(
            "shadow kernel radius or depth bias is outside the reference bound"));
    }
    return core::Result<void>::success();
}

core::Result<ShadowEvaluation> evaluate_shadow_reference(
    std::span<const float> depth_map,
    std::uint32_t width,
    std::uint32_t height,
    float u,
    float v,
    float receiver_depth,
    ShadowKernel kernel) {
    if (width == 0U || height == 0U ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) !=
            depth_map.size()) {
        return core::Result<ShadowEvaluation>::failure(invalid(
            "shadow map dimensions must match a non-empty depth image"));
    }
    if (auto result = kernel.validate(); !result) {
        return core::Result<ShadowEvaluation>::failure(result.error());
    }
    if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(receiver_depth) ||
        receiver_depth < 0.0F || receiver_depth > 1.0F) {
        return core::Result<ShadowEvaluation>::failure(invalid(
            "shadow query coordinates and receiver depth must be finite and in range"));
    }
    for (const float depth : depth_map) {
        if (!std::isfinite(depth) || depth < 0.0F || depth > 1.0F) {
            return core::Result<ShadowEvaluation>::failure(validation(
                "shadow map contains a non-finite or out-of-range depth"));
        }
    }

    const float clamped_u = std::clamp(u, 0.0F, 1.0F);
    const float clamped_v = std::clamp(v, 0.0F, 1.0F);
    const auto center_x = static_cast<std::int64_t>(std::floor(
        clamped_u * static_cast<float>(width - 1U)));
    const auto center_y = static_cast<std::int64_t>(std::floor(
        clamped_v * static_cast<float>(height - 1U)));
    const std::int64_t radius = static_cast<std::int64_t>(kernel.radius);
    std::uint32_t lit = 0U;
    std::uint32_t taps = 0U;
    for (std::int64_t y = -radius; y <= radius; ++y) {
        for (std::int64_t x = -radius; x <= radius; ++x) {
            const auto sample_x = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                center_x + x, 0, static_cast<std::int64_t>(width - 1U)));
            const auto sample_y = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                center_y + y, 0, static_cast<std::int64_t>(height - 1U)));
            const std::size_t index = static_cast<std::size_t>(sample_y) * width + sample_x;
            if (receiver_depth - kernel.depth_bias <= depth_map[index]) ++lit;
            ++taps;
        }
    }
    if (taps == 0U) {
        return core::Result<ShadowEvaluation>::failure(validation(
            "shadow kernel produced no taps"));
    }
    return core::Result<ShadowEvaluation>::success({
        static_cast<float>(lit) / static_cast<float>(taps), taps});
}

} // namespace carto::render
