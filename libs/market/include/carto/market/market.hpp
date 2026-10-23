#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/plugin_package/signature.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::market {

inline constexpr std::uint32_t kMarketPackageFormatVersion = 1U;
inline constexpr std::uint32_t kMarketLockfileFormatVersion = 1U;
inline constexpr std::uint32_t kCompatibilityReceiptVersion = 1U;
inline constexpr std::uint32_t kMarketPublicationReceiptVersion = 1U;
inline constexpr std::uint32_t kMarketInstallReceiptVersion = 1U;

inline constexpr std::size_t kMaxMarketManifestBytes = 128U * 1024U;
inline constexpr std::size_t kMaxMarketStringBytes = 512U;
inline constexpr std::size_t kMaxMarketArrayItems = 4096U;
inline constexpr std::size_t kMaxMarketLockfileBytes = 2U * 1024U * 1024U;
inline constexpr std::size_t kMaxCompatibilityReceiptBytes = 256U * 1024U;
inline constexpr std::size_t kMaxMarketPublicationReceiptBytes = 256U * 1024U;

struct PackageEntrypoint {
    std::string kind;
    std::string path;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const PackageEntrypoint&) const noexcept = default;
};

struct TargetArtifact {
    std::string engine;
    std::string profile;
    std::string artifact;
    std::optional<assets::Sha256Digest> digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const TargetArtifact&) const noexcept = default;
};

struct DependencySpec {
    std::string package_id;
    std::string version_constraint;
    bool optional = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const DependencySpec&) const noexcept = default;
};

struct InventoryItem {
    std::string kind;
    std::uint64_t count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const InventoryItem&) const noexcept = default;
};

// This is the public, provider-neutral package identity. It contains no
// payment, seller secret, entitlement token, or runtime authority.
struct MarketPackageManifest {
    std::string format = "cartographer-market-package";
    std::uint32_t format_version = kMarketPackageFormatVersion;
    std::string package_id;
    std::string listing_id;
    std::string listing_version_id;
    std::string publisher_id;
    std::optional<std::string> source_revision;
    assets::Sha256Digest content_digest;
    std::string license_id;
    std::vector<PackageEntrypoint> entrypoints;
    std::vector<TargetArtifact> targets;
    std::vector<DependencySpec> dependencies;
    std::vector<InventoryItem> inventory;
    std::optional<std::string> provenance_receipt;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> package_identity_digest() const;
    [[nodiscard]] static core::Result<MarketPackageManifest> deserialize(
        std::string_view json);
};

struct MarketValidationLimits {
    std::uint64_t max_archive_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_payload_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_single_file_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint32_t max_files = 16'384U;
    std::uint32_t max_manifest_bytes = static_cast<std::uint32_t>(kMaxMarketManifestBytes);
};

struct MarketValidationReport {
    MarketPackageManifest manifest;
    assets::Sha256Digest computed_content_digest;
    assets::Sha256Digest computed_package_digest;
    std::uint64_t payload_bytes = 0U;
    std::uint32_t payload_files = 0U;
    bool executable_payload = false;
    std::vector<std::string> files;

    [[nodiscard]] core::Result<void> validate() const;
};

class MarketPackageValidator final {
public:
    [[nodiscard]] static core::Result<assets::Sha256Digest> directory_payload_digest(
        const std::filesystem::path& root,
        const MarketValidationLimits& limits = {});

    [[nodiscard]] static core::Result<assets::Sha256Digest> directory_package_digest(
        const std::filesystem::path& root,
        const MarketValidationLimits& limits = {});

    [[nodiscard]] static core::Result<MarketValidationReport> validate_directory(
        const std::filesystem::path& root,
        const MarketValidationLimits& limits = {});

    // The archive reader is deliberately data-only: unsupported compression,
    // traversal, links, duplicate paths, and oversized entries fail closed.
    [[nodiscard]] static core::Result<MarketValidationReport> validate_archive(
        std::span<const std::uint8_t> archive,
        const MarketValidationLimits& limits = {});

