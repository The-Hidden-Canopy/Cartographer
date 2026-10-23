#include <carto/assets/source_registry.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-project-source-assets-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

assets::Sha256Digest digest(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

assets::SourceImportRecord record(
    core::Revision generation,
    std::string_view source,
    std::string_view geometry) {
    assets::SourceImportRecord value;
    value.identity = "asset.oak";
    value.relative_source_path = "Assets/trees/oak.glb";
    value.source_generation = generation;
    value.kind = assets::SourceAssetKind::mesh;
    value.source_digest = digest(source);
    value.importer_identity = "carto.import.gltf";
    value.importer_version = 1U;
    value.artifact_identity = "mesh.oak";
    value.artifact_digest = digest(geometry);
    value.domain_digests = {{assets::SourceAssetDomain::geometry, digest(geometry)}};
    return value;
}

void project_source_reimport_is_journaled_and_persistent() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "source-assets.log");
    const auto original = record(core::Revision{1U}, "source-1", "geometry-1");
    const auto initial_plan = assets::plan_source_reimport(
        document.source_asset_registry(), {original});
    require(static_cast<bool>(initial_plan), "first imported source plan is constructed");

    auto create = project::ProjectTransaction::begin(document, journal, "asset-import-test");
    require(create && create.value().apply_source_reimport_plan(initial_plan.value()),
        "project transaction stages a validated initial reimport plan");
    require(document.source_asset_registry().records().empty(),
        "staged import is not visible before its journal append");
    const auto created = create.value().commit("source.asset.import");
    require(created && created.value().revision_after == core::Revision{1U} &&
            document.source_asset_registry().records().size() == 1U,
        "journaled import publishes exactly one project revision");
    const auto entries = journal.read_all();
    require(entries && entries.value().size() == 1U &&
            entries.value().front().event_type == "source.asset.import",
        "successful asset registry mutation has a matching journal event");

    const auto encoded = document.serialize();
    const auto reopened = project::ProjectDocument::deserialize(encoded);
    require(reopened && reopened.value().source_asset_registry().serialize() ==
            document.source_asset_registry().serialize(),
        ".carto schema 21 round-trips imported source identity and generation lineage");
    const auto path = directory.path() / "assets.carto";
    const auto saved = document.save_atomic(path);
    require(static_cast<bool>(saved), saved
        ? "project with a source registry atomically saves"
        : "project with a source registry atomically saves: " + saved.error().message);
    const auto resaved = document.save_atomic(path);
    require(static_cast<bool>(resaved), resaved
        ? "project with a source registry atomically replaces an existing save"
        : "project with a source registry atomically replaces an existing save: " +
            resaved.error().message);
    const auto loaded = project::ProjectDocument::load(path);
    require(loaded && loaded.value().source_asset_registry().records().size() == 1U,
        "source identity survives file close and reopen");

    auto stale = project::ProjectTransaction::begin(document, journal, "asset-import-test");
    require(stale && !stale.value().apply_source_reimport_plan(initial_plan.value()),
        "a previously reviewed import plan is blocked after the project base changes");
    require(static_cast<bool>(stale.value().rollback()),
        "rejected stale reimport transaction rolls back");

    const auto updated_record = record(
        core::Revision{2U}, "source-2", "geometry-2");
    const auto update_plan = assets::plan_source_reimport(
        document.source_asset_registry(), {updated_record});
    require(static_cast<bool>(update_plan), "changed source produces a reviewed plan");
    auto update = project::ProjectTransaction::begin(document, journal, "asset-import-test");
    require(update && update.value().apply_source_reimport_plan(update_plan.value()),
        "reviewed geometry update stages through the transaction boundary");
    const auto updated = update.value().commit("source.asset.reimport");
    require(updated && document.source_asset_registry().records().at("asset.oak").serialize() ==
            updated_record.serialize(),
        "reimport publishes the exact classified candidate through the journal");

    const auto remove_plan = assets::plan_source_reimport(document.source_asset_registry(), {});
    require(static_cast<bool>(remove_plan), "source removal is represented as a reviewed plan");
    auto remove = project::ProjectTransaction::begin(document, journal, "asset-import-test");
    require(remove && remove.value().apply_source_reimport_plan(remove_plan.value()),
        "reviewed source removal stages through the transaction boundary");
    const auto removed = remove.value().commit("source.asset.remove");
    require(removed && document.source_asset_registry().records().empty() &&
            document.source_asset_registry().lineage().at("asset.oak") == core::Revision{2U},
        "removal retains generation tombstone so stale imports cannot return");
    const auto final_open = project::ProjectDocument::deserialize(document.serialize());
    require(final_open && final_open.value().source_asset_registry().lineage().at("asset.oak") ==
            core::Revision{2U},
        "source generation tombstones persist in the project file");
}

