#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace carto::render {

// Tangent generation consumes a derived render vertex stream. Callers must
// split vertices at authored UV seams and hard-normal boundaries before calling
// this helper; it never mutates authoring topology.
struct TangentVertex {
    core::Vec3d position{};
    core::Vec3d normal{0.0, 0.0, 1.0};
    core::Vec2d uv{};
};

// Signed UV-space area term for one triangle's corners. Fails with the shared
// degeneracy rule that generate_tangents applies, so callers that only need the
// mirroring sign admit exactly the triangles tangent generation admits.
[[nodiscard]] core::Result<double> tangent_uv_determinant(
    core::Vec2d a, core::Vec2d b, core::Vec2d c);

// Returns tangent.xyz plus a handedness sign in tangent.w, suitable for the
// bounded native PBR vertex ABI.
[[nodiscard]] core::Result<std::vector<core::Vec4d>> generate_tangents(
    std::span<const TangentVertex> vertices,
    std::span<const std::uint32_t> indices);

} // namespace carto::render
