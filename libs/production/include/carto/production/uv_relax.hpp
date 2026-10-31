#pragma once

#include <carto/geometry/mesh.hpp>
#include <cstdint>
#include <string>

namespace carto::production {

// Headless uniform Laplacian relaxation of an existing triangular UV chart.
// A new UV set is returned on a copy; source and active-set choices are unchanged.
// Boundaries, explicit seams and UV discontinuities are fixed. Every iteration
// rejects UV triangles with absolute doubled area <= 1e-14 or flipped winding;
// world triangles with squared cross-product magnitude <= 1e-28 are rejected.
// This is not an unwrap, overlap
// solver, distortion optimum, transaction, or asynchronous stale-data receipt.
[[nodiscard]] core::Result<geometry::EditableMesh>
relax_uv_triangles(const geometry::EditableMesh &source, attributes::UvSetId source_uv_set,
                   attributes::UvSetId output_uv_set, std::string output_name,
                   std::string output_layer_id, std::uint32_t iterations, double strength);

} // namespace carto::production
