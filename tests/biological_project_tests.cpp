#include <carto/biological/authoring.hpp>
#include <carto/biological/proxy_mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-biological-project-tests-" + std::to_string(stamp));
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

biological::BiologicalAuthoringDocument make_source(
    std::uint64_t asset_id,
    core::Revision revision,
    std::string_view version_label) {
    biological::BiologicalAuthoringDocument source;
    const std::string family = asset_id == 7002U ? "canid-proxy" : "human-proxy";
    source.source = biological::AuthoringSourceIdentity{
        "fixture.condition", asset_id, revision,
        digest(family + ":source:" + std::to_string(revision.value()))};
    source.organism_family = family;

    require(static_cast<bool>(source.insert_structure(biological::StructureRecord{
                .id = biological::StructureId{1U},
                .kind = biological::StructureKind::organism,
                .parent = std::nullopt,
                .semantic_type = family + ".organism",
                .coordinate_frame = "organism.local",
                .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt,
                .geometry_digest = digest(std::string(version_label) + ":body"),
                .proxy_surface = biological::EllipsoidProxyProfile{
                    .center = {0.0, 0.5, 0.0}, .radii = {0.75, 1.0, 0.5},
                    .longitude_segments = 12U, .latitude_segments = 6U,
                },
            })),
        "organism source structure is authored");
    require(static_cast<bool>(source.insert_structure(biological::StructureRecord{
                .id = biological::StructureId{2U},
                .kind = biological::StructureKind::skin,
                .parent = biological::StructureId{1U},
                .semantic_type = family + ".surface",
                .coordinate_frame = "organism.local",
                .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt,
                .geometry_digest = digest(std::string(version_label) + ":surface"),
                .proxy_surface = std::nullopt,
            })),
        "surface source structure is authored");
    require(static_cast<bool>(source.validate()),
        "biological source fixture validates before project admission");
    return source;
}

core::Result<project::TransactionReceipt> commit_source(
    project::ProjectDocument& document,
    journal::Journal& journal,
    biological::BiologicalAuthoringDocument source,
    std::string operation) {
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    if (!transaction) {
        return core::Result<project::TransactionReceipt>::failure(transaction.error());
    }
    if (auto result = transaction.value().set_biological_authoring(std::move(source)); !result) {
        return core::Result<project::TransactionReceipt>::failure(result.error());
    }
    return transaction.value().commit(std::move(operation));
}

