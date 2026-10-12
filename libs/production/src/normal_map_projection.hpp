#pragma once

#include <carto/production/bake_graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace carto::production::detail {

inline constexpr std::size_t kMaxNormalMapProjectionSamples = 1'000'000U;

struct NormalMapProjectionTriangle {
    std::uint64_t face_id = 0U;
    std::uint32_t fan_index = 0U;
    std::array<core::Vec3d, 3U> positions{};
    std::array<core::Vec3d, 3U> normals{};
};

struct NormalMapProjectionSample {
    geometry::FaceId target_face;
    std::uint32_t texel_x = 0U;
    std::uint32_t texel_y = 0U;
    std::size_t pixel_index = 0U;
    core::Vec3d ray_origin;
    core::Vec3d ray_direction;
    core::Vec3d target_normal;
    core::Vec3d tangent;
    core::Vec3d bitangent;
};

struct NormalMapProjectionOutcome {
    bool hit = false;
    std::size_t triangle_index = 0U;
    bool backface = false;
    double distance = 0.0;
    std::array<double, 3U> barycentric{};
    std::optional<std::size_t> rejected_backface_triangle_index;
};

struct NormalMapBakeWork {
    BakeExecutionReport report;
    BakeRecipe recipe;
    BakeNormalSpace normal_space = BakeNormalSpace::tangent;
    assets::Sha256Digest settings_digest;
    std::vector<NormalMapProjectionTriangle> source_triangles;
    std::vector<NormalMapProjectionSample> samples;
    std::vector<std::uint8_t> pixels;
    std::vector<BakeProjectionMissSample> miss_samples;
    bool miss_samples_truncated = false;
};

[[nodiscard]] NormalMapBakeWork prepare_normal_map_work(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    BakeNormalSpace normal_space);

[[nodiscard]] std::vector<NormalMapProjectionOutcome> cast_normal_map_cpu_reference(
    const NormalMapBakeWork& work,
    const geometry::EditableMesh& source);

[[nodiscard]] BakeExecutionReport finalize_normal_map_work(
    NormalMapBakeWork work,
    std::span<const NormalMapProjectionOutcome> outcomes);

} // namespace carto::production::detail
