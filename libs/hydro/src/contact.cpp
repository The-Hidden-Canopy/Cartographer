#include <carto/hydro/contact.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace carto::hydro {
namespace {

// Keep the largest admitted sphere within the bounded evidence range even
// before the submerged fraction is applied: its full volume remains below
// kMaxContactValue.
constexpr double kMaxRadiusM = 1.0e6;
constexpr double kMaxContactValue = 1.0e24;
constexpr double kNormalTolerance = 1.0e-9;

bool valid_direction(SurfaceCrossingDirection direction) noexcept {
    switch (direction) {
    case SurfaceCrossingDirection::entering:
    case SurfaceCrossingDirection::exiting:
        return true;
    }
    return false;
}

double signed_distance(const PlanarFreeSurface& surface, core::Vec3d point) noexcept {
    return core::dot(point - surface.origin, surface.normal);
}

HydroContactEvidence make_evidence(
    const PlanarFreeSurface& surface,
    core::Vec3d point,
    double distance,
    double submerged_fraction,
    double submerged_volume,
    double wetted_area) {
    return HydroContactEvidence{
        surface.source,
        point,
        surface.normal,
        surface.fluid_velocity_mps,
        distance,
        submerged_fraction,
        submerged_volume,
        wetted_area,
        true,
        false};
}

} // namespace

