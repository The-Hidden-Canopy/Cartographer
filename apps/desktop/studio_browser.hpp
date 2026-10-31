#pragma once

#include <carto/application/application.hpp>
#include <carto/assets/blob_store.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace carto::studio {

inline constexpr std::size_t kMaxBrowserObjects = 100'000U;
inline constexpr std::size_t kMaxBrowserSearchBytes = 256U;

enum class SceneBrowserFilter { all, visible, hidden, locked, meshes };

struct SceneBrowserRow {
    std::size_t object_index = 0U;
    std::size_t depth = 0U;
    bool context_only = false;
    bool has_children = false;
};

struct SceneBrowser {
    std::vector<SceneBrowserRow> rows;
    std::size_t matching_objects = 0U;
};

struct MeshAssetBrowserRow {
    std::uint64_t mesh_asset = 0U;
    std::size_t representative_index = 0U;
    std::vector<std::size_t> object_indices;
    // Root, visible, unlocked instances with an explicit mesh revision can be
    // considered as placement sources. The viewport must still confirm that
    // its compiled mesh carries this exact revision before publishing a drag.
    std::vector<std::size_t> placeable_object_indices;
    std::optional<std::size_t> source_index;
};

[[nodiscard]] inline bool has_mesh_revision_binding(
    const application::ObjectView& object) noexcept {
    return object.mesh_revision.has_value() && object.mesh_revision->value() > 0U;
}

[[nodiscard]] inline bool mesh_revision_matches_current_snapshot(
    const application::ObjectView& object, core::Revision compiled_revision) noexcept {
    return has_mesh_revision_binding(object) &&
        *object.mesh_revision == compiled_revision;
}

struct MeshAssetBrowser {
    std::vector<MeshAssetBrowserRow> rows;
    std::size_t total_mesh_assets = 0U;
    std::size_t total_mesh_instances = 0U;
};

struct MeshThumbnailTriangle {
    std::array<std::uint32_t, 3U> vertex_indices{};
    std::array<double, 3U> camera_depth{};
    double depth = 0.0;
    double shade = 0.5;
};

// Bounded orthographic camera for the source-reimport comparison. One step is
// five degrees; elevation stays away from the pole to keep the right axis stable.
struct MeshThumbnailView {
    std::int32_t azimuth_steps = 9;
    std::int32_t elevation_steps = 6;

    [[nodiscard]] bool valid() const noexcept {
        return azimuth_steps >= 0 && azimuth_steps < 72 &&
            elevation_steps >= -17 && elevation_steps <= 17;
    }
    [[nodiscard]] bool operator==(const MeshThumbnailView&) const noexcept = default;
};

struct MeshThumbnailProjection {
    std::vector<core::Vec2d> vertices;
    std::vector<MeshThumbnailTriangle> triangles;
    // Keep source-plane bounds so review pairs can render at one mesh-unit scale.
    core::Vec2d projected_minimum{};
    core::Vec2d projected_extent{};
    assets::Sha256Digest source_digest;
    MeshThumbnailView view;
};

struct MeshThumbnailSharedFrame {
    core::Vec2d center{};
    double extent = 0.0;
};

inline constexpr std::size_t kMaxThumbnailVertices = 8'192U;
inline constexpr std::size_t kMaxThumbnailTriangles = 16'384U;
inline constexpr std::size_t kMaxThumbnailDepthPairChecks = 262'144U;
inline constexpr std::uint32_t kMaxThumbnailRasterDimension = 256U;
inline constexpr std::uint64_t kMaxThumbnailRasterPixelChecks = 4'194'304ULL;
inline constexpr double kMeshThumbnailPadding = 0.06;

namespace detail {

struct MeshThumbnailPolygon {
    std::array<core::Vec2d, 12U> points{};
    std::size_t count = 0U;

    [[nodiscard]] bool append(core::Vec2d point) noexcept {
        constexpr double duplicate_epsilon = 1.0e-14;
        if (count > 0U &&
            std::abs(points[count - 1U].x - point.x) <= duplicate_epsilon &&
            std::abs(points[count - 1U].y - point.y) <= duplicate_epsilon) {
            return true;
        }
        if (count >= points.size()) return false;
        points[count++] = point;
        return true;
    }

