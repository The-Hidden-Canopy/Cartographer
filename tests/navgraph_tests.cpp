#include <carto/assets/blob_store.hpp>
#include <carto/navgraph/access.hpp>
#include <carto/navgraph/agent_profile.hpp>
#include <carto/navgraph/envelope.hpp>
#include <carto/navgraph/invalidation.hpp>
#include <carto/navgraph/links.hpp>
#include <carto/navgraph/port_reference.hpp>
#include <carto/navgraph/region.hpp>
#include <carto/navgraph/route.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/world/model.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

carto::world::SemanticArea area_record(
    std::uint64_t id,
    std::string semantic_type,
    std::optional<carto::world::AreaId> parent) {
    carto::world::SemanticArea area;
    area.id = carto::world::AreaId{id};
    area.semantic_type = std::move(semantic_type);
    area.parent = parent;
    return area;
}

carto::world::SemanticPath path_record(
    std::uint64_t id,
    std::string semantic_type,
    std::vector<carto::core::Vec3d> control_points,
    double width,
    bool closed) {
    carto::world::SemanticPath path;
    path.id = carto::world::PathId{id};
    path.semantic_type = std::move(semantic_type);
    path.control_points = std::move(control_points);
    path.width = width;
    path.closed = closed;
    return path;
}

carto::world::StructureGraph access_graph_fixture() {
    using namespace carto::world;
    StructureGraph graph;
    graph.id = StructureGraphId{1U};
    graph.semantic_type = "building-access";
    graph.nodes.emplace(1U, StructureNode{1U, "building", AreaId{1U}});
    graph.nodes.emplace(2U, StructureNode{2U, "room", AreaId{2U}});
    graph.nodes.emplace(3U, StructureNode{3U, "door", AreaId{2U}});
    graph.nodes.emplace(4U, StructureNode{4U, "ladder", std::nullopt});
    graph.edges.emplace(
        1U, StructureEdge{1U, 3U, 4U, "access-shaft", PathId{1U}, true});
    return graph;
}

carto::navgraph::SemanticAreaRegion compile_region_or_fail(
    const carto::world::SemanticArea& area,
    std::vector<carto::core::Vec3d> boundary,
    carto::navgraph::NavigationPlane plane,
    double minimum_clearance,
    const char* message) {
    auto region = carto::navgraph::compile_region(
        area, boundary, plane, minimum_clearance);
    require(static_cast<bool>(region), message);
    return region.value();
}

