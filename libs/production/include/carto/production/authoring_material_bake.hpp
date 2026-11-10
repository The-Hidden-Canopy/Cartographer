#pragma once
#include <carto/production/authoring_material.hpp>
namespace carto::production {
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
[[nodiscard]] core::Result<AuthoringMaterialBake> bake_authoring_material(
    const AuthoringMaterialProgram& program, std::uint32_t resolution);
} // namespace carto::production
