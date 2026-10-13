#include <carto/hydro/runoff.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <istream>
#include <sstream>
#include <utility>

namespace carto::hydro {
namespace {

constexpr std::string_view kRunoffMagic = "CARTOGRAPHER_HYDRO_RUNOFF_PLAN";
constexpr double kMaxSlope = 1.0e9;

bool close_enough(double left, double right) noexcept {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-9 * scale;
}

bool finite_non_negative_bounded(double value, double maximum) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= maximum;
}

core::Result<void> validate_terrain_binding(
    const HydroRunoffCatchment& catchment,
    const HydroTerrainRunoffEvidence& terrain) {
    if (catchment.terrain_identity != terrain.terrain_identity ||
        catchment.terrain_revision != terrain.terrain_revision ||
        catchment.terrain_digest != terrain.terrain_digest) {
        return core::Result<void>::failure(detail::stale(
            "runoff terrain evidence is stale for the authored catchment"));
    }
    return core::Result<void>::success();
}

bool read_surface_source(std::istream& input, HydroSurfaceSource& source) {
    return detail::require_token(input, "SOURCE") &&
        static_cast<bool>(input >> std::quoted(source.domain_id)) &&
        detail::read_revision(input, source.source_revision) &&
        detail::read_digest(input, source.source_digest) &&
        static_cast<bool>(input >> std::quoted(source.coordinate_frame)) &&
        static_cast<bool>(input >> std::quoted(source.provenance));
}

bool read_catchment(std::istream& input, HydroRunoffCatchment& catchment) {
    return detail::require_token(input, "CATCHMENT") &&
        static_cast<bool>(input >> std::quoted(catchment.stable_id)) &&
        read_surface_source(input, catchment.source) &&
        detail::require_token(input, "TERRAIN_BINDING") &&
        static_cast<bool>(input >> std::quoted(catchment.terrain_identity)) &&
        detail::read_revision(input, catchment.terrain_revision) &&
        detail::read_digest(input, catchment.terrain_digest) &&
        detail::require_token(input, "BOUNDARY_EDGE") &&
        detail::read_unsigned(input, catchment.boundary_edge_id) &&
        detail::require_token(input, "AREA") &&
        detail::read_double(input, catchment.catchment_area_m2) &&
        detail::require_token(input, "RUNOFF_COEFFICIENT") &&
        detail::read_double(input, catchment.runoff_coefficient) &&
        detail::require_token(input, "INFILTRATION_FRACTION") &&
        detail::read_double(input, catchment.infiltration_fraction);
}

} // namespace

