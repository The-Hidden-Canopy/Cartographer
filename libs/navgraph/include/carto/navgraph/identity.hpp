#pragma once

#include <carto/world/model.hpp>

#include <cstdint>

namespace carto::navgraph {

// Strong identities for compiled navigation records. Source identities always
// reference the authoritative world:: records; compiled identities are stable,
// deterministic derivations from them and never reuse source value ranges.
struct RegionTag;
struct RouteTag;
struct CorridorTag;
struct AccessTag;
struct LinkTag;
struct AnchorTag;

using RegionId = world::Id<RegionTag>;
using RouteId = world::Id<RouteTag>;
using CorridorId = world::Id<CorridorTag>;
using AccessId = world::Id<AccessTag>;
using LinkId = world::Id<LinkTag>;
using AnchorId = world::Id<AnchorTag>;

// Source identities retained verbatim for correspondence back to authoring
// truth. These are world::Id instantiations over the world model's own tags.
using SourceAreaId = world::AreaId;
using SourcePathId = world::PathId;
using SourceStructureGraphId = world::StructureGraphId;

// Deterministic compiled identities derived from stable source identities.
// The derivation is a pure projection: equal source identities always produce
// equal compiled identities, and compiled identities never collide with the
// source value range because the high bit is forced on.
[[nodiscard]] constexpr std::uint64_t derive_compiled_identity(
    std::uint64_t source_value) noexcept {
    return source_value | (1ULL << 63U);
}

[[nodiscard]] constexpr RegionId derive_region_id(SourceAreaId source) noexcept {
    return RegionId{derive_compiled_identity(source.value)};
}

[[nodiscard]] constexpr RouteId derive_route_id(SourcePathId source) noexcept {
    return RouteId{derive_compiled_identity(source.value)};
}

[[nodiscard]] constexpr CorridorId derive_corridor_id(SourcePathId source) noexcept {
    return CorridorId{derive_compiled_identity(source.value)};
}

// Access points compile from structure-graph node identities; the graph scope
// is carried by the owning product so node identity alone is the stable key.
[[nodiscard]] constexpr AccessId derive_access_id(std::uint64_t node_identity) noexcept {
    return AccessId{derive_compiled_identity(node_identity)};
}

[[nodiscard]] constexpr LinkId derive_link_id(std::uint64_t ordinal) noexcept {
    return LinkId{derive_compiled_identity(ordinal)};
}

[[nodiscard]] constexpr AnchorId derive_anchor_id(std::uint64_t ordinal) noexcept {
    return AnchorId{derive_compiled_identity(ordinal)};
}

} // namespace carto::navgraph
