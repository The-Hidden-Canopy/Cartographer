#include <carto/project/material_graph.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <type_traits>
#include <utility>

namespace carto::project {
namespace {

constexpr double kMaxConstantMagnitude = 1.0e9;
constexpr std::string_view kMagic = "CARTOGRAPHER_MATERIAL_GRAPH";
constexpr std::string_view kUnsupportedCode = "unsupported-node";

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}

core::Diagnostic validation(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

bool safe_ascii(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= 0x20U && c <= 0x7eU;
        });
}

bool safe_identifier(std::string_view value, std::size_t maximum) {
    return safe_ascii(value, maximum) &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
}

bool safe_graph_identity(std::string_view value) {
    if (!safe_ascii(value, MaterialGraph::kMaxIdentityBytes) ||
        value.front() == '/' || value.front() == '\\' ||
        value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos ||
        value.find("://") != std::string_view::npos) return false;
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

bool zero_constants(const std::array<double, 4U>& values) {
    return values == std::array<double, 4U>{};
}

bool default_non_graph_payload(const MaterialGraphNode& node) {
    return node.texture_asset == 0U && node.uv_set == 1U &&
        node.surface_input == MaterialSurfaceInput::unknown &&
        node.parameter_name.empty() && node.unsupported_identity.empty();
}

bool valid_type(MaterialGraphValueType value) {
    return value == MaterialGraphValueType::scalar ||
        value == MaterialGraphValueType::color || value == MaterialGraphValueType::normal;
}

bool valid_kind(MaterialGraphNodeKind value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(MaterialGraphNodeKind::unsupported);
}

bool valid_surface(MaterialSurfaceInput value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(MaterialSurfaceInput::authored_mask);
}

bool finite_constants(const std::array<double, 4U>& values) {
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value) && std::abs(value) <= kMaxConstantMagnitude;
    });
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) return false;
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool expect(std::istream& input, std::string_view token) {
    std::string actual;
    return static_cast<bool>(input >> actual) && actual == token;
}

bool read_enum(std::istream& input, unsigned max_value, unsigned& value) {
    return read_unsigned(input, value) && value <= max_value;
}

