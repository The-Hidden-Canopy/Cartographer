#pragma once

#include <carto/attributes/uv.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/production/bake_graph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace carto::desktop {

inline constexpr std::size_t kMaxUvInspectionTriangles = 65'536U;
inline constexpr std::size_t kMaxUvInspectionOverlapPairs = 1'000'000U;
inline constexpr std::size_t kMaxUvInspectionSegments = 262'144U;
inline constexpr std::size_t kMaxUvInspectionCorners = 262'144U;
inline constexpr double kMaxUvInspectionCoordinateMagnitude = 1.0e6;
inline constexpr std::uint32_t kMaxUvTexelDensityTileResolution = 16'384U;

struct UvInspectionSegment {
    geometry::FaceId face;
    core::Vec2d first;
    core::Vec2d second;
    bool seam = false;
};

struct UvInspectionCorner {
    geometry::FaceId face;
    geometry::VertexId vertex;
    core::Vec2d coordinate;
};

struct UvInspectionSeam {
    geometry::EdgeId edge;
    geometry::FaceId first_face;
    geometry::FaceId second_face;
    geometry::VertexId first_vertex;
    geometry::VertexId second_vertex;
    bool explicitly_marked = false;
    bool coordinate_discontinuity = false;
};

struct UvInspectionInternalEdge {
    geometry::EdgeId edge;
    geometry::FaceId first_face;
    geometry::FaceId second_face;
    geometry::VertexId first_vertex;
    geometry::VertexId second_vertex;
    bool explicitly_marked = false;
    bool coordinate_discontinuity = false;
};

struct UvInspectionTriangle {
    geometry::FaceId face;
    std::array<core::Vec2d, 3U> coordinates{};
    // UV-tile units squared and model-space units squared; density analysis
    // supplies explicit pixel dimensions instead of inferring texture assets.
    double uv_area = 0.0;
    double surface_area = 0.0;
    bool surface_area_finite = false;
    double minimum_u = 0.0;
    double maximum_u = 0.0;
    double minimum_v = 0.0;
    double maximum_v = 0.0;
};

struct UvInspectionIsland {
    // The lowest face ID is a stable display identity for this current report.
    geometry::FaceId identity;
    std::vector<geometry::FaceId> faces;
};

struct UvInspectionBakeTile {
    std::uint32_t tile_u = 0U;
    std::uint32_t tile_v = 0U;

    [[nodiscard]] bool operator==(const UvInspectionBakeTile&) const noexcept = default;
};

struct UvSetInspectionReport {
    attributes::UvSetDescriptor descriptor;
    std::size_t corner_count = 0U;
    std::size_t seam_edge_count = 0U;
    std::size_t uv_island_count = 0U;
    std::vector<UvInspectionIsland> islands;
    std::size_t cross_tile_face_count = 0U;
    std::size_t degenerate_uv_triangle_count = 0U;
    std::size_t overlap_face_pair_count = 0U;
    std::size_t overlap_pair_checks = 0U;
    bool overlap_scan_capped = false;
    double minimum_u = 0.0;
    double maximum_u = 1.0;
    double minimum_v = 0.0;
    double maximum_v = 1.0;
    std::vector<geometry::FaceId> cross_tile_faces;
    std::vector<geometry::FaceId> overlapping_faces;
    std::vector<UvInspectionCorner> corners;
    std::vector<UvInspectionInternalEdge> internal_edges;
    std::vector<UvInspectionSeam> seams;
    std::vector<UvInspectionSegment> segments;
    std::vector<UvInspectionTriangle> triangles;
    // This is the production scheduler's read-only tile preflight, not a bake
    // result. Unsupported, empty, or out-of-range UV coverage retains its
    // diagnostic instead of exposing a partial tile list.
    std::vector<UvInspectionBakeTile> bake_tiles;
    std::optional<std::string> bake_tile_schedule_error;
};

struct UvTexelDensityReport {
    std::uint32_t tile_width_pixels = 0U;
    std::uint32_t tile_height_pixels = 0U;
    std::size_t measured_triangle_count = 0U;
    std::size_t degenerate_uv_triangle_count = 0U;
    std::size_t degenerate_surface_triangle_count = 0U;
    std::size_t unavailable_surface_area_triangle_count = 0U;
    double minimum_area_equivalent_pixels_per_unit = 0.0;
    double maximum_area_equivalent_pixels_per_unit = 0.0;
};

