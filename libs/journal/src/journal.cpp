#include <carto/journal/journal.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
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

std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0U;
    while (start <= line.size()) {
        const std::size_t separator = line.find('\t', start);
        if (separator == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, separator - start));
        start = separator + 1U;
    }
    return fields;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

core::Result<std::vector<std::uint8_t>> decode_hex(std::string_view value) {
    if (value.size() % 2U != 0U) {
        return core::Result<std::vector<std::uint8_t>>::failure(invalid("hex field has an odd length"));
    }
    std::vector<std::uint8_t> result(value.size() / 2U);
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const int high = hex_value(value[index * 2U]);
        const int low = hex_value(value[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return core::Result<std::vector<std::uint8_t>>::failure(
                invalid("hex field contains a non-hex character"));
        }
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(result));
}

std::string encode_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(bytes.size() * 2U);
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    return result;
}

core::Result<std::uint64_t> parse_uint64(std::string_view value) {
    if (value.empty()) {
        return core::Result<std::uint64_t>::failure(invalid("journal integer field is empty"));
    }
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return core::Result<std::uint64_t>::failure(invalid("journal integer field is invalid"));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

Uuid make_uuid() {
    Uuid result;
    std::random_device random;
    for (auto& byte : result.bytes) {
        byte = static_cast<std::uint8_t>(random());
    }
    result.bytes[6] = static_cast<std::uint8_t>((result.bytes[6] & 0x0fU) | 0x40U);
    result.bytes[8] = static_cast<std::uint8_t>((result.bytes[8] & 0x3fU) | 0x80U);
    return result;
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

bool Uuid::is_zero() const noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t value) { return value == 0U; });
}

std::string Uuid::hex() const {
    return encode_hex(bytes);
}

core::Result<Uuid> Uuid::from_hex(std::string_view value) {
    if (value.size() != 32U) {
        return core::Result<Uuid>::failure(invalid("journal event id must contain 32 hex characters"));
    }
    const auto decoded = decode_hex(value);
    if (!decoded) {
        return core::Result<Uuid>::failure(decoded.error());
    }
    Uuid result;
    std::copy(decoded.value().begin(), decoded.value().end(), result.bytes.begin());
    return core::Result<Uuid>::success(result);
}

Journal::Journal(std::filesystem::path path, JournalLimits limits)
    : path_(std::move(path)), limits_(limits) {}

assets::Sha256Digest Journal::entry_digest(const JournalEntry& entry) {
    const std::string canonical = entry.event_id.hex() + "\t" +
        std::to_string(entry.revision_before.value()) + "\t" +
        std::to_string(entry.revision_after.value()) + "\t" +
        encode_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(entry.event_type.data()), entry.event_type.size())) + "\t" +
        encode_hex(entry.payload) + "\t" +
        std::to_string(entry.committed_at_ms) + "\t" +
        entry.previous_entry_hash.hex();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(canonical.data());
    return assets::sha256(std::span<const std::uint8_t>(bytes, canonical.size()));
}

std::string Journal::serialize_record(const JournalEntry& entry) const {
    return entry.event_id.hex() + "\t" + std::to_string(entry.revision_before.value()) + "\t" +
        std::to_string(entry.revision_after.value()) + "\t" +
        encode_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(entry.event_type.data()), entry.event_type.size())) + "\t" +
        encode_hex(entry.payload) + "\t" + std::to_string(entry.committed_at_ms) + "\t" +
        entry.previous_entry_hash.hex() + "\t" + entry.entry_hash.hex();
}

