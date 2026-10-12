#pragma once

#include <carto/hydro/contact.hpp>
#include <carto/hydro/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace carto::hydro {

// Terrain evidence is supplied by the terrain owner. Cartographer records its
// identity and uses it to reject a runoff plan when the sampled terrain has
// moved or been replaced; it does not become a second terrain authority.
struct HydroTerrainRunoffEvidence final {
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxTerrainIdentityBytes = 256U;
    static constexpr std::size_t kMaxTileIdentityBytes = 256U;
    static constexpr double kMaxFlowDistanceM = 1.0e6;

    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    std::string source_tile_identity;
    std::string outlet_tile_identity;
    double source_elevation_m = 0.0;
    double outlet_elevation_m = 0.0;
    double flow_distance_m = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// A catchment is authored hydro intent. Rainfall and terrain samples are
// inputs to a derived plan; they are not persistent runtime state or a water
// mesh.
struct HydroRunoffCatchment final {
    static constexpr std::size_t kMaxSerializedBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr double kMaxCatchmentAreaM2 = 1.0e12;

    std::string stable_id;
    HydroSurfaceSource source;
    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    std::uint64_t boundary_edge_id = 0U;
    double catchment_area_m2 = 0.0;
    double runoff_coefficient = 0.0;
    double infiltration_fraction = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct HydroRunoffInput final {
    static constexpr double kMaxPrecipitationRateMPerS = 1.0e3;
    static constexpr double kMaxElapsedSeconds = 86'400.0;

    // Precipitation is expressed as water depth per second over the authored
    // catchment. The product with area and elapsed time is a volume.
    double precipitation_rate_m_per_s = 0.0;
    double elapsed_seconds = 0.0;
    HydroTerrainRunoffEvidence terrain;

    [[nodiscard]] core::Result<void> validate() const;
};

struct HydroRunoffPlan final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr double kMaxVolumeM3 = 1.0e24;

    HydroRunoffCatchment catchment;
    HydroTerrainRunoffEvidence terrain;
    double precipitation_rate_m_per_s = 0.0;
    double elapsed_seconds = 0.0;
    double slope = 0.0;
    double slope_capture_fraction = 0.0;
    double gross_precipitation_volume_m3 = 0.0;
    double infiltrated_volume_m3 = 0.0;
    double runoff_volume_m3 = 0.0;
    double conservation_residual_m3 = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<HydroRunoffPlan> deserialize(
        std::string_view text);

    // Bind the derived plan to the current graph before producing a flux. The
    // graph remains authoritative for edge identity, enablement, capacity and
    // source revision; this method never mutates either object.
    [[nodiscard]] core::Result<HydroBoundaryFlux> boundary_flux_for(
        const HydroGraph& graph,
        double delta_seconds) const;
};

[[nodiscard]] core::Result<HydroRunoffPlan> plan_runoff(
    const HydroRunoffCatchment& catchment,
    const HydroRunoffInput& input);

} // namespace carto::hydro
