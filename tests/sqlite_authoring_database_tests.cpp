#include <carto/project/sqlite_authoring_database.hpp>
#include <carto/project/package.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>
#include "sqlite_authoring_database_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#endif

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
            ("cartographer-sqlite-authoring-" + std::to_string(stamp));
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

void fresh_database_uses_wal_and_records_the_initial_migration() {
    TempDirectory directory;
    const auto path = directory.path() / "document.db";
    require(!std::filesystem::exists(path), "fixture starts without a database file");

    auto opened = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(opened), "fresh authoring database opens");
    require(std::filesystem::is_regular_file(path) &&
            std::filesystem::file_size(path) > 0U,
        "fresh open creates a nonempty database file");

    std::array<char, 16U> magic{};
    std::ifstream database_file(path, std::ios::binary);
    database_file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    require(database_file.gcount() == static_cast<std::streamsize>(magic.size()) &&
            std::string_view(magic.data(), magic.size()) ==
                std::string_view("SQLite format 3\0", 16U),
        "fresh open creates a real SQLite database rather than a placeholder");

    const auto inspected = opened.value().inspect();
    require(static_cast<bool>(inspected), "open database can be inspected");
    require(inspected.value().schema_version ==
            project::SqliteAuthoringDatabase::kCurrentSchemaVersion,
        "fresh database records the current schema version");
    require(inspected.value().journal_mode == "wal",
        "fresh database is in WAL journal mode");
    require(inspected.value().migrations.size() == 2U,
        "fresh database records both schema migrations");
    const auto first_migration = inspected.value().migrations.front();
    require(first_migration.version == 1U &&
            first_migration.migration_id == "001-initial-migration-ledger" &&
            first_migration.checksum.size() == 64U &&
            first_migration.applied_at_utc.size() >= 20U &&
            first_migration.applied_at_utc.back() == 'Z',
        "initial migration record has a stable identity, checksum, and explicit UTC timestamp");
    const auto snapshot_migration = inspected.value().migrations.back();
    require(snapshot_migration.version == 2U &&
            snapshot_migration.migration_id == "002-project-document-snapshot" &&
            snapshot_migration.checksum.size() == 64U &&
            snapshot_migration.applied_at_utc.size() >= 20U &&
            snapshot_migration.applied_at_utc.back() == 'Z',
        "project snapshot migration has a stable identity, checksum, and UTC timestamp");

    require(static_cast<bool>(opened.value().close()), "database closes explicitly");
    require(!opened.value().is_open(), "closed database no longer reports an open handle");
    const auto closed_inspection = opened.value().inspect();
    require(!closed_inspection &&
            closed_inspection.error().code == core::ErrorCode::invalid_state,
        "closed database cannot be inspected through a stale handle");

    auto reopened = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(reopened), "database reopens after close");
    const auto reopened_info = reopened.value().inspect();
    require(static_cast<bool>(reopened_info), "reopened database can be inspected");
    require(reopened_info.value().journal_mode == "wal" &&
            reopened_info.value().schema_version == inspected.value().schema_version &&
            reopened_info.value().migrations == inspected.value().migrations,
        "WAL mode and migration lineage survive close and reopen without duplicate migration");
    require(static_cast<bool>(reopened.value().close()), "reopened database closes cleanly");
}

void malformed_existing_file_is_refused_without_rewriting_it() {
    TempDirectory directory;
    const auto path = directory.path() / "document.db";
    const std::string contents = "user-owned data that is not a SQLite database\n";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "malformed fixture file opens for setup");
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    const auto rejected = project::SqliteAuthoringDatabase::open(path);
    require(!rejected, "malformed existing database is refused");
    std::ifstream input(path, std::ios::binary);
    const std::string after{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(after == contents,
        "failed admission leaves the existing file byte-for-byte unchanged");
}