carto::navgraph::PreparedNavigationEnvelope envelope_fixture() {
    using namespace carto;
    namespace ng = carto::navgraph;
    ng::PreparedNavigationEnvelope envelope;
    envelope.source_namespace = "condition/pine-ridge/nav/v1";
    envelope.source_revision = core::Revision{7U};
    envelope.source_digest = digest("pine-ridge-world-v7");

    envelope.regions.push_back(compile_region_or_fail(
        area_record(1U, "building", std::nullopt),
        {{0.0, 1.0, 0.0}, {10.0, 1.0, 0.0}, {10.0, 1.0, 6.0}, {0.0, 1.0, 6.0}},
        ng::NavigationPlane::xz, 2.0, "building region compiles"));
    envelope.regions.push_back(compile_region_or_fail(
        area_record(2U, "room", world::AreaId{1U}),
        {{2.0, 1.0, 2.0}, {8.0, 1.0, 2.0}, {8.0, 1.0, 4.0}, {2.0, 1.0, 4.0}},
        ng::NavigationPlane::xz, 2.0, "room region compiles"));

    const auto corridor_path = path_record(
        1U, "corridor", {{0.0, 0.0, 0.0}, {30.0, 0.0, 0.0}, {30.0, 0.0, 5.0}}, 6.0, false);
    auto route = ng::compile_route(corridor_path, ng::NavigationPlane::xz);
    require(static_cast<bool>(route), "corridor route compiles");
    auto corridor = ng::compile_corridor(route.value());
    require(static_cast<bool>(corridor), "corridor product compiles");
    envelope.routes.push_back(route.value());
    envelope.corridors.push_back(corridor.value());

    const auto ladder_path = path_record(
        2U, "ladder-route",
        {{40.0, 0.0, 0.0}, {46.0, 0.0, 0.0}, {46.0, 0.0, 6.0}, {40.0, 0.0, 6.0}}, 0.0, true);
    auto ladder_route = ng::compile_route(ladder_path, ng::NavigationPlane::xz);
    require(static_cast<bool>(ladder_route), "closed ladder route compiles");
    auto ladder_corridor = ng::compile_corridor(ladder_route.value());
    require(static_cast<bool>(ladder_corridor), "ladder corridor compiles");
    envelope.routes.push_back(ladder_route.value());
    envelope.corridors.push_back(ladder_corridor.value());

    auto access_points = ng::compile_access_points(access_graph_fixture());
    require(static_cast<bool>(access_points), "access points compile");
    require(access_points.value().size() == 2U, "door and ladder nodes compile as access points");
    envelope.access_points = access_points.value();

    ng::TraversalAnchor door_anchor;
    door_anchor.id = ng::derive_anchor_id(0U);
    door_anchor.medium = ng::TraversalMedium::land;
    door_anchor.position = {5.0, 0.0, 3.0};
    door_anchor.access = envelope.access_points[0].id;
    door_anchor.label = "door-threshold";
    ng::TraversalAnchor dock_anchor;
    dock_anchor.id = ng::derive_anchor_id(1U);
    dock_anchor.medium = ng::TraversalMedium::water;
    dock_anchor.position = {5.0, 0.0, 10.0};
    dock_anchor.label = "dock-end";
    envelope.anchors = {door_anchor, dock_anchor};

    ng::PortReference door_port;
    door_port.port_identity = "port/door/threshold";
    door_port.owner_identity = "structure/building-1";
    door_port.tags = {"door", "gangway"};
    ng::PortReference dock_port;
    dock_port.port_identity = "port/dock/gangway";
    dock_port.owner_identity = "structure/dock-1";
    dock_port.tags = {"gangway"};
    auto links = ng::compile_offmesh_links(
        {door_port, dock_port}, envelope.anchors, {0U, 1U});
    require(static_cast<bool>(links), "gangway ports compile into an off-mesh link");
    require(links.value().size() == 1U, "one link candidate is compiled");
    envelope.links = links.value();
    auto transitions = ng::compile_medium_transitions(envelope.links);
    require(static_cast<bool>(transitions), "medium transitions compile");
    require(transitions.value().size() == 1U, "the gangway crossing records one transition");
    envelope.medium_transitions = transitions.value();

    ng::AgentProfile pedestrian;
    pedestrian.profile_identity = "agent/pedestrian/v1";
    pedestrian.locomotion = ng::LocomotionClass::pedestrian;
    pedestrian.clearances = {{0.0, 0.0, 0.0}, {0.6, 1.9, 0.6}};
    pedestrian.primitives = {
        ng::TraversalPrimitive::ground, ng::TraversalPrimitive::ladder,
        ng::TraversalPrimitive::hatch, ng::TraversalPrimitive::portal};
    ng::AgentProfile vehicle;
    vehicle.profile_identity = "agent/vehicle/v1";
    vehicle.locomotion = ng::LocomotionClass::vehicle;
    vehicle.clearances = {{0.0, 0.0, 0.0}, {2.4, 3.0, 6.0}};
    vehicle.primitives = {ng::TraversalPrimitive::ground};
    envelope.agent_profiles = {pedestrian, vehicle};

    ng::AgentAreaBinding area_binding;
    area_binding.region = envelope.regions[0].id;
    area_binding.profile_identity = pedestrian.profile_identity;
    envelope.area_bindings = {area_binding};

    ng::AgentPathBinding corridor_binding;
    corridor_binding.route = envelope.routes[0].id;
    corridor_binding.profile_identity = pedestrian.profile_identity;
    corridor_binding.required_primitives = ng::classify_route_primitives("corridor");
    ng::AgentPathBinding ladder_binding;
    ladder_binding.route = envelope.routes[1].id;
    ladder_binding.profile_identity = vehicle.profile_identity;
    ladder_binding.required_primitives = ng::classify_route_primitives("ladder-route");
    envelope.path_bindings = {corridor_binding, ladder_binding};

    ng::NavigationTileGrid grid;
    grid.grid_identity = "condition-pine-ridge";
    grid.tile_extent = 4.0;
    grid.level = 0U;
    const auto footprint = ng::footprint_for_bounds(grid, corridor.value().corridor_bounds);
    require(static_cast<bool>(footprint), "corridor footprint derives");
    envelope.footprints = {footprint.value()};
    return envelope;
}

bool near(double left, double right) {
    return std::fabs(left - right) <= 1e-9 * std::max(1.0, std::fabs(right));
}

