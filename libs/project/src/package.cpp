#include <carto/project/package.hpp>
#include <carto/core/json.hpp>
#include <carto/project/file_lock.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

namespace carto::project {

namespace {

constexpr std::size_t kMaxManifestBytes = 64U * 1024U;
constexpr std::size_t kMaxProjectIdBytes = 128U;
constexpr std::size_t kMaxNameBytes = 4096U;
constexpr std::string_view kEvaluationGraphMediaType =
    "application/vnd.cartographer.evaluation-graph+text;v=1";
std::atomic<std::uint64_t> g_package_temp_counter{0U};

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

} // namespace

core::Result<void> ProjectPackage::validate_manifest(const PackageManifest& manifest) {
    if (!safe_identifier(manifest.project_id)) {
        return core::Result<void>::failure(invalid("package project_id is not a safe identifier"));
    }
    if (!safe_json_text(manifest.name, kMaxNameBytes) ||
        !safe_json_text(manifest.units, 64U) || !safe_json_text(manifest.up_axis, 16U)) {
        return core::Result<void>::failure(invalid("package manifest contains invalid text"));
    }
    if (manifest.evaluation_graph_blob.has_value() &&
        manifest.evaluation_graph_blob->is_zero()) {
        return core::Result<void>::failure(
            invalid("package evaluation graph blob digest must not be zero"));
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
    std::string text =
        "{\n"
        "  \"format\": \"cartographer-project\",\n"
        "  \"format_version\": " + std::to_string(PackageManifest::kCurrentFormatVersion) + ",\n"
        "  \"project_id\": \"" + json_escape(manifest_.project_id) + "\",\n"
        "  \"name\": \"" + json_escape(manifest_.name) + "\",\n"
        "  \"units\": \"" + json_escape(manifest_.units) + "\",\n"
        "  \"up_axis\": \"" + json_escape(manifest_.up_axis) + "\",\n"
        "  \"coordinate_system\": {\"type\": \"local\", \"origin\": [0.0, 0.0, 0.0]},\n"
        "  \"document_database\": \"document.db\",\n"
        "  \"blob_root\": \"blobs/sha256\"";
    if (manifest_.evaluation_graph_blob.has_value()) {
        text += ",\n  \"evaluation_graph_blob\": \"" +
            manifest_.evaluation_graph_blob->hex() + "\"";
    }
    text += "\n}\n";
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_package_temp_counter.fetch_add(1U, std::memory_order_relaxed);
    const auto temporary = manifest_path().string() + ".tmp-" +
        std::to_string(static_cast<unsigned long long>(stamp)) + "-" +
        std::to_string(static_cast<unsigned long long>(thread)) + "-" +
        std::to_string(static_cast<unsigned long long>(counter));
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
    if (root.empty() || root.filename().empty()) {
        return core::Result<ProjectPackage>::failure(
            invalid("package root must name a directory"));
    }
    const auto parent = root.parent_path();
    if (!parent.empty()) {
        std::error_code parent_error;
        std::filesystem::create_directories(parent, parent_error);
        if (parent_error) {
            return core::Result<ProjectPackage>::failure(
                io_error("unable to create package parent directory"));
        }
    }
    FileLock file_lock(root);
    if (auto result = file_lock.acquire(); !result) {
        return core::Result<ProjectPackage>::failure(result.error());
    }
    std::error_code error;
    const bool root_exists = std::filesystem::exists(root, error);
    if (error) {
        return core::Result<ProjectPackage>::failure(io_error("unable to inspect package root"));
    }
    if (root_exists) {
        if (!std::filesystem::is_directory(root, error) || error) {
            return core::Result<ProjectPackage>::failure(io_error("package root is not a directory"));
        }
        const bool root_is_empty = std::filesystem::is_empty(root, error);
        if (error) {
            return core::Result<ProjectPackage>::failure(io_error("unable to inspect package root contents"));
        }
        if (!root_is_empty) {
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
    const bool regular_file = std::filesystem::is_regular_file(path, error);
    if (error) {
        return core::Result<PackageManifest>::failure(
            io_error("unable to inspect package manifest"));
    }
    if (!regular_file) {
        return core::Result<PackageManifest>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "package manifest does not exist"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<PackageManifest>::failure(
            io_error("unable to inspect package manifest size"));
    }
    if (size > kMaxManifestBytes || size > std::numeric_limits<std::size_t>::max()) {
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
    const auto object = core::json::parse_object(text);
    if (!object) {
        return core::Result<PackageManifest>::failure(
            object.error().with_context("package manifest JSON"));
    }
    const auto string_field = [&object](std::string_view key) {
        const auto* member = core::json::find_member(object.value(), key);
        if (member == nullptr) {
            return core::Result<std::string>::failure(
                validation("package manifest field is missing: " + std::string(key)));
        }
        const auto value = core::json::decode_string(member->raw_value);
        if (!value) {
            return core::Result<std::string>::failure(
                value.error().with_context("package manifest field: " + std::string(key)));
        }
        return value;
    };
    const auto integer_field = [&object](std::string_view key) {
        const auto* member = core::json::find_member(object.value(), key);
        if (member == nullptr) {
            return core::Result<std::uint64_t>::failure(
                validation("package manifest field is missing: " + std::string(key)));
        }
        const auto value = core::json::parse_uint(member->raw_value);
        if (!value) {
            return core::Result<std::uint64_t>::failure(
                value.error().with_context("package manifest field: " + std::string(key)));
        }
        return value;
    };
    const auto format = string_field("format");
    const auto version = integer_field("format_version");
    const auto project_id = string_field("project_id");
    const auto name = string_field("name");
    const auto units = string_field("units");
    const auto up_axis = string_field("up_axis");
    const auto database = string_field("document_database");
    const auto blobs = string_field("blob_root");
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
    std::optional<assets::Sha256Digest> evaluation_graph_blob;
    if (const auto* member = core::json::find_member(object.value(), "evaluation_graph_blob");
        member != nullptr) {
        const auto encoded = core::json::decode_string(member->raw_value);
        if (!encoded) {
            return core::Result<PackageManifest>::failure(
                encoded.error().with_context("package evaluation graph blob"));
        }
        const auto digest = assets::Sha256Digest::from_hex(encoded.value());
        if (!digest) {
            return core::Result<PackageManifest>::failure(
                digest.error().with_context("package evaluation graph blob"));
        }
        evaluation_graph_blob = digest.value();
    }
    PackageManifest manifest{
        project_id.value(), name.value(), units.value(), up_axis.value(), evaluation_graph_blob,
    };
    if (auto result = validate_manifest(manifest); !result) {
        return core::Result<PackageManifest>::failure(result.error());
    }
    return core::Result<PackageManifest>::success(std::move(manifest));
}

core::Result<ProjectPackage> ProjectPackage::open(const std::filesystem::path& root) {
    std::error_code error;
    const bool root_is_directory = std::filesystem::is_directory(root, error);
    if (error) {
        return core::Result<ProjectPackage>::failure(io_error("unable to inspect package root"));
    }
    if (!root_is_directory) {
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
    if (manifest_.evaluation_graph_blob.has_value()) {
        if (auto result = blob_store().verify(*manifest_.evaluation_graph_blob); !result) {
            return core::Result<void>::failure(
                result.error().with_context("package evaluation graph blob"));
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

core::Result<void> ProjectPackage::validate_document_binding(
    const ProjectDocument& document) const {
    if (auto result = validate(); !result) {
        return result;
    }
    if (document.evaluation_graph_digest() != manifest_.evaluation_graph_blob) {
        return core::Result<void>::failure(validation(
            "project document evaluation graph reference does not match package manifest"));
    }
    return core::Result<void>::success();
}

assets::BlobStore ProjectPackage::blob_store() const {
    return assets::BlobStore(root_ / "blobs");
}

core::Result<assets::BlobRef> ProjectPackage::store_evaluation_graph(
    const eval::EvaluationGraph& graph) {
    FileLock file_lock(root_);
    if (auto result = file_lock.acquire(); !result) {
        return core::Result<assets::BlobRef>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        return core::Result<assets::BlobRef>::failure(result.error());
    }
    const auto serialized = graph.serialize();
    if (!serialized) {
        return core::Result<assets::BlobRef>::failure(
            serialized.error().with_context("evaluation graph snapshot"));
    }
    auto blob = blob_store().put(
        serialized.value(), std::string{kEvaluationGraphMediaType});
    if (!blob) return blob;

    const auto previous = manifest_.evaluation_graph_blob;
    manifest_.evaluation_graph_blob = blob.value().digest;
    if (auto result = write_manifest(); !result) {
        manifest_.evaluation_graph_blob = previous;
        return core::Result<assets::BlobRef>::failure(result.error());
    }
    return blob;
}

core::Result<eval::EvaluationGraph> ProjectPackage::load_evaluation_graph() const {
    if (auto result = validate(); !result) {
        return core::Result<eval::EvaluationGraph>::failure(result.error());
    }
    if (!manifest_.evaluation_graph_blob.has_value()) {
        return core::Result<eval::EvaluationGraph>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "package does not contain an evaluation graph snapshot"));
    }
    const auto bytes = blob_store().read(*manifest_.evaluation_graph_blob);
    if (!bytes) return core::Result<eval::EvaluationGraph>::failure(bytes.error());
    const std::string serialized(
        reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    return eval::EvaluationGraph::deserialize(serialized);
}

core::Result<bool> ProjectPackage::has_document_database() const {
    std::error_code error;
    const bool exists = std::filesystem::exists(document_database_path(), error);
    if (error) {
        return core::Result<bool>::failure(io_error(
            "unable to inspect package document database"));
    }
    if (exists && !std::filesystem::is_regular_file(document_database_path(), error)) {
        if (error) {
            return core::Result<bool>::failure(io_error(
                "unable to inspect package document database"));
        }
        return core::Result<bool>::failure(validation(
            "package document database path is not a regular file"));
    }
    if (error) {
        return core::Result<bool>::failure(io_error(
            "unable to inspect package document database"));
    }
    return core::Result<bool>::success(exists);
}

} // namespace carto::project
