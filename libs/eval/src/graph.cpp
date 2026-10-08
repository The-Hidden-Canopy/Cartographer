#include <carto/eval/graph.hpp>

#include <carto/assets/blob_store.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <string_view>
#include <utility>

namespace carto::eval {

namespace {

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

core::Result<void> validation(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::validation_failed, std::move(message)));
}

core::Result<void> exhausted() {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_state, "evaluation graph revision space is exhausted"));
}

core::Result<void> parse_error(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::validation_failed, std::move(message)));
}

bool contains_whitespace(std::string_view value) noexcept {
    return value.find_first_of(" \t\r\n") != std::string_view::npos;
}

core::Result<void> expect(std::istringstream& stream, std::string_view expected) {
    std::string actual;
    if (!(stream >> actual) || actual != expected) {
        return parse_error("evaluation graph serialization expected " + std::string(expected));
    }
    return core::Result<void>::success();
}

core::Result<std::uint64_t> read_uint(
    std::istringstream& stream,
    std::string_view field) {
    std::string token;
    if (!(stream >> token)) {
        return core::Result<std::uint64_t>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "evaluation graph serialization is missing " + std::string(field)));
    }
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return core::Result<std::uint64_t>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "evaluation graph serialization contains an invalid " + std::string(field)));
    }
    return core::Result<std::uint64_t>::success(value);
}

core::Result<std::size_t> read_count(
    std::istringstream& stream,
    std::string_view field,
    std::uint64_t limit) {
    const auto count = read_uint(stream, field);
    if (!count) return core::Result<std::size_t>::failure(count.error());
    if (count.value() > limit || count.value() > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::size_t>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "evaluation graph serialization count exceeds its limit"));
    }
    return core::Result<std::size_t>::success(static_cast<std::size_t>(count.value()));
}

std::string canonical_graph(
    const std::map<NodeTypeId, NodeTypeDefinition>& types,
    const std::map<NodeId, NodeInstance>& nodes,
    const std::vector<Connection>& connections,
    core::Revision revision) {
    std::ostringstream stream;
    stream << "revision=" << revision.value() << '\n';
    for (const auto& [type_id, type] : types) {
        stream << "type=" << type_id.value << '\n';
        for (const auto& port : type.ports) {
            stream << "port=" << port.id.value << ':'
                   << static_cast<int>(port.direction) << ':'
                   << static_cast<int>(port.value_kind) << '\n';
        }
    }
    for (const auto& [node_id, node] : nodes) {
        stream << "node=" << node_id.value << ':' << node.type.value << ':'
               << node.parameter_revision.value() << ':' << node.parameters_digest << '\n';
    }
    std::vector<Connection> sorted_connections = connections;
    std::sort(sorted_connections.begin(), sorted_connections.end());
    for (const auto& connection : sorted_connections) {
        stream << "edge=" << connection.output_node.value << ':'
               << connection.output_port.value << ':' << connection.input_node.value << ':'
               << connection.input_port.value << '\n';
    }
    return stream.str();
}

} // namespace

core::Result<void> EvaluationGraph::register_type(NodeTypeDefinition definition) {
    if (auto result = definition.validate(); !result) return result;
    if (types_.contains(definition.id)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "evaluation node type is already registered"));
    }
    types_.emplace(definition.id, std::move(definition));
    return core::Result<void>::success();
}

core::Result<NodeId> EvaluationGraph::create_node(
    NodeTypeId type,
    std::string parameters_digest) {
    if (type.value.empty() || parameters_digest.empty() ||
        contains_whitespace(type.value) || contains_whitespace(parameters_digest)) {
        return core::Result<NodeId>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "evaluation node creation requires a type and parameter digest"));
    }
    if (!types_.contains(type)) {
        return core::Result<NodeId>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "evaluation node type is not registered"));
    }
    if (revision_.exhausted() ||
        next_node_id_ == 0U ||
        next_node_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<NodeId>::failure(exhausted().error());
    }
    const NodeId id{next_node_id_++};
    nodes_.emplace(id, NodeInstance{id, std::move(type), revision_, std::move(parameters_digest)});
    if (auto result = bump_revision(); !result) {
        nodes_.erase(id);
        return core::Result<NodeId>::failure(result.error());
    }
    return core::Result<NodeId>::success(id);
}

