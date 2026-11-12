#pragma once
#include <carto/production/authoring_material.hpp>
#include <cstdint>
namespace carto::production {
inline constexpr std::uint32_t kMinAuthoringMaterialBakeResolution = 4;
inline constexpr std::uint32_t kMaxAuthoringMaterialBakeResolution = 1024;
// Six retained RGBA8 maps require at most 24 MiB for one bake. Export uses
// this same texel allowance across all unique material/variant bakes.
inline constexpr std::uint64_t kMaxAuthoringMaterialBakeTexels = 1024ULL * 1024;
[[nodiscard]] constexpr bool authoring_material_bake_fits_budget(
    std::uint32_t resolution, std::uint64_t used_texels = 0) noexcept {
    if (resolution < kMinAuthoringMaterialBakeResolution ||
        resolution > kMaxAuthoringMaterialBakeResolution ||
        (resolution & (resolution - 1)))
        return false;
    const auto texels = std::uint64_t(resolution) * resolution;
    // Subtraction after validating the bounded extent avoids accumulator
    // overflow even when a caller supplies an invalid prior budget.
    return used_texels <= kMaxAuthoringMaterialBakeTexels - texels;
}
struct AuthoringMaterialBake {
    // Base/alpha, roughness-metallic, tangent normal, AO, emission, clearcoat.
    // Color RGB is sRGB encoded; alpha and all data channels remain linear.
    std::array<assets::Rgba8MipLevel, 6> maps;
    render::StandardMaterial factors;
    render::UvSetId uv = 1;
    bool normal_mapping = false;
    assets::Sha256Digest program_digest;
};
// Explicit finite-resolution approximation of the existing executable program
// over one [0,1] UV tile. No authoring source is changed. Pixel centers and
// derivatives are deterministic; output error/filtering is not analytic parity.
// Resolution must be a power of two from 4 through 1024. This is a retained
// raster bake, not a streamed or multi-tile/UDIM bake.
[[nodiscard]] core::Result<AuthoringMaterialBake> bake_authoring_material(
    const AuthoringMaterialProgram& program, std::uint32_t resolution);
} // namespace carto::production
