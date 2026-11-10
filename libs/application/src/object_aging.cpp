#include <algorithm>
#include <carto/application/application.hpp>
#include <carto/production/authoring_aging.hpp>
#include <carto/production/material_binding.hpp>
#include <limits>
#include <set>
#include <stdexcept>
namespace carto::application {
namespace {
template <class T> T need(core::Result<T> r) {
  if (!r)
    throw std::runtime_error(r.error().message);
  return std::move(r.value());
}
void checked(core::Result<void> r) {
  if (!r)
    throw std::runtime_error(r.error().message);
}
std::uint64_t next_id(const auto &records, std::size_t count = 1) {
  const auto last = records.empty() ? 0 : records.rbegin()->first;
  if (last > std::numeric_limits<std::uint64_t>::max() - count)
    throw std::runtime_error("aging asset identity space exhausted");
  return last + 1;
}
void unlocked(const project::ProjectDocument &d, scene::ObjectId id) {
  for (auto current = std::optional<scene::ObjectId>{id}; current;) {
    const auto *o = d.scene().find(*current);
    if (!o || o->locked)
      throw std::runtime_error("aging object/ancestor is missing or locked");
    current = o->parent;
  }
}
std::set<std::uint32_t> slots(const geometry::EditableMesh &mesh) {
  const auto layer = need(production::authoring_material_slot_layer(mesh));
  if (!layer)
    throw std::runtime_error(
        "assign the source material to mesh regions before aging");
  std::set<std::uint32_t> ids;
  for (const auto &v : mesh.attributes().find(*layer)->values) {
    const auto *id = std::get_if<std::uint32_t>(&v);
    if (!id || !*id)
      throw std::runtime_error("aging requires positive material regions");
    ids.insert(*id);
  }
  if (ids.empty())
    throw std::runtime_error("aging mesh has no material regions");
  return ids;
}
bool shared(const project::ProjectDocument &d,
            const project::ObjectAgingLayer &layer) {
  for (const auto &o : d.scene().objects_sorted())
    if (o.id != layer.object && o.mesh_asset == layer.isolated_mesh)
      return true;
  for (const auto &[key, a] : d.material_catalog().assignments())
    if (a.material == layer.output_material &&
        key.mesh_asset != layer.isolated_mesh)
      return true;
  for (const auto &[id, m] : d.material_catalog().materials())
    if (id != layer.output_material)
      for (const auto *ref :
           {&m.definition.base_color, &m.definition.metallic_roughness,
            &m.definition.normal, &m.definition.occlusion,
            &m.definition.emissive})
        if (*ref && std::find(layer.output_textures.begin(),
                              layer.output_textures.end(),
                              ref->asset) != layer.output_textures.end())
          return true;
  for (const auto &[id, v] : d.material_catalog().variants()) {
    static_cast<void>(id);
    if (v.base_material == layer.output_material)
      return true;
    for (const auto *ref :
         {&v.overrides.base_color, &v.overrides.metallic_roughness,
          &v.overrides.normal, &v.overrides.occlusion, &v.overrides.emissive})
      if (*ref &&
          std::find(layer.output_textures.begin(), layer.output_textures.end(),
                    (*ref)->asset) != layer.output_textures.end())
        return true;
  }
  for (const auto &[id, g] : d.material_catalog().material_graphs()) {
    static_cast<void>(id);
    for (const auto &n : g.nodes)
      if (n.kind == project::MaterialGraphNodeKind::texture_sample &&
          std::find(layer.output_textures.begin(), layer.output_textures.end(),
                    n.texture_asset) != layer.output_textures.end())
        return true;
  }
  return false;
}
std::vector<std::uint8_t> tga(const assets::Rgba8MipLevel &image) {
  if (!image.width || !image.height || image.width > 256 ||
      image.height > 256 ||
      image.pixels.size() != std::size_t(image.width) * image.height * 4)
    throw std::runtime_error("invalid bounded aging image");
  std::vector<std::uint8_t> out(18);
  out[2] = 2;
  out[12] = static_cast<std::uint8_t>(image.width);
  out[13] = static_cast<std::uint8_t>(image.width >> 8);
  out[14] = static_cast<std::uint8_t>(image.height);
  out[15] = static_cast<std::uint8_t>(image.height >> 8);
  out[16] = 32;
  out[17] = 0x28;
  out.reserve(18 + image.pixels.size());
  for (std::size_t p = 0; p < image.pixels.size(); p += 4)
    out.insert(out.end(), {image.pixels[p + 2], image.pixels[p + 1],
                           image.pixels[p], image.pixels[p + 3]});
  return out;
}
} // namespace
core::Result<DispatchReceipt>
ApplicationSession::apply_object_aging(const ApplyObjectAgingAction &action) {
  if (active_source_ != OperationSource::human)
    return failure(
        {core::ErrorCode::unsupported, "aging bake requires human authoring"});
  if (project_generation_ != action.expected_project_generation ||
      document_.revision() != action.expected_project_revision)
    return failure({core::ErrorCode::stale_data, "aging snapshot is stale"});
  if (!project_path_)
    return failure({core::ErrorCode::invalid_state,
                    "save before generating managed aging maps"});
  auto r = action.layer;
  if (auto v = r.validate_parameters(); !v)
    return failure(v.error());
  try {
    unlocked(document_, r.object);
    const auto *node = document_.scene().find(r.object);
    if (!node->mesh_asset)
      throw std::runtime_error("aging requires one mesh instance");
    const auto previous = document_.aging_layers().find(r.object);
    const bool updating = previous != document_.aging_layers().end();
    const auto source_mesh = *node->mesh_asset;
    const auto regions = slots(document_.meshes().at(source_mesh));
    if (updating) {
      const auto &old = previous->second;
      if (old.operation_identity != r.operation_identity ||
          old.isolated_mesh != source_mesh ||
          r.source_material == old.output_material)
        throw std::runtime_error(
            "aging operation/mesh changed or source feeds back into "
            "its own output; unlink before replacing it");
      if (document_.material_catalog().material_graph_bindings().contains(
              old.output_material))
        throw std::runtime_error(
            "aging output has an authored graph edit; unlink before rebaking");
    }
    for (auto region : regions) {
      auto bound = document_.material_catalog().assignments().find(
          {source_mesh, region});
      if (bound == document_.material_catalog().assignments().end() ||
          bound->second.material != (updating ? previous->second.output_material
                                              : r.source_material) ||
          bound->second.variant !=
              (updating ? std::optional<project::MaterialVariantId>{}
                        : r.source_variant))
        throw std::runtime_error("bounded aging requires one source "
                                 "material/variant across all regions; "
                                 "existing manual material edits are retained");
    }
    const auto root = project_path_->parent_path().empty()
                          ? std::filesystem::current_path()
                          : project_path_->parent_path();
    assets::BlobStore store(root / ".carto-assets" / "source-blobs");
    production::AuthoringTextureLibrary library;
    const auto base = need(document_.material_catalog().resolve_material(
        r.source_material, r.source_variant));
    std::set<assets::TextureAssetId> inputs;
    for (const auto *ref : {&base.base_color, &base.metallic_roughness,
                            &base.normal, &base.occlusion, &base.emissive})
      if (*ref)
        inputs.insert(ref->asset);
    if (updating)
      for (auto id : inputs)
        if (std::find(previous->second.output_textures.begin(),
                      previous->second.output_textures.end(),
                      id) != previous->second.output_textures.end())
          throw std::runtime_error(
              "aging source must not sample its generated output");
    for (auto id : inputs) {
      const auto &texture = document_.material_catalog().textures().at(id);
      checked(library.admit(texture, need(store.read(texture.content_digest))));
    }
    const auto baked =
        need(production::bake_authoring_aging(document_, r, &library));
    auto candidate = document_;
    auto catalog = document_.material_catalog();
    const bool fresh = !updating || shared(document_, previous->second);
    r.isolated_mesh =
        fresh ? need(candidate.add_mesh(document_.meshes().at(source_mesh)))
              : source_mesh;
    if (fresh)
      checked(candidate.attach_mesh(r.object, r.isolated_mesh));
    if (fresh) {
      r.output_material = next_id(catalog.materials());
      r.output_sampler = next_id(catalog.samplers());
      const auto texture_start = next_id(catalog.textures(), 6);
      for (std::size_t i = 0; i < 6; ++i)
        r.output_textures[i] = texture_start + i;
      project::SamplerIntent sampler;
      sampler.id = r.output_sampler;
      sampler.name = "Aging clamp sampler";
      sampler.address_u = sampler.address_v =
          project::SamplerAddressMode::clamp_to_edge;
      sampler.revision = core::Revision{1};
      checked(catalog.insert_sampler(sampler));
    } else {
      r.output_material = previous->second.output_material;
      r.output_sampler = previous->second.output_sampler;
      r.output_textures = previous->second.output_textures;
    }
    std::array<std::vector<std::uint8_t>, 6> encoded;
    for (std::size_t i = 0; i < 6; ++i) {
      encoded[i] = tga(i < 5 ? baked.material.maps[i] : baked.masks);
      project::TextureSourceRecord texture;
      texture.id = r.output_textures[i];
      texture.name = "Aging " + r.operation_identity + " " + std::to_string(i);
      texture.content_digest = assets::sha256(encoded[i]);
      texture.format = assets::TextureSourceFormat::tga;
      texture.width = texture.height = r.resolution;
      texture.relative_locator = store.path_for(texture.content_digest)
                                     .lexically_relative(root)
                                     .generic_string();
      texture.provenance = "Independent heuristic aging bake; object " +
                           std::to_string(r.object.value) + "; operation " +
                           r.operation_identity;
      texture.revision = core::Revision{1};
      if (fresh)
        checked(catalog.insert_texture(texture));
      else {
        const auto &old = catalog.textures().at(texture.id);
        if (texture.content_digest != old.content_digest) {
          if (old.revision.exhausted())
            throw std::runtime_error("aging texture revision exhausted");
          texture.revision = old.revision.next();
          checked(catalog.replace_texture(texture));
        }
      }
    }
    render::MaterialAsset output;
    output.id = r.output_material;
    output.name = "Aged " + r.operation_identity + " object " +
                  std::to_string(r.object.value);
    output.definition = baked.material.factors;
    output.revision = fresh ? core::Revision{1}
                            : catalog.materials().at(output.id).revision.next();
    const auto binding = [&](std::size_t i) {
      render::TextureRef ref;
      ref.asset = r.output_textures[i];
      ref.sampler = r.output_sampler;
      ref.uv_set = r.uv_set;
      ref.transform.pivot = {0, 0};
      ref.normal_convention = render::NormalConvention::opengl_y_plus;
      ref.interpretation = (i == 0 || i == 4)
                               ? render::TextureInterpretation::srgb
                               : render::TextureInterpretation::linear;
      return ref;
    };
    output.definition.base_color = binding(0);
    output.definition.metallic_roughness = binding(1);
    output.definition.normal =
        baked.material.normal_mapping ? binding(2) : render::TextureRef{};
    output.definition.occlusion = binding(3);
    output.definition.emissive = binding(4);
    if (fresh)
      checked(catalog.insert_material(output));
    else
      checked(catalog.replace_material(output));
    for (auto region : regions) {
      project::MaterialRegionAssignment assignment{
          {r.isolated_mesh, region}, r.output_material, {}};
      if (catalog.assignments().contains(assignment.key))
        checked(catalog.replace_assignment(assignment));
      else
        checked(catalog.insert_assignment(assignment));
    }
    checked(candidate.set_material_catalog(catalog));
    r.source_material_revision =
        catalog.materials().at(r.source_material).revision;
    r.baked_mesh_revision = candidate.meshes().at(r.isolated_mesh).revision();
    r.baked_object = r.object;
    r.baked_world = need(candidate.scene().world_transform(r.object));
    r.source_program_digest = baked.material.program_digest;
    r.source_dependency_digest = baked.source_dependency_digest;
    checked(candidate.set_aging_layer(r));
    const auto recipe = r.serialize();
    const auto recipe_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(recipe.data()), recipe.size());
    const auto recipe_digest = assets::sha256(recipe_bytes);
    assets::SourceImportRecord provenance;
    provenance.identity =
        "aging." + std::to_string(r.object.value) + "." + r.operation_identity;
    provenance.relative_source_path =
        store.path_for(recipe_digest).lexically_relative(root).generic_string();
    const auto prior =
        candidate.source_asset_registry().records().find(provenance.identity);
    provenance.source_generation =
        prior == candidate.source_asset_registry().records().end()
            ? core::Revision{1}
            : prior->second.source_generation.next();
    provenance.kind = assets::SourceAssetKind::material;
    provenance.source_digest = recipe_digest;
    provenance.importer_identity = "cartographer.authoring-aging.v1";
    // Operation identity remains stable when isolating a consumer of an
    // older appearance; generated material/texture IDs may legitimately change.
    provenance.artifact_identity = provenance.identity;
    project::MaterialCatalog artifact;
    checked(artifact.insert_material(output));
    checked(artifact.insert_sampler(catalog.samplers().at(r.output_sampler)));
    for (auto id : r.output_textures)
      checked(artifact.insert_texture(catalog.textures().at(id)));
    provenance.artifact_digest = need(artifact.canonical_digest());
    provenance.domain_digests = {
        {assets::SourceAssetDomain::material, provenance.artifact_digest}};
    provenance.options.import_hierarchy = provenance.options.import_normals =
        false;
    checked(candidate.source_asset_registry_.set_record(provenance));
    checked(candidate.validate());
    for (const auto &image : encoded)
      static_cast<void>(need(store.put(image, "image/x-tga")));
    static_cast<void>(need(store.put(recipe_bytes, "text/plain")));
    editor::ProjectCommandAdmission admission;
    auto receipt = execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, std::move(candidate), "Bake Object Aging"),
        "Bake Object Aging " + r.operation_identity,
        AffectedSet{{r.object}, {}, {}, {}});
    if (receipt)
      receipt.value().warnings.push_back(
          {core::ErrorCode::unsupported,
           "Explicit unit-tile RGBA8 heuristic aging bake; not physical "
           "erosion or GI. "
           "Re-bake after geometry/UV/source or object-transform edits; base "
           "sources remain "
           "editable."});
    return receipt;
  } catch (const std::exception &e) {
    return failure({core::ErrorCode::validation_failed, e.what()});
  }
}
core::Result<DispatchReceipt>
ApplicationSession::remove_object_aging(const RemoveObjectAgingAction &action) {
  if (active_source_ != OperationSource::human)
    return failure({core::ErrorCode::unsupported,
                    "aging unlink requires human authoring"});
  if (project_generation_ != action.expected_project_generation ||
      document_.revision() != action.expected_project_revision)
    return failure(
        {core::ErrorCode::stale_data, "aging unlink snapshot is stale"});
  try {
    unlocked(document_, action.object);
    const auto found = document_.aging_layers().find(action.object);
    if (found == document_.aging_layers().end())
      throw std::runtime_error("object has no aging operation");
    const auto &r = found->second;
    const auto *o = document_.scene().find(action.object);
    auto candidate = document_;
    auto catalog = document_.material_catalog();
    if (o->mesh_asset && *o->mesh_asset == r.isolated_mesh) {
      auto target = r.isolated_mesh;
      if (shared(document_, r)) {
        target = need(candidate.add_mesh(document_.meshes().at(target)));
        checked(candidate.attach_mesh(r.object, target));
        for (const auto &[key, a] : document_.material_catalog().assignments())
          if (key.mesh_asset == r.isolated_mesh) {
            auto copy = a;
            copy.key.mesh_asset = target;
            checked(catalog.insert_assignment(copy));
          }
      }
      auto variant = r.source_variant;
      if (variant) {
        const auto &source = catalog.variants().at(*variant);
        if (!source.permitted_mesh_assets.empty() &&
            !std::binary_search(source.permitted_mesh_assets.begin(),
                                source.permitted_mesh_assets.end(), target)) {
          auto copy = source;
          copy.id = next_id(catalog.variants());
          copy.name += " restored";
          copy.permitted_mesh_assets = {target};
          copy.revision = core::Revision{1};
          variant = copy.id;
          checked(catalog.insert_variant(copy));
        }
      }
      std::vector<project::MaterialRegionAssignment> restore;
      for (const auto &[key, a] : catalog.assignments())
        if (key.mesh_asset == target && a.material == r.output_material)
          restore.push_back({key, r.source_material, variant});
      for (const auto &a : restore)
        checked(catalog.replace_assignment(a));
    }
    checked(candidate.erase_aging_layer(action.object));
    checked(candidate.set_material_catalog(catalog));
    checked(candidate.validate());
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, std::move(candidate), "Unlink Object Aging"),
        "Unlink Object Aging", AffectedSet{{action.object}, {}, {}, {}});
  } catch (const std::exception &e) {
    return failure({core::ErrorCode::validation_failed, e.what()});
  }
}
} // namespace carto::application
