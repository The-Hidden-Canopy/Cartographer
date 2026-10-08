#pragma once

#include <carto/geometry/mesh.hpp>
#include <carto/production/closure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::production {

inline constexpr std::uint32_t kRenderMeshArtifactFormatVersion = 2U;
inline constexpr std::size_t kMaxRenderMeshArtifactBytes = 256U * 1024U * 1024U;
inline constexpr std::size_t kMaxRenderMeshArtifactVertices = 2'000'000U;
inline constexpr std::size_t kMaxRenderMeshArtifactIndices = 6'000'000U;
inline constexpr std::size_t kMaxRenderMeshArtifactSubmeshes = 65'536U;

struct RenderMeshVertex {
    geometry::VertexId source_vertex;
    core::Vec3d position;
    core::Vec3d normal;
    // uv remains the primary/render stream (UV0) for source compatibility.
    std::optional<core::Vec2d> uv;
    std::optional<core::Vec2d> uv1;
    std::optional<core::Vec4d> color;
    std::optional<core::Vec4d> tangent;
};

struct RenderMeshSubmesh {
    std::optional<std::uint32_t> material_slot;
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;

    [[nodiscard]] bool operator==(const RenderMeshSubmesh&) const noexcept = default;
};

struct RenderMeshBounds {
    core::Bounds3d aabb;
    core::Vec3d sphere_center;
    double sphere_radius = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Portable derived geometry only. It contains no material definitions, runtime
// handles, paths, telemetry, or representation-selection policy.
struct RenderMeshArtifact {
    core::Revision source_revision;
    core::Revision mesh_revision;
    assets::Sha256Digest source_digest;
    std::vector<RenderMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<geometry::FaceId> triangle_faces;
    std::vector<RenderMeshSubmesh> submeshes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<RenderMeshBounds> bounds() const;
    // One deterministic cluster bound per canonical material submesh.
    [[nodiscard]] core::Result<std::vector<RenderMeshBounds>> submesh_bounds() const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> serialize() const;
    [[nodiscard]] static core::Result<RenderMeshArtifact> deserialize(
        std::span<const std::uint8_t> bytes);
};

struct RenderMeshCompileOptions {
    // UV selection is explicit: null means omit UVs; a set ID must resolve to
    // an authored corner-domain vec2 set on this mesh.
    std::optional<attributes::UvSetId> uv_set;
    // Optional secondary/lightmap stream. It must resolve independently and
    // must not alias uv_set.
    std::optional<attributes::UvSetId> uv1_set;
    bool generate_tangents = false;
    // When absent, triangles have no material assignment. When present, this
    // must name a face-domain uint32 slot-index layer.
    std::optional<std::string> material_slot_layer;
    // Optional authored color4 layer. Vertex and corner domains are supported;
    // corner colors participate in deterministic seam splitting.
    std::optional<std::string> vertex_color_layer;
};

// Float32 upload ABI consumed by the public native PBR vertex shader. The
// derived artifact remains double precision; this is an explicit, validated
// projection made only at the device boundary.
struct PbrUploadVertex {
    std::array<float, 4U> position{};
    std::array<float, 3U> normal{};
    std::array<float, 2U> uv0{};
    std::array<float, 4U> tangent{};
};

inline constexpr std::uint32_t kPbrUploadVertexStrideBytes = 52U;
static_assert(sizeof(PbrUploadVertex) == kPbrUploadVertexStrideBytes);
static_assert(std::is_trivially_copyable_v<PbrUploadVertex>);

struct PbrMeshUpload {
    core::Revision source_revision;
    core::Revision mesh_revision;
    assets::Sha256Digest source_digest;
    std::vector<PbrUploadVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RenderMeshSubmesh> submeshes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept;
};

// Produces the exact interleaved layout consumed by cartographer_pbr.hlsl.
// Missing authored UV/tangent data and values that cannot be represented as
// finite float32 fail closed rather than being defaulted or clamped.
[[nodiscard]] core::Result<PbrMeshUpload> pack_pbr_mesh_upload(
    const RenderMeshArtifact& artifact);

// Compiles one bounded public render-mesh representation from authoritative
// Cartographer mesh data. It never edits the source mesh. The caller supplies
// the enclosing saved-project source identity; source revision/digest are
// copied into the artifact and should be bound to the production receipt.
[[nodiscard]] core::Result<RenderMeshArtifact> compile_render_mesh_artifact(
    const AuthoritativeSource& source,
    const geometry::EditableMesh& mesh,
    const RenderMeshCompileOptions& options = {});

// Saved-document overload: validates the whole ProjectDocument, derives its
// source digest from canonical project serialization, and selects the mesh by
// its project asset ID. project_identity must be a stable non-path identifier.
[[nodiscard]] core::Result<RenderMeshArtifact> compile_render_mesh_artifact(
    std::string_view project_identity,
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset_id,
    const RenderMeshCompileOptions& options = {});

} // namespace carto::production
