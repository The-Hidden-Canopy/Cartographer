#include <carto/web_geometry/page_interest.hpp>

#include <carto/web_geometry/diagnostics.hpp>

#include <algorithm>
#include <set>
#include <utility>

namespace carto::web_geometry {

namespace {

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

core::Result<void> RuntimePageInterestPlan::validate() const {
    if (package_digest.size() != 64U ||
        !assets::Sha256Digest::from_hex(package_digest) || source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "web geometry page-interest plan identity is invalid"));
    }
    if (!std::is_sorted(pages.begin(), pages.end()) ||
        std::adjacent_find(pages.begin(), pages.end()) != pages.end() ||
        std::find(pages.begin(), pages.end(), kUnassignedRuntimePage) != pages.end()) {
        return core::Result<void>::failure(validation(
            "web geometry page-interest plan pages are not unique and sorted"));
    }
    if (!std::is_sorted(visible_clusters.begin(), visible_clusters.end()) ||
        std::adjacent_find(visible_clusters.begin(), visible_clusters.end()) !=
            visible_clusters.end()) {
        return core::Result<void>::failure(validation(
            "web geometry page-interest plan clusters are not unique and sorted"));
    }
    return core::Result<void>::success();
}

core::Result<RuntimePageInterestPlan> plan_runtime_page_interest(
    const WebGeometryPackage& package,
    const ClusterOverlaySnapshot& overlays) {
    if (auto result = package.validate(); !result) {
        return core::Result<RuntimePageInterestPlan>::failure(result.error());
    }
    if (auto result = overlays.validate(); !result) {
        return core::Result<RuntimePageInterestPlan>::failure(result.error());
    }
    if (overlays.package_digest != package.digest.hex() ||
        overlays.source_revision != package.source_revision) {
        return core::Result<RuntimePageInterestPlan>::failure(validation(
            "web geometry overlay snapshot is stale for page-interest planning"));
    }

    const auto diagnostics = inspect_package(package);
    if (!diagnostics) {
        return core::Result<RuntimePageInterestPlan>::failure(diagnostics.error());
    }
    if (auto result = diagnostics.value().validate_against(package); !result) {
        return core::Result<RuntimePageInterestPlan>::failure(result.error());
    }

    std::set<RuntimePageId> page_ids;
    std::set<ClusterId> visible_clusters;
    for (const auto& overlay : overlays.clusters) {
        if (overlay.cluster >= package.hierarchy.clusters.size()) {
            return core::Result<RuntimePageInterestPlan>::failure(validation(
                "web geometry overlay references an unknown cluster"));
        }
        const auto& expected = diagnostics.value().clusters.at(overlay.cluster);
        if (overlay.parent != expected.parent || overlay.page != expected.page ||
            overlay.grouping_only != expected.grouping_only ||
            overlay.error_receipt_digest != expected.error_receipt_digest) {
            return core::Result<RuntimePageInterestPlan>::failure(validation(
                "web geometry overlay metadata does not match package diagnostics"));
        }
        if (!overlay.visible) continue;
        visible_clusters.insert(overlay.cluster);
        if (overlay.page != kUnassignedRuntimePage) {
            page_ids.insert(overlay.page);
        }
    }

    RuntimePageInterestPlan result;
    result.package_digest = package.digest.hex();
    result.source_revision = package.source_revision;
    result.pages.assign(page_ids.begin(), page_ids.end());
    result.visible_clusters.assign(visible_clusters.begin(), visible_clusters.end());
    if (auto validation_result = result.validate(); !validation_result) {
        return core::Result<RuntimePageInterestPlan>::failure(validation_result.error());
    }
    return core::Result<RuntimePageInterestPlan>::success(std::move(result));
}

} // namespace carto::web_geometry
