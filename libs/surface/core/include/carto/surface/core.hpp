#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/compiled_mesh.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::surface {

struct SurfaceDomainId {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const SurfaceDomainId&) const noexcept = default;
};

struct RepresentationId {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const RepresentationId&) const noexcept = default;
};

enum class SurfaceRepresentationKind {
    authored,
    dense_geometry,
    simplified_geometry,
    displacement_surface,
    normal_distribution,
    closure_proxy,
    collision_proxy,
    runtime_cluster,
};

enum class RepresentationMode {
    geometry_only,
    fidelity_closure,
};

enum class MigrationTarget {
    displacement,
    normal_field,
    ndf,
    roughness,
    anisotropy,
    cavity,
    directional_visibility,
    silhouette_descriptor,
};

enum class EdgeClass {
    flat,
    soft_convex,
    hard_convex,
    soft_concave,
    hard_concave,
    boundary,
    non_manifold,
};

struct SurfaceDomain {
    SurfaceDomainId id;
    std::vector<geometry::FaceId> source_faces;
    core::Revision source_revision;
    std::string semantic_class;
    std::string material_class;
    std::string source_content_digest;
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& mesh) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest(
        const geometry::CompiledMesh& mesh);
};

[[nodiscard]] core::Result<SurfaceDomain> build_surface_domain(
    const geometry::CompiledMesh& mesh,
    SurfaceDomainId id,
    std::string semantic_class = {},
    std::string material_class = {});

struct SurfacePatch {
    SurfaceDomainId domain;
    std::vector<geometry::FaceId> faces;
    core::Bounds3d bounds;
    double area = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct CurvatureStats {
    double mean_curvature = 0.0;
    double gaussian_curvature = 0.0;
    double maximum_abs_curvature = 0.0;
    core::Vec3d principal_direction{1.0, 0.0, 0.0};

    [[nodiscard]] core::Result<void> validate() const;
};

struct EdgeStats {
    std::uint32_t flat = 0U;
    std::uint32_t soft_convex = 0U;
    std::uint32_t hard_convex = 0U;
    std::uint32_t soft_concave = 0U;
    std::uint32_t hard_concave = 0U;
    std::uint32_t boundary = 0U;
    std::uint32_t non_manifold = 0U;

    [[nodiscard]] std::uint64_t total() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
};

struct ThicknessStats {
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double confidence = 0.0;
    std::uint32_t measured_samples = 0U;
    bool ambiguous = false;