    [[nodiscard]] static core::Result<plugin_package::SignatureVerdict>
    verify_publication_receipt(
        const MarketValidationReport& report,
        const struct MarketPublicationReceipt& receipt,
        const plugin_package::KeyRing& key_ring);
};

struct ResolvedPackage {
    std::string package_id;
    std::string version;
    assets::Sha256Digest content_digest;
    assets::Sha256Digest package_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const ResolvedPackage&) const noexcept = default;
};

class DependencyResolutionPlan final {
public:
    static constexpr std::uint32_t kFormatVersion = 1U;

    std::vector<ResolvedPackage> packages;
    assets::Sha256Digest plan_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

[[nodiscard]] core::Result<DependencyResolutionPlan> resolve_dependencies(
    std::span<const MarketPackageManifest> candidates,
    std::span<const DependencySpec> roots);

struct LockedTargetArtifact {
    std::string engine;
    std::string profile;
    assets::Sha256Digest artifact_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const LockedTargetArtifact&) const noexcept = default;
};

struct LockedPackage {
    std::string package_id;
    std::string package_version;
    std::string listing_id;
    std::string listing_version_id;
    assets::Sha256Digest package_digest;
    std::string license_snapshot_id;
    std::vector<LockedTargetArtifact> targets;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const LockedPackage&) const noexcept = default;
};

class ProjectMarketLockfile final {
public:
    static constexpr std::uint32_t kFormatVersion = kMarketLockfileFormatVersion;

    std::string project_id;
    std::vector<LockedPackage> packages;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] core::Result<void> matches(
        const DependencyResolutionPlan& plan) const;
    [[nodiscard]] static core::Result<ProjectMarketLockfile> deserialize(
        std::string_view json);
};

enum class CompatibilityStatus : std::uint8_t {
    verified_pass,
    verified_warn,
    verified_fail,
    seller_declared,
    untested,
    expired,
};

[[nodiscard]] const char* compatibility_status_name(
    CompatibilityStatus status) noexcept;

struct CompatibilityReceipt {
    std::uint32_t format_version = kCompatibilityReceiptVersion;
    assets::Sha256Digest package_digest;
    std::string engine;
    std::string profile;
    CompatibilityStatus status = CompatibilityStatus::untested;
    std::string validator_version;
    std::string observed_at;
    std::optional<std::string> valid_until;
    std::optional<assets::Sha256Digest> evidence_digest;
    std::vector<std::string> warnings;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool applies_to(
        const assets::Sha256Digest& package_digest_value,
        std::string_view engine_value,
        std::string_view profile_value) const noexcept;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CompatibilityReceipt> deserialize(
        std::string_view json);
};

// Publication evidence is a provider-neutral detached signature over an
// immutable package identity and the exact publication context. It carries a
// public verification key id, never a private key, entitlement, payment, or
// catalog authority.
struct MarketPublicationReceipt {
    std::uint32_t format_version = kMarketPublicationReceiptVersion;
    assets::Sha256Digest package_digest;
    std::string package_id;
    std::string listing_version_id;
    std::string publisher_id;
    std::string validator_version;
    std::string observed_at;
    std::string key_id;
    std::string algorithm = "ed25519";
    std::array<std::uint8_t, 64> signature{};

    [[nodiscard]] core::Result<void> validate() const;
    // These bytes are the exact message a publisher/service signs. Private
    // signing keys remain outside Cartographer.
    [[nodiscard]] std::vector<std::uint8_t> signing_bytes() const;
    [[nodiscard]] core::Result<plugin_package::SignatureVerdict> verify(
        const plugin_package::KeyRing& key_ring) const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MarketPublicationReceipt> deserialize(
        std::string_view json);
};

