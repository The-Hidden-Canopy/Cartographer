#include <carto/production/prepared_publication.hpp>

#include <carto/project/file_lock.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#endif

namespace carto::production {
namespace {

constexpr std::string_view kSelectorMagic = "CARTOGRAPHER_PREPARED_SELECTION";
constexpr std::uint32_t kSelectorVersion = 1U;
constexpr std::uintmax_t kMaxSelectorBytes = 4U * 1024U;
constexpr std::uintmax_t kMaxPreparedProductBytes = 512U * 1024U * 1024U;
constexpr std::uintmax_t kMaxPreparedRevisionBytes = 1024U * 1024U * 1024U;
std::atomic<std::uint64_t> g_publication_counter{0U};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

std::string unique_suffix() {
    const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_publication_counter.fetch_add(1U, std::memory_order_relaxed);
    return std::to_string(static_cast<unsigned long long>(clock)) + "-" +
        std::to_string(static_cast<unsigned long long>(thread)) + "-" +
        std::to_string(static_cast<unsigned long long>(counter));
}

core::Result<void> reject_symlink(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error &&
        error.default_error_condition() !=
            std::make_error_condition(std::errc::no_such_file_or_directory)) {
        return core::Result<void>::failure(io_error(
            "unable to inspect prepared publication path"));
    }
    if (!error && std::filesystem::is_symlink(status)) {
        return core::Result<void>::failure(validation(
            "prepared publication paths cannot be symbolic links"));
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD failure = GetLastError();
        if (failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) {
            return core::Result<void>::failure(io_error(
                "unable to inspect prepared publication path attributes"));
        }
    } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return core::Result<void>::failure(validation(
            "prepared publication paths cannot traverse reparse points"));
    }
#endif
    return core::Result<void>::success();
}

core::Result<void> reject_link_chain(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        return core::Result<void>::failure(io_error(
            "unable to resolve prepared publication path"));
    }

    auto cursor = absolute.root_path();
    if (!cursor.empty()) {
        if (auto result = reject_symlink(cursor); !result) return result;
    }
    for (const auto& component : absolute.relative_path()) {
        if (component.empty() || component == ".") continue;
        if (component == "..") {
            return core::Result<void>::failure(validation(
                "prepared publication paths cannot traverse a parent component"));
        }
        cursor /= component;
        if (auto result = reject_symlink(cursor); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<bool> revision_store_is_empty(
    const std::filesystem::path& revisions) {
    if (auto result = reject_link_chain(revisions); !result) {
        return core::Result<bool>::failure(result.error());
    }
    std::error_code error;
    if (!std::filesystem::exists(revisions, error)) {
        if (error) {
            return core::Result<bool>::failure(io_error(
                "unable to inspect prepared revision store"));
        }
        return core::Result<bool>::success(true);
    }
    if (!std::filesystem::is_directory(revisions, error) || error) {
        return core::Result<bool>::failure(validation(
            "prepared revision store is not a directory"));
    }
    const std::filesystem::directory_iterator first(revisions, error);
    if (error) {
        return core::Result<bool>::failure(io_error(
            "unable to enumerate prepared revision store"));
    }
    return core::Result<bool>::success(
        first == std::filesystem::directory_iterator{});
}

core::Result<std::vector<std::uint8_t>> read_bounded_bytes(
    const std::filesystem::path& path,
    std::uintmax_t maximum,
    std::string_view label) {
    if (auto result = reject_link_chain(path); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            std::string(label) + " is missing or not a regular file"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum || size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            std::string(label) + " exceeds its bounded size"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            "unable to open " + std::string(label)));
    }
    if (!bytes.empty()) {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        (!input.good() && !input.eof())) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            "unable to read complete " + std::string(label)));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<void> write_bytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes,
    std::string_view label) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            std::string(label) + " path must name a file"));
    }
    if (auto result = reject_link_chain(path.parent_path()); !result) return result;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return core::Result<void>::failure(io_error(
            "unable to create " + std::string(label) + " parent"));
    }
    if (auto result = reject_link_chain(path.parent_path()); !result) return result;
    if (auto result = reject_link_chain(path); !result) return result;
    if (std::filesystem::exists(path, error) || error) {
        return core::Result<void>::failure(validation(
            std::string(label) + " refuses to overwrite an existing path"));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<void>::failure(io_error(
            "unable to create " + std::string(label)));
    }
    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    output.close();
    if (!output) {
        return core::Result<void>::failure(io_error(
            "unable to durably write " + std::string(label)));
    }
    return core::Result<void>::success();
}

