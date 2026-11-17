#pragma once
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace carto::d3d12 {
struct AuthoredAnisotropyReference {
    double alpha_t = 0.0, alpha_b = 0.0;
    double distribution = 0.0, visibility = 0.0, kernel = 0.0;
};

// CPU-only scalar direct-light reference for authored KHR_materials_anisotropy.
// https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_anisotropy
// Follows the extension's directional roughness and its non-normative clamped
// correlated-Smith visibility example. Kernel is D*V, excluding Fresnel, N.L,
// radiance and clearcoat. Not the signed graph model; no renderer is activated.
// Directions are in the authored orthonormal (T,B,N) frame, with B carrying
// authored handedness. Rotation is CCW from T toward B. No textures or IBL.
// Deliberate numerical policy: alpha >= 1e-4 regularizes the zero-roughness
// delta lobe. This is not a normative glTF floor or an exact mirror BRDF.
[[nodiscard]] inline core::Result<AuthoredAnisotropyReference>
evaluate_authored_anisotropy_reference(double roughness, double strength,
    double rotation, core::Vec3d view, core::Vec3d light) {
    const auto unit = [](core::Vec3d v) {
        return v.finite() && std::abs(v.length_squared() - 1.0) <= 1e-5;
    };
    if (!std::isfinite(roughness) || roughness < 0 || roughness > 1 ||
        !std::isfinite(strength) || strength < 0 || strength > 1 ||
        !std::isfinite(rotation) || !unit(view) || !unit(light))
        return core::Result<AuthoredAnisotropyReference>::failure({
            core::ErrorCode::validation_failed,
            "authored anisotropy requires bounded factors, finite rotation and unit directions"});
    AuthoredAnisotropyReference result;
    result.alpha_b = std::max(1e-4, roughness * roughness);
    result.alpha_t = result.alpha_b + (1.0 - result.alpha_b) * strength * strength;
    if (view.z <= 0 || light.z <= 0)
        return core::Result<AuthoredAnisotropyReference>::success(result);
    view = view.normalized(); light = light.normalized();
    const double angle = std::remainder(rotation, 2.0 * std::numbers::pi_v<double>);
    const double c = std::cos(angle), s = std::sin(angle);
    const auto rotate = [c,s](core::Vec3d v) {
        return core::Vec3d{c*v.x + s*v.y, -s*v.x + c*v.y, v.z};
    };
    view = rotate(view); light = rotate(light);
    const auto sum = view + light;
    const double magnitude = sum.length();
    const core::Vec3d half{sum.x / magnitude, sum.y / magnitude, sum.z / magnitude};
    const double t = half.x / result.alpha_t, b = half.y / result.alpha_b;
    const double ellipse = t*t + b*b + half.z*half.z;
    result.distribution = 1.0 / (std::numbers::pi_v<double> *
        result.alpha_t * result.alpha_b * ellipse * ellipse);
    const auto projected_length = [&](core::Vec3d v) {
        return std::hypot(std::hypot(result.alpha_t*v.x, result.alpha_b*v.y), v.z);
    };
    const double denominator = light.z*projected_length(view) + view.z*projected_length(light);
    result.visibility = denominator > 0 ? std::min(1.0, 0.5 / denominator) : 1.0;
    result.kernel = result.distribution * result.visibility;
    return core::Result<AuthoredAnisotropyReference>::success(result);
}
} // namespace carto::d3d12
