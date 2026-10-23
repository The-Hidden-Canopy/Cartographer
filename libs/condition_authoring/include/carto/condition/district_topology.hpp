#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace carto::condition {

using DistrictTopologyId = std::uint64_t;
using DistrictRegionId = std::uint64_t;
using DistrictRouteId = std::uint64_t;
using DistrictConstraintId = std::uint64_t;

enum class DistrictElevationRelation : std::uint8_t {
    unresolved,
    equal,
    first_higher,
    second_higher,
};

enum class DistrictRouteHierarchy : std::uint8_t {
    local,
    collector,
    arterial,
    regional,
};

enum class DistrictRouteState : std::uint8_t {
    planned,
    active,
    out_of_service,
};

enum class DistrictPathKind : std::uint8_t {
    adjacency,
    active_route,
};

struct DistrictRegion {
    DistrictRegionId id = 0U;
    std::optional<std::string> land_use_identity;
    std::optional<std::string> maintenance_identity;
    friend bool operator==(const DistrictRegion&, const DistrictRegion&) = default;
};

// Adjacency and elevation are authored claims. This graph validates their
// references and preserves unresolved relationships; it does not derive
// physical adjacency or elevation from geometry.
struct DistrictAdjacency {
    DistrictRegionId first = 0U;
    DistrictRegionId second = 0U;
    DistrictElevationRelation elevation = DistrictElevationRelation::unresolved;
    friend bool operator==(const DistrictAdjacency&, const DistrictAdjacency&) = default;
};

struct DistrictRoute {
    DistrictRouteId id = 0U;
    DistrictRegionId first = 0U;
    DistrictRegionId second = 0U;
    DistrictRouteHierarchy hierarchy = DistrictRouteHierarchy::local;
    DistrictRouteState state = DistrictRouteState::planned;
    friend bool operator==(const DistrictRoute&, const DistrictRoute&) = default;
};

struct DistrictConnectivityConstraint {
    DistrictConstraintId id = 0U;
    DistrictPathKind path_kind = DistrictPathKind::adjacency;
    std::vector<DistrictRegionId> region_ids;
    friend bool operator==(const DistrictConnectivityConstraint&,
        const DistrictConnectivityConstraint&) = default;
};

struct DistrictUpsertRegion { DistrictRegion region; };
struct DistrictRemoveRegion { DistrictRegionId id = 0U; };
struct DistrictUpsertAdjacency { DistrictAdjacency adjacency; };
struct DistrictRemoveAdjacency { DistrictRegionId first = 0U; DistrictRegionId second = 0U; };
struct DistrictUpsertRoute { DistrictRoute route; };
struct DistrictRemoveRoute { DistrictRouteId id = 0U; };
struct DistrictUpsertConstraint { DistrictConnectivityConstraint constraint; };
struct DistrictRemoveConstraint { DistrictConstraintId id = 0U; };

struct DistrictTopologyEdit {
    std::uint64_t expected_revision = 0U;
    std::variant<DistrictUpsertRegion, DistrictRemoveRegion,
        DistrictUpsertAdjacency, DistrictRemoveAdjacency,
        DistrictUpsertRoute, DistrictRemoveRoute,
        DistrictUpsertConstraint, DistrictRemoveConstraint> operation;
};

struct DistrictTopologyEditReceipt {
    DistrictTopologyId topology_id = 0U;
    std::uint64_t revision_before = 0U;
    std::uint64_t revision_after = 0U;
    std::vector<DistrictRegionId> changed_region_ids;
};

struct DistrictTopologyReport {
    DistrictTopologyId topology_id = 0U;
    std::uint64_t revision = 0U;
    std::vector<std::vector<DistrictRegionId>> adjacency_components;
    std::vector<std::vector<DistrictRegionId>> active_route_components;
    std::vector<DistrictConstraintId> unsatisfied_constraints;
    [[nodiscard]] bool constraints_passed() const noexcept {
        return unsatisfied_constraints.empty();
    }
};

// Stable region IDs are supplied by the author and are never renumbered by
// local edits. This graph is an authoring model, not spatial or publication
// authority; adjacency and elevation remain explicit authored relationships.
class DistrictTopology final {
public:
    [[nodiscard]] static core::Result<DistrictTopology> create(
        DistrictTopologyId id,
        std::vector<DistrictRegion> regions = {},
        std::vector<DistrictAdjacency> adjacency = {},
        std::vector<DistrictRoute> routes = {},
        std::vector<DistrictConnectivityConstraint> constraints = {});

    [[nodiscard]] DistrictTopologyId id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const std::vector<DistrictRegion>& regions() const noexcept { return regions_; }
    [[nodiscard]] const std::vector<DistrictAdjacency>& adjacency() const noexcept { return adjacency_; }
    [[nodiscard]] const std::vector<DistrictRoute>& routes() const noexcept { return routes_; }
    [[nodiscard]] const std::vector<DistrictConnectivityConstraint>& constraints() const noexcept {
        return constraints_;
    }

    [[nodiscard]] core::Result<DistrictTopologyEditReceipt> apply(
        const DistrictTopologyEdit& edit);
    [[nodiscard]] core::Result<DistrictTopologyReport> inspect() const;

private:
    DistrictTopology(DistrictTopologyId id,
        std::vector<DistrictRegion> regions,
        std::vector<DistrictAdjacency> adjacency,
        std::vector<DistrictRoute> routes,
        std::vector<DistrictConnectivityConstraint> constraints);

    DistrictTopologyId id_ = 0U;
    std::uint64_t revision_ = 1U;
    std::vector<DistrictRegion> regions_;
    std::vector<DistrictAdjacency> adjacency_;
    std::vector<DistrictRoute> routes_;
    std::vector<DistrictConnectivityConstraint> constraints_;
};

} // namespace carto::condition
