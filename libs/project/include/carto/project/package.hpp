#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace carto::project {

struct PackageManifest {
    static constexpr std::uint32_t kCurrentFormatVersion = 2U;

    std::string project_id;
    std::string name;
    std::string units = "meter";
    std::string up_axis = "z";

    [[nodiscard]] bool operator==(const PackageManifest&) const noexcept = default;
};

// Directory/manifest boundary for the long-term package format. This class
// intentionally does not fabricate a document.db; the SQLite authoring store
// must be supplied by a later, explicit storage backend.
class ProjectPackage {
public:
    [[nodiscard]] static core::Result<ProjectPackage> create(
        const std::filesystem::path& root,
        PackageManifest manifest);
    [[nodiscard]] static core::Result<ProjectPackage> open(
        const std::filesystem::path& root);

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] assets::BlobStore blob_store() const;

    [[nodiscard]] const PackageManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] std::filesystem::path manifest_path() const { return root_ / "manifest.json"; }
    [[nodiscard]] std::filesystem::path document_database_path() const { return root_ / "document.db"; }
    [[nodiscard]] bool has_document_database() const;

private:
    ProjectPackage(std::filesystem::path root, PackageManifest manifest)
        : root_(std::move(root)), manifest_(std::move(manifest)) {}

    [[nodiscard]] static core::Result<void> validate_manifest(const PackageManifest& manifest);
    [[nodiscard]] static core::Result<PackageManifest> read_manifest(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> write_manifest() const;
    [[nodiscard]] core::Result<void> create_layout() const;

    std::filesystem::path root_;
    PackageManifest manifest_;
};

} // namespace carto::project
