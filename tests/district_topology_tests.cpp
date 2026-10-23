#include <carto/condition/district_topology.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* expression) {
    if (!condition) throw std::runtime_error(expression);
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

using namespace carto::condition;

DistrictTopology fixture() {
    auto result = DistrictTopology::create(9U,
        {{1U, "residential", "north-maintenance"},
            {2U, "commercial", "central-maintenance"},
            {3U, std::nullopt, "south-maintenance"},
            {4U, "industrial", std::nullopt}},
        {{1U, 2U, DistrictElevationRelation::first_higher},
            {2U, 3U, DistrictElevationRelation::unresolved}},
        {{10U, 1U, 2U, DistrictRouteHierarchy::collector, DistrictRouteState::active},
            {11U, 2U, 3U, DistrictRouteHierarchy::local, DistrictRouteState::active},
            {12U, 3U, 4U, DistrictRouteHierarchy::regional, DistrictRouteState::planned}},
        {{100U, DistrictPathKind::adjacency, {1U, 2U, 3U}},
            {101U, DistrictPathKind::active_route, {1U, 3U}},
            {102U, DistrictPathKind::active_route, {1U, 4U}}});
    REQUIRE(result);
    return std::move(result.value());
}

void graph_preserves_region_identity_and_authored_relationships() {
    auto topology = fixture();
    REQUIRE(topology.id() == 9U);
    REQUIRE(topology.revision() == 1U);
    REQUIRE(topology.regions().size() == 4U);
    REQUIRE(topology.regions()[0].id == 1U);
    REQUIRE(topology.regions()[0].land_use_identity == "residential");
    REQUIRE(!topology.regions()[2].land_use_identity.has_value());
    REQUIRE(topology.adjacency()[0].first == 1U);
    REQUIRE(topology.adjacency()[0].second == 2U);
    REQUIRE(topology.adjacency()[0].elevation ==
        DistrictElevationRelation::first_higher);
    REQUIRE(topology.routes()[0].hierarchy == DistrictRouteHierarchy::collector);
    REQUIRE(topology.routes()[0].state == DistrictRouteState::active);
    REQUIRE(topology.routes()[2].state == DistrictRouteState::planned);

    auto reversed = DistrictTopology::create(10U,
        {{2U, "commercial", "central"}, {3U, "residential", "south"}},
        {{3U, 2U, DistrictElevationRelation::first_higher}});
    REQUIRE(reversed);
    REQUIRE(reversed.value().adjacency().front().first == 2U);
    REQUIRE(reversed.value().adjacency().front().second == 3U);
    REQUIRE(reversed.value().adjacency().front().elevation ==
        DistrictElevationRelation::second_higher);
}

void adjacency_route_hierarchy_and_constraints_are_reported_separately() {
    const auto topology = fixture();
    const auto report = topology.inspect();
    REQUIRE(report);
    REQUIRE(report.value().revision == 1U);
    REQUIRE(report.value().adjacency_components.size() == 2U);
    REQUIRE((report.value().adjacency_components[0] ==
        std::vector<DistrictRegionId>{1U, 2U, 3U}));
    REQUIRE(report.value().active_route_components.size() == 2U);
    REQUIRE((report.value().active_route_components[0] ==
        std::vector<DistrictRegionId>{1U, 2U, 3U}));
    REQUIRE(!report.value().constraints_passed());
    REQUIRE(report.value().unsatisfied_constraints ==
        std::vector<DistrictConstraintId>{102U});
}

void malformed_adjacency_routes_and_constraints_fail_closed() {
    REQUIRE(!DistrictTopology::create(0U));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, " \t\v\f ", std::nullopt}}));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, "residential", std::nullopt}, {2U, "commercial", std::nullopt}},
        {{1U, 1U, DistrictElevationRelation::equal}}));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, "residential", std::nullopt}, {2U, "commercial", std::nullopt}},
        {{1U, 2U, DistrictElevationRelation::equal},
            {2U, 1U, DistrictElevationRelation::equal}}));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, "residential", std::nullopt}, {2U, "commercial", std::nullopt}},
        {{1U, 2U, static_cast<DistrictElevationRelation>(255U)}}));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, "residential", std::nullopt}}, {},
        {{1U, 1U, 2U, DistrictRouteHierarchy::local, DistrictRouteState::active}}));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, "residential", std::nullopt}, {2U, "commercial", std::nullopt}},
        {}, {{1U, 1U, 2U, static_cast<DistrictRouteHierarchy>(255U),
            DistrictRouteState::active}}));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, "residential", std::nullopt}, {2U, "commercial", std::nullopt}},
        {}, {}, {{1U, DistrictPathKind::adjacency, {1U, 3U}}}));
    REQUIRE(!DistrictTopology::create(1U,
        {{1U, "residential", std::nullopt}, {2U, "commercial", std::nullopt}},
        {}, {}, {{1U, DistrictPathKind::adjacency, {1U, 1U}}}));
}

