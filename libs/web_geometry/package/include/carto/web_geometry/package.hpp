#pragma once

#include <carto/web_geometry/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace carto::web_geometry {

// The package format is a bounded, deterministic sidecar representation. It
// carries derived geometry only; it cannot mutate or stand in for .carto
// authoring truth.
[[nodiscard]] core::Result<std::string> serialize_package(
    const WebGeometryPackage& package);
[[nodiscard]] core::Result<WebGeometryPackage> deserialize_package(
    std::string_view text);

struct WebGeometryImportBudget {
    static constexpr std::size_t kMaxSerializedBytes = 256U * 1024U * 1024U;
    static constexpr std::uint64_t kMaxClusters = 1'000'000U;
    static constexpr std::uint64_t kMaxTriangles = 11'000'000U;
    static constexpr std::uint64_t kMaxPages = 1'000'000U;
    static constexpr std::uint64_t kMaxPayloadBytes = 256U * 1024U * 1024U;

    core::Revision expected_source_revision;
    std::size_t max_serialized_bytes = kMaxSerializedBytes;
    std::uint64_t max_clusters = kMaxClusters;
    std::uint64_t max_triangles = kMaxTriangles;
    std::uint64_t max_pages = kMaxPages;
    std::uint64_t max_payload_bytes = kMaxPayloadBytes;
    std::optional<assets::Sha256Digest> expected_options_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct WebGeometryImportReceipt {
    assets::Sha256Digest package_digest;
    core::Revision source_revision;
    assets::Sha256Digest options_digest;
    std::uint64_t cluster_count = 0U;
    std::uint64_t triangle_count = 0U;
    std::uint64_t page_count = 0U;
    std::uint64_t payload_bytes = 0U;
    std::uint64_t serialized_bytes = 0U;
    assets::Sha256Digest admission_digest;
};

struct AdmittedWebGeometryPackage {
    WebGeometryPackage package;
    WebGeometryImportReceipt receipt;
};

[[nodiscard]] core::Result<AdmittedWebGeometryPackage> admit_package_for_consumer(
    std::string_view text,
    const WebGeometryImportBudget& budget);

struct WebGeometryRuntimeBudget {
    std::string device_name;
    std::string driver;
    std::uint32_t viewport_width = 0U;
    std::uint32_t viewport_height = 0U;
    std::uint64_t max_clusters = 0U;
    std::uint64_t max_triangles = 0U;
    std::uint64_t max_pages = 0U;
    std::uint64_t max_resident_payload_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct WebGeometryRuntimeAdmissionReceipt {
    assets::Sha256Digest package_digest;
    core::Revision source_revision;
    assets::Sha256Digest options_digest;
    assets::Sha256Digest consumer_admission_digest;
    assets::Sha256Digest runtime_budget_digest;
    assets::Sha256Digest runtime_admission_digest;
    std::string device_name;
    std::string driver;
    std::uint32_t viewport_width = 0U;
    std::uint32_t viewport_height = 0U;
    std::uint64_t cluster_count = 0U;
    std::uint64_t triangle_count = 0U;
    std::uint64_t page_count = 0U;
    std::uint64_t resident_payload_bytes = 0U;
    std::uint64_t max_clusters = 0U;
    std::uint64_t max_triangles = 0U;
    std::uint64_t max_pages = 0U;
    std::uint64_t max_resident_payload_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct RuntimeAdmittedWebGeometryPackage {
    AdmittedWebGeometryPackage consumer_admission;
    WebGeometryRuntimeAdmissionReceipt runtime_receipt;
};

// Applies device-specific resource limits after the canonical consumer import
// gate. The receipt records measured package counts and the caller-supplied
// device, driver, viewport, and declared resident-payload limits.
[[nodiscard]] core::Result<RuntimeAdmittedWebGeometryPackage> admit_package_for_runtime(
    std::string_view text,
    const WebGeometryImportBudget& import_budget,
    const WebGeometryRuntimeBudget& runtime_budget);

} // namespace carto::web_geometry
