#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace carto::character {

// Typed character product kinds participating in invalidation.
enum class ProductKind : std::uint8_t {
    source_mesh,
    rest_skeleton,
    skin_weights,
    morph_target,
    tangent_data,
    collision_proxy,
    deformation_lod,
    material_shader,
    sampled_animation,
    runtime_frame,
    prepared_bundle,
};

// Typed dependency kinds an edit can introduce (ch04 migration/invalidation
// DAG). Each edge in the DAG is labelled with the dependency kind that, when
// changed, invalidates the edge's target.
enum class DependencyKind : std::uint8_t {
    mesh_topology_change,
    timeline_edit,
    material_edit,
    rig_edit,
    uv_change,
};

[[nodiscard]] std::string_view product_kind_name(ProductKind kind) noexcept;
[[nodiscard]] std::string_view dependency_kind_name(DependencyKind kind) noexcept;

struct InvalidationNode {
    ProductKind kind = ProductKind::source_mesh;
    // Content digest of the source record this product was built from; zero
    // for synthetic runtime-only kinds.
    assets::Sha256Digest source_digest;
};

struct InvalidationEdge {
    // `from` invalidates `to` when a `dependency` change lands on `from`'s
    // source digest.
    ProductKind from = ProductKind::source_mesh;
    ProductKind to = ProductKind::source_mesh;
    DependencyKind dependency = DependencyKind::mesh_topology_change;
};

// Typed invalidation DAG over product kinds and source digests. The default
// graph encodes the Chapter 04 guarantees: a mesh topology edit invalidates
// weights where correspondence cannot be maintained, dependent morphs,
// tangents, collision proxies, deformation LODs and every prepared bundle; a
// timeline edit invalidates sampled animation and runtime frames but NOT
// materials; a material edit invalidates material/shader state but NOT the
// rest skeleton.
class InvalidationDag final {
public:
    static constexpr std::size_t kMaxNodes = 256U;
    static constexpr std::size_t kMaxEdges = 1'024U;

    [[nodiscard]] core::Result<void> insert_node(InvalidationNode node);
    [[nodiscard]] core::Result<void> insert_edge(InvalidationEdge edge);

    // The Chapter 04 default wiring.
    [[nodiscard]] static InvalidationDag character_default();

    [[nodiscard]] const std::vector<InvalidationNode>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] const std::vector<InvalidationEdge>& edges() const noexcept {
        return edges_;
    }

    [[nodiscard]] core::Result<void> validate() const;

    // Transitive closure: every product kind invalidated when edits of the
    // given dependency kinds land. Result is sorted and unique.
    [[nodiscard]] std::set<ProductKind> propagate(
        std::span<const DependencyKind> changed) const;

private:
    std::vector<InvalidationNode> nodes_;
    std::vector<InvalidationEdge> edges_;
};

// Whether `change` invalidates `target` under `dag` (default: the Chapter 04
// graph). Encoded reachability used by the negative-guarantee tests.
[[nodiscard]] bool invalidates(
    ProductKind target,
    DependencyKind change,
    const InvalidationDag& dag = InvalidationDag::character_default());

} // namespace carto::character
