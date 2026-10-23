#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>

namespace carto::geometry {

// Dimensions are full extents. The returned meshes use deterministic stable
// element IDs and outward/upward winding, respectively.
[[nodiscard]] core::Result<EditableMesh> make_box(core::Vec3d size);
[[nodiscard]] core::Result<EditableMesh> make_plane(double width, double depth);

} // namespace carto::geometry