void region_compilation_is_neutral_and_derived() {
    using namespace carto;
    namespace ng = carto::navgraph;
    const auto region = compile_region_or_fail(
        area_record(9U, "building", std::nullopt),
        {{0.0, 1.0, 0.0}, {10.0, 1.0, 0.0}, {10.0, 1.0, 6.0}, {0.0, 1.0, 6.0}},
        ng::NavigationPlane::xz, 2.0, "rectangle region compiles");
    require(region.id == ng::derive_region_id(world::AreaId{9U}),
            "region identity derives deterministically from the source area");
    require(region.source_area == world::AreaId{9U} && region.semantic_type == "building" &&
            !region.parent_area.has_value(),
            "region preserves source identity, type, and hierarchy");
    require(region.boundary.size() == 4U &&
            ng::vec3_equal(region.boundary[0], {0.0, 0.0, 0.0}) &&
            ng::vec3_equal(region.boundary[2], {10.0, 0.0, 6.0}),
            "region boundary is projected onto the navigation plane");
    require(ng::bounds_equal(region.bounds, {{0.0, 0.0, 0.0}, {10.0, 0.0, 6.0}}),
            "region bounds conservatively cover the boundary");
    require(region.minimum_clearance == 2.0, "region clearance is preserved");

    const auto child = compile_region_or_fail(
        area_record(10U, "room", world::AreaId{9U}),
        {{2.0, 1.0, 2.0}, {8.0, 1.0, 2.0}, {8.0, 1.0, 4.0}, {2.0, 1.0, 4.0}},
        ng::NavigationPlane::xz, 2.0, "child region compiles");
    require(child.parent_area.has_value() && child.parent_area.value() == world::AreaId{9U},
            "region parent is retained as stable source identity");

    require(!ng::compile_region(
                area_record(9U, "building", std::nullopt),
                {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}},
                ng::NavigationPlane::xz, 2.0),
            "region compilation rejects a degenerate boundary");
    require(!ng::compile_region(
                area_record(9U, "building", std::nullopt),
                {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {0.0, 1.0, 0.0}},
                ng::NavigationPlane::xz, -1.0),
            "region compilation rejects a negative clearance");
    require(!ng::compile_region(
                area_record(9U, "building", std::nullopt),
                {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
                ng::NavigationPlane::xz, 2.0),
            "region compilation rejects a non-simple boundary with a repeated point");
}

void route_and_corridor_geometry_is_exact() {
    using namespace carto;
    namespace ng = carto::navgraph;
    auto path = path_record(
        5U, "corridor", {{0.0, 3.0, 0.0}, {30.0, 0.0, 0.0}, {30.0, 0.0, 5.0}}, 6.0, false);
    path.start_area = world::AreaId{2U};
    path.end_area = world::AreaId{1U};
    auto route = ng::compile_route(path, ng::NavigationPlane::xz);
    require(static_cast<bool>(route), "semantic path compiles into a route product");
    const auto& product = route.value();
    require(product.id == ng::derive_route_id(world::PathId{5U}),
            "route identity derives deterministically from the source path");
    require(product.source_path == world::PathId{5U} && product.semantic_type == "corridor" &&
            !product.closed && product.width == 6.0,
            "route preserves source identity, type, closure, and width");
    require(product.spine.control_points.size() == 3U &&
            ng::vec3_equal(product.spine.control_points[0], {0.0, 0.0, 0.0}) &&
            ng::vec3_equal(product.spine.control_points[1], {30.0, 0.0, 0.0}) &&
            ng::vec3_equal(product.spine.control_points[2], {30.0, 0.0, 5.0}),
            "route spine projects control points onto the navigation plane");
    const double expected_leg = 5.0;
    require(product.spine.cumulative_distances.size() == 3U &&
            product.spine.cumulative_distances[0] == 0.0 &&
            product.spine.cumulative_distances[1] == 30.0 &&
            near(product.spine.cumulative_distances[2], 30.0 + expected_leg),
            "route spine accumulates monotone centerline distances");
    require(ng::bounds_equal(product.bounds, {{0.0, 0.0, 0.0}, {30.0, 0.0, 5.0}}),
            "route bounds conservatively cover the centerline");
    require(product.start_area.has_value() && product.start_area.value() == world::AreaId{2U} &&
            product.end_area.has_value() && product.end_area.value() == world::AreaId{1U},
            "route terminal areas are retained as source identities");

    auto corridor = ng::compile_corridor(product);
    require(static_cast<bool>(corridor), "route compiles into a corridor product");
    require(corridor.value().id == ng::derive_corridor_id(world::PathId{5U}),
            "corridor identity derives deterministically from the source path");
    require(corridor.value().route == product.id &&
            corridor.value().source_path == world::PathId{5U},
            "corridor binds to its route and source identity");
    require(ng::vec3_span_equal(corridor.value().polygon, product.spine.control_points),
            "corridor polygon sweeps the projected centerline");
    require(ng::bounds_equal(
                corridor.value().corridor_bounds, {{-3.0, 0.0, -3.0}, {33.0, 0.0, 8.0}}),
            "corridor bounds expand the route bounds by half the width on the plane axes");

    auto closed_path = path_record(
        6U, "patrol-loop",
        {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 0.0, 10.0}, {0.0, 0.0, 10.0}}, 0.0, true);
    auto closed_route = ng::compile_route(closed_path, ng::NavigationPlane::xz);
    require(static_cast<bool>(closed_route), "closed path compiles");
    auto closed_corridor = ng::compile_corridor(closed_route.value());
    require(static_cast<bool>(closed_corridor) &&
            ng::bounds_equal(
                closed_corridor.value().corridor_bounds,
                closed_route.value().bounds),
            "zero-width corridors exactly cover the route bounds");

    auto bad_path = path_record(7U, "corridor", {{0.0, 0.0, 0.0}}, 6.0, false);
    require(!ng::compile_route(bad_path, ng::NavigationPlane::xz),
            "route compilation rejects a one-point path");
    auto repeated = path_record(
        7U, "corridor", {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}, 6.0, false);
    require(!ng::compile_route(repeated, ng::NavigationPlane::xz),
            "route compilation rejects duplicate consecutive control points");
}