void generated_botanical_import_receipt_is_persistent_and_schema_gated() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "botanical-generation.log");

    botanical::Source source;
    source.identity = "pine-project-fixture";
    source.revision = core::Revision{3U};
    const std::string snapshot = source.serialize();
    const auto input_digest = digest(snapshot);
    auto generated = record(core::Revision{1U}, "generated-gltf-bytes", "generated-geometry");
    generated.identity = "botanical.generated.fixture";
    generated.relative_source_path = ".carto-assets/source-blobs/generated.gltf";
    generated.importer_identity = "carto.import.generated-gltf";
    generated.artifact_identity = "mesh.botanical.fixture";
    generated.botanical_generation = assets::BotanicalGenerationReceipt{
        snapshot, digest("opaque-job-id"), input_digest, generated.source_digest};

    const auto plan = assets::plan_source_reimport(document.source_asset_registry(), {generated});
    require(static_cast<bool>(plan),
        "generated mesh and exact source receipt form an import plan");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "botanical-generation-test");
    require(transaction && transaction.value().apply_source_reimport_plan(plan.value()),
        "generated mesh receipt stages at the governed project transaction boundary");
    const auto committed = transaction.value().commit("source.asset.import");
    require(committed && committed.value().revision_after > committed.value().revision_before &&
            committed.value().operation == "source.asset.import",
        "generation receipt mutation receives a revisioned project journal entry");

    const std::string encoded = document.serialize();
    require(encoded.starts_with("CARTOGRAPHER_PROJECT 33\n"),
        "generation receipts are marked with project schema 33");
    const auto reopened = project::ProjectDocument::deserialize(encoded);
    require(reopened && reopened.value().source_asset_registry().records().at(
            generated.identity).botanical_generation.has_value() &&
            reopened.value().source_asset_registry().records().at(
                generated.identity).botanical_generation->source_snapshot == snapshot,
        "project save/reopen preserves the exact source-to-output receipt");
    const auto path = directory.path() / "botanical-generation.carto";
    require(static_cast<bool>(document.save_atomic(path)),
        "generated receipt project saves atomically");
    const auto loaded = project::ProjectDocument::load(path);
    require(loaded && loaded.value().source_asset_registry().records().at(
            generated.identity).botanical_generation->output_artifact_digest ==
                generated.source_digest,
        "on-disk reopen preserves the verified output digest");

    std::string mislabeled_v26 = encoded;
    const auto patch_table = mislabeled_v26.find("MESH_PATCHES 0\n");
    require(patch_table != std::string::npos,
        "schema 26 fixture removes the v29 persistent patch table");
    mislabeled_v26.erase(patch_table, std::string("MESH_PATCHES 0\n").size());
    const auto placement_table = mislabeled_v26.find(
        "CONDITION_VEGETATION_PLACEMENT_RECEIPTS 0\n");
    require(placement_table != std::string::npos,
        "schema 26 fixture removes the v28 placement receipt table");
    mislabeled_v26.erase(placement_table,
        std::string("CONDITION_VEGETATION_PLACEMENT_RECEIPTS 0\n").size());
    const auto rig_module_table = mislabeled_v26.find("RIG_MODULE_TEMPLATES 0\n");
    require(rig_module_table != std::string::npos,
        "schema 26 fixture removes the v30 rig-module library table");
    mislabeled_v26.erase(rig_module_table,
        std::string("RIG_MODULE_TEMPLATES 0\n").size());
    const auto decal_table = mislabeled_v26.find("DECAL_PLACEMENTS 0\n");
    require(decal_table != std::string::npos,
        "schema 26 fixture removes the v31 decal placement table");
    mislabeled_v26.erase(decal_table, std::string("DECAL_PLACEMENTS 0\n").size());
    const auto standards_line = mislabeled_v26.find("AUTHORING_STANDARDS_HEX ");
    require(standards_line != std::string::npos,
        "schema 26 fixture removes the v32 authoring standards record");
    const auto standards_end = mislabeled_v26.find('\n', standards_line);
    mislabeled_v26.erase(standards_line, standards_end - standards_line + 1U);
    for (const std::string table : {
            "POSE_LIBRARIES 0\n",
            "WEIGHT_EDIT_RECEIPTS 0\n",
            "CURVE_EDIT_RECEIPTS 0\n"}) {
        const auto record = mislabeled_v26.find(table);
        require(record != std::string::npos,
            "schema 26 fixture removes a schema v33 character record table");
        mislabeled_v26.erase(record, table.size());
    }
    mislabeled_v26.replace(0U, std::string("CARTOGRAPHER_PROJECT 33\n").size(),
        "CARTOGRAPHER_PROJECT 26\n");
    require(!project::ProjectDocument::deserialize(mislabeled_v26),
        "schema 26 refuses a v2 generation receipt instead of silently ignoring it");
}

