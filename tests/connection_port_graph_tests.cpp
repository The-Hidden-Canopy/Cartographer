#include <carto/connection/attachment.hpp>
#include <carto/connection/graph.hpp>
#include <carto/connection/port.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <locale>
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

void edge_identity_exhaustion_fails_closed() {
    auto graph = make_graph(71U);
    const auto initial = graph.connect(
        connection::PortId{71U}, connection::PortId{72U},
        connection::ConnectionKind::mechanical);
    require(static_cast<bool>(initial), "edge identity fixture connects");
    std::string encoded = graph.serialize();
    const std::string original = "EDGE " + std::to_string(initial.value().id) + " ";
    const auto edge_record = encoded.find(original);
    require(edge_record != std::string::npos, "serialized edge identity is present");
    const std::string exhausted = "EDGE " +
        std::to_string(std::numeric_limits<std::uint64_t>::max()) + " ";
    encoded.replace(edge_record, original.size(), exhausted);

    auto restored = connection::ConnectionPortGraph::deserialize(encoded);
    require(static_cast<bool>(restored), "maximum edge identity remains readable");
    const auto edge_count = restored.value().edges().size();
    const auto next_edge = restored.value().connect(
        connection::PortId{71U}, connection::PortId{73U},
        connection::ConnectionKind::mechanical);
    require(!next_edge && next_edge.error().code == core::ErrorCode::invalid_state &&
        restored.value().edges().size() == edge_count,
        "exhausted edge identity refuses without wrapping or mutating the graph");
    require(static_cast<bool>(restored.value().erase_edge(
        std::numeric_limits<std::uint64_t>::max())), "the exhausted edge can be removed");
    auto deleted_maximum = connection::ConnectionPortGraph::deserialize(restored.value().serialize());
    require(static_cast<bool>(deleted_maximum), "exhaustion survives deletion and reopen");
    const auto deleted_bytes = deleted_maximum.value().serialize();
    require(!deleted_maximum.value().connect(
        connection::PortId{71U}, connection::PortId{73U},
        connection::ConnectionKind::mechanical) &&
        deleted_maximum.value().serialize() == deleted_bytes,
        "deletion cannot reset exhausted allocation after reopen");

    const std::string final_id = "EDGE " +
        std::to_string(std::numeric_limits<std::uint64_t>::max() - 1U) + " ";
    const auto maximum_record = encoded.find(exhausted);
    require(maximum_record != std::string::npos,
        "maximum edge identity fixture record is present");
    encoded.replace(maximum_record, exhausted.size(), final_id);
    auto final_slot = connection::ConnectionPortGraph::deserialize(encoded);
    require(static_cast<bool>(final_slot), "penultimate edge identity remains readable");
    const auto last_edge = final_slot.value().connect(
        connection::PortId{71U}, connection::PortId{73U},
        connection::ConnectionKind::mechanical);
    require(last_edge && last_edge.value().id == std::numeric_limits<std::uint64_t>::max(),
        "the maximum valid edge identity is allocated exactly once");
    const auto exhausted_after_final = final_slot.value().connect(
        connection::PortId{72U}, connection::PortId{73U},
        connection::ConnectionKind::mechanical);
    require(!exhausted_after_final &&
        exhausted_after_final.error().code == core::ErrorCode::invalid_state &&
        final_slot.value().edges().size() == edge_count + 1U,
        "a graph refuses further edges after using the maximum identity");
}