void access_points_compile_from_structure_graphs() {
    using namespace carto;
    namespace ng = carto::navgraph;
    auto access_points = ng::compile_access_points(access_graph_fixture());
    require(static_cast<bool>(access_points), "structure graph compiles access points");
    const auto& records = access_points.value();
    require(records.size() == 2U, "passive structure nodes do not produce access records");
    require(records[0].source_node == 3U && records[0].primitive == ng::AccessPrimitive::door &&
            records[0].semantic_type == "door" &&
            records[0].area.has_value() && records[0].area.value() == world::AreaId{2U},
            "door node compiles into a typed access point with area correspondence");
    require(records[0].id == ng::derive_access_id(3U),
            "access identity derives deterministically from the node identity");
    require(records[1].source_node == 4U && records[1].primitive == ng::AccessPrimitive::ladder,
            "ladder node compiles into a typed access point");
    require(records[1].source_graph == world::StructureGraphId{1U},
            "access records retain their structure-graph source identity");

    world::StructureGraph dangling;
    dangling.id = world::StructureGraphId{2U};
    dangling.semantic_type = "broken";
    dangling.nodes.emplace(1U, world::StructureNode{1U, "door", std::nullopt});
    dangling.edges.emplace(
        1U, world::StructureEdge{1U, 1U, 99U, "through", std::nullopt, false});
    require(!ng::compile_access_points(dangling),
            "access compilation rejects a malformed structure graph");
}

void offmesh_links_and_medium_transitions_compile() {
    using namespace carto;
    namespace ng = carto::navgraph;
    ng::TraversalAnchor land_anchor;
    land_anchor.id = ng::derive_anchor_id(0U);
    land_anchor.medium = ng::TraversalMedium::land;
    land_anchor.position = {0.0, 0.0, 0.0};
    ng::TraversalAnchor water_anchor;
    water_anchor.id = ng::derive_anchor_id(1U);
    water_anchor.medium = ng::TraversalMedium::water;
    water_anchor.position = {3.0, 0.0, 4.0};

    ng::PortReference dock_port;
    dock_port.port_identity = "port/dock/a";
    dock_port.owner_identity = "structure/dock";
    dock_port.tags = {"gangway"};
    ng::PortReference vessel_port;
    vessel_port.port_identity = "port/vessel/a";
    vessel_port.owner_identity = "vessel/trawler-1";
    vessel_port.tags = {"gangway"};
    auto links = ng::compile_offmesh_links(
        {dock_port, vessel_port}, {land_anchor, water_anchor}, {0U, 1U});
    require(static_cast<bool>(links), "shared-tag ports compile into a link candidate");
    require(links.value().size() == 1U, "exactly one candidate is compiled");
    const auto& link = links.value()[0];
    require(link.start_anchor == land_anchor.id && link.end_anchor == water_anchor.id,
            "link binds both resolved anchors");
    require(link.start_medium == ng::TraversalMedium::land &&
            link.end_medium == ng::TraversalMedium::water,
            "link records the crossed static mediums");
    require(link.start_port_identity.has_value() &&
            link.start_port_identity.value() == "port/dock/a" &&
            link.end_port_identity.has_value() &&
            link.end_port_identity.value() == "port/vessel/a",
            "link retains stable port identities for correspondence");
    require(near(link.length, 5.0), "link length is the anchor separation");

    auto transitions = ng::compile_medium_transitions(links.value());
    require(static_cast<bool>(transitions) && transitions.value().size() == 1U,
            "medium-boundary crossing records one transition");
    require(transitions.value()[0].from_medium == ng::TraversalMedium::land &&
            transitions.value()[0].to_medium == ng::TraversalMedium::water &&
            transitions.value()[0].link == link.id,
            "transition preserves link identity and medium direction");

    ng::TraversalAnchor shed_anchor;
    shed_anchor.id = ng::derive_anchor_id(2U);
    shed_anchor.medium = ng::TraversalMedium::land;
    shed_anchor.position = {1.0, 0.0, 0.0};
    ng::PortReference lone_port;
    lone_port.port_identity = "port/lone/a";
    lone_port.owner_identity = "structure/shed";
    lone_port.tags = {"mooring"};
    auto none = ng::compile_offmesh_links(
        {lone_port, dock_port}, {land_anchor, shed_anchor}, {0U, 1U});
    require(static_cast<bool>(none) && none.value().empty(),
            "ports without shared tags or medium crossings compile no candidates");

    require(!ng::compile_offmesh_links(
                {dock_port, vessel_port}, {land_anchor, water_anchor}, {0U, 9U}),
            "link compilation rejects an out-of-range anchor ordinal");
    require(!ng::compile_offmesh_links(
                {dock_port, vessel_port, vessel_port},
                {land_anchor, water_anchor},
                {0U, 1U, 1U}),
            "link compilation rejects duplicate port identities");

    ng::TraversalAnchor duplicate_anchor = land_anchor;
    require(!ng::compile_offmesh_links(
                {dock_port, vessel_port},
                {land_anchor, duplicate_anchor},
                {0U, 1U}),
            "link compilation rejects duplicate anchor identities");

    ng::PortReference unsorted_tags;
    unsorted_tags.port_identity = "port/unsorted/a";
    unsorted_tags.owner_identity = "structure/shed";
    unsorted_tags.tags = {"gangway", "alpha"};
    require(!unsorted_tags.validate(),
            "port references reject unsorted compatibility tags");
}

