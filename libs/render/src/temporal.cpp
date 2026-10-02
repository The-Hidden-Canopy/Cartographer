#include <carto/render/temporal.hpp>

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

core::Result<TemporalResolveResult> resolve_temporal_reference(
    const TemporalResolveInput& input) {
    if (input.width == 0U || input.height == 0U) {
        return core::Result<TemporalResolveResult>::failure(invalid(
            "temporal resolve extent must be non-zero"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4U ||
        input.current_rgba.size() != pixels * 4U) {
        return core::Result<TemporalResolveResult>::failure(invalid(
            "temporal current image does not match its extent"));
    }
    if (input.history_valid && input.history_rgba.size() != pixels * 4U) {
        return core::Result<TemporalResolveResult>::failure(invalid(
            "valid temporal history does not match the current extent"));
    }
    if (!input.motion_magnitude.empty() && input.motion_magnitude.size() != pixels) {
        return core::Result<TemporalResolveResult>::failure(invalid(
            "temporal motion data does not match the current extent"));
    }
    if (!std::isfinite(input.feedback) || input.feedback < 0.0F || input.feedback > 1.0F ||
        !std::isfinite(input.neighborhood_clamp) || input.neighborhood_clamp < 0.0F ||
        !std::isfinite(input.motion_rejection) || input.motion_rejection < 0.0F) {
        return core::Result<TemporalResolveResult>::failure(invalid(
            "temporal feedback, clamp, and rejection settings are invalid"));
    }
    for (const float value : input.current_rgba) {
        if (!std::isfinite(value)) {
            return core::Result<TemporalResolveResult>::failure(validation(
                "temporal current image contains non-finite data"));
        }
    }
    if (input.history_valid) {
        for (const float value : input.history_rgba) {
            if (!std::isfinite(value)) {
                return core::Result<TemporalResolveResult>::failure(validation(
                    "temporal history contains non-finite data"));
            }
        }
    }
    for (const float value : input.motion_magnitude) {
        if (!std::isfinite(value) || value < 0.0F) {
            return core::Result<TemporalResolveResult>::failure(validation(
                "temporal motion contains invalid data"));
        }
    }

    const bool history_eligible = input.history_valid && !input.camera_cut &&
        input.current_revision == input.history_revision;
    TemporalResolveResult result;
    result.rgba.assign(input.current_rgba.begin(), input.current_rgba.end());
    result.history_used = history_eligible;
    if (!history_eligible) return core::Result<TemporalResolveResult>::success(std::move(result));

    for (std::uint64_t pixel = 0U; pixel < pixels; ++pixel) {
        float weight = input.feedback;
        if (!input.motion_magnitude.empty()) {
            const float motion = input.motion_magnitude[static_cast<std::size_t>(pixel)];
            if (input.motion_rejection == 0.0F || motion >= input.motion_rejection) {
                weight = 0.0F;
                ++result.rejected_pixels;
            } else {
                weight *= 1.0F - motion / input.motion_rejection;
            }
        }
        const std::size_t base = static_cast<std::size_t>(pixel) * 4U;
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            const float current = input.current_rgba[base + channel];
            const float history = std::clamp(
                input.history_rgba[base + channel],
                current - input.neighborhood_clamp,
                current + input.neighborhood_clamp);
            result.rgba[base + channel] = current * (1.0F - weight) + history * weight;
        }
    }
    return core::Result<TemporalResolveResult>::success(std::move(result));
}

} // namespace carto::render
