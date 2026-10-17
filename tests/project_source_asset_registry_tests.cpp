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
        path_ = std::filesystem::temp_directory_path() /
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

void schema_twenty_migrates_to_an_empty_registry() {
    project::ProjectDocument document;
    std::string schema_twenty = document.serialize();
    const std::string current_header = "CARTOGRAPHER_PROJECT 22\n";
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
    const auto registry = schema_twenty.find("SOURCE_ASSET_REGISTRY_HEX ");
    require(registry != std::string::npos, "fixture includes new registry record to remove");
    const auto line_end = schema_twenty.find('\n', registry);
    schema_twenty.erase(registry, line_end - registry + 1U);

    const auto migrated = project::ProjectDocument::deserialize(schema_twenty);
    require(migrated && migrated.value().source_asset_registry().records().empty() &&
            migrated.value().serialize().starts_with("CARTOGRAPHER_PROJECT 22\n"),
        "schema 20 remains readable and upgrades to a valid empty registry on next save");
}

} // namespace

int main() {
    project_source_reimport_is_journaled_and_persistent();
    schema_twenty_migrates_to_an_empty_registry();
    std::cout << "project source asset registry tests passed\n";
    return EXIT_SUCCESS;
}
