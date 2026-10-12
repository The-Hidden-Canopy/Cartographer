#include <carto/assets/blob_store.hpp>
#include <carto/hydro/bathymetry.hpp>
#include <carto/hydro/contact.hpp>
#include <carto/hydro/domain.hpp>
#include <carto/hydro/graph.hpp>
#include <carto/hydro/runoff.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double left, double right, double epsilon = 1.0e-9) {
    return std::abs(left - right) <= epsilon * std::max({1.0, std::abs(left), std::abs(right)});
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

carto::hydro::HydroDomain domain_fixture() {
    carto::hydro::HydroDomain domain;
    domain.stable_id = "pine-ridge-basin";
    domain.kind = carto::hydro::HydroBodyKind::river;
    domain.coordinate_frame = "carto.local/v1";
    domain.source_revision = carto::core::Revision{7U};
    domain.source_digest = digest("pine-ridge-authored-source");
    domain.provenance = "authoring-reference";
    return domain;
}

carto::hydro::HydroGraph graph_fixture(
    carto::hydro::HydroDomain domain = domain_fixture()) {
    using namespace carto;
    hydro::HydroGraph graph{domain};
    hydro::HydroCell upstream{1U, 10.0, 0.0, 1.0};
    hydro::HydroCell downstream{2U, 10.0, 0.0, 0.0};
    require(static_cast<bool>(graph.insert_cell(upstream)), "upstream cell enters graph");
    require(static_cast<bool>(graph.insert_cell(downstream)), "downstream cell enters graph");
    hydro::HydroEdge channel;
    channel.id = 11U;
    channel.from_cell = 1U;
    channel.to_cell = 2U;
    channel.kind = hydro::HydroEdgeKind::channel;
    channel.capacity_m3_per_s = 2.0;
    require(static_cast<bool>(graph.insert_edge(channel)), "channel edge enters graph");
    hydro::HydroEdge boundary;
    boundary.id = 12U;
    boundary.from_cell = 2U;
    boundary.kind = hydro::HydroEdgeKind::boundary;
    boundary.capacity_m3_per_s = 1.0;
    require(static_cast<bool>(graph.insert_edge(boundary)), "boundary edge enters graph");
    require(static_cast<bool>(graph.validate()), "graph fixture validates");
    return graph;
}

carto::hydro::BathymetryField bathymetry_fixture() {
    using namespace carto;
    hydro::BathymetryField field;
    field.source.domain_id = "pine-ridge-basin";
    field.source.source_revision = core::Revision{7U};
    field.source.source_digest = digest("pine-ridge-authored-source");
    field.source.coordinate_frame = "carto.local/v1";
    field.source.provenance = "authoring-reference";
    field.terrain_identity = "pine-ridge-terrain";
    field.terrain_revision = core::Revision{4U};
    field.terrain_digest = digest("pine-ridge-terrain-v4");
    field.origin_xy = {0.0, 0.0};
    field.cell_size_m = 1.0;
    field.width = 3U;
    field.height = 2U;
    field.bed_elevations_m = {0.0, 1.0, 0.0, -1.0, 2.0, -1.0};
    return field;
}

carto::hydro::HydroRunoffCatchment runoff_catchment_fixture() {
    using namespace carto;
    hydro::HydroRunoffCatchment catchment;
    catchment.stable_id = "pine-ridge-catchment";
    catchment.source.domain_id = "pine-ridge-basin";
    catchment.source.source_revision = core::Revision{7U};
    catchment.source.source_digest = digest("pine-ridge-authored-source");
    catchment.source.coordinate_frame = "carto.local/v1";
    catchment.source.provenance = "authoring-reference";
    catchment.terrain_identity = "pine-ridge-terrain";
    catchment.terrain_revision = core::Revision{4U};
    catchment.terrain_digest = digest("pine-ridge-terrain-v4");
    catchment.boundary_edge_id = 12U;
    catchment.catchment_area_m2 = 100.0;
    catchment.runoff_coefficient = 0.8;
    catchment.infiltration_fraction = 0.25;
    return catchment;
}

