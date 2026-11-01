#include <carto/weights/transfer_record.hpp>
#include <carto/assets/blob_store.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
carto::assets::Sha256Digest digest(std::string_view text) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
}

int main() {
    using namespace carto;
    kinematic::KinematicGraph graph;
    kinematic::KinematicNode root;
    root.id = kinematic::JointId{1};
    require(static_cast<bool>(graph.insert_node(root)), "insert graph root");
    const auto graph_digest = graph.canonical_digest();
    require(static_cast<bool>(graph_digest), "valid graph");

    kinematic::SkinBinding source;
    source.mesh_asset = 7;
    source.mesh_digest = digest("source mesh");
    source.graph_digest = graph_digest.value();
    source.influences.emplace(0, std::vector<kinematic::JointInfluence>{{{1}, 1.0}});
    character::SkinWeightLayer source_layer;
    source_layer.layer_id = character::RigLayerId{1};
    source_layer.mesh_topology_digest = source.mesh_digest;
    source_layer.joint_graph_digest = source.graph_digest;
    source_layer.influenced_vertex_count = 1;
    source_layer.total_influence_count = 1;
    source_layer.provenance_digest = digest("authored");
    weights::WeightCorrespondence correspondence;
    require(static_cast<bool>(correspondence.map(9, {{0, 1.0}})), "map target");
    weights::WeightTransferOptions options;
    options.output_layer_id = character::RigLayerId{2};
    options.output_mesh_asset = 8;
    options.output_mesh_digest = digest("target mesh");
    const auto transferred = weights::transfer_weights(source, source_layer,
        correspondence, weights::JointTransferMap{}, options);
    require(static_cast<bool>(transferred), "transfer succeeds");
    weights::TransferredWeightRecord record{transferred.value()};
    require(static_cast<bool>(record.validate_against_graph(graph)), "record and graph validate");
    const auto bytes = record.serialize();
    const auto decoded = weights::TransferredWeightRecord::deserialize(bytes);
    require(static_cast<bool>(decoded), "canonical record parses");
    require(decoded.value().serialize() == bytes, "round-trip preserves complete record");
    require(decoded.value().output.binding.influences.at(9).at(0).joint.value == 1,
        "round-trip preserves target influence");
    require(record.canonical_digest().value() == decoded.value().canonical_digest().value(),
        "record digest survives round-trip");
    weights::WeightLocks locks;
    require(static_cast<bool>(locks.lock(9, kinematic::JointId{1})), "lock target joint");
    const auto locked_transfer = weights::transfer_weights(source, source_layer,
        correspondence, weights::JointTransferMap{}, options, &locks);
    require(static_cast<bool>(locked_transfer), "locked transfer succeeds");
    weights::TransferredWeightRecord locked_record{locked_transfer.value()};
    const auto locked_decoded = weights::TransferredWeightRecord::deserialize(locked_record.serialize());
    require(static_cast<bool>(locked_decoded) && locked_decoded.value().output.layer.locks_digest.has_value(),
        "lock pin survives round-trip");

    auto altered = record;
    altered.output.binding.influences.at(9).at(0).joint.value = 2;
    require(!altered.validate(), "binding tamper fails receipt pin");
    altered = record;
    altered.output.layer.total_influence_count = 2;
    require(!altered.validate(), "incorrect layer population fails");
    altered = record;
    altered.output.layer.mesh_topology_digest = digest("different mesh");
    require(!altered.validate(), "mesh pin mismatch fails");
    altered = record;
    altered.output.receipt.output_binding_digest = digest("different binding");
    require(!altered.validate(), "receipt pin mismatch fails");
    altered = record;
    altered.output.layer.locks_digest = digest("locks");
    require(!altered.validate(), "unmatched lock pin fails");
    altered = record;
    altered.output.layer.masks_digest = digest("mask");
    require(!altered.validate(), "post-transfer mask cannot masquerade as raw output");
    altered = record;
    altered.output.layer.staleness = character::WeightStaleness::stale_mesh_topology;
    altered.output.layer.stale_since = core::Revision{1};
    require(!altered.validate(), "stale layer cannot masquerade as fresh transfer output");
    altered = record;
    altered.output.receipt.transferred_vertex_count = 2;
    require(!altered.validate(), "receipt population mismatch fails");
    require(!weights::TransferredWeightRecord::deserialize(bytes + " "), "trailing data fails");
    require(!weights::TransferredWeightRecord::deserialize(bytes.substr(0, bytes.size() - 3)),
        "truncated bytes fail");
    auto noncanonical = bytes;
    noncanonical.insert(noncanonical.find("LAYER ") + 6, "0");
    require(!weights::TransferredWeightRecord::deserialize(noncanonical), "noncanonical integer fails");
    auto oversized_length = bytes;
    const auto size_start = oversized_length.find("BINDING ") + 8;
    const auto size_end = oversized_length.find('\n', size_start);
    oversized_length.replace(size_start, size_end - size_start, "99999999999999999999");
    require(!weights::TransferredWeightRecord::deserialize(oversized_length), "oversized frame fails");

    // Even a self-consistent record is not admissible against a graph whose
    // joint set omits an influence, despite matching its graph digest.
    altered = record;
    altered.output.binding.influences.at(9).at(0).joint.value = 2;
    altered.output.receipt.output_binding_digest = altered.output.binding.canonical_digest().value();
    require(static_cast<bool>(altered.validate()), "modified binding can be internally consistent");
    require(!altered.validate_against_graph(graph), "unknown influenced joint fails");
    kinematic::KinematicGraph other;
    root.id = kinematic::JointId{3};
    require(static_cast<bool>(other.insert_node(root)), "insert other graph root");
    require(!record.validate_against_graph(other), "different graph fails");
    std::cout << "transferred weight record tests passed\n";
}