    void remove_repeated_closing_point() noexcept {
        constexpr double duplicate_epsilon = 1.0e-14;
        if (count > 1U &&
            std::abs(points.front().x - points[count - 1U].x) <= duplicate_epsilon &&
            std::abs(points.front().y - points[count - 1U].y) <= duplicate_epsilon) {
            --count;
        }
    }
};

[[nodiscard]] inline double mesh_thumbnail_cross(
    core::Vec2d a, core::Vec2d b, core::Vec2d c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

[[nodiscard]] inline std::optional<MeshThumbnailPolygon>
mesh_thumbnail_triangle_overlap(
    const MeshThumbnailTriangle& subject,
    const MeshThumbnailTriangle& clip,
    const std::vector<core::Vec2d>& vertices) noexcept {
    MeshThumbnailPolygon polygon;
    for (const auto index : subject.vertex_indices) {
        if (!polygon.append(vertices[index])) return std::nullopt;
    }
    polygon.remove_repeated_closing_point();
    const double clip_area = mesh_thumbnail_cross(
        vertices[clip.vertex_indices[0U]], vertices[clip.vertex_indices[1U]],
        vertices[clip.vertex_indices[2U]]);
    if (!std::isfinite(clip_area)) return std::nullopt;
    if (std::abs(clip_area) <= 1.0e-14) return MeshThumbnailPolygon{};
    const double orientation = clip_area > 0.0 ? 1.0 : -1.0;
    constexpr double inside_epsilon = 1.0e-12;
    for (std::size_t edge = 0U; edge < 3U && polygon.count > 0U; ++edge) {
        const core::Vec2d edge_start = vertices[clip.vertex_indices[edge]];
        const core::Vec2d edge_end = vertices[clip.vertex_indices[(edge + 1U) % 3U]];
        MeshThumbnailPolygon clipped;
        core::Vec2d start = polygon.points[polygon.count - 1U];
        double start_distance = orientation * mesh_thumbnail_cross(
            edge_start, edge_end, start);
        bool start_inside = start_distance >= -inside_epsilon;
        for (std::size_t index = 0U; index < polygon.count; ++index) {
            const core::Vec2d end = polygon.points[index];
            const double end_distance = orientation * mesh_thumbnail_cross(
                edge_start, edge_end, end);
            const bool end_inside = end_distance >= -inside_epsilon;
            if (start_inside != end_inside) {
                const double denominator = start_distance - end_distance;
                if (std::abs(denominator) <= 1.0e-18) return std::nullopt;
                const double factor = std::clamp(start_distance / denominator, 0.0, 1.0);
                const core::Vec2d intersection{
                    start.x + (end.x - start.x) * factor,
                    start.y + (end.y - start.y) * factor};
                if (!intersection.finite() || !clipped.append(intersection)) {
                    return std::nullopt;
                }
            }
            if (end_inside && !clipped.append(end)) return std::nullopt;
            start = end;
            start_distance = end_distance;
            start_inside = end_inside;
        }
        clipped.remove_repeated_closing_point();
        polygon = clipped;
    }
    return polygon;
}

[[nodiscard]] inline double mesh_thumbnail_interpolated_depth(
    const MeshThumbnailTriangle& triangle,
    const std::vector<core::Vec2d>& vertices,
    core::Vec2d point) noexcept {
    const auto a = vertices[triangle.vertex_indices[0U]];
    const auto b = vertices[triangle.vertex_indices[1U]];
    const auto c = vertices[triangle.vertex_indices[2U]];
    const double area = mesh_thumbnail_cross(a, b, c);
    if (!std::isfinite(area) || std::abs(area) <= 1.0e-14) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double first_weight = mesh_thumbnail_cross(b, c, point) / area;
    const double second_weight = mesh_thumbnail_cross(c, a, point) / area;
    const double third_weight = 1.0 - first_weight - second_weight;
    return first_weight * triangle.camera_depth[0U] +
        second_weight * triangle.camera_depth[1U] +
        third_weight * triangle.camera_depth[2U];
}

[[nodiscard]] inline double mesh_thumbnail_overlap_area(
    const MeshThumbnailPolygon& polygon) noexcept {
    if (polygon.count < 3U) return 0.0;
    double twice_area = 0.0;
    for (std::size_t index = 0U; index < polygon.count; ++index) {
        const auto& current = polygon.points[index];
        const auto& next = polygon.points[(index + 1U) % polygon.count];
        twice_area += current.x * next.y - next.x * current.y;
    }
    return std::abs(twice_area) * 0.5;
}

[[nodiscard]] inline core::Result<std::vector<std::size_t>>
mesh_thumbnail_depth_order(
    const std::vector<MeshThumbnailTriangle>& triangles,
    const std::vector<core::Vec2d>& vertices) {
    using Result = core::Result<std::vector<std::size_t>>;
    if (triangles.empty()) {
        return Result::success(std::vector<std::size_t>{});
    }
    struct Bounds final {
        double minimum_x = 0.0;
        double maximum_x = 0.0;
        double minimum_y = 0.0;
        double maximum_y = 0.0;
    };
    std::vector<Bounds> bounds;
    bounds.reserve(triangles.size());
    std::vector<std::size_t> by_minimum_x(triangles.size());
    for (std::size_t index = 0U; index < triangles.size(); ++index) {
        const auto& triangle = triangles[index];
        Bounds item{
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()};
        for (const auto vertex_index : triangle.vertex_indices) {
            const auto& point = vertices[vertex_index];
            item.minimum_x = std::min(item.minimum_x, point.x);
            item.maximum_x = std::max(item.maximum_x, point.x);
            item.minimum_y = std::min(item.minimum_y, point.y);
            item.maximum_y = std::max(item.maximum_y, point.y);
        }
        bounds.push_back(item);
        by_minimum_x[index] = index;
    }
    std::stable_sort(by_minimum_x.begin(), by_minimum_x.end(),
        [&bounds](std::size_t left, std::size_t right) {
            return bounds[left].minimum_x < bounds[right].minimum_x;
        });

    std::vector<std::pair<std::size_t, std::size_t>> ordering_edges;
    std::vector<std::size_t> active;
    constexpr double projection_epsilon = 1.0e-12;
    std::size_t pair_checks = 0U;

    for (const std::size_t current : by_minimum_x) {
        active.erase(std::remove_if(active.begin(), active.end(),
            [&bounds, current](std::size_t previous) {
                return bounds[previous].maximum_x <
                    bounds[current].minimum_x - projection_epsilon;
            }), active.end());
        for (const std::size_t previous : active) {
            if (++pair_checks > kMaxThumbnailDepthPairChecks) {
                return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
                    "Mesh thumbnail overlap exceeds the bounded depth-order budget."));
            }
            if (bounds[previous].maximum_y <
                    bounds[current].minimum_y - projection_epsilon ||
                bounds[current].maximum_y <
                    bounds[previous].minimum_y - projection_epsilon) {
                continue;
            }
            const auto overlap = mesh_thumbnail_triangle_overlap(
                triangles[previous], triangles[current], vertices);
            if (!overlap) {
                return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
                    "Mesh thumbnail triangle overlap could not be resolved safely."));
            }
            if (mesh_thumbnail_overlap_area(*overlap) <= 1.0e-14) continue;

            double minimum_difference = std::numeric_limits<double>::infinity();
            double maximum_difference = -std::numeric_limits<double>::infinity();
            double depth_magnitude = 1.0;
            for (std::size_t point = 0U; point < overlap->count; ++point) {
                const double previous_depth = mesh_thumbnail_interpolated_depth(
                    triangles[previous], vertices, overlap->points[point]);
                const double current_depth = mesh_thumbnail_interpolated_depth(
                    triangles[current], vertices, overlap->points[point]);
                const double difference = previous_depth - current_depth;
                if (!std::isfinite(difference)) {
                    return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
                        "Mesh thumbnail depth comparison is not finite."));
                }
                depth_magnitude = std::max(depth_magnitude,
                    std::max(std::abs(previous_depth), std::abs(current_depth)));
                minimum_difference = std::min(minimum_difference, difference);
                maximum_difference = std::max(maximum_difference, difference);
            }
            const double depth_epsilon = 64.0 *
                std::numeric_limits<double>::epsilon() * depth_magnitude;
            if (minimum_difference < -depth_epsilon &&
                maximum_difference > depth_epsilon) {
                return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
                    "Mesh thumbnail surfaces cross in projected depth; a depth-correct preview is unavailable."));
            }
            if (maximum_difference > depth_epsilon) {
                // The previous triangle is closer, so draw the current one first.
                ordering_edges.emplace_back(current, previous);
            } else if (minimum_difference < -depth_epsilon) {
                ordering_edges.emplace_back(previous, current);
            }
        }
        active.push_back(current);
    }

    std::vector<std::size_t> out_degree(triangles.size(), 0U);
    std::vector<std::size_t> in_degree(triangles.size(), 0U);
    for (const auto& [farther, closer] : ordering_edges) {
        ++out_degree[farther];
        ++in_degree[closer];
    }
    std::vector<std::size_t> offsets(triangles.size() + 1U, 0U);
    for (std::size_t index = 0U; index < triangles.size(); ++index) {
        offsets[index + 1U] = offsets[index] + out_degree[index];
    }
    std::vector<std::size_t> targets(ordering_edges.size());
    std::vector<std::size_t> cursor(offsets.begin(), offsets.end() - 1);
    for (const auto& [farther, closer] : ordering_edges) {
        targets[cursor[farther]++] = closer;
    }

    const auto compare = [&triangles](std::size_t left, std::size_t right) {
        if (triangles[left].depth != triangles[right].depth) {
            return triangles[left].depth > triangles[right].depth;
        }
        return left > right;
    };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(compare)>
        ready(compare);
    for (std::size_t index = 0U; index < in_degree.size(); ++index) {
        if (in_degree[index] == 0U) ready.push(index);
    }
    std::vector<std::size_t> ordered;
    ordered.reserve(triangles.size());
    while (!ready.empty()) {
        const std::size_t current = ready.top();
        ready.pop();
        ordered.push_back(current);
        for (std::size_t edge = offsets[current]; edge < offsets[current + 1U]; ++edge) {
            const std::size_t next = targets[edge];
            if (--in_degree[next] == 0U) ready.push(next);
        }
    }
    if (ordered.size() != triangles.size()) {
        return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "Mesh thumbnail has cyclic projected occlusion; a depth-correct preview is unavailable."));
    }
    return Result::success(std::move(ordered));
}

} // namespace detail