core::Result<void> HydroTerrainRunoffEvidence::validate() const {
    if (!detail::safe_text(terrain_identity, kMaxTerrainIdentityBytes) ||
        terrain_revision.value() == 0U || terrain_digest.is_zero() ||
        !detail::safe_text(source_tile_identity, kMaxTileIdentityBytes) ||
        !detail::safe_text(outlet_tile_identity, kMaxTileIdentityBytes) ||
        !std::isfinite(source_elevation_m) || !std::isfinite(outlet_elevation_m) ||
        !std::isfinite(flow_distance_m) || flow_distance_m <= 0.0 ||
        flow_distance_m > kMaxFlowDistanceM) {
        return core::Result<void>::failure(detail::validation(
            "runoff terrain evidence identity, revision, elevation, or distance is invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroTerrainRunoffEvidence::canonical() const {
    std::ostringstream output;
    output << "TERRAIN_SAMPLE " << std::quoted(terrain_identity) << ' '
           << terrain_revision.value() << ' ' << terrain_digest.hex() << ' '
           << std::quoted(source_tile_identity) << ' '
           << std::quoted(outlet_tile_identity) << ' '
           << detail::number(source_elevation_m) << ' '
           << detail::number(outlet_elevation_m) << ' '
           << detail::number(flow_distance_m);
    return output.str();
}

core::Result<void> HydroRunoffCatchment::validate() const {
    if (!detail::safe_text(stable_id, kMaxIdentityBytes)) {
        return core::Result<void>::failure(detail::validation(
            "runoff catchment identity, source, area, or partition coefficients are invalid"));
    }
    if (auto result = source.validate(); !result) {
        return result;
    }
    if (!detail::safe_text(terrain_identity, HydroTerrainRunoffEvidence::kMaxTerrainIdentityBytes) ||
        terrain_revision.value() == 0U || terrain_digest.is_zero() ||
        boundary_edge_id == 0U ||
        !finite_non_negative_bounded(catchment_area_m2, kMaxCatchmentAreaM2) ||
        catchment_area_m2 <= 0.0 || !std::isfinite(runoff_coefficient) ||
        runoff_coefficient < 0.0 || runoff_coefficient > 1.0 ||
        !std::isfinite(infiltration_fraction) || infiltration_fraction < 0.0 ||
        infiltration_fraction > 1.0) {
        return core::Result<void>::failure(detail::validation(
            "runoff catchment identity, source, area, or partition coefficients are invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroRunoffCatchment::canonical() const {
    std::ostringstream output;
    output << "CATCHMENT " << std::quoted(stable_id) << '\n'
           << source.canonical() << '\n'
           << "TERRAIN_BINDING " << std::quoted(terrain_identity) << ' '
           << terrain_revision.value() << ' ' << terrain_digest.hex() << '\n'
           << "BOUNDARY_EDGE " << boundary_edge_id << '\n'
           << "AREA " << detail::number(catchment_area_m2) << '\n'
           << "RUNOFF_COEFFICIENT " << detail::number(runoff_coefficient) << '\n'
           << "INFILTRATION_FRACTION " << detail::number(infiltration_fraction);
    return output.str();
}

core::Result<void> HydroRunoffInput::validate() const {
    if (!finite_non_negative_bounded(
            precipitation_rate_m_per_s, kMaxPrecipitationRateMPerS) ||
        !std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0 ||
        elapsed_seconds > kMaxElapsedSeconds) {
        return core::Result<void>::failure(detail::validation(
            "runoff input precipitation or interval is invalid"));
    }
    return terrain.validate();
}

core::Result<void> HydroRunoffPlan::validate() const {
    if (auto result = catchment.validate(); !result) return result;
    if (auto result = terrain.validate(); !result) return result;
    if (auto result = validate_terrain_binding(catchment, terrain); !result) return result;
    if (terrain.source_tile_identity != terrain.outlet_tile_identity) {
        return core::Result<void>::failure(detail::stale(
            "runoff terrain evidence crosses a tile version boundary"));
    }
    if (!finite_non_negative_bounded(
            precipitation_rate_m_per_s, HydroRunoffInput::kMaxPrecipitationRateMPerS) ||
        !std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0 ||
        elapsed_seconds > HydroRunoffInput::kMaxElapsedSeconds ||
        !std::isfinite(slope) || slope <= 0.0 || slope > kMaxSlope ||
        !std::isfinite(slope_capture_fraction) || slope_capture_fraction < 0.0 ||
        slope_capture_fraction > 1.0 ||
        !finite_non_negative_bounded(gross_precipitation_volume_m3, kMaxVolumeM3) ||
        !finite_non_negative_bounded(infiltrated_volume_m3, kMaxVolumeM3) ||
        !finite_non_negative_bounded(runoff_volume_m3, kMaxVolumeM3) ||
        !std::isfinite(conservation_residual_m3) ||
        std::abs(conservation_residual_m3) > 1.0e-7) {
        return core::Result<void>::failure(detail::validation(
            "runoff plan contains an invalid or unbounded derived value"));
    }

    const double height_delta = terrain.source_elevation_m - terrain.outlet_elevation_m;
    if (!std::isfinite(height_delta) || height_delta <= 0.0 ||
        !close_enough(slope, height_delta / terrain.flow_distance_m)) {
        return core::Result<void>::failure(detail::validation(
            "runoff plan slope is not bound to downhill terrain evidence"));
    }
    const double expected_capture = std::clamp(slope / (1.0 + slope), 0.0, 1.0);
    if (!close_enough(slope_capture_fraction, expected_capture)) {
        return core::Result<void>::failure(detail::validation(
            "runoff plan slope capture is not derived from its slope"));
    }
    const double expected_gross = precipitation_rate_m_per_s *
        catchment.catchment_area_m2 * elapsed_seconds;
    const double expected_runoff = expected_gross * catchment.runoff_coefficient *
        (1.0 - catchment.infiltration_fraction) * slope_capture_fraction;
    const double expected_infiltrated = expected_gross - expected_runoff;
    if (!close_enough(gross_precipitation_volume_m3, expected_gross) ||
        !close_enough(runoff_volume_m3, expected_runoff) ||
        !close_enough(infiltrated_volume_m3, expected_infiltrated) ||
        !close_enough(
            conservation_residual_m3,
            (infiltrated_volume_m3 + runoff_volume_m3) - gross_precipitation_volume_m3)) {
        return core::Result<void>::failure(detail::validation(
            "runoff plan partition does not close against its inputs"));
    }
    return core::Result<void>::success();
}

std::string HydroRunoffPlan::canonical() const {
    std::ostringstream output;
    output << kRunoffMagic << ' ' << kSchemaVersion << '\n'
           << catchment.canonical() << '\n'
           << terrain.canonical() << '\n'
           << "INPUT " << detail::number(precipitation_rate_m_per_s) << ' '
           << detail::number(elapsed_seconds) << '\n'
           << "RESULT " << detail::number(slope) << ' '
           << detail::number(slope_capture_fraction) << ' '
           << detail::number(gross_precipitation_volume_m3) << ' '
           << detail::number(infiltrated_volume_m3) << ' '
           << detail::number(runoff_volume_m3) << ' '
           << detail::number(conservation_residual_m3) << '\n'
           << "END\n";
    return output.str();
}

std::string HydroRunoffPlan::serialize() const { return canonical(); }

core::Result<assets::Sha256Digest> HydroRunoffPlan::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(canonical()));
}

core::Result<HydroRunoffPlan> HydroRunoffPlan::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<HydroRunoffPlan>::failure(detail::validation(
            "hydro runoff plan document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    HydroRunoffPlan plan;
    if (!(input >> magic >> version) || magic != kRunoffMagic) {
        return core::Result<HydroRunoffPlan>::failure(detail::parse_error(
            "hydro runoff plan header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<HydroRunoffPlan>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported hydro runoff plan version"));
    }
    if (!read_catchment(input, plan.catchment) ||
        !detail::require_token(input, "TERRAIN_SAMPLE") ||
        !(input >> std::quoted(plan.terrain.terrain_identity)) ||
        !detail::read_revision(input, plan.terrain.terrain_revision) ||
        !detail::read_digest(input, plan.terrain.terrain_digest) ||
        !(input >> std::quoted(plan.terrain.source_tile_identity)) ||
        !(input >> std::quoted(plan.terrain.outlet_tile_identity)) ||
        !detail::read_double(input, plan.terrain.source_elevation_m) ||
        !detail::read_double(input, plan.terrain.outlet_elevation_m) ||
        !detail::read_double(input, plan.terrain.flow_distance_m) ||
        !detail::require_token(input, "INPUT") ||
        !detail::read_double(input, plan.precipitation_rate_m_per_s) ||
        !detail::read_double(input, plan.elapsed_seconds) ||
        !detail::require_token(input, "RESULT") ||
        !detail::read_double(input, plan.slope) ||
        !detail::read_double(input, plan.slope_capture_fraction) ||
        !detail::read_double(input, plan.gross_precipitation_volume_m3) ||
        !detail::read_double(input, plan.infiltrated_volume_m3) ||
        !detail::read_double(input, plan.runoff_volume_m3) ||
        !detail::read_double(input, plan.conservation_residual_m3) ||
        !detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<HydroRunoffPlan>::failure(detail::parse_error(
            "hydro runoff plan records are malformed or incomplete"));
    }
    if (auto result = plan.validate(); !result) {
        return core::Result<HydroRunoffPlan>::failure(result.error());
    }
    if (plan.serialize() != text) {
        return core::Result<HydroRunoffPlan>::failure(detail::validation(
            "hydro runoff plan is not in canonical form"));
    }
    return core::Result<HydroRunoffPlan>::success(std::move(plan));
}

core::Result<HydroBoundaryFlux> HydroRunoffPlan::boundary_flux_for(
    const HydroGraph& graph,
    double delta_seconds) const {
    if (auto result = validate(); !result) return core::Result<HydroBoundaryFlux>::failure(result.error());
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0 ||
        !close_enough(delta_seconds, elapsed_seconds)) {
        return core::Result<HydroBoundaryFlux>::failure(detail::invalid(
            "runoff plan interval does not match graph step interval"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<HydroBoundaryFlux>::failure(result.error());
    }
    const auto& domain = graph.domain();
    if (domain.stable_id != catchment.source.domain_id ||
        domain.source_revision != catchment.source.source_revision ||
        domain.source_digest != catchment.source.source_digest) {
        return core::Result<HydroBoundaryFlux>::failure(detail::stale(
            "runoff plan is stale for the hydro graph source"));
    }
    const auto edge = graph.edges().find(catchment.boundary_edge_id);
    if (edge == graph.edges().end()) {
        return core::Result<HydroBoundaryFlux>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "runoff plan boundary edge is missing"));
    }
    if (edge->second.kind != HydroEdgeKind::boundary) {
        return core::Result<HydroBoundaryFlux>::failure(detail::validation(
            "runoff plan boundary edge is not a boundary edge"));
    }
    if (!edge->second.enabled && runoff_volume_m3 != 0.0) {
        return core::Result<HydroBoundaryFlux>::failure(detail::validation(
            "disabled runoff boundary edge cannot carry nonzero flux"));
    }
    const double capacity = edge->second.capacity_m3_per_s * delta_seconds;
    if (!std::isfinite(capacity) || runoff_volume_m3 > capacity +
            1.0e-9 * std::max(1.0, capacity)) {
        return core::Result<HydroBoundaryFlux>::failure(detail::validation(
            "runoff plan exceeds the current graph boundary capacity"));
    }
    return core::Result<HydroBoundaryFlux>::success(
        HydroBoundaryFlux{catchment.boundary_edge_id, runoff_volume_m3});
}

core::Result<HydroRunoffPlan> plan_runoff(
    const HydroRunoffCatchment& catchment,
    const HydroRunoffInput& input) {
    if (auto result = catchment.validate(); !result) {
        return core::Result<HydroRunoffPlan>::failure(result.error());
    }
    if (auto result = input.validate(); !result) {
        return core::Result<HydroRunoffPlan>::failure(result.error());
    }
    if (auto result = validate_terrain_binding(catchment, input.terrain); !result) {
        return core::Result<HydroRunoffPlan>::failure(result.error());
    }
    if (input.terrain.source_tile_identity != input.terrain.outlet_tile_identity) {
        return core::Result<HydroRunoffPlan>::failure(detail::stale(
            "runoff step crossed a terrain tile version boundary"));
    }
    const double height_delta = input.terrain.source_elevation_m -
        input.terrain.outlet_elevation_m;
    if (!std::isfinite(height_delta) || height_delta <= 0.0) {
        return core::Result<HydroRunoffPlan>::failure(detail::invalid(
            "runoff outlet is not downhill from the source"));
    }
    const double slope = height_delta / input.terrain.flow_distance_m;
    const double slope_capture_fraction = std::clamp(slope / (1.0 + slope), 0.0, 1.0);
    const double gross = input.precipitation_rate_m_per_s *
        catchment.catchment_area_m2 * input.elapsed_seconds;
    const double runoff = gross * catchment.runoff_coefficient *
        (1.0 - catchment.infiltration_fraction) * slope_capture_fraction;
    const double infiltrated = gross - runoff;
    const double residual = (infiltrated + runoff) - gross;
    if (!finite_non_negative_bounded(gross, HydroRunoffPlan::kMaxVolumeM3) ||
        !finite_non_negative_bounded(runoff, HydroRunoffPlan::kMaxVolumeM3) ||
        !finite_non_negative_bounded(infiltrated, HydroRunoffPlan::kMaxVolumeM3)) {
        return core::Result<HydroRunoffPlan>::failure(detail::validation(
            "runoff partition produced an unbounded volume"));
    }

    HydroRunoffPlan plan{
        .catchment = catchment,
        .terrain = input.terrain,
        .precipitation_rate_m_per_s = input.precipitation_rate_m_per_s,
        .elapsed_seconds = input.elapsed_seconds,
        .slope = slope,
        .slope_capture_fraction = slope_capture_fraction,
        .gross_precipitation_volume_m3 = gross,
        .infiltrated_volume_m3 = infiltrated,
        .runoff_volume_m3 = runoff,
        .conservation_residual_m3 = residual,
    };
    if (auto result = plan.validate(); !result) {
        return core::Result<HydroRunoffPlan>::failure(result.error());
    }
    return core::Result<HydroRunoffPlan>::success(std::move(plan));
}

} // namespace carto::hydro
