#include <carto/character/invalidation.hpp>

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_dependency(DependencyKind kind) {
    switch (kind) {
    case DependencyKind::mesh_topology_change:
    case DependencyKind::timeline_edit:
    case DependencyKind::material_edit:
    case DependencyKind::rig_edit:
    case DependencyKind::uv_change:
        return true;
    }
    return false;
}

} // namespace

std::string_view product_kind_name(ProductKind kind) noexcept {
    switch (kind) {
    case ProductKind::source_mesh: return "source-mesh";
    case ProductKind::rest_skeleton: return "rest-skeleton";
    case ProductKind::skin_weights: return "skin-weights";
    case ProductKind::morph_target: return "morph-target";
    case ProductKind::tangent_data: return "tangent-data";
    case ProductKind::collision_proxy: return "collision-proxy";
    case ProductKind::deformation_lod: return "deformation-lod";
    case ProductKind::material_shader: return "material-shader";
    case ProductKind::sampled_animation: return "sampled-animation";
    case ProductKind::runtime_frame: return "runtime-frame";
    case ProductKind::prepared_bundle: return "prepared-bundle";
    }
    return "unknown";
}

std::string_view dependency_kind_name(DependencyKind kind) noexcept {
    switch (kind) {
    case DependencyKind::mesh_topology_change: return "mesh-topology-change";
    case DependencyKind::timeline_edit: return "timeline-edit";
    case DependencyKind::material_edit: return "material-edit";
    case DependencyKind::rig_edit: return "rig-edit";
    case DependencyKind::uv_change: return "uv-change";
    }
    return "unknown";
}

core::Result<void> InvalidationDag::insert_node(InvalidationNode node) {
    if (nodes_.size() >= kMaxNodes) {
        return core::Result<void>::failure(validation(
            "invalidation DAG has reached its bounded node capacity"));
    }
    if (std::any_of(nodes_.begin(), nodes_.end(), [&](const InvalidationNode& existing) {
            return existing.kind == node.kind;
        })) {
        return core::Result<void>::failure(invalid(
            "invalidation DAG node kind is duplicated"));
    }
    nodes_.push_back(node);
    return core::Result<void>::success();
}

core::Result<void> InvalidationDag::insert_edge(InvalidationEdge edge) {
    if (edges_.size() >= kMaxEdges || !valid_dependency(edge.dependency)) {
        return core::Result<void>::failure(validation(
            "invalidation DAG edge is invalid or exceeds its bounded capacity"));
    }
    if (edge.from == edge.to) {
        return core::Result<void>::failure(invalid(
            "invalidation DAG edge must not be self-referential"));
    }
    edges_.push_back(edge);
    return core::Result<void>::success();
}