void local_edits_preserve_ids_and_refuse_stale_or_invalid_changes_atomically() {
    auto topology = fixture();
    const auto changed = topology.apply(DistrictTopologyEdit{
        1U, DistrictUpsertRegion{{2U, "mixed-use", "central-maintenance"}}});
    REQUIRE(changed);
    REQUIRE(changed.value().revision_before == 1U);
    REQUIRE(changed.value().revision_after == 2U);
    REQUIRE(changed.value().changed_region_ids ==
        std::vector<DistrictRegionId>{2U});
    REQUIRE(topology.regions().size() == 4U);
    REQUIRE(topology.regions()[1].id == 2U);
    REQUIRE(topology.regions()[1].land_use_identity == "mixed-use");
    REQUIRE(topology.routes()[0].id == 10U);
    REQUIRE(topology.routes()[1].id == 11U);

    const auto stale = topology.apply(DistrictTopologyEdit{
        1U, DistrictRemoveRegion{2U}});
    REQUIRE(!stale);
    const auto invalid = topology.apply(DistrictTopologyEdit{
        2U, DistrictUpsertRoute{{13U, 1U, 99U,
            DistrictRouteHierarchy::local, DistrictRouteState::active}}});
    REQUIRE(!invalid);
    REQUIRE(topology.revision() == 2U);
    REQUIRE(topology.regions().size() == 4U);
    REQUIRE(topology.routes().size() == 3U);
}

void activating_a_route_satisfies_only_route_connectivity_constraints() {
    auto topology = fixture();
    const auto edit = topology.apply(DistrictTopologyEdit{
        1U, DistrictUpsertRoute{{12U, 3U, 4U,
            DistrictRouteHierarchy::regional, DistrictRouteState::active}}});
    REQUIRE(edit);
    REQUIRE((edit.value().changed_region_ids ==
        std::vector<DistrictRegionId>{3U, 4U}));
    const auto report = topology.inspect();
    REQUIRE(report);
    REQUIRE(report.value().active_route_components.size() == 1U);
    REQUIRE(report.value().constraints_passed());
    REQUIRE(report.value().unsatisfied_constraints.empty());
}

void constrained_regions_cannot_be_removed_or_silently_drop_requirements() {
    auto topology = fixture();
    const auto blocked = topology.apply(DistrictTopologyEdit{
        1U, DistrictRemoveRegion{2U}});
    REQUIRE(!blocked);
    REQUIRE(topology.revision() == 1U);
    REQUIRE(topology.regions().size() == 4U);

    const auto removed_constraint = topology.apply(DistrictTopologyEdit{
        1U, DistrictRemoveConstraint{100U}});
    REQUIRE(removed_constraint);
    const auto removed_region = topology.apply(DistrictTopologyEdit{
        2U, DistrictRemoveRegion{2U}});
    REQUIRE(removed_region);
    REQUIRE(topology.revision() == 3U);
    REQUIRE(topology.regions().size() == 3U);
    REQUIRE(topology.regions()[0].id == 1U);
    REQUIRE(topology.regions()[1].id == 3U);
    REQUIRE(topology.regions()[2].id == 4U);
    REQUIRE(topology.adjacency().empty());
    REQUIRE(topology.routes().size() == 1U);
    REQUIRE(topology.routes().front().id == 12U);
}

} // namespace

int main() {
    try {
        graph_preserves_region_identity_and_authored_relationships();
        adjacency_route_hierarchy_and_constraints_are_reported_separately();
        malformed_adjacency_routes_and_constraints_fail_closed();
        local_edits_preserve_ids_and_refuse_stale_or_invalid_changes_atomically();
        activating_a_route_satisfies_only_route_connectivity_constraints();
        constrained_regions_cannot_be_removed_or_silently_drop_requirements();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
