#include <algorithm>
#include <carto/project/aging_layer.hpp>
#include <carto/project/project.hpp>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
namespace carto::project {
namespace {
core::Result<void> invalid(const char *s) {
  return core::Result<void>::failure({core::ErrorCode::validation_failed, s});
}
} // namespace
core::Result<void> ObjectAgingLayer::validate_parameters() const {
  if (!object || operation_identity.empty() || operation_identity.size() > 96 ||
      !std::all_of(operation_identity.begin(), operation_identity.end(),
                   [](unsigned char c) {
                     return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                            c == '_';
                   }) ||
      !source_material || !uv_set || (source_variant && !*source_variant))
    return invalid("invalid aging object, operation or material/UV identity");
  if (!std::isfinite(variation) || variation < 0 || variation > .5 ||
      !std::isfinite(feature_scale_meters) || feature_scale_meters < .001 ||
      feature_scale_meters > 1000 || resolution < 16 || resolution > 256 ||
      (resolution & (resolution - 1)))
    return invalid(
        "aging requires variation [0,.5], feature scale [.001,1000] meters and "
        "power-of-two resolution16..256");
  return controls.validate();
}
core::Result<void> ObjectAgingLayer::validate() const {
  if (auto v = validate_parameters(); !v)
    return v;
  if (!baked_object || !isolated_mesh || !output_material || !output_sampler ||
      output_material == source_material ||
      source_material_revision.value() == 0 ||
      baked_mesh_revision.value() == 0 || source_program_digest.is_zero() ||
      source_dependency_digest.is_zero() || !baked_world.finite())
    return invalid("aging source/output correspondence is incomplete");
  std::set<assets::TextureAssetId> unique;
  for (auto id : output_textures)
    if (!id || !unique.insert(id).second)
      return invalid("aging textures must have distinct nonzero identities");
  return core::Result<void>::success();
}
std::string ObjectAgingLayer::serialize() const {
  std::ostringstream o;
  o.imbue(std::locale::classic());
  o << std::setprecision(17) << "AGING1 " << object.value << ' '
    << std::quoted(operation_identity) << ' ' << controls.age << ' '
    << controls.rain_exposure << ' ' << controls.solar_exposure << ' '
    << controls.moisture << ' ' << controls.traffic << ' '
    << controls.maintenance << ' ' << controls.seed << ' ' << variation << ' '
    << feature_scale_meters << ' ' << resolution << ' ' << uv_set << ' '
    << source_material << ' ' << source_variant.value_or(0) << ' '
    << isolated_mesh << ' ' << output_material << ' ' << output_sampler;
  for (auto id : output_textures)
    o << ' ' << id;
  o << ' ' << source_material_revision.value() << ' '
    << baked_mesh_revision.value() << ' ' << baked_object.value << ' '
    << baked_world.translation.x << ' ' << baked_world.translation.y << ' '
    << baked_world.translation.z << ' ' << baked_world.rotation.x << ' '
    << baked_world.rotation.y << ' ' << baked_world.rotation.z << ' '
    << baked_world.rotation.w << ' ' << baked_world.scale.x << ' '
    << baked_world.scale.y << ' ' << baked_world.scale.z << ' '
    << source_program_digest.hex() << ' ' << source_dependency_digest.hex();
  return o.str();
}
core::Result<ObjectAgingLayer>
ObjectAgingLayer::deserialize(std::string_view text) {
  using R = core::Result<ObjectAgingLayer>;
  if (text.size() > 4096)
    return R::failure(
        {core::ErrorCode::validation_failed, "aging record exceeds4096 bytes"});
  std::istringstream in{std::string(text)};
  in.imbue(std::locale::classic());
  ObjectAgingLayer r;
  std::string magic, digest, dependency, tail;
  std::uint64_t variant = 0, mrev = 0, grev = 0;
  if (!(in >> magic >> r.object.value >> std::quoted(r.operation_identity) >>
        r.controls.age >> r.controls.rain_exposure >>
        r.controls.solar_exposure >> r.controls.moisture >>
        r.controls.traffic >> r.controls.maintenance >> r.controls.seed >>
        r.variation >> r.feature_scale_meters >> r.resolution >> r.uv_set >>
        r.source_material >> variant >> r.isolated_mesh >> r.output_material >>
        r.output_sampler) ||
      magic != "AGING1")
    return R::failure(
        {core::ErrorCode::validation_failed, "malformed aging source record"});
  for (auto &id : r.output_textures)
    if (!(in >> id))
      return R::failure(
          {core::ErrorCode::validation_failed, "missing aging texture"});
  if (!(in >> mrev >> grev >> r.baked_object.value >>
        r.baked_world.translation.x >> r.baked_world.translation.y >>
        r.baked_world.translation.z >> r.baked_world.rotation.x >>
        r.baked_world.rotation.y >> r.baked_world.rotation.z >>
        r.baked_world.rotation.w >> r.baked_world.scale.x >>
        r.baked_world.scale.y >> r.baked_world.scale.z >> digest >>
        dependency) ||
      (in >> tail))
    return R::failure({core::ErrorCode::validation_failed,
                       "malformed aging dependency record"});
  if (variant)
    r.source_variant = variant;
  r.source_material_revision = core::Revision{mrev};
  r.baked_mesh_revision = core::Revision{grev};
  auto sha = assets::Sha256Digest::from_hex(digest);
  if (!sha)
    return R::failure(sha.error());
  r.source_program_digest = sha.value();
  sha = assets::Sha256Digest::from_hex(dependency);
  if (!sha)
    return R::failure(sha.error());
  r.source_dependency_digest = sha.value();
  if (auto v = r.validate(); !v)
    return R::failure(v.error());
  if (r.serialize() != text)
    return R::failure(
        {core::ErrorCode::validation_failed, "aging payload is not canonical"});
  return R::success(std::move(r));
}
core::Result<void> ProjectDocument::validate_aging_layers() const {
  if (aging_layers_.size() > 64)
    return invalid("project exceeds64 aging operations");
  for (const auto &[object, r] : aging_layers_) {
    if (object != r.object)
      return invalid("aging key does not match object");
    if (auto v = r.validate(); !v)
      return v;
    const auto *node = scene_.find(object);
    if (!node || !meshes_.contains(r.isolated_mesh) ||
        !material_catalog_.materials().contains(r.source_material) ||
        !material_catalog_.materials().contains(r.output_material) ||
        !material_catalog_.samplers().contains(r.output_sampler))
      return invalid(
          "aging source or output was removed without unlinking its operation");
    if (r.source_variant &&
        !material_catalog_.variants().contains(*r.source_variant))
      return invalid("aging source variant is missing");
    for (auto id : r.output_textures)
      if (!material_catalog_.textures().contains(id))
        return invalid("aging output texture is missing");
  }
  return core::Result<void>::success();
}
core::Result<void> ProjectDocument::set_aging_layer(ObjectAgingLayer r) {
  if (revision_.exhausted())
    return invalid("project revision exhausted");
  auto copy = *this;
  copy.aging_layers_[r.object] = r;
  if (auto v = copy.validate_aging_layers(); !v)
    return v;
  aging_layers_[r.object] = std::move(r);
  revision_ = revision_.next();
  return core::Result<void>::success();
}
core::Result<void> ProjectDocument::erase_aging_layer(scene::ObjectId object) {
  if (revision_.exhausted())
    return invalid("project revision exhausted");
  if (!aging_layers_.erase(object))
    return invalid("object has no aging operation");
  revision_ = revision_.next();
  return core::Result<void>::success();
}
} // namespace carto::project