core::Result<void> HydroSurfaceSource::validate() const {
    if (!detail::safe_text(domain_id, kMaxDomainBytes) ||
        source_digest.is_zero() ||
        !detail::safe_text(coordinate_frame, kMaxFrameBytes) ||
        !detail::safe_text(provenance, kMaxProvenanceBytes)) {
        return core::Result<void>::failure(detail::validation(
            "hydro surface source identity, frame, provenance, or digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroSurfaceSource::canonical() const {
    std::ostringstream output;
    output << "SOURCE " << std::quoted(domain_id) << ' '
           << source_revision.value() << ' ' << source_digest.hex() << ' '
           << std::quoted(coordinate_frame) << ' ' << std::quoted(provenance);
    return output.str();
}

core::Result<void> PlanarFreeSurface::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!origin.finite() || !normal.finite() || !fluid_velocity_mps.finite()) {
        return core::Result<void>::failure(detail::validation(
            "planar free surface contains non-finite geometry or velocity"));
    }
    const double length = normal.length();
    if (!std::isfinite(length) || length <= 1.0e-12 ||
        std::abs(length - 1.0) > kNormalTolerance) {
        return core::Result<void>::failure(detail::validation(
            "planar free surface normal must be unit length"));
    }
    return core::Result<void>::success();
}

core::Result<void> HydroContactEvidence::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!query_point.finite() || !surface_normal.finite() ||
        !fluid_velocity_mps.finite() ||
        !std::isfinite(surface_signed_distance_m) ||
        !std::isfinite(submerged_fraction) || submerged_fraction < 0.0 ||
        submerged_fraction > 1.0 || !detail::finite_bounded(
            submerged_volume_m3, kMaxContactValue) ||
        !detail::finite_bounded(wetted_area_m2, kMaxContactValue) ||
        !fluid_contact || solid_collision ||
        std::abs(surface_normal.length() - 1.0) > kNormalTolerance) {
        return core::Result<void>::failure(detail::validation(
            "hydro contact evidence is invalid or claims solid collision"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceCrossing::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!valid_direction(direction) || !std::isfinite(time_of_impact) ||
        time_of_impact < 0.0 || time_of_impact > 1.0 || !point.finite()) {
        return core::Result<void>::failure(detail::validation(
            "hydro surface crossing is invalid or unbounded"));
    }
    return core::Result<void>::success();
}

core::Result<HydroContactEvidence> query_point(
    const PlanarFreeSurface& surface,
    core::Vec3d point) {
    if (auto result = surface.validate(); !result) {
        return core::Result<HydroContactEvidence>::failure(result.error());
    }
    if (!point.finite()) {
        return core::Result<HydroContactEvidence>::failure(detail::invalid(
            "hydro point query is non-finite"));
    }
    const double distance = signed_distance(surface, point);
    const auto evidence = make_evidence(surface, point, distance, distance <= 0.0 ? 1.0 : 0.0, 0.0, 0.0);
    if (auto result = evidence.validate(); !result) {
        return core::Result<HydroContactEvidence>::failure(result.error());
    }
    return core::Result<HydroContactEvidence>::success(evidence);
}

core::Result<HydroContactEvidence> query_sphere(
    const PlanarFreeSurface& surface,
    core::Vec3d center,
    double radius_m) {
    if (auto result = surface.validate(); !result) {
        return core::Result<HydroContactEvidence>::failure(result.error());
    }
    if (!center.finite() || !std::isfinite(radius_m) || radius_m <= 0.0 ||
        radius_m > kMaxRadiusM) {
        return core::Result<HydroContactEvidence>::failure(detail::invalid(
            "hydro sphere query has an invalid center or radius"));
    }
    const double distance = signed_distance(surface, center);
    const double pi = std::acos(-1.0);
    const double full_volume = (4.0 / 3.0) * pi * radius_m * radius_m * radius_m;
    const double full_area = 4.0 * pi * radius_m * radius_m;
    double fraction = 0.0;
    double wetted_area = 0.0;
    if (distance <= -radius_m) {
        fraction = 1.0;
        wetted_area = full_area;
    } else if (distance < radius_m) {
        const double cap_height = radius_m - distance;
        fraction = (cap_height * cap_height * (3.0 * radius_m - cap_height)) /
            (4.0 * radius_m * radius_m * radius_m);
        wetted_area = 2.0 * pi * radius_m * cap_height;
    }
    const auto evidence = make_evidence(
        surface, center, distance, std::clamp(fraction, 0.0, 1.0),
        full_volume * std::clamp(fraction, 0.0, 1.0),
        std::clamp(wetted_area, 0.0, full_area));
    if (auto result = evidence.validate(); !result) {
        return core::Result<HydroContactEvidence>::failure(result.error());
    }
    return core::Result<HydroContactEvidence>::success(evidence);
}

core::Result<std::optional<SurfaceCrossing>> sweep_surface(
    const PlanarFreeSurface& surface,
    core::Vec3d start,
    core::Vec3d end) {
    if (auto result = surface.validate(); !result) {
        return core::Result<std::optional<SurfaceCrossing>>::failure(result.error());
    }
    if (!start.finite() || !end.finite()) {
        return core::Result<std::optional<SurfaceCrossing>>::failure(detail::invalid(
            "hydro surface sweep contains a non-finite endpoint"));
    }
    const double first = signed_distance(surface, start);
    const double second = signed_distance(surface, end);
    if ((first == 0.0 && second == 0.0) ||
        (first > 0.0 && second > 0.0) || (first < 0.0 && second < 0.0)) {
        return core::Result<std::optional<SurfaceCrossing>>::success(std::nullopt);
    }

    SurfaceCrossing crossing;
    crossing.source = surface.source;
    if (first >= 0.0 && second <= 0.0) {
        crossing.direction = SurfaceCrossingDirection::entering;
    } else {
        crossing.direction = SurfaceCrossingDirection::exiting;
    }
    if (first == 0.0) {
        crossing.time_of_impact = 0.0;
    } else if (second == 0.0) {
        crossing.time_of_impact = 1.0;
    } else {
        crossing.time_of_impact = first / (first - second);
    }
    crossing.time_of_impact = std::clamp(crossing.time_of_impact, 0.0, 1.0);
    crossing.point = start + (end - start) * crossing.time_of_impact;
    if (auto result = crossing.validate(); !result) {
        return core::Result<std::optional<SurfaceCrossing>>::failure(result.error());
    }
    return core::Result<std::optional<SurfaceCrossing>>::success(crossing);
}

} // namespace carto::hydro
