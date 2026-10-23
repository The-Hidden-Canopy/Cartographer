#include <carto/condition/district_topology.hpp>

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <tuple>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool nonblank(const std::string& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' &&
            ch != '\v' && ch != '\f';
    });
}

bool valid_elevation(DistrictElevationRelation relation) noexcept {
    switch (relation) {
    case DistrictElevationRelation::unresolved:
    case DistrictElevationRelation::equal:
    case DistrictElevationRelation::first_higher:
    case DistrictElevationRelation::second_higher:
        return true;
    }
    return false;
}

bool valid_hierarchy(DistrictRouteHierarchy hierarchy) noexcept {
    switch (hierarchy) {
    case DistrictRouteHierarchy::local:
    case DistrictRouteHierarchy::collector:
    case DistrictRouteHierarchy::arterial:
    case DistrictRouteHierarchy::regional:
        return true;
    }
    return false;
}

bool valid_route_state(DistrictRouteState state) noexcept {
    switch (state) {
    case DistrictRouteState::planned:
    case DistrictRouteState::active:
    case DistrictRouteState::out_of_service:
        return true;
    }
    return false;
}

bool valid_path_kind(DistrictPathKind kind) noexcept {
    return kind == DistrictPathKind::adjacency || kind == DistrictPathKind::active_route;
}

void normalize_adjacency(std::vector<DistrictAdjacency>& values) {
    for (auto& value : values) {
        if (value.second < value.first) {
            std::swap(value.first, value.second);
            if (value.elevation == DistrictElevationRelation::first_higher) {
                value.elevation = DistrictElevationRelation::second_higher;
            } else if (value.elevation == DistrictElevationRelation::second_higher) {
                value.elevation = DistrictElevationRelation::first_higher;
            }
        }
    }
}

void normalize_routes(std::vector<DistrictRoute>& values) {
    for (auto& value : values) {
        if (value.second < value.first) std::swap(value.first, value.second);
    }
}