inline assets::Sha256Digest mesh_thumbnail_source_digest(
    const geometry::CompiledMesh& mesh) {
    std::vector<std::uint8_t> bytes;
    const auto append = [&bytes](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        static_assert(std::is_trivially_copyable_v<Value>);
        const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.insert(bytes.end(), begin, begin + sizeof(Value));
    };
    const auto append_count = [&append](std::size_t count) {
        const auto canonical_count = static_cast<std::uint64_t>(count);
        append(canonical_count);
    };

    append_count(mesh.positions.size());
    append_count(mesh.indices.size());
    if (mesh.positions.size() <= kMaxThumbnailVertices &&
        mesh.indices.size() / 3U <= kMaxThumbnailTriangles) {
        bytes.reserve(2U * sizeof(std::uint64_t) +
            mesh.positions.size() * 3U * sizeof(double) +
            mesh.indices.size() * sizeof(std::uint32_t));
        for (const auto& point : mesh.positions) {
            append(point.x);
            append(point.y);
            append(point.z);
        }
        for (const std::uint32_t index : mesh.indices) append(index);
    }
    return assets::sha256(std::span<const std::uint8_t>{bytes});
}

inline core::Result<MeshThumbnailProjection> build_mesh_thumbnail_projection(
    const geometry::CompiledMesh& mesh,
    MeshThumbnailView view = {}) {
    using Result = core::Result<MeshThumbnailProjection>;
    if (!mesh.valid() || mesh.positions.empty()) {
        return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "Mesh thumbnail requires valid nonempty compiled geometry."));
    }
    if (!view.valid()) {
        return Result::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "Mesh thumbnail camera is outside its bounded azimuth/elevation range."));
    }
    if (mesh.positions.size() > kMaxThumbnailVertices ||
        mesh.indices.size() / 3U > kMaxThumbnailTriangles) {
        return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "Mesh exceeds the bounded thumbnail projection size."));
    }

    constexpr double kDegreesToRadians = 0.08726646259971647;
    const double azimuth = static_cast<double>(view.azimuth_steps) *
        5.0 * kDegreesToRadians;
    const double elevation = static_cast<double>(view.elevation_steps) *
        5.0 * kDegreesToRadians;
    const core::Vec3d view_direction{
        std::cos(elevation) * std::cos(azimuth),
        std::sin(elevation),
        std::cos(elevation) * std::sin(azimuth)};
    const core::Vec3d right = core::Vec3d{
        view_direction.z, 0.0, -view_direction.x}.normalized();
    const core::Vec3d camera_up = core::cross(view_direction, right).normalized();
    MeshThumbnailProjection result;
    result.vertices.reserve(mesh.positions.size());
    std::vector<double> depths;
    depths.reserve(mesh.positions.size());
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();
    for (const auto& point : mesh.positions) {
        const double projected_x = core::dot(point, right);
        const double projected_y = core::dot(point, camera_up);
        const double depth = core::dot(point, view_direction);
        if (!std::isfinite(projected_x) || !std::isfinite(projected_y) ||
            !std::isfinite(depth)) {
            return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
                "Mesh coordinate range cannot be projected safely for a thumbnail."));
        }
        minimum_x = std::min(minimum_x, projected_x);
        minimum_y = std::min(minimum_y, projected_y);
        maximum_x = std::max(maximum_x, projected_x);
        maximum_y = std::max(maximum_y, projected_y);
        result.vertices.push_back({projected_x, projected_y});
        depths.push_back(depth);
    }
    const double extent_x = maximum_x - minimum_x;
    const double extent_y = maximum_y - minimum_y;
    if (!std::isfinite(extent_x) || !std::isfinite(extent_y) ||
        extent_x <= 1.0e-12 || extent_y <= 1.0e-12) {
        return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "Mesh has no stable three-dimensional thumbnail projection."));
    }
    result.projected_minimum = {minimum_x, minimum_y};
    result.projected_extent = {extent_x, extent_y};
    result.source_digest = mesh_thumbnail_source_digest(mesh);
    result.view = view;
    constexpr double kPadding = kMeshThumbnailPadding;
    for (auto& point : result.vertices) {
        point.x = kPadding + (point.x - minimum_x) / extent_x * (1.0 - 2.0 * kPadding);
        point.y = 1.0 - kPadding -
            (point.y - minimum_y) / extent_y * (1.0 - 2.0 * kPadding);
    }

    result.triangles.reserve(mesh.indices.size() / 3U);
    for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U) {
        const std::array<std::uint32_t, 3U> indices{
            mesh.indices[offset], mesh.indices[offset + 1U], mesh.indices[offset + 2U]};
        const auto& a = mesh.positions[indices[0U]];
        const auto& b = mesh.positions[indices[1U]];
        const auto& c = mesh.positions[indices[2U]];
        const double ab_x = b.x - a.x;
        const double ab_y = b.y - a.y;
        const double ab_z = b.z - a.z;
        const double ac_x = c.x - a.x;
        const double ac_y = c.y - a.y;
        const double ac_z = c.z - a.z;
        const double normal_x = ab_y * ac_z - ab_z * ac_y;
        const double normal_y = ab_z * ac_x - ab_x * ac_z;
        const double normal_z = ab_x * ac_y - ab_y * ac_x;
        const double normal_length = std::sqrt(normal_x * normal_x +
            normal_y * normal_y + normal_z * normal_z);
        double shade = 0.32;
        if (std::isfinite(normal_length) && normal_length > 1.0e-12) {
            constexpr double kLightX = 0.3651483716701107;
            constexpr double kLightY = 0.9128709291752769;
            constexpr double kLightZ = -0.1825741858350554;
            const double diffuse = std::abs((normal_x * kLightX + normal_y * kLightY +
                normal_z * kLightZ) / normal_length);
            shade += 0.68 * std::clamp(diffuse, 0.0, 1.0);
        }
        const double depth = (depths[indices[0U]] + depths[indices[1U]] +
            depths[indices[2U]]) / 3.0;
        if (!std::isfinite(depth) || !std::isfinite(shade)) {
            return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
                "Mesh triangle cannot be ordered safely for a thumbnail."));
        }
        result.triangles.push_back({
            indices,
            {depths[indices[0U]], depths[indices[1U]], depths[indices[2U]]},
            depth,
            shade});
    }
    // Triangle order is preserved; all visible thumbnail paths resolve
    // overlapping surfaces in the per-pixel depth buffer below.
    return Result::success(std::move(result));
}

