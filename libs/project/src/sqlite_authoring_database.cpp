#include <carto/project/sqlite_authoring_database.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <array>
#if defined(CARTO_SQLITE_TEST_FAULT_INJECTION)
#include <cstdlib>
#include <fstream>
#include <iterator>
#endif
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#if defined(CARTO_USE_WINSQLITE3)
#include <winsqlite3.h>
#else
#include <sqlite3.h>
#endif

namespace carto::project {
namespace {

constexpr std::string_view kMigrationTableSql =
    "CREATE TABLE schema_migrations("
    "version INTEGER NOT NULL PRIMARY KEY CHECK(version > 0),"
    "migration_id TEXT NOT NULL UNIQUE,"
    "checksum TEXT NOT NULL CHECK(length(checksum) = 64),"
    "applied_at_utc TEXT NOT NULL CHECK(substr(applied_at_utc, -1, 1) = 'Z')"
    ");";
constexpr std::string_view kInitialMigrationId = "001-initial-migration-ledger";
constexpr std::string_view kProjectDocumentMigrationId = "002-project-document-snapshot";
constexpr std::string_view kProjectDocumentSnapshotSql =
    "CREATE TABLE project_document_snapshot("
    "singleton INTEGER NOT NULL PRIMARY KEY CHECK(singleton = 1),"
    "source_sha256 TEXT NOT NULL CHECK(length(source_sha256) = 64),"
    "document_bytes BLOB NOT NULL CHECK(length(document_bytes) > 0)"
    ");";
constexpr int kMaxProjectSnapshotBytes = 128 * 1024 * 1024;
#if defined(CARTO_SQLITE_TEST_FAULT_INJECTION)
bool g_crash_before_project_snapshot_commit = false;
#endif

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic database_error(sqlite3* database, std::string message) {
    if (database != nullptr) {
        const char* detail = sqlite3_errmsg(database);
        if (detail != nullptr && *detail != '\0') {
            message += ": ";
            message += detail;
        }
    }
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

class ConnectionGuard final {
public:
    explicit ConnectionGuard(sqlite3* database) noexcept : database_(database) {}
    ~ConnectionGuard() {
        if (database_ != nullptr) sqlite3_close_v2(database_);
    }
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    [[nodiscard]] sqlite3* get() const noexcept { return database_; }
    [[nodiscard]] sqlite3* release() noexcept {
        sqlite3* result = database_;
        database_ = nullptr;
        return result;
    }

private:
    sqlite3* database_ = nullptr;
};

class Statement final {
public:
    explicit Statement(sqlite3_stmt* statement = nullptr) noexcept
        : statement_(statement) {}
    ~Statement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept
        : statement_(std::exchange(other.statement_, nullptr)) {}
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            if (statement_ != nullptr) sqlite3_finalize(statement_);
            statement_ = std::exchange(other.statement_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

core::Result<Statement> prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return core::Result<Statement>::failure(database_error(
            database, "unable to prepare authoring database statement"));
    }
    return core::Result<Statement>::success(Statement(statement));
}

core::Result<std::int64_t> read_integer(sqlite3* database, const char* sql) {
    auto statement = prepare(database, sql);
    if (!statement) return core::Result<std::int64_t>::failure(statement.error());
    const int step = sqlite3_step(statement.value().get());
    if (step != SQLITE_ROW) {
        return core::Result<std::int64_t>::failure(database_error(
            database, "authoring database integer query returned no row"));
    }
    return core::Result<std::int64_t>::success(
        sqlite3_column_int64(statement.value().get(), 0));
}

core::Result<std::string> read_text(sqlite3* database, const char* sql) {
    auto statement = prepare(database, sql);
    if (!statement) return core::Result<std::string>::failure(statement.error());
    const int step = sqlite3_step(statement.value().get());
    if (step != SQLITE_ROW) {
        return core::Result<std::string>::failure(database_error(
            database, "authoring database text query returned no row"));
    }
    const auto* value = sqlite3_column_text(statement.value().get(), 0);
    if (value == nullptr) {
        return core::Result<std::string>::failure(invalid(
            "authoring database text query returned NULL"));
    }
    return core::Result<std::string>::success(
        reinterpret_cast<const char*>(value));
}

core::Result<void> execute(sqlite3* database, const char* sql) {
    char* detail = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &detail);
    if (result == SQLITE_OK) return core::Result<void>::success();
    std::string message = "authoring database statement failed";
    if (detail != nullptr && *detail != '\0') {
        message += ": ";
        message += detail;
    } else {
        message += ": ";
        message += sqlite3_errmsg(database);
    }
    sqlite3_free(detail);
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::io_error, std::move(message)));
}

