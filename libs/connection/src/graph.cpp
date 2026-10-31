#include <carto/connection/graph.hpp>

#include "record_io.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace carto::connection {
namespace {

constexpr std::string_view kGraphMagic = "CARTOGRAPHER_CONNECTION_PORT_GRAPH";
constexpr std::string_view kAcceptanceMagic = "CARTOGRAPHER_PORT_GRAPH_ACCEPTANCE";

bool valid_connection_kind(ConnectionKind kind) {
    switch (kind) {
    case ConnectionKind::mechanical:
    case ConnectionKind::equipment:
    case ConnectionKind::door_hinge:
    case ConnectionKind::maritime:
    case ConnectionKind::utility:
        return true;
    }
    return false;
}

bool valid_acceptance_tier(PortGraphAcceptanceTier tier) {
    switch (tier) {
    case PortGraphAcceptanceTier::structural:
    case PortGraphAcceptanceTier::reference_behavior:
    case PortGraphAcceptanceTier::native_evidence:
    case PortGraphAcceptanceTier::full_game:
        return true;
    }
    return false;
}

bool same_edge_endpoints(const PortConnectionEdge& edge, PortId first, PortId second) {
    return (edge.first_port == first && edge.second_port == second) ||
        (edge.first_port == second && edge.second_port == first);
}

bool evidence_supports_kind(const PortCompatibilityEvidence& evidence, ConnectionKind kind) {
    const std::string_view required_space = connection_kind_space(kind);
    return std::any_of(evidence.shared_tags.begin(), evidence.shared_tags.end(),
        [required_space](const auto& tag) { return tag.space == required_space; });
}

std::map<PortId, std::vector<PortId>> adjacency_of(
    const std::map<std::uint64_t, PortConnectionEdge>& edges) {
    std::map<PortId, std::vector<PortId>> adjacency;
    for (const auto& [identity, edge] : edges) {
        static_cast<void>(identity);
        adjacency[edge.first_port].push_back(edge.second_port);
        adjacency[edge.second_port].push_back(edge.first_port);
    }
    for (auto& [port, neighbors] : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(
            std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    return adjacency;
}

bool contains_cycle(const std::map<PortId, std::vector<PortId>>& adjacency) {
    std::set<PortId> visited;
    for (const auto& [root, root_neighbors] : adjacency) {
        static_cast<void>(root_neighbors);
        if (visited.contains(root)) continue;
        std::vector<std::pair<PortId, PortId>> stack;
        stack.emplace_back(root, PortId{});
        visited.insert(root);
        while (!stack.empty()) {
            const auto [current, parent] = stack.back();
            stack.pop_back();
            const auto found = adjacency.find(current);
            if (found == adjacency.end()) continue;
            for (const auto& neighbor : found->second) {
                if (!visited.contains(neighbor)) {
                    visited.insert(neighbor);
                    stack.emplace_back(neighbor, current);
                } else if (neighbor != parent) {
                    return true;
                }
            }
        }
    }
    return false;
}

template <typename Record>
core::Result<assets::Sha256Digest> digest_of(const Record& record) {
    if (auto result = record.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = record.serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

} // namespace

std::string_view connection_kind_name(ConnectionKind kind) noexcept {
    switch (kind) {
    case ConnectionKind::mechanical: return "mechanical";
    case ConnectionKind::equipment: return "equipment";
    case ConnectionKind::door_hinge: return "door-hinge";
    case ConnectionKind::maritime: return "maritime";
    case ConnectionKind::utility: return "utility";
    }
    return "unknown";
}

std::string_view connection_kind_space(ConnectionKind kind) noexcept {
    switch (kind) {
    case ConnectionKind::mechanical: return "mechanical-hitch";
    case ConnectionKind::equipment: return "equipment";
    case ConnectionKind::door_hinge: return "door-hinge";
    case ConnectionKind::maritime: return "maritime";
    case ConnectionKind::utility: return "utility";
    }
    return "unknown";
}

core::Result<void> PortConnectionEdge::validate() const {
    if (id == 0U || !first_port || !second_port || first_port == second_port) {
        return core::Result<void>::failure(
            detail::invalid_field("connection edge identity or endpoints are invalid"));
    }
    if (!valid_connection_kind(kind)) {
        return core::Result<void>::failure(
            detail::invalid_field("connection edge kind is invalid"));
    }
    if (auto result = evidence.validate(); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> ConnectionPortGraph::insert_port(ConnectionPort port) {
    if (auto result = port.validate(); !result) return result;
    if (ports_.size() >= kMaxPortsPerGraph) {
        return core::Result<void>::failure(detail::validation_error(
            "connection port graph has reached its bounded port capacity"));
    }
    if (!ports_.emplace(port.id, std::move(port)).second) {
        return core::Result<void>::failure(
            detail::invalid_field("connection port identity is duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<void> ConnectionPortGraph::erase_port(PortId id) {
    if (!id) {
        return core::Result<void>::failure(detail::invalid_field(
            "connection port identity is missing"));
    }
    if (!ports_.contains(id)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "connection port does not exist"));
    }
    const bool connected = std::any_of(edges_.begin(), edges_.end(), [id](const auto& item) {
        const auto& edge = item.second;
        return edge.first_port == id || edge.second_port == id;
    });
    if (connected) {
        return core::Result<void>::failure(detail::validation_error(
            "connected port must be disconnected before it can be removed"));
    }
    ports_.erase(id);
    return core::Result<void>::success();
}

core::Result<PortConnectionEdge> ConnectionPortGraph::connect(
    PortId first,
    PortId second,
    ConnectionKind kind) {
    if (!valid_connection_kind(kind)) {
        return core::Result<PortConnectionEdge>::failure(
            detail::invalid_field("connection edge kind is invalid"));
    }
    if (!first || !second || first == second) {
        return core::Result<PortConnectionEdge>::failure(detail::invalid_field(
            "connection edge endpoints must be two distinct port identities"));
    }
    if (!ports_.contains(first) || !ports_.contains(second)) {
        return core::Result<PortConnectionEdge>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "connection edge references a port missing from the graph"));
    }
    if (edges_.size() >= kMaxEdgesPerGraph) {
        return core::Result<PortConnectionEdge>::failure(detail::validation_error(
            "connection port graph has reached its bounded edge capacity"));
    }
    if (next_edge_id_ == 0U) {
        return core::Result<PortConnectionEdge>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "connection edge identity space is exhausted"));
    }
    for (const auto& [identity, edge] : edges_) {
        static_cast<void>(identity);
        if (edge.kind == kind && same_edge_endpoints(edge, first, second)) {
            return core::Result<PortConnectionEdge>::failure(detail::validation_error(
                "an identical connection edge already exists"));
        }
    }
    auto evidence = check_port_compatibility(ports_.at(first), ports_.at(second));
    if (!evidence) {
        return core::Result<PortConnectionEdge>::failure(evidence.error());
    }
    if (!evidence_supports_kind(evidence.value(), kind)) {
        return core::Result<PortConnectionEdge>::failure(detail::validation_error(
            "shared compatibility tags do not include the edge-kind namespace"));
    }
    if (would_create_cycle(first, second)) {
        return core::Result<PortConnectionEdge>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "connection would create an attachment cycle"));
    }

    PortConnectionEdge edge;
    edge.id = next_edge_id_;
    next_edge_id_ = edge.id == std::numeric_limits<std::uint64_t>::max()
        ? 0U : edge.id + 1U;
    edge.first_port = first;
    edge.second_port = second;
    edge.kind = kind;
    edge.evidence = std::move(evidence).value();
    const auto [iterator, inserted] = edges_.emplace(edge.id, edge);
    static_cast<void>(inserted);
    return core::Result<PortConnectionEdge>::success(iterator->second);
}

core::Result<void> ConnectionPortGraph::erase_edge(std::uint64_t edge_id) {
    if (edge_id == 0U) {
        return core::Result<void>::failure(detail::invalid_field(
            "connection edge identity is missing"));
    }
    if (edges_.erase(edge_id) == 0U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "connection edge does not exist"));
    }
    return core::Result<void>::success();
}

