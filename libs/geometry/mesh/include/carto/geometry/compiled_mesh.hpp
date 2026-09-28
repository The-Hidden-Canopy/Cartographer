#pragma once

#include <carto/core/math.hpp>
#include <carto/core/revision.hpp>

#include <cstdint>
#include <vector>

namespace carto::geometry {

struct CompiledMesh {
    std::vector<core::Vec3d> positions;
    std::vector<std::uint32_t> indices;
    std::vector<core::Vec3d> normals;
    core::Bounds3d bounds;
    core::Revision source_revision;

    [[nodiscard]] bool valid() const noexcept {
        return positions.size() == normals.size() && indices.size() % 3U == 0U && bounds.valid();
    }
};

} // namespace carto::geometry

