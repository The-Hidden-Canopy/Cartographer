#include "internal.hpp"

#include <carto/core/json.hpp>
#include <carto/plugin_package/install.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::plugin_package {

namespace {

std::mutex g_install_mutex;
std::atomic<std::uint64_t> g_temp_counter{0U};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

bool safe_component(std::string_view value) {
    if (value.empty() || value.size() > 128U || value == "." || value == ".." ||
        value.find_first_of("/\\:<>\"|?*") != std::string_view::npos ||
        value.back() == '.' || value.back() == ' ' ||
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        }) == false) return false;
    std::string stem(value);
    if (const auto dot = stem.find('.'); dot != std::string::npos) stem.resize(dot);
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char byte) {
        return static_cast<char>(std::toupper(byte));
    });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4U && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        stem.back() >= '1' && stem.back() <= '9') return false;
    return true;
}

std::map<std::string, std::vector<std::uint8_t>> entry_map(
    const std::vector<PackageEntry>& entries) {
    std::map<std::string, std::vector<std::uint8_t>> result;
    for (const auto& entry : entries) result.emplace(entry.path, entry.bytes);
    return result;
}

bool executable_suffix(std::string_view path) {
    const auto separator = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (separator != std::string_view::npos && dot < separator)) return false;
    std::string suffix(path.substr(dot));
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    static constexpr std::array<std::string_view, 10> suffixes{
        ".exe", ".dll", ".so", ".dylib", ".bin", ".wasm", ".com", ".bat", ".cmd", ".ps1"};
    return std::find(suffixes.begin(), suffixes.end(), suffix) != suffixes.end() || suffix == ".sh";
}

bool allowed_fields(
    const std::vector<core::json::Member>& object,
    std::initializer_list<std::string_view> allowed) {
    for (const auto& member : object) {
        if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end()) return false;
    }
    return true;
}

core::Result<std::string> receipt_string(
    const std::vector<core::json::Member>& object,
    std::string_view field,
    std::size_t maximum) {
    const auto* member = core::json::find_member(object, field);
    if (member == nullptr) return core::Result<std::string>::failure(validation("install receipt field is missing: " + std::string(field)));
    const auto value = core::json::decode_string(member->raw_value);
    if (!value || value.value().empty() || value.value().size() > maximum) {
        return core::Result<std::string>::failure(validation("install receipt field is invalid: " + std::string(field)));
    }
    return value;
}

core::Result<std::vector<std::string>> receipt_capabilities(std::string_view raw) {
    if (raw.size() < 2U || raw.front() != '[' || raw.back() != ']') {
        return core::Result<std::vector<std::string>>::failure(validation("install receipt capabilities are invalid"));
    }
    std::vector<std::string> result;
    std::size_t cursor = 1U;
    const auto skip = [&]() {
        while (cursor + 1U < raw.size() &&
               (raw[cursor] == ' ' || raw[cursor] == '\n' || raw[cursor] == '\r' || raw[cursor] == '\t')) ++cursor;
    };
    skip();
    if (cursor + 1U == raw.size()) return core::Result<std::vector<std::string>>::success(std::move(result));
    while (cursor < raw.size() - 1U) {
        if (result.size() >= 64U || raw[cursor] != '"') {
            return core::Result<std::vector<std::string>>::failure(validation("install receipt capabilities contain an invalid value"));
        }
        const std::size_t start = cursor++;
        bool escape = false;
        while (cursor < raw.size() - 1U) {
            const char byte = raw[cursor++];
            if (escape) escape = false;
            else if (byte == '\\') escape = true;
            else if (byte == '"') break;
        }
        if (cursor == 0U || raw[cursor - 1U] != '"') {
            return core::Result<std::vector<std::string>>::failure(validation("install receipt capabilities are unterminated"));
        }
        const auto decoded = core::json::decode_string(raw.substr(start, cursor - start));
        if (!decoded || decoded.value().empty() || decoded.value().size() > 128U) {
            return core::Result<std::vector<std::string>>::failure(validation("install receipt capability is invalid"));
        }
        result.push_back(decoded.value());
        skip();
        if (cursor >= raw.size() - 1U) break;
        if (raw[cursor++] != ',') {
            return core::Result<std::vector<std::string>>::failure(validation("install receipt capabilities have an invalid separator"));
        }
        skip();
    }
    return core::Result<std::vector<std::string>>::success(std::move(result));
}