const ConnectionPort* ConnectionPortGraph::find_port(PortId id) const noexcept {
    const auto found = ports_.find(id);
    return found == ports_.end() ? nullptr : &found->second;
}

bool ConnectionPortGraph::empty() const noexcept {
    return ports_.empty() && edges_.empty();
}

bool ConnectionPortGraph::would_create_cycle(PortId first, PortId second) const {
    if (!ports_.contains(first) || !ports_.contains(second)) return false;
    const auto adjacency = adjacency_of(edges_);
    std::set<PortId> visited{first};
    std::vector<PortId> stack{first};
    while (!stack.empty()) {
        const PortId current = stack.back();
        stack.pop_back();
        const auto found = adjacency.find(current);
        if (found == adjacency.end()) continue;
        for (const auto& neighbor : found->second) {
            if (neighbor == second) return true;
            if (visited.insert(neighbor).second) stack.push_back(neighbor);
        }
    }
    return false;
}

void ConnectionPortGraph::refresh_next_edge_id() noexcept {
    if (edges_.empty()) {
        next_edge_id_ = 1U;
        return;
    }
    const std::uint64_t last_id = edges_.rbegin()->first;
    next_edge_id_ = last_id == std::numeric_limits<std::uint64_t>::max()
        ? 0U : last_id + 1U;
}

