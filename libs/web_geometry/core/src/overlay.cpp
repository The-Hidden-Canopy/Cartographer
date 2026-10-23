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

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
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

std::array<float, 4U> overlay_color(ClusterId cluster, bool grouping_only) noexcept {
    if (grouping_only) return {1.0F, 0.55F, 0.08F, 1.0F};
    constexpr std::array<std::array<float, 4U>, 6U> palette = {{
        {{0.10F, 0.82F, 1.0F, 1.0F}},
        {{1.0F, 0.30F, 0.78F, 1.0F}},
        {{0.25F, 1.0F, 0.40F, 1.0F}},
        {{1.0F, 0.84F, 0.18F, 1.0F}},
        {{0.66F, 0.46F, 1.0F, 1.0F}},
        {{1.0F, 0.43F, 0.18F, 1.0F}},
    }};
    return palette[cluster % palette.size()];
}

void append_quad(
    std::vector<ClusterOverlayVertex>& vertices,
    const OverlayViewport viewport,
    double left,
    double top,
    double right,
    double bottom,
    const std::array<float, 4U>& color) {
    const auto vertex = [&viewport, &color](double x, double y) {
        return ClusterOverlayVertex{
            static_cast<float>(2.0 * x / static_cast<double>(viewport.width) - 1.0),
            static_cast<float>(1.0 - 2.0 * y / static_cast<double>(viewport.height)),
            color,
        };
    };
    const auto top_left = vertex(left, top);
    const auto bottom_left = vertex(left, bottom);
    const auto bottom_right = vertex(right, bottom);
    const auto top_right = vertex(right, top);
    vertices.insert(vertices.end(), {
        top_left, bottom_left, bottom_right,
        top_left, bottom_right, top_right,
    });
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

core::Result<void> ClusterOverlayDrawPlan::validate() const {
    if (package_digest.size() != 64U || !assets::Sha256Digest::from_hex(package_digest) ||
        source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "web geometry overlay draw-plan identity is invalid"));
    }
    if (auto result = viewport.validate(); !result) return result;
    if (draws.size() > kMaxClusterOverlayDraws) {
        return core::Result<void>::failure(validation(
            "web geometry overlay draw plan exceeds the cluster budget"));
    }
    std::set<ClusterId> ids;
    std::uint64_t expected_first_vertex = 0U;
    for (const auto& draw : draws) {
        if (!ids.insert(draw.cluster).second || draw.first_vertex != expected_first_vertex ||
            draw.vertex_count != 24U) {
            return core::Result<void>::failure(validation(
                "web geometry overlay draw ranges are not unique and contiguous"));
        }
        expected_first_vertex += draw.vertex_count;
        if (expected_first_vertex > vertices.size()) {
            return core::Result<void>::failure(validation(
                "web geometry overlay draw range exceeds its vertex payload"));
        }
    }
    if (expected_first_vertex != vertices.size()) {
        return core::Result<void>::failure(validation(
            "web geometry overlay vertex payload has unclaimed data"));
    }
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            vertex.x < -1.0F || vertex.x > 1.0F || vertex.y < -1.0F || vertex.y > 1.0F ||
            !std::all_of(vertex.color.begin(), vertex.color.end(), [](float channel) {
                return std::isfinite(channel) && channel >= 0.0F && channel <= 1.0F;
            })) {
            return core::Result<void>::failure(validation(
                "web geometry overlay vertex is outside its finite clip/color range"));
        }
    }
    return core::Result<void>::success();
}

core::Result<ClusterOverlaySnapshot> project_cluster_overlays(
    const WebGeometryPackage& package,
    const OverlayProjection& projection,
    OverlayViewport viewport,
    std::string source_object_identity) {
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
    result.source_object_identity = std::move(source_object_identity);
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

core::Result<std::vector<ClusterOverlayPointerHit>> resolve_cluster_overlay_pointer(
    const WebGeometryPackage& package,
    const ClusterOverlaySnapshot& snapshot,
    const double pointer_x,
    const double pointer_y,
    const std::string_view current_source_object_identity,
    const core::Revision current_source_revision) {
    if (auto result = package.validate(); !result) {
        return core::Result<std::vector<ClusterOverlayPointerHit>>::failure(result.error());
    }
    if (auto result = snapshot.validate(); !result) {
        return core::Result<std::vector<ClusterOverlayPointerHit>>::failure(result.error());
    }
    if (!std::isfinite(pointer_x) || !std::isfinite(pointer_y)) {
        return core::Result<std::vector<ClusterOverlayPointerHit>>::failure(invalid(
            "web geometry pointer coordinates must be finite"));
    }
    if (current_source_object_identity.empty() || snapshot.source_object_identity.empty() ||
        snapshot.source_object_identity != current_source_object_identity ||
        snapshot.package_digest != package.digest.hex() ||
        snapshot.source_revision != package.source_revision ||
        snapshot.source_revision != current_source_revision) {
        return core::Result<std::vector<ClusterOverlayPointerHit>>::failure(stale(
            "web geometry pointer snapshot is stale for the current object, package, or revision"));
    }
    if (pointer_x < 0.0 || pointer_y < 0.0 ||
        pointer_x >= static_cast<double>(snapshot.viewport.width) ||
        pointer_y >= static_cast<double>(snapshot.viewport.height)) {
        return core::Result<std::vector<ClusterOverlayPointerHit>>::success({});
    }

    struct Candidate {
        double depth = 0.0;
        ClusterOverlayPointerHit hit;
    };
    std::vector<Candidate> candidates;
    for (const auto& cluster : snapshot.clusters) {
        if (!cluster.visible || pointer_x < cluster.minimum_x ||
            pointer_x > cluster.maximum_x || pointer_y < cluster.minimum_y ||
            pointer_y > cluster.maximum_y) {
            continue;
        }
        const auto provenance = std::find_if(
            package.provenance.begin(), package.provenance.end(),
            [&cluster](const ClusterProvenance& record) {
                return record.cluster == cluster.cluster;
            });
        if (provenance == package.provenance.end()) {
            return core::Result<std::vector<ClusterOverlayPointerHit>>::failure(validation(
                "web geometry pointer cluster has no source provenance"));
        }
        candidates.push_back(Candidate{
            cluster.minimum_depth,
            ClusterOverlayPointerHit{
                snapshot.source_object_identity,
                snapshot.package_digest,
                snapshot.source_revision,
                cluster.cluster,
                cluster.page,
                provenance->source_faces,
            },
        });
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        if (left.depth != right.depth) return left.depth < right.depth;
        return left.hit.cluster < right.hit.cluster;
    });
    std::vector<ClusterOverlayPointerHit> hits;
    hits.reserve(candidates.size());
    for (auto& candidate : candidates) hits.push_back(std::move(candidate.hit));
    return core::Result<std::vector<ClusterOverlayPointerHit>>::success(std::move(hits));
}

