#pragma once

#include <carto/production/artifact_store.hpp>
#include <carto/production/sculpt_stack.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::production {

inline constexpr std::uint32_t kMaxMultiresSubdivisionLevels = 4U;
inline constexpr std::uint64_t kMaxMultiresHierarchyFaces = 128'000ULL;
inline constexpr std::uint64_t kMaxMultiresHierarchyVertices = 200'000ULL;
inline constexpr std::uint64_t kMaxMultiresHierarchyBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxMultiresArtifactBytes =
    kMaxMultiresHierarchyBytes + 4'096ULL;

struct MultiresSettings {
    std::uint32_t subdivision_levels = 2U;
    // Importance is an explicit authored/job input. Higher values retain
    // sculpted detail as geometry regardless of its estimated frequency.
    double runtime_importance = 0.5;
    double geometry_importance_threshold = 0.75;
    double displacement_frequency_threshold = 2.0;
    double minimum_detail_displacement = 1e-6;
    bool preserve_boundary_geometry = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> digest() const;
};

enum class MultiresDetailTarget : std::uint8_t {
    unchanged = 0U,
    geometry = 1U,
    displacement = 2U,
    normal_field = 3U,
};

struct MultiresDetailClassification {
    geometry::FaceId source_face;
    std::array<geometry::VertexId, 3> source_vertices{};
    double estimated_frequency = 0.0;
    double rms_normal_displacement = 0.0;
    double rms_tangential_displacement = 0.0;
    double runtime_importance = 0.0;
    bool boundary_sensitive = false;
    MultiresDetailTarget target = MultiresDetailTarget::unchanged;
    // Explicit source-corner displacement vectors and one object-space
    // triangle normal. Only the field selected by target may be non-zero.
    std::array<core::Vec3d, 3> displacement_vectors{};
    core::Vec3d object_space_normal;
};

struct MultiresHierarchyVertex {
    geometry::VertexId id;
    core::Vec3d position;
};

struct MultiresHierarchyFace {
    geometry::FaceId id;
    geometry::FaceId source_face;
    std::array<geometry::VertexId, 3> vertices{};
    // Per-corner anchors retain the source triangle and seam side.
    std::array<SculptSurfaceAnchor, 3> source_corners{};
};

struct MultiresHierarchyLevel {
    std::uint32_t level = 0U;
    std::vector<MultiresHierarchyVertex> vertices;
    std::vector<MultiresHierarchyFace> faces;
};

struct MultiresLevelSourceError {
    std::uint32_t level = 0U;
    std::uint64_t sample_count = 0U;
    double maximum_anchor_reconstruction_error = 0.0;
    double rms_anchor_reconstruction_error = 0.0;
};

struct MultiresSurfaceHierarchy {
    BakeMeshBinding source;
    MultiresSettings settings;
    std::string algorithm{"carto.multires.cpu.triangle-midpoint-detail-map.v4"};
    std::string evidence_class{"carto.cpu-reference"};
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t byte_size = 0U;
    std::vector<MultiresHierarchyLevel> levels;
    std::vector<MultiresDetailClassification> detail_classifications;
    std::vector<MultiresLevelSourceError> source_reconstruction_errors;

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<void> validate(
        const SculptEvaluationArtifact& sculpted,
        const geometry::EditableMesh& base_mesh) const;
};

// Builds derived levels from an evaluated SculptStack. The source mesh and
// authored stack remain unchanged; the output is revision-bound and rebuildable.
[[nodiscard]] core::Result<MultiresSurfaceHierarchy> compile_multires_surface_hierarchy(
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const MultiresSettings& settings = {});

[[nodiscard]] core::Result<ProductionCookKey> multires_production_cook_key(
    const MultiresSurfaceHierarchy& hierarchy,
    std::string platform_profile);
[[nodiscard]] core::Result<void> publish_multires_surface_hierarchy(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const MultiresSurfaceHierarchy& hierarchy,
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const AdmissionPolicy& policy);
[[nodiscard]] core::Result<MultiresSurfaceHierarchy> read_multires_surface_hierarchy(
    const ProductionArtifactStore& store,
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const MultiresSettings& settings,
    std::string platform_profile,
    const AdmissionPolicy& policy);

} // namespace carto::production
