#include <carto/web_geometry/overlay.hpp>
#include <carto/web_geometry/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace carto::web_geometry {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

struct ClipPoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
};

ClipPoint transform(
    const OverlayProjection& projection,
    const core::Vec3d point) noexcept {
    const auto& m = projection.values;
    return {
        m[0U] * point.x + m[1U] * point.y + m[2U] * point.z + m[3U],
        m[4U] * point.x + m[5U] * point.y + m[6U] * point.z + m[7U],
        m[8U] * point.x + m[9U] * point.y + m[10U] * point.z + m[11U],
        m[12U] * point.x + m[13U] * point.y + m[14U] * point.z + m[15U],
    };
}

std::array<core::Vec3d, 8U> corners(const core::Bounds3d& bounds) {
    return {
        core::Vec3d{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z},
        core::Vec3d{bounds.minimum.x, bounds.minimum.y, bounds.maximum.z},
        core::Vec3d{bounds.minimum.x, bounds.maximum.y, bounds.minimum.z},
        core::Vec3d{bounds.minimum.x, bounds.maximum.y, bounds.maximum.z},
        core::Vec3d{bounds.maximum.x, bounds.minimum.y, bounds.minimum.z},
        core::Vec3d{bounds.maximum.x, bounds.minimum.y, bounds.maximum.z},
        core::Vec3d{bounds.maximum.x, bounds.maximum.y, bounds.minimum.z},
        core::Vec3d{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z},
    };
}

double clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

} // namespace

