#include <carto/assets/blob_store.hpp>
#include <carto/navgraph/agent_profile.hpp>
#include <carto/navgraph/fence.hpp>
#include <carto/navgraph/identity.hpp>
#include <carto/navgraph/links.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

carto::navgraph::AgentProfile pedestrian_profile() {
    carto::navgraph::AgentProfile profile;
    profile.profile_identity = "agent/pedestrian/v1";
    profile.locomotion = carto::navgraph::LocomotionClass::pedestrian;
    profile.clearances = {{0.0, 0.0, 0.0}, {0.6, 1.9, 0.6}};
    profile.primitives = {
        carto::navgraph::TraversalPrimitive::ground,
        carto::navgraph::TraversalPrimitive::ladder,
        carto::navgraph::TraversalPrimitive::hatch,
        carto::navgraph::TraversalPrimitive::portal};
    return profile;
}

carto::navgraph::AgentProfile vehicle_profile() {
    carto::navgraph::AgentProfile profile;
    profile.profile_identity = "agent/vehicle/v1";
    profile.locomotion = carto::navgraph::LocomotionClass::vehicle;
    profile.clearances = {{0.0, 0.0, 0.0}, {2.4, 3.0, 6.0}};
    profile.primitives = {carto::navgraph::TraversalPrimitive::ground};
    return profile;
}

