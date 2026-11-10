#include <carto/application/application.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/journal/journal.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/project/package.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace carto;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void u32(std::vector<std::uint8_t>& out, std::uint32_t n) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(n >> shift));
}
std::vector<std::uint8_t> fixture(std::string_view extra = {}, std::string_view node_extras = {},
                                  std::string_view primitive_extras = {}, bool missing_materials = false) {
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"parent","children":[1,2]},{"name":"first","mesh":0},{"name":"second","mesh":1}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]},{"primitives":[{"attributes":{"POSITION":0},"material":1}]}],"materials":[{"name":"red","doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}},{"name":"green","doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[0,1,0,1]}}],"buffers":[{"byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}]})";
    if (!node_extras.empty()) {
        const std::string marker = R"({"name":"first","mesh":0})";
        const auto position = json.find(marker);
        require(position != std::string::npos, "node fixture marker found");
        json.replace(position, marker.size(),
            R"({"name":"first","mesh":0,"extras":)" + std::string(node_extras) + "}");
    }
    if (!primitive_extras.empty()) {
        const std::string marker = R"({"attributes":{"POSITION":0},"material":0})";
        const auto position = json.find(marker);
        require(position != std::string::npos, "primitive fixture marker found");
        json.replace(position, marker.size(),
            R"({"attributes":{"POSITION":0},"material":0,"extras":)" +
                std::string(primitive_extras) + "}");
    }
    if (missing_materials) {
        for (const auto* marker : {",\"material\":0", ",\"material\":1"}) {
            const auto position = json.find(marker);
            require(position != std::string::npos, "material fixture marker found");
            json.erase(position, std::string_view(marker).size());
        }
        const auto materials = json.find(",\"materials\":[");
        const auto buffers = json.find(",\"buffers\":", materials);
        require(materials != std::string::npos && buffers != std::string::npos,
                "material array fixture marker found");
        json.erase(materials, buffers - materials);
    }
    json.insert(json.size() - 1U, extra);
    while (json.size() % 4U) json.push_back(' ');
    std::vector<std::uint8_t> bin;
    for (float f : {0.F,0.F,0.F, 1.F,0.F,0.F, 0.F,1.F,0.F}) u32(bin, std::bit_cast<std::uint32_t>(f));
    std::vector<std::uint8_t> result;
    u32(result, 0x46546c67U); u32(result, 2U);
    u32(result, static_cast<std::uint32_t>(12U + 8U + json.size() + 8U + bin.size()));
    u32(result, static_cast<std::uint32_t>(json.size())); u32(result, 0x4e4f534aU);
    result.insert(result.end(), json.begin(), json.end());
    u32(result, static_cast<std::uint32_t>(bin.size())); u32(result, 0x004e4942U);
    result.insert(result.end(), bin.begin(), bin.end());
    return result;
}

void registry_compatibility() {
    assets::SourceImportRecord old;
    old.identity = "old"; old.relative_source_path = "source/old.obj";
    old.source_generation = core::Revision{1}; old.kind = assets::SourceAssetKind::mesh;
    old.source_digest = assets::sha256(fixture()); old.artifact_digest = old.source_digest;
    old.importer_identity = "carto.import.obj"; old.artifact_identity = "mesh.asset.1";
    auto decoded = assets::SourceImportRecord::deserialize(old.serialize());
    require(decoded && decoded.value().serialize() == old.serialize(), "old record round trip");
    auto scene = old; scene.identity = "scene"; scene.kind = assets::SourceAssetKind::other;
    scene.importer_identity = "carto.import.gltf-scene"; scene.artifact_identity = "gltf.scene.scene";
    scene.document_products = {{assets::SourceDocumentProductKind::mesh, 1, 9},
                               {assets::SourceDocumentProductKind::node, 1, 12},
                               {assets::SourceDocumentProductKind::material, 1, 7}};
    scene.artifact_digest = assets::source_document_product_digest(scene.source_digest, scene.document_products);
    auto current = assets::SourceImportRecord::deserialize(scene.serialize());
    require(current && current.value().document_products == scene.document_products, "v4 mapping round trip");
    scene.document_products.push_back({assets::SourceDocumentProductKind::material, 1, 13});
    require(!scene.validate(), "duplicate source product refused");
    auto malformed = current.value().serialize();
    malformed += "unbound";
    require(!assets::SourceImportRecord::deserialize(malformed), "trailing data refused");
    auto tampered = current.value();
    tampered.document_products[0].product_id++;
    require(!tampered.validate(), "mapping tamper without digest update refused");
}

void import_and_history(const std::filesystem::path& root) {
    using namespace application;
    const auto path = root / "scene.carto";
    ApplicationSession session;
    auto dispatch = [&session](const ApplicationAction& action) { return HumanApplicationAccess::dispatch(session, action); };
    require(bool(dispatch(NewProjectAction{"Import regression"})), "new project");
    require(bool(dispatch(SaveProjectAction{path})), "save project");
    require(bool(dispatch(CreateBoxAction{"existing", {1, 1, 1}})), "existing project content");
    assets::BlobStore store(root / ".carto-assets" / "source-blobs");
    auto blob = store.put(fixture(), "model/gltf-binary");
    if (!blob) std::cerr << "source blob detail: " << blob.error().message << '\n';
    require(bool(blob), "stage source blob");
    const auto before = session.snapshot();
    ImportGltfSceneAction action{before.project_generation, before.project_revision, before.source_namespace, "scene-import",
                                 core::Revision{}, blob.value().digest, true, "rights confirmed"};
    auto missing = action; missing.rights_confirmed = false;
    require(!dispatch(missing), "missing rights rejected");
    auto stale = action; stale.expected_project_generation++;
    require(!dispatch(stale), "stale generation rejected");
    auto foreign = action; foreign.expected_source_namespace = "different-project";
    require(!dispatch(foreign), "cross-project namespace rejected");
    auto bad_bytes = fixture(); bad_bytes[0] = 0U;
    auto bad_blob = store.put(bad_bytes, "model/gltf-binary");
    require(bool(bad_blob), "stage malformed fixture");
    auto bad = action; bad.source_digest = bad_blob.value().digest;
    require(!dispatch(bad), "malformed glTF rejected");
    auto skinned_blob = store.put(fixture(",\"skins\":[{}]"), "model/gltf-binary");
    require(bool(skinned_blob), "stage unsupported fixture");
    auto skinned = action; skinned.source_digest = skinned_blob.value().digest;
    require(!dispatch(skinned), "unsupported skin rejected");
    require(session.snapshot().project_revision == before.project_revision &&
            session.snapshot().undo_count == before.undo_count, "refusal preserves history");
    const auto journal_path = std::filesystem::path(path.string() + ".journal");
    const auto parked_journal = root / "parked-journal";
    std::filesystem::rename(journal_path, parked_journal);
    std::filesystem::create_directory(journal_path);
    require(!dispatch(action), "failed journal append refuses import");
    require(session.snapshot().project_revision == before.project_revision &&
            session.snapshot().undo_count == before.undo_count &&
            session.snapshot().source_assets.empty(), "failed append rolls back import and history");
    std::filesystem::remove(journal_path);
    std::filesystem::rename(parked_journal, journal_path);
    auto imported = dispatch(action);
    require(bool(imported), "supported glTF imported");
    const auto after = session.snapshot();
    require(after.objects.size() == before.objects.size() + 3U && after.materials.size() == 2U &&
            after.source_assets.size() == 1U && after.source_assets.front().document_products.size() == 7U,
            "scene and product mapping admitted");
    require(after.materials[0].definition.base_color_factor.x == 1.0 &&
            after.materials[0].definition.base_color_factor.y == 0.0 &&
            after.materials[1].definition.base_color_factor.y == 1.0,
            "authored material colors preserved");
    require(after.source_assets.front().source_generation == core::Revision{1}, "initial source generation");
    require(bool(dispatch(UndoAction{})), "undo import");
    const auto undone = session.snapshot();
    require(undone.objects.size() == before.objects.size() && undone.source_assets.empty() &&
            undone.source_asset_tombstone_count == 1U, "undo retains source tombstone");
    auto stale_lineage = action;
    stale_lineage.expected_project_revision = undone.project_revision;
    require(!dispatch(stale_lineage), "stale source generation refused");
    const auto source_path = store.path_for(blob.value().digest);
    const auto parked_source = root / "parked-source";
    std::filesystem::rename(source_path, parked_source);
    require(!dispatch(RedoAction{}), "redo refuses missing source blob");
    require(session.snapshot().project_revision == undone.project_revision &&
            session.snapshot().source_assets.empty() && session.snapshot().redo_count == undone.redo_count,
            "missing-source redo preserves state and history");
    std::filesystem::rename(parked_source, source_path);
    require(bool(dispatch(RedoAction{})), "redo import");
    const auto redone = session.snapshot();
    require(redone.source_assets.front().source_generation == core::Revision{2} &&
            redone.source_assets.front().document_products == after.source_assets.front().document_products,
            "redo advances generation and preserves mapping");
    require(bool(dispatch(SaveProjectAction{})), "save imported scene");
    ApplicationSession reopened;
    require(bool(HumanApplicationAccess::dispatch(reopened, OpenProjectAction{path})), "reopen project");
    require(reopened.snapshot().source_assets.front().document_products == redone.source_assets.front().document_products,
            "reopen preserves mapping");
    journal::Journal journal(journal_path);
    require(bool(journal.verify()), "journal chain verifies");
    const auto events = journal.read_all();
    require(events && !events.value().empty() &&
            events.value().back().event_type == "cartographer.application_action",
            "successful import history remains journaled");
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "test artifact is readable");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write_source(const std::filesystem::path& path, const std::vector<std::uint8_t>& source) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(source.data()),
                 static_cast<std::streamsize>(source.size()));
    stream.close();
    require(bool(stream), "source fixture written");
}