void footprint_derivation_matches_spatial_tile_range_shape() {
    using namespace carto;
    namespace ng = carto::navgraph;
    ng::NavigationTileGrid grid;
    grid.grid_identity = "condition-pine-ridge";
    grid.tile_extent = 4.0;
    grid.level = 0U;

    core::Bounds3d bounds;
    bounds.minimum = {0.0, 0.0, -2.0};
    bounds.maximum = {10.0, 4.0, 2.0};
    auto footprint = ng::footprint_for_bounds(grid, bounds);
    require(static_cast<bool>(footprint), "bounds derive a tile footprint");
    require(footprint.value().grid_identity == "condition-pine-ridge" &&
            footprint.value().level == 0U,
            "footprint retains the grid identity and level");
    require(footprint.value().minimum_x == 0 && footprint.value().maximum_x == 2 &&
            footprint.value().minimum_y == 0 && footprint.value().maximum_y == 0 &&
            footprint.value().minimum_z == -1 && footprint.value().maximum_z == 0,
            "footprint tiles derive with floor/ceil containment semantics");

    grid.level = 1U;
    auto finer = ng::footprint_for_bounds(grid, bounds);
    require(static_cast<bool>(finer) && finer.value().level == 1U &&
            finer.value().maximum_x == 4,
            "each grid level halves the tile extent");

    const auto other = ng::footprint_for_bounds(
        ng::NavigationTileGrid{"condition-pine-ridge", 4.0, 0U},
        core::Bounds3d{{40.0, 0.0, 0.0}, {46.0, 0.0, 6.0}});
    require(static_cast<bool>(other), "second bounds derive a footprint");
    auto combined = ng::union_footprints(
        "condition-pine-ridge", 0U, {footprint.value(), other.value()});
    require(static_cast<bool>(combined), "footprints on one grid union");
    require(combined.value().minimum_x == -1 || combined.value().minimum_x == 0,
            "union covers both footprints");
    require(combined.value().maximum_x == 11 && combined.value().maximum_z == 1,
            "union derives the minimal covering range");

    require(!ng::union_footprints("other-grid", 0U, {footprint.value()}),
            "footprint union fails closed on a mixed grid");
    const ng::NavigationTileFootprint overlapping{
        "condition-pine-ridge", 0U, 1, 0, -1, 3, 0, 0};
    auto intersection = footprint.value().intersects(overlapping);
    require(static_cast<bool>(intersection) && intersection.value(),
            "compatible footprints intersect");
    require(!footprint.value().intersects(
                ng::NavigationTileFootprint{"other-grid", 0U, 0, 0, 0, 1, 1, 1}),
            "incompatible grids fail closed instead of assuming non-overlap");

    core::Bounds3d empty_bounds;
    require(!ng::footprint_for_bounds(grid, empty_bounds),
            "footprint derivation rejects unordered bounds");
    ng::NavigationTileGrid broken_grid;
    broken_grid.grid_identity = "broken";
    broken_grid.tile_extent = 0.0;
    require(!ng::footprint_for_bounds(broken_grid, bounds),
            "footprint derivation rejects a non-positive tile extent");
    ng::NavigationTileFootprint inverted;
    inverted.grid_identity = "inverted";
    inverted.minimum_x = 5;
    inverted.maximum_x = 1;
    require(!inverted.validate(),
            "footprints reject an inverted tile range");
    require(inverted.canonical().find("tile-range-v1{") == 0U,
            "footprint canonical form matches the production tile-range shape");
}

