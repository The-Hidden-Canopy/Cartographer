#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/web_geometry/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::collision {

// Exact triangle collision candidate bound to a web_geometry cluster
// hierarchy donor. The candidate references the donor hierarchy by digest and
// lists the runtime page identities the consumer must realize; it never
// copies triangle payload. Terrain-style donors produce this evidence today;
// the binding stays neutral and versioned.
struct TriangleCollisionCandidate {
    static constexpr std::size_t kMaxBoundPages = 4096U;

    std::string source_identity;
    core::Revision source_revision;
    assets::Sha256Digest hierarchy_digest;
    std::vector<web_geometry::RuntimePageId> bound_pages;
    std::uint64_t triangle_count = 0U;
    double geometric_error_evidence = 0.0;
    assets::Sha256Digest candidate_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] bool operator==(
        const TriangleCollisionCandidate&) const noexcept = default;
};

// Binds a triangle candidate to every cluster page the donor hierarchy
// already assigned. The hierarchy is validated first; unassigned or empty
// hierarchies fail closed. Bound pages are sorted and unique so the canonical
// form is unambiguous.
[[nodiscard]] core::Result<TriangleCollisionCandidate>
bind_triangle_collision_candidate(
    std::string source_identity,
    core::Revision source_revision,
    const web_geometry::ClusterHierarchy& hierarchy);

} // namespace carto::collision