core::Result<void> OverlayViewport::validate() const {
    if (width == 0U || height == 0U) {
        return core::Result<void>::failure(invalid(
            "web geometry overlay viewport must be non-zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> OverlayProjection::validate() const {
    if (!std::all_of(values.begin(), values.end(), [](const double value) {
            return std::isfinite(value);
        })) {
        return core::Result<void>::failure(invalid(
            "web geometry overlay projection must contain finite values"));
    }
    return core::Result<void>::success();
}

core::Result<void> ClusterOverlaySnapshot::validate() const {
    if (package_digest.size() != 64U ||
        !assets::Sha256Digest::from_hex(package_digest) || source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "web geometry overlay snapshot identity is invalid"));
    }
    if (auto result = viewport.validate(); !result) return result;
    std::set<ClusterId> ids;
    for (const auto& cluster : clusters) {
        if (!ids.insert(cluster.cluster).second || cluster.error_receipt_digest.size() != 64U ||
            !assets::Sha256Digest::from_hex(cluster.error_receipt_digest)) {
            return core::Result<void>::failure(validation(
                "web geometry overlay cluster identity is invalid"));
        }
        if (!std::isfinite(cluster.minimum_x) || !std::isfinite(cluster.minimum_y) ||
            !std::isfinite(cluster.maximum_x) || !std::isfinite(cluster.maximum_y) ||
            !std::isfinite(cluster.minimum_depth) || !std::isfinite(cluster.maximum_depth) ||
            cluster.minimum_x > cluster.maximum_x || cluster.minimum_y > cluster.maximum_y ||
            cluster.minimum_depth > cluster.maximum_depth || cluster.minimum_x < 0.0 ||
            cluster.minimum_y < 0.0 || cluster.maximum_x > static_cast<double>(viewport.width) ||
            cluster.maximum_y > static_cast<double>(viewport.height) ||
            cluster.minimum_depth < 0.0 || cluster.maximum_depth > 1.0) {
            return core::Result<void>::failure(validation(
                "web geometry overlay bounds are invalid"));
        }
        if (!cluster.visible && cluster.clipped) {
            return core::Result<void>::failure(validation(
                "invisible web geometry overlay cannot be marked clipped"));
        }
    }
    return core::Result<void>::success();
}

core::Result<ClusterOverlaySnapshot> project_cluster_overlays(
    const WebGeometryPackage& package,
    const OverlayProjection& projection,
    OverlayViewport viewport) {
    if (auto result = package.validate(); !result) {
        return core::Result<ClusterOverlaySnapshot>::failure(result.error());
    }
    if (auto result = projection.validate(); !result) {
        return core::Result<ClusterOverlaySnapshot>::failure(result.error());
    }
    if (auto result = viewport.validate(); !result) {
        return core::Result<ClusterOverlaySnapshot>::failure(result.error());
    }
    const auto diagnostics = inspect_package(package);
    if (!diagnostics) {
        return core::Result<ClusterOverlaySnapshot>::failure(diagnostics.error());
    }
    if (auto result = diagnostics.value().validate_against(package); !result) {
        return core::Result<ClusterOverlaySnapshot>::failure(result.error());
    }

    constexpr double kMinimumW = 1e-12;
    ClusterOverlaySnapshot result;
    result.package_digest = package.digest.hex();
    result.source_revision = package.source_revision;
    result.viewport = viewport;
    result.clusters.reserve(diagnostics.value().clusters.size());
    for (const auto& diagnostic : diagnostics.value().clusters) {
        ProjectedClusterOverlay overlay;
        overlay.cluster = diagnostic.cluster;
        overlay.parent = diagnostic.parent;
        overlay.page = diagnostic.page;
        overlay.error_receipt_digest = diagnostic.error_receipt_digest;
        overlay.grouping_only = diagnostic.grouping_only;

        const auto points = corners(diagnostic.bounds.aabb);
        std::array<ClipPoint, 8U> clip_points{};
        bool any_positive_w = false;
        bool any_non_positive_w = false;
        for (std::size_t index = 0U; index < points.size(); ++index) {
            clip_points[index] = transform(projection, points[index]);
            const auto& point = clip_points[index];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z) || !std::isfinite(point.w)) {
                return core::Result<ClusterOverlaySnapshot>::failure(validation(
                    "web geometry overlay projection produced a non-finite clip point"));
            }
            if (point.w > kMinimumW) any_positive_w = true;
            else any_non_positive_w = true;
        }
        if (!any_positive_w) {
            result.clusters.push_back(std::move(overlay));
            continue;
        }
        if (any_non_positive_w) {
            overlay.visible = true;
            overlay.clipped = true;
            overlay.minimum_x = 0.0;
            overlay.minimum_y = 0.0;
            overlay.maximum_x = static_cast<double>(viewport.width);
            overlay.maximum_y = static_cast<double>(viewport.height);
            overlay.minimum_depth = 0.0;
            overlay.maximum_depth = 1.0;
            result.clusters.push_back(std::move(overlay));
            continue;
        }

        const auto all = [&clip_points](const auto& predicate) {
            return std::all_of(clip_points.begin(), clip_points.end(), predicate);
        };
        if (all([](const ClipPoint& point) { return point.x < -point.w; }) ||
            all([](const ClipPoint& point) { return point.x > point.w; }) ||
            all([](const ClipPoint& point) { return point.y < -point.w; }) ||
            all([](const ClipPoint& point) { return point.y > point.w; }) ||
            all([](const ClipPoint& point) { return point.z < 0.0; }) ||
            all([](const ClipPoint& point) { return point.z > point.w; })) {
            result.clusters.push_back(std::move(overlay));
            continue;
        }

        double minimum_x = std::numeric_limits<double>::infinity();
        double minimum_y = std::numeric_limits<double>::infinity();
        double maximum_x = -std::numeric_limits<double>::infinity();
        double maximum_y = -std::numeric_limits<double>::infinity();
        double minimum_depth = std::numeric_limits<double>::infinity();
        double maximum_depth = -std::numeric_limits<double>::infinity();
        bool clipped = false;
        for (const auto& point : clip_points) {
            const double ndc_x = point.x / point.w;
            const double ndc_y = point.y / point.w;
            const double ndc_z = point.z / point.w;
            const double screen_x = (ndc_x + 1.0) * 0.5 * static_cast<double>(viewport.width);
            const double screen_y = (1.0 - ndc_y) * 0.5 * static_cast<double>(viewport.height);
            minimum_x = std::min(minimum_x, screen_x);
            minimum_y = std::min(minimum_y, screen_y);
            maximum_x = std::max(maximum_x, screen_x);
            maximum_y = std::max(maximum_y, screen_y);
            minimum_depth = std::min(minimum_depth, ndc_z);
            maximum_depth = std::max(maximum_depth, ndc_z);
            clipped = clipped || ndc_x < -1.0 || ndc_x > 1.0 || ndc_y < -1.0 ||
                ndc_y > 1.0 || ndc_z < 0.0 || ndc_z > 1.0;
        }
        overlay.visible = true;
        overlay.clipped = clipped;
        overlay.minimum_x = std::clamp(minimum_x, 0.0, static_cast<double>(viewport.width));
        overlay.minimum_y = std::clamp(minimum_y, 0.0, static_cast<double>(viewport.height));
        overlay.maximum_x = std::clamp(maximum_x, 0.0, static_cast<double>(viewport.width));
        overlay.maximum_y = std::clamp(maximum_y, 0.0, static_cast<double>(viewport.height));
        overlay.minimum_depth = clamp01(minimum_depth);
        overlay.maximum_depth = clamp01(maximum_depth);
        result.clusters.push_back(std::move(overlay));
    }
    if (auto validation_result = result.validate(); !validation_result) {
        return core::Result<ClusterOverlaySnapshot>::failure(validation_result.error());
    }
    return core::Result<ClusterOverlaySnapshot>::success(std::move(result));
}

} // namespace carto::web_geometry
