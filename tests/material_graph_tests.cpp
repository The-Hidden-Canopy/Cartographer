#include <carto/project/material_graph.hpp>
#include "../apps/desktop/studio_material_graph_draft.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <utility>

namespace {

using carto::project::MaterialGraph;
using carto::project::MaterialGraphNode;
using carto::project::MaterialGraphNodeKind;
using carto::project::MaterialGraphEvaluationInputs;
using carto::project::MaterialGraphValue;
using carto::project::MaterialGraphValueType;
using carto::project::MaterialSurfaceInput;

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

void cpu_evaluation_is_typed_deterministic_and_fail_closed() {
    using namespace carto;
    MaterialGraph graph;
    graph.identity = "materials/test/evaluation";
    graph.revision = core::Revision{3U};

    MaterialGraphNode albedo;
    albedo.id = 1U;
    albedo.kind = MaterialGraphNodeKind::parameter_color;
    albedo.value_type = MaterialGraphValueType::color;
    albedo.parameter_name = "albedo";

    MaterialGraphNode swatch;
    swatch.id = 2U;
    swatch.kind = MaterialGraphNodeKind::constant_color;
    swatch.value_type = MaterialGraphValueType::color;
    swatch.constants = {0.2, 0.4, 0.6, 1.0};

    const auto scalar = [](std::uint64_t id, double value) {
        MaterialGraphNode node;
        node.id = id;
        node.kind = MaterialGraphNodeKind::constant_scalar;
        node.value_type = MaterialGraphValueType::scalar;
        node.constants[0] = value;
        return node;
    };
    auto mix_weight = scalar(3U, 0.5);

    MaterialGraphNode mix;
    mix.id = 4U;
    mix.kind = MaterialGraphNodeKind::mix;
    mix.value_type = MaterialGraphValueType::color;
    mix.inputs = {1U, 2U, 3U};

    MaterialGraphNode mask;
    mask.id = 5U;
    mask.kind = MaterialGraphNodeKind::surface_input;
    mask.value_type = MaterialGraphValueType::scalar;
    mask.surface_input = MaterialSurfaceInput::authored_mask;

    auto alpha_offset = scalar(6U, 0.1);
    MaterialGraphNode alpha;
    alpha.id = 7U;
    alpha.kind = MaterialGraphNodeKind::add;
    alpha.value_type = MaterialGraphValueType::scalar;
    alpha.inputs = {5U, 6U};

    MaterialGraphNode roughness;
    roughness.id = 8U;
    roughness.kind = MaterialGraphNodeKind::parameter_scalar;
    roughness.value_type = MaterialGraphValueType::scalar;
    roughness.parameter_name = "roughness";
    auto roughness_scale = scalar(9U, 0.5);
    MaterialGraphNode scaled_roughness;
    scaled_roughness.id = 10U;
    scaled_roughness.kind = MaterialGraphNodeKind::multiply;
    scaled_roughness.value_type = MaterialGraphValueType::scalar;
    scaled_roughness.inputs = {8U, 9U};

    auto metallic = scalar(11U, 0.15);
    MaterialGraphNode dead_unsupported;
    dead_unsupported.id = 12U;
    dead_unsupported.kind = MaterialGraphNodeKind::unsupported;
    dead_unsupported.value_type = MaterialGraphValueType::scalar;
    dead_unsupported.unsupported_identity = "unused-noise";

    graph.nodes = {albedo, swatch, mix_weight, mix, mask, alpha_offset, alpha,
        roughness, roughness_scale, scaled_roughness, metallic, dead_unsupported};
    graph.outputs.base_color = 4U;
    graph.outputs.alpha = 7U;
    graph.outputs.roughness = 10U;
    graph.outputs.metallic = 11U;

    MaterialGraphEvaluationInputs inputs;
    MaterialGraphValue albedo_value;
    albedo_value.type = MaterialGraphValueType::color;
    albedo_value.components = {0.8, 0.2, 0.4, 1.0};
    inputs.parameters.emplace("albedo", albedo_value);
    MaterialGraphValue roughness_value;
    roughness_value.type = MaterialGraphValueType::scalar;
    roughness_value.components[0] = 0.6;
    inputs.parameters.emplace("roughness", roughness_value);
    MaterialGraphValue mask_value;
    mask_value.type = MaterialGraphValueType::scalar;
    mask_value.components[0] = 0.3;
    inputs.surface_inputs.emplace(MaterialSurfaceInput::authored_mask, mask_value);

    const auto evaluation = evaluate_material_graph(graph, inputs);
    require(static_cast<bool>(evaluation),
        "reachable scalar/color graph nodes evaluate with explicit inputs");
    const auto close = [](double left, double right) {
        return std::abs(left - right) < 1.0e-12;
    };
    const auto& base = evaluation.value().node_values.at(graph.outputs.base_color);
    require(close(base.components[0], 0.5) && close(base.components[1], 0.3) &&
            close(base.components[2], 0.5) && close(base.components[3], 1.0),
        "color mix evaluates component-wise without clamping");
    require(close(evaluation.value().node_values.at(graph.outputs.alpha).components[0], 0.4),
        "surface input and scalar addition produce the assigned alpha output");
    require(close(evaluation.value().node_values.at(graph.outputs.roughness).components[0], 0.3),
        "parameter and scalar multiplication produce the assigned roughness output");
    require(close(evaluation.value().node_values.at(graph.outputs.metallic).components[0], 0.15),
        "constant node produces the assigned metallic output");
    require(!evaluation.value().node_values.contains(dead_unsupported.id),
        "unreachable unsupported node is retained but excluded from output evaluation");
    require(evaluation.value().validate(graph) &&
            evaluation.value().validate(graph, inputs) &&
            evaluation.value().canonical_digest(),
        "evaluation receipt validates against the exact graph and supplied inputs");

    MaterialGraphEvaluationInputs reordered_inputs;
    reordered_inputs.surface_inputs.emplace(MaterialSurfaceInput::authored_mask, mask_value);
    reordered_inputs.parameters.emplace("roughness", roughness_value);
    reordered_inputs.parameters.emplace("albedo", albedo_value);
    const auto input_digest = inputs.canonical_digest();
    const auto reordered_digest = reordered_inputs.canonical_digest();
    require(input_digest && reordered_digest && input_digest.value() == reordered_digest.value(),
        "input digest is independent of caller insertion order");

    auto changed_inputs = inputs;
    changed_inputs.parameters.at("roughness").components[0] = 0.7;
    const auto changed_evaluation = evaluate_material_graph(graph, changed_inputs);
    require(changed_evaluation && changed_evaluation.value().inputs_digest !=
            evaluation.value().inputs_digest,
        "evaluation receipts bind the exact supplied parameter values");
    require(!evaluation.value().validate(graph, changed_inputs),
        "evaluation receipt refuses a different supplied input set");
    const auto original_receipt_digest = evaluation.value().canonical_digest();
    auto tampered_evaluation = evaluation.value();
    tampered_evaluation.node_values.at(graph.outputs.base_color).components[0] += 0.01;
    const auto changed_receipt_digest = tampered_evaluation.canonical_digest();
    require(original_receipt_digest && changed_receipt_digest &&
            original_receipt_digest.value() != changed_receipt_digest.value(),
        "evaluation receipt digest covers its output values");
    require(!tampered_evaluation.validate(graph, inputs),
        "receipt validation recomputes outputs instead of trusting supplied values");

    auto missing_parameter = inputs;
    missing_parameter.parameters.erase("albedo");
    require(!evaluate_material_graph(graph, missing_parameter),
        "missing graph parameters are refused without a fallback value");
    auto missing_surface = inputs;
    missing_surface.surface_inputs.clear();
    require(!evaluate_material_graph(graph, missing_surface),
        "missing surface context is refused without a fabricated value");
    auto mistyped_input = inputs;
    mistyped_input.parameters.at("albedo").type = MaterialGraphValueType::scalar;
    require(!evaluate_material_graph(graph, mistyped_input),
        "parameter values must match the connected node type");
    auto unreachable_parameter = inputs;
    MaterialGraphValue unused_scalar;
    unused_scalar.type = MaterialGraphValueType::scalar;
    unused_scalar.components[0] = 0.2;
    unreachable_parameter.parameters.emplace("not_reachable", unused_scalar);
    const auto unreachable_parameter_result =
        evaluate_material_graph(graph, unreachable_parameter);
    require(!unreachable_parameter_result &&
            unreachable_parameter_result.error().code == core::ErrorCode::validation_failed,
        "unused parameter values outside the output closure are refused");
    auto unreachable_surface = inputs;
    unreachable_surface.surface_inputs.emplace(
        MaterialSurfaceInput::curvature, mask_value);
    const auto unreachable_surface_result =
        evaluate_material_graph(graph, unreachable_surface);
    require(!unreachable_surface_result &&
            unreachable_surface_result.error().code == core::ErrorCode::validation_failed,
        "unused surface values outside the output closure are refused");

    auto stale_graph = graph;
    stale_graph.revision = core::Revision{4U};
    require(!evaluation.value().validate(stale_graph),
        "evaluation receipt refuses a changed source revision");
    auto changed_same_revision_graph = graph;
    for (auto& node : changed_same_revision_graph.nodes) {
        if (node.id == metallic.id) node.constants[0] = 0.25;
    }
    require(!evaluation.value().validate(changed_same_revision_graph),
        "evaluation receipt refuses changed graph content even at the same revision");
    auto incomplete_receipt = evaluation.value();
    incomplete_receipt.node_values.erase(graph.outputs.metallic);
    require(!incomplete_receipt.validate(graph),
        "evaluation receipt must retain the exact output dependency closure");

    auto texture_graph = graph;
    MaterialGraphNode texture_sample;
    texture_sample.id = 13U;
    texture_sample.kind = MaterialGraphNodeKind::texture_sample;
    texture_sample.value_type = MaterialGraphValueType::color;
    texture_sample.texture_asset = 501U;
    texture_sample.inputs = {3U, 3U};
    texture_graph.nodes.push_back(texture_sample);
    texture_graph.outputs.base_color = texture_sample.id;
    require(!evaluate_material_graph(texture_graph, inputs),
        "reachable texture sampling is explicitly unsupported by the CPU evaluator");

    auto overflow_graph = graph;
    for (auto& node : overflow_graph.nodes) {
        if (node.id == 9U) node.constants[0] = 2.0;
    }
    auto huge_input = inputs;
    huge_input.parameters.at("roughness").components[0] = 1.7e308;
    require(!evaluate_material_graph(overflow_graph, huge_input),
        "non-finite arithmetic intermediates are rejected");
}

} // namespace

