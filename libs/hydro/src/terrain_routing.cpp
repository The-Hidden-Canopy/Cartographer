#include <carto/hydro/terrain_routing.hpp>

#include "detail.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <istream>
#include <sstream>
#include <utility>

namespace carto::hydro {
namespace {

constexpr std::string_view kRoutingMagic = "CARTOGRAPHER_HYDRO_TERRAIN_ROUTING";

bool same_source(const HydroSurfaceSource& left, const HydroSurfaceSource& right) {
    return left.domain_id == right.domain_id &&
        left.source_revision == right.source_revision &&
        left.source_digest == right.source_digest &&
        left.coordinate_frame == right.coordinate_frame &&
        left.provenance == right.provenance;
}

bool close_enough(double left, double right) noexcept {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-9 * scale;
}

std::string_view kind_name(HydroTerrainRouteKind kind) {
    switch (kind) {
    case HydroTerrainRouteKind::downslope: return "DOWNSLOPE";
    case HydroTerrainRouteKind::flat: return "FLAT";
    case HydroTerrainRouteKind::outlet: return "OUTLET";
    case HydroTerrainRouteKind::sink: return "SINK";
    }
    return "INVALID";
}

bool read_kind(std::istream& input, HydroTerrainRouteKind& kind) {
    std::string encoded;
    if (!(input >> encoded)) return false;
    if (encoded == "DOWNSLOPE") kind = HydroTerrainRouteKind::downslope;
    else if (encoded == "FLAT") kind = HydroTerrainRouteKind::flat;
    else if (encoded == "OUTLET") kind = HydroTerrainRouteKind::outlet;
    else if (encoded == "SINK") kind = HydroTerrainRouteKind::sink;
    else return false;
    return true;
}

struct NeighborList final {
    std::array<std::size_t, 8U> values{};
    std::size_t count = 0U;

    [[nodiscard]] const std::size_t* begin() const noexcept { return values.data(); }
    [[nodiscard]] const std::size_t* end() const noexcept {
        return values.data() + count;
    }
};

NeighborList neighbors(std::size_t index, std::uint32_t width, std::uint32_t height) {
    const std::size_t x = index % width;
    const std::size_t y = index / width;
    NeighborList result;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const auto nx = static_cast<std::int64_t>(x) + dx;
            const auto ny = static_cast<std::int64_t>(y) + dy;
            if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
            result.values[result.count++] = static_cast<std::size_t>(ny) * width +
                static_cast<std::size_t>(nx);
        }
    }
    return result;
}