std::vector<std::pair<std::string, std::string>> blob_inventory(
    const std::filesystem::path& root) {
    std::vector<std::pair<std::string, std::string>> result;
    const auto store = root / ".carto-assets" / "source-blobs";
    if (!std::filesystem::exists(store)) return result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(store)) {
        if (!entry.is_regular_file()) continue;
        const auto bytes = file_bytes(entry.path());
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        result.emplace_back(entry.path().lexically_relative(store).generic_string(),
            assets::sha256(std::span<const std::uint8_t>{data, bytes.size()}).hex());
    }
    std::sort(result.begin(), result.end());
    return result;
}

void ui_import_and_retained_generation(const std::filesystem::path& root) {
    using namespace application;
    std::filesystem::create_directories(root);
    const auto project_path = root / "ui.carto";
    const auto source_path = root / "selected.glb";
    const auto source = fixture({},
        R"({"vanta_object_id":42,"vanta_visible":false,"vanta_locked":true,"visible":false})",
        R"({"vanta_material_slot":17})");
    write_source(source_path, source);
    ApplicationSession session;
    ui::UiController controller(session);
    require(bool(controller.dispatch(NewProjectAction{"UI import"})), "UI project created");
    require(bool(controller.dispatch(SaveProjectAction{project_path})), "UI project saved");
    const auto before = controller.snapshot();
    const auto saved_before = file_bytes(project_path);
    const auto blobs_before = blob_inventory(root);
    const auto unchanged = [&] {
        const auto after = controller.snapshot();
        require(after.project_revision == before.project_revision &&
                after.undo_count == before.undo_count && after.redo_count == before.redo_count &&
                after.source_assets.empty() && after.operations.size() == before.operations.size() &&
                file_bytes(project_path) == saved_before && blob_inventory(root) == blobs_before,
                "UI refusal preserves authoring, history, ledger, saved bytes and source cache");
    };
    require(!controller.import_static_gltf_scene(source_path, before.project_generation,
        before.project_revision, before.source_namespace, false, "reviewed source"),
        "rights are required before staging");
    unchanged();
    for (const auto& reason : {std::string{}, std::string{" \t "}, std::string{"reason\ninjected"}}) {
        require(!controller.import_static_gltf_scene(source_path, before.project_generation,
            before.project_revision, before.source_namespace, true, reason),
            "blank or control justification refused before staging");
        unchanged();
    }
    require(!controller.import_static_gltf_scene(source_path, before.project_generation + 1U,
        before.project_revision, before.source_namespace, true, "reviewed source"),
        "stale project generation refused before staging");
    unchanged();
    require(!controller.import_static_gltf_scene(source_path, before.project_generation,
        before.project_revision.next(), before.source_namespace, true, "reviewed source"),
        "stale project revision refused before staging");
    unchanged();
    require(!controller.import_static_gltf_scene(source_path, before.project_generation,
        before.project_revision, "foreign-project", true, "reviewed source"),
        "foreign project namespace refused before staging");
    unchanged();
    require(!controller.submit_ai_proposal(ImportGltfSceneAction{}), "raw AI import is refused");
    unchanged();
    for (const auto& rejected : {std::vector<std::uint8_t>{'b', 'a', 'd'},
                                 fixture(R"(,"skins":[{}])"), fixture(R"(,"animations":[{}])"),
                                 fixture({}, R"({"visible":true,"vanta_visible":false})"),
                                 fixture({}, R"({"vanta_locked":"true"})")}) {
        const auto rejected_path = root / "unsupported.glb";
        write_source(rejected_path, rejected);
        require(!controller.import_static_gltf_scene(rejected_path, before.project_generation,
            before.project_revision, before.source_namespace, true, "reviewed source"),
            "malformed and unsupported sources refused before staging");
        unchanged();
    }
    const auto identity = "gltf.scene." + assets::sha256(source).hex();
    const auto zero = session.source_asset_generation(identity, before.source_namespace,
        before.project_generation, before.project_revision);
    require(zero && zero.value() == core::Revision{}, "new source has no retained generation");
    const auto admitted = controller.import_static_gltf_scene(source_path, before.project_generation,
        before.project_revision, before.source_namespace, true, "Append reviewed static source");
    require(admitted && admitted.value().document_changed &&
            admitted.value().source == OperationSource::human, "UI uses the human scene action");
    require(std::any_of(admitted.value().warnings.begin(), admitted.value().warnings.end(),
            [](const auto& warning) { return warning.message.find("VANTA object IDs") != std::string::npos; }),
            "source identity remapping is reported");
    require(std::any_of(admitted.value().warnings.begin(), admitted.value().warnings.end(),
            [](const auto& warning) { return warning.message.find("VANTA material-slot IDs") != std::string::npos; }),
            "unmapped source material slots are reported");
    const auto imported = controller.snapshot();
    const auto imported_node = std::find_if(imported.objects.begin(), imported.objects.end(),
        [](const auto& view) { return view.object.name == "first"; });
    require(imported_node != imported.objects.end() && !imported_node->object.visible &&
            imported_node->object.locked, "VANTA visibility and locking preserved");
    require(imported.source_assets.size() == 1U && imported.source_assets.front().identity == identity &&
            imported.source_assets.front().source_generation == core::Revision{1U} &&
            imported.operations.size() == before.operations.size() + 1U,
            "UI import retains exact source identity and one operation ledger entry");
    assets::BlobStore store(root / ".carto-assets" / "source-blobs");
    const auto cached = store.read(assets::sha256(source));
    require(cached && cached.value() == source, "UI stages exact original bytes");
    require(bool(controller.dispatch(UndoAction{})), "UI import undo");
    const auto undone = controller.snapshot();
    const auto retained = session.source_asset_generation(identity, undone.source_namespace,
        undone.project_generation, undone.project_revision);
    require(retained && retained.value() == core::Revision{1U} && undone.source_assets.empty(),
            "read-only generation query includes the undone source tombstone");
    require(!session.source_asset_generation(identity, "foreign-project",
        undone.project_generation, undone.project_revision), "generation query is project-bound");
    require(bool(controller.import_static_gltf_scene(source_path, undone.project_generation,
        undone.project_revision, undone.source_namespace, true, "Reimport reviewed static source")),
        "UI reimport uses retained generation");
    const auto reimported = controller.snapshot();
    require(reimported.source_assets.size() == 1U &&
            reimported.source_assets.front().source_generation == core::Revision{2U} &&
            reimported.redo_count == 0U, "reimport advances lineage and replaces the redo branch");
    require(bool(controller.dispatch(SaveProjectAction{})), "save UI import");
    ApplicationSession reopened;
    require(bool(HumanApplicationAccess::dispatch(reopened, OpenProjectAction{project_path})),
            "cold reopen UI-imported project");
    const auto reopened_state = reopened.snapshot();
    const auto reopened_node = std::find_if(reopened_state.objects.begin(), reopened_state.objects.end(),
        [](const auto& view) { return view.object.name == "first"; });
    require(reopened_node != reopened_state.objects.end() && !reopened_node->object.visible &&
            reopened_node->object.locked, "VANTA node flags survive cold reopen");
    require(reopened_state.source_assets.size() == 1U &&
            reopened_state.source_assets.front().document_products ==
                reimported.source_assets.front().document_products &&
            reopened_state.source_assets.front().source_generation == core::Revision{2U},
            "UI source mapping and generation survive cold reopen");
    journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    require(bool(journal.verify()), "UI import history has a valid journal chain");
}

