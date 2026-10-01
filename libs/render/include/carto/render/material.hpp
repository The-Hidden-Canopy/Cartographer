#pragma once

#include <carto/assets/texture.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <array>
#include <cstdint>

namespace carto::render {

struct TextureRef {
    assets::TextureAssetId asset = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return asset != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const TextureRef&) const noexcept = default;
};

enum class AlphaMode {
    opaque,
    masked,
    blended,
};

struct StandardMaterial {
    core::Vec4d base_color_factor{1.0, 1.0, 1.0, 1.0};
    double metallic = 0.0;
    double roughness = 0.5;
    double normal_scale = 1.0;
    double occlusion_strength = 1.0;
    core::Vec3d emissive_factor{};
    double emissive_strength = 0.0;
    AlphaMode alpha_mode = AlphaMode::opaque;
    double alpha_cutoff = 0.5;
    TextureRef base_color;
    TextureRef metallic_roughness;
    TextureRef normal;
    TextureRef occlusion;
    TextureRef emissive;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PbrLight {
    core::Vec3d direction_to_light{};
    core::Vec3d radiance{};
};

struct PbrSurface {
    core::Vec3d normal{0.0, 0.0, 1.0};
    core::Vec3d view_direction{0.0, 0.0, 1.0};
    core::Vec3d irradiance{};
    core::Vec3d prefiltered_environment{};
    std::array<double, 2U> brdf_lut{1.0, 0.0};
};

struct PbrEvaluation {
    core::Vec3d direct_diffuse{};
    core::Vec3d direct_specular{};
    core::Vec3d ibl_diffuse{};
    core::Vec3d ibl_specular{};
    core::Vec3d emissive{};
    core::Vec3d hdr_color{};
};

// This is a deterministic CPU reference for shader-facing material semantics.
// It is useful for acceptance and numerical tests, but is not GPU execution evidence.
[[nodiscard]] core::Result<PbrEvaluation> evaluate_pbr_reference(
    const StandardMaterial& material,
    const PbrSurface& surface,
    const PbrLight& light);

enum class ToneMapMode {
    aces_fitted,
    neutral,
    linear_diagnostic,
};

[[nodiscard]] core::Result<core::Vec3d> apply_exposure(
    core::Vec3d linear_hdr,
    double exposure_compensation);
[[nodiscard]] core::Result<core::Vec3d> tone_map(
    core::Vec3d linear_hdr,
    ToneMapMode mode);
[[nodiscard]] core::Result<core::Vec3d> linear_to_srgb(core::Vec3d linear_color);

} // namespace carto::render
