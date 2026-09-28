#pragma once

#include <carto/core/math.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace carto::geometry {

struct CompiledMesh {
    std::vector<core::Vec3d> positions;
    std::vector<std::uint32_t> indices;
    std::vector<core::Vec3d> normals;
    // Derived-index identity maps keep viewport picking and diagnostics tied
    // to authoring IDs.  They are never an alternate source of topology.
    std::vector<VertexId> vertex_ids;
    std::vector<FaceId> triangle_faces;
    core::Bounds3d bounds;
    core::Revision source_revision;

    [[nodiscard]] bool valid() const noexcept {
        if (positions.size() != normals.size() || positions.size() != vertex_ids.size() ||
            indices.size() % 3U != 0U || triangle_faces.size() != indices.size() / 3U ||
            !bounds.valid()) {
            return false;
        }
        for (std::size_t index = 0U; index < positions.size(); ++index) {
            if (!positions[index].finite() || !normals[index].finite() || !vertex_ids[index]) {
                return false;
            }
        }
        for (const std::uint32_t index : indices) {
            if (index >= positions.size()) {
                return false;
            }
        }
        for (const FaceId face : triangle_faces) {
            if (!face) {
                return false;
            }
        }
        return true;
    }
};

} // namespace carto::geometry
