#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace carto::navgraph {

// Standalone footprint record for navigation invalidation. The field shape
// deliberately mirrors production::SpatialTileRange (string grid identity,
// uint32 level, inclusive int64 minimum/maximum on three axes) so the
// production-side wiring can map it without transformation. This lib does not
// depend on carto_production; that mapping is the production agent's seam.
struct NavigationTileFootprint {
    static constexpr std::size_t kMaxGridIdentityBytes = 128U;

    std::string grid_identity;
    std::uint32_t level = 0U;
    std::int64_t minimum_x = 0;
    std::int64_t minimum_y = 0;
    std::int64_t minimum_z = 0;
    std::int64_t maximum_x = 0;
    std::int64_t maximum_y = 0;
    std::int64_t maximum_z = 0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<bool> intersects(const NavigationTileFootprint& other) const;
    [[nodiscard]] bool operator==(const NavigationTileFootprint&) const noexcept = default;
};

struct NavigationTileGrid {
    static constexpr std::size_t kMaxGridIdentityBytes = 128U;

    std::string grid_identity;
    // World units covered by one tile edge. Must be finite and positive.
    double tile_extent = 1.0;
    // Grid level 0 spans the world origin; each level above halves the extent.
    std::uint32_t level = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derives the inclusive tile footprint covering an axis-aligned world-space
// extent on one grid. The bounds minimum/maximum must be finite and ordered;
// the result is the minimal integer tile range whose union contains the extent,
// computed with floor/ceil so float-boundary ambiguity cannot leak in.
[[nodiscard]] core::Result<NavigationTileFootprint> footprint_for_bounds(
    const NavigationTileGrid& grid,
    const core::Bounds3d& bounds);

// Computes the union footprint covering every supplied footprint. All inputs
// must share the caller's grid identity and level; mixed grids or levels fail
// closed instead of assuming non-overlap.
[[nodiscard]] core::Result<NavigationTileFootprint> union_footprints(
    std::string grid_identity,
    std::uint32_t level,
    const std::vector<NavigationTileFootprint>& footprints);

} // namespace carto::navgraph
