#pragma once
#include <array>
#include <carto/project/presentation_catalog.hpp>
#include <memory>
#include <span>

namespace carto::production {
[[nodiscard]] core::Result<assets::Sha256Digest>
environment_content_digest(const project::AuthoredEnvironment &);

enum class EnvironmentLightingQuality { standard, high, ultra };

// Immutable disposable lighting, compiled from one saved environment source.
// One bounded linear RGBA32F atlas with seven specular levels, a 32x16
// diffuse E/pi region, and a 64x64 split-sum BRDF region. High quality uses
// a 256-wide atlas, ultra uses 512, and standard remains 128-wide.
// No exposure, source rotation/intensity or GPU handle is baked into the atlas.
class EnvironmentLighting final {
  public:
    static constexpr unsigned width = 128, height = 207, levels = 7, diffuse_row = 127,
                              brdf_row = 143;
    [[nodiscard]] std::span<const std::array<float, 4>> atlas() const { return atlas_; }
    [[nodiscard]] unsigned atlas_width() const { return atlas_width_; }
    [[nodiscard]] unsigned atlas_height() const { return atlas_width_ + 79; }
    [[nodiscard]] assets::Sha256Digest source_digest() const { return source_digest_; }
    [[nodiscard]] core::Vec3d diffuse(core::Vec3d local_normal) const;
    [[nodiscard]] core::Vec3d specular(core::Vec3d local_reflection, double roughness) const;
    [[nodiscard]] std::array<double, 2> brdf(double n_dot_v, double roughness) const;

  private:
    EnvironmentLighting() = default;
    std::vector<std::array<float, 4>> atlas_;
    assets::Sha256Digest source_digest_;
    unsigned atlas_width_ = width;
    friend core::Result<std::shared_ptr<const EnvironmentLighting>>
    compile_environment_lighting(const project::AuthoredEnvironment &);
    friend core::Result<std::shared_ptr<const EnvironmentLighting>>
    compile_environment_lighting(const project::AuthoredEnvironment &, EnvironmentLightingQuality);
};
[[nodiscard]] core::Result<std::shared_ptr<const EnvironmentLighting>>
compile_environment_lighting(const project::AuthoredEnvironment &);
// Explicit quality overrides the automatic choice based on source resolution.
[[nodiscard]] core::Result<std::shared_ptr<const EnvironmentLighting>>
compile_environment_lighting(const project::AuthoredEnvironment &, EnvironmentLightingQuality);
[[nodiscard]] core::Result<core::Vec3d>
evaluate_environment_lighting(const EnvironmentLighting &, const project::AuthoredEnvironment &,
                              const render::StandardMaterial &, core::Vec3d normal,
                              core::Vec3d view, double ambient_occlusion = 1);
} // namespace carto::production
