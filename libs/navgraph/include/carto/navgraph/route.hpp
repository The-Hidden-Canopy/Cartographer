#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/navgraph/identity.hpp>
#include <carto/navgraph/math_compare.hpp>
#include <carto/navgraph/region.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::navgraph {

// Ordered spine of a compiled route. Control points are the authored path
// control points projected onto the route plane; cumulative distances are
// monotone, start at zero, and are measured in world units.
struct RouteSpine {
    NavigationPlane plane = NavigationPlane::xz;
    std::vector<core::Vec3d> control_points;
    std::vector<double> cumulative_distances;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const RouteSpine& other) const noexcept {
        return plane == other.plane &&
            vec3_span_equal(control_points, other.control_points) &&
            cumulative_distances == other.cumulative_distances;
    }
};

// Neutral route product derived deterministically from world::SemanticPath.
// It carries the authored centerline, width, terminal area references, and the
// conservative bounds of the centerline. Which agent classes may use it is a
// separate question answered by AgentProfile support, not stored per route.
struct SemanticRouteProduct {
    static constexpr std::size_t kMaxControlPoints = 65'536U;

    RouteId id;
    SourcePathId source_path;
    std::string semantic_type;
    RouteSpine spine;
    double width = 0.0;
    bool closed = false;
    std::optional<SourceAreaId> start_area;
    std::optional<SourceAreaId> end_area;
    core::Bounds3d bounds;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const SemanticRouteProduct& other) const noexcept {
        return id == other.id && source_path == other.source_path &&
            semantic_type == other.semantic_type && spine == other.spine &&
            width == other.width && closed == other.closed &&
            start_area == other.start_area && end_area == other.end_area &&
            bounds_equal(bounds, other.bounds);
    }
};

// Neutral corridor product: the conservative swept footprint of one semantic
// path. Polygon vertices are planar projections of the path control points,
// one vertex per control point, in path order; the polygon is closed
// implicitly. The corridor bounds expand the route bounds by half the width on
// the two plane axes so the envelope bounds every point within `width` of the
// centerline.
struct SemanticCorridorProduct {
    CorridorId id;
    SourcePathId source_path;
    RouteId route;
    NavigationPlane plane = NavigationPlane::xz;
    std::vector<core::Vec3d> polygon;
    core::Bounds3d corridor_bounds;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const SemanticCorridorProduct& other) const noexcept {
        return id == other.id && source_path == other.source_path && route == other.route &&
            plane == other.plane && vec3_span_equal(polygon, other.polygon) &&
            bounds_equal(corridor_bounds, other.corridor_bounds);
    }
};

// Compiles one validated semantic path into its neutral route and corridor
// products. The route records the authored centerline exactly; the corridor
// expands it conservatively by half the authored width. Terminal area
// references are retained as stable source identities for correspondence.
[[nodiscard]] core::Result<SemanticRouteProduct> compile_route(
    const world::SemanticPath& path,
    NavigationPlane plane);

[[nodiscard]] core::Result<SemanticCorridorProduct> compile_corridor(
    const SemanticRouteProduct& route);

} // namespace carto::navgraph
