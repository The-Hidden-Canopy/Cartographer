#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/collision/primitive.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::collision {

// Measured approximation error between a collision proxy and its reference.
// Every term is non-negative and measured in mesh-local units. A zero record
// means the proxy is exact under every measured term.
struct CollisionErrorMetrics {
    double volume_overlap_residual = 0.0;
    double max_penetration_depth = 0.0;
    double one_sided_hausdorff = 0.0;
    double clearance_violation = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const CollisionErrorMetrics&) const noexcept = default;
};

// Public threshold contract for admitting a collision candidate. Mirrors the
// production AdmissionPolicy idiom: policy internals stay opaque, the exact
// public threshold contract is bound by digest.
struct CollisionErrorThresholdPolicy {
    std::optional<double> max_volume_overlap_residual;
    std::optional<double> max_penetration_depth;
    std::optional<double> max_one_sided_hausdorff;
    std::optional<double> max_clearance_violation;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Refuses with validation_failed when any measured term exceeds its bound.
[[nodiscard]] core::Result<void> evaluate_collision_error(
    const CollisionErrorMetrics& metrics,
    const CollisionErrorThresholdPolicy& policy);

// Shared deterministic geometric evidence helpers. Mesh volume uses the
// signed tetrahedron sum and is exact for consistently wound closed meshes.
[[nodiscard]] core::Result<double> mesh_volume(const geometry::CompiledMesh& mesh);
[[nodiscard]] double one_sided_hausdorff(
    std::span<const core::Vec3d> from,
    std::span<const core::Vec3d> to);

// Exact analytic overlap volume for two spheres.
[[nodiscard]] double sphere_sphere_overlap_volume(
    core::Vec3d first_center,
    double first_radius,
    core::Vec3d second_center,
    double second_radius);

// Exact analytic overlap volume for two axis-aligned boxes given centers and
// half extents.
[[nodiscard]] double box_box_overlap_volume(
    core::Vec3d first_center,
    core::Vec3d first_half_extents,
    core::Vec3d second_center,
    core::Vec3d second_half_extents);

// Exact analytic overlap volume between a sphere and an axis-aligned box.
[[nodiscard]] double sphere_box_overlap_volume(
    core::Vec3d sphere_center,
    double sphere_radius,
    core::Vec3d box_center,
    core::Vec3d box_half_extents);

// Pairwise proxy overlap volume. Sphere/box combinations are exact; capsule
// and cylinder kinds contribute their conservative axis-aligned bounding
// volume so the term never silently under-reports.
[[nodiscard]] core::Result<double> primitive_pair_volume_overlap(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second);

// Pairwise maximum penetration depth. Sphere-sphere and box-box (minimal
// translation depth over the separating axes) are exact; sphere-box uses the
// sphere center to box surface distance; capsule and cylinder kinds fall back
// to their conservative bounding boxes.
[[nodiscard]] core::Result<double> primitive_pair_max_penetration(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second);

// Bounding-sphere separation between two proxies, reduced by a required
// clearance. Never negative; a zero result means clearance is satisfied.
[[nodiscard]] double clearance_violation(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second,
    double required_clearance);

// Fills every measured term for a proxy pair. The required clearance may be
// zero when the profile does not demand separation.
[[nodiscard]] core::Result<CollisionErrorMetrics> compute_pair_metrics(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second,
    double required_clearance);

} // namespace carto::collision
