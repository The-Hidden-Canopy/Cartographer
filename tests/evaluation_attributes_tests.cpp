#include <carto/attributes/set.hpp>
#include <carto/attributes/provenance.hpp>
#include <carto/attributes/transfer.hpp>
#include <carto/eval/cache.hpp>
#include <carto/eval/graph.hpp>
#include <carto/eval/receipt.hpp>
#include <carto/eval/scheduler.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/mesh_attributes/provenance.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

carto::eval::NodeTypeDefinition source_type() {
    using namespace carto::eval;
    return NodeTypeDefinition{
        NodeTypeId{"source_mesh"},
        {PortDescriptor{PortId{"mesh"}, PortDirection::output, ValueKind::editable_mesh}},
    };
}

carto::eval::NodeTypeDefinition transform_type() {
    using namespace carto::eval;
    return NodeTypeDefinition{
        NodeTypeId{"transform"},
        {
            PortDescriptor{PortId{"source"}, PortDirection::input, ValueKind::editable_mesh},
            PortDescriptor{PortId{"result"}, PortDirection::output, ValueKind::editable_mesh},
        },
    };
}

carto::eval::NodeTypeDefinition output_type() {
    using namespace carto::eval;
    return NodeTypeDefinition{
        NodeTypeId{"output"},
        {PortDescriptor{PortId{"mesh"}, PortDirection::input, ValueKind::editable_mesh}},
    };
}

void graph_rejects_cycles_and_preserves_deterministic_order() {
    using namespace carto;
    eval::EvaluationGraph graph;
    REQUIRE(graph.register_type(source_type()));
    REQUIRE(graph.register_type(transform_type()));
    REQUIRE(graph.register_type(output_type()));
    const auto source = graph.create_node(eval::NodeTypeId{"source_mesh"}, "source-v1");
    const auto transform = graph.create_node(eval::NodeTypeId{"transform"}, "transform-v1");
    const auto output = graph.create_node(eval::NodeTypeId{"output"}, "output-v1");
    REQUIRE(source && transform && output);
    REQUIRE(graph.connect({
        source.value(), {"mesh"}, transform.value(), {"source"}}));
    REQUIRE(graph.connect({
        transform.value(), {"result"}, output.value(), {"mesh"}}));
    REQUIRE(graph.validate());
    const auto order = graph.topological_order();
    REQUIRE(order);
    REQUIRE(order.value().size() == 3U);
    REQUIRE(order.value().front() == source.value());
    REQUIRE(order.value().back() == output.value());
    const auto digest = graph.content_digest();
    REQUIRE(digest.size() == 64U);

    const auto cycle = graph.connect({
        transform.value(), {"result"}, transform.value(), {"source"}});
    REQUIRE(!cycle);
    REQUIRE(cycle.error().code == core::ErrorCode::cycle_detected);
    REQUIRE(graph.connection_count() == 2U);

    const auto serialized = graph.serialize();
    REQUIRE(serialized);
    const auto reopened = eval::EvaluationGraph::deserialize(serialized.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().content_digest() == graph.content_digest());
    REQUIRE(reopened.value().node_count() == graph.node_count());
    REQUIRE(reopened.value().connection_count() == graph.connection_count());

    const auto trailing = eval::EvaluationGraph::deserialize(serialized.value() + "unexpected");
    REQUIRE(!trailing);
    REQUIRE(trailing.error().code == core::ErrorCode::validation_failed);

    const auto unsafe_digest = graph.create_node(eval::NodeTypeId{"source_mesh"}, "bad digest");
    REQUIRE(!unsafe_digest);
    REQUIRE(unsafe_digest.error().code == core::ErrorCode::invalid_argument);
}

