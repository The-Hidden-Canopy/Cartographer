#include <carto/connection/attachment.hpp>
#include <carto/connection/graph.hpp>
#include <carto/connection/port.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using namespace carto;

assets::Sha256Digest digest_of(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

connection::ConnectionPort make_port(
    std::uint64_t id,
    std::uint64_t owner,
    std::vector<connection::PortCompatibilityTag> tags) {
    connection::ConnectionPort port;
    port.id = connection::PortId{id};
    port.owner = scene::ObjectId{owner};
    port.authoring.kind = world::AuthoringBindingKind::scene_object;
    port.authoring.identity = owner;
    port.local_frame.frame_identity = "frame-" + std::to_string(id);
    port.local_frame.coordinate_convention = "carto.core/v1";
    port.compatibility_tags = std::move(tags);
    return port;
}

std::vector<connection::PortCompatibilityTag> hitch_tags() {
    return {connection::compatibility::mechanical_hitch::pin_hitch_19mm};
}

std::vector<connection::PortCompatibilityTag> dual_tags() {
    return {connection::compatibility::mechanical_hitch::pin_hitch_19mm,
            connection::compatibility::utility::electrical_16a};
}

connection::ConnectionPortGraph make_graph(std::uint64_t first_id) {
    connection::ConnectionPortGraph graph;
    require(
        static_cast<bool>(graph.insert_port(make_port(first_id, 201U, dual_tags()))),
        "first port enters the graph");
    require(
        static_cast<bool>(graph.insert_port(make_port(first_id + 1U, 202U, dual_tags()))),
        "second port enters the graph");
    require(
        static_cast<bool>(graph.insert_port(make_port(first_id + 2U, 203U, hitch_tags()))),
        "third port enters the graph");
    return graph;
}

void graph_connects_only_compatible_ports() {
    auto graph = make_graph(11U);
    auto edge = graph.connect(
        connection::PortId{11U}, connection::PortId{12U},
        connection::ConnectionKind::mechanical);
    require(static_cast<bool>(edge), "compatible ports join mechanically");

    const auto before = graph.edges().size();
    const auto refused = graph.connect(
        connection::PortId{11U}, connection::PortId{13U},
        connection::ConnectionKind::equipment);
    require(
        !refused && graph.edges().size() == before,
        "incompatible endpoints refuse before any state change");

    const auto missing = graph.connect(
        connection::PortId{11U}, connection::PortId{99U},
        connection::ConnectionKind::mechanical);
    require(
        !missing && missing.error().code == core::ErrorCode::not_found,
        "edges to missing ports refuse with not_found");

    const auto self = graph.connect(
        connection::PortId{11U}, connection::PortId{11U},
        connection::ConnectionKind::mechanical);
    require(!self, "self edges refuse");

    require(static_cast<bool>(graph.validate()), "partial graph validates");
}

void typed_edges_enforce_namespaces() {
    auto graph = make_graph(21U);
    const auto wrong_namespace = graph.connect(
        connection::PortId{21U}, connection::PortId{22U},
        connection::ConnectionKind::maritime);
    require(
        !wrong_namespace,
        "edge kind refuses shared tags outside its namespace");

    const auto utility = graph.connect(
        connection::PortId{21U}, connection::PortId{22U},
        connection::ConnectionKind::utility);
    require(
        static_cast<bool>(utility),
        "utility edge admits shared utility-namespace tags");
    require(
        utility.value().evidence.shared_tags.size() == 2U,
        "typed edge records its compatibility evidence");

    const auto duplicate = graph.connect(
        connection::PortId{21U}, connection::PortId{22U},
        connection::ConnectionKind::utility);
    require(!duplicate, "identical edge kind between the same pair refuses");
}

void attachment_cycles_fail_closed() {
    auto graph = make_graph(31U);
    require(
        static_cast<bool>(graph.connect(
            connection::PortId{31U}, connection::PortId{32U},
            connection::ConnectionKind::mechanical)),
        "first chain link joins");
    require(
        static_cast<bool>(graph.connect(
            connection::PortId{32U}, connection::PortId{33U},
            connection::ConnectionKind::mechanical)),
        "second chain link joins");
    const auto cycle = graph.connect(
        connection::PortId{33U}, connection::PortId{31U},
        connection::ConnectionKind::mechanical);
    require(
        !cycle && cycle.error().code == core::ErrorCode::cycle_detected,
        "closing an attachment cycle refuses with cycle_detected");
    require(
        static_cast<bool>(graph.validate()),
        "graph without the refused cycle edge stays valid");
}

void graph_canonical_form_is_deterministic() {
    auto first = make_graph(41U);
    auto second = make_graph(41U);
    require(
        static_cast<bool>(first.connect(
            connection::PortId{41U}, connection::PortId{42U},
            connection::ConnectionKind::mechanical)),
        "first graph takes an edge");
    require(
        static_cast<bool>(second.connect(
            connection::PortId{41U}, connection::PortId{42U},
            connection::ConnectionKind::mechanical)),
        "second graph takes an edge");

    const auto first_digest = first.canonical_digest();
    const auto second_digest = second.canonical_digest();
    require(
        static_cast<bool>(first_digest) && first_digest.value() == second_digest.value(),
        "identical graphs digest identically");
    const std::string encoded = first.serialize();
    require(encoded == second.serialize(), "identical graphs serialize identically");

    auto parsed = connection::ConnectionPortGraph::deserialize(encoded);
    require(static_cast<bool>(parsed), "graph deserialize accepts canonical form");
    require(
        parsed.value().serialize() == encoded,
        "graph round-trip preserves the canonical form");

    require(
        static_cast<bool>(parsed.value().connect(
            connection::PortId{42U}, connection::PortId{43U},
            connection::ConnectionKind::mechanical)),
        "deserialized graph keeps allocating fresh edge identities");
}

void acceptance_tiers_are_never_promoted_silently() {
    auto graph = make_graph(51U);
    const auto graph_digest = graph.canonical_digest();
    require(static_cast<bool>(graph_digest), "graph digest binds the receipt");

    connection::PortGraphAcceptanceReceipt receipt;
    receipt.graph_digest = graph_digest.value();
    require(
        static_cast<bool>(receipt.validate()),
        "structural tier needs no evidence");
    receipt.evidence_digests.push_back(digest_of("behavior-log"));
    require(
        !receipt.validate(),
        "evidence without a tier promotion fails validation");
    receipt.evidence_digests.clear();

    auto reference = connection::advance_port_graph_acceptance(
        receipt, connection::PortGraphAcceptanceTier::reference_behavior,
        digest_of("reference-behavior-evidence"));
    require(
        static_cast<bool>(reference),
        "reference-behavior tier advances with evidence");

    const auto skipped = connection::advance_port_graph_acceptance(
        reference.value(), connection::PortGraphAcceptanceTier::full_game,
        digest_of("full-game-evidence"));
    require(!skipped, "tiers may not be skipped");

    const auto demoted = connection::advance_port_graph_acceptance(
        reference.value(), connection::PortGraphAcceptanceTier::structural,
        digest_of("structural-evidence"));
    require(!demoted, "tiers may not demote");

    const auto repeated = connection::advance_port_graph_acceptance(
        reference.value(), connection::PortGraphAcceptanceTier::reference_behavior,
        digest_of("more-evidence"));
    require(!repeated, "re-claiming the current tier refuses");

    const auto zero_evidence = connection::advance_port_graph_acceptance(
        reference.value(), connection::PortGraphAcceptanceTier::native_evidence,
        assets::Sha256Digest{});
    require(!zero_evidence, "zero evidence digests refuse");

    auto native = connection::advance_port_graph_acceptance(
        reference.value(), connection::PortGraphAcceptanceTier::native_evidence,
        digest_of("native-backend-evidence"));
    require(static_cast<bool>(native), "native-evidence tier advances with evidence");
    auto full = connection::advance_port_graph_acceptance(
        native.value(), connection::PortGraphAcceptanceTier::full_game,
        digest_of("full-game-evidence"));
    require(static_cast<bool>(full), "full-game tier advances with evidence");
    require(
        !connection::advance_port_graph_acceptance(
            full.value(), connection::PortGraphAcceptanceTier::full_game,
            digest_of("beyond")),
        "full-game is terminal");

    const std::string encoded = full.value().serialize();
    auto parsed = connection::PortGraphAcceptanceReceipt::deserialize(encoded);
    require(static_cast<bool>(parsed), "acceptance receipt round-trips");
    require(
        parsed.value().serialize() == encoded,
        "acceptance receipt canonical form is stable");
    require(
        static_cast<bool>(full.value().canonical_digest()),
        "acceptance receipt is digest-bound");
}

void attachment_preservation_survives_reimport() {
    auto graph_before = make_graph(61U);
    connection::PortAttachmentState state;
    require(
        static_cast<bool>(state.attach(connection::PortId{61U}, "runtime-token-a")),
        "first runtime token attaches");
    require(
        static_cast<bool>(state.attach(connection::PortId{61U}, "runtime-token-b")),
        "second runtime token attaches");
    require(
        static_cast<bool>(state.attach(connection::PortId{63U}, "runtime-token-c")),
        "token attaches to the doomed port");
    require(
        !state.attach(connection::PortId{61U}, "runtime-token-a"),
        "duplicate attachment tokens refuse");

    // Simulated reimport: port 63 is gone, port 64 is new; 61 and 62 survive.
    connection::ConnectionPortGraph reimported;
    require(
        static_cast<bool>(reimported.insert_port(make_port(61U, 201U, dual_tags()))),
        "reimported first port enters");
    require(
        static_cast<bool>(reimported.insert_port(make_port(62U, 202U, dual_tags()))),
        "reimported second port enters");
    require(
        static_cast<bool>(reimported.insert_port(make_port(64U, 204U, dual_tags()))),
        "newly authored port enters");

    auto receipt = connection::reconcile_attachments_after_reimport(state, reimported);
    require(static_cast<bool>(receipt), "reconciliation succeeds against valid inputs");
    require(
        receipt.value().preserved_ports ==
            std::vector<connection::PortId>{connection::PortId{61U}},
        "attachments survive only where their ports survive");
    require(
        receipt.value().dropped_ports ==
            std::vector<connection::PortId>{connection::PortId{63U}},
        "destroyed ports report their dropped attachments");
    require(
        receipt.value().preserved_attachments == 2U &&
            receipt.value().dropped_attachments == 1U,
        "preservation counts bound the surviving and dropped state");

    connection::PortAttachmentState expected_surviving;
    require(
        static_cast<bool>(expected_surviving.attach(
            connection::PortId{61U}, "runtime-token-a")),
        "expected surviving token a");
    require(
        static_cast<bool>(expected_surviving.attach(
            connection::PortId{61U}, "runtime-token-b")),
        "expected surviving token b");
    const auto expected_digest = expected_surviving.canonical_digest();
    require(
        expected_digest && expected_digest.value() == receipt.value().surviving_state_digest,
        "surviving state digest binds exactly the surviving attachments");

    const std::string encoded = receipt.value().serialize();
    auto parsed = connection::AttachmentPreservationReceipt::deserialize(encoded);
    require(static_cast<bool>(parsed), "preservation receipt round-trips");
    require(
        parsed.value().serialize() == encoded,
        "preservation receipt canonical form is stable");

    // Full-loss reimport: nothing survives, and the receipt still binds both
    // sides of the reconciliation.
    connection::PortAttachmentState doomed;
    require(
        static_cast<bool>(doomed.attach(connection::PortId{63U}, "runtime-token-c")),
        "doomed token attaches");
    auto total_loss = connection::reconcile_attachments_after_reimport(doomed, reimported);
    require(
        static_cast<bool>(total_loss) && total_loss.value().preserved_attachments == 0U &&
            total_loss.value().dropped_attachments == 1U &&
            total_loss.value().preserved_ports.empty(),
        "a full-loss reimport reports zero preservation without failing");
}

void hostile_graph_inputs_fail_closed() {
    auto graph = make_graph(71U);
    require(
        static_cast<bool>(graph.connect(
            connection::PortId{71U}, connection::PortId{72U},
            connection::ConnectionKind::mechanical)),
        "fixture edge joins");
    const std::string encoded = graph.serialize();

    require(
        !connection::ConnectionPortGraph::deserialize(encoded + "trailing"),
        "trailing unbound data is rejected");

    std::string future = encoded;
    future.replace(
        0U, std::string{"CARTOGRAPHER_CONNECTION_PORT_GRAPH 1"}.size(),
        "CARTOGRAPHER_CONNECTION_PORT_GRAPH 99");
    const auto future_result = connection::ConnectionPortGraph::deserialize(future);
    require(
        !future_result &&
            future_result.error().code == core::ErrorCode::version_mismatch,
        "unknown future versions fail with version_mismatch");

    require(
        !connection::ConnectionPortGraph::deserialize(
            "CARTOGRAPHER_CONNECTION_PORT_GRAPH 1\nPORTS 65537\n"),
        "oversized port counts are rejected before allocation");

    const std::string duplicate_ports =
        "CARTOGRAPHER_CONNECTION_PORT_GRAPH 1\n"
        "PORTS 2\n"
        "PORT 1 201 1 201 FRAME \"f-a\" \"\" 0 0 0 0 0 0 1 1 1 1 \"carto.core/v1\" "
        "TAGS 2 \"mechanical-hitch\" \"pin-hitch-19mm\" \"utility\" \"electrical-16a\" "
        "REQUIRED 0 OFFERED 0\n"
        "PORT 1 202 1 202 FRAME \"f-b\" \"\" 0 0 0 0 0 0 1 1 1 1 \"carto.core/v1\" "
        "TAGS 2 \"mechanical-hitch\" \"pin-hitch-19mm\" \"utility\" \"electrical-16a\" "
        "REQUIRED 0 OFFERED 0\n"
        "EDGES 0\n"
        "END\n";
    require(
        !connection::ConnectionPortGraph::deserialize(duplicate_ports),
        "duplicate port identities refuse deserialization");

    // Failure injection: an edge whose endpoint was never authored.
    const std::string dangling_edge =
        "CARTOGRAPHER_CONNECTION_PORT_GRAPH 1\n"
        "PORTS 0\n"
        "EDGES 1\n"
        "EDGE 1 71 72 0 SHARED 1 \"mechanical-hitch\" \"pin-hitch-19mm\" FIRST 0 "
        "SECOND 0\n"
        "END\n";
    const auto injected = connection::ConnectionPortGraph::deserialize(dangling_edge);
    require(
        !injected && injected.error().code == core::ErrorCode::not_found,
        "injected edges over missing ports refuse with not_found");
}

void hostile_attachment_inputs_fail_closed() {
    connection::PortAttachmentState state;
    require(
        static_cast<bool>(state.attach(connection::PortId{81U}, "runtime-token")),
        "fixture attachment forms");
    const std::string encoded = state.serialize();

    require(
        !connection::PortAttachmentState::deserialize(encoded + "trailing"),
        "attachment state rejects trailing unbound data");

    std::string future = encoded;
    future.replace(
        0U, std::string{"CARTOGRAPHER_PORT_ATTACHMENT_STATE 1"}.size(),
        "CARTOGRAPHER_PORT_ATTACHMENT_STATE 99");
    require(
        !connection::PortAttachmentState::deserialize(future),
        "attachment state rejects future versions");

    require(
        !connection::PortAttachmentState::deserialize(
            "CARTOGRAPHER_PORT_ATTACHMENT_STATE 1\nATTACHMENTS 65537\n"),
        "oversized attachment counts are rejected before allocation");

    const std::string duplicate_ports =
        "CARTOGRAPHER_PORT_ATTACHMENT_STATE 1\n"
        "ATTACHMENTS 2\n"
        "ATTACHMENT 81 1 \"token-a\"\n"
        "ATTACHMENT 81 1 \"token-b\"\n"
        "END\n";
    require(
        !connection::PortAttachmentState::deserialize(duplicate_ports),
        "duplicate attachment port records refuse");

    const std::string unsorted_tokens =
        "CARTOGRAPHER_PORT_ATTACHMENT_STATE 1\n"
        "ATTACHMENTS 1\n"
        "ATTACHMENT 81 2 \"token-b\" \"token-a\"\n"
        "END\n";
    require(
        !connection::PortAttachmentState::deserialize(unsorted_tokens),
        "unsorted attachment tokens refuse canonical validation");
}

} // namespace

int main() {
    graph_connects_only_compatible_ports();
    typed_edges_enforce_namespaces();
    attachment_cycles_fail_closed();
    graph_canonical_form_is_deterministic();
    acceptance_tiers_are_never_promoted_silently();
    attachment_preservation_survives_reimport();
    hostile_graph_inputs_fail_closed();
    hostile_attachment_inputs_fail_closed();
    std::cout << "connection port graph tests passed\n";
    return EXIT_SUCCESS;
}
