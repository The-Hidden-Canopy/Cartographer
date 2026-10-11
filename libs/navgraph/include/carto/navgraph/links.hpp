#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/navgraph/access.hpp>
#include <carto/navgraph/identity.hpp>
#include <carto/navgraph/math_compare.hpp>
#include <carto/navgraph/port_reference.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::navgraph {

// Traversal medium of a compiled link or medium-boundary crossing. The medium
// is static authored classification; current conditions (flooded, iced, locked
// down) are game-owned runtime state and never appear on these records.
enum class TraversalMedium : std::uint8_t {
    land,
    water,
    vessel,
    building,
};

[[nodiscard]] constexpr const char* traversal_medium_name(TraversalMedium medium) noexcept {
    switch (medium) {
    case TraversalMedium::land: return "land";
    case TraversalMedium::water: return "water";
    case TraversalMedium::vessel: return "vessel";
    case TraversalMedium::building: return "building";
    }
    return "unknown";
}

// Anchored traversal endpoint. An anchor is a neutral, world-frame point with a
// classified medium; it is derived from compiled products (access points,
// region centroids, route terminals) and owns no handle or runtime state.
struct TraversalAnchor {
    static constexpr std::size_t kMaxLabelBytes = 128U;

    AnchorId id;
    TraversalMedium medium = TraversalMedium::land;
    core::Vec3d position{};
    std::optional<RegionId> region;
    std::optional<AccessId> access;
    std::string label;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const TraversalAnchor& other) const noexcept {
        return id == other.id && medium == other.medium &&
            vec3_equal(position, other.position) && region == other.region &&
            access == other.access && label == other.label;
    }
};

// Neutral off-mesh link candidate compiled from compatible ports and/or
// anchors. Static traversability only: the record states that the connection
// statically exists and which traversal primitive and mediums it spans. It
// does not state whether the link is currently passable; that is runtime/game
// state layered on top by the consumer.
struct OffMeshLinkCandidate {
    LinkId id;
    AccessPrimitive primitive = AccessPrimitive::portal;
    AnchorId start_anchor;
    AnchorId end_anchor;
    TraversalMedium start_medium = TraversalMedium::land;
    TraversalMedium end_medium = TraversalMedium::land;
    std::optional<std::string> start_port_identity;
    std::optional<std::string> end_port_identity;
    double length = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const OffMeshLinkCandidate&) const noexcept = default;
};

// Neutral medium-continuity record for land/water/vessel/building transitions.
// Compiled wherever a link crosses a medium boundary (gangway, dock, deck
// hatch); it exists so consumers can preserve traversal continuity across the
// boundary without consulting runtime state.
struct MediumTransition {
    LinkId link;
    TraversalMedium from_medium = TraversalMedium::land;
    TraversalMedium to_medium = TraversalMedium::land;
    AccessPrimitive primitive = AccessPrimitive::portal;
    std::optional<std::string> start_port_identity;
    std::optional<std::string> end_port_identity;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const MediumTransition&) const noexcept = default;
};

// Compiles off-mesh link candidates from ports and anchors. Every port must
// reference a resolved anchor; a port pair compiles into a link candidate only
// when at least one of a shared compatibility tag or a medium-boundary
// crossing is present, and duplicate pairs fail closed. The caller supplies
// anchor ordinals; derived link and anchor identities are deterministic
// functions of input order.
inline constexpr std::size_t kMaxOffMeshLinkInputs = 65'536U;

[[nodiscard]] core::Result<std::vector<OffMeshLinkCandidate>> compile_offmesh_links(
    const std::vector<PortReference>& ports,
    const std::vector<TraversalAnchor>& anchors,
    const std::vector<std::uint64_t>& port_anchor_ordinals);

// Projects link candidates onto their medium boundary, producing one record
// per candidate whose start and end mediums differ. Pure projection: link
// order and identity are preserved.
[[nodiscard]] core::Result<std::vector<MediumTransition>> compile_medium_transitions(
    const std::vector<OffMeshLinkCandidate>& links);

} // namespace carto::navgraph