core::Result<void> validate_grid(const HydroTerrainRouting& routing) {
    const std::uint64_t count = static_cast<std::uint64_t>(routing.width) * routing.height;
    const double cell_area = routing.cell_size_m * routing.cell_size_m;
    if (!routing.origin_xy.finite() || !std::isfinite(routing.cell_size_m) ||
        routing.cell_size_m <= 0.0 || routing.width == 0U || routing.height == 0U ||
        count == 0U || count > HydroTerrainRouting::kMaxSamples ||
        routing.cells.size() != count || !std::isfinite(cell_area) || cell_area <= 0.0) {
        return core::Result<void>::failure(detail::validation(
            "terrain routing grid is invalid or exceeds its bounded sample count"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_source_binding(const HydroTerrainRouting& routing) {
    if (auto result = routing.source.validate(); !result) return result;
    if (!detail::safe_text(routing.terrain_identity,
            BathymetryField::kMaxTerrainIdentityBytes) ||
        routing.terrain_revision.value() == 0U || routing.terrain_digest.is_zero() ||
        routing.bathymetry_digest.is_zero()) {
        return core::Result<void>::failure(detail::validation(
            "terrain routing source identity or digest is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<HydroTerrainRouting> build_routing(
    const BathymetryField& bathymetry,
    std::span<const std::uint64_t> requested_outlets) {
    if (auto result = bathymetry.validate(); !result) {
        return core::Result<HydroTerrainRouting>::failure(result.error());
    }
    const auto bathymetry_digest = bathymetry.canonical_digest();
    if (!bathymetry_digest) {
        return core::Result<HydroTerrainRouting>::failure(bathymetry_digest.error());
    }

    HydroTerrainRouting routing;
    routing.source = bathymetry.source;
    routing.terrain_identity = bathymetry.terrain_identity;
    routing.terrain_revision = bathymetry.terrain_revision;
    routing.terrain_digest = bathymetry.terrain_digest;
    routing.bathymetry_digest = bathymetry_digest.value();
    routing.origin_xy = bathymetry.origin_xy;
    routing.cell_size_m = bathymetry.cell_size_m;
    routing.width = bathymetry.width;
    routing.height = bathymetry.height;

    const std::size_t count = bathymetry.bed_elevations_m.size();
    const double cell_area = bathymetry.cell_size_m * bathymetry.cell_size_m;
    if (!std::isfinite(cell_area) || cell_area <= 0.0) {
        return core::Result<HydroTerrainRouting>::failure(detail::validation(
            "terrain routing cell area is not representable"));
    }
    if (requested_outlets.size() > HydroTerrainRouting::kMaxSamples) {
        return core::Result<HydroTerrainRouting>::failure(detail::validation(
            "terrain routing outlet list exceeds its bounded size"));
    }
    routing.declared_outlet_cell_ids.assign(
        requested_outlets.begin(), requested_outlets.end());
    std::sort(routing.declared_outlet_cell_ids.begin(),
        routing.declared_outlet_cell_ids.end());
    if (std::adjacent_find(routing.declared_outlet_cell_ids.begin(),
            routing.declared_outlet_cell_ids.end()) !=
        routing.declared_outlet_cell_ids.end()) {
        return core::Result<HydroTerrainRouting>::failure(detail::invalid(
            "terrain routing outlet identities must be unique"));
    }
    std::vector<bool> is_outlet(count, false);
    for (const std::uint64_t outlet_id : routing.declared_outlet_cell_ids) {
        if (outlet_id == 0U || outlet_id > count) {
            return core::Result<HydroTerrainRouting>::failure(detail::invalid(
                "terrain routing outlet identity is outside the source grid"));
        }
        is_outlet[static_cast<std::size_t>(outlet_id - 1U)] = true;
    }

    routing.cells.resize(count);
    for (std::size_t i = 0U; i < count; ++i) {
        auto& cell = routing.cells[i];
        cell.cell_id = static_cast<std::uint64_t>(i) + 1U;
        cell.bed_elevation_m = bathymetry.bed_elevations_m[i];
        cell.contributing_area_m2 = cell_area;
        if (is_outlet[i]) cell.kind = HydroTerrainRouteKind::outlet;
    }

    // Select a deterministic steepest lower D8 neighbor. Equal slope ties use
    // the lower stable cell ID; boundaries are not implicit outlets.
    for (std::size_t i = 0U; i < count; ++i) {
        if (is_outlet[i]) continue;
        const double elevation = bathymetry.bed_elevations_m[i];
        double best_slope = -1.0;
        std::size_t best_neighbor = count;
        for (const std::size_t candidate : neighbors(i, routing.width, routing.height)) {
            const double drop = elevation - bathymetry.bed_elevations_m[candidate];
            if (drop <= 0.0) continue;
            const std::size_t x = i % routing.width;
            const std::size_t y = i / routing.width;
            const std::size_t nx = candidate % routing.width;
            const std::size_t ny = candidate / routing.width;
            const std::size_t dx = x > nx ? x - nx : nx - x;
            const std::size_t dy = y > ny ? y - ny : ny - y;
            const double distance = routing.cell_size_m *
                std::hypot(static_cast<double>(dx), static_cast<double>(dy));
            const double slope = drop / distance;
            if (!std::isfinite(distance) || distance <= 0.0 || !std::isfinite(slope)) {
                return core::Result<HydroTerrainRouting>::failure(detail::validation(
                    "terrain routing slope is not representable"));
            }
            const std::uint64_t candidate_id = static_cast<std::uint64_t>(candidate) + 1U;
            if (best_neighbor == count || slope > best_slope || (slope == best_slope &&
                    candidate_id < routing.cells[best_neighbor].cell_id)) {
                best_slope = slope;
                best_neighbor = candidate;
            }
        }
        if (best_neighbor != count) {
            routing.cells[i].kind = HydroTerrainRouteKind::downslope;
            routing.cells[i].downstream_cell_id =
                static_cast<std::uint64_t>(best_neighbor) + 1U;
        }
    }

    // Route equal-elevation plateaus by deterministic multi-source BFS from
    // their lower exits/outlets. A closed plateau gets one explicit sink.
    std::vector<bool> component_seen(count, false);
    std::vector<bool> flat_assigned(count, false);
    std::vector<std::size_t> component_queue;
    std::vector<std::size_t> component;
    std::vector<std::size_t> seed_queue;
    std::vector<std::size_t> bfs;
    for (std::size_t start = 0U; start < count; ++start) {
        if (component_seen[start]) continue;
        component_queue.clear();
        component.clear();
        component_queue.push_back(start);
        component_seen[start] = true;
        for (std::size_t cursor = 0U; cursor < component_queue.size(); ++cursor) {
            const std::size_t current = component_queue[cursor];
            component.push_back(current);
            for (const std::size_t adjacent : neighbors(
                    current, routing.width, routing.height)) {
                if (!component_seen[adjacent] &&
                    bathymetry.bed_elevations_m[adjacent] ==
                        bathymetry.bed_elevations_m[current]) {
                    component_seen[adjacent] = true;
                    component_queue.push_back(adjacent);
                }
            }
        }

        seed_queue.clear();
        for (const std::size_t cell_index : component) {
            const auto kind = routing.cells[cell_index].kind;
            if (kind == HydroTerrainRouteKind::outlet ||
                kind == HydroTerrainRouteKind::downslope) {
                seed_queue.push_back(cell_index);
            }
        }
        std::sort(seed_queue.begin(), seed_queue.end());
        if (seed_queue.empty()) {
            const std::size_t sink = *std::min_element(component.begin(), component.end());
            routing.cells[sink].kind = HydroTerrainRouteKind::sink;
            routing.cells[sink].downstream_cell_id = 0U;
            seed_queue.push_back(sink);
        }

        bfs.clear();
        bfs.reserve(component.size());
        for (const std::size_t seed : seed_queue) {
            flat_assigned[seed] = true;
            bfs.push_back(seed);
        }
        for (std::size_t cursor = 0U; cursor < bfs.size(); ++cursor) {
            const std::size_t current = bfs[cursor];
            for (const std::size_t adjacent : neighbors(
                    current, routing.width, routing.height)) {
                if (flat_assigned[adjacent] ||
                    bathymetry.bed_elevations_m[adjacent] !=
                        bathymetry.bed_elevations_m[current]) {
                    continue;
                }
                flat_assigned[adjacent] = true;
                routing.cells[adjacent].kind = HydroTerrainRouteKind::flat;
                routing.cells[adjacent].downstream_cell_id =
                    static_cast<std::uint64_t>(current) + 1U;
                bfs.push_back(adjacent);
            }
        }
    }

    // Accumulate each cell's contributing area once in stable Kahn order.
    std::vector<std::size_t> upstream_count(count, 0U);
    std::vector<double> contributing(count, cell_area);
    for (const auto& cell : routing.cells) {
        if (cell.downstream_cell_id != 0U) {
            ++upstream_count[static_cast<std::size_t>(cell.downstream_cell_id - 1U)];
        }
    }
    std::vector<std::size_t> ready;
    ready.reserve(count);
    for (std::size_t i = 0U; i < count; ++i) {
        if (upstream_count[i] == 0U) ready.push_back(i);
    }
    for (std::size_t cursor = 0U; cursor < ready.size(); ++cursor) {
        const std::size_t current = ready[cursor];
        const std::uint64_t next_id = routing.cells[current].downstream_cell_id;
        if (next_id == 0U) continue;
        const std::size_t next = static_cast<std::size_t>(next_id - 1U);
        contributing[next] += contributing[current];
        if (!std::isfinite(contributing[next])) {
            return core::Result<HydroTerrainRouting>::failure(detail::validation(
                "terrain routing contributing area overflowed"));
        }
        if (--upstream_count[next] == 0U) ready.push_back(next);
    }
    if (ready.size() != count) {
        return core::Result<HydroTerrainRouting>::failure(detail::validation(
            "terrain routing produced a cycle"));
    }
    for (std::size_t i = 0U; i < count; ++i) {
        routing.cells[i].contributing_area_m2 = contributing[i];
    }
    if (auto result = routing.validate(); !result) {
        return core::Result<HydroTerrainRouting>::failure(result.error());
    }
    return core::Result<HydroTerrainRouting>::success(std::move(routing));
}

} // namespace

core::Result<void> HydroTerrainRouting::validate() const {
    if (auto result = validate_source_binding(*this); !result) return result;
    if (auto result = validate_grid(*this); !result) return result;
    if (!std::is_sorted(declared_outlet_cell_ids.begin(),
            declared_outlet_cell_ids.end()) ||
        std::adjacent_find(declared_outlet_cell_ids.begin(),
            declared_outlet_cell_ids.end()) != declared_outlet_cell_ids.end()) {
        return core::Result<void>::failure(detail::validation(
            "terrain routing outlets are not canonical and unique"));
    }

    const std::size_t count = cells.size();
    std::vector<bool> is_outlet(count, false);
    for (const std::uint64_t id : declared_outlet_cell_ids) {
        if (id == 0U || id > count) {
            return core::Result<void>::failure(detail::validation(
                "terrain routing outlet is outside the source grid"));
        }
        is_outlet[static_cast<std::size_t>(id - 1U)] = true;
    }
    const double cell_area = cell_size_m * cell_size_m;
    std::vector<std::size_t> upstream_count(count, 0U);
    std::vector<double> expected_area(count, cell_area);
    for (std::size_t i = 0U; i < count; ++i) {
        const auto& cell = cells[i];
        if (cell.cell_id != static_cast<std::uint64_t>(i) + 1U ||
            !std::isfinite(cell.bed_elevation_m) ||
            !std::isfinite(cell.contributing_area_m2) ||
            cell.contributing_area_m2 < cell_area) {
            return core::Result<void>::failure(detail::validation(
                "terrain routing cell identity, elevation, or area is invalid"));
        }
        if (cell.kind == HydroTerrainRouteKind::outlet) {
            if (!is_outlet[i] || cell.downstream_cell_id != 0U) {
                return core::Result<void>::failure(detail::validation(
                    "terrain routing outlet is undeclared or has a downstream cell"));
            }
        } else if (cell.kind == HydroTerrainRouteKind::sink) {
            if (is_outlet[i] || cell.downstream_cell_id != 0U) {
                return core::Result<void>::failure(detail::validation(
                    "terrain routing sink conflicts with declared outlet state"));
            }
        } else if (cell.kind == HydroTerrainRouteKind::downslope ||
                   cell.kind == HydroTerrainRouteKind::flat) {
            if (cell.downstream_cell_id == 0U ||
                cell.downstream_cell_id > count || cell.downstream_cell_id == cell.cell_id) {
                return core::Result<void>::failure(detail::validation(
                    "terrain routing edge has an invalid downstream identity"));
            }
            const std::size_t downstream =
                static_cast<std::size_t>(cell.downstream_cell_id - 1U);
            const std::size_t x = i % width;
            const std::size_t y = i / width;
            const std::size_t nx = downstream % width;
            const std::size_t ny = downstream / width;
            const std::size_t dx = x > nx ? x - nx : nx - x;
            const std::size_t dy = y > ny ? y - ny : ny - y;
            if (dx > 1U || dy > 1U ||
                (cell.kind == HydroTerrainRouteKind::downslope &&
                    !(cell.bed_elevation_m > cells[downstream].bed_elevation_m)) ||
                (cell.kind == HydroTerrainRouteKind::flat &&
                    cell.bed_elevation_m != cells[downstream].bed_elevation_m)) {
                return core::Result<void>::failure(detail::validation(
                    "terrain routing edge violates its local elevation policy"));
            }
            ++upstream_count[downstream];
        } else {
            return core::Result<void>::failure(detail::validation(
                "terrain routing cell has an unknown route kind"));
        }
    }
    for (std::size_t i = 0U; i < count; ++i) {
        if (is_outlet[i] && cells[i].kind != HydroTerrainRouteKind::outlet) {
            return core::Result<void>::failure(detail::validation(
                "declared terrain outlet is not represented as an outlet"));
        }
    }

    std::vector<std::size_t> ready;
    ready.reserve(count);
    for (std::size_t i = 0U; i < count; ++i) {
        if (upstream_count[i] == 0U) ready.push_back(i);
    }
    for (std::size_t cursor = 0U; cursor < ready.size(); ++cursor) {
        const std::size_t current = ready[cursor];
        const std::uint64_t next_id = cells[current].downstream_cell_id;
        if (!close_enough(cells[current].contributing_area_m2, expected_area[current])) {
            return core::Result<void>::failure(detail::validation(
                "terrain routing contributing area does not match its flow graph"));
        }
        if (next_id == 0U) continue;
        const std::size_t next = static_cast<std::size_t>(next_id - 1U);
        expected_area[next] += expected_area[current];
        if (!std::isfinite(expected_area[next])) {
            return core::Result<void>::failure(detail::validation(
                "terrain routing contributing area overflowed"));
        }
        if (--upstream_count[next] == 0U) ready.push_back(next);
    }
    if (ready.size() != count) {
        return core::Result<void>::failure(detail::validation(
            "terrain routing graph contains a cycle"));
    }
    return core::Result<void>::success();
}

core::Result<void> HydroTerrainRouting::validate_against(
    const BathymetryField& bathymetry) const {
    if (auto result = validate(); !result) return result;
    if (auto result = bathymetry.validate(); !result) return result;
    const auto digest = bathymetry.canonical_digest();
    if (!digest) return core::Result<void>::failure(digest.error());
    if (!same_source(source, bathymetry.source) ||
        terrain_identity != bathymetry.terrain_identity ||
        terrain_revision != bathymetry.terrain_revision ||
        terrain_digest != bathymetry.terrain_digest ||
        bathymetry_digest != digest.value() || origin_xy.x != bathymetry.origin_xy.x ||
        origin_xy.y != bathymetry.origin_xy.y || cell_size_m != bathymetry.cell_size_m ||
        width != bathymetry.width || height != bathymetry.height) {
        return core::Result<void>::failure(detail::stale(
            "terrain routing is stale for its bathymetry source"));
    }
    for (std::size_t i = 0U; i < cells.size(); ++i) {
        if (cells[i].bed_elevation_m != bathymetry.bed_elevations_m[i]) {
            return core::Result<void>::failure(detail::stale(
                "terrain routing elevation differs from its bathymetry source"));
        }
    }
    const auto expected = build_routing(bathymetry, declared_outlet_cell_ids);
    if (!expected) return core::Result<void>::failure(expected.error());
    if (canonical() != expected.value().canonical()) {
        return core::Result<void>::failure(detail::validation(
            "terrain routing differs from deterministic source derivation"));
    }
    return core::Result<void>::success();
}

std::string HydroTerrainRouting::canonical() const {
    std::ostringstream output;
    output << kRoutingMagic << ' ' << kSchemaVersion << '\n'
           << source.canonical() << '\n'
           << "TERRAIN " << std::quoted(terrain_identity) << ' '
           << terrain_revision.value() << ' ' << terrain_digest.hex() << '\n'
           << "BATHYMETRY_DIGEST " << bathymetry_digest.hex() << '\n'
           << "GRID " << detail::number(origin_xy.x) << ' '
           << detail::number(origin_xy.y) << ' ' << detail::number(cell_size_m) << ' '
           << width << ' ' << height << '\n'
           << "OUTLETS " << declared_outlet_cell_ids.size();
    for (const std::uint64_t id : declared_outlet_cell_ids) output << ' ' << id;
    output << '\n' << "CELLS " << cells.size() << '\n';
    for (const auto& cell : cells) {
        output << "CELL " << cell.cell_id << ' ' << cell.downstream_cell_id << ' '
               << kind_name(cell.kind) << ' ' << detail::number(cell.bed_elevation_m)
               << ' ' << detail::number(cell.contributing_area_m2) << '\n';
    }
    output << "END\n";
    return output.str();
}

std::string HydroTerrainRouting::serialize() const { return canonical(); }

core::Result<assets::Sha256Digest> HydroTerrainRouting::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(canonical()));
}

core::Result<HydroTerrainRouting> HydroTerrainRouting::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<HydroTerrainRouting>::failure(detail::validation(
            "terrain routing document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    HydroTerrainRouting routing;
    std::size_t outlet_count = 0U;
    std::size_t cell_count = 0U;
    if (!(input >> magic >> version) || magic != kRoutingMagic) {
        return core::Result<HydroTerrainRouting>::failure(detail::parse_error(
            "terrain routing header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<HydroTerrainRouting>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported terrain routing version"));
    }
    if (!detail::require_token(input, "SOURCE") ||
        !(input >> std::quoted(routing.source.domain_id)) ||
        !detail::read_revision(input, routing.source.source_revision) ||
        !detail::read_digest(input, routing.source.source_digest) ||
        !(input >> std::quoted(routing.source.coordinate_frame)) ||
        !(input >> std::quoted(routing.source.provenance)) ||
        !detail::require_token(input, "TERRAIN") ||
        !(input >> std::quoted(routing.terrain_identity)) ||
        !detail::read_revision(input, routing.terrain_revision) ||
        !detail::read_digest(input, routing.terrain_digest) ||
        !detail::require_token(input, "BATHYMETRY_DIGEST") ||
        !detail::read_digest(input, routing.bathymetry_digest) ||
        !detail::require_token(input, "GRID") ||
        !detail::read_double(input, routing.origin_xy.x) ||
        !detail::read_double(input, routing.origin_xy.y) ||
        !detail::read_double(input, routing.cell_size_m) ||
        !detail::read_unsigned(input, routing.width) ||
        !detail::read_unsigned(input, routing.height) ||
        !detail::require_token(input, "OUTLETS") ||
        !detail::read_count(input, kMaxSamples, outlet_count)) {
        return core::Result<HydroTerrainRouting>::failure(detail::parse_error(
            "terrain routing source or grid record is malformed"));
    }
    routing.declared_outlet_cell_ids.resize(outlet_count);
    for (std::uint64_t& id : routing.declared_outlet_cell_ids) {
        if (!detail::read_unsigned(input, id)) {
            return core::Result<HydroTerrainRouting>::failure(detail::parse_error(
                "terrain routing outlet record is malformed"));
        }
    }
    if (!detail::require_token(input, "CELLS") ||
        !detail::read_count(input, kMaxSamples, cell_count) ||
        cell_count != static_cast<std::uint64_t>(routing.width) * routing.height) {
        return core::Result<HydroTerrainRouting>::failure(detail::parse_error(
            "terrain routing cell count is malformed"));
    }
    routing.cells.resize(cell_count);
    for (auto& cell : routing.cells) {
        if (!detail::require_token(input, "CELL") ||
            !detail::read_unsigned(input, cell.cell_id) ||
            !detail::read_unsigned(input, cell.downstream_cell_id) ||
            !read_kind(input, cell.kind) ||
            !detail::read_double(input, cell.bed_elevation_m) ||
            !detail::read_double(input, cell.contributing_area_m2)) {
            return core::Result<HydroTerrainRouting>::failure(detail::parse_error(
                "terrain routing cell record is malformed"));
        }
    }
    if (!detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<HydroTerrainRouting>::failure(detail::parse_error(
            "terrain routing document has trailing or missing terminator data"));
    }
    if (auto result = routing.validate(); !result) {
        return core::Result<HydroTerrainRouting>::failure(result.error());
    }
    if (routing.serialize() != text) {
        return core::Result<HydroTerrainRouting>::failure(detail::validation(
            "terrain routing document is not in canonical form"));
    }
    return core::Result<HydroTerrainRouting>::success(std::move(routing));
}

core::Result<HydroTerrainRouting> derive_terrain_routing(
    const BathymetryField& bathymetry,
    std::span<const std::uint64_t> declared_outlet_cell_ids) {
    return build_routing(bathymetry, declared_outlet_cell_ids);
}

} // namespace carto::hydro
