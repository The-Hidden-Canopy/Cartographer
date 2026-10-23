#pragma once

#include <carto/production/bake_graph.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace carto::production::detail {

struct AmbientOcclusionProjectionTriangle {
    std::uint64_t face_id = 0U;
    std::uint32_t fan_index = 0U;
    std::array<core::Vec3d, 3U> positions{};
};

struct AmbientOcclusionProjectionRay {
    geometry::FaceId target_face;
    std::uint32_t texel_x = 0U;
    std::uint32_t texel_y = 0U;
    std::uint32_t sample_index = 0U;
    core::Vec3d origin;
    core::Vec3d direction;
};

struct AmbientOcclusionProjectionWork {
    // Contains the CPU rasterization diagnostics, counts, canonical coverage
    // mask, and ordered rays. The all-visible artifact is reused as the
    // template when verified worker hits are folded into the AO map.
    AmbientOcclusionExecutionReport validation;
    std::vector<AmbientOcclusionProjectionTriangle> source_triangles;
    std::vector<AmbientOcclusionProjectionRay> rays;
};

[[nodiscard]] AmbientOcclusionProjectionWork prepare_ambient_occlusion_projection_work(
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);

[[nodiscard]] AmbientOcclusionExecutionReport compile_ambient_occlusion_from_projection(
    AmbientOcclusionProjectionWork work,
    std::span<const std::uint8_t> hit_flags);

} // namespace carto::production::detail
