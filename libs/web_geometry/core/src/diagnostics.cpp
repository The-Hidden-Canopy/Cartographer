#include <carto/web_geometry/diagnostics.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace carto::web_geometry {

namespace {

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

std::string receipt_digest(
    const WebGeometryPackage& package,
    ClusterId cluster,
    std::string_view method,
    double error_bound) {
    std::ostringstream canonical;
    canonical << "package=" << package.digest.hex() << ";cluster=" << cluster
              << ";method=" << method << ";bound=" << std::setprecision(17) << error_bound;
    const std::string value = canonical.str();
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

} // namespace

core::Result<void> WebGeometryDiagnostics::validate() const {
    if (package_digest.size() != 64U ||
        !assets::Sha256Digest::from_hex(package_digest) || source_revision.exhausted() ||
        cluster_count != clusters.size() || leaf_count + grouping_count != cluster_count ||
        page_count == 0U || payload_bytes == 0U) {
        return core::Result<void>::failure(validation(
            "web geometry diagnostics summary is inconsistent"));
    }
    std::set<ClusterId> ids;
    std::size_t computed_leaf_count = 0U;
    std::size_t computed_grouping_count = 0U;
    for (const auto& cluster : clusters) {
        if (cluster.cluster >= cluster_count || !ids.insert(cluster.cluster).second ||
            !std::isfinite(cluster.geometric_error) || cluster.geometric_error < 0.0 ||
            !cluster.bounds.aabb.valid() || cluster.error_receipt_digest.size() != 64U ||
            !assets::Sha256Digest::from_hex(cluster.error_receipt_digest)) {
            return core::Result<void>::failure(validation(
                "web geometry cluster diagnostic is invalid"));
        }
        if (cluster.grouping_only) {
            ++computed_grouping_count;
            if (cluster.triangle_count != 0U || cluster.has_runtime_payload) {
                return core::Result<void>::failure(validation(
                    "grouping-only cluster diagnostic has a leaf payload"));
            }
        } else {
            ++computed_leaf_count;
            if (cluster.triangle_count == 0U || !cluster.has_runtime_payload) {
                return core::Result<void>::failure(validation(
                    "leaf cluster diagnostic is missing runtime payload"));
            }
        }
    }
    if (computed_leaf_count != leaf_count || computed_grouping_count != grouping_count) {
        return core::Result<void>::failure(validation(
            "web geometry diagnostic leaf/grouping counts are inconsistent"));
    }
    if (error_receipts.size() != clusters.size()) {
        return core::Result<void>::failure(validation(
            "web geometry error receipt cardinality is inconsistent"));
    }
    for (std::size_t index = 0U; index < error_receipts.size(); ++index) {
        const auto& receipt = error_receipts[index];
        const auto& cluster = clusters[index];
        if (receipt.cluster != cluster.cluster || receipt.method.empty() ||
            receipt.digest.size() != 64U || !assets::Sha256Digest::from_hex(receipt.digest) ||
            !std::isfinite(receipt.error_bound) || receipt.error_bound < 0.0) {
            return core::Result<void>::failure(validation(
                "web geometry error receipt is invalid"));
        }
        if (cluster.grouping_only) {
            if (!receipt.conservative || receipt.renderable || receipt.error_bound <= 0.0 ||
                receipt.method != "grouping-sphere-radius-v1") {
                return core::Result<void>::failure(validation(
                    "grouping-only cluster lacks a conservative non-renderable error receipt"));
            }
        } else if (!receipt.renderable || receipt.conservative || receipt.error_bound != 0.0 ||
                   receipt.method != "exact-leaf-v1") {
            return core::Result<void>::failure(validation(
                "exact leaf cluster has an invalid error receipt"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> WebGeometryDiagnostics::validate_against(
    const WebGeometryPackage& package) const {
    if (auto result = validate(); !result) return result;
    if (auto result = package.validate(); !result) return result;
    if (package.digest.hex() != package_digest ||
        package.source_revision != source_revision ||
        package.hierarchy.clusters.size() != cluster_count ||
        package.pages.size() != page_count ||
        package.payload.size() != payload_bytes) {
        return core::Result<void>::failure(validation(
            "web geometry diagnostics are stale for the supplied package"));
    }
    for (const auto& cluster : package.hierarchy.clusters) {
        const auto diagnostic = std::find_if(
            clusters.begin(), clusters.end(),
            [cluster_id = cluster.id](const ClusterDiagnostic& candidate) {
                return candidate.cluster == cluster_id;
            });
        const auto receipt = std::find_if(
            error_receipts.begin(), error_receipts.end(),
            [cluster_id = cluster.id](const ClusterErrorReceipt& candidate) {
                return candidate.cluster == cluster_id;
            });
        if (diagnostic == clusters.end() || receipt == error_receipts.end()) {
            return core::Result<void>::failure(validation(
                "web geometry diagnostics omit a package cluster"));
        }
        const bool grouping_only = cluster.child_count != 0U && cluster.triangle_count == 0U;
        const double error_bound = grouping_only ? cluster.bounds.sphere_radius : 0.0;
        const std::string method = grouping_only ? "grouping-sphere-radius-v1" : "exact-leaf-v1";
        const std::string expected_digest = receipt_digest(
            package, cluster.id, method, error_bound);
        const auto& expected_trace = package.provenance.at(cluster.id);
        if (diagnostic->parent != cluster.parent ||
            diagnostic->bounds.canonical() != cluster.bounds.canonical() ||
            diagnostic->triangle_count != cluster.triangle_count ||
            diagnostic->source_face_count != expected_trace.source_faces.size() ||
            diagnostic->geometric_error != cluster.geometric_error ||
            diagnostic->page != cluster.page || diagnostic->grouping_only != grouping_only ||
            diagnostic->has_runtime_payload != (cluster.page != kUnassignedRuntimePage) ||
            receipt->method != method || receipt->error_bound != error_bound ||
            receipt->conservative != grouping_only || receipt->renderable == grouping_only ||
            receipt->digest != expected_digest || diagnostic->error_receipt_digest != expected_digest) {
            return core::Result<void>::failure(validation(
                "web geometry diagnostics error receipt is stale or forged"));
        }
    }
    return core::Result<void>::success();
}

core::Result<WebGeometryDiagnostics> inspect_package(
    const WebGeometryPackage& package) {
    if (auto result = package.validate(); !result) {
        return core::Result<WebGeometryDiagnostics>::failure(result.error());
    }
    WebGeometryDiagnostics diagnostics;
    diagnostics.package_digest = package.digest.hex();
    diagnostics.source_revision = package.source_revision;
    diagnostics.cluster_count = package.hierarchy.clusters.size();
    diagnostics.page_count = package.pages.size();
    diagnostics.payload_bytes = package.payload.size();
    diagnostics.clusters.reserve(package.hierarchy.clusters.size());
    diagnostics.error_receipts.reserve(package.hierarchy.clusters.size());
    for (const auto& cluster : package.hierarchy.clusters) {
        const auto& trace = package.provenance.at(cluster.id);
        const bool grouping_only = cluster.child_count != 0U && cluster.triangle_count == 0U;
        const bool has_runtime_payload = cluster.page != kUnassignedRuntimePage;
        const double error_bound = grouping_only ? cluster.bounds.sphere_radius : 0.0;
        const std::string method = grouping_only ? "grouping-sphere-radius-v1" : "exact-leaf-v1";
        const std::string error_digest = receipt_digest(
            package, cluster.id, method, error_bound);
        diagnostics.error_receipts.push_back(ClusterErrorReceipt{
            cluster.id,
            error_bound,
            grouping_only,
            !grouping_only,
            method,
            error_digest,
        });
        diagnostics.clusters.push_back(ClusterDiagnostic{
            cluster.id,
            cluster.parent,
            cluster.bounds,
            cluster.triangle_count,
            trace.source_faces.size(),
            cluster.geometric_error,
            cluster.page,
            error_digest,
            grouping_only,
            has_runtime_payload,
        });
        if (grouping_only) {
            ++diagnostics.grouping_count;
            diagnostics.warnings.push_back(
                "cluster " + std::to_string(cluster.id) +
                " is grouping-only; conservative error receipt is non-renderable until parent simplification");
        } else {
            ++diagnostics.leaf_count;
        }
        if (!has_runtime_payload) {
            diagnostics.warnings.push_back(
                "cluster " + std::to_string(cluster.id) +
                " has no runtime page payload");
        }
    }
    if (auto result = diagnostics.validate_against(package); !result) {
        return core::Result<WebGeometryDiagnostics>::failure(result.error());
    }
    return core::Result<WebGeometryDiagnostics>::success(std::move(diagnostics));
}

} // namespace carto::web_geometry
