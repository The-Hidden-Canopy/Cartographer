#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::collision {

// Neutral primitive collision representation kinds. Candidates are fits over
// authoring geometry only; they carry no runtime handle and no engine shape.
enum class PrimitiveCollisionKind : std::uint8_t {
    sphere,
    box,
    capsule,
    cylinder,
};

[[nodiscard]] std::string_view primitive_collision_kind_name(
    PrimitiveCollisionKind kind) noexcept;

// Fit error evidence for one primitive candidate. Both terms are measured in
// mesh-local units: maximum unsigned distance from any source vertex to
// outside the primitive, plus the absolute volume residual between the source
// mesh volume and the primitive volume.
struct CollisionFitError {
    double max_outside_distance = 0.0;
    double volume_residual = 0.0;

    [[nodiscard]] double aggregate() const noexcept {
        return max_outside_distance + volume_residual;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const CollisionFitError&) const noexcept = default;
};

// One deterministic primitive fit of a compiled authoring mesh. All
// parameters live in mesh-local coordinates. Unused parameters are exactly
// zero for the selected kind. The record is digest-bound: mutate-and-refresh
// is required after any field change, mirroring the production receipt idiom.
struct PrimitiveCollisionCandidate {
    PrimitiveCollisionKind kind = PrimitiveCollisionKind::sphere;
    core::Vec3d center{};
    core::Vec3d half_extents{};
    double radius = 0.0;
    double half_height = 0.0;
    CollisionFitError fit;
    std::uint64_t fitted_vertex_count = 0U;
    assets::Sha256Digest candidate_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct PrimitiveFitOptions {
    // Deterministic generation order; kinds absent from this list are skipped.
    std::vector<PrimitiveCollisionKind> permitted_kinds{
        PrimitiveCollisionKind::sphere,
        PrimitiveCollisionKind::box,
        PrimitiveCollisionKind::capsule,
        PrimitiveCollisionKind::cylinder,
    };

    [[nodiscard]] core::Result<void> validate() const;
};

[[nodiscard]] core::Result<PrimitiveCollisionCandidate> fit_sphere_candidate(
    const geometry::CompiledMesh& mesh);
[[nodiscard]] core::Result<PrimitiveCollisionCandidate> fit_box_candidate(
    const geometry::CompiledMesh& mesh);
[[nodiscard]] core::Result<PrimitiveCollisionCandidate> fit_capsule_candidate(
    const geometry::CompiledMesh& mesh);
[[nodiscard]] core::Result<PrimitiveCollisionCandidate> fit_cylinder_candidate(
    const geometry::CompiledMesh& mesh);

// Generates every permitted primitive candidate in a fixed order with fixed
// formulas, so identical source meshes and options always produce identical
// candidate digests. Fails closed on invalid meshes or empty permissions.
[[nodiscard]] core::Result<std::vector<PrimitiveCollisionCandidate>>
generate_primitive_candidates(
    const geometry::CompiledMesh& mesh,
    const PrimitiveFitOptions& options = {});

} // namespace carto::collision
