#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace carto::hydro {

// This source binding is intentionally provider-neutral. It records why a
// fluid query is admissible without exposing a runtime handle or a solid
// collision registration.
struct HydroSurfaceSource final {
    static constexpr std::size_t kMaxDomainBytes = 256U;
    static constexpr std::size_t kMaxFrameBytes = 128U;
    static constexpr std::size_t kMaxProvenanceBytes = 256U;

    std::string domain_id;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string coordinate_frame;
    std::string provenance;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// A bounded analytic free-surface reference product. More complex surfaces
// may provide the same evidence contract later; this reference plane keeps
// the CPU path deterministic and makes contact/CCD acceptance executable now.
struct PlanarFreeSurface final {
    HydroSurfaceSource source;
    core::Vec3d origin{};
    core::Vec3d normal{0.0, 0.0, 1.0};
    core::Vec3d fluid_velocity_mps{};

    [[nodiscard]] core::Result<void> validate() const;
};

struct HydroContactEvidence final {
    HydroSurfaceSource source;
    core::Vec3d query_point{};
    core::Vec3d surface_normal{0.0, 0.0, 1.0};
    core::Vec3d fluid_velocity_mps{};
    double surface_signed_distance_m = 0.0;
    double submerged_fraction = 0.0;
    double submerged_volume_m3 = 0.0;
    double wetted_area_m2 = 0.0;
    bool fluid_contact = true;
    bool solid_collision = false;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class SurfaceCrossingDirection : std::uint8_t {
    entering,
    exiting,
};

struct SurfaceCrossing final {
    HydroSurfaceSource source;
    SurfaceCrossingDirection direction = SurfaceCrossingDirection::entering;
    double time_of_impact = 0.0;
    core::Vec3d point{};

    [[nodiscard]] core::Result<void> validate() const;
};

[[nodiscard]] core::Result<HydroContactEvidence> query_point(
    const PlanarFreeSurface& surface,
    core::Vec3d point);

[[nodiscard]] core::Result<HydroContactEvidence> query_sphere(
    const PlanarFreeSurface& surface,
    core::Vec3d center,
    double radius_m);

[[nodiscard]] core::Result<std::optional<SurfaceCrossing>> sweep_surface(
    const PlanarFreeSurface& surface,
    core::Vec3d start,
    core::Vec3d end);

} // namespace carto::hydro
