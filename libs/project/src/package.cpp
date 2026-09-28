#include <carto/project/package.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace carto::project {

namespace {

constexpr std::size_t kMaxManifestBytes = 64U * 1024U;
constexpr std::size_t kMaxProjectIdBytes = 128U;
constexpr std::size_t kMaxNameBytes = 4096U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

bool safe_identifier(std::string_view value) {
    return !value.empty() && value.size() <= kMaxProjectIdBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
        });
}

bool safe_json_text(std::string_view value, std::size_t max_bytes) {
    return !value.empty() && value.size() <= max_bytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        });
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char byte : value) {
        if (byte == '\\' || byte == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(byte);
    }
    return escaped;
}

std::size_t skip_space(std::string_view text, std::size_t offset) {
    while (offset < text.size() &&
           (text[offset] == ' ' || text[offset] == '\n' || text[offset] == '\r' || text[offset] == '\t')) {
        ++offset;
    }
    return offset;
}

bool valid_json_object(std::string_view text) {
    const std::size_t first = skip_space(text, 0U);
    if (first >= text.size() || text[first] != '{') return false;

    std::size_t object_depth = 0U;
    std::size_t array_depth = 0U;
    bool in_string = false;
    bool escaped = false;
    bool root_closed = false;
    for (std::size_t offset = first; offset < text.size(); ++offset) {
        const char byte = text[offset];
        if (in_string) {
            if (escaped) {
                if (byte != '\\' && byte != '"') return false;
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            } else if (static_cast<unsigned char>(byte) < 0x20U) {
                return false;
            }
            continue;
        }

        if (root_closed) {
            if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t') return false;
            continue;
        }
        if (byte == '"') {
            in_string = true;
        } else if (byte == '{') {
            ++object_depth;
        } else if (byte == '}') {
            if (object_depth == 0U) return false;
            --object_depth;
            if (object_depth == 0U && array_depth == 0U) root_closed = true;
        } else if (byte == '[') {
            ++array_depth;
        } else if (byte == ']') {
            if (array_depth == 0U) return false;
            --array_depth;
        }
    }
    return root_closed && !in_string && !escaped && object_depth == 0U && array_depth == 0U;
}

std::size_t key_count(std::string_view text, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while (true) {
        offset = text.find(marker, offset);
        if (offset == std::string_view::npos) return count;
        const std::size_t after = skip_space(text, offset + marker.size());
        if (after < text.size() && text[after] == ':') {
            ++count;
        }
        offset += marker.size();
    }
}

core::Result<std::string> json_string_field(std::string_view text, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    const std::size_t marker_offset = text.find(marker);
    if (marker_offset == std::string_view::npos) {
        return core::Result<std::string>::failure(invalid("manifest field is missing: " + std::string(key)));
    }
    const std::size_t colon = text.find(':', marker_offset + marker.size());
    if (colon == std::string_view::npos) {
        return core::Result<std::string>::failure(invalid("manifest field has no value: " + std::string(key)));
    }
    std::size_t offset = skip_space(text, colon + 1U);
    if (offset >= text.size() || text[offset] != '"') {
        return core::Result<std::string>::failure(invalid("manifest field is not a string: " + std::string(key)));
    }
    ++offset;
    std::string value;
    while (offset < text.size()) {
        const char byte = text[offset++];
        if (byte == '"') {
            return core::Result<std::string>::success(std::move(value));
        }
        if (byte == '\\') {
            if (offset >= text.size() || (text[offset] != '\\' && text[offset] != '"')) {
                return core::Result<std::string>::failure(
                    invalid("manifest string uses unsupported escaping: " + std::string(key)));
            }
            value.push_back(text[offset++]);
            continue;
        }
        if (static_cast<unsigned char>(byte) < 0x20U) {
            return core::Result<std::string>::failure(
                invalid("manifest string contains a control byte: " + std::string(key)));
        }
        value.push_back(byte);
    }
    return core::Result<std::string>::failure(invalid("manifest string is unterminated: " + std::string(key)));
}

