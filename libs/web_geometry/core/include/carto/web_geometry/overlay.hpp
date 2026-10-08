#pragma once

#include <carto/core/result.hpp>
#include <carto/web_geometry/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::web_geometry {

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
    core::Revision source_revision;
    OverlayViewport viewport;
    std::vector<ProjectedClusterOverlay> clusters;

    [[nodiscard]] core::Result<void> validate() const;
};

// Projects validated cluster AABBs for editor/viewport overlay consumers. It
// performs no rendering, GPU allocation, selection mutation, or runtime
// admission. Clusters crossing a clip plane are conservatively retained and
// marked clipped; clusters fully outside one clip plane are not visible.
[[nodiscard]] core::Result<ClusterOverlaySnapshot> project_cluster_overlays(
    const WebGeometryPackage& package,
    const OverlayProjection& projection,
    OverlayViewport viewport);

} // namespace carto::web_geometry