namespace uv_inspector_detail {

using FaceVertex = std::pair<geometry::FaceId, geometry::VertexId>;

inline std::size_t find_face_root(
    std::vector<std::size_t>& parents,
    std::size_t face_index) noexcept {
    while (parents[face_index] != face_index) {
        parents[face_index] = parents[parents[face_index]];
        face_index = parents[face_index];
    }
    return face_index;
}

inline void join_uv_islands(
    std::vector<std::size_t>& parents,
    std::vector<std::uint8_t>& ranks,
    std::size_t first,
    std::size_t second) noexcept {
    first = find_face_root(parents, first);
    second = find_face_root(parents, second);
    if (first == second) return;
    if (ranks[first] < ranks[second]) std::swap(first, second);
    parents[second] = first;
    if (ranks[first] == ranks[second]) ++ranks[first];
}

inline bool triangles_overlap_with_area(
    const std::array<core::Vec2d, 3U>& first,
    const std::array<core::Vec2d, 3U>& second) noexcept {
    constexpr double kTolerance = 1.0e-12;
    const auto separated = [=](const auto& axis_triangle,
                                const auto& first_triangle,
                                const auto& second_triangle) {
        for (std::size_t edge = 0U; edge < 3U; ++edge) {
            const core::Vec2d delta = axis_triangle[(edge + 1U) % 3U] -
                axis_triangle[edge];
            const core::Vec2d axis{-delta.y, delta.x};
            if (!axis.finite() || axis.length_squared() <= 1.0e-24) continue;
            double first_minimum = std::numeric_limits<double>::infinity();
            double first_maximum = -std::numeric_limits<double>::infinity();
            double second_minimum = std::numeric_limits<double>::infinity();
            double second_maximum = -std::numeric_limits<double>::infinity();
            for (const core::Vec2d point : first_triangle) {
                const double projection = core::dot(point, axis);
                first_minimum = std::min(first_minimum, projection);
                first_maximum = std::max(first_maximum, projection);
            }
            for (const core::Vec2d point : second_triangle) {
                const double projection = core::dot(point, axis);
                second_minimum = std::min(second_minimum, projection);
                second_maximum = std::max(second_maximum, projection);
            }
            if (!std::isfinite(first_minimum) || !std::isfinite(first_maximum) ||
                !std::isfinite(second_minimum) || !std::isfinite(second_maximum)) {
                return true;
            }
            const double tolerance = kTolerance * std::max({
                1.0, std::abs(first_minimum), std::abs(first_maximum),
                std::abs(second_minimum), std::abs(second_maximum)});
            if (first_maximum <= second_minimum + tolerance ||
                second_maximum <= first_minimum + tolerance) {
                return true;
            }
        }
        return false;
    };
    return !separated(first, first, second) && !separated(second, first, second);
}

} // namespace uv_inspector_detail

