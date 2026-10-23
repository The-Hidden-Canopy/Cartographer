#pragma once

// Character package publication, rollback and migration records (spec ch27
// on top of the ch18 producer contract in prepared_character.hpp).
//
// publish_prepared_character is a thin, honest wrapper over the existing
// prepared-scene compare-and-swap publisher: the character manifest is
// mirrored into a PreparedSceneEnvelope so the immutable revisions/ store,
// the file-locked CAS selector, symlink rejection, staged write + full
// re-read verification and atomic selector replacement all apply unchanged.
// Any failure leaves the previously selected revision intact; the rollback
// receipt proves what the selector still points at.

#include <carto/character/manifest.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/prepared_character.hpp>
#include <carto/production/prepared_publication.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

// Receipt for one committed (or reused) immutable character package
// revision. `selected_digest` is the selector/envelope digest the immutable
// revision store is keyed by (what `current` points at); `manifest_digest`
// is the character manifest's own canonical digest. `closure_digest` binds
// the receipt to the exact resolved dependency set, not to manifest bytes.
struct CharacterPackagePublicationReceipt {
    assets::Sha256Digest manifest_digest;
    assets::Sha256Digest selected_digest;
    assets::Sha256Digest closure_digest;
    std::optional<assets::Sha256Digest> previous_selected_digest;
    core::Revision source_revision;
    std::filesystem::path revision_path;
    bool reused_immutable_revision = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Proof emitted after a failed publication attempt: the selector digest that
// was attempted and the digest the selector still resolves to (nullopt when
// no revision was selected, so nothing could be lost). A preserved selection
// that differs from the attempt is the ch27 last-good guarantee.
struct CharacterPublicationRollbackReceipt {
    static constexpr std::size_t kMaxStageBytes = 128U;
    static constexpr std::size_t kMaxDetailBytes = 4'096U;

    assets::Sha256Digest attempted_selected_digest;
    std::optional<assets::Sha256Digest> preserved_selected_digest;
    std::string failed_stage;
    std::string detail;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// ch27 schema-bump migration record. A migration is a transactional journal
// event: it names the expected basis revision it was validated against,
// preserves the original bytes by digest, lists every loss explicitly, and
// declares whether a rollback path exists. Old-schema input is never
// rewritten in place.
struct CharacterPackageMigrationReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 512U;
    static constexpr std::size_t kMaxLossEntries = 256U;
    static constexpr std::size_t kMaxLossEntryBytes = 512U;

    std::string package_identity;
    core::Revision expected_basis_revision;
    core::Revision migrated_revision;
    std::uint32_t from_schema_major = 0U;
    std::uint32_t from_schema_minor = 0U;
    std::uint32_t to_schema_major = 0U;
    std::uint32_t to_schema_minor = 0U;
    assets::Sha256Digest backup_digest;
    assets::Sha256Digest migrated_manifest_digest;
    bool rollback_path_available = false;
    // Sorted, unique, explicit loss report; empty means lossless.
    std::vector<std::string> loss_report;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Publishes one complete immutable character package revision and
// atomically selects it. `expected_selected_digest` is the mandatory
// compare-and-swap expectation (nullopt: nothing may be selected). A failed
// or stale attempt never changes the selector; pair a failure with
// capture_character_rollback_receipt to prove last-good preservation.
[[nodiscard]] core::Result<CharacterPackagePublicationReceipt> publish_prepared_character(
    const std::filesystem::path& output_root,
    const PreparedCharacterPackage& package,
    std::optional<assets::Sha256Digest> expected_selected_digest);

// Reads the current selection once and records what the selector still
// resolves to after a failed attempt. Fails closed when the selector is
// unreadable, because an unprovable last-good is treated as lost.
[[nodiscard]] core::Result<CharacterPublicationRollbackReceipt> capture_character_rollback_receipt(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& attempted_selected_digest,
    std::string failed_stage,
    std::string detail);

// Builds the migration receipt for one committed schema bump. The original
// bytes are digested here and preserved by the caller; the migrated manifest
// digest pins the post-migration state. Loss report entries are checked
// sorted and unique.
[[nodiscard]] core::Result<CharacterPackageMigrationReceipt> make_character_migration_receipt(
    std::string package_identity,
    core::Revision expected_basis_revision,
    core::Revision migrated_revision,
    std::uint32_t from_schema_major,
    std::uint32_t from_schema_minor,
    std::uint32_t to_schema_major,
    std::uint32_t to_schema_minor,
    std::span<const std::uint8_t> preserved_original_bytes,
    const character::CharacterPreparedManifest& migrated_manifest,
    bool rollback_path_available,
    std::vector<std::string> loss_report);

} // namespace carto::production
