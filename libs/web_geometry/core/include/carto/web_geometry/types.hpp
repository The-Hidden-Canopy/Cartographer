#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/ids.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::geometry {
struct CompiledMesh;
}

namespace carto::web_geometry {

using ClusterId = std::uint32_t;
using RuntimePageId = std::uint32_t;

inline constexpr RuntimePageId kUnassignedRuntimePage =
    static_cast<RuntimePageId>(-1);
inline constexpr std::uint32_t kRuntimePagePayloadVersion = 1U;
inline constexpr std::array<std::uint8_t, 4> kRuntimePagePayloadMagic = {
    'W', 'G', 'P', '1'};

struct WebGeometryCompileOptions {
    std::uint32_t target_triangles_per_leaf = 128U;
    std::uint32_t hard_max_triangles_per_leaf = 256U;
    std::uint32_t max_children_per_node = 8U;
    std::uint32_t target_page_payload_bytes = 256U * 1024U;
    std::uint32_t hard_max_page_payload_bytes = 512U * 1024U;
    bool preserve_material_boundaries = true;
    bool preserve_semantic_boundaries = true;
    bool preserve_hard_normal_boundaries = true;
    bool preserve_authored_break_boundaries = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct ProvenanceContext {
    core::Revision source_revision;
    std::vector<geometry::FaceId> source_faces;

    [[nodiscard]] static core::Result<ProvenanceContext> from_compiled_mesh(
        const geometry::CompiledMesh& mesh);
    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& mesh) const;
};

struct ClusterBounds {
    core::Bounds3d aabb;
    core::Vec3d sphere_center{};
    double sphere_radius = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct ClusterRecord {
    ClusterId id = 0U;
    std::optional<ClusterId> parent;
    std::uint32_t first_child = 0U;
    std::uint32_t child_count = 0U;
    std::uint32_t first_triangle = 0U;
    std::uint32_t triangle_count = 0U;
    ClusterBounds bounds;
    double geometric_error = 0.0;
    std::uint32_t material_group = 0U;
    std::uint32_t semantic_group = 0U;
    RuntimePageId page = kUnassignedRuntimePage;
    std::uint64_t content_digest = 0U;
};

struct ClusterHierarchy {
    std::vector<ClusterRecord> clusters;
    std::vector<ClusterId> child_ids;
    std::vector<std::uint32_t> triangle_indices;
    std::vector<geometry::FaceId> source_faces;
    core::Revision source_revision;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct ClusterProvenance {
    ClusterId cluster = 0U;
    std::vector<geometry::FaceId> source_faces;
    core::Revision source_revision;
    std::string compiler_options_digest;
    std::string content_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct RuntimeGeometryPage {
    RuntimePageId id = 0U;
    std::uint32_t first_cluster = 0U;
    std::uint32_t cluster_count = 0U;
    std::uint64_t payload_offset = 0U;
    std::uint64_t payload_bytes = 0U;
    core::Bounds3d bounds;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate(std::size_t cluster_total) const;
    [[nodiscard]] std::string canonical() const;
};

struct WebGeometryPackage {
    static constexpr std::uint32_t current_schema_version = 2U;

    std::uint32_t schema_version = current_schema_version;
    std::string compiler_version = "cartographer-web-geometry/0.1";
    core::Revision source_revision;
    WebGeometryCompileOptions options;
    ClusterHierarchy hierarchy;
    std::vector<ClusterProvenance> provenance;
    std::vector<RuntimeGeometryPage> pages;
    std::vector<std::uint8_t> payload;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

} // namespace carto::web_geometry
