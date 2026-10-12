#include <carto/market/market.hpp>

#include <carto/core/json.hpp>
#include <carto/plugin_package/package.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::market {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic conflict(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

bool printable(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

bool safe_token(std::string_view value) {
    return printable(value, kMaxMarketStringBytes) &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-' || byte == ':';
        });
}

bool safe_package_path(std::string_view value) {
    if (value.empty() || !printable(value, kMaxMarketStringBytes) || value.front() == '/' ||
        value.find('\\') != std::string_view::npos || value.find(':') != std::string_view::npos ||
        value.ends_with('/')) {
        return false;
    }
    std::size_t cursor = 0U;
    while (cursor < value.size()) {
        const std::size_t separator = value.find('/', cursor);
        const std::size_t end = separator == std::string_view::npos ? value.size() : separator;
        const auto component = value.substr(cursor, end - cursor);
        if (component.empty() || component == "." || component == "..") return false;
        cursor = separator == std::string_view::npos ? value.size() : separator + 1U;
    }
    return true;
}

bool safe_library_identity(std::string_view value) {
    return safe_token(value) && safe_package_path(value);
}

std::string quote(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[byte >> 4U]);
                result.push_back(digits[byte & 0x0fU]);
            } else {
                result.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string digest_text(const assets::Sha256Digest& digest) {
    return "sha256:" + digest.hex();
}

std::string bytes_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::optional<std::uint8_t> hex_nibble(char value) {
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
    return std::nullopt;
}

core::Result<std::array<std::uint8_t, 64>> parse_signature_bytes(
    const std::vector<core::json::Member>& object) {
    const auto* member = core::json::find_member(object, "signature");
    if (member == nullptr) {
        return core::Result<std::array<std::uint8_t, 64>>::failure(
            validation("market publication signature is missing"));
    }
    const auto encoded = core::json::decode_string(member->raw_value);
    if (!encoded || encoded.value().size() != 128U) {
        return core::Result<std::array<std::uint8_t, 64>>::failure(
            validation("market publication signature must contain exactly 64 bytes"));
    }
    std::array<std::uint8_t, 64> result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto high = hex_nibble(encoded.value()[index * 2U]);
        const auto low = hex_nibble(encoded.value()[index * 2U + 1U]);
        if (!high || !low) {
            return core::Result<std::array<std::uint8_t, 64>>::failure(
                validation("market publication signature is not hexadecimal"));
        }
        result[index] = static_cast<std::uint8_t>(high.value() * 16U + low.value());
    }
    return core::Result<std::array<std::uint8_t, 64>>::success(result);
}

void append_u32_le(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_publication_text(std::vector<std::uint8_t>& output, std::string_view value) {
    append_u32_le(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

core::Result<assets::Sha256Digest> parse_digest(
    std::string_view value,
    std::string_view field) {
    if (!value.starts_with("sha256:")) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("market digest must use sha256 prefix: " + std::string(field)));
    }
    const auto parsed = assets::Sha256Digest::from_hex(value.substr(7U));
    if (!parsed || parsed.value().is_zero()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("market digest is invalid: " + std::string(field)));
    }
    return parsed;
}

bool allowed_fields(
    const std::vector<core::json::Member>& object,
    std::initializer_list<std::string_view> allowed) {
    return std::all_of(object.begin(), object.end(), [&allowed](const auto& member) {
        return std::find(allowed.begin(), allowed.end(), member.key) != allowed.end();
    });
}

core::Result<std::string> required_string(
    const std::vector<core::json::Member>& object,
    std::string_view field,
    std::size_t maximum,
    bool token = false) {
    const auto* member = core::json::find_member(object, field);
    if (member == nullptr) {
        return core::Result<std::string>::failure(
            validation("market field is missing: " + std::string(field)));
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded || !printable(decoded.value(), maximum)) {
        return core::Result<std::string>::failure(
            validation("market string field is invalid: " + std::string(field)));
    }
    if (token && !safe_token(decoded.value())) {
        return core::Result<std::string>::failure(
            validation("market token field is invalid: " + std::string(field)));
    }
    return decoded;
}

core::Result<std::optional<std::string>> optional_string(
    const std::vector<core::json::Member>& object,
    std::string_view field,
    std::size_t maximum) {
    const auto* member = core::json::find_member(object, field);
    if (member == nullptr) return core::Result<std::optional<std::string>>::success(std::nullopt);
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded || !printable(decoded.value(), maximum)) {
        return core::Result<std::optional<std::string>>::failure(
            validation("market optional string field is invalid: " + std::string(field)));
    }
    return core::Result<std::optional<std::string>>::success(decoded.value());
}

core::Result<std::uint64_t> required_uint(
    const std::vector<core::json::Member>& object,
    std::string_view field) {
    const auto* member = core::json::find_member(object, field);
    if (member == nullptr) {
        return core::Result<std::uint64_t>::failure(
            validation("market integer field is missing: " + std::string(field)));
    }
    const auto parsed = core::json::parse_uint(member->raw_value);
    if (!parsed) {
        return core::Result<std::uint64_t>::failure(
            validation("market integer field is invalid: " + std::string(field)));
    }
    return parsed;
}

core::Result<std::uint32_t> required_uint32(
    const std::vector<core::json::Member>& object,
    std::string_view field) {
    const auto parsed = required_uint(object, field);
    if (!parsed || parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure(
            validation("market uint32 field is invalid: " + std::string(field)));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

core::Result<bool> optional_bool(
    const std::vector<core::json::Member>& object,
    std::string_view field,
    bool default_value) {
    const auto* member = core::json::find_member(object, field);
    if (member == nullptr) return core::Result<bool>::success(default_value);
    if (member->raw_value == "true") return core::Result<bool>::success(true);
    if (member->raw_value == "false") return core::Result<bool>::success(false);
    return core::Result<bool>::failure(
        validation("market boolean field is invalid: " + std::string(field)));
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\n' ||
                              value.front() == '\r' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\n' ||
                              value.back() == '\r' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

core::Result<std::vector<std::string_view>> array_items(
    std::string_view raw,
    std::string_view field) {
    if (raw.size() < 2U || raw.front() != '[' || raw.back() != ']') {
        return core::Result<std::vector<std::string_view>>::failure(
            validation("market array is invalid: " + std::string(field)));
    }
    std::vector<std::string_view> items;
    std::size_t cursor = 1U;
    while (cursor < raw.size() - 1U) {
        while (cursor < raw.size() - 1U &&
               (raw[cursor] == ' ' || raw[cursor] == '\n' || raw[cursor] == '\r' || raw[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor == raw.size() - 1U) break;
        const std::size_t start = cursor;
        int object_depth = 0;
        int array_depth = 0;
        bool in_string = false;
        bool escaped = false;
        bool ended = false;
        while (cursor < raw.size() - 1U) {
            const char byte = raw[cursor];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (byte == '\\') {
                    escaped = true;
                } else if (byte == '"') {
                    in_string = false;
                }
                ++cursor;
                continue;
            }
            if (byte == '"') {
                in_string = true;
                ++cursor;
                continue;
            }
            if (byte == '{') ++object_depth;
            else if (byte == '}') {
                if (object_depth == 0) {
                    return core::Result<std::vector<std::string_view>>::failure(
                        validation("market array contains an unmatched object close: " + std::string(field)));
                }
                --object_depth;
            } else if (byte == '[') {
                ++array_depth;
            } else if (byte == ']') {
                if (array_depth > 0) {
                    --array_depth;
                } else if (object_depth == 0) {
                    ended = true;
                    break;
                }
            } else if (byte == ',' && object_depth == 0 && array_depth == 0) {
                ended = true;
                break;
            }
            ++cursor;
        }
        const auto item = trim(raw.substr(start, cursor - start));
        if (item.empty()) {
            return core::Result<std::vector<std::string_view>>::failure(
                validation("market array has an empty item: " + std::string(field)));
        }
        if (items.size() >= kMaxMarketArrayItems) {
            return core::Result<std::vector<std::string_view>>::failure(
                validation("market array exceeds its item limit: " + std::string(field)));
        }
        items.push_back(item);
        if (cursor >= raw.size() - 1U) break;
        if (raw[cursor] == ',') {
            ++cursor;
            if (cursor == raw.size() - 1U) {
                return core::Result<std::vector<std::string_view>>::failure(
                    validation("market array has a trailing separator: " + std::string(field)));
            }
        } else if (raw[cursor] == ']') {
            break;
        } else if (!ended) {
            return core::Result<std::vector<std::string_view>>::failure(
                validation("market array is unterminated: " + std::string(field)));
        }
    }
    return core::Result<std::vector<std::string_view>>::success(std::move(items));
}

core::Result<std::optional<assets::Sha256Digest>> optional_digest(
    const std::vector<core::json::Member>& object,
    std::string_view field) {
    const auto* member = core::json::find_member(object, field);
    if (member == nullptr) return core::Result<std::optional<assets::Sha256Digest>>::success(std::nullopt);
    const auto text = core::json::decode_string(member->raw_value);
    if (!text) return core::Result<std::optional<assets::Sha256Digest>>::failure(text.error());
    const auto parsed = parse_digest(text.value(), field);
    if (!parsed) return core::Result<std::optional<assets::Sha256Digest>>::failure(parsed.error());
    return core::Result<std::optional<assets::Sha256Digest>>::success(parsed.value());
}

core::Result<PackageEntrypoint> parse_entrypoint(std::string_view raw) {
    const auto object = core::json::parse_object(raw);
    if (!object) return core::Result<PackageEntrypoint>::failure(object.error());
    if (!allowed_fields(object.value(), {"kind", "path"})) {
        return core::Result<PackageEntrypoint>::failure(validation("market entrypoint has an unknown field"));
    }
    const auto kind = required_string(object.value(), "kind", kMaxMarketStringBytes, true);
    const auto path = required_string(object.value(), "path", kMaxMarketStringBytes);
    if (!kind || !path) return core::Result<PackageEntrypoint>::failure(!kind ? kind.error() : path.error());
    PackageEntrypoint result{kind.value(), path.value()};
    if (auto valid = result.validate(); !valid) {
        return core::Result<PackageEntrypoint>::failure(valid.error());
    }
    return core::Result<PackageEntrypoint>::success(std::move(result));
}

core::Result<TargetArtifact> parse_target(std::string_view raw) {
    const auto object = core::json::parse_object(raw);
    if (!object) return core::Result<TargetArtifact>::failure(object.error());
    if (!allowed_fields(object.value(), {"engine", "profile", "artifact", "artifact_digest"})) {
        return core::Result<TargetArtifact>::failure(validation("market target has an unknown field"));
    }
    const auto engine = required_string(object.value(), "engine", kMaxMarketStringBytes, true);
    const auto profile = required_string(object.value(), "profile", kMaxMarketStringBytes, true);
    const auto artifact = required_string(object.value(), "artifact", kMaxMarketStringBytes);
    const auto digest = optional_digest(object.value(), "artifact_digest");
    if (!engine || !profile || !artifact || !digest) {
        return core::Result<TargetArtifact>::failure(
            !engine ? engine.error() : !profile ? profile.error() : !artifact ? artifact.error() : digest.error());
    }
    TargetArtifact result{engine.value(), profile.value(), artifact.value(), digest.value()};
    if (auto valid = result.validate(); !valid) {
        return core::Result<TargetArtifact>::failure(valid.error());
    }
    return core::Result<TargetArtifact>::success(std::move(result));
}

core::Result<DependencySpec> parse_dependency(std::string_view raw) {
    const auto object = core::json::parse_object(raw);
    if (!object) return core::Result<DependencySpec>::failure(object.error());
    if (!allowed_fields(object.value(), {"package_id", "version", "version_constraint", "optional"})) {
        return core::Result<DependencySpec>::failure(validation("market dependency has an unknown field"));
    }
    const auto package_id = required_string(object.value(), "package_id", kMaxMarketStringBytes, true);
    const auto* version_member = core::json::find_member(object.value(), "version_constraint");
    if (version_member == nullptr) version_member = core::json::find_member(object.value(), "version");
    if (!package_id || version_member == nullptr) {
        return core::Result<DependencySpec>::failure(
            !package_id ? package_id.error() : validation("market dependency version is missing"));
    }
    const auto version = core::json::decode_string(version_member->raw_value);
    const auto optional = optional_bool(object.value(), "optional", false);
    if (!version || !optional || !printable(version.value(), kMaxMarketStringBytes)) {
        return core::Result<DependencySpec>::failure(
            !version ? version.error() : !optional ? optional.error() : invalid("market dependency version is invalid"));
    }
    DependencySpec result{package_id.value(), version.value(), optional.value()};
    if (auto valid = result.validate(); !valid) {
        return core::Result<DependencySpec>::failure(valid.error());
    }
    return core::Result<DependencySpec>::success(std::move(result));
}

core::Result<InventoryItem> parse_inventory_item(std::string_view key, std::string_view raw) {
    if (!safe_token(key)) {
        return core::Result<InventoryItem>::failure(validation("market inventory key is invalid"));
    }
    const auto count = core::json::parse_uint(raw);
    if (!count) return core::Result<InventoryItem>::failure(count.error());
    InventoryItem result{std::string(key), count.value()};
    if (auto valid = result.validate(); !valid) {
        return core::Result<InventoryItem>::failure(valid.error());
    }
    return core::Result<InventoryItem>::success(std::move(result));
}

std::string join_array(const std::vector<std::string>& values) {
    std::string result = "[";
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) result.push_back(',');
        result += values[index];
    }
    result.push_back(']');
    return result;
}

std::string uint_text(std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), converted.ptr);
}