void agent_profiles_validate_static_compatibility_only() {
    using namespace carto;
    namespace ng = carto::navgraph;
    const auto pedestrian = pedestrian_profile();
    const auto vehicle = vehicle_profile();
    require(static_cast<bool>(pedestrian.validate()),
            "pedestrian profile validates");
    require(static_cast<bool>(vehicle.validate()), "vehicle profile validates");
    require(pedestrian.locomotion == ng::LocomotionClass::pedestrian &&
            vehicle.locomotion == ng::LocomotionClass::vehicle,
            "profiles record their neutral locomotion class");
    require(ng::bounds_equal(pedestrian.clearances, {{0.0, 0.0, 0.0}, {0.6, 1.9, 0.6}}),
            "profile clearances are the agent physical envelope");
    require(pedestrian.supports(ng::TraversalPrimitive::ladder) &&
            pedestrian.supports(ng::TraversalPrimitive::portal) &&
            pedestrian.supports(ng::TraversalPrimitive::ground) &&
            !pedestrian.supports(ng::TraversalPrimitive::water),
            "pedestrian support reflects its declared primitives");
    require(vehicle.supports(ng::TraversalPrimitive::ground) &&
            !vehicle.supports(ng::TraversalPrimitive::ladder) &&
            !vehicle.supports(ng::TraversalPrimitive::portal) &&
            !vehicle.supports(ng::TraversalPrimitive::water),
            "vehicle support is ground-only");

    ng::AgentProfile broken = pedestrian;
    broken.primitives = {
        ng::TraversalPrimitive::ladder, ng::TraversalPrimitive::ground};
    require(!broken.validate(),
            "profiles reject unsorted primitive lists");
    broken = pedestrian;
    broken.primitives = {
        ng::TraversalPrimitive::ground, ng::TraversalPrimitive::ground};
    require(!broken.validate(),
            "profiles reject duplicate primitives");
    broken = pedestrian;
    broken.profile_identity.clear();
    require(!broken.validate(), "profiles reject an empty identity");
    broken = pedestrian;
    broken.clearances = {{5.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    require(!broken.validate(), "profiles reject inverted clearances");
    broken = pedestrian;
    broken.locomotion = static_cast<ng::LocomotionClass>(200U);
    require(!broken.validate(), "profiles reject unknown locomotion classes");
}

void agent_filtering_rejects_unsupported_traversal() {
    using namespace carto;
    namespace ng = carto::navgraph;
    const auto pedestrian = pedestrian_profile();
    const auto vehicle = vehicle_profile();

    // A ladder-shaft route statically requires the ladder primitive.
    const auto ladder_requirements = ng::classify_route_primitives("ladder-route");
    require(ladder_requirements.size() == 1U &&
            ladder_requirements[0] == ng::TraversalPrimitive::ladder,
            "ladder routes classify to the ladder primitive");
    const auto water_requirements = ng::classify_route_primitives("water-route");
    require(water_requirements.size() == 1U &&
            water_requirements[0] == ng::TraversalPrimitive::water,
            "water routes classify to the water primitive");
    const auto road_requirements = ng::classify_route_primitives("farm-road");
    require(road_requirements.size() == 1U &&
            road_requirements[0] == ng::TraversalPrimitive::ground,
            "unrecognized routes require ground traversal");
    const auto deck_requirements = ng::classify_route_primitives("vessel-deck-route");
    require(deck_requirements.size() == 2U &&
            deck_requirements[0] == ng::TraversalPrimitive::ground &&
            deck_requirements[1] == ng::TraversalPrimitive::vessel_deck,
            "deck routes classify to ground and vessel-deck primitives");

    ng::AgentPathBinding ladder_binding;
    ladder_binding.route = ng::derive_route_id(world::PathId{9U});
    ladder_binding.profile_identity = vehicle.profile_identity;
    ladder_binding.required_primitives = ladder_requirements;
    require(!ng::agent_supports_route(vehicle, ladder_binding),
            "vehicle profiles reject ladder-only routes");
    ladder_binding.profile_identity = pedestrian.profile_identity;
    require(ng::agent_supports_route(pedestrian, ladder_binding),
            "pedestrian profiles accept ladder routes");

    ng::AgentPathBinding water_binding;
    water_binding.route = ng::derive_route_id(world::PathId{10U});
    water_binding.profile_identity = vehicle.profile_identity;
    water_binding.required_primitives = water_requirements;
    require(!ng::agent_supports_route(vehicle, water_binding),
            "vehicle profiles reject water routes");

    ng::AgentAreaBinding deck_binding;
    deck_binding.region = ng::derive_region_id(world::AreaId{4U});
    deck_binding.profile_identity = vehicle.profile_identity;
    deck_binding.required_primitives = {ng::TraversalPrimitive::vessel_deck};
    require(!ng::agent_supports_region(vehicle, deck_binding),
            "vehicle profiles reject deck-only regions");

    ng::AgentAreaBinding open_binding;
    open_binding.region = ng::derive_region_id(world::AreaId{4U});
    open_binding.profile_identity = vehicle.profile_identity;
    require(ng::agent_supports_region(vehicle, open_binding),
            "unconstrained region bindings impose no filtering");

    require(ng::agent_supports_access(pedestrian, ng::AccessPrimitive::door) &&
            ng::agent_supports_access(vehicle, ng::AccessPrimitive::door),
            "doors are traversal-neutral for every profile");
    require(ng::agent_supports_access(pedestrian, ng::AccessPrimitive::ladder) &&
            !ng::agent_supports_access(vehicle, ng::AccessPrimitive::ladder),
            "ladder access requires the ladder primitive");
    require(ng::agent_supports_access(pedestrian, ng::AccessPrimitive::hatch) &&
            !ng::agent_supports_access(vehicle, ng::AccessPrimitive::hatch),
            "hatch access requires the hatch primitive");
    require(ng::agent_supports_access(pedestrian, ng::AccessPrimitive::portal) &&
            !ng::agent_supports_access(vehicle, ng::AccessPrimitive::portal),
            "portal access requires the portal primitive");
    require(ng::traversal_primitive_for_access(ng::AccessPrimitive::door) ==
                ng::TraversalPrimitive::ground &&
            ng::traversal_primitive_for_access(ng::AccessPrimitive::ladder) ==
                ng::TraversalPrimitive::ladder,
            "access primitives map onto neutral traversal primitives");

    ng::AgentPathBinding bad_binding;
    bad_binding.route = ng::RouteId{0U};
    bad_binding.profile_identity = vehicle.profile_identity;
    require(!bad_binding.validate(), "bindings reject a missing route identity");
    bad_binding.route = ng::derive_route_id(world::PathId{9U});
    bad_binding.profile_identity.clear();
    require(!bad_binding.validate(), "bindings reject an empty profile identity");
    bad_binding.profile_identity = vehicle.profile_identity;
    bad_binding.required_primitives = {
        ng::TraversalPrimitive::ground, ng::TraversalPrimitive::ground};
    require(static_cast<bool>(bad_binding.validate()),
            "bindings tolerate redundant requirement lists");
}

void dynamic_cost_fields_are_fenced_out_at_compile_time() {
    using namespace carto;
    namespace ng = carto::navgraph;
    // The dynamic cost/knowledge layer is game-owned. These fences fail the
    // build if a public navigation record ever grows such a member.
    static_assert(
        !ng::kHasDynamicTraversalCost<ng::AgentProfile>::value,
        "agent profiles must not carry dynamic cost fields");
    static_assert(
        !ng::kHasDynamicTraversalCost<ng::AgentAreaBinding>::value &&
            !ng::kHasDynamicTraversalCost<ng::AgentPathBinding>::value,
        "agent bindings must not carry dynamic cost fields");
    static_assert(
        !ng::kHasDynamicTraversalCost<ng::OffMeshLinkCandidate>::value &&
            !ng::kHasDynamicTraversalCost<ng::MediumTransition>::value,
        "link records must not carry dynamic cost fields");
    static_assert(
        std::is_same_v<
            decltype(ng::kHasDynamicTraversalCost<ng::AgentProfile>::value),
            const bool>,
        "the dynamic-cost fence resolves at compile time");
    require(digest("agent-profile-fixture").is_zero() == false,
            "fixture digest helper produces a binding digest");
}

} // namespace

int main() {
    agent_profiles_validate_static_compatibility_only();
    agent_filtering_rejects_unsupported_traversal();
    dynamic_cost_fields_are_fenced_out_at_compile_time();
    std::cout << "navgraph agent profile tests passed\n";
    return EXIT_SUCCESS;
}