void biological_sources_are_journaled_and_round_trip() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "events.log");

    require(static_cast<bool>(commit_source(
                document, journal, make_source(7002U, core::Revision{3U}, "canid-v3"),
                "biological.source.canid.create")),
        "the imported canid source enters through a project transaction");
    require(static_cast<bool>(commit_source(
                document, journal, make_source(7003U, core::Revision{5U}, "human-v5"),
                "biological.source.human.create")),
        "the independent human-proxy source enters the same project");
    require(document.biological_authoring_sources().size() == 2U &&
            static_cast<bool>(document.validate()),
        "both source identities and their revisions are retained in project state");

    const std::string encoded = document.serialize();
    require(encoded.starts_with("CARTOGRAPHER_PROJECT 33\n") &&
            encoded.find("BIOLOGICAL_AUTHORING_SOURCES 2\n") != std::string::npos,
        "schema 33 preserves the biological source table");
    const auto reopened = project::ProjectDocument::deserialize(encoded);
    const project::BiologicalAuthoringKey canid_key{"fixture.condition", 7002U};
    const project::BiologicalAuthoringKey human_key{"fixture.condition", 7003U};
    require(reopened && reopened.value().biological_authoring_sources().size() == 2U &&
            reopened.value().biological_authoring_sources().at(canid_key).bytes ==
                document.biological_authoring_sources().at(canid_key).bytes &&
            reopened.value().biological_authoring_sources().at(human_key).digest ==
                document.biological_authoring_sources().at(human_key).digest,
        "project text round-trip preserves both exact sources and byte digests");
    const auto vegetation_receipt_table = encoded.find(
        "CONDITION_VEGETATION_PLACEMENT_RECEIPTS 0\n");
    require(vegetation_receipt_table != std::string::npos,
        "schema 28 always writes its CONDITION placement receipt table");
    std::string missing_vegetation_receipts = encoded;
    missing_vegetation_receipts.erase(vegetation_receipt_table,
        std::string("CONDITION_VEGETATION_PLACEMENT_RECEIPTS 0\n").size());
    require(!project::ProjectDocument::deserialize(missing_vegetation_receipts),
        "schema 28 refuses a missing placement receipt table instead of inventing an empty one");
    std::string excessive_vegetation_receipts = encoded;
    excessive_vegetation_receipts.replace(vegetation_receipt_table,
        std::string("CONDITION_VEGETATION_PLACEMENT_RECEIPTS 0").size(),
        "CONDITION_VEGETATION_PLACEMENT_RECEIPTS 50001");
    require(!project::ProjectDocument::deserialize(excessive_vegetation_receipts),
        "schema 28 refuses a placement receipt table above its hard bound");
    std::string schema_26 = encoded;
    const auto schema_26_mesh_patches = schema_26.find("MESH_PATCHES 0\n");
    require(schema_26_mesh_patches != std::string::npos,
        "schema 25 fixture removes the v29 persistent patch table");
    schema_26.erase(schema_26_mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const auto proxy_receipts = schema_26.find("BIOLOGICAL_PROXY_INSTANCE_RECEIPTS ");
    const auto proxy_receipts_end = schema_26.find("END\n", proxy_receipts);
    require(proxy_receipts != std::string::npos && proxy_receipts_end != std::string::npos,
        "schema 28 includes the mandatory proxy receipt table");
    schema_26.erase(proxy_receipts, proxy_receipts_end - proxy_receipts);
    schema_26.replace(0U, std::string("CARTOGRAPHER_PROJECT 32\n").size(),
        "CARTOGRAPHER_PROJECT 25\n");
    const auto migrated_schema_26 = project::ProjectDocument::deserialize(schema_26);
    require(migrated_schema_26 &&
            migrated_schema_26.value().biological_authoring_sources().size() == 2U &&
            migrated_schema_26.value().biological_proxy_instance_receipts().empty(),
        "schema 25 sources migrate without fabricating proxy instance receipts");
    const auto persisted_source = biological::BiologicalAuthoringDocument::deserialize(
        reopened.value().biological_authoring_sources().at(canid_key).bytes);
    const auto persisted_mesh = persisted_source
        ? biological::compile_ellipsoid_proxy(persisted_source.value(),
            biological::StructureId{1U})
        : core::Result<biological::ProxyMeshProduct>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "test source did not deserialize"));
    require(persisted_mesh && persisted_mesh.value().validate_against(persisted_source.value()),
        "project persistence retains explicit morphology parameters used by the proxy compiler");

    const auto file = directory.path() / "biology.carto";
    require(static_cast<bool>(document.save_atomic(file)),
        "biological authoring project atomically saves");
    const auto loaded = project::ProjectDocument::load(file);
    require(loaded && loaded.value().biological_authoring_sources().size() == 2U,
        "disk close/reopen restores biological sources");

    std::string bad_digest = encoded;
    const std::string identity_record = "\"fixture.condition\" 7002 ";
    const auto identity_position = bad_digest.find(identity_record);
    require(identity_position != std::string::npos,
        "serialized source identity is located for tamper testing");
    const auto digest_position = identity_position + identity_record.size();
    bad_digest.replace(digest_position, 64U, std::string(64U, '0'));
    require(!project::ProjectDocument::deserialize(bad_digest),
        "tampered biological source digest fails project admission");

    std::string missing_lineage = encoded;
    const auto lineage = missing_lineage.find("BIOLOGICAL_AUTHORING_LINEAGE ");
    const auto end = missing_lineage.find("END\n", lineage);
    require(lineage != std::string::npos && end != std::string::npos,
        "lineage section is present for mandatory-record testing");
    missing_lineage.erase(lineage, end - lineage);
    require(!project::ProjectDocument::deserialize(missing_lineage),
        "schema 28 refuses omitted source lineage instead of inventing revisions");

    const auto schema_table = encoded.find("BIOLOGICAL_AUTHORING_SOURCES ");
    const auto schema_end = encoded.rfind("END\n");
    std::string legacy = encoded;
    require(schema_table != std::string::npos && schema_end != std::string::npos,
        "new biological records are isolated at the schema tail");
    legacy.erase(schema_table, schema_end - schema_table);
    const auto legacy_mesh_patches = legacy.find("MESH_PATCHES 0\n");
    require(legacy_mesh_patches != std::string::npos,
        "schema 24 fixture removes the v29 persistent patch table");
    legacy.erase(legacy_mesh_patches, std::string("MESH_PATCHES 0\n").size());
    legacy.replace(0U, std::string("CARTOGRAPHER_PROJECT 32\n").size(),
        "CARTOGRAPHER_PROJECT 24\n");
    const auto legacy_open = project::ProjectDocument::deserialize(legacy);
    if (!legacy_open) {
        std::cerr << "legacy migration diagnostic: "
                  << legacy_open.error().message << '\n';
    }
    require(legacy_open && legacy_open.value().biological_authoring_sources().empty(),
        "schema 24 projects migrate with no fabricated biological sources");
}