core::Result<void> write_text(
    const std::filesystem::path& path,
    std::string_view text,
    std::string_view label) {
    return write_bytes(path, std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, label);
}

core::Result<std::optional<assets::Sha256Digest>> read_selector(
    const std::filesystem::path& output_root) {
    const auto path = output_root / "current";
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(io_error(
            "unable to inspect prepared selection"));
    }
    if (!exists) {
        return core::Result<std::optional<assets::Sha256Digest>>::success(std::nullopt);
    }
    const auto bytes = read_bounded_bytes(path, kMaxSelectorBytes, "prepared selection");
    if (!bytes) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(bytes.error());
    }
    const std::string text(bytes.value().begin(), bytes.value().end());
    std::istringstream input(text);
    std::string magic;
    std::uint32_t version = 0U;
    std::string record;
    std::string encoded_digest;
    std::string end;
    if (!(input >> magic >> version >> record >> encoded_digest >> end) ||
        magic != kSelectorMagic || version != kSelectorVersion ||
        record != "REVISION" || end != "END") {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(validation(
            "prepared selection record is invalid"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(validation(
            "prepared selection contains trailing data"));
    }
    const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
    if (!digest || digest.value().is_zero()) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(validation(
            "prepared selection digest is invalid"));
    }
    return core::Result<std::optional<assets::Sha256Digest>>::success(digest.value());
}

std::string selector_text(const assets::Sha256Digest& digest) {
    return std::string(kSelectorMagic) + " " + std::to_string(kSelectorVersion) +
        "\nREVISION " + digest.hex() + "\nEND\n";
}

core::Result<void> replace_selector_atomically(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& digest) {
    const auto current = output_root / "current";
    const auto temporary = output_root / (".current-stage-" + unique_suffix());
    if (auto result = reject_link_chain(current); !result) return result;
    if (auto result = write_text(
            temporary, selector_text(digest), "prepared selection staging file");
        !result) {
        return result;
    }
#ifdef _WIN32
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(current, exists_error);
    if (exists_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error(
            "unable to inspect current prepared selection"));
    }
    BOOL replaced = FALSE;
    if (exists) {
        replaced = ReplaceFileW(
            current.wstring().c_str(), temporary.wstring().c_str(), nullptr,
            REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    } else {
        replaced = MoveFileExW(
            temporary.wstring().c_str(), current.wstring().c_str(),
            MOVEFILE_WRITE_THROUGH);
    }
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error(
            "atomic prepared selection replacement failed"));
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, current, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return core::Result<void>::failure(io_error(
            "atomic prepared selection replacement failed"));
    }
#endif
    return core::Result<void>::success();
}

