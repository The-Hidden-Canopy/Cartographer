#include <carto/condition/utility_network.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool valid_kind(UtilityNetworkKind kind) noexcept {
    switch (kind) {
    case UtilityNetworkKind::power:
    case UtilityNetworkKind::water:
    case UtilityNetworkKind::fuel:
    case UtilityNetworkKind::communications:
        return true;
    }
    return false;
}

bool valid_node_kind(UtilityNodeKind kind) noexcept {
    switch (kind) {
    case UtilityNodeKind::source:
    case UtilityNodeKind::sink:
    case UtilityNodeKind::junction:
    case UtilityNodeKind::service_connection:
        return true;
    }
    return false;
}

bool valid_state(UtilityNodeState state) noexcept {
    switch (state) {
    case UtilityNodeState::planned:
    case UtilityNodeState::active:
    case UtilityNodeState::out_of_service:
        return true;
    }
    return false;
}

bool valid_capacity_class(const std::string& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return !std::isspace(character);
    });
}

core::Result<bool> validate_nodes_and_links(
    const std::vector<UtilityNetworkNode>& nodes,
    const std::vector<UtilityNetworkLink>& links) {
    std::unordered_set<UtilityNodeId> node_ids;
    node_ids.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (node.id == 0U || !node_ids.insert(node.id).second ||
            !valid_node_kind(node.kind) || !valid_state(node.state) ||
            !valid_capacity_class(node.capacity_class)) {
            return core::Result<bool>::failure(invalid(
                "utility network node identity, type, state, or capacity class is invalid"));
        }
    }

    std::unordered_set<UtilityLinkId> link_ids;
    std::set<std::pair<UtilityNodeId, UtilityNodeId>> endpoint_pairs;
    link_ids.reserve(links.size());
    for (const auto& link : links) {
        const auto pair = std::minmax(link.first, link.second);
        if (link.id == 0U || !link_ids.insert(link.id).second ||
            link.first == link.second || !node_ids.contains(link.first) ||
            !node_ids.contains(link.second) || !valid_state(link.state) ||
            !endpoint_pairs.emplace(pair.first, pair.second).second) {
            return core::Result<bool>::failure(invalid(
                "utility network link identity, endpoints, state, or uniqueness is invalid"));
        }
    }
    return core::Result<bool>::success(true);
}

using Adjacency = std::unordered_map<UtilityNodeId, std::vector<UtilityNodeId>>;

Adjacency make_adjacency(
    const std::vector<UtilityNetworkNode>& nodes,
    const std::vector<UtilityNetworkLink>& links,
    bool active_only) {
    Adjacency adjacency;
    adjacency.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (!active_only || node.state == UtilityNodeState::active) {
            adjacency.emplace(node.id, std::vector<UtilityNodeId>{});
        }
    }
    for (const auto& link : links) {
        if (active_only && link.state != UtilityNodeState::active) continue;
        const auto first = adjacency.find(link.first);
        const auto second = adjacency.find(link.second);
        if (first == adjacency.end() || second == adjacency.end()) continue;
        first->second.push_back(link.second);
        second->second.push_back(link.first);
    }
    for (auto& [id, neighbors] : adjacency) {
        static_cast<void>(id);
        std::sort(neighbors.begin(), neighbors.end());
    }
    return adjacency;
}

std::unordered_set<UtilityNodeId> component_for(
    UtilityNodeId start,
    const Adjacency& adjacency) {
    std::unordered_set<UtilityNodeId> visited;
    if (!adjacency.contains(start)) return visited;
    std::queue<UtilityNodeId> pending;
    pending.push(start);
    visited.insert(start);
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop();
        const auto neighbors = adjacency.find(current);
        if (neighbors == adjacency.end()) continue;
        for (const auto neighbor : neighbors->second) {
            if (visited.insert(neighbor).second) pending.push(neighbor);
        }
    }
    return visited;
}

