#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/navgraph/identity.hpp>
#include <carto/navgraph/math_compare.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::navgraph {

// Typed traversal interfaces authored on structure-graph nodes. The authored
// semantic type is preserved verbatim so games keep their domain vocabulary;
// the primitive classification is the neutral, versioned interpretation that
// link compilation and agent support checks consume.
enum class AccessPrimitive : std::uint8_t {
    door,
    portal,
    ladder,
    hatch,
};

[[nodiscard]] constexpr const char* access_primitive_name(AccessPrimitive primitive) noexcept {
    switch (primitive) {
    case AccessPrimitive::door: return "door";
    case AccessPrimitive::portal: return "portal";
    case AccessPrimitive::ladder: return "ladder";
    case AccessPrimitive::hatch: return "hatch";
    }
    return "unknown";
}

// Maps an authored structure-node semantic type to its neutral access
// primitive. Authored vocabulary not recognized as a traversal interface
// yields std::nullopt and is compiled as a passive structure node instead.
[[nodiscard]] constexpr std::optional<AccessPrimitive> classify_access_primitive(
    std::string_view semantic_type) noexcept {
    if (semantic_type == "door") return AccessPrimitive::door;
    if (semantic_type == "portal") return AccessPrimitive::portal;
    if (semantic_type == "ladder") return AccessPrimitive::ladder;
    if (semantic_type == "hatch") return AccessPrimitive::hatch;
    return std::nullopt;
}

// Neutral access point compiled from one world::StructureNode. A node
// classifies as an access point when its authored semantic type names a known
// traversal interface (door/portal/ladder/hatch); passive nodes do not produce
// access records. The owning structure-graph identity is retained so the
// record traces back to stable source identity even though the access identity
// itself derives from the node identity.
struct StructureAccessProduct {
    static constexpr std::size_t kMaxSemanticBytes = 128U;

    AccessId id;
    SourceStructureGraphId source_graph;
    std::uint64_t source_node = 0U;
    AccessPrimitive primitive = AccessPrimitive::door;
    std::string semantic_type;
    std::optional<SourceAreaId> area;
    core::Vec3d position{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const StructureAccessProduct& other) const noexcept {
        return id == other.id && source_graph == other.source_graph &&
            source_node == other.source_node && primitive == other.primitive &&
            semantic_type == other.semantic_type && area == other.area &&
            vec3_equal(position, other.position);
    }
};

// Compiles one validated structure graph into its neutral access-point list.
// Output is ordered by ascending source node identity so the compiled product
// is byte-stable regardless of map iteration order. Passive nodes are omitted.
[[nodiscard]] core::Result<std::vector<StructureAccessProduct>> compile_access_points(
    const world::StructureGraph& graph);

} // namespace carto::navgraph
