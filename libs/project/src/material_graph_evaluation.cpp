#include <carto/project/material_graph.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <span>
#include <utility>
#include <vector>

namespace carto::project {
namespace {

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}

core::Diagnostic validation(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

core::Diagnostic missing_input(std::string message) {
    return {core::ErrorCode::not_found, std::move(message)};
}

core::Diagnostic unsupported(std::string message) {
    return {core::ErrorCode::unsupported, std::move(message)};
}

bool safe_parameter_name(std::string_view value) {
    return !value.empty() && value.size() <= MaterialGraph::kMaxParameterBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

bool safe_graph_identity(std::string_view value) {
    if (value.empty() || value.size() > MaterialGraph::kMaxIdentityBytes ||
        !std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte <= 0x7eU;
        }) || value.front() == '/' || value.front() == '\\' ||
        value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos || value.find("://") != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0U;
    while (begin <= value.size()) {
        const auto slash = value.find('/', begin);
        const auto end = slash == std::string_view::npos ? value.size() : slash;
        const auto segment = value.substr(begin, end - begin);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (slash == std::string_view::npos) break;
        begin = slash + 1U;
    }
    return true;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::array<std::uint64_t, 15U> output_ids(const MaterialGraphOutputs& outputs) {
    return {outputs.base_color, outputs.alpha, outputs.roughness, outputs.metallic,
        outputs.normal, outputs.occlusion, outputs.emissive, outputs.transmission,
        outputs.subsurface, outputs.sheen, outputs.anisotropy, outputs.clearcoat,
        outputs.clearcoat_roughness, outputs.opacity, outputs.displacement};
}

core::Result<std::set<std::uint64_t>> reachable_nodes(const MaterialGraph& graph) {
    std::set<std::uint64_t> reachable;
    std::vector<std::uint64_t> pending;
    for (const auto id : output_ids(graph.outputs)) {
        if (id != 0U) pending.push_back(id);
    }
    while (!pending.empty()) {
        const std::uint64_t id = pending.back();
        pending.pop_back();
        if (!reachable.insert(id).second) continue;
        const auto* node = graph.find(id);
        if (node == nullptr) {
            return core::Result<std::set<std::uint64_t>>::failure(validation(
                "material graph output closure references a missing node"));
        }
        pending.insert(pending.end(), node->inputs.begin(), node->inputs.end());
        if (reachable.size() > MaterialGraph::kMaxNodes) {
            return core::Result<std::set<std::uint64_t>>::failure(validation(
                "material graph output closure exceeds its node bound"));
        }
    }
    return core::Result<std::set<std::uint64_t>>::success(std::move(reachable));
}

std::string evaluation_payload(const MaterialGraphEvaluation& evaluation) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "CARTOGRAPHER_MATERIAL_GRAPH_EVALUATION 1\n"
        << "IDENTITY " << std::quoted(evaluation.graph_identity) << '\n'
        << "REVISION " << evaluation.graph_revision.value() << '\n'
        << "GRAPH_DIGEST " << evaluation.graph_digest.hex() << '\n'
        << "INPUTS_DIGEST " << evaluation.inputs_digest.hex() << '\n'
        << "VALUES " << evaluation.node_values.size() << '\n';
    for (const auto& [id, value] : evaluation.node_values) {
        output << "VALUE " << id << ' ' << static_cast<unsigned>(value.type);
        for (const double component : value.components) output << ' ' << component;
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<std::map<std::uint64_t, MaterialGraphValue>> evaluate_output_values(
    const MaterialGraph& graph,
    const MaterialGraphEvaluationInputs& inputs) {
    auto reachable = reachable_nodes(graph);
    auto order = graph.topological_order();
    if (!reachable || !order) {
        return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(validation(
            "material graph evaluation dependency order could not be derived"));
    }

    std::map<std::uint64_t, MaterialGraphValue> values;
    std::set<std::string> consumed_parameters;
    std::set<MaterialSurfaceInput> consumed_surface_inputs;
    const auto read_value = [&values](std::uint64_t id)
        -> core::Result<MaterialGraphValue> {
        const auto found = values.find(id);
        if (found == values.end()) {
            return core::Result<MaterialGraphValue>::failure(validation(
                "material graph evaluator reached a node before its dependencies"));
        }
        return core::Result<MaterialGraphValue>::success(found->second);
    };
    for (const std::uint64_t id : order.value()) {
        if (!reachable.value().contains(id)) continue;
        const auto* node = graph.find(id);
        if (node == nullptr) {
            return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                validation("material graph evaluator lost a reachable node"));
        }

        MaterialGraphValue value;
        value.type = node->value_type;
        switch (node->kind) {
        case MaterialGraphNodeKind::constant_scalar:
        case MaterialGraphNodeKind::constant_color:
            value.components = node->constants;
            break;
        case MaterialGraphNodeKind::parameter_scalar:
        case MaterialGraphNodeKind::parameter_color: {
            consumed_parameters.insert(node->parameter_name);
            const auto found = inputs.parameters.find(node->parameter_name);
            if (found == inputs.parameters.end()) {
                return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                    missing_input("material graph parameter input is missing: " +
                        node->parameter_name));
            }
            value = found->second;
            break;
        }
        case MaterialGraphNodeKind::surface_input: {
            consumed_surface_inputs.insert(node->surface_input);
            const auto found = inputs.surface_inputs.find(node->surface_input);
            if (found == inputs.surface_inputs.end()) {
                return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                    missing_input("material graph surface input is missing"));
            }
            value = found->second;
            break;
        }
        case MaterialGraphNodeKind::add:
        case MaterialGraphNodeKind::multiply:
        case MaterialGraphNodeKind::mix: {
            auto first = read_value(node->inputs[0]);
            auto second = read_value(node->inputs[1]);
            if (!first || !second) {
                return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                    !first ? first.error() : second.error());
            }
            MaterialGraphValue third;
            if (node->kind == MaterialGraphNodeKind::mix) {
                auto weight = read_value(node->inputs[2]);
                if (!weight) {
                    return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                        weight.error());
                }
                third = weight.value();
            }
            for (std::size_t component = 0U; component < value.components.size(); ++component) {
                if (node->kind == MaterialGraphNodeKind::add) {
                    value.components[component] = first.value().components[component] +
                        second.value().components[component];
                } else if (node->kind == MaterialGraphNodeKind::multiply) {
                    value.components[component] = first.value().components[component] *
                        second.value().components[component];
                } else {
                    const double weight = third.components[0];
                    value.components[component] = first.value().components[component] *
                        (1.0 - weight) + second.value().components[component] * weight;
                }
            }
            break;
        }
        case MaterialGraphNodeKind::texture_sample:
            return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                unsupported("material graph CPU evaluation does not sample texture assets"));
        case MaterialGraphNodeKind::unsupported:
            return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                unsupported("material graph output depends on a node without an evaluator"));
        }
        if (value.type != node->value_type) {
            return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(validation(
                "material graph evaluator received a mismatched typed input"));
        }
        if (auto valid = value.validate(); !valid) {
            return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
                valid.error().with_context("material graph evaluated node"));
        }
        values.emplace(id, std::move(value));
    }
    if (consumed_parameters.size() != inputs.parameters.size() ||
        consumed_surface_inputs.size() != inputs.surface_inputs.size()) {
        return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::failure(
            validation("material graph input set contains values unreachable from outputs"));
    }
    return core::Result<std::map<std::uint64_t, MaterialGraphValue>>::success(
        std::move(values));
}

} // namespace

