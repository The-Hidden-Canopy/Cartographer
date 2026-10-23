#include <carto/navgraph/route.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cmath>

namespace carto::navgraph {

core::Result<void> RouteSpine::validate() const {
    if (!detail::valid_plane(plane) ||
        control_points.size() < 2U ||
        control_points.size() > SemanticRouteProduct::kMaxControlPoints ||
        control_points.size() != cumulative_distances.size()) {
        return core::Result<void>::failure(detail::invalid(
            "route spine plane, control-point count, or distance table is invalid"));
    }
    for (std::size_t index = 0U; index < control_points.size(); ++index) {
        if (!control_points[index].finite() ||
            !std::isfinite(cumulative_distances[index])) {
            return core::Result<void>::failure(detail::invalid(
                "route spine contains a non-finite point or distance"));
        }
        if (index > 0U) {
            const double segment = (control_points[index] - control_points[index - 1U]).length();
            const double expected = cumulative_distances[index - 1U] + segment;
            const double tolerance = 1e-9 * std::max(1.0, std::fabs(expected));
            if (cumulative_distances[index] < cumulative_distances[index - 1U] ||
                std::fabs(cumulative_distances[index] - expected) > tolerance) {
                return core::Result<void>::failure(detail::validation(
                    "route spine cumulative distances do not match its geometry"));
            }
        } else if (cumulative_distances[index] != 0.0) {
            return core::Result<void>::failure(detail::validation(
                "route spine cumulative distances must start at zero"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> SemanticRouteProduct::validate() const {
    if (!id || !source_path || !std::isfinite(width) || width < 0.0 ||
        !detail::safe_text(semantic_type, detail::kMaxSemanticTypeBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "route product identity, source, width, or type is invalid"));
    }
    if (auto result = spine.validate(); !result) return result;
    if (closed && (start_area.has_value() || end_area.has_value())) {
        return core::Result<void>::failure(detail::validation(
            "closed routes may not claim terminal areas"));
    }
    if (start_area.has_value() && !start_area.value()) {
        return core::Result<void>::failure(detail::validation(
            "route start area reference is invalid"));
    }
    if (end_area.has_value() && !end_area.value()) {
        return core::Result<void>::failure(detail::validation(
            "route end area reference is invalid"));
    }
    if (!bounds.valid()) {
        return core::Result<void>::failure(detail::invalid(
            "route product bounds are invalid"));
    }
    for (const auto& point : spine.control_points) {
        if (point.x < bounds.minimum.x || point.x > bounds.maximum.x ||
            point.y < bounds.minimum.y || point.y > bounds.maximum.y ||
            point.z < bounds.minimum.z || point.z > bounds.maximum.z) {
            return core::Result<void>::failure(detail::validation(
                "route product bounds do not cover its centerline"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> SemanticCorridorProduct::validate() const {
    if (!id || !source_path || !route || !detail::valid_plane(plane) ||
        polygon.size() < 3U || polygon.size() > SemanticRouteProduct::kMaxControlPoints ||
        !corridor_bounds.valid()) {
        return core::Result<void>::failure(detail::invalid(
            "corridor product identity, source, route, polygon, or bounds is invalid"));
    }
    for (const auto& point : polygon) {
        if (!project_to_plane(point, plane).finite()) {
            return core::Result<void>::failure(detail::invalid(
                "corridor polygon contains a non-finite planar projection"));
        }
        if (point.x < corridor_bounds.minimum.x || point.x > corridor_bounds.maximum.x ||
            point.y < corridor_bounds.minimum.y || point.y > corridor_bounds.maximum.y ||
            point.z < corridor_bounds.minimum.z || point.z > corridor_bounds.maximum.z) {
            return core::Result<void>::failure(detail::validation(
                "corridor bounds do not cover its polygon"));
        }
    }
    return core::Result<void>::success();
}

core::Result<SemanticRouteProduct> compile_route(
    const world::SemanticPath& path,
    NavigationPlane plane) {
    if (auto result = path.validate(); !result) {
        return core::Result<SemanticRouteProduct>::failure(result.error());
    }
    SemanticRouteProduct route;
    route.id = derive_route_id(path.id);
    route.source_path = path.id;
    route.semantic_type = path.semantic_type;
    route.width = path.width;
    route.closed = path.closed;
    route.start_area = path.start_area;
    route.end_area = path.end_area;
    route.spine.plane = plane;
    route.spine.control_points.reserve(path.control_points.size());
    route.spine.cumulative_distances.reserve(path.control_points.size());
    double cumulative = 0.0;
    for (std::size_t index = 0U; index < path.control_points.size(); ++index) {
        const core::Vec3d point = project_to_plane(path.control_points[index], plane);
        route.spine.control_points.push_back(point);
        if (index > 0U) {
            cumulative += (point - route.spine.control_points[index - 1U]).length();
        }
        route.spine.cumulative_distances.push_back(cumulative);
        route.bounds.include(point);
    }
    if (auto result = route.validate(); !result) {
        return core::Result<SemanticRouteProduct>::failure(result.error());
    }
    return core::Result<SemanticRouteProduct>::success(std::move(route));
}

core::Result<SemanticCorridorProduct> compile_corridor(
    const SemanticRouteProduct& route) {
    SemanticCorridorProduct corridor;
    corridor.id = derive_corridor_id(route.source_path);
    corridor.source_path = route.source_path;
    corridor.route = route.id;
    corridor.plane = route.spine.plane;
    corridor.polygon = route.spine.control_points;
    corridor.corridor_bounds = route.bounds;
    const double half = route.width * 0.5;
    switch (route.spine.plane) {
    case NavigationPlane::xy:
        corridor.corridor_bounds.minimum.x -= half;
        corridor.corridor_bounds.minimum.y -= half;
        corridor.corridor_bounds.maximum.x += half;
        corridor.corridor_bounds.maximum.y += half;
        break;
    case NavigationPlane::xz:
        corridor.corridor_bounds.minimum.x -= half;
        corridor.corridor_bounds.minimum.z -= half;
        corridor.corridor_bounds.maximum.x += half;
        corridor.corridor_bounds.maximum.z += half;
        break;
    case NavigationPlane::yz:
        corridor.corridor_bounds.minimum.y -= half;
        corridor.corridor_bounds.minimum.z -= half;
        corridor.corridor_bounds.maximum.y += half;
        corridor.corridor_bounds.maximum.z += half;
        break;
    }
    if (auto result = corridor.validate(); !result) {
        return core::Result<SemanticCorridorProduct>::failure(result.error());
    }
    return core::Result<SemanticCorridorProduct>::success(std::move(corridor));
}

} // namespace carto::navgraph