std::string initial_migration_checksum() {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(kMigrationTableSql.data());
    return assets::sha256(std::span<const std::uint8_t>(
        bytes, kMigrationTableSql.size())).hex();
}

std::string project_document_migration_checksum() {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(
        kProjectDocumentSnapshotSql.data());
    return assets::sha256(std::span<const std::uint8_t>(
        bytes, kProjectDocumentSnapshotSql.size())).hex();
}

core::Result<std::string> set_and_verify_wal(sqlite3* database) {
    auto statement = prepare(database, "PRAGMA journal_mode=WAL;");
    if (!statement) return core::Result<std::string>::failure(statement.error());
    if (sqlite3_step(statement.value().get()) != SQLITE_ROW) {
        return core::Result<std::string>::failure(database_error(
            database, "unable to enable SQLite WAL mode"));
    }
    const auto* value = sqlite3_column_text(statement.value().get(), 0);
    const std::string journal_mode = value == nullptr
        ? std::string{} : reinterpret_cast<const char*>(value);
    if (journal_mode != "wal") {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "SQLite provider did not enable WAL mode"));
    }
    return core::Result<std::string>::success(journal_mode);
}

core::Result<std::vector<SqliteMigrationRecord>> read_migrations(sqlite3* database) {
    auto statement = prepare(database,
        "SELECT version, migration_id, checksum, applied_at_utc "
        "FROM schema_migrations ORDER BY version;");
    if (!statement) {
        return core::Result<std::vector<SqliteMigrationRecord>>::failure(
            invalid("authoring database migration ledger is missing or malformed"));
    }
    std::vector<SqliteMigrationRecord> records;
    for (;;) {
        const int step = sqlite3_step(statement.value().get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return core::Result<std::vector<SqliteMigrationRecord>>::failure(
                database_error(database, "unable to read authoring migration ledger"));
        }
        const auto version = sqlite3_column_int64(statement.value().get(), 0);
        const auto* migration_id = sqlite3_column_text(statement.value().get(), 1);
        const auto* checksum = sqlite3_column_text(statement.value().get(), 2);
        const auto* applied_at = sqlite3_column_text(statement.value().get(), 3);
        if (version < 0 || version > std::numeric_limits<std::uint32_t>::max() ||
            migration_id == nullptr || checksum == nullptr || applied_at == nullptr) {
            return core::Result<std::vector<SqliteMigrationRecord>>::failure(
                invalid("authoring database migration record is malformed"));
        }
        records.push_back({
            static_cast<std::uint32_t>(version),
            reinterpret_cast<const char*>(migration_id),
            reinterpret_cast<const char*>(checksum),
            reinterpret_cast<const char*>(applied_at)});
    }
    return core::Result<std::vector<SqliteMigrationRecord>>::success(std::move(records));
}

