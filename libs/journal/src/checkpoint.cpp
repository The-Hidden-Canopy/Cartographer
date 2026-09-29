#include <carto/journal/checkpoint.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace carto::journal {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

constexpr std::uintmax_t kMaxCheckpointBytes = 64U * 1024U;
std::mutex g_checkpoint_mutex;
std::atomic<std::uint64_t> g_checkpoint_temp_counter{0U};

core::Result<std::uint64_t> parse_uint64(std::string_view value) {
    if (value.empty()) {
        return core::Result<std::uint64_t>::failure(invalid("checkpoint integer field is empty"));
    }
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return core::Result<std::uint64_t>::failure(invalid("checkpoint integer field is invalid"));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

core::Result<std::string> field(
    const std::map<std::string, std::string>& fields,
    std::string_view name) {
    const auto found = fields.find(std::string(name));
    if (found == fields.end()) {
        return core::Result<std::string>::failure(validation("checkpoint field is missing"));
    }
    return core::Result<std::string>::success(found->second);
}

} // namespace

CheckpointStore::CheckpointStore(std::filesystem::path root)
    : root_(std::move(root)), snapshots_(root_ / "snapshots") {}

std::filesystem::path CheckpointStore::path_for(core::Revision revision) const {
    return root_ / ("checkpoint-" + std::to_string(revision.value()) + ".meta");
}

assets::Sha256Digest CheckpointStore::record_digest(const CheckpointRecord& record) {
    const std::string canonical =
        std::to_string(record.project_revision.value()) + "\n" +
        record.snapshot.digest.hex() + "\n" + std::to_string(record.snapshot.bytes) + "\n" +
        record.snapshot.media_type + "\n" + record.asset_manifest_digest.hex() + "\n" +
        std::to_string(record.journal_entries) + "\n" + record.journal_head_hash.hex();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(canonical.data());
    return assets::sha256(std::span<const std::uint8_t>(bytes, canonical.size()));
}

std::string CheckpointStore::serialize(const CheckpointRecord& record) {
    return std::string("CARTOGRAPHER_CHECKPOINT_V1\n") +
        "revision=" + std::to_string(record.project_revision.value()) + "\n" +
        "snapshot_digest=" + record.snapshot.digest.hex() + "\n" +
        "snapshot_bytes=" + std::to_string(record.snapshot.bytes) + "\n" +
        "snapshot_media_type=" + record.snapshot.media_type + "\n" +
        "asset_manifest_digest=" + record.asset_manifest_digest.hex() + "\n" +
        "journal_entries=" + std::to_string(record.journal_entries) + "\n" +
        "journal_head_hash=" + record.journal_head_hash.hex() + "\n" +
        "integrity_digest=" + record.integrity_digest.hex() + "\n";
}