std::vector<MaterialGraphNode> canonical_nodes(const MaterialGraph& graph) {
    auto nodes = graph.nodes;
    std::sort(nodes.begin(), nodes.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return nodes;
}

std::string canonical_payload(
    const MaterialGraph& graph,
    std::uint32_t schema_version = MaterialGraph::kSchemaVersion) {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << kMagic << ' ' << schema_version << '\n';
    out << "IDENTITY " << std::quoted(graph.identity) << '\n';
    out << "REVISION " << graph.revision.value() << '\n';
    const auto nodes = canonical_nodes(graph);
    out << "NODES " << nodes.size() << '\n';
    for (const auto& node : nodes) {
        out << "NODE " << node.id << ' ' << static_cast<unsigned>(node.kind) << ' '
            << static_cast<unsigned>(node.value_type) << ' '
            << static_cast<unsigned>(node.surface_input) << ' '
            << node.texture_asset << ' ' << node.uv_set;
        for (const double value : node.constants) out << ' ' << value;
        out << ' ' << std::quoted(node.parameter_name) << ' '
            << std::quoted(node.unsupported_identity) << ' ' << node.inputs.size();
        for (const auto input : node.inputs) out << ' ' << input;
        out << '\n';
    }
    if (schema_version >= 2U) {
        out << "RETIRED_NODES " << graph.retired_node_ids.size() << '\n';
        for (const auto id : graph.retired_node_ids) {
            out << "RETIRED_NODE " << id << '\n';
        }
    }
    const auto& o = graph.outputs;
    out << "OUTPUTS " << o.base_color << ' ' << o.alpha << ' ' << o.roughness << ' '
        << o.metallic << ' ' << o.normal << ' ' << o.occlusion << ' ' << o.emissive << ' '
        << o.transmission << ' ' << o.subsurface << ' ' << o.sheen << ' ' << o.anisotropy << ' '
        << o.clearcoat << ' ' << o.clearcoat_roughness << ' ' << o.opacity << ' '
        << o.displacement << '\n';
    out << "END\n";
    return out.str();
}

core::Result<void> validate_output(
    const MaterialGraph& graph,
    std::uint64_t id,
    MaterialGraphValueType expected,
    bool required,
    std::string_view name) {
    if (id == 0U) {
        return required ? core::Result<void>::failure(validation(
            "material graph required output is unbound: " + std::string(name))) :
            core::Result<void>::success();
    }
    const auto* node = graph.find(id);
    if (node == nullptr || node->value_type != expected) {
        return core::Result<void>::failure(validation(
            "material graph output is missing or has the wrong type: " + std::string(name)));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> MaterialGraphNode::validate() const {
    if (id == 0U || !valid_kind(kind) || !valid_type(value_type) ||
        inputs.size() > MaterialGraph::kMaxInputsPerNode || !finite_constants(constants)) {
        return core::Result<void>::failure(invalid(
            "material graph node identity, kind, type, inputs, or constants are invalid"));
    }
    switch (kind) {
    case MaterialGraphNodeKind::constant_scalar:
        if (value_type != MaterialGraphValueType::scalar || !inputs.empty() ||
            !default_non_graph_payload(*this) || constants[1] != 0.0 ||
            constants[2] != 0.0 || constants[3] != 0.0) break;
        return core::Result<void>::success();
    case MaterialGraphNodeKind::constant_color:
        if (value_type == MaterialGraphValueType::color && inputs.empty() &&
            default_non_graph_payload(*this)) return core::Result<void>::success();
        break;
    case MaterialGraphNodeKind::parameter_scalar:
    case MaterialGraphNodeKind::parameter_color: {
        const bool scalar = kind == MaterialGraphNodeKind::parameter_scalar;
        if (value_type == (scalar ? MaterialGraphValueType::scalar : MaterialGraphValueType::color) &&
            inputs.empty() && texture_asset == 0U && uv_set == 1U &&
            surface_input == MaterialSurfaceInput::unknown &&
            safe_identifier(parameter_name, MaterialGraph::kMaxParameterBytes) &&
            unsupported_identity.empty() && zero_constants(constants)) {
            return core::Result<void>::success();
        }
        break;
    }
    case MaterialGraphNodeKind::texture_sample:
        if (value_type == MaterialGraphValueType::color && texture_asset != 0U && uv_set != 0U &&
            inputs.size() == 2U && surface_input == MaterialSurfaceInput::unknown &&
            parameter_name.empty() && unsupported_identity.empty() && zero_constants(constants)) {
            return core::Result<void>::success();
        }
        break;
    case MaterialGraphNodeKind::surface_input:
        if (valid_surface(surface_input) && surface_input != MaterialSurfaceInput::unknown &&
            inputs.empty() && texture_asset == 0U && uv_set == 1U &&
            parameter_name.empty() && unsupported_identity.empty() && zero_constants(constants) &&
            value_type != MaterialGraphValueType::normal) {
            return core::Result<void>::success();
        }
        break;
    case MaterialGraphNodeKind::add:
    case MaterialGraphNodeKind::multiply:
        if ((value_type == MaterialGraphValueType::scalar ||
             value_type == MaterialGraphValueType::color) && inputs.size() == 2U &&
            default_non_graph_payload(*this) && zero_constants(constants)) {
            return core::Result<void>::success();
        }
        break;
    case MaterialGraphNodeKind::mix:
        if ((value_type == MaterialGraphValueType::scalar ||
             value_type == MaterialGraphValueType::color) && inputs.size() == 3U &&
            default_non_graph_payload(*this) && zero_constants(constants)) {
            return core::Result<void>::success();
        }
        break;
    case MaterialGraphNodeKind::unsupported:
        if ((value_type == MaterialGraphValueType::scalar ||
             value_type == MaterialGraphValueType::color) && inputs.empty() &&
            texture_asset == 0U && uv_set == 1U &&
            surface_input == MaterialSurfaceInput::unknown && parameter_name.empty() &&
            safe_identifier(unsupported_identity, MaterialGraph::kMaxParameterBytes) &&
            zero_constants(constants)) {
            return core::Result<void>::success();
        }
        break;
    }
    return core::Result<void>::failure(validation("material graph node payload is invalid"));
}

const MaterialGraphNode* MaterialGraph::find(std::uint64_t id) const noexcept {
    for (const auto& node : nodes) if (node.id == id) return &node;
    return nullptr;
}

core::Result<std::vector<std::uint64_t>> MaterialGraph::topological_order() const {
    std::map<std::uint64_t, std::size_t> indegree;
    std::map<std::uint64_t, std::vector<std::uint64_t>> outgoing;
    for (const auto& node : nodes) {
        indegree.emplace(node.id, 0U);
        outgoing.emplace(node.id, std::vector<std::uint64_t>{});
    }
    for (const auto& node : nodes) {
        for (const auto input : node.inputs) {
            if (!indegree.contains(input)) {
                return core::Result<std::vector<std::uint64_t>>::failure(
                    validation("material graph input references a missing node"));
            }
            ++indegree.at(node.id);
            outgoing.at(input).push_back(node.id);
        }
    }
    std::set<std::uint64_t> ready;
    for (const auto& [id, degree] : indegree) if (degree == 0U) ready.insert(id);
    std::vector<std::uint64_t> order;
    order.reserve(nodes.size());
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(id);
        for (const auto next : outgoing.at(id)) {
            auto& degree = indegree.at(next);
            if (--degree == 0U) ready.insert(next);
        }
    }
    if (order.size() != nodes.size()) {
        return core::Result<std::vector<std::uint64_t>>::failure(
            {core::ErrorCode::cycle_detected, "material graph contains a cycle"});
    }
    return core::Result<std::vector<std::uint64_t>>::success(std::move(order));
}

core::Result<void> MaterialGraph::validate() const {
    if (!safe_graph_identity(identity) || revision.exhausted()) {
        return core::Result<void>::failure(invalid("material graph identity or revision is invalid"));
    }
    if (nodes.empty() || nodes.size() > kMaxNodes ||
        retired_node_ids.size() > kMaxRetiredNodeIds) {
        return core::Result<void>::failure(validation("material graph node count is outside its bound"));
    }
    std::set<std::uint64_t> ids;
    for (const auto& node : nodes) {
        if (auto result = node.validate(); !result) return result;
        if (!ids.insert(node.id).second) {
            return core::Result<void>::failure(validation("material graph node identity is duplicated"));
        }
    }
    if (!std::is_sorted(retired_node_ids.begin(), retired_node_ids.end()) ||
        std::adjacent_find(retired_node_ids.begin(), retired_node_ids.end()) !=
            retired_node_ids.end() ||
        std::any_of(retired_node_ids.begin(), retired_node_ids.end(),
            [&ids](std::uint64_t id) { return id == 0U || ids.contains(id); })) {
        return core::Result<void>::failure(validation(
            "retired material graph node IDs must be sorted, unique, non-zero, and inactive"));
    }
    for (const auto& node : nodes) {
        for (const auto input : node.inputs) {
            if (!ids.contains(input)) {
                return core::Result<void>::failure(validation("material graph input is unresolved"));
            }
        }
        const auto type_of = [this](std::uint64_t id) { return find(id)->value_type; };
        if (node.kind == MaterialGraphNodeKind::add || node.kind == MaterialGraphNodeKind::multiply) {
            if (type_of(node.inputs[0]) != node.value_type || type_of(node.inputs[1]) != node.value_type) {
                return core::Result<void>::failure(validation("binary material graph input types differ"));
            }
        } else if (node.kind == MaterialGraphNodeKind::mix) {
            if (type_of(node.inputs[0]) != node.value_type ||
                type_of(node.inputs[1]) != node.value_type ||
                type_of(node.inputs[2]) != MaterialGraphValueType::scalar) {
                return core::Result<void>::failure(validation("material graph mix input types differ"));
            }
        } else if (node.kind == MaterialGraphNodeKind::texture_sample) {
            if (type_of(node.inputs[0]) != MaterialGraphValueType::scalar ||
                type_of(node.inputs[1]) != MaterialGraphValueType::scalar) {
                return core::Result<void>::failure(validation("texture coordinates must be scalar"));
            }
        }
    }
    const auto order = topological_order();
    if (!order) return core::Result<void>::failure(order.error());
    const auto& o = outputs;
    if (auto r = validate_output(*this, o.base_color, MaterialGraphValueType::color, true, "base color"); !r) return r;
    if (auto r = validate_output(*this, o.alpha, MaterialGraphValueType::scalar, true, "alpha"); !r) return r;
    if (auto r = validate_output(*this, o.roughness, MaterialGraphValueType::scalar, true, "roughness"); !r) return r;
    if (auto r = validate_output(*this, o.metallic, MaterialGraphValueType::scalar, true, "metallic"); !r) return r;
    const auto optional = [this](std::uint64_t id, MaterialGraphValueType type, std::string_view name) {
        return validate_output(*this, id, type, false, name);
    };
    if (auto r = optional(o.normal, MaterialGraphValueType::normal, "normal"); !r) return r;
    if (auto r = optional(o.occlusion, MaterialGraphValueType::scalar, "occlusion"); !r) return r;
    if (auto r = optional(o.emissive, MaterialGraphValueType::color, "emissive"); !r) return r;
    for (const auto id : {o.transmission, o.subsurface, o.sheen, o.anisotropy,
            o.clearcoat, o.clearcoat_roughness, o.opacity, o.displacement}) {
        if (auto r = optional(id, MaterialGraphValueType::scalar, "scalar surface channel"); !r) return r;
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> MaterialGraph::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical_payload(*this)));
}

core::Result<std::string> MaterialGraph::serialize() const {
    const auto digest = canonical_digest();
    if (!digest) return core::Result<std::string>::failure(digest.error());
    std::string result = canonical_payload(*this);
    result += "DIGEST " + digest.value().hex() + "\n";
    if (result.size() > kMaxSerializedBytes) {
        return core::Result<std::string>::failure(validation("material graph serialization exceeds its limit"));
    }
    return core::Result<std::string>::success(std::move(result));
}

core::Result<MaterialGraph> MaterialGraph::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<MaterialGraph>::failure(invalid("material graph input is empty or oversized"));
    }
    std::istringstream input{std::string{text}};
    if (!expect(input, kMagic)) return core::Result<MaterialGraph>::failure(validation("material graph magic is invalid"));
    unsigned version = 0U;
    if (!read_unsigned(input, version) || (version != 1U && version != kSchemaVersion)) {
        return core::Result<MaterialGraph>::failure({core::ErrorCode::version_mismatch, "material graph version is unsupported"});
    }
    MaterialGraph graph;
    if (!expect(input, "IDENTITY") || !(input >> std::quoted(graph.identity))) {
        return core::Result<MaterialGraph>::failure(validation("material graph identity is missing"));
    }
    std::uint64_t revision = 0U;
    if (!expect(input, "REVISION") || !read_unsigned(input, revision)) {
        return core::Result<MaterialGraph>::failure(validation("material graph revision is invalid"));
    }
    graph.revision = core::Revision{revision};
    std::size_t count = 0U;
    if (!expect(input, "NODES") || !read_count(input, kMaxNodes, count)) {
        return core::Result<MaterialGraph>::failure(validation("material graph node count is invalid"));
    }
    graph.nodes.reserve(count);
    for (std::size_t i = 0U; i < count; ++i) {
        MaterialGraphNode node;
        unsigned kind = 0U, type = 0U, surface = 0U;
        if (!expect(input, "NODE") || !read_unsigned(input, node.id) ||
            !read_enum(input, static_cast<unsigned>(MaterialGraphNodeKind::unsupported), kind) ||
            !read_enum(input, static_cast<unsigned>(MaterialGraphValueType::normal), type) ||
            !read_enum(input, static_cast<unsigned>(MaterialSurfaceInput::authored_mask), surface) ||
            !read_unsigned(input, node.texture_asset) || !read_unsigned(input, node.uv_set)) {
            return core::Result<MaterialGraph>::failure(validation("material graph node header is malformed"));
        }
        node.kind = static_cast<MaterialGraphNodeKind>(kind);
        node.value_type = static_cast<MaterialGraphValueType>(type);
        node.surface_input = static_cast<MaterialSurfaceInput>(surface);
        for (double& value : node.constants) if (!(input >> value)) {
            return core::Result<MaterialGraph>::failure(validation("material graph constant is malformed"));
        }
        std::size_t input_count = 0U;
        if (!(input >> std::quoted(node.parameter_name) >> std::quoted(node.unsupported_identity)) ||
            !read_count(input, kMaxInputsPerNode, input_count)) {
            return core::Result<MaterialGraph>::failure(validation("material graph node payload is malformed"));
        }
        node.inputs.resize(input_count);
        for (auto& node_input : node.inputs) if (!read_unsigned(input, node_input)) {
            return core::Result<MaterialGraph>::failure(validation("material graph edge is malformed"));
        }
        graph.nodes.push_back(std::move(node));
    }
    if (version >= 2U) {
        std::size_t retired_count = 0U;
        if (!expect(input, "RETIRED_NODES") ||
            !read_count(input, kMaxRetiredNodeIds, retired_count)) {
            return core::Result<MaterialGraph>::failure(validation(
                "material graph retired-node count is malformed"));
        }
        graph.retired_node_ids.reserve(retired_count);
        for (std::size_t index = 0U; index < retired_count; ++index) {
            std::uint64_t id = 0U;
            if (!expect(input, "RETIRED_NODE") || !read_unsigned(input, id)) {
                return core::Result<MaterialGraph>::failure(validation(
                    "material graph retired-node identity is malformed"));
            }
            graph.retired_node_ids.push_back(id);
        }
    }
    auto& o = graph.outputs;
    if (!expect(input, "OUTPUTS")) return core::Result<MaterialGraph>::failure(validation("material graph outputs are missing"));
    for (auto* value : {&o.base_color, &o.alpha, &o.roughness, &o.metallic, &o.normal,
            &o.occlusion, &o.emissive, &o.transmission, &o.subsurface, &o.sheen,
            &o.anisotropy, &o.clearcoat, &o.clearcoat_roughness, &o.opacity, &o.displacement}) {
        if (!read_unsigned(input, *value)) return core::Result<MaterialGraph>::failure(validation("material graph output is malformed"));
    }
    if (!expect(input, "END") || !expect(input, "DIGEST")) {
        return core::Result<MaterialGraph>::failure(validation("material graph terminator is malformed"));
    }
    std::string encoded_digest, trailing;
    if (!(input >> encoded_digest) || (input >> trailing)) {
        return core::Result<MaterialGraph>::failure(validation("material graph digest framing is invalid"));
    }
    const auto expected = assets::Sha256Digest::from_hex(encoded_digest);
    if (!expected || expected.value().is_zero()) return core::Result<MaterialGraph>::failure(validation("material graph digest is invalid"));
    if (auto result = graph.validate(); !result) return core::Result<MaterialGraph>::failure(result.error());
    const auto actual = digest_text(canonical_payload(graph, version));
    if (actual != expected.value()) {
        return core::Result<MaterialGraph>::failure(validation("material graph content digest does not match"));
    }
    return core::Result<MaterialGraph>::success(std::move(graph));
}