carto::hydro::HydroRunoffInput runoff_input_fixture() {
    using namespace carto;
    hydro::HydroRunoffInput input;
    input.precipitation_rate_m_per_s = 0.001;
    input.elapsed_seconds = 10.0;
    input.terrain.terrain_identity = "pine-ridge-terrain";
    input.terrain.terrain_revision = core::Revision{4U};
    input.terrain.terrain_digest = digest("pine-ridge-terrain-v4");
    input.terrain.source_tile_identity = "tile-01";
    input.terrain.outlet_tile_identity = "tile-01";
    input.terrain.source_elevation_m = 10.0;
    input.terrain.outlet_elevation_m = 9.0;
    input.terrain.flow_distance_m = 100.0;
    return input;
}

void domain_round_trip_is_canonical_and_bounded() {
    using namespace carto;
    const hydro::HydroDomain domain = domain_fixture();
    require(static_cast<bool>(domain.validate()), "domain fixture validates");
    const std::string encoded = domain.serialize();
    const auto reopened = hydro::HydroDomain::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "domain survives a canonical round trip");
    require(domain.canonical_digest() && reopened.value().canonical_digest() &&
            domain.canonical_digest().value() == reopened.value().canonical_digest().value(),
        "domain digest is deterministic");
    require(!hydro::HydroDomain::deserialize(encoded + "trailing"),
        "domain rejects trailing data");
    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_HYDRO_DOMAIN 1"}.size(),
        "CARTOGRAPHER_HYDRO_DOMAIN 99");
    const auto future_result = hydro::HydroDomain::deserialize(future);
    require(!future_result && future_result.error().code == core::ErrorCode::version_mismatch,
        "domain rejects future versions precisely");
    hydro::HydroDomain missing_digest = domain;
    missing_digest.source_digest = {};
    require(!missing_digest.validate(), "domain refuses an unbound source digest");
}

void graph_round_trip_preserves_identity_and_rejects_dangling_edges() {
    using namespace carto;
    const hydro::HydroGraph graph = graph_fixture();
    const std::string encoded = graph.serialize();
    const auto reopened = hydro::HydroGraph::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "graph survives a canonical round trip");
    require(graph.canonical_digest() && reopened.value().canonical_digest() &&
            graph.canonical_digest().value() == reopened.value().canonical_digest().value(),
        "graph digest is deterministic");
    require(!hydro::HydroGraph::deserialize(encoded + "trailing"),
        "graph rejects trailing data");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_HYDRO_GRAPH 1"}.size(),
        "CARTOGRAPHER_HYDRO_GRAPH 99");
    require(!hydro::HydroGraph::deserialize(future), "graph rejects future versions");

    hydro::HydroGraph dangling{domain_fixture()};
    require(static_cast<bool>(dangling.insert_cell({1U, 1.0, 0.0, 0.0})),
        "dangling fixture cell enters");
    hydro::HydroEdge edge;
    edge.id = 1U;
    edge.from_cell = 1U;
    edge.to_cell = 99U;
    edge.capacity_m3_per_s = 1.0;
    require(!dangling.insert_edge(edge), "dangling edge refuses before mutation");

    hydro::HydroEdge boundary;
    boundary.id = 2U;
    boundary.from_cell = 1U;
    boundary.to_cell = 1U;
    boundary.kind = hydro::HydroEdgeKind::boundary;
    boundary.capacity_m3_per_s = 1.0;
    require(!dangling.insert_edge(boundary), "boundary edge cannot have a target cell");
}