[[nodiscard]] inline core::Result<UvSetInspectionReport> inspect_uv_set(
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set,
    std::size_t maximum_triangles = kMaxUvInspectionTriangles,
    std::size_t maximum_overlap_pair_checks = kMaxUvInspectionOverlapPairs) {
    using namespace uv_inspector_detail;
    const auto fail = [](core::ErrorCode code, std::string message) {
        return core::Result<UvSetInspectionReport>::failure(
            core::Diagnostic(code, std::move(message)));
    };
    if (uv_set == 0U || maximum_triangles == 0U ||
        maximum_overlap_pair_checks == 0U) {
        return fail(core::ErrorCode::invalid_argument,
            "UV inspection requires a non-zero set and positive work bounds");
    }
    maximum_triangles = std::min(maximum_triangles, kMaxUvInspectionTriangles);
    maximum_overlap_pair_checks = std::min(
        maximum_overlap_pair_checks, kMaxUvInspectionOverlapPairs);
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<UvSetInspectionReport>::failure(topology.error());
    }
    if (topology.value().corners.size() > kMaxUvInspectionCorners ||
        topology.value().edges.size() > kMaxUvInspectionSegments / 2U) {
        return fail(core::ErrorCode::unsupported,
            "UV topology exceeds the inspector's corner or edge bound");
    }
    const auto* descriptor = mesh.uv_sets().find(uv_set);
    if (descriptor == nullptr) {
        return fail(core::ErrorCode::not_found, "UV set does not exist on this mesh");
    }
    const auto* layer = mesh.attributes().find(descriptor->layer_id);
    if (layer == nullptr ||
        layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2 ||
        layer->values.size() != topology.value().corners.size()) {
        return fail(core::ErrorCode::validation_failed,
            "UV set corner layer does not match the validated topology snapshot");
    }

    UvSetInspectionReport report;
    report.descriptor = *descriptor;
    report.corner_count = topology.value().corners.size();
    report.corners.reserve(topology.value().corners.size());
    report.segments.reserve(std::min(
        kMaxUvInspectionSegments, topology.value().edges.size() * 2U));
    report.seams.reserve(std::min(
        topology.value().edges.size(), kMaxUvInspectionSegments / 2U));
    report.internal_edges.reserve(std::min(
        topology.value().edges.size(), kMaxUvInspectionSegments / 2U));
    const std::string seam_layer_id = attributes::uv_seam_attribute_id(uv_set);
    const auto* seam_layer = mesh.attributes().find(seam_layer_id);
    if (seam_layer != nullptr &&
        (seam_layer->descriptor.domain != attributes::AttributeDomain::edge ||
         seam_layer->descriptor.type != attributes::AttributeType::boolean ||
         seam_layer->values.size() != topology.value().edges.size())) {
        return fail(core::ErrorCode::validation_failed,
            "UV seam marker layer does not match the validated edge topology");
    }
    if (seam_layer != nullptr) {
        if (auto valid = seam_layer->validate(topology.value().edges.size()); !valid) {
            return core::Result<UvSetInspectionReport>::failure(
                valid.error().with_context("UV seam marker layer"));
        }
    }
    std::map<FaceVertex, core::Vec2d> uv_by_face_vertex;
    report.minimum_u = std::numeric_limits<double>::infinity();
    report.maximum_u = -std::numeric_limits<double>::infinity();
    report.minimum_v = std::numeric_limits<double>::infinity();
    report.maximum_v = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto* coordinate = std::get_if<core::Vec2d>(&layer->values[index]);
        if (coordinate == nullptr || !coordinate->finite() ||
            std::abs(coordinate->x) > kMaxUvInspectionCoordinateMagnitude ||
            std::abs(coordinate->y) > kMaxUvInspectionCoordinateMagnitude) {
            return fail(core::ErrorCode::unsupported,
                "UV coordinate is invalid or exceeds the inspector's numeric bound");
        }
        const auto& corner = topology.value().corners[index];
        report.corners.push_back({corner.face, corner.vertex, *coordinate});
        if (!uv_by_face_vertex.emplace(
                FaceVertex{corner.face, corner.vertex}, *coordinate).second) {
            return fail(core::ErrorCode::validation_failed,
                "UV topology contains a duplicate face corner identity");
        }
        report.minimum_u = std::min(report.minimum_u, coordinate->x);
        report.maximum_u = std::max(report.maximum_u, coordinate->x);
        report.minimum_v = std::min(report.minimum_v, coordinate->y);
        report.maximum_v = std::max(report.maximum_v, coordinate->y);
    }
    if (uv_by_face_vertex.empty()) {
        report.minimum_u = 0.0;
        report.maximum_u = 1.0;
        report.minimum_v = 0.0;
        report.maximum_v = 1.0;
    }

    const auto faces = mesh.faces_sorted();
    if (faces.size() > kMaxUvInspectionCorners) {
        return fail(core::ErrorCode::unsupported,
            "UV face diagnostics exceed the inspector's face bound");
    }
    std::map<geometry::FaceId, std::size_t> face_island_index;
    std::vector<std::size_t> island_parents;
    std::vector<std::uint8_t> island_ranks;
    std::vector<geometry::FaceId> island_face_ids;
    for (const geometry::Face& face : faces) {
        if (face.vertices.empty()) continue;
        const std::size_t index = island_parents.size();
        if (!face_island_index.emplace(face.id, index).second) {
            return fail(core::ErrorCode::validation_failed,
                "UV topology contains a duplicate face identity");
        }
        island_parents.push_back(index);
        island_ranks.push_back(0U);
        island_face_ids.push_back(face.id);
    }

    std::set<geometry::FaceId> cross_tile_faces;
    constexpr double kTileBoundaryTolerance = 1.0e-10;
    for (const geometry::Face& face : faces) {
        if (face.vertices.empty()) continue;
        double minimum_u = std::numeric_limits<double>::infinity();
        double maximum_u = -std::numeric_limits<double>::infinity();
        double minimum_v = std::numeric_limits<double>::infinity();
        double maximum_v = -std::numeric_limits<double>::infinity();
        for (const geometry::VertexId vertex : face.vertices) {
            const auto coordinate = uv_by_face_vertex.find({face.id, vertex});
            if (coordinate == uv_by_face_vertex.end()) {
                return fail(core::ErrorCode::validation_failed,
                    "UV set is missing a source face corner");
            }
            minimum_u = std::min(minimum_u, coordinate->second.x);
            maximum_u = std::max(maximum_u, coordinate->second.x);
            minimum_v = std::min(minimum_v, coordinate->second.y);
            maximum_v = std::max(maximum_v, coordinate->second.y);
        }
        const bool spans_u = maximum_u - minimum_u > kTileBoundaryTolerance &&
            std::floor(minimum_u + kTileBoundaryTolerance) !=
                std::floor(maximum_u - kTileBoundaryTolerance);
        const bool spans_v = maximum_v - minimum_v > kTileBoundaryTolerance &&
            std::floor(minimum_v + kTileBoundaryTolerance) !=
                std::floor(maximum_v - kTileBoundaryTolerance);
        if (spans_u || spans_v) cross_tile_faces.insert(face.id);
    }
    report.cross_tile_faces.assign(cross_tile_faces.begin(), cross_tile_faces.end());
    report.cross_tile_face_count = report.cross_tile_faces.size();

    std::map<geometry::HalfEdgeId, geometry::HalfEdgeRecord> half_edges;
    for (const auto& half_edge : topology.value().half_edges) {
        half_edges.emplace(half_edge.id, half_edge);
    }
    const auto append_side = [&](geometry::FaceId face,
                                 geometry::VertexId first,
                                 geometry::VertexId second,
                                 bool seam) -> bool {
        const auto first_uv = uv_by_face_vertex.find({face, first});
        const auto second_uv = uv_by_face_vertex.find({face, second});
        if (first_uv == uv_by_face_vertex.end() || second_uv == uv_by_face_vertex.end() ||
            report.segments.size() >= kMaxUvInspectionSegments) {
            return false;
        }
        report.segments.push_back({face, first_uv->second, second_uv->second, seam});
        return true;
    };
    for (std::size_t edge_index = 0U;
         edge_index < topology.value().edges.size(); ++edge_index) {
        const geometry::EdgeRecord& edge = topology.value().edges[edge_index];
        const auto first_half_edge = half_edges.find(edge.first_half_edge);
        if (first_half_edge == half_edges.end()) {
            return fail(core::ErrorCode::validation_failed,
                "UV inspection topology is missing an edge half-edge");
        }
        const auto second_half_edge = edge.second_half_edge.has_value()
            ? half_edges.find(*edge.second_half_edge) : half_edges.end();
        if (edge.second_half_edge.has_value() && second_half_edge == half_edges.end()) {
            return fail(core::ErrorCode::validation_failed,
                "UV inspection topology is missing a paired edge half-edge");
        }
        bool coordinate_discontinuity = false;
        bool explicitly_marked = false;
        if (seam_layer != nullptr) {
            const auto* marked = std::get_if<bool>(&seam_layer->values[edge_index]);
            if (marked == nullptr) {
                return fail(core::ErrorCode::validation_failed,
                    "UV seam marker layer contains a non-boolean value");
            }
            explicitly_marked = *marked;
        }
        bool seam = false;
        if (second_half_edge != half_edges.end()) {
            const auto first_a = uv_by_face_vertex.find(
                {first_half_edge->second.face, edge.first});
            const auto first_b = uv_by_face_vertex.find(
                {first_half_edge->second.face, edge.second});
            const auto second_a = uv_by_face_vertex.find(
                {second_half_edge->second.face, edge.first});
            const auto second_b = uv_by_face_vertex.find(
                {second_half_edge->second.face, edge.second});
            if (first_a == uv_by_face_vertex.end() || first_b == uv_by_face_vertex.end() ||
                second_a == uv_by_face_vertex.end() || second_b == uv_by_face_vertex.end()) {
                return fail(core::ErrorCode::validation_failed,
                    "UV inspection edge is missing an endpoint corner");
            }
            coordinate_discontinuity =
                (first_a->second - second_a->second).length_squared() > 1.0e-20 ||
                (first_b->second - second_b->second).length_squared() > 1.0e-20;
            seam = explicitly_marked || coordinate_discontinuity;
            report.internal_edges.push_back({
                edge.id,
                first_half_edge->second.face,
                second_half_edge->second.face,
                edge.first,
                edge.second,
                explicitly_marked,
                coordinate_discontinuity});
            if (seam) {
                ++report.seam_edge_count;
                report.seams.push_back({
                    edge.id,
                    first_half_edge->second.face,
                    second_half_edge->second.face,
                    edge.first,
                    edge.second,
                    explicitly_marked,
                    coordinate_discontinuity});
            } else {
                const auto first_face = face_island_index.find(first_half_edge->second.face);
                const auto second_face = face_island_index.find(second_half_edge->second.face);
                if (first_face == face_island_index.end() ||
                    second_face == face_island_index.end()) {
                    return fail(core::ErrorCode::validation_failed,
                        "UV adjacency references a face without mapped corners");
                }
                join_uv_islands(island_parents, island_ranks,
                    first_face->second, second_face->second);
            }
        }
        if (!append_side(first_half_edge->second.face, edge.first, edge.second, seam)) {
            return fail(core::ErrorCode::unsupported,
                "UV edge preview exceeds its bounded segment count");
        }
        if (seam && !append_side(second_half_edge->second.face,
                edge.first, edge.second, true)) {
            return fail(core::ErrorCode::unsupported,
                "UV seam preview exceeds its bounded segment count");
        }
    }
    std::map<std::size_t, UvInspectionIsland> islands_by_root;
    for (std::size_t face_index = 0U; face_index < island_parents.size(); ++face_index) {
        const std::size_t root = find_face_root(island_parents, face_index);
        const geometry::FaceId face = island_face_ids[face_index];
        auto& island = islands_by_root[root];
        if (island.faces.empty()) island.identity = face;
        island.faces.push_back(face);
    }
    report.islands.reserve(islands_by_root.size());
    for (auto& [root, island] : islands_by_root) {
        static_cast<void>(root);
        report.islands.push_back(std::move(island));
    }
    std::sort(report.islands.begin(), report.islands.end(),
        [](const UvInspectionIsland& first, const UvInspectionIsland& second) {
            return first.identity < second.identity;
        });
    report.uv_island_count = report.islands.size();

    const auto compiled = mesh.compile();
    if (!compiled) return core::Result<UvSetInspectionReport>::failure(compiled.error());
    const std::size_t triangle_count = compiled.value().triangle_faces.size();
    if (triangle_count > maximum_triangles) {
        return fail(core::ErrorCode::unsupported,
            "UV triangle diagnostics exceed the configured inspection bound");
    }
    report.triangles.reserve(triangle_count);
    std::set<geometry::FaceId> overlapping_faces;
    for (std::size_t triangle_index = 0U;
         triangle_index < triangle_count; ++triangle_index) {
        UvInspectionTriangle triangle;
        triangle.face = compiled.value().triangle_faces[triangle_index];
        triangle.minimum_u = triangle.minimum_v = std::numeric_limits<double>::infinity();
        triangle.maximum_u = triangle.maximum_v = -std::numeric_limits<double>::infinity();
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const std::uint32_t mesh_index = compiled.value().indices[
                triangle_index * 3U + corner];
            const geometry::VertexId vertex = compiled.value().vertex_ids[mesh_index];
            const auto coordinate = uv_by_face_vertex.find({triangle.face, vertex});
            if (coordinate == uv_by_face_vertex.end()) {
                return fail(core::ErrorCode::validation_failed,
                    "UV triangle cannot resolve a source face corner");
            }
            triangle.coordinates[corner] = coordinate->second;
            triangle.minimum_u = std::min(triangle.minimum_u, coordinate->second.x);
            triangle.maximum_u = std::max(triangle.maximum_u, coordinate->second.x);
            triangle.minimum_v = std::min(triangle.minimum_v, coordinate->second.y);
            triangle.maximum_v = std::max(triangle.maximum_v, coordinate->second.y);
        }
        const core::Vec2d first_edge = triangle.coordinates[1U] - triangle.coordinates[0U];
        const core::Vec2d second_edge = triangle.coordinates[2U] - triangle.coordinates[0U];
        const double twice_area = first_edge.x * second_edge.y -
            first_edge.y * second_edge.x;
        if (!std::isfinite(twice_area)) {
            return fail(core::ErrorCode::unsupported,
                "UV triangle area exceeds the inspector's numeric bound");
        }
        if (std::abs(twice_area) <= 1.0e-12) {
            ++report.degenerate_uv_triangle_count;
        } else {
            triangle.uv_area = std::abs(twice_area) * 0.5;
        }
        const std::uint32_t first_index = compiled.value().indices[triangle_index * 3U];
        const std::uint32_t second_index = compiled.value().indices[triangle_index * 3U + 1U];
        const std::uint32_t third_index = compiled.value().indices[triangle_index * 3U + 2U];
        const core::Vec3d world_normal = core::cross(
            compiled.value().positions[second_index] - compiled.value().positions[first_index],
            compiled.value().positions[third_index] - compiled.value().positions[first_index]);
        const double twice_surface_area = std::hypot(
            std::hypot(world_normal.x, world_normal.y), world_normal.z);
        if (std::isfinite(twice_surface_area)) {
            triangle.surface_area = twice_surface_area * 0.5;
            triangle.surface_area_finite = std::isfinite(triangle.surface_area);
        }
        report.triangles.push_back(std::move(triangle));
    }

    std::vector<std::size_t> order(report.triangles.size());
    for (std::size_t index = 0U; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&report](std::size_t first, std::size_t second) {
        const auto& a = report.triangles[first];
        const auto& b = report.triangles[second];
        if (a.minimum_u != b.minimum_u) return a.minimum_u < b.minimum_u;
        if (a.face != b.face) return a.face < b.face;
        return first < second;
    });
    std::set<std::pair<geometry::FaceId, geometry::FaceId>> overlap_pairs;
    constexpr double kBoundsTolerance = 1.0e-12;
    for (std::size_t first_order = 0U; first_order < order.size(); ++first_order) {
        const auto& first = report.triangles[order[first_order]];
        const core::Vec2d first_edge_a = first.coordinates[1U] - first.coordinates[0U];
        const core::Vec2d first_edge_b = first.coordinates[2U] - first.coordinates[0U];
        if (std::abs(first_edge_a.x * first_edge_b.y -
                     first_edge_a.y * first_edge_b.x) <= 1.0e-12) {
            continue;
        }
        for (std::size_t second_order = first_order + 1U;
             second_order < order.size(); ++second_order) {
            const auto& second = report.triangles[order[second_order]];
            if (second.minimum_u >= first.maximum_u - kBoundsTolerance) break;
            if (report.overlap_pair_checks == maximum_overlap_pair_checks) {
                report.overlap_scan_capped = true;
                break;
            }
            ++report.overlap_pair_checks;
            if (first.face == second.face ||
                second.minimum_v >= first.maximum_v - kBoundsTolerance ||
                first.minimum_v >= second.maximum_v - kBoundsTolerance) {
                continue;
            }
            const core::Vec2d second_edge_a = second.coordinates[1U] - second.coordinates[0U];
            const core::Vec2d second_edge_b = second.coordinates[2U] - second.coordinates[0U];
            if (std::abs(second_edge_a.x * second_edge_b.y -
                         second_edge_a.y * second_edge_b.x) <= 1.0e-12) {
                continue;
            }
            if (triangles_overlap_with_area(first.coordinates, second.coordinates)) {
                overlap_pairs.emplace(std::min(first.face, second.face),
                    std::max(first.face, second.face));
            }
        }
        if (report.overlap_scan_capped) break;
    }
    for (const auto& pair : overlap_pairs) {
        overlapping_faces.insert(pair.first);
        overlapping_faces.insert(pair.second);
    }
    report.overlap_face_pair_count = overlap_pairs.size();
    report.overlapping_faces.assign(overlapping_faces.begin(), overlapping_faces.end());

    const auto bake_tiles = production::enumerate_bake_tiles(mesh, uv_set);
    if (!bake_tiles) {
        report.bake_tile_schedule_error = bake_tiles.error().message;
    } else {
        report.bake_tiles.reserve(bake_tiles.value().size());
        for (const production::BakeTileCoordinate tile : bake_tiles.value()) {
            report.bake_tiles.push_back({tile.tile_u, tile.tile_v});
        }
    }
    return core::Result<UvSetInspectionReport>::success(std::move(report));
}

