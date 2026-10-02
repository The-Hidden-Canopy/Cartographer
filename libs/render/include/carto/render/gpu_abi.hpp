#pragma once

#include <carto/core/result.hpp>
#include <carto/render/material.hpp>

#include <array>
#include <cstdint>

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
    std::array<float, 4U> emissive_factor_and_strength{};
    std::array<float, 4U> surface_factors{};
    std::array<float, 4U> alpha_factors{};
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

static_assert(sizeof(GpuFrameConstants) % 16U == 0U);
static_assert(sizeof(GpuObjectConstants) % 16U == 0U);
static_assert(sizeof(GpuMaterialConstants) % 16U == 0U);
static_assert(sizeof(GpuLightConstants) % 16U == 0U);
static_assert(sizeof(GpuTemporalConstants) % 16U == 0U);

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
