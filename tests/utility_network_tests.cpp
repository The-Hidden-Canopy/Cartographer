#include <carto/condition/utility_network.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* expression) {
    if (!condition) throw std::runtime_error(expression);
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

using carto::condition::UtilityNetwork;
using carto::condition::UtilityNetworkEdit;
using carto::condition::UtilityNetworkKind;
using carto::condition::UtilityNetworkLink;
using carto::condition::UtilityNetworkNode;
using carto::condition::UtilityNodeKind;
using carto::condition::UtilityNodeState;

UtilityNetworkNode node(
    std::uint64_t id,
    UtilityNodeKind kind,
    std::string capacity_class = "distribution",
    UtilityNodeState state = UtilityNodeState::active) {
    return {id, kind, state, std::move(capacity_class)};
}

UtilityNetworkLink link(std::uint64_t id, std::uint64_t first, std::uint64_t second) {
    return {id, first, second, UtilityNodeState::active};
}

UtilityNetwork basic_network() {
    auto result = UtilityNetwork::create(7U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source, "trunk"),
            node(2U, UtilityNodeKind::junction),
            node(3U, UtilityNodeKind::sink, "service"),
            node(10U, UtilityNodeKind::junction),
            node(11U, UtilityNodeKind::service_connection, "service")},
        {link(1U, 1U, 2U), link(2U, 2U, 3U), link(3U, 10U, 11U)});
    REQUIRE(result);
    return std::move(result.value());
}

void typed_network_reports_source_continuity_and_components() {
    auto result = UtilityNetwork::create(7U, UtilityNetworkKind::power,
        {node(1U, UtilityNodeKind::source, "primary"),
            node(2U, UtilityNodeKind::junction, "distribution"),
            node(3U, UtilityNodeKind::sink, "service"),
            node(4U, UtilityNodeKind::service_connection, "service"),
            node(8U, UtilityNodeKind::sink, "service", UtilityNodeState::planned)},
        {link(1U, 1U, 2U), link(2U, 2U, 3U), link(3U, 2U, 4U)});
    REQUIRE(result);
    const auto report = result.value().inspect_connectivity();
    REQUIRE(report);
    REQUIRE(report.value().network_id == 7U);
    REQUIRE(report.value().kind == UtilityNetworkKind::power);
    REQUIRE(report.value().revision == 1U);
    REQUIRE(report.value().continuity_passed);
    REQUIRE(report.value().components.size() == 1U);
    REQUIRE((report.value().components.front().node_ids ==
        std::vector<std::uint64_t>{1U, 2U, 3U, 4U}));
    REQUIRE(report.value().components.front().has_active_source);
    REQUIRE(report.value().components.front().unserved_terminals.empty());
}

void unserved_active_terminals_fail_but_inactive_terminals_do_not() {
    auto result = UtilityNetwork::create(8U, UtilityNetworkKind::communications,
        {node(1U, UtilityNodeKind::sink),
            node(2U, UtilityNodeKind::service_connection),
            node(3U, UtilityNodeKind::sink, "service", UtilityNodeState::out_of_service)});
    REQUIRE(result);
    const auto report = result.value().inspect_connectivity();
    REQUIRE(report);
    REQUIRE(!report.value().continuity_passed);
    REQUIRE(report.value().components.size() == 2U);
    REQUIRE(report.value().components[0].unserved_terminals ==
        std::vector<std::uint64_t>{1U});
    REQUIRE(report.value().components[1].unserved_terminals ==
        std::vector<std::uint64_t>{2U});
}

void malformed_sources_and_links_fail_closed() {
    REQUIRE(!UtilityNetwork::create(0U, UtilityNetworkKind::water));
    REQUIRE(!UtilityNetwork::create(1U, static_cast<UtilityNetworkKind>(255U)));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source, "")}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source, " \t ")}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {UtilityNetworkNode{1U, static_cast<UtilityNodeKind>(255U),
            UtilityNodeState::active, "service"}}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {UtilityNetworkNode{1U, UtilityNodeKind::source,
            static_cast<UtilityNodeState>(255U), "service"}}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source), node(1U, UtilityNodeKind::sink)}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source), node(2U, UtilityNodeKind::sink)},
        {link(1U, 1U, 3U)}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source)}, {link(1U, 1U, 1U)}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source), node(2U, UtilityNodeKind::sink)},
        {link(1U, 1U, 2U), link(2U, 2U, 1U)}));
    REQUIRE(!UtilityNetwork::create(1U, UtilityNetworkKind::water,
        {node(1U, UtilityNodeKind::source), node(2U, UtilityNodeKind::sink)},
        {UtilityNetworkLink{1U, 1U, 2U,
            static_cast<UtilityNodeState>(255U)}}));
}