void schema_v1_database_migrates_to_project_snapshot_schema() {
    TempDirectory directory;
    const auto path = directory.path() / "legacy-v1-document.db";
    require(project::testing::create_schema_v1_database_for_test(path) == 0,
        "test fixture creates a committed schema-v1 database");
    auto upgraded = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(upgraded), "schema-v1 database upgrades on open");
    const auto info = upgraded.value().inspect();
    require(info && info.value().schema_version ==
                project::SqliteAuthoringDatabase::kCurrentSchemaVersion &&
            info.value().migrations.size() == 2U &&
            info.value().migrations.back().version == 2U,
        "upgrade records schema-v2 snapshot migration exactly once");
    require(static_cast<bool>(upgraded.value().close()),
        "upgraded schema-v1 database closes cleanly");
    const auto integrity = project::SqliteAuthoringDatabase::scan_integrity(path);
    require(integrity && integrity.value().healthy(),
        "upgraded schema-v1 database passes read-only integrity scan");
}

void concurrent_open_reuses_the_validated_migration_ledger() {
    TempDirectory directory;
    const auto path = directory.path() / "document.db";
    auto first = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(first), "first database connection opens");
    auto second = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(second), "second database connection opens in WAL mode");
    const auto first_info = first.value().inspect();
    const auto second_info = second.value().inspect();
    require(first_info && second_info && first_info.value().migrations.size() == 2U &&
            second_info.value().migrations == first_info.value().migrations,
        "concurrent open validates and reuses one migration ledger");
    require(static_cast<bool>(second.value().close()), "second connection closes");
    require(static_cast<bool>(first.value().close()), "first connection closes");
}

std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "database file opens for byte snapshot");
    return std::vector<char>(std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void integrity_scan_reports_ledger_corruption_without_repairing() {
    TempDirectory directory;
    const auto path = directory.path() / "document.db";
    auto opened = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(opened), "database opens before integrity scan");
    require(static_cast<bool>(opened.value().close()), "database closes before byte mutation");

    const auto initial_bytes = read_file_bytes(path);
    const auto clean_scan = project::SqliteAuthoringDatabase::scan_integrity(path);
    require(clean_scan && clean_scan.value().healthy(),
        "read-only scan accepts a clean database and migration ledger");
    require(read_file_bytes(path) == initial_bytes,
        "read-only clean scan leaves the database byte-for-byte unchanged");

    auto corrupted_bytes = initial_bytes;
    constexpr std::string_view migration_id = "001-initial-migration-ledger";
    const auto marker = std::search(corrupted_bytes.begin(), corrupted_bytes.end(),
        migration_id.begin(), migration_id.end());
    require(marker != corrupted_bytes.end(),
        "initial migration row is present in the closed database file");
    const auto checksum_start = static_cast<std::size_t>(marker - corrupted_bytes.begin()) +
        migration_id.size();
    const auto checksum_byte = std::find_if(
        corrupted_bytes.begin() + static_cast<std::ptrdiff_t>(checksum_start),
        corrupted_bytes.end(), [](char byte) { return byte >= '0' && byte <= '9'; });
    require(checksum_byte != corrupted_bytes.end(),
        "migration checksum bytes are present in the database file");
    *checksum_byte = *checksum_byte == '0' ? '1' : '0';
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "corrupted database opens for fixture setup");
        output.write(corrupted_bytes.data(), static_cast<std::streamsize>(corrupted_bytes.size()));
        require(static_cast<bool>(output), "corrupted database fixture is fully written");
    }

    const auto scan = project::SqliteAuthoringDatabase::scan_integrity(path);
    require(static_cast<bool>(scan), "integrity scan reports a corrupt ledger instead of repairing it");
    require(scan.value().sqlite_integrity_check_passed && scan.value().sqlite_issues.empty(),
        "ledger value corruption is distinguished from physical SQLite corruption");
    require(scan.value().migration_ledger_issues.size() == 1U,
        "altered migration checksum is reported as a ledger issue");
    require(read_file_bytes(path) == corrupted_bytes,
        "integrity scan leaves corrupt source bytes unchanged");
}

