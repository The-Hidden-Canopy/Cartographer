#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace carto::d3d12 {

struct SceneAnisotropyReference {
    // Scalar specular kernels before Fresnel, clearcoat attenuation, N.L and
    // incident radiance. These are not environment-lighting integrals.
    double legacy_kernel = 0.0;
    double anisotropic_kernel = 0.0;
    double mixed_kernel = 0.0;
};

// Unit view/light directions in the authored UV0 tangent frame (T, B, N).
// Signed strength swaps broad/narrow axes; abs(strength) also blends from the
// legacy Schlick-GGX kernel to correlated-Smith anisotropic GGX, preserving a
// continuous zero-strength boundary without changing existing base materials.
[[nodiscard]] inline core::Result<SceneAnisotropyReference>
evaluate_scene_anisotropy_reference(double roughness, double anisotropy,
    core::Vec3d view, core::Vec3d light) {
    if (!std::isfinite(roughness) || roughness < 0.0 || roughness > 1.0 ||
        !std::isfinite(anisotropy) || anisotropy < -1.0 || anisotropy > 1.0 ||
        !view.finite() || !light.finite() ||
        std::abs(view.length_squared() - 1.0) > 1.0e-5 ||
        std::abs(light.length_squared() - 1.0) > 1.0e-5) {
        return core::Result<SceneAnisotropyReference>::failure({
            core::ErrorCode::validation_failed,
            "scene anisotropy requires finite bounded factors and unit tangent-space directions"});
    }
    SceneAnisotropyReference result;
    if (view.z <= 0.0 || light.z <= 0.0)
        return core::Result<SceneAnisotropyReference>::success(result);
    const auto sum = view + light;
    const auto half = sum.length_squared() > 1.0e-20
        ? sum.normalized() : core::Vec3d{0.0, 0.0, 1.0};
    const double n_dot_l = std::clamp(light.z, 0.0, 1.0);
    const double n_dot_v = std::clamp(view.z, 0.0, 1.0);
    const double n_dot_h = std::clamp(half.z, 0.0, 1.0);
    const double alpha = std::max(0.045, roughness * roughness);
    const double alpha_squared = alpha * alpha;
    const double denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0;
    const double distribution = alpha_squared /
        (std::numbers::pi_v<double> * denominator * denominator);
    const double k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    const auto geometry = [k](double cosine) {
        return cosine / std::max(cosine * (1.0 - k) + k, 1.0e-5);
    };
    result.legacy_kernel = distribution * geometry(n_dot_l) * geometry(n_dot_v) /
        std::max(4.0 * n_dot_l * n_dot_v, 1.0e-5);
    const double strength = std::abs(anisotropy);
    const double aspect = std::sqrt(1.0 - 0.9 * strength);
    double alpha_t = alpha / aspect;
    double alpha_b = alpha * aspect;
    if (anisotropy < 0.0) std::swap(alpha_t, alpha_b);
    const double ellipse = (half.x / alpha_t) * (half.x / alpha_t) +
        (half.y / alpha_b) * (half.y / alpha_b) + n_dot_h * n_dot_h;
    const double anisotropic_distribution = 1.0 /
        (std::numbers::pi_v<double> * alpha_t * alpha_b * ellipse * ellipse);
    const auto smith_length = [alpha_t, alpha_b](core::Vec3d direction) {
        return std::sqrt((alpha_t * direction.x) * (alpha_t * direction.x) +
            (alpha_b * direction.y) * (alpha_b * direction.y) + direction.z * direction.z);
    };
    const double visibility = 0.5 / std::max(1.0e-5,
        n_dot_l * smith_length(view) + n_dot_v * smith_length(light));
    result.anisotropic_kernel = anisotropic_distribution * visibility;
    result.mixed_kernel = strength == 0.0 ? result.legacy_kernel :
        result.legacy_kernel * (1.0 - strength) + result.anisotropic_kernel * strength;
    return core::Result<SceneAnisotropyReference>::success(result);
}

} // namespace carto::d3d12