core::Result<void> MaterialGraphValue::validate() const {
    if (static_cast<unsigned>(type) >
            static_cast<unsigned>(MaterialGraphValueType::normal) ||
        !std::all_of(components.begin(), components.end(), [](double component) {
            return std::isfinite(component);
        }) ||
        (type == MaterialGraphValueType::scalar &&
            (components[1] != 0.0 || components[2] != 0.0 || components[3] != 0.0))) {
        return core::Result<void>::failure(validation(
            "material graph value type or components are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> MaterialGraphEvaluationInputs::validate() const {
    if (parameters.size() > MaterialGraph::kMaxNodes ||
        surface_inputs.size() >
            static_cast<std::size_t>(MaterialSurfaceInput::authored_mask)) {
        return core::Result<void>::failure(validation(
            "material graph evaluation input count exceeds its bound"));
    }
    for (const auto& [name, value] : parameters) {
        if (!safe_parameter_name(name)) {
            return core::Result<void>::failure(invalid(
                "material graph evaluation parameter name is invalid"));
        }
        if (auto valid = value.validate(); !valid) {
            return core::Result<void>::failure(
                valid.error().with_context("material graph evaluation parameter"));
        }
    }
    for (const auto& [surface, value] : surface_inputs) {
        if (surface == MaterialSurfaceInput::unknown ||
            static_cast<unsigned>(surface) >
                static_cast<unsigned>(MaterialSurfaceInput::authored_mask)) {
            return core::Result<void>::failure(invalid(
                "material graph surface input identity is invalid"));
        }
        if (auto valid = value.validate(); !valid) {
            return core::Result<void>::failure(
                valid.error().with_context("material graph surface input"));
        }
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> MaterialGraphEvaluationInputs::canonical_digest() const {
    if (auto valid = validate(); !valid) {
        return core::Result<assets::Sha256Digest>::failure(valid.error());
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "CARTOGRAPHER_MATERIAL_GRAPH_INPUTS 1\n"
        << "PARAMETERS " << parameters.size() << '\n';
    for (const auto& [name, value] : parameters) {
        output << "PARAMETER " << std::quoted(name) << ' '
            << static_cast<unsigned>(value.type);
        for (const double component : value.components) output << ' ' << component;
        output << '\n';
    }
    output << "SURFACE_INPUTS " << surface_inputs.size() << '\n';
    for (const auto& [surface, value] : surface_inputs) {
        output << "SURFACE " << static_cast<unsigned>(surface) << ' '
            << static_cast<unsigned>(value.type);
        for (const double component : value.components) output << ' ' << component;
        output << '\n';
    }
    output << "END\n";
    const auto bytes = output.str();
    return core::Result<assets::Sha256Digest>::success(digest_text(bytes));
}

core::Result<void> validate_material_graph_input_subset(
    const MaterialGraph& graph,
    const MaterialGraphEvaluationInputs& inputs) {
    if (auto valid = graph.validate(); !valid) {
        return core::Result<void>::failure(
            valid.error().with_context("material graph input source"));
    }
    if (auto valid = inputs.validate(); !valid) {
        return core::Result<void>::failure(valid.error());
    }
    const auto reachable = reachable_nodes(graph);
    if (!reachable) return core::Result<void>::failure(reachable.error());

    std::map<std::string, MaterialGraphValueType> parameter_types;
    std::map<MaterialSurfaceInput, MaterialGraphValueType> surface_types;
    for (const std::uint64_t id : reachable.value()) {
        const auto* node = graph.find(id);
        if (node == nullptr) {
            return core::Result<void>::failure(validation(
                "material graph input source lost a reachable node"));
        }
        if (node->kind == MaterialGraphNodeKind::parameter_scalar ||
            node->kind == MaterialGraphNodeKind::parameter_color) {
            const auto [found, inserted] = parameter_types.emplace(
                node->parameter_name, node->value_type);
            if (!inserted && found->second != node->value_type) {
                return core::Result<void>::failure(validation(
                    "material graph input key has conflicting reachable types"));
            }
        } else if (node->kind == MaterialGraphNodeKind::surface_input) {
            const auto [found, inserted] = surface_types.emplace(
                node->surface_input, node->value_type);
            if (!inserted && found->second != node->value_type) {
                return core::Result<void>::failure(validation(
                    "material graph surface input has conflicting reachable types"));
            }
        }
    }
    for (const auto& [name, value] : inputs.parameters) {
        const auto expected = parameter_types.find(name);
        if (expected == parameter_types.end() || expected->second != value.type) {
            return core::Result<void>::failure(validation(
                "material graph parameter binding is unreachable or mistyped"));
        }
    }
    for (const auto& [surface, value] : inputs.surface_inputs) {
        const auto expected = surface_types.find(surface);
        if (expected == surface_types.end() || expected->second != value.type) {
            return core::Result<void>::failure(validation(
                "material graph surface binding is unreachable or mistyped"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> MaterialGraphEvaluation::validate(const MaterialGraph& graph) const {
    if (auto valid = graph.validate(); !valid) {
        return core::Result<void>::failure(
            valid.error().with_context("material graph evaluation source"));
    }
    if (graph_identity != graph.identity || graph_revision != graph.revision ||
        graph_digest.is_zero() || inputs_digest.is_zero() ||
        node_values.empty() || node_values.size() > graph.nodes.size()) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "material graph evaluation identity, revisions, digests, or value count are stale"));
    }
    auto actual_graph_digest = graph.canonical_digest();
    if (!actual_graph_digest || actual_graph_digest.value() != graph_digest) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "material graph evaluation source digest is stale"));
    }
    auto reachable = reachable_nodes(graph);
    if (!reachable || reachable.value().size() != node_values.size()) {
        return core::Result<void>::failure(validation(
            "material graph evaluation does not contain the exact output dependency closure"));
    }
    for (const auto& [id, value] : node_values) {
        const auto* node = graph.find(id);
        if (!reachable.value().contains(id) || node == nullptr ||
            value.type != node->value_type) {
            return core::Result<void>::failure(validation(
                "material graph evaluation contains a foreign or mistyped node value"));
        }
        if (auto valid = value.validate(); !valid) {
            return core::Result<void>::failure(
                valid.error().with_context("material graph evaluation node value"));
        }
    }
    for (const std::uint64_t output : output_ids(graph.outputs)) {
        if (output != 0U && !node_values.contains(output)) {
            return core::Result<void>::failure(validation(
                "material graph evaluation is missing an assigned output value"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> MaterialGraphEvaluation::validate(
    const MaterialGraph& graph,
    const MaterialGraphEvaluationInputs& inputs) const {
    if (auto valid = validate(graph); !valid) {
        return core::Result<void>::failure(valid.error());
    }
    auto actual_inputs_digest = inputs.canonical_digest();
    if (!actual_inputs_digest) {
        return core::Result<void>::failure(actual_inputs_digest.error().with_context(
            "material graph evaluation input receipt"));
    }
    if (actual_inputs_digest.value() != inputs_digest) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "material graph evaluation input digest does not match the supplied inputs"));
    }
    auto expected_values = evaluate_output_values(graph, inputs);
    if (!expected_values) {
        return core::Result<void>::failure(expected_values.error().with_context(
            "material graph evaluation receipt replay"));
    }
    if (expected_values.value() != node_values) {
        return core::Result<void>::failure(validation(
            "material graph evaluation values do not match the exact source and inputs"));
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> MaterialGraphEvaluation::canonical_digest() const {
    if (!safe_graph_identity(graph_identity) ||
        graph_revision.exhausted() || graph_digest.is_zero() || inputs_digest.is_zero() ||
        node_values.empty() || node_values.size() > MaterialGraph::kMaxNodes) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "material graph evaluation receipt identity or bounds are invalid"));
    }
    for (const auto& [id, value] : node_values) {
        if (id == 0U) {
            return core::Result<assets::Sha256Digest>::failure(validation(
                "material graph evaluation receipt contains a zero node ID"));
        }
        if (auto valid = value.validate(); !valid) {
            return core::Result<assets::Sha256Digest>::failure(valid.error());
        }
    }
    return core::Result<assets::Sha256Digest>::success(
        digest_text(evaluation_payload(*this)));
}

core::Result<MaterialGraphEvaluation> evaluate_material_graph(
    const MaterialGraph& graph,
    const MaterialGraphEvaluationInputs& inputs) {
    if (auto valid = graph.validate(); !valid) {
        return core::Result<MaterialGraphEvaluation>::failure(
            valid.error().with_context("material graph evaluation source"));
    }
    if (auto valid = inputs.validate(); !valid) {
        return core::Result<MaterialGraphEvaluation>::failure(
            valid.error().with_context("material graph evaluation inputs"));
    }
    auto graph_digest = graph.canonical_digest();
    auto inputs_digest = inputs.canonical_digest();
    auto values = evaluate_output_values(graph, inputs);
    if (!graph_digest || !inputs_digest) {
        return core::Result<MaterialGraphEvaluation>::failure(validation(
            "material graph evaluation evidence could not be derived"));
    }
    if (!values) {
        return core::Result<MaterialGraphEvaluation>::failure(values.error());
    }

    MaterialGraphEvaluation result;
    result.graph_identity = graph.identity;
    result.graph_revision = graph.revision;
    result.graph_digest = graph_digest.value();
    result.inputs_digest = inputs_digest.value();
    result.node_values = std::move(values.value());
    if (auto valid = result.validate(graph); !valid) {
        return core::Result<MaterialGraphEvaluation>::failure(valid.error());
    }
    return core::Result<MaterialGraphEvaluation>::success(std::move(result));
}

} // namespace carto::project
