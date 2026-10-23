#pragma once

#include <carto/surface/core.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::surface {

enum class RepresentationPolicyField : std::uint8_t {
    view = 1U,
    material = 2U,
    constraint = 3U,
    fracture = 4U,
};

struct RepresentationInheritanceRecord {
    RepresentationId representation;
    RepresentationId parent;
    std::vector<RepresentationPolicyField> inherited_fields;
    std::vector<RepresentationPolicyField> override_fields;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<RepresentationInheritanceRecord> parse(
        std::string_view bytes);
};

struct RepresentationViewPolicyBinding {
    RepresentationId representation;
    std::optional<SurfaceQualityPolicy> override_policy;
};

struct RepresentationViewPolicyOutcome {
    RepresentationId representation;
    std::string representation_digest;
    std::string effective_policy_digest;
    std::string inheritance_digest;
    std::string digest;
    bool accepted = false;
};

[[nodiscard]] core::Result<RepresentationViewPolicyOutcome> evaluate_inherited_view_policy(
    const SurfaceRepresentationTree& tree,
    const std::vector<RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationViewPolicyBinding>& bindings,
    RepresentationId representation);

enum class SurfaceConstraintMetric : std::uint8_t {
    max_position_error = 1U,
    max_normal_error = 2U,
    max_silhouette_error = 3U,
    rms_radiance_error = 4U,
    max_radiance_error = 5U,
    diffuse_error = 6U,
    specular_error = 7U,
    visibility_error = 8U,
    energy_error = 9U,
};

struct SurfaceRepresentationConstraint {
    std::uint64_t id = 0U;
    SurfaceConstraintMetric metric = SurfaceConstraintMetric::max_position_error;
    double maximum = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    friend constexpr bool operator==(const SurfaceRepresentationConstraint&,
                                     const SurfaceRepresentationConstraint&) = default;
};

struct RepresentationConstraintBinding {
    RepresentationId representation;
    std::vector<SurfaceRepresentationConstraint> constraints;
};

struct RepresentationConstraintPolicyOutcome {
    RepresentationId representation;
    std::string representation_digest;
    std::string effective_constraints_digest;
    std::string inheritance_digest;
    std::string digest;
    std::vector<std::uint64_t> violated_constraint_ids;
    bool accepted = false;
};

[[nodiscard]] core::Result<RepresentationConstraintPolicyOutcome>
evaluate_inherited_constraint_policy(
    const SurfaceRepresentationTree& tree,
    const std::vector<RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationConstraintBinding>& bindings,
    RepresentationId representation);

enum class SurfaceFractureMode : std::uint8_t {
    balanced_face_id_order = 1U,
};

struct RepresentationFracturePolicyBinding {
    RepresentationId representation;
    SurfaceFractureMode mode = SurfaceFractureMode::balanced_face_id_order;
    std::uint32_t shard_count = 0U;
};

struct SurfaceFractureShard {
    std::string stable_id;
    std::vector<geometry::FaceId> source_faces;
};

struct RepresentationFracturePolicyOutcome {
    RepresentationId representation;
    std::string representation_digest;
    std::string effective_policy_digest;
    std::string inheritance_digest;
    std::string digest;
    std::vector<SurfaceFractureShard> shards;
};

struct SurfaceFractureGeometryShard {
    std::string stable_id;
    geometry::CompiledMesh mesh;
};

struct SurfaceFractureGeometry {
    RepresentationId representation;
    std::string source_mesh_digest;
    std::string policy_outcome_digest;
    std::string digest;
    std::vector<SurfaceFractureGeometryShard> shards;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source,
        const RepresentationFracturePolicyOutcome& outcome) const;
};

[[nodiscard]] core::Result<RepresentationFracturePolicyOutcome>
evaluate_inherited_fracture_policy(
    const SurfaceRepresentationTree& tree,
    const std::vector<RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationFracturePolicyBinding>& bindings,
    const std::vector<geometry::FaceId>& source_faces,
    RepresentationId representation);

// Materializes a face-partition plan as deterministic mesh shards. Faces are
// assigned whole; this does not split triangles or create fracture surfaces.
[[nodiscard]] core::Result<SurfaceFractureGeometry> materialize_fracture_geometry(
    const geometry::CompiledMesh& source,
    const RepresentationFracturePolicyOutcome& outcome);

} // namespace carto::surface
