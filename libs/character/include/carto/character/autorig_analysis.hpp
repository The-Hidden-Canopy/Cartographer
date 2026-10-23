#pragma once

#include <carto/character/definition.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace carto::character {

enum class AutorigAxis : std::uint8_t { x = 0U, y = 1U, z = 2U };

struct AutorigAnalysisBudget final {
    static constexpr std::size_t kHardMaxVertices = 200'000U;
    static constexpr std::size_t kHardMaxEdges = 600'000U;
    static constexpr std::size_t kHardMaxGeodesicSeeds = 16U;
    static constexpr std::size_t kHardMaxSymmetryCandidates = 2'000'000U;

    std::size_t max_vertices = 100'000U;
    std::size_t max_edges = 300'000U;
    std::size_t max_geodesic_seeds = 8U;
    std::size_t max_symmetry_candidates = 1'000'000U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AutorigAnalysisRequest final {
    MeshAssetRef source;
    core::Revision expected_mesh_revision;
    AutorigAxis symmetry_axis = AutorigAxis::x;
    double symmetry_plane_offset = 0.0;
    double symmetry_tolerance = 1.0e-6;
    // Seeds are sorted internally; duplicate or missing stable IDs are errors.
    std::vector<geometry::VertexId> geodesic_seeds;
    AutorigAnalysisBudget budget;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AutorigComponentFeature final {
    std::uint32_t component_index = 0U;
    geometry::VertexId representative;
    std::size_t vertex_count = 0U;
};

struct AutorigVertexFeature final {
    geometry::VertexId vertex;
    std::uint32_t component_index = 0U;
    std::uint32_t valence = 0U;
    std::uint32_t incident_face_count = 0U;
    bool boundary = false;
};

struct AutorigGeodesicDistance final {
    geometry::VertexId vertex;
    double distance = 0.0;
    bool reachable = false;
};

struct AutorigGeodesicField final {
    geometry::VertexId seed;
    std::vector<AutorigGeodesicDistance> distances;
};

struct AutorigSymmetryPair final {
    geometry::VertexId negative_side;
    geometry::VertexId positive_side;
    double positional_error = 0.0;
    bool topology_compatible = false;
};

struct AutorigFeatureAnalysis final {
    MeshAssetRef source;
    core::Revision mesh_revision;
    AutorigAxis symmetry_axis = AutorigAxis::x;
    double symmetry_plane_offset = 0.0;
    double symmetry_tolerance = 0.0;
    assets::Sha256Digest mesh_geometry_digest;
    assets::Sha256Digest analysis_digest;
    std::vector<AutorigComponentFeature> components;
    std::vector<AutorigVertexFeature> vertices;
    std::vector<AutorigGeodesicField> geodesic_fields;
    std::vector<AutorigSymmetryPair> symmetry_pairs;
    std::vector<geometry::VertexId> centerline_vertices;
    std::vector<geometry::VertexId> ambiguous_vertices;
    std::vector<geometry::VertexId> unmatched_vertices;

    [[nodiscard]] core::Result<void> validate() const;
};

// Deterministic, source-bound geometry analysis for later review-only autorig
// proposals. It reports topology, edge-length geodesics, and geometric
// symmetry evidence. It does not infer anatomy, create a skeleton, or mutate
// the mesh/project; ambiguity stays explicit for later candidate admission.
[[nodiscard]] core::Result<AutorigFeatureAnalysis> analyze_autorig_features(
    const geometry::EditableMesh& mesh,
    const AutorigAnalysisRequest& request);

} // namespace carto::character
