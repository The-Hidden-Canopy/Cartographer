#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::journal {

struct Uuid {
    std::array<std::uint8_t, 16> bytes{};

    [[nodiscard]] bool is_zero() const noexcept;
    [[nodiscard]] std::string hex() const;
    [[nodiscard]] static core::Result<Uuid> from_hex(std::string_view value);
    [[nodiscard]] constexpr auto operator<=>(const Uuid&) const noexcept = default;
};

struct JournalEntry {
    Uuid event_id;
    core::Revision revision_before;
    core::Revision revision_after;
    std::string event_type;
    assets::Sha256Digest payload_digest;
    std::uint64_t committed_at_ms = 0U;
    assets::Sha256Digest previous_entry_hash;
    assets::Sha256Digest entry_hash;
    std::vector<std::uint8_t> payload;
};

struct JournalAppend {
    core::Revision revision_before;
    core::Revision revision_after;
    std::string event_type;
    std::span<const std::uint8_t> payload;
    std::optional<std::uint64_t> committed_at_ms;
};

struct JournalLimits {
    std::uint64_t max_payload_bytes = 64ULL * 1024ULL * 1024ULL;
    std::uint32_t max_event_type_bytes = 256U;
    std::uint64_t max_record_bytes = 128ULL * 1024ULL * 1024ULL;
    std::uint64_t max_file_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_entries = 1'000'000U;
};

class Journal {
public:
    static constexpr std::string_view kHeader = "CARTOGRAPHER_JOURNAL_V1";

    explicit Journal(std::filesystem::path path, JournalLimits limits = {});

    [[nodiscard]] core::Result<JournalEntry> append(const JournalAppend& request);
    [[nodiscard]] core::Result<std::vector<JournalEntry>> read_all() const;
    [[nodiscard]] core::Result<void> verify() const;
    [[nodiscard]] core::Result<core::Revision> current_revision() const;
    [[nodiscard]] core::Result<void> replay_from(
        core::Revision checkpoint_revision,
        const std::function<core::Result<void>(const JournalEntry&)>& apply) const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    [[nodiscard]] core::Result<JournalEntry> parse_record(std::string_view line) const;
    [[nodiscard]] core::Result<std::vector<JournalEntry>> read_all_unlocked() const;
    [[nodiscard]] std::string serialize_record(const JournalEntry& entry) const;
    [[nodiscard]] static assets::Sha256Digest entry_digest(const JournalEntry& entry);

    std::filesystem::path path_;
    JournalLimits limits_;
};

} // namespace carto::journal