void envelope_digest_is_deterministic_and_round_trips() {
    using namespace carto;
    namespace ng = carto::navgraph;
    auto first = envelope_fixture();
    auto second = envelope_fixture();
    require(static_cast<bool>(first.validate()) && static_cast<bool>(second.validate()),
            "fixture envelope validates");
    const auto first_digest = first.canonical_digest();
    const auto second_digest = second.canonical_digest();
    require(static_cast<bool>(first_digest) && static_cast<bool>(second_digest),
            "envelope digest computes");
    require(first_digest.value() == second_digest.value(),
            "same source and scope produce the same prepared identity");
    require(first.serialize() == second.serialize(),
            "envelope serialization is byte-stable across builds");

    const std::string text = first.serialize();
    auto reopened = ng::PreparedNavigationEnvelope::deserialize(text);
    require(static_cast<bool>(reopened), "envelope round-trips through serialization");
    require(reopened.value().serialize() == text,
            "reopened envelope re-serializes to identical bytes");
    const auto reopened_digest = reopened.value().canonical_digest();
    require(static_cast<bool>(reopened_digest) &&
            reopened_digest.value() == first_digest.value(),
            "reopened envelope preserves the prepared identity digest");
    require(reopened.value().regions == first.regions &&
            reopened.value().routes == first.routes &&
            reopened.value().links == first.links,
            "reopened envelope preserves every compiled record");

    auto mutated = envelope_fixture();
    mutated.routes[0].width += 1.0;
    const auto mutated_digest = mutated.canonical_digest();
    require(static_cast<bool>(mutated_digest) &&
            mutated_digest.value() != first_digest.value(),
            "a semantic change produces a different prepared identity");

    auto reordered = envelope_fixture();
    std::swap(reordered.regions[0], reordered.regions[1]);
    const auto reordered_digest = reordered.canonical_digest();
    require(static_cast<bool>(reordered_digest) &&
            reordered_digest.value() != first_digest.value(),
            "record order is part of the canonical identity");
}

void envelope_cross_references_fail_closed() {
    using namespace carto;
    namespace ng = carto::navgraph;

    auto bad_terminal = envelope_fixture();
    bad_terminal.routes[0].start_area = world::AreaId{999U};
    require(!bad_terminal.validate(),
            "routes fail closed on terminal areas outside the envelope scope");

    auto bad_corridor = envelope_fixture();
    bad_corridor.corridors[0].route = ng::RouteId{1U << 20U};
    require(!bad_corridor.validate(),
            "corridors fail closed on a missing route reference");

    auto bad_link = envelope_fixture();
    bad_link.links[0].start_anchor = ng::AnchorId{1U << 21U};
    require(!bad_link.validate(),
            "links fail closed on a missing anchor reference");

    auto bad_transition = envelope_fixture();
    bad_transition.medium_transitions[0].to_medium = ng::TraversalMedium::building;
    require(!bad_transition.validate(),
            "medium transitions fail closed when they disagree with their link");

    auto bad_binding = envelope_fixture();
    bad_binding.path_bindings[0].profile_identity = "agent/missing/v1";
    require(!bad_binding.validate(),
            "bindings fail closed on a missing agent profile");

    auto zero_digest = envelope_fixture();
    zero_digest.source_digest = assets::Sha256Digest{};
    require(!zero_digest.validate(),
            "envelope fails closed without its source digest binding");

    auto duplicate_profile = envelope_fixture();
    duplicate_profile.agent_profiles.push_back(duplicate_profile.agent_profiles[0]);
    require(!duplicate_profile.validate(),
            "envelope rejects duplicate agent profile identities");
}

void envelope_rejects_hostile_serialization() {
    using namespace carto;
    namespace ng = carto::navgraph;
    const std::string text = envelope_fixture().serialize();

    require(!ng::PreparedNavigationEnvelope::deserialize(""),
            "empty text fails closed");
    require(!ng::PreparedNavigationEnvelope::deserialize("not an envelope"),
            "foreign text fails closed");

    std::string future = text;
    require(future.find("CARTOGRAPHER_NAVIGATION_ENVELOPE 1\n") == 0U,
            "fixture uses the current schema version");
    future.replace(0U, std::string("CARTOGRAPHER_NAVIGATION_ENVELOPE 1\n").size(),
                   "CARTOGRAPHER_NAVIGATION_ENVELOPE 2\n");
    require(!ng::PreparedNavigationEnvelope::deserialize(future),
            "future schema versions fail closed");

    std::string oversized = text;
    require(oversized.find("REGIONS 2\n") != std::string::npos,
            "fixture region count is located");
    oversized.replace(oversized.find("REGIONS 2\n"), std::string("REGIONS 2\n").size(),
                      "REGIONS 99999999999999999999\n");
    require(!ng::PreparedNavigationEnvelope::deserialize(oversized),
            "oversized section counts fail closed");

    std::string trailing = text + "EXTRA\n";
    require(!ng::PreparedNavigationEnvelope::deserialize(trailing),
            "trailing data fails closed");

    std::string missing_end = text;
    require(missing_end.ends_with("END\n"), "fixture ends with its END record");
    missing_end.erase(missing_end.size() - std::string("END\n").size());
    require(!ng::PreparedNavigationEnvelope::deserialize(missing_end),
            "a missing END record fails closed");

    std::string corrupt_digest = text;
    const auto header_start = corrupt_digest.find("HEADER \"");
    const auto namespace_end =
        corrupt_digest.find('"', header_start + std::string("HEADER \"").size());
    const auto revision_start = corrupt_digest.find(' ', namespace_end + 1U);
    const auto digest_start = corrupt_digest.find(' ', revision_start + 1U);
    require(header_start != std::string::npos && namespace_end != std::string::npos &&
            revision_start != std::string::npos && digest_start != std::string::npos,
            "fixture header tokens are located");
    corrupt_digest[digest_start + 1U] = 'z';
    require(!ng::PreparedNavigationEnvelope::deserialize(corrupt_digest),
            "a malformed source digest fails closed");

    std::string duplicate_record = text;
    const auto region_line_start = duplicate_record.find("\nREGION ");
    const auto region_line_end = duplicate_record.find('\n', region_line_start + 1U);
    const std::string region_line =
        duplicate_record.substr(region_line_start, region_line_end - region_line_start);
    duplicate_record.insert(region_line_end, region_line);
    duplicate_record.replace(duplicate_record.find("REGIONS 2\n"),
                             std::string("REGIONS 2\n").size(), "REGIONS 3\n");
    require(!ng::PreparedNavigationEnvelope::deserialize(duplicate_record),
            "duplicate compiled identities fail closed");

    std::string non_finite = text;
    const auto target = non_finite.find("\nREGION ");
    const auto coords = non_finite.find(" 0 0 0", target);
    require(coords != std::string::npos, "fixture region coordinates are located");
    non_finite.replace(coords, std::string(" 0 0 0").size(), " 1e999 0 0");
    require(!ng::PreparedNavigationEnvelope::deserialize(non_finite),
            "non-finite coordinates fail closed");
}