core::Result<SelectedPreparedScene> load_revision(
    const std::filesystem::path& revision_path,
    const assets::Sha256Digest& expected_digest) {
    if (auto result = reject_link_chain(revision_path); !result) {
        return core::Result<SelectedPreparedScene>::failure(result.error());
    }
    std::error_code error;
    if (!std::filesystem::is_directory(revision_path, error) || error) {
        return core::Result<SelectedPreparedScene>::failure(io_error(
            "prepared revision directory is unavailable"));
    }
    const auto manifest_bytes = read_bounded_bytes(
        revision_path / "manifest.carto-prepared", kMaxPreparedSceneBytes,
        "prepared-scene manifest");
    if (!manifest_bytes) {
        return core::Result<SelectedPreparedScene>::failure(manifest_bytes.error());
    }
    const std::string manifest(
        manifest_bytes.value().begin(), manifest_bytes.value().end());
    const auto envelope = PreparedSceneEnvelope::deserialize(manifest);
    if (!envelope) {
        return core::Result<SelectedPreparedScene>::failure(
            envelope.error().with_context("published prepared-scene manifest"));
    }
    const auto digest = envelope.value().canonical_digest();
    if (!digest || digest.value() != expected_digest) {
        return core::Result<SelectedPreparedScene>::failure(validation(
            "prepared revision manifest digest does not match its selected identity"));
    }

    std::set<std::string> expected_files{"manifest.carto-prepared"};
    std::set<std::string> expected_directories;
    for (const auto& product : envelope.value().products) {
        expected_files.insert(product.relative_path);
        auto parent = std::filesystem::path(product.relative_path).parent_path();
        while (!parent.empty()) {
            expected_directories.insert(parent.generic_string());
            parent = parent.parent_path();
        }
    }
    auto remaining_files = expected_files;
    std::filesystem::recursive_directory_iterator cursor(
        revision_path, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return core::Result<SelectedPreparedScene>::failure(io_error(
            "unable to enumerate prepared revision"));
    }
    while (cursor != end) {
        const auto entry_path = cursor->path();
        if (auto result = reject_symlink(entry_path); !result) {
            return core::Result<SelectedPreparedScene>::failure(result.error());
        }
        const auto status = cursor->symlink_status(error);
        if (error) {
            return core::Result<SelectedPreparedScene>::failure(io_error(
                "unable to inspect prepared revision entry"));
        }
        const auto relative = std::filesystem::relative(
            entry_path, revision_path, error).generic_string();
        if (error || relative.empty()) {
            return core::Result<SelectedPreparedScene>::failure(io_error(
                "unable to resolve prepared revision entry"));
        }
        if (std::filesystem::is_directory(status)) {
            if (!expected_directories.contains(relative)) {
                return core::Result<SelectedPreparedScene>::failure(validation(
                    "prepared revision contains an undeclared directory"));
            }
        } else if (!std::filesystem::is_regular_file(status) ||
                   remaining_files.erase(relative) != 1U) {
            return core::Result<SelectedPreparedScene>::failure(validation(
                "prepared revision contains an undeclared or invalid file"));
        }
        cursor.increment(error);
        if (error) {
            return core::Result<SelectedPreparedScene>::failure(io_error(
                "unable to enumerate complete prepared revision"));
        }
    }
    if (!remaining_files.empty()) {
        return core::Result<SelectedPreparedScene>::failure(validation(
            "prepared revision is missing a declared file"));
    }

    std::uintmax_t total_bytes = manifest_bytes.value().size();
    for (const auto& product : envelope.value().products) {
        const auto bytes = read_bounded_bytes(
            revision_path / std::filesystem::path(product.relative_path),
            kMaxPreparedProductBytes, "prepared product");
        if (!bytes) {
            return core::Result<SelectedPreparedScene>::failure(
                bytes.error().with_context(product.identity));
        }
        if (total_bytes > kMaxPreparedRevisionBytes - bytes.value().size()) {
            return core::Result<SelectedPreparedScene>::failure(validation(
                "prepared revision exceeds its aggregate byte bound"));
        }
        total_bytes += bytes.value().size();
        if (assets::sha256(bytes.value()) != product.content_digest) {
            return core::Result<SelectedPreparedScene>::failure(validation(
                "prepared product digest does not match its manifest reference"));
        }
    }
    return core::Result<SelectedPreparedScene>::success(SelectedPreparedScene{
        expected_digest, envelope.value(), revision_path});
}

class StagingCleanup final {
public:
    explicit StagingCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~StagingCleanup() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_ = true;
};

bool same_optional_digest(
    const std::optional<assets::Sha256Digest>& left,
    const std::optional<assets::Sha256Digest>& right) {
    return left.has_value() == right.has_value() &&
        (!left.has_value() || left.value() == right.value());
}

} // namespace

