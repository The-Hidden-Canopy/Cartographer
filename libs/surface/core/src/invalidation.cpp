#include <carto/surface/invalidation.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <utility>

namespace carto::surface {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic cycle(std::string message) {
    return core::Diagnostic(core::ErrorCode::cycle_detected, std::move(message));
}

bool valid_edge_type(SurfaceDependencyEdgeType type) {
    switch (type) {
    case SurfaceDependencyEdgeType::derived_from:
    case SurfaceDependencyEdgeType::uses:
    case SurfaceDependencyEdgeType::invalidates:
    case SurfaceDependencyEdgeType::invalidated_by:
    case SurfaceDependencyEdgeType::depends_on:
    case SurfaceDependencyEdgeType::caused_by:
        return true;
    }
    return false;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

bool sorted_unique_faces(const std::vector<geometry::FaceId>& faces) {
    return std::all_of(faces.begin(), faces.end(), [](geometry::FaceId id) {
               return static_cast<bool>(id);
           }) &&
        std::is_sorted(faces.begin(), faces.end()) &&
        std::adjacent_find(faces.begin(), faces.end()) == faces.end();
}

bool sorted_unique_edges(const std::vector<geometry::EdgeId>& edges) {
    return std::all_of(edges.begin(), edges.end(), [](geometry::EdgeId id) {
               return static_cast<bool>(id);
           }) &&
        std::is_sorted(edges.begin(), edges.end()) &&
        std::adjacent_find(edges.begin(), edges.end()) == edges.end();
}

bool intersects(const std::vector<geometry::FaceId>& left,
                const std::vector<geometry::FaceId>& right) {
    std::size_t a = 0U;
    std::size_t b = 0U;
    while (a < left.size() && b < right.size()) {
        if (left[a] == right[b]) return true;
        if (left[a] < right[b]) ++a;
        else ++b;
    }
    return false;
}

bool intersects(const std::vector<geometry::EdgeId>& left,
                const std::vector<geometry::EdgeId>& right) {
    std::size_t a = 0U;
    std::size_t b = 0U;
    while (a < left.size() && b < right.size()) {
        if (left[a] == right[b]) return true;
        if (left[a] < right[b]) ++a;
        else ++b;
    }
    return false;
}

bool directly_affected(const SurfaceArtifactNode& node, const SurfaceStateDelta& delta) {
    if (node.domain != delta.domain || node.source_revision != delta.previous_revision) return false;
    const auto& dependency = node.dependency;
    if (delta.geometry_changed && dependency.uses_geometry) {
        const bool global_geometry = dependency.faces.empty() && dependency.edges.empty();
        const bool unknown_footprint = delta.changed_faces.empty() && delta.changed_edges.empty();
        const bool face_match = !dependency.faces.empty() && !delta.changed_faces.empty() &&
            intersects(dependency.faces, delta.changed_faces);
        const bool edge_match = !dependency.edges.empty() && !delta.changed_edges.empty() &&
            intersects(dependency.edges, delta.changed_edges);
        if (global_geometry || unknown_footprint || face_match || edge_match) return true;
    }
    return (delta.material_changed && dependency.uses_material) ||
        (delta.process_changed && dependency.uses_process) ||
        (delta.semantics_changed && dependency.uses_semantics);
}

} // namespace

const char* surface_dependency_edge_type_name(SurfaceDependencyEdgeType type) noexcept {
    switch (type) {
    case SurfaceDependencyEdgeType::derived_from: return "derived-from";
    case SurfaceDependencyEdgeType::uses: return "uses";
    case SurfaceDependencyEdgeType::invalidates: return "invalidates";
    case SurfaceDependencyEdgeType::invalidated_by: return "invalidated-by";
    case SurfaceDependencyEdgeType::depends_on: return "depends-on";
    case SurfaceDependencyEdgeType::caused_by: return "caused-by";
    }
    return "unknown";
}

core::Result<void> SurfaceStateDelta::validate() const {
    if (!domain || previous_revision.exhausted() || new_revision.exhausted() ||
        new_revision <= previous_revision || (!geometry_changed && !material_changed &&
                                              !process_changed && !semantics_changed)) {
        return core::Result<void>::failure(invalid("surface state delta is invalid"));
    }
    if (!sorted_unique_faces(changed_faces) || !sorted_unique_edges(changed_edges) ||
        (!changed_faces.empty() && !geometry_changed) ||
        (!changed_edges.empty() && !geometry_changed)) {
        return core::Result<void>::failure(
            validation("surface state delta geometry footprint is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceStateDelta::canonical() const {
    std::ostringstream output;
    output << "surface-state-delta-v1;domain=" << domain.value
           << ";previous=" << previous_revision.value()
           << ";new=" << new_revision.value()
           << ";geometry=" << geometry_changed
           << ";material=" << material_changed
           << ";process=" << process_changed
           << ";semantics=" << semantics_changed
           << ";faces=";
    for (const geometry::FaceId face : changed_faces) output << face.value << ',';
    output << ";edges=";
    for (const geometry::EdgeId edge : changed_edges) output << edge.value << ',';
    return output.str();
}

core::Result<void> SurfaceArtifactDependency::validate() const {
    if (!sorted_unique_faces(faces) || !sorted_unique_edges(edges) ||
        (!faces.empty() && !uses_geometry) || (!edges.empty() && !uses_geometry) ||
        (!uses_geometry && !uses_material && !uses_process && !uses_semantics)) {
        return core::Result<void>::failure(validation("surface artifact dependency is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceArtifactDependency::canonical() const {
    std::ostringstream output;
    output << "geometry=" << uses_geometry
           << ";material=" << uses_material
           << ";process=" << uses_process
           << ";semantics=" << uses_semantics
           << ";faces=";
    for (const geometry::FaceId face : faces) output << face.value << ',';
    output << ";edges=";
    for (const geometry::EdgeId edge : edges) output << edge.value << ',';
    return output.str();
}

core::Result<void> SurfaceArtifactNode::validate() const {
    if (!id || !domain || source_revision.exhausted()) {
        return core::Result<void>::failure(invalid("surface artifact node identity is invalid"));
    }
    return dependency.validate();
}

core::Result<void> SurfaceDependencyEdge::validate() const {
    if (!upstream || !downstream || upstream == downstream || !valid_edge_type(type)) {
        return core::Result<void>::failure(invalid("surface dependency edge identity is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceInvalidationReceipt::validate() const {
    if (auto result = delta.validate(); !result) return result;
    if (!std::is_sorted(directly_affected.begin(), directly_affected.end()) ||
        !std::is_sorted(invalidated.begin(), invalidated.end()) ||
        std::adjacent_find(directly_affected.begin(), directly_affected.end()) !=
            directly_affected.end() ||
            std::adjacent_find(invalidated.begin(), invalidated.end()) != invalidated.end()) {
        return core::Result<void>::failure(validation("surface invalidation receipt is not canonical"));
    }
    if (!std::all_of(invalidated.begin(), invalidated.end(), [](SurfaceArtifactId id) {
            return static_cast<bool>(id);
        })) {
        return core::Result<void>::failure(
            validation("surface invalidation receipt contains a zero artifact ID"));
    }
    for (const SurfaceArtifactId id : directly_affected) {
        if (!id || !std::binary_search(invalidated.begin(), invalidated.end(), id)) {
            return core::Result<void>::failure(
                validation("surface invalidation receipt direct set is inconsistent"));
        }
    }
    if (!assets::Sha256Digest::from_hex(digest) ||
        digest != digest_text(canonical_without_digest()).hex()) {
        return core::Result<void>::failure(validation("surface invalidation receipt digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceInvalidationReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << delta.canonical() << ";direct=";
    for (const SurfaceArtifactId id : directly_affected) output << id.value << ',';
    output << ";invalidated=";
    for (const SurfaceArtifactId id : invalidated) output << id.value << ',';
    return output.str();
}

core::Result<void> SurfaceInvalidationReceipt::refresh_digest() {
    if (auto result = delta.validate(); !result) return result;
    digest = digest_text(canonical_without_digest()).hex();
    return core::Result<void>::success();
}

core::Result<void> SurfaceInvalidationGraph::add_node(SurfaceArtifactNode node) {
    if (nodes_.size() >= max_nodes) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                             "surface invalidation graph reached its node limit"));
    }
    if (auto result = node.validate(); !result) return result;
    if (std::any_of(nodes_.begin(), nodes_.end(), [&node](const auto& current) {
            return current.id == node.id;
        })) {
        return core::Result<void>::failure(invalid("surface invalidation node ID is duplicated"));
    }
    nodes_.push_back(std::move(node));
    return core::Result<void>::success();
}

core::Result<void> SurfaceInvalidationGraph::add_edge(SurfaceDependencyEdge edge) {
    if (edges_.size() >= max_edges) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                             "surface invalidation graph reached its edge limit"));
    }
    if (auto result = edge.validate(); !result) return result;
    const auto has_node = [this](SurfaceArtifactId id) {
        return std::any_of(nodes_.begin(), nodes_.end(), [id](const auto& node) {
            return node.id == id;
        });
    };
    if (!has_node(edge.upstream) || !has_node(edge.downstream)) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                             "surface invalidation edge references an unknown node"));
    }
    if (std::any_of(edges_.begin(), edges_.end(), [&edge](const auto& current) {
            return current.upstream == edge.upstream && current.downstream == edge.downstream &&
                current.type == edge.type;
        })) {
        return core::Result<void>::failure(invalid("surface invalidation edge is duplicated"));
    }
    edges_.push_back(std::move(edge));
    return core::Result<void>::success();
}

core::Result<void> SurfaceInvalidationGraph::validate() const {
    if (nodes_.size() > max_nodes || edges_.size() > max_edges) {
        return core::Result<void>::failure(validation("surface invalidation graph exceeds its limits"));
    }
    std::set<SurfaceArtifactId> ids;
    for (const auto& node : nodes_) {
        if (auto result = node.validate(); !result) return result;
        if (!ids.insert(node.id).second) {
            return core::Result<void>::failure(invalid("surface invalidation node ID is duplicated"));
        }
    }
    for (const auto& edge : edges_) {
        if (auto result = edge.validate(); !result) return result;
        if (!ids.contains(edge.upstream) || !ids.contains(edge.downstream)) {
            return core::Result<void>::failure(
                core::Diagnostic(core::ErrorCode::not_found,
                                 "surface invalidation edge references an unknown node"));
        }
    }

    std::map<SurfaceArtifactId, std::vector<SurfaceArtifactId>> adjacency;
    for (const auto& edge : edges_) {
        if (edge.type == SurfaceDependencyEdgeType::invalidated_by) {
            adjacency[edge.downstream].push_back(edge.upstream);
        } else {
            adjacency[edge.upstream].push_back(edge.downstream);
        }
    }
    std::set<SurfaceArtifactId> visiting;
    std::set<SurfaceArtifactId> visited;
    const auto visit = [&](auto&& self, SurfaceArtifactId id) -> core::Result<void> {
        if (visiting.contains(id)) {
            return core::Result<void>::failure(cycle("surface invalidation graph contains a cycle"));
        }
        if (!visited.insert(id).second) return core::Result<void>::success();
        visiting.insert(id);
        for (const SurfaceArtifactId child : adjacency[id]) {
            if (auto result = self(self, child); !result) return result;
        }
        visiting.erase(id);
        return core::Result<void>::success();
    };
    for (const SurfaceArtifactId id : ids) {
        if (auto result = visit(visit, id); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<SurfaceInvalidationReceipt> SurfaceInvalidationGraph::invalidate(
    const SurfaceStateDelta& delta) const {
    if (auto result = validate(); !result) {
        return core::Result<SurfaceInvalidationReceipt>::failure(result.error());
    }
    if (auto result = delta.validate(); !result) {
        return core::Result<SurfaceInvalidationReceipt>::failure(result.error());
    }

    std::set<SurfaceArtifactId> affected;
    for (const auto& node : nodes_) {
        if (directly_affected(node, delta)) affected.insert(node.id);
    }
    std::set<SurfaceArtifactId> direct(affected.begin(), affected.end());
    std::set<SurfaceArtifactId> frontier(affected.begin(), affected.end());
    while (!frontier.empty()) {
        const SurfaceArtifactId current = *frontier.begin();
        frontier.erase(frontier.begin());
        for (const auto& edge : edges_) {
            if (edge.type == SurfaceDependencyEdgeType::invalidated_by) {
                if (edge.downstream == current && affected.insert(edge.upstream).second) {
                    frontier.insert(edge.upstream);
                }
            } else if (edge.upstream == current && affected.insert(edge.downstream).second) {
                frontier.insert(edge.downstream);
            }
        }
    }

    SurfaceInvalidationReceipt receipt;
    receipt.delta = delta;
    receipt.directly_affected.assign(direct.begin(), direct.end());
    receipt.invalidated.assign(affected.begin(), affected.end());
    if (auto result = receipt.refresh_digest(); !result) {
        return core::Result<SurfaceInvalidationReceipt>::failure(result.error());
    }
    return core::Result<SurfaceInvalidationReceipt>::success(std::move(receipt));
}

} // namespace carto::surface
