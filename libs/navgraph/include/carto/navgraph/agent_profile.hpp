#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/navgraph/access.hpp>
#include <carto/navgraph/identity.hpp>
#include <carto/navgraph/math_compare.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::navgraph {

// Neutral locomotion classes. These classify static geometry compatibility
// only; they carry no behavioral logic and no runtime handles.
enum class LocomotionClass : std::uint8_t {
    pedestrian,
    animal,
    vehicle,
    vessel,
};

[[nodiscard]] constexpr const char* locomotion_class_name(LocomotionClass kind) noexcept {
    switch (kind) {
    case LocomotionClass::pedestrian: return "pedestrian";
    case LocomotionClass::animal: return "animal";
    case LocomotionClass::vehicle: return "vehicle";
    case LocomotionClass::vessel: return "vessel";
    }
    return "unknown";
}

// Traversal primitives an agent profile statically supports. The set mirrors
// the neutral access primitives plus the ground/water/vessel surface modes so
// both structures and open mediums can be filtered consistently.
enum class TraversalPrimitive : std::uint8_t {
    ground,
    water,
    vessel_deck,
    ladder,
    hatch,
    portal,
};

[[nodiscard]] constexpr const char* traversal_primitive_name(TraversalPrimitive primitive) noexcept {
    switch (primitive) {
    case TraversalPrimitive::ground: return "ground";
    case TraversalPrimitive::water: return "water";
    case TraversalPrimitive::vessel_deck: return "vessel-deck";
    case TraversalPrimitive::ladder: return "ladder";
    case TraversalPrimitive::hatch: return "hatch";
    case TraversalPrimitive::portal: return "portal";
    }
    return "unknown";
}

// Neutral agent profile: static geometry compatibility for one agent class.
// Clearances are the agent's physical envelope; traversal primitives are the
// full set of traversal interfaces and surface modes the agent can use. Which
// compiled products the profile can actually traverse is derived by support
// checks, never stored as mutable per-product state.
//
// This record intentionally contains no dynamic cost or knowledge fields.
// Traversal cost, risk, and learned accessibility are game-owned runtime
// layers applied on top of these static products by the consumer.
struct AgentProfile {
    static constexpr std::size_t kMaxLabelBytes = 128U;
    static constexpr std::size_t kMaxPrimitives = 8U;

    std::string profile_identity;
    LocomotionClass locomotion = LocomotionClass::pedestrian;
    core::Bounds3d clearances;
    std::vector<TraversalPrimitive> primitives;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool supports(TraversalPrimitive primitive) const noexcept;
    [[nodiscard]] bool operator==(const AgentProfile& other) const noexcept {
        return profile_identity == other.profile_identity && locomotion == other.locomotion &&
            bounds_equal(clearances, other.clearances) && primitives == other.primitives;
    }
};

// Binding of an agent profile to one compiled region. An empty primitive list
// means the region filtering imposes no additional primitive constraint;
// explicit entries (for example vessel_deck for a compartment) refine the
// binding. The binding is neutral: no per-agent runtime occupancy or cost.
struct AgentAreaBinding {
    RegionId region;
    std::string profile_identity;
    std::vector<TraversalPrimitive> required_primitives;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const AgentAreaBinding&) const noexcept = default;
};

// Binding of an agent profile to one compiled route. The profile must support
// every primitive the route traversal requires; whether a route requires more
// than ground is classified from the route's authored semantic type.
struct AgentPathBinding {
    RouteId route;
    std::string profile_identity;
    std::vector<TraversalPrimitive> required_primitives;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const AgentPathBinding&) const noexcept = default;
};

// Maps an authored route semantic type onto the neutral traversal primitives
// its traversal requires. Unrecognized types require ground only.
[[nodiscard]] std::vector<TraversalPrimitive> classify_route_primitives(
    std::string_view semantic_type);

// Neutral primitive required to traverse one access interface. Doors are
// traversal-neutral: crossing a door requires ordinary ground traversal, while
// portals, ladders, and hatches each require their matching primitive.
[[nodiscard]] constexpr TraversalPrimitive traversal_primitive_for_access(
    AccessPrimitive primitive) noexcept {
    switch (primitive) {
    case AccessPrimitive::door: return TraversalPrimitive::ground;
    case AccessPrimitive::portal: return TraversalPrimitive::portal;
    case AccessPrimitive::ladder: return TraversalPrimitive::ladder;
    case AccessPrimitive::hatch: return TraversalPrimitive::hatch;
    }
    return TraversalPrimitive::ground;
}

// Static support decisions. These are pure functions of profile and product;
// the same inputs always produce the same decision.
[[nodiscard]] bool agent_supports_region(
    const AgentProfile& profile,
    const AgentAreaBinding& binding) noexcept;

[[nodiscard]] bool agent_supports_route(
    const AgentProfile& profile,
    const AgentPathBinding& binding) noexcept;

[[nodiscard]] bool agent_supports_access(
    const AgentProfile& profile,
    AccessPrimitive primitive) noexcept;

} // namespace carto::navgraph