[[nodiscard]] inline std::optional<MeshThumbnailSharedFrame> mesh_thumbnail_shared_frame(
    const MeshThumbnailProjection* first,
    const MeshThumbnailProjection* second) noexcept {
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();
    bool has_projection = false;
    const auto include = [&](const MeshThumbnailProjection* projection) {
        if (projection == nullptr) return true;
        const double right = projection->projected_minimum.x +
            projection->projected_extent.x;
        const double top = projection->projected_minimum.y +
            projection->projected_extent.y;
        if (!std::isfinite(projection->projected_minimum.x) ||
            !std::isfinite(projection->projected_minimum.y) ||
            !std::isfinite(projection->projected_extent.x) ||
            !std::isfinite(projection->projected_extent.y) ||
            !std::isfinite(right) || !std::isfinite(top) ||
            projection->projected_extent.x <= 1.0e-12 ||
            projection->projected_extent.y <= 1.0e-12) {
            return false;
        }
        minimum_x = std::min(minimum_x, projection->projected_minimum.x);
        minimum_y = std::min(minimum_y, projection->projected_minimum.y);
        maximum_x = std::max(maximum_x, right);
        maximum_y = std::max(maximum_y, top);
        has_projection = true;
        return true;
    };
    if (!include(first) || !include(second) || !has_projection) return std::nullopt;
    const double extent_x = maximum_x - minimum_x;
    const double extent_y = maximum_y - minimum_y;
    const double extent = std::max(extent_x, extent_y);
    const core::Vec2d center{
        minimum_x + extent_x * 0.5,
        minimum_y + extent_y * 0.5};
    if (!std::isfinite(extent_x) || !std::isfinite(extent_y) ||
        !std::isfinite(extent) || extent <= 1.0e-12 ||
        !std::isfinite(center.x) || !std::isfinite(center.y)) {
        return std::nullopt;
    }
    return MeshThumbnailSharedFrame{center, extent};
}

[[nodiscard]] inline std::optional<core::Vec2d> mesh_thumbnail_vertex_at_shared_frame(
    const MeshThumbnailProjection& projection,
    std::size_t vertex_index,
    const MeshThumbnailSharedFrame& frame) noexcept {
    constexpr double content_extent = 1.0 - 2.0 * kMeshThumbnailPadding;
    if (vertex_index >= projection.vertices.size() ||
        !std::isfinite(frame.extent) || frame.extent <= 1.0e-12 ||
        !std::isfinite(frame.center.x) || !std::isfinite(frame.center.y) ||
        !std::isfinite(projection.projected_minimum.x) ||
        !std::isfinite(projection.projected_minimum.y) ||
        !std::isfinite(projection.projected_extent.x) ||
        !std::isfinite(projection.projected_extent.y) ||
        projection.projected_extent.x <= 1.0e-12 ||
        projection.projected_extent.y <= 1.0e-12) {
        return std::nullopt;
    }
    const auto normalized = projection.vertices[vertex_index];
    const double projected_x = projection.projected_minimum.x +
        (normalized.x - kMeshThumbnailPadding) / content_extent *
            projection.projected_extent.x;
    const double projected_y = projection.projected_minimum.y +
        (1.0 - kMeshThumbnailPadding - normalized.y) / content_extent *
            projection.projected_extent.y;
    const core::Vec2d point{
        0.5 + (projected_x - frame.center.x) / frame.extent * content_extent,
        0.5 - (projected_y - frame.center.y) / frame.extent * content_extent};
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return std::nullopt;
    return point;
}

struct MeshThumbnailRaster {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> rgba8;
    // Retained as review evidence for deterministic depth-buffer regressions.
    // A negative-infinity value denotes a pixel not covered by a triangle.
    std::vector<double> camera_depth;
    assets::Sha256Digest pixel_digest;
};