core::Result<PreparedPublicationReceipt> publish_prepared_scene(
    const std::filesystem::path& output_root,
    const PreparedSceneEnvelope& envelope,
    const std::vector<PreparedProductPayload>& payloads,
    std::optional<assets::Sha256Digest> expected_selected_digest) {
    if (output_root.empty() || output_root.filename().empty()) {
        return core::Result<PreparedPublicationReceipt>::failure(invalid(
            "prepared publication root must name a directory"));
    }
    if (auto result = envelope.validate(); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }
    if (expected_selected_digest.has_value() && expected_selected_digest->is_zero()) {
        return core::Result<PreparedPublicationReceipt>::failure(invalid(
            "expected prepared selection digest must not be zero"));
    }
    const auto envelope_digest = envelope.canonical_digest();
    if (!envelope_digest) {
        return core::Result<PreparedPublicationReceipt>::failure(envelope_digest.error());
    }

    std::map<std::string, const PreparedProductPayload*> payload_map;
    std::uintmax_t total_payload_bytes = 0U;
    for (const auto& payload : payloads) {
        if (payload.identity.empty() || payload.bytes.empty() ||
            payload.bytes.size() > kMaxPreparedProductBytes ||
            !payload_map.emplace(payload.identity, &payload).second) {
            return core::Result<PreparedPublicationReceipt>::failure(invalid(
                "prepared product payload identity, size, or uniqueness is invalid"));
        }
        if (total_payload_bytes > kMaxPreparedRevisionBytes - payload.bytes.size()) {
            return core::Result<PreparedPublicationReceipt>::failure(validation(
                "prepared product payloads exceed their aggregate byte bound"));
        }
        total_payload_bytes += payload.bytes.size();
    }
    if (payload_map.size() != envelope.products.size()) {
        return core::Result<PreparedPublicationReceipt>::failure(validation(
            "prepared product payload set is incomplete or contains extras"));
    }
    for (const auto& product : envelope.products) {
        const auto payload = payload_map.find(product.identity);
        if (payload == payload_map.end() ||
            assets::sha256(payload->second->bytes) != product.content_digest) {
            return core::Result<PreparedPublicationReceipt>::failure(validation(
                "prepared product payload does not match its manifest identity"));
        }
    }

    if (auto result = reject_link_chain(output_root); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }
    std::error_code error;
    std::filesystem::create_directories(output_root, error);
    if (error) {
        return core::Result<PreparedPublicationReceipt>::failure(io_error(
            "unable to create prepared publication root"));
    }
    if (auto result = reject_link_chain(output_root); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }
    const auto revisions = output_root / "revisions";
    if (auto result = reject_link_chain(revisions); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }
    std::filesystem::create_directories(revisions, error);
    if (error) {
        return core::Result<PreparedPublicationReceipt>::failure(io_error(
            "unable to create prepared revision store"));
    }
    if (auto result = reject_link_chain(revisions); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }

    if (auto result = reject_link_chain(output_root / "current.lock"); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }
    project::FileLock lock(output_root / "current");
    if (auto result = lock.acquire(); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }
    const auto selected = read_selector(output_root);
    if (!selected) {
        return core::Result<PreparedPublicationReceipt>::failure(selected.error());
    }
    if (!same_optional_digest(selected.value(), expected_selected_digest)) {
        return core::Result<PreparedPublicationReceipt>::failure(stale(
            "prepared publication expectation no longer matches current"));
    }

    if (!selected.value().has_value()) {
        const auto empty_store = revision_store_is_empty(revisions);
        if (!empty_store) {
            return core::Result<PreparedPublicationReceipt>::failure(
                empty_store.error());
        }
        if (!empty_store.value()) {
            return core::Result<PreparedPublicationReceipt>::failure(stale(
                "prepared selector is missing while immutable revisions remain"));
        }
    }

    if (selected.value().has_value()) {
        const auto selected_revision = load_revision(
            revisions / selected.value()->hex(), selected.value().value());
        if (!selected_revision) {
            return core::Result<PreparedPublicationReceipt>::failure(
                selected_revision.error().with_context(
                    "current prepared revision validation"));
        }
        const auto& current = selected_revision.value().envelope;
        if (current.source_namespace != envelope.source_namespace) {
            return core::Result<PreparedPublicationReceipt>::failure(stale(
                "prepared publication root belongs to a different source namespace"));
        }
        if (envelope.source_revision < current.source_revision) {
            return core::Result<PreparedPublicationReceipt>::failure(stale(
                "prepared publication refuses to select an older source revision"));
        }
        if (envelope.source_revision == current.source_revision &&
            envelope.source_digest != current.source_digest) {
            return core::Result<PreparedPublicationReceipt>::failure(stale(
                "prepared source revision conflicts with the selected source digest"));
        }
    }

    const auto final_path = revisions / envelope_digest.value().hex();
    if (auto result = reject_link_chain(final_path); !result) {
        return core::Result<PreparedPublicationReceipt>::failure(result.error());
    }
    bool reused = false;
    const bool final_exists = std::filesystem::exists(final_path, error);
    if (error) {
        return core::Result<PreparedPublicationReceipt>::failure(io_error(
            "unable to inspect immutable prepared revision"));
    }
    if (final_exists) {
        const auto existing = load_revision(final_path, envelope_digest.value());
        if (!existing || existing.value().envelope.serialize() != envelope.serialize()) {
            return core::Result<PreparedPublicationReceipt>::failure(validation(
                "existing immutable prepared revision is corrupt or conflicting"));
        }
        reused = true;
    } else {
        const auto staging = revisions / (".stage-" + unique_suffix());
        if (!std::filesystem::create_directory(staging, error) || error) {
            return core::Result<PreparedPublicationReceipt>::failure(io_error(
                "unable to create prepared revision staging directory"));
        }
        StagingCleanup cleanup(staging);
        for (const auto& product : envelope.products) {
            const auto payload = payload_map.at(product.identity);
            if (auto result = write_bytes(
                    staging / std::filesystem::path(product.relative_path),
                    payload->bytes, "prepared product");
                !result) {
                return core::Result<PreparedPublicationReceipt>::failure(
                    result.error().with_context(product.identity));
            }
        }
        if (auto result = write_text(
                staging / "manifest.carto-prepared", envelope.serialize(),
                "prepared-scene manifest");
            !result) {
            return core::Result<PreparedPublicationReceipt>::failure(result.error());
        }
        const auto verified = load_revision(staging, envelope_digest.value());
        if (!verified) {
            return core::Result<PreparedPublicationReceipt>::failure(
                verified.error().with_context("prepared revision staging verification"));
        }
        std::filesystem::rename(staging, final_path, error);
        if (error) {
            return core::Result<PreparedPublicationReceipt>::failure(io_error(
                "unable to publish immutable prepared revision"));
        }
        cleanup.release();
    }

    if (!selected.value().has_value() ||
        selected.value().value() != envelope_digest.value()) {
        if (auto result = replace_selector_atomically(output_root, envelope_digest.value());
            !result) {
            return core::Result<PreparedPublicationReceipt>::failure(result.error());
        }
    }
    return core::Result<PreparedPublicationReceipt>::success(PreparedPublicationReceipt{
        envelope_digest.value(), selected.value(), envelope.source_revision,
        final_path, reused});
}