void port_and_edge_removal_is_explicit_and_fail_closed() {
    auto graph = make_graph(81U);
    const auto edge = graph.connect(
        connection::PortId{81U}, connection::PortId{82U},
        connection::ConnectionKind::mechanical);
    require(static_cast<bool>(edge), "removal fixture connects compatible ports");
    const auto attached_port = graph.erase_port(connection::PortId{81U});
    require(!attached_port && graph.ports().size() == 3U && graph.edges().size() == 1U,
        "a port with an edge cannot be removed implicitly");
    const auto missing_edge = graph.erase_edge(999U);
    require(!missing_edge && missing_edge.error().code == core::ErrorCode::not_found &&
        graph.edges().size() == 1U,
        "removing a missing edge refuses without changing the graph");
    require(static_cast<bool>(graph.erase_edge(edge.value().id)) && graph.edges().empty(),
        "an explicit edge removal succeeds");
    require(static_cast<bool>(graph.erase_port(connection::PortId{81U})) &&
        graph.find_port(connection::PortId{81U}) == nullptr,
        "a disconnected port can then be explicitly removed");
    const auto missing_port = graph.erase_port(connection::PortId{81U});
    require(!missing_port && missing_port.error().code == core::ErrorCode::not_found,
        "removing a missing port returns not_found");
    require(static_cast<bool>(graph.validate()),
        "explicit edge and port removals preserve graph validity");
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

void deleted_edge_allocation_history_survives_reopen() {
    auto graph = make_graph(91U);
    const auto first = graph.connect(connection::PortId{91U}, connection::PortId{92U},
        connection::ConnectionKind::mechanical);
    require(first && graph.erase_edge(first.value().id), "first edge is explicitly removed");
    const auto encoded = graph.serialize();
    auto reopened = connection::ConnectionPortGraph::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "deleted-edge allocation state round-trips canonically");
    const auto next = reopened.value().connect(connection::PortId{91U}, connection::PortId{93U},
        connection::ConnectionKind::mechanical);
    require(next && next.value().id > first.value().id,
        "reopening after deletion must not reuse an admitted edge identity");
    const auto live = reopened.value().serialize();
    const auto third = reopened.value().connect(connection::PortId{92U}, connection::PortId{93U},
        connection::ConnectionKind::mechanical);
    require(third && reopened.value().erase_edge(next.value().id),
        "removing a lower identity leaves the live maximum intact");
    auto lower_removed = connection::ConnectionPortGraph::deserialize(reopened.value().serialize());
    require(static_cast<bool>(lower_removed), "lower-edge deletion round-trips");
    const auto after_lower_deletion = lower_removed.value().connect(
        connection::PortId{91U}, connection::PortId{92U},
        connection::ConnectionKind::mechanical);
    require(after_lower_deletion && after_lower_deletion.value().id > third.value().id,
        "surviving maximum retains monotonic allocation after lower deletion");

    auto overlap = live;
    overlap.replace(0U, std::string{"CARTOGRAPHER_CONNECTION_PORT_GRAPH 1"}.size(),
        "CARTOGRAPHER_CONNECTION_PORT_GRAPH 2");
    overlap.insert(overlap.rfind("END\n"), "NEXT_EDGE_ID 2\n");
    require(!connection::ConnectionPortGraph::deserialize(overlap),
        "an allocation watermark overlapping a live edge refuses");
    for (const auto token : {"-1", "+2", "18446744073709551616", "2x"}) {
        auto malformed = encoded;
        const auto value = malformed.find("NEXT_EDGE_ID ") + std::string{"NEXT_EDGE_ID "}.size();
        malformed.replace(value, malformed.find('\n', value) - value, token);
        require(!connection::ConnectionPortGraph::deserialize(malformed),
            "malformed allocation watermarks refuse without numeric coercion");
    }
    auto missing = encoded;
    missing.erase(missing.find("NEXT_EDGE_ID "), std::string{"NEXT_EDGE_ID 2\n"}.size());
    require(!connection::ConnectionPortGraph::deserialize(missing),
        "version two requires explicit allocation state");
    auto downgraded = encoded;
    downgraded.replace(0U, std::string{"CARTOGRAPHER_CONNECTION_PORT_GRAPH 2"}.size(),
        "CARTOGRAPHER_CONNECTION_PORT_GRAPH 1");
    require(!connection::ConnectionPortGraph::deserialize(downgraded),
        "legacy framing refuses an unbound allocation record");
}