std::vector<UtilityNodeId> sorted_ids(
    const std::unordered_set<UtilityNodeId>& ids) {
    std::vector<UtilityNodeId> result(ids.begin(), ids.end());
    std::sort(result.begin(), result.end());
    return result;
}

void normalize_links(std::vector<UtilityNetworkLink>& links) {
    for (auto& link : links) {
        if (link.second < link.first) std::swap(link.first, link.second);
    }
}

} // namespace

UtilityNetwork::UtilityNetwork(UtilityNetworkId id, UtilityNetworkKind kind,
    std::vector<UtilityNetworkNode> nodes,
    std::vector<UtilityNetworkLink> links) noexcept
    : id_(id), kind_(kind), nodes_(std::move(nodes)), links_(std::move(links)) {
    std::sort(nodes_.begin(), nodes_.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(links_.begin(), links_.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
}

core::Result<UtilityNetwork> UtilityNetwork::create(
    UtilityNetworkId id,
    UtilityNetworkKind kind,
    std::vector<UtilityNetworkNode> nodes,
    std::vector<UtilityNetworkLink> links) {
    if (id == 0U || !valid_kind(kind)) {
        return core::Result<UtilityNetwork>::failure(invalid(
            "utility network identity or type is invalid"));
    }
    normalize_links(links);
    auto valid = validate_nodes_and_links(nodes, links);
    if (!valid) return core::Result<UtilityNetwork>::failure(valid.error());
    return core::Result<UtilityNetwork>::success(UtilityNetwork(
        id, kind, std::move(nodes), std::move(links)));
}

core::Result<UtilityNetworkEditReceipt> UtilityNetwork::apply(
    const UtilityNetworkEdit& edit) {
    if (edit.expected_revision != revision_) {
        return core::Result<UtilityNetworkEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "utility network edit revision no longer matches the source"));
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<UtilityNetworkEditReceipt>::failure(invalid(
            "utility network revision is exhausted"));
    }

    auto next_nodes = nodes_;
    auto next_links = links_;
    std::unordered_set<UtilityNodeId> anchors;
    std::unordered_set<UtilityNodeId> changed;
    const auto find_node = [&](UtilityNodeId id) {
        return std::find_if(next_nodes.begin(), next_nodes.end(),
            [id](const auto& node) { return node.id == id; });
    };
    const auto find_link = [&](UtilityLinkId id) {
        return std::find_if(next_links.begin(), next_links.end(),
            [id](const auto& link) { return link.id == id; });
    };

    const auto applied = std::visit([&](const auto& operation) -> core::Result<bool> {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, UtilityUpsertNode>) {
            auto found = find_node(operation.node.id);
            if (found != next_nodes.end()) {
                if (*found == operation.node) {
                    return core::Result<bool>::failure(invalid(
                        "utility network edit does not change its source"));
                }
                anchors.insert(found->id);
                *found = operation.node;
            } else {
                anchors.insert(operation.node.id);
                next_nodes.push_back(operation.node);
            }
            changed.insert(operation.node.id);
        } else if constexpr (std::is_same_v<Operation, UtilityRemoveNode>) {
            auto found = find_node(operation.id);
            if (found == next_nodes.end()) {
                return core::Result<bool>::failure(core::Diagnostic(
                    core::ErrorCode::not_found, "utility network node was not found"));
            }
            anchors.insert(operation.id);
            changed.insert(operation.id);
            next_nodes.erase(found);
            std::erase_if(next_links, [&](const auto& link) {
                if (link.first != operation.id && link.second != operation.id) return false;
                anchors.insert(link.first);
                anchors.insert(link.second);
                return true;
            });
        } else if constexpr (std::is_same_v<Operation, UtilityUpsertLink>) {
            auto desired = operation.link;
            if (desired.second < desired.first) std::swap(desired.first, desired.second);
            auto found = find_link(desired.id);
            if (found != next_links.end()) {
                if (*found == desired) {
                    return core::Result<bool>::failure(invalid(
                        "utility network edit does not change its source"));
                }
                anchors.insert(found->first);
                anchors.insert(found->second);
                changed.insert(found->first);
                changed.insert(found->second);
                *found = desired;
            } else {
                next_links.push_back(desired);
            }
            anchors.insert(desired.first);
            anchors.insert(desired.second);
            changed.insert(desired.first);
            changed.insert(desired.second);
        } else if constexpr (std::is_same_v<Operation, UtilityRemoveLink>) {
            auto found = find_link(operation.id);
            if (found == next_links.end()) {
                return core::Result<bool>::failure(core::Diagnostic(
                    core::ErrorCode::not_found, "utility network link was not found"));
            }
            anchors.insert(found->first);
            anchors.insert(found->second);
            changed.insert(found->first);
            changed.insert(found->second);
            next_links.erase(found);
        }
        return core::Result<bool>::success(true);
    }, edit.operation);
    if (!applied) {
        return core::Result<UtilityNetworkEditReceipt>::failure(applied.error());
    }

    auto valid = validate_nodes_and_links(next_nodes, next_links);
    if (!valid) {
        return core::Result<UtilityNetworkEditReceipt>::failure(valid.error());
    }

    const auto before_adjacency = make_adjacency(nodes_, links_, false);
    const auto after_adjacency = make_adjacency(next_nodes, next_links, false);
    std::unordered_set<UtilityNodeId> invalidated;
    for (const auto anchor : anchors) {
        const auto before = component_for(anchor, before_adjacency);
        invalidated.insert(before.begin(), before.end());
        const auto after = component_for(anchor, after_adjacency);
        invalidated.insert(after.begin(), after.end());
    }

    UtilityNetworkEditReceipt receipt;
    receipt.network_id = id_;
    receipt.revision_before = revision_;
    receipt.revision_after = revision_ + 1U;
    receipt.changed_node_ids = sorted_ids(changed);
    receipt.invalidated_node_ids = sorted_ids(invalidated);

    nodes_ = std::move(next_nodes);
    links_ = std::move(next_links);
    revision_ = receipt.revision_after;
    std::sort(nodes_.begin(), nodes_.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(links_.begin(), links_.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    return core::Result<UtilityNetworkEditReceipt>::success(std::move(receipt));
}

core::Result<UtilityConnectivityReport> UtilityNetwork::inspect_connectivity() const {
    auto valid = validate_nodes_and_links(nodes_, links_);
    if (!valid) {
        return core::Result<UtilityConnectivityReport>::failure(valid.error());
    }

    UtilityConnectivityReport report;
    report.network_id = id_;
    report.kind = kind_;
    report.revision = revision_;
    const auto adjacency = make_adjacency(nodes_, links_, true);
    std::unordered_map<UtilityNodeId, const UtilityNetworkNode*> by_id;
    by_id.reserve(nodes_.size());
    for (const auto& node : nodes_) by_id.emplace(node.id, &node);

    std::unordered_set<UtilityNodeId> visited;
    for (const auto& node : nodes_) {
        if (node.state != UtilityNodeState::active || visited.contains(node.id)) continue;
        const auto component_ids = component_for(node.id, adjacency);
        visited.insert(component_ids.begin(), component_ids.end());
        UtilityNetworkComponent component;
        component.node_ids = sorted_ids(component_ids);
        component.has_active_source = std::any_of(component.node_ids.begin(),
            component.node_ids.end(), [&](UtilityNodeId id) {
                return by_id.at(id)->kind == UtilityNodeKind::source;
            });
        for (const auto id : component.node_ids) {
            const auto kind = by_id.at(id)->kind;
            if ((kind == UtilityNodeKind::sink ||
                 kind == UtilityNodeKind::service_connection) &&
                !component.has_active_source) {
                component.unserved_terminals.push_back(id);
            }
        }
        if (!component.unserved_terminals.empty()) report.continuity_passed = false;
        report.components.push_back(std::move(component));
    }
    return core::Result<UtilityConnectivityReport>::success(std::move(report));
}

} // namespace carto::condition