carto::production::ProductionCookKey source_change_key(
    std::string identity,
    std::string token) {
    carto::production::ProductionCookKey key;
    key.source = {std::move(identity), carto::core::Revision{7U},
                  digest(token + "-source")};
    key.kind = carto::production::ArtifactKind::render_mesh;
    key.form = carto::production::ExecutionForm::cached;
    key.algorithm = "source-change-v1";
    key.settings_digest = digest(token + "-settings");
    key.platform_profile = "cpu-reference";
    return key;
}

bool contains_key(
    const std::vector<carto::production::ProductionCookKey>& keys,
    const carto::production::ProductionCookKey& expected) {
    return std::find(keys.begin(), keys.end(), expected) != keys.end();
}

void prepared_navigation_product_binds_stable_identity_and_footprint() {
    using namespace carto;
    namespace ng = carto::navgraph;
    namespace production = carto::production;
    auto envelope = envelope_fixture();
    const auto envelope_digest = envelope.canonical_digest();
    require(static_cast<bool>(envelope_digest), "fixture envelope digest computes");

    const auto product = production::compose_prepared_navigation_product(envelope);
    require(static_cast<bool>(product), "navigation product reference composes");
    require(product.value().identity == "world.navigation" &&
                product.value().kind == production::PreparedProductKind::navigation &&
                product.value().format_version == ng::PreparedNavigationEnvelope::kSchemaVersion &&
                product.value().relative_path == "products/navigation-envelope-v1.bin",
            "navigation product follows the world.model composition pattern");
    require(product.value().content_digest == envelope_digest.value(),
            "navigation product is content-addressed by the envelope digest");

    auto mutated = envelope_fixture();
    mutated.routes[0].width += 1.0;
    const auto mutated_product = production::compose_prepared_navigation_product(mutated);
    require(static_cast<bool>(mutated_product) &&
                mutated_product.value().content_digest != product.value().content_digest,
            "a semantic change produces a different prepared product identity");

    production::ProductionCookGraph graph;
    const auto nav_key = production::register_prepared_navigation_node(graph, envelope);
    require(static_cast<bool>(nav_key),
            "navigation node registers without upstream dependencies");
    require(nav_key.value().source.identity == envelope.source_namespace &&
                nav_key.value().source.revision == envelope.source_revision &&
                nav_key.value().source.content_digest == envelope_digest.value(),
            "navigation cook key is keyed by the envelope stable identity");
    const production::SpatialTileRange expected_footprint{
        "condition-pine-ridge", 0U, -1, 0, -1, 8, 0, 1};
    require(graph.nodes().size() == 1U &&
                graph.nodes()[0].footprint.has_value() &&
                graph.nodes()[0].footprint.value() == expected_footprint,
            "navigation footprint maps onto the spatial tile range without transformation");
    require(nav_key.value().spatial_scope ==
                graph.nodes()[0].footprint.value().canonical(),
            "footprint canonical form is bound into the cook identity");

    require(!production::register_prepared_navigation_node(graph, envelope),
            "the same prepared identity cannot register twice");

    auto mixed = envelope_fixture();
    mixed.footprints.push_back(
        ng::NavigationTileFootprint{"other-grid", 0U, 0, 0, 0, 1, 1, 1});
    production::ProductionCookGraph rejected;
    require(!production::register_prepared_navigation_node(rejected, mixed) &&
                rejected.size() == 0U,
            "mixed-grid footprint evidence fails closed instead of assuming non-overlap");

    auto broken = envelope_fixture();
    broken.links[0].start_anchor = ng::AnchorId{1U << 21U};
    production::ProductionCookGraph invalid_graph;
    require(!production::register_prepared_navigation_node(invalid_graph, broken) &&
                invalid_graph.size() == 0U,
            "an invalid envelope fails closed before registration");
}