core::Result<std::uint32_t> json_uint_field(std::string_view text, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    const std::size_t marker_offset = text.find(marker);
    if (marker_offset == std::string_view::npos) {
        return core::Result<std::uint32_t>::failure(invalid("manifest field is missing: " + std::string(key)));
    }
    const std::size_t colon = text.find(':', marker_offset + marker.size());
    if (colon == std::string_view::npos) {
        return core::Result<std::uint32_t>::failure(invalid("manifest field has no value: " + std::string(key)));
    }
    const std::size_t start = skip_space(text, colon + 1U);
    std::size_t end = start;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
    }
    if (start == end) {
        return core::Result<std::uint32_t>::failure(invalid("manifest version is not an integer"));
    }
    std::uint32_t value = 0U;
    const auto result = std::from_chars(text.data() + start, text.data() + end, value);
    if (result.ec != std::errc{} || result.ptr != text.data() + end) {
        return core::Result<std::uint32_t>::failure(invalid("manifest version is invalid"));
    }
    return core::Result<std::uint32_t>::success(value);
}

} // namespace

core::Result<void> ProjectPackage::validate_manifest(const PackageManifest& manifest) {
    if (!safe_identifier(manifest.project_id)) {
        return core::Result<void>::failure(invalid("package project_id is not a safe identifier"));
    }
    if (!safe_json_text(manifest.name, kMaxNameBytes) ||
        !safe_json_text(manifest.units, 64U) || !safe_json_text(manifest.up_axis, 16U)) {
        return core::Result<void>::failure(invalid("package manifest contains invalid text"));
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectPackage::create_layout() const {
    static constexpr std::array<std::string_view, 8> directories{
        "blobs/sha256", "assets", "previews", "exports", "autosave", "recovery", "cache", "logs",
    };
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) {
        return core::Result<void>::failure(io_error("unable to create package root"));
    }
    for (const auto directory : directories) {
        std::filesystem::create_directories(root_ / directory, error);
        if (error) {
            return core::Result<void>::failure(io_error("unable to create package directory"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectPackage::write_manifest() const {
    const std::string text =
        "{\n"
        "  \"format\": \"cartographer-project\",\n"
        "  \"format_version\": " + std::to_string(PackageManifest::kCurrentFormatVersion) + ",\n"
        "  \"project_id\": \"" + json_escape(manifest_.project_id) + "\",\n"
        "  \"name\": \"" + json_escape(manifest_.name) + "\",\n"
        "  \"units\": \"" + json_escape(manifest_.units) + "\",\n"
        "  \"up_axis\": \"" + json_escape(manifest_.up_axis) + "\",\n"
        "  \"coordinate_system\": {\"type\": \"local\", \"origin\": [0.0, 0.0, 0.0]},\n"
        "  \"document_database\": \"document.db\",\n"
        "  \"blob_root\": \"blobs/sha256\"\n"
        "}\n";
    const auto temporary = manifest_path().string() + ".tmp-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Result<void>::failure(io_error("unable to create package manifest"));
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error("unable to write package manifest"));
    }
    std::error_code error;
    std::filesystem::rename(temporary, manifest_path(), error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return core::Result<void>::failure(io_error("unable to publish package manifest atomically"));
    }
    return core::Result<void>::success();
}

core::Result<ProjectPackage> ProjectPackage::create(
    const std::filesystem::path& root,
    PackageManifest manifest) {
    if (auto result = validate_manifest(manifest); !result) {
        return core::Result<ProjectPackage>::failure(result.error());
    }
    std::error_code error;
    if (std::filesystem::exists(root, error)) {
        if (error || !std::filesystem::is_directory(root, error)) {
            return core::Result<ProjectPackage>::failure(io_error("package root is not a directory"));
        }
        if (!std::filesystem::is_empty(root, error) || error) {
            return core::Result<ProjectPackage>::failure(
                invalid("refusing to create a package in a non-empty directory"));
        }
    }
    ProjectPackage package(root, std::move(manifest));
    if (auto result = package.create_layout(); !result) {
        return core::Result<ProjectPackage>::failure(result.error());
    }
    if (auto result = package.write_manifest(); !result) {
        return core::Result<ProjectPackage>::failure(result.error());
    }
    return core::Result<ProjectPackage>::success(std::move(package));
}

core::Result<PackageManifest> ProjectPackage::read_manifest(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return core::Result<PackageManifest>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "package manifest does not exist"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxManifestBytes || size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<PackageManifest>::failure(validation("package manifest is too large"));
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<PackageManifest>::failure(io_error("unable to open package manifest"));
    }
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream && !stream.eof()) {
        return core::Result<PackageManifest>::failure(io_error("unable to read package manifest"));
    }
    const std::size_t first = skip_space(text, 0U);
    std::size_t last = text.size();
    while (last > first &&
           (text[last - 1U] == ' ' || text[last - 1U] == '\n' || text[last - 1U] == '\r' ||
            text[last - 1U] == '\t')) {
        --last;
    }
    if (first >= last || text[first] != '{' || text[last - 1U] != '}' ||
        !valid_json_object(text.substr(first, last - first))) {
        return core::Result<PackageManifest>::failure(validation("package manifest is not one JSON object"));
    }
    for (const auto key : {"format", "format_version", "project_id", "name", "units", "up_axis",
                           "document_database", "blob_root"}) {
        if (key_count(text, key) != 1U) {
            return core::Result<PackageManifest>::failure(
                validation("package manifest has a missing or duplicate field"));
        }
    }
    const auto format = json_string_field(text, "format");
    const auto version = json_uint_field(text, "format_version");
    const auto project_id = json_string_field(text, "project_id");
    const auto name = json_string_field(text, "name");
    const auto units = json_string_field(text, "units");
    const auto up_axis = json_string_field(text, "up_axis");
    const auto database = json_string_field(text, "document_database");
    const auto blobs = json_string_field(text, "blob_root");
    if (!format || !version || !project_id || !name || !units || !up_axis || !database || !blobs) {
        const auto& diagnostic = !format ? format.error() : !version ? version.error() :
            !project_id ? project_id.error() : !name ? name.error() : !units ? units.error() :
            !up_axis ? up_axis.error() : !database ? database.error() : blobs.error();
        return core::Result<PackageManifest>::failure(diagnostic);
    }
    if (format.value() != "cartographer-project") {
        return core::Result<PackageManifest>::failure(validation("package manifest format is unsupported"));
    }
    if (version.value() > PackageManifest::kCurrentFormatVersion) {
        return core::Result<PackageManifest>::failure(version_mismatch("package manifest is from a newer format"));
    }
    if (version.value() != PackageManifest::kCurrentFormatVersion) {
        return core::Result<PackageManifest>::failure(
            version_mismatch("package manifest version has no migration in this build"));
    }
    if (database.value() != "document.db" || blobs.value() != "blobs/sha256") {
        return core::Result<PackageManifest>::failure(validation("package manifest paths are invalid"));
    }
    PackageManifest manifest{
        project_id.value(), name.value(), units.value(), up_axis.value(),
    };
    if (auto result = validate_manifest(manifest); !result) {
        return core::Result<PackageManifest>::failure(result.error());
    }
    return core::Result<PackageManifest>::success(std::move(manifest));
}

core::Result<ProjectPackage> ProjectPackage::open(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return core::Result<ProjectPackage>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "package root does not exist"));
    }
    const auto manifest = read_manifest(root / "manifest.json");
    if (!manifest) {
        return core::Result<ProjectPackage>::failure(manifest.error());
    }
    ProjectPackage package(root, manifest.value());
    if (auto result = package.validate(); !result) {
        return core::Result<ProjectPackage>::failure(result.error());
    }
    return core::Result<ProjectPackage>::success(std::move(package));
}