const char* signature_name(SignatureVerdict verdict) noexcept {
    switch (verdict) {
    case SignatureVerdict::verified: return "verified";
    case SignatureVerdict::unsigned_package: return "unsigned";
    case SignatureVerdict::unknown_key: return "unknown_key";
    case SignatureVerdict::invalid: return "invalid";
    case SignatureVerdict::algorithm_unsupported: return "algorithm_unsupported";
    }
    return "invalid";
}

core::Result<SignatureVerdict> parse_signature_verdict(std::string_view value) {
    if (value == "verified") return core::Result<SignatureVerdict>::success(SignatureVerdict::verified);
    if (value == "unsigned") return core::Result<SignatureVerdict>::success(SignatureVerdict::unsigned_package);
    if (value == "unknown_key") return core::Result<SignatureVerdict>::success(SignatureVerdict::unknown_key);
    if (value == "invalid") return core::Result<SignatureVerdict>::success(SignatureVerdict::invalid);
    if (value == "algorithm_unsupported") return core::Result<SignatureVerdict>::success(SignatureVerdict::algorithm_unsupported);
    return core::Result<SignatureVerdict>::failure(validation("install receipt signature verdict is invalid"));
}

const char* trust_name(TrustClass trust) noexcept {
    switch (trust) {
    case TrustClass::sandboxed_process: return "sandboxed_process";
    case TrustClass::trusted_native: return "trusted_native";
    case TrustClass::data_only: return "data_only";
    }
    return "data_only";
}

core::Result<TrustClass> parse_trust(std::string_view value) {
    if (value == "sandboxed_process") return core::Result<TrustClass>::success(TrustClass::sandboxed_process);
    if (value == "trusted_native") return core::Result<TrustClass>::success(TrustClass::trusted_native);
    if (value == "data_only") return core::Result<TrustClass>::success(TrustClass::data_only);
    return core::Result<TrustClass>::failure(validation("install receipt trust class is invalid"));
}

void append_json_string(std::string& output, std::string_view value) {
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') output.push_back('\\');
        output.push_back(static_cast<char>(byte));
    }
    output.push_back('"');
}

std::string receipt_json(const InstallReceipt& receipt) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        receipt.install_timestamp.time_since_epoch()).count();
    std::string output = "{\"plugin_id\":";
    append_json_string(output, receipt.plugin_id);
    output += ",\"version\":";
    append_json_string(output, receipt.version);
    output += ",\"publisher\":";
    append_json_string(output, receipt.publisher);
    output += ",\"package_digest\":";
    append_json_string(output, receipt.package_digest.hex());
    output += ",\"manifest_digest\":";
    append_json_string(output, receipt.manifest_digest.hex());
    output += ",\"signature_verdict\":";
    append_json_string(output, signature_name(receipt.signature_verdict));
    output += ",\"trust_class\":";
    append_json_string(output, trust_name(receipt.trust_class));
    output += ",\"capabilities\":[";
    for (std::size_t index = 0U; index < receipt.capabilities.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_json_string(output, receipt.capabilities[index]);
    }
    output += "],\"install_timestamp\":" + std::to_string(seconds);
    output += ",\"cartographer_version\":";
    append_json_string(output, receipt.cartographer_version);
    output += "}\n";
    return output;
}

