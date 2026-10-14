#include <carto/project/material_graph.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <utility>

namespace {

using carto::project::MaterialGraph;
using carto::project::MaterialGraphNode;
using carto::project::MaterialGraphNodeKind;
using carto::project::MaterialGraphValueType;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MaterialGraph graph_fixture(bool include_unsupported = true) {
    using namespace carto;
    MaterialGraph graph;
    graph.identity = "materials/test/surface";
    graph.revision = core::Revision{7U};
    const auto scalar = [](std::uint64_t id, double value) {
        MaterialGraphNode node;
        node.id = id;
        node.kind = MaterialGraphNodeKind::constant_scalar;
        node.value_type = MaterialGraphValueType::scalar;
        node.constants[0] = value;
        return node;
    };
    MaterialGraphNode texture_sample;
    texture_sample.id = 3U;
    texture_sample.kind = MaterialGraphNodeKind::texture_sample;
    texture_sample.value_type = MaterialGraphValueType::color;
    texture_sample.texture_asset = 501U;
    texture_sample.inputs = {1U, 2U};
    graph.nodes = {
        scalar(6U, 0.1), scalar(5U, 0.5), scalar(4U, 1.0), texture_sample,
        scalar(2U, 0.25), scalar(1U, 0.75),
    };
    if (include_unsupported) {
        MaterialGraphNode unknown;
        unknown.id = 7U;
        unknown.kind = MaterialGraphNodeKind::unsupported;
        unknown.value_type = MaterialGraphValueType::scalar;
        unknown.unsupported_identity = "unmapped-noise";
        graph.nodes.push_back(std::move(unknown));
    }
    graph.outputs.base_color = 3U;
    graph.outputs.alpha = 4U;
    graph.outputs.roughness = 5U;
    graph.outputs.metallic = 6U;
    return graph;
}

void deterministic_round_trip_and_compilation_report() {
    auto graph = graph_fixture();
    require(static_cast<bool>(graph.validate()), "well-typed graph validates");
    const auto order = graph.topological_order();
    require(order && order.value() == std::vector<std::uint64_t>{1U, 2U, 3U, 4U, 5U, 6U, 7U},
        "topological order is deterministic across node insertion order");

    const auto encoded = graph.serialize();
    const auto digest = graph.canonical_digest();
    require(encoded && digest, "valid graph produces serialized and digest evidence");
    auto reordered = graph;
    std::reverse(reordered.nodes.begin(), reordered.nodes.end());
    const auto reordered_bytes = reordered.serialize();
    const auto reordered_digest = reordered.canonical_digest();
    require(reordered_bytes && reordered_digest && reordered_bytes.value() == encoded.value() &&
            reordered_digest.value() == digest.value(),
        "node insertion order cannot change canonical graph identity");

    const auto reopened = MaterialGraph::deserialize(encoded.value());
    require(reopened && reopened.value().serialize().value() == encoded.value(),
        "graph survives digest-checked serialization and reopen");

    auto with_tombstone = graph_fixture(false);
    with_tombstone.retired_node_ids = {9U, 12U};
    const auto tombstone_encoded = with_tombstone.serialize();
    require(static_cast<bool>(tombstone_encoded), "tombstone graph serializes");
    const auto tombstone_reopened = MaterialGraph::deserialize(tombstone_encoded.value());
    require(tombstone_reopened &&
            tombstone_reopened.value().retired_node_ids ==
                std::vector<std::uint64_t>{9U, 12U},
        "retired node identities persist in canonical graph data");

    auto legacy_v1 = encoded.value();
    const auto retired_section = legacy_v1.find("RETIRED_NODES 0\n");
    const auto digest_marker = legacy_v1.find("DIGEST ");
    require(retired_section != std::string::npos && digest_marker != std::string::npos,
        "current graph fixture exposes schema-2 lineage and digest sections");
    legacy_v1.erase(retired_section, std::string("RETIRED_NODES 0\n").size());
    legacy_v1.replace(0U, std::string("CARTOGRAPHER_MATERIAL_GRAPH 2").size(),
        "CARTOGRAPHER_MATERIAL_GRAPH 1");
    const auto legacy_digest_marker = legacy_v1.find("DIGEST ");
    const auto legacy_digest = carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(legacy_v1.data()), legacy_digest_marker});
    legacy_v1.replace(legacy_digest_marker + std::string("DIGEST ").size(),
        std::string::npos, legacy_digest.hex() + "\n");
    const auto migrated_v1 = MaterialGraph::deserialize(legacy_v1);
    require(migrated_v1 && migrated_v1.value().retired_node_ids.empty() &&
            migrated_v1.value().serialize().value().find(
                "CARTOGRAPHER_MATERIAL_GRAPH 2") == 0U,
        "schema-1 graphs migrate to schema 2 with an explicit empty tombstone set");

    const auto unsupported = carto::project::compile_material_graph(graph);
    require(unsupported && !unsupported.value().supported &&
            unsupported.value().diagnostics.size() == 1U &&
            unsupported.value().diagnostics.front().node_id == 7U,
        "unknown nodes are retained but cannot be reported as supported lowering");
    auto supported_graph = graph_fixture(false);
    const auto supported = carto::project::compile_material_graph(supported_graph);
    require(supported && supported.value().supported && supported.value().diagnostics.empty(),
        "fully mapped graph receives a structural support report");
}