core::Result<void> MaterialGraphCompileReport::validate() const {
    if (source_digest.is_zero() || topological_order.empty()) {
        return core::Result<void>::failure(validation("material graph compile report lacks source evidence"));
    }
    const std::set<std::uint64_t> order(topological_order.begin(), topological_order.end());
    if (order.size() != topological_order.size() || order.contains(0U)) {
        return core::Result<void>::failure(validation("material graph compile order is invalid"));
    }
    std::uint64_t previous = 0U;
    for (const auto& diagnostic : diagnostics) {
        if (!safe_identifier(diagnostic.code, 128U) || !safe_ascii(diagnostic.message, 512U) ||
            diagnostic.node_id == 0U || !order.contains(diagnostic.node_id) ||
            (previous != 0U && diagnostic.node_id < previous)) {
            return core::Result<void>::failure(validation("material graph diagnostic is unsafe or non-canonical"));
        }
        previous = diagnostic.node_id;
    }
    if (supported != diagnostics.empty()) {
        return core::Result<void>::failure(validation("material graph support flag contradicts diagnostics"));
    }
    return core::Result<void>::success();
}

core::Result<MaterialGraphCompileReport> compile_material_graph(const MaterialGraph& graph) {
    if (auto result = graph.validate(); !result) return core::Result<MaterialGraphCompileReport>::failure(result.error());
    const auto digest = graph.canonical_digest();
    const auto order = graph.topological_order();
    if (!digest || !order) return core::Result<MaterialGraphCompileReport>::failure(validation("material graph evidence could not be derived"));
    MaterialGraphCompileReport report;
    report.source_digest = digest.value();
    report.topological_order = order.value();
    auto nodes = canonical_nodes(graph);
    for (const auto& node : nodes) if (node.kind == MaterialGraphNodeKind::unsupported) {
        report.diagnostics.push_back({std::string{kUnsupportedCode},
            "node is retained as authoring data but has no supported lowering", node.id});
    }
    report.supported = report.diagnostics.empty();
    if (auto result = report.validate(); !result) return core::Result<MaterialGraphCompileReport>::failure(result.error());
    return core::Result<MaterialGraphCompileReport>::success(std::move(report));
}

core::Result<void> validate_material_graph_source(
    const MaterialGraph& graph, std::string_view current_identity,
    core::Revision current_revision, const assets::Sha256Digest& current_digest) {
    if (auto result = graph.validate(); !result) return result;
    if (graph.identity != current_identity || graph.revision != current_revision) {
        return core::Result<void>::failure({core::ErrorCode::stale_data, "material graph identity or revision is stale"});
    }
    const auto actual = graph.canonical_digest();
    if (!actual || actual.value() != current_digest) {
        return core::Result<void>::failure({core::ErrorCode::stale_data, "material graph digest is stale"});
    }
    return core::Result<void>::success();
}

} // namespace carto::project