void ui_default_material_projection(const std::filesystem::path& root) {
    using namespace application;
    std::filesystem::create_directories(root);
    const auto project_path = root / "projection.carto";
    const auto source_path = root / "default.glb";
    const auto source = fixture({}, {}, {}, true);
    write_source(source_path, source);
    ApplicationSession session;
    ui::UiController controller(session);
    require(bool(controller.dispatch(NewProjectAction{"Projection"})), "projection project created");
    require(bool(controller.dispatch(SaveProjectAction{project_path})), "projection project saved");
    const auto before = controller.snapshot();
    require(!controller.import_static_gltf_scene(source_path, before.project_generation,
        before.project_revision, before.source_namespace, true, "reviewed"),
        "implicit material requires explicit projection");
    require(controller.snapshot().project_revision == before.project_revision &&
            controller.snapshot().source_assets.empty() && blob_inventory(root).empty(),
            "projection refusal leaves project and cache unchanged");
    const auto admitted = controller.import_static_gltf_scene(source_path, before.project_generation,
        before.project_revision, before.source_namespace, true, "reviewed", true);
    require(bool(admitted), "explicit default-material projection admitted");
    require(std::any_of(admitted.value().warnings.begin(), admitted.value().warnings.end(),
            [](const auto& warning) { return warning.message.find("back-face appearance") != std::string::npos; }),
            "projection loss is reported");
    require(std::any_of(admitted.value().warnings.begin(), admitted.value().warnings.end(),
            [](const auto& warning) { return warning.message.find("updated Cartographer reader") != std::string::npos; }),
            "project reader compatibility is reported");
    const auto after = controller.snapshot();
    require(after.source_assets.size() == 1U && after.source_assets.front().importer_version == 2U,
            "projection records the versioned source contract");
    require(after.materials.size() == 1U && after.materials.back().definition.metallic == 1.0 &&
            after.materials.back().definition.roughness == 1.0,
            "implicit glTF material uses its default PBR factors");
    require(std::none_of(after.source_assets.front().document_products.begin(),
                         after.source_assets.front().document_products.end(),
        [](const auto& product) {
            return product.kind == assets::SourceDocumentProductKind::material;
        }), "derived material is not presented as an authored source material");
    assets::BlobStore store(root / ".carto-assets" / "source-blobs");
    const auto retained = store.read(assets::sha256(source));
    require(retained && retained.value() == source, "projection preserves exact source bytes");
    require(bool(controller.dispatch(UndoAction{})), "projection undo");
    require(bool(controller.dispatch(RedoAction{})), "projection redo");
    const auto redone = controller.snapshot();
    require(bool(controller.dispatch(SaveProjectAction{})), "save projection");
    ApplicationSession reopened;
    require(bool(HumanApplicationAccess::dispatch(reopened, OpenProjectAction{project_path})),
            "reopen projected scene");
    const auto restored = reopened.snapshot();
    require(restored.materials.size() == 1U && restored.source_assets.size() == 1U &&
            restored.source_assets.front().importer_version == 2U &&
            restored.source_assets.front().source_digest == redone.source_assets.front().source_digest &&
            restored.source_assets.front().artifact_digest == redone.source_assets.front().artifact_digest &&
            restored.source_assets.front().domain_digests == redone.source_assets.front().domain_digests &&
            restored.source_assets.front().document_products == redone.source_assets.front().document_products,
            "projected source lineage survives undo, redo, save, and reopen");
    require(restored.materials.front().id == redone.materials.front().id &&
            restored.materials.front().definition.metallic == 1.0 &&
            restored.materials.front().definition.roughness == 1.0 &&
            restored.material_assignments.size() == redone.material_assignments.size() &&
            !restored.material_assignments.empty(), "projected material and assignments survive reopen");
    for (std::size_t i = 0; i < restored.material_assignments.size(); ++i) {
        require(restored.material_assignments[i].key == redone.material_assignments[i].key &&
                restored.material_assignments[i].material == redone.material_assignments[i].material &&
                restored.material_assignments[i].variant == redone.material_assignments[i].variant,
                "each projected material assignment survives reopen");
    }
    const auto original_document = project::ProjectDocument::load(project_path);
    require(bool(original_document), "projected document loads for migration");
    const auto package = project::ProjectPackage::migrate_flat_project(project_path, root / "projected-package");
    require(bool(package), "projected document migrates into package");
    const auto exported = package.value().export_project_document();
    require(bool(exported), "migrated projected document exports");
    const auto migrated_document = project::ProjectDocument::deserialize(exported.value());
    require(migrated_document &&
            migrated_document.value().source_asset_registry().serialize() ==
                original_document.value().source_asset_registry().serialize() &&
            migrated_document.value().material_catalog().serialize() ==
                original_document.value().material_catalog().serialize(),
            "package migration preserves lineage and material catalog bytes");
    assets::BlobStore migrated_store(package.value().root() / ".carto-assets" / "source-blobs");
    const auto migrated_source = migrated_store.read(restored.source_assets.front().source_digest);
    require(migrated_source && migrated_source.value() == source,
            "package migration preserves exact captured source bytes");
    const auto missing_root = root / "missing-source";
    require(std::filesystem::create_directory(missing_root), "missing-source fixture created");
    std::filesystem::copy_file(project_path, missing_root / "projection.carto");
    const auto missing = project::ProjectPackage::migrate_flat_project(
        missing_root / "projection.carto", missing_root / "rejected-package");
    require(!missing && !std::filesystem::exists(missing_root / "rejected-package"),
            "migration refuses to publish a package without its captured source");
}