core::Result<void> EvaluationGraph::remove_node(NodeId node) {
    if (!node) return invalid("cannot remove an invalid evaluation node");
    if (revision_.exhausted()) return exhausted();
    if (!nodes_.contains(node)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "cannot remove a missing evaluation node"));
    }
    nodes_.erase(node);
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(), [node](const Connection& connection) {
            return connection.output_node == node || connection.input_node == node;
        }),
        connections_.end());
    return bump_revision();
}

core::Result<void> EvaluationGraph::validate_connection(const Connection& connection) const {
    const auto* output_node = find_node(connection.output_node);
    const auto* input_node = find_node(connection.input_node);
    if (output_node == nullptr || input_node == nullptr) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "evaluation connection references a missing node"));
    }
    const auto* output_type = find_type(output_node->type);
    const auto* input_type = find_type(input_node->type);
    if (output_type == nullptr || input_type == nullptr) {
        return validation("evaluation connection references an unregistered node type");
    }
    const auto* output_port = output_type->find_port(connection.output_port);
    const auto* input_port = input_type->find_port(connection.input_port);
    if (output_port == nullptr || input_port == nullptr) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "evaluation connection references a missing port"));
    }
    if (output_port->direction != PortDirection::output ||
        input_port->direction != PortDirection::input) {
        return validation("evaluation connection must run from an output to an input");
    }
    if (output_port->value_kind != input_port->value_kind) {
        return validation("evaluation connection contains incompatible port types");
    }
    if (connection.output_node == connection.input_node) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "evaluation graph does not allow self-cycles"));
    }
    return core::Result<void>::success();
}

core::Result<void> EvaluationGraph::connect(Connection connection) {
    if (auto result = validate_connection(connection); !result) return result;
    if (revision_.exhausted()) return exhausted();
    if (std::find(connections_.begin(), connections_.end(), connection) != connections_.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "evaluation connection already exists"));
    }
    for (const auto& existing : connections_) {
        if (existing.input_node == connection.input_node &&
            existing.input_port == connection.input_port) {
            return validation("evaluation input ports accept only one connection");
        }
    }
    connections_.push_back(std::move(connection));
    if (auto order = topological_order(); !order) {
        connections_.pop_back();
        return core::Result<void>::failure(order.error());
    }
    return bump_revision();
}

core::Result<void> EvaluationGraph::disconnect(Connection connection) {
    if (revision_.exhausted()) return exhausted();
    const auto iterator = std::find(connections_.begin(), connections_.end(), connection);
    if (iterator == connections_.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "evaluation connection does not exist"));
    }
    connections_.erase(iterator);
    return bump_revision();
}

core::Result<void> EvaluationGraph::validate() const {
    for (const auto& [type_id, type] : types_) {
        if (type_id != type.id) return validation("evaluation type map key does not match its id");
        if (auto result = type.validate(); !result) return result;
    }
    for (const auto& [node_id, node] : nodes_) {
        if (node_id != node.id) return validation("evaluation node map key does not match its id");
        if (auto result = node.validate(); !result) return result;
        if (node.parameter_revision > revision_) {
            return validation("evaluation node parameter revision is newer than the graph");
        }
        if (!types_.contains(node.type)) return validation("evaluation node type is missing");
    }
    std::set<Connection> unique_connections;
    for (const auto& connection : connections_) {
        if (!unique_connections.insert(connection).second) {
            return validation("evaluation graph contains duplicate connections");
        }
        if (auto result = validate_connection(connection); !result) return result;
    }
    for (const auto& connection : connections_) {
        for (const auto& other : connections_) {
            if (&connection != &other && connection.input_node == other.input_node &&
                connection.input_port == other.input_port) {
                return validation("evaluation input port has multiple connections");
            }
        }
    }
    const auto order = topological_order();
    if (!order) return core::Result<void>::failure(order.error());
    return core::Result<void>::success();
}