[[nodiscard]] inline core::Result<MeshThumbnailRaster> rasterize_mesh_thumbnail(
    const MeshThumbnailProjection& projection,
    const MeshThumbnailSharedFrame& frame,
    std::uint32_t width = 144U,
    std::uint32_t height = 144U) {
    using Result = core::Result<MeshThumbnailRaster>;
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(width) * height;
    if (width == 0U || height == 0U ||
        width > kMaxThumbnailRasterDimension ||
        height > kMaxThumbnailRasterDimension || pixel_count > 65'536ULL) {
        return Result::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "Mesh thumbnail raster dimensions exceed the bounded image size."));
    }
    if (projection.vertices.size() > kMaxThumbnailVertices ||
        projection.triangles.size() > kMaxThumbnailTriangles ||
        !std::isfinite(frame.center.x) || !std::isfinite(frame.center.y) ||
        !std::isfinite(frame.extent) || frame.extent <= 1.0e-12) {
        return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "Mesh thumbnail raster input is outside its finite geometry bounds."));
    }

    MeshThumbnailRaster raster;
    raster.width = width;
    raster.height = height;
    raster.rgba8.assign(static_cast<std::size_t>(pixel_count) * 4U, 0U);
    raster.camera_depth.assign(static_cast<std::size_t>(pixel_count),
        -std::numeric_limits<double>::infinity());
    std::uint64_t pixel_checks = 0U;
    for (const auto& triangle : projection.triangles) {
        std::array<core::Vec2d, 3U> points{};
        for (std::size_t corner = 0U; corner < points.size(); ++corner) {
            const auto point = mesh_thumbnail_vertex_at_shared_frame(
                projection, triangle.vertex_indices[corner], frame);
            const double depth = triangle.camera_depth[corner];
            if (!point.has_value() || !std::isfinite(depth) ||
                point->x < -1.0e-9 || point->x > 1.0 + 1.0e-9 ||
                point->y < -1.0e-9 || point->y > 1.0 + 1.0e-9) {
                return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "Mesh thumbnail triangle has an invalid shared-frame vertex."));
            }
            points[corner] = {
                point->x * static_cast<double>(width - 1U),
                point->y * static_cast<double>(height - 1U)};
        }
        if (!std::isfinite(triangle.shade)) {
            return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "Mesh thumbnail triangle shading is not finite."));
        }
        const double denominator =
            (points[1U].y - points[2U].y) * (points[0U].x - points[2U].x) +
            (points[2U].x - points[1U].x) * (points[0U].y - points[2U].y);
        if (!std::isfinite(denominator)) {
            return Result::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "Mesh thumbnail triangle cannot be rasterized safely."));
        }
        if (std::abs(denominator) <= 1.0e-12) continue;

        const double minimum_x = std::min({points[0U].x, points[1U].x, points[2U].x});
        const double maximum_x = std::max({points[0U].x, points[1U].x, points[2U].x});
        const double minimum_y = std::min({points[0U].y, points[1U].y, points[2U].y});
        const double maximum_y = std::max({points[0U].y, points[1U].y, points[2U].y});
        const auto minimum_pixel = [](double value, std::uint32_t limit) {
            const auto rounded = static_cast<std::int64_t>(std::floor(value));
            return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                rounded, 0, static_cast<std::int64_t>(limit - 1U)));
        };
        const auto maximum_pixel = [](double value, std::uint32_t limit) {
            const auto rounded = static_cast<std::int64_t>(std::ceil(value));
            return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                rounded, 0, static_cast<std::int64_t>(limit - 1U)));
        };
        const std::uint32_t x_begin = minimum_pixel(minimum_x, width);
        const std::uint32_t x_end = maximum_pixel(maximum_x, width);
        const std::uint32_t y_begin = minimum_pixel(minimum_y, height);
        const std::uint32_t y_end = maximum_pixel(maximum_y, height);
        const std::uint64_t candidate_pixels =
            static_cast<std::uint64_t>(x_end - x_begin + 1U) *
            static_cast<std::uint64_t>(y_end - y_begin + 1U);
        if (candidate_pixels > kMaxThumbnailRasterPixelChecks - pixel_checks) {
            return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
                "Mesh thumbnail raster work exceeds its bounded pixel budget."));
        }
        pixel_checks += candidate_pixels;

        const double shade = std::clamp(triangle.shade, 0.0, 1.0);
        const std::array<std::uint8_t, 3U> color{
            static_cast<std::uint8_t>(std::lround(158.0 * shade)),
            static_cast<std::uint8_t>(std::lround(181.0 * shade)),
            static_cast<std::uint8_t>(std::lround(207.0 * shade))};
        for (std::uint32_t y = y_begin; y <= y_end; ++y) {
            for (std::uint32_t x = x_begin; x <= x_end; ++x) {
                const double sample_x = static_cast<double>(x) + 0.5;
                const double sample_y = static_cast<double>(y) + 0.5;
                const double weight_a =
                    ((points[1U].y - points[2U].y) * (sample_x - points[2U].x) +
                     (points[2U].x - points[1U].x) * (sample_y - points[2U].y)) /
                    denominator;
                const double weight_b =
                    ((points[2U].y - points[0U].y) * (sample_x - points[2U].x) +
                     (points[0U].x - points[2U].x) * (sample_y - points[2U].y)) /
                    denominator;
                const double weight_c = 1.0 - weight_a - weight_b;
                constexpr double edge_epsilon = 1.0e-10;
                if (weight_a < -edge_epsilon || weight_b < -edge_epsilon ||
                    weight_c < -edge_epsilon) {
                    continue;
                }
                const double depth = weight_a * triangle.camera_depth[0U] +
                    weight_b * triangle.camera_depth[1U] +
                    weight_c * triangle.camera_depth[2U];
                const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
                if (!std::isfinite(depth) || depth <= raster.camera_depth[pixel]) continue;
                raster.camera_depth[pixel] = depth;
                const std::size_t offset = pixel * 4U;
                raster.rgba8[offset] = color[0U];
                raster.rgba8[offset + 1U] = color[1U];
                raster.rgba8[offset + 2U] = color[2U];
                raster.rgba8[offset + 3U] = 255U;
            }
        }
    }
    raster.pixel_digest = assets::sha256(
        std::span<const std::uint8_t>{raster.rgba8});
    return Result::success(std::move(raster));
}

class MeshThumbnailRasterCacheEntry {
public:
    const std::optional<MeshThumbnailRaster>& raster_for(
        const MeshThumbnailProjection* projection,
        const MeshThumbnailSharedFrame* frame,
        std::uint32_t width = 144U,
        std::uint32_t height = 144U) {
        if (projection == nullptr || frame == nullptr) {
            key_ = {};
            raster_.reset();
            return raster_;
        }
        std::vector<std::uint8_t> key_bytes;
        key_bytes.reserve(sizeof(width) + sizeof(height) + 5U * sizeof(double) +
            projection->source_digest.hex().size() + 2U * sizeof(std::int32_t));
        const auto append = [&key_bytes](const auto& value) {
            using Value = std::remove_cvref_t<decltype(value)>;
            static_assert(std::is_trivially_copyable_v<Value>);
            const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
            key_bytes.insert(key_bytes.end(), begin, begin + sizeof(Value));
        };
        const std::string source_identity = projection->source_digest.hex();
        key_bytes.insert(key_bytes.end(), source_identity.begin(), source_identity.end());
        append(projection->view.azimuth_steps);
        append(projection->view.elevation_steps);
        append(frame->center.x);
        append(frame->center.y);
        append(frame->extent);
        append(width);
        append(height);
        const auto next_key = assets::sha256(std::span<const std::uint8_t>{key_bytes});
        if (key_ != next_key) {
            key_ = next_key;
            raster_.reset();
            auto built = rasterize_mesh_thumbnail(*projection, *frame, width, height);
            if (built) raster_ = std::move(built.value());
        }
        return raster_;
    }

private:
    assets::Sha256Digest key_;
    std::optional<MeshThumbnailRaster> raster_;
};