core::Result<InstallReceipt> parse_receipt(std::string_view json) {
    const auto object = core::json::parse_object(json);
    if (!object) return core::Result<InstallReceipt>::failure(object.error());
    if (!allowed_fields(object.value(), {
        "plugin_id", "version", "publisher", "package_digest", "manifest_digest", "signature_verdict",
        "trust_class", "capabilities", "install_timestamp", "cartographer_version"})) {
        return core::Result<InstallReceipt>::failure(validation("install receipt contains an unknown field"));
    }
    const auto plugin_id = receipt_string(object.value(), "plugin_id", 128U);
    const auto version = receipt_string(object.value(), "version", 128U);
    const auto publisher = receipt_string(object.value(), "publisher", 256U);
    const auto package_digest = receipt_string(object.value(), "package_digest", 64U);
    const auto manifest_digest = receipt_string(object.value(), "manifest_digest", 64U);
    const auto verdict = receipt_string(object.value(), "signature_verdict", 64U);
    const auto trust = receipt_string(object.value(), "trust_class", 64U);
    const auto cartographer_version = receipt_string(object.value(), "cartographer_version", 128U);
    if (!plugin_id || !version || !publisher || !package_digest || !manifest_digest || !verdict || !trust || !cartographer_version) {
        return core::Result<InstallReceipt>::failure(!plugin_id ? plugin_id.error() : !version ? version.error() :
            !publisher ? publisher.error() : !package_digest ? package_digest.error() : !manifest_digest ? manifest_digest.error() :
            !verdict ? verdict.error() : !trust ? trust.error() : cartographer_version.error());
    }
    const auto parsed_package_digest = assets::Sha256Digest::from_hex(package_digest.value());
    const auto parsed_manifest_digest = assets::Sha256Digest::from_hex(manifest_digest.value());
    const auto parsed_verdict = parse_signature_verdict(verdict.value());
    const auto parsed_trust = parse_trust(trust.value());
    const auto* capabilities = core::json::find_member(object.value(), "capabilities");
    const auto* timestamp = core::json::find_member(object.value(), "install_timestamp");
    if (!parsed_package_digest || !parsed_manifest_digest || !parsed_verdict || !parsed_trust ||
        capabilities == nullptr || timestamp == nullptr) {
        return core::Result<InstallReceipt>::failure(!parsed_package_digest ? parsed_package_digest.error() :
            !parsed_manifest_digest ? parsed_manifest_digest.error() : !parsed_verdict ? parsed_verdict.error() :
            !parsed_trust ? parsed_trust.error() : validation("install receipt is missing capabilities or timestamp"));
    }
    const auto parsed_capabilities = receipt_capabilities(capabilities->raw_value);
    const auto parsed_timestamp = core::json::parse_uint(timestamp->raw_value);
    if (!parsed_capabilities || !parsed_timestamp || parsed_timestamp.value() >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return core::Result<InstallReceipt>::failure(!parsed_capabilities ? parsed_capabilities.error() :
            !parsed_timestamp ? parsed_timestamp.error() : invalid("install receipt timestamp is out of range"));
    }
    InstallReceipt result{
        plugin_id.value(), version.value(), publisher.value(), parsed_package_digest.value(),
        parsed_manifest_digest.value(), parsed_verdict.value(), parsed_trust.value(), parsed_capabilities.value(),
        std::chrono::system_clock::time_point(std::chrono::seconds(parsed_timestamp.value())),
        cartographer_version.value()};
    if (!safe_component(result.plugin_id) || !safe_component(result.version)) {
        return core::Result<InstallReceipt>::failure(validation("install receipt identity is not path-safe"));
    }
    return core::Result<InstallReceipt>::success(std::move(result));
}

core::Result<std::vector<std::uint8_t>> read_file(
    const std::filesystem::path& path,
    std::uint64_t maximum) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "plugin package file does not exist or is not regular"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum || size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation("plugin package file exceeds its size limit"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to open plugin package file"));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream && !stream.eof()) return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to read plugin package file"));
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<void> write_atomic(
    const std::filesystem::path& destination,
    std::span<const std::uint8_t> bytes) {
    const auto temporary = destination.string() + ".tmp-" +
        std::to_string(g_temp_counter.fetch_add(1U, std::memory_order_relaxed));
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return core::Result<void>::failure(io_error("unable to create plugin install temporary file"));
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        stream.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error("unable to write plugin install temporary file"));
    }
    stream.close();
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return core::Result<void>::failure(io_error("unable to publish plugin install file atomically"));
    }
    return core::Result<void>::success();
}