void hostile_graph_boundaries_fail_closed() {
    auto graph = graph_fixture(false);

    auto bad_type = graph;
    MaterialGraphNode wrong_type;
    wrong_type.id = 8U;
    wrong_type.kind = MaterialGraphNodeKind::add;
    wrong_type.value_type = MaterialGraphValueType::scalar;
    wrong_type.inputs = {3U, 1U};
    bad_type.nodes.push_back(std::move(wrong_type));
    require(!bad_type.validate(), "typed binary input mismatch is rejected");

    auto cycle = graph;
    MaterialGraphNode* first = nullptr;
    MaterialGraphNode* fifth = nullptr;
    for (auto& node : cycle.nodes) {
        if (node.id == 1U) first = &node;
        if (node.id == 5U) fifth = &node;
    }
    require(first != nullptr && fifth != nullptr, "cycle fixture nodes resolve");
    first->kind = MaterialGraphNodeKind::add;
    first->constants = {};
    first->inputs = {5U, 2U};
    fifth->kind = MaterialGraphNodeKind::multiply;
    fifth->constants = {};
    fifth->inputs = {1U, 2U};
    require(!cycle.validate(), "cyclic material node dependencies are rejected");

    auto duplicate = graph;
    duplicate.nodes.push_back(duplicate.nodes.front());
    require(!duplicate.validate(), "duplicate node identities are rejected");

    auto active_retired = graph;
    active_retired.retired_node_ids = {1U};
    require(!active_retired.validate(), "retired IDs cannot collide with active nodes");
    auto unsorted_retired = graph;
    unsorted_retired.retired_node_ids = {12U, 9U};
    require(!unsorted_retired.validate(), "retired node IDs must be canonically sorted");
    auto duplicate_retired = graph;
    duplicate_retired.retired_node_ids = {9U, 9U};
    require(!duplicate_retired.validate(), "retired node IDs cannot be duplicated");

    auto missing_input = graph;
    missing_input.nodes.back().inputs = {999U};
    require(!missing_input.validate(), "unresolved graph edges are rejected");

    auto wrong_output = graph;
    wrong_output.outputs.roughness = 3U;
    require(!wrong_output.validate(), "output channel type mismatch is rejected");

    auto traversal = graph;
    traversal.identity = "materials/../outside";
    require(!traversal.validate(), "path traversal graph identities are rejected");
    traversal.identity = "E:/private/material";
    require(!traversal.validate(), "absolute workstation graph identities are rejected");

    auto encoded = graph.serialize();
    require(static_cast<bool>(encoded), "tamper fixture serializes");
    auto tampered = encoded.value();
    const auto marker = tampered.find("materials/test/surface");
    tampered.replace(marker, std::string("materials/test/surface").size(),
        "materials/test/changed");
    require(!MaterialGraph::deserialize(tampered),
        "identity tampering is rejected by graph digest validation");
    require(!MaterialGraph::deserialize(encoded.value() + "unbound"),
        "trailing serialized graph data is rejected");
    require(!MaterialGraph::deserialize(std::string(MaterialGraph::kMaxSerializedBytes + 1U, 'x')),
        "oversized graph payload is rejected before parsing");

    const auto current_digest = graph.canonical_digest();
    require(current_digest && carto::project::validate_material_graph_source(
                graph, graph.identity, graph.revision, current_digest.value()),
        "current source identity, revision, and digest are admitted");
    require(!carto::project::validate_material_graph_source(
                graph, graph.identity, carto::core::Revision{8U}, current_digest.value()),
        "stale graph revision is rejected before derived work");
    require(!carto::project::validate_material_graph_source(
                graph, "materials/test/other", graph.revision, current_digest.value()),
        "substituted graph identity is rejected before derived work");
    auto stale_digest = current_digest.value();
    stale_digest.bytes[0] ^= 1U;
    require(!carto::project::validate_material_graph_source(
                graph, graph.identity, graph.revision, stale_digest),
        "stale source digest is rejected before derived work");
}

} // namespace

int main() {
    deterministic_round_trip_and_compilation_report();
    hostile_graph_boundaries_fail_closed();
    std::cout << "material graph tests passed\n";
    return EXIT_SUCCESS;
}