class MeshThumbnailCacheEntry {
public:
    const std::optional<MeshThumbnailProjection>& projection_for(
        std::uint64_t project_generation, core::Revision mesh_revision,
        const std::shared_ptr<const geometry::CompiledMesh>& mesh,
        MeshThumbnailView view = {}) {
        if (mesh == nullptr) {
            project_generation_ = project_generation;
            mesh_revision_ = mesh_revision;
            view_ = view;
            source_.reset();
            source_digest_ = {};
            source_valid_ = false;
            projection_.reset();
            static_cast<void>(raster_cache_.raster_for(nullptr, nullptr));
            return projection_;
        }

        const auto source_digest = mesh_thumbnail_source_digest(*mesh);
        const bool within_bounds = mesh->positions.size() <= kMaxThumbnailVertices &&
            mesh->indices.size() / 3U <= kMaxThumbnailTriangles;
        const bool source_valid = view.valid() && within_bounds &&
            mesh->source_revision == mesh_revision && mesh->valid();
        if (project_generation_ != project_generation || mesh_revision_ != mesh_revision ||
            source_.lock() != mesh || source_digest_ != source_digest ||
            source_valid_ != source_valid || view_ != view) {
            project_generation_ = project_generation;
            mesh_revision_ = mesh_revision;
            view_ = view;
            source_ = mesh;
            source_digest_ = source_digest;
            source_valid_ = source_valid;
            projection_.reset();
            static_cast<void>(raster_cache_.raster_for(nullptr, nullptr));
            if (source_valid) {
                auto projected = build_mesh_thumbnail_projection(*mesh, view);
                if (projected) projection_ = std::move(projected.value());
            }
        }
        return projection_;
    }

    const std::optional<MeshThumbnailRaster>& raster_for(
        const MeshThumbnailSharedFrame* frame,
        std::uint32_t width = 64U,
        std::uint32_t height = 64U) {
        if (!projection_.has_value())
            return raster_cache_.raster_for(nullptr, nullptr, width, height);
        return raster_cache_.raster_for(&*projection_, frame, width, height);
    }

private:
    std::uint64_t project_generation_ = 0U;
    core::Revision mesh_revision_;
    MeshThumbnailView view_;
    std::weak_ptr<const geometry::CompiledMesh> source_;
    assets::Sha256Digest source_digest_;
    bool source_valid_ = false;
    std::optional<MeshThumbnailProjection> projection_;
    MeshThumbnailRasterCacheEntry raster_cache_;
};

struct SourceAssetBrowserRow {
    std::size_t source_index = 0U;
};

struct SourceAssetBrowser {
    std::vector<SourceAssetBrowserRow> rows;
    std::size_t total_source_assets = 0U;
};

enum class SourceBlobIntegrityStatus : std::uint8_t {
    unchecked,
    verified,
    missing,
    corrupt,
    catalog_mismatch,
};

[[nodiscard]] inline SourceBlobIntegrityStatus source_blob_integrity_status(
    const assets::BlobOrphanReport* report,
    bool report_current,
    const assets::Sha256Digest& digest) noexcept {
    if (report == nullptr || !report_current) return SourceBlobIntegrityStatus::unchecked;
    const auto contains = [digest](const auto& values) {
        return std::find(values.begin(), values.end(), digest) != values.end();
    };
    if (contains(report->missing_live_references)) return SourceBlobIntegrityStatus::missing;
    if (contains(report->corrupt)) return SourceBlobIntegrityStatus::corrupt;
    if (contains(report->orphan_candidates)) return SourceBlobIntegrityStatus::catalog_mismatch;
    return SourceBlobIntegrityStatus::verified;
}

[[nodiscard]] inline std::string_view source_blob_integrity_label(
    SourceBlobIntegrityStatus status) noexcept {
    switch (status) {
    case SourceBlobIntegrityStatus::unchecked: return "unchecked";
    case SourceBlobIntegrityStatus::verified: return "verified on last inventory";
    case SourceBlobIntegrityStatus::missing: return "missing";
    case SourceBlobIntegrityStatus::corrupt: return "failed digest verification";
    case SourceBlobIntegrityStatus::catalog_mismatch: return "catalog reachability mismatch";
    }
    return "unchecked";
}

namespace browser_detail {

inline core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline std::string folded(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    }
    return result;
}

inline bool contains_folded(std::string_view text, std::string_view query) {
    return query.empty() || std::search(text.begin(), text.end(), query.begin(), query.end(),
        [](char left, char right) {
            if (left >= 'A' && left <= 'Z') left = static_cast<char>(left + ('a' - 'A'));
            return left == right;
        }) != text.end();
}

inline core::Result<std::string> search_key(std::string_view query) {
    if (query.size() > kMaxBrowserSearchBytes || query.find('\0') != std::string_view::npos) {
        return core::Result<std::string>::failure(invalid("Browser search exceeds its text bound."));
    }
    const auto first = query.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return core::Result<std::string>::success({});
    const auto last = query.find_last_not_of(" \t\r\n");
    return core::Result<std::string>::success(folded(query.substr(first, last - first + 1U)));
}

inline bool matches(const scene::SceneObject& object, std::string_view query) {
    return query.empty() || folded(object.name).find(query) != std::string::npos ||
        std::to_string(object.id.value).find(query) != std::string::npos ||
        (object.mesh_asset.has_value() &&
            std::to_string(*object.mesh_asset).find(query) != std::string::npos);
}

inline std::string_view source_kind_label(assets::SourceAssetKind kind) {
    switch (kind) {
        case assets::SourceAssetKind::mesh: return "mesh";
        case assets::SourceAssetKind::material: return "material";
        case assets::SourceAssetKind::texture: return "texture";
        case assets::SourceAssetKind::image: return "image";
        case assets::SourceAssetKind::audio: return "audio";
        case assets::SourceAssetKind::terrain: return "terrain";
        case assets::SourceAssetKind::other: return "other";
    }
    return "unknown";
}

inline std::string_view source_domain_label(assets::SourceAssetDomain domain) {
    switch (domain) {
        case assets::SourceAssetDomain::geometry: return "geometry";
        case assets::SourceAssetDomain::hierarchy: return "hierarchy";
        case assets::SourceAssetDomain::material: return "material";
    }
    return "unknown";
}

inline bool valid_authored_tag(std::string_view tag) {
    if (tag.empty() || tag.size() > assets::SourceImportRecord::kMaxAuthoredTagBytes ||
        tag.front() == '.' || tag.front() == '-' || tag.front() == '_') return false;
    return std::all_of(tag.begin(), tag.end(), [](char character) {
        const bool letter = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        return letter || digit || character == '.' || character == '_' || character == '-';
    });
}

