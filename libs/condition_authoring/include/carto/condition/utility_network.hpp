#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace carto::condition {

using UtilityNetworkId = std::uint64_t;
using UtilityNodeId = std::uint64_t;
using UtilityLinkId = std::uint64_t;

enum class UtilityNetworkKind : std::uint8_t {
    power,
    water,
    fuel,
    communications,
};

enum class UtilityNodeKind : std::uint8_t {
    source,
    sink,
    junction,
    service_connection,
};

enum class UtilityNodeState : std::uint8_t {
    planned,
    active,
    out_of_service,
};

struct UtilityNetworkNode {
    UtilityNodeId id = 0U;
    UtilityNodeKind kind = UtilityNodeKind::junction;
    UtilityNodeState state = UtilityNodeState::planned;
    // An authored class label only; the kernel does not infer physical units
    // or claim that two different labels are quantitatively compatible.
    std::string capacity_class;
    friend bool operator==(const UtilityNetworkNode&, const UtilityNetworkNode&) = default;
};

struct UtilityNetworkLink {
    UtilityLinkId id = 0U;
    UtilityNodeId first = 0U;
    UtilityNodeId second = 0U;
    UtilityNodeState state = UtilityNodeState::planned;
    friend bool operator==(const UtilityNetworkLink&, const UtilityNetworkLink&) = default;
};

struct UtilityUpsertNode {
    UtilityNetworkNode node;
};

struct UtilityRemoveNode {
    UtilityNodeId id = 0U;
};

struct UtilityUpsertLink {
    UtilityNetworkLink link;
};

struct UtilityRemoveLink {
    UtilityLinkId id = 0U;
};

struct UtilityNetworkEdit {
    std::uint64_t expected_revision = 0U;
    std::variant<UtilityUpsertNode, UtilityRemoveNode, UtilityUpsertLink,
        UtilityRemoveLink> operation;
};

struct UtilityNetworkEditReceipt {
    UtilityNetworkId network_id = 0U;
    std::uint64_t revision_before = 0U;
    std::uint64_t revision_after = 0U;
    std::vector<UtilityNodeId> changed_node_ids;
    // The union of the affected components before and after the edit. A caller
    // can invalidate derived work for these IDs while retaining unrelated work.
    std::vector<UtilityNodeId> invalidated_node_ids;
};

struct UtilityNetworkComponent {
    std::vector<UtilityNodeId> node_ids;
    bool has_active_source = false;
    std::vector<UtilityNodeId> unserved_terminals;
};

struct UtilityConnectivityReport {
    UtilityNetworkId network_id = 0U;
    UtilityNetworkKind kind = UtilityNetworkKind::power;
    std::uint64_t revision = 0U;
    bool continuity_passed = true;
    std::vector<UtilityNetworkComponent> components;
};

// Revisioned authoring kernel for typed utility source graphs. It is a local
// data model: it does not publish geometry, grant write authority, or persist a
// project on its own.
class UtilityNetwork final {
public:
    [[nodiscard]] static core::Result<UtilityNetwork> create(
        UtilityNetworkId id,
        UtilityNetworkKind kind,
        std::vector<UtilityNetworkNode> nodes = {},
        std::vector<UtilityNetworkLink> links = {});

    [[nodiscard]] UtilityNetworkId id() const noexcept { return id_; }
    [[nodiscard]] UtilityNetworkKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const std::vector<UtilityNetworkNode>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] const std::vector<UtilityNetworkLink>& links() const noexcept {
        return links_;
    }

    [[nodiscard]] core::Result<UtilityNetworkEditReceipt> apply(
        const UtilityNetworkEdit& edit);
    [[nodiscard]] core::Result<UtilityConnectivityReport> inspect_connectivity() const;

private:
    UtilityNetwork(UtilityNetworkId id, UtilityNetworkKind kind,
        std::vector<UtilityNetworkNode> nodes,
        std::vector<UtilityNetworkLink> links) noexcept;

    UtilityNetworkId id_ = 0U;
    UtilityNetworkKind kind_ = UtilityNetworkKind::power;
    std::uint64_t revision_ = 1U;
    std::vector<UtilityNetworkNode> nodes_;
    std::vector<UtilityNetworkLink> links_;
};

} // namespace carto::condition
