#pragma once

#include <carto/production/closure.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::production {

// Inclusive integer tile range in one explicitly named grid and level. The
// integer contract avoids float-boundary ambiguity in sparse invalidation.
struct SpatialTileRange {
    std::string grid_identity;
    std::uint32_t level = 0U;
    std::int64_t minimum_x = 0;
    std::int64_t minimum_y = 0;
    std::int64_t minimum_z = 0;
    std::int64_t maximum_x = 0;
    std::int64_t maximum_y = 0;
    std::int64_t maximum_z = 0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<bool> intersects(const SpatialTileRange& other) const;
    [[nodiscard]] bool operator==(const SpatialTileRange&) const noexcept = default;
};

struct ProductionCookNode {
    ProductionCookKey key;
    std::vector<ProductionCookKey> dependencies;
    // Missing means global/unknown and therefore cannot be spatially skipped.
    std::optional<SpatialTileRange> footprint;

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
    [[nodiscard]] core::Result<void> add_spatial_node(
        ProductionCookKey key,
        SpatialTileRange footprint,
        std::vector<ProductionCookKey> dependencies = {});
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<ProductionCookKey>> topological_order() const;
    [[nodiscard]] core::Result<std::vector<ProductionCookKey>> invalidation_order(
        const ProductionCookKey& changed) const;
    // Spatially filters downstream invalidation. Any global node remains
    // dirty; incompatible grids/levels fail closed instead of assuming
    // non-overlap.
    [[nodiscard]] core::Result<std::vector<ProductionCookKey>> invalidation_order(
        const ProductionCookKey& changed,
        const SpatialTileRange& dirty_region) const;

    [[nodiscard]] const std::vector<ProductionCookNode>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

private:
    [[nodiscard]] core::Result<void> add_node_internal(ProductionCookNode node);
    std::vector<ProductionCookNode> nodes_;
};

} // namespace carto::production