inline bool valid_source_identity(std::string_view identity) {
    if (identity.empty() ||
        identity.size() > assets::SourceImportRecord::kMaxIdentityBytes ||
        identity.front() == '.' || identity.front() == '-' || identity.front() == '_') {
        return false;
    }
    return std::all_of(identity.begin(), identity.end(), [](char character) {
        const bool letter = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        return letter || digit || character == '.' || character == '_' || character == '-';
    });
}

inline bool matches(const application::SourceAssetView& source, std::string_view query) {
    if (query.empty()) return true;
    const std::string generation = std::to_string(source.source_generation.value());
    const std::string importer_version = std::to_string(source.importer_version);
    const std::string bound_mesh_asset = source.bound_mesh_asset.has_value()
        ? std::to_string(*source.bound_mesh_asset) : std::string{};
    const std::string digest = source.source_digest.hex();
    const std::string artifact_digest = source.artifact_digest.hex();
    if (contains_folded(source.identity, query) ||
        contains_folded(source.relative_source_path, query) ||
        contains_folded(source_kind_label(source.kind), query) ||
        contains_folded(source.importer_identity, query) ||
        contains_folded(importer_version, query) ||
        contains_folded(source.artifact_identity, query) ||
        contains_folded(generation, query) || contains_folded(bound_mesh_asset, query) ||
        contains_folded(digest, query) ||
        contains_folded(artifact_digest, query)) {
        return true;
    }
    return std::any_of(source.domain_digests.begin(), source.domain_digests.end(),
        [query](const assets::SourceDomainDigest& domain) {
            const std::string domain_digest = domain.digest.hex();
            return contains_folded(source_domain_label(domain.domain), query) ||
                contains_folded(domain_digest, query);
        }) ||
        std::any_of(source.authored_tags.begin(), source.authored_tags.end(),
            [query](const std::string& tag) { return contains_folded(tag, query); }) ||
        std::any_of(source.dependencies.begin(), source.dependencies.end(),
            [query](const std::string& dependency) {
                return contains_folded(dependency, query);
            });
}

inline core::Result<void> validate_source_asset_views(
    std::span<const application::SourceAssetView> sources) {
    using Result = core::Result<void>;
    if (sources.size() > kMaxBrowserObjects) {
        return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "Browser source count exceeds its inspection bound."));
    }
    std::map<std::string_view, std::size_t> identities;
    std::map<std::uint64_t, std::size_t> bound_meshes;
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (!valid_source_identity(source.identity) ||
            !identities.emplace(source.identity, index).second) {
            return Result::failure(invalid(
                "Browser source records require distinct, nonempty identities."));
        }
        if (auto valid_options = source.options.validate(); !valid_options) {
            return Result::failure(valid_options.error().with_context(
                "Browser source importer options"));
        }
        if (source.bound_mesh_asset.has_value() &&
            (*source.bound_mesh_asset == 0U || source.kind != assets::SourceAssetKind::mesh ||
             !bound_meshes.emplace(*source.bound_mesh_asset, index).second)) {
            return Result::failure(invalid(
                "Browser mesh bindings require a unique nonzero mesh source."));
        }
        if (source.bound_mesh_asset.has_value() !=
                source.bound_mesh_vertex_count.has_value() ||
            source.bound_mesh_asset.has_value() != source.bound_mesh_face_count.has_value()) {
            return Result::failure(invalid(
                "Browser mesh binding counts must accompany a live bound mesh asset."));
        }
        std::array<bool, 3U> seen_domains{};
        for (const auto& domain : source.domain_digests) {
            const auto domain_index = static_cast<std::size_t>(domain.domain);
            if (domain_index >= seen_domains.size() || domain.digest.is_zero() ||
                seen_domains[domain_index]) {
                return Result::failure(invalid(
                    "Browser source domains require distinct recognized types and nonzero digests."));
            }
            seen_domains[domain_index] = true;
        }
        if (source.authored_tags.size() > assets::SourceImportRecord::kMaxAuthoredTags ||
            source.dependencies.size() > assets::SourceImportRecord::kMaxDependencies) {
            return Result::failure(invalid(
                "Browser authored source metadata exceeds its inspection bound."));
        }
        std::string previous_tag;
        for (const auto& tag : source.authored_tags) {
            if (!valid_authored_tag(tag) || (!previous_tag.empty() && tag <= previous_tag)) {
                return Result::failure(invalid(
                    "Browser source tags require canonical sorted unique tokens."));
            }
            previous_tag = tag;
        }
        std::string previous_dependency;
        for (const auto& dependency : source.dependencies) {
            if (!valid_source_identity(dependency) ||
                dependency == source.identity ||
                (!previous_dependency.empty() && dependency <= previous_dependency)) {
                return Result::failure(invalid(
                    "Browser dependencies require distinct sorted source identities."));
            }
            previous_dependency = dependency;
        }
    }
    for (const auto& source : sources) {
        for (const auto& dependency : source.dependencies) {
            if (identities.find(std::string_view{dependency}) == identities.end()) {
                return Result::failure(invalid(
                    "Browser dependency points outside the active source registry."));
            }
        }
    }
    return Result::success();
}

inline core::Result<std::map<scene::ObjectId, std::size_t>> object_index(
    std::span<const application::ObjectView> objects) {
    using Index = std::map<scene::ObjectId, std::size_t>;
    if (objects.size() > kMaxBrowserObjects) {
        return core::Result<Index>::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "Browser object count exceeds its inspection bound."));
    }
    Index index;
    for (std::size_t position = 0U; position < objects.size(); ++position) {
        const auto& object = objects[position].object;
        if (!object.id || object.name.empty() ||
            (object.mesh_asset.has_value() && *object.mesh_asset == 0U) ||
            !index.emplace(object.id, position).second) {
            return core::Result<Index>::failure(invalid(
                "Browser requires distinct nonzero object IDs, names, and valid mesh IDs."));
        }
    }
    return core::Result<Index>::success(std::move(index));
}

} // namespace browser_detail

