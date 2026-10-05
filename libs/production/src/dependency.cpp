#include <carto/production/dependency.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace carto::production {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

std::optional<std::size_t> find_node(
    const std::vector<ProductionCookNode>& nodes,
    const ProductionCookKey& key) {
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        if (nodes[index].key == key) return index;
    }
    return std::nullopt;
}

bool key_less(const ProductionCookKey& left, const ProductionCookKey& right) {
    if (left.source.identity != right.source.identity) {
        return left.source.identity < right.source.identity;
    }
    if (left.source.revision != right.source.revision) {
        return left.source.revision < right.source.revision;
    }
    if (left.source.content_digest != right.source.content_digest) {
        return left.source.content_digest < right.source.content_digest;
    }
    if (left.kind != right.kind) {
        return static_cast<unsigned>(left.kind) < static_cast<unsigned>(right.kind);
    }
    if (left.form != right.form) {
        return static_cast<unsigned>(left.form) < static_cast<unsigned>(right.form);
    }
    if (left.algorithm != right.algorithm) return left.algorithm < right.algorithm;
    if (left.settings_digest != right.settings_digest) {
        return left.settings_digest < right.settings_digest;
    }
    return left.platform_profile < right.platform_profile;
}

core::Result<std::vector<std::size_t>> topological_indices(
    const std::vector<ProductionCookNode>& nodes) {
    std::vector<std::size_t> indegree(nodes.size(), 0U);
    std::vector<std::vector<std::size_t>> dependents(nodes.size());
    for (std::size_t node_index = 0U; node_index < nodes.size(); ++node_index) {
        for (const auto& dependency : nodes[node_index].dependencies) {
            const auto dependency_index = find_node(nodes, dependency);
            if (!dependency_index.has_value()) continue;
            ++indegree[node_index];
            dependents[dependency_index.value()].push_back(node_index);
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t index = 0U; index < indegree.size(); ++index) {
        if (indegree[index] == 0U) ready.push_back(index);
    }

    const auto sort_ready = [&nodes](std::vector<std::size_t>& values) {
        std::sort(values.begin(), values.end(), [&nodes](std::size_t left, std::size_t right) {
            return key_less(nodes[left].key, nodes[right].key);
        });
    };
    sort_ready(ready);

    std::vector<std::size_t> order;
    order.reserve(nodes.size());
    while (!ready.empty()) {
        const std::size_t current = ready.front();
        ready.erase(ready.begin());
        order.push_back(current);
        for (const std::size_t dependent : dependents[current]) {
            --indegree[dependent];
            if (indegree[dependent] == 0U) ready.push_back(dependent);
        }
        sort_ready(ready);
    }

    if (order.size() != nodes.size()) {
        return core::Result<std::vector<std::size_t>>::failure(validation(
            "production cook graph contains a dependency cycle"));
    }
    return core::Result<std::vector<std::size_t>>::success(std::move(order));
}

} // namespace

core::Result<void> ProductionCookNode::validate() const {
    if (auto result = key.validate(); !result) return result;
    if (dependencies.size() > ProductionCookGraph::max_dependencies_per_node) {
        return core::Result<void>::failure(validation(
            "production cook node exceeds its dependency bound"));
    }
    for (std::size_t index = 0U; index < dependencies.size(); ++index) {
        if (auto result = dependencies[index].validate(); !result) return result;
        if (dependencies[index] == key) {
            return core::Result<void>::failure(validation(
                "production cook node cannot depend on itself"));
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (dependencies[prior] == dependencies[index]) {
                return core::Result<void>::failure(invalid(
                    "production cook node contains duplicate dependencies"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ProductionCookGraph::add_node(
    ProductionCookKey key,
    std::vector<ProductionCookKey> dependencies) {
    if (nodes_.size() >= max_nodes) {
        return core::Result<void>::failure(validation(
            "production cook graph has reached its bounded node capacity"));
    }
    ProductionCookNode node{std::move(key), std::move(dependencies)};
    if (auto result = node.validate(); !result) return result;
    if (find_node(nodes_, node.key).has_value()) {
        return core::Result<void>::failure(invalid(
            "production cook graph cannot contain duplicate node keys"));
    }

    nodes_.push_back(std::move(node));
    if (auto result = validate(); !result) {
        nodes_.pop_back();
        return result;
    }
    return core::Result<void>::success();
}

core::Result<void> ProductionCookGraph::validate() const {
    if (nodes_.size() > max_nodes) {
        return core::Result<void>::failure(validation(
            "production cook graph exceeds its bounded node capacity"));
    }
    for (std::size_t index = 0U; index < nodes_.size(); ++index) {
        if (auto result = nodes_[index].validate(); !result) return result;
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (nodes_[prior].key == nodes_[index].key) {
                return core::Result<void>::failure(invalid(
                    "production cook graph contains duplicate node keys"));
            }
        }
    }
    const auto order = topological_indices(nodes_);
    if (!order) return core::Result<void>::failure(order.error());
    return core::Result<void>::success();
}

core::Result<std::vector<ProductionCookKey>> ProductionCookGraph::topological_order() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    const auto indices = topological_indices(nodes_);
    if (!indices) return core::Result<std::vector<ProductionCookKey>>::failure(indices.error());

    std::vector<ProductionCookKey> order;
    order.reserve(indices.value().size());
    for (const std::size_t index : indices.value()) order.push_back(nodes_[index].key);
    return core::Result<std::vector<ProductionCookKey>>::success(std::move(order));
}

core::Result<std::vector<ProductionCookKey>> ProductionCookGraph::invalidation_order(
    const ProductionCookKey& changed) const {
    if (auto result = changed.validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    const auto indices = topological_indices(nodes_);
    if (!indices) return core::Result<std::vector<ProductionCookKey>>::failure(indices.error());

    std::vector<bool> invalidated(nodes_.size(), false);
    for (const std::size_t index : indices.value()) {
        if (nodes_[index].key == changed) {
            invalidated[index] = true;
            continue;
        }
        invalidated[index] = std::any_of(
            nodes_[index].dependencies.begin(),
            nodes_[index].dependencies.end(),
            [&changed, &invalidated, this](const ProductionCookKey& dependency) {
                if (dependency == changed) return true;
                const auto dependency_index = find_node(nodes_, dependency);
                return dependency_index.has_value() && invalidated[dependency_index.value()];
            });
    }

    std::vector<ProductionCookKey> result;
    for (const std::size_t index : indices.value()) {
        if (invalidated[index]) result.push_back(nodes_[index].key);
    }
    return core::Result<std::vector<ProductionCookKey>>::success(std::move(result));
}

} // namespace carto::production