core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> numeric_version(std::string_view value) {
    std::array<std::uint32_t, 3> parts{};
    std::size_t cursor = 0U;
    for (std::size_t index = 0U; index < parts.size(); ++index) {
        const auto end = value.find('.', cursor);
        const auto stop = end == std::string_view::npos ? value.size() : end;
        if (stop == cursor) return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("version is invalid"));
        const auto parsed = std::from_chars(value.data() + cursor, value.data() + stop, parts[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + stop) return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("version is invalid"));
        if (index + 1U == parts.size() && end != std::string_view::npos) return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("version is invalid"));
        if (index + 1U < parts.size() && end == std::string_view::npos) return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("version is invalid"));
        cursor = end == std::string_view::npos ? value.size() : end + 1U;
    }
    return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::success({parts[0], parts[1], parts[2]});
}

bool version_at_least(std::string_view actual, std::string_view minimum) {
    const auto left = numeric_version(actual);
    const auto right = numeric_version(minimum);
    return left && right && left.value() >= right.value();
}

core::Result<InstallReceipt> read_receipt_path(const std::filesystem::path& path) {
    const auto bytes = read_file(path, 65536U);
    if (!bytes) return core::Result<InstallReceipt>::failure(bytes.error());
    return parse_receipt(std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
}

} // namespace