void sort_records(
    std::vector<DistrictRegion>& regions,
    std::vector<DistrictAdjacency>& adjacency,
    std::vector<DistrictRoute>& routes,
    std::vector<DistrictConnectivityConstraint>& constraints) {
    std::sort(regions.begin(), regions.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(adjacency.begin(), adjacency.end(), [](const auto& left, const auto& right) {
        return std::tie(left.first, left.second) < std::tie(right.first, right.second);
    });
    std::sort(routes.begin(), routes.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(constraints.begin(), constraints.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    for (auto& constraint : constraints) {
        std::sort(constraint.region_ids.begin(), constraint.region_ids.end());
    }
}

core::Result<bool> validate_records(
    const std::vector<DistrictRegion>& regions,
    const std::vector<DistrictAdjacency>& adjacency,
    const std::vector<DistrictRoute>& routes,
    const std::vector<DistrictConnectivityConstraint>& constraints) {
    std::unordered_set<DistrictRegionId> region_ids;
    region_ids.reserve(regions.size());
    for (const auto& region : regions) {
        if (region.id == 0U || !region_ids.insert(region.id).second ||
            (region.land_use_identity && !nonblank(*region.land_use_identity)) ||
            (region.maintenance_identity && !nonblank(*region.maintenance_identity))) {
            return core::Result<bool>::failure(invalid(
                "district region identity or optional classification is invalid"));
        }
    }

    std::set<std::pair<DistrictRegionId, DistrictRegionId>> adjacency_pairs;
    for (const auto& edge : adjacency) {
        if (edge.first == 0U || edge.second == 0U || edge.first == edge.second ||
            !region_ids.contains(edge.first) || !region_ids.contains(edge.second) ||
            !valid_elevation(edge.elevation) ||
            !adjacency_pairs.emplace(edge.first, edge.second).second) {
            return core::Result<bool>::failure(invalid(
                "district adjacency has invalid, missing, or duplicate endpoints"));
        }
    }

    std::unordered_set<DistrictRouteId> route_ids;
    route_ids.reserve(routes.size());
    for (const auto& route : routes) {
        if (route.id == 0U || !route_ids.insert(route.id).second ||
            route.first == 0U || route.second == 0U || route.first == route.second ||
            !region_ids.contains(route.first) || !region_ids.contains(route.second) ||
            !valid_hierarchy(route.hierarchy) || !valid_route_state(route.state)) {
            return core::Result<bool>::failure(invalid(
                "district route identity, hierarchy, state, or endpoints are invalid"));
        }
    }

    std::unordered_set<DistrictConstraintId> constraint_ids;
    constraint_ids.reserve(constraints.size());
    for (const auto& constraint : constraints) {
        if (constraint.id == 0U || !constraint_ids.insert(constraint.id).second ||
            !valid_path_kind(constraint.path_kind) || constraint.region_ids.size() < 2U ||
            !std::is_sorted(constraint.region_ids.begin(), constraint.region_ids.end()) ||
            std::adjacent_find(constraint.region_ids.begin(), constraint.region_ids.end()) !=
                constraint.region_ids.end()) {
            return core::Result<bool>::failure(invalid(
                "district connectivity constraint identity, path, or region set is invalid"));
        }
        for (const auto id : constraint.region_ids) {
            if (!region_ids.contains(id)) {
                return core::Result<bool>::failure(invalid(
                    "district connectivity constraint references a missing region"));
            }
        }
    }
    return core::Result<bool>::success(true);
}

using Components = std::vector<std::vector<DistrictRegionId>>;

Components compute_components(
    const std::vector<DistrictRegion>& regions,
    const std::vector<std::pair<DistrictRegionId, DistrictRegionId>>& edges,
    bool active_routes_only,
    const std::vector<DistrictRoute>* routes = nullptr) {
    std::unordered_map<DistrictRegionId, std::vector<DistrictRegionId>> graph;
    graph.reserve(regions.size());
    for (const auto& region : regions) graph.emplace(region.id, std::vector<DistrictRegionId>{});
    if (active_routes_only && routes != nullptr) {
        for (const auto& route : *routes) {
            if (route.state != DistrictRouteState::active) continue;
            graph[route.first].push_back(route.second);
            graph[route.second].push_back(route.first);
        }
    } else {
        for (const auto& [first, second] : edges) {
            graph[first].push_back(second);
            graph[second].push_back(first);
        }
    }
    for (auto& [id, neighbors] : graph) {
        static_cast<void>(id);
        std::sort(neighbors.begin(), neighbors.end());
    }

    Components components;
    std::unordered_set<DistrictRegionId> visited;
    for (const auto& region : regions) {
        if (!visited.insert(region.id).second) continue;
        std::queue<DistrictRegionId> pending;
        pending.push(region.id);
        std::vector<DistrictRegionId> component;
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop();
            component.push_back(current);
            for (const auto neighbor : graph[current]) {
                if (visited.insert(neighbor).second) pending.push(neighbor);
            }
        }
        std::sort(component.begin(), component.end());
        components.push_back(std::move(component));
    }
    return components;
}

std::unordered_map<DistrictRegionId, std::size_t> component_index(const Components& components) {
    std::unordered_map<DistrictRegionId, std::size_t> result;
    for (std::size_t index = 0U; index < components.size(); ++index) {
        for (const auto id : components[index]) result.emplace(id, index);
    }
    return result;
}

std::vector<DistrictRegionId> sorted_ids(const std::unordered_set<DistrictRegionId>& ids) {
    std::vector<DistrictRegionId> result(ids.begin(), ids.end());
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace

DistrictTopology::DistrictTopology(DistrictTopologyId id,
    std::vector<DistrictRegion> regions,
    std::vector<DistrictAdjacency> adjacency,
    std::vector<DistrictRoute> routes,
    std::vector<DistrictConnectivityConstraint> constraints)
    : id_(id), regions_(std::move(regions)), adjacency_(std::move(adjacency)),
      routes_(std::move(routes)), constraints_(std::move(constraints)) {
    sort_records(regions_, adjacency_, routes_, constraints_);
}

core::Result<DistrictTopology> DistrictTopology::create(
    DistrictTopologyId id,
    std::vector<DistrictRegion> regions,
    std::vector<DistrictAdjacency> adjacency,
    std::vector<DistrictRoute> routes,
    std::vector<DistrictConnectivityConstraint> constraints) {
    if (id == 0U) {
        return core::Result<DistrictTopology>::failure(invalid(
            "district topology identity must be non-zero"));
    }
    normalize_adjacency(adjacency);
    normalize_routes(routes);
    sort_records(regions, adjacency, routes, constraints);
    auto valid = validate_records(regions, adjacency, routes, constraints);
    if (!valid) return core::Result<DistrictTopology>::failure(valid.error());
    return core::Result<DistrictTopology>::success(DistrictTopology(id,
        std::move(regions), std::move(adjacency), std::move(routes),
        std::move(constraints)));
}

core::Result<DistrictTopologyEditReceipt> DistrictTopology::apply(
    const DistrictTopologyEdit& edit) {
    if (edit.expected_revision != revision_) {
        return core::Result<DistrictTopologyEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "district topology edit revision no longer matches the source"));
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<DistrictTopologyEditReceipt>::failure(invalid(
            "district topology revision is exhausted"));
    }

    auto next_regions = regions_;
    auto next_adjacency = adjacency_;
    auto next_routes = routes_;
    auto next_constraints = constraints_;
    std::unordered_set<DistrictRegionId> changed;

    const auto applied = std::visit([&](const auto& operation) -> core::Result<bool> {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, DistrictUpsertRegion>) {
            const auto found = std::find_if(next_regions.begin(), next_regions.end(),
                [&](const auto& region) { return region.id == operation.region.id; });
            if (found == next_regions.end()) {
                next_regions.push_back(operation.region);
            } else if (*found == operation.region) {
                return core::Result<bool>::failure(invalid(
                    "district topology edit does not change its source"));
            } else {
                *found = operation.region;
            }
            changed.insert(operation.region.id);
        } else if constexpr (std::is_same_v<Operation, DistrictRemoveRegion>) {
            const auto found = std::find_if(next_regions.begin(), next_regions.end(),
                [&](const auto& region) { return region.id == operation.id; });
            if (found == next_regions.end()) {
                return core::Result<bool>::failure(core::Diagnostic(
                    core::ErrorCode::not_found, "district region was not found"));
            }
            const auto constrained = std::any_of(next_constraints.begin(), next_constraints.end(),
                [&](const auto& constraint) {
                    return std::find(constraint.region_ids.begin(), constraint.region_ids.end(),
                        operation.id) != constraint.region_ids.end();
                });
            if (constrained) {
                return core::Result<bool>::failure(invalid(
                    "district region is referenced by a connectivity constraint"));
            }
            changed.insert(operation.id);
            next_regions.erase(found);
            std::erase_if(next_adjacency, [&](const auto& edge) {
                if (edge.first != operation.id && edge.second != operation.id) return false;
                changed.insert(edge.first);
                changed.insert(edge.second);
                return true;
            });
            std::erase_if(next_routes, [&](const auto& route) {
                if (route.first != operation.id && route.second != operation.id) return false;
                changed.insert(route.first);
                changed.insert(route.second);
                return true;
            });
        } else if constexpr (std::is_same_v<Operation, DistrictUpsertAdjacency>) {
            auto desired = operation.adjacency;
            std::vector<DistrictAdjacency> normalized{desired};
            normalize_adjacency(normalized);
            desired = normalized.front();
            const auto found = std::find_if(next_adjacency.begin(), next_adjacency.end(),
                [&](const auto& edge) {
                    return edge.first == desired.first && edge.second == desired.second;
                });
            if (found == next_adjacency.end()) next_adjacency.push_back(desired);
            else if (*found == desired) {
                return core::Result<bool>::failure(invalid(
                    "district topology edit does not change its source"));
            } else *found = desired;
            changed.insert(desired.first);
            changed.insert(desired.second);
        } else if constexpr (std::is_same_v<Operation, DistrictRemoveAdjacency>) {
            const auto low = std::min(operation.first, operation.second);
            const auto high = std::max(operation.first, operation.second);
            const auto found = std::find_if(next_adjacency.begin(), next_adjacency.end(),
                [&](const auto& edge) { return edge.first == low && edge.second == high; });
            if (found == next_adjacency.end()) {
                return core::Result<bool>::failure(core::Diagnostic(
                    core::ErrorCode::not_found, "district adjacency was not found"));
            }
            changed.insert(found->first);
            changed.insert(found->second);
            next_adjacency.erase(found);
        } else if constexpr (std::is_same_v<Operation, DistrictUpsertRoute>) {
            auto desired = operation.route;
            if (desired.second < desired.first) std::swap(desired.first, desired.second);
            const auto found = std::find_if(next_routes.begin(), next_routes.end(),
                [&](const auto& route) { return route.id == desired.id; });
            if (found == next_routes.end()) next_routes.push_back(desired);
            else if (*found == desired) {
                return core::Result<bool>::failure(invalid(
                    "district topology edit does not change its source"));
            } else {
                changed.insert(found->first);
                changed.insert(found->second);
                *found = desired;
            }
            changed.insert(desired.first);
            changed.insert(desired.second);
        } else if constexpr (std::is_same_v<Operation, DistrictRemoveRoute>) {
            const auto found = std::find_if(next_routes.begin(), next_routes.end(),
                [&](const auto& route) { return route.id == operation.id; });
            if (found == next_routes.end()) {
                return core::Result<bool>::failure(core::Diagnostic(
                    core::ErrorCode::not_found, "district route was not found"));
            }
            changed.insert(found->first);
            changed.insert(found->second);
            next_routes.erase(found);
        } else if constexpr (std::is_same_v<Operation, DistrictUpsertConstraint>) {
            auto desired = operation.constraint;
            std::sort(desired.region_ids.begin(), desired.region_ids.end());
            const auto found = std::find_if(next_constraints.begin(), next_constraints.end(),
                [&](const auto& constraint) { return constraint.id == desired.id; });
            if (found == next_constraints.end()) next_constraints.push_back(desired);
            else if (*found == desired) {
                return core::Result<bool>::failure(invalid(
                    "district topology edit does not change its source"));
            } else {
                changed.insert(found->region_ids.begin(), found->region_ids.end());
                *found = desired;
            }
            changed.insert(desired.region_ids.begin(), desired.region_ids.end());
        } else if constexpr (std::is_same_v<Operation, DistrictRemoveConstraint>) {
            const auto found = std::find_if(next_constraints.begin(), next_constraints.end(),
                [&](const auto& constraint) { return constraint.id == operation.id; });
            if (found == next_constraints.end()) {
                return core::Result<bool>::failure(core::Diagnostic(
                    core::ErrorCode::not_found, "district connectivity constraint was not found"));
            }
            changed.insert(found->region_ids.begin(), found->region_ids.end());
            next_constraints.erase(found);
        }
        return core::Result<bool>::success(true);
    }, edit.operation);
    if (!applied) return core::Result<DistrictTopologyEditReceipt>::failure(applied.error());

    normalize_adjacency(next_adjacency);
    normalize_routes(next_routes);
    sort_records(next_regions, next_adjacency, next_routes, next_constraints);
    auto valid = validate_records(next_regions, next_adjacency, next_routes, next_constraints);
    if (!valid) return core::Result<DistrictTopologyEditReceipt>::failure(valid.error());

    DistrictTopologyEditReceipt receipt;
    receipt.topology_id = id_;
    receipt.revision_before = revision_;
    receipt.revision_after = revision_ + 1U;
    receipt.changed_region_ids = sorted_ids(changed);
    regions_ = std::move(next_regions);
    adjacency_ = std::move(next_adjacency);
    routes_ = std::move(next_routes);
    constraints_ = std::move(next_constraints);
    revision_ = receipt.revision_after;
    return core::Result<DistrictTopologyEditReceipt>::success(std::move(receipt));
}