void cache_and_publication_gate_reject_stale_worker_results() {
    using namespace carto;
    eval::EvaluationCache cache;
    const eval::EvaluationCacheKey key{eval::NodeId{1U}, core::Revision{4U}, "dependency-a"};
    REQUIRE(cache.put(key, eval::EvaluationCacheValue{"output-a"}));
    REQUIRE(cache.find(key).has_value());
    REQUIRE(cache.find(key)->output_digest == "output-a");
    const auto exhausted_cache = cache.put(
        eval::EvaluationCacheKey{
            eval::NodeId{2U}, core::Revision{core::Revision::max_value()}, "dependency-b"},
        eval::EvaluationCacheValue{"output-b"});
    REQUIRE(!exhausted_cache);
    REQUIRE(exhausted_cache.error().code == core::ErrorCode::invalid_argument);

    const eval::EvaluationTicket ticket{
        eval::NodeId{1U}, core::Revision{8U}, core::Revision{4U}, "dependency-a"};
    REQUIRE(eval::validate_publication(
        ticket, core::Revision{8U}, core::Revision{4U}, "dependency-a"));
    const auto stale = eval::validate_publication(
        ticket, core::Revision{9U}, core::Revision{4U}, "dependency-a");
    REQUIRE(!stale);
    REQUIRE(stale.error().code == core::ErrorCode::stale_data);

    const eval::EvaluationReceipt exhausted_receipt{
        eval::NodeId{1U}, core::Revision{core::Revision::max_value()}, core::Revision{4U},
        "dependency-a", "output-a", eval::EvaluationStatus::evaluated};
    const auto invalid_receipt = eval::validate(exhausted_receipt);
    REQUIRE(!invalid_receipt);
    REQUIRE(invalid_receipt.error().code == core::ErrorCode::invalid_state);
}

void scheduler_is_bounded_and_only_returns_candidates() {
    using namespace carto;
    eval::EvaluationScheduler scheduler(1U, 1U);
    const eval::EvaluationTicket ticket{
        eval::NodeId{7U}, core::Revision{2U}, core::Revision{1U}, "dependency"};
    REQUIRE(scheduler.submit(ticket, [] {
        return core::Result<eval::EvaluationCacheValue>::success(
            eval::EvaluationCacheValue{"candidate-output"});
    }));
    const auto full = scheduler.submit(ticket, [] {
        return core::Result<eval::EvaluationCacheValue>::success(
            eval::EvaluationCacheValue{"second-output"});
    });
    REQUIRE(!full);
    REQUIRE(full.error().code == core::ErrorCode::invalid_state);
    scheduler.stop();
    const auto candidate = scheduler.poll();
    REQUIRE(candidate.has_value());
    REQUIRE(candidate->receipt.status == eval::EvaluationStatus::evaluated);
    REQUIRE(candidate->output.has_value());
    REQUIRE(candidate->output->output_digest == "candidate-output");
    REQUIRE(scheduler.outstanding() == 0U);
}

void scheduler_shutdown_rejects_new_work_and_preserves_failure_receipts() {
    using namespace carto;
    eval::EvaluationScheduler scheduler(2U, 4U);
    const eval::EvaluationTicket throwing_ticket{
        eval::NodeId{8U}, core::Revision{3U}, core::Revision{2U}, "throwing"};
    const eval::EvaluationTicket failed_ticket{
        eval::NodeId{9U}, core::Revision{3U}, core::Revision{2U}, "failed"};
    REQUIRE(scheduler.submit(throwing_ticket, []() -> core::Result<eval::EvaluationCacheValue> {
        throw std::runtime_error("adversarial evaluator failure");
    }));
    REQUIRE(scheduler.submit(failed_ticket, [] {
        return core::Result<eval::EvaluationCacheValue>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed, "candidate rejected"));
    }));

    scheduler.stop();
    scheduler.stop();
    const auto rejected = scheduler.submit(throwing_ticket, [] {
        return core::Result<eval::EvaluationCacheValue>::success(
            eval::EvaluationCacheValue{"must-not-run"});
    });
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::invalid_state);

    std::size_t failed_candidates = 0U;
    while (const auto candidate = scheduler.poll()) {
        REQUIRE(candidate->receipt.status == eval::EvaluationStatus::failed);
        REQUIRE(candidate->diagnostic.has_value());
        ++failed_candidates;
    }
    REQUIRE(failed_candidates == 2U);
    REQUIRE(scheduler.outstanding() == 0U);
}

