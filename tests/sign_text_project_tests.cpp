#include <carto/geometry/sign.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
        path_ = std::filesystem::current_path() /
            ("cartographer-sign-text-project-tests-" + std::to_string(stamp));
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

geometry::SignTextSource make_source(core::Revision revision = core::Revision{1U}) {
    geometry::SignTextSource source;
    source.source_identity = "fixture.sign.generic";
    source.source_revision = revision;
    source.text = "OPEN\nWELCOME";
    return source;
}

core::Result<project::TransactionReceipt> commit_source(
    project::ProjectDocument& document,
    journal::Journal& journal,
    geometry::SignTextSource source,
    std::string operation) {
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "sign-text-project-tests");
    if (!transaction) {
        return core::Result<project::TransactionReceipt>::failure(transaction.error());
    }
    if (auto result = transaction.value().set_sign_text_source(std::move(source)); !result) {
        return core::Result<project::TransactionReceipt>::failure(result.error());
    }
    return transaction.value().commit(std::move(operation));
}

void transaction_persistence_and_monotonic_source_lineage() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "events.log");

    const auto created = commit_source(
        document, journal, make_source(), "sign.source.create");
    if (!created) std::cerr << "transaction diagnostic: " << created.error().message << '\n';
    require(static_cast<bool>(created),
        "sign source enters the project through its durable transaction");
    require(document.sign_text_sources().size() == 1U &&
        document.sign_text_sources().at("fixture.sign.generic").source_revision ==
            core::Revision{1U} && document.validate(),
        "project retains the active sign source and matching lineage");

    const auto path = directory.path() / "signs.carto";
    const auto saved = document.save_atomic(path);
    if (!saved) std::cerr << "save diagnostic: " << saved.error().message << '\n';
    require(static_cast<bool>(saved), "sign-text project atomically saves");
    const auto reopened = project::ProjectDocument::load(path);
    require(reopened && reopened.value().sign_text_sources().size() == 1U &&
        reopened.value().sign_text_sources().at("fixture.sign.generic").serialize() ==
            document.sign_text_sources().at("fixture.sign.generic").serialize(),
        "disk close/reopen preserves the canonical source and revision");

    auto same_revision = make_source();
    same_revision.text = "CLOSED\nWELCOME";
    auto rejected = project::ProjectTransaction::begin(
        document, journal, "sign-text-project-tests");
    require(rejected && !rejected.value().set_sign_text_source(same_revision) &&
        rejected.value().rollback(),
        "same-revision rewrite is rejected without publishing a mutation");

    auto update = make_source(core::Revision{2U});
    update.text = "CLOSED\nWELCOME";
    require(static_cast<bool>(commit_source(
            document, journal, update, "sign.source.update")),
        "source edit must advance exactly one retained revision");
    auto stale_delete = project::ProjectTransaction::begin(
        document, journal, "sign-text-project-tests");
    require(stale_delete && !stale_delete.value().erase_sign_text_source(
            "fixture.sign.generic", core::Revision{1U}) && stale_delete.value().rollback(),
        "stale source deletion is rejected");
    auto erase = project::ProjectTransaction::begin(
        document, journal, "sign-text-project-tests");
    require(erase && erase.value().erase_sign_text_source(
            "fixture.sign.generic", core::Revision{2U}) &&
        erase.value().commit("sign.source.delete") && document.sign_text_sources().empty(),
        "exact-revision deletion commits while retaining the tombstone");
    require(static_cast<bool>(commit_source(
            document, journal, make_source(core::Revision{3U}), "sign.source.recreate")) &&
        document.sign_text_sources().at("fixture.sign.generic").source_revision ==
            core::Revision{3U},
        "deleted sign source identity can only return at its next retained revision");
    const auto events = journal.read_all();
    require(events && events.value().size() == 4U,
        "create, update, delete, and recreation each have a journal event");
}

