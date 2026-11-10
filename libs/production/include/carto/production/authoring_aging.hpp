#pragma once
#include <carto/production/authoring_material_bake.hpp>
#include <carto/project/aging_layer.hpp>
#include <carto/project/project.hpp>
namespace carto::production {
struct AuthoringAgingBake {
  AuthoringMaterialBake material;
  assets::Rgba8MipLevel masks;
  std::uint64_t stable_seed = 0;
  std::uint64_t covered_texels = 0;
  assets::Sha256Digest source_dependency_digest;
};
// Explicit offline authoring bake, not physically validated erosion or GI.
// Geometry, UV coverage and curvature use the existing bake infrastructure.
[[nodiscard]] core::Result<AuthoringAgingBake>
bake_authoring_aging(const project::ProjectDocument &,
                     const project::ObjectAgingLayer &,
                     const AuthoringTextureLibrary * = nullptr);
[[nodiscard]] bool aging_needs_rebake(const project::ProjectDocument &,
                                      const project::ObjectAgingLayer &);
[[nodiscard]] core::Result<assets::Sha256Digest>
aging_source_digest(const project::MaterialCatalog &, render::MaterialId,
                    std::optional<project::MaterialVariantId>);
} // namespace carto::production
