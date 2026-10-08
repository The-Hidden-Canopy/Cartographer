#pragma once

#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/eval/node.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::eval {

struct GraphSnapshot {
    core::Revision revision;
    std::vector<NodeTypeDefinition> types;
    std::vector<NodeInstance> nodes;
    std::vector<Connection> connections;
    std::string content_digest;
};

class EvaluationGraph {
public:
    [[nodiscard]] core::Result<void> register_type(NodeTypeDefinition definition);
    [[nodiscard]] core::Result<NodeId> create_node(
        NodeTypeId type,
        std::string parameters_digest);
    [[nodiscard]] core::Result<void> remove_node(NodeId node);
    [[nodiscard]] core::Result<void> connect(Connection connection);
    [[nodiscard]] core::Result<void> disconnect(Connection connection);

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<NodeId>> topological_order() const;
    [[nodiscard]] core::Result<GraphSnapshot> snapshot() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<EvaluationGraph> deserialize(std::string_view text);
    [[nodiscard]] std::string content_digest() const;

    [[nodiscard]] const NodeInstance* find_node(NodeId node) const noexcept;
    [[nodiscard]] const NodeTypeDefinition* find_type(NodeTypeId type) const noexcept;
    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t connection_count() const noexcept { return connections_.size(); }

private:
    [[nodiscard]] core::Result<void> validate_connection(const Connection& connection) const;
    [[nodiscard]] core::Result<void> bump_revision();

    std::map<NodeTypeId, NodeTypeDefinition> types_;
    std::map<NodeId, NodeInstance> nodes_;
    std::vector<Connection> connections_;
    std::uint64_t next_node_id_ = 1U;
    core::Revision revision_{};
};

} // namespace carto::eval
