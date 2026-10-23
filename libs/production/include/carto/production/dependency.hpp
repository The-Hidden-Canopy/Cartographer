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

enum class ProductionDependencyResolution : std::uint8_t {
    registered_node,
    declared_external_input,
    missing_input,
};

struct ProductionCookDependencyEdge final {
    ProductionCookKey product;
    ProductionCookKey dependency;
    ProductionDependencyResolution resolution =
        ProductionDependencyResolution::missing_input;
};

struct ProductionCookGraphInspection final {
    // Nodes are returned in the same deterministic order used by cooking.
    std::vector<ProductionCookNode> ordered_nodes;
    std::vector<ProductionCookDependencyEdge> edges;
    std::vector<ProductionCookKey> external_inputs;
    std::vector<ProductionCookKey> missing_inputs;
};

enum class ProductionInvalidationReason : std::uint8_t {
    not_affected,
    changed_source,
    direct_dependency_changed,
    upstream_dependency_changed,
    global_footprint,
    spatial_overlap,
    spatially_disjoint,
    upstream_spatially_disjoint,
};

struct ProductionInvalidationExplanation final {
    ProductionCookKey product;
    bool invalidated = false;
    ProductionInvalidationReason reason = ProductionInvalidationReason::not_affected;
    std::optional<ProductionCookKey> via_dependency;
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
    // Classifies every dependency against registered nodes and the caller's
    // explicit authoritative external-input inventory. Unlisted dependencies
    // are reported as missing; this diagnostic does not change cook behavior.
    [[nodiscard]] core::Result<ProductionCookGraphInspection> inspect(
        const std::vector<ProductionCookKey>& declared_external_inputs = {}) const;
    // Returns one row per registered node in deterministic topological order,
    // including unaffected and spatially filtered products with their reason.
    [[nodiscard]] core::Result<std::vector<ProductionInvalidationExplanation>>
    explain_invalidation(
        const ProductionCookKey& changed,
        const std::optional<SpatialTileRange>& dirty_region = std::nullopt) const;
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