void project_source_relink_is_journaled_and_persistent() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "source-relink.log");
    const auto original = record(core::Revision{1U}, "source-1", "geometry-1");
    const auto initial_plan = assets::plan_source_reimport(
        document.source_asset_registry(), {original});
    require(static_cast<bool>(initial_plan), "relink fixture import plan is constructed");
    auto create = project::ProjectTransaction::begin(document, journal, "asset-import-test");
    require(create && create.value().apply_source_reimport_plan(initial_plan.value()),
        "relink fixture import stages through the transaction boundary");
    require(static_cast<bool>(create.value().commit("source.asset.import")),
        "relink fixture import publishes one project revision");

    const auto relink = assets::plan_source_relink(document.source_asset_registry(),
        "asset.oak", core::Revision{1U}, "Assets/trees/oak-moved.glb",
        original.source_digest);
    require(relink && relink.value().disposition == assets::SourceRelinkDisposition::moved,
        "digest-identical staged bytes review as a pure move");
    const auto adoption = assets::plan_source_reimport(
        document.source_asset_registry(), {relink.value().incoming});
    require(adoption && adoption.value().entries.size() == 1U &&
            adoption.value().entries.front().change ==
                assets::SourceReimportChange::metadata_only,
        "pure move adopts through the reviewed metadata-only classification");
    auto update = project::ProjectTransaction::begin(document, journal, "asset-relink-test");
    require(update && update.value().apply_source_reimport_plan(adoption.value()),
        "reviewed relink stages through the transaction boundary");
    const auto updated = update.value().commit("source.asset.relink");
    require(updated && updated.value().revision_after == core::Revision{2U} &&
            document.source_asset_registry().records().at("asset.oak").identity == "asset.oak" &&
            document.source_asset_registry().records().at("asset.oak").relative_source_path ==
                "Assets/trees/oak-moved.glb" &&
            document.source_asset_registry().records().at("asset.oak").source_digest ==
                original.source_digest &&
            document.source_asset_registry().records().at("asset.oak").source_generation ==
                core::Revision{2U},
        "relink publishes the rebound record through the journal");
    const auto entries = journal.read_all();
    require(entries && entries.value().size() == 2U &&
            entries.value().back().event_type == "source.asset.relink",
        "successful source relink has a matching journal event");

    auto stale = project::ProjectTransaction::begin(document, journal, "asset-relink-test");
    require(stale && !stale.value().apply_source_reimport_plan(adoption.value()),
        "a previously reviewed relink is blocked after the project base changes");
    require(static_cast<bool>(stale.value().rollback()),
        "rejected stale relink transaction rolls back");

    const auto changed = record(core::Revision{2U}, "source-2", "geometry-1");
    require(!assets::plan_source_relink(document.source_asset_registry(), "asset.oak",
            core::Revision{2U}, "Assets/trees/oak-v2.glb", changed.source_digest),
        "digest-changed staged bytes fail closed and require the reimport flow");

    const auto path = directory.path() / "relink.carto";
    const auto saved = document.save_atomic(path);
    require(static_cast<bool>(saved), saved
        ? "project with a relinked source atomically saves"
        : "project with a relinked source atomically saves: " + saved.error().message);
    const auto loaded = project::ProjectDocument::load(path);
    require(loaded && loaded.value().source_asset_registry().records().at("asset.oak")
                .relative_source_path == "Assets/trees/oak-moved.glb" &&
            loaded.value().source_asset_registry().records().at("asset.oak").source_digest ==
                original.source_digest &&
            loaded.value().source_asset_registry().lineage().at("asset.oak") ==
                core::Revision{2U},
        "relinked identity, locator, and generation lineage survive file close and reopen");
}

