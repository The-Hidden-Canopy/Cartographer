#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/weights_record.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace carto::weights {

// Deterministic automatic skin-weight generation (Chapter 08 "Automatic weight
// solver"): per-joint support regions are derived from joint anchors and
// mesh geodesic distance, seeded by normalized inverse-distance falloff,
// optionally heat-smoothed, then refined by a bounded Laplacian smoothing pass
// over mesh adjacency with projection onto the admission simplex after every
// step. The output is a complete kinematic::SkinBinding whose every per-vertex
// influence list passes character::validate_influences. This is a
// cpu_reference producer: no claim beyond bounded deterministic CPU evidence.

enum class DistanceMode : std::uint8_t {
    // Multi-source Dijkstra over mesh edges with Euclidean edge lengths.
    geodesic,
    // Bounded explicit heat diffusion from the anchors; distance is
    // -log(heat). Deterministic and bounded, documented as a first-pass heat
    // approximation, not a full heat-method solve.
    heat,
};

[[nodiscard]] std::string_view distance_mode_name(DistanceMode mode) noexcept;

// Immutable solver configuration. Every field is bounded; validation fails
// closed on hostile input before any allocation driven by the values.
struct AutoWeightRecipe {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::uint32_t kMinInfluencesPerVertex = 1U;
    static constexpr std::uint32_t kMaxInfluencesPerVertex =
        kinematic::SkinBinding::kMaxInfluencesPerVertex;
    static constexpr std::uint32_t kMaxSmoothingIterations = 64U;
    static constexpr std::uint32_t kMaxHeatSteps = 1'024U;
    static constexpr std::uint32_t kMaxCandidateJoints = 64U;
    static constexpr std::size_t kMaxSolverJoints = 1'024U;
    static constexpr std::size_t kMaxAnchorOverrides = 4'096U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;

    // Layer identity the produced SkinWeightLayer is pinned to.
    character::RigLayerId output_layer_id;
    // Final maximum active influences per vertex (1..8).
    std::uint32_t max_influences_per_vertex = 4U;
    // Candidate joints considered per vertex (>= max_influences, bounded).
    std::uint32_t nearest_joint_candidates = 4U;
    // Inverse-distance falloff exponent; strictly positive.
    double distance_falloff = 2.0;
    std::uint32_t smoothing_iterations = 8U;
    // Laplacian blend factor, strictly inside (0, 1).
    double smoothing_strength = 0.5;
    DistanceMode distance = DistanceMode::geodesic;
    // Diffusion steps for DistanceMode::heat.
    std::uint32_t heat_steps = 32U;
    // Explicit anchor overrides; the default anchor is the joint's rest
    // transform translation. An override is additive: the joint keeps its
    // default anchor and gains the override as an extra distance-field seed,
    // so pointing one at a detached rigid accessory assigns that region
    // without abandoning the region the default anchor already reaches.
    std::map<kinematic::JointId, core::Vec3d> anchor_overrides;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<AutoWeightRecipe> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Provenance/quality receipt for one automatic generation run. Contains no
// wall time: determinism and identity come from the recipe digest, the bounded
// iteration count and the output digest.
struct AutoWeightReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    assets::Sha256Digest recipe_digest;
    DistanceMode distance = DistanceMode::geodesic;
    // Smoothing iterations actually executed (<= recipe bound).
    std::uint32_t iterations_used = 0U;
    // Maximum per-influence weight change observed in the final accepted
    // smoothing iteration; the solver's residual statistic.
    double final_residual = 0.0;
    std::uint64_t joint_count = 0U;
    std::uint64_t influenced_vertex_count = 0U;
    std::uint64_t total_influence_count = 0U;
    assets::Sha256Digest output_binding_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<AutoWeightReceipt> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct AutoWeightOutput {
    kinematic::SkinBinding binding;
    character::SkinWeightLayer layer;
    AutoWeightReceipt receipt;
};

// Generates a complete weight layer for `mesh` against `graph`. `mesh_digest`
// and `mesh_asset` pin the binding to the exact mesh content identity. Fails
// closed (never partially deforming) when: the graph is invalid, any mesh
// vertex is unreachable from every joint anchor (disconnected regions require
// an explicit anchor override or an honest refusal), or any produced vertex
// fails character::validate_influences.
[[nodiscard]] core::Result<AutoWeightOutput> generate_auto_weights(
    const geometry::CompiledMesh& mesh,
    const kinematic::KinematicGraph& graph,
    const assets::Sha256Digest& mesh_digest,
    std::uint64_t mesh_asset,
    const AutoWeightRecipe& recipe);

} // namespace carto::weights
