#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/navgraph/identity.hpp>
#include <carto/navgraph/math_compare.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::navgraph {

// Axis-aligned navigation plane the compiled products are measured on. This is
// a neutral prepared coordinate frame, not a destination engine convention.
enum class NavigationPlane : std::uint8_t {
    xy,
    xz,
    yz,
};

[[nodiscard]] constexpr const char* navigation_plane_name(NavigationPlane plane) noexcept {
    switch (plane) {
    case NavigationPlane::xy: return "xy";
    case NavigationPlane::xz: return "xz";
    case NavigationPlane::yz: return "yz";
    }
    return "unknown";
}

// Neutral semantic region compiled from world::SemanticArea. It preserves the
// stable source identity, semantic classification, hierarchical parent, and a
// conservative planar footprint plus vertical extent of the authored boundary.
// The region record is static authored meaning only: no occupancy, cost, or
// runtime state belongs here.
struct SemanticAreaRegion {
    static constexpr std::size_t kMaxBoundaryPoints = 65'536U;

    RegionId id;
    SourceAreaId source_area;
    std::string semantic_type;
    std::optional<SourceAreaId> parent_area;
    NavigationPlane plane = NavigationPlane::xz;
    std::vector<core::Vec3d> boundary;
    core::Bounds3d bounds;
    double minimum_clearance = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const SemanticAreaRegion& other) const noexcept {
        return id == other.id && source_area == other.source_area &&
            semantic_type == other.semantic_type && parent_area == other.parent_area &&
            plane == other.plane && vec3_span_equal(boundary, other.boundary) &&
            bounds_equal(bounds, other.bounds) &&
            minimum_clearance == other.minimum_clearance;
    }
};

// Projects a world-space point onto the region plane, keeping the out-of-plane
// axis at zero so planar products are deterministic and grid-friendly.
[[nodiscard]] core::Vec3d project_to_plane(core::Vec3d point, NavigationPlane plane) noexcept;

// Compiles one validated semantic area into its neutral region record. The
// area boundary must be a simple, non-self-intersecting planar loop expressed
// in the same world frame as the model; the compiled footprint is the planar
// projection of that loop and the compiled bounds are its conservative
// axis-aligned extent.
[[nodiscard]] core::Result<SemanticAreaRegion> compile_region(
    const world::SemanticArea& area,
    const std::vector<core::Vec3d>& boundary,
    NavigationPlane plane,
    double minimum_clearance);

} // namespace carto::navgraph