core::Result<DistrictTopologyReport> DistrictTopology::inspect() const {
    auto valid = validate_records(regions_, adjacency_, routes_, constraints_);
    if (!valid) return core::Result<DistrictTopologyReport>::failure(valid.error());

    DistrictTopologyReport report;
    report.topology_id = id_;
    report.revision = revision_;
    std::vector<std::pair<DistrictRegionId, DistrictRegionId>> adjacency_edges;
    adjacency_edges.reserve(adjacency_.size());
    for (const auto& edge : adjacency_) adjacency_edges.emplace_back(edge.first, edge.second);
    report.adjacency_components = compute_components(regions_, adjacency_edges, false);
    report.active_route_components = compute_components(regions_, {}, true, &routes_);
    const auto adjacency_index = component_index(report.adjacency_components);
    const auto route_index = component_index(report.active_route_components);
    for (const auto& constraint : constraints_) {
        const auto& index = constraint.path_kind == DistrictPathKind::adjacency
            ? adjacency_index : route_index;
        const auto first = index.at(constraint.region_ids.front());
        const bool connected = std::all_of(constraint.region_ids.begin() + 1,
            constraint.region_ids.end(), [&](DistrictRegionId id) {
                return index.at(id) == first;
            });
        if (!connected) report.unsatisfied_constraints.push_back(constraint.id);
    }
    std::sort(report.unsatisfied_constraints.begin(), report.unsatisfied_constraints.end());
    return core::Result<DistrictTopologyReport>::success(std::move(report));
}

} // namespace carto::condition