core::Result<JournalEntry> Journal::parse_record(std::string_view line) const {
    if (line.size() > limits_.max_record_bytes) {
        return core::Result<JournalEntry>::failure(validation("journal record exceeds the configured limit"));
    }
    const auto fields = split_fields(line);
    if (fields.size() != 8U) {
        return core::Result<JournalEntry>::failure(validation("journal record has the wrong field count"));
    }
    const auto event_id = Uuid::from_hex(fields[0]);
    const auto before = parse_uint64(fields[1]);
    const auto after = parse_uint64(fields[2]);
    const auto timestamp = parse_uint64(fields[5]);
    const auto previous = assets::Sha256Digest::from_hex(fields[6]);
    const auto hash = assets::Sha256Digest::from_hex(fields[7]);
    if (fields[3].size() % 2U != 0U || fields[4].size() % 2U != 0U ||
        fields[3].size() / 2U > limits_.max_event_type_bytes ||
        fields[4].size() / 2U > limits_.max_payload_bytes) {
        return core::Result<JournalEntry>::failure(validation("journal record exceeds field limits"));
    }
    const auto event_type = decode_hex(fields[3]);
    const auto payload = decode_hex(fields[4]);
    if (!event_id || !before || !after || !event_type || !payload || !timestamp || !previous || !hash) {
        const auto& diagnostic = !event_id ? event_id.error() :
            !before ? before.error() : !after ? after.error() :
            !event_type ? event_type.error() : !payload ? payload.error() :
            !timestamp ? timestamp.error() : !previous ? previous.error() : hash.error();
        return core::Result<JournalEntry>::failure(diagnostic);
    }
    if (event_id.value().is_zero() || event_type.value().empty() ||
        event_type.value().size() > limits_.max_event_type_bytes ||
        payload.value().size() > limits_.max_payload_bytes) {
        return core::Result<JournalEntry>::failure(validation("journal record exceeds field limits"));
    }
    if (!std::all_of(event_type.value().begin(), event_type.value().end(),
                     [](std::uint8_t byte) { return byte >= 0x20U && byte != 0x7fU; })) {
        return core::Result<JournalEntry>::failure(validation("journal event type contains a control byte"));
    }
    JournalEntry entry;
    entry.event_id = event_id.value();
    entry.revision_before = core::Revision(before.value());
    entry.revision_after = core::Revision(after.value());
    entry.event_type.assign(reinterpret_cast<const char*>(event_type.value().data()), event_type.value().size());
    entry.payload = payload.value();
    entry.payload_digest = assets::sha256(entry.payload);
    entry.committed_at_ms = timestamp.value();
    entry.previous_entry_hash = previous.value();
    entry.entry_hash = hash.value();
    if (entry.revision_after <= entry.revision_before || entry_digest(entry) != entry.entry_hash) {
        return core::Result<JournalEntry>::failure(validation("journal record integrity check failed"));
    }
    return core::Result<JournalEntry>::success(std::move(entry));
}

core::Result<std::vector<JournalEntry>> Journal::read_all() const {
    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        if (error) {
            return core::Result<std::vector<JournalEntry>>::failure(io_error("unable to inspect journal path"));
        }
        return core::Result<std::vector<JournalEntry>>::success({});
    }
    const auto file_size = std::filesystem::file_size(path_, error);
    if (error || file_size > limits_.max_file_bytes) {
        return core::Result<std::vector<JournalEntry>>::failure(
            validation("journal file exceeds the configured limit"));
    }
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) {
        return core::Result<std::vector<JournalEntry>>::failure(io_error("unable to open journal"));
    }
    std::string header;
    if (!std::getline(stream, header) || header != kHeader) {
        return core::Result<std::vector<JournalEntry>>::failure(validation("journal header is invalid"));
    }
    std::vector<JournalEntry> entries;
    std::set<Uuid> event_ids;
    assets::Sha256Digest previous_hash;
    core::Revision previous_revision;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            return core::Result<std::vector<JournalEntry>>::failure(validation("journal contains an empty record"));
        }
        const auto parsed = parse_record(line);
        if (!parsed) {
            return core::Result<std::vector<JournalEntry>>::failure(parsed.error());
        }
        const JournalEntry& entry = parsed.value();
        if (!event_ids.insert(entry.event_id).second) {
            return core::Result<std::vector<JournalEntry>>::failure(validation("journal contains a duplicate event id"));
        }
        if (entry.previous_entry_hash != previous_hash || entry.revision_before != previous_revision) {
            return core::Result<std::vector<JournalEntry>>::failure(validation("journal hash or revision chain is broken"));
        }
        previous_hash = entry.entry_hash;
        previous_revision = entry.revision_after;
        entries.push_back(entry);
        if (entries.size() > limits_.max_entries) {
            return core::Result<std::vector<JournalEntry>>::failure(
                validation("journal contains too many entries"));
        }
    }
    if (!stream.eof()) {
        return core::Result<std::vector<JournalEntry>>::failure(io_error("unable to read journal"));
    }
    return core::Result<std::vector<JournalEntry>>::success(std::move(entries));
}

