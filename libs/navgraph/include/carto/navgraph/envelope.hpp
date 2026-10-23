#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/navgraph/access.hpp>
#include <carto/navgraph/agent_profile.hpp>
#include <carto/navgraph/fence.hpp>
#include <carto/navgraph/identity.hpp>
#include <carto/navgraph/invalidation.hpp>
#include <carto/navgraph/links.hpp>
#include <carto/navgraph/region.hpp>
#include <carto/navgraph/route.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::navgraph {

// Neutral, digest-bound navigation envelope: the deterministic prepared
// product unifying compiled regions, routes, corridors, access points,
// off-mesh links, medium transitions, agent bindings, and invalidation
// footprints for one scoped world preparation.
//
// The envelope records static authored traversability only. It contains no
// runtime cost, occupancy, or knowledge fields, no destination engine handles,
// and no mutation authority. downstream consumer owns the runtime navigation index built
// from these products; games own the dynamic cost and knowledge layers. This
// is enforced at compile time: the fence traits below assert that no member of
// the public navigation records can carry a dynamic traversal cost.
struct PreparedNavigationEnvelope {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecordsPerKind = 65'536U;
    static constexpr std::size_t kMaxPrimitivesPerProfile = 8U;

    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    // Optional scene snapshot identity for correspondence. Scene content is
    // never embedded; the revision and identity are stable references only.
    std::optional<scene::ObjectId> scene_root;

    std::vector<SemanticAreaRegion> regions;
    std::vector<SemanticRouteProduct> routes;
    std::vector<SemanticCorridorProduct> corridors;
    std::vector<StructureAccessProduct> access_points;
    std::vector<TraversalAnchor> anchors;
    std::vector<OffMeshLinkCandidate> links;
    std::vector<MediumTransition> medium_transitions;
    std::vector<AgentProfile> agent_profiles;
    std::vector<AgentAreaBinding> area_bindings;
    std::vector<AgentPathBinding> path_bindings;
    std::vector<NavigationTileFootprint> footprints;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PreparedNavigationEnvelope> deserialize(
        std::string_view text);
};

} // namespace carto::navgraph

// Compile-time fence: the dynamic traversal-cost layer is game-owned and must
// never appear on Cartographer navigation records. These assertions fail the
// build if any public record grows a dynamic-cost, dynamic-knowledge, or
// runtime-handle member.
static_assert(
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::SemanticAreaRegion>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::SemanticRouteProduct>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::SemanticCorridorProduct>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::StructureAccessProduct>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::TraversalAnchor>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::OffMeshLinkCandidate>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::MediumTransition>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::AgentProfile>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::AgentAreaBinding>::value &&
    !carto::navgraph::kHasDynamicTraversalCost<carto::navgraph::AgentPathBinding>::value,
    "navigation records must not carry dynamic cost or knowledge fields");