core::Result<ClusterOverlayDrawPlan> build_cluster_overlay_draw_plan(
    const ClusterOverlaySnapshot& snapshot,
    const double border_thickness_pixels) {
    if (auto result = snapshot.validate(); !result) {
        return core::Result<ClusterOverlayDrawPlan>::failure(result.error());
    }
    if (!std::isfinite(border_thickness_pixels) || border_thickness_pixels <= 0.0 ||
        border_thickness_pixels > 64.0) {
        return core::Result<ClusterOverlayDrawPlan>::failure(invalid(
            "web geometry overlay border thickness must be finite and within (0, 64] pixels"));
    }

    std::vector<const ProjectedClusterOverlay*> visible;
    for (const auto& cluster : snapshot.clusters) {
        if (cluster.visible) visible.push_back(&cluster);
    }
    std::sort(visible.begin(), visible.end(), [](const auto* left, const auto* right) {
        if (left->minimum_depth != right->minimum_depth) {
            return left->minimum_depth > right->minimum_depth;
        }
        return left->cluster < right->cluster;
    });
    if (visible.size() > kMaxClusterOverlayDraws) {
        return core::Result<ClusterOverlayDrawPlan>::failure(validation(
            "web geometry overlay exceeds the visible-cluster draw budget"));
    }
    if (visible.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() / 24U)) {
        return core::Result<ClusterOverlayDrawPlan>::failure(validation(
            "web geometry overlay draw plan exceeds the vertex-index budget"));
    }

    ClusterOverlayDrawPlan plan;
    plan.package_digest = snapshot.package_digest;
    plan.source_object_identity = snapshot.source_object_identity;
    plan.source_revision = snapshot.source_revision;
    plan.viewport = snapshot.viewport;
    plan.vertices.reserve(visible.size() * 24U);
    plan.draws.reserve(visible.size());
    for (const auto* cluster : visible) {
        double left = cluster->minimum_x;
        double right = cluster->maximum_x;
        double top = cluster->minimum_y;
        double bottom = cluster->maximum_y;
        const auto expand_degenerate = [border_thickness_pixels](
            double& minimum, double& maximum, const double extent) {
            if (maximum - minimum >= 2.0 * border_thickness_pixels) return;
            const double center = (minimum + maximum) * 0.5;
            minimum = std::max(0.0, center - border_thickness_pixels);
            maximum = std::min(extent, center + border_thickness_pixels);
        };
        expand_degenerate(left, right, static_cast<double>(snapshot.viewport.width));
        expand_degenerate(top, bottom, static_cast<double>(snapshot.viewport.height));
        const double thickness = std::min({
            border_thickness_pixels,
            (right - left) * 0.5,
            (bottom - top) * 0.5,
        });
        const auto color = overlay_color(cluster->cluster, cluster->grouping_only);
        const std::uint32_t first_vertex = static_cast<std::uint32_t>(plan.vertices.size());
        append_quad(plan.vertices, plan.viewport, left, top, right, top + thickness, color);
        append_quad(plan.vertices, plan.viewport, left, bottom - thickness, right, bottom, color);
        append_quad(plan.vertices, plan.viewport, left, top + thickness,
                    left + thickness, bottom - thickness, color);
        append_quad(plan.vertices, plan.viewport, right - thickness, top + thickness,
                    right, bottom - thickness, color);
        plan.draws.push_back(ClusterOverlayDrawRange{
            cluster->cluster, first_vertex, 24U, cluster->grouping_only});
    }
    if (auto result = plan.validate(); !result) {
        return core::Result<ClusterOverlayDrawPlan>::failure(result.error());
    }
    return core::Result<ClusterOverlayDrawPlan>::success(std::move(plan));
}

} // namespace carto::web_geometry