std::string uint_text32(std::uint32_t value) {
    return uint_text(static_cast<std::uint64_t>(value));
}

struct Semver {
    std::uint32_t major = 0U;
    std::uint32_t minor = 0U;
    std::uint32_t patch = 0U;

    [[nodiscard]] auto operator<=>(const Semver&) const noexcept = default;
};

core::Result<Semver> parse_semver(std::string_view text, std::string_view field) {
    Semver result;
    std::array<std::uint32_t*, 3> parts{&result.major, &result.minor, &result.patch};
    std::size_t cursor = 0U;
    for (std::size_t index = 0U; index < parts.size(); ++index) {
        const std::size_t end = text.find('.', cursor);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        if (stop == cursor) {
            return core::Result<Semver>::failure(
                invalid("market version is invalid: " + std::string(field)));
        }
        const auto parsed = std::from_chars(text.data() + cursor, text.data() + stop, *parts[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + stop) {
            return core::Result<Semver>::failure(
                invalid("market version is invalid: " + std::string(field)));
        }
        if (index + 1U == parts.size()) {
            if (end != std::string_view::npos) {
                return core::Result<Semver>::failure(
                    invalid("market version has too many components: " + std::string(field)));
            }
        } else if (end == std::string_view::npos) {
            return core::Result<Semver>::failure(
                invalid("market version has too few components: " + std::string(field)));
        }
        cursor = end == std::string_view::npos ? text.size() : end + 1U;
    }
    return core::Result<Semver>::success(result);
}

bool version_matches(std::string_view version, std::string_view constraint) {
    if (constraint == "*") return true;
    bool caret = false;
    bool tilde = false;
    if (!constraint.empty() && (constraint.front() == '^' || constraint.front() == '~')) {
        caret = constraint.front() == '^';
        tilde = constraint.front() == '~';
        constraint.remove_prefix(1U);
    }
    const auto candidate = parse_semver(version, "candidate");
    const auto requested = parse_semver(constraint, "constraint");
    if (!candidate || !requested) return false;
    if (!caret && !tilde) return candidate.value() == requested.value();
    if (candidate.value() < requested.value()) return false;
    if (caret) return candidate.value().major == requested.value().major;
    return candidate.value().major == requested.value().major &&
        candidate.value().minor == requested.value().minor;
}

bool executable_path(std::string_view value) {
    const auto dot = value.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    std::string extension(value.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    static constexpr std::array<std::string_view, 9> extensions{
        ".bat", ".cmd", ".dll", ".dylib", ".exe", ".js", ".ps1", ".sh", ".wasm"};
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

struct PayloadRecord {
    std::string path;
    std::uint64_t bytes = 0U;
    assets::Sha256Digest digest;
    bool executable = false;
};

core::Result<std::vector<std::uint8_t>> read_file_bytes(
    const std::filesystem::path& path,
    std::uint64_t maximum) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to inspect package file"));
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            validation("market package file must be a regular non-symlink file"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to inspect package file size"));
    if (size > maximum || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation("package file exceeds its size limit"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to open package file"));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size != 0U) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input && !input.eof()) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to read package file"));
    }
    if (input.gcount() != static_cast<std::streamsize>(size) && size != 0U) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error("package file read was truncated"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<void> ensure_directory(const std::filesystem::path& path) {
    std::error_code error;
    auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        error.clear();
        std::filesystem::create_directories(path, error);
        if (error) return core::Result<void>::failure(io_error("unable to create market directory"));
        status = std::filesystem::symlink_status(path, error);
    }
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        return core::Result<void>::failure(
            io_error("market directory is missing, not a directory, or is a symlink"));
    }
    return core::Result<void>::success();
}

core::Result<void> reject_symlink(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return core::Result<void>::success();
    }
    if (error) return core::Result<void>::failure(io_error("unable to inspect market path"));
    if (std::filesystem::is_symlink(status)) {
        return core::Result<void>::failure(
            validation("market library path must not resolve through a symbolic link"));
    }
    return core::Result<void>::success();
}

std::string payload_canonical(const std::vector<PayloadRecord>& records) {
    std::string canonical;
    for (const auto& record : records) {
        canonical += record.path;
        canonical.push_back('\n');
        canonical += uint_text(record.bytes);
        canonical.push_back('\n');
        canonical += record.digest.hex();
        canonical.push_back('\n');
    }
    return canonical;
}

assets::Sha256Digest payload_digest(const std::vector<PayloadRecord>& records) {
    const std::string canonical = payload_canonical(records);
    const auto* data = reinterpret_cast<const std::uint8_t*>(canonical.data());
    return assets::sha256(std::span<const std::uint8_t>{data, canonical.size()});
}

assets::Sha256Digest market_package_identity_digest(const MarketPackageManifest& manifest) {
    const std::string manifest_text = manifest.serialize();
    std::string canonical = "cartographer-market-package-identity.v1\n";
    canonical += uint_text(static_cast<std::uint64_t>(manifest_text.size()));
    canonical.push_back('\n');
    canonical += manifest_text;
    const auto* data = reinterpret_cast<const std::uint8_t*>(canonical.data());
    return assets::sha256(std::span<const std::uint8_t>{data, canonical.size()});
}

core::Result<std::vector<PayloadRecord>> collect_directory_payload(
    const std::filesystem::path& root,
    const MarketValidationLimits& limits) {
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error || root_status.type() == std::filesystem::file_type::not_found ||
        std::filesystem::is_symlink(root_status) || !std::filesystem::is_directory(root_status)) {
        return core::Result<std::vector<PayloadRecord>>::failure(
            validation("market package root is not a directory"));
    }
    std::vector<PayloadRecord> records;
    std::uint64_t total = 0U;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) return core::Result<std::vector<PayloadRecord>>::failure(io_error("unable to enumerate market package"));
    for (const auto& entry : iterator) {
        const auto status = entry.symlink_status(error);
        if (error) return core::Result<std::vector<PayloadRecord>>::failure(io_error("unable to inspect market package entry"));
        if (std::filesystem::is_symlink(status)) {
            return core::Result<std::vector<PayloadRecord>>::failure(
                validation("market package must not contain symbolic links"));
        }
        if (std::filesystem::is_directory(status)) continue;
        if (!std::filesystem::is_regular_file(status)) {
            return core::Result<std::vector<PayloadRecord>>::failure(
                validation("market package contains a non-regular entry"));
        }
        const auto relative = std::filesystem::relative(entry.path(), root, error);
        if (error || relative.empty()) {
            return core::Result<std::vector<PayloadRecord>>::failure(io_error("unable to resolve market package path"));
        }
        const std::string path = relative.generic_string();
        if (!safe_package_path(path)) {
            return core::Result<std::vector<PayloadRecord>>::failure(
                validation("market package contains an unsafe relative path"));
        }
        if (path == "manifest.json") continue;
        if (records.size() >= limits.max_files) {
            return core::Result<std::vector<PayloadRecord>>::failure(validation("market package contains too many files"));
        }
        const auto bytes = read_file_bytes(entry.path(), limits.max_single_file_bytes);
        if (!bytes) return core::Result<std::vector<PayloadRecord>>::failure(bytes.error().with_context(path));
        if (bytes.value().size() > limits.max_total_payload_bytes -
            std::min(limits.max_total_payload_bytes, total)) {
            return core::Result<std::vector<PayloadRecord>>::failure(validation("market package payload exceeds its size limit"));
        }
        total += static_cast<std::uint64_t>(bytes.value().size());
        const auto* data = bytes.value().data();
        records.push_back(PayloadRecord{
            path,
            static_cast<std::uint64_t>(bytes.value().size()),
            assets::sha256(std::span<const std::uint8_t>{data, bytes.value().size()}),
            executable_path(path)});
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
    return core::Result<std::vector<PayloadRecord>>::success(std::move(records));
}

core::Result<assets::Sha256Digest> directory_target_digest(
    const std::filesystem::path& root,
    std::string_view artifact,
    const MarketValidationLimits& limits) {
    const auto target = root / std::filesystem::path(artifact);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(target, error);
    if (error || status.type() == std::filesystem::file_type::not_found ||
        std::filesystem::is_symlink(status)) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("market target artifact is missing or is a symbolic link"));
    }
    if (std::filesystem::is_regular_file(status)) {
        const auto bytes = read_file_bytes(target, limits.max_single_file_bytes);
        if (!bytes) return core::Result<assets::Sha256Digest>::failure(bytes.error());
        return core::Result<assets::Sha256Digest>::success(
            assets::sha256(std::span<const std::uint8_t>{bytes.value().data(), bytes.value().size()}));
    }
    if (!std::filesystem::is_directory(status)) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("market target artifact is not a regular file or directory"));
    }
    const auto records = collect_directory_payload(target, limits);
    if (!records) return core::Result<assets::Sha256Digest>::failure(records.error());
    return core::Result<assets::Sha256Digest>::success(payload_digest(records.value()));
}

core::Result<assets::Sha256Digest> archive_target_digest(
    const std::vector<PayloadRecord>& records,
    std::string_view artifact) {
    const auto exact = std::find_if(records.begin(), records.end(), [artifact](const auto& record) {
        return record.path == artifact;
    });
    if (exact != records.end()) return core::Result<assets::Sha256Digest>::success(exact->digest);
    const std::string prefix = std::string(artifact) + "/";
    std::vector<PayloadRecord> children;
    for (const auto& record : records) {
        if (!record.path.starts_with(prefix)) continue;
        auto child = record;
        child.path.erase(0U, prefix.size());
        children.push_back(std::move(child));
    }
    if (children.empty()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("market archive target artifact is missing"));
    }
    return core::Result<assets::Sha256Digest>::success(payload_digest(children));
}

core::Result<void> validate_directory_target_digests(
    const std::filesystem::path& root,
    const MarketPackageManifest& manifest,
    const MarketValidationLimits& limits) {
    for (const auto& target : manifest.targets) {
        if (!target.digest.has_value()) continue;
        const auto computed = directory_target_digest(root, target.artifact, limits);
        if (!computed) return core::Result<void>::failure(computed.error());
        if (computed.value() != target.digest.value()) {
            return core::Result<void>::failure(
                validation("market target artifact digest mismatch: " + target.artifact));
        }
    }
    return core::Result<void>::success();
}

core::Result<std::string> read_text_file(
    const std::filesystem::path& path,
    std::uint64_t maximum) {
    const auto bytes = read_file_bytes(path, maximum);
    if (!bytes) return core::Result<std::string>::failure(bytes.error());
    return core::Result<std::string>::success(std::string(
        reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
}

std::atomic<std::uint64_t> g_install_counter{0U};

std::filesystem::path install_receipt_path(
    const std::filesystem::path& root,
    std::string_view package_id,
    std::string_view listing_version_id) {
    return root / "receipts" / std::filesystem::path(package_id) /
        (std::string(listing_version_id) + ".json");
}

std::filesystem::path publication_receipt_path(
    const std::filesystem::path& root,
    std::string_view package_id,
    std::string_view listing_version_id) {
    return root / "receipts" / std::filesystem::path(package_id) /
        (std::string(listing_version_id) + ".publication.json");
}

bool same_install_receipt(
    const MarketInstallReceipt& left,
    const MarketInstallReceipt& right) {
    return left.package_id == right.package_id &&
        left.listing_version_id == right.listing_version_id &&
        left.package_digest == right.package_digest &&
        left.dependency_graph_digest == right.dependency_graph_digest &&
        left.publication_receipt_digest == right.publication_receipt_digest;
}

core::Result<void> write_install_receipt(
    const std::filesystem::path& path,
    const MarketInstallReceipt& receipt) {
    if (auto valid = receipt.validate(); !valid) return valid;
    const auto existing = read_text_file(path, kMaxMarketManifestBytes);
    if (existing) {
        const auto parsed = MarketInstallReceipt::deserialize(existing.value());
        if (!parsed) return core::Result<void>::failure(parsed.error());
        if (!same_install_receipt(parsed.value(), receipt)) {
            return core::Result<void>::failure(
                conflict("market install receipt identity differs"));
        }
        return core::Result<void>::success();
    }
    if (existing.error().code != core::ErrorCode::io_error &&
        existing.error().code != core::ErrorCode::not_found) {
        return core::Result<void>::failure(existing.error());
    }
    std::error_code error;
    if (auto ready = ensure_directory(path.parent_path()); !ready) return ready;
    const auto temporary = path.string() + ".tmp-" +
        std::to_string(static_cast<unsigned long long>(g_install_counter.fetch_add(1U)));
    const std::filesystem::path temporary_path(temporary);
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) return core::Result<void>::failure(io_error("unable to create market install receipt"));
        const std::string serialized = receipt.serialize();
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            return core::Result<void>::failure(io_error("unable to write market install receipt"));
        }
    }
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return core::Result<void>::failure(io_error("unable to publish market install receipt"));
    }
    return core::Result<void>::success();
}

core::Result<MarketInstallReceipt> read_install_receipt(
    const std::filesystem::path& path) {
    const auto text = read_text_file(path, kMaxMarketManifestBytes);
    if (!text) return core::Result<MarketInstallReceipt>::failure(text.error());
    return MarketInstallReceipt::deserialize(text.value());
}

