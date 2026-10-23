#include <carto/production/dependency.hpp>

#include <algorithm>
#include <limits>
#include <map>
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

struct ProductionCookKeyLess final {
    [[nodiscard]] bool operator()(
        const ProductionCookKey& left,
        const ProductionCookKey& right) const {
        return key_less(left, right);
    }
};

using ProductionCookNodeIndex =
    std::map<ProductionCookKey, std::size_t, ProductionCookKeyLess>;

ProductionCookNodeIndex index_nodes(const std::vector<ProductionCookNode>& nodes) {
    ProductionCookNodeIndex index;
    for (std::size_t node_index = 0U; node_index < nodes.size(); ++node_index) {
        index.emplace(nodes[node_index].key, node_index);
    }
    return index;
}

core::Result<std::vector<std::size_t>> topological_indices(
    const std::vector<ProductionCookNode>& nodes) {
    const auto indices_by_key = index_nodes(nodes);
    std::vector<std::size_t> indegree(nodes.size(), 0U);
    std::vector<std::vector<std::size_t>> dependents(nodes.size());
    for (std::size_t node_index = 0U; node_index < nodes.size(); ++node_index) {
        for (const auto& dependency : nodes[node_index].dependencies) {
            const auto dependency_position = indices_by_key.find(dependency);
            if (dependency_position == indices_by_key.end()) continue;
            const std::size_t dependency_index = dependency_position->second;
            ++indegree[node_index];
            dependents[dependency_index].push_back(node_index);
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
        constexpr std::size_t no_position = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> position(nodes.size(), no_position);
        std::vector<std::size_t> path;
        std::optional<std::size_t> start;
        for (std::size_t index = 0U; index < indegree.size(); ++index) {
            if (indegree[index] == 0U) continue;
            if (!start.has_value() || key_less(nodes[index].key, nodes[*start].key)) {
                start = index;
            }
        }

        std::optional<std::size_t> repeated;
        std::size_t current = start.value_or(0U);
        while (start.has_value() && position[current] == no_position) {
            position[current] = path.size();
            path.push_back(current);
            std::optional<std::size_t> next;
            for (const auto& dependency : nodes[current].dependencies) {
                const auto dependency_position = indices_by_key.find(dependency);
                if (dependency_position == indices_by_key.end() ||
                    indegree[dependency_position->second] == 0U) {
                    continue;
                }
                const std::size_t dependency_index = dependency_position->second;
                if (!next.has_value() || key_less(
                        nodes[dependency_index].key, nodes[*next].key)) {
                    next = dependency_index;
                }
            }
            if (!next.has_value()) break;
            if (position[*next] != no_position) {
                repeated = next;
                break;
            }
            current = *next;
        }

        std::ostringstream message;
        message << "production cook graph contains a dependency cycle";
        if (repeated.has_value()) {
            const std::size_t first = position[*repeated];
            message << ": ";
            bool truncated = false;
            for (std::size_t path_index = first; path_index <= path.size(); ++path_index) {
                const std::size_t node_index = path_index == path.size()
                    ? *repeated : path[path_index];
                const auto& key = nodes[node_index].key;
                std::ostringstream label;
                label << key.source.identity << '@' << key.source.revision.value()
                      << '/' << artifact_kind_name(key.kind) << '/' << key.algorithm
                      << '#' << key.source.content_digest.hex().substr(0U, 12U);
                const std::string value = label.str();
                if (message.tellp() > std::streampos(1800) ||
                    value.size() > 2048U - static_cast<std::size_t>(message.tellp())) {
                    message << "...";
                    truncated = true;
                    break;
                }
                if (path_index != first) message << " -> ";
                message << value;
            }
            if (truncated) message << " (cycle path truncated)";
        }
        return core::Result<std::vector<std::size_t>>::failure(validation(
            message.str()));
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
    ProductionCookNodeIndex seen;
    for (std::size_t index = 0U; index < nodes_.size(); ++index) {
        if (auto result = nodes_[index].validate(); !result) return result;
        if (!seen.emplace(nodes_[index].key, index).second) {
            return core::Result<void>::failure(invalid(
                "production cook graph contains duplicate node keys"));
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

core::Result<ProductionCookGraphInspection> ProductionCookGraph::inspect(
    const std::vector<ProductionCookKey>& declared_external_inputs) const {
    if (auto result = validate(); !result) {
        return core::Result<ProductionCookGraphInspection>::failure(result.error());
    }
    if (declared_external_inputs.size() >
        max_nodes * max_dependencies_per_node) {
        return core::Result<ProductionCookGraphInspection>::failure(validation(
            "production cook inspection exceeds its external-input bound"));
    }

    std::vector<ProductionCookKey> external_inputs = declared_external_inputs;
    const auto graph_index = index_nodes(nodes_);
    for (const auto& input : external_inputs) {
        if (auto result = input.validate(); !result) {
            return core::Result<ProductionCookGraphInspection>::failure(result.error());
        }
        if (graph_index.contains(input)) {
            return core::Result<ProductionCookGraphInspection>::failure(invalid(
                "production cook input cannot be both an external input and a graph node"));
        }
    }
    std::sort(external_inputs.begin(), external_inputs.end(), key_less);
    if (std::adjacent_find(external_inputs.begin(), external_inputs.end()) !=
        external_inputs.end()) {
        return core::Result<ProductionCookGraphInspection>::failure(invalid(
            "production cook inspection contains duplicate external inputs"));
    }

    const auto indices = topological_indices(nodes_);
    if (!indices) {
        return core::Result<ProductionCookGraphInspection>::failure(indices.error());
    }
    ProductionCookGraphInspection inspection;
    inspection.ordered_nodes.reserve(nodes_.size());
    for (const std::size_t node_index : indices.value()) {
        const ProductionCookNode& node = nodes_[node_index];
        inspection.ordered_nodes.push_back(node);
        std::vector<ProductionCookKey> dependencies = node.dependencies;
        std::sort(dependencies.begin(), dependencies.end(), key_less);
        for (const auto& dependency : dependencies) {
            ProductionDependencyResolution resolution =
                ProductionDependencyResolution::missing_input;
            if (graph_index.contains(dependency)) {
                resolution = ProductionDependencyResolution::registered_node;
            } else if (std::binary_search(
                           external_inputs.begin(), external_inputs.end(),
                           dependency, key_less)) {
                resolution = ProductionDependencyResolution::declared_external_input;
                inspection.external_inputs.push_back(dependency);
            } else {
                inspection.missing_inputs.push_back(dependency);
            }
            inspection.edges.push_back({node.key, dependency, resolution});
        }
    }
    const auto unique_keys = [](std::vector<ProductionCookKey>& keys) {
        std::sort(keys.begin(), keys.end(), key_less);
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    };
    unique_keys(inspection.external_inputs);
    unique_keys(inspection.missing_inputs);
    return core::Result<ProductionCookGraphInspection>::success(
        std::move(inspection));
}

core::Result<std::vector<ProductionInvalidationExplanation>>
ProductionCookGraph::explain_invalidation(
    const ProductionCookKey& changed,
    const std::optional<SpatialTileRange>& dirty_region) const {
    if (auto result = changed.validate(); !result) {
        return core::Result<std::vector<ProductionInvalidationExplanation>>::failure(
            result.error());
    }
    if (dirty_region.has_value()) {
        if (auto result = dirty_region->validate(); !result) {
            return core::Result<std::vector<ProductionInvalidationExplanation>>::failure(
                result.error());
        }
    }
    if (auto result = validate(); !result) {
        return core::Result<std::vector<ProductionInvalidationExplanation>>::failure(
            result.error());
    }
    const auto indices = topological_indices(nodes_);
    if (!indices) {
        return core::Result<std::vector<ProductionInvalidationExplanation>>::failure(
            indices.error());
    }

    std::vector<ProductionInvalidationExplanation> by_node(nodes_.size());
    const auto graph_index = index_nodes(nodes_);
    for (std::size_t index = 0U; index < nodes_.size(); ++index) {
        by_node[index].product = nodes_[index].key;
    }
    for (const std::size_t index : indices.value()) {
        auto& explanation = by_node[index];
        const ProductionCookNode& node = nodes_[index];
        if (node.key == changed) {
            explanation.invalidated = true;
            explanation.reason = ProductionInvalidationReason::changed_source;
            continue;
        }

        std::vector<ProductionCookKey> dependencies = node.dependencies;
        std::sort(dependencies.begin(), dependencies.end(), key_less);
        std::optional<ProductionCookKey> invalidating_dependency;
        std::optional<ProductionCookKey> filtered_dependency;
        for (const auto& dependency : dependencies) {
            if (dependency == changed) {
                invalidating_dependency = dependency;
                break;
            }
            const auto dependency_position = graph_index.find(dependency);
            if (dependency_position == graph_index.end()) continue;
            const auto& dependency_explanation = by_node[dependency_position->second];
            if (dependency_explanation.invalidated) {
                invalidating_dependency = dependency;
                break;
            }
            if (!filtered_dependency.has_value() &&
                (dependency_explanation.reason ==
                     ProductionInvalidationReason::spatially_disjoint ||
                 dependency_explanation.reason ==
                     ProductionInvalidationReason::upstream_spatially_disjoint)) {
                filtered_dependency = dependency;
            }
        }

        if (invalidating_dependency.has_value()) {
            explanation.via_dependency = *invalidating_dependency;
            const bool direct = *invalidating_dependency == changed;
            if (!dirty_region.has_value()) {
                explanation.invalidated = true;
                explanation.reason = direct
                    ? ProductionInvalidationReason::direct_dependency_changed
                    : ProductionInvalidationReason::upstream_dependency_changed;
            } else if (!node.footprint.has_value()) {
                explanation.invalidated = true;
                explanation.reason = ProductionInvalidationReason::global_footprint;
            } else {
                const auto overlap = node.footprint->intersects(*dirty_region);
                if (!overlap) {
                    return core::Result<
                        std::vector<ProductionInvalidationExplanation>>::failure(
                            overlap.error());
                }
                explanation.invalidated = overlap.value();
                explanation.reason = overlap.value()
                    ? ProductionInvalidationReason::spatial_overlap
                    : ProductionInvalidationReason::spatially_disjoint;
            }
        } else if (filtered_dependency.has_value()) {
            explanation.via_dependency = *filtered_dependency;
            explanation.reason =
                ProductionInvalidationReason::upstream_spatially_disjoint;
        }
    }

    std::vector<ProductionInvalidationExplanation> ordered;
    ordered.reserve(by_node.size());
    for (const std::size_t index : indices.value()) {
        ordered.push_back(std::move(by_node[index]));
    }
    return core::Result<std::vector<ProductionInvalidationExplanation>>::success(
        std::move(ordered));
}

core::Result<std::vector<ProductionCookKey>> ProductionCookGraph::invalidation_order(
    const ProductionCookKey& changed) const {
    const auto explanations = explain_invalidation(changed);
    if (!explanations) {
        return core::Result<std::vector<ProductionCookKey>>::failure(
            explanations.error());
    }
    std::vector<ProductionCookKey> result;
    for (const auto& explanation : explanations.value()) {
        if (explanation.invalidated) result.push_back(explanation.product);
    }
    return core::Result<std::vector<ProductionCookKey>>::success(std::move(result));
}

core::Result<std::vector<ProductionCookKey>> ProductionCookGraph::invalidation_order(
    const ProductionCookKey& changed,
    const SpatialTileRange& dirty_region) const {
    const auto explanations = explain_invalidation(changed, dirty_region);
    if (!explanations) {
        return core::Result<std::vector<ProductionCookKey>>::failure(
            explanations.error());
    }
    std::vector<ProductionCookKey> result;
    for (const auto& explanation : explanations.value()) {
        if (explanation.invalidated) result.push_back(explanation.product);
    }
    return core::Result<std::vector<ProductionCookKey>>::success(std::move(result));
}

} // namespace carto::production
