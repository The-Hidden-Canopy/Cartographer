#include <carto/production/dependency.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace carto::production {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
                byte != '\r' && byte != '\n';
        });
}

bool bounded_axis(std::int64_t minimum, std::int64_t maximum) {
    constexpr std::int64_t coordinate_limit = 1LL << 40;
    constexpr std::uint64_t span_limit = 1ULL << 20;
    if (minimum < -coordinate_limit || maximum > coordinate_limit || minimum > maximum) {
        return false;
    }
    return static_cast<std::uint64_t>(maximum - minimum) <= span_limit;
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
    if (left.platform_profile != right.platform_profile) {
        return left.platform_profile < right.platform_profile;
    }
    if (left.representation_kind != right.representation_kind) {
        return left.representation_kind < right.representation_kind;
    }
    if (left.spatial_scope != right.spatial_scope) {
        return left.spatial_scope < right.spatial_scope;
    }
    if (left.decision_context.truth_class != right.decision_context.truth_class) {
        return left.decision_context.truth_class < right.decision_context.truth_class;
    }
    if (left.decision_context.observer_class != right.decision_context.observer_class) {
        return left.decision_context.observer_class < right.decision_context.observer_class;
    }
    if (left.decision_context.precision_requirement !=
        right.decision_context.precision_requirement) {
        return left.decision_context.precision_requirement <
            right.decision_context.precision_requirement;
    }
    return left.decision_context.materialization_requirement <
        right.decision_context.materialization_requirement;
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

core::Result<void> SpatialTileRange::validate() const {
    if (!safe_text(grid_identity, 128U) || level > 31U ||
        !bounded_axis(minimum_x, maximum_x) ||
        !bounded_axis(minimum_y, maximum_y) ||
        !bounded_axis(minimum_z, maximum_z)) {
        return core::Result<void>::failure(invalid(
            "spatial tile range grid, level, coordinates, or span is invalid"));
    }
    return core::Result<void>::success();
}

std::string SpatialTileRange::canonical() const {
    std::ostringstream output;
    output << "tile-range-v1{" << grid_identity << ';' << level << ';'
           << minimum_x << ',' << minimum_y << ',' << minimum_z << ';'
           << maximum_x << ',' << maximum_y << ',' << maximum_z << '}';
    return output.str();
}

core::Result<bool> SpatialTileRange::intersects(const SpatialTileRange& other) const {
    if (auto result = validate(); !result) {
        return core::Result<bool>::failure(result.error());
    }
    if (auto result = other.validate(); !result) {
        return core::Result<bool>::failure(result.error());
    }
    if (grid_identity != other.grid_identity || level != other.level) {
        return core::Result<bool>::failure(validation(
            "spatial tile ranges use incompatible grids or levels"));
    }
    return core::Result<bool>::success(
        minimum_x <= other.maximum_x && maximum_x >= other.minimum_x &&
        minimum_y <= other.maximum_y && maximum_y >= other.minimum_y &&
        minimum_z <= other.maximum_z && maximum_z >= other.minimum_z);
}

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
    if (footprint.has_value()) {
        if (auto result = footprint->validate(); !result) return result;
        if (key.spatial_scope != footprint->canonical()) {
            return core::Result<void>::failure(validation(
                "production cook node footprint is not bound into its cook key"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ProductionCookGraph::add_node(
    ProductionCookKey key,
    std::vector<ProductionCookKey> dependencies) {
    return add_node_internal(ProductionCookNode{
        std::move(key), std::move(dependencies), std::nullopt});
}

core::Result<void> ProductionCookGraph::add_spatial_node(
    ProductionCookKey key,
    SpatialTileRange footprint,
    std::vector<ProductionCookKey> dependencies) {
    return add_node_internal(ProductionCookNode{
        std::move(key), std::move(dependencies), std::move(footprint)});
}

core::Result<void> ProductionCookGraph::add_node_internal(ProductionCookNode node) {
    if (nodes_.size() >= max_nodes) {
        return core::Result<void>::failure(validation(
            "production cook graph has reached its bounded node capacity"));
    }
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

core::Result<std::vector<ProductionCookKey>> ProductionCookGraph::invalidation_order(
    const ProductionCookKey& changed,
    const SpatialTileRange& dirty_region) const {
    if (auto result = changed.validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    if (auto result = dirty_region.validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    if (auto result = validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    const auto indices = topological_indices(nodes_);
    if (!indices) return core::Result<std::vector<ProductionCookKey>>::failure(indices.error());

    std::vector<bool> invalidated(nodes_.size(), false);
    for (const std::size_t index : indices.value()) {
        const bool exact_change = nodes_[index].key == changed;
        const bool downstream = std::any_of(
            nodes_[index].dependencies.begin(), nodes_[index].dependencies.end(),
            [&changed, &invalidated, this](const ProductionCookKey& dependency) {
                if (dependency == changed) return true;
                const auto dependency_index = find_node(nodes_, dependency);
                return dependency_index.has_value() && invalidated[dependency_index.value()];
            });
        if (!exact_change && !downstream) continue;
        if (exact_change || !nodes_[index].footprint.has_value()) {
            invalidated[index] = true;
            continue;
        }
        const auto overlap = nodes_[index].footprint->intersects(dirty_region);
        if (!overlap) {
            return core::Result<std::vector<ProductionCookKey>>::failure(overlap.error());
        }
        invalidated[index] = overlap.value();
    }

    std::vector<ProductionCookKey> result;
    for (const std::size_t index : indices.value()) {
        if (invalidated[index]) result.push_back(nodes_[index].key);
    }
    return core::Result<std::vector<ProductionCookKey>>::success(std::move(result));
}

} // namespace carto::production