core::Result<CheckpointRecord> CheckpointStore::parse(
    const std::filesystem::path& path) const {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<CheckpointRecord>::failure(
            io_error("unable to inspect checkpoint metadata size"));
    }
    if (size > kMaxCheckpointBytes || size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<CheckpointRecord>::failure(
            validation("checkpoint metadata is missing or too large"));
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<CheckpointRecord>::failure(io_error("unable to open checkpoint"));
    }
    std::string header;
    if (!std::getline(stream, header) || header != "CARTOGRAPHER_CHECKPOINT_V1") {
        return core::Result<CheckpointRecord>::failure(validation("checkpoint header is invalid"));
    }
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() > 4096U) {
            return core::Result<CheckpointRecord>::failure(validation("checkpoint line is too long"));
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<CheckpointRecord>::failure(validation("checkpoint field is malformed"));
        }
    }
    if (!stream.eof() || fields.size() != 8U) {
        return core::Result<CheckpointRecord>::failure(validation("checkpoint field set is invalid"));
    }
    const auto revision = field(fields, "revision");
    const auto snapshot_digest = field(fields, "snapshot_digest");
    const auto snapshot_bytes = field(fields, "snapshot_bytes");
    const auto snapshot_media_type = field(fields, "snapshot_media_type");
    const auto asset_manifest_digest = field(fields, "asset_manifest_digest");
    const auto journal_entries = field(fields, "journal_entries");
    const auto journal_head_hash = field(fields, "journal_head_hash");
    const auto integrity_digest = field(fields, "integrity_digest");
    if (!revision || !snapshot_digest || !snapshot_bytes || !snapshot_media_type ||
        !asset_manifest_digest || !journal_entries || !journal_head_hash || !integrity_digest) {
        return core::Result<CheckpointRecord>::failure(validation("checkpoint field is missing"));
    }
    const auto parsed_revision = parse_uint64(revision.value());
    const auto parsed_bytes = parse_uint64(snapshot_bytes.value());
    const auto parsed_entries = parse_uint64(journal_entries.value());
    const auto parsed_snapshot = assets::Sha256Digest::from_hex(snapshot_digest.value());
    const auto parsed_assets = assets::Sha256Digest::from_hex(asset_manifest_digest.value());
    const auto parsed_head = assets::Sha256Digest::from_hex(journal_head_hash.value());
    const auto parsed_integrity = assets::Sha256Digest::from_hex(integrity_digest.value());
    if (!parsed_revision || !parsed_bytes || !parsed_entries || !parsed_snapshot || !parsed_assets ||
        !parsed_head || !parsed_integrity || snapshot_media_type.value().empty() ||
        snapshot_media_type.value().find_first_of("\r\n") != std::string::npos) {
        return core::Result<CheckpointRecord>::failure(validation("checkpoint field value is invalid"));
    }
    CheckpointRecord record;
    record.project_revision = core::Revision(parsed_revision.value());
    record.snapshot = {parsed_snapshot.value(), parsed_bytes.value(), snapshot_media_type.value()};
    record.asset_manifest_digest = parsed_assets.value();
    record.journal_entries = parsed_entries.value();
    record.journal_head_hash = parsed_head.value();
    record.integrity_digest = parsed_integrity.value();
    if (record_digest(record) != record.integrity_digest) {
        return core::Result<CheckpointRecord>::failure(validation("checkpoint integrity digest is invalid"));
    }
    return core::Result<CheckpointRecord>::success(std::move(record));
}

core::Result<void> CheckpointStore::verify_record(
    const CheckpointRecord& record,
    const std::vector<JournalEntry>& entries) const {
    if (record.journal_entries > entries.size()) {
        return core::Result<void>::failure(stale("checkpoint references an unavailable journal prefix"));
    }
    if (record.journal_entries == 0U) {
        if (record.project_revision != core::Revision{} || !record.journal_head_hash.is_zero()) {
            return core::Result<void>::failure(validation("empty checkpoint has non-empty journal metadata"));
        }
    } else {
        const JournalEntry& tail = entries[static_cast<std::size_t>(record.journal_entries - 1U)];
        if (tail.revision_after != record.project_revision || tail.entry_hash != record.journal_head_hash) {
            return core::Result<void>::failure(validation("checkpoint journal prefix does not match the journal"));
        }
    }
    const auto snapshot = snapshots_.read(record.snapshot.digest);
    if (!snapshot) {
        return core::Result<void>::failure(snapshot.error().with_context("checkpoint snapshot"));
    }
    if (snapshot.value().size() != record.snapshot.bytes) {
        return core::Result<void>::failure(validation("checkpoint snapshot byte count is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<CheckpointRecord> CheckpointStore::save(
    const Journal& journal,
    core::Revision project_revision,
    std::span<const std::uint8_t> snapshot,
    assets::Sha256Digest asset_manifest_digest) {
    std::lock_guard lock(g_checkpoint_mutex);
    const auto entries = journal.read_all();
    if (!entries) return core::Result<CheckpointRecord>::failure(entries.error());
    const core::Revision current = entries.value().empty()
        ? core::Revision{}
        : entries.value().back().revision_after;
    if (current != project_revision) {
        return core::Result<CheckpointRecord>::failure(stale("checkpoint revision does not match journal tail"));
    }
    const auto stored = snapshots_.put(snapshot, "application/x-cartographer-snapshot");
    if (!stored) return core::Result<CheckpointRecord>::failure(stored.error());

    CheckpointRecord record;
    record.project_revision = project_revision;
    record.snapshot = stored.value();
    record.asset_manifest_digest = asset_manifest_digest;
    record.journal_entries = entries.value().size();
    if (!entries.value().empty()) record.journal_head_hash = entries.value().back().entry_hash;
    record.integrity_digest = record_digest(record);

    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) return core::Result<CheckpointRecord>::failure(io_error("unable to create checkpoint directory"));
    const std::filesystem::path destination = path_for(project_revision);
    const bool destination_exists = std::filesystem::exists(destination, error);
    if (error) {
        return core::Result<CheckpointRecord>::failure(
            io_error("unable to inspect checkpoint destination"));
    }
    if (destination_exists) {
        return core::Result<CheckpointRecord>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "checkpoint revision already exists"));
    }
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_checkpoint_temp_counter.fetch_add(1U, std::memory_order_relaxed);
    const std::filesystem::path temporary = destination.string() + ".tmp-" +
        std::to_string(static_cast<unsigned long long>(ticks)) + "-" +
        std::to_string(static_cast<unsigned long long>(thread)) + "-" +
        std::to_string(static_cast<unsigned long long>(counter));
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return core::Result<CheckpointRecord>::failure(io_error("unable to create checkpoint temporary file"));
    stream << serialize(record);
    stream.flush();
    stream.close();
    if (!stream) {
        std::filesystem::remove(temporary, error);
        return core::Result<CheckpointRecord>::failure(io_error("unable to write checkpoint"));
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return core::Result<CheckpointRecord>::failure(io_error("unable to publish checkpoint atomically"));
    }
    return core::Result<CheckpointRecord>::success(std::move(record));
}

