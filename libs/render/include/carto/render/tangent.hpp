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

// Returns tangent.xyz plus a handedness sign in tangent.w, suitable for the
// bounded native PBR vertex ABI.
[[nodiscard]] core::Result<std::vector<core::Vec4d>> generate_tangents(
    std::span<const TangentVertex> vertices,
    std::span<const std::uint32_t> indices);

} // namespace carto::render