void acceptance_tier_decoder_refuses_numeric_narrowing() {
    connection::PortGraphAcceptanceReceipt receipt;
    receipt.graph_digest = digest_of("tier-decoder-source");
    std::size_t admitted = 0U;
    for (unsigned rank = 0U; rank <= 3U; ++rank) {
        const auto original = receipt.serialize();
        const auto valid = connection::PortGraphAcceptanceReceipt::deserialize(original);
        require(valid && valid.value().serialize() == original,
            "every valid acceptance tier retains exact receipt bytes");
        for (const auto& token : {std::to_string(rank + 256U), std::to_string(rank + 512U),
                 std::string{"-1"}, std::string{"+1"}, std::string{"4"},
                 std::string{"18446744073709551616"}, std::string{"1x"}}) {
            auto malformed = original;
            const auto start = malformed.find("TIER ") + std::string{"TIER "}.size();
            malformed.replace(start, malformed.find('\n', start) - start, token);
            if (connection::PortGraphAcceptanceReceipt::deserialize(malformed)) {
                std::cerr << "Invalid acceptance tier accepted: " << token << '\n';
                ++admitted;
            }
        }
        if (rank < 3U) {
            const auto next = connection::advance_port_graph_acceptance(receipt,
                static_cast<connection::PortGraphAcceptanceTier>(rank + 1U),
                digest_of("tier-evidence-" + std::to_string(rank)));
            require(static_cast<bool>(next), "valid receipt progresses exactly one tier");
            receipt = next.value();
        }
    }
    require(admitted == 0U, "acceptance tier numbers must be checked before enum narrowing");
}

void graph_codec_ignores_process_numeric_locale() {
    struct GroupedNumbers final : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
        char do_thousands_sep() const override { return '.'; }
        std::string do_grouping() const override { return "\3"; }
    };
    auto graph = make_graph(1001U);
    const auto edge = graph.connect(connection::PortId{1001U}, connection::PortId{1002U},
        connection::ConnectionKind::mechanical);
    require(edge && graph.erase_edge(edge.value().id), "locale fixture retains a retired edge");
    const auto canonical = graph.serialize();
    const auto previous = std::locale::global(std::locale(std::locale::classic(), new GroupedNumbers));
    const auto localized = graph.serialize();
    const auto decoded = connection::ConnectionPortGraph::deserialize(canonical);
    std::locale::global(previous);
    require(localized == canonical && decoded && decoded.value().serialize() == canonical,
        "numeric locale cannot change graph framing or identity history");
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

void loaded_edges_recheck_the_authored_connection_contract() {
    auto graph = make_graph(101U);
    require(static_cast<bool>(graph.connect(connection::PortId{101U}, connection::PortId{102U},
        connection::ConnectionKind::mechanical)), "admission fixture connects");
    const auto before = graph.serialize();
    std::size_t admitted = 0U;
    const auto check_refusal = [&admitted](const auto& candidate, const char* label) {
        if (candidate.validate() || candidate.canonical_digest() ||
            connection::ConnectionPortGraph::deserialize(candidate.serialize())) {
            std::cerr << "Invalid graph accepted: " << label << '\n';
            ++admitted;
        }
    };
    auto fabricated = graph;
    auto& fabricated_edge = const_cast<connection::PortConnectionEdge&>(
        fabricated.edges().begin()->second);
    fabricated_edge.evidence.first_satisfied_requirements = {"power-transfer"};
    check_refusal(fabricated, "fabricated satisfied requirement");

    auto incomplete = graph;
    auto& incomplete_edge = const_cast<connection::PortConnectionEdge&>(
        incomplete.edges().begin()->second);
    incomplete_edge.evidence.shared_tags.pop_back();
    check_refusal(incomplete, "incomplete shared-tag evidence");

    auto wrong_kind = graph;
    const_cast<connection::PortConnectionEdge&>(wrong_kind.edges().begin()->second).kind =
        connection::ConnectionKind::maritime;
    check_refusal(wrong_kind, "wrong edge-kind namespace");

    auto unsatisfied = graph;
    auto& changed_port = const_cast<connection::ConnectionPort&>(
        unsatisfied.ports().begin()->second);
    changed_port.capability_contract.required = {"power-transfer"};
    check_refusal(unsatisfied, "new unsatisfied endpoint requirement");

    auto changed_tags = graph;
    const_cast<connection::ConnectionPort&>(changed_tags.ports().begin()->second).
        compatibility_tags = {connection::compatibility::utility::electrical_16a};
    check_refusal(changed_tags, "stale endpoint compatibility evidence");

    auto parallel = graph;
    require(static_cast<bool>(parallel.connect(connection::PortId{102U}, connection::PortId{103U},
        connection::ConnectionKind::mechanical)), "parallel fixture has two admitted edges");
    auto& parallel_edge = const_cast<connection::PortConnectionEdge&>(parallel.edges().rbegin()->second);
    parallel_edge.first_port = connection::PortId{102U};
    parallel_edge.second_port = connection::PortId{101U};
    parallel_edge.kind = connection::ConnectionKind::utility;
    parallel_edge.evidence = graph.edges().begin()->second.evidence;
    check_refusal(parallel, "two-edge cycle with different kinds and reversed endpoints");

    for (const auto kind : {"256", "260", "18446744073709551615"}) {
        auto wrapped_kind = before;
        const std::string original = "EDGE 1 101 102 0";
        wrapped_kind.replace(wrapped_kind.find(original), original.size(),
            std::string{"EDGE 1 101 102 "} + kind);
        if (connection::ConnectionPortGraph::deserialize(wrapped_kind)) {
            std::cerr << "Invalid graph accepted: narrowed edge kind " << kind << '\n';
            ++admitted;
        }
    }
    require(graph.serialize() == before && graph.validate(),
        "admission probes do not change valid source graph bytes");
    require(admitted == 0U, "loaded edges must enforce the authored connection contract");
}

