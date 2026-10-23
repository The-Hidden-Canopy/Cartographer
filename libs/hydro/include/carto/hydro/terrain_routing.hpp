#pragma once

#include <carto/hydro/bathymetry.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::hydro {

enum class HydroTerrainRouteKind : std::uint8_t {
    downslope = 0U,
    flat = 1U,
    outlet = 2U,
    sink = 3U,
};

struct HydroTerrainRouteCell final {
    // Stable row-major identity: y * field.width + x + 1.
    std::uint64_t cell_id = 0U;
    // Zero only for explicitly declared outlets and closed-depression sinks.
    std::uint64_t downstream_cell_id = 0U;
    HydroTerrainRouteKind kind = HydroTerrainRouteKind::sink;
    double bed_elevation_m = 0.0;
    double contributing_area_m2 = 0.0;
};

// Deterministic D8 terrain routing over an authored BathymetryField. This is
// a derived analysis product, not authoritative water state or a solver.
struct HydroTerrainRouting final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 128U * 1024U * 1024U;
    static constexpr std::size_t kMaxSamples = BathymetryField::kMaxSamples;

    HydroSurfaceSource source;
    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    assets::Sha256Digest bathymetry_digest;
    core::Vec2d origin_xy{};
    double cell_size_m = 0.0;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint64_t> declared_outlet_cell_ids;
    std::vector<HydroTerrainRouteCell> cells;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_against(
        const BathymetryField& bathymetry) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<HydroTerrainRouting> deserialize(
        std::string_view text);
};

// Outlet identities are one-based row-major cell IDs. No boundary is treated
// as an outlet unless it is explicitly listed; closed depressions remain sinks.
[[nodiscard]] core::Result<HydroTerrainRouting> derive_terrain_routing(
    const BathymetryField& bathymetry,
    std::span<const std::uint64_t> declared_outlet_cell_ids = {});

} // namespace carto::hydro
