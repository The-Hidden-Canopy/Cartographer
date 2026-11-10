#include <algorithm>
#include <bit>
#include <carto/production/authoring_aging.hpp>
#include <carto/production/bake_graph.hpp>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
namespace carto::production {
namespace {
template <class T> T need(core::Result<T> r) {
  if (!r)
    throw std::runtime_error(r.error().message);
  return std::move(r.value());
}
std::uint64_t read64(std::span<const std::uint8_t> b, std::size_t p) {
  if (p > b.size() || b.size() - p < 8)
    throw std::runtime_error("truncated aging world-position field");
  std::uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i)
    n |= std::uint64_t(b[p + i]) << (8 * i);
  return n;
}
std::uint64_t mix(std::uint64_t n) {
  n ^= n >> 30;
  n *= 0xbf58476d1ce4e5b9ULL;
  n ^= n >> 27;
  n *= 0x94d049bb133111ebULL;
  return n ^ (n >> 31);
}
double hash_value(std::uint64_t seed, std::int64_t x, std::int64_t y,
                  std::int64_t z) {
  return double(mix(seed ^ mix(std::bit_cast<std::uint64_t>(x)) ^
                    std::rotl(mix(std::bit_cast<std::uint64_t>(y)), 21) ^
                    std::rotl(mix(std::bit_cast<std::uint64_t>(z)), 42)) >>
                11) *
         (1. / 9007199254740992.);
}
double noise(std::uint64_t seed, core::Vec3d p, double scale) {
  p = p * (1 / scale);
  if (!p.finite() ||
      std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) > 1e9)
    throw std::runtime_error("aging field coordinates exceed lattice budget");
  const auto x = static_cast<std::int64_t>(std::floor(p.x)),
             y = static_cast<std::int64_t>(std::floor(p.y)),
             z = static_cast<std::int64_t>(std::floor(p.z));
  const auto smooth = [](double v) { return v * v * (3 - 2 * v); };
  const double u = smooth(p.x - double(x)), v = smooth(p.y - double(y)),
               w = smooth(p.z - double(z));
  double value = 0;
  for (int a = 0; a < 2; ++a)
    for (int b = 0; b < 2; ++b)
      for (int c = 0; c < 2; ++c)
        value += hash_value(seed, x + a, y + b, z + c) * (a ? u : 1 - u) *
                 (b ? v : 1 - v) * (c ? w : 1 - w);
  return value;
}
double linear(std::uint8_t v) {
  const double s = v / 255.;
  return s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
}
std::uint8_t byte(double v, bool color = false) {
  v = std::clamp(v, 0., 1.);
  if (color)
    v = v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055;
  return static_cast<std::uint8_t>(std::lround(v * 255));
}
std::string transform_key(const core::Transform &t) {
  std::ostringstream s;
  s << std::setprecision(17) << t.translation.x << ' ' << t.translation.y << ' '
    << t.translation.z << ' ' << t.rotation.x << ' ' << t.rotation.y << ' '
    << t.rotation.z << ' ' << t.rotation.w << ' ' << t.scale.x << ' '
    << t.scale.y << ' ' << t.scale.z;
  return s.str();
}
} // namespace
core::Result<assets::Sha256Digest>
aging_source_digest(const project::MaterialCatalog &source,
                    render::MaterialId material,
                    std::optional<project::MaterialVariantId> variant) {
  using R = core::Result<assets::Sha256Digest>;
  try {
    project::MaterialCatalog projection;
    const auto resolved = need(source.resolve_material(material, variant));
    std::set<assets::TextureAssetId> textures;
    std::set<render::SamplerId> samplers;
    const auto collect = [&](const render::StandardMaterial &m) {
      for (const auto *ref : {&m.base_color, &m.metallic_roughness, &m.normal,
                              &m.occlusion, &m.emissive})
        if (*ref) {
          textures.insert(ref->asset);
          if (ref->sampler)
            samplers.insert(ref->sampler);
        }
    };
    collect(resolved);
    collect(source.materials().at(material).definition);
    for (auto id : textures) {
      auto v = projection.insert_texture(source.textures().at(id));
      if (!v)
        throw std::runtime_error(v.error().message);
    }
    for (auto id : samplers) {
      auto v = projection.insert_sampler(source.samplers().at(id));
      if (!v)
        throw std::runtime_error(v.error().message);
    }
    if (auto v = projection.insert_material(source.materials().at(material));
        !v)
      throw std::runtime_error(v.error().message);
    if (variant)
      if (auto v = projection.insert_variant(source.variants().at(*variant));
          !v)
        throw std::runtime_error(v.error().message);
    std::string binding;
    if (auto b = source.material_graph_bindings().find(material);
        b != source.material_graph_bindings().end()) {
      const auto &graph = source.material_graphs().at(b->second.graph_identity);
      if (auto v = projection.insert_material_graph(graph); !v)
        throw std::runtime_error(v.error().message);
      if (auto v = projection.bind_material_graph(material, graph.identity); !v)
        throw std::runtime_error(v.error().message);
      binding = b->second.source_digest.hex() +
                std::to_string(b->second.source_revision.value());
    }
    const auto bytes = projection.serialize() + binding;
    return R::success(assets::sha256(
        {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}));
  } catch (const std::exception &e) {
    return R::failure({core::ErrorCode::validation_failed, e.what()});
  }
}
bool aging_needs_rebake(const project::ProjectDocument &d,
                        const project::ObjectAgingLayer &r) {
  const auto *o = d.scene().find(r.object);
  if (r.object != r.baked_object || !o || o->mesh_asset != r.isolated_mesh)
    return true;
  const auto mesh = d.meshes().find(r.isolated_mesh);
  const auto material =
      d.material_catalog().materials().find(r.source_material);
  if (mesh == d.meshes().end() ||
      material == d.material_catalog().materials().end() ||
      mesh->second.revision() != r.baked_mesh_revision ||
      material->second.revision != r.source_material_revision)
    return true;
  const auto world = d.scene().world_transform(r.object);
  if (!world || transform_key(world.value()) != transform_key(r.baked_world))
    return true;
  // Graph and texture edits can advance independently of the base material.
  const auto digest = aging_source_digest(d.material_catalog(),
                                          r.source_material, r.source_variant);
  return !digest || digest.value() != r.source_dependency_digest;
}
core::Result<AuthoringAgingBake>
bake_authoring_aging(const project::ProjectDocument &d,
                     const project::ObjectAgingLayer &r,
                     const AuthoringTextureLibrary *textures) {
  using R = core::Result<AuthoringAgingBake>;
  try {
    if (auto v = r.validate_parameters(); !v)
      return R::failure(v.error());
    const auto *object = d.scene().find(r.object);
    if (!object || !object->mesh_asset)
      throw std::runtime_error("aging requires a mesh instance");
    const auto &mesh = d.meshes().at(*object->mesh_asset);
    if (mesh.face_count() > 100000 || mesh.vertex_count() > 200000)
      throw std::runtime_error(
          "aging source exceeds100000 faces/200000 vertices");
    const auto world = need(d.scene().world_transform(r.object));
    if (std::min({std::abs(world.scale.x), std::abs(world.scale.y),
                  std::abs(world.scale.z)}) < 1e-9)
      throw std::runtime_error("aging cannot bake a singular transform");
    auto program = need(compile_authoring_material(
        d.material_catalog(), r.source_material, r.source_variant, textures));
    if (program.uv_set() && *program.uv_set() != r.uv_set)
      throw std::runtime_error(
          "aging UV must match the source material program");
    AuthoringAgingBake result;
    result.material = need(bake_authoring_material(program, r.resolution));
    result.source_dependency_digest = need(aging_source_digest(
        d.material_catalog(), r.source_material, r.source_variant));
    const auto &coat = result.material.maps[5].pixels;
    for (std::size_t p = 0; p < coat.size(); p += 4)
      if (coat[p] != coat[0] || coat[p + 1] != coat[1])
        throw std::runtime_error("aging cannot flatten spatial clearcoat "
                                 "without a supported output binding");
    const auto center = need(program.evaluate(
        {{.5, .5}, {1. / r.resolution, 0}, {0, 1. / r.resolution}}));
    result.material.factors.clearcoat = center.material.clearcoat;
    result.material.factors.clearcoat_roughness =
        center.material.clearcoat_roughness;
    const auto position_recipe =
        need(bind_world_position_recipe("aging.positions", d, r.object.value,
                                        r.uv_set, r.resolution, r.resolution));
    const auto positions = compile_world_position_cpu(position_recipe, d);
    if (!positions.succeeded())
      throw std::runtime_error(positions.diagnostics.empty()
                                   ? "aging UV/world-position bake failed"
                                   : positions.diagnostics.front().message);
    CurvatureRecipe curvature;
    curvature.binding.identity = "aging.curvature";
    curvature.binding.source = need(bind_bake_source("aging.source", mesh));
    curvature.binding.target =
        need(bind_bake_target("aging.target", mesh, r.uv_set));
    curvature.binding.target_uv_set = r.uv_set;
    curvature.binding.width = curvature.binding.height = r.resolution;
    curvature.binding.projection_half_extent = .01;
    curvature.normalization_scale = 10;
    const auto curves = compile_curvature_cpu(curvature, mesh, mesh);
    if (!curves.succeeded())
      throw std::runtime_error(curves.diagnostics.empty()
                                   ? "aging curvature bake failed"
                                   : curves.diagnostics.front().message);
    const auto compiled = need(mesh.compile());
    const auto domain = need(surface::build_surface_domain(
        compiled, {r.object.value}, "authoring.surface", "standard"));
    auto analysis = need(surface::analyze_surface(compiled, domain));
    const std::string identity =
        d.source_namespace() + "/aging/" + std::to_string(r.object.value) +
        "/" + r.operation_identity + "/" + std::to_string(r.controls.seed);
    const auto seed_digest =
        assets::sha256({reinterpret_cast<const std::uint8_t *>(identity.data()),
                        identity.size()});
    result.stable_seed = read64(seed_digest.bytes, 0);
    auto controls = r.controls;
    controls.seed = result.stable_seed;
    analysis.mean_normal = {0, 0, 1};
    analysis.visibility.cavity = 0;
    const auto exposed = need(surface::derive_weathering(analysis, controls));
    analysis.visibility.cavity = 1;
    const auto sheltered = need(surface::derive_weathering(analysis, controls));
    std::map<geometry::FaceId, core::Vec3d> normals;
    for (std::size_t i = 0; i < compiled.triangle_faces.size(); ++i) {
      const auto a = compiled.indices[i * 3], b = compiled.indices[i * 3 + 1],
                 c = compiled.indices[i * 3 + 2];
      normals[compiled.triangle_faces[i]] =
          normals[compiled.triangle_faces[i]] +
          core::cross(compiled.positions[b] - compiled.positions[a],
                      compiled.positions[c] - compiled.positions[a]);
    }
    for (auto &[id, n] : normals) {
      static_cast<void>(id);
      n = world.rotation
              .rotate({n.x / world.scale.x, n.y / world.scale.y,
                       n.z / world.scale.z})
              .normalized();
      if (!n.finite())
        throw std::runtime_error("aging face normal is invalid");
    }
    result.masks.width = result.masks.height = r.resolution;
    result.masks.pixels.assign(std::size_t(r.resolution) * r.resolution * 4, 0);
    const auto q = world.rotation.normalized();
    const core::Quaternion inverse{-q.x, -q.y, -q.z, q.w};
    const auto sun = core::Vec3d{.5, .8, .25}.normalized();
    const double age = std::clamp(r.controls.age / 100., 0., 1.);
    for (std::size_t p = 0; p < result.masks.pixels.size() / 4; ++p) {
      const auto face = read64(positions.artifact->output_bytes, p * 32);
      if (!face)
        continue;
      const auto &n = normals.at({face});
      core::Vec3d point{std::bit_cast<double>(read64(
                            positions.artifact->output_bytes, p * 32 + 8)),
                        std::bit_cast<double>(read64(
                            positions.artifact->output_bytes, p * 32 + 16)),
                        std::bit_cast<double>(read64(
                            positions.artifact->output_bytes, p * 32 + 24))};
      point = inverse.rotate(point - world.translation);
      point = {point.x / world.scale.x, point.y / world.scale.y,
               point.z / world.scale.z};
      const auto grain =
          noise(result.stable_seed, point, r.feature_scale_meters);
      const double signed_curve =
          (int(curves.artifact->rgba8[p * 4]) - 128) / 127.;
      const double cavity = std::clamp(-signed_curve, 0., 1.),
                   edge = std::clamp(signed_curve, 0., 1.);
      const double up = std::max(0., n.y),
                   sunlight = std::max(0., core::dot(n, sun));
      const double oxidation = std::clamp(
          std::lerp(exposed.oxidation, sheltered.oxidation, cavity) * age *
              (1 - r.controls.maintenance) *
              (.5 + .5 * r.controls.rain_exposure * up) * (.75 + .5 * grain),
          0., 1.);
      const double wear = std::clamp(exposed.contact_wear * age *
                                         (.15 + .85 * edge) * (.25 + .75 * up),
                                     0., 1.);
      const double fade = std::clamp(exposed.solar_fade * sunlight, 0., 1.);
      auto &masks = result.masks.pixels;
      masks[p * 4] = byte(oxidation);
      masks[p * 4 + 1] = byte(wear);
      masks[p * 4 + 2] = byte(fade);
      masks[p * 4 + 3] = 255;
      ++result.covered_texels;
      for (std::size_t c = 0; c < 3; ++c) {
        const auto source = result.material.maps[0].pixels[p * 4 + c];
        if (oxidation == 0 && wear == 0 && fade == 0 && r.variation == 0)
          continue;
        const double base = linear(source);
        const double oxide = c == 0 ? .18 : c == 1 ? .07 : .025;
        double value = std::lerp(base, oxide, oxidation * .65);
        value = std::lerp(value, .55, fade * .2) + wear * .04;
        value *=
            1 + r.variation * ((grain - .5) +
                               hash_value(result.stable_seed, 0, 0, 0) - .5);
        result.material.maps[0].pixels[p * 4 + c] = byte(value, true);
      }
      if (oxidation != 0 || wear != 0) {
        auto &mr = result.material.maps[1].pixels;
        mr[p * 4 + 1] = byte(std::clamp(
            mr[p * 4 + 1] / 255. + oxidation * .35 - wear * .2, 0., 1.));
        mr[p * 4 + 2] = byte(mr[p * 4 + 2] / 255. * (1 - oxidation * .8));
      }
    }
    if (!result.covered_texels)
      throw std::runtime_error("aging bake has no UV coverage");
    return R::success(std::move(result));
  } catch (const std::exception &e) {
    return R::failure({core::ErrorCode::validation_failed,
                       std::string("aging bake: ") + e.what()});
  }
}
} // namespace carto::production
