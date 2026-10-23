#include <carto/hydro/graph.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace carto::hydro {
namespace {

constexpr std::string_view kGraphMagic = "CARTOGRAPHER_HYDRO_GRAPH";
constexpr std::string_view kStateMagic = "CARTOGRAPHER_HYDRO_STATE";
constexpr double kMaxAreaM2 = 1.0e15;
constexpr double kMaxDepthM = 1.0e9;
constexpr double kMaxCapacityM3PerSecond = 1.0e15;
constexpr double kMaxRoughness = 1.0e9;
constexpr double kMaxVolumeM3 = 1.0e24;
constexpr double kMaxDeltaSeconds = 86'400.0;

bool valid_edge_kind(HydroEdgeKind kind) noexcept {
    switch (kind) {
    case HydroEdgeKind::channel:
    case HydroEdgeKind::culvert:
    case HydroEdgeKind::pipe:
    case HydroEdgeKind::pump:
    case HydroEdgeKind::gate:
    case HydroEdgeKind::breach:
    case HydroEdgeKind::overflow:
    case HydroEdgeKind::boundary:
        return true;
    }
    return false;
}

bool parse_edge_kind(std::string_view value, HydroEdgeKind& kind) noexcept {
    for (const HydroEdgeKind candidate : {
             HydroEdgeKind::channel, HydroEdgeKind::culvert, HydroEdgeKind::pipe,
             HydroEdgeKind::pump, HydroEdgeKind::gate, HydroEdgeKind::breach,
             HydroEdgeKind::overflow, HydroEdgeKind::boundary}) {
        if (hydro_edge_kind_name(candidate) == value) {
            kind = candidate;
            return true;
        }
    }
    return false;
}

bool close_enough(double left, double right) noexcept {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-9 * scale;
}

core::Result<void> check_source_binding(
    const HydroDomain& domain,
    const HydroState& state) {
    if (state.source_revision != domain.source_revision ||
        state.source_digest != domain.source_digest) {
        return core::Result<void>::failure(detail::stale(
            "hydro state is stale for the graph source revision or digest"));
    }
    return core::Result<void>::success();
}

} // namespace

std::string_view hydro_edge_kind_name(HydroEdgeKind kind) noexcept {
    switch (kind) {
    case HydroEdgeKind::channel: return "channel";
    case HydroEdgeKind::culvert: return "culvert";
    case HydroEdgeKind::pipe: return "pipe";
    case HydroEdgeKind::pump: return "pump";
    case HydroEdgeKind::gate: return "gate";
    case HydroEdgeKind::breach: return "breach";
    case HydroEdgeKind::overflow: return "overflow";
    case HydroEdgeKind::boundary: return "boundary";
    }
    return "unknown";
}