void schema_twenty_migrates_to_an_empty_registry() {
    project::ProjectDocument document;
    std::string schema_twenty = document.serialize();
    const auto mesh_patches = schema_twenty.find("MESH_PATCHES 0\n");
    require(mesh_patches != std::string::npos,
        "schema 20 fixture removes the v29 persistent patch table");
    schema_twenty.erase(mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const std::string current_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion) + "\n";
    const auto header = schema_twenty.find(current_header);
    require(header == 0U, "fixture begins with current project schema header");
    schema_twenty.replace(header, current_header.size(), "CARTOGRAPHER_PROJECT 20\n");
    const auto sweep_sources = schema_twenty.find("SWEEP_SOURCES 0\n");
    require(sweep_sources != std::string::npos,
        "schema 20 fixture removes the v22 sweep-source table");
    schema_twenty.erase(sweep_sources, std::string("SWEEP_SOURCES 0\n").size());
    const auto sweep_lineage = schema_twenty.find("SWEEP_SOURCE_LINEAGE 0\n");
    require(sweep_lineage != std::string::npos,
        "schema 20 fixture removes the v22 sweep-source lineage table");
    schema_twenty.erase(sweep_lineage, std::string("SWEEP_SOURCE_LINEAGE 0\n").size());
    for (const std::string table : {
            "SIGN_TEXT_SOURCES 0\n",
            "SIGN_TEXT_SOURCE_LINEAGE 0\n",
            "SIGN_TEXT_INSTANCE_RECEIPTS 0\n",
            "ROAD_SURFACE_INSTANCE_RECEIPTS 0\n",
            "BIOLOGICAL_AUTHORING_SOURCES 0\n",
            "BIOLOGICAL_AUTHORING_LINEAGE 0\n",
            "BIOLOGICAL_PROXY_INSTANCE_RECEIPTS 0\n",
            "CONDITION_VEGETATION_PLACEMENT_RECEIPTS 0\n",
            "RIG_MODULE_TEMPLATES 0\n",
            "DECAL_PLACEMENTS 0\n",
            "POSE_LIBRARIES 0\n",
            "WEIGHT_EDIT_RECEIPTS 0\n",
            "CURVE_EDIT_RECEIPTS 0\n"}) {
        const auto record = schema_twenty.find(table);
        require(record != std::string::npos,
            "schema 20 fixture removes a later source table");
        schema_twenty.erase(record, table.size());
    }
    const auto registry = schema_twenty.find("SOURCE_ASSET_REGISTRY_HEX ");
    require(registry != std::string::npos, "fixture includes new registry record to remove");
    const auto line_end = schema_twenty.find('\n', registry);
    schema_twenty.erase(registry, line_end - registry + 1U);
    const auto standards = schema_twenty.find("AUTHORING_STANDARDS_HEX ");
    require(standards != std::string::npos,
        "schema 20 fixture removes the v32 authoring standards record");
    const auto standards_end = schema_twenty.find('\n', standards);
    schema_twenty.erase(standards, standards_end - standards + 1U);

    const auto migrated = project::ProjectDocument::deserialize(schema_twenty);
    require(migrated && migrated.value().source_asset_registry().records().empty() &&
            migrated.value().serialize().starts_with(current_header),
        "schema 20 remains readable and upgrades to a valid empty registry on next save");
}