void corner_attributes_interpolate_and_discrete_layers_invalidate() {
    using namespace carto;
    attributes::AttributeSet source({
        {attributes::AttributeDomain::corner, 2U},
        {attributes::AttributeDomain::face, 2U},
    });
    REQUIRE(source.add_layer(attributes::AttributeLayer{
        {"uv0", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 1.0}},
    }));
    REQUIRE(source.add_layer(attributes::AttributeLayer{
        {"material_slot", attributes::AttributeDomain::face, attributes::AttributeType::uint32},
        {std::uint32_t{3U}, std::uint32_t{5U}},
    }));
    REQUIRE(source.validate());

    attributes::AttributeTransferPolicy policy;
    policy.default_mode = attributes::TransferMode::invalidate;
    policy.per_layer.emplace("uv0", attributes::TransferMode::interpolate);
    const attributes::TopologyProvenance provenance{
        {{attributes::AttributeDomain::corner, {{0U, 1U}, {1U}}},
         {attributes::AttributeDomain::face, {{0U}, {1U}}}}};
    const auto transferred = attributes::transfer(
        source, provenance, policy);
    REQUIRE(transferred);
    REQUIRE(transferred.value().domain_count(attributes::AttributeDomain::corner) == 2U);
    const auto* uv = transferred.value().find("uv0");
    REQUIRE(uv != nullptr);
    REQUIRE(std::get<core::Vec2d>(uv->values[0]).x == 0.5);
    REQUIRE(std::get<core::Vec2d>(uv->values[0]).y == 0.5);
    const auto* material = transferred.value().find("material_slot");
    REQUIRE(material != nullptr);
    REQUIRE(std::get<std::uint32_t>(material->values[0]) == 0U);

    const attributes::TopologyProvenance created_element_provenance{
        {{attributes::AttributeDomain::corner, {{0U}, {}, {1U}}},
         {attributes::AttributeDomain::face, {{0U}, {1U}}}}};
    const auto invalidated_created = attributes::transfer(
        source, created_element_provenance, attributes::AttributeTransferPolicy{});
    REQUIRE(invalidated_created);
    REQUIRE(invalidated_created.value().domain_count(attributes::AttributeDomain::corner) == 3U);
    const auto* created_uv = invalidated_created.value().find("uv0");
    REQUIRE(created_uv != nullptr);
    REQUIRE(std::get<core::Vec2d>(created_uv->values[1]).x == 0.0);
    REQUIRE(std::get<core::Vec2d>(created_uv->values[1]).y == 0.0);

    attributes::AttributeTransferPolicy unknown_mode = policy;
    unknown_mode.default_mode = static_cast<attributes::TransferMode>(99);
    const auto rejected_mode = attributes::transfer(source, provenance, unknown_mode);
    REQUIRE(!rejected_mode);
    REQUIRE(rejected_mode.error().code == core::ErrorCode::invalid_argument);

    attributes::AttributeTransferPolicy unknown_layer = policy;
    unknown_layer.per_layer.emplace("missing", attributes::TransferMode::preserve);
    const auto rejected_layer = attributes::transfer(source, provenance, unknown_layer);
    REQUIRE(!rejected_layer);
    REQUIRE(rejected_layer.error().code == core::ErrorCode::not_found);

    attributes::AttributeSet wrong_source({{attributes::AttributeDomain::vertex, 1U}});
    const auto wrong_value = wrong_source.add_layer(
        attributes::AttributeLayer{
            {"bad", attributes::AttributeDomain::vertex, attributes::AttributeType::float32},
            {std::uint32_t{1U}},
        });
    REQUIRE(!wrong_value);
}

