#pragma once

#include <carto/core/result.hpp>
#include <carto/web_geometry/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::web_geometry {

struct ClusterDiagnostic {
    ClusterId cluster = 0U;
    std::optional<ClusterId> parent;
    ClusterBounds bounds;
    std::uint32_t triangle_count = 0U;
    std::size_t source_face_count = 0U;
    double geometric_error = 0.0;
    RuntimePageId page = kUnassignedRuntimePage;
    std::string error_receipt_digest;
    bool grouping_only = false;
    bool has_runtime_payload = false;
};

struct ClusterErrorReceipt {
    ClusterId cluster = 0U;
    double error_bound = 0.0;
    bool conservative = false;
    bool renderable = false;
    std::string method;
    std::string digest;
};

struct WebGeometryDiagnostics {
    std::string package_digest;
    core::Revision source_revision;
    std::size_t cluster_count = 0U;
    std::size_t leaf_count = 0U;
    std::size_t grouping_count = 0U;
    std::size_t page_count = 0U;
    std::uint64_t payload_bytes = 0U;
    std::vector<ClusterDiagnostic> clusters;
    std::vector<ClusterErrorReceipt> error_receipts;
    std::vector<std::string> warnings;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_against(
        const WebGeometryPackage& package) const;
};

// Produces a deterministic, read-only inspection snapshot for editor/problem
// consumers. It does not project bounds, mutate a viewport, or admit runtime
// geometry.
[[nodiscard]] core::Result<WebGeometryDiagnostics> inspect_package(
    const WebGeometryPackage& package);

} // namespace carto::web_geometry
