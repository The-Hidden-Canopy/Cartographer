#pragma once

#include <carto/production/bake_graph.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {
class ProjectDocument;
class ProjectTransaction;
}

namespace carto::production {

inline constexpr std::uint32_t kGarmentAssetVersion = 2U;
inline constexpr std::size_t kMaxGarmentPanels = 4'096U;
inline constexpr std::size_t kMaxGarmentSeams = 4'096U;
inline constexpr std::size_t kMaxGarmentPins = 65'536U;
inline constexpr std::size_t kMaxGarmentCollisionCapsules = 4'096U;
inline constexpr std::size_t kMaxGarmentChainVertices = 65'536U;
inline constexpr std::size_t kMaxGarmentAttachments = 65'536U;
inline constexpr std::size_t kMaxGarmentAssetBytes = 16U * 1024U * 1024U;

struct GarmentFabricIntent {
    double areal_density = 0.3;
    double stretch_compliance = 1e-8;
    double shear_compliance = 1e-7;
    double bend_compliance = 1e-4;
    double damping = 0.01;
    double thickness = 0.001;

    [[nodiscard]] core::Result<void> validate() const;
};

struct GarmentPanel {
    std::string identity;
    std::vector<geometry::FaceId> faces;
};

struct GarmentSeam {
    std::string identity;
    std::vector<geometry::VertexId> first_chain;
    std::vector<geometry::VertexId> second_chain;
    double compliance = 0.0;
};

struct GarmentPin {
    std::string identity;
    geometry::VertexId vertex;
    core::Vec3d target_position;
    double compliance = 0.0;
};

// A durable semantic attachment anchor. target_identity is an authored
// identity resolved by a runtime or host adapter; target_position is its
// bind-pose point for the deterministic CPU reference solver.
struct GarmentAttachment {
    std::string identity;
    std::string target_identity;
    geometry::VertexId vertex;
    core::Vec3d target_position;
    double compliance = 0.0;
};

struct GarmentCollisionCapsule {
    std::string identity;
    core::Vec3d start;
    core::Vec3d end;
    double radius = 0.05;
};

// Solver-neutral authored garment source. The bound mesh supplies imported
// surface topology; panels, seams, pins, and fabric intent remain durable data.
struct GarmentAsset {
    std::string identity;
    std::string project_namespace;
    std::uint64_t mesh_asset = 0U;
    BakeMeshBinding source;
    GarmentFabricIntent fabric;
    std::vector<GarmentPanel> panels;
    std::vector<GarmentSeam> seams;
    std::vector<GarmentCollisionCapsule> collision_capsules;
    std::vector<GarmentPin> pins;
    std::vector<GarmentAttachment> attachments;
    std::uint32_t format_version = kGarmentAssetVersion;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate(
        const geometry::EditableMesh& source_mesh) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> digest() const;
    [[nodiscard]] static core::Result<GarmentAsset> deserialize(std::string_view bytes);
};

[[nodiscard]] std::string garment_mesh_source_identity(
    std::string_view project_namespace,
    std::uint64_t mesh_asset);
[[nodiscard]] core::Result<GarmentAsset> make_garment_asset(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    std::string identity);
[[nodiscard]] core::Result<void> stage_garment_asset(
    project::ProjectTransaction& transaction,
    const GarmentAsset& asset,
    std::optional<assets::Sha256Digest> expected_current_digest = std::nullopt);
[[nodiscard]] std::string garment_asset_transaction_operation(
    const assets::Sha256Digest& asset_digest);

} // namespace carto::production