int main() {
    {
        using namespace carto;
        using studio::MaterialGraphDraftIdentity;
        using studio::MaterialGraphDraftState;
        const MaterialGraphDraftIdentity original{1U, core::Revision{5U}, "graph.a", core::Revision{2U}};
        const MaterialGraphDraftIdentity other_graph{1U, core::Revision{5U}, "graph.b", core::Revision{2U}};
        const MaterialGraphDraftIdentity other_project{2U, core::Revision{5U}, "graph.a", core::Revision{2U}};
        const MaterialGraphDraftIdentity project_edit{1U, core::Revision{6U}, "graph.a", core::Revision{2U}};
        const MaterialGraphDraftIdentity graph_edit{1U, core::Revision{5U}, "graph.a", core::Revision{3U}};
        require(studio::material_graph_draft_state(true,true,original,original) ==
                    MaterialGraphDraftState::current,
                "matching dirty graph draft remains applicable");
        for (const auto& source : {other_graph, other_project, project_edit, graph_edit}) {
            require(studio::material_graph_draft_state(true,true,original,source) ==
                        MaterialGraphDraftState::stale,
                    "dirty graph draft stays stale on any source identity change");
            require(studio::material_graph_draft_state(true,false,original,source) ==
                        MaterialGraphDraftState::reload,
                    "clean graph draft reloads on any source identity change");
        }
        require(studio::material_graph_draft_state(false,true,original,original) ==
                    MaterialGraphDraftState::stale &&
                studio::material_graph_draft_state(false,false,original,original) ==
                    MaterialGraphDraftState::reload,
                "missing loaded identity never silently discards a dirty graph draft");
        const studio::PendingMaterialGraphSelection pending{
            1U, core::Revision{5U}, "graph.new", core::Revision{1U}};
        using studio::PendingMaterialGraphState;
        require(studio::pending_material_graph_state(pending,1U,core::Revision{5U},std::nullopt) ==
                    PendingMaterialGraphState::waiting,
                "new selection waits while the pre-creation snapshot is shown");
        require(studio::pending_material_graph_state(pending,1U,core::Revision{6U},core::Revision{1U}) ==
                    PendingMaterialGraphState::confirmed,
                "new selection confirms the expected source revision in the same project");
        require(studio::pending_material_graph_state(pending,2U,core::Revision{6U},core::Revision{1U}) ==
                    PendingMaterialGraphState::unavailable &&
                studio::pending_material_graph_state(pending,1U,core::Revision{6U},std::nullopt) ==
                    PendingMaterialGraphState::unavailable &&
                studio::pending_material_graph_state(pending,1U,core::Revision{5U},core::Revision{1U}) ==
                    PendingMaterialGraphState::unavailable &&
                studio::pending_material_graph_state(pending,1U,core::Revision{6U},core::Revision{2U}) ==
                    PendingMaterialGraphState::unavailable,
                "project switches, missing sources and changed source revisions require a new choice");
    }
    deterministic_round_trip_and_compilation_report();
    hostile_graph_boundaries_fail_closed();
    cpu_evaluation_is_typed_deterministic_and_fail_closed();
    std::cout << "material graph tests passed\n";
    return EXIT_SUCCESS;
}