void stable_ids_derive_created_and_deleted_provenance() {
    using namespace carto;
    const attributes::StableTopologySnapshot source{
        {{attributes::AttributeDomain::vertex, {10U, 20U}},
         {attributes::AttributeDomain::face, {100U}}}};
    const attributes::StableTopologySnapshot destination{
        {{attributes::AttributeDomain::vertex, {20U, 30U}},
         {attributes::AttributeDomain::face, {}}}};
    const auto provenance = attributes::derive_provenance(source, destination);
    REQUIRE(provenance);
    REQUIRE(provenance.value().source_indices.at(attributes::AttributeDomain::vertex).size() == 2U);
    REQUIRE(provenance.value().source_indices.at(attributes::AttributeDomain::vertex)[0] ==
            std::vector<std::size_t>{1U});
    REQUIRE(provenance.value().source_indices.at(attributes::AttributeDomain::vertex)[1].empty());
    REQUIRE(provenance.value().source_indices.at(attributes::AttributeDomain::face).empty());

    const attributes::StableTopologySnapshot duplicate_ids{
        {{attributes::AttributeDomain::vertex, {10U, 10U}}}};
    const auto rejected = attributes::derive_provenance(duplicate_ids, destination);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::validation_failed);
}

void mesh_kernel_provenance_uses_stable_ids_and_preserves_created_boundaries() {
    using namespace carto;
    geometry::EditableMesh source;
    const auto v0 = source.add_vertex({0.0, 0.0, 0.0});
    const auto v1 = source.add_vertex({1.0, 0.0, 0.0});
    const auto v2 = source.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(v0 && v1 && v2);
    REQUIRE(source.add_face({v0.value(), v1.value(), v2.value()}));

    geometry::EditableMesh destination = source;
    const auto created = destination.add_vertex({0.0, 0.0, 1.0});
    REQUIRE(created);
    const auto snapshot = mesh_attributes::stable_topology_snapshot(destination);
    REQUIRE(snapshot);
    REQUIRE(snapshot.value().element_ids.at(attributes::AttributeDomain::vertex).back() ==
            created.value().value);

    const auto provenance = mesh_attributes::derive_provenance(source, destination);
    REQUIRE(provenance);
    const auto& vertex_mapping = provenance.value().source_indices.at(
        attributes::AttributeDomain::vertex);
    REQUIRE(vertex_mapping.size() == 4U);
    REQUIRE(vertex_mapping.back().empty());
    REQUIRE(provenance.value().source_indices.at(attributes::AttributeDomain::face).size() == 1U);
}