void ui_external_buffer_capture(const std::filesystem::path& root) {
    using namespace application;
    std::filesystem::create_directories(root);
    const auto project_path = root / "external.carto";
    const auto source_path = root / "scene.gltf";
    const auto buffer_path = root / "scene.bin";
    const auto glb = fixture({}, R"({"vanta_object_id":42,"vanta_visible":false,"vanta_locked":true})", {}, true);
    const auto json_size = static_cast<std::uint32_t>(glb[12]) |
        (static_cast<std::uint32_t>(glb[13]) << 8U) |
        (static_cast<std::uint32_t>(glb[14]) << 16U) |
        (static_cast<std::uint32_t>(glb[15]) << 24U);
    std::string json(glb.begin() + 20, glb.begin() + 20 + json_size);
    const std::string marker = R"({"byteLength":36})";
    const auto position = json.find(marker);
    require(position != std::string::npos, "external buffer fixture marker found");
    const auto document_with_uri = [&](std::string_view uri) {
        auto result = json;
        result.replace(position, marker.size(),
            R"({"byteLength":36,"uri":")" + std::string(uri) + R"("})");
        return std::vector<std::uint8_t>(result.begin(), result.end());
    };
    const auto document = document_with_uri("scene.bin");
    const std::vector<std::uint8_t> buffer(glb.end() - 36, glb.end());
    write_source(source_path, document_with_uri("../outside.bin"));
    write_source(buffer_path, buffer);
    ApplicationSession session;
    ui::UiController controller(session);
    require(bool(controller.dispatch(NewProjectAction{"External buffer"})), "external project created");
    require(bool(controller.dispatch(SaveProjectAction{project_path})), "external project saved");
    const auto before = controller.snapshot();
    const auto attempt = [&] {
        return controller.import_static_gltf_scene(source_path, before.project_generation,
            before.project_revision, before.source_namespace, true, "reviewed", true);
    };
    require(!attempt(), "traversal buffer URI rejected");
    write_source(source_path, document_with_uri("missing.bin"));
    require(!attempt(), "missing buffer rejected");
    write_source(source_path, document_with_uri("http:%2f%2fexample.bin"));
    require(!attempt(), "URL and encoded path syntax rejected");
    std::error_code link_error;
    std::filesystem::create_symlink(buffer_path, root / "linked.bin", link_error);
    if (!link_error) {
        write_source(source_path, document_with_uri("linked.bin"));
        require(!attempt(), "linked dependency rejected");
    } else {
        std::cout << "SKIP: dependency symlink fixture unavailable: " << link_error.message() << '\n';
    }
    write_source(source_path, document);
    auto short_buffer = buffer;
    short_buffer.pop_back();
    write_source(buffer_path, short_buffer);
    require(!attempt(), "incorrect buffer length rejected");
    require(controller.snapshot().project_revision == before.project_revision &&
            controller.snapshot().source_assets.empty() && blob_inventory(root).empty(),
            "dependency refusals preserve project and source cache");
    write_source(buffer_path, buffer);
    const auto imported = attempt();
    require(bool(imported), "captured external buffer imports");
    require(std::any_of(imported.value().warnings.begin(), imported.value().warnings.end(),
        [](const auto& warning) { return warning.message.find("captured together") != std::string::npos; }),
        "capture is reported");
    const auto after = controller.snapshot();
    require(after.source_assets.size() == 1U, "source package has one lineage record");
    assets::BlobStore store(root / ".carto-assets" / "source-blobs");
    const auto retained = store.read(after.source_assets.front().source_digest);
    require(retained && std::search(retained.value().begin(), retained.value().end(), document.begin(), document.end()) != retained.value().end() &&
            std::search(retained.value().begin(), retained.value().end(), buffer.begin(), buffer.end()) != retained.value().end(),
            "source package retains exact document and buffer bytes");
    std::filesystem::remove(source_path);
    std::filesystem::remove(buffer_path);
    require(bool(controller.dispatch(UndoAction{})) && bool(controller.dispatch(RedoAction{})),
            "undo and redo use captured source after originals disappear");
    require(bool(controller.dispatch(SaveProjectAction{})), "save captured scene");
    ApplicationSession reopened;
    require(bool(HumanApplicationAccess::dispatch(reopened, OpenProjectAction{project_path})),
            "reopen captured scene without originals");
    require(reopened.snapshot().source_assets.front().source_digest == after.source_assets.front().source_digest,
            "source package identity survives reopen");
}