core::Result<void> write_publication_receipt(
    const std::filesystem::path& path,
    const MarketPublicationReceipt& receipt) {
    if (auto valid = receipt.validate(); !valid) return valid;
    const auto existing = read_text_file(path, kMaxMarketPublicationReceiptBytes);
    if (existing) {
        const auto parsed = MarketPublicationReceipt::deserialize(existing.value());
        if (!parsed) return core::Result<void>::failure(parsed.error());
        const auto existing_digest = parsed.value().canonical_digest();
        const auto requested_digest = receipt.canonical_digest();
        if (!existing_digest || !requested_digest || existing_digest.value() != requested_digest.value()) {
            return core::Result<void>::failure(
                conflict("market publication receipt identity differs"));
        }
        return core::Result<void>::success();
    }
    if (existing.error().code != core::ErrorCode::io_error &&
        existing.error().code != core::ErrorCode::not_found) {
        return core::Result<void>::failure(existing.error());
    }
    if (auto ready = ensure_directory(path.parent_path()); !ready) return ready;
    const auto temporary = path.string() + ".tmp-" +
        std::to_string(static_cast<unsigned long long>(g_install_counter.fetch_add(1U)));
    const std::filesystem::path temporary_path(temporary);
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) return core::Result<void>::failure(io_error("unable to create market publication receipt"));
        const std::string serialized = receipt.serialize();
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            return core::Result<void>::failure(io_error("unable to write market publication receipt"));
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return core::Result<void>::failure(io_error("unable to publish market publication receipt"));
    }
    return core::Result<void>::success();
}

core::Result<MarketPublicationReceipt> read_publication_receipt(
    const std::filesystem::path& path) {
    const auto text = read_text_file(path, kMaxMarketPublicationReceiptBytes);
    if (!text) return core::Result<MarketPublicationReceipt>::failure(text.error());
    return MarketPublicationReceipt::deserialize(text.value());
}

core::Result<void> validate_manifest_paths(
    const MarketPackageManifest& manifest,
    const std::function<bool(std::string_view)>& has_file,
    const std::function<bool(std::string_view)>& has_prefix) {
    for (const auto& entrypoint : manifest.entrypoints) {
        if (!has_file(entrypoint.path)) {
            return core::Result<void>::failure(
                validation("market entrypoint is not present in the package: " + entrypoint.path));
        }
    }
    for (const auto& target : manifest.targets) {
        if (!has_file(target.artifact) && !has_prefix(target.artifact)) {
            return core::Result<void>::failure(
                validation("market target artifact is not present in the package: " + target.artifact));
        }
    }
    return core::Result<void>::success();
}

std::string status_text(CompatibilityStatus status) {
    return compatibility_status_name(status);
}

core::Result<CompatibilityStatus> parse_status(std::string_view value) {
    static constexpr std::array<std::pair<std::string_view, CompatibilityStatus>, 6> values{
        std::pair{"VERIFIED_PASS", CompatibilityStatus::verified_pass},
        std::pair{"VERIFIED_WARN", CompatibilityStatus::verified_warn},
        std::pair{"VERIFIED_FAIL", CompatibilityStatus::verified_fail},
        std::pair{"SELLER_DECLARED", CompatibilityStatus::seller_declared},
        std::pair{"UNTESTED", CompatibilityStatus::untested},
        std::pair{"EXPIRED", CompatibilityStatus::expired}};
    for (const auto& [text, status] : values) {
        if (value == text) return core::Result<CompatibilityStatus>::success(status);
    }
    return core::Result<CompatibilityStatus>::failure(validation("compatibility status is invalid"));
}

core::Result<std::chrono::sys_seconds> parse_utc_timestamp(std::string_view value) {
    if (value.size() != 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return core::Result<std::chrono::sys_seconds>::failure(
            validation("compatibility timestamp must be UTC RFC3339 seconds"));
    }
    const auto component = [value](std::size_t offset, std::size_t count) -> std::optional<int> {
        int result = 0;
        const auto parsed = std::from_chars(value.data() + offset, value.data() + offset + count, result);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + offset + count) return std::nullopt;
        return result;
    };
    const auto year = component(0U, 4U);
    const auto month = component(5U, 2U);
    const auto day = component(8U, 2U);
    const auto hour = component(11U, 2U);
    const auto minute = component(14U, 2U);
    const auto second = component(17U, 2U);
    if (!year || !month || !day || !hour || !minute || !second || year.value() < 1970 ||
        month.value() < 1 || month.value() > 12 || hour.value() > 23 || minute.value() > 59 ||
        second.value() > 59) {
        return core::Result<std::chrono::sys_seconds>::failure(
            validation("compatibility timestamp is invalid"));
    }
    constexpr std::array<int, 12> days_in_month{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year.value() % 4 == 0 &&
        (year.value() % 100 != 0 || year.value() % 400 == 0);
    const int maximum_day = days_in_month[static_cast<std::size_t>(month.value() - 1)] +
        (month.value() == 2 && leap ? 1 : 0);
    if (day.value() < 1 || day.value() > maximum_day) {
        return core::Result<std::chrono::sys_seconds>::failure(
            validation("compatibility timestamp day is invalid"));
    }

    // Howard Hinnant's civil-date conversion, anchored at 1970-01-01.
    int adjusted_year = year.value() - (month.value() <= 2 ? 1 : 0);
    const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned month_index = static_cast<unsigned>(month.value() + (month.value() > 2 ? -3 : 9));
    const unsigned day_of_year = (153U * month_index + 2U) / 5U + static_cast<unsigned>(day.value()) - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    const auto days_since_epoch = era * 146097 + static_cast<int>(day_of_era) - 719468;
    return core::Result<std::chrono::sys_seconds>::success(
        std::chrono::sys_seconds{std::chrono::days{days_since_epoch} + std::chrono::hours{hour.value()} +
            std::chrono::minutes{minute.value()} + std::chrono::seconds{second.value()}});
}

core::Result<std::vector<std::string>> parse_string_array(
    std::string_view raw,
    std::string_view field) {
    const auto values = array_items(raw, field);
    if (!values) return core::Result<std::vector<std::string>>::failure(values.error());
    std::vector<std::string> result;
    result.reserve(values.value().size());
    for (const auto item : values.value()) {
        const auto decoded = core::json::decode_string(item);
        if (!decoded || !printable(decoded.value(), kMaxMarketStringBytes)) {
            return core::Result<std::vector<std::string>>::failure(validation("market warning is invalid"));
        }
        result.push_back(decoded.value());
    }
    return core::Result<std::vector<std::string>>::success(std::move(result));
}

} // namespace