core::Result<void> ConnectionPortGraph::validate() const {
    if (ports_.size() > kMaxPortsPerGraph || edges_.size() > kMaxEdgesPerGraph) {
        return core::Result<void>::failure(detail::validation_error(
            "connection port graph exceeds its bounded capacity"));
    }
    if (next_edge_id_ != 0U && !edges_.empty() &&
        next_edge_id_ <= edges_.rbegin()->first) {
        return core::Result<void>::failure(detail::validation_error(
            "connection edge allocation state overlaps an existing identity"));
    }
    for (const auto& [identity, port] : ports_) {
        if (identity != port.id) {
            return core::Result<void>::failure(detail::validation_error(
                "connection port map identity does not match its record"));
        }
        if (auto result = port.validate(); !result) return result;
    }
    std::set<std::pair<PortId, PortId>> endpoint_pairs;
    for (const auto& [identity, edge] : edges_) {
        if (identity != edge.id) {
            return core::Result<void>::failure(detail::validation_error(
                "connection edge map identity does not match its record"));
        }
        if (auto result = edge.validate(); !result) return result;
        if (!ports_.contains(edge.first_port) || !ports_.contains(edge.second_port)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "connection edge references a port missing from the graph"));
        }
        const auto expected = check_port_compatibility(
            ports_.at(edge.first_port), ports_.at(edge.second_port));
        if (!expected) {
            return core::Result<void>::failure(expected.error().with_context(
                "connection edge endpoint compatibility"));
        }
        if (!evidence_supports_kind(expected.value(), edge.kind)) {
            return core::Result<void>::failure(detail::validation_error(
                "shared compatibility tags do not include the edge-kind namespace"));
        }
        if (edge.evidence.shared_tags != expected.value().shared_tags ||
            edge.evidence.first_satisfied_requirements != expected.value().first_satisfied_requirements ||
            edge.evidence.second_satisfied_requirements != expected.value().second_satisfied_requirements) {
            return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
                "connection edge evidence does not match its exact endpoint contract"));
        }
        if (!endpoint_pairs.emplace(std::min(edge.first_port, edge.second_port),
                std::max(edge.first_port, edge.second_port)).second) {
            return core::Result<void>::failure(detail::validation_error(
                "connection port graph contains duplicate attachment endpoints"));
        }
    }
    if (contains_cycle(adjacency_of(edges_))) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected,
            "connection port graph contains an attachment cycle"));
    }
    return core::Result<void>::success();
}

std::string ConnectionPortGraph::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    const auto last_id = edges_.empty() ? 0U : edges_.rbegin()->first;
    const auto inferred_next = last_id == std::numeric_limits<std::uint64_t>::max()
        ? 0U : last_id + 1U;
    // Preserve legacy bytes unless deletion retained allocation history that
    // cannot be reconstructed from the remaining live edges.
    const bool retained_allocation = next_edge_id_ != inferred_next;
    output << kGraphMagic << ' ' << (retained_allocation ? kConnectionPortGraphVersion : 1U)
           << '\n';
    output << std::setprecision(17);
    output << "PORTS " << ports_.size() << '\n';
    for (const auto& [identity, port] : ports_) {
        static_cast<void>(identity);
        detail::write_port_record(output, port);
    }
    output << "EDGES " << edges_.size() << '\n';
    for (const auto& [identity, edge] : edges_) {
        output << "EDGE " << identity << ' ' << edge.first_port.value << ' '
               << edge.second_port.value << ' ' << static_cast<unsigned>(edge.kind);
        detail::write_evidence_inline(output, edge.evidence);
        output << '\n';
    }
    if (retained_allocation) output << "NEXT_EDGE_ID " << next_edge_id_ << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ConnectionPortGraph::canonical_digest() const {
    return digest_of(*this);
}