core::Result<std::optional<SelectedPreparedScene>> read_selected_prepared_scene(
    const std::filesystem::path& output_root) {
    if (output_root.empty() || output_root.filename().empty()) {
        return core::Result<std::optional<SelectedPreparedScene>>::failure(invalid(
            "prepared publication root must name a directory"));
    }
    if (auto result = reject_link_chain(output_root); !result) {
        return core::Result<std::optional<SelectedPreparedScene>>::failure(result.error());
    }
    const auto selected = read_selector(output_root);
    if (!selected) {
        return core::Result<std::optional<SelectedPreparedScene>>::failure(selected.error());
    }
    if (!selected.value().has_value()) {
        const auto empty_store = revision_store_is_empty(output_root / "revisions");
        if (!empty_store) {
            return core::Result<std::optional<SelectedPreparedScene>>::failure(
                empty_store.error());
        }
        if (!empty_store.value()) {
            return core::Result<std::optional<SelectedPreparedScene>>::failure(
                validation(
                    "prepared selector is missing while immutable revisions remain"));
        }
        return core::Result<std::optional<SelectedPreparedScene>>::success(std::nullopt);
    }
    const auto revision_path =
        output_root / "revisions" / selected.value()->hex();
    const auto revision = load_revision(revision_path, selected.value().value());
    if (!revision) {
        return core::Result<std::optional<SelectedPreparedScene>>::failure(revision.error());
    }
    return core::Result<std::optional<SelectedPreparedScene>>::success(revision.value());
}

} // namespace carto::production