core::Result<std::vector<NodeId>> EvaluationGraph::topological_order() const {
    std::map<NodeId, std::size_t> indegree;
    std::map<NodeId, std::vector<NodeId>> outgoing;
    for (const auto& [node_id, ignored] : nodes_) {
        static_cast<void>(ignored);
        indegree.emplace(node_id, 0U);
        outgoing.emplace(node_id, std::vector<NodeId>{});
    }
    for (const auto& connection : connections_) {
        if (!nodes_.contains(connection.output_node) || !nodes_.contains(connection.input_node)) {
            return core::Result<std::vector<NodeId>>::failure(core::Diagnostic(
                core::ErrorCode::not_found, "evaluation graph edge references a missing node"));
        }
        ++indegree.at(connection.input_node);
        outgoing.at(connection.output_node).push_back(connection.input_node);
    }
    std::set<NodeId> ready;
    for (const auto& [node_id, degree] : indegree) {
        if (degree == 0U) ready.insert(node_id);
    }
    std::vector<NodeId> order;
    order.reserve(nodes_.size());
    while (!ready.empty()) {
        const NodeId node = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(node);
        for (const NodeId next : outgoing.at(node)) {
            auto& degree = indegree.at(next);
            --degree;
            if (degree == 0U) ready.insert(next);
        }
    }
    if (order.size() != nodes_.size()) {
        return core::Result<std::vector<NodeId>>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "evaluation graph contains a cycle"));
    }
    return core::Result<std::vector<NodeId>>::success(std::move(order));
}

core::Result<GraphSnapshot> EvaluationGraph::snapshot() const {
    if (auto result = validate(); !result) {
        return core::Result<GraphSnapshot>::failure(result.error());
    }
    GraphSnapshot snapshot;
    snapshot.revision = revision_;
    for (const auto& [ignored, type] : types_) {
        static_cast<void>(ignored);
        snapshot.types.push_back(type);
    }
    for (const auto& [ignored, node] : nodes_) {
        static_cast<void>(ignored);
        snapshot.nodes.push_back(node);
    }
    snapshot.connections = connections_;
    snapshot.content_digest = content_digest();
    return core::Result<GraphSnapshot>::success(std::move(snapshot));
}

std::string EvaluationGraph::content_digest() const {
    const std::string canonical = canonical_graph(types_, nodes_, connections_, revision_);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(canonical.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, canonical.size()}).hex();
}

core::Result<std::string> EvaluationGraph::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    std::ostringstream stream;
    stream << "CARTOGRAPHER_EVAL_GRAPH 1\n";
    stream << "REVISION " << revision_.value() << "\n";
    stream << "TYPES " << types_.size() << "\n";
    for (const auto& [ignored, type] : types_) {
        static_cast<void>(ignored);
        stream << "TYPE " << type.id.value << ' ' << type.ports.size() << "\n";
        for (const auto& port : type.ports) {
            stream << "PORT " << port.id.value << ' '
                   << static_cast<std::uint32_t>(port.direction) << ' '
                   << static_cast<std::uint32_t>(port.value_kind) << "\n";
        }
    }
    stream << "NODES " << nodes_.size() << "\n";
    for (const auto& [ignored, node] : nodes_) {
        static_cast<void>(ignored);
        stream << "NODE " << node.id.value << ' ' << node.type.value << ' '
               << node.parameter_revision.value() << ' ' << node.parameters_digest << "\n";
    }
    std::vector<Connection> sorted_connections = connections_;
    std::sort(sorted_connections.begin(), sorted_connections.end());
    stream << "CONNECTIONS " << sorted_connections.size() << "\n";
    for (const auto& connection : sorted_connections) {
        stream << "LINK " << connection.output_node.value << ' '
               << connection.output_port.value << ' ' << connection.input_node.value << ' '
               << connection.input_port.value << "\n";
    }
    stream << "END\n";
    return core::Result<std::string>::success(stream.str());
}

