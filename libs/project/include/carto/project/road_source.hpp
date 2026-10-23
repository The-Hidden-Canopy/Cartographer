#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/curve_path.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace carto::project {

enum class RoadSurfaceClass : std::uint8_t {
    asphalt = 0U,
    concrete = 1U,
    gravel = 2U,
    dirt = 3U,
    trail = 4U,
};

enum class RoadFunctionClass : std::uint8_t {
    local = 0U,
    collector = 1U,
    arterial = 2U,
    service = 3U,
    driveway = 4U,
    pedestrian = 5U,
};

enum class RoadAccessState : std::uint8_t {
    open = 0U,
    restricted = 1U,
    closed = 2U,
};

enum class RoadDrainageIntent : std::uint8_t {
    none = 0U,
    roadside_ditch = 1U,
    culvert = 2U,
    swale_crossing = 3U,
};

struct RoadCoverageBounds final {
    double min_x = 0.0;
    double min_y = 0.0;
    double min_z = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    double max_z = 0.0;
};

// Cartographer-owned road design intent. The centerline is a separately
// revisioned CurvePath; this record never copies or silently regenerates it.
struct RoadSource final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr double kMaxWidthMeters = 100.0;
    static constexpr double kMaxShoulderMeters = 25.0;
    static constexpr double kMaxGradeRatio = 1.0;
    static constexpr double kMaxCurveRadiusMeters = 1.0e9;

    std::string source_identity;
    core::Revision source_revision;
    std::string centerline_identity;
    core::Revision centerline_revision;
    double width_meters = 6.0;
    double shoulder_meters = 0.0;
    double maximum_grade_ratio = 0.12;
    double minimum_curve_radius_meters = 20.0;
    RoadSurfaceClass surface_class = RoadSurfaceClass::asphalt;
    RoadFunctionClass function_class = RoadFunctionClass::local;
    RoadAccessState access_state = RoadAccessState::open;
    RoadDrainageIntent drainage_intent = RoadDrainageIntent::none;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<RoadCoverageBounds> coverage_bounds(
        const geometry::CurvePath& centerline) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<RoadSource> deserialize(std::string_view text);
    [[nodiscard]] assets::Sha256Digest source_digest() const;
};

} // namespace carto::project