core::Result<VerificationReport> verify_package(
    std::span<const std::uint8_t> archive,
    const std::vector<PackageEntry>& entries,
    const PluginPackageManifest& manifest,
    const KeyRing& key_ring) {
    if (archive.empty() || archive.size() > 256ULL * 1024ULL * 1024ULL) {
        return core::Result<VerificationReport>::failure(validation("plugin package archive is empty or too large"));
    }
    const auto inspected_entries = read_cartoplug(archive);
    if (!inspected_entries) return core::Result<VerificationReport>::failure(inspected_entries.error());
    const auto map = entry_map(entries);
    const auto inspected_map = entry_map(inspected_entries.value());
    if (map.size() != entries.size() || inspected_map != map) {
        return core::Result<VerificationReport>::failure(
            validation("plugin package entries are not bound to the supplied archive"));
    }
    const auto manifest_entry = map.find("manifest.json");
    if (manifest_entry == map.end()) return core::Result<VerificationReport>::failure(validation("plugin package manifest is missing"));
    const std::string manifest_json(reinterpret_cast<const char*>(manifest_entry->second.data()), manifest_entry->second.size());
    const auto parsed_manifest = parse_manifest(manifest_json);
    if (!parsed_manifest) return core::Result<VerificationReport>::failure(parsed_manifest.error());
    if (canonical_manifest_json(parsed_manifest.value()) != canonical_manifest_json(manifest)) {
        return core::Result<VerificationReport>::failure(validation("plugin package manifest identity does not match the inspected manifest"));
    }
    const auto canonical = canonical_manifest_json(manifest);
    const auto manifest_digest = assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()));
    const auto archive_digest = package_content_digest(map);

    std::set<std::string> declared_entrypoints;
    const auto validate_platforms = [&](const std::map<std::string, PlatformEntry>& platforms) -> core::Result<void> {
        for (const auto& [platform, entrypoint] : platforms) {
            static_cast<void>(platform);
            if (!declared_entrypoints.insert(entrypoint.entrypoint).second) {
                return core::Result<void>::failure(validation("plugin package declares the same entrypoint more than once"));
            }
            const auto found = map.find(entrypoint.entrypoint);
            if (found == map.end()) return core::Result<void>::failure(validation("plugin package declared entrypoint is missing"));
            if (assets::sha256(found->second) != entrypoint.sha256) {
                return core::Result<void>::failure(validation("plugin package entrypoint digest does not match its manifest"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto valid = validate_platforms(manifest.platforms); !valid) return core::Result<VerificationReport>::failure(valid.error());
    if (manifest.vanta.has_value()) {
        if (auto valid = validate_platforms(manifest.vanta->platforms); !valid) return core::Result<VerificationReport>::failure(valid.error());
    }
    for (const auto& [path, bytes] : map) {
        static_cast<void>(bytes);
        if (executable_suffix(path) && declared_entrypoints.find(path) == declared_entrypoints.end()) {
            return core::Result<VerificationReport>::failure(validation("plugin package contains an undeclared executable"));
        }
    }
    if (manifest.trust_class == TrustClass::data_only && (!manifest.platforms.empty() || manifest.vanta.has_value())) {
        return core::Result<VerificationReport>::failure(validation("data-only plugin package contains executable declarations"));
    }
    const auto protocol_manifest = to_plugin_manifest(manifest);
    if (!protocol_manifest) return core::Result<VerificationReport>::failure(protocol_manifest.error());
    const auto verdict = verify_package_signature(map, manifest_digest, archive_digest, key_ring);
    if (!verdict) return core::Result<VerificationReport>::failure(verdict.error());
    return core::Result<VerificationReport>::success(VerificationReport{manifest_digest, archive_digest, verdict.value()});
}

PluginInstallStore::PluginInstallStore(std::filesystem::path root)
    : root_(std::move(root)) {}

core::Result<InstallReceipt> PluginInstallStore::install(
    const std::filesystem::path& package_path,
    const InstallOptions& options) {
    std::scoped_lock lock(g_install_mutex);
    const auto archive = read_file(package_path, 256ULL * 1024ULL * 1024ULL);
    if (!archive) return core::Result<InstallReceipt>::failure(archive.error());
    const auto entries = read_cartoplug(archive.value());
    if (!entries) return core::Result<InstallReceipt>::failure(entries.error());
    const auto map = entry_map(entries.value());
    const auto manifest_entry = map.find("manifest.json");
    if (manifest_entry == map.end()) return core::Result<InstallReceipt>::failure(validation("plugin package manifest is missing"));
    const auto manifest = parse_manifest(std::string_view(
        reinterpret_cast<const char*>(manifest_entry->second.data()), manifest_entry->second.size()));
    if (!manifest) return core::Result<InstallReceipt>::failure(manifest.error());
    const auto verification = verify_package(archive.value(), entries.value(), manifest.value(), options.key_ring);
    if (!verification) return core::Result<InstallReceipt>::failure(verification.error());
    if (verification.value().signature_verdict != SignatureVerdict::verified &&
        !(verification.value().signature_verdict == SignatureVerdict::unsigned_package && options.developer_mode &&
          manifest.value().trust_class != TrustClass::trusted_native)) {
        return core::Result<InstallReceipt>::failure(validation("plugin package signature is not trusted for this install mode"));
    }
    if (!version_at_least(
#ifdef CARTOGRAPHER_VERSION
        CARTOGRAPHER_VERSION,
#else
        "0.1.0",
#endif
        manifest.value().min_cartographer_version)) {
        return core::Result<InstallReceipt>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "plugin package requires a newer Cartographer version"));
    }
    if (!safe_component(manifest.value().plugin_id) || !safe_component(manifest.value().version)) {
        return core::Result<InstallReceipt>::failure(invalid("plugin package identity is not path-safe"));
    }
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error || std::filesystem::is_symlink(root_, error) || error ||
        !std::filesystem::is_directory(root_, error) || error) {
        return core::Result<InstallReceipt>::failure(io_error("plugin install root is missing, not a directory, or is a symlink"));
    }
    const auto plugin_root = root_ / manifest.value().plugin_id;
    const auto version_root = plugin_root / manifest.value().version;
    if (std::filesystem::exists(plugin_root, error)) {
        if (error || std::filesystem::is_symlink(plugin_root, error) || error ||
            !std::filesystem::is_directory(plugin_root, error) || error) {
            return core::Result<InstallReceipt>::failure(io_error("plugin install identity resolves through an invalid directory"));
        }
    }
    if (std::filesystem::exists(version_root, error) || error) {
        return core::Result<InstallReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "plugin version is already installed"));
    }
    std::filesystem::create_directories(version_root, error);
    if (error) return core::Result<InstallReceipt>::failure(io_error("unable to create plugin install directory"));
    const auto package_destination = version_root / "package.cartoplug";
    const auto package_bytes = std::span<const std::uint8_t>(archive.value().data(), archive.value().size());
    if (auto written = write_atomic(package_destination, package_bytes); !written) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(version_root, cleanup_error);
        return core::Result<InstallReceipt>::failure(written.error());
    }
    InstallReceipt receipt{
        manifest.value().plugin_id,
        manifest.value().version,
        manifest.value().publisher,
        assets::sha256(package_bytes),
        verification.value().manifest_digest,
        verification.value().signature_verdict,
        manifest.value().trust_class,
        manifest.value().capabilities,
        std::chrono::system_clock::now(),
#ifdef CARTOGRAPHER_VERSION
        CARTOGRAPHER_VERSION
#else
        "0.1.0"
#endif
    };
    const auto receipt_text = receipt_json(receipt);
    const auto receipt_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(receipt_text.data()), receipt_text.size());
    if (auto written = write_atomic(version_root / "install_receipt.json", receipt_bytes); !written) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(version_root, cleanup_error);
        return core::Result<InstallReceipt>::failure(written.error());
    }
    return core::Result<InstallReceipt>::success(std::move(receipt));
}