core::Result<EvaluationGraph> EvaluationGraph::deserialize(std::string_view text) {
    if (text.empty() || text.size() > 128ULL * 1024ULL * 1024ULL) {
        return core::Result<EvaluationGraph>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "evaluation graph serialization is empty or exceeds the size limit"));
    }
    std::istringstream stream{std::string{text}};
    if (auto result = expect(stream, "CARTOGRAPHER_EVAL_GRAPH"); !result) {
        return core::Result<EvaluationGraph>::failure(result.error());
    }
    const auto version = read_uint(stream, "version");
    if (!version) return core::Result<EvaluationGraph>::failure(version.error());
    if (version.value() != 1U) {
        return core::Result<EvaluationGraph>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "unsupported evaluation graph serialization version"));
    }
    if (auto result = expect(stream, "REVISION"); !result) {
        return core::Result<EvaluationGraph>::failure(result.error());
    }
    const auto revision = read_uint(stream, "revision");
    if (!revision) return core::Result<EvaluationGraph>::failure(revision.error());
    if (revision.value() == core::Revision::max_value()) {
        return core::Result<EvaluationGraph>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "evaluation graph revision space is exhausted"));
    }

    EvaluationGraph graph;
    graph.revision_ = core::Revision{revision.value()};

    if (auto result = expect(stream, "TYPES"); !result) {
        return core::Result<EvaluationGraph>::failure(result.error());
    }
    const auto type_count = read_count(stream, "type count", 10000U);
    if (!type_count) return core::Result<EvaluationGraph>::failure(type_count.error());
    for (std::size_t index = 0U; index < type_count.value(); ++index) {
        if (auto result = expect(stream, "TYPE"); !result) {
            return core::Result<EvaluationGraph>::failure(result.error());
        }
        std::string type_id;
        std::size_t port_count = 0U;
        if (!(stream >> type_id)) {
            return core::Result<EvaluationGraph>::failure(parse_error(
                "evaluation graph serialization is missing a type id").error());
        }
        const auto parsed_ports = read_count(stream, "port count", 128U);
        if (!parsed_ports) return core::Result<EvaluationGraph>::failure(parsed_ports.error());
        port_count = parsed_ports.value();
        NodeTypeDefinition definition;
        definition.id = NodeTypeId{std::move(type_id)};
        definition.ports.reserve(port_count);
        for (std::size_t port_index = 0U; port_index < port_count; ++port_index) {
            if (auto result = expect(stream, "PORT"); !result) {
                return core::Result<EvaluationGraph>::failure(result.error());
            }
            std::string port_id;
            if (!(stream >> port_id)) {
                return core::Result<EvaluationGraph>::failure(parse_error(
                    "evaluation graph serialization is missing a port id").error());
            }
            const auto direction = read_uint(stream, "port direction");
            const auto value_kind = read_uint(stream, "port value kind");
            if (!direction || !value_kind || direction.value() > std::numeric_limits<std::uint32_t>::max() ||
                value_kind.value() > std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<EvaluationGraph>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "evaluation graph serialization contains an invalid port enum"));
            }
            definition.ports.push_back(PortDescriptor{
                PortId{std::move(port_id)},
                static_cast<PortDirection>(direction.value()),
                static_cast<ValueKind>(value_kind.value()),
            });
        }
        if (auto result = graph.register_type(std::move(definition)); !result) {
            return core::Result<EvaluationGraph>::failure(result.error());
        }
    }

    if (auto result = expect(stream, "NODES"); !result) {
        return core::Result<EvaluationGraph>::failure(result.error());
    }
    const auto node_count = read_count(stream, "node count", 1'000'000U);
    if (!node_count) return core::Result<EvaluationGraph>::failure(node_count.error());
    std::uint64_t maximum_node_id = 0U;
    for (std::size_t index = 0U; index < node_count.value(); ++index) {
        if (auto result = expect(stream, "NODE"); !result) {
            return core::Result<EvaluationGraph>::failure(result.error());
        }
        const auto id = read_uint(stream, "node id");
        if (!id) return core::Result<EvaluationGraph>::failure(id.error());
        std::string type_id;
        std::uint64_t parameter_revision = 0U;
        std::string parameters_digest;
        if (!(stream >> type_id >> parameter_revision >> parameters_digest)) {
            return core::Result<EvaluationGraph>::failure(parse_error(
                "evaluation graph serialization contains an incomplete node").error());
        }
        NodeInstance node{
            NodeId{id.value()},
            NodeTypeId{std::move(type_id)},
            core::Revision{parameter_revision},
            std::move(parameters_digest),
        };
        if (auto result = node.validate(); !result) {
            return core::Result<EvaluationGraph>::failure(result.error());
        }
        if (!graph.types_.contains(node.type) || graph.nodes_.contains(node.id)) {
            return core::Result<EvaluationGraph>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "evaluation graph serialization contains an invalid or duplicate node"));
        }
        maximum_node_id = std::max(maximum_node_id, node.id.value);
        graph.nodes_.emplace(node.id, std::move(node));
    }
    if (maximum_node_id == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<EvaluationGraph>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "evaluation node id space is exhausted"));
    }
    graph.next_node_id_ = maximum_node_id + 1U;

    if (auto result = expect(stream, "CONNECTIONS"); !result) {
        return core::Result<EvaluationGraph>::failure(result.error());
    }
    const auto connection_count = read_count(stream, "connection count", 2'000'000U);
    if (!connection_count) return core::Result<EvaluationGraph>::failure(connection_count.error());
    graph.connections_.reserve(connection_count.value());
    for (std::size_t index = 0U; index < connection_count.value(); ++index) {
        if (auto result = expect(stream, "LINK"); !result) {
            return core::Result<EvaluationGraph>::failure(result.error());
        }
        const auto output_node = read_uint(stream, "output node");
        std::string output_port;
        if (!output_node || !(stream >> output_port)) {
            return core::Result<EvaluationGraph>::failure(parse_error(
                "evaluation graph serialization contains an incomplete connection").error());
        }
        const auto input_node = read_uint(stream, "input node");
        std::string input_port;
        if (!input_node || !(stream >> input_port)) {
            return core::Result<EvaluationGraph>::failure(parse_error(
                "evaluation graph serialization contains an incomplete connection").error());
        }
        Connection connection{
            NodeId{output_node.value()},
            PortId{std::move(output_port)},
            NodeId{input_node.value()},
            PortId{std::move(input_port)},
        };
        if (auto result = graph.validate_connection(connection); !result) {
            return core::Result<EvaluationGraph>::failure(result.error());
        }
        graph.connections_.push_back(std::move(connection));
    }
    if (auto result = expect(stream, "END"); !result) {
        return core::Result<EvaluationGraph>::failure(result.error());
    }
    std::string trailing;
    if (stream >> trailing) {
        return core::Result<EvaluationGraph>::failure(parse_error(
            "evaluation graph serialization contains trailing data").error());
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<EvaluationGraph>::failure(result.error());
    }
    return core::Result<EvaluationGraph>::success(std::move(graph));
}

const NodeInstance* EvaluationGraph::find_node(NodeId node) const noexcept {
    const auto iterator = nodes_.find(node);
    return iterator == nodes_.end() ? nullptr : &iterator->second;
}

const NodeTypeDefinition* EvaluationGraph::find_type(NodeTypeId type) const noexcept {
    const auto iterator = types_.find(type);
    return iterator == types_.end() ? nullptr : &iterator->second;
}

core::Result<void> EvaluationGraph::bump_revision() {
    if (revision_.exhausted()) return exhausted();
    revision_ = revision_.next();
    return core::Result<void>::success();
}

} // namespace carto::eval