void edits_are_revision_bound_atomic_and_invalidate_only_local_components() {
    auto network = basic_network();
    auto changed_node = node(2U, UtilityNodeKind::junction, "local-distribution");
    const auto edit = network.apply(UtilityNetworkEdit{
        1U, carto::condition::UtilityUpsertNode{changed_node}});
    REQUIRE(edit);
    REQUIRE(edit.value().revision_before == 1U);
    REQUIRE(edit.value().revision_after == 2U);
    REQUIRE(edit.value().changed_node_ids == std::vector<std::uint64_t>{2U});
    REQUIRE((edit.value().invalidated_node_ids ==
        std::vector<std::uint64_t>{1U, 2U, 3U}));
    REQUIRE(network.revision() == 2U);
    REQUIRE(network.nodes()[1].capacity_class == "local-distribution");

    const auto stale = network.apply(UtilityNetworkEdit{
        1U, carto::condition::UtilityRemoveNode{2U}});
    REQUIRE(!stale);
    REQUIRE(network.revision() == 2U);
    REQUIRE(network.nodes().size() == 5U);

    const auto invalid_link = network.apply(UtilityNetworkEdit{
        2U, carto::condition::UtilityUpsertLink{link(4U, 3U, 99U)}});
    REQUIRE(!invalid_link);
    REQUIRE(network.revision() == 2U);
    REQUIRE(network.links().size() == 3U);

    const auto no_op = network.apply(UtilityNetworkEdit{
        2U, carto::condition::UtilityUpsertNode{network.nodes()[1]}});
    REQUIRE(!no_op);
    REQUIRE(network.revision() == 2U);

    const auto reversed_no_op = network.apply(UtilityNetworkEdit{
        2U, carto::condition::UtilityUpsertLink{link(1U, 2U, 1U)}});
    REQUIRE(!reversed_no_op);
    REQUIRE(network.revision() == 2U);
}

void only_active_links_and_sources_satisfy_active_service_terminals() {
    auto created = UtilityNetwork::create(9U, UtilityNetworkKind::fuel,
        {node(1U, UtilityNodeKind::source), node(2U, UtilityNodeKind::sink)},
        {UtilityNetworkLink{1U, 1U, 2U, UtilityNodeState::planned}});
    REQUIRE(created);
    auto network = std::move(created.value());
    auto report = network.inspect_connectivity();
    REQUIRE(report);
    REQUIRE(!report.value().continuity_passed);

    const auto activate_link = network.apply(UtilityNetworkEdit{
        1U, carto::condition::UtilityUpsertLink{link(1U, 1U, 2U)}});
    REQUIRE(activate_link);
    report = network.inspect_connectivity();
    REQUIRE(report);
    REQUIRE(report.value().continuity_passed);

    const auto disable_source = network.apply(UtilityNetworkEdit{
        2U, carto::condition::UtilityUpsertNode{
            node(1U, UtilityNodeKind::source, "distribution",
                UtilityNodeState::out_of_service)}});
    REQUIRE(disable_source);
    REQUIRE((disable_source.value().invalidated_node_ids ==
        std::vector<std::uint64_t>{1U, 2U}));
    report = network.inspect_connectivity();
    REQUIRE(report);
    REQUIRE(!report.value().continuity_passed);
    REQUIRE(report.value().components.front().unserved_terminals ==
        std::vector<std::uint64_t>{2U});
}

void merging_and_splitting_components_invalidates_both_old_and_new_sides() {
    auto network = basic_network();
    const auto merge = network.apply(UtilityNetworkEdit{
        1U, carto::condition::UtilityUpsertLink{link(4U, 3U, 10U)}});
    REQUIRE(merge);
    REQUIRE((merge.value().invalidated_node_ids ==
        std::vector<std::uint64_t>{1U, 2U, 3U, 10U, 11U}));

    const auto split = network.apply(UtilityNetworkEdit{
        2U, carto::condition::UtilityRemoveLink{4U}});
    REQUIRE(split);
    REQUIRE((split.value().invalidated_node_ids ==
        std::vector<std::uint64_t>{1U, 2U, 3U, 10U, 11U}));
    REQUIRE(network.revision() == 3U);
}

void node_removal_removes_incident_links_and_invalidates_prior_component() {
    auto network = basic_network();
    const auto removal = network.apply(UtilityNetworkEdit{
        1U, carto::condition::UtilityRemoveNode{2U}});
    REQUIRE(removal);
    REQUIRE(removal.value().changed_node_ids == std::vector<std::uint64_t>{2U});
    REQUIRE((removal.value().invalidated_node_ids ==
        std::vector<std::uint64_t>{1U, 2U, 3U}));
    REQUIRE(network.nodes().size() == 4U);
    REQUIRE(network.links().size() == 1U);
    REQUIRE(network.links().front().first == 10U);
    REQUIRE(network.links().front().second == 11U);
}

} // namespace

int main() {
    try {
        typed_network_reports_source_continuity_and_components();
        unserved_active_terminals_fail_but_inactive_terminals_do_not();
        malformed_sources_and_links_fail_closed();
        edits_are_revision_bound_atomic_and_invalidate_only_local_components();
        only_active_links_and_sources_satisfy_active_service_terminals();
        merging_and_splitting_components_invalidates_both_old_and_new_sides();
        node_removal_removes_incident_links_and_invalidates_prior_component();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