void exact_endpoint_requirements_survive_graph_readback() {
    connection::ConnectionPortGraph graph;
    auto first = make_port(201U, 301U, hitch_tags());
    auto second = make_port(202U, 302U, hitch_tags());
    first.capability_contract.required = {std::string(connection::capabilities::power_transfer)};
    first.capability_contract.offered = {std::string(connection::capabilities::signal_transfer)};
    second.capability_contract.required = {std::string(connection::capabilities::signal_transfer)};
    second.capability_contract.offered = {std::string(connection::capabilities::power_transfer)};
    require(graph.insert_port(first) && graph.insert_port(second), "capability fixture ports are valid");
    const auto edge = graph.connect(first.id, second.id, connection::ConnectionKind::mechanical);
    require(edge && graph.validate() &&
        edge.value().evidence.first_satisfied_requirements == first.capability_contract.required &&
        edge.value().evidence.second_satisfied_requirements == second.capability_contract.required,
        "admission captures exact asymmetric endpoint requirements");
    const auto encoded = graph.serialize();
    const auto reopened = connection::ConnectionPortGraph::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "valid capability-bearing graphs retain exact bytes across readback");
    auto swapped_evidence = graph;
    auto& altered = const_cast<connection::PortConnectionEdge&>(swapped_evidence.edges().begin()->second);
    std::swap(altered.evidence.first_satisfied_requirements, altered.evidence.second_satisfied_requirements);
    require(!swapped_evidence.validate() &&
        !connection::ConnectionPortGraph::deserialize(swapped_evidence.serialize()),
        "opposite-side requirements cannot be substituted for exact endpoint evidence");
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
    edge_identity_exhaustion_fails_closed();
    port_and_edge_removal_is_explicit_and_fail_closed();
    deleted_edge_allocation_history_survives_reopen();
    graph_codec_ignores_process_numeric_locale();
    acceptance_tiers_are_never_promoted_silently();
    acceptance_tier_decoder_refuses_numeric_narrowing();
    attachment_preservation_survives_reimport();
    hostile_graph_inputs_fail_closed();
    loaded_edges_recheck_the_authored_connection_contract();
    exact_endpoint_requirements_survive_graph_readback();
    hostile_attachment_inputs_fail_closed();
    std::cout << "connection port graph tests passed\n";
    return EXIT_SUCCESS;
}