core::Result<void> PackageEntrypoint::validate() const {
    if (!safe_token(kind) || !safe_package_path(path) || path == "manifest.json") {
        return core::Result<void>::failure(validation("market entrypoint is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> TargetArtifact::validate() const {
    if (!safe_token(engine) || !safe_token(profile) || !safe_package_path(artifact)) {
        return core::Result<void>::failure(validation("market target artifact is invalid"));
    }
    if (digest.has_value() && digest->is_zero()) {
        return core::Result<void>::failure(validation("market target artifact digest is zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> DependencySpec::validate() const {
    if (!safe_token(package_id) || !printable(version_constraint, kMaxMarketStringBytes)) {
        return core::Result<void>::failure(validation("market dependency is invalid"));
    }
    if (version_constraint != "*" &&
        !parse_semver(version_constraint.starts_with('^') || version_constraint.starts_with('~')
                ? version_constraint.substr(1U) : version_constraint,
            "dependency constraint")) {
        return core::Result<void>::failure(validation("market dependency version constraint is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> InventoryItem::validate() const {
    if (!safe_token(kind)) return core::Result<void>::failure(validation("market inventory item is invalid"));
    return core::Result<void>::success();
}

core::Result<void> MarketPackageManifest::validate() const {
    if (format != "cartographer-market-package" || format_version != kMarketPackageFormatVersion) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "market package format is unsupported"));
    }
    if (!safe_library_identity(package_id) || !safe_token(listing_id) ||
        !safe_library_identity(listing_version_id) ||
        !safe_token(publisher_id) || !safe_token(license_id) || content_digest.is_zero()) {
        return core::Result<void>::failure(validation("market package identity is invalid"));
    }
    if (source_revision.has_value() && !printable(*source_revision, kMaxMarketStringBytes)) {
        return core::Result<void>::failure(validation("market package source revision is invalid"));
    }
    if (provenance_receipt.has_value() && !safe_package_path(*provenance_receipt)) {
        return core::Result<void>::failure(validation("market package provenance receipt is invalid"));
    }
    if (entrypoints.empty() || entrypoints.size() > kMaxMarketArrayItems ||
        targets.size() > kMaxMarketArrayItems || dependencies.size() > kMaxMarketArrayItems ||
        inventory.size() > kMaxMarketArrayItems) {
        return core::Result<void>::failure(validation("market package array bounds are invalid"));
    }
    std::set<std::string> entrypoint_paths;
    for (const auto& entrypoint : entrypoints) {
        if (auto valid = entrypoint.validate(); !valid) return valid;
        if (!entrypoint_paths.insert(entrypoint.path).second) {
            return core::Result<void>::failure(validation("market package has duplicate entrypoints"));
        }
    }
    std::set<std::pair<std::string, std::string>> target_keys;
    for (const auto& target : targets) {
        if (auto valid = target.validate(); !valid) return valid;
        if (!target_keys.insert({target.engine, target.profile}).second) {
            return core::Result<void>::failure(validation("market package has duplicate target profiles"));
        }
    }
    std::set<std::string> dependency_ids;
    for (const auto& dependency : dependencies) {
        if (auto valid = dependency.validate(); !valid) return valid;
        if (!dependency_ids.insert(dependency.package_id).second) {
            return core::Result<void>::failure(validation("market package has duplicate dependencies"));
        }
    }
    std::set<std::string> inventory_kinds;
    for (const auto& item : inventory) {
        if (auto valid = item.validate(); !valid) return valid;
        if (!inventory_kinds.insert(item.kind).second) {
            return core::Result<void>::failure(validation("market package has duplicate inventory kinds"));
        }
    }
    return core::Result<void>::success();
}

std::string MarketPackageManifest::serialize() const {
    std::vector<PackageEntrypoint> sorted_entrypoints = entrypoints;
    std::sort(sorted_entrypoints.begin(), sorted_entrypoints.end(), [](const auto& left, const auto& right) {
        return std::tie(left.kind, left.path) < std::tie(right.kind, right.path);
    });
    std::vector<TargetArtifact> sorted_targets = targets;
    std::sort(sorted_targets.begin(), sorted_targets.end(), [](const auto& left, const auto& right) {
        return std::tie(left.engine, left.profile, left.artifact) <
            std::tie(right.engine, right.profile, right.artifact);
    });
    std::vector<DependencySpec> sorted_dependencies = dependencies;
    std::sort(sorted_dependencies.begin(), sorted_dependencies.end(), [](const auto& left, const auto& right) {
        return std::tie(left.package_id, left.version_constraint, left.optional) <
            std::tie(right.package_id, right.version_constraint, right.optional);
    });
    std::vector<InventoryItem> sorted_inventory = inventory;
    std::sort(sorted_inventory.begin(), sorted_inventory.end(), [](const auto& left, const auto& right) {
        return left.kind < right.kind;
    });

    std::vector<std::string> entrypoint_json;
    for (const auto& entrypoint : sorted_entrypoints) {
        entrypoint_json.push_back("{\"kind\":" + quote(entrypoint.kind) +
            ",\"path\":" + quote(entrypoint.path) + "}");
    }
    std::vector<std::string> target_json;
    for (const auto& target : sorted_targets) {
        std::string value = "{\"artifact\":" + quote(target.artifact) +
            ",\"engine\":" + quote(target.engine) +
            ",\"profile\":" + quote(target.profile);
        if (target.digest.has_value()) value += ",\"artifact_digest\":" + quote(digest_text(*target.digest));
        value += "}";
        target_json.push_back(std::move(value));
    }
    std::vector<std::string> dependency_json;
    for (const auto& dependency : sorted_dependencies) {
        dependency_json.push_back("{\"optional\":" + std::string(dependency.optional ? "true" : "false") +
            ",\"package_id\":" + quote(dependency.package_id) +
            ",\"version_constraint\":" + quote(dependency.version_constraint) + "}");
    }
    std::string inventory_json = "{";
    for (std::size_t index = 0U; index < sorted_inventory.size(); ++index) {
        if (index != 0U) inventory_json.push_back(',');
        inventory_json += quote(sorted_inventory[index].kind) + ":" + uint_text(sorted_inventory[index].count);
    }
    inventory_json.push_back('}');

    std::string result = "{\"content_digest\":" + quote(digest_text(content_digest)) +
        ",\"dependencies\":" + join_array(dependency_json) +
        ",\"entrypoints\":" + join_array(entrypoint_json) +
        ",\"format\":" + quote(format) +
        ",\"format_version\":" + uint_text32(format_version) +
        ",\"inventory\":" + inventory_json +
        ",\"license_id\":" + quote(license_id) +
        ",\"listing_id\":" + quote(listing_id) +
        ",\"listing_version_id\":" + quote(listing_version_id) +
        ",\"package_id\":" + quote(package_id) +
        ",\"publisher_id\":" + quote(publisher_id);
    if (provenance_receipt.has_value()) {
        result += ",\"provenance\":{\"receipt\":" + quote(*provenance_receipt) + "}";
    } else {
        result += ",\"provenance\":{}";
    }
    if (source_revision.has_value()) result += ",\"source_revision\":" + quote(*source_revision);
    result += ",\"targets\":" + join_array(target_json) + "}";
    return result;
}

core::Result<assets::Sha256Digest> MarketPackageManifest::canonical_digest() const {
    if (auto valid = validate(); !valid) return core::Result<assets::Sha256Digest>::failure(valid.error());
    const std::string serialized = serialize();
    const auto* data = reinterpret_cast<const std::uint8_t*>(serialized.data());
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(std::span<const std::uint8_t>{data, serialized.size()}));
}

core::Result<assets::Sha256Digest> MarketPackageManifest::package_identity_digest() const {
    if (auto valid = validate(); !valid) {
        return core::Result<assets::Sha256Digest>::failure(valid.error());
    }
    return core::Result<assets::Sha256Digest>::success(market_package_identity_digest(*this));
}

core::Result<MarketPackageManifest> MarketPackageManifest::deserialize(std::string_view json) {
    if (json.size() > kMaxMarketManifestBytes) {
        return core::Result<MarketPackageManifest>::failure(validation("market manifest exceeds its size limit"));
    }
    const auto object = core::json::parse_object(json);
    if (!object) return core::Result<MarketPackageManifest>::failure(object.error());
    if (!allowed_fields(object.value(), {
            "content_digest", "dependencies", "entrypoints", "format", "format_version",
            "inventory", "license_id", "listing_id", "listing_version_id", "package_id",
            "provenance", "publisher_id", "source_revision", "targets"})) {
        return core::Result<MarketPackageManifest>::failure(validation("market manifest contains an unknown field"));
    }
    MarketPackageManifest manifest;
    const auto format = required_string(object.value(), "format", kMaxMarketStringBytes, true);
    const auto version = required_uint32(object.value(), "format_version");
    const auto package_id = required_string(object.value(), "package_id", kMaxMarketStringBytes, true);
    const auto listing_id = required_string(object.value(), "listing_id", kMaxMarketStringBytes, true);
    const auto listing_version_id = required_string(object.value(), "listing_version_id", kMaxMarketStringBytes, true);
    const auto publisher_id = required_string(object.value(), "publisher_id", kMaxMarketStringBytes, true);
    const auto license_id = required_string(object.value(), "license_id", kMaxMarketStringBytes, true);
    const auto content_text = required_string(object.value(), "content_digest", 71U);
    if (!format || !version || !package_id || !listing_id || !listing_version_id || !publisher_id ||
        !license_id || !content_text) {
        return core::Result<MarketPackageManifest>::failure(
            !format ? format.error() : !version ? version.error() : !package_id ? package_id.error() :
            !listing_id ? listing_id.error() : !listing_version_id ? listing_version_id.error() :
            !publisher_id ? publisher_id.error() : !license_id ? license_id.error() : content_text.error());
    }
    const auto digest = parse_digest(content_text.value(), "content_digest");
    if (!digest) return core::Result<MarketPackageManifest>::failure(digest.error());
    manifest.format = format.value();
    manifest.format_version = version.value();
    manifest.package_id = package_id.value();
    manifest.listing_id = listing_id.value();
    manifest.listing_version_id = listing_version_id.value();
    manifest.publisher_id = publisher_id.value();
    manifest.license_id = license_id.value();
    manifest.content_digest = digest.value();
    const auto source_revision = optional_string(object.value(), "source_revision", kMaxMarketStringBytes);
    if (!source_revision) return core::Result<MarketPackageManifest>::failure(source_revision.error());
    manifest.source_revision = source_revision.value();
    if (const auto* provenance = core::json::find_member(object.value(), "provenance"); provenance != nullptr) {
        const auto provenance_object = core::json::parse_object(provenance->raw_value);
        if (!provenance_object || !allowed_fields(provenance_object.value(), {"receipt"})) {
            return core::Result<MarketPackageManifest>::failure(
                provenance_object ? validation("market provenance has an unknown field") : provenance_object.error());
        }
        const auto receipt = optional_string(provenance_object.value(), "receipt", kMaxMarketStringBytes);
        if (!receipt) return core::Result<MarketPackageManifest>::failure(receipt.error());
        manifest.provenance_receipt = receipt.value();
    }
    for (const auto field : {std::string_view{"entrypoints"}, std::string_view{"targets"},
                             std::string_view{"dependencies"}}) {
        const auto* member = core::json::find_member(object.value(), field);
        if (member == nullptr) {
            return core::Result<MarketPackageManifest>::failure(
                validation("market manifest array is missing: " + std::string(field)));
        }
        const auto items = array_items(member->raw_value, field);
        if (!items) return core::Result<MarketPackageManifest>::failure(items.error());
        for (const auto item : items.value()) {
            if (field == "entrypoints") {
                const auto parsed = parse_entrypoint(item);
                if (!parsed) return core::Result<MarketPackageManifest>::failure(parsed.error());
                manifest.entrypoints.push_back(parsed.value());
            } else if (field == "targets") {
                const auto parsed = parse_target(item);
                if (!parsed) return core::Result<MarketPackageManifest>::failure(parsed.error());
                manifest.targets.push_back(parsed.value());
            } else {
                const auto parsed = parse_dependency(item);
                if (!parsed) return core::Result<MarketPackageManifest>::failure(parsed.error());
                manifest.dependencies.push_back(parsed.value());
            }
        }
    }
    if (const auto* inventory = core::json::find_member(object.value(), "inventory"); inventory != nullptr) {
        const auto inventory_object = core::json::parse_object(inventory->raw_value);
        if (!inventory_object) return core::Result<MarketPackageManifest>::failure(inventory_object.error());
        for (const auto& member : inventory_object.value()) {
            const auto parsed = parse_inventory_item(member.key, member.raw_value);
            if (!parsed) return core::Result<MarketPackageManifest>::failure(parsed.error());
            manifest.inventory.push_back(parsed.value());
        }
    }
    if (auto valid = manifest.validate(); !valid) {
        return core::Result<MarketPackageManifest>::failure(valid.error());
    }
    return core::Result<MarketPackageManifest>::success(std::move(manifest));
}

core::Result<void> MarketValidationReport::validate() const {
    if (auto valid = manifest.validate(); !valid) return valid;
    if (computed_content_digest.is_zero() || computed_content_digest != manifest.content_digest ||
        computed_package_digest.is_zero() ||
        computed_package_digest != market_package_identity_digest(manifest)) {
        return core::Result<void>::failure(validation("market validation report digest mismatch"));
    }
    if (payload_files != files.size()) {
        return core::Result<void>::failure(validation("market validation report file count mismatch"));
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> MarketPackageValidator::directory_payload_digest(
    const std::filesystem::path& root,
    const MarketValidationLimits& limits) {
    const auto records = collect_directory_payload(root, limits);
    if (!records) return core::Result<assets::Sha256Digest>::failure(records.error());
    return core::Result<assets::Sha256Digest>::success(payload_digest(records.value()));
}

core::Result<assets::Sha256Digest> MarketPackageValidator::directory_package_digest(
    const std::filesystem::path& root,
    const MarketValidationLimits& limits) {
    const auto manifest_text = read_text_file(root / "manifest.json", limits.max_manifest_bytes);
    if (!manifest_text) return core::Result<assets::Sha256Digest>::failure(manifest_text.error());
    const auto manifest = MarketPackageManifest::deserialize(manifest_text.value());
    if (!manifest) return core::Result<assets::Sha256Digest>::failure(manifest.error());
    const auto records = collect_directory_payload(root, limits);
    if (!records) return core::Result<assets::Sha256Digest>::failure(records.error());
    const auto computed_content = payload_digest(records.value());
    if (computed_content != manifest.value().content_digest) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("market package content digest mismatch"));
    }
    return core::Result<assets::Sha256Digest>::success(
        market_package_identity_digest(manifest.value()));
}

core::Result<MarketValidationReport> MarketPackageValidator::validate_directory(
    const std::filesystem::path& root,
    const MarketValidationLimits& limits) {
    const auto manifest_text = read_text_file(root / "manifest.json", limits.max_manifest_bytes);
    if (!manifest_text) return core::Result<MarketValidationReport>::failure(manifest_text.error());
    const auto manifest = MarketPackageManifest::deserialize(manifest_text.value());
    if (!manifest) return core::Result<MarketValidationReport>::failure(manifest.error());
    const auto records = collect_directory_payload(root, limits);
    if (!records) return core::Result<MarketValidationReport>::failure(records.error());
    const auto computed = payload_digest(records.value());
    if (computed != manifest.value().content_digest) {
        return core::Result<MarketValidationReport>::failure(validation("market package content digest mismatch"));
    }
    std::uint64_t bytes = 0U;
    std::vector<std::string> files;
    for (const auto& record : records.value()) {
        bytes += record.bytes;
        files.push_back(record.path);
    }
    if (std::any_of(records.value().begin(), records.value().end(), [](const auto& record) {
            return record.executable;
        })) {
        return core::Result<MarketValidationReport>::failure(
            validation("market package contains executable content; public market packages are data-only"));
    }
    const auto paths = validate_manifest_paths(
        manifest.value(),
        [&root](std::string_view path) {
            std::error_code error;
            return std::filesystem::is_regular_file(root / std::filesystem::path(path), error) && !error;
        },
        [&root](std::string_view path) {
            std::error_code error;
            return std::filesystem::is_directory(root / std::filesystem::path(path), error) && !error;
        });
    if (!paths) return core::Result<MarketValidationReport>::failure(paths.error());
    if (auto targets = validate_directory_target_digests(root, manifest.value(), limits); !targets) {
        return core::Result<MarketValidationReport>::failure(targets.error());
    }
    MarketValidationReport report{
        manifest.value(), computed, market_package_identity_digest(manifest.value()), bytes,
        static_cast<std::uint32_t>(records.value().size()), false, std::move(files)};
    if (auto valid = report.validate(); !valid) return core::Result<MarketValidationReport>::failure(valid.error());
    return core::Result<MarketValidationReport>::success(std::move(report));
}

core::Result<MarketValidationReport> MarketPackageValidator::validate_archive(
    std::span<const std::uint8_t> archive,
    const MarketValidationLimits& limits) {
    plugin_package::PackageLimits package_limits;
    package_limits.max_archive_bytes = limits.max_archive_bytes;
    if (limits.max_total_payload_bytes >
        std::numeric_limits<std::uint64_t>::max() - limits.max_manifest_bytes) {
        return core::Result<MarketValidationReport>::failure(
            validation("market archive limits overflow"));
    }
    package_limits.max_total_uncompressed = limits.max_total_payload_bytes + limits.max_manifest_bytes;
    package_limits.max_single_file = limits.max_single_file_bytes;
    package_limits.max_entries = limits.max_files;
    package_limits.max_manifest_bytes = limits.max_manifest_bytes;
    const auto entries = plugin_package::read_cartoplug(archive, package_limits);
    if (!entries) return core::Result<MarketValidationReport>::failure(entries.error());
    const auto manifest_entry = std::find_if(entries.value().begin(), entries.value().end(), [](const auto& entry) {
        return entry.path == "manifest.json";
    });
    if (manifest_entry == entries.value().end()) {
        return core::Result<MarketValidationReport>::failure(validation("market archive has no manifest.json"));
    }
    const auto manifest_text = std::string(
        reinterpret_cast<const char*>(manifest_entry->bytes.data()), manifest_entry->bytes.size());
    const auto manifest = MarketPackageManifest::deserialize(manifest_text);
    if (!manifest) return core::Result<MarketValidationReport>::failure(manifest.error());
    std::vector<PayloadRecord> records;
    std::uint64_t bytes = 0U;
    std::set<std::string> files;
    for (const auto& entry : entries.value()) {
        if (entry.path == "manifest.json") continue;
        const auto entry_bytes = static_cast<std::uint64_t>(entry.bytes.size());
        if (entry_bytes > limits.max_total_payload_bytes -
            std::min(limits.max_total_payload_bytes, bytes)) {
            return core::Result<MarketValidationReport>::failure(
                validation("market archive payload exceeds its size limit"));
        }
        bytes += entry_bytes;
        files.insert(entry.path);
        records.push_back(PayloadRecord{
            entry.path,
            entry_bytes,
            assets::sha256(std::span<const std::uint8_t>{entry.bytes.data(), entry.bytes.size()}),
            executable_path(entry.path)});
    }
    if (std::any_of(records.begin(), records.end(), [](const auto& record) { return record.executable; })) {
        return core::Result<MarketValidationReport>::failure(
            validation("market archive contains executable content; public market packages are data-only"));
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
    const auto computed = payload_digest(records);
    if (computed != manifest.value().content_digest) {
        return core::Result<MarketValidationReport>::failure(validation("market archive content digest mismatch"));
    }
    const auto paths = validate_manifest_paths(
        manifest.value(),
        [&files](std::string_view path) { return files.contains(std::string(path)); },
        [&files](std::string_view path) {
            const std::string prefix = std::string(path) + "/";
            return std::any_of(files.begin(), files.end(), [&prefix](const auto& file) {
                return file.starts_with(prefix);
            });
        });
    if (!paths) return core::Result<MarketValidationReport>::failure(paths.error());
    for (const auto& target : manifest.value().targets) {
        if (!target.digest.has_value()) continue;
        const auto computed_target = archive_target_digest(records, target.artifact);
        if (!computed_target) return core::Result<MarketValidationReport>::failure(computed_target.error());
        if (computed_target.value() != target.digest.value()) {
            return core::Result<MarketValidationReport>::failure(
                validation("market archive target artifact digest mismatch: " + target.artifact));
        }
    }
    std::vector<std::string> file_list(files.begin(), files.end());
    MarketValidationReport report{
        manifest.value(), computed, market_package_identity_digest(manifest.value()), bytes,
        static_cast<std::uint32_t>(records.size()), false, std::move(file_list)};
    if (auto valid = report.validate(); !valid) return core::Result<MarketValidationReport>::failure(valid.error());
    return core::Result<MarketValidationReport>::success(std::move(report));
}

core::Result<plugin_package::SignatureVerdict>
MarketPackageValidator::verify_publication_receipt(
    const MarketValidationReport& report,
    const MarketPublicationReceipt& receipt,
    const plugin_package::KeyRing& key_ring) {
    if (auto valid = report.validate(); !valid) {
        return core::Result<plugin_package::SignatureVerdict>::failure(valid.error());
    }
    if (auto valid = receipt.validate(); !valid) {
        return core::Result<plugin_package::SignatureVerdict>::failure(valid.error());
    }
    if (receipt.package_digest != report.computed_package_digest ||
        receipt.package_id != report.manifest.package_id ||
        receipt.listing_version_id != report.manifest.listing_version_id ||
        receipt.publisher_id != report.manifest.publisher_id) {
        return core::Result<plugin_package::SignatureVerdict>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "market publication receipt does not match package identity"));
    }
    return receipt.verify(key_ring);
}

core::Result<void> ResolvedPackage::validate() const {
    if (!safe_token(package_id) || !parse_semver(version, "resolved package version") ||
        content_digest.is_zero() || package_digest.is_zero()) {
        return core::Result<void>::failure(validation("resolved market package is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> DependencyResolutionPlan::validate() const {
    if (packages.empty() || packages.size() > kMaxMarketArrayItems || plan_digest.is_zero()) {
        return core::Result<void>::failure(validation("dependency resolution plan is empty or unbound"));
    }
    std::string previous;
    for (const auto& package : packages) {
        if (auto valid = package.validate(); !valid) return valid;
        if (!previous.empty() && previous >= package.package_id) {
            return core::Result<void>::failure(validation("dependency resolution plan is not canonically sorted"));
        }
        previous = package.package_id;
    }
    return core::Result<void>::success();
}

std::string DependencyResolutionPlan::canonical_without_digest() const {
    std::vector<ResolvedPackage> sorted = packages;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.package_id < right.package_id;
    });
    std::string result = "version=1\n";
    for (const auto& package : sorted) {
        result += package.package_id + "\n" + package.version + "\n" +
            package.content_digest.hex() + "\n" + package.package_digest.hex() + "\n";
    }
    return result;
}

core::Result<void> DependencyResolutionPlan::refresh_digest() {
    for (const auto& package : packages) {
        if (auto valid = package.validate(); !valid) return valid;
    }
    std::sort(packages.begin(), packages.end(), [](const auto& left, const auto& right) {
        return left.package_id < right.package_id;
    });
    const std::string canonical = canonical_without_digest();
    const auto* data = reinterpret_cast<const std::uint8_t*>(canonical.data());
    plan_digest = assets::sha256(std::span<const std::uint8_t>{data, canonical.size()});
    return core::Result<void>::success();
}

core::Result<DependencyResolutionPlan> resolve_dependencies(
    std::span<const MarketPackageManifest> candidates,
    std::span<const DependencySpec> roots) {
    if (candidates.empty() || roots.empty()) {
        return core::Result<DependencyResolutionPlan>::failure(validation("dependency resolution needs candidates and roots"));
    }
    std::map<std::string, std::vector<const MarketPackageManifest*>> by_id;
    for (const auto& candidate : candidates) {
        if (auto valid = candidate.validate(); !valid) return core::Result<DependencyResolutionPlan>::failure(valid.error());
        by_id[candidate.package_id].push_back(&candidate);
    }
    for (auto& [id, values] : by_id) {
        std::sort(values.begin(), values.end(), [](const auto* left, const auto* right) {
            const auto left_version = parse_semver(left->listing_version_id, "listing version");
            const auto right_version = parse_semver(right->listing_version_id, "listing version");
            if (left_version && right_version && left_version.value() != right_version.value()) {
                return left_version.value() > right_version.value();
            }
            return left->content_digest.hex() < right->content_digest.hex();
        });
    }
    std::map<std::string, std::vector<std::string>> constraints;
    std::map<std::string, const MarketPackageManifest*> selected;
    std::vector<std::string> pending;
    std::set<std::string> pending_set;
    std::set<std::string> required_ids;
    auto enqueue = [&pending, &pending_set, &required_ids](const std::string& id, bool optional) {
        if (!optional) required_ids.insert(id);
        if (pending_set.insert(id).second) pending.push_back(id);
    };
    for (const auto& root : roots) {
        if (auto valid = root.validate(); !valid) return core::Result<DependencyResolutionPlan>::failure(valid.error());
        constraints[root.package_id].push_back(root.version_constraint);
        enqueue(root.package_id, root.optional);
    }
    while (!pending.empty()) {
        const std::string id = pending.back();
        pending.pop_back();
        pending_set.erase(id);
        const auto candidates_for_id = by_id.find(id);
        if (candidates_for_id == by_id.end()) {
            if (!required_ids.contains(id)) continue;
            return core::Result<DependencyResolutionPlan>::failure(
                validation("dependency package is unavailable: " + id));
        }
        const auto matches = [&constraints, &id](const auto* candidate) {
            const auto found = constraints.find(id);
            return found != constraints.end() && std::all_of(
                found->second.begin(), found->second.end(), [&candidate](const auto& constraint) {
                    return version_matches(candidate->listing_version_id, constraint);
                });
        };
        const auto found = std::find_if(
            candidates_for_id->second.begin(), candidates_for_id->second.end(), matches);
        if (found == candidates_for_id->second.end()) {
            return core::Result<DependencyResolutionPlan>::failure(
                conflict("dependency constraints cannot be satisfied: " + id));
        }
        const auto* chosen = *found;
        const auto previous = selected.find(id);
        if (previous != selected.end() && previous->second != chosen) {
            return core::Result<DependencyResolutionPlan>::failure(
                conflict("dependency resolution changed an already selected version: " + id));
        }
        if (previous != selected.end()) continue;
        selected.emplace(id, chosen);
        for (const auto& dependency : chosen->dependencies) {
            constraints[dependency.package_id].push_back(dependency.version_constraint);
            enqueue(dependency.package_id, dependency.optional);
        }
    }

    enum class Visit : std::uint8_t { unseen, active, done };
    std::map<std::string, Visit> visits;
    std::function<core::Result<void>(const std::string&)> visit = [&](const std::string& id) {
        if (visits[id] == Visit::active) return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "market dependency cycle detected at: " + id));
        if (visits[id] == Visit::done) return core::Result<void>::success();
        visits[id] = Visit::active;
        const auto* package = selected.at(id);
        for (const auto& dependency : package->dependencies) {
            if (!selected.contains(dependency.package_id)) {
                if (dependency.optional) continue;
                return core::Result<void>::failure(validation("resolved dependency is missing: " + dependency.package_id));
            }
            if (auto result = visit(dependency.package_id); !result) return result;
        }
        visits[id] = Visit::done;
        return core::Result<void>::success();
    };
    for (const auto& [id, package] : selected) {
        (void)package;
        if (auto result = visit(id); !result) return core::Result<DependencyResolutionPlan>::failure(result.error());
    }
    DependencyResolutionPlan plan;
    for (const auto& [id, package] : selected) {
        plan.packages.push_back(ResolvedPackage{
            id, package->listing_version_id, package->content_digest,
            market_package_identity_digest(*package)});
    }
    if (auto result = plan.refresh_digest(); !result) {
        return core::Result<DependencyResolutionPlan>::failure(result.error());
    }
    return core::Result<DependencyResolutionPlan>::success(std::move(plan));
}

core::Result<void> LockedTargetArtifact::validate() const {
    if (!safe_token(engine) || !safe_token(profile) || artifact_digest.is_zero()) {
        return core::Result<void>::failure(validation("locked target artifact is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> LockedPackage::validate() const {
    if (!safe_token(package_id) || !parse_semver(package_version, "locked package version") ||
        !safe_token(listing_id) || !safe_token(listing_version_id) || package_digest.is_zero() ||
        !safe_token(license_snapshot_id)) {
        return core::Result<void>::failure(validation("locked market package is invalid"));
    }
    std::set<std::pair<std::string, std::string>> keys;
    for (const auto& target : targets) {
        if (auto valid = target.validate(); !valid) return valid;
        if (!keys.insert({target.engine, target.profile}).second) {
            return core::Result<void>::failure(validation("locked package has duplicate target profiles"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectMarketLockfile::validate() const {
    if (!safe_token(project_id) || packages.empty() || packages.size() > kMaxMarketArrayItems) {
        return core::Result<void>::failure(validation("market lockfile identity or package list is invalid"));
    }
    std::string previous;
    for (const auto& package : packages) {
        if (auto valid = package.validate(); !valid) return valid;
        if (!previous.empty() && previous >= package.package_id) {
            return core::Result<void>::failure(validation("market lockfile is not canonically sorted"));
        }
        previous = package.package_id;
    }
    return core::Result<void>::success();
}

std::string ProjectMarketLockfile::serialize() const {
    std::vector<LockedPackage> sorted = packages;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.package_id < right.package_id;
    });
    std::vector<std::string> package_json;
    for (const auto& package : sorted) {
        std::vector<LockedTargetArtifact> targets = package.targets;
        std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
            return std::tie(left.engine, left.profile) < std::tie(right.engine, right.profile);
        });
        std::vector<std::string> target_json;
        for (const auto& target : targets) {
            target_json.push_back("{\"artifact_digest\":" + quote(digest_text(target.artifact_digest)) +
                ",\"engine\":" + quote(target.engine) + ",\"profile\":" + quote(target.profile) + "}");
        }
        package_json.push_back("{\"license_snapshot_id\":" + quote(package.license_snapshot_id) +
            ",\"listing_id\":" + quote(package.listing_id) +
            ",\"listing_version_id\":" + quote(package.listing_version_id) +
            ",\"package_digest\":" + quote(digest_text(package.package_digest)) +
            ",\"package_id\":" + quote(package.package_id) +
            ",\"package_version\":" + quote(package.package_version) +
            ",\"target\":" + join_array(target_json) + "}");
    }
    return "{\"format_version\":" + uint_text32(kFormatVersion) +
        ",\"market_package\":" + join_array(package_json) +
        ",\"project_id\":" + quote(project_id) + "}";
}

core::Result<assets::Sha256Digest> ProjectMarketLockfile::canonical_digest() const {
    if (auto valid = validate(); !valid) return core::Result<assets::Sha256Digest>::failure(valid.error());
    const std::string serialized = serialize();
    const auto* data = reinterpret_cast<const std::uint8_t*>(serialized.data());
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(std::span<const std::uint8_t>{data, serialized.size()}));
}

core::Result<void> ProjectMarketLockfile::matches(const DependencyResolutionPlan& plan) const {
    if (auto valid = validate(); !valid) return valid;
    if (auto valid = plan.validate(); !valid) return valid;
    if (packages.size() != plan.packages.size()) {
        return core::Result<void>::failure(conflict("market lockfile package set differs from resolution plan"));
    }
    for (const auto& resolved : plan.packages) {
        const auto found = std::find_if(packages.begin(), packages.end(), [&resolved](const auto& package) {
            return package.package_id == resolved.package_id;
        });
        if (found == packages.end() || found->package_version != resolved.version ||
            found->package_digest != resolved.package_digest) {
            return core::Result<void>::failure(conflict("market lockfile pin differs for: " + resolved.package_id));
        }
    }
    return core::Result<void>::success();
}

core::Result<ProjectMarketLockfile> ProjectMarketLockfile::deserialize(std::string_view json) {
    if (json.size() > kMaxMarketLockfileBytes) {
        return core::Result<ProjectMarketLockfile>::failure(validation("market lockfile exceeds its size limit"));
    }
    const auto object = core::json::parse_object(json);
    if (!object || !allowed_fields(object.value(), {"format_version", "market_package", "project_id"})) {
        return core::Result<ProjectMarketLockfile>::failure(
            object ? validation("market lockfile contains an unknown field") : object.error());
    }
    const auto version = required_uint32(object.value(), "format_version");
    const auto project = required_string(object.value(), "project_id", kMaxMarketStringBytes, true);
    const auto* package_member = core::json::find_member(object.value(), "market_package");
    if (!version || !project || package_member == nullptr) {
        return core::Result<ProjectMarketLockfile>::failure(
            !version ? version.error() : !project ? project.error() : validation("market lockfile packages are missing"));
    }
    if (version.value() != kFormatVersion) {
        return core::Result<ProjectMarketLockfile>::failure(
            core::Diagnostic(core::ErrorCode::version_mismatch, "market lockfile format is unsupported"));
    }
    const auto items = array_items(package_member->raw_value, "market_package");
    if (!items) return core::Result<ProjectMarketLockfile>::failure(items.error());
    ProjectMarketLockfile result;
    result.project_id = project.value();
    for (const auto item : items.value()) {
        const auto package_object = core::json::parse_object(item);
        if (!package_object || !allowed_fields(package_object.value(), {
                "license_snapshot_id", "listing_id", "listing_version_id", "package_digest",
                "package_id", "package_version", "target"})) {
            return core::Result<ProjectMarketLockfile>::failure(
                package_object ? validation("market lockfile package has an unknown field") : package_object.error());
        }
        const auto package_id = required_string(package_object.value(), "package_id", kMaxMarketStringBytes, true);
        const auto package_version = required_string(package_object.value(), "package_version", kMaxMarketStringBytes, true);
        const auto listing_id = required_string(package_object.value(), "listing_id", kMaxMarketStringBytes, true);
        const auto listing_version_id = required_string(package_object.value(), "listing_version_id", kMaxMarketStringBytes, true);
        const auto license = required_string(package_object.value(), "license_snapshot_id", kMaxMarketStringBytes, true);
        const auto digest_text_value = required_string(package_object.value(), "package_digest", 71U);
        if (!package_id || !package_version || !listing_id || !listing_version_id || !license || !digest_text_value) {
            return core::Result<ProjectMarketLockfile>::failure(
                !package_id ? package_id.error() : !package_version ? package_version.error() :
                !listing_id ? listing_id.error() : !listing_version_id ? listing_version_id.error() :
                !license ? license.error() : digest_text_value.error());
        }
        const auto digest = parse_digest(digest_text_value.value(), "package_digest");
        const auto* targets = core::json::find_member(package_object.value(), "target");
        if (!digest || targets == nullptr) {
            return core::Result<ProjectMarketLockfile>::failure(
                !digest ? digest.error() : validation("market lockfile targets are missing"));
        }
        const auto target_items = array_items(targets->raw_value, "target");
        if (!target_items) return core::Result<ProjectMarketLockfile>::failure(target_items.error());
        LockedPackage package{
            package_id.value(), package_version.value(), listing_id.value(), listing_version_id.value(),
            digest.value(), license.value(), {}};
        for (const auto target_item : target_items.value()) {
            const auto target_object = core::json::parse_object(target_item);
            if (!target_object || !allowed_fields(target_object.value(), {"artifact_digest", "engine", "profile"})) {
                return core::Result<ProjectMarketLockfile>::failure(
                    target_object ? validation("market lockfile target has an unknown field") : target_object.error());
            }
            const auto engine = required_string(target_object.value(), "engine", kMaxMarketStringBytes, true);
            const auto profile = required_string(target_object.value(), "profile", kMaxMarketStringBytes, true);
            const auto target_digest_text = required_string(target_object.value(), "artifact_digest", 71U);
            if (!engine || !profile || !target_digest_text) {
                return core::Result<ProjectMarketLockfile>::failure(
                    !engine ? engine.error() : !profile ? profile.error() : target_digest_text.error());
            }
            const auto target_digest = parse_digest(target_digest_text.value(), "artifact_digest");
            if (!target_digest) return core::Result<ProjectMarketLockfile>::failure(target_digest.error());
            package.targets.push_back(LockedTargetArtifact{engine.value(), profile.value(), target_digest.value()});
        }
        if (auto valid = package.validate(); !valid) return core::Result<ProjectMarketLockfile>::failure(valid.error());
        result.packages.push_back(std::move(package));
    }
    if (auto valid = result.validate(); !valid) return core::Result<ProjectMarketLockfile>::failure(valid.error());
    return core::Result<ProjectMarketLockfile>::success(std::move(result));
}

const char* compatibility_status_name(CompatibilityStatus status) noexcept {
    switch (status) {
    case CompatibilityStatus::verified_pass: return "VERIFIED_PASS";
    case CompatibilityStatus::verified_warn: return "VERIFIED_WARN";
    case CompatibilityStatus::verified_fail: return "VERIFIED_FAIL";
    case CompatibilityStatus::seller_declared: return "SELLER_DECLARED";
    case CompatibilityStatus::untested: return "UNTESTED";
    case CompatibilityStatus::expired: return "EXPIRED";
    }
    return "UNTESTED";
}

core::Result<void> CompatibilityReceipt::validate() const {
    if (format_version != kCompatibilityReceiptVersion || package_digest.is_zero() ||
        !safe_token(engine) || !safe_token(profile) || !safe_token(validator_version) ||
        !printable(observed_at, kMaxMarketStringBytes)) {
        return core::Result<void>::failure(validation("compatibility receipt is invalid"));
    }
    switch (status) {
    case CompatibilityStatus::verified_pass:
    case CompatibilityStatus::verified_warn:
    case CompatibilityStatus::verified_fail:
    case CompatibilityStatus::seller_declared:
    case CompatibilityStatus::untested:
    case CompatibilityStatus::expired:
        break;
    default:
        return core::Result<void>::failure(validation("compatibility receipt status is invalid"));
    }
    const auto observed = parse_utc_timestamp(observed_at);
    if (!observed) return core::Result<void>::failure(observed.error());
    const bool verified = status == CompatibilityStatus::verified_pass ||
        status == CompatibilityStatus::verified_warn || status == CompatibilityStatus::verified_fail;
    if (verified && !valid_until.has_value()) {
        return core::Result<void>::failure(validation("verified compatibility receipt requires an expiry"));
    }
    if (valid_until.has_value()) {
        if (!printable(*valid_until, kMaxMarketStringBytes)) {
            return core::Result<void>::failure(validation("compatibility receipt expiry is invalid"));
        }
        const auto expiry = parse_utc_timestamp(*valid_until);
        if (!expiry || expiry.value() <= observed.value()) {
            return core::Result<void>::failure(validation("compatibility receipt expiry is not after observation"));
        }
    }
    if ((status == CompatibilityStatus::verified_pass || status == CompatibilityStatus::verified_warn ||
         status == CompatibilityStatus::verified_fail) &&
        (!evidence_digest.has_value() || evidence_digest->is_zero())) {
        return core::Result<void>::failure(validation("verified compatibility receipt requires evidence"));
    }
    if (warnings.size() > kMaxMarketArrayItems) {
        return core::Result<void>::failure(validation("compatibility receipt has too many warnings"));
    }
    for (const auto& warning : warnings) {
        if (!printable(warning, kMaxMarketStringBytes)) {
            return core::Result<void>::failure(validation("compatibility receipt warning is invalid"));
        }
    }
    return core::Result<void>::success();
}

bool CompatibilityReceipt::applies_to(
    const assets::Sha256Digest& package_digest_value,
    std::string_view engine_value,
    std::string_view profile_value) const noexcept {
    if ((status != CompatibilityStatus::verified_pass && status != CompatibilityStatus::verified_warn) ||
        !valid_until.has_value() || package_digest != package_digest_value ||
        engine != engine_value || profile != profile_value) {
        return false;
    }
    const auto observed = parse_utc_timestamp(observed_at);
    const auto expiry = parse_utc_timestamp(*valid_until);
    if (!observed || !expiry || expiry.value() <= observed.value()) return false;
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    return now >= observed.value() && now < expiry.value();
}

std::string CompatibilityReceipt::serialize() const {
    std::vector<std::string> warning_json;
    for (const auto& warning : warnings) warning_json.push_back(quote(warning));
    std::string result = "{\"engine\":" + quote(engine) +
        ",\"format_version\":" + uint_text32(format_version) +
        ",\"observed_at\":" + quote(observed_at) +
        ",\"package_digest\":" + quote(digest_text(package_digest)) +
        ",\"profile\":" + quote(profile) +
        ",\"status\":" + quote(status_text(status)) +
        ",\"validator_version\":" + quote(validator_version) +
        ",\"warnings\":" + join_array(warning_json);
    if (valid_until.has_value()) result += ",\"valid_until\":" + quote(*valid_until);
    if (evidence_digest.has_value()) result += ",\"evidence_digest\":" + quote(digest_text(*evidence_digest));
    result += "}";
    return result;
}

core::Result<CompatibilityReceipt> CompatibilityReceipt::deserialize(std::string_view json) {
    if (json.size() > kMaxCompatibilityReceiptBytes) {
        return core::Result<CompatibilityReceipt>::failure(validation("compatibility receipt is too large"));
    }
    const auto object = core::json::parse_object(json);
    if (!object || !allowed_fields(object.value(), {
            "evidence_digest", "engine", "format_version", "observed_at", "package_digest",
            "profile", "status", "valid_until", "validator_version", "warnings"})) {
        return core::Result<CompatibilityReceipt>::failure(
            object ? validation("compatibility receipt has an unknown field") : object.error());
    }
    const auto version = required_uint32(object.value(), "format_version");
    const auto package_text = required_string(object.value(), "package_digest", 71U);
    const auto engine = required_string(object.value(), "engine", kMaxMarketStringBytes, true);
    const auto profile = required_string(object.value(), "profile", kMaxMarketStringBytes, true);
    const auto status = required_string(object.value(), "status", kMaxMarketStringBytes, true);
    const auto validator = required_string(object.value(), "validator_version", kMaxMarketStringBytes, true);
    const auto observed = required_string(object.value(), "observed_at", kMaxMarketStringBytes);
    const auto valid_until = optional_string(object.value(), "valid_until", kMaxMarketStringBytes);
    if (!version || !package_text || !engine || !profile || !status || !validator || !observed || !valid_until) {
        return core::Result<CompatibilityReceipt>::failure(
            !version ? version.error() : !package_text ? package_text.error() : !engine ? engine.error() :
            !profile ? profile.error() : !status ? status.error() : !validator ? validator.error() :
            !observed ? observed.error() : valid_until.error());
    }
    const auto package_digest = parse_digest(package_text.value(), "package_digest");
    const auto parsed_status = parse_status(status.value());
    const auto evidence = optional_digest(object.value(), "evidence_digest");
    const auto* warning_member = core::json::find_member(object.value(), "warnings");
    if (!package_digest || !parsed_status || !evidence || warning_member == nullptr) {
        return core::Result<CompatibilityReceipt>::failure(
            !package_digest ? package_digest.error() : !parsed_status ? parsed_status.error() :
            !evidence ? evidence.error() : validation("compatibility receipt warnings are missing"));
    }
    const auto warnings = parse_string_array(warning_member->raw_value, "warnings");
    if (!warnings) return core::Result<CompatibilityReceipt>::failure(warnings.error());
    CompatibilityReceipt receipt{
        version.value(), package_digest.value(), engine.value(), profile.value(), parsed_status.value(),
        validator.value(), observed.value(), valid_until.value(), evidence.value(), warnings.value()};
    if (auto valid = receipt.validate(); !valid) return core::Result<CompatibilityReceipt>::failure(valid.error());
    return core::Result<CompatibilityReceipt>::success(std::move(receipt));
}

core::Result<void> MarketPublicationReceipt::validate() const {
    if (format_version != kMarketPublicationReceiptVersion || package_digest.is_zero() ||
        !safe_library_identity(package_id) || !safe_library_identity(listing_version_id) ||
        !safe_token(publisher_id) || !safe_token(validator_version) ||
        !safe_token(key_id) || !safe_token(algorithm) || !printable(observed_at, kMaxMarketStringBytes)) {
        return core::Result<void>::failure(validation("market publication receipt is invalid"));
    }
    if (!parse_utc_timestamp(observed_at)) {
        return core::Result<void>::failure(validation("market publication receipt timestamp is invalid"));
    }
    if (std::all_of(signature.begin(), signature.end(), [](std::uint8_t byte) { return byte == 0U; })) {
        return core::Result<void>::failure(validation("market publication receipt signature is empty"));
    }
    return core::Result<void>::success();
}

std::vector<std::uint8_t> MarketPublicationReceipt::signing_bytes() const {
    std::vector<std::uint8_t> result;
    result.reserve(256U + package_id.size() + listing_version_id.size() + publisher_id.size() +
        validator_version.size() + observed_at.size() + key_id.size() + algorithm.size());
    append_publication_text(result, "cartographer-market-publication.v1");
    append_u32_le(result, format_version);
    append_publication_text(result, package_id);
    append_publication_text(result, listing_version_id);
    append_publication_text(result, publisher_id);
    append_publication_text(result, validator_version);
    append_publication_text(result, observed_at);
    append_publication_text(result, key_id);
    append_publication_text(result, algorithm);
    result.insert(result.end(), package_digest.bytes.begin(), package_digest.bytes.end());
    return result;
}

core::Result<plugin_package::SignatureVerdict> MarketPublicationReceipt::verify(
    const plugin_package::KeyRing& key_ring) const {
    if (auto valid = validate(); !valid) {
        return core::Result<plugin_package::SignatureVerdict>::failure(valid.error());
    }
    const auto message = signing_bytes();
    const std::span<const std::uint8_t, 64> detached_signature(signature.data(), signature.size());
    return plugin_package::verify_detached_signature(
        key_id, algorithm, message, detached_signature, key_ring);
}

core::Result<assets::Sha256Digest> MarketPublicationReceipt::canonical_digest() const {
    if (auto valid = validate(); !valid) {
        return core::Result<assets::Sha256Digest>::failure(valid.error());
    }
    const std::string canonical = "cartographer-market-publication-receipt.v1\n" + serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(canonical.data());
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(std::span<const std::uint8_t>{bytes, canonical.size()}));
}

std::string MarketPublicationReceipt::serialize() const {
    return "{\"algorithm\":" + quote(algorithm) +
        ",\"format_version\":" + uint_text32(format_version) +
        ",\"key_id\":" + quote(key_id) +
        ",\"listing_version_id\":" + quote(listing_version_id) +
        ",\"observed_at\":" + quote(observed_at) +
        ",\"package_digest\":" + quote(digest_text(package_digest)) +
        ",\"package_id\":" + quote(package_id) +
        ",\"publisher_id\":" + quote(publisher_id) +
        ",\"signature\":" + quote(bytes_hex(signature)) +
        ",\"validator_version\":" + quote(validator_version) + "}";
}

core::Result<MarketPublicationReceipt> MarketPublicationReceipt::deserialize(std::string_view json) {
    if (json.size() > kMaxMarketPublicationReceiptBytes) {
        return core::Result<MarketPublicationReceipt>::failure(
            validation("market publication receipt is too large"));
    }
    const auto object = core::json::parse_object(json);
    if (!object || !allowed_fields(object.value(), {
            "algorithm", "format_version", "key_id", "listing_version_id", "observed_at",
            "package_digest", "package_id", "publisher_id", "signature", "validator_version"})) {
        return core::Result<MarketPublicationReceipt>::failure(
            object ? validation("market publication receipt has an unknown field") : object.error());
    }
    const auto version = required_uint32(object.value(), "format_version");
    const auto algorithm = required_string(object.value(), "algorithm", kMaxMarketStringBytes, true);
    const auto key_id = required_string(object.value(), "key_id", kMaxMarketStringBytes, true);
    const auto listing_version = required_string(
        object.value(), "listing_version_id", kMaxMarketStringBytes, true);
    const auto observed = required_string(object.value(), "observed_at", kMaxMarketStringBytes);
    const auto package_text = required_string(object.value(), "package_digest", 71U);
    const auto package_id = required_string(object.value(), "package_id", kMaxMarketStringBytes, true);
    const auto publisher = required_string(object.value(), "publisher_id", kMaxMarketStringBytes, true);
    const auto validator = required_string(
        object.value(), "validator_version", kMaxMarketStringBytes, true);
    const auto signature = parse_signature_bytes(object.value());
    if (!version || !algorithm || !key_id || !listing_version || !observed || !package_text ||
        !package_id || !publisher || !validator || !signature) {
        return core::Result<MarketPublicationReceipt>::failure(
            !version ? version.error() : !algorithm ? algorithm.error() : !key_id ? key_id.error() :
            !listing_version ? listing_version.error() : !observed ? observed.error() :
            !package_text ? package_text.error() : !package_id ? package_id.error() :
            !publisher ? publisher.error() : !validator ? validator.error() : signature.error());
    }
    const auto package_digest = parse_digest(package_text.value(), "package_digest");
    if (!package_digest) return core::Result<MarketPublicationReceipt>::failure(package_digest.error());
    MarketPublicationReceipt receipt{
        version.value(), package_digest.value(), package_id.value(), listing_version.value(),
        publisher.value(), validator.value(), observed.value(), key_id.value(), algorithm.value(),
        signature.value()};
    if (auto valid = receipt.validate(); !valid) {
        return core::Result<MarketPublicationReceipt>::failure(valid.error());
    }
    return core::Result<MarketPublicationReceipt>::success(std::move(receipt));
}

core::Result<void> MarketInstallReceipt::validate() const {
    if (!safe_library_identity(package_id) || !safe_library_identity(listing_version_id) ||
        package_digest.is_zero() || dependency_graph_digest.is_zero() ||
        (publication_receipt_digest.has_value() && publication_receipt_digest->is_zero())) {
        return core::Result<void>::failure(validation("market install receipt is invalid"));
    }
    return core::Result<void>::success();
}

std::string MarketInstallReceipt::serialize() const {
    std::string result = "{\"format_version\":" + uint_text32(kMarketInstallReceiptVersion) +
        ",\"listing_version_id\":" + quote(listing_version_id) +
        ",\"package_digest\":" + quote(digest_text(package_digest)) +
        ",\"dependency_graph_digest\":" + quote(digest_text(dependency_graph_digest)) +
        ",\"package_id\":" + quote(package_id);
    if (publication_receipt_digest.has_value()) {
        result += ",\"publication_receipt_digest\":" +
            quote(digest_text(*publication_receipt_digest));
    }
    result += "}";
    return result;
}

core::Result<MarketInstallReceipt> MarketInstallReceipt::deserialize(std::string_view json) {
    if (json.size() > kMaxMarketManifestBytes) {
        return core::Result<MarketInstallReceipt>::failure(
            validation("market install receipt is too large"));
    }
    const auto object = core::json::parse_object(json);
    if (!object || !allowed_fields(object.value(), {
            "dependency_graph_digest", "format_version", "listing_version_id",
            "package_digest", "package_id", "publication_receipt_digest"})) {
        return core::Result<MarketInstallReceipt>::failure(
            object ? validation("market install receipt has an unknown field") : object.error());
    }
    const auto version = required_uint32(object.value(), "format_version");
    const auto package_id = required_string(object.value(), "package_id", kMaxMarketStringBytes, true);
    const auto listing_version = required_string(
        object.value(), "listing_version_id", kMaxMarketStringBytes, true);
    const auto digest_text_value = required_string(object.value(), "package_digest", 71U);
    const auto dependency_digest_text = required_string(
        object.value(), "dependency_graph_digest", 71U);
    const auto publication_digest = optional_digest(object.value(), "publication_receipt_digest");
    if (!version || !package_id || !listing_version || !digest_text_value ||
        !dependency_digest_text || !publication_digest) {
        return core::Result<MarketInstallReceipt>::failure(
            !version ? version.error() : !package_id ? package_id.error() :
            !listing_version ? listing_version.error() : !digest_text_value ? digest_text_value.error() :
            !dependency_digest_text ? dependency_digest_text.error() : publication_digest.error());
    }
    if (version.value() != kMarketInstallReceiptVersion) {
        return core::Result<MarketInstallReceipt>::failure(
            core::Diagnostic(core::ErrorCode::version_mismatch,
                "market install receipt format is unsupported"));
    }
    const auto digest = parse_digest(digest_text_value.value(), "package_digest");
    const auto dependency_digest = parse_digest(
        dependency_digest_text.value(), "dependency_graph_digest");
    if (!digest || !dependency_digest) {
        return core::Result<MarketInstallReceipt>::failure(
            !digest ? digest.error() : dependency_digest.error());
    }
    MarketInstallReceipt receipt{
        package_id.value(), listing_version.value(), digest.value(), dependency_digest.value(), {},
        publication_digest.value()};
    if (auto valid = receipt.validate(); !valid) {
        return core::Result<MarketInstallReceipt>::failure(valid.error());
    }
    return core::Result<MarketInstallReceipt>::success(std::move(receipt));
}

MarketLibrary::MarketLibrary(std::filesystem::path root) : root_(std::move(root)) {}

core::Result<MarketInstallReceipt> MarketLibrary::install_directory(
    const std::filesystem::path& package_root,
    std::optional<assets::Sha256Digest> expected_digest,
    const MarketValidationLimits& limits) {
    return install_directory_impl(package_root, expected_digest, nullptr, nullptr, nullptr, limits);
}

core::Result<MarketInstallReceipt> MarketLibrary::install_directory(
    const std::filesystem::path& package_root,
    const DependencyResolutionPlan& resolution_plan,
    std::optional<assets::Sha256Digest> expected_digest,
    const MarketValidationLimits& limits) {
    return install_directory_impl(package_root, expected_digest, &resolution_plan, nullptr, nullptr, limits);
}

core::Result<MarketInstallReceipt> MarketLibrary::install_directory(
    const std::filesystem::path& package_root,
    const MarketPublicationReceipt& publication_receipt,
    const plugin_package::KeyRing& key_ring,
    std::optional<assets::Sha256Digest> expected_digest,
    const MarketValidationLimits& limits) {
    return install_directory_impl(
        package_root, expected_digest, nullptr, &publication_receipt, &key_ring, limits);
}

core::Result<MarketInstallReceipt> MarketLibrary::install_archive(
    const std::filesystem::path& archive_path,
    std::optional<assets::Sha256Digest> expected_digest,
    const MarketValidationLimits& limits) {
    return install_archive_impl(
        archive_path, expected_digest, nullptr, nullptr, nullptr, limits);
}

core::Result<MarketInstallReceipt> MarketLibrary::install_archive(
    const std::filesystem::path& archive_path,
    const DependencyResolutionPlan& resolution_plan,
    std::optional<assets::Sha256Digest> expected_digest,
    const MarketValidationLimits& limits) {
    return install_archive_impl(
        archive_path, expected_digest, &resolution_plan, nullptr, nullptr, limits);
}

core::Result<MarketInstallReceipt> MarketLibrary::install_archive(
    const std::filesystem::path& archive_path,
    const MarketPublicationReceipt& publication_receipt,
    const plugin_package::KeyRing& key_ring,
    std::optional<assets::Sha256Digest> expected_digest,
    const MarketValidationLimits& limits) {
    return install_archive_impl(
        archive_path, expected_digest, nullptr, &publication_receipt, &key_ring, limits);
}

core::Result<MarketInstallReceipt> MarketLibrary::install_archive_impl(
    const std::filesystem::path& archive_path,
    std::optional<assets::Sha256Digest> expected_digest,
    const DependencyResolutionPlan* resolution_plan,
    const MarketPublicationReceipt* publication_receipt,
    const plugin_package::KeyRing* key_ring,
    const MarketValidationLimits& limits) {
    if (archive_path.empty()) {
        return core::Result<MarketInstallReceipt>::failure(
            invalid("market archive path must name a file"));
    }
    const auto archive = read_file_bytes(archive_path, limits.max_archive_bytes);
    if (!archive) return core::Result<MarketInstallReceipt>::failure(archive.error());
    const auto report = MarketPackageValidator::validate_archive(
        std::span<const std::uint8_t>{archive.value().data(), archive.value().size()}, limits);
    if (!report) return core::Result<MarketInstallReceipt>::failure(report.error());
    if (expected_digest.has_value() && expected_digest != report.value().computed_package_digest) {
        return core::Result<MarketInstallReceipt>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "market archive expected digest does not match package"));
    }

    plugin_package::PackageLimits package_limits;
    package_limits.max_archive_bytes = limits.max_archive_bytes;
    if (limits.max_total_payload_bytes >
        std::numeric_limits<std::uint64_t>::max() - limits.max_manifest_bytes) {
        return core::Result<MarketInstallReceipt>::failure(
            validation("market archive limits overflow"));
    }
    package_limits.max_total_uncompressed =
        limits.max_total_payload_bytes + limits.max_manifest_bytes;
    package_limits.max_single_file = limits.max_single_file_bytes;
    package_limits.max_entries = limits.max_files;
    package_limits.max_manifest_bytes = limits.max_manifest_bytes;
    const auto entries = plugin_package::read_cartoplug(
        std::span<const std::uint8_t>{archive.value().data(), archive.value().size()}, package_limits);
    if (!entries) return core::Result<MarketInstallReceipt>::failure(entries.error());

    if (auto ready = ensure_directory(root_); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    const auto archive_staging_root = root_ / "archive-staging";
    if (auto ready = ensure_directory(archive_staging_root); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    std::filesystem::path extraction;
    bool fresh_extraction_path = false;
    for (std::uint32_t attempt = 0U; attempt < 1024U; ++attempt) {
        const auto candidate = archive_staging_root /
            ("package-" + std::to_string(static_cast<unsigned long long>(
                g_install_counter.fetch_add(1U, std::memory_order_relaxed))));
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(candidate, status_error);
        if (status.type() == std::filesystem::file_type::not_found) {
            status_error.clear();
        } else if (status_error) {
            return core::Result<MarketInstallReceipt>::failure(
                io_error("unable to inspect market archive staging path"));
        }
        if (status.type() == std::filesystem::file_type::not_found) {
            extraction = candidate;
            fresh_extraction_path = true;
            break;
        }
    }
    if (!fresh_extraction_path) {
        return core::Result<MarketInstallReceipt>::failure(
            conflict("market archive staging namespace is exhausted"));
    }
    if (auto ready = ensure_directory(extraction); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{extraction};

    for (const auto& entry : entries.value()) {
        if (!safe_package_path(entry.path)) {
            return core::Result<MarketInstallReceipt>::failure(
                validation("market archive contains an unsafe relative path"));
        }
        const auto destination = extraction / std::filesystem::path(entry.path);
        if (auto ready = ensure_directory(destination.parent_path()); !ready) {
            return core::Result<MarketInstallReceipt>::failure(ready.error());
        }
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            return core::Result<MarketInstallReceipt>::failure(
                io_error("unable to extract market archive entry"));
        }
        if (!entry.bytes.empty()) {
            output.write(reinterpret_cast<const char*>(entry.bytes.data()),
                         static_cast<std::streamsize>(entry.bytes.size()));
        }
        output.flush();
        if (!output) {
            return core::Result<MarketInstallReceipt>::failure(
                io_error("unable to write extracted market archive entry"));
        }
    }

    const auto bound_digest = expected_digest.value_or(report.value().computed_package_digest);
    return install_directory_impl(
        extraction, bound_digest, resolution_plan, publication_receipt, key_ring, limits);
}

core::Result<MarketInstallReceipt> MarketLibrary::install_directory_impl(
    const std::filesystem::path& package_root,
    std::optional<assets::Sha256Digest> expected_digest,
    const DependencyResolutionPlan* resolution_plan,
    const MarketPublicationReceipt* publication_receipt,
    const plugin_package::KeyRing* key_ring,
    const MarketValidationLimits& limits) {
    const auto report = MarketPackageValidator::validate_directory(package_root, limits);
    if (!report) return core::Result<MarketInstallReceipt>::failure(report.error());
    std::optional<assets::Sha256Digest> publication_digest;
    if (publication_receipt != nullptr) {
        if (key_ring == nullptr) {
            return core::Result<MarketInstallReceipt>::failure(
                invalid("market publication verification requires a public key ring"));
        }
        const auto verified = MarketPackageValidator::verify_publication_receipt(
            report.value(), *publication_receipt, *key_ring);
        if (!verified) return core::Result<MarketInstallReceipt>::failure(verified.error());
        if (verified.value() != plugin_package::SignatureVerdict::verified) {
            return core::Result<MarketInstallReceipt>::failure(
                validation("market publication receipt is not trusted"));
        }
        const auto digest = publication_receipt->canonical_digest();
        if (!digest) return core::Result<MarketInstallReceipt>::failure(digest.error());
        publication_digest = digest.value();
    }
    if (expected_digest.has_value() && expected_digest != report.value().computed_package_digest) {
        return core::Result<MarketInstallReceipt>::failure(
            core::Diagnostic(core::ErrorCode::stale_data, "market install expected digest does not match package"));
    }
    const auto& manifest = report.value().manifest;
    DependencyResolutionPlan effective_plan;
    if (resolution_plan != nullptr) {
        effective_plan = *resolution_plan;
    } else {
        if (!manifest.dependencies.empty()) {
            return core::Result<MarketInstallReceipt>::failure(
                validation("market install requires a dependency resolution plan"));
        }
        effective_plan.packages.push_back(ResolvedPackage{
            manifest.package_id, manifest.listing_version_id, manifest.content_digest,
            report.value().computed_package_digest});
        if (auto refreshed = effective_plan.refresh_digest(); !refreshed) {
            return core::Result<MarketInstallReceipt>::failure(refreshed.error());
        }
    }
    if (auto valid = effective_plan.validate(); !valid) {
        return core::Result<MarketInstallReceipt>::failure(valid.error());
    }
    const auto resolved_root = std::find_if(
        effective_plan.packages.begin(), effective_plan.packages.end(),
        [&manifest](const auto& package) { return package.package_id == manifest.package_id; });
    if (resolved_root == effective_plan.packages.end() ||
        resolved_root->version != manifest.listing_version_id ||
        resolved_root->content_digest != manifest.content_digest ||
        resolved_root->package_digest != report.value().computed_package_digest) {
        return core::Result<MarketInstallReceipt>::failure(
            conflict("market install resolution plan does not match package identity"));
    }
    const auto dependency_graph_digest = effective_plan.plan_digest;
    if (auto ready = ensure_directory(root_); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    if (auto ready = ensure_directory(root_ / "packages"); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    if (auto ready = ensure_directory(root_ / "receipts"); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    if (auto ready = ensure_directory(root_ / "staging"); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    std::error_code error;
    const auto destination = root_ / "packages" / manifest.package_id / manifest.listing_version_id;
    if (auto ready = ensure_directory(destination.parent_path()); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    if (std::filesystem::exists(destination, error) || error) {
        if (error) return core::Result<MarketInstallReceipt>::failure(io_error("unable to inspect installed market package"));
        const auto existing = MarketPackageValidator::validate_directory(destination, limits);
        if (!existing) return core::Result<MarketInstallReceipt>::failure(existing.error());
        if (existing.value().computed_package_digest != report.value().computed_package_digest) {
            return core::Result<MarketInstallReceipt>::failure(
                conflict("immutable market package path already contains a different digest"));
        }
        MarketInstallReceipt receipt{
            manifest.package_id, manifest.listing_version_id,
            report.value().computed_package_digest, dependency_graph_digest, destination,
            publication_digest};
        if (publication_receipt != nullptr) {
            if (auto written = write_publication_receipt(
                    publication_receipt_path(root_, manifest.package_id, manifest.listing_version_id),
                    *publication_receipt); !written) {
                return core::Result<MarketInstallReceipt>::failure(written.error());
            }
        }
        if (auto written = write_install_receipt(
                install_receipt_path(root_, manifest.package_id, manifest.listing_version_id), receipt);
            !written) {
            return core::Result<MarketInstallReceipt>::failure(written.error());
        }
        return core::Result<MarketInstallReceipt>::success(std::move(receipt));
    }

    const auto counter = g_install_counter.fetch_add(1U, std::memory_order_relaxed);
    const auto staging = root_ / "staging" / (manifest.package_id + "-" +
        manifest.listing_version_id + "-" + std::to_string(static_cast<unsigned long long>(counter)));
    if (auto ready = ensure_directory(staging.parent_path()); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    if (auto ready = ensure_directory(staging); !ready) {
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    for (const auto& entry : std::filesystem::directory_iterator(package_root, error)) {
        if (error) break;
        std::filesystem::copy(
            entry.path(), staging / entry.path().filename(),
            std::filesystem::copy_options::recursive, error);
        if (error) break;
    }
    if (error) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return core::Result<MarketInstallReceipt>::failure(io_error("market package staging copy failed"));
    }
    const auto staged = MarketPackageValidator::validate_directory(staging, limits);
    if (!staged) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return core::Result<MarketInstallReceipt>::failure(
            staged.error().with_context("market package changed during staging"));
    }
    if (staged.value().computed_package_digest != report.value().computed_package_digest) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return core::Result<MarketInstallReceipt>::failure(
            core::Diagnostic(core::ErrorCode::stale_data, "market package changed during staging"));
    }
    if (publication_receipt != nullptr) {
        const auto verified = MarketPackageValidator::verify_publication_receipt(
            staged.value(), *publication_receipt, *key_ring);
        if (!verified || verified.value() != plugin_package::SignatureVerdict::verified) {
            std::error_code ignored;
            std::filesystem::remove_all(staging, ignored);
            return core::Result<MarketInstallReceipt>::failure(
                verified ? validation("market publication receipt is not trusted") : verified.error());
        }
    }
    if (auto ready = ensure_directory(destination.parent_path()); !ready) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return core::Result<MarketInstallReceipt>::failure(ready.error());
    }
    if (std::filesystem::exists(destination, error) || error) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return core::Result<MarketInstallReceipt>::failure(conflict("market package destination became occupied"));
    }
    std::filesystem::rename(staging, destination, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return core::Result<MarketInstallReceipt>::failure(io_error("market package publication failed"));
    }
    MarketInstallReceipt receipt{
        manifest.package_id, manifest.listing_version_id,
        report.value().computed_package_digest, dependency_graph_digest, destination,
        publication_digest};
    if (publication_receipt != nullptr) {
        if (auto written = write_publication_receipt(
                publication_receipt_path(root_, manifest.package_id, manifest.listing_version_id),
                *publication_receipt); !written) {
            return core::Result<MarketInstallReceipt>::failure(written.error());
        }
    }
    if (auto written = write_install_receipt(
            install_receipt_path(root_, manifest.package_id, manifest.listing_version_id), receipt);
        !written) {
        return core::Result<MarketInstallReceipt>::failure(written.error());
    }
    return core::Result<MarketInstallReceipt>::success(std::move(receipt));
}

core::Result<MarketValidationReport> MarketLibrary::inspect(
    std::string_view package_id,
    std::string_view listing_version_id,
    const MarketValidationLimits& limits) const {
    if (!safe_library_identity(package_id) || !safe_library_identity(listing_version_id)) {
        return core::Result<MarketValidationReport>::failure(invalid("market library lookup identity is invalid"));
    }
    for (const auto& path : {
             root_, root_ / "packages", root_ / "packages" / std::filesystem::path(package_id),
             root_ / "receipts", root_ / "receipts" / std::filesystem::path(package_id)}) {
        if (auto safe = reject_symlink(path); !safe) {
            return core::Result<MarketValidationReport>::failure(safe.error());
        }
    }
    const auto package_root = root_ / "packages" / std::filesystem::path(package_id) /
        std::filesystem::path(listing_version_id);
    const auto report = MarketPackageValidator::validate_directory(package_root, limits);
    if (!report) return report;
    const auto receipt = read_install_receipt(
        install_receipt_path(root_, package_id, listing_version_id));
    if (!receipt) {
        return core::Result<MarketValidationReport>::failure(
            receipt.error().with_context("market install receipt is missing or invalid"));
    }
    if (receipt.value().package_id != package_id ||
        receipt.value().listing_version_id != listing_version_id ||
        receipt.value().package_digest != report.value().computed_package_digest) {
        return core::Result<MarketValidationReport>::failure(
            conflict("market install receipt does not match installed package"));
    }
    if (receipt.value().publication_receipt_digest.has_value()) {
        const auto publication = read_publication_receipt(
            publication_receipt_path(root_, package_id, listing_version_id));
        if (!publication) {
            return core::Result<MarketValidationReport>::failure(
                publication.error().with_context("market publication receipt is missing or invalid"));
        }
        const auto publication_digest = publication.value().canonical_digest();
        if (!publication_digest || publication_digest.value() !=
                receipt.value().publication_receipt_digest.value()) {
            return core::Result<MarketValidationReport>::failure(
                conflict("market publication receipt does not match install evidence"));
        }
    }
    return report;
}

core::Result<std::vector<MarketInstallReceipt>> MarketLibrary::list(
    const MarketValidationLimits& limits) const {
    if (auto safe = reject_symlink(root_); !safe) {
        return core::Result<std::vector<MarketInstallReceipt>>::failure(safe.error());
    }
    std::error_code error;
    const auto packages_root = root_ / "packages";
    if (auto safe = reject_symlink(packages_root); !safe) {
        return core::Result<std::vector<MarketInstallReceipt>>::failure(safe.error());
    }
    const auto packages_status = std::filesystem::symlink_status(packages_root, error);
    if (packages_status.type() == std::filesystem::file_type::not_found) {
        return core::Result<std::vector<MarketInstallReceipt>>::success({});
    }
    if (error || std::filesystem::is_symlink(packages_status) ||
        !std::filesystem::is_directory(packages_status)) {
        return core::Result<std::vector<MarketInstallReceipt>>::failure(
            io_error("market package library is missing, not a directory, or is a symlink"));
    }

    std::vector<MarketInstallReceipt> receipts;
    for (const auto& package_entry : std::filesystem::directory_iterator(packages_root, error)) {
        if (error) {
            return core::Result<std::vector<MarketInstallReceipt>>::failure(
                io_error("unable to enumerate market package library"));
        }
        const auto package_status = package_entry.symlink_status(error);
        if (error || std::filesystem::is_symlink(package_status) ||
            !std::filesystem::is_directory(package_status)) {
            return core::Result<std::vector<MarketInstallReceipt>>::failure(
                validation("market package library contains an invalid package directory"));
        }
        const std::string package_id = package_entry.path().filename().string();
        if (!safe_library_identity(package_id)) {
            return core::Result<std::vector<MarketInstallReceipt>>::failure(
                validation("market package library contains an unsafe package identity"));
        }
        for (const auto& version_entry : std::filesystem::directory_iterator(package_entry.path(), error)) {
            if (error) {
                return core::Result<std::vector<MarketInstallReceipt>>::failure(
                    io_error("unable to enumerate installed market versions"));
            }
            const auto version_status = version_entry.symlink_status(error);
            if (error || std::filesystem::is_symlink(version_status) ||
                !std::filesystem::is_directory(version_status)) {
                return core::Result<std::vector<MarketInstallReceipt>>::failure(
                    validation("market package library contains an invalid version directory"));
            }
            const std::string listing_version_id = version_entry.path().filename().string();
            if (!safe_library_identity(listing_version_id)) {
                return core::Result<std::vector<MarketInstallReceipt>>::failure(
                    validation("market package library contains an unsafe version identity"));
            }
            const auto report = inspect(package_id, listing_version_id, limits);
            if (!report) {
                return core::Result<std::vector<MarketInstallReceipt>>::failure(
                    report.error().with_context("market package library entry is invalid"));
            }
            const auto receipt = read_install_receipt(
                install_receipt_path(root_, package_id, listing_version_id));
            if (!receipt) {
                return core::Result<std::vector<MarketInstallReceipt>>::failure(
                    receipt.error().with_context("market package library receipt is invalid"));
            }
            if (receipt.value().package_id != package_id ||
                receipt.value().listing_version_id != listing_version_id ||
                receipt.value().package_digest != report.value().computed_package_digest) {
                return core::Result<std::vector<MarketInstallReceipt>>::failure(
                    conflict("market package library receipt does not match its package"));
            }
            auto installed = receipt.value();
            installed.install_root = version_entry.path();
            receipts.push_back(std::move(installed));
        }
    }
    std::sort(receipts.begin(), receipts.end(), [](const auto& left, const auto& right) {
        return std::tie(left.package_id, left.listing_version_id) <
            std::tie(right.package_id, right.listing_version_id);
    });
    return core::Result<std::vector<MarketInstallReceipt>>::success(std::move(receipts));
}

core::Result<void> MarketLibrary::verify_lockfile(
    const ProjectMarketLockfile& lockfile,
    const MarketValidationLimits& limits) const {
    if (auto valid = lockfile.validate(); !valid) return valid;
    for (const auto& locked : lockfile.packages) {
        const auto report = inspect(locked.package_id, locked.listing_version_id, limits);
        if (!report) {
            return core::Result<void>::failure(
                report.error().with_context("market lockfile package is not installed"));
        }
        const auto& manifest = report.value().manifest;
        if (manifest.package_id != locked.package_id ||
            manifest.listing_id != locked.listing_id ||
            manifest.listing_version_id != locked.listing_version_id ||
            manifest.listing_version_id != locked.package_version ||
            report.value().computed_package_digest != locked.package_digest) {
            return core::Result<void>::failure(
                conflict("market lockfile package pin differs from the installed package"));
        }
        for (const auto& locked_target : locked.targets) {
            const auto target = std::find_if(
                manifest.targets.begin(), manifest.targets.end(), [&locked_target](const auto& candidate) {
                    return candidate.engine == locked_target.engine &&
                        candidate.profile == locked_target.profile;
                });
            if (target == manifest.targets.end() || !target->digest.has_value() ||
                target->digest.value() != locked_target.artifact_digest) {
                return core::Result<void>::failure(
                    conflict("market lockfile target pin differs from the installed package"));
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::market