InvalidationDag InvalidationDag::character_default() {
    // Chapter 04 wiring. An edge (from -dep-> to) reads: a `dep` change on the
    // source behind `from` invalidates `to` (and `from` itself, whose prior
    // derived content is now stale). Propagation follows only edges whose
    // label is among the changed dependency kinds.
    //
    // Encoded guarantees:
    // - mesh topology edits invalidate weights, dependent morphs, tangents,
    //   collision proxies, deformation LODs, and every prepared bundle;
    // - rig edits invalidate the rest skeleton and everything built on it;
    // - UV edits invalidate tangent data;
    // - timeline edits invalidate sampled animation and runtime frame outputs
    //   and NOTHING on the material or skeleton side;
    // - material edits invalidate material/shader product state and NOTHING
    //   on the rest skeleton.
    InvalidationDag dag;
    const auto node = [](ProductKind kind) { return InvalidationNode{kind, {}}; };
    const auto edge = [](ProductKind from, ProductKind to, DependencyKind dependency) {
        return InvalidationEdge{from, to, dependency};
    };
    using P = ProductKind;
    using D = DependencyKind;
    static_cast<void>(dag.insert_node(node(P::source_mesh)));
    static_cast<void>(dag.insert_node(node(P::rest_skeleton)));
    static_cast<void>(dag.insert_node(node(P::skin_weights)));
    static_cast<void>(dag.insert_node(node(P::morph_target)));
    static_cast<void>(dag.insert_node(node(P::tangent_data)));
    static_cast<void>(dag.insert_node(node(P::collision_proxy)));
    static_cast<void>(dag.insert_node(node(P::deformation_lod)));
    static_cast<void>(dag.insert_node(node(P::material_shader)));
    static_cast<void>(dag.insert_node(node(P::sampled_animation)));
    static_cast<void>(dag.insert_node(node(P::runtime_frame)));
    static_cast<void>(dag.insert_node(node(P::prepared_bundle)));

    static_cast<void>(dag.insert_edge(edge(P::source_mesh, P::skin_weights, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::source_mesh, P::morph_target, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::source_mesh, P::tangent_data, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::source_mesh, P::collision_proxy, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::source_mesh, P::deformation_lod, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::skin_weights, P::prepared_bundle, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::morph_target, P::prepared_bundle, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::tangent_data, P::prepared_bundle, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::collision_proxy, P::prepared_bundle, D::mesh_topology_change)));
    static_cast<void>(dag.insert_edge(edge(P::deformation_lod, P::prepared_bundle, D::mesh_topology_change)));

    static_cast<void>(dag.insert_edge(edge(P::rest_skeleton, P::skin_weights, D::rig_edit)));
    static_cast<void>(dag.insert_edge(edge(P::rest_skeleton, P::collision_proxy, D::rig_edit)));
    static_cast<void>(dag.insert_edge(edge(P::rest_skeleton, P::deformation_lod, D::rig_edit)));
    static_cast<void>(dag.insert_edge(edge(P::skin_weights, P::prepared_bundle, D::rig_edit)));
    static_cast<void>(dag.insert_edge(edge(P::collision_proxy, P::prepared_bundle, D::rig_edit)));
    static_cast<void>(dag.insert_edge(edge(P::deformation_lod, P::prepared_bundle, D::rig_edit)));

    static_cast<void>(dag.insert_edge(edge(P::source_mesh, P::tangent_data, D::uv_change)));
    static_cast<void>(dag.insert_edge(edge(P::tangent_data, P::prepared_bundle, D::uv_change)));

    static_cast<void>(dag.insert_edge(edge(P::sampled_animation, P::runtime_frame, D::timeline_edit)));
    static_cast<void>(dag.insert_edge(edge(P::sampled_animation, P::prepared_bundle, D::timeline_edit)));
    static_cast<void>(dag.insert_edge(edge(P::runtime_frame, P::prepared_bundle, D::timeline_edit)));

    static_cast<void>(dag.insert_edge(edge(P::material_shader, P::prepared_bundle, D::material_edit)));
    return dag;
}

core::Result<void> InvalidationDag::validate() const {
    if (nodes_.size() > kMaxNodes || edges_.size() > kMaxEdges) {
        return core::Result<void>::failure(validation(
            "invalidation DAG exceeds its bounded capacity"));
    }
    for (const InvalidationEdge& edge : edges_) {
        if (!valid_dependency(edge.dependency)) {
            return core::Result<void>::failure(invalid(
                "invalidation DAG edge carries an unknown dependency kind"));
        }
        const bool from_known = std::any_of(nodes_.begin(), nodes_.end(),
            [&](const InvalidationNode& node) { return node.kind == edge.from; });
        const bool to_known = std::any_of(nodes_.begin(), nodes_.end(),
            [&](const InvalidationNode& node) { return node.kind == edge.to; });
        if (!from_known || !to_known) {
            return core::Result<void>::failure(validation(
                "invalidation DAG edge references an unknown product kind"));
        }
    }
    return core::Result<void>::success();
}

std::set<ProductKind> InvalidationDag::propagate(
    std::span<const DependencyKind> changed) const {
    std::set<ProductKind> invalidated;
    std::set<DependencyKind> kinds(changed.begin(), changed.end());
    std::map<ProductKind, std::vector<ProductKind>> downstream;
    std::queue<ProductKind> pending;
    for (const InvalidationEdge& edge : edges_) {
        if (kinds.contains(edge.dependency)) {
            downstream[edge.from].push_back(edge.to);
            // The origin's previously derived content is stale as well.
            if (invalidated.insert(edge.from).second) pending.push(edge.from);
        }
    }
    while (!pending.empty()) {
        const ProductKind current = pending.front();
        pending.pop();
        const auto found = downstream.find(current);
        if (found == downstream.end()) continue;
        for (const ProductKind next : found->second) {
            if (invalidated.insert(next).second) pending.push(next);
        }
    }
    return invalidated;
}

bool invalidates(ProductKind target, DependencyKind change, const InvalidationDag& dag) {
    const DependencyKind changes[] = {change};
    return dag.propagate(changes).contains(target);
}

} // namespace carto::character
