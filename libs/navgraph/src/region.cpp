#include <carto/navgraph/region.hpp>

#include "detail.hpp"

#include <cmath>

namespace carto::navgraph {

core::Vec3d project_to_plane(core::Vec3d point, NavigationPlane plane) noexcept {
    switch (plane) {
    case NavigationPlane::xy: return {point.x, point.y, 0.0};
    case NavigationPlane::xz: return {point.x, 0.0, point.z};
    case NavigationPlane::yz: return {0.0, point.y, point.z};
    }
    return point;
}

core::Result<void> SemanticAreaRegion::validate() const {
    if (!id || !source_area || !detail::valid_plane(plane) ||
        !detail::safe_text(semantic_type, detail::kMaxSemanticTypeBytes) ||
        boundary.size() < 3U || boundary.size() > kMaxBoundaryPoints ||
        !std::isfinite(minimum_clearance) || minimum_clearance < 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "semantic region identity, source, plane, type, boundary, or clearance is invalid"));
    }
    if (parent_area.has_value() && !parent_area.value()) {
        return core::Result<void>::failure(detail::validation(
            "semantic region parent source area is invalid"));
    }
    if (parent_area.has_value() && parent_area.value() == source_area) {
        return core::Result<void>::failure(detail::validation(
            "semantic region parent must differ from its source area"));
    }
    for (const auto& point : boundary) {
        if (!project_to_plane(point, plane).finite()) {
            return core::Result<void>::failure(detail::invalid(
                "semantic region boundary contains a non-finite planar projection"));
        }
    }
    if (vec3_equal(project_to_plane(boundary.front(), plane),
                   project_to_plane(boundary.back(), plane))) {
        return core::Result<void>::failure(detail::validation(
            "semantic region boundaries close implicitly and may not repeat the first point"));
    }
    if (!bounds.valid()) {
        return core::Result<void>::failure(detail::invalid(
            "semantic region bounds are invalid"));
    }
    // The footprint must conservatively cover the boundary projection.
    for (const auto& point : boundary) {
        const core::Vec3d projected = project_to_plane(point, plane);
        if (projected.x < bounds.minimum.x || projected.x > bounds.maximum.x ||
            projected.y < bounds.minimum.y || projected.y > bounds.maximum.y ||
            projected.z < bounds.minimum.z || projected.z > bounds.maximum.z) {
            return core::Result<void>::failure(detail::validation(
                "semantic region bounds do not cover its boundary"));
        }
    }
    return core::Result<void>::success();
}

core::Result<SemanticAreaRegion> compile_region(
    const world::SemanticArea& area,
    const std::vector<core::Vec3d>& boundary,
    NavigationPlane plane,
    double minimum_clearance) {
    if (auto result = area.validate(); !result) {
        return core::Result<SemanticAreaRegion>::failure(result.error());
    }
    SemanticAreaRegion region;
    region.id = derive_region_id(area.id);
    region.source_area = area.id;
    region.semantic_type = area.semantic_type;
    region.parent_area = area.parent;
    region.plane = plane;
    region.minimum_clearance = minimum_clearance;
    region.boundary.reserve(boundary.size());
    for (const auto& point : boundary) {
        region.boundary.push_back(project_to_plane(point, plane));
        region.bounds.include(region.boundary.back());
    }
    if (auto result = region.validate(); !result) {
        return core::Result<SemanticAreaRegion>::failure(result.error());
    }
    return core::Result<SemanticAreaRegion>::success(std::move(region));
}

} // namespace carto::navgraph
