#include "application_access.hpp"
#include "authoring_viewport_fixture.hpp"
#include "../apps/desktop/bake_workflow.hpp"
#include <carto/ui/ui.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/project/package.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/production/prepared_publication.hpp>
#include <fstream>
#include <iostream>

namespace {
void send(application::ApplicationSession& session, const application::ApplicationAction& action) {
    take(application::testing::dispatch(session, action));
}
std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    check(bool(in), "file readable");
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(bool(out), "image written");
}
std::vector<std::uint8_t> image(std::uint8_t r, bool checker = false, unsigned width = 8) {
    std::vector<std::uint8_t> out(18U);
    out[2] = 2; out[12] = static_cast<std::uint8_t>(width); out[14] = 8;
    out[16] = 32; out[17] = 0x28;
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < width; ++x) {
        const bool dark = checker && ((x / 2 + y / 2) % 2);
        out.insert(out.end(), {std::uint8_t(dark ? 24 : 190), std::uint8_t(dark ? 30 : 120),
            std::uint8_t(dark ? 220 : r), 255});
    }
    return out;
}
auto source(application::ApplicationSession& session) {
    return take(session.authoring_viewport_source(session.snapshot().project_revision));
}
production::AuthoringViewportRequest view() {
    production::AuthoringViewportRequest request;
    request.width = 384; request.height = 256;
    request.view.position = {3, 2, 4};
    const double yaw = std::atan2(3.0, 4.0), pitch = -std::atan2(1.75, 5.0);
    request.view.rotation = {std::sin(pitch/2)*std::cos(yaw/2), std::cos(pitch/2)*std::sin(yaw/2),
        -std::sin(pitch/2)*std::sin(yaw/2), std::cos(pitch/2)*std::cos(yaw/2)};
    request.view.orthographic_height = 2.4;
    return request;
}
auto render_frame(application::ApplicationSession& session) {
    const auto request = take(session.prepare_authoring_viewport_request(view(), session.snapshot().project_revision));
    return take(production::render_authoring_viewport(source(session), request));
}
void configure_light(application::ApplicationSession& session) {
    const auto snap = session.snapshot();
    project::AuthoredLight light;
    light.object = snap.objects.front().object.id; light.identity = "texturing.directional.key";
    light.kind = project::AuthoredLightKind::directional; light.intensity = 5; light.range = 0;
    light.revision = core::Revision{1U};
    project::PresentationCatalog presentation; okay(presentation.insert_light(light));
    send(session, application::SetPresentationCatalogAction{snap.project_generation, snap.project_revision, std::move(presentation)});
}
void save_ppm(const std::filesystem::path& path, const production::AuthoringViewportImage& frame) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
    for (std::size_t i = 0; i < frame.display_rgba8.size(); i += 4)
        out.write(reinterpret_cast<const char*>(frame.display_rgba8.data()+i), 3);
}
} // namespace