[[nodiscard]] inline core::Result<UvTexelDensityReport> analyze_uv_texel_density(
    const UvSetInspectionReport& inspection,
    std::uint32_t tile_width_pixels,
    std::uint32_t tile_height_pixels) {
    using Result = core::Result<UvTexelDensityReport>;
    const auto fail = [](core::ErrorCode code, std::string message) {
        return Result::failure(core::Diagnostic(code, std::move(message)));
    };
    if (tile_width_pixels == 0U || tile_height_pixels == 0U ||
        tile_width_pixels > kMaxUvTexelDensityTileResolution ||
        tile_height_pixels > kMaxUvTexelDensityTileResolution) {
        return fail(core::ErrorCode::invalid_argument,
            "UV texel density requires explicit tile dimensions from 1 to 16,384 pixels");
    }
    if (inspection.triangles.size() > kMaxUvInspectionTriangles) {
        return fail(core::ErrorCode::unsupported,
            "UV texel density input exceeds its triangle work bound");
    }

    UvTexelDensityReport report;
    report.tile_width_pixels = tile_width_pixels;
    report.tile_height_pixels = tile_height_pixels;
    report.minimum_area_equivalent_pixels_per_unit =
        std::numeric_limits<double>::infinity();
    report.maximum_area_equivalent_pixels_per_unit = 0.0;
    const double log_width = std::log(static_cast<double>(tile_width_pixels));
    const double log_height = std::log(static_cast<double>(tile_height_pixels));
    for (const UvInspectionTriangle& triangle : inspection.triangles) {
        if (triangle.uv_area <= 0.0) {
            ++report.degenerate_uv_triangle_count;
            continue;
        }
        if (!triangle.surface_area_finite) {
            ++report.unavailable_surface_area_triangle_count;
            continue;
        }
        if (triangle.surface_area <= 0.0) {
            ++report.degenerate_surface_triangle_count;
            continue;
        }
        const double log_density = 0.5 * (
            std::log(triangle.uv_area) + log_width + log_height -
            std::log(triangle.surface_area));
        const double density = std::exp(log_density);
        if (!std::isfinite(density) || density <= 0.0) {
            return fail(core::ErrorCode::unsupported,
                "UV texel density exceeds the inspector's numeric range");
        }
        report.minimum_area_equivalent_pixels_per_unit = std::min(
            report.minimum_area_equivalent_pixels_per_unit, density);
        report.maximum_area_equivalent_pixels_per_unit = std::max(
            report.maximum_area_equivalent_pixels_per_unit, density);
        ++report.measured_triangle_count;
    }
    if (report.measured_triangle_count == 0U) {
        return fail(core::ErrorCode::unsupported,
            "UV texel density has no non-degenerate source and UV triangles to measure");
    }
    return Result::success(std::move(report));
}

} // namespace carto::desktop