core::Result<JournalEntry> Journal::append(const JournalAppend& request) {
    if (request.event_type.empty() || request.event_type.size() > limits_.max_event_type_bytes ||
        request.event_type.find_first_of("\t\r\n") != std::string::npos ||
        !std::all_of(request.event_type.begin(), request.event_type.end(),
                     [](unsigned char byte) { return byte >= 0x20U && byte != 0x7fU; })) {
        return core::Result<JournalEntry>::failure(invalid("journal event type is invalid or too long"));
    }
    if (request.payload.size() > limits_.max_payload_bytes) {
        return core::Result<JournalEntry>::failure(invalid("journal payload exceeds the configured limit"));
    }
    const auto existing = read_all();
    if (!existing) {
        return core::Result<JournalEntry>::failure(existing.error());
    }
    const core::Revision current = existing.value().empty()
        ? core::Revision{}
        : existing.value().back().revision_after;
    if (request.revision_before != current) {
        return core::Result<JournalEntry>::failure(stale("journal append revision does not match the current tail"));
    }
    if (request.revision_after <= request.revision_before) {
        return core::Result<JournalEntry>::failure(invalid("journal revision must advance"));
    }
    if (request.revision_after.exhausted() && request.revision_before.exhausted()) {
        return core::Result<JournalEntry>::failure(invalid("journal revision is exhausted"));
    }

    JournalEntry entry;
    entry.event_id = make_uuid();
    entry.revision_before = request.revision_before;
    entry.revision_after = request.revision_after;
    entry.event_type = request.event_type;
    entry.payload.assign(request.payload.begin(), request.payload.end());
    entry.payload_digest = assets::sha256(entry.payload);
    entry.committed_at_ms = request.committed_at_ms.value_or(now_ms());
    if (!existing.value().empty()) {
        entry.previous_entry_hash = existing.value().back().entry_hash;
    }
    entry.entry_hash = entry_digest(entry);
    const std::string record = serialize_record(entry);
    if (record.size() > limits_.max_record_bytes) {
        return core::Result<JournalEntry>::failure(invalid("journal record exceeds the configured limit"));
    }

    const auto parent = path_.parent_path();
    std::error_code error;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return core::Result<JournalEntry>::failure(io_error("unable to create journal directory"));
        }
    }
    const bool exists = std::filesystem::exists(path_, error);
    if (error) {
        return core::Result<JournalEntry>::failure(io_error("unable to inspect journal before append"));
    }
    bool needs_header = !exists;
    if (exists) {
        const auto size = std::filesystem::file_size(path_, error);
        if (error) {
            return core::Result<JournalEntry>::failure(io_error("unable to inspect journal size"));
        }
        needs_header = size == 0U;
    }
    std::ofstream stream(path_, std::ios::binary | std::ios::app);
    if (!stream) {
        return core::Result<JournalEntry>::failure(io_error("unable to open journal for append"));
    }
    if (needs_header) {
        stream << kHeader << '\n';
    }
    stream << record << '\n';
    stream.flush();
    if (!stream) {
        return core::Result<JournalEntry>::failure(io_error("unable to append journal record"));
    }
    return core::Result<JournalEntry>::success(std::move(entry));
}

core::Result<void> Journal::verify() const {
    const auto entries = read_all();
    if (!entries) {
        return core::Result<void>::failure(entries.error());
    }
    return core::Result<void>::success();
}

core::Result<core::Revision> Journal::current_revision() const {
    const auto entries = read_all();
    if (!entries) {
        return core::Result<core::Revision>::failure(entries.error());
    }
    return core::Result<core::Revision>::success(
        entries.value().empty() ? core::Revision{} : entries.value().back().revision_after);
}

core::Result<void> Journal::replay_from(
    core::Revision checkpoint_revision,
    const std::function<core::Result<void>(const JournalEntry&)>& apply) const {
    if (!apply) {
        return core::Result<void>::failure(invalid("journal replay requires an apply callback"));
    }
    const auto entries = read_all();
    if (!entries) {
        return core::Result<void>::failure(entries.error());
    }
    const core::Revision current = entries.value().empty()
        ? core::Revision{}
        : entries.value().back().revision_after;
    if (checkpoint_revision > current) {
        return core::Result<void>::failure(stale("checkpoint revision is newer than the journal"));
    }
    for (const auto& entry : entries.value()) {
        if (entry.revision_after <= checkpoint_revision) {
            continue;
        }
        if (auto result = apply(entry); !result) {
            return result;
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::journal
