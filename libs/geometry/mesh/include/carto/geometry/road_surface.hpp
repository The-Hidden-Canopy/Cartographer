#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/curve_path.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::geometry {

struct RoadSurfaceStation final {
    core::Vec3d center;
    core::Vec3d shoulder_left;
    core::Vec3d road_left;
    core::Vec3d road_right;
    core::Vec3d shoulder_right;
};

// A disposable ribbon derived from one exact centerline and road profile.
// It is a preview product, not a persisted mesh or runtime collision object.
struct RoadSurfaceMesh final {
    static constexpr std::size_t kMaxStations = 16'384U;

    std::string centerline_identity;
    core::Revision centerline_revision;
    double road_width_meters = 0.0;
    double shoulder_width_meters = 0.0;
    double maximum_grade_ratio = 0.0;
    double minimum_curve_radius_meters = 0.0;
    std::uint32_t samples_per_segment = 0U;
    std::vector<RoadSurfaceStation> stations;

    [[nodiscard]] std::size_t road_triangle_count() const noexcept {
        return stations.size() > 1U ? (stations.size() - 1U) * 2U : 0U;
    }
    [[nodiscard]] std::size_t shoulder_triangle_count() const noexcept {
        return stations.size() > 1U ? (stations.size() - 1U) * 4U : 0U;
    }
    [[nodiscard]] core::Result<void> validate() const;
};

inline constexpr std::uint32_t kRoadSurfaceRoadMaterialSlot = 1U;
inline constexpr std::uint32_t kRoadSurfaceShoulderMaterialSlot = 2U;
inline constexpr std::uint64_t kRoadSurfaceUvSetId = 1U;
inline constexpr std::string_view kRoadSurfaceUvLayerIdentity = "cartographer.uv0";
inline constexpr std::string_view kRoadSurfaceMaterialSlotLayerIdentity =
    "cartographer.material_slot";

[[nodiscard]] core::Result<RoadSurfaceMesh> build_road_surface(
    const CurvePath& centerline,
    double road_width_meters,
    double shoulder_width_meters,
    double maximum_grade_ratio,
    double minimum_curve_radius_meters,
    std::uint32_t samples_per_segment = 16U);

// Converts a current road ribbon into editable, source-ready surface geometry.
// Road and shoulder strips carry separate material slots and world-scale UVs;
// this does not add thickness, terrain draping, collision, or engineering proof.
[[nodiscard]] core::Result<EditableMesh> build_road_surface_editable_mesh(
    const RoadSurfaceMesh& road_surface);

} // namespace carto::geometry