core::Result<ConnectionPortGraph> ConnectionPortGraph::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxConnectionPortGraphBytes) {
        return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
            "connection port graph text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_unsigned(input, version)) {
        return core::Result<ConnectionPortGraph>::failure(
            detail::parse_error("connection port graph framing is invalid"));
    }
    if (magic != kGraphMagic) {
        return core::Result<ConnectionPortGraph>::failure(
            detail::parse_error("connection port graph framing is invalid"));
    }
    if (version != 1U && version != kConnectionPortGraphVersion) {
        return core::Result<ConnectionPortGraph>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "connection port graph schema version is not supported"));
    }

    ConnectionPortGraph graph;
    std::size_t port_count = 0U;
    if (!detail::require_token(input, "PORTS") ||
        !detail::read_count(input, kMaxPortsPerGraph, port_count)) {
        return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
            "connection port graph port header is invalid"));
    }
    for (std::size_t index = 0U; index < port_count; ++index) {
        ConnectionPort port;
        if (!detail::read_port_record(input, port)) {
            return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
                "connection port graph port record is malformed"));
        }
        if (auto result = graph.insert_port(std::move(port)); !result) {
            return core::Result<ConnectionPortGraph>::failure(result.error());
        }
    }

    std::size_t edge_count = 0U;
    if (!detail::require_token(input, "EDGES") ||
        !detail::read_count(input, kMaxEdgesPerGraph, edge_count)) {
        return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
            "connection port graph edge header is invalid"));
    }
    for (std::size_t index = 0U; index < edge_count; ++index) {
        PortConnectionEdge edge;
        std::uint64_t kind = 0U;
        if (!detail::require_token(input, "EDGE") ||
            !detail::read_unsigned(input, edge.id) ||
            !detail::read_unsigned(input, edge.first_port.value) ||
            !detail::read_unsigned(input, edge.second_port.value) ||
            !detail::read_unsigned(input, kind) ||
            kind > static_cast<std::uint64_t>(ConnectionKind::utility) ||
            !detail::read_evidence_inline(input, edge.evidence)) {
            return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
                "connection port graph edge record is malformed"));
        }
        edge.kind = static_cast<ConnectionKind>(kind);
        if (edge.id == 0U || !graph.edges_.emplace(edge.id, std::move(edge)).second) {
            return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
                "connection port graph edge identity is missing or duplicated"));
        }
    }
    graph.refresh_next_edge_id();
    if (version >= 2U &&
        (!detail::require_token(input, "NEXT_EDGE_ID") ||
         !detail::read_unsigned(input, graph.next_edge_id_))) {
        return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
            "connection port graph allocation state is malformed"));
    }

    if (!detail::require_token(input, "END")) {
        return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
            "connection port graph is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ConnectionPortGraph>::failure(detail::parse_error(
            "connection port graph contains trailing unbound data"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<ConnectionPortGraph>::failure(result.error());
    }
    return core::Result<ConnectionPortGraph>::success(std::move(graph));
}

std::string_view port_graph_acceptance_tier_name(PortGraphAcceptanceTier tier) noexcept {
    switch (tier) {
    case PortGraphAcceptanceTier::structural: return "structural";
    case PortGraphAcceptanceTier::reference_behavior: return "reference-behavior";
    case PortGraphAcceptanceTier::native_evidence: return "native-evidence";
    case PortGraphAcceptanceTier::full_game: return "full-game";
    }
    return "unknown";
}

core::Result<void> PortGraphAcceptanceReceipt::validate() const {
    if (graph_digest.is_zero()) {
        return core::Result<void>::failure(
            detail::invalid_field("acceptance receipt is not bound to a graph digest"));
    }
    if (!valid_acceptance_tier(tier)) {
        return core::Result<void>::failure(
            detail::invalid_field("acceptance tier is invalid"));
    }
    if (evidence_digests.size() != port_graph_acceptance_tier_rank(tier) ||
        evidence_digests.size() > kMaxAcceptanceEvidenceDigests) {
        return core::Result<void>::failure(detail::validation_error(
            "acceptance evidence count does not match the claimed tier"));
    }
    if (!std::all_of(evidence_digests.begin(), evidence_digests.end(),
            [](const assets::Sha256Digest& digest) { return !digest.is_zero(); })) {
        return core::Result<void>::failure(detail::validation_error(
            "acceptance evidence digests must be non-zero"));
    }
    return core::Result<void>::success();
}

