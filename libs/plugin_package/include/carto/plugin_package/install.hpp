#pragma once

#include <carto/core/result.hpp>
#include <carto/plugin_package/manifest.hpp>
#include <carto/plugin_package/package.hpp>
#include <carto/plugin_package/signature.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace carto::plugin_package {

struct VerificationReport {
    assets::Sha256Digest manifest_digest;
    assets::Sha256Digest archive_digest;
    SignatureVerdict signature_verdict;
};

[[nodiscard]] core::Result<VerificationReport> verify_package(
    std::span<const std::uint8_t> archive,
    const std::vector<PackageEntry>& entries,
    const PluginPackageManifest& manifest,
    const KeyRing& key_ring);

struct InstallReceipt {
    std::string plugin_id;
    std::string version;
    std::string publisher;
    assets::Sha256Digest package_digest;
    assets::Sha256Digest manifest_digest;
    SignatureVerdict signature_verdict = SignatureVerdict::unsigned_package;
    TrustClass trust_class = TrustClass::sandboxed_process;
    std::vector<std::string> capabilities;
    std::chrono::system_clock::time_point install_timestamp;
    std::string cartographer_version;
};

struct InstallOptions {
    bool developer_mode = false;
    KeyRing key_ring;
};

class PluginInstallStore {
public:
    explicit PluginInstallStore(std::filesystem::path root);

    [[nodiscard]] core::Result<InstallReceipt> install(
        const std::filesystem::path& package_path,
        const InstallOptions& options);
    [[nodiscard]] core::Result<void> remove(
        std::string_view plugin_id,
        std::optional<std::string_view> version = std::nullopt);
    [[nodiscard]] core::Result<std::vector<InstallReceipt>> list() const;
    [[nodiscard]] core::Result<std::optional<InstallReceipt>> find(std::string_view plugin_id) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

class PluginManager {
public:
    explicit PluginManager(PluginInstallStore store);

    [[nodiscard]] core::Result<InstallReceipt> install(
        const std::filesystem::path& package_path,
        const InstallOptions& options);
    [[nodiscard]] core::Result<void> remove(
        std::string_view plugin_id,
        std::optional<std::string_view> version = std::nullopt);
    [[nodiscard]] core::Result<std::vector<InstallReceipt>> list() const;
    [[nodiscard]] core::Result<std::optional<InstallReceipt>> find(std::string_view plugin_id) const;

private:
    PluginInstallStore store_;
};

} // namespace carto::plugin_package
