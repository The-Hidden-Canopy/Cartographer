#pragma once
#include <carto/project/material_catalog.hpp>
#include <carto/scene/scene.hpp>
#include <carto/surface/process.hpp>
namespace carto::project {
// One bounded, explicit bake operation per object. Seed identity excludes
// project/material/geometry revisions so unrelated edits cannot reshuffle it.
struct ObjectAgingLayer {
  scene::ObjectId object;
  std::string operation_identity = "aging.surface";
  surface::WeatheringControls controls;
  double variation = 0;
  double feature_scale_meters = .25;
  std::uint32_t resolution = 64;
  render::UvSetId uv_set = 1;
  render::MaterialId source_material = 0;
  std::optional<MaterialVariantId> source_variant;
  std::uint64_t isolated_mesh = 0;
  render::MaterialId output_material = 0;
  render::SamplerId output_sampler = 0;
  // base/alpha, MR, normal, AO, emission, cause mask (oxidation/wear/fade).
  std::array<assets::TextureAssetId, 6> output_textures{};
  core::Revision source_material_revision;
  core::Revision baked_mesh_revision;
  // A duplicate keeps the prior appearance until explicitly baked for its own
  // identity.
  scene::ObjectId baked_object;
  core::Transform baked_world;
  assets::Sha256Digest source_program_digest;
  assets::Sha256Digest source_dependency_digest;
  [[nodiscard]] core::Result<void> validate_parameters() const;
  [[nodiscard]] core::Result<void> validate() const;
  [[nodiscard]] std::string serialize() const;
  [[nodiscard]] static core::Result<ObjectAgingLayer>
      deserialize(std::string_view);
};
} // namespace carto::project
