#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/journal/journal.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace carto::journal {

struct CheckpointRecord {
    core::Revision project_revision;
    assets::BlobRef snapshot;
    assets::Sha256Digest asset_manifest_digest;
    std::uint64_t journal_entries = 0U;
    assets::Sha256Digest journal_head_hash;
    assets::Sha256Digest integrity_digest;
};

struct RecoveryCandidate {
    CheckpointRecord checkpoint;
    std::vector<std::uint8_t> snapshot;
    std::vector<JournalEntry> pending_entries;
};

class CheckpointStore {
public:
    explicit CheckpointStore(std::filesystem::path root);

    [[nodiscard]] core::Result<CheckpointRecord> save(
        const Journal& journal,
        core::Revision project_revision,
        std::span<const std::uint8_t> snapshot,
        assets::Sha256Digest asset_manifest_digest);
    [[nodiscard]] core::Result<RecoveryCandidate> recover(const Journal& journal) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    [[nodiscard]] core::Result<CheckpointRecord> parse(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> verify_record(
        const CheckpointRecord& record,
        const std::vector<JournalEntry>& entries) const;
    [[nodiscard]] static assets::Sha256Digest record_digest(const CheckpointRecord& record);
    [[nodiscard]] static std::string serialize(const CheckpointRecord& record);
    [[nodiscard]] std::filesystem::path path_for(core::Revision revision) const;

    std::filesystem::path root_;
    assets::BlobStore snapshots_;
};

} // namespace carto::journal
