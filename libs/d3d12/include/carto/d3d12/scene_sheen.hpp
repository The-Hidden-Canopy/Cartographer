#pragma once

#include <carto/core/result.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace carto::d3d12 {

struct SceneSheenReference {
    double base_layer_weight = 1.0;
    // Multiply by linear base color, outer clearcoat attenuation, N.L and
    // incident radiance. This is a direct-light BRDF, not an IBL integral.
    double sheen_brdf = 0.0;
};

// CPU reference for the saved-scene shader's bounded Charlie distribution /
// Neubelt visibility mixture. Uses the existing perceptual surface roughness
// with a 0.07 microfacet-alpha floor. It is not GPU or white-furnace evidence.
[[nodiscard]] inline core::Result<SceneSheenReference> evaluate_scene_sheen_reference(
    double sheen_weight, double roughness, double metallic,
    double n_dot_l, double n_dot_v, double n_dot_h) {
    for (const double value : {sheen_weight, roughness, metallic,
            n_dot_l, n_dot_v, n_dot_h}) {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return core::Result<SceneSheenReference>::failure({
                core::ErrorCode::validation_failed,
                "scene sheen factors and cosines must each be finite and in [0, 1]"});
        }
    }
    const double weight = sheen_weight * (1.0 - metallic);
    SceneSheenReference result{1.0 - weight, 0.0};
    if (weight == 0.0 || n_dot_l == 0.0 || n_dot_v == 0.0)
        return core::Result<SceneSheenReference>::success(result);
    const double alpha = std::max(0.07, roughness * roughness);
    const double inverse_alpha = 1.0 / alpha;
    const double distribution = (2.0 + inverse_alpha) *
        std::pow(std::max(0.0, 1.0 - n_dot_h * n_dot_h), 0.5 * inverse_alpha) /
        (2.0 * std::numbers::pi_v<double>);
    const double visibility = 1.0 / std::max(1.0e-5,
        4.0 * (n_dot_l + n_dot_v - n_dot_l * n_dot_v));
    result.sheen_brdf = weight * distribution * visibility;
    return core::Result<SceneSheenReference>::success(result);
}

} // namespace carto::d3d12
