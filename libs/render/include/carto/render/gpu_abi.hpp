#pragma once

#include <carto/core/result.hpp>
#include <carto/render/material.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace carto::render {

// These structures are the CPU-side ABI for the first native GPU passes. All
// members are 16-byte groups so the layout can be copied to a D3D12/Vulkan
// constant-buffer allocation without backend-specific packing assumptions.
struct alignas(16) GpuFrameConstants {
    std::array<float, 16U> view_projection{};
    std::array<float, 16U> previous_view_projection{};
    std::array<float, 4U> camera_position_exposure{};
    std::array<float, 4U> viewport_and_inverse_viewport{};
    std::array<std::uint32_t, 4U> frame_flags{};
};

struct alignas(16) GpuObjectConstants {
    std::array<float, 16U> model{};
    std::array<float, 16U> previous_model{};
    std::array<float, 16U> normal_matrix{};
    std::array<std::uint32_t, 4U> object_material_ids{};
};

struct alignas(16) GpuMaterialConstants {
    std::array<float, 4U> base_color_factor{};
    // Emissive factor and strength are pre-multiplied before narrowing. The
    // reclaimed w lane carries scalar dielectric F0 in the native push ABI.
    std::array<float, 4U> emissive_radiance_and_f0{};
    std::array<float, 4U> surface_factors{};
    std::array<float, 4U> alpha_factors{};
    // x clearcoat weight, y clearcoat roughness, z/w reserved.
    std::array<float, 4U> clearcoat_factors{};
    // Each asset id is stored as low/high uint32 pairs. A backend resolves
    // those ids to descriptor indices during upload; truncation is forbidden.
    std::array<std::uint32_t, 12U> texture_asset_ids{};
};

struct alignas(16) GpuLightConstants {
    std::array<float, 4U> direction_and_intensity{};
    std::array<float, 4U> radiance_and_range{};
    std::array<float, 16U> shadow_view_projection{};
    std::array<float, 4U> shadow_parameters{};
};

struct alignas(16) GpuTemporalConstants {
    std::array<float, 4U> feedback_and_clamp{};
    std::array<float, 4U> jitter_and_inv_extent{};
    std::array<std::uint32_t, 4U> history_flags{};
};

static_assert(std::is_standard_layout_v<GpuFrameConstants>);
static_assert(std::is_standard_layout_v<GpuObjectConstants>);
static_assert(std::is_standard_layout_v<GpuMaterialConstants>);
static_assert(std::is_standard_layout_v<GpuLightConstants>);
static_assert(std::is_standard_layout_v<GpuTemporalConstants>);
static_assert(alignof(GpuFrameConstants) == 16U && sizeof(GpuFrameConstants) == 176U);
static_assert(alignof(GpuObjectConstants) == 16U && sizeof(GpuObjectConstants) == 208U);
static_assert(alignof(GpuMaterialConstants) == 16U && sizeof(GpuMaterialConstants) == 128U);
static_assert(alignof(GpuLightConstants) == 16U && sizeof(GpuLightConstants) == 112U);
static_assert(alignof(GpuTemporalConstants) == 16U && sizeof(GpuTemporalConstants) == 48U);
static_assert(offsetof(GpuMaterialConstants, base_color_factor) == 0U);
static_assert(offsetof(GpuMaterialConstants, emissive_radiance_and_f0) == 16U);
static_assert(offsetof(GpuMaterialConstants, surface_factors) == 32U);
static_assert(offsetof(GpuMaterialConstants, alpha_factors) == 48U);
static_assert(offsetof(GpuMaterialConstants, clearcoat_factors) == 64U);
static_assert(offsetof(GpuMaterialConstants, texture_asset_ids) == 80U);

[[nodiscard]] core::Result<void> validate(const GpuFrameConstants& constants);
[[nodiscard]] core::Result<void> validate(const GpuObjectConstants& constants);
[[nodiscard]] core::Result<void> validate(const GpuMaterialConstants& constants);
[[nodiscard]] core::Result<void> validate(const GpuLightConstants& constants);
[[nodiscard]] core::Result<void> validate(const GpuTemporalConstants& constants);

[[nodiscard]] core::Result<GpuMaterialConstants> pack_material_for_gpu(
    const StandardMaterial& material);
[[nodiscard]] core::Result<GpuLightConstants> pack_light_for_gpu(
    const PbrLight& light);

} // namespace carto::render
