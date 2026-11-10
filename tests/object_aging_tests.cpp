#include "application_access.hpp"
#include "gltf_test_payload.hpp"
#include "scene_import_test_support.hpp"
#include <carto/production/authoring_aging.hpp>
#include <carto/production/authoring_viewport.hpp>
#include <locale>
namespace {
void send(application::ApplicationSession &s,
          const application::ApplicationAction &a) {
  take(application::testing::dispatch(s, a));
}
project::ProjectDocument source(application::ApplicationSession &s) {
  return take(s.authoring_viewport_source(s.snapshot().project_revision));
}
project::ProjectDocument fixture(const std::filesystem::path &path) {
  auto d = take(project::ProjectDocument::create(
      "Independent two-instance aging fixture"));
  journal::Journal j(path.parent_path() / "fixture-transaction.journal");
  auto initial = d.serialize();
  take(j.append(
      {{},
       d.revision(),
       "aging.fixture",
       {reinterpret_cast<const std::uint8_t *>(initial.data()), initial.size()},
       std::nullopt}));
  auto tx = take(project::ProjectTransaction::begin(d, j, "aging.fixture"));
  geometry::EditableMesh mesh;
  okay(mesh.insert_bulk(
      {{{1}, {0, 0, 0}}, {{2}, {1, 0, 0}}, {{3}, {1, 1, 0}}, {{4}, {0, 1, 0}}},
      {{{1}, {{1}, {2}, {3}}}, {{2}, {{1}, {3}, {4}}}}));
  attributes::AttributeSet attrs({{attributes::AttributeDomain::corner, 6},
                                  {attributes::AttributeDomain::face, 2}});
  okay(attrs.add_layer(
      {{"source.uv", attributes::AttributeDomain::corner,
        attributes::AttributeType::vec2},
       {core::Vec2d{0, 0}, core::Vec2d{1, 0}, core::Vec2d{1, 1},
        core::Vec2d{0, 0}, core::Vec2d{1, 1}, core::Vec2d{0, 1}}}));
  okay(attrs.add_layer(
      {{"cartographer.material_slot", attributes::AttributeDomain::face,
        attributes::AttributeType::uint32},
       {std::uint32_t(1), std::uint32_t(1)}}));
  attributes::UvSetTable uv;
  okay(uv.add({7, "Source UV", "source.uv", true, true, false, 1}));
  okay(mesh.set_attribute_payload(std::move(attrs), std::move(uv)));
  const auto asset = take(tx.add_mesh(mesh));
  okay(tx.insert_object({{1}, "First", {}, std::nullopt, asset}));
  okay(tx.insert_object({{2}, "Second", {{2, 0, 0}}, std::nullopt, asset}));
  project::MaterialCatalog catalog;
  render::MaterialAsset mat;
  mat.id = 1;
  mat.name = "Original standard source";
  mat.definition.base_color_factor = {.35, .35, .35, 1};
  mat.definition.metallic = .8;
  mat.definition.roughness = .3;
  mat.revision = core::Revision{1};
  okay(catalog.insert_material(mat));
  okay(catalog.insert_assignment({{asset, 1}, 1, {}}));
  okay(tx.set_material_catalog(catalog));
  project::PresentationCatalog presentation;
  project::AuthoredEnvironment environment;
  environment.identity = "public.synthetic.aging.environment";
  environment.width = 4;
  environment.height = 2;
  environment.linear_radiance.assign(8, core::Vec3d{.4, .3, .2});
  environment.linear_radiance[1] = {1.5, .8, .3};
  okay(presentation.set_environment(environment));
  okay(tx.set_presentation_catalog(presentation));
  take(tx.commit());
  take(d.save_atomic(path));
  return d;
}
project::ObjectAgingLayer recipe(scene::ObjectId object) {
  project::ObjectAgingLayer r;
  r.object = object;
  r.operation_identity = "aging.test";
  r.source_material = 1;
  r.uv_set = 7;
  r.resolution = 16;
  r.feature_scale_meters = .2;
  r.controls.seed = 9;
  return r;
}
void apply(application::ApplicationSession &s,
           const project::ObjectAgingLayer &r) {
  const auto snap = s.snapshot();
  send(s, application::ApplyObjectAgingAction{snap.project_generation,
                                              snap.project_revision, r});
}
std::vector<std::uint8_t> pixels(const project::ProjectDocument &d,
                                 const std::filesystem::path &path,
                                 assets::TextureAssetId id) {
  assets::BlobStore store(path.parent_path() / ".carto-assets" /
                          "source-blobs");
  return take(assets::decode_texture_rgba8(take(store.read(
                  d.material_catalog().textures().at(id).content_digest))))
      .pixels;
}
std::vector<assets::Sha256Digest> digests(const project::ProjectDocument &d,
                                          const project::ObjectAgingLayer &r) {
  std::vector<assets::Sha256Digest> result;
  for (auto id : r.output_textures)
    result.push_back(d.material_catalog().textures().at(id).content_digest);
  return result;
}
production::AuthoringViewportImage image(const project::ProjectDocument &d,
                                         const std::filesystem::path &path) {
  auto library = std::make_shared<production::AuthoringTextureLibrary>();
  assets::BlobStore store(path.parent_path() / ".carto-assets" /
                          "source-blobs");
  for (const auto &[id, t] : d.material_catalog().textures()) {
    static_cast<void>(id);
    okay(library->admit(t, take(store.read(t.content_digest))));
  }
  production::AuthoringViewportRequest r;
  r.width = 128;
  r.height = 64;
  r.view.position = {1.5, .5, 3};
  r.view.rotation = {0, 0, 0, 1};
  r.view.orthographic_height = 1.5;
  r.textures = library;
  return take(production::render_authoring_viewport(d, r));
}
void ppm(const std::filesystem::path &path, unsigned width, unsigned height,
         std::span<const std::uint8_t> rgba) {
  std::ofstream out(path, std::ios::binary);
  check(bool(out) && rgba.size() == std::size_t(width) * height * 4,
        "bounded reference image output");
  out << "P6\n" << width << ' ' << height << "\n255\n";
  for (std::size_t p = 0; p < rgba.size(); p += 4)
    out.write(reinterpret_cast<const char *>(rgba.data() + p), 3);
  check(bool(out), "reference image written");
}
} // namespace
int main(int argc, char **argv) {
  try {
    auto root =
        std::filesystem::temp_directory_path() /
        ("object-aging-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    if (argc == 3 && std::string_view(argv[1]) == "--evidence") {
      root = argv[2];
      check(!std::filesystem::exists(root), "evidence destination must be new");
    } else {
      check(argc == 1, "usage: object_aging_tests [--evidence new-directory]");
    }
    std::filesystem::create_directories(root);
    const auto path = root / "aging.carto";
    auto d = fixture(path);
    const auto before = d.serialize();
    auto r = recipe({1});
    const auto neutral = take(production::bake_authoring_aging(d, r));
    const auto program =
        take(production::compile_authoring_material(d.material_catalog(), 1));
    const auto baseline =
        take(production::bake_authoring_material(program, 16));
    for (unsigned i = 0; i < 5; ++i)
      check(neutral.material.maps[i].pixels == baseline.maps[i].pixels,
            "age0/variation0 preserves the bounded source bake exactly");
    for (std::size_t p = 0; p < neutral.masks.pixels.size(); p += 4)
      check(neutral.masks.pixels[p] == 0 && neutral.masks.pixels[p + 1] == 0 &&
                neutral.masks.pixels[p + 2] == 0,
            "zero-age cause masks are neutral");
    check(d.serialize() == before, "baking does not mutate source document");
    const std::string v40_header = "CARTOGRAPHER_PROJECT 40";
    check(before.starts_with(v40_header), "integrated project uses schema v40");
    for (const std::string table : {"RIG_TEMPLATE_BINDINGS 0\n",
                                    "AGING_LAYERS 0\n"}) {
      auto missing_table = before;
      const auto at = missing_table.rfind(table);
      check(at != std::string::npos, "v40 mandatory table exists");
      missing_table.erase(at, table.size());
      check(!project::ProjectDocument::deserialize(missing_table),
            "v40 cannot omit an integrated table");
    }
    auto legacy = before;
    const auto cut = legacy.find("CONNECTION_PORT_GRAPH ");
    check(cut != std::string::npos, "v34 realism layout boundary exists");
    legacy.erase(cut);
    legacy += "RIG_TEMPLATE_BINDINGS 0\nEND\n";
    legacy.replace(0, v40_header.size(), "CARTOGRAPHER_PROJECT 34");
    check(take(project::ProjectDocument::deserialize(legacy))
              .aging_layers()
              .empty(),
          "v34 migration adds no invented operation");
    application::ApplicationSession s;
    send(s, application::OpenProjectAction{path});
    r.controls.age = 80;
    r.controls.rain_exposure = .9;
    r.controls.solar_exposure = .8;
    r.controls.moisture = .9;
    r.controls.traffic = .8;
    r.variation = .2;
    const auto original_frame = image(d, path);
    apply(s, r);
    auto aged0 = source(s);
    auto layer = aged0.aging_layers().at({1});
    {
      struct CommaDecimal final : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
      };
      struct RestoreLocale {
        std::locale previous;
        ~RestoreLocale() { std::locale::global(previous); }
      } restore{std::locale()};
      const auto canonical_layer = layer.serialize();
      const auto canonical_project = aged0.serialize();
      std::locale::global(std::locale(restore.previous, new CommaDecimal));
      const auto foreign_layer = layer.serialize();
      const auto foreign_project = aged0.serialize();
      const auto parsed_layer = project::ObjectAgingLayer::deserialize(canonical_layer);
      const auto parsed_project = project::ProjectDocument::deserialize(canonical_project);
      std::locale::global(restore.previous);
      check(foreign_layer == canonical_layer && parsed_layer,
            "aging records use the canonical decimal locale in both directions");
      check(parsed_project && project::ProjectDocument::deserialize(foreign_project),
            "aging projects reopen across decimal locales");
    }
    check(aged0.scene().find({1})->mesh_asset !=
              aged0.scene().find({2})->mesh_asset,
          "aging isolates one shared mesh instance");
    check(aged0.material_catalog().materials().at(1).revision ==
                  core::Revision{1} &&
              aged0.material_catalog().assignments().at({1, 1}).material == 1,
          "original material and second-instance assignment remain untouched");
    check(!production::aging_needs_rebake(aged0, layer),
          "fresh dependency binding is valid");
    const auto first_hashes = digests(aged0, layer);
    const auto mask = pixels(aged0, path, layer.output_textures[5]);
    bool oxidation = false, wear = false, fade = false;
    for (std::size_t p = 0; p < mask.size(); p += 4) {
      oxidation |= mask[p] > 0;
      wear |= mask[p + 1] > 0;
      fade |= mask[p + 2] > 0;
    }
    check(oxidation && wear && fade,
          "rain/moisture, traffic and sun produce independent mask channels");
    const auto frame = image(aged0, path);
    if (argc == 3) {
      ppm(root / "cpu-reference-before.ppm", original_frame.width,
          original_frame.height, original_frame.display_rgba8);
      ppm(root / "cpu-reference-aged.ppm", frame.width, frame.height,
          frame.display_rgba8);
      ppm(root / "aging-cause-mask.ppm", layer.resolution, layer.resolution,
          mask);
    }
    std::size_t changed = 0, unchanged = 0;
    for (std::size_t p = 0; p < frame.objects.size(); ++p) {
      if (frame.objects[p] == scene::ObjectId{1} &&
          (frame.linear_hdr[p] - original_frame.linear_hdr[p]).length() > 1e-6)
        ++changed;
      if (frame.objects[p] == scene::ObjectId{2} &&
          (frame.linear_hdr[p] - original_frame.linear_hdr[p]).length() < 1e-12)
        ++unchanged;
    }
    if (changed <= 50 || unchanged <= 50)
      std::cerr << "pixel counts changed=" << changed
                << " unchanged=" << unchanged << '\n';
    check(changed > 50 && unchanged > 50,
          "actual reference pixels change only for the aged instance");
    send(s, application::UndoAction{});
    check(source(s).aging_layers().empty() &&
              source(s).scene().find({1})->mesh_asset == 1 &&
              source(s).material_catalog().materials().size() == 1,
          "undo restores geometry sharing, source material and operation "
          "admission");
    send(s, application::RedoAction{});
    check(digests(source(s), source(s).aging_layers().at({1})) == first_hashes,
          "redo retains generated identities and pixels");
    apply(s, r);
    auto aged1 = source(s);
    check(aged1.aging_layers().at({1}).output_textures ==
                  layer.output_textures &&
              digests(aged1, aged1.aging_layers().at({1})) == first_hashes,
          "same operation reuses IDs and pixel bytes");
    for (auto id : layer.output_textures)
      check(aged1.material_catalog().textures().at(id).revision ==
                core::Revision{1},
            "unchanged pixels do not advance texture/upload identity");
    auto moved = aged1.scene().find({2})->local_transform;
    moved.translation.x += 1;
    send(s, application::SetObjectTransformAction{{2}, moved, {}});
    check(!production::aging_needs_rebake(source(s),
                                          source(s).aging_layers().at({1})),
          "unrelated object edits do not invalidate or reshuffle aging");
    apply(s, r);
    check(digests(source(s), source(s).aging_layers().at({1})) == first_hashes,
          "re-bake after unrelated revision is deterministic");
    auto second = r;
    second.object = {2};
    apply(s, second);
    auto aged2 = source(s);
    const auto second_mask =
        pixels(aged2, path, aged2.aging_layers().at({2}).output_textures[5]);
    check(second_mask != mask, "same seed/operation on a second object has "
                               "stable object-specific variation");
    send(s, application::SaveProjectAction{std::nullopt});
    const auto reopened = take(project::ProjectDocument::load(path));
    check(
        reopened.aging_layers().size() == 2 &&
            reopened.aging_layers().at({1}).serialize() ==
                aged2.aging_layers().at({1}).serialize(),
        "causes, seed, operation identity and source/output bindings persist");
    check(image(reopened, path).display_rgba8 ==
              image(aged2, path).display_rgba8,
          "save/reopen reproduces authored reference image");
    auto invalid = r;
    invalid.uv_set = 999;
    auto snap = s.snapshot();
    const auto good = source(s).serialize();
    check(!application::testing::dispatch(
              s, application::ApplyObjectAgingAction{snap.project_generation,
                                                     snap.project_revision,
                                                     invalid}),
          "missing UV fails visibly");
    check(source(s).serialize() == good &&
              pixels(source(s), path, layer.output_textures[5]) == mask,
          "failed replacement retains prior operation and masks");
    invalid = r;
    invalid.variation = .6;
    check(!application::testing::dispatch(
              s, application::ApplyObjectAgingAction{snap.project_generation,
                                                     snap.project_revision,
                                                     invalid}),
          "out-of-budget variation rejected");
    check(!application::testing::dispatch(
              s, application::ApplyObjectAgingAction{snap.project_generation,
                                                     core::Revision{1}, r}),
          "stale authoring snapshot rejected");
    auto malformed = aged2.aging_layers().at({1});
    malformed.output_textures[1] = malformed.output_textures[0];
    check(!malformed.validate(), "duplicate generated identities refused");
    check(!project::ObjectAgingLayer::deserialize(
              aged2.aging_layers().at({1}).serialize() + " trailing"),
          "malformed persisted recipes rejected");
    r.controls.maintenance = 1;
    apply(s, r);
    auto aged3 = source(s);
    auto maintained =
        pixels(aged3, path, aged3.aging_layers().at({1}).output_textures[5]);
    for (std::size_t p = 0; p < maintained.size(); p += 4)
      check(maintained[p] == 0 && maintained[p + 1] == 0,
            "maintenance removes oxidation/contact wear without changing sun "
            "cause");
    io::GltfExportOptions options;
    options.project_path = path;
    const auto out = root / "aged.glb";
    auto exported = take(io::export_glb(aged3, out, options));
    check(std::any_of(exported.warnings.begin(), exported.warnings.end(),
                      [](const auto &w) {
                        return w.find("editable cause") != std::string::npos;
                      }),
          "export reports loss of editable recipe while retaining baked "
          "appearance");
    const auto good_export = read(out);
    auto material = aged3.material_catalog().materials().at(1);
    material.definition.base_color_factor.x = .5;
    snap = s.snapshot();
    send(s, application::UpdateMaterialAction{snap.project_revision, 1,
                                              material.revision,
                                              material.definition});
    auto aged4 = source(s);
    check(
        production::aging_needs_rebake(aged4, aged4.aging_layers().at({1})),
        "source edits invalidate the bake without discarding prior good maps");
    check(!io::export_glb(aged4, out, options) && read(out) == good_export,
          "stale aging export refuses before replacing a good artifact");
    apply(s, r);
    apply(s, second);
    auto aged5 = source(s);
    take(io::export_glb(aged5, out, options));
    // Exercise the exported artifact through the actual scene-import action,
    // then edit and persist it. Lighting is reapplied explicitly because glTF
    // export reports that Cartographer's environment is not represented.
    const auto roundtrip_path = root / "roundtrip.carto";
    auto empty =
        take(project::ProjectDocument::create("Public aging round-trip"));
    take(empty.save_atomic(roundtrip_path));
    application::ApplicationSession imported_session;
    send(imported_session, application::OpenProjectAction{roundtrip_path});
    auto imported_snap = imported_session.snapshot();
    take(carto::testing::import_gltf_scene_file(imported_session, out, true));
    imported_snap = imported_session.snapshot();
    send(imported_session,
         application::SetPresentationCatalogAction{
             imported_snap.project_generation, imported_snap.project_revision,
             aged5.presentation_catalog()});
    const auto imported_doc = source(imported_session);
    check(imported_doc.aging_layers().empty(),
          "glTF imports the baked appearance, not invented editable causes");
    const auto expected_image = image(aged5, path);
    const auto imported_image = image(imported_doc, roundtrip_path);
    check(expected_image.display_rgba8.size() ==
              imported_image.display_rgba8.size(),
          "round-trip frame extent");
    unsigned max_channel_error = 0;
    for (std::size_t p = 0; p < expected_image.display_rgba8.size(); ++p)
      max_channel_error =
          std::max(max_channel_error,
                   unsigned(std::abs(int(expected_image.display_rgba8[p]) -
                                     int(imported_image.display_rgba8[p]))));
    if (max_channel_error > 3)
      std::cerr << "round-trip max display-channel error=" << max_channel_error
                << '\n';
    check(max_channel_error <= 3, "export and real scene reimport retain lit "
                                  "reference pixels within 3/255");
    const auto imported_object =
        imported_doc.scene().objects_sorted().front().id;
    const auto imported_mesh =
        *imported_doc.scene().find(imported_object)->mesh_asset;
    auto imported_recipe = recipe(imported_object);
    imported_recipe.uv_set = imported_doc.meshes()
                                 .at(imported_mesh)
                                 .uv_sets()
                                 .active_for_render()
                                 ->id;
    const auto imported_assignment =
        std::find_if(imported_doc.material_catalog().assignments().begin(),
                     imported_doc.material_catalog().assignments().end(),
                     [&](const auto &entry) {
                       return entry.first.mesh_asset == imported_mesh;
                     });
    check(imported_assignment !=
              imported_doc.material_catalog().assignments().end(),
          "reimport exposes an actual material-region binding");
    imported_recipe.source_material = imported_assignment->second.material;
    imported_recipe.controls = r.controls;
    imported_recipe.variation = .1;
    apply(imported_session, imported_recipe);
    const auto edited_import = source(imported_session);
    check(!edited_import.aging_layers().empty(),
          "imported material can be aged again");
    const auto edited_frame = image(edited_import, roundtrip_path);
    check(edited_frame.display_rgba8 != imported_image.display_rgba8,
          "editing a reimported material changes the rendered result");
    send(imported_session, application::SaveProjectAction{std::nullopt});
    const auto reopened_import =
        take(project::ProjectDocument::load(roundtrip_path));
    check(image(reopened_import, roundtrip_path).display_rgba8 ==
              edited_frame.display_rgba8,
          "import, aging edit, render and save/reopen remain coherent");
    if (argc == 3) {
      ppm(root / "cpu-reference-reimport.ppm", imported_image.width,
          imported_image.height, imported_image.display_rgba8);
      ppm(root / "cpu-reference-reimport-edited.ppm", edited_frame.width,
          edited_frame.height, edited_frame.display_rgba8);
    }
    std::cout << "Round-trip max display-channel error: " << max_channel_error
              << "/255\n";
    const auto copy_receipt = application::testing::dispatch(
        s,
        application::DuplicateMeshInstanceAction{{1}, "Aged copy", {4, 0, 0}});
    take(copy_receipt);
    const auto copied = source(s);
    const auto copy_id = copied.scene().objects_sorted().back().id;
    check(copied.aging_layers().contains(copy_id) &&
              production::aging_needs_rebake(copied,
                                             copied.aging_layers().at(copy_id)),
          "duplicate retains editable causes and explicitly needs its own "
          "identity bake");
    const auto copy_recipe = copied.aging_layers().at(copy_id).serialize();
    send(s, application::UndoAction{});
    check(!source(s).aging_layers().contains(copy_id),
          "duplicate undo removes copied causes");
    send(s, application::RedoAction{});
    check(source(s).aging_layers().at(copy_id).serialize() == copy_recipe,
          "duplicate redo restores causes and prior baked identity");
    const auto old_mesh = copied.scene().find(copy_id)->mesh_asset;
    const auto old_output = copied.aging_layers().at({1});
    const auto old_mask = pixels(copied, path, old_output.output_textures[5]);
    ++r.controls.seed;
    apply(s, r);
    auto aged6 = source(s);
    check(aged6.scene().find(copy_id)->mesh_asset == old_mesh &&
              aged6.scene().find({1})->mesh_asset != old_mesh &&
              pixels(aged6, path, old_output.output_textures[5]) == old_mask,
          "updating a shared aged instance allocates isolated outputs and "
          "preserves copy "
          "appearance");
    auto copy_bake = aged6.aging_layers().at(copy_id);
    const auto a_hashes = digests(aged6, aged6.aging_layers().at({1}));
    const auto copy_hashes = digests(aged6, copy_bake);
    apply(s, copy_bake);
    const auto copy_baked = source(s);
    check(!production::aging_needs_rebake(
              copy_baked, copy_baked.aging_layers().at(copy_id)) &&
              copy_baked.aging_layers().at(copy_id).baked_object == copy_id &&
              copy_baked.aging_layers().at(copy_id).output_textures !=
                  copy_baked.aging_layers().at({1}).output_textures &&
              digests(copy_baked, copy_baked.aging_layers().at(copy_id)) !=
                  copy_hashes &&
              digests(copy_baked, copy_baked.aging_layers().at({1})) ==
                  a_hashes,
          "duplicate re-bake derives its own identity without changing the "
          "source instance");
    snap = s.snapshot();
    const auto old_layer = aged6.aging_layers().at({1});
    send(s, application::RemoveObjectAgingAction{
                snap.project_generation, snap.project_revision, {1}});
    auto unlinked = source(s);
    check(!unlinked.aging_layers().contains({1}) &&
              unlinked.material_catalog()
                      .assignments()
                      .at({*unlinked.scene().find({1})->mesh_asset, 1})
                      .material == 1,
          "unlink restores source appearance without undoing geometry");
    send(s, application::UndoAction{});
    check(source(s).aging_layers().at({1}).serialize() == old_layer.serialize(),
          "unlink has an independent complete undo");
    send(s, application::SaveProjectAction{std::nullopt});
    std::cout << "PASS deterministic per-object aging, cause masks, reference "
                 "pixels, isolation, "
                 "replacement atomicity, undo/reopen, invalidation and export "
                 "loss guards\n";
    std::cout << "Fixture: " << root.generic_string() << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
