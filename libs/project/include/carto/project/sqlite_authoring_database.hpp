#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {

struct SqliteMigrationRecord {
    std::uint32_t version = 0U;
    std::string migration_id;
    std::string checksum;
    std::string applied_at_utc;

    [[nodiscard]] bool operator==(const SqliteMigrationRecord&) const noexcept = default;
};

struct SqliteDatabaseInfo {
    std::uint32_t schema_version = 0U;
    std::string journal_mode;
    std::vector<SqliteMigrationRecord> migrations;
};

struct SqliteIntegrityReport {
    bool sqlite_integrity_check_passed = false;
    std::vector<std::string> sqlite_issues;
    std::vector<std::string> migration_ledger_issues;
    std::vector<std::string> authoring_issues;

    [[nodiscard]] bool healthy() const noexcept {
        return sqlite_integrity_check_passed && sqlite_issues.empty() &&
            migration_ledger_issues.empty() && authoring_issues.empty();
    }
};

// Owns the package's SQLite document database connection. Opening a fresh file
// creates the migration ledger and empty versioned snapshot table; the legacy
// document bytes are added only through the insert-only migration import.
class SqliteAuthoringDatabase {
public:
    static constexpr std::uint32_t kCurrentSchemaVersion = 2U;
    static constexpr std::int64_t kApplicationId = 0x43415254;

    [[nodiscard]] static core::Result<SqliteAuthoringDatabase> open(
        const std::filesystem::path& path);
    // Opens the database read-only, reports structural and migration-ledger
    // issues, and never attempts to repair or rewrite the file.
    [[nodiscard]] static core::Result<SqliteIntegrityReport> scan_integrity(
        const std::filesystem::path& path);

    ~SqliteAuthoringDatabase();
    SqliteAuthoringDatabase(SqliteAuthoringDatabase&& other) noexcept;
    SqliteAuthoringDatabase& operator=(SqliteAuthoringDatabase&& other) noexcept;

    SqliteAuthoringDatabase(const SqliteAuthoringDatabase&) = delete;
    SqliteAuthoringDatabase& operator=(const SqliteAuthoringDatabase&) = delete;

    [[nodiscard]] core::Result<SqliteDatabaseInfo> inspect() const;
    // Imports a validated legacy .carto snapshot exactly once. The original
    // serialized bytes are retained for byte-identical migration export.
    [[nodiscard]] core::Result<void> import_project_document(
        std::string_view serialized_document);
    [[nodiscard]] core::Result<std::string> export_project_document() const;
    [[nodiscard]] core::Result<void> close();
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    struct Impl;

    SqliteAuthoringDatabase(std::unique_ptr<Impl> implementation,
                            std::filesystem::path path) noexcept;

    std::unique_ptr<Impl> implementation_;
    std::filesystem::path path_;
};

} // namespace carto::project
