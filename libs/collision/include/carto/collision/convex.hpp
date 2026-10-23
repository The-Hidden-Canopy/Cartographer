#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/compiled_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::collision {

// Convex proxies remain a roadmap item. Admission is capability-gated: when
// the target cannot realize convex decomposition, the request fails with
// explicit unsupported evidence instead of silently substituting a default
// primitive.
enum class ConvexProxyCapability : std::uint8_t {
    unsupported,
    // A CPU reference implementation exists and may produce conservative
    // evidence. This is never native realization evidence.
    cpu_reference,
};

enum class ConvexProxyStatus : std::uint8_t {
    capability_blocked,
    admitted_reference_evidence,
};

[[nodiscard]] std::string_view convex_proxy_capability_name(
    ConvexProxyCapability capability) noexcept;
[[nodiscard]] std::string_view convex_proxy_status_name(
    ConvexProxyStatus status) noexcept;

// Staged convex proxy record. Hull vertices arrive from an external donor or
// a future compiler stage; this record only validates, measures, and binds
// them. Error terms are conservative reference evidence: the one-sided
// Hausdorff term uses hull vertices, and the volume residual uses the hull
// bounding volume.
struct ConvexProxyCandidate {
    static constexpr std::size_t kMaxHullVertices = 4096U;

    std::string source_identity;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    ConvexProxyStatus status = ConvexProxyStatus::capability_blocked;
    std::vector<core::Vec3d> hull_vertices;
    double one_sided_hausdorff_evidence = 0.0;
    double volume_residual_evidence = 0.0;
    assets::Sha256Digest candidate_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Capability-gated admission. An unsupported capability refuses with
// ErrorCode::unsupported; a zero or oversized hull refuses with
// validation_failed. Nothing here invents hull geometry.
[[nodiscard]] core::Result<ConvexProxyCandidate> admit_convex_proxy_candidate(
    std::string source_identity,
    core::Revision source_revision,
    assets::Sha256Digest source_digest,
    std::vector<core::Vec3d> hull_vertices,
    const geometry::CompiledMesh& mesh,
    ConvexProxyCapability capability);

} // namespace carto::collision