std::string PortGraphAcceptanceReceipt::serialize() const {
    std::ostringstream output;
    output << kAcceptanceMagic << ' ' << kPortGraphAcceptanceVersion << '\n';
    output << "GRAPH " << graph_digest.hex() << '\n';
    output << "TIER " << static_cast<unsigned>(tier) << '\n';
    output << "EVIDENCE " << evidence_digests.size();
    for (const auto& digest : evidence_digests) output << ' ' << digest.hex();
    output << "\nEND\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PortGraphAcceptanceReceipt::canonical_digest() const {
    return digest_of(*this);
}

core::Result<PortGraphAcceptanceReceipt> PortGraphAcceptanceReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxPortGraphAcceptanceBytes) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(detail::parse_error(
            "acceptance receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_unsigned(input, version)) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt framing is invalid"));
    }
    if (magic != kAcceptanceMagic) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt framing is invalid"));
    }
    if (version != kPortGraphAcceptanceVersion) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "acceptance receipt schema version is not supported"));
    }

    PortGraphAcceptanceReceipt receipt;
    std::uint64_t tier = 0U;
    std::size_t evidence_count = 0U;
    std::string graph_digest;
    if (!detail::require_token(input, "GRAPH") ||
        !(input >> graph_digest) ||
        !detail::require_token(input, "TIER") ||
        !detail::read_unsigned(input, tier) ||
        tier > static_cast<std::uint64_t>(PortGraphAcceptanceTier::full_game) ||
        !detail::require_token(input, "EVIDENCE") ||
        !detail::read_count(input, kMaxAcceptanceEvidenceDigests, evidence_count)) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt record is malformed"));
    }
    const auto parsed_graph_digest = assets::Sha256Digest::from_hex(graph_digest);
    if (!parsed_graph_digest) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt graph digest is malformed"));
    }
    receipt.graph_digest = parsed_graph_digest.value();
    receipt.tier = static_cast<PortGraphAcceptanceTier>(tier);
    receipt.evidence_digests.reserve(evidence_count);
    for (std::size_t index = 0U; index < evidence_count; ++index) {
        std::string encoded;
        if (!(input >> encoded)) {
            return core::Result<PortGraphAcceptanceReceipt>::failure(
                detail::parse_error("acceptance receipt evidence digest is malformed"));
        }
        const auto parsed = assets::Sha256Digest::from_hex(encoded);
        if (!parsed) {
            return core::Result<PortGraphAcceptanceReceipt>::failure(
                detail::parse_error("acceptance receipt evidence digest is malformed"));
        }
        receipt.evidence_digests.push_back(parsed.value());
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(result.error());
    }
    return core::Result<PortGraphAcceptanceReceipt>::success(std::move(receipt));
}

core::Result<PortGraphAcceptanceReceipt> advance_port_graph_acceptance(
    const PortGraphAcceptanceReceipt& receipt,
    PortGraphAcceptanceTier next_tier,
    assets::Sha256Digest evidence_digest) {
    if (auto result = receipt.validate(); !result) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(result.error());
    }
    if (!valid_acceptance_tier(next_tier)) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::invalid_field("acceptance tier is invalid"));
    }
    const auto expected =
        static_cast<std::uint8_t>(port_graph_acceptance_tier_rank(receipt.tier) + 1U);
    if (port_graph_acceptance_tier_rank(next_tier) != expected) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::validation_error(
                "acceptance tiers advance exactly one step and never demote"));
    }
    if (evidence_digest.is_zero()) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(
            detail::invalid_field("acceptance evidence digest is missing"));
    }
    PortGraphAcceptanceReceipt advanced = receipt;
    advanced.tier = next_tier;
    advanced.evidence_digests.push_back(evidence_digest);
    if (auto result = advanced.validate(); !result) {
        return core::Result<PortGraphAcceptanceReceipt>::failure(result.error());
    }
    return core::Result<PortGraphAcceptanceReceipt>::success(std::move(advanced));
}

} // namespace carto::connection
