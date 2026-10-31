#pragma once

#include <carto/production/bake_graph.hpp>
#include <utility>

namespace carto::d3d12::detail {

// AO and bent-normal projection share validation evidence, not artifacts.
// A failed projection cannot publish the AO template as a bent-normal map.
[[nodiscard]] inline production::BentNormalExecutionReport bent_normal_validation_failure(
    production::AmbientOcclusionExecutionReport validation) {
    return {std::nullopt, std::move(validation.diagnostics),
        validation.target_texels_visited, validation.covered_texels,
        validation.rays_cast, validation.occluded_rays};
}

} // namespace carto::d3d12::detail
