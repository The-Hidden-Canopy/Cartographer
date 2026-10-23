#pragma once

#include <carto/surface/core.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::geometry {
class EditableMesh;
}

namespace carto::surface {

// The caller identifies source edges whose lineage must survive. QEM locks
// both endpoints conservatively, so the edge cannot be collapsed away.
enum class SurfaceQemBoundaryPolicy : std::uint8_t {
    material,
    constraint,
    fracture,
};

struct SurfaceQemProtectedEdge {
    geometry::EdgeId edge;
    SurfaceQemBoundaryPolicy policy = SurfaceQemBoundaryPolicy::constraint;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] auto operator<=>(const SurfaceQemProtectedEdge&) const noexcept = default;
};

// Names an authored attribute layer used to infer protected QEM boundaries.
// Face-domain layers protect transitions between distinct values. Edge-domain
// boolean/integer layers protect nonzero edges. The selected source domain and
// layer values are resolved from an EditableMesh, never guessed from geometry.
struct SurfaceQemBoundaryAttributeRule {
    std::string layer_id;
    SurfaceQemBoundaryPolicy policy = SurfaceQemBoundaryPolicy::material;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] auto operator<=>(const SurfaceQemBoundaryAttributeRule&) const noexcept = default;
};

struct SurfaceQemOptions {
    std::uint32_t max_source_triangles = 4096U;
    std::uint32_t target_triangles = 1U;
    std::uint32_t max_collapses = 256U;
    double minimum_triangle_area = 1e-12;
    bool preserve_boundary = true;
    std::vector<SurfaceQemProtectedEdge> protected_edges;
    std::vector<SurfaceQemBoundaryAttributeRule> boundary_attribute_rules;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceQemCollapse {
    geometry::VertexId kept_vertex;
    geometry::VertexId removed_vertex;
    core::Vec3d proposed_position;
    double cost = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SurfaceQemResult {
    SurfaceDomainId domain;
    core::Revision source_revision;
    std::string source_content_digest;
    std::string options_digest;
    std::uint32_t source_triangles = 0U;
    std::uint32_t target_triangles = 0U;
    bool target_reached = false;
    std::vector<core::Vec3d> positions;
    std::vector<core::Vec3d> normals;
    std::vector<std::uint32_t> indices;
    std::vector<geometry::VertexId> vertex_ids;
    std::vector<geometry::FaceId> triangle_faces;
    std::vector<SurfaceQemProtectedEdge> protected_edges;
    std::vector<SurfaceQemCollapse> collapses;
    std::string algorithm_version = "cartographer-surface-qem/0.2";
    std::string digest;

    [[nodiscard]] std::size_t triangle_count() const noexcept {
        return indices.size() / 3U;
    }
    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source,
        const SurfaceDomain& source_domain) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Build a bounded, read-only constrained QEM candidate.  The result is a
// candidate surface and receipt; it does not mutate the authored mesh or admit
// the result into a runtime representation tree.
[[nodiscard]] core::Result<SurfaceQemResult> build_constrained_qem(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceQemOptions& options);

// Attribute-driven boundary inference requires the editable source so stable
// face/edge attribute order can be resolved. The compiled-mesh overload refuses
// options with boundary_attribute_rules rather than silently ignoring them.
[[nodiscard]] core::Result<SurfaceQemResult> build_constrained_qem(
    const geometry::EditableMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceQemOptions& options);

} // namespace carto::surface