void graph_step_conserves_volume_and_is_deterministic() {
    using namespace carto;
    const hydro::HydroGraph first = graph_fixture();
    const hydro::HydroGraph second = graph_fixture();
    const auto initial = first.initial_state();
    require(static_cast<bool>(initial), "initial state is admitted");
    require(first.total_volume_m3(initial.value()) &&
            near(first.total_volume_m3(initial.value()).value(), 10.0),
        "initial water volume is derived from cell area and depth");

    const std::vector<hydro::HydroBoundaryFlux> fluxes{{12U, 0.5}};
    const auto first_step = first.advance(initial.value(), 1.0, fluxes);
    const auto second_step = second.advance(initial.value(), 1.0, fluxes);
    require(first_step && second_step, "bounded hydro step is admitted");
    require(static_cast<bool>(first_step.value().validate()), "hydro step receipt validates");
    require(first_step.value().receipt_digest == second_step.value().receipt_digest &&
            first_step.value().state.serialize() == second_step.value().state.serialize(),
        "identical source and inputs produce identical hydro state and receipt");
    require(near(first_step.value().ledger.initial_volume_m3, 10.0) &&
            near(first_step.value().ledger.boundary_inflow_m3, 0.5) &&
            near(first_step.value().ledger.boundary_outflow_m3, 0.0) &&
            near(first_step.value().ledger.final_volume_m3, 10.5),
        "volume ledger accounts for boundary input and internal transfer");
    require(near(first_step.value().state.cells.at(1U).water_depth_m, 0.81) &&
            near(first_step.value().state.cells.at(2U).water_depth_m, 0.24),
        "positive-head transfer and boundary input settle in stable edge order");

    hydro::HydroState stale = initial.value();
    stale.source_digest = digest("different-authored-source");
    const auto stale_result = first.total_volume_m3(stale);
    require(!stale_result && stale_result.error().code == core::ErrorCode::stale_data,
        "state bound to another source refuses graph queries");

    const std::vector<hydro::HydroBoundaryFlux> duplicate{{12U, 0.1}, {12U, 0.1}};
    require(!first.advance(initial.value(), 1.0, duplicate),
        "duplicate boundary fluxes refuse before state mutation");
    const std::vector<hydro::HydroBoundaryFlux> too_large{{12U, 1.1}};
    require(!first.advance(initial.value(), 1.0, too_large),
        "boundary capacity overflow refuses");
    const std::vector<hydro::HydroBoundaryFlux> impossible_outflow{{12U, -11.0}};
    require(!first.advance(initial.value(), 1.0, impossible_outflow),
        "outflow that would create negative volume refuses");
}

