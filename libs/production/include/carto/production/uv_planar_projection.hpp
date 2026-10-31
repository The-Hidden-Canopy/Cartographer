#pragma once

#include <carto/geometry/mesh.hpp>
#include <cstdint>
#include <string>

namespace carto::production {

enum class UvProjectionPlane : std::uint32_t { xy = 0U, yz = 1U, zx = 2U };

struct UvPlanarProjection {
    UvProjectionPlane plane = UvProjectionPlane::xy;
    core::Vec2d origin{};
    double scale = 1.0;
    core::Vec2d offset{};
    attributes::UvSetId output_uv_set = 0U;
    std::string output_name;
    std::string output_layer_id;
};

// Creates a new, inactive corner UV set on a copy. Every occurrence of a vertex
// gets the same projection: ((plane_coordinate - origin) * scale) + offset.
// Triangles only; projected doubled triangle area must exceed 1e-14. Existing
// UVs and seams are untouched; the new projection has no authored seam marks.
// Noncoplanar geometry is flattened and may fold or overlap. This does not
// detect overlap or create separate charts, pack, commit a project
// transaction, or bind an asynchronous result to a later source revision.
[[nodiscard]] core::Result<geometry::EditableMesh>
project_uv_planar(const geometry::EditableMesh &source, const UvPlanarProjection &settings);

} // namespace carto::production