core::Result<void> PluginInstallStore::remove(
    std::string_view plugin_id,
    std::optional<std::string_view> version) {
    std::scoped_lock lock(g_install_mutex);
    if (!safe_component(plugin_id) || (version.has_value() && !safe_component(version.value()))) {
        return core::Result<void>::failure(invalid("plugin removal identity is not path-safe"));
    }
    const auto target = root_ / std::string(plugin_id) /
        (version.has_value() ? std::filesystem::path(std::string(version.value())) : std::filesystem::path{});
    std::error_code error;
    if (!std::filesystem::exists(target, error) || error) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::not_found, "plugin installation does not exist"));
    }
    std::filesystem::remove_all(target, error);
    if (error) return core::Result<void>::failure(io_error("unable to remove plugin installation"));
    if (!version.has_value()) {
        return core::Result<void>::success();
    }
    const auto plugin_root = root_ / std::string(plugin_id);
    if (std::filesystem::is_empty(plugin_root, error) && !error) static_cast<void>(std::filesystem::remove(plugin_root, error));
    return error ? core::Result<void>::failure(io_error("unable to remove empty plugin directory")) : core::Result<void>::success();
}

core::Result<std::vector<InstallReceipt>> PluginInstallStore::list() const {
    std::scoped_lock lock(g_install_mutex);
    std::vector<InstallReceipt> receipts;
    std::error_code error;
    if (!std::filesystem::exists(root_, error)) {
        if (error) return core::Result<std::vector<InstallReceipt>>::failure(io_error("unable to inspect plugin install root"));
        return core::Result<std::vector<InstallReceipt>>::success(std::move(receipts));
    }
    if (!std::filesystem::is_directory(root_, error) || error) {
        return core::Result<std::vector<InstallReceipt>>::failure(io_error("plugin install root is not a directory"));
    }
    for (const auto& plugin : std::filesystem::directory_iterator(root_, error)) {
        if (error) return core::Result<std::vector<InstallReceipt>>::failure(io_error("unable to enumerate plugin install root"));
        if (plugin.is_symlink(error) || error || !plugin.is_directory(error) || error ||
            !safe_component(plugin.path().filename().string())) continue;
        for (const auto& version : std::filesystem::directory_iterator(plugin.path(), error)) {
            if (error) return core::Result<std::vector<InstallReceipt>>::failure(io_error("unable to enumerate plugin versions"));
            if (version.is_symlink(error) || error || !version.is_directory(error) || error) continue;
            const auto receipt = read_receipt_path(version.path() / "install_receipt.json");
            if (!receipt) return core::Result<std::vector<InstallReceipt>>::failure(receipt.error());
            if (receipt.value().plugin_id != plugin.path().filename().string() ||
                receipt.value().version != version.path().filename().string()) {
                return core::Result<std::vector<InstallReceipt>>::failure(
                    validation("install receipt identity does not match its directory"));
            }
            const auto package = read_file(version.path() / "package.cartoplug", 256ULL * 1024ULL * 1024ULL);
            if (!package || assets::sha256(package.value()) != receipt.value().package_digest) {
                return core::Result<std::vector<InstallReceipt>>::failure(
                    validation("install receipt package digest is stale or missing"));
            }
            const auto entries = read_cartoplug(package.value());
            if (!entries) return core::Result<std::vector<InstallReceipt>>::failure(entries.error());
            const auto package_entries = entry_map(entries.value());
            const auto manifest_entry = package_entries.find("manifest.json");
            if (manifest_entry == package_entries.end()) {
                return core::Result<std::vector<InstallReceipt>>::failure(
                    validation("installed plugin package manifest is missing"));
            }
            const auto manifest = parse_manifest(std::string_view(
                reinterpret_cast<const char*>(manifest_entry->second.data()), manifest_entry->second.size()));
            if (!manifest) return core::Result<std::vector<InstallReceipt>>::failure(manifest.error());
            const auto canonical = canonical_manifest_json(manifest.value());
            const auto manifest_digest = assets::sha256(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()));
            if (receipt.value().plugin_id != manifest.value().plugin_id ||
                receipt.value().version != manifest.value().version ||
                receipt.value().publisher != manifest.value().publisher ||
                receipt.value().manifest_digest != manifest_digest ||
                receipt.value().trust_class != manifest.value().trust_class ||
                receipt.value().capabilities != manifest.value().capabilities) {
                return core::Result<std::vector<InstallReceipt>>::failure(
                    validation("install receipt is not bound to its package manifest"));
            }
            const bool has_signature = package_entries.find("signature.json") != package_entries.end();
            if ((!has_signature && receipt.value().signature_verdict != SignatureVerdict::unsigned_package) ||
                (has_signature && receipt.value().signature_verdict == SignatureVerdict::unsigned_package)) {
                return core::Result<std::vector<InstallReceipt>>::failure(
                    validation("install receipt signature verdict does not match package contents"));
            }
            receipts.push_back(receipt.value());
        }
    }
    std::sort(receipts.begin(), receipts.end(), [](const InstallReceipt& left, const InstallReceipt& right) {
        return std::tie(left.plugin_id, left.version) < std::tie(right.plugin_id, right.version);
    });
    return core::Result<std::vector<InstallReceipt>>::success(std::move(receipts));
}

