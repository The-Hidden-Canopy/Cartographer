#include <carto/render/material.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace carto::render {

namespace {

using carto::core::Vec3d;

constexpr double kPi = std::numbers::pi_v<double>;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

Vec3d add(Vec3d left, Vec3d right) {
    return left + right;
}

Vec3d scale(Vec3d value, double factor) {
    return value * factor;
}

Vec3d multiply(Vec3d left, Vec3d right) {
    return core::componentwise_multiply(left, right);
}

Vec3d clamp01(Vec3d value) {
    return {
        std::clamp(value.x, 0.0, 1.0),
        std::clamp(value.y, 0.0, 1.0),
        std::clamp(value.z, 0.0, 1.0),
    };
}

bool finite(Vec3d value) {
    return value.finite();
}

} // namespace

core::Result<void> StandardMaterial::validate() const {
    if (!base_color_factor.finite() || !emissive_factor.finite()) {
        return core::Result<void>::failure(
            invalid("material factors must contain finite values"));
    }
    if (base_color_factor.x < 0.0 || base_color_factor.y < 0.0 ||
        base_color_factor.z < 0.0 || base_color_factor.w < 0.0 ||
        base_color_factor.w > 1.0) {
        return core::Result<void>::failure(
            validation("material base color factors must be non-negative with alpha in range"));
    }
    if (!std::isfinite(metallic) || metallic < 0.0 || metallic > 1.0 ||
        !std::isfinite(roughness) || roughness < 0.0 || roughness > 1.0 ||
        !finite_nonnegative(normal_scale) || !std::isfinite(occlusion_strength) ||
        occlusion_strength < 0.0 || occlusion_strength > 1.0 ||
        !finite_nonnegative(emissive_strength) || !std::isfinite(alpha_cutoff) ||
        alpha_cutoff < 0.0 || alpha_cutoff > 1.0) {
        return core::Result<void>::failure(
            invalid("material scalar factors are outside their supported ranges"));
    }
    if (emissive_factor.x < 0.0 || emissive_factor.y < 0.0 || emissive_factor.z < 0.0) {
        return core::Result<void>::failure(
            validation("material emissive factors must be non-negative"));
    }
    return core::Result<void>::success();
}

core::Result<PbrEvaluation> evaluate_pbr_reference(
    const StandardMaterial& material,
    const PbrSurface& surface,
    const PbrLight& light) {
    if (auto result = material.validate(); !result) {
        return core::Result<PbrEvaluation>::failure(result.error());
    }
    if (!surface.normal.finite() || !surface.view_direction.finite() ||
        !surface.irradiance.finite() || !surface.prefiltered_environment.finite() ||
        !light.direction_to_light.finite() || !light.radiance.finite() ||
        !std::isfinite(surface.brdf_lut[0]) || !std::isfinite(surface.brdf_lut[1])) {
        return core::Result<PbrEvaluation>::failure(
            invalid("PBR reference inputs must be finite"));
    }

    const Vec3d normal = surface.normal.normalized();
    const Vec3d view = surface.view_direction.normalized();
    const Vec3d light_direction = light.direction_to_light.normalized();
    if (!finite(normal) || !finite(view) || !finite(light_direction)) {
        return core::Result<PbrEvaluation>::failure(
            validation("PBR reference directions must be non-degenerate"));
    }

    const double n_dot_l = std::max(0.0, core::dot(normal, light_direction));
    const double n_dot_v = std::max(0.0, core::dot(normal, view));
    const Vec3d base_color{
        material.base_color_factor.x,
        material.base_color_factor.y,
        material.base_color_factor.z,
    };
    const Vec3d dielectric_f0{0.04, 0.04, 0.04};
    const Vec3d f0 = add(
        scale(dielectric_f0, 1.0 - material.metallic),
        scale(base_color, material.metallic));
    const Vec3d dielectric_diffuse_weight =
        scale(Vec3d{1.0, 1.0, 1.0} - f0, 1.0 - material.metallic);
    Vec3d direct_diffuse{};
    Vec3d direct_specular{};
    if (n_dot_l > 0.0 && n_dot_v > 0.0) {
        const Vec3d half_vector = (view + light_direction).normalized();
        if (!finite(half_vector)) {
            return core::Result<PbrEvaluation>::failure(
                validation("PBR reference half-vector is non-degenerate"));
        }
        const double n_dot_h = std::max(0.0, core::dot(normal, half_vector));
        const double v_dot_h = std::max(0.0, core::dot(view, half_vector));
        const double alpha = std::max(0.045, material.roughness * material.roughness);
        const double alpha_squared = alpha * alpha;
        const double denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0;
        const double distribution = alpha_squared / (kPi * denominator * denominator);
        const auto smith_term = [alpha](double n_dot_x) {
            const double k = (alpha + 1.0) * (alpha + 1.0) / 8.0;
            return n_dot_x / (n_dot_x * (1.0 - k) + k);
        };
        const double geometry = smith_term(n_dot_l) * smith_term(n_dot_v);
        const double fresnel_factor = std::pow(1.0 - v_dot_h, 5.0);
        const Vec3d fresnel = add(f0, scale(
            Vec3d{1.0, 1.0, 1.0} - f0, fresnel_factor));
        const Vec3d diffuse_weight = scale(
            Vec3d{1.0, 1.0, 1.0} - fresnel, 1.0 - material.metallic);
        direct_diffuse = scale(
            multiply(diffuse_weight, base_color), n_dot_l / kPi);
        direct_specular = scale(
            fresnel, distribution * geometry / std::max(1e-12, 4.0 * n_dot_l * n_dot_v));
    }

    const Vec3d ibl_diffuse = multiply(
        surface.irradiance,
        scale(multiply(dielectric_diffuse_weight, base_color), material.occlusion_strength));
    const Vec3d specular_brdf = add(
        scale(f0, surface.brdf_lut[0]),
        Vec3d{surface.brdf_lut[1], surface.brdf_lut[1], surface.brdf_lut[1]});
    const Vec3d ibl_specular = multiply(surface.prefiltered_environment, specular_brdf);
    const Vec3d emissive = material.emissive_factor * material.emissive_strength;
    const Vec3d lit_direct = multiply(add(direct_diffuse, direct_specular), light.radiance);
    const Vec3d hdr = add(add(lit_direct, add(ibl_diffuse, ibl_specular)), emissive);
    if (!finite(hdr)) {
        return core::Result<PbrEvaluation>::failure(
            validation("PBR reference evaluation produced non-finite HDR radiance"));
    }
    return core::Result<PbrEvaluation>::success(PbrEvaluation{
        direct_diffuse,
        direct_specular,
        ibl_diffuse,
        ibl_specular,
        emissive,
        hdr,
    });
}

