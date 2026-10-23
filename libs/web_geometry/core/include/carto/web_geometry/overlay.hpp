#pragma once

#include <carto/core/result.hpp>
#include <carto/web_geometry/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::web_geometry {

inline constexpr std::size_t kMaxClusterOverlayDraws = 65'536U;

struct OverlayViewport {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Row-major matrix with clip coordinates produced as M * (x, y, z, 1).
// Clip-space depth is the Vulkan/D3D convention [0, w].
struct OverlayProjection {
    std::array<double, 16U> values{};

    [[nodiscard]] core::Result<void> validate() const;
};

struct ProjectedClusterOverlay {
    ClusterId cluster = 0U;
    std::optional<ClusterId> parent;
    RuntimePageId page = kUnassignedRuntimePage;
    double minimum_x = 0.0;
    double minimum_y = 0.0;
    double maximum_x = 0.0;
    double maximum_y = 0.0;
    double minimum_depth = 0.0;
    double maximum_depth = 0.0;
    std::string error_receipt_digest;
    bool visible = false;
    bool clipped = false;
    bool grouping_only = false;
};

struct ClusterOverlaySnapshot {
    std::string package_digest;
    std::string source_object_identity;
    core::Revision source_revision;
    OverlayViewport viewport;
    std::vector<ProjectedClusterOverlay> clusters;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ClusterOverlayPointerHit {
    std::string source_object_identity;
    std::string package_digest;
    core::Revision source_revision;
    ClusterId cluster = 0U;
    RuntimePageId page = kUnassignedRuntimePage;
    std::vector<geometry::FaceId> source_faces;
};

struct ClusterOverlayVertex {
    float x = 0.0F;
    float y = 0.0F;
    std::array<float, 4U> color{};
};

struct ClusterOverlayDrawRange {
    ClusterId cluster = 0U;
    std::uint32_t first_vertex = 0U;
    std::uint32_t vertex_count = 0U;
    bool grouping_only = false;
};

struct ClusterOverlayDrawPlan {
    std::string package_digest;
    std::string source_object_identity;
    core::Revision source_revision;
    OverlayViewport viewport;
    std::vector<ClusterOverlayVertex> vertices;
    std::vector<ClusterOverlayDrawRange> draws;

    [[nodiscard]] core::Result<void> validate() const;
};

// Projects validated cluster AABBs for editor/viewport overlay consumers. It
// performs no rendering, GPU allocation, selection mutation, or runtime
// admission. Clusters crossing a clip plane are conservatively retained and
// marked clipped; clusters fully outside one clip plane are not visible.
[[nodiscard]] core::Result<ClusterOverlaySnapshot> project_cluster_overlays(
    const WebGeometryPackage& package,
    const OverlayProjection& projection,
    OverlayViewport viewport,
    std::string source_object_identity = {});

// Resolves a pointer against the exact package/object/revision represented by
// the overlay snapshot. Overlapping cluster bounds are returned as candidates
// in front-to-back order; this function never silently chooses among them.
[[nodiscard]] core::Result<std::vector<ClusterOverlayPointerHit>>
resolve_cluster_overlay_pointer(
    const WebGeometryPackage& package,
    const ClusterOverlaySnapshot& snapshot,
    double pointer_x,
    double pointer_y,
    std::string_view current_source_object_identity,
    core::Revision current_source_revision);

// Builds screen-space rectangle outlines as a backend-neutral triangle list.
// Visible clusters are ordered back-to-front with stable cluster-ID ties;
// grouping-only bounds use a distinct warning color.
[[nodiscard]] core::Result<ClusterOverlayDrawPlan> build_cluster_overlay_draw_plan(
    const ClusterOverlaySnapshot& snapshot,
    double border_thickness_pixels = 2.0);

} // namespace carto::web_geometry