struct MarketInstallReceipt {
    std::string package_id;
    std::string listing_version_id;
    // The immutable identity digest covers the canonical manifest and payload.
    assets::Sha256Digest package_digest;
    // The resolver plan that admitted this package to the local library.
    assets::Sha256Digest dependency_graph_digest;
    std::filesystem::path install_root;
    std::optional<assets::Sha256Digest> publication_receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MarketInstallReceipt> deserialize(
        std::string_view json);
};

// Local installation is intentionally content-addressed and transactional.
// It does not decide entitlement, charge money, execute package code, or call
// a provider. Those concerns stay in the Hub/service boundary.
class MarketLibrary final {
public:
    explicit MarketLibrary(std::filesystem::path root);

    [[nodiscard]] core::Result<MarketInstallReceipt> install_directory(
        const std::filesystem::path& package_root,
        std::optional<assets::Sha256Digest> expected_digest = std::nullopt,
        const MarketValidationLimits& limits = {});
    [[nodiscard]] core::Result<MarketInstallReceipt> install_directory(
        const std::filesystem::path& package_root,
        const DependencyResolutionPlan& resolution_plan,
        std::optional<assets::Sha256Digest> expected_digest = std::nullopt,
        const MarketValidationLimits& limits = {});
    [[nodiscard]] core::Result<MarketInstallReceipt> install_directory(
        const std::filesystem::path& package_root,
        const MarketPublicationReceipt& publication_receipt,
        const plugin_package::KeyRing& key_ring,
        std::optional<assets::Sha256Digest> expected_digest = std::nullopt,
        const MarketValidationLimits& limits = {});
    [[nodiscard]] core::Result<MarketInstallReceipt> install_archive(
        const std::filesystem::path& archive_path,
        std::optional<assets::Sha256Digest> expected_digest = std::nullopt,
        const MarketValidationLimits& limits = {});
    [[nodiscard]] core::Result<MarketInstallReceipt> install_archive(
        const std::filesystem::path& archive_path,
        const DependencyResolutionPlan& resolution_plan,
        std::optional<assets::Sha256Digest> expected_digest = std::nullopt,
        const MarketValidationLimits& limits = {});
    [[nodiscard]] core::Result<MarketInstallReceipt> install_archive(
        const std::filesystem::path& archive_path,
        const MarketPublicationReceipt& publication_receipt,
        const plugin_package::KeyRing& key_ring,
        std::optional<assets::Sha256Digest> expected_digest = std::nullopt,
        const MarketValidationLimits& limits = {});
    [[nodiscard]] core::Result<MarketValidationReport> inspect(
        std::string_view package_id,
        std::string_view listing_version_id,
        const MarketValidationLimits& limits = {}) const;
    [[nodiscard]] core::Result<std::vector<MarketInstallReceipt>> list(
        const MarketValidationLimits& limits = {}) const;
    // Verifies only the immutable package/version/target pins against the
    // local cache. Entitlement and organization policy remain service-owned.
    [[nodiscard]] core::Result<void> verify_lockfile(
        const ProjectMarketLockfile& lockfile,
        const MarketValidationLimits& limits = {}) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    [[nodiscard]] core::Result<MarketInstallReceipt> install_directory_impl(
        const std::filesystem::path& package_root,
        std::optional<assets::Sha256Digest> expected_digest,
        const DependencyResolutionPlan* resolution_plan,
        const MarketPublicationReceipt* publication_receipt,
        const plugin_package::KeyRing* key_ring,
        const MarketValidationLimits& limits);
    [[nodiscard]] core::Result<MarketInstallReceipt> install_archive_impl(
        const std::filesystem::path& archive_path,
        std::optional<assets::Sha256Digest> expected_digest,
        const DependencyResolutionPlan* resolution_plan,
        const MarketPublicationReceipt* publication_receipt,
        const plugin_package::KeyRing* key_ring,
        const MarketValidationLimits& limits);

    std::filesystem::path root_;
};

} // namespace carto::market
