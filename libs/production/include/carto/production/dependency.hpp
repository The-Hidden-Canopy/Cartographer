#pragma once

#include <carto/production/closure.hpp>

#include <cstddef>
#include <vector>

namespace carto::production {

struct ProductionCookNode {
    ProductionCookKey key;
    std::vector<ProductionCookKey> dependencies;

    [[nodiscard]] core::Result<void> validate() const;
};

// A bounded, deterministic dependency graph for derived production products.
// Dependencies not registered as nodes are treated as authoritative/external
// inputs; registered dependencies participate in cycle and invalidation checks.
class ProductionCookGraph final {
public:
    static constexpr std::size_t max_nodes = 4096U;
    static constexpr std::size_t max_dependencies_per_node = 64U;

    [[nodiscard]] core::Result<void> add_node(
        ProductionCookKey key,
        std::vector<ProductionCookKey> dependencies = {});
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<ProductionCookKey>> topological_order() const;
    [[nodiscard]] core::Result<std::vector<ProductionCookKey>> invalidation_order(
        const ProductionCookKey& changed) const;

    [[nodiscard]] const std::vector<ProductionCookNode>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

private:
    std::vector<ProductionCookNode> nodes_;
};

} // namespace carto::production