void source_edits_and_deletion_preserve_monotonic_lineage() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "events.log");
    const project::BiologicalAuthoringKey canid_key{"fixture.condition", 7002U};

    require(static_cast<bool>(commit_source(
                document, journal, make_source(7002U, core::Revision{3U}, "canid-v3"),
                "biological.source.create")),
        "source starts at its imported revision");

    auto update = make_source(7002U, core::Revision{4U}, "canid-v4");
    auto update_transaction = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    require(update_transaction && update_transaction.value().set_biological_authoring(update),
        "continuous source edit stages at the next exact source revision");
    require(static_cast<bool>(update_transaction.value().commit("biological.source.update")),
        "accepted source edit journals before project publication");

    const core::Revision before_rejections = document.revision();
    auto same_revision = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    require(same_revision && !same_revision.value().set_biological_authoring(
                make_source(7002U, core::Revision{4U}, "canid-tampered-v4")),
        "changed bytes at the same source revision are rejected");
    require(static_cast<bool>(same_revision.value().rollback()) &&
            document.revision() == before_rejections,
        "same-revision rejection leaves published state untouched");

    auto skipped = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    require(skipped && !skipped.value().set_biological_authoring(
                make_source(7002U, core::Revision{6U}, "canid-v6")),
        "source revision gaps fail closed");
    require(static_cast<bool>(skipped.value().rollback()),
        "revision-gap transaction rolls back without publication");

    auto stale_delete = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    require(stale_delete && !stale_delete.value().erase_biological_authoring(
                "fixture.condition", 7002U, core::Revision{3U}),
        "deletion requires the exact active source revision");
    require(static_cast<bool>(stale_delete.value().rollback()),
        "stale deletion transaction rolls back");

    auto remove = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    require(remove && remove.value().erase_biological_authoring(
                "fixture.condition", 7002U, core::Revision{4U}),
        "exact-revision biological source deletion stages successfully");
    require(static_cast<bool>(remove.value().commit("biological.source.delete")) &&
            !document.biological_authoring_sources().contains(canid_key),
        "deletion commits while retaining its source revision tombstone");

    auto recreate = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    require(recreate && recreate.value().set_biological_authoring(
                make_source(7002U, core::Revision{5U}, "canid-v5")),
        "recreated identity must continue its retained source revision");
    require(static_cast<bool>(recreate.value().commit("biological.source.recreate")) &&
            document.biological_authoring_sources().contains(canid_key),
        "valid source recreation is journaled and published");

    const auto events = journal.read_all();
    require(events && events.value().size() == 4U,
        "create, update, delete, and recreation each have a journal event; rejected edits do not");
    const auto reopened = project::ProjectDocument::deserialize(document.serialize());
    require(reopened && reopened.value().validate() &&
            reopened.value().biological_authoring_sources().contains(canid_key),
        "active source and tombstone lineage remain valid after project reopen");
}

void invalid_source_does_not_mutate_project() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "events.log");
    auto invalid_source = make_source(7004U, core::Revision{1U}, "invalid");
    invalid_source.source.source_namespace = "fixture\nprivate";

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "biological-project-tests");
    require(transaction && !transaction.value().set_biological_authoring(
                std::move(invalid_source)),
        "malformed biological source identity is rejected before staging");
    require(static_cast<bool>(transaction.value().rollback()) &&
            document.biological_authoring_sources().empty() &&
            document.revision() == core::Revision{} &&
            journal.read_all().value().empty(),
        "invalid source leaves project, revision, and journal unchanged");
}

} // namespace

int main() {
    biological_sources_are_journaled_and_round_trip();
    source_edits_and_deletion_preserve_monotonic_lineage();
    invalid_source_does_not_mutate_project();
    std::cout << "biological project tests passed\n";
    return EXIT_SUCCESS;
}
