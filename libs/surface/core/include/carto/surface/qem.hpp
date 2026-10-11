#pragma once

#include <carto/surface/core.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::surface {

struct SurfaceQemOptions {
    std::uint32_t max_source_triangles = 4096U;
    std::uint32_t target_triangles = 1U;
    std::uint32_t max_collapses = 256U;
    double minimum_triangle_area = 1e-12;
    bool preserve_boundary = true;

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
    std::uint32_t source_triangles = 0U;
    std::uint32_t target_triangles = 0U;
    bool target_reached = false;
    std::vector<core::Vec3d> positions;
    std::vector<core::Vec3d> normals;
    std::vector<std::uint32_t> indices;
    std::vector<geometry::VertexId> vertex_ids;
    std::vector<geometry::FaceId> triangle_faces;
    std::vector<SurfaceQemCollapse> collapses;
    std::string algorithm_version = "cartographer-surface-qem/0.1";
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

} // namespace carto::surface
