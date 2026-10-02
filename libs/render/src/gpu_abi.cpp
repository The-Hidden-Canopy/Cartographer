#include <carto/render/gpu_abi.hpp>

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

template <std::size_t N>
bool finite(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

core::Result<float> narrow(double value, const char* label) {
    if (!std::isfinite(value) ||
        value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value > static_cast<double>(std::numeric_limits<float>::max())) {
        return core::Result<float>::failure(invalid(
            std::string("GPU ABI value is not representable: ") + label));
    }
    return core::Result<float>::success(static_cast<float>(value));
}

core::Result<std::array<float, 4U>> narrow_vec4(
    core::Vec4d value,
    const char* label) {
    const auto x = narrow(value.x, label);
    const auto y = narrow(value.y, label);
    const auto z = narrow(value.z, label);
    const auto w = narrow(value.w, label);
    if (!x || !y || !z || !w) {
        return core::Result<std::array<float, 4U>>::failure(
            !x ? x.error() : !y ? y.error() : !z ? z.error() : w.error());
    }
    return core::Result<std::array<float, 4U>>::success({
        x.value(), y.value(), z.value(), w.value()});
}

} // namespace

core::Result<void> validate(const GpuFrameConstants& constants) {
    if (!finite(constants.view_projection) ||
        !finite(constants.previous_view_projection) ||
        !finite(constants.camera_position_exposure) ||
        !finite(constants.viewport_and_inverse_viewport)) {
        return core::Result<void>::failure(invalid(
            "GPU frame constants must contain finite floating-point values"));
    }
    if (constants.viewport_and_inverse_viewport[0] <= 0.0F ||
        constants.viewport_and_inverse_viewport[1] <= 0.0F ||
        constants.viewport_and_inverse_viewport[2] <= 0.0F ||
        constants.viewport_and_inverse_viewport[3] <= 0.0F) {
        return core::Result<void>::failure(validation(
            "GPU frame constants require a positive viewport and inverse extent"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const GpuObjectConstants& constants) {
    if (!finite(constants.model) || !finite(constants.previous_model) ||
        !finite(constants.normal_matrix)) {
        return core::Result<void>::failure(invalid(
            "GPU object constants must contain finite matrices"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const GpuMaterialConstants& constants) {
    if (!finite(constants.base_color_factor) ||
        !finite(constants.emissive_factor_and_strength) ||
        !finite(constants.surface_factors) || !finite(constants.alpha_factors)) {
        return core::Result<void>::failure(invalid(
            "GPU material constants must contain finite floating-point values"));
    }
    if (constants.base_color_factor[0] < 0.0F ||
        constants.base_color_factor[1] < 0.0F ||
        constants.base_color_factor[2] < 0.0F ||
        constants.base_color_factor[3] < 0.0F ||
        constants.base_color_factor[3] > 1.0F ||
        constants.surface_factors[0] < 0.0F ||
        constants.surface_factors[0] > 1.0F ||
        constants.surface_factors[1] < 0.0F ||
        constants.surface_factors[1] > 1.0F ||
        constants.surface_factors[2] < 0.0F ||
        constants.surface_factors[3] < 0.0F ||
        constants.surface_factors[3] > 1.0F ||
        constants.alpha_factors[0] < 0.0F ||
        constants.alpha_factors[0] > 1.0F) {
        return core::Result<void>::failure(validation(
            "GPU material constants contain out-of-range surface factors"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const GpuLightConstants& constants) {
    if (!finite(constants.direction_and_intensity) ||
        !finite(constants.radiance_and_range) ||
        !finite(constants.shadow_view_projection) ||
        !finite(constants.shadow_parameters)) {
        return core::Result<void>::failure(invalid(
            "GPU light constants must contain finite values"));
    }
    if (constants.direction_and_intensity[3] < 0.0F ||
        constants.radiance_and_range[0] < 0.0F ||
        constants.radiance_and_range[1] < 0.0F ||
        constants.radiance_and_range[2] < 0.0F ||
        constants.radiance_and_range[3] < 0.0F) {
        return core::Result<void>::failure(validation(
            "GPU light intensity and range must be non-negative"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const GpuTemporalConstants& constants) {
    if (!finite(constants.feedback_and_clamp) ||
        !finite(constants.jitter_and_inv_extent)) {
        return core::Result<void>::failure(invalid(
            "GPU temporal constants must contain finite values"));
    }
    if (constants.feedback_and_clamp[0] < 0.0F ||
        constants.feedback_and_clamp[0] > 1.0F ||
        constants.feedback_and_clamp[1] < 0.0F ||
        constants.feedback_and_clamp[1] > 1.0F ||
        constants.feedback_and_clamp[2] < 0.0F ||
        constants.feedback_and_clamp[3] < 0.0F) {
        return core::Result<void>::failure(validation(
            "GPU temporal feedback and clamp values are outside their bounds"));
    }
    return core::Result<void>::success();
}

core::Result<GpuMaterialConstants> pack_material_for_gpu(
    const StandardMaterial& material) {
    if (auto result = material.validate(); !result) {
        return core::Result<GpuMaterialConstants>::failure(result.error());
    }
    const auto base = narrow_vec4(material.base_color_factor, "base_color_factor");
    if (!base) return core::Result<GpuMaterialConstants>::failure(base.error());
    const auto emissive_x = narrow(material.emissive_factor.x, "emissive_factor");
    const auto emissive_y = narrow(material.emissive_factor.y, "emissive_factor");
    const auto emissive_z = narrow(material.emissive_factor.z, "emissive_factor");
    const auto metallic = narrow(material.metallic, "metallic");
    const auto roughness = narrow(material.roughness, "roughness");
    const auto normal_scale = narrow(material.normal_scale, "normal_scale");
    const auto occlusion = narrow(material.occlusion_strength, "occlusion_strength");
    const auto alpha_cutoff = narrow(material.alpha_cutoff, "alpha_cutoff");
    const auto emissive_strength = narrow(material.emissive_strength, "emissive_strength");
    if (!emissive_x || !emissive_y || !emissive_z || !metallic || !roughness ||
        !normal_scale || !occlusion || !alpha_cutoff || !emissive_strength) {
        return core::Result<GpuMaterialConstants>::failure(invalid(
            "material value cannot be represented by the GPU ABI"));
    }

    GpuMaterialConstants result{};
    result.base_color_factor = base.value();
    result.emissive_factor_and_strength = {
        emissive_x.value(), emissive_y.value(), emissive_z.value(), emissive_strength.value()};
    result.surface_factors = {
        metallic.value(), roughness.value(), normal_scale.value(), occlusion.value()};
    result.alpha_factors = {alpha_cutoff.value(), 0.0F, 0.0F, 0.0F};
    const auto split_asset_id = [](assets::TextureAssetId value) {
        return std::array<std::uint32_t, 2U>{
            static_cast<std::uint32_t>(value & 0xffffffffULL),
            static_cast<std::uint32_t>(value >> 32U),
        };
    };
    const auto base_color_id = split_asset_id(material.base_color.asset);
    const auto metallic_roughness_id = split_asset_id(material.metallic_roughness.asset);
    const auto normal_id = split_asset_id(material.normal.asset);
    const auto occlusion_id = split_asset_id(material.occlusion.asset);
    const auto emissive_id = split_asset_id(material.emissive.asset);
    result.texture_asset_ids = {
        base_color_id[0], base_color_id[1],
        metallic_roughness_id[0], metallic_roughness_id[1],
        normal_id[0], normal_id[1],
        occlusion_id[0], occlusion_id[1],
        emissive_id[0], emissive_id[1],
        0U, 0U,
    };
    result.alpha_factors[1] = static_cast<float>(material.alpha_mode);
    if (auto validation_result = validate(result); !validation_result) {
        return core::Result<GpuMaterialConstants>::failure(validation_result.error());
    }
    return core::Result<GpuMaterialConstants>::success(result);
}

core::Result<GpuLightConstants> pack_light_for_gpu(const PbrLight& light) {
    if (!light.direction_to_light.finite() || !light.radiance.finite()) {
        return core::Result<GpuLightConstants>::failure(invalid(
            "GPU light packing requires finite source values"));
    }
    const auto dx = narrow(light.direction_to_light.x, "light_direction");
    const auto dy = narrow(light.direction_to_light.y, "light_direction");
    const auto dz = narrow(light.direction_to_light.z, "light_direction");
    const auto rx = narrow(light.radiance.x, "light_radiance");
    const auto ry = narrow(light.radiance.y, "light_radiance");
    const auto rz = narrow(light.radiance.z, "light_radiance");
    if (!dx || !dy || !dz || !rx || !ry || !rz) {
        return core::Result<GpuLightConstants>::failure(invalid(
            "light value cannot be represented by the GPU ABI"));
    }
    GpuLightConstants result{};
    result.direction_and_intensity = {dx.value(), dy.value(), dz.value(), 1.0F};
    result.radiance_and_range = {rx.value(), ry.value(), rz.value(), 0.0F};
    result.shadow_view_projection = {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
    result.shadow_parameters = {0.001F, 1.0F, 1.0F, 0.0F};
    if (auto validation_result = validate(result); !validation_result) {
        return core::Result<GpuLightConstants>::failure(validation_result.error());
    }
    return core::Result<GpuLightConstants>::success(result);
}

} // namespace carto::render