core::Result<void> validate_schema(sqlite3* database) {
    const auto application_id = read_integer(database, "PRAGMA application_id;");
    if (!application_id) return core::Result<void>::failure(application_id.error());
    if (application_id.value() != SqliteAuthoringDatabase::kApplicationId) {
        return core::Result<void>::failure(invalid(
            "authoring database application id is not Cartographer"));
    }

    const auto schema_version = read_integer(database, "PRAGMA user_version;");
    if (!schema_version) return core::Result<void>::failure(schema_version.error());
    if (schema_version.value() < 1 ||
        schema_version.value() > SqliteAuthoringDatabase::kCurrentSchemaVersion) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "authoring database schema version is unsupported"));
    }

    auto columns = prepare(database, "PRAGMA table_info(schema_migrations);");
    if (!columns) return core::Result<void>::failure(invalid(
        "authoring database migration ledger schema is missing"));
    constexpr std::array<std::string_view, 4> expected_names{
        "version", "migration_id", "checksum", "applied_at_utc"};
    constexpr std::array<std::string_view, 4> expected_types{
        "INTEGER", "TEXT", "TEXT", "TEXT"};
    std::size_t column_index = 0U;
    for (;;) {
        const int step = sqlite3_step(columns.value().get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW || column_index >= expected_names.size()) {
            return core::Result<void>::failure(invalid(
                "authoring database migration ledger columns are invalid"));
        }
        const auto* name = sqlite3_column_text(columns.value().get(), 1);
        const auto* type = sqlite3_column_text(columns.value().get(), 2);
        const int not_null = sqlite3_column_int(columns.value().get(), 3);
        const int primary_key = sqlite3_column_int(columns.value().get(), 5);
        if (name == nullptr || type == nullptr ||
            reinterpret_cast<const char*>(name) != expected_names[column_index] ||
            reinterpret_cast<const char*>(type) != expected_types[column_index] ||
            not_null != 1 || primary_key != (column_index == 0U ? 1 : 0)) {
            return core::Result<void>::failure(invalid(
                "authoring database migration ledger columns are invalid"));
        }
        ++column_index;
    }
    if (column_index != expected_names.size()) {
        return core::Result<void>::failure(invalid(
            "authoring database migration ledger has missing columns"));
    }

    const auto migrations = read_migrations(database);
    if (!migrations) return core::Result<void>::failure(migrations.error());
    if (migrations.value().size() != static_cast<std::size_t>(schema_version.value())) {
        return core::Result<void>::failure(invalid(
            "authoring database migration ledger has an unexpected record count"));
    }
    for (std::size_t index = 0U; index < migrations.value().size(); ++index) {
        const auto& record = migrations.value()[index];
        const auto expected_id = index == 0U
            ? kInitialMigrationId : kProjectDocumentMigrationId;
        const auto expected_checksum = index == 0U
            ? initial_migration_checksum() : project_document_migration_checksum();
        if (record.version != index + 1U || record.migration_id != expected_id ||
            record.checksum != expected_checksum || record.applied_at_utc.size() < 20U ||
            record.applied_at_utc.back() != 'Z') {
            return core::Result<void>::failure(invalid(
                "authoring database migration ledger does not match the known schema"));
        }
    }

    if (schema_version.value() >= 2) {
        auto snapshot_columns = prepare(database,
            "PRAGMA table_info(project_document_snapshot);");
        if (!snapshot_columns) return core::Result<void>::failure(invalid(
            "authoring database project snapshot table is missing"));
        constexpr std::array<std::string_view, 3> snapshot_names{
            "singleton", "source_sha256", "document_bytes"};
        constexpr std::array<std::string_view, 3> snapshot_types{
            "INTEGER", "TEXT", "BLOB"};
        std::size_t snapshot_index = 0U;
        for (;;) {
            const int step = sqlite3_step(snapshot_columns.value().get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW || snapshot_index >= snapshot_names.size()) {
                return core::Result<void>::failure(invalid(
                    "authoring database project snapshot table columns are invalid"));
            }
            const auto* name = sqlite3_column_text(snapshot_columns.value().get(), 1);
            const auto* type = sqlite3_column_text(snapshot_columns.value().get(), 2);
            const int not_null = sqlite3_column_int(snapshot_columns.value().get(), 3);
            const int primary_key = sqlite3_column_int(snapshot_columns.value().get(), 5);
            if (name == nullptr || type == nullptr ||
                reinterpret_cast<const char*>(name) != snapshot_names[snapshot_index] ||
                reinterpret_cast<const char*>(type) != snapshot_types[snapshot_index] ||
                not_null != 1 || primary_key != (snapshot_index == 0U ? 1 : 0)) {
                return core::Result<void>::failure(invalid(
                    "authoring database project snapshot table columns are invalid"));
            }
            ++snapshot_index;
        }
        if (snapshot_index != snapshot_names.size()) {
            return core::Result<void>::failure(invalid(
                "authoring database project snapshot table has missing columns"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> validate_project_document_snapshot(sqlite3* database) {
    const auto count = read_integer(database,
        "SELECT count(*) FROM project_document_snapshot;");
    if (!count) return core::Result<void>::failure(count.error());
    if (count.value() == 0) return core::Result<void>::success();
    if (count.value() != 1) return core::Result<void>::failure(invalid(
        "authoring database contains an unexpected project snapshot count"));

    auto statement = prepare(database,
        "SELECT source_sha256, document_bytes FROM project_document_snapshot "
        "WHERE singleton=1;");
    if (!statement) return core::Result<void>::failure(statement.error());
    if (sqlite3_step(statement.value().get()) != SQLITE_ROW) {
        return core::Result<void>::failure(database_error(
            database, "authoring database project snapshot row is missing"));
    }
    const auto* digest_text = sqlite3_column_text(statement.value().get(), 0);
    const auto* bytes = static_cast<const std::uint8_t*>(
        sqlite3_column_blob(statement.value().get(), 1));
    const int byte_count = sqlite3_column_bytes(statement.value().get(), 1);
    const int digest_byte_count = sqlite3_column_bytes(statement.value().get(), 0);
    if (digest_text == nullptr || bytes == nullptr || byte_count <= 0 ||
        byte_count > kMaxProjectSnapshotBytes || digest_byte_count != 64) {
        return core::Result<void>::failure(invalid(
            "authoring database project snapshot row is malformed"));
    }
    const std::string serialized(reinterpret_cast<const char*>(bytes),
        static_cast<std::size_t>(byte_count));
    const std::string stored_digest(reinterpret_cast<const char*>(digest_text),
        static_cast<std::size_t>(digest_byte_count));
    if (stored_digest != assets::sha256(std::span<const std::uint8_t>(bytes,
            static_cast<std::size_t>(byte_count))).hex()) {
        return core::Result<void>::failure(invalid(
            "authoring database project snapshot digest does not match its bytes"));
    }
    const auto document = ProjectDocument::deserialize(serialized);
    if (!document) return core::Result<void>::failure(document.error().with_context(
        "authoring database project snapshot"));
    return core::Result<void>::success();
}

core::Result<void> initialize_initial_schema(
    sqlite3* database,
    bool crash_before_commit = false) {
    if (auto result = execute(database, "BEGIN IMMEDIATE;"); !result) return result;
    const auto rollback = [database](core::Diagnostic diagnostic) {
        static_cast<void>(execute(database, "ROLLBACK;"));
        return core::Result<void>::failure(std::move(diagnostic));
    };
    if (auto result = execute(database, kMigrationTableSql.data()); !result) {
        return rollback(result.error());
    }
    auto statement = prepare(database,
        "INSERT INTO schema_migrations(version, migration_id, checksum, applied_at_utc) "
        "VALUES(1, ?1, ?2, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));");
    if (!statement) return rollback(statement.error());
    const std::string checksum = initial_migration_checksum();
    if (sqlite3_bind_text(statement.value().get(), 1, kInitialMigrationId.data(),
            static_cast<int>(kInitialMigrationId.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement.value().get(), 2, checksum.data(),
            static_cast<int>(checksum.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(statement.value().get()) != SQLITE_DONE) {
        return rollback(database_error(database,
            "unable to record initial authoring database migration"));
    }
    if (auto result = execute(database,
            "PRAGMA application_id=1128354388; PRAGMA user_version=1;"); !result) {
        return rollback(result.error());
    }
    if (crash_before_commit) {
#if defined(CARTO_SQLITE_TEST_FAULT_INJECTION)
        std::_Exit(86);
#else
        return rollback(core::Diagnostic(core::ErrorCode::unsupported,
            "SQLite migration crash injection is available only in test builds"));
#endif
    }
    if (auto result = execute(database, "COMMIT;"); !result) return rollback(result.error());
    return validate_schema(database);
}

core::Result<void> migrate_project_document_schema(sqlite3* database) {
    if (auto result = execute(database, "BEGIN IMMEDIATE;"); !result) return result;
    const auto rollback = [database](core::Diagnostic diagnostic) {
        static_cast<void>(execute(database, "ROLLBACK;"));
        return core::Result<void>::failure(std::move(diagnostic));
    };
    if (auto result = execute(database, kProjectDocumentSnapshotSql.data()); !result) {
        return rollback(result.error());
    }
    {
        auto statement = prepare(database,
            "INSERT INTO schema_migrations(version, migration_id, checksum, applied_at_utc) "
            "VALUES(2, ?1, ?2, strftime('%Y-%m-%dT%H:%M:%fZ', 'now')); ");
        if (!statement) return rollback(statement.error());
        const std::string checksum = project_document_migration_checksum();
        if (sqlite3_bind_text(statement.value().get(), 1,
                kProjectDocumentMigrationId.data(),
                static_cast<int>(kProjectDocumentMigrationId.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement.value().get(), 2, checksum.data(),
                static_cast<int>(checksum.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(statement.value().get()) != SQLITE_DONE) {
            return rollback(database_error(database,
                "unable to record project document snapshot migration"));
        }
    }
    if (auto result = execute(database, "PRAGMA user_version=2;"); !result) {
        return rollback(result.error());
    }
    if (auto result = execute(database, "COMMIT;"); !result) return rollback(result.error());
    return validate_schema(database);
}

core::Result<void> validate_or_initialize(sqlite3* database) {
    const auto application_id = read_integer(database, "PRAGMA application_id;");
    if (!application_id) return core::Result<void>::failure(application_id.error());
    const auto schema_version = read_integer(database, "PRAGMA user_version;");
    if (!schema_version) return core::Result<void>::failure(schema_version.error());
    const auto object_count = read_integer(database,
        "SELECT count(*) FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' AND type IN ('table','index','view','trigger');");
    if (!object_count) return core::Result<void>::failure(object_count.error());

    if (application_id.value() == 0 && schema_version.value() == 0 &&
        object_count.value() == 0) {
        if (auto wal = set_and_verify_wal(database); !wal) {
            return core::Result<void>::failure(wal.error());
        }
        if (auto result = initialize_initial_schema(database); !result) return result;
    }
    if (schema_version.value() > SqliteAuthoringDatabase::kCurrentSchemaVersion) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "authoring database was created by a newer schema version"));
    }
    if (auto result = validate_schema(database); !result) return result;
    if (auto wal = set_and_verify_wal(database); !wal) {
        return core::Result<void>::failure(wal.error());
    }
    const auto current_version = read_integer(database, "PRAGMA user_version;");
    if (!current_version) return core::Result<void>::failure(current_version.error());
    if (current_version.value() == 1) {
        if (auto result = migrate_project_document_schema(database); !result) return result;
    }
    if (auto result = validate_project_document_snapshot(database); !result) return result;
    return core::Result<void>::success();
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

} // namespace

#if defined(CARTO_SQLITE_TEST_FAULT_INJECTION)
namespace testing {

int crash_during_initial_migration(const std::filesystem::path& path) noexcept {
    if (path.empty() || path.filename().empty()) return 80;
    const std::string encoded_path = utf8_path(path);
    sqlite3* raw_connection = nullptr;
    const int open_result = sqlite3_open_v2(encoded_path.c_str(), &raw_connection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    ConnectionGuard connection(raw_connection);
    if (open_result != SQLITE_OK || connection.get() == nullptr) return 81;
    if (sqlite3_busy_timeout(connection.get(), 2500) != SQLITE_OK) return 82;
    if (!set_and_verify_wal(connection.get())) return 83;
    const auto result = initialize_initial_schema(connection.get(), true);
    return result ? 84 : 85;
}

int corrupt_project_snapshot_digest_for_test(
    const std::filesystem::path& path) noexcept {
    if (path.empty() || path.filename().empty()) return 90;
    const std::string encoded_path = utf8_path(path);
    sqlite3* raw_connection = nullptr;
    const int open_result = sqlite3_open_v2(encoded_path.c_str(), &raw_connection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    ConnectionGuard connection(raw_connection);
    if (open_result != SQLITE_OK || connection.get() == nullptr) return 91;
    const auto changed = execute(connection.get(),
        "UPDATE project_document_snapshot "
        "SET source_sha256='0000000000000000000000000000000000000000000000000000000000000000' "
        "WHERE singleton=1;");
    if (!changed) return 92;
    return sqlite3_changes(connection.get()) == 1 ? 0 : 93;
}

int create_schema_v1_database_for_test(const std::filesystem::path& path) noexcept {
    if (path.empty() || path.filename().empty()) return 100;
    const std::string encoded_path = utf8_path(path);
    sqlite3* raw_connection = nullptr;
    const int open_result = sqlite3_open_v2(encoded_path.c_str(), &raw_connection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    ConnectionGuard connection(raw_connection);
    if (open_result != SQLITE_OK || connection.get() == nullptr) return 101;
    if (sqlite3_busy_timeout(connection.get(), 2500) != SQLITE_OK) return 102;
    if (!set_and_verify_wal(connection.get())) return 103;
    return initialize_initial_schema(connection.get()) ? 0 : 104;
}

int crash_during_project_document_import_for_test(
    const std::filesystem::path& database_path,
    const std::filesystem::path& source_document_path) noexcept {
    auto database = SqliteAuthoringDatabase::open(database_path);
    if (!database) return 110;
    std::ifstream input(source_document_path, std::ios::binary);
    if (!input) return 111;
    const std::string serialized{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return 112;
    g_crash_before_project_snapshot_commit = true;
    const auto result = database.value().import_project_document(serialized);
    return result ? 113 : 114;
}

} // namespace testing
#endif

struct SqliteAuthoringDatabase::Impl {
    explicit Impl(sqlite3* connection_value) noexcept : connection(connection_value) {}
    ~Impl() {
        if (connection != nullptr) sqlite3_close_v2(connection);
    }
    sqlite3* connection = nullptr;
};

SqliteAuthoringDatabase::SqliteAuthoringDatabase(
    std::unique_ptr<Impl> implementation,
    std::filesystem::path path) noexcept
    : implementation_(std::move(implementation)), path_(std::move(path)) {}

SqliteAuthoringDatabase::~SqliteAuthoringDatabase() = default;
SqliteAuthoringDatabase::SqliteAuthoringDatabase(SqliteAuthoringDatabase&& other) noexcept = default;
SqliteAuthoringDatabase& SqliteAuthoringDatabase::operator=(
    SqliteAuthoringDatabase&& other) noexcept = default;

core::Result<SqliteAuthoringDatabase> SqliteAuthoringDatabase::open(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<SqliteAuthoringDatabase>::failure(invalid(
            "authoring database path must name a file"));
    }
    const std::string encoded_path = utf8_path(path);
    sqlite3* raw_connection = nullptr;
    const int open_result = sqlite3_open_v2(encoded_path.c_str(), &raw_connection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    ConnectionGuard connection(raw_connection);
    if (open_result != SQLITE_OK) {
        return core::Result<SqliteAuthoringDatabase>::failure(database_error(
            connection.get(), "unable to open authoring database"));
    }
    if (sqlite3_busy_timeout(connection.get(), 2500) != SQLITE_OK) {
        return core::Result<SqliteAuthoringDatabase>::failure(database_error(
            connection.get(), "unable to configure authoring database busy timeout"));
    }
    if (auto result = validate_or_initialize(connection.get()); !result) {
        return core::Result<SqliteAuthoringDatabase>::failure(
            result.error().with_context("authoring database admission"));
    }
    if (auto result = execute(connection.get(), "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
        !result) {
        return core::Result<SqliteAuthoringDatabase>::failure(result.error());
    }
    auto implementation = std::make_unique<Impl>(connection.release());
    return core::Result<SqliteAuthoringDatabase>::success(
        SqliteAuthoringDatabase(std::move(implementation), path));
}

core::Result<SqliteIntegrityReport> SqliteAuthoringDatabase::scan_integrity(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<SqliteIntegrityReport>::failure(invalid(
            "authoring database path must name a file"));
    }
    const std::string encoded_path = utf8_path(path);
    sqlite3* raw_connection = nullptr;
    const int open_result = sqlite3_open_v2(encoded_path.c_str(), &raw_connection,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    ConnectionGuard connection(raw_connection);
    if (open_result != SQLITE_OK) {
        return core::Result<SqliteIntegrityReport>::failure(database_error(
            connection.get(), "unable to open authoring database for read-only integrity scan"));
    }

    SqliteIntegrityReport report;
    auto statement = prepare(connection.get(), "PRAGMA integrity_check;");
    if (!statement) {
        return core::Result<SqliteIntegrityReport>::failure(statement.error());
    }
    bool saw_integrity_row = false;
    for (;;) {
        const int step = sqlite3_step(statement.value().get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return core::Result<SqliteIntegrityReport>::failure(database_error(
                connection.get(), "unable to complete SQLite integrity scan"));
        }
        saw_integrity_row = true;
        const auto* value = sqlite3_column_text(statement.value().get(), 0);
        if (value == nullptr) {
            report.sqlite_issues.emplace_back(
                "SQLite integrity scan returned a NULL diagnostic");
            continue;
        }
        const std::string diagnostic(reinterpret_cast<const char*>(value));
        if (diagnostic == "ok") {
            report.sqlite_integrity_check_passed = true;
        } else {
            report.sqlite_issues.push_back(diagnostic);
        }
    }
    if (!saw_integrity_row) {
        report.sqlite_issues.emplace_back(
            "SQLite integrity scan returned no diagnostic rows");
    }

    if (auto schema = validate_schema(connection.get()); !schema) {
        report.migration_ledger_issues.push_back(schema.error().message);
    } else {
        const auto schema_version = read_integer(connection.get(), "PRAGMA user_version;");
        if (!schema_version || schema_version.value() >= 2) {
            if (auto snapshot = validate_project_document_snapshot(connection.get()); !snapshot) {
                report.authoring_issues.push_back(snapshot.error().message);
            }
        }
    }
    return core::Result<SqliteIntegrityReport>::success(std::move(report));
}

core::Result<SqliteDatabaseInfo> SqliteAuthoringDatabase::inspect() const {
    if (!is_open()) {
        return core::Result<SqliteDatabaseInfo>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "authoring database is closed"));
    }
    sqlite3* database = implementation_->connection;
    const auto journal_mode = read_text(database, "PRAGMA journal_mode;");
    if (!journal_mode) return core::Result<SqliteDatabaseInfo>::failure(journal_mode.error());
    const auto schema_version = read_integer(database, "PRAGMA user_version;");
    if (!schema_version) return core::Result<SqliteDatabaseInfo>::failure(schema_version.error());
    if (schema_version.value() < 0 ||
        schema_version.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<SqliteDatabaseInfo>::failure(invalid(
            "authoring database schema version is out of range"));
    }
    const auto migrations = read_migrations(database);
    if (!migrations) return core::Result<SqliteDatabaseInfo>::failure(migrations.error());
    return core::Result<SqliteDatabaseInfo>::success({
        static_cast<std::uint32_t>(schema_version.value()),
        journal_mode.value(), migrations.value()});
}

core::Result<void> SqliteAuthoringDatabase::import_project_document(
    std::string_view serialized_document) {
    if (!is_open()) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_state, "authoring database is closed"));
    const auto document = ProjectDocument::deserialize(serialized_document);
    if (!document) return core::Result<void>::failure(document.error().with_context(
        "flat project migration source"));
    sqlite3* database = implementation_->connection;
    if (auto schema = validate_schema(database); !schema) {
        return core::Result<void>::failure(schema.error());
    }
    if (auto existing = validate_project_document_snapshot(database); !existing) {
        return core::Result<void>::failure(existing.error());
    }
    if (auto result = execute(database, "BEGIN IMMEDIATE;"); !result) return result;
    const auto rollback = [database](core::Diagnostic diagnostic) {
        static_cast<void>(execute(database, "ROLLBACK;"));
        return core::Result<void>::failure(std::move(diagnostic));
    };
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_document.data());
    const std::string digest = assets::sha256(std::span<const std::uint8_t>(
        bytes, serialized_document.size())).hex();
    {
        auto statement = prepare(database,
            "INSERT INTO project_document_snapshot(singleton, source_sha256, document_bytes) "
            "VALUES(1, ?1, ?2) ON CONFLICT(singleton) DO NOTHING;");
        if (!statement) return rollback(statement.error());
        if (sqlite3_bind_text(statement.value().get(), 1, digest.data(),
                static_cast<int>(digest.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_blob(statement.value().get(), 2, serialized_document.data(),
                static_cast<int>(serialized_document.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(statement.value().get()) != SQLITE_DONE) {
            return rollback(database_error(database,
                "unable to import project document snapshot"));
        }
    }
    if (sqlite3_changes(database) != 1) {
        return rollback(invalid(
            "authoring database already contains a project document snapshot"));
    }
#if defined(CARTO_SQLITE_TEST_FAULT_INJECTION)
    if (std::exchange(g_crash_before_project_snapshot_commit, false)) {
        std::_Exit(87);
    }
#endif
    if (auto result = execute(database, "COMMIT;"); !result) return rollback(result.error());
    return validate_project_document_snapshot(database);
}

core::Result<std::string> SqliteAuthoringDatabase::export_project_document() const {
    if (!is_open()) return core::Result<std::string>::failure(core::Diagnostic(
        core::ErrorCode::invalid_state, "authoring database is closed"));
    sqlite3* database = implementation_->connection;
    if (auto schema = validate_schema(database); !schema) {
        return core::Result<std::string>::failure(schema.error());
    }
    if (auto snapshot = validate_project_document_snapshot(database); !snapshot) {
        return core::Result<std::string>::failure(snapshot.error());
    }
    auto statement = prepare(database,
        "SELECT document_bytes FROM project_document_snapshot WHERE singleton=1;");
    if (!statement) return core::Result<std::string>::failure(statement.error());
    const int step = sqlite3_step(statement.value().get());
    if (step == SQLITE_DONE) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "authoring database has no migrated project document snapshot"));
    }
    if (step != SQLITE_ROW) {
        return core::Result<std::string>::failure(database_error(
            database, "unable to export project document snapshot"));
    }
    const auto* bytes = static_cast<const char*>(
        sqlite3_column_blob(statement.value().get(), 0));
    const int byte_count = sqlite3_column_bytes(statement.value().get(), 0);
    if (bytes == nullptr || byte_count <= 0 || byte_count > kMaxProjectSnapshotBytes) {
        return core::Result<std::string>::failure(invalid(
            "authoring database project snapshot row is malformed"));
    }
    return core::Result<std::string>::success(
        std::string(bytes, static_cast<std::size_t>(byte_count)));
}

core::Result<void> SqliteAuthoringDatabase::close() {
    if (!is_open()) return core::Result<void>::success();
    sqlite3* database = implementation_->connection;
    const int result = sqlite3_close(database);
    if (result != SQLITE_OK) {
        return core::Result<void>::failure(database_error(
            database, "unable to close authoring database"));
    }
    implementation_->connection = nullptr;
    return core::Result<void>::success();
}

bool SqliteAuthoringDatabase::is_open() const noexcept {
    return implementation_ != nullptr && implementation_->connection != nullptr;
}

} // namespace carto::project