core::Result<void> ProjectPackage::validate() const {
    if (auto result = validate_manifest(manifest_); !result) {
        return result;
    }
    const auto on_disk = read_manifest(manifest_path());
    if (!on_disk) {
        return core::Result<void>::failure(on_disk.error());
    }
    if (on_disk.value() != manifest_) {
        return core::Result<void>::failure(validation("package manifest changed after open"));
    }
    static constexpr std::array<std::string_view, 8> directories{
        "blobs/sha256", "assets", "previews", "exports", "autosave", "recovery", "cache", "logs",
    };
    std::error_code error;
    if (!std::filesystem::is_directory(root_, error) || error) {
        return core::Result<void>::failure(validation("package root is not a directory"));
    }
    for (const auto directory : directories) {
        if (!std::filesystem::is_directory(root_ / directory, error) || error) {
            return core::Result<void>::failure(
                validation("package layout is incomplete: " + std::string(directory)));
        }
    }
    const bool database_exists = std::filesystem::exists(document_database_path(), error);
    if (error) {
        return core::Result<void>::failure(io_error("unable to inspect package document database"));
    }
    if (database_exists && !std::filesystem::is_regular_file(document_database_path(), error)) {
        return core::Result<void>::failure(
            validation("package document database path is not a regular file"));
    }
    if (error) {
        return core::Result<void>::failure(io_error("unable to inspect package document database"));
    }
    return core::Result<void>::success();
}

assets::BlobStore ProjectPackage::blob_store() const {
    return assets::BlobStore(root_ / "blobs");
}

bool ProjectPackage::has_document_database() const {
    std::error_code error;
    return std::filesystem::exists(document_database_path(), error) && !error;
}

} // namespace carto::project