void integrity_scan_reports_project_snapshot_corruption_without_repairing() {
    TempDirectory directory;
    const auto path = directory.path() / "document.db";
    auto document = project::ProjectDocument::create("snapshot integrity fixture");
    require(static_cast<bool>(document), "snapshot integrity fixture is created");
    const std::string serialized = document.value().serialize();
    auto opened = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(opened), "snapshot integrity database opens");
    require(static_cast<bool>(opened.value().import_project_document(serialized)),
        "valid project snapshot imports before corruption");
    require(static_cast<bool>(opened.value().close()),
        "snapshot integrity database closes before fault injection");
    require(project::testing::corrupt_project_snapshot_digest_for_test(path) == 0,
        "test fault injection corrupts only the stored snapshot digest");

    const auto corrupted_bytes = read_file_bytes(path);
    const auto scan = project::SqliteAuthoringDatabase::scan_integrity(path);
    require(scan && scan.value().sqlite_integrity_check_passed &&
            scan.value().authoring_issues.size() == 1U &&
            !scan.value().healthy(),
        "read-only scan reports snapshot digest corruption as an authoring issue");
    require(read_file_bytes(path) == corrupted_bytes,
        "read-only authoring integrity scan does not repair or rewrite corrupted bytes");
}

#if !defined(_WIN32)
std::string shell_argument(const std::string& value) {
    std::string quoted = "'";
    for (const char byte : value) {
        if (byte == '\'') quoted += "'\\''";
        else quoted.push_back(byte);
    }
    quoted.push_back('\'');
    return quoted;
}
#endif

