#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

// A decal is an authored projection request bound to one exact mesh source.
// It does not change mesh topology or imply a material has been rendered.
struct DecalPlacement {
    static constexpr std::size_t kMaxSerializedBytes = 4096U;
    static constexpr double kMaxExtent = 1'000'000.0;

    std::uint64_t id = 0U;
    core::Revision revision;
    std::uint64_t target_mesh_asset = 0U;
    core::Revision target_mesh_revision;
    assets::Sha256Digest target_mesh_digest;
    std::uint64_t material_id = 0U;
    core::Vec3d center{};
    core::Vec3d normal{0.0, 0.0, 1.0};
    core::Vec3d tangent{1.0, 0.0, 0.0};
    double width = 1.0;
    double height = 1.0;
    double depth = 0.1;
    // Minimum dot product of the target face normal and projection normal.
    // Zero admits perpendicular surfaces; one admits only aligned surfaces.
    double minimum_normal_dot = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct DecalProjectedVertex {
    core::Vec2d uv;
    std::array<double, 3U> source_barycentric{};
};

struct DecalProjectedPolygon {
    geometry::FaceId source_face;
    std::uint32_t source_triangle = 0U;
    std::vector<DecalProjectedVertex> vertices;
};

struct DecalProjection {
    std::uint64_t placement_id = 0U;
    core::Revision placement_revision;
    std::uint64_t target_mesh_asset = 0U;
    core::Revision target_mesh_revision;
    assets::Sha256Digest target_mesh_digest;
    std::uint64_t material_id = 0U;
    std::vector<DecalProjectedPolygon> polygons;
};

[[nodiscard]] core::Result<void> validate_decal_source(
    const DecalPlacement& placement,
    std::uint64_t current_mesh_asset,
    core::Revision current_mesh_revision,
    const assets::Sha256Digest& current_mesh_digest);

// Requires the caller to present the currently observed source identity and
// digest. Stale placement never silently projects onto a changed mesh.
[[nodiscard]] core::Result<DecalProjection> project_decal(
    const DecalPlacement& placement,
    const geometry::EditableMesh& mesh,
    std::uint64_t current_mesh_asset,
    const assets::Sha256Digest& current_mesh_digest);

[[nodiscard]] core::Result<DecalPlacement> revise_decal_placement(
    const DecalPlacement& current,
    core::Revision expected_revision,
    DecalPlacement desired);

[[nodiscard]] core::Result<std::string> serialize_decal_placement(
    const DecalPlacement& placement);
[[nodiscard]] core::Result<DecalPlacement> deserialize_decal_placement(
    std::string_view bytes);
[[nodiscard]] core::Result<core::Revision> stage_decal_placement(
    project::ProjectTransaction& transaction,
    const DecalPlacement& placement,
    core::Revision expected_project_placement_revision);
[[nodiscard]] core::Result<DecalPlacement> load_decal_placement(
    const project::ProjectDocument& document,
    std::uint64_t placement_id);

} // namespace carto::production