core::Result<void> HydroCell::validate() const {
    if (id == 0U || !detail::finite_bounded(area_m2, kMaxAreaM2) ||
        area_m2 <= 0.0 || !std::isfinite(bed_elevation_m) ||
        !detail::finite_bounded(initial_water_depth_m, kMaxDepthM)) {
        return core::Result<void>::failure(detail::validation(
            "hydro cell identity, area, bed, or initial depth is invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroCell::canonical() const {
    std::ostringstream output;
    output << "CELL " << id << ' ' << detail::number(area_m2) << ' '
           << detail::number(bed_elevation_m) << ' '
           << detail::number(initial_water_depth_m);
    return output.str();
}

core::Result<void> HydroEdge::validate() const {
    const bool has_target = to_cell.has_value();
    if (id == 0U || from_cell == 0U ||
        (has_target && (to_cell.value() == 0U || to_cell.value() == from_cell)) ||
        (kind == HydroEdgeKind::boundary && has_target) ||
        (kind != HydroEdgeKind::boundary && !has_target) ||
        !valid_edge_kind(kind) ||
        !detail::finite_bounded(capacity_m3_per_s, kMaxCapacityM3PerSecond) ||
        capacity_m3_per_s <= 0.0 ||
        !detail::finite_bounded(roughness, kMaxRoughness) ||
        !std::isfinite(sill_elevation_m)) {
        return core::Result<void>::failure(detail::validation(
            "hydro edge identity, endpoints, kind, capacity, or coefficient is invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroEdge::canonical() const {
    std::ostringstream output;
    output << "EDGE " << id << ' ' << from_cell << ' '
           << (to_cell.has_value() ? std::to_string(to_cell.value()) : "-") << ' '
           << hydro_edge_kind_name(kind) << ' '
           << detail::number(capacity_m3_per_s) << ' '
           << detail::number(roughness) << ' '
           << detail::number(sill_elevation_m) << ' ' << (enabled ? 1 : 0);
    return output.str();
}

core::Result<void> HydroBoundaryFlux::validate() const {
    if (edge_id == 0U || !std::isfinite(volume_m3) ||
        std::abs(volume_m3) > kMaxVolumeM3) {
        return core::Result<void>::failure(detail::validation(
            "hydro boundary flux identity or volume is invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroBoundaryFlux::canonical() const {
    return "FLUX " + std::to_string(edge_id) + ' ' + detail::number(volume_m3);
}

core::Result<void> HydroCellState::validate() const {
    if (cell_id == 0U || !detail::finite_bounded(water_depth_m, kMaxDepthM) ||
        !velocity_mps.finite() || velocity_mps.length() > 1.0e9) {
        return core::Result<void>::failure(detail::validation(
            "hydro cell state identity, depth, or velocity is invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroCellState::canonical() const {
    std::ostringstream output;
    output << "STATE " << cell_id << ' ' << detail::number(water_depth_m) << ' '
           << detail::number(velocity_mps.x) << ' '
           << detail::number(velocity_mps.y) << ' '
           << detail::number(velocity_mps.z);
    return output.str();
}

core::Result<void> HydroState::validate() const {
    if (source_digest.is_zero() || cells.size() > kMaxCells) {
        return core::Result<void>::failure(detail::validation(
            "hydro state source binding or bounded cell count is invalid"));
    }
    for (const auto& [identity, state] : cells) {
        if (identity != state.cell_id) {
            return core::Result<void>::failure(detail::validation(
                "hydro state map identity does not match its cell state"));
        }
        if (auto result = state.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string HydroState::canonical() const {
    std::ostringstream output;
    output << kStateMagic << ' ' << kSchemaVersion << '\n'
           << "SOURCE_REVISION " << source_revision.value() << '\n'
           << "SOURCE_DIGEST " << source_digest.hex() << '\n'
           << "CELLS " << cells.size() << '\n';
    for (const auto& [identity, state] : cells) {
        static_cast<void>(identity);
        output << state.canonical() << '\n';
    }
    output << "END\n";
    return output.str();
}

std::string HydroState::serialize() const { return canonical(); }

core::Result<assets::Sha256Digest> HydroState::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(canonical()));
}

core::Result<HydroState> HydroState::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<HydroState>::failure(detail::validation(
            "hydro state document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    HydroState state;
    std::size_t count = 0U;
    if (!(input >> magic >> version) || magic != kStateMagic) {
        return core::Result<HydroState>::failure(detail::parse_error(
            "hydro state header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<HydroState>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported hydro state version"));
    }
    if (!detail::require_token(input, "SOURCE_REVISION") ||
        !detail::read_revision(input, state.source_revision) ||
        !detail::require_token(input, "SOURCE_DIGEST") ||
        !detail::read_digest(input, state.source_digest) ||
        !detail::require_token(input, "CELLS") ||
        !detail::read_count(input, kMaxCells, count)) {
        return core::Result<HydroState>::failure(detail::parse_error(
            "hydro state source binding or cell count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (!detail::require_token(input, "STATE")) {
            return core::Result<HydroState>::failure(detail::parse_error(
                "hydro state cell record is missing"));
        }
        HydroCellState cell;
        if (!detail::read_unsigned(input, cell.cell_id) ||
            !detail::read_double(input, cell.water_depth_m) ||
            !detail::read_double(input, cell.velocity_mps.x) ||
            !detail::read_double(input, cell.velocity_mps.y) ||
            !detail::read_double(input, cell.velocity_mps.z) ||
            !state.cells.emplace(cell.cell_id, cell).second) {
            return core::Result<HydroState>::failure(detail::parse_error(
                "hydro state cell record is malformed or duplicated"));
        }
    }
    if (!detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<HydroState>::failure(detail::parse_error(
            "hydro state has trailing or missing terminator data"));
    }
    if (auto result = state.validate(); !result) {
        return core::Result<HydroState>::failure(result.error());
    }
    if (state.serialize() != text) {
        return core::Result<HydroState>::failure(detail::validation(
            "hydro state is not in canonical form"));
    }
    return core::Result<HydroState>::success(std::move(state));
}

core::Result<void> HydroVolumeLedger::validate() const {
    if (!detail::finite_bounded(initial_volume_m3, kMaxVolumeM3) ||
        !detail::finite_bounded(boundary_inflow_m3, kMaxVolumeM3) ||
        !detail::finite_bounded(boundary_outflow_m3, kMaxVolumeM3) ||
        !detail::finite_bounded(final_volume_m3, kMaxVolumeM3) ||
        !std::isfinite(internal_transfer_m3) ||
        std::abs(internal_transfer_m3) > 1.0e-7 ||
        !close_enough(
            final_volume_m3,
            initial_volume_m3 + boundary_inflow_m3 - boundary_outflow_m3)) {
        return core::Result<void>::failure(detail::validation(
            "hydro volume ledger does not conserve water"));
    }
    return core::Result<void>::success();
}

std::string HydroVolumeLedger::canonical() const {
    std::ostringstream output;
    output << "LEDGER " << detail::number(initial_volume_m3) << ' '
           << detail::number(internal_transfer_m3) << ' '
           << detail::number(boundary_inflow_m3) << ' '
           << detail::number(boundary_outflow_m3) << ' '
           << detail::number(final_volume_m3);
    return output.str();
}

core::Result<void> HydroStepResult::validate() const {
    if (auto result = state.validate(); !result) return result;
    if (auto result = ledger.validate(); !result) return result;
    if (graph_digest.is_zero() || receipt_digest.is_zero()) {
        return core::Result<void>::failure(detail::validation(
            "hydro step receipt is missing its graph or receipt digest"));
    }
    if (detail::digest_of(canonical_without_digest()) != receipt_digest) {
        return core::Result<void>::failure(detail::stale(
            "hydro step receipt digest does not match its content"));
    }
    return core::Result<void>::success();
}

std::string HydroStepResult::canonical_without_digest() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_HYDRO_STEP_RESULT 1\n"
           << "GRAPH_DIGEST " << graph_digest.hex() << '\n'
           << "STATE " << std::quoted(state.serialize()) << '\n'
           << ledger.canonical() << '\n'
           << "END\n";
    return output.str();
}

core::Result<void> HydroStepResult::refresh_digest() {
    if (auto result = state.validate(); !result) return result;
    if (auto result = ledger.validate(); !result) return result;
    if (graph_digest.is_zero()) {
        return core::Result<void>::failure(detail::validation(
            "hydro step result requires a graph digest"));
    }
    receipt_digest = detail::digest_of(canonical_without_digest());
    return core::Result<void>::success();
}

HydroGraph::HydroGraph(HydroDomain domain) : domain_(std::move(domain)) {}

core::Result<void> HydroGraph::insert_cell(HydroCell cell) {
    if (auto result = cell.validate(); !result) return result;
    if (cells_.size() >= kMaxCells) {
        return core::Result<void>::failure(detail::validation(
            "hydro graph has reached its bounded cell capacity"));
    }
    if (!cells_.emplace(cell.id, std::move(cell)).second) {
        return core::Result<void>::failure(detail::invalid(
            "hydro cell identity is duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<void> HydroGraph::insert_edge(HydroEdge edge) {
    if (auto result = edge.validate(); !result) return result;
    if (!cells_.contains(edge.from_cell) ||
        (edge.to_cell.has_value() && !cells_.contains(edge.to_cell.value()))) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "hydro edge references a missing cell"));
    }
    if (edges_.size() >= kMaxEdges) {
        return core::Result<void>::failure(detail::validation(
            "hydro graph has reached its bounded edge capacity"));
    }
    if (!edges_.emplace(edge.id, std::move(edge)).second) {
        return core::Result<void>::failure(detail::invalid(
            "hydro edge identity is duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<void> HydroGraph::validate() const {
    if (auto result = domain_.validate(); !result) return result;
    if (cells_.size() > kMaxCells || edges_.size() > kMaxEdges) {
        return core::Result<void>::failure(detail::validation(
            "hydro graph exceeds its bounded capacity"));
    }
    for (const auto& [identity, cell] : cells_) {
        if (identity != cell.id) {
            return core::Result<void>::failure(detail::validation(
                "hydro graph cell map identity does not match its record"));
        }
        if (auto result = cell.validate(); !result) return result;
    }
    for (const auto& [identity, edge] : edges_) {
        if (identity != edge.id) {
            return core::Result<void>::failure(detail::validation(
                "hydro graph edge map identity does not match its record"));
        }
        if (auto result = edge.validate(); !result) return result;
        if (!cells_.contains(edge.from_cell) ||
            (edge.to_cell.has_value() && !cells_.contains(edge.to_cell.value()))) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found, "hydro graph edge references a missing cell"));
        }
    }
    return core::Result<void>::success();
}

core::Result<HydroState> HydroGraph::initial_state() const {
    if (auto result = validate(); !result) {
        return core::Result<HydroState>::failure(result.error());
    }
    HydroState state;
    state.source_revision = domain_.source_revision;
    state.source_digest = domain_.source_digest;
    for (const auto& [id, cell] : cells_) {
        state.cells.emplace(id, HydroCellState{id, cell.initial_water_depth_m, {}});
    }
    return core::Result<HydroState>::success(std::move(state));
}

core::Result<double> HydroGraph::total_volume_m3(const HydroState& state) const {
    if (auto result = validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (auto result = state.validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (auto result = check_source_binding(domain_, state); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (state.cells.size() != cells_.size()) {
        return core::Result<double>::failure(detail::stale(
            "hydro state cell set does not match the graph"));
    }
    double total = 0.0;
    for (const auto& [id, cell] : cells_) {
        const auto state_cell = state.cells.find(id);
        if (state_cell == state.cells.end()) {
            return core::Result<double>::failure(detail::stale(
                "hydro state is missing a graph cell"));
        }
        total += cell.area_m2 * state_cell->second.water_depth_m;
        if (!detail::finite_bounded(total, kMaxVolumeM3)) {
            return core::Result<double>::failure(detail::validation(
                "hydro volume exceeds its bounded range"));
        }
    }
    return core::Result<double>::success(total);
}

core::Result<HydroStepResult> HydroGraph::advance(
    const HydroState& state,
    double delta_seconds,
    std::span<const HydroBoundaryFlux> boundary_fluxes) const {
    if (auto result = validate(); !result) {
        return core::Result<HydroStepResult>::failure(result.error());
    }
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0 ||
        delta_seconds > kMaxDeltaSeconds) {
        return core::Result<HydroStepResult>::failure(detail::invalid(
            "hydro step duration must be finite, positive, and bounded"));
    }
    if (auto result = state.validate(); !result) {
        return core::Result<HydroStepResult>::failure(result.error());
    }
    if (auto result = check_source_binding(domain_, state); !result) {
        return core::Result<HydroStepResult>::failure(result.error());
    }
    if (state.cells.size() != cells_.size()) {
        return core::Result<HydroStepResult>::failure(detail::stale(
            "hydro step state cell set does not match the graph"));
    }

    std::map<std::uint64_t, double> volumes;
    std::map<std::uint64_t, core::Vec3d> velocities;
    double initial_volume = 0.0;
    for (const auto& [id, cell] : cells_) {
        const auto state_cell = state.cells.find(id);
        if (state_cell == state.cells.end()) {
            return core::Result<HydroStepResult>::failure(detail::stale(
                "hydro step state is missing a graph cell"));
        }
        volumes.emplace(id, cell.area_m2 * state_cell->second.water_depth_m);
        velocities.emplace(id, state_cell->second.velocity_mps);
        initial_volume += volumes.at(id);
    }

    std::map<std::uint64_t, double> boundary_by_edge;
    for (const HydroBoundaryFlux& flux : boundary_fluxes) {
        if (auto result = flux.validate(); !result) {
            return core::Result<HydroStepResult>::failure(result.error());
        }
        if (!boundary_by_edge.emplace(flux.edge_id, flux.volume_m3).second) {
            return core::Result<HydroStepResult>::failure(detail::invalid(
                "duplicate hydro boundary flux edge identity"));
        }
        const auto edge = edges_.find(flux.edge_id);
        if (edge == edges_.end() || edge->second.kind != HydroEdgeKind::boundary) {
            return core::Result<HydroStepResult>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "boundary flux does not reference a boundary edge"));
        }
        if (!edge->second.enabled && flux.volume_m3 != 0.0) {
            return core::Result<HydroStepResult>::failure(detail::validation(
                "disabled hydro boundary edge cannot carry nonzero flux"));
        }
        const double capacity = edge->second.capacity_m3_per_s * delta_seconds;
        if (std::abs(flux.volume_m3) > capacity + 1.0e-9 * std::max(1.0, capacity)) {
            return core::Result<HydroStepResult>::failure(detail::validation(
                "hydro boundary flux exceeds the edge capacity"));
        }
        double& volume = volumes.at(edge->second.from_cell);
        if (flux.volume_m3 < 0.0 && -flux.volume_m3 > volume + 1.0e-9) {
            return core::Result<HydroStepResult>::failure(detail::validation(
                "hydro boundary outflow would create negative water volume"));
        }
        volume += flux.volume_m3;
        if (volume < 0.0 && volume > -1.0e-9) volume = 0.0;
    }

    double boundary_inflow = 0.0;
    double boundary_outflow = 0.0;
    for (const auto& [edge_id, flux] : boundary_by_edge) {
        static_cast<void>(edge_id);
        if (flux >= 0.0) boundary_inflow += flux;
        else boundary_outflow -= flux;
    }

    // Stable edge order makes the reference result reproducible. This is a
    // deliberately simple positive-head transfer, not a claim of CFD fidelity.
    for (const auto& [id, edge] : edges_) {
        static_cast<void>(id);
        if (!edge.enabled || edge.kind == HydroEdgeKind::boundary) continue;
        const auto target = edge.to_cell.value();
        const HydroCell& from_cell = cells_.at(edge.from_cell);
        const HydroCell& to_cell = cells_.at(target);
        const double from_level = from_cell.bed_elevation_m +
            volumes.at(edge.from_cell) / from_cell.area_m2;
        const double to_level = to_cell.bed_elevation_m +
            volumes.at(target) / to_cell.area_m2;
        const double head = from_level - to_level - edge.sill_elevation_m;
        if (head <= 0.0) continue;
        const double normalized_head = std::min(1.0, head / (1.0 + edge.roughness));
        const double transfer = std::min(
            volumes.at(edge.from_cell),
            edge.capacity_m3_per_s * normalized_head * delta_seconds);
        if (transfer <= 0.0) continue;
        volumes.at(edge.from_cell) -= transfer;
        volumes.at(target) += transfer;
    }

    HydroState next;
    next.source_revision = state.source_revision;
    next.source_digest = state.source_digest;
    for (const auto& [id, cell] : cells_) {
        const double depth = volumes.at(id) / cell.area_m2;
        next.cells.emplace(id, HydroCellState{id, depth, velocities.at(id)});
    }
    const auto final_volume = total_volume_m3(next);
    if (!final_volume) {
        return core::Result<HydroStepResult>::failure(final_volume.error());
    }
    HydroStepResult result;
    result.state = std::move(next);
    result.ledger = HydroVolumeLedger{
        initial_volume, 0.0, boundary_inflow, boundary_outflow,
        final_volume.value()};
    const auto graph_digest = canonical_digest();
    if (!graph_digest) {
        return core::Result<HydroStepResult>::failure(graph_digest.error());
    }
    result.graph_digest = graph_digest.value();
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<HydroStepResult>::failure(refreshed.error());
    }
    return core::Result<HydroStepResult>::success(std::move(result));
}

std::string HydroGraph::serialize() const {
    std::ostringstream output;
    output << kGraphMagic << ' ' << kSchemaVersion << '\n'
           << "DOMAIN " << std::quoted(domain_.serialize()) << '\n'
           << "CELLS " << cells_.size() << '\n';
    for (const auto& [identity, cell] : cells_) {
        static_cast<void>(identity);
        output << cell.canonical() << '\n';
    }
    output << "EDGES " << edges_.size() << '\n';
    for (const auto& [identity, edge] : edges_) {
        static_cast<void>(identity);
        output << edge.canonical() << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> HydroGraph::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<HydroGraph> HydroGraph::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<HydroGraph>::failure(detail::validation(
            "hydro graph document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string encoded_domain;
    std::size_t cell_count = 0U;
    std::size_t edge_count = 0U;
    if (!(input >> magic >> version) || magic != kGraphMagic) {
        return core::Result<HydroGraph>::failure(detail::parse_error(
            "hydro graph header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<HydroGraph>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported hydro graph version"));
    }
    if (!detail::require_token(input, "DOMAIN") ||
        !(input >> std::quoted(encoded_domain))) {
        return core::Result<HydroGraph>::failure(detail::parse_error(
            "hydro graph domain record is malformed"));
    }
    const auto domain = HydroDomain::deserialize(encoded_domain);
    if (!domain) return core::Result<HydroGraph>::failure(domain.error());
    HydroGraph graph{domain.value()};
    if (!detail::require_token(input, "CELLS") ||
        !detail::read_count(input, kMaxCells, cell_count)) {
        return core::Result<HydroGraph>::failure(detail::parse_error(
            "hydro graph cell count is malformed"));
    }
    for (std::size_t index = 0U; index < cell_count; ++index) {
        if (!detail::require_token(input, "CELL")) {
            return core::Result<HydroGraph>::failure(detail::parse_error(
                "hydro graph cell record is missing"));
        }
        HydroCell cell;
        if (!detail::read_unsigned(input, cell.id) ||
            !detail::read_double(input, cell.area_m2) ||
            !detail::read_double(input, cell.bed_elevation_m) ||
            !detail::read_double(input, cell.initial_water_depth_m) ||
            !graph.insert_cell(std::move(cell))) {
            return core::Result<HydroGraph>::failure(detail::parse_error(
                "hydro graph cell record is malformed or rejected"));
        }
    }
    if (!detail::require_token(input, "EDGES") ||
        !detail::read_count(input, kMaxEdges, edge_count)) {
        return core::Result<HydroGraph>::failure(detail::parse_error(
            "hydro graph edge count is malformed"));
    }
    for (std::size_t index = 0U; index < edge_count; ++index) {
        if (!detail::require_token(input, "EDGE")) {
            return core::Result<HydroGraph>::failure(detail::parse_error(
                "hydro graph edge record is missing"));
        }
        HydroEdge edge;
        std::uint64_t target = 0U;
        std::string target_token;
        std::string kind;
        if (!detail::read_unsigned(input, edge.id) ||
            !detail::read_unsigned(input, edge.from_cell) ||
            !(input >> target_token)) {
            return core::Result<HydroGraph>::failure(detail::parse_error(
                "hydro graph edge record is malformed"));
        }
        if (target_token != "-") {
            const auto parsed = std::from_chars(
                target_token.data(), target_token.data() + target_token.size(), target);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != target_token.data() + target_token.size()) {
                return core::Result<HydroGraph>::failure(detail::parse_error(
                    "hydro graph edge target is malformed"));
            }
            edge.to_cell = target;
        }
        if (!(input >> kind) || !parse_edge_kind(kind, edge.kind) ||
            !detail::read_double(input, edge.capacity_m3_per_s) ||
            !detail::read_double(input, edge.roughness) ||
            !detail::read_double(input, edge.sill_elevation_m) ||
            !detail::read_bool(input, edge.enabled) ||
            !graph.insert_edge(std::move(edge))) {
            return core::Result<HydroGraph>::failure(detail::parse_error(
                "hydro graph edge record is malformed or rejected"));
        }
    }
    if (!detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<HydroGraph>::failure(detail::parse_error(
            "hydro graph has trailing or missing terminator data"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<HydroGraph>::failure(result.error());
    }
    if (graph.serialize() != text) {
        return core::Result<HydroGraph>::failure(detail::validation(
            "hydro graph is not in canonical form"));
    }
    return core::Result<HydroGraph>::success(std::move(graph));
}

} // namespace carto::hydro