int run_crashing_child(
    const std::filesystem::path& executable,
    const std::filesystem::path& database) {
#if defined(_WIN32)
    const std::wstring executable_wide = executable.wstring();
    std::wstring command = L"\"" + executable_wide +
        L"\" --crash-initial-migration \"" + database.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable_wide.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0U;
    const bool read_exit_code = wait_result == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return read_exit_code ? static_cast<int>(exit_code) : -1;
#else
    const std::string command = shell_argument(executable.string()) +
        " --crash-initial-migration " + shell_argument(database.string());
    const int status = std::system(command.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

int run_project_snapshot_crash_child(
    const std::filesystem::path& executable,
    const std::filesystem::path& database,
    const std::filesystem::path& source_document) {
#if defined(_WIN32)
    const std::wstring executable_wide = executable.wstring();
    std::wstring command = L"\"" + executable_wide +
        L"\" --crash-project-document-import \"" + database.wstring() +
        L"\" \"" + source_document.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable_wide.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0U;
    const bool read_exit_code = wait_result == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return read_exit_code ? static_cast<int>(exit_code) : -1;
#else
    const std::string command = shell_argument(executable.string()) +
        " --crash-project-document-import " + shell_argument(database.string()) +
        " " + shell_argument(source_document.string());
    const int status = std::system(command.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

void interrupted_initial_migration_recovers_last_committed_state(
    const std::filesystem::path& executable) {
    TempDirectory directory;
    const auto path = directory.path() / "interrupted-document.db";
    require(run_crashing_child(executable, path) == 86,
        "child process terminates after initial migration writes but before COMMIT");

    auto recovered = project::SqliteAuthoringDatabase::open(path);
    require(static_cast<bool>(recovered),
        "WAL recovery admits and completes the previously uncommitted initial migration");
    const auto inspected = recovered.value().inspect();
    require(inspected && inspected.value().schema_version ==
                project::SqliteAuthoringDatabase::kCurrentSchemaVersion &&
            inspected.value().journal_mode == "wal" &&
            inspected.value().migrations.size() == 2U &&
            inspected.value().migrations.front().version == 1U &&
            inspected.value().migrations.back().version ==
                project::SqliteAuthoringDatabase::kCurrentSchemaVersion,
        "recovery publishes a complete migration ledger at the current schema version");
    require(static_cast<bool>(recovered.value().close()),
        "recovered database closes cleanly");
    const auto integrity = project::SqliteAuthoringDatabase::scan_integrity(path);
    require(integrity && integrity.value().healthy(),
        "post-crash recovery leaves SQLite structure and migration ledger healthy");
}

void interrupted_project_snapshot_import_recovers_without_partial_row(
    const std::filesystem::path& executable) {
    TempDirectory directory;
    const auto database_path = directory.path() / "interrupted-snapshot.db";
    const auto source_path = directory.path() / "snapshot-source.carto";
    auto initial = project::SqliteAuthoringDatabase::open(database_path);
    require(static_cast<bool>(initial), "database opens before interrupted snapshot import");
    require(static_cast<bool>(initial.value().close()),
        "database closes before child snapshot import");
    auto document = project::ProjectDocument::create("interrupted import fixture");
    require(static_cast<bool>(document), "interrupted import document is created");
    const std::string source_bytes = document.value().serialize();
    {
        std::ofstream output(source_path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "snapshot source opens for child process");
        output.write(source_bytes.data(), static_cast<std::streamsize>(source_bytes.size()));
        require(static_cast<bool>(output), "snapshot source is written for child process");
    }
    require(run_project_snapshot_crash_child(
                executable, database_path, source_path) == 87,
        "child exits after snapshot INSERT and before WAL COMMIT");

    auto recovered = project::SqliteAuthoringDatabase::open(database_path);
    require(static_cast<bool>(recovered), "database reopens after interrupted snapshot import");
    const auto absent = recovered.value().export_project_document();
    require(!absent && absent.error().code == core::ErrorCode::not_found,
        "WAL recovery discards the uncommitted snapshot row instead of exposing partial data");
    require(static_cast<bool>(recovered.value().import_project_document(source_bytes)),
        "normal snapshot import succeeds after recovery");
    const auto exported = recovered.value().export_project_document();
    require(exported && exported.value() == source_bytes,
        "recovered database exports the exact retried source document");
    require(static_cast<bool>(recovered.value().close()),
        "recovered project database closes cleanly");
    const auto integrity = project::SqliteAuthoringDatabase::scan_integrity(database_path);
    require(integrity && integrity.value().healthy(),
        "post-crash snapshot retry leaves a healthy database");
}

void flat_project_migration_preserves_exact_export_and_rejects_overwrite() {
    TempDirectory directory;
    const auto source = directory.path() / "source.carto";
    const auto destination = directory.path() / "MigratedProject.carto-package";
    auto created = project::ProjectDocument::create("migration fixture");
    require(static_cast<bool>(created), "migration fixture document is created");
    const std::string source_bytes = created.value().serialize();
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "flat source opens for fixture setup");
        output.write(source_bytes.data(), static_cast<std::streamsize>(source_bytes.size()));
        require(static_cast<bool>(output), "flat source is written exactly");
    }

    auto migrated = project::ProjectPackage::migrate_flat_project(source, destination);
    require(static_cast<bool>(migrated), "valid flat project migrates to a package");
    const auto has_database = migrated.value().has_document_database();
    require(has_database && has_database.value(),
        "migration publishes a real document database");
    const auto exported = migrated.value().export_project_document();
    require(exported && exported.value() == source_bytes,
        "package export reproduces source authoring bytes byte-for-byte");
    require(migrated.value().manifest().name == "migration fixture" &&
            migrated.value().manifest().units == "meters" &&
            migrated.value().manifest().up_axis == "y",
        "package metadata records the source name and authoring coordinate convention");
    const auto integrity = project::SqliteAuthoringDatabase::scan_integrity(
        migrated.value().document_database_path());
    require(integrity && integrity.value().healthy(),
        "migrated snapshot passes read-only SQLite and authoring integrity checks");

    auto database = project::SqliteAuthoringDatabase::open(
        migrated.value().document_database_path());
    require(static_cast<bool>(database), "migrated database reopens");
    const auto duplicate = database.value().import_project_document(source_bytes);
    require(!duplicate,
        "insert-only migration refuses to overwrite an existing document snapshot");
    const auto still_exported = database.value().export_project_document();
    require(still_exported && still_exported.value() == source_bytes,
        "rejected duplicate import leaves original authoring bytes unchanged");
    require(static_cast<bool>(database.value().close()), "migrated database closes cleanly");
}

void flat_project_migration_preserves_verified_graph_binding() {
    TempDirectory directory;
    const auto source_root = directory.path() / "LegacyProject";
    const auto source = source_root / "LegacyProject.carto";
    const auto destination = directory.path() / "MigratedGraphProject.carto-package";
    auto source_package = project::ProjectPackage::create(source_root, {
        "legacy-graph", "Untitled", "meters", "y",
    });
    require(static_cast<bool>(source_package), "legacy package fixture is created");

    eval::EvaluationGraph graph;
    require(static_cast<bool>(graph.register_type(eval::NodeTypeDefinition{
        eval::NodeTypeId{"source"},
        {eval::PortDescriptor{eval::PortId{"mesh"}, eval::PortDirection::output,
            eval::ValueKind::editable_mesh}},
    })), "source graph type is registered");
    require(static_cast<bool>(graph.create_node(eval::NodeTypeId{"source"}, "source-v1")),
        "source graph node is created");
    const auto graph_bytes = graph.serialize();
    require(static_cast<bool>(graph_bytes), "source graph serializes");
    const auto stored_graph = source_package.value().store_evaluation_graph(graph);
    require(static_cast<bool>(stored_graph), "legacy package stores its graph blob");

    project::ProjectDocument document;
    journal::Journal journal_file(directory.path() / "legacy-journal" / "journal.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal_file, "migration-test");
    require(static_cast<bool>(transaction), "graph binding transaction begins");
    require(static_cast<bool>(transaction.value().set_evaluation_graph_digest(
        stored_graph.value().digest)), "source document binds exact graph digest");
    require(static_cast<bool>(transaction.value().commit("graph.bind")),
        "source graph binding commits");
    const std::string source_bytes = document.serialize();
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "graph-bound flat source opens");
        output.write(source_bytes.data(), static_cast<std::streamsize>(source_bytes.size()));
        require(static_cast<bool>(output), "graph-bound flat source writes exactly");
    }

    auto migrated = project::ProjectPackage::migrate_flat_project(source, destination);
    if (!migrated) std::cerr << "graph migration diagnostic: "
        << migrated.error().message << '\n';
    require(static_cast<bool>(migrated), "graph-bound project migrates");
    require(migrated.value().manifest().evaluation_graph_blob ==
            std::optional<assets::Sha256Digest>{stored_graph.value().digest},
        "migrated manifest preserves the document-bound graph digest");
    require(static_cast<bool>(migrated.value().validate_document_binding(document)),
        "migrated package and exact source document remain bound");
    const auto loaded_graph = migrated.value().load_evaluation_graph();
    require(static_cast<bool>(loaded_graph), "migrated graph blob verifies and loads");
    const auto loaded_bytes = loaded_graph.value().serialize();
    require(loaded_bytes && loaded_bytes.value() == graph_bytes.value(),
        "migrated package preserves the graph blob bytes");
    const auto exported = migrated.value().export_project_document();
    require(exported && exported.value() == source_bytes,
        "graph-bound project document still exports byte-for-byte");
}

void invalid_flat_project_migration_leaves_no_destination() {
    TempDirectory directory;
    const auto source = directory.path() / "invalid.carto";
    const auto destination = directory.path() / "MustNotExist.carto-package";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "invalid source opens for fixture setup");
        output << "not a Cartographer project";
    }
    const auto rejected = project::ProjectPackage::migrate_flat_project(source, destination);
    require(!rejected && !std::filesystem::exists(destination),
        "malformed source is rejected before package publication");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--crash-initial-migration") {
        return project::testing::crash_during_initial_migration(argv[2]);
    }
    if (argc == 4 && std::string_view(argv[1]) == "--crash-project-document-import") {
        return project::testing::crash_during_project_document_import_for_test(
            argv[2], argv[3]);
    }
    fresh_database_uses_wal_and_records_the_initial_migration();
    malformed_existing_file_is_refused_without_rewriting_it();
    schema_v1_database_migrates_to_project_snapshot_schema();
    concurrent_open_reuses_the_validated_migration_ledger();
    integrity_scan_reports_ledger_corruption_without_repairing();
    integrity_scan_reports_project_snapshot_corruption_without_repairing();
    interrupted_initial_migration_recovers_last_committed_state(
        std::filesystem::absolute(std::filesystem::path(
            argc > 1 ? argv[1] : argv[0])));
    interrupted_project_snapshot_import_recovers_without_partial_row(
        std::filesystem::absolute(std::filesystem::path(
            argc > 1 ? argv[1] : argv[0])));
    flat_project_migration_preserves_exact_export_and_rejects_overwrite();
    flat_project_migration_preserves_verified_graph_binding();
    invalid_flat_project_migration_leaves_no_destination();
    std::cout << "SQLite authoring database checks passed\n";
}