void runoff_partition_is_source_bound_and_graph_admissible() {
    using namespace carto;
    const auto catchment = runoff_catchment_fixture();
    const auto input = runoff_input_fixture();
    require(static_cast<bool>(catchment.validate()), "runoff catchment fixture validates");
    require(static_cast<bool>(input.validate()), "runoff input fixture validates");

    const auto planned = hydro::plan_runoff(catchment, input);
    require(static_cast<bool>(planned), "runoff partition is admitted");
    require(static_cast<bool>(planned.value().validate()), "runoff plan validates");
    require(near(planned.value().gross_precipitation_volume_m3, 1.0) &&
            near(planned.value().infiltrated_volume_m3 + planned.value().runoff_volume_m3,
                planned.value().gross_precipitation_volume_m3) &&
            planned.value().runoff_volume_m3 > 0.0,
        "rainfall, infiltration, and runoff close their volume ledger");

    const auto reopened = hydro::HydroRunoffPlan::deserialize(planned.value().serialize());
    require(reopened && reopened.value().serialize() == planned.value().serialize(),
        "runoff plan survives a canonical round trip");
    require(planned.value().canonical_digest() && reopened.value().canonical_digest() &&
            planned.value().canonical_digest().value() ==
                reopened.value().canonical_digest().value(),
        "runoff plan digest is deterministic");
    require(!hydro::HydroRunoffPlan::deserialize(planned.value().serialize() + "trailing"),
        "runoff plan rejects trailing data");
    std::string future = planned.value().serialize();
    future.replace(0U, std::string{"CARTOGRAPHER_HYDRO_RUNOFF_PLAN 1"}.size(),
        "CARTOGRAPHER_HYDRO_RUNOFF_PLAN 99");
    require(!hydro::HydroRunoffPlan::deserialize(future),
        "runoff plan rejects future versions");

    const auto graph = graph_fixture();
    const auto admitted_flux = planned.value().boundary_flux_for(graph, 10.0);
    require(static_cast<bool>(admitted_flux),
        "runoff plan binds to the current graph boundary before producing flux");
    const std::vector<hydro::HydroBoundaryFlux> fluxes{admitted_flux.value()};
    const auto initial = graph.initial_state();
    require(static_cast<bool>(initial), "runoff graph initial state is admitted");
    const auto step = graph.advance(initial.value(), 10.0, fluxes);
    require(static_cast<bool>(step) &&
            near(step.value().ledger.final_volume_m3,
                step.value().ledger.initial_volume_m3 + planned.value().runoff_volume_m3),
        "admitted runoff enters the conserved graph ledger");

    auto stale_input = input;
    stale_input.terrain.terrain_revision = core::Revision{5U};
    const auto stale_plan = hydro::plan_runoff(catchment, stale_input);
    require(!stale_plan && stale_plan.error().code == core::ErrorCode::stale_data,
        "runoff plan refuses stale terrain revisions");

    auto crossed_tile = input;
    crossed_tile.terrain.outlet_tile_identity = "tile-02";
    const auto crossed_plan = hydro::plan_runoff(catchment, crossed_tile);
    require(!crossed_plan && crossed_plan.error().code == core::ErrorCode::stale_data,
        "runoff plan refuses tile-version boundary crossings");

    auto uphill = input;
    uphill.terrain.outlet_elevation_m = 11.0;
    const auto uphill_plan = hydro::plan_runoff(catchment, uphill);
    require(!uphill_plan && uphill_plan.error().code == core::ErrorCode::invalid_argument,
        "runoff plan refuses an uphill outlet");

    auto non_finite = input;
    non_finite.precipitation_rate_m_per_s = std::numeric_limits<double>::quiet_NaN();
    require(!hydro::plan_runoff(catchment, non_finite),
        "runoff plan refuses non-finite precipitation");

    auto overloaded_input = input;
    overloaded_input.precipitation_rate_m_per_s = 10.0;
    const auto overloaded = hydro::plan_runoff(catchment, overloaded_input);
    require(static_cast<bool>(overloaded), "overloaded runoff still records a valid derived plan");
    require(!overloaded.value().boundary_flux_for(graph, 10.0),
        "graph admission refuses runoff above current boundary capacity");

    hydro::HydroDomain changed_domain = domain_fixture();
    changed_domain.source_revision = core::Revision{8U};
    const auto stale_graph = graph_fixture(changed_domain);
    require(!planned.value().boundary_flux_for(stale_graph, 10.0) &&
            planned.value().boundary_flux_for(stale_graph, 10.0).error().code ==
                core::ErrorCode::stale_data,
        "runoff plan refuses a graph from another hydro source revision");
}