void vanta_roundtrip(const std::filesystem::path& source, const std::filesystem::path& root) {
    using namespace application;
    require(!std::filesystem::exists(root), "round-trip output directory must be new");
    require(std::filesystem::create_directories(root), "round-trip output directory created");
    const auto project_path = root / "roundtrip.carto";
    ApplicationSession session;
    ui::UiController controller(session);
    require(bool(controller.dispatch(NewProjectAction{"VANTA round trip"})), "round-trip project created");
    require(bool(controller.dispatch(SaveProjectAction{project_path})), "round-trip project saved");
    const auto before = controller.snapshot();
    const auto imported = controller.import_static_gltf_scene(source, before.project_generation,
        before.project_revision, before.source_namespace, true, "VANTA compatibility acceptance", true);
    if (!imported) std::cerr << "round-trip import: " << imported.error().message << '\n';
    require(bool(imported), "VANTA output imported");
    const auto imported_snapshot = controller.snapshot();
    require(!imported_snapshot.objects.empty() && !imported_snapshot.source_assets.empty(),
            "VANTA scene and source are present");
    require(bool(controller.dispatch(UndoAction{})) && bool(controller.dispatch(RedoAction{})),
            "round-trip scene undo and redo");
    require(bool(controller.dispatch(SaveProjectAction{})), "round-trip imported project saved");
    const auto project = project::ProjectDocument::load(project_path);
    require(bool(project), "round-trip project cold loaded");
    const auto exported = io::export_gltf(project.value(), root / "roundtrip.gltf");
    if (!exported) std::cerr << "round-trip export: " << exported.error().message << '\n';
    require(bool(exported), "Cartographer re-export succeeded");
    std::cout << "VANTA exchange: objects=" << imported_snapshot.objects.size()
              << " meshes=" << exported.value().meshes << " triangles=" << exported.value().triangles << '\n';
    for (const auto& warning : imported.value().warnings)
        std::cout << "import warning: " << warning.message << '\n';
    for (const auto& warning : exported.value().warnings)
        std::cout << "export warning: " << warning << '\n';
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            vanta_roundtrip(argv[1], argv[2]);
            return 0;
        }
        require(argc == 1, "usage: native_scene_import_tests [vanta.gltf new-output-directory]");
        registry_compatibility();
        const auto root = std::filesystem::temp_directory_path() /
            ("carto-import-sprint-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        import_and_history(root);
        ui_import_and_retained_generation(root / "ui");
        ui_default_material_projection(root / "projection");
        ui_external_buffer_capture(root / "external");
        std::filesystem::remove_all(root);
        std::cout << "import sprint regression source complete\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