void schema_tables_are_mandatory_and_legacy_v22_does_not_invent_signs() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "events.log");
    require(static_cast<bool>(commit_source(
            document, journal, make_source(), "sign.source.create")),
        "valid sign source is committed before hostile serialization checks");
    const auto serialized = document.serialize();

    std::string missing_source_table = serialized;
    const auto source_begin = missing_source_table.find("SIGN_TEXT_SOURCES ");
    const auto source_end = missing_source_table.find("SPATIAL_FIELDS ", source_begin);
    require(source_begin != std::string::npos && source_end != std::string::npos,
        "schema 23 frames the sign-text source table");
    missing_source_table.erase(source_begin, source_end - source_begin);
    require(!project::ProjectDocument::deserialize(missing_source_table),
        "schema 23 rejects a missing sign-text source table");

    std::string missing_lineage = serialized;
    const auto lineage_begin = missing_lineage.find("SIGN_TEXT_SOURCE_LINEAGE ");
    const auto lineage_end = missing_lineage.find("SIGN_TEXT_INSTANCE_RECEIPTS ", lineage_begin);
    require(lineage_begin != std::string::npos && lineage_end != std::string::npos,
        "schema 23 frames the sign-text identity lineage");
    missing_lineage.erase(lineage_begin, lineage_end - lineage_begin);
    require(!project::ProjectDocument::deserialize(missing_lineage),
        "schema 23 rejects a missing sign-text lineage table");

    std::string missing_receipts = serialized;
    const auto receipt_begin = missing_receipts.find("SIGN_TEXT_INSTANCE_RECEIPTS ");
    const auto receipt_end = missing_receipts.find("SOURCE_ASSET_REGISTRY_HEX ", receipt_begin);
    require(receipt_begin != std::string::npos && receipt_end != std::string::npos,
        "schema 23 frames the generated sign receipt table");
    missing_receipts.erase(receipt_begin, receipt_end - receipt_begin);
    require(!project::ProjectDocument::deserialize(missing_receipts),
        "schema 23 rejects missing sign-text instance provenance");

    std::string legacy_v22 = project::ProjectDocument{}.serialize();
    const auto legacy_mesh_patches = legacy_v22.find("MESH_PATCHES 0\n");
    require(legacy_mesh_patches != std::string::npos,
        "schema v22 fixture removes the v29 persistent patch table");
    legacy_v22.erase(legacy_mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const auto legacy_source_begin = legacy_v22.find("SIGN_TEXT_SOURCES ");
    const auto legacy_source_end = legacy_v22.find("SPATIAL_FIELDS ", legacy_source_begin);
    legacy_v22.erase(legacy_source_begin, legacy_source_end - legacy_source_begin);
    const auto legacy_lineage_begin = legacy_v22.find("SIGN_TEXT_SOURCE_LINEAGE ");
    const auto legacy_lineage_end = legacy_v22.find(
        "SIGN_TEXT_INSTANCE_RECEIPTS ", legacy_lineage_begin);
    legacy_v22.erase(legacy_lineage_begin, legacy_lineage_end - legacy_lineage_begin);
    const auto legacy_receipt_begin = legacy_v22.find("SIGN_TEXT_INSTANCE_RECEIPTS ");
    const auto legacy_receipt_end = legacy_v22.find(
        "SOURCE_ASSET_REGISTRY_HEX ", legacy_receipt_begin);
    legacy_v22.erase(legacy_receipt_begin, legacy_receipt_end - legacy_receipt_begin);
    const auto biological_v22 = legacy_v22.find("BIOLOGICAL_AUTHORING_SOURCES ");
    const auto end_v22 = legacy_v22.find("END\n", biological_v22);
    legacy_v22.erase(biological_v22, end_v22 - biological_v22);
    legacy_v22.replace(0U, std::string("CARTOGRAPHER_PROJECT 26\n").size(),
        "CARTOGRAPHER_PROJECT 22\n");
    const auto migrated = project::ProjectDocument::deserialize(legacy_v22);
    require(migrated && migrated.value().sign_text_sources().empty() &&
        migrated.value().sign_text_instance_receipts().empty(),
        "schema 22 migrates without fabricated sign-text source or instance records");
}

} // namespace

int main() {
    transaction_persistence_and_monotonic_source_lineage();
    schema_tables_are_mandatory_and_legacy_v22_does_not_invent_signs();
    std::cout << "All sign-text project tests passed.\n";
    return EXIT_SUCCESS;
}
