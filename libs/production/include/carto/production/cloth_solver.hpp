#pragma once

#include <carto/production/garment_asset.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace carto::production {

class ProductionArtifactStore;

inline constexpr std::uint32_t kMaxClothFrames = 600U;
inline constexpr std::uint32_t kMaxClothSubsteps = 16U;
inline constexpr std::uint32_t kMaxClothIterations = 64U;
inline constexpr std::uint64_t kMaxClothProjectionOperations = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxClothVertices = 100'000ULL;
inline constexpr std::uint64_t kMaxClothFaces = 100'000ULL;
inline constexpr std::uint64_t kMaxClothConstraints = 1'000'000ULL;
inline constexpr std::uint64_t kMaxClothSelfCollisionCellOccupancy = 64ULL;
inline constexpr std::uint64_t kMaxClothSelfCollisionTriangles = 200'000ULL;
inline constexpr std::uint64_t kMaxClothSelfCollisionEdges = 600'000ULL;
inline constexpr std::uint64_t kMaxClothSimulationSamples = 1'000'000ULL;
inline constexpr std::uint64_t kMaxClothSimulationArtifactBytes =
    128ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxClothRestPoseReceiptBytes = 16U * 1024U;

struct ClothSolverSettings {
    double frame_time_step = 1.0 / 60.0;
    std::uint32_t frame_count = 60U;
    std::uint32_t substeps_per_frame = 2U;
    std::uint32_t projection_iterations = 8U;
    core::Vec3d gravity{0.0, -9.81, 0.0};
    bool enable_self_collision = false;
    double settle_displacement_tolerance = 1e-3;
    std::uint32_t settle_window_frames = 3U;
    bool require_settled = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> digest() const;
};

struct ClothSimulationFrame {
    std::uint32_t index = 0U;
    // Maximum vertex movement from the previous saved frame. Frame zero is
    // measured from the authored source mesh during deterministic compilation.
    double maximum_frame_displacement = 0.0;
    // Vertices are sorted by stable source ID and have the same ID sequence in
    // every frame. Frame positions are the solver output; topology and
    // attributes remain bound to the GarmentAsset source mesh.
    std::vector<geometry::Vertex> vertices;
};

struct ClothSimulationArtifact {
    BakeMeshBinding source;
    assets::Sha256Digest garment_digest;
    ClothSolverSettings settings;
    std::string algorithm{"carto.cloth.cpu.xpbd-reference.v5"};
    std::string evidence_class{"carto.cpu-reference"};
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t byte_size = 0U;
    std::uint64_t completed_substeps = 0U;
    double maximum_constraint_error = 0.0;
    double maximum_collision_correction = 0.0;
    // Index of the first frame ending a complete stable displacement window.
    // Empty means the configured window was not reached within frame_count.
    std::optional<std::uint32_t> settled_frame;
    std::vector<ClothSimulationFrame> frames;

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<ClothSimulationArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<void> validate(
        const GarmentAsset& garment,
        const geometry::EditableMesh& base_mesh) const;
};

// Durable provenance for a settled simulation frame baked onto a derived mesh.
// The mesh is reconstructible from the bound source plus simulation artifact;
// this receipt records the exact output mesh binding selected for publication.
struct ClothRestPoseBakeReceipt {
    BakeMeshBinding source;
    assets::Sha256Digest garment_digest;
    assets::Sha256Digest simulation_artifact_digest;
    std::uint32_t frame_index = 0U;
    BakeMeshBinding output;
    std::uint64_t vertex_count = 0U;
    std::string algorithm{"carto.cloth.rest-pose-bake.v1"};
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<ClothRestPoseBakeReceipt> deserialize(
        std::span<const std::uint8_t> bytes);
};

struct ClothRestPoseBakeResult {
    ClothRestPoseBakeReceipt receipt;
    geometry::EditableMesh mesh;

    [[nodiscard]] core::Result<void> validate(
        const ClothSimulationArtifact& simulation,
        const GarmentAsset& garment,
        const geometry::EditableMesh& source_mesh) const;
};

// Converts only the simulation's derived settled-frame endpoint. This is an
// in-memory product; callers publish its mesh through ProjectTransaction and
// retain the canonical receipt in their existing derived-product store.
[[nodiscard]] core::Result<ClothRestPoseBakeResult> bake_cloth_settled_rest_pose(
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& source_mesh);
[[nodiscard]] core::Result<void> stage_cloth_rest_pose_bake_receipt(
    project::ProjectTransaction& transaction,
    std::uint64_t output_mesh_asset,
    const ClothRestPoseBakeResult& result,
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& source_mesh);

[[nodiscard]] core::Result<ProductionCookKey> cloth_rest_pose_bake_cook_key(
    const ClothSimulationArtifact& simulation,
    std::string platform_profile);
[[nodiscard]] core::Result<void> publish_cloth_rest_pose_bake(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const ClothRestPoseBakeResult& result,
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_source_mesh,
    const AdmissionPolicy& policy);
[[nodiscard]] core::Result<ClothRestPoseBakeResult> read_cloth_rest_pose_bake(
    const ProductionArtifactStore& store,
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_source_mesh,
    std::string platform_profile,
    const AdmissionPolicy& policy);

// Deterministic CPU XPBD reference. It never mutates the authored mesh or the
// GarmentAsset. Optional vertex-vertex, vertex-face and edge-edge self-contact
// uses bounded neighborhoods; dense cells or excessive pair work fail
// explicitly rather than being truncated.
[[nodiscard]] core::Result<ClothSimulationArtifact> compile_cloth_cpu(
    const GarmentAsset& garment,
    const geometry::EditableMesh& base_mesh,
    const ClothSolverSettings& settings = {});

[[nodiscard]] core::Result<ProductionCookKey> cloth_production_cook_key(
    const GarmentAsset& garment,
    const ClothSolverSettings& settings,
    std::string platform_profile);

[[nodiscard]] core::Result<void> publish_cloth_simulation(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const ClothSimulationArtifact& artifact,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_mesh,
    const AdmissionPolicy& policy);

[[nodiscard]] core::Result<ClothSimulationArtifact> read_cloth_simulation(
    const ProductionArtifactStore& store,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_mesh,
    const ClothSolverSettings& settings,
    std::string platform_profile,
    const AdmissionPolicy& policy);

} // namespace carto::production