void bathymetry_wet_dry_and_shoreline_preserve_lineage() {
    using namespace carto;
    const hydro::BathymetryField bathymetry = bathymetry_fixture();
    require(static_cast<bool>(bathymetry.validate()), "bathymetry fixture validates");
    require(bathymetry.sample_bed({0.25, 1.25}) &&
            near(bathymetry.sample_bed({0.25, 1.25}).value(), -1.0),
        "bathymetry samples the authored row-major bed");
    const auto outside = bathymetry.sample_bed({3.0, 0.0});
    require(!outside && outside.error().code == core::ErrorCode::not_found,
        "bathymetry refuses queries outside the authored field");

    const auto wet_dry = hydro::derive_wet_dry(bathymetry, 0.5);
    require(static_cast<bool>(wet_dry), "wet/dry field derives from bathymetry");
    require(wet_dry.value().wet_cells == std::vector<std::uint8_t>{1U, 0U, 1U, 1U, 0U, 1U},
        "wet/dry admission uses a deterministic zero-depth interface");
    require(static_cast<bool>(wet_dry.value().validate_against(bathymetry)),
        "wet/dry field remains bound to its bathymetry lineage");
    const auto wet_dry_reopened =
        hydro::WetDryField::deserialize(wet_dry.value().serialize());
    require(wet_dry_reopened && wet_dry_reopened.value().serialize() ==
            wet_dry.value().serialize(),
        "wet/dry field survives a canonical round trip");

    const auto shoreline = hydro::derive_shoreline(bathymetry, wet_dry.value());
    require(static_cast<bool>(shoreline) && !shoreline.value().segments.empty(),
        "shoreline derives from wet/dry interfaces");
    require(static_cast<bool>(shoreline.value().validate_against(
                bathymetry, wet_dry.value())),
        "shoreline preserves source and wet/dry lineage");
    const auto shoreline_reopened =
        hydro::ShorelineField::deserialize(shoreline.value().serialize());
    require(shoreline_reopened && shoreline_reopened.value().serialize() ==
            shoreline.value().serialize(),
        "shoreline survives a canonical round trip");
    for (std::size_t index = 1U; index < shoreline.value().segments.size(); ++index) {
        require(shoreline.value().segments[index - 1U].identity <
                shoreline.value().segments[index].identity,
            "shoreline identities are stable and strictly ordered");
    }

    const auto raised_wet_dry = hydro::derive_wet_dry(bathymetry, 1.5);
    const auto raised_shoreline = hydro::derive_shoreline(
        bathymetry, raised_wet_dry.value());
    require(raised_wet_dry && raised_shoreline,
        "a water-level change derives a new bounded shoreline product");
    std::set<std::uint64_t> original_ids;
    for (const auto& segment : shoreline.value().segments) original_ids.insert(segment.identity);
    std::set<std::uint64_t> raised_ids;
    for (const auto& segment : raised_shoreline.value().segments) raised_ids.insert(segment.identity);
    bool retained_identity = false;
    for (const auto identity : original_ids) retained_identity |= raised_ids.contains(identity);
    require(retained_identity, "water-level motion retains stable shoreline identities where topology persists");

    hydro::BathymetryField stale_bathymetry = bathymetry;
    stale_bathymetry.terrain_revision = core::Revision{5U};
    require(!wet_dry.value().validate_against(stale_bathymetry) &&
            wet_dry.value().validate_against(stale_bathymetry).error().code ==
                core::ErrorCode::stale_data,
        "terrain revision changes invalidate dependent wet/dry products");

    const std::string encoded = bathymetry.serialize();
    require(hydro::BathymetryField::deserialize(encoded + "trailing").has_value() == false,
        "bathymetry rejects trailing data");
    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_BATHYMETRY_FIELD 1"}.size(),
        "CARTOGRAPHER_BATHYMETRY_FIELD 99");
    require(!hydro::BathymetryField::deserialize(future),
        "bathymetry rejects future versions");
}

