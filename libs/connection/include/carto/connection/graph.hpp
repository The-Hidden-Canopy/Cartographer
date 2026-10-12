#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/connection/port.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::connection {

// The typed connection graph over authored ports. Edges carry the
// compatibility evidence captured when the join was admitted; the graph is an
// acyclic attachment forest. Structural validation never implies runtime,
// native, or full-game acceptance; see PortGraphAcceptanceReceipt.

inline constexpr std::uint32_t kConnectionPortGraphVersion = 1U;
inline constexpr std::uint32_t kPortGraphAcceptanceVersion = 1U;
inline constexpr std::size_t kMaxConnectionPortGraphBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxPortGraphAcceptanceBytes = 256U * 1024U;
inline constexpr std::size_t kMaxPortsPerGraph = 65'536U;
inline constexpr std::size_t kMaxEdgesPerGraph = 262'144U;
inline constexpr std::size_t kMaxAcceptanceEvidenceDigests = 8U;

enum class ConnectionKind : std::uint8_t {
    mechanical,
    equipment,
    door_hinge,
    maritime,
    utility,
};

[[nodiscard]] std::string_view connection_kind_name(ConnectionKind kind) noexcept;
// The compatibility-tag namespace an edge kind is allowed to join on.
[[nodiscard]] std::string_view connection_kind_space(ConnectionKind kind) noexcept;

struct PortConnectionEdge {
    std::uint64_t id = 0U;
    PortId first_port;
    PortId second_port;
    ConnectionKind kind = ConnectionKind::mechanical;
    PortCompatibilityEvidence evidence;

    [[nodiscard]] core::Result<void> validate() const;
};

class ConnectionPortGraph final {
public:
    [[nodiscard]] core::Result<void> insert_port(ConnectionPort port);
    // Fail-closed join: every check (existence, compatibility, edge-kind
    // namespace, duplicates, cycles, bounds) runs before any state change.
    [[nodiscard]] core::Result<PortConnectionEdge> connect(
        PortId first,
        PortId second,
        ConnectionKind kind);

    [[nodiscard]] const std::map<PortId, ConnectionPort>& ports() const noexcept {
        return ports_;
    }
    [[nodiscard]] const std::map<std::uint64_t, PortConnectionEdge>& edges() const noexcept {
        return edges_;
    }
    [[nodiscard]] const ConnectionPort* find_port(PortId id) const noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ConnectionPortGraph> deserialize(std::string_view text);

private:
    [[nodiscard]] bool would_create_cycle(PortId first, PortId second) const;
    void refresh_next_edge_id() noexcept;

    std::map<PortId, ConnectionPort> ports_;
    std::map<std::uint64_t, PortConnectionEdge> edges_;
    std::uint64_t next_edge_id_ = 1U;
};

// Acceptance tiers are strict and never silently promoted: structural
// validation, reference-behavior agreement, native backend evidence, and
// full-game acceptance are separate claims with separate evidence.
enum class PortGraphAcceptanceTier : std::uint8_t {
    structural,
    reference_behavior,
    native_evidence,
    full_game,
};

[[nodiscard]] std::string_view port_graph_acceptance_tier_name(
    PortGraphAcceptanceTier tier) noexcept;
[[nodiscard]] constexpr std::uint8_t port_graph_acceptance_tier_rank(
    PortGraphAcceptanceTier tier) noexcept {
    return static_cast<std::uint8_t>(tier);
}

struct PortGraphAcceptanceReceipt {
    assets::Sha256Digest graph_digest;
    PortGraphAcceptanceTier tier = PortGraphAcceptanceTier::structural;
    // Exactly tier_rank(tier) digests, ordered structural -> claimed tier.
    std::vector<assets::Sha256Digest> evidence_digests;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PortGraphAcceptanceReceipt> deserialize(
        std::string_view text);
};

// Advance acceptance exactly one tier with fresh, non-zero evidence. Skipping
// tiers, demoting, re-claiming the current tier, and advancing past
// full_game all fail closed.
[[nodiscard]] core::Result<PortGraphAcceptanceReceipt> advance_port_graph_acceptance(
    const PortGraphAcceptanceReceipt& receipt,
    PortGraphAcceptanceTier next_tier,
    assets::Sha256Digest evidence_digest);

} // namespace carto::connection
