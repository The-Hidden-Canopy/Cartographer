#pragma once

#include <carto/web_geometry/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace carto::web_geometry {

struct RuntimePagePayloadTriangle {
    std::uint32_t source_triangle = 0U;
    geometry::FaceId source_face;
    std::array<core::Vec3d, 3> positions{};
    std::array<core::Vec3d, 3> normals{};
};

struct RuntimePagePayloadCluster {
    ClusterId cluster = 0U;
    std::vector<RuntimePagePayloadTriangle> triangles;
};

struct RuntimePagePayload {
    std::vector<RuntimePagePayloadCluster> clusters;
};

// The page payload is a vendor-neutral little-endian record stream. It is
// derived geometry only and never carries mutable authoring state.
[[nodiscard]] core::Result<RuntimePagePayload> decode_runtime_page_payload(
    std::span<const std::uint8_t> payload);

} // namespace carto::web_geometry