core::Result<Vec3d> apply_exposure(Vec3d linear_hdr, double exposure_compensation) {
    if (!linear_hdr.finite() || !std::isfinite(exposure_compensation)) {
        return core::Result<Vec3d>::failure(invalid("exposure inputs must be finite"));
    }
    const double multiplier = std::exp2(exposure_compensation);
    const Vec3d exposed = linear_hdr * multiplier;
    if (!exposed.finite()) {
        return core::Result<Vec3d>::failure(
            validation("exposure produced non-finite radiance"));
    }
    return core::Result<Vec3d>::success(exposed);
}

core::Result<Vec3d> tone_map(Vec3d linear_hdr, ToneMapMode mode) {
    if (!linear_hdr.finite()) {
        return core::Result<Vec3d>::failure(invalid("tone-map input must be finite"));
    }
    const auto neutral = [](double value) { return value / (1.0 + std::max(0.0, value)); };
    const auto aces = [](double value) {
        const double x = std::max(0.0, value);
        constexpr double a = 2.51;
        constexpr double b = 0.03;
        constexpr double c = 2.43;
        constexpr double d = 0.59;
        constexpr double e = 0.14;
        return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
    };
    Vec3d mapped{};
    switch (mode) {
    case ToneMapMode::aces_fitted:
        mapped = {aces(linear_hdr.x), aces(linear_hdr.y), aces(linear_hdr.z)};
        break;
    case ToneMapMode::neutral:
        mapped = {neutral(linear_hdr.x), neutral(linear_hdr.y), neutral(linear_hdr.z)};
        break;
    case ToneMapMode::linear_diagnostic:
        mapped = clamp01(linear_hdr);
        break;
    }
    return core::Result<Vec3d>::success(mapped);
}

core::Result<Vec3d> linear_to_srgb(Vec3d linear_color) {
    if (!linear_color.finite()) {
        return core::Result<Vec3d>::failure(invalid("sRGB input must be finite"));
    }
    const auto encode = [](double value) {
        const double clamped = std::clamp(value, 0.0, 1.0);
        return clamped <= 0.0031308
            ? clamped * 12.92
            : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
    };
    return core::Result<Vec3d>::success({
        encode(linear_color.x), encode(linear_color.y), encode(linear_color.z)});
}

} // namespace carto::render
