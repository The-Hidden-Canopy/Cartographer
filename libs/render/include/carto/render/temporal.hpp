#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace carto::render {

struct TemporalResolveInput {
    core::Revision current_revision;
    core::Revision history_revision;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::span<const float> current_rgba;
    std::span<const float> history_rgba;
    std::span<const float> motion_magnitude;
    bool history_valid = false;
    bool camera_cut = false;
    float feedback = 0.9F;
    float neighborhood_clamp = 0.25F;
    float motion_rejection = 1.0F;
};

struct TemporalResolveResult {
    std::vector<float> rgba;
    bool history_used = false;
    std::uint32_t rejected_pixels = 0U;
};

// Deterministic CPU reference for a bounded temporal resolve. A revision or
// camera-cut mismatch invalidates history; motion reduces or rejects history;
// history samples are clamped around the current sample before blending.
[[nodiscard]] core::Result<TemporalResolveResult> resolve_temporal_reference(
    const TemporalResolveInput& input);

} // namespace carto::render