core::Result<RecoveryCandidate> CheckpointStore::recover(const Journal& journal) const {
    std::lock_guard lock(g_checkpoint_mutex);
    const auto entries = journal.read_all();
    if (!entries) return core::Result<RecoveryCandidate>::failure(entries.error());
    std::error_code error;
    const bool root_is_directory = std::filesystem::is_directory(root_, error);
    if (error) {
        return core::Result<RecoveryCandidate>::failure(
            io_error("unable to inspect checkpoint directory"));
    }
    if (!root_is_directory) {
        return core::Result<RecoveryCandidate>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "checkpoint directory does not exist"));
    }
    std::optional<CheckpointRecord> selected;
    for (const auto& item : std::filesystem::directory_iterator(root_, error)) {
        if (error) break;
        const bool regular_file = item.is_regular_file(error);
        if (error) {
            return core::Result<RecoveryCandidate>::failure(
                io_error("unable to inspect checkpoint directory entry"));
        }
        if (!regular_file) continue;
        const std::string filename = item.path().filename().string();
        if (!filename.starts_with("checkpoint-") || !filename.ends_with(".meta")) continue;
        const std::string filename_revision = filename.substr(
            std::string("checkpoint-").size(),
            filename.size() - std::string("checkpoint-").size() - std::string(".meta").size());
        const auto parsed_filename_revision = parse_uint64(filename_revision);
        if (!parsed_filename_revision) {
            return core::Result<RecoveryCandidate>::failure(
                validation("checkpoint filename revision is invalid"));
        }
        if (filename_revision != std::to_string(parsed_filename_revision.value())) {
            return core::Result<RecoveryCandidate>::failure(
                validation("checkpoint filename revision is not canonical"));
        }
        const auto parsed = parse(item.path());
        if (!parsed) {
            return core::Result<RecoveryCandidate>::failure(
                parsed.error().with_context("checkpoint metadata: " + item.path().string()));
        }
        if (parsed.value().project_revision.value() != parsed_filename_revision.value()) {
            return core::Result<RecoveryCandidate>::failure(
                validation("checkpoint filename revision does not match its metadata"));
        }
        if (!selected.has_value() || parsed.value().project_revision > selected->project_revision) {
            selected = parsed.value();
        }
    }
    if (error) return core::Result<RecoveryCandidate>::failure(io_error("unable to scan checkpoint directory"));
    if (!selected.has_value()) {
        return core::Result<RecoveryCandidate>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "no valid checkpoint exists"));
    }
    if (auto result = verify_record(*selected, entries.value()); !result) {
        return core::Result<RecoveryCandidate>::failure(result.error());
    }
    const auto snapshot = snapshots_.read(selected->snapshot.digest);
    if (!snapshot) return core::Result<RecoveryCandidate>::failure(snapshot.error());
    RecoveryCandidate candidate{*selected, snapshot.value(), {}};
    for (const auto& entry : entries.value()) {
        if (entry.revision_after > selected->project_revision) candidate.pending_entries.push_back(entry);
    }
    return core::Result<RecoveryCandidate>::success(std::move(candidate));
}

} // namespace carto::journal