void navigation_rebuild_invalidation_is_localized_to_footprint() {
    using namespace carto;
    namespace production = carto::production;
    auto envelope = envelope_fixture();
    const auto envelope_digest = envelope.canonical_digest();
    require(static_cast<bool>(envelope_digest), "fixture envelope digest computes");

    // The changed identities are durable world source records: one semantic
    // area and one semantic path feeding the compiled navigation product.
    const auto area_change = source_change_key(
        "condition/pine-ridge/world/area/1", "area-1");
    const auto path_change = source_change_key(
        "condition/pine-ridge/world/path/1", "path-1");

    production::ProductionCookGraph graph;
    require(static_cast<bool>(graph.add_node(area_change)) &&
                static_cast<bool>(graph.add_node(path_change)),
            "changed area and path identities register as source nodes");
    const auto nav_key = production::register_prepared_navigation_node(
        graph, envelope, {area_change, path_change});
    require(static_cast<bool>(nav_key) && graph.size() == 3U,
            "navigation product registers behind its source dependencies");
    require(nav_key.value().source.content_digest == envelope_digest.value(),
            "navigation cook key binds the exact prepared identity");

    const production::SpatialTileRange overlapping{
        "condition-pine-ridge", 0U, 0, 0, 0, 2, 0, 1};
    const production::SpatialTileRange disjoint{
        "condition-pine-ridge", 0U, 20, 0, 0, 21, 0, 1};
    const auto overlap_check = graph.nodes()[2].footprint.value().intersects(overlapping);
    const auto disjoint_check = graph.nodes()[2].footprint.value().intersects(disjoint);
    require(static_cast<bool>(overlap_check) && overlap_check.value() &&
                static_cast<bool>(disjoint_check) && !disjoint_check.value(),
            "fixture footprint overlaps the changed corridor and not the far region");

    const auto area_local = graph.invalidation_order(area_change, overlapping);
    require(area_local && area_local.value().size() == 2U &&
                contains_key(area_local.value(), nav_key.value()) &&
                area_local.value().front() == area_change,
            "a changed area with an overlapping dirty region re-cooks navigation");
    const auto path_local = graph.invalidation_order(path_change, overlapping);
    require(path_local && path_local.value().size() == 2U &&
                contains_key(path_local.value(), nav_key.value()) &&
                path_local.value().front() == path_change,
            "a changed path with an overlapping dirty region re-cooks navigation");

    const auto area_remote = graph.invalidation_order(area_change, disjoint);
    require(area_remote && area_remote.value().size() == 1U &&
                area_remote.value().front() == area_change &&
                !contains_key(area_remote.value(), nav_key.value()),
            "a disjoint dirty region re-cooks only the changed source, never the whole scene");
    const auto path_remote = graph.invalidation_order(path_change, disjoint);
    require(path_remote && path_remote.value().size() == 1U &&
                !contains_key(path_remote.value(), nav_key.value()),
            "remote path evidence leaves the navigation product clean");

    const auto unfiltered = graph.invalidation_order(area_change);
    require(unfiltered && unfiltered.value().size() == 2U &&
                contains_key(unfiltered.value(), nav_key.value()),
            "without a dirty region the full downstream chain still invalidates");

    auto foreign_grid = overlapping;
    foreign_grid.grid_identity = "another-grid";
    require(!graph.invalidation_order(area_change, foreign_grid),
            "incompatible dirty grids fail closed instead of skipping work");

    auto no_footprints = envelope_fixture();
    no_footprints.footprints.clear();
    production::ProductionCookGraph global_graph;
    require(static_cast<bool>(global_graph.add_node(area_change)),
            "changed source registers in the footprint-less graph");
    const auto global_nav = production::register_prepared_navigation_node(
        global_graph, no_footprints, {area_change});
    require(static_cast<bool>(global_nav) &&
                global_nav.value().spatial_scope.empty(),
            "without footprint evidence the navigation node stays global");
    const auto global_remote = global_graph.invalidation_order(area_change, disjoint);
    require(global_remote && global_remote.value().size() == 2U &&
                contains_key(global_remote.value(), global_nav.value()),
            "a global navigation node is never spatially skipped");
}

} // namespace

int main() {
    region_compilation_is_neutral_and_derived();
    route_and_corridor_geometry_is_exact();
    access_points_compile_from_structure_graphs();
    offmesh_links_and_medium_transitions_compile();
    footprint_derivation_matches_spatial_tile_range_shape();
    envelope_digest_is_deterministic_and_round_trips();
    envelope_cross_references_fail_closed();
    envelope_rejects_hostile_serialization();
    prepared_navigation_product_binds_stable_identity_and_footprint();
    navigation_rebuild_invalidation_is_localized_to_footprint();
    std::cout << "navgraph tests passed\n";
    return EXIT_SUCCESS;
}
