#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/surface/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::surface {

enum class SurfaceDependencyEdgeType {
    derived_from,
    uses,
    invalidates,
    invalidated_by,
    depends_on,
    caused_by,
};

[[nodiscard]] const char* surface_dependency_edge_type_name(
    SurfaceDependencyEdgeType type) noexcept;

struct SurfaceStateDelta {
    SurfaceDomainId domain;
    core::Revision previous_revision;
    core::Revision new_revision;
    std::vector<geometry::FaceId> changed_faces;
    std::vector<geometry::EdgeId> changed_edges;
    bool geometry_changed = false;
    bool material_changed = false;
    bool process_changed = false;
    bool semantics_changed = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct SurfaceArtifactId {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const SurfaceArtifactId&) const noexcept = default;
};

struct SurfaceArtifactDependency {
    std::vector<geometry::FaceId> faces;
    std::vector<geometry::EdgeId> edges;
    bool uses_geometry = false;
    bool uses_material = false;
    bool uses_process = false;
    bool uses_semantics = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct SurfaceArtifactNode {
    SurfaceArtifactId id;
    SurfaceDomainId domain;
    core::Revision source_revision;
    SurfaceArtifactDependency dependency;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SurfaceDependencyEdge {
    SurfaceArtifactId upstream;
    SurfaceArtifactId downstream;
    SurfaceDependencyEdgeType type = SurfaceDependencyEdgeType::depends_on;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SurfaceInvalidationReceipt {
    SurfaceStateDelta delta;
    std::vector<SurfaceArtifactId> directly_affected;
    std::vector<SurfaceArtifactId> invalidated;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

class SurfaceInvalidationGraph final {
public:
    static constexpr std::size_t max_nodes = 8192U;
    static constexpr std::size_t max_edges = 16384U;

    [[nodiscard]] core::Result<void> add_node(SurfaceArtifactNode node);
    [[nodiscard]] core::Result<void> add_edge(SurfaceDependencyEdge edge);
    [[nodiscard]] core::Result<SurfaceInvalidationReceipt> invalidate(
        const SurfaceStateDelta& delta) const;
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] const std::vector<SurfaceArtifactNode>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] const std::vector<SurfaceDependencyEdge>& edges() const noexcept {
        return edges_;
    }

private:
    std::vector<SurfaceArtifactNode> nodes_;
    std::vector<SurfaceDependencyEdge> edges_;
};

} // namespace carto::surface