void hydro_contact_is_fluid_evidence_not_solid_collision() {
    using namespace carto;
    hydro::PlanarFreeSurface surface;
    surface.source.domain_id = "pine-ridge-basin";
    surface.source.source_revision = core::Revision{7U};
    surface.source.source_digest = digest("pine-ridge-authored-source");
    surface.source.coordinate_frame = "carto.local/v1";
    surface.source.provenance = "analytic-reference";
    surface.fluid_velocity_mps = {1.0, 0.0, 0.0};
    require(static_cast<bool>(surface.validate()), "planar surface fixture validates");

    const auto above = hydro::query_point(surface, {0.0, 0.0, 2.0});
    const auto below = hydro::query_point(surface, {0.0, 0.0, -2.0});
    require(above && below, "point fluid queries are admitted");
    require(above.value().submerged_fraction == 0.0 &&
            below.value().submerged_fraction == 1.0 &&
            above.value().fluid_contact && !above.value().solid_collision,
        "point queries report fluid state without solid collision");

    const auto shallow = hydro::query_sphere(surface, {0.0, 0.0, 0.5}, 1.0);
    const auto centered = hydro::query_sphere(surface, {0.0, 0.0, 0.0}, 1.0);
    const auto deep = hydro::query_sphere(surface, {0.0, 0.0, -0.5}, 1.0);
    require(shallow && centered && deep, "sphere fluid queries are admitted");
    require(shallow.value().submerged_fraction < centered.value().submerged_fraction &&
            centered.value().submerged_fraction < deep.value().submerged_fraction &&
            shallow.value().wetted_area_m2 < deep.value().wetted_area_m2,
        "sphere submergence and wetted area are monotonic with depth");
    require(near(centered.value().submerged_fraction, 0.5),
        "sphere centered on a plane has half-volume submergence");

    const auto entry = hydro::sweep_surface(surface, {0.0, 0.0, 2.0}, {0.0, 0.0, -2.0});
    const auto exit = hydro::sweep_surface(surface, {0.0, 0.0, -2.0}, {0.0, 0.0, 2.0});
    const auto no_crossing = hydro::sweep_surface(surface, {0.0, 0.0, 2.0}, {0.0, 0.0, 1.0});
    require(entry && exit && no_crossing && entry.value().has_value() &&
            exit.value().has_value() && !no_crossing.value().has_value(),
        "surface sweep distinguishes entry, exit, and no crossing");
    require(entry.value()->direction == hydro::SurfaceCrossingDirection::entering &&
            exit.value()->direction == hydro::SurfaceCrossingDirection::exiting &&
            near(entry.value()->time_of_impact, 0.5) &&
            near(exit.value()->time_of_impact, 0.5),
        "surface crossing reports bounded deterministic time of impact");

    hydro::PlanarFreeSurface invalid = surface;
    invalid.normal = {0.0, 0.0, 2.0};
    require(!invalid.validate(), "non-unit free-surface normal refuses");
    invalid = surface;
    invalid.source.source_digest = {};
    require(!invalid.validate(), "unbound free-surface source refuses");
    require(!hydro::query_sphere(surface, {0.0, 0.0, 0.0}, 0.0),
        "zero-radius sphere query refuses");
    require(!hydro::query_sphere(surface, {0.0, 0.0, 0.0}, 1.0e6 + 1.0),
        "sphere query refuses radii outside the bounded evidence range");
}

void hostile_state_documents_fail_closed() {
    using namespace carto;
    const auto graph = graph_fixture();
    const auto state = graph.initial_state();
    require(static_cast<bool>(state), "state fixture is admitted");
    const std::string encoded = state.value().serialize();
    const auto reopened = hydro::HydroState::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "state survives a canonical round trip");
    require(!hydro::HydroState::deserialize(encoded + "trailing"),
        "state rejects trailing data");

    std::string duplicate = encoded;
    duplicate.replace(duplicate.find("CELLS 2"), std::string{"CELLS 2"}.size(), "CELLS 3");
    duplicate.insert(duplicate.find("END\n"), "STATE 1 1 0 0 0\n");
    require(!hydro::HydroState::deserialize(duplicate),
        "duplicate state cell identities refuse");

    std::string corrupt = encoded;
    const std::string digest_text = state.value().source_digest.hex();
    const std::size_t position = corrupt.find(digest_text);
    require(position != std::string::npos, "state carries a source digest");
    corrupt.replace(position, digest_text.size(), std::string(64U, '0'));
    require(!hydro::HydroState::deserialize(corrupt),
        "malformed or unbound source digest refuses");
}

} // namespace

int main() {
    domain_round_trip_is_canonical_and_bounded();
    graph_round_trip_preserves_identity_and_rejects_dangling_edges();
    graph_step_conserves_volume_and_is_deterministic();
    runoff_partition_is_source_bound_and_graph_admissible();
    bathymetry_wet_dry_and_shoreline_preserve_lineage();
    hydro_contact_is_fluid_evidence_not_solid_collision();
    hostile_state_documents_fail_closed();
    std::cout << "hydro tests passed\n";
    return EXIT_SUCCESS;
}