core::Result<std::optional<InstallReceipt>> PluginInstallStore::find(std::string_view plugin_id) const {
    if (!safe_component(plugin_id)) return core::Result<std::optional<InstallReceipt>>::failure(invalid("plugin lookup identity is not path-safe"));
    const auto all = list();
    if (!all) return core::Result<std::optional<InstallReceipt>>::failure(all.error());
    std::optional<InstallReceipt> found;
    for (const auto& receipt : all.value()) {
        if (receipt.plugin_id != plugin_id) continue;
        if (!found.has_value()) {
            found = receipt;
            continue;
        }
        const auto candidate_version = numeric_version(receipt.version);
        const auto found_version = numeric_version(found->version);
        if (candidate_version && (!found_version || candidate_version.value() > found_version.value() ||
            (candidate_version.value() == found_version.value() && receipt.version > found->version))) {
            found = receipt;
        }
    }
    return core::Result<std::optional<InstallReceipt>>::success(std::move(found));
}

PluginManager::PluginManager(PluginInstallStore store)
    : store_(std::move(store)) {}

core::Result<InstallReceipt> PluginManager::install(
    const std::filesystem::path& package_path,
    const InstallOptions& options) {
    return store_.install(package_path, options);
}

core::Result<void> PluginManager::remove(
    std::string_view plugin_id,
    std::optional<std::string_view> version) {
    return store_.remove(plugin_id, version);
}

core::Result<std::vector<InstallReceipt>> PluginManager::list() const {
    return store_.list();
}

core::Result<std::optional<InstallReceipt>> PluginManager::find(std::string_view plugin_id) const {
    return store_.find(plugin_id);
}

} // namespace carto::plugin_package