void mesh_receipt_provenance_maps_created_elements_and_rejects_stale_states() {
    using namespace carto;
    auto source_result = geometry::make_plane(2.0, 2.0);
    REQUIRE(source_result);
    geometry::EditableMesh source = source_result.value();
    geometry::EditableMesh destination = source;
    const auto source_face = source.faces_sorted().front().id;
    const auto receipt = destination.extrude_face_with_receipt(source_face, 0.25);
    REQUIRE(receipt);

    const auto provenance = mesh_attributes::provenance_from_receipt(
        source, destination, receipt.value());
    REQUIRE(provenance);
    const auto destination_snapshot = mesh_attributes::stable_topology_snapshot(destination);
    REQUIRE(destination_snapshot);
    const auto source_topology = source.topology();
    REQUIRE(source_topology);
    const auto destination_index = [&destination_snapshot](
        attributes::AttributeDomain domain, std::uint64_t id) {
        const auto& ids = destination_snapshot.value().element_ids.at(domain);
        return static_cast<std::size_t>(std::distance(ids.begin(), std::find(ids.begin(), ids.end(), id)));
    };
    for (const auto& [id, ignored] : receipt.value().vertex_origins) {
        static_cast<void>(ignored);
        const auto index = destination_index(attributes::AttributeDomain::vertex, id.value);
        REQUIRE(index < provenance.value().source_indices.at(attributes::AttributeDomain::vertex).size());
        REQUIRE(!provenance.value().source_indices.at(attributes::AttributeDomain::vertex)[index].empty());
    }
    for (const auto& [id, ignored] : receipt.value().face_origins) {
        static_cast<void>(ignored);
        const auto index = destination_index(attributes::AttributeDomain::face, id.value);
        REQUIRE(!provenance.value().source_indices.at(attributes::AttributeDomain::face)[index].empty());
    }

    attributes::AttributeSet source_attributes({
        {attributes::AttributeDomain::vertex, source.vertex_count()},
        {attributes::AttributeDomain::edge, source_topology.value().edges.size()},
        {attributes::AttributeDomain::face, source.face_count()},
        {attributes::AttributeDomain::corner, source_topology.value().corners.size()},
    });
    REQUIRE(source_attributes.add_layer(attributes::AttributeLayer{
        {"vertex_position", attributes::AttributeDomain::vertex, attributes::AttributeType::vec3},
        {core::Vec3d{0.0, 0.0, 0.0}, core::Vec3d{1.0, 0.0, 0.0},
         core::Vec3d{1.0, 1.0, 0.0}, core::Vec3d{0.0, 1.0, 0.0}},
    }));
    REQUIRE(source_attributes.add_layer(attributes::AttributeLayer{
        {"edge_weight", attributes::AttributeDomain::edge, attributes::AttributeType::float64},
        {1.0, 2.0, 3.0, 4.0},
    }));
    REQUIRE(source_attributes.add_layer(attributes::AttributeLayer{
        {"face_material", attributes::AttributeDomain::face, attributes::AttributeType::uint32},
        {std::uint32_t{7U}},
    }));
    REQUIRE(source_attributes.add_layer(attributes::AttributeLayer{
        {"corner_uv", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
         core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}},
    }));
    REQUIRE(source_attributes.validate());
    attributes::AttributeTransferPolicy policy;
    policy.per_layer.emplace("vertex_position", attributes::TransferMode::interpolate);
    policy.per_layer.emplace("edge_weight", attributes::TransferMode::duplicate);
    policy.per_layer.emplace("face_material", attributes::TransferMode::duplicate);
    policy.per_layer.emplace("corner_uv", attributes::TransferMode::interpolate);
    const auto transferred = attributes::transfer(
        source_attributes, provenance.value(), policy);
    REQUIRE(transferred);
    REQUIRE(transferred.value().domain_count(attributes::AttributeDomain::vertex) ==
            destination.vertex_count());
    REQUIRE(transferred.value().domain_count(attributes::AttributeDomain::face) ==
            destination.face_count());

    geometry::EditableMesh stale_destination = destination;
    const auto first_destination_vertex = stale_destination.vertices_sorted().front();
    REQUIRE(stale_destination.set_vertex_position(
        first_destination_vertex.id, first_destination_vertex.position + core::Vec3d{0.01, 0.0, 0.0}));
    const auto stale = mesh_attributes::provenance_from_receipt(
        source, stale_destination, receipt.value());
    REQUIRE(!stale);
    REQUIRE(stale.error().code == core::ErrorCode::stale_data);

    auto forged = receipt.value();
    REQUIRE(!forged.vertex_origins.empty());
    forged.vertex_origins.begin()->second.source_ids.front() = 999999U;
    const auto rejected_forgery = mesh_attributes::provenance_from_receipt(
        source, destination, forged);
    REQUIRE(!rejected_forgery);
    REQUIRE(rejected_forgery.error().code == core::ErrorCode::validation_failed);
}

} // namespace

int main() {
    try {
        graph_rejects_cycles_and_preserves_deterministic_order();
        cache_and_publication_gate_reject_stale_worker_results();
        scheduler_is_bounded_and_only_returns_candidates();
        scheduler_shutdown_rejects_new_work_and_preserves_failure_receipts();
        corner_attributes_interpolate_and_discrete_layers_invalidate();
        stable_ids_derive_created_and_deleted_provenance();
        mesh_kernel_provenance_uses_stable_ids_and_preserves_created_boundaries();
        mesh_receipt_provenance_maps_created_elements_and_rejects_stale_states();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
