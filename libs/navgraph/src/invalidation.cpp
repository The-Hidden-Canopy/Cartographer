#include <carto/navgraph/invalidation.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace carto::navgraph {

core::Result<void> NavigationTileGrid::validate() const {
    if (!detail::safe_text(grid_identity, kMaxGridIdentityBytes) ||
        !std::isfinite(tile_extent) || tile_extent <= 0.0 || level > 31U) {
        return core::Result<void>::failure(detail::invalid(
            "navigation tile grid identity, extent, or level is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> NavigationTileFootprint::validate() const {
    if (!detail::safe_text(grid_identity, kMaxGridIdentityBytes) || level > 31U ||
        minimum_x > maximum_x || minimum_y > maximum_y || minimum_z > maximum_z) {
        return core::Result<void>::failure(detail::invalid(
            "navigation tile footprint grid, level, or ordered extent is invalid"));
    }
    constexpr std::int64_t kAxisBound = (std::int64_t{1} << 40) - 1;
    if (minimum_x < -kAxisBound || maximum_x > kAxisBound ||
        minimum_y < -kAxisBound || maximum_y > kAxisBound ||
        minimum_z < -kAxisBound || maximum_z > kAxisBound) {
        return core::Result<void>::failure(detail::validation(
            "navigation tile footprint coordinates exceed the bounded span"));
    }
    return core::Result<void>::success();
}

std::string NavigationTileFootprint::canonical() const {
    std::ostringstream output;
    output << "tile-range-v1{" << grid_identity << ';' << level << ';'
           << minimum_x << ',' << minimum_y << ',' << minimum_z << ';'
           << maximum_x << ',' << maximum_y << ',' << maximum_z << '}';
    return output.str();
}

core::Result<bool> NavigationTileFootprint::intersects(
    const NavigationTileFootprint& other) const {
    if (auto result = validate(); !result) {
        return core::Result<bool>::failure(result.error());
    }
    if (auto result = other.validate(); !result) {
        return core::Result<bool>::failure(result.error());
    }
    if (grid_identity != other.grid_identity || level != other.level) {
        return core::Result<bool>::failure(detail::validation(
            "navigation tile footprints use incompatible grids or levels"));
    }
    return core::Result<bool>::success(
        minimum_x <= other.maximum_x && maximum_x >= other.minimum_x &&
        minimum_y <= other.maximum_y && maximum_y >= other.minimum_y &&
        minimum_z <= other.maximum_z && maximum_z >= other.minimum_z);
}

core::Result<NavigationTileFootprint> footprint_for_bounds(
    const NavigationTileGrid& grid,
    const core::Bounds3d& bounds) {
    if (auto result = grid.validate(); !result) {
        return core::Result<NavigationTileFootprint>::failure(result.error());
    }
    if (!bounds.valid()) {
        return core::Result<NavigationTileFootprint>::failure(detail::invalid(
            "footprint derivation requires finite, ordered bounds"));
    }
    const double level_scale = std::ldexp(1.0, -static_cast<int>(grid.level));
    const double extent = grid.tile_extent * level_scale;
    if (!std::isfinite(extent) || extent <= 0.0) {
        return core::Result<NavigationTileFootprint>::failure(detail::validation(
            "footprint derivation produced a non-finite tile extent"));
    }

    auto axis_tiles = [extent](double minimum, double maximum,
                               std::int64_t& tile_minimum, std::int64_t& tile_maximum)
        -> bool {
        // Closed-tile semantics: tile t covers [t * extent, (t + 1) * extent],
        // so a range endpoint landing exactly on a tile boundary belongs to the
        // lower tile. Degenerate ranges cover the single tile holding the point.
        const double scaled_minimum = std::floor(minimum / extent);
        const double scaled_maximum =
            maximum == minimum ? scaled_minimum : std::ceil(maximum / extent) - 1.0;
        if (scaled_minimum < -9.0e17 || scaled_maximum > 9.0e17 ||
            scaled_minimum > scaled_maximum) {
            return false;
        }
        tile_minimum = static_cast<std::int64_t>(scaled_minimum);
        tile_maximum = static_cast<std::int64_t>(scaled_maximum);
        return true;
    };

    NavigationTileFootprint footprint;
    footprint.grid_identity = grid.grid_identity;
    footprint.level = grid.level;
    if (!axis_tiles(bounds.minimum.x, bounds.maximum.x,
                    footprint.minimum_x, footprint.maximum_x) ||
        !axis_tiles(bounds.minimum.y, bounds.maximum.y,
                    footprint.minimum_y, footprint.maximum_y) ||
        !axis_tiles(bounds.minimum.z, bounds.maximum.z,
                    footprint.minimum_z, footprint.maximum_z)) {
        return core::Result<NavigationTileFootprint>::failure(detail::validation(
            "footprint derivation produced an out-of-range tile span"));
    }
    if (auto result = footprint.validate(); !result) {
        return core::Result<NavigationTileFootprint>::failure(result.error());
    }
    return core::Result<NavigationTileFootprint>::success(std::move(footprint));
}

core::Result<NavigationTileFootprint> union_footprints(
    std::string grid_identity,
    std::uint32_t level,
    const std::vector<NavigationTileFootprint>& footprints) {
    if (footprints.empty()) {
        return core::Result<NavigationTileFootprint>::failure(detail::invalid(
            "footprint union requires at least one footprint"));
    }
    NavigationTileFootprint combined;
    combined.grid_identity = std::move(grid_identity);
    combined.level = level;
    for (const auto& footprint : footprints) {
        if (footprint.grid_identity != combined.grid_identity ||
            footprint.level != combined.level) {
            return core::Result<NavigationTileFootprint>::failure(detail::validation(
                "footprint union received a mixed grid or level"));
        }
        if (auto result = footprint.validate(); !result) {
            return core::Result<NavigationTileFootprint>::failure(result.error());
        }
        combined.minimum_x = std::min(combined.minimum_x, footprint.minimum_x);
        combined.minimum_y = std::min(combined.minimum_y, footprint.minimum_y);
        combined.minimum_z = std::min(combined.minimum_z, footprint.minimum_z);
        combined.maximum_x = std::max(combined.maximum_x, footprint.maximum_x);
        combined.maximum_y = std::max(combined.maximum_y, footprint.maximum_y);
        combined.maximum_z = std::max(combined.maximum_z, footprint.maximum_z);
    }
    if (auto result = combined.validate(); !result) {
        return core::Result<NavigationTileFootprint>::failure(result.error());
    }
    return core::Result<NavigationTileFootprint>::success(std::move(combined));
}

} // namespace carto::navgraph
