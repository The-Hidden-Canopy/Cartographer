#include <carto/geometry/sweep.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
            ("cartographer-sweep-project-tests-" + std::to_string(stamp));
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

geometry::CurvePath make_centerline(core::Revision revision = core::Revision{1U}) {
    geometry::CurvePath path;
    path.source_identity = "fixture.pipe.centerline";
    path.source_revision = revision;
    path.segments.push_back({
        {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
        {0.0, 2.0, 0.0}, {0.0, 3.0, 0.0}});
    return path;
}

geometry::SweepSource make_source(
    core::Revision source_revision = core::Revision{1U},
    core::Revision centerline_revision = core::Revision{1U}) {
    geometry::SweepSource source;
    source.source_identity = "fixture.pipe.recipe";
    source.source_revision = source_revision;
    source.centerline_identity = "fixture.pipe.centerline";
    source.centerline_revision = centerline_revision;
    source.profile = {{0.1, 0.0}, {0.0, 0.1}, {-0.1, 0.0}, {0.0, -0.1}};
    source.reference_up = {0.0, 0.0, 1.0};
    source.samples_per_segment = 3U;
    source.cap_policy = geometry::SweepCapPolicy::both;
    return source;
}

core::Result<project::TransactionReceipt> commit_source_pair(
    project::ProjectDocument& document,
    journal::Journal& journal,
    geometry::CurvePath path,
    geometry::SweepSource source,
    std::string operation) {
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    if (!transaction) {
        return core::Result<project::TransactionReceipt>::failure(transaction.error());
    }
    if (auto result = transaction.value().set_curve_path(std::move(path)); !result) {
        return core::Result<project::TransactionReceipt>::failure(result.error());
    }
    if (auto result = transaction.value().set_sweep_source(std::move(source)); !result) {
        return core::Result<project::TransactionReceipt>::failure(result.error());
    }
    return transaction.value().commit(std::move(operation));
}

void project_save_reopen_and_lineage_are_governed() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "events.log");
    const auto created = commit_source_pair(
        document, journal, make_centerline(), make_source(), "sweep.source.create");
    require(created && document.sweep_sources().size() == 1U &&
            document.sweep_sources().at("fixture.pipe.recipe").source_revision ==
                core::Revision{1U},
        "centerline and sweep recipe commit together through the project transaction");
    require(static_cast<bool>(document.validate()),
        "active sweep binding passes full project validation");

    const std::string encoded = document.serialize();
    require(encoded.starts_with("CARTOGRAPHER_PROJECT 33\n"),
        "project schema 33 preserves current source-authoring format compatibility");
    const auto reopened = project::ProjectDocument::deserialize(encoded);
    require(reopened && reopened.value().sweep_sources().size() == 1U &&
            reopened.value().sweep_sources().at("fixture.pipe.recipe").serialize() ==
                document.sweep_sources().at("fixture.pipe.recipe").serialize(),
        "save/reopen preserves exact sweep recipe identity and source parameters");
    const auto path = directory.path() / "pipe.carto";
    const auto saved_project = document.save_atomic(path);
    if (!saved_project) std::cerr << "save diagnostic: " << saved_project.error().message << '\n';
    require(static_cast<bool>(saved_project),
        "project with sweep source atomically saves");
    const auto loaded = project::ProjectDocument::load(path);
    require(loaded && loaded.value().sweep_sources().size() == 1U,
        "project file close/reopen restores the sweep source");

    auto source_update = make_source(core::Revision{2U});
    source_update.end_scale = 0.65;
    auto update = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    require(update && update.value().set_sweep_source(source_update),
        "sweep source update stages only at the next identity revision");
    const auto updated = update.value().commit("sweep.source.update");
    require(updated && document.sweep_sources().at("fixture.pipe.recipe").source_revision ==
                core::Revision{2U},
        "source update advances one revision through journaled publication");

    auto stale_update = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    auto stale_source = make_source(core::Revision{2U});
    stale_source.end_scale = 0.2;
    require(stale_update && !stale_update.value().set_sweep_source(stale_source),
        "same-revision source rewrite is rejected");
    require(static_cast<bool>(stale_update.value().rollback()),
        "rejected same-revision source edit leaves no staged transaction open");

    auto erase_curve = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    require(erase_curve && !erase_curve.value().erase_curve_path(
            "fixture.pipe.centerline", core::Revision{1U}),
        "centerline cannot be erased while an active sweep source references it");
    require(static_cast<bool>(erase_curve.value().rollback()),
        "blocked centerline removal rolls back cleanly");

    auto erase_source = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    require(erase_source && erase_source.value().erase_sweep_source(
            "fixture.pipe.recipe", core::Revision{2U}),
        "source can be removed with an exact expected revision");
    const auto erased = erase_source.value().commit("sweep.source.delete");
    require(erased && document.sweep_sources().empty(),
        "source deletion is published through the journal");

    auto recreate = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    auto revision_three = make_source(core::Revision{3U});
    require(recreate && recreate.value().set_sweep_source(revision_three),
        "deleted source identity can only return at the next retained revision");
    const auto recreated = recreate.value().commit("sweep.source.recreate");
    require(recreated && document.sweep_sources().at("fixture.pipe.recipe").source_revision ==
                core::Revision{3U},
        "tombstoned sweep source lineage survives deletion and recreation");

    const auto journal_events = journal.read_all();
    require(journal_events && journal_events.value().size() == 4U,
        "create, update, delete, and recreation each retain one journal receipt");
}