void custom_corner_normals_survive_project_save_and_reopen() {
    geometry::EditableMesh mesh;
    const geometry::VertexId first{1U};
    const geometry::VertexId second{2U};
    const geometry::VertexId third{3U};
    const geometry::VertexId fourth{4U};
    require(static_cast<bool>(mesh.insert_bulk(
        {{first, {0.0, 0.0, 0.0}}, {second, {1.0, 0.0, 0.0}},
         {third, {1.0, 1.0, 0.0}}, {fourth, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {first, second, third}},
         {geometry::FaceId{2U}, {first, third, fourth}}})),
        "custom-normal project fixture topology inserts");
    const auto topology = mesh.topology();
    require(static_cast<bool>(topology),
        "custom-normal project fixture topology resolves");
    std::vector<attributes::AttributeValue> normal_values;
    normal_values.reserve(topology.value().corners.size());
    for (const auto& corner : topology.value().corners) {
        const core::Vec3d normal = corner.face == geometry::FaceId{2U} &&
            corner.vertex == first ? core::Vec3d{0.0, 1.0, 1.0} :
            core::Vec3d{0.0, 0.0, 1.0};
        normal_values.emplace_back(normal);
    }
    attributes::AttributeSet payload;
    require(static_cast<bool>(payload.set_domain_count(
        attributes::AttributeDomain::corner, normal_values.size())) &&
        static_cast<bool>(payload.add_layer(attributes::AttributeLayer{
            {"cartographer.normal0", attributes::AttributeDomain::corner,
                attributes::AttributeType::vec3}, std::move(normal_values)})),
        "custom-normal project fixture attaches seam-specific corner values");
    require(static_cast<bool>(mesh.set_attribute_payload(
        std::move(payload), attributes::UvSetTable{})),
        "custom-normal project fixture validates its attribute payload");

    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "custom-normal.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "custom-normal-roundtrip-test");
    require(static_cast<bool>(transaction),
        "custom-normal project transaction begins");
    const auto mesh_asset = transaction.value().add_mesh(std::move(mesh));
    require(static_cast<bool>(mesh_asset),
        "custom-normal mesh enters the project document");
    const auto committed = transaction.value().commit("mesh.source.import");
    require(static_cast<bool>(committed) && committed.value().revision_after ==
            core::Revision{1U},
        "custom-normal mesh admission commits through the journal");
    const auto path = directory.path() / "custom-normals.carto";
    const auto saved = document.save_atomic(path);
    require(static_cast<bool>(saved), saved
        ? "custom-normal project atomically saves"
        : "custom-normal project atomically saves: " + saved.error().message);
    const auto reopened = project::ProjectDocument::load(path);
    require(static_cast<bool>(reopened), reopened
        ? "custom-normal project reopens"
        : "custom-normal project reopens: " + reopened.error().message);
    const auto* normals = reopened.value().meshes().at(mesh_asset.value())
        .attributes().find("cartographer.normal0");
    require(normals != nullptr && normals->values.size() == topology.value().corners.size(),
        ".carto save/reopen retains the typed custom-normal corner layer");
    const auto reopened_topology = reopened.value().meshes().at(mesh_asset.value()).topology();
    require(static_cast<bool>(reopened_topology),
        "reopened custom-normal topology resolves");
    bool first_face_normal_seen = false;
    bool seam_normal_seen = false;
    for (std::size_t index = 0U; index < reopened_topology.value().corners.size(); ++index) {
        const auto& corner = reopened_topology.value().corners[index];
        const auto* normal = std::get_if<core::Vec3d>(&normals->values[index]);
        require(normal != nullptr, "reopened custom normal remains a vec3");
        if (corner.face == geometry::FaceId{1U} && corner.vertex == first) {
            first_face_normal_seen = normal->z == 1.0 && normal->y == 0.0;
        }
        if (corner.face == geometry::FaceId{2U} && corner.vertex == first) {
            seam_normal_seen = normal->y == 1.0 && normal->z == 1.0;
        }
    }
    require(first_face_normal_seen && seam_normal_seen,
        "save/reopen retains distinct normals at the shared-vertex seam");
}

} // namespace

int main() {
    project_source_reimport_is_journaled_and_persistent();
    generated_botanical_import_receipt_is_persistent_and_schema_gated();
    project_source_relink_is_journaled_and_persistent();
    schema_twenty_migrates_to_an_empty_registry();
    custom_corner_normals_survive_project_save_and_reopen();
    std::cout << "project source asset registry tests passed\n";
    return EXIT_SUCCESS;
}