// Pure projection: no scene mutation, ID reassignment, or inferred parents.
// Validate the complete hierarchy before filtering so a hidden error cannot
// disappear merely because its name did not match the search.
inline core::Result<SceneBrowser> build_scene_browser(
    std::span<const application::ObjectView> objects,
    std::string_view query = {}, SceneBrowserFilter filter = SceneBrowserFilter::all) {
    using Result = core::Result<SceneBrowser>;
    auto indexed = browser_detail::object_index(objects);
    if (!indexed) return Result::failure(indexed.error());
    auto key = browser_detail::search_key(query);
    if (!key) return Result::failure(key.error());
    if (filter != SceneBrowserFilter::all && filter != SceneBrowserFilter::visible &&
        filter != SceneBrowserFilter::hidden && filter != SceneBrowserFilter::locked &&
        filter != SceneBrowserFilter::meshes) {
        return Result::failure(browser_detail::invalid("Scene filter is unknown."));
    }
    const auto& index = indexed.value();
    std::vector<std::vector<std::size_t>> children(objects.size());
    std::vector<std::size_t> roots;
    for (const auto& [id, position] : index) {
        const auto parent = objects[position].object.parent;
        if (!parent.has_value()) {
            roots.push_back(position);
        } else {
            const auto found = index.find(*parent);
            if (found == index.end()) {
                return Result::failure(browser_detail::invalid(
                    "Scene object " + std::to_string(id.value) + " has a missing parent."));
            }
            children[found->second].push_back(position);
        }
    }
    std::vector<std::uint8_t> visited(objects.size(), 0U);
    for (const auto& [id, position] : index) {
        static_cast<void>(id);
        if (visited[position] != 0U) continue;
        std::vector<std::size_t> chain;
        std::size_t current = position;
        while (visited[current] == 0U) {
            visited[current] = 1U;
            chain.push_back(current);
            const auto parent = objects[current].object.parent;
            if (!parent.has_value()) break;
            current = index.at(*parent);
            if (visited[current] == 1U) {
                return Result::failure(core::Diagnostic(core::ErrorCode::cycle_detected,
                    "Scene hierarchy contains a parent cycle."));
            }
        }
        for (const auto member : chain) visited[member] = 2U;
    }
    std::vector<bool> matched(objects.size(), false);
    std::vector<bool> included(objects.size(), false);
    SceneBrowser result;
    for (const auto& [id, position] : index) {
        static_cast<void>(id);
        const auto& object = objects[position].object;
        const bool state_matches = filter == SceneBrowserFilter::all ||
            (filter == SceneBrowserFilter::visible && object.visible) ||
            (filter == SceneBrowserFilter::hidden && !object.visible) ||
            (filter == SceneBrowserFilter::locked && object.locked) ||
            (filter == SceneBrowserFilter::meshes && object.mesh_asset.has_value());
        matched[position] = state_matches && browser_detail::matches(object, key.value());
        if (!matched[position]) continue;
        ++result.matching_objects;
        std::size_t current = position;
        while (!included[current]) {
            included[current] = true;
            const auto parent = objects[current].object.parent;
            if (!parent.has_value()) break;
            current = index.at(*parent);
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> pending;
    for (auto root = roots.rbegin(); root != roots.rend(); ++root) pending.emplace_back(*root, 0U);
    while (!pending.empty()) {
        const auto [position, depth] = pending.back();
        pending.pop_back();
        if (!included[position]) continue;
        const bool shown_children = std::any_of(children[position].begin(), children[position].end(),
            [&](std::size_t child) { return included[child]; });
        result.rows.push_back({position, depth, !matched[position], shown_children});
        for (auto child = children[position].rbegin(); child != children[position].rend(); ++child) {
            pending.emplace_back(*child, depth + 1U);
        }
    }
    return Result::success(std::move(result));
}

// Group actual object references by mesh asset; an asset match keeps the full
// instance count while choosing a matching object as the selection target.
inline core::Result<MeshAssetBrowser> build_mesh_asset_browser(
    std::span<const application::ObjectView> objects, std::string_view query = {},
    std::span<const application::SourceAssetView> sources = {}) {
    using Result = core::Result<MeshAssetBrowser>;
    auto indexed = browser_detail::object_index(objects);
    if (!indexed) return Result::failure(indexed.error());
    auto key = browser_detail::search_key(query);
    if (!key) return Result::failure(key.error());
    if (auto valid_sources = browser_detail::validate_source_asset_views(sources);
        !valid_sources) {
        return Result::failure(valid_sources.error());
    }
    std::map<std::uint64_t, std::size_t> source_by_mesh;
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        if (sources[index].bound_mesh_asset.has_value()) {
            source_by_mesh.emplace(*sources[index].bound_mesh_asset, index);
        }
    }
    std::map<std::uint64_t, MeshAssetBrowserRow> assets;
    MeshAssetBrowser result;
    for (const auto& [id, position] : indexed.value()) {
        static_cast<void>(id);
        const auto& object = objects[position].object;
        if (!object.mesh_asset.has_value()) continue;
        auto [asset, inserted] = assets.try_emplace(*object.mesh_asset);
        if (inserted) {
            asset->second.mesh_asset = *object.mesh_asset;
            asset->second.representative_index = position;
        }
        asset->second.object_indices.push_back(position);
        const auto& view = objects[position];
        if (!object.parent.has_value() && object.visible && !object.locked &&
            has_mesh_revision_binding(view)) {
            asset->second.placeable_object_indices.push_back(position);
        }
        ++result.total_mesh_instances;
    }
    result.total_mesh_assets = assets.size();
    for (auto& [id, asset] : assets) {
        const auto source = source_by_mesh.find(id);
        if (source != source_by_mesh.end()) asset.source_index = source->second;
        const auto matching = std::find_if(asset.object_indices.begin(), asset.object_indices.end(),
            [&](std::size_t position) {
                return browser_detail::matches(objects[position].object, key.value());
            });
        const bool source_matches = asset.source_index.has_value() &&
            browser_detail::matches(sources[*asset.source_index], key.value());
        if (matching == asset.object_indices.end() && !source_matches) continue;
        if (matching != asset.object_indices.end()) asset.representative_index = *matching;
        result.rows.push_back(std::move(asset));
    }
    return Result::success(std::move(result));
}

// Read-only projection of the project source registry. Search reflects only
// recorded identity, path, kind, importer/version, artifact, generation,
// digests and typed source-domain metadata; it does not imply that a source
// was reviewed or is current on disk.
inline core::Result<SourceAssetBrowser> build_source_asset_browser(
    std::span<const application::SourceAssetView> sources, std::string_view query = {}) {
    using Result = core::Result<SourceAssetBrowser>;
    auto key = browser_detail::search_key(query);
    if (!key) return Result::failure(key.error());
    if (auto valid_sources = browser_detail::validate_source_asset_views(sources);
        !valid_sources) return Result::failure(valid_sources.error());
    SourceAssetBrowser result;
    result.total_source_assets = sources.size();
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        if (browser_detail::matches(sources[index], key.value())) {
            result.rows.push_back({index});
        }
    }
    return Result::success(std::move(result));
}

} // namespace carto::studio
