#pragma once
#include <array>
#include <carto/project/presentation_catalog.hpp>
#include <memory>
#include <span>

namespace carto::production {
[[nodiscard]] core::Result<assets::Sha256Digest>
environment_content_digest(const project::AuthoredEnvironment &);

// Immutable disposable lighting, compiled from one saved environment source.
// One linear RGBA32F atlas: seven specular levels with an8x4 directional floor
// (region rows0..126; coarsest two share row120 at columns8 and16),
// 32x16 diffuse E/pi (rows127..142),64x64 split-sum BRDF (rows143..206).
// No exposure, source rotation/intensity or GPU handle is baked into the atlas.
class EnvironmentLighting final {
  public:
    static constexpr unsigned width = 128, height = 207, levels = 7, diffuse_row = 127,
                              brdf_row = 143;
    [[nodiscard]] std::span<const std::array<float, 4>> atlas() const { return atlas_; }
    [[nodiscard]] assets::Sha256Digest source_digest() const { return source_digest_; }
    [[nodiscard]] core::Vec3d diffuse(core::Vec3d local_normal) const;
    [[nodiscard]] core::Vec3d specular(core::Vec3d local_reflection, double roughness) const;
    [[nodiscard]] std::array<double, 2> brdf(double n_dot_v, double roughness) const;

  private:
    EnvironmentLighting() = default;
    std::vector<std::array<float, 4>> atlas_;
    assets::Sha256Digest source_digest_;
    friend core::Result<std::shared_ptr<const EnvironmentLighting>>
    compile_environment_lighting(const project::AuthoredEnvironment &);
};
[[nodiscard]] core::Result<std::shared_ptr<const EnvironmentLighting>>
compile_environment_lighting(const project::AuthoredEnvironment &);
[[nodiscard]] core::Result<core::Vec3d>
evaluate_environment_lighting(const EnvironmentLighting &, const project::AuthoredEnvironment &,
                              const render::StandardMaterial &, core::Vec3d normal,
                              core::Vec3d view, double ambient_occlusion = 1);
} // namespace carto::production