    [[nodiscard]] core::Result<void> validate() const;
};

struct NdfClosure {
    core::Vec3d reference_normal{0.0, 0.0, 1.0};
    double alpha_x = 0.001;
    double alpha_y = 0.001;
    double orientation = 0.0;
    double fit_error = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct VisibilityClosure {
    static constexpr std::size_t directional_bin_count = 8U;

    core::Vec3d reference_normal{0.0, 0.0, 1.0};
    core::Vec3d bent_normal{0.0, 0.0, 1.0};
    double ambient_visibility = 1.0;
    double cavity = 0.0;
    std::array<float, directional_bin_count> directional_visibility{};
    double silhouette_density = 0.0;
    double rms_error = 0.0;
    double maximum_error = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SurfaceAnalysisOptions {
    std::uint32_t max_source_triangles = 4096U;
    std::uint32_t visibility_sample_count = 32U;
    double thickness_max_distance = 100.0;
    double soft_edge_angle = 0.35;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceAnalysisRecord {
    SurfaceDomain domain;
    SurfacePatch patch;
    core::Vec3d mean_normal{0.0, 0.0, 1.0};
    CurvatureStats curvature;
    ThicknessStats thickness;
    EdgeStats edges;
    NdfClosure ndf;
    VisibilityClosure visibility;
    std::string algorithm_version = "cartographer-surface-analysis/0.1";
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& mesh) const;
    [[nodiscard]] std::string canonical() const;
};

[[nodiscard]] core::Result<SurfaceAnalysisRecord> analyze_surface(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceAnalysisOptions& options = {});

struct SurfaceBand {
    double min_frequency = 0.0;
    double max_frequency = 0.0;
    double rms_displacement = 0.0;
    double rms_normal_change = 0.0;
    double silhouette_contribution = 0.0;
    std::uint32_t triangle_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FrequencyDecompositionOptions {
    double feature_scale = 1.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

[[nodiscard]] core::Result<std::vector<SurfaceBand>> decompose_frequency(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const FrequencyDecompositionOptions& options = {});

struct GeometryProxy {
    std::vector<core::Vec3d> positions;
    std::vector<core::Vec3d> normals;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> source_triangles;

    [[nodiscard]] std::size_t triangle_count() const noexcept {
        return indices.size() / 3U;
    }
    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source) const;
};

[[nodiscard]] core::Result<GeometryProxy> build_geometry_proxy(
    const geometry::CompiledMesh& mesh,
    std::uint32_t target_triangles);

struct SurfaceErrorEnvelope {
    double max_position_error = 0.0;
    double max_normal_error = 0.0;
    double max_silhouette_error = 0.0;
    double rms_radiance_error = 0.0;
    double max_radiance_error = 0.0;
    double diffuse_error = 0.0;
    double specular_error = 0.0;
    double visibility_error = 0.0;
    double energy_error = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SurfaceQualityPolicy {
    double max_rms_radiance_error = 0.15;
    double max_radiance_error = 0.5;
    double max_diffuse_error = 0.25;
    double max_specular_error = 0.25;
    double max_visibility_error = 0.25;
    double max_silhouette_error = 1.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct RadiometricSample {
    core::Vec3d view_direction{0.0, 0.0, 1.0};
    core::Vec3d light_direction{0.0, 0.0, 1.0};

    [[nodiscard]] core::Result<void> validate() const;
};

struct RadiometricEvaluationOptions {
    std::uint32_t view_count = 8U;
    std::uint32_t light_count = 8U;
    double specular_weight = 1.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct RadiometricEvaluation {
    SurfaceErrorEnvelope envelope;
    std::uint32_t sample_count = 0U;
    std::string policy_digest;
    std::string source_digest;
    std::string candidate_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SurfaceClosureCompileOptions {
    std::uint32_t target_triangles = 1000U;
    SurfaceAnalysisOptions analysis{};
    RadiometricEvaluationOptions evaluation{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct SurfaceClosurePolygon {
    SurfaceDomainId domain;
    RepresentationId representation;
    SurfaceRepresentationKind kind = SurfaceRepresentationKind::closure_proxy;
    RepresentationMode mode = RepresentationMode::fidelity_closure;
    GeometryProxy geometry;
    NdfClosure ndf;
    VisibilityClosure visibility;
    SurfaceErrorEnvelope error;
    std::vector<geometry::FaceId> source_faces;
    core::Revision source_revision;
    std::string algorithm_version = "cartographer-surface-closure/0.1";
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source,
        const SurfaceDomain& source_domain) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct RepresentationMigration {
    SurfaceDomainId domain;
    RepresentationId source_representation;
    RepresentationId target_representation;
    std::vector<MigrationTarget> targets;
    double residual_radiometric_error = 0.0;
    core::Revision source_revision;
    std::string algorithm_version = "cartographer-surface-migration/0.1";
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

[[nodiscard]] core::Result<SurfaceClosurePolygon> compile_surface_representation(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceClosureCompileOptions& options = {},
    RepresentationMode mode = RepresentationMode::fidelity_closure);

[[nodiscard]] core::Result<RadiometricEvaluation> evaluate_radiometric_error(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceClosurePolygon& candidate,
    const RadiometricEvaluationOptions& options = {});

[[nodiscard]] core::Result<void> admit_surface_representation(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceClosurePolygon& candidate,
    const SurfaceQualityPolicy& policy = {});

struct SurfaceRepresentationNode {
    RepresentationId id;
    SurfaceDomainId domain;
    std::optional<RepresentationId> parent;
    std::vector<RepresentationId> children;
    SurfaceRepresentationKind kind = SurfaceRepresentationKind::authored;
    SurfaceErrorEnvelope error;
    std::string content_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

class SurfaceRepresentationTree {
public:
    [[nodiscard]] core::Result<void> add(SurfaceRepresentationNode node);
    [[nodiscard]] core::Result<void> validate(SurfaceDomainId domain) const;
    [[nodiscard]] const std::vector<SurfaceRepresentationNode>& nodes() const noexcept {
        return nodes_;
    }

private:
    std::vector<SurfaceRepresentationNode> nodes_;
};

// Camera-relative inputs are intentionally explicit: minimum_view_depth and
// maximum_projected_radius_pixels are conservative bounds supplied by the
// caller from the active camera and source bounds. Selection projects the
// existing error envelope as an estimate and applies the appearance policy;
// it does not mutate the tree or admit a runtime representation.
struct SurfaceViewSelectionOptions {
    double minimum_view_depth = 1.0;
    double maximum_projected_radius_pixels = 0.0;
    double vertical_fov_radians = 1.0471975511965976;
    std::uint32_t viewport_height_pixels = 1080U;
    double maximum_estimated_position_error_pixels = 1.0;
    SurfaceQualityPolicy quality_policy{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceViewSelection {
    SurfaceDomainId domain;
    RepresentationId representation;
    core::Revision source_revision;
    std::string domain_digest;
    std::string representation_digest;
    std::string candidate_set_digest;
    std::string options_digest;
    std::uint32_t triangle_count = 0U;
    double estimated_projected_position_error_pixels = 0.0;
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source,
        const SurfaceDomain& source_domain,
        const SurfaceRepresentationTree& tree,
        const std::vector<SurfaceClosurePolygon>& candidates,
        const SurfaceViewSelectionOptions& options) const;
    [[nodiscard]] std::string canonical() const;
};

[[nodiscard]] core::Result<SurfaceViewSelection> select_surface_representation_for_view(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceRepresentationTree& tree,
    const std::vector<SurfaceClosurePolygon>& candidates,
    const SurfaceViewSelectionOptions& options = {});

struct SurfaceRuntimeRecord {
    SurfaceDomainId domain;
    RepresentationId representation;
    double geometric_error = 0.0;
    double radiometric_error = 0.0;
    std::uint64_t continuity_id = 0U;
    std::uint32_t material_group = 0U;
    std::uint32_t semantic_group = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FidelityPackage {
    static constexpr std::uint32_t current_schema_version = 2U;

    std::uint32_t schema_version = current_schema_version;
    core::Revision source_revision;
    std::vector<SurfaceDomain> domains;
    std::vector<SurfaceRepresentationNode> representations;
    std::vector<SurfaceRuntimeRecord> runtime_records;
    std::vector<SurfaceClosurePolygon> closures;
    std::vector<RepresentationMigration> migrations;
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source) const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

} // namespace carto::surface