void stale_bindings_and_schema_migrations_fail_closed() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "events.log");
    auto missing = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    require(missing && !missing.value().set_sweep_source(make_source()),
        "sweep source cannot bind to an absent centerline");
    require(static_cast<bool>(missing.value().rollback()),
        "missing-centerline source admission can be rolled back");

    const auto created = commit_source_pair(
        document, journal, make_centerline(), make_source(), "sweep.source.create");
    require(static_cast<bool>(created), "valid source pair commits before stale-path tests");

    auto stale_path = make_centerline(core::Revision{2U});
    auto invalid_update = project::ProjectTransaction::begin(
        document, journal, "procedural-sweep-tests");
    require(invalid_update && invalid_update.value().set_curve_path(stale_path),
        "new centerline revision can be staged for atomic source review");
    require(!invalid_update.value().commit("sweep.centerline.stale-binding"),
        "project transaction refuses to publish when the recipe would become stale");
    require(static_cast<bool>(invalid_update.value().rollback()) &&
            document.curve_paths().at("fixture.pipe.centerline").source_revision ==
                core::Revision{1U},
        "rejected centerline edit leaves the authoritative path unchanged");

    std::string missing_table = document.serialize();
    const auto sweep_table = missing_table.find("SWEEP_SOURCES ");
    const auto spatial_table = missing_table.find("SPATIAL_FIELDS ", sweep_table);
    require(sweep_table != std::string::npos && spatial_table != std::string::npos,
        "current project has a separately framed sweep table");
    missing_table.erase(sweep_table, spatial_table - sweep_table);
    require(!project::ProjectDocument::deserialize(missing_table),
        "current schema refuses a missing mandatory sweep table");

    std::string missing_lineage = document.serialize();
    const auto lineage_table = missing_lineage.find("SWEEP_SOURCE_LINEAGE ");
    const auto registry_table = missing_lineage.find("SOURCE_ASSET_REGISTRY_HEX ", lineage_table);
    require(lineage_table != std::string::npos && registry_table != std::string::npos,
        "current project persists sweep lineage before its source asset table");
    missing_lineage.erase(lineage_table, registry_table - lineage_table);
    require(!project::ProjectDocument::deserialize(missing_lineage),
        "current schema refuses missing sweep lineage rather than inventing revisions");

    std::string legacy_v22 = document.serialize();
    const auto legacy_mesh_patches = legacy_v22.find("MESH_PATCHES 0\n");
    require(legacy_mesh_patches != std::string::npos,
        "schema 22 fixture removes the v29 persistent patch table");
    legacy_v22.erase(legacy_mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const auto sign_table_start = legacy_v22.find("SIGN_TEXT_SOURCES ");
    const auto legacy_spatial_table = legacy_v22.find("SPATIAL_FIELDS ", sign_table_start);
    require(sign_table_start != std::string::npos &&
            legacy_spatial_table != std::string::npos,
        "schema 23 sign-text records can be isolated to construct a real schema-22 fixture");
    legacy_v22.erase(sign_table_start, legacy_spatial_table - sign_table_start);
    const auto sign_lineage_start = legacy_v22.find("SIGN_TEXT_SOURCE_LINEAGE ");
    const auto legacy_registry = legacy_v22.find(
        "SOURCE_ASSET_REGISTRY_HEX ", sign_lineage_start);
    require(sign_lineage_start != std::string::npos &&
            legacy_registry != std::string::npos,
        "schema 23 sign-text lineage and receipts can be isolated for schema-22 migration");
    legacy_v22.erase(sign_lineage_start, legacy_registry - sign_lineage_start);
    const auto biological_v22 = legacy_v22.find("BIOLOGICAL_AUTHORING_SOURCES ");
    const auto end_v22 = legacy_v22.find("END\n", biological_v22);
    legacy_v22.erase(biological_v22, end_v22 - biological_v22);
    legacy_v22.replace(0U, std::string("CARTOGRAPHER_PROJECT 32\n").size(),
        "CARTOGRAPHER_PROJECT 22\n");
    const auto migrated_v22 = project::ProjectDocument::deserialize(legacy_v22);
    require(migrated_v22 && migrated_v22.value().sweep_sources().size() == 1U,
        "schema 22 preserves sweep recipes and migrates with no invented sources");

    project::ProjectDocument empty;
    std::string legacy_v21 = empty.serialize();
    const auto empty_mesh_patches = legacy_v21.find("MESH_PATCHES 0\n");
    require(empty_mesh_patches != std::string::npos,
        "schema 21 fixture removes the v29 persistent patch table");
    legacy_v21.erase(empty_mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const auto empty_sweep_table = legacy_v21.find("SWEEP_SOURCES ");
    const auto empty_spatial_table = legacy_v21.find("SPATIAL_FIELDS ", empty_sweep_table);
    legacy_v21.erase(empty_sweep_table, empty_spatial_table - empty_sweep_table);
    const auto empty_sweep_lineage = legacy_v21.find("SWEEP_SOURCE_LINEAGE ");
    const auto empty_registry = legacy_v21.find("SOURCE_ASSET_REGISTRY_HEX ", empty_sweep_lineage);
    legacy_v21.erase(empty_sweep_lineage, empty_registry - empty_sweep_lineage);
    const auto biological_v21 = legacy_v21.find("BIOLOGICAL_AUTHORING_SOURCES ");
    const auto end_v21 = legacy_v21.find("END\n", biological_v21);
    legacy_v21.erase(biological_v21, end_v21 - biological_v21);
    legacy_v21.replace(0U, std::string("CARTOGRAPHER_PROJECT 32\n").size(),
        "CARTOGRAPHER_PROJECT 21\n");
    const auto migrated = project::ProjectDocument::deserialize(legacy_v21);
    require(migrated && migrated.value().sweep_sources().empty(),
        "schema 21 migrates without fabricated procedural sources");
}

} // namespace

int main() {
    project_save_reopen_and_lineage_are_governed();
    stale_bindings_and_schema_migrations_fail_closed();
    std::cout << "All sweep source project tests passed.\n";
    return EXIT_SUCCESS;
}