int main(int argc, char** argv) try {
    const auto root = std::filesystem::path(argc > 1 ? argv[1] : ".") /
        ("texturing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directories(root), "reserve new evidence directory");
    const auto project_path = root / "textured-first-block.carto";
    application::ApplicationSession session;
    ui::UiController ui(session);
    send(session, application::SaveProjectAction{project_path});
    take(ui.import_mesh(std::filesystem::path(CARTO_SOURCE_ROOT) / "examples/first-block/first-block.obj", ui::MeshImportFormat::obj));
    auto snapshot = session.snapshot();
    const auto object = snapshot.objects.front().object.id;
    const auto mesh_id = *snapshot.objects.front().object.mesh_asset;
    check(!snapshot.source_assets.empty(), "real mesh source and provenance captured");
    const auto before_uv = source(session).serialize();
    check(!application::testing::dispatch(session, application::GenerateUniqueLightmapUvAction{
        snapshot.project_revision, object, 0, 64, 2, 1}), "invalid UV identity rejected");
    check(source(session).serialize() == before_uv, "invalid UV action leaves project intact");
    send(session, application::GenerateUniqueLightmapUvAction{snapshot.project_revision, object, 1, 64, 2, 1});
    snapshot = session.snapshot();
    render::StandardMaterial definition;
    definition.metallic = .6; definition.roughness = .8;
    definition.emissive_factor = {.2, .2, .2}; definition.emissive_strength = 1;
    send(session, application::CreateMaterialAction{snapshot.project_revision, "Textured first block", definition});
    snapshot = session.snapshot();
    const auto created = std::find_if(snapshot.materials.begin(), snapshot.materials.end(),
        [](const auto& item) { return item.name == "Textured first block"; });
    check(created != snapshot.materials.end(), "newly created material is present");
    const auto material = created->id;
    const auto current_material = [&]() { return source(session).material_catalog().materials().at(material); };
    send(session, application::AssignMaterialToMeshAction{snapshot.project_generation, snapshot.project_revision,
        object, material, current_material().revision, {}, {}});
    configure_light(session);
    const auto base_path = root / u8"painted-checker-é.tga", rough_path = root / "roughness.tga", metal_path = root / "metallic.tga";
    write(base_path, image(40, true)); write(rough_path, image(180)); write(metal_path, image(96));
    take(ui.import_material_texture(base_path)); take(ui.import_material_texture(rough_path)); take(ui.import_material_texture(metal_path));
    snapshot = session.snapshot();
    check(snapshot.texture_sources[0].name == "painted-checker-é.tga", "UTF-8 source filename retained");
    definition = current_material().definition;
    definition.base_color.asset = snapshot.texture_sources[0].id;
    definition.emissive = definition.base_color;
    definition.base_color.transform.offset = {.125, .25};
    definition.emissive.transform = definition.base_color.transform;
    const application::TexturingCommand command = application::UpdateMaterialAction{snapshot.project_revision,
        material, current_material().revision, definition};
    const auto before_review = source(session).serialize();
    auto review = take(ui.begin_texturing_review(command));
    check(source(session).serialize() == before_review && review.after() != review.before(), "proposal is read-only and inspectable");
    check(!application::HumanApplicationAccess::accept_texturing(session, review, "  "), "proposal requires justification");
    application::ApplicationSession other_session;
    check(!application::HumanApplicationAccess::accept_texturing(other_session, review, "cross-session attempt"), "proposal cannot cross sessions/projects");
    take(ui.commit_texturing_review(review, "Approve checker map with explicit transform"));
    check(!application::HumanApplicationAccess::accept_texturing(session, review, "replay"), "consumed proposal cannot replay");
    snapshot = session.snapshot();
    const application::PackMaterialMapsAction pack{snapshot.project_generation, snapshot.project_revision,
        material, current_material().revision, snapshot.texture_sources[1].id, snapshot.texture_sources[2].id};
    send(session, pack);
    auto doc = source(session);
    const auto packed_id = doc.material_catalog().materials().at(material).definition.metallic_roughness.asset;
    assets::BlobStore blobs(root / ".carto-assets" / "source-blobs");
    const auto& packed = doc.material_catalog().textures().at(packed_id);
    const auto decoded = take(assets::decode_texture_rgba8(take(blobs.read(packed.content_digest))));
    check(decoded.pixels[1] == 180 && decoded.pixels[2] == 96 && decoded.pixels[0] == 255,
        "packing uses exact linear red channels in G/B without sRGB conversion");
    check(take(packed.lineage_digests()).size() == 2, "packing retains both source digests");
    send(session, application::UndoAction{});
    check(!current_material().definition.metallic_roughness, "packing undo is atomic");
    send(session, application::RedoAction{});
    const auto packed_path = blobs.path_for(packed.content_digest);
    std::filesystem::rename(packed_path, packed_path.string() + ".withheld");
    snapshot = session.snapshot();
    send(session, application::PackMaterialMapsAction{snapshot.project_generation, snapshot.project_revision,
        material, current_material().revision, snapshot.texture_sources[1].id, snapshot.texture_sources[2].id});
    check(take(blobs.read(packed.content_digest)).size() > 18U &&
        current_material().definition.metallic_roughness.asset == packed_id,
        "explicit regeneration restores missing pixels while retaining source identity and lineage");
    const auto rendered = render_frame(session);
    check(std::count_if(rendered.objects.begin(), rendered.objects.end(), [](auto id){return bool(id);}) > 1000,
        "real mesh has inspectable textured pixels");
    save_ppm(root / "before-bake.ppm", rendered);
    send(session, application::SaveProjectAction{});
    const auto bake_document = source(session);
    const auto& mesh = bake_document.meshes().at(mesh_id);
    const auto bake = take(desktop::bake_ambient_occlusion_tiles(project_path, mesh_id, 1U, mesh.revision(),
        take(production::bake_mesh_target_digest(mesh, 1U)), 32, 32, 8, 1, 42, root / "ao-bake"));
    check(source(session).serialize() == bake_document.serialize(), "background baking cannot mutate project state");
    const auto manifest = root / "ao-bake/receipts.carto-production-receipts";
    const auto ledger = take(production::ProductionReceiptLedger::deserialize(read(manifest)));
    assets::BlobStore baked_blobs(root / "ao-bake/blobs");
    snapshot = session.snapshot();
    application::AdoptBakedMaterialMapAction adopt{snapshot.project_generation, snapshot.project_revision, object,
        material, current_material().revision, ledger.records().front(),
        take(baked_blobs.read(ledger.records().front().output_digest)), "Adopt verified real-mesh AO"};
    const auto unchanged = source(session).serialize();
    const auto history = session.snapshot().undo_count;
    const auto journal_path = std::filesystem::path(project_path.string() + ".journal");
    const auto journal_before = read(journal_path);
    const auto refuse = [&](const application::AdoptBakedMaterialMapAction& bad) {
        check(!application::testing::dispatch(session, bad), "invalid bake adoption refused");
        check(source(session).serialize() == unchanged && session.snapshot().undo_count == history &&
            read(journal_path) == journal_before, "refusal preserves project, history and journal bytes");
    };
    auto bad = adopt; bad.justification.clear(); refuse(bad);
    bad = adopt; ++bad.expected_project_generation; refuse(bad);
    bad = adopt; bad.expected_material_revision = core::Revision{999}; refuse(bad);
    bad = adopt; bad.artifact_bytes.back() ^= 1U; refuse(bad);
    bad = adopt; bad.receipt.status = production::ReceiptStatus::stale;
    bad.receipt.output_digest = {}; bad.receipt.output_bytes = 0;
    okay(bad.receipt.refresh_digest()); refuse(bad);
    // A digest-valid foreign bake remains foreign, even with identical geometry.
    auto foreign = take(production::AmbientOcclusionMapArtifact::deserialize(adopt.artifact_bytes));
    foreign.recipe.binding.source.identity = "foreign-project/mesh/1";
    foreign.recipe.binding.target.identity = "foreign-project/mesh/1";
    okay(foreign.refresh_digests());
    bad = adopt; bad.artifact_bytes = take(foreign.serialize());
    bad.receipt.key = take(production::bake_ambient_occlusion_cook_key(foreign.recipe));
    bad.receipt.output_digest = assets::sha256(bad.artifact_bytes); bad.receipt.output_bytes = bad.artifact_bytes.size();
    okay(bad.receipt.refresh_digest()); refuse(bad);
    auto wrong_tile = take(production::AmbientOcclusionMapArtifact::deserialize(adopt.artifact_bytes));
    wrong_tile.recipe.binding.tile_u = 1;
    okay(wrong_tile.refresh_digests());
    bad = adopt; bad.artifact_bytes = take(wrong_tile.serialize());
    bad.receipt.key = take(production::bake_ambient_occlusion_cook_key(wrong_tile.recipe));
    bad.receipt.output_digest = assets::sha256(bad.artifact_bytes); bad.receipt.output_bytes = bad.artifact_bytes.size();
    okay(bad.receipt.refresh_digest()); refuse(bad);
    auto invalid_uv = take(production::AmbientOcclusionMapArtifact::deserialize(adopt.artifact_bytes));
    invalid_uv.recipe.binding.target_uv_set = 999;
    okay(invalid_uv.refresh_digests());
    bad = adopt; bad.artifact_bytes = take(invalid_uv.serialize());
    bad.receipt.key = take(production::bake_ambient_occlusion_cook_key(invalid_uv.recipe));
    bad.receipt.output_digest = assets::sha256(bad.artifact_bytes); bad.receipt.output_bytes = bad.artifact_bytes.size();
    okay(bad.receipt.refresh_digest()); refuse(bad);
    take(ui.adopt_baked_material_map(manifest, object, material, "Approve current bake with exact source pins"));
    check(read(journal_path) != journal_before, "accepted bake emits durable application mutation event");
    const auto adopted_document = source(session);
    const auto ao_id = adopted_document.material_catalog().materials().at(material).definition.occlusion.asset;
    const auto ao_record = adopted_document.material_catalog().textures().at(ao_id);
    for (const auto& digest : take(ao_record.lineage_digests())) take(blobs.read(digest));
    check(take(assets::decode_texture_rgba8(take(blobs.read(ao_record.content_digest)))).pixels ==
        take(production::AmbientOcclusionMapArtifact::deserialize(adopt.artifact_bytes)).rgba8,
        "bake adoption preserves UV-row orientation and exact pixel values");
    send(session, application::UndoAction{});
    check(!current_material().definition.occlusion, "bake undo restores previous binding and removes new record");
    send(session, application::RedoAction{});
    send(session, application::SaveProjectAction{});
    const auto final_frame = render_frame(session); save_ppm(root / "after-bake.ppm", final_frame);
    application::ApplicationSession reopened;
    send(reopened, application::OpenProjectAction{project_path});
    check(render_frame(reopened).display_rgba8 == final_frame.display_rgba8, "save/reopen reproduces exact CPU viewport pixels");
    const auto migrated = take(project::ProjectPackage::migrate_flat_project(project_path, root / "package"));
    assets::BlobStore migrated_blobs(migrated.root() / ".carto-assets/source-blobs");
    for (const auto& digest : take(ao_record.lineage_digests())) take(migrated_blobs.read(digest));
    check(take(migrated.export_project_document()) == source(session).serialize(), "package migration retains material truth and bake lineage");
    // glTF/GLB uses the existing exporter and importer, including image embedding.
    const auto export_document = source(session);
    take(io::export_glb(export_document, root / "textured-first-block.glb", io::GltfExportOptions{project_path}));
    take(io::export_gltf(export_document, root / "textured-first-block.gltf", io::GltfExportOptions{project_path}));
    application::ApplicationSession imported;
    ui::UiController imported_ui(imported);
    send(imported, application::SaveProjectAction{root / "reimported.carto"});
    auto imported_snapshot = imported.snapshot();
    take(imported_ui.import_static_gltf_scene(root / "textured-first-block.glb", imported_snapshot.project_generation,
        imported_snapshot.project_revision, imported_snapshot.source_namespace, true, "Round-trip our authored example"));
    // glTF's existing profile reports lighting loss. Configure the same explicit
    // consumer light before judging material appearance; do not infer parity.
    configure_light(imported);
    const auto roundtrip = render_frame(imported);
    save_ppm(root / "roundtrip.ppm", roundtrip);
    check(roundtrip.display_rgba8 == final_frame.display_rgba8, "export/reimport retains textured viewport appearance");
    // Missing pixels are surfaced, never replaced with a fallback material.
    const auto color = source(reopened).material_catalog().textures().at(definition.base_color.asset);
    const auto stored_path = blobs.path_for(color.content_digest);
    std::filesystem::rename(stored_path, stored_path.string() + ".withheld");
    check(!reopened.prepare_authoring_viewport_request(view(), reopened.snapshot().project_revision), "missing texture refuses viewport preparation");
    std::filesystem::rename(stored_path.string() + ".withheld", stored_path);
    // Save As must carry exact source/lineage bytes, rather than make a project
    // file whose relative texture paths point at an empty asset directory.
    std::filesystem::create_directory(root / "saved-as");
    send(reopened, application::SaveProjectAction{root / "saved-as/copied.carto"});
    application::ApplicationSession saved_as;
    send(saved_as, application::OpenProjectAction{root / "saved-as/copied.carto"});
    check(render_frame(saved_as).display_rgba8 == final_frame.display_rgba8, "Save As carries the texture appearance");
    assets::BlobStore copied_blobs(root / "saved-as/.carto-assets/source-blobs");
    for (const auto& digest : take(ao_record.lineage_digests())) take(copied_blobs.read(digest));
    // The existing prepared-scene publication path carries these material maps
    // to destination adapters, including VANTA, without claiming runtime admission.
    production::RenderMeshCompileOptions options;
    options.uv_set = 1; options.generate_tangents = true;
    options.material_slot_layer = "condition.material_region";
    const auto render_mesh = take(production::compile_render_mesh_artifact(export_document.source_namespace(), export_document, mesh_id, options));
    check(!production::compile_material_artifact("texturing/material", export_document.material_catalog().materials().at(material)),
        "native packed lookdev explicitly rejects custom mappings instead of discarding them");
    // The neutral prepared path can preserve source materials without claiming
    // a packed lookdev realization that this profile cannot represent.
    const production::PreparedAssetInput asset{mesh_id, &render_mesh, nullptr};
    const auto profile = assets::sha256(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>("texturing"), 9});
    const auto prepared = take(production::compose_prepared_scene(export_document.source_namespace(), export_document,
        "texturing.static.v1", profile, std::span{&asset, 1U}));
    std::vector<production::PreparedProductPayload> payloads;
    const auto scope = take(production::resolve_preparation_scope(export_document,
        {production::PreparationScopeKind::scene, export_document.revision(), {}, {}}));
    const auto prepared_catalog = take(production::compose_prepared_material_catalog(export_document, scope));
    const auto bytes_of = [](const std::string& text) { return std::vector<std::uint8_t>(text.begin(), text.end()); };
    for (const auto& product : prepared.products) {
        if (product.kind == production::PreparedProductKind::render_mesh)
            payloads.push_back({product.identity, take(render_mesh.serialize())});
        else if (product.kind == production::PreparedProductKind::material_catalog)
            payloads.push_back({product.identity, bytes_of(prepared_catalog.serialize())});
        else if (product.kind == production::PreparedProductKind::texture_source)
            payloads.push_back({product.identity, take(blobs.read(product.content_digest))});
        else check(false, "no unhandled prepared product in real-mesh test");
    }
    take(production::publish_prepared_scene(root / "prepared", prepared, payloads, std::nullopt));
    check(take(production::read_selected_prepared_scene(root / "prepared")).has_value(), "published prepared assets verify on re-read");
    // Normal baking uses the same catalog path and carries its Y convention.
    const auto normal_bake = take(desktop::bake_tangent_normal_tiles(project_path, mesh_id, 1, mesh.revision(),
        take(production::bake_mesh_target_digest(mesh, 1)), 32, 32, root / "normal-bake"));
    take(ui.adopt_baked_material_map(normal_bake.output_directory / "receipts.carto-production-receipts", object,
        material, "Approve tangent normal from the same real mesh"));
    check(current_material().definition.normal.normal_convention == render::NormalConvention::opengl_y_plus,
        "baked normal convention is carried rather than guessed");
    const auto normal_frame = render_frame(session);
    save_ppm(root / "normal-bake.ppm", normal_frame);
    send(session, application::SaveProjectAction{});
    take(io::export_glb(source(session), root / "with-baked-normal.glb", io::GltfExportOptions{project_path}));
    application::ApplicationSession normal_imported;
    ui::UiController normal_ui(normal_imported);
    send(normal_imported, application::SaveProjectAction{root / "normal-reimported.carto"});
    const auto normal_snapshot = normal_imported.snapshot();
    take(normal_ui.import_static_gltf_scene(root / "with-baked-normal.glb", normal_snapshot.project_generation,
        normal_snapshot.project_revision, normal_snapshot.source_namespace, true, "Round-trip the adopted tangent-normal bake"));
    configure_light(normal_imported);
    const auto normal_roundtrip = render_frame(normal_imported);
    check(normal_roundtrip.display_rgba8.size() == normal_frame.display_rgba8.size(), "normal round-trip extent");
    for (std::size_t i = 0; i < normal_frame.display_rgba8.size(); ++i)
        check(std::abs(int(normal_frame.display_rgba8[i]) - int(normal_roundtrip.display_rgba8[i])) <= 1,
            "baked-normal export/reimport retains appearance within float32/display quantization");
    save_ppm(root / "normal-roundtrip.ppm", normal_roundtrip);
    // Updating an already generated source cannot mislabel replacement pixels
    // with the old bake's receipt.
    const auto replacement = root / "unregistered-replacement.tga";
    write(replacement, image(200));
    const auto refused_reimport = ui.reimport_material_texture(ao_id, replacement);
    check(!refused_reimport && refused_reimport.error().code == core::ErrorCode::unsupported,
        "generated map reimport policy blocks fresh replacement pixels with false bake lineage");
    const auto snap_for_review = session.snapshot();
    auto canceled = take(session.review_texturing(application::UpdateMaterialAction{snap_for_review.project_revision,
        material, current_material().revision, current_material().definition}));
    canceled.cancel();
    check(!application::HumanApplicationAccess::accept_texturing(session, canceled, "cannot apply canceled review"),
        "invalid proposal state transition is blocked");
    std::cout << "PASS real mesh -> generated UVs -> import/pack -> reviewed material -> AO adoption -> viewport -> undo/redo -> save/reopen -> package -> glTF/GLB reimport\nevidence=" << root << '\n';
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
