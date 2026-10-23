#pragma once

#include <filesystem>

namespace carto::project::testing {

// Opens a fresh SQLite file, starts the real initial migration transaction,
// and terminates the process after its writes but before COMMIT.
[[nodiscard]] int crash_during_initial_migration(
    const std::filesystem::path& path) noexcept;

// Changes only the stored migration snapshot checksum to exercise read-only
// authoring-integrity reporting.
[[nodiscard]] int corrupt_project_snapshot_digest_for_test(
    const std::filesystem::path& path) noexcept;

// Creates a committed schema-v1 database to exercise the v1-to-v2 migration.
[[nodiscard]] int create_schema_v1_database_for_test(
    const std::filesystem::path& path) noexcept;

// Calls the production snapshot import path and terminates after its INSERT,
// before the WAL transaction commits.
[[nodiscard]] int crash_during_project_document_import_for_test(
    const std::filesystem::path& database_path,
    const std::filesystem::path& source_document_path) noexcept;

} // namespace carto::project::testing
