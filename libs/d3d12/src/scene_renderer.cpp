#include <carto/d3d12/scene_renderer.hpp>
#include <carto/d3d12/texture_upload.hpp>
#include <carto/render_graph/graph.hpp>
#include <carto/d3d12/detail/render_graph_dispatch.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::d3d12 {
namespace {
using core::Result;
using Matrix = std::array<float, 16U>;
struct ScenePbrUploadVertex {
    std::array<float, 4U> position{};
    std::array<float, 3U> normal{};
    std::array<float, 2U> uv{};
    std::array<float, 4U> tangent{};
    std::array<float, 4U> light_position{};
    std::array<float, 2U> metallic_roughness_uv{};
    std::array<float, 2U> normal_uv{};
    std::array<float, 2U> occlusion_uv{};
    std::array<float, 2U> emissive_uv{};
    std::array<float, 4U> camera_clearcoat{};
    std::array<float, 4U> surface_factors{};
    std::array<float, 4U> material_flags{};
    std::array<float, 4U> vertex_color{};
    std::array<float, 4U> channel_mapping_a{};
    std::array<float, 4U> channel_mapping_b{};
    std::array<float, 4U> channel_mapping_c{};
};
static_assert(sizeof(ScenePbrUploadVertex) == 212U);
static_assert(offsetof(ScenePbrUploadVertex, uv) == 28U);
static_assert(offsetof(ScenePbrUploadVertex, tangent) == 36U);
static_assert(offsetof(ScenePbrUploadVertex, light_position) == 52U);
static_assert(offsetof(ScenePbrUploadVertex, metallic_roughness_uv) == 68U);
static_assert(offsetof(ScenePbrUploadVertex, normal_uv) == 76U);
static_assert(offsetof(ScenePbrUploadVertex, occlusion_uv) == 84U);
static_assert(offsetof(ScenePbrUploadVertex, emissive_uv) == 92U);
static_assert(offsetof(ScenePbrUploadVertex, camera_clearcoat) == 100U);
static_assert(offsetof(ScenePbrUploadVertex, surface_factors) == 116U);
static_assert(offsetof(ScenePbrUploadVertex, material_flags) == 132U);
static_assert(offsetof(ScenePbrUploadVertex, vertex_color) == 148U);
static_assert(offsetof(ScenePbrUploadVertex, channel_mapping_a) == 164U);
static_assert(offsetof(ScenePbrUploadVertex, channel_mapping_b) == 180U);
static_assert(offsetof(ScenePbrUploadVertex, channel_mapping_c) == 196U);
constexpr std::uint64_t kPayloadLimit = 512ULL * 1024U * 1024U;
constexpr std::uint64_t kMaxSpecularEnvironmentPixels = 4'194'304ULL;
constexpr std::uint32_t kSplitSumBrdfLutExtent = 128U;
constexpr std::uint32_t kSplitSumBrdfSampleCount = 256U;
constexpr std::uint32_t kShadowMapExtent = 1024U;
constexpr std::array<assets::TextureSemantic, 5U> kSemantics{
    assets::TextureSemantic::base_color, assets::TextureSemantic::metallic_roughness,
    assets::TextureSemantic::normal, assets::TextureSemantic::occlusion,
    assets::TextureSemantic::emissive};

Result<void> validate_executed_texture_binding(
    const render::TextureRef& binding,
    assets::TextureSemantic semantic,
    std::string_view slot_name) {
    if (!binding) return Result<void>::success();

    const auto unsupported = [slot_name](std::string_view setting) {
        return Result<void>::failure({core::ErrorCode::unsupported,
            "scene PBR does not execute the authored " + std::string(slot_name) +
                " texture " + std::string(setting)});
    };
    if (semantic != assets::TextureSemantic::normal &&
        binding.normal_convention != render::NormalConvention::directx_y_minus)
        return unsupported("normal convention outside the normal-map slot");

    const assets::ColorSpace expected = assets::expected_color_space(semantic);
    const assets::ColorSpace requested = binding.interpretation ==
        render::TextureInterpretation::linear
        ? assets::ColorSpace::linear
        : binding.interpretation == render::TextureInterpretation::srgb
            ? assets::ColorSpace::srgb
            : expected;
    if (requested != expected)
        return unsupported("color interpretation; it conflicts with the texture semantic");
    return Result<void>::success();
}

std::array<render::SamplerId, 5U> material_draw_samplers(
    const render::StandardMaterial& material) {
    const std::array<const render::TextureRef*, 5U> bindings{
        &material.base_color, &material.metallic_roughness, &material.normal,
        &material.occlusion, &material.emissive};
    std::array<render::SamplerId, 5U> result{};
    for (std::size_t slot = 0U; slot < bindings.size(); ++slot) {
        if (static_cast<bool>(*bindings[slot])) result[slot] = bindings[slot]->sampler;
    }
    return result;
}

std::array<render::UvSetId, 5U> material_draw_uv_sets(
    const render::StandardMaterial& material) {
    const std::array<const render::TextureRef*, 5U> bindings{
        &material.base_color, &material.metallic_roughness, &material.normal,
        &material.occlusion, &material.emissive};
    std::array<render::UvSetId, 5U> result{};
    result.fill(render::kUv0);
    for (std::size_t slot = 0U; slot < bindings.size(); ++slot) {
        if (static_cast<bool>(*bindings[slot])) result[slot] = bindings[slot]->uv_set;
    }
    return result;
}

float channel_selector(render::TextureChannel channel) {
    switch (channel) {
    case render::TextureChannel::red: return 0.0F;
    case render::TextureChannel::green: return 1.0F;
    case render::TextureChannel::blue: return 2.0F;
    case render::TextureChannel::alpha: return 3.0F;
    case render::TextureChannel::one_minus_red: return 4.0F;
    case render::TextureChannel::one_minus_green: return 5.0F;
    case render::TextureChannel::one_minus_blue: return 6.0F;
    case render::TextureChannel::one_minus_alpha: return 7.0F;
    case render::TextureChannel::constant_zero: return 8.0F;
    case render::TextureChannel::constant_one: return 9.0F;
    }
    return 8.0F;
}

std::array<std::array<float, 4U>, 5U> material_draw_channel_mappings(
    const render::StandardMaterial& material) {
    const std::array<const render::TextureRef*, 5U> bindings{
        &material.base_color, &material.metallic_roughness, &material.normal,
        &material.occlusion, &material.emissive};
    std::array<std::array<float, 4U>, 5U> result{};
    for (std::size_t slot = 0U; slot < bindings.size(); ++slot) {
        for (std::size_t component = 0U; component < 4U; ++component) {
            result[slot][component] = channel_selector(
                bindings[slot]->channels.rgba[component]);
        }
    }
    return result;
}

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}
bool narrowable(double value) {
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}
bool narrowable(core::Vec3d value) {
    return narrowable(value.x) && narrowable(value.y) && narrowable(value.z);
}
using UvAffine = std::array<double, 6U>;
using MaterialUvAffines = std::array<UvAffine, 5U>;

struct SceneMaterialUvState {
    MaterialUvAffines transforms{};
    bool open_gl_y_plus_normal = false;
    std::array<render::SamplerId, 5U> samplers{};
    std::array<render::UvSetId, 5U> uv_sets{};
    std::array<std::array<float, 4U>, 5U> channel_mappings{};
    float sheen_weight = 0.0F;
    float anisotropy = 0.0F;
};

Result<UvAffine> texture_uv_affine(const render::TextureTransform& transform) {
    if (auto valid = transform.validate(); !valid)
        return Result<UvAffine>::failure(valid.error());
    // Authored mapping: pivot + offset + R*S*(uv - pivot).
    const double cosine = std::cos(transform.rotation);
    const double sine = std::sin(transform.rotation);
    const double a = cosine * transform.scale.x;
    const double b = -sine * transform.scale.y;
    const double c = sine * transform.scale.x;
    const double d = cosine * transform.scale.y;
    const double translate_u = transform.pivot.x + transform.offset.x -
        a * transform.pivot.x - b * transform.pivot.y;
    const double translate_v = transform.pivot.y + transform.offset.y -
        c * transform.pivot.x - d * transform.pivot.y;
    const UvAffine result{a, b, c, d, translate_u, translate_v};
    if (!std::all_of(result.begin(), result.end(),
            [](double value) { return std::isfinite(value); })) {
        return Result<UvAffine>::failure(invalid(
            "scene texture transform overflows its affine representation"));
    }
    return Result<UvAffine>::success(result);
}

Result<MaterialUvAffines> material_uv_affines(
    const render::StandardMaterial& material) {
    const std::array<const render::TextureRef*, 5U> bindings{
        &material.base_color, &material.metallic_roughness, &material.normal,
        &material.occlusion, &material.emissive};
    MaterialUvAffines result{};
    for (std::size_t slot = 0U; slot < bindings.size(); ++slot) {
        const auto affine = texture_uv_affine(bindings[slot]->transform);
        if (!affine) return Result<MaterialUvAffines>::failure(affine.error());
        result[slot] = affine.value();
    }
    return Result<MaterialUvAffines>::success(result);
}

Result<std::array<float, 2U>> transform_scene_uv(
    core::Vec2d uv,
    const UvAffine& transform) {
    const double u = transform[0U] * uv.x + transform[1U] * uv.y + transform[4U];
    const double v = transform[2U] * uv.x + transform[3U] * uv.y + transform[5U];
    if (!narrowable(u) || !narrowable(v))
        return Result<std::array<float, 2U>>::failure(invalid(
            "scene transformed UV coordinate overflows float32"));
    return Result<std::array<float, 2U>>::success({
        static_cast<float>(u), static_cast<float>(v)});
}

Result<void> validate_material_uv_bounds(
    const render::StandardMaterial& material,
    const production::RenderMeshArtifact& mesh,
    render::UvSetId uv0_set,
    std::optional<render::UvSetId> uv1_set) {
    const std::array<const render::TextureRef*, 5U> bindings{
        &material.base_color, &material.metallic_roughness, &material.normal,
        &material.occlusion, &material.emissive};
    if (std::none_of(bindings.begin(), bindings.end(),
            [](const auto* binding) { return static_cast<bool>(*binding); })) {
        return Result<void>::success();
    }
    const auto transforms = material_uv_affines(material);
    if (!transforms) return Result<void>::failure(transforms.error());
    for (std::size_t slot = 0U; slot < bindings.size(); ++slot) {
        if (!static_cast<bool>(*bindings[slot])) continue;
        if (slot == 2U && bindings[slot]->uv_set != uv0_set) {
            return Result<void>::failure({core::ErrorCode::unsupported,
                "normal-map UV set must match the primary tangent UV stream"});
        }
        const auto uv_for_vertex = [&](const production::RenderMeshVertex& vertex)
                -> std::optional<core::Vec2d> {
            if (bindings[slot]->uv_set == uv0_set) return vertex.uv;
            if (uv1_set.has_value() && bindings[slot]->uv_set == *uv1_set)
                return vertex.uv1;
            const auto additional = std::lower_bound(
                mesh.additional_uv_set_ids.begin(), mesh.additional_uv_set_ids.end(),
                bindings[slot]->uv_set);
            if (additional == mesh.additional_uv_set_ids.end() ||
                *additional != bindings[slot]->uv_set) return std::nullopt;
            const auto index = static_cast<std::size_t>(std::distance(
                mesh.additional_uv_set_ids.begin(), additional));
            if (index >= vertex.additional_uvs.size()) return std::nullopt;
            return vertex.additional_uvs[index];
        };
        if (mesh.vertices.empty() || !uv_for_vertex(mesh.vertices.front()).has_value()) {
            return Result<void>::failure({core::ErrorCode::unsupported,
                "native scene texture sampling requires authored UV set " +
                    std::to_string(bindings[slot]->uv_set)});
        }
        core::Vec2d minimum = *uv_for_vertex(mesh.vertices.front());
        core::Vec2d maximum = minimum;
        for (const auto& vertex : mesh.vertices) {
            const auto uv = uv_for_vertex(vertex);
            if (!uv.has_value()) {
                return Result<void>::failure({core::ErrorCode::unsupported,
                    "native scene texture sampling requires authored UV set " +
                        std::to_string(bindings[slot]->uv_set)});
            }
            minimum.x = std::min(minimum.x, uv->x);
            minimum.y = std::min(minimum.y, uv->y);
            maximum.x = std::max(maximum.x, uv->x);
            maximum.y = std::max(maximum.y, uv->y);
        }
        const std::array<core::Vec2d, 4U> corners{{
            {minimum.x, minimum.y}, {minimum.x, maximum.y},
            {maximum.x, minimum.y}, {maximum.x, maximum.y}}};
        if (slot == 2U) {
            const auto& normal_transform = transforms.value()[slot];
            const double determinant = normal_transform[0U] * normal_transform[3U] -
                normal_transform[1U] * normal_transform[2U];
            if (!std::isfinite(determinant) || determinant == 0.0)
                return Result<void>::failure(invalid(
                    "normal-map UV transform is singular or overflows"));
        }
        for (const auto corner : corners) {
            if (auto transformed = transform_scene_uv(corner, transforms.value()[slot]);
                !transformed) {
                return Result<void>::failure(transformed.error());
            }
        }
    }
    return Result<void>::success();
}
template <typename T> std::span<const std::uint8_t> bytes(const T& values) {
    return {reinterpret_cast<const std::uint8_t*>(values.data()),
            values.size() * sizeof(typename T::value_type)};
}
template <typename T> std::vector<std::uint8_t> push_bytes(const T& values) {
    const auto view = bytes(values);
    return {view.begin(), view.end()};
}
Matrix multiply(const Matrix& left, const Matrix& right) {
    Matrix result{};
    for (std::size_t column = 0U; column < 4U; ++column)
        for (std::size_t row = 0U; row < 4U; ++row)
            for (std::size_t k = 0U; k < 4U; ++k)
                result[column * 4U + row] += left[k * 4U + row] * right[column * 4U + k];
    return result;
}
Result<Matrix> view_projection(const SceneRenderRequest& request) {
    const auto& camera = request.camera;
    const bool valid_projection =
        camera.projection == SceneRenderProjection::perspective ||
        camera.projection == SceneRenderProjection::orthographic;
    if (!narrowable(camera.position) || !narrowable(camera.target) ||
        !narrowable(camera.up) || !valid_projection ||
        !request.subpixel_jitter_pixels.finite() ||
        std::abs(request.subpixel_jitter_pixels.x) > 0.5 ||
        std::abs(request.subpixel_jitter_pixels.y) > 0.5 ||
        !std::isfinite(camera.vertical_fov) ||
        (camera.projection == SceneRenderProjection::perspective &&
         (camera.vertical_fov < 0.01 || camera.vertical_fov > 3.13)) ||
        !std::isfinite(camera.orthographic_height) ||
        (camera.projection == SceneRenderProjection::orthographic &&
         (camera.orthographic_height <= 0.0 ||
          camera.orthographic_height > 1.0e8)) ||
        !std::isfinite(camera.near_plane) || camera.near_plane < 0.0001 ||
        !std::isfinite(camera.far_plane) || camera.far_plane <= camera.near_plane ||
        camera.far_plane > 1.0e8 || !std::isfinite(camera.exposure_ev) ||
        std::abs(camera.exposure_ev) > 20.0) {
        return Result<Matrix>::failure(invalid(
            "scene render camera or pixel jitter exceeds finite projection bounds"));
    }
    const auto forward = (camera.target - camera.position).normalized();
    const auto right = core::cross(forward, camera.up.normalized()).normalized();
    const auto up = core::cross(right, forward).normalized();
    if (!forward.finite() || !right.finite() || !up.finite())
        return Result<Matrix>::failure(invalid("scene render camera requires distinct target and nonparallel up"));
    const std::array<double, 16U> source_view{
        right.x, up.x, -forward.x, 0.0,
        right.y, up.y, -forward.y, 0.0,
        right.z, up.z, -forward.z, 0.0,
        -core::dot(right, camera.position), -core::dot(up, camera.position),
        core::dot(forward, camera.position), 1.0};
    Matrix view{};
    for (std::size_t i = 0U; i < view.size(); ++i) {
        if (!narrowable(source_view[i])) return Result<Matrix>::failure(invalid("camera view overflows float32"));
        view[i] = static_cast<float>(source_view[i]);
    }
    Matrix projection{};
    if (camera.projection == SceneRenderProjection::perspective) {
        const double f = 1.0 / std::tan(camera.vertical_fov * 0.5);
        projection[0U] = static_cast<float>(f * request.height / request.width);
        projection[5U] = static_cast<float>(f);
        projection[10U] = static_cast<float>(
            camera.far_plane / (camera.near_plane - camera.far_plane));
        projection[11U] = -1.0F;
        projection[14U] = static_cast<float>(camera.far_plane * camera.near_plane /
                                           (camera.near_plane - camera.far_plane));
    } else {
        const double orthographic_width = camera.orthographic_height *
            static_cast<double>(request.width) / request.height;
        const double depth_scale = 1.0 /
            (camera.near_plane - camera.far_plane);
        if (!narrowable(orthographic_width) || orthographic_width <= 0.0 ||
            !narrowable(depth_scale)) {
            return Result<Matrix>::failure(invalid(
                "orthographic camera extent exceeds float32 projection bounds"));
        }
        projection[0U] = static_cast<float>(2.0 / orthographic_width);
        projection[5U] = static_cast<float>(2.0 / camera.orthographic_height);
        projection[10U] = static_cast<float>(depth_scale);
        projection[14U] = static_cast<float>(camera.near_plane * depth_scale);
        projection[15U] = 1.0F;
    }
    if (camera.projection == SceneRenderProjection::perspective) {
        // Clip-space Z is negative in front of the camera and W is -Z. These
        // signs produce the declared right/down output-pixel convention.
        projection[8U] = static_cast<float>(
            -2.0 * request.subpixel_jitter_pixels.x / request.width);
        projection[9U] = static_cast<float>(
            2.0 * request.subpixel_jitter_pixels.y / request.height);
    } else {
        projection[12U] += static_cast<float>(
            2.0 * request.subpixel_jitter_pixels.x / request.width);
        projection[13U] -= static_cast<float>(
            2.0 * request.subpixel_jitter_pixels.y / request.height);
    }
    const auto result = multiply(projection, view);
    if (!std::all_of(result.begin(), result.end(), [](float x) { return std::isfinite(x); }))
        return Result<Matrix>::failure(invalid("camera projection overflows float32"));
    return Result<Matrix>::success(result);
}
Result<Matrix> model_matrix(const core::Transform& transform) {
    if (!transform.finite() || !narrowable(transform.translation))
        return Result<Matrix>::failure(invalid("scene render transform must be finite"));
    for (const double scale : {transform.scale.x, transform.scale.y, transform.scale.z}) {
        if (std::abs(scale) < 0.0001 || std::abs(scale) > 10000.0)
            return Result<Matrix>::failure(invalid("scene render scale must be invertible within [1e-4,1e4] magnitude"));
    }
    const auto x = transform.rotation.rotate({transform.scale.x, 0.0, 0.0});
    const auto y = transform.rotation.rotate({0.0, transform.scale.y, 0.0});
    const auto z = transform.rotation.rotate({0.0, 0.0, transform.scale.z});
    return Result<Matrix>::success({
        static_cast<float>(x.x), static_cast<float>(x.y), static_cast<float>(x.z), 0.0F,
        static_cast<float>(y.x), static_cast<float>(y.y), static_cast<float>(y.z), 0.0F,
        static_cast<float>(z.x), static_cast<float>(z.y), static_cast<float>(z.z), 0.0F,
        static_cast<float>(transform.translation.x), static_cast<float>(transform.translation.y),
        static_cast<float>(transform.translation.z), 1.0F});
}
Result<Matrix> shadow_view_projection(const SceneRenderRequest& request) {
    const auto spot_shadow = std::find_if(request.local_lights.begin(),
        request.local_lights.end(), [](const SceneRenderLocalLight& light) {
            return light.casts_shadows;
        });
    if (spot_shadow != request.local_lights.end()) {
        if (spot_shadow->kind != SceneRenderLocalLightKind::spot ||
            spot_shadow->range < kSceneRenderLocalShadowMinimumRangeMeters ||
            spot_shadow->outer_cone_radians <
                kSceneRenderLocalShadowMinimumOuterConeRadians) {
            return Result<Matrix>::failure({core::ErrorCode::unsupported,
                "local shadow projection requires a bounded spotlight cone and range"});
        }
        const core::Vec3d forward =
            spot_shadow->emission_direction.normalized();
        const core::Vec3d reference_up = std::abs(forward.y) > 0.95
            ? core::Vec3d{0.0, 0.0, 1.0}
            : core::Vec3d{0.0, 1.0, 0.0};
        const core::Vec3d right =
            core::cross(forward, reference_up).normalized();
        const core::Vec3d up = core::cross(right, forward).normalized();
        const core::Vec3d eye = spot_shadow->position;
        const std::array<double, 16U> source_view{
            right.x, up.x, -forward.x, 0.0,
            right.y, up.y, -forward.y, 0.0,
            right.z, up.z, -forward.z, 0.0,
            -core::dot(right, eye), -core::dot(up, eye),
            core::dot(forward, eye), 1.0};
        Matrix view{};
        for (std::size_t i = 0U; i < view.size(); ++i) {
            if (!narrowable(source_view[i]))
                return Result<Matrix>::failure(invalid(
                    "spot shadow view exceeds float32 bounds"));
            view[i] = static_cast<float>(source_view[i]);
        }
        const double near_plane = 0.01;
        const double far_plane = spot_shadow->range;
        const double focal_length =
            1.0 / std::tan(spot_shadow->outer_cone_radians);
        if (!narrowable(eye) || !narrowable(far_plane) ||
            !narrowable(focal_length) || !right.finite() || !up.finite() ||
            far_plane <= near_plane || focal_length <= 0.0) {
            return Result<Matrix>::failure(invalid(
                "spot shadow projection exceeds finite float32 bounds"));
        }
        Matrix projection{};
        projection[0U] = static_cast<float>(focal_length);
        projection[5U] = static_cast<float>(focal_length);
        projection[10U] = static_cast<float>(
            far_plane / (near_plane - far_plane));
        projection[11U] = -1.0F;
        projection[14U] = static_cast<float>(
            far_plane * near_plane / (near_plane - far_plane));
        const Matrix result = multiply(projection, view);
        if (!std::all_of(result.begin(), result.end(),
                [](float value) { return std::isfinite(value); })) {
            return Result<Matrix>::failure(invalid(
                "spot shadow projection overflows float32"));
        }
        return Result<Matrix>::success(result);
    }

    core::Vec3d minimum{};
    core::Vec3d maximum{};
    bool first = true;
    for (const auto& object : request.objects) {
        for (const auto& vertex : object.mesh.vertices) {
            const auto world = object.world_transform.translation + object.world_transform.rotation.rotate(
                core::componentwise_multiply(object.world_transform.scale, vertex.position));
            if (first) {
                minimum = world;
                maximum = world;
                first = false;
            } else {
                minimum = {std::min(minimum.x, world.x), std::min(minimum.y, world.y),
                    std::min(minimum.z, world.z)};
                maximum = {std::max(maximum.x, world.x), std::max(maximum.y, world.y),
                    std::max(maximum.z, world.z)};
            }
        }
    }
    if (first) return Result<Matrix>::failure(invalid("shadow projection requires scene vertices"));

    const core::Vec3d center = minimum + (maximum - minimum) * 0.5;
    double radius = 0.0;
    for (const auto& object : request.objects) {
        for (const auto& vertex : object.mesh.vertices) {
            const auto world = object.world_transform.translation + object.world_transform.rotation.rotate(
                core::componentwise_multiply(object.world_transform.scale, vertex.position));
            radius = std::max(radius, (world - center).length());
        }
    }
    radius = std::max(radius, 0.01);
    const double padding = std::max(0.01, radius * 0.05);
    const double near_plane = 0.001;
    const double far_plane = radius * 2.0 + padding * 2.0;
    const double half_extent = radius + padding;
    const auto toward_light = request.light.direction_to_light.normalized();
    const core::Vec3d forward{-toward_light.x, -toward_light.y, -toward_light.z};
    const core::Vec3d reference_up = std::abs(forward.y) > 0.95
        ? core::Vec3d{0.0, 0.0, 1.0} : core::Vec3d{0.0, 1.0, 0.0};
    const auto right = core::cross(forward, reference_up).normalized();
    const auto up = core::cross(right, forward).normalized();
    if (!narrowable(center) || !narrowable(radius) || !narrowable(padding) ||
        !narrowable(far_plane) || !narrowable(half_extent) ||
        !right.finite() || !up.finite()) {
        return Result<Matrix>::failure(invalid("directional shadow bounds exceed float32 projection range"));
    }

    const core::Vec3d eye = center - forward * (radius + padding);
    const std::array<double, 16U> source_view{
        right.x, up.x, -forward.x, 0.0,
        right.y, up.y, -forward.y, 0.0,
        right.z, up.z, -forward.z, 0.0,
        -core::dot(right, eye), -core::dot(up, eye), core::dot(forward, eye), 1.0};
    Matrix view{};
    for (std::size_t i = 0U; i < view.size(); ++i) {
        if (!narrowable(source_view[i]))
            return Result<Matrix>::failure(invalid("directional shadow view overflows float32"));
        view[i] = static_cast<float>(source_view[i]);
    }
    Matrix projection{};
    projection[0U] = static_cast<float>(1.0 / half_extent);
    projection[5U] = static_cast<float>(1.0 / half_extent);
    projection[10U] = static_cast<float>(1.0 / (near_plane - far_plane));
    projection[14U] = static_cast<float>(near_plane / (near_plane - far_plane));
    projection[15U] = 1.0F;
    const auto result = multiply(projection, view);
    if (!std::all_of(result.begin(), result.end(), [](float value) { return std::isfinite(value); }))
        return Result<Matrix>::failure(invalid("directional shadow projection overflows float32"));
    return Result<Matrix>::success(result);
}
std::array<float, 4U> transform_position(const Matrix& matrix, const std::array<float, 4U>& position) {
    std::array<float, 4U> transformed{};
    for (std::size_t row = 0U; row < transformed.size(); ++row) {
        for (std::size_t column = 0U; column < position.size(); ++column)
            transformed[row] += matrix[column * 4U + row] * position[column];
    }
    return transformed;
}
std::uint64_t texture_id(const render::GpuMaterialConstants& material, std::size_t slot) {
    return material.texture_asset_ids[slot * 2U] |
        (static_cast<std::uint64_t>(material.texture_asset_ids[slot * 2U + 1U]) << 32U);
}
void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}
void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}
void append_double(std::vector<std::uint8_t>& output, double value) {
    append_u64(output, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}
void append_sampler(std::vector<std::uint8_t>& output,
    render::SamplerId identity, const gpu::SamplerDesc& sampler) {
    append_u64(output, identity);
    append_u64(output, static_cast<std::uint64_t>(sampler.min_filter));
    append_u64(output, static_cast<std::uint64_t>(sampler.mag_filter));
    append_u64(output, static_cast<std::uint64_t>(sampler.mip_filter));
    append_u64(output, static_cast<std::uint64_t>(sampler.u));
    append_u64(output, static_cast<std::uint64_t>(sampler.v));
    append_u64(output, static_cast<std::uint64_t>(sampler.w));
    append_u64(output, sampler.anisotropy ? 1U : 0U);
    append_u32(output, std::bit_cast<std::uint32_t>(
        sampler.max_anisotropy == 0.0F ? 0.0F : sampler.max_anisotropy));
    append_u64(output, sampler.compare ? 1U : 0U);
    append_u64(output, static_cast<std::uint64_t>(sampler.compare_op));
    append_u32(output, std::bit_cast<std::uint32_t>(
        sampler.min_lod == 0.0F ? 0.0F : sampler.min_lod));
    append_u32(output, std::bit_cast<std::uint32_t>(
        sampler.max_lod == 0.0F ? 0.0F : sampler.max_lod));
}
void append_digest(std::vector<std::uint8_t>& output, const assets::Sha256Digest& value) {
    output.insert(output.end(), value.bytes.begin(), value.bytes.end());
}
void append_vec(std::vector<std::uint8_t>& output, core::Vec3d value) {
    append_double(output, value.x); append_double(output, value.y); append_double(output, value.z);
}
assets::Sha256Digest request_digest(const SceneRenderRequest& request) {
    constexpr std::string_view algorithm = kSceneRenderAlgorithm;
    std::vector<std::uint8_t> data(algorithm.begin(), algorithm.end());
    append_u64(data, request.source_revision.value()); append_digest(data, request.source_digest);
    append_u64(data, request.width); append_u64(data, request.height);
    append_u64(data, static_cast<std::uint64_t>(request.antialiasing));
    append_vec(data, request.camera.position); append_vec(data, request.camera.target);
    append_vec(data, request.camera.up);
    append_u64(data, static_cast<std::uint64_t>(request.camera.projection));
    append_double(data, request.camera.vertical_fov);
    append_double(data, request.camera.orthographic_height);
    append_double(data, request.subpixel_jitter_pixels.x);
    append_double(data, request.subpixel_jitter_pixels.y);
    append_double(data, request.camera.near_plane); append_double(data, request.camera.far_plane);
    append_double(data, request.camera.exposure_ev); append_vec(data, request.light.direction_to_light);
    append_vec(data, request.light.radiance);
    append_u64(data, request.local_lights.size());
    for (const auto& light : request.local_lights) {
        append_u64(data, light.object.value);
        append_u64(data, static_cast<std::uint64_t>(light.kind));
        append_vec(data, light.position);
        append_vec(data, light.emission_direction);
        append_vec(data, light.radiance);
        append_double(data, light.range);
        append_double(data, light.inner_cone_radians);
        append_double(data, light.outer_cone_radians);
        append_u64(data, light.casts_shadows ? 1U : 0U);
    }
    append_vec(data, request.background_linear);
    append_u64(data, request.objects.size());
    for (const auto& object : request.objects) {
        append_u64(data, object.object.value); append_digest(data, object.draw_package.package_digest);
        append_vec(data, object.world_transform.translation); append_vec(data, object.world_transform.scale);
        append_u64(data, object.uv0_set);
        append_u64(data, object.uv1_set.has_value() ? 1U : 0U);
        if (object.uv1_set.has_value()) append_u64(data, *object.uv1_set);
        append_u64(data, object.mesh.additional_uv_set_ids.size());
        for (const auto uv_set : object.mesh.additional_uv_set_ids)
            append_u64(data, uv_set);
        const auto rotation = object.world_transform.rotation.normalized();
        for (double value : {rotation.x, rotation.y, rotation.z, rotation.w}) append_double(data, value);
        append_u64(data, object.materials.size());
        for (const auto& binding : object.materials) {
            append_u64(data, binding.material_slot.has_value()
                ? static_cast<std::uint64_t>(*binding.material_slot) + 1U : 0U);
            append_double(data, binding.sheen_weight);
            append_double(data, binding.anisotropy);
            for (const auto sampler : material_draw_samplers(
                     binding.current_material.definition))
                append_u64(data, sampler);
            for (const auto uv_set : material_draw_uv_sets(
                     binding.current_material.definition))
                append_u64(data, uv_set);
            for (const auto& mapping : material_draw_channel_mappings(
                     binding.current_material.definition)) {
                for (const float selector : mapping)
                    append_u32(data, std::bit_cast<std::uint32_t>(selector));
            }
        }
    }
    append_u64(data, request.textures.size());
    for (const auto& texture : request.textures) {
        append_u64(data, texture.asset); append_digest(data, texture.current_source_digest);
        append_digest(data, texture.mips.artifact_digest);
    }
    append_u64(data, request.samplers.size());
    for (const auto& [identity, sampler] : request.samplers)
        append_sampler(data, identity, sampler);
    append_u64(data, request.temporal.has_value() ? 1U : 0U);
    if (request.temporal.has_value()) {
        append_digest(data, request.temporal->history_content_digest);
        append_digest(data, request.temporal->history_request_digest);
        append_u64(data, request.temporal->history_source_revision.value());
        append_digest(data, request.temporal->history_source_digest);
        append_u64(data, static_cast<std::uint64_t>(
            request.temporal->history_initial_state));
        append_digest(data, request.temporal->motion_vectors_digest);
        append_digest(data, request.temporal->motion_history_request_digest);
        append_u64(data, static_cast<std::uint64_t>(
            request.temporal->motion_initial_state));
        append_u64(data, static_cast<std::uint64_t>(
            request.temporal->resolved_initial_state));
        append_u32(data, std::bit_cast<std::uint32_t>(
            request.temporal->feedback == 0.0F ? 0.0F :
                request.temporal->feedback));
        append_u64(data, request.temporal->camera_cut ? 1U : 0U);
        append_u64(data, request.temporal->zero_motion ? 1U : 0U);
        append_u64(data,
            request.temporal->constant_motion_pixels.has_value() ? 1U : 0U);
        if (request.temporal->constant_motion_pixels.has_value()) {
            append_double(data, request.temporal->constant_motion_pixels->x);
            append_double(data, request.temporal->constant_motion_pixels->y);
        }
    }
    append_u64(data, request.environment.has_value() ? 1U : 0U);
    if (request.environment.has_value()) {
        append_digest(data, request.environment->source_digest);
        append_u64(data, request.environment->source_color_space.has_value()
            ? static_cast<std::uint64_t>(*request.environment->source_color_space) + 1U
            : 0U);
        for (const auto coefficient : request.environment->diffuse_irradiance_over_pi_sh)
            append_vec(data, coefficient);
        append_u64(data,
            request.environment->specular_environment_mips.has_value() ? 1U : 0U);
        if (request.environment->specular_environment_mips.has_value())
            append_digest(data,
                request.environment->specular_environment_mips->artifact_digest);
    }
    return assets::sha256(data);
}

assets::Sha256Digest mesh_cache_identity(const SceneRenderRequest& request) {
    constexpr std::string_view domain = "carto.d3d12.scene-mesh-cache.v9";
    std::vector<std::uint8_t> data(domain.begin(), domain.end());
    append_u64(data, request.source_revision.value());
    append_digest(data, request.source_digest);
    append_vec(data, request.light.direction_to_light);
    append_vec(data, request.camera.position);
    append_u64(data, request.local_lights.size());
    for (const auto& light : request.local_lights) {
        append_u64(data, light.object.value);
        append_u64(data, static_cast<std::uint64_t>(light.kind));
        append_vec(data, light.position);
        append_vec(data, light.emission_direction);
        append_double(data, light.range);
        append_double(data, light.inner_cone_radians);
        append_double(data, light.outer_cone_radians);
        append_u64(data, light.casts_shadows ? 1U : 0U);
    }
    append_u64(data, request.objects.size());
    for (const auto& object : request.objects) {
        append_u64(data, object.object.value);
        append_digest(data, object.draw_package.package_digest);
        append_digest(data, object.draw_package.mesh_artifact_digest);
        append_u64(data, object.uv0_set);
        append_u64(data, object.uv1_set.has_value() ? 1U : 0U);
        if (object.uv1_set.has_value()) append_u64(data, *object.uv1_set);
        append_u64(data, object.mesh.additional_uv_set_ids.size());
        for (const auto uv_set : object.mesh.additional_uv_set_ids)
            append_u64(data, uv_set);
        for (const auto& binding : object.materials) {
            append_u64(data, binding.material_slot.has_value()
                ? static_cast<std::uint64_t>(*binding.material_slot) + 1U : 0U);
            append_double(data, binding.sheen_weight);
            append_double(data, binding.anisotropy);
            for (const auto& mapping : material_draw_channel_mappings(
                     binding.current_material.definition)) {
                for (const float selector : mapping)
                    append_u32(data, std::bit_cast<std::uint32_t>(selector));
            }
        }
        append_vec(data, object.world_transform.translation);
        append_vec(data, object.world_transform.scale);
        const auto rotation = object.world_transform.rotation.normalized();
        for (double value : {rotation.x, rotation.y, rotation.z, rotation.w})
            append_double(data, value);
    }
    return assets::sha256(data);
}

assets::Sha256Digest material_texture_cache_identity(
    const SceneRenderRequest& request) {
    constexpr std::string_view domain = "carto.d3d12.scene-material-cache.v6";
    std::vector<std::uint8_t> data(domain.begin(), domain.end());
    append_u64(data, request.source_revision.value());
    append_digest(data, request.source_digest);
    append_u64(data, request.local_lights.size());
    for (const auto& light : request.local_lights) {
        append_u64(data, light.object.value);
        append_u64(data, static_cast<std::uint64_t>(light.kind));
        append_vec(data, light.position);
        append_vec(data, light.emission_direction);
        append_vec(data, light.radiance);
        append_double(data, light.range);
        append_double(data, light.inner_cone_radians);
        append_double(data, light.outer_cone_radians);
        append_u64(data, light.casts_shadows ? 1U : 0U);
    }
    append_u64(data, request.textures.size());
    for (const auto& texture : request.textures) {
        append_u64(data, texture.asset);
        append_digest(data, texture.current_source_digest);
        append_digest(data, texture.mips.artifact_digest);
    }
    append_u64(data, request.samplers.size());
    for (const auto& [identity, sampler] : request.samplers)
        append_sampler(data, identity, sampler);
    append_u64(data, request.environment.has_value() ? 1U : 0U);
    if (request.environment.has_value()) {
        append_digest(data, request.environment->source_digest);
        append_u64(data, request.environment->source_color_space.has_value()
            ? static_cast<std::uint64_t>(*request.environment->source_color_space) + 1U
            : 0U);
        for (const auto coefficient : request.environment->diffuse_irradiance_over_pi_sh)
            append_vec(data, coefficient);
        append_u64(data,
            request.environment->specular_environment_mips.has_value() ? 1U : 0U);
        if (request.environment->specular_environment_mips.has_value())
            append_digest(data,
                request.environment->specular_environment_mips->artifact_digest);
    }
    return assets::sha256(data);
}

// Owns handles for one render or cache entry. A submission with no returned
// serial remains uncertain and waits before retirement; successful submissions
// retire handles through the device's fence-keyed deferred-destruction queue.
struct Resources {
    D3D12Device& device;
    std::vector<gpu::BufferHandle> buffers;
    std::vector<gpu::TextureHandle> textures;
    std::vector<gpu::SamplerHandle> samplers;
    std::vector<gpu::ShaderHandle> shaders;
    std::vector<gpu::PipelineHandle> pipelines;
    bool submitted = false;
    explicit Resources(D3D12Device& owner) : device(owner) {}
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;
    ~Resources() {
        if (submitted) static_cast<void>(device.wait_idle());
        for (auto handle : pipelines) static_cast<void>(device.destroy_pipeline(handle));
        for (auto handle : shaders) static_cast<void>(device.destroy_shader(handle));
        for (auto handle : samplers) static_cast<void>(device.destroy_sampler(handle));
        for (auto handle : textures) static_cast<void>(device.destroy_texture(handle));
        for (auto handle : buffers) static_cast<void>(device.destroy_buffer(handle));
    }
    Result<gpu::BufferHandle> buffer(const gpu::BufferDesc& desc) {
        auto result = device.create_buffer(desc); if (result) buffers.push_back(result.value()); return result;
    }
    Result<gpu::TextureHandle> texture(const gpu::TextureDesc& desc) {
        auto result = device.create_texture(desc); if (result) textures.push_back(result.value()); return result;
    }
};
void transition(device_ir::DeviceCommandStream& stream, device_ir::ResourceHandle handle,
                device_ir::ResourceState before, device_ir::ResourceState after) {
    stream.append(device_ir::CmdTransitionResource{handle, before, after});
}
Result<std::string> shader_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 1024 * 1024)
        return Result<std::string>::failure({core::ErrorCode::io_error, "scene shader missing or exceeds 1 MiB"});
    std::string source(static_cast<std::size_t>(input.tellg()), '\0');
    input.seekg(0); input.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (!input) return Result<std::string>::failure({core::ErrorCode::io_error, "scene shader read failed"});
    return Result<std::string>::success(std::move(source));
}

struct SceneProgram {
    std::array<gpu::ShaderHandle, 7U> shaders{};
    std::array<assets::Sha256Digest, 7U> shader_digests{};
    gpu::PipelineHandle pbr_pipeline;
    gpu::PipelineHandle pbr_blended_pipeline;
    gpu::PipelineHandle shadow_pipeline;
    gpu::PipelineHandle tone_pipeline;
    gpu::PipelineHandle temporal_pipeline;
    gpu::SamplerHandle sampler;
};

struct SceneMeshBuffers {
    gpu::BufferHandle vertices;
    gpu::BufferHandle indices;
    std::uint64_t vertex_bytes = 0U;
    std::uint64_t index_bytes = 0U;
};
struct SceneDrawRange {
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;
};
struct SceneDrawMeshBuffers {
    SceneMeshBuffers buffers;
    std::vector<SceneDrawRange> draws;
};

struct SceneMaterialTextures {
    std::map<assets::TextureAssetId, gpu::TextureHandle> source_textures;
    std::map<render::SamplerId, gpu::SamplerHandle> samplers;
    std::array<gpu::TextureHandle, 5U> neutral{};
    gpu::TextureHandle environment_texture;
    gpu::TextureHandle split_sum_brdf_lut;
    gpu::TextureHandle local_lights_texture;
};

struct SceneRenderTargets {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t shadow_extent = 0U;
    gpu::TextureHandle hdr;
    gpu::TextureHandle depth;
    gpu::TextureHandle shadow_color;
    gpu::TextureHandle shadow_depth;
    gpu::TextureHandle display;
    device_ir::ResourceState hdr_state = device_ir::ResourceState::undefined;
    device_ir::ResourceState depth_state = device_ir::ResourceState::undefined;
    device_ir::ResourceState shadow_color_state = device_ir::ResourceState::undefined;
    device_ir::ResourceState shadow_depth_state = device_ir::ResourceState::undefined;
    device_ir::ResourceState display_state = device_ir::ResourceState::undefined;
    bool states_known = false;

    [[nodiscard]] bool matches(
        std::uint32_t candidate_width,
        std::uint32_t candidate_height,
        std::uint32_t candidate_shadow_extent) const noexcept {
        return width == candidate_width && height == candidate_height &&
            shadow_extent == candidate_shadow_extent && static_cast<bool>(hdr) &&
            static_cast<bool>(depth) && static_cast<bool>(shadow_color) &&
            static_cast<bool>(shadow_depth);
    }
};

constexpr std::uint64_t kSceneTargetCacheBudgetBytes = 128ULL * 1024ULL * 1024ULL;

// This is a conservative logical texel-byte estimate for the retained scene
// and shadow attachments. Requests above the bound use temporary resources.
std::uint64_t scene_target_allocation_estimate(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t shadow_extent,
    bool include_display) {
    const std::uint64_t scene_pixels = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t shadow_pixels =
        static_cast<std::uint64_t>(shadow_extent) * shadow_extent;
    constexpr std::uint64_t kRgba32Bytes = 4ULL * sizeof(float);
    constexpr std::uint64_t kDepth32Bytes = sizeof(float);
    return scene_pixels * (kRgba32Bytes + kDepth32Bytes +
        (include_display ? kRgba32Bytes : 0ULL)) +
        shadow_pixels * (kRgba32Bytes + kDepth32Bytes);
}

Result<SceneProgram> build_scene_program(
    D3D12Device& device,
    Resources& resources,
    const std::array<std::string, 7U>& sources,
    gpu::Format tone_map_format) {
    SceneProgram program;
    for (std::size_t i = 0U; i < program.shaders.size(); ++i) {
        const std::string name = i < 2U ? "cartographer_scene_pbr.hlsl" :
            i < 4U ? "cartographer_scene_shadow.hlsl" :
            i < 6U ? "cartographer_tonemap.hlsl" : "cartographer_temporal.hlsl";
        const auto stage = i == 6U ? gpu::ShaderStage::compute :
            i % 2U == 0U ? gpu::ShaderStage::vertex : gpu::ShaderStage::fragment;
        const std::string entry_point = i == 6U ? "CSMain" :
            i % 2U == 0U ? "VSMain" : "PSMain";
        const auto binary = compile_hlsl({"scene_" + std::to_string(i),
            stage, entry_point, name, sources[i]});
        if (!binary) return Result<SceneProgram>::failure(binary.error());
        const auto digest = assets::Sha256Digest::from_hex(binary.value().binary_digest);
        if (!digest) return Result<SceneProgram>::failure(digest.error());
        program.shader_digests[i] = digest.value();
        const auto handle = device.create_shader(binary.value());
        if (!handle) return Result<SceneProgram>::failure(handle.error());
        program.shaders[i] = handle.value();
        resources.shaders.push_back(handle.value());
    }

    gpu::PipelineDesc pbr;
    pbr.debug_name = "cartographer_scene_pbr";
    pbr.shaders = {program.shaders[0U], program.shaders[1U]};
    for (std::uint32_t slot = 0U; slot < 9U; ++slot)
        pbr.descriptor_bindings.push_back({0U, slot, gpu::ShaderStage::fragment});
    pbr.sampled_texture_count = 9U;
    pbr.sampled_sampler_count = 5U;
    pbr.cull_mode = gpu::CullMode::none;
    pbr.color_format = gpu::Format::rgba32_float;
    pbr.push_constant_bytes = 192U;
    pbr.vertex_stride_bytes = static_cast<std::uint32_t>(sizeof(ScenePbrUploadVertex));
    pbr.vertex_attributes = {{"POSITION", 0U, gpu::VertexFormat::float4, 0U, 0U},
        {"NORMAL", 0U, gpu::VertexFormat::float3, 16U, 0U},
        {"TEXCOORD", 0U, gpu::VertexFormat::float2, 28U, 0U},
        {"TANGENT", 0U, gpu::VertexFormat::float4, 36U, 0U},
        {"TEXCOORD", 1U, gpu::VertexFormat::float4, 52U, 0U},
        {"TEXCOORD", 2U, gpu::VertexFormat::float2, 68U, 0U},
        {"TEXCOORD", 3U, gpu::VertexFormat::float2, 76U, 0U},
        {"TEXCOORD", 4U, gpu::VertexFormat::float2, 84U, 0U},
        {"TEXCOORD", 5U, gpu::VertexFormat::float2, 92U, 0U},
        {"TEXCOORD", 6U, gpu::VertexFormat::float4, 100U, 0U},
        {"TEXCOORD", 7U, gpu::VertexFormat::float4, 116U, 0U},
        {"TEXCOORD", 8U, gpu::VertexFormat::float4, 132U, 0U},
        {"COLOR", 0U, gpu::VertexFormat::float4, 148U, 0U},
        {"TEXCOORD", 9U, gpu::VertexFormat::float4, 164U, 0U},
        {"TEXCOORD", 10U, gpu::VertexFormat::float4, 180U, 0U},
        {"TEXCOORD", 11U, gpu::VertexFormat::float4, 196U, 0U}};
    const auto pbr_pipeline = device.create_pipeline(pbr);
    if (!pbr_pipeline) return Result<SceneProgram>::failure(pbr_pipeline.error());
    program.pbr_pipeline = pbr_pipeline.value();
    resources.pipelines.push_back(pbr_pipeline.value());

    auto pbr_blended = pbr;
    pbr_blended.debug_name = "cartographer_scene_pbr_blended";
    pbr_blended.depth_write = false;
    pbr_blended.blend = true;
    pbr_blended.source_color = gpu::BlendFactor::source_alpha;
    pbr_blended.destination_color = gpu::BlendFactor::one_minus_source_alpha;
    pbr_blended.color_blend = gpu::BlendOp::add;
    const auto pbr_blended_pipeline = device.create_pipeline(pbr_blended);
    if (!pbr_blended_pipeline)
        return Result<SceneProgram>::failure(pbr_blended_pipeline.error());
    program.pbr_blended_pipeline = pbr_blended_pipeline.value();
    resources.pipelines.push_back(pbr_blended_pipeline.value());

    gpu::PipelineDesc shadow;
    shadow.debug_name = "cartographer_scene_shadow";
    shadow.shaders = {program.shaders[2U], program.shaders[3U]};
    shadow.descriptor_bindings = {{0U, 0U, gpu::ShaderStage::fragment}};
    shadow.sampled_texture_count = 1U;
    shadow.cull_mode = gpu::CullMode::none;
    shadow.color_format = gpu::Format::rgba32_float;
    shadow.depth_format = gpu::Format::depth32_float;
    shadow.depth_test = true;
    shadow.depth_write = true;
    shadow.push_constant_bytes = 144U;
    shadow.vertex_stride_bytes = static_cast<std::uint32_t>(sizeof(ScenePbrUploadVertex));
    shadow.vertex_attributes = {{"POSITION", 0U, gpu::VertexFormat::float4, 0U, 0U},
        {"TEXCOORD", 0U, gpu::VertexFormat::float2, 28U, 0U},
        {"COLOR", 0U, gpu::VertexFormat::float4, 148U, 0U}};
    const auto shadow_pipeline = device.create_pipeline(shadow);
    if (!shadow_pipeline) return Result<SceneProgram>::failure(shadow_pipeline.error());
    program.shadow_pipeline = shadow_pipeline.value();
    resources.pipelines.push_back(shadow_pipeline.value());

    gpu::PipelineDesc tone;
    tone.debug_name = "cartographer_scene_tonemap";
    tone.shaders = {program.shaders[4U], program.shaders[5U]};
    tone.descriptor_bindings = {{0U, 0U, gpu::ShaderStage::fragment}};
    tone.cull_mode = gpu::CullMode::none;
    tone.depth_test = false;
    tone.depth_write = false;
    tone.color_format = tone_map_format;
    tone.push_constant_bytes = 16U;
    const auto tone_pipeline = device.create_pipeline(tone);
    if (!tone_pipeline) return Result<SceneProgram>::failure(tone_pipeline.error());
    program.tone_pipeline = tone_pipeline.value();
    resources.pipelines.push_back(tone_pipeline.value());

    gpu::PipelineDesc temporal;
    temporal.debug_name = "cartographer_scene_temporal";
    temporal.shaders = {program.shaders[6U]};
    temporal.descriptor_bindings = {
        {0U, 0U, gpu::ShaderStage::compute},
        {0U, 1U, gpu::ShaderStage::compute},
        {0U, 2U, gpu::ShaderStage::compute},
        {0U, 3U, gpu::ShaderStage::compute}};
    temporal.sampled_texture_count = 3U;
    temporal.depth_test = false;
    temporal.push_constant_bytes = 48U;
    const auto temporal_pipeline = device.create_pipeline(temporal);
    if (!temporal_pipeline)
        return Result<SceneProgram>::failure(temporal_pipeline.error());
    program.temporal_pipeline = temporal_pipeline.value();
    resources.pipelines.push_back(temporal_pipeline.value());

    const auto sampler = device.create_sampler({});
    if (!sampler) return Result<SceneProgram>::failure(sampler.error());
    program.sampler = sampler.value();
    resources.samplers.push_back(sampler.value());
    return Result<SceneProgram>::success(std::move(program));
}

Result<void> validate_environment_mip_structure(
    const SceneRenderEnvironmentPrefilteredMipChain& chain) {
    if (chain.source_digest.is_zero() || chain.levels.size() < 2U ||
        chain.levels.size() > 32U) {
        return Result<void>::failure(invalid(
            "prefiltered environment mip chain is missing identity or complete levels"));
    }
    if (chain.source_color_space != assets::ColorSpace::linear &&
        chain.source_color_space != assets::ColorSpace::srgb) {
        return Result<void>::failure(invalid(
            "prefiltered environment has an unsupported source color space"));
    }
    std::uint32_t expected_width = chain.levels.front().width;
    std::uint32_t expected_height = chain.levels.front().height;
    if (expected_width < 16U || expected_height < 8U ||
        static_cast<std::uint64_t>(expected_width) !=
            static_cast<std::uint64_t>(expected_height) * 2U ||
        static_cast<std::uint64_t>(expected_width) * expected_height >
            kMaxSpecularEnvironmentPixels) {
        return Result<void>::failure(invalid(
            "prefiltered environment dimensions exceed the supported 2:1 bound"));
    }
    std::size_t expected_level_count = 1U;
    while (expected_width != 1U || expected_height != 1U) {
        expected_width = std::max(1U, expected_width / 2U);
        expected_height = std::max(1U, expected_height / 2U);
        ++expected_level_count;
    }
    if (chain.levels.size() != expected_level_count) {
        return Result<void>::failure(invalid(
            "prefiltered environment mip chain must contain exactly one complete level set"));
    }
    expected_width = chain.levels.front().width;
    expected_height = chain.levels.front().height;
    std::uint64_t payload_bytes = 0U;
    for (const auto& level : chain.levels) {
        const std::uint64_t pixel_count =
            static_cast<std::uint64_t>(expected_width) * expected_height;
        if (level.width != expected_width || level.height != expected_height ||
            level.linear_rgb.size() != pixel_count ||
            pixel_count > (64ULL * 1024ULL * 1024ULL - payload_bytes) /
                sizeof(std::array<float, 3U>)) {
            return Result<void>::failure(invalid(
                "prefiltered environment levels are noncanonical or exceed 64 MiB"));
        }
        payload_bytes += pixel_count * sizeof(std::array<float, 3U>);
        for (const auto& pixel : level.linear_rgb) {
            for (const float channel : pixel) {
                if (!std::isfinite(channel) || channel < 0.0F || channel > 1.0e12F) {
                    return Result<void>::failure(invalid(
                        "prefiltered environment radiance is non-finite or exceeds its bound"));
                }
            }
        }
        expected_width = std::max(1U, expected_width / 2U);
        expected_height = std::max(1U, expected_height / 2U);
    }
    if (chain.levels.back().width != 1U || chain.levels.back().height != 1U) {
        return Result<void>::failure(invalid(
            "prefiltered environment mip chain must end at one texel"));
    }
    return Result<void>::success();
}

assets::Sha256Digest environment_mip_digest(
    const SceneRenderEnvironmentPrefilteredMipChain& chain) {
    constexpr std::string_view domain = "carto.d3d12.environment-ggx-prefilter.v1";
    std::vector<std::uint8_t> payload(domain.begin(), domain.end());
    append_digest(payload, chain.source_digest);
    append_u64(payload, static_cast<std::uint64_t>(chain.source_color_space));
    append_u64(payload, kSceneRenderEnvironmentPrefilterSampleCount);
    append_u64(payload, chain.levels.size());
    for (const auto& level : chain.levels) {
        append_u64(payload, level.width);
        append_u64(payload, level.height);
        for (const auto& pixel : level.linear_rgb) {
            for (float channel : pixel) {
                if (channel == 0.0F) channel = 0.0F;
                const std::uint32_t bits = std::bit_cast<std::uint32_t>(channel);
                for (unsigned shift = 0U; shift < 32U; shift += 8U)
                    payload.push_back(static_cast<std::uint8_t>(bits >> shift));
            }
        }
    }
    return assets::sha256(payload);
}

std::uint32_t reverse_bits_for_hammersley(std::uint32_t value) noexcept {
    value = (value << 16U) | (value >> 16U);
    value = ((value & 0x55555555U) << 1U) | ((value >> 1U) & 0x55555555U);
    value = ((value & 0x33333333U) << 2U) | ((value >> 2U) & 0x33333333U);
    value = ((value & 0x0f0f0f0fU) << 4U) | ((value >> 4U) & 0x0f0f0f0fU);
    value = ((value & 0x00ff00ffU) << 8U) | ((value >> 8U) & 0x00ff00ffU);
    return value;
}

const std::vector<std::array<float, 4U>>& split_sum_brdf_lut_pixels() {
    static const std::vector<std::array<float, 4U>> pixels = [] {
        constexpr double pi = 3.141592653589793238462643383279502884;
        std::array<std::array<double, 2U>, kSplitSumBrdfSampleCount> samples{};
        for (std::uint32_t sample = 0U; sample < kSplitSumBrdfSampleCount; ++sample) {
            samples[sample] = {
                static_cast<double>(sample) /
                    static_cast<double>(kSplitSumBrdfSampleCount),
                static_cast<double>(reverse_bits_for_hammersley(sample)) /
                    4294967296.0};
        }

        std::vector<std::array<float, 4U>> result(
            static_cast<std::size_t>(kSplitSumBrdfLutExtent) *
                kSplitSumBrdfLutExtent);
        for (std::uint32_t y = 0U; y < kSplitSumBrdfLutExtent; ++y) {
            const double roughness = (static_cast<double>(y) + 0.5) /
                static_cast<double>(kSplitSumBrdfLutExtent);
            const double alpha = std::max(0.045, roughness * roughness);
            const double alpha_squared = alpha * alpha;
            const double geometry_k = roughness * roughness * 0.5;
            for (std::uint32_t x = 0U; x < kSplitSumBrdfLutExtent; ++x) {
                const double n_dot_v = (static_cast<double>(x) + 0.5) /
                    static_cast<double>(kSplitSumBrdfLutExtent);
                const std::array<double, 3U> view{
                    std::sqrt(std::max(0.0, 1.0 - n_dot_v * n_dot_v)), 0.0, n_dot_v};
                double scale = 0.0;
                double bias = 0.0;
                for (const auto& sample : samples) {
                    const double phi = 2.0 * pi * sample[0U];
                    const double denominator = 1.0 +
                        (alpha_squared - 1.0) * sample[1U];
                    const double cos_half = std::sqrt(std::clamp(
                        (1.0 - sample[1U]) / denominator, 0.0, 1.0));
                    const double sin_half = std::sqrt(std::max(
                        0.0, 1.0 - cos_half * cos_half));
                    const std::array<double, 3U> half_vector{
                        std::cos(phi) * sin_half,
                        std::sin(phi) * sin_half,
                        cos_half};
                    const double v_dot_h = std::max(0.0,
                        view[0U] * half_vector[0U] +
                        view[1U] * half_vector[1U] +
                        view[2U] * half_vector[2U]);
                    const double n_dot_h = std::max(half_vector[2U], 0.0);
                    const double n_dot_l = std::max(
                        2.0 * v_dot_h * half_vector[2U] - n_dot_v, 0.0);
                    if (n_dot_l <= 0.0 || n_dot_h <= 0.0) continue;

                    const auto geometry_schlick = [geometry_k](double n_dot_x) {
                        return n_dot_x / std::max(
                            n_dot_x * (1.0 - geometry_k) + geometry_k, 1.0e-8);
                    };
                    const double visibility = geometry_schlick(n_dot_v) *
                        geometry_schlick(n_dot_l) * v_dot_h /
                        std::max(n_dot_h * n_dot_v, 1.0e-8);
                    const double fresnel_edge = 1.0 - v_dot_h;
                    const double fresnel_fifth = fresnel_edge * fresnel_edge *
                        fresnel_edge * fresnel_edge * fresnel_edge;
                    scale += (1.0 - fresnel_fifth) * visibility;
                    bias += fresnel_fifth * visibility;
                }
                const double reciprocal_sample_count =
                    1.0 / static_cast<double>(kSplitSumBrdfSampleCount);
                result[static_cast<std::size_t>(y) * kSplitSumBrdfLutExtent + x] = {
                    static_cast<float>(scale * reciprocal_sample_count),
                    static_cast<float>(bias * reciprocal_sample_count), 0.0F, 1.0F};
            }
        }
        return result;
    }();
    return pixels;
}

struct ScenePipelineCache {
    D3D12Device* device = nullptr;
    gpu::Format output_format = gpu::Format::rgba32_float;
    std::array<assets::Sha256Digest, 7U> source_digests{};
    SceneProgram program;
    std::unique_ptr<Resources> resources;
    assets::Sha256Digest mesh_identity;
    std::vector<SceneDrawMeshBuffers> meshes;
    std::unique_ptr<Resources> mesh_resources;
    assets::Sha256Digest material_texture_identity;
    SceneMaterialTextures material_textures;
    std::unique_ptr<Resources> material_texture_resources;
    SceneRenderTargets targets;
    std::unique_ptr<Resources> target_resources;

    [[nodiscard]] bool matches(
        const D3D12Device& candidate_device,
        gpu::Format candidate_output_format,
        const std::array<assets::Sha256Digest, 7U>& candidate_digests) const noexcept {
        return device == &candidate_device && output_format == candidate_output_format &&
            source_digests == candidate_digests &&
            resources != nullptr;
    }
};
} // namespace

core::Result<production::CompiledMaterialArtifact>
compile_scene_render_material_artifact(
    std::string source_identity,
    const render::MaterialAsset& material) {
    const auto source_digest = production::material_source_digest(material);
    if (!source_digest)
        return core::Result<production::CompiledMaterialArtifact>::failure(
            source_digest.error());
    const auto& definition = material.definition;
    const std::array<std::tuple<const render::TextureRef*, assets::TextureSemantic,
        std::string_view>, 5U> texture_bindings{{
        {&definition.base_color, assets::TextureSemantic::base_color, "base-color"},
        {&definition.metallic_roughness, assets::TextureSemantic::metallic_roughness,
            "metallic-roughness"},
        {&definition.normal, assets::TextureSemantic::normal, "normal"},
        {&definition.occlusion, assets::TextureSemantic::occlusion, "occlusion"},
        {&definition.emissive, assets::TextureSemantic::emissive, "emissive"},
    }};
    for (const auto& [binding, semantic, slot_name] : texture_bindings) {
        if (auto valid = validate_executed_texture_binding(
                *binding, semantic, slot_name); !valid) {
            return core::Result<production::CompiledMaterialArtifact>::failure(
                valid.error());
        }
    }
    if (auto transforms = material_uv_affines(material.definition); !transforms)
        return core::Result<production::CompiledMaterialArtifact>::failure(
            transforms.error());

    auto gpu_material = material;
    const auto normalize_scene_adapter_fields = [](render::TextureRef& binding) {
        if (binding) {
            binding.uv_set = render::kUv0;
            binding.transform = {};
            binding.sampler = 0U;
        binding.channels = {};
            binding.normal_convention = render::NormalConvention::directx_y_minus;
        }
    };
    normalize_scene_adapter_fields(gpu_material.definition.base_color);
    normalize_scene_adapter_fields(gpu_material.definition.metallic_roughness);
    normalize_scene_adapter_fields(gpu_material.definition.normal);
    normalize_scene_adapter_fields(gpu_material.definition.occlusion);
    normalize_scene_adapter_fields(gpu_material.definition.emissive);
    auto artifact = production::compile_material_artifact(
        std::move(source_identity), gpu_material);
    if (!artifact)
        return core::Result<production::CompiledMaterialArtifact>::failure(
            artifact.error());

    auto result = std::move(artifact).value();
    result.source_digest = source_digest.value();
    if (auto refreshed = result.refresh_digest(); !refreshed)
        return core::Result<production::CompiledMaterialArtifact>::failure(
            refreshed.error());
    return core::Result<production::CompiledMaterialArtifact>::success(
        std::move(result));
}

namespace {
core::Result<void> validate_scene_render_material_artifact_source(
    const production::CompiledMaterialArtifact& artifact,
    std::string_view source_identity,
    const render::MaterialAsset& current_material) {
    const auto current_digest = production::material_source_digest(current_material);
    if (!current_digest)
        return core::Result<void>::failure(current_digest.error());
    if (artifact.source_identity != source_identity ||
        artifact.material_id != current_material.id ||
        artifact.source_revision != current_material.revision ||
        artifact.source_digest != current_digest.value()) {
        return core::Result<void>::failure({core::ErrorCode::stale_data,
            "D3D12 scene material artifact has stale source identity, revision, or digest"});
    }
    if (auto valid = artifact.validate(); !valid) return valid;
    const auto expected = compile_scene_render_material_artifact(
        std::string(source_identity), current_material);
    if (!expected)
        return core::Result<void>::failure(expected.error());
    if (expected.value().artifact_digest != artifact.artifact_digest) {
        return core::Result<void>::failure({core::ErrorCode::stale_data,
            "D3D12 scene material artifact differs from the current authored source"});
    }
    return core::Result<void>::success();
}
} // namespace

core::Result<void> SceneRenderEnvironmentPrefilteredMipChain::refresh_digest() {
    if (auto valid = validate_environment_mip_structure(*this); !valid)
        return valid;
    artifact_digest = environment_mip_digest(*this);
    return core::Result<void>::success();
}

core::Result<void> SceneRenderEnvironmentPrefilteredMipChain::validate() const {
    if (auto valid = validate_environment_mip_structure(*this); !valid)
        return valid;
    if (artifact_digest.is_zero() || artifact_digest != environment_mip_digest(*this)) {
        return core::Result<void>::failure({core::ErrorCode::validation_failed,
            "prefiltered environment digest does not match its canonical linear pixels"});
    }
    return core::Result<void>::success();
}

struct SceneRenderCache::Impl {
    std::unique_ptr<ScenePipelineCache> program;
    std::uint64_t generation = 0U;
};

SceneRenderCache::SceneRenderCache() = default;
SceneRenderCache::~SceneRenderCache() = default;
SceneRenderCache::SceneRenderCache(SceneRenderCache&&) noexcept = default;
SceneRenderCache& SceneRenderCache::operator=(SceneRenderCache&&) noexcept = default;

void SceneRenderCache::reset() noexcept {
    impl_.reset();
}

std::uint64_t SceneRenderCache::generation() const noexcept {
    return impl_ != nullptr ? impl_->generation : 0U;
}

core::Result<void> SceneRenderEnvironment::validate() const {
    if (source_digest.is_zero())
        return core::Result<void>::failure(invalid(
            "diffuse irradiance SH requires a nonzero source digest"));
    if (source_color_space.has_value() &&
        *source_color_space != assets::ColorSpace::linear &&
        *source_color_space != assets::ColorSpace::srgb) {
        return core::Result<void>::failure(invalid(
            "diffuse environment image color space must be linear or sRGB"));
    }
    for (const auto coefficient : diffuse_irradiance_over_pi_sh) {
        if (!narrowable(coefficient) || std::abs(coefficient.x) > 1.0e6 ||
            std::abs(coefficient.y) > 1.0e6 || std::abs(coefficient.z) > 1.0e6) {
            return core::Result<void>::failure(invalid(
                "diffuse irradiance SH coefficients must be finite and bounded"));
        }
    }
    if (specular_environment_mips.has_value()) {
        const auto& mips = *specular_environment_mips;
        if (mips.source_digest != source_digest) {
            return core::Result<void>::failure({core::ErrorCode::stale_data,
                "specular environment mip source differs from the diffuse environment"});
        }
        if (!source_color_space.has_value() ||
            mips.source_color_space != *source_color_space) {
            return core::Result<void>::failure(invalid(
                "specular environment source color space differs from its interpretation"));
        }
        if (auto valid = mips.validate(); !valid) return valid;
    }
    return core::Result<void>::success();
}

Result<void> SceneRenderRequest::validate() const {
    if (source_digest.is_zero() || source_revision.exhausted() || width == 0U || height == 0U ||
        width > kSceneRenderMaximumDimension ||
        height > kSceneRenderMaximumDimension ||
        static_cast<std::uint64_t>(width) * height > kSceneRenderMaximumPixels ||
        objects.empty() || objects.size() > 4096U || textures.size() > 1024U ||
        local_lights.size() > 64U ||
        samplers.size() > 1024U ||
        (antialiasing != SceneRenderAntialiasing::none &&
         antialiasing != SceneRenderAntialiasing::fxaa))
        return Result<void>::failure(invalid("scene capture requires source identity and bounded extent/objects/textures/samplers"));
    if (temporal.has_value()) {
        const auto& inputs = *temporal;
        const bool has_history = static_cast<bool>(inputs.history_color);
        const bool has_motion = static_cast<bool>(inputs.motion_vectors);
        const bool has_constant_motion = inputs.constant_motion_pixels.has_value();
        const bool has_history_identity =
            !inputs.history_content_digest.is_zero() &&
            !inputs.history_request_digest.is_zero() &&
            inputs.history_source_revision == source_revision &&
            inputs.history_source_digest == source_digest;
        const bool has_any_history_identity =
            !inputs.history_content_digest.is_zero() ||
            !inputs.history_request_digest.is_zero() ||
            inputs.history_source_revision.value() != 0U ||
            !inputs.history_source_digest.is_zero();
        if (has_constant_motion) {
            const auto offset = *inputs.constant_motion_pixels;
            if (!offset.finite() ||
                std::abs(offset.x) > static_cast<double>(width) ||
                std::abs(offset.y) > static_cast<double>(height)) {
                return Result<void>::failure(invalid(
                    "constant temporal motion must be finite and bounded by the frame extent"));
            }
        }
        const auto readable_prior_state = [](device_ir::ResourceState state) {
            return state == device_ir::ResourceState::shader_read ||
                state == device_ir::ResourceState::copy_source ||
                state == device_ir::ResourceState::render_target;
        };
        if (!inputs.resolved_color ||
            (has_history && inputs.history_color == inputs.resolved_color) ||
            (has_motion && inputs.motion_vectors == inputs.resolved_color) ||
            (has_history && has_motion &&
             inputs.history_color == inputs.motion_vectors) ||
            !std::isfinite(inputs.feedback) || inputs.feedback < 0.0F ||
            inputs.feedback > 1.0F ||
            (has_history ? !readable_prior_state(inputs.history_initial_state)
                         : inputs.history_initial_state !=
                            device_ir::ResourceState::undefined) ||
            (has_motion ? !readable_prior_state(inputs.motion_initial_state)
                        : inputs.motion_initial_state !=
                            device_ir::ResourceState::undefined) ||
            (inputs.resolved_initial_state != device_ir::ResourceState::undefined &&
             inputs.resolved_initial_state != device_ir::ResourceState::shader_read &&
             inputs.resolved_initial_state != device_ir::ResourceState::copy_source)) {
            return Result<void>::failure(invalid(
                "temporal resolve requires distinct textures, bound provenance, finite feedback, and a supported output state"));
        }
        if (inputs.camera_cut) {
            if (has_history || has_motion || has_constant_motion ||
                has_any_history_identity ||
                !inputs.motion_vectors_digest.is_zero() ||
                !inputs.motion_history_request_digest.is_zero() ||
                inputs.zero_motion) {
                return Result<void>::failure(invalid(
                    "camera-cut temporal reset cannot carry stale history or motion inputs"));
            }
        } else if (has_history) {
            if (!has_history_identity ||
                (inputs.zero_motion && (has_motion || has_constant_motion ||
                    !inputs.motion_vectors_digest.is_zero() ||
                    !inputs.motion_history_request_digest.is_zero())) ||
                (!inputs.zero_motion &&
                    ((!has_motion && !has_constant_motion) ||
                    (has_motion && inputs.motion_vectors_digest.is_zero()) ||
                    (!has_motion && !inputs.motion_vectors_digest.is_zero()) ||
                    inputs.motion_history_request_digest.is_zero() ||
                    inputs.motion_history_request_digest !=
                        inputs.history_request_digest))) {
                if (inputs.history_source_revision != source_revision ||
                    inputs.history_source_digest != source_digest) {
                    return Result<void>::failure({core::ErrorCode::stale_data,
                        "temporal history belongs to a different scene source"});
                }
                return Result<void>::failure(invalid(
                    "temporal history requires exact provenance and either motion vectors or an explicit zero-motion assertion"));
            }
        } else if (has_motion || has_constant_motion || has_any_history_identity ||
                   !inputs.motion_vectors_digest.is_zero() ||
                   !inputs.motion_history_request_digest.is_zero() ||
                   inputs.zero_motion) {
            return Result<void>::failure(invalid(
                "temporal motion inputs cannot be supplied without valid history"));
        }
    }
    if (environment.has_value()) {
        if (auto valid = environment->validate(); !valid)
            return Result<void>::failure(valid.error());
    }
    if (auto view = view_projection(*this); !view) return Result<void>::failure(view.error());
    if (!narrowable(background_linear) || background_linear.x < 0.0 || background_linear.y < 0.0 ||
        background_linear.z < 0.0 || !light.direction_to_light.normalized().finite())
        return Result<void>::failure(invalid("scene capture requires nonnegative background and nonzero light direction"));
    if (auto packed = render::pack_light_for_gpu(light); !packed) return Result<void>::failure(packed.error());
    std::set<scene::ObjectId> local_light_objects;
    std::size_t local_shadow_count = 0U;
    for (const auto& local : local_lights) {
        const bool point = local.kind == SceneRenderLocalLightKind::point;
        const bool spot = local.kind == SceneRenderLocalLightKind::spot;
        if (!local.object || !local_light_objects.insert(local.object).second ||
            (!point && !spot) || !narrowable(local.position) ||
            !local.emission_direction.finite() || !narrowable(local.radiance) ||
            local.radiance.x < 0.0 || local.radiance.y < 0.0 ||
            local.radiance.z < 0.0 ||
            local.radiance.x > kSceneRenderMaximumLocalLightIntensity ||
            local.radiance.y > kSceneRenderMaximumLocalLightIntensity ||
            local.radiance.z > kSceneRenderMaximumLocalLightIntensity ||
            !std::isfinite(local.range) || !narrowable(local.range) ||
            local.range <= 0.0 || local.range > 1.0e6 ||
            !std::isfinite(local.inner_cone_radians) ||
            !std::isfinite(local.outer_cone_radians)) {
            return Result<void>::failure(invalid(
                "scene point/spot light identity, pose, radiance, or range is invalid"));
        }
        if (spot) {
            if (!local.emission_direction.normalized().finite() ||
                local.inner_cone_radians < 0.0 ||
                local.inner_cone_radians > local.outer_cone_radians ||
                local.outer_cone_radians >= 1.5707963267948966) {
                return Result<void>::failure(invalid(
                    "scene spot cone or emission direction is invalid"));
            }
        } else if (local.inner_cone_radians != 0.0 ||
                   local.outer_cone_radians != 0.0) {
            return Result<void>::failure(invalid(
                "scene point lights cannot carry spot cone angles"));
        }
        if (local.casts_shadows) {
            if (!spot) {
                return Result<void>::failure({core::ErrorCode::unsupported,
                    "point-light shadows are not supported by the bounded scene shadow map"});
            }
            if (++local_shadow_count > 1U) {
                return Result<void>::failure({core::ErrorCode::unsupported,
                    "the bounded scene shadow map supports at most one spotlight source"});
            }
            if (local.range < kSceneRenderLocalShadowMinimumRangeMeters ||
                local.outer_cone_radians <
                    kSceneRenderLocalShadowMinimumOuterConeRadians) {
                return Result<void>::failure(invalid(
                    "spot shadow range and outer cone are below the stable projection bound"));
            }
        }
    }
    if (local_shadow_count != 0U &&
        (light.radiance.x > 0.0 || light.radiance.y > 0.0 ||
         light.radiance.z > 0.0)) {
        return Result<void>::failure({core::ErrorCode::unsupported,
            "one shadow map cannot serve a key light and a local spotlight in the same capture"});
    }
    std::uint64_t payload = 0U, draws = 0U;
    std::map<assets::TextureAssetId, const SceneRenderTexture*> available;
    for (const auto& [identity, sampler] : samplers) {
        if (identity == 0U)
            return Result<void>::failure(invalid(
                "scene sampler table must omit the reserved default sampler ID"));
        if (auto valid = gpu::validate(sampler); !valid)
            return Result<void>::failure(valid.error().with_context(
                "scene sampler " + std::to_string(identity)));
        if (sampler.compare)
            return Result<void>::failure({core::ErrorCode::unsupported,
                "scene PBR sampling does not execute comparison samplers"});
    }
    for (const auto& texture : textures) {
        if (texture.asset == 0U || !available.emplace(texture.asset, &texture).second ||
            texture.current_source_digest.is_zero() || texture.mips.source.digest != texture.current_source_digest)
            return Result<void>::failure({core::ErrorCode::stale_data, "scene texture has duplicate/missing identity or stale source digest"});
        if (texture.mips.resident_bytes() > kPayloadLimit - payload)
            return Result<void>::failure(invalid("scene mip payload exceeds 512 MiB"));
        payload += texture.mips.resident_bytes();
        if (auto valid = texture.mips.validate(); !valid) return valid;
    }
    std::set<scene::ObjectId> object_ids;
    std::set<assets::TextureAssetId> used_textures;
    std::set<render::SamplerId> used_samplers;
    for (const auto& object : objects) {
        if (!object.object || !object_ids.insert(object.object).second ||
            object.mesh.source_revision != source_revision || object.mesh.source_digest != source_digest)
            return Result<void>::failure({core::ErrorCode::stale_data, "scene object has duplicate identity or foreign/stale mesh source"});
        if (auto valid = object.mesh.validate(); !valid) return valid;
        if (object.uv0_set == 0U ||
            (object.uv1_set.has_value() &&
             (*object.uv1_set == 0U || *object.uv1_set == object.uv0_set))) {
            return Result<void>::failure(invalid(
                "scene object UV stream identities must be nonzero and distinct"));
        }
        if (1U + (object.uv1_set.has_value() ? 1U : 0U) +
                object.mesh.additional_uv_set_ids.size() >
                    production::kMaxRenderMeshArtifactUvStreams ||
            std::binary_search(object.mesh.additional_uv_set_ids.begin(),
                object.mesh.additional_uv_set_ids.end(), object.uv0_set) ||
            (object.uv1_set.has_value() && std::binary_search(
                object.mesh.additional_uv_set_ids.begin(),
                object.mesh.additional_uv_set_ids.end(), *object.uv1_set))) {
            return Result<void>::failure(invalid(
                "scene object UV stream identities exceed the bound or alias"));
        }
        if (object.mesh.vertices.size() > production::kMaxRenderMeshArtifactVertices ||
            object.mesh.indices.size() > production::kMaxRenderMeshArtifactIndices ||
            object.materials.size() > 65536U || object.draw_package.draws.size() > 65536U - draws)
            return Result<void>::failure(invalid("scene geometry or draw count exceeds capture bounds"));
        draws += object.draw_package.draws.size();
        if (auto model = model_matrix(object.world_transform); !model) return Result<void>::failure(model.error());
        for (const auto& vertex : object.mesh.vertices) {
            const auto world = object.world_transform.translation + object.world_transform.rotation.rotate(
                core::componentwise_multiply(object.world_transform.scale, vertex.position));
            if (!narrowable(world)) return Result<void>::failure(invalid("world vertex overflows float32"));
        }
        if (auto valid = object.draw_package.validate(); !valid) return valid;
        const auto packed_mesh_bytes = static_cast<std::uint64_t>(object.mesh.vertices.size()) *
            sizeof(production::PbrUploadVertex) +
            static_cast<std::uint64_t>(object.mesh.indices.size()) * sizeof(std::uint32_t);
        if (packed_mesh_bytes > kPayloadLimit - payload)
            return Result<void>::failure(invalid("scene packed mesh upload exceeds 512 MiB"));
        payload += packed_mesh_bytes;
        const auto additional_uv_bytes =
            static_cast<std::uint64_t>(object.mesh.vertices.size()) *
            static_cast<std::uint64_t>(object.mesh.additional_uv_set_ids.size()) *
            sizeof(core::Vec2d);
        if (additional_uv_bytes > kPayloadLimit - payload)
            return Result<void>::failure(invalid(
                "scene additional UV streams exceed 512 MiB"));
        payload += additional_uv_bytes;
        for (const auto& draw : object.draw_package.draws) {
            const auto maximum_draw_vertices = std::min<std::uint64_t>(
                draw.index_count, object.mesh.vertices.size());
            const auto draw_upload_bytes = maximum_draw_vertices *
                sizeof(ScenePbrUploadVertex) +
                static_cast<std::uint64_t>(draw.index_count) * sizeof(std::uint32_t);
            if (draw_upload_bytes > kPayloadLimit - payload)
                return Result<void>::failure(invalid(
                    "per-material transformed mesh uploads exceed 512 MiB"));
            payload += draw_upload_bytes;
        }
        std::vector<production::LookdevMaterialBinding> bindings;
        for (const auto& binding : object.materials) {
            if (!std::isfinite(binding.sheen_weight) || binding.sheen_weight < 0.0 ||
                binding.sheen_weight > 1.0) {
                return Result<void>::failure(invalid(
                    "scene sheen weight must be finite and in [0, 1]"));
            }
            if (binding.sheen_weight > 0.0 && environment.has_value()) {
                return Result<void>::failure({core::ErrorCode::unsupported,
                    "scene sheen currently supports analytic direct lights only; "
                    "environment sheen integration is unsupported"});
            }
            if (auto valid = validate_scene_render_material_artifact_source(
                    binding.material, binding.source_identity,
                    binding.current_material); !valid) return valid;
            const auto& definition = binding.current_material.definition;
            if (!std::isfinite(binding.anisotropy) || binding.anisotropy < -1.0 ||
                binding.anisotropy > 1.0) {
                return Result<void>::failure(invalid(
                    "scene anisotropy must be finite and in [-1, 1]"));
            }
            if (binding.anisotropy != 0.0) {
                if (environment.has_value()) {
                    return Result<void>::failure({core::ErrorCode::unsupported,
                        "scene anisotropy currently refuses environment lighting"});
                }
                if (definition.normal && (definition.normal.uv_set != render::kUv0 ||
                        definition.normal.transform.rotation != 0.0 ||
                        definition.normal.transform.scale.x <= 0.0 ||
                        definition.normal.transform.scale.y <= 0.0)) {
                    return Result<void>::failure({core::ErrorCode::unsupported,
                        "anisotropic normal maps require UV0 with zero rotation and positive axis scales"});
                }
                if (object.uv0_set != render::kUv0 || object.mesh.vertices.empty() ||
                    !object.mesh.vertices.front().uv.has_value() ||
                    !object.mesh.vertices.front().tangent.has_value()) {
                    return Result<void>::failure({core::ErrorCode::unsupported,
                        "scene anisotropy requires an explicit authored UV0 tangent frame"});
                }
            }
            const auto alpha_selector =
                definition.base_color.channels.rgba[3U];
            const bool alpha_coverage_is_required =
                definition.alpha_mode == render::AlphaMode::masked &&
                definition.base_color && definition.alpha_cutoff > 0.0 &&
                definition.base_color_factor.w >= definition.alpha_cutoff;
            const bool alpha_selector_is_constant =
                alpha_selector == render::TextureChannel::constant_zero ||
                alpha_selector == render::TextureChannel::constant_one;
            if (alpha_coverage_is_required && !alpha_selector_is_constant &&
                alpha_selector != render::TextureChannel::alpha) {
                return Result<void>::failure({core::ErrorCode::unsupported,
                    "masked base-color alpha mapping requires mip coverage support for its selected channel"});
            }
            const std::array<const render::TextureRef*, 5U> texture_slots{
                &definition.base_color, &definition.metallic_roughness,
                &definition.normal, &definition.occlusion, &definition.emissive};
            const auto sampler_ids = material_draw_samplers(definition);
            for (std::size_t slot = 0U; slot < sampler_ids.size(); ++slot) {
                const auto identity = sampler_ids[slot];
                if (!static_cast<bool>(*texture_slots[slot]) || identity == 0U) continue;
                if (!samplers.contains(identity))
                    return Result<void>::failure({core::ErrorCode::not_found,
                        "bound scene material sampler has no current descriptor"});
                used_samplers.insert(identity);
            }
            const std::array<std::tuple<const render::TextureRef*, assets::TextureSemantic,
                std::string_view>, 5U> texture_bindings{{
                {&definition.base_color, assets::TextureSemantic::base_color, "base-color"},
                {&definition.metallic_roughness, assets::TextureSemantic::metallic_roughness,
                    "metallic-roughness"},
                {&definition.normal, assets::TextureSemantic::normal, "normal"},
                {&definition.occlusion, assets::TextureSemantic::occlusion, "occlusion"},
                {&definition.emissive, assets::TextureSemantic::emissive, "emissive"},
            }};
            for (const auto& [texture, semantic, slot_name] : texture_bindings) {
                if (auto valid = validate_executed_texture_binding(
                        *texture, semantic, slot_name); !valid) return valid;
            }
            if (auto valid = validate_material_uv_bounds(
                    definition, object.mesh, object.uv0_set,
                    object.uv1_set); !valid) {
                return valid;
            }
            bindings.push_back({binding.material_slot, binding.material});
        }
        const auto expected_package = production::compile_lookdev_draw_package(
            object.mesh, bindings);
        if (!expected_package)
            return Result<void>::failure(expected_package.error());
        if (expected_package.value().package_digest !=
            object.draw_package.package_digest) {
            return Result<void>::failure({core::ErrorCode::stale_data,
                "scene draw package differs from current D3D12 material and mesh sources"});
        }
        // Finite values alone do not prove usable float32 world geometry. An
        // otherwise valid local mesh can collapse after a large translation.
        std::vector<core::Vec3d> float_world;
        for (const auto& vertex : object.mesh.vertices) {
            const auto world = object.world_transform.translation + object.world_transform.rotation.rotate(
                core::componentwise_multiply(object.world_transform.scale, vertex.position));
            float_world.push_back({static_cast<float>(world.x), static_cast<float>(world.y), static_cast<float>(world.z)});
        }
        for (std::size_t i = 0U; i < object.mesh.indices.size(); i += 3U) {
            const auto a = float_world[object.mesh.indices[i]], b = float_world[object.mesh.indices[i + 1U]],
                       c = float_world[object.mesh.indices[i + 2U]];
            if (core::cross(b - a, c - a).length_squared() == 0.0)
                return Result<void>::failure(invalid("world triangle collapses at native float32 precision"));
        }
        for (const auto& draw : object.draw_package.draws) {
            for (std::size_t slot = 0U; slot < kSemantics.size(); ++slot) {
                const auto id = texture_id(draw.gpu_constants, slot);
                if (id == 0U) continue; // Unbound material factor, not missing source data.
                const auto found = available.find(id);
                if (found == available.end()) return Result<void>::failure({core::ErrorCode::not_found, "bound scene texture is unavailable; no fallback permitted"});
                const auto& mip = found->second->mips;
                if (mip.semantic != kSemantics[slot] || mip.color_space != assets::expected_color_space(kSemantics[slot]))
                    return Result<void>::failure(invalid("scene texture semantic/color space differs from material slot"));
                used_textures.insert(id);
            }
        }
    }
    if (used_textures.size() != available.size()) return Result<void>::failure(invalid("scene capture contains unused texture inputs"));
    if (used_samplers.size() != samplers.size())
        return Result<void>::failure(invalid(
            "scene capture contains unused sampler descriptors"));
    if (auto shadow = shadow_view_projection(*this); !shadow) return Result<void>::failure(shadow.error());
    return Result<void>::success();
}

Result<SceneRenderEnvironment> load_scene_render_environment_sh(
    const std::filesystem::path& path) {
    constexpr std::uint64_t maximum_bytes = 4096U;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() >
        static_cast<std::streamoff>(maximum_bytes)) {
        return Result<SceneRenderEnvironment>::failure({core::ErrorCode::io_error,
            "diffuse irradiance SH file is missing or exceeds 4096 bytes"});
    }
    std::string source(static_cast<std::size_t>(input.tellg()), '\0');
    input.seekg(0);
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (!input)
        return Result<SceneRenderEnvironment>::failure({core::ErrorCode::io_error,
            "diffuse irradiance SH file read failed"});
    std::istringstream values(source);
    values.imbue(std::locale::classic());
    std::string header;
    if (!(values >> header) || header != "CARTO_DIFFUSE_IRRADIANCE_SH_V1")
        return Result<SceneRenderEnvironment>::failure(invalid(
            "diffuse irradiance SH file has an unsupported header"));
    SceneRenderEnvironment environment;
    const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(source.data());
    environment.source_digest = assets::sha256(
        std::span<const std::uint8_t>{source_bytes, source.size()});
    for (auto& coefficient : environment.diffuse_irradiance_over_pi_sh) {
        if (!(values >> coefficient.x >> coefficient.y >> coefficient.z))
            return Result<SceneRenderEnvironment>::failure(invalid(
                "diffuse irradiance SH file must contain exactly nine RGB triples"));
    }
    std::string trailing;
    if (values >> trailing)
        return Result<SceneRenderEnvironment>::failure(invalid(
            "diffuse irradiance SH file contains trailing fields"));
    if (auto valid = environment.validate(); !valid)
        return Result<SceneRenderEnvironment>::failure(valid.error());
    return Result<SceneRenderEnvironment>::success(std::move(environment));
}

Result<assets::Sha256Digest> SceneRenderRequest::digest() const {
    if (auto valid = validate(); !valid) return Result<assets::Sha256Digest>::failure(valid.error());
    return Result<assets::Sha256Digest>::success(request_digest(*this));
}

namespace {

Result<SceneRenderImage> render_scene_impl(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    std::optional<gpu::TextureHandle> target,
    device_ir::ResourceState target_initial_state,
    std::optional<gpu::TextureHandle> frame_target = std::nullopt,
    std::unique_ptr<ScenePipelineCache>* pipeline_cache = nullptr,
    std::uint64_t* cache_generation = nullptr,
    bool wait_for_completion = true,
    bool capture_gpu_timing = false) {
    if (auto valid = request.validate(); !valid) return Result<SceneRenderImage>::failure(valid.error());
    using MaterialUvTransforms =
        std::map<std::optional<std::uint32_t>, SceneMaterialUvState>;
    std::vector<MaterialUvTransforms> material_uv_transforms;
    material_uv_transforms.reserve(request.objects.size());
    for (const auto& object : request.objects) {
        MaterialUvTransforms object_transforms;
        for (const auto& binding : object.materials) {
            auto transforms = material_uv_affines(binding.current_material.definition);
            if (!transforms)
                return Result<SceneRenderImage>::failure(transforms.error());
            const bool open_gl_y_plus_normal =
                static_cast<bool>(binding.current_material.definition.normal) &&
                binding.current_material.definition.normal.normal_convention ==
                    render::NormalConvention::opengl_y_plus;
            if (!object_transforms.emplace(binding.material_slot,
                    SceneMaterialUvState{transforms.value(), open_gl_y_plus_normal,
                        material_draw_samplers(binding.current_material.definition),
                        material_draw_uv_sets(binding.current_material.definition),
                        material_draw_channel_mappings(
                            binding.current_material.definition),
                        static_cast<float>(binding.sheen_weight),
                        static_cast<float>(binding.anisotropy)}).second) {
                return Result<SceneRenderImage>::failure(invalid(
                    "scene material UV transforms contain a duplicate slot"));
            }
        }
        material_uv_transforms.push_back(std::move(object_transforms));
    }
    const auto transform_for_draw = [&](std::size_t object_index,
            const production::LookdevSubmeshDraw& draw)
            -> Result<SceneMaterialUvState> {
        if (object_index >= material_uv_transforms.size()) {
            return Result<SceneMaterialUvState>::failure(invalid(
                "scene draw references a missing object UV transform table"));
        }
        const auto found = material_uv_transforms[object_index].find(
            draw.material_slot);
        if (found == material_uv_transforms[object_index].end()) {
            return Result<SceneMaterialUvState>::failure({core::ErrorCode::stale_data,
                "scene draw has no current material UV transform"});
        }
        return Result<SceneMaterialUvState>::success(found->second);
    };
    if (!wait_for_completion && (!target.has_value() || !frame_target.has_value())) {
        return Result<SceneRenderImage>::failure(invalid(
            "asynchronous scene submission requires a direct target and frame handoff"));
    }
    if (capture_gpu_timing && !wait_for_completion) {
        return Result<SceneRenderImage>::failure(invalid(
            "GPU timing capture requires synchronous completion"));
    }
    if (target.has_value()) {
        if (!*target || (target_initial_state != device_ir::ResourceState::undefined &&
                         target_initial_state != device_ir::ResourceState::copy_source)) {
            return Result<SceneRenderImage>::failure(invalid(
                "direct scene rendering requires a live target in undefined or copy-source state"));
        }
        const auto descriptor = device.describe_texture(*target);
        if (!descriptor) return Result<SceneRenderImage>::failure(descriptor.error());
        const auto& actual = descriptor.value();
        if (actual.width != request.width || actual.height != request.height ||
            actual.depth != 1U || actual.layers != 1U || actual.mip_levels != 1U ||
            actual.dimension != gpu::TextureDimension::texture_2d ||
            actual.format != gpu::Format::bgra8_unorm ||
            !gpu::has_usage(actual.usage, gpu::TextureUsage::color_attachment) ||
            !gpu::has_usage(actual.usage, gpu::TextureUsage::transfer_source)) {
            return Result<SceneRenderImage>::failure(invalid(
                "direct scene target must be a matching BGRA8 color/copy-source texture"));
        }
    }
    if (frame_target.has_value()) {
        if (!target.has_value() || !*frame_target || *frame_target == *target) {
            return Result<SceneRenderImage>::failure(invalid(
                "integrated scene presentation requires a distinct live frame target"));
        }
        const auto descriptor = device.describe_texture(*frame_target);
        if (!descriptor) return Result<SceneRenderImage>::failure(descriptor.error());
        const auto& actual = descriptor.value();
        if (actual.width != request.width || actual.height != request.height ||
            actual.depth != 1U || actual.layers != 1U || actual.mip_levels != 1U ||
            actual.dimension != gpu::TextureDimension::texture_2d ||
            actual.format != gpu::Format::bgra8_unorm ||
            !gpu::has_usage(actual.usage, gpu::TextureUsage::transfer_destination)) {
            return Result<SceneRenderImage>::failure(invalid(
                "integrated frame target must be a matching BGRA8 copy destination"));
        }
    }
    if (request.temporal.has_value()) {
        const auto& temporal = *request.temporal;
        if ((target.has_value() &&
                ((temporal.history_color && temporal.history_color == *target) ||
                 (temporal.motion_vectors && temporal.motion_vectors == *target) ||
                 temporal.resolved_color == *target)) ||
            (frame_target.has_value() &&
                ((temporal.history_color && temporal.history_color == *frame_target) ||
                 (temporal.motion_vectors && temporal.motion_vectors == *frame_target) ||
                 temporal.resolved_color == *frame_target))) {
            return Result<SceneRenderImage>::failure(invalid(
                "temporal inputs and output must not alias a presentation target"));
        }
        const auto validate_temporal_texture = [&](gpu::TextureHandle handle,
                gpu::Format format, gpu::TextureUsage usage,
                device_ir::ResourceState initial_state,
                std::string_view label) -> Result<void> {
            const auto descriptor = device.describe_texture(handle);
            if (!descriptor) return Result<void>::failure(descriptor.error());
            const auto& actual = descriptor.value();
            const bool state_usage_matches =
                initial_state == device_ir::ResourceState::undefined ||
                (initial_state == device_ir::ResourceState::shader_read &&
                 gpu::has_usage(actual.usage, gpu::TextureUsage::sampled)) ||
                (initial_state == device_ir::ResourceState::copy_source &&
                 gpu::has_usage(actual.usage, gpu::TextureUsage::transfer_source)) ||
                (initial_state == device_ir::ResourceState::render_target &&
                 gpu::has_usage(actual.usage, gpu::TextureUsage::color_attachment));
            if (actual.width != request.width || actual.height != request.height ||
                actual.depth != 1U || actual.layers != 1U ||
                actual.mip_levels != 1U ||
                actual.dimension != gpu::TextureDimension::texture_2d ||
                actual.format != format || !gpu::has_usage(actual.usage, usage) ||
                !state_usage_matches) {
                return Result<void>::failure(invalid(
                    "temporal " + std::string(label) +
                    " texture must match the request extent, format, and usage"));
            }
            return Result<void>::success();
        };
        if (temporal.history_color) {
            if (auto valid = validate_temporal_texture(temporal.history_color,
                    gpu::Format::rgba32_float, gpu::TextureUsage::sampled,
                    temporal.history_initial_state,
                    "history"); !valid) {
                return Result<SceneRenderImage>::failure(valid.error());
            }
        }
        if (temporal.motion_vectors) {
            if (auto valid = validate_temporal_texture(temporal.motion_vectors,
                    gpu::Format::rg32_float, gpu::TextureUsage::sampled,
                    temporal.motion_initial_state,
                    "motion-vector"); !valid) {
                return Result<SceneRenderImage>::failure(valid.error());
            }
        }
        const auto resolved_usage = gpu::TextureUsage::storage |
            gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_source;
        if (auto valid = validate_temporal_texture(temporal.resolved_color,
                gpu::Format::rgba32_float, resolved_usage,
                temporal.resolved_initial_state,
                "resolved-output"); !valid) {
            return Result<SceneRenderImage>::failure(valid.error());
        }
    }
    if (device.is_software_adapter() || request.width > device.capabilities().max_texture_dimension_2d ||
        request.height > device.capabilities().max_texture_dimension_2d)
        return Result<SceneRenderImage>::failure({core::ErrorCode::unsupported, "scene rendering requires a supported hardware adapter/extent"});
    const auto gpu_timestamp_frequency = capture_gpu_timing
        ? device.timestamp_frequency(gpu::QueueType::graphics) : 0U;
    if (capture_gpu_timing && gpu_timestamp_frequency == 0U) {
        return Result<SceneRenderImage>::failure({core::ErrorCode::unsupported,
            "graphics queue timestamp queries are unavailable"});
    }
    const auto output_format = target.has_value()
        ? gpu::Format::bgra8_unorm : gpu::Format::rgba32_float;
    Resources owned{device};
    std::optional<gpu::BufferHandle> gpu_timestamp_readback;
    if (capture_gpu_timing) {
        const auto readback = owned.buffer({
            2U * sizeof(std::uint64_t), gpu::MemoryClass::readback,
            gpu::BufferUsage::transfer_destination});
        if (!readback) return Result<SceneRenderImage>::failure(readback.error());
        gpu_timestamp_readback = readback.value();
    }
    SceneRenderImage result;
    result.width = request.width; result.height = request.height;
    result.source_revision = request.source_revision; result.source_digest = request.source_digest;
    result.request_digest = request_digest(request);
    result.antialiasing = request.antialiasing;
    if (request.environment.has_value()) {
        result.environment_digest = request.environment->source_digest;
        result.environment_color_space = request.environment->source_color_space;
    }
    result.device = device.identity();
    const std::string antialiasing_limitation =
        request.antialiasing == SceneRenderAntialiasing::fxaa
            ? request.temporal.has_value()
                ? "FXAA spatial filtering and caller-bound temporal HDR resolve enabled"
                : "FXAA spatial filtering enabled; temporal inputs were not supplied"
            : request.temporal.has_value()
                ? "Spatial antialiasing disabled; caller-bound temporal HDR resolve enabled"
                : "Spatial antialiasing disabled; temporal inputs were not supplied";
    result.limitations = {"One bounded key-directional or single-spot shadow map with 3x3 PCF; no cascades or contact shadows",
        request.environment.has_value()
            ? request.environment->specular_environment_mips.has_value()
                ? "Order-2 Lambert diffuse SH plus " +
                    std::to_string(kSceneRenderEnvironmentPrefilterSampleCount) +
                    "-sample GGX-prefiltered panorama reflections with split-sum BRDF integration; " +
                    antialiasing_limitation
                : "Order-2 Lambert diffuse irradiance SH only; no specular environment; " + antialiasing_limitation
            : "Fixed 0.03 ambient approximation; no environment lighting or reflections; " + antialiasing_limitation,
        "Straight-alpha blended submeshes sort by centroid; intersecting transparent surfaces and fractional transparent shadows are unsupported",
        frame_target.has_value()
            ? "Scene render and desktop copy/present share one graph; photorealistic acceptance remains absent"
            : target.has_value()
            ? "Direct GPU render target; no full application render-graph parity or photorealistic acceptance implied"
            : "Synchronous offscreen capture; no interactive viewport, full application render-graph parity or photorealistic acceptance implied"};
    if (!request.local_lights.empty()) {
        const bool has_spot_shadow = std::any_of(request.local_lights.begin(),
            request.local_lights.end(), [](const SceneRenderLocalLight& light) {
                return light.casts_shadows;
            });
        result.limitations.push_back(has_spot_shadow
            ? "One authored spotlight uses the scene shadow map; other local lights remain unshadowed; inverse-square intensity uses a 1 cm floor, quartic range fade, and uncalibrated linear units"
            : "Point/spot lights are unshadowed; inverse-square intensity uses a 1 cm floor, quartic range fade, and uncalibrated linear units");
    }
    const bool uses_sheen = std::any_of(request.objects.begin(), request.objects.end(),
        [](const SceneRenderObject& object) {
            return std::any_of(object.materials.begin(), object.materials.end(),
                [](const SceneRenderMaterialBinding& binding) {
                    return binding.sheen_weight > 0.0;
                });
        });
    if (uses_sheen) {
        result.limitations.push_back(
            "Scalar base-tinted Charlie/Neubelt sheen uses a metallic-suppressed base mixture "
            "for analytic direct lights only; shared surface roughness, no sheen IBL, "
            "independent sheen texture/color, or measured cloth/energy acceptance");
    }
    const bool uses_anisotropy = std::any_of(request.objects.begin(), request.objects.end(),
        [](const SceneRenderObject& object) {
            return std::any_of(object.materials.begin(), object.materials.end(),
                [](const SceneRenderMaterialBinding& binding) {
                    return binding.anisotropy != 0.0;
                });
        });
    if (uses_anisotropy) {
        result.limitations.push_back(
            "Signed UV0-tangent anisotropy mixes legacy specular with anisotropic GGX/Smith "
            "for analytic direct lights only; normal maps require UV0, zero rotation and positive scales; "
            "shortest-rotation shading axes use a deterministic antiparallel fallback; no anisotropic IBL, "
            "independent direction/roughness texture, or measured energy/appearance acceptance");
    }
    const std::array<std::string, 7U> shader_names{
        "cartographer_scene_pbr.hlsl", "cartographer_scene_pbr.hlsl",
        "cartographer_scene_shadow.hlsl", "cartographer_scene_shadow.hlsl",
        "cartographer_tonemap.hlsl", "cartographer_tonemap.hlsl",
        "cartographer_temporal.hlsl"};
    std::array<std::string, 7U> shader_sources;
    std::array<assets::Sha256Digest, 7U> source_digests{};
    for (std::size_t i = 0U; i < shader_sources.size(); ++i) {
        const auto source = shader_text(shader_directory / shader_names[i]);
        if (!source) return Result<SceneRenderImage>::failure(source.error());
        shader_sources[i] = source.value();
        const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(
            shader_sources[i].data());
        source_digests[i] = assets::sha256(
            std::span<const std::uint8_t>{source_bytes, shader_sources[i].size()});
    }

    SceneProgram program;
    ScenePipelineCache* active_cache = nullptr;
    if (pipeline_cache != nullptr) {
        auto& cached = *pipeline_cache;
        if (cached == nullptr ||
            !cached->matches(device, output_format, source_digests)) {
            cached.reset();
            auto candidate = std::make_unique<ScenePipelineCache>();
            auto persistent_resources = std::make_unique<Resources>(device);
            auto created = build_scene_program(device, *persistent_resources,
                shader_sources, output_format);
            if (!created)
                return Result<SceneRenderImage>::failure(created.error());
            candidate->device = &device;
            candidate->output_format = output_format;
            candidate->source_digests = source_digests;
            candidate->program = std::move(created.value());
            candidate->resources = std::move(persistent_resources);
            cached = std::move(candidate);
            if (cache_generation != nullptr &&
                *cache_generation != std::numeric_limits<std::uint64_t>::max()) {
                ++*cache_generation;
            }
        } else if (cached->resources->submitted) {
            if (auto idle = device.wait_idle(); !idle)
                return Result<SceneRenderImage>::failure(idle.error());
            cached->resources->submitted = false;
            if (cached->mesh_resources != nullptr)
                cached->mesh_resources->submitted = false;
            if (cached->material_texture_resources != nullptr)
                cached->material_texture_resources->submitted = false;
            if (cached->target_resources != nullptr) {
                cached->target_resources->submitted = false;
                if (!cached->targets.states_known) {
                    cached->target_resources.reset();
                    cached->targets = {};
                }
            }
        }
        program = cached->program;
        active_cache = cached.get();
    } else {
        auto created = build_scene_program(device, owned, shader_sources,
            output_format);
        if (!created) return Result<SceneRenderImage>::failure(created.error());
        program = std::move(created.value());
    }
    result.shader_digests = program.shader_digests;
    device_ir::DeviceCommandStream stream;
    const auto upload_texture = [&](Resources& texture_owner,
                                    const TextureUploadPlan& plan) -> Result<gpu::TextureHandle> {
        const auto staging = owned.buffer({plan.staging_bytes.size(), gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
        if (!staging) return Result<gpu::TextureHandle>::failure(staging.error());
        const auto texture = texture_owner.texture(plan.texture);
        if (!texture) return texture;
        if (auto write = device.write_buffer(staging.value(), 0U, plan.staging_bytes); !write) return Result<gpu::TextureHandle>::failure(write.error());
        transition(stream, staging.value(), device_ir::ResourceState::undefined, device_ir::ResourceState::copy_source);
        transition(stream, texture.value(), device_ir::ResourceState::undefined, device_ir::ResourceState::copy_destination);
        if (auto copies = append_texture_upload_copies(stream, staging.value(), texture.value(), plan); !copies)
            return Result<gpu::TextureHandle>::failure(copies.error());
        return texture;
    };
    const auto desired_material_texture_identity = active_cache != nullptr
        ? material_texture_cache_identity(request) : assets::Sha256Digest{};
    bool material_texture_cache_hit = false;
    std::unique_ptr<Resources> pending_material_texture_resources;
    SceneMaterialTextures material_textures;
    Resources* material_texture_owner = &owned;
    if (active_cache != nullptr) {
        if (active_cache->material_texture_resources != nullptr &&
            active_cache->material_texture_identity == desired_material_texture_identity) {
            if (active_cache->material_texture_resources->submitted) {
                if (auto idle = device.wait_idle(); !idle)
                    return Result<SceneRenderImage>::failure(idle.error());
                active_cache->material_texture_resources->submitted = false;
            }
            material_textures = active_cache->material_textures;
            material_texture_cache_hit = true;
            material_texture_owner = active_cache->material_texture_resources.get();
        } else {
            active_cache->material_texture_resources.reset();
            active_cache->material_textures = {};
            active_cache->material_texture_identity = {};
            pending_material_texture_resources = std::make_unique<Resources>(device);
            material_texture_owner = pending_material_texture_resources.get();
        }
    }
    if (!material_texture_cache_hit) {
        for (const auto& input : request.textures) {
            const auto plan = build_texture_upload_plan(input.mips);
            if (!plan) return Result<SceneRenderImage>::failure(plan.error());
            const auto handle = upload_texture(*material_texture_owner, plan.value());
            if (!handle) return Result<SceneRenderImage>::failure(handle.error());
            material_textures.source_textures.emplace(input.asset, handle.value());
        }
        for (const auto& [identity, descriptor] : request.samplers) {
            const auto handle = device.create_sampler(descriptor);
            if (!handle) return Result<SceneRenderImage>::failure(handle.error());
            material_texture_owner->samplers.push_back(handle.value());
            material_textures.samplers.emplace(identity, handle.value());
        }
        // Exact neutral factor textures for unbound slots only. They never
        // replace a missing asset. Normal XY is disabled when unbound.
        for (std::size_t slot = 0U; slot < material_textures.neutral.size(); ++slot) {
            assets::Rgba8MipChain chain;
            chain.semantic = kSemantics[slot];
            chain.color_space = assets::expected_color_space(chain.semantic);
            std::vector<std::uint8_t> texel = slot == 2U
                ? std::vector<std::uint8_t>{128U, 128U, 255U, 255U}
                : std::vector<std::uint8_t>{255U, 255U, 255U, 255U};
            chain.source = {assets::sha256(texel), texel.size(),
                "application/x-carto-neutral-factor"};
            chain.levels.push_back({1U, 1U, std::move(texel)});
            if (auto refreshed = chain.refresh_digest(); !refreshed)
                return Result<SceneRenderImage>::failure(refreshed.error());
            const auto plan = build_texture_upload_plan(chain);
            if (!plan) return Result<SceneRenderImage>::failure(plan.error());
            const auto handle = upload_texture(*material_texture_owner, plan.value());
            if (!handle) return Result<SceneRenderImage>::failure(handle.error());
            material_textures.neutral[slot] = handle.value();
        }
        // Four RGBA32F texels per row bind bounded, source-resolved local
        // lights. The outer-cone row's Y component carries the explicit
        // single-spot shadow flag. Keep one zero row when no local sources are
        // present so the shader has a valid t8 binding in every invocation.
        constexpr std::uint64_t kLocalLightRowPitch = 256U;
        const std::uint32_t local_light_count = static_cast<std::uint32_t>(
            std::max<std::size_t>(1U, request.local_lights.size()));
        std::vector<std::uint8_t> local_light_upload(
            static_cast<std::size_t>(kLocalLightRowPitch) * local_light_count, 0U);
        for (std::size_t index = 0U; index < request.local_lights.size(); ++index) {
            const auto& light = request.local_lights[index];
            const auto emission_direction =
                light.kind == SceneRenderLocalLightKind::spot
                ? light.emission_direction.normalized() : core::Vec3d{};
            const float inner_cosine = light.kind == SceneRenderLocalLightKind::spot
                ? static_cast<float>(std::cos(light.inner_cone_radians)) : 1.0F;
            const float outer_cosine = light.kind == SceneRenderLocalLightKind::spot
                ? static_cast<float>(std::cos(light.outer_cone_radians)) : -1.0F;
            const std::array<std::array<float, 4U>, 4U> texels{{
                {static_cast<float>(light.position.x),
                    static_cast<float>(light.position.y),
                    static_cast<float>(light.position.z),
                    static_cast<float>(light.range)},
                {static_cast<float>(emission_direction.x),
                    static_cast<float>(emission_direction.y),
                    static_cast<float>(emission_direction.z),
                    light.kind == SceneRenderLocalLightKind::spot ? 1.0F : 0.0F},
                {static_cast<float>(light.radiance.x),
                    static_cast<float>(light.radiance.y),
                    static_cast<float>(light.radiance.z), inner_cosine},
                {outer_cosine, light.casts_shadows ? 1.0F : 0.0F, 0.0F, 0.0F}}};
            const std::uint64_t row_offset =
                static_cast<std::uint64_t>(index) * kLocalLightRowPitch;
            std::memcpy(local_light_upload.data() + row_offset,
                texels.data(), sizeof(texels));
        }
        const auto local_light_staging = owned.buffer({
            local_light_upload.size(), gpu::MemoryClass::upload,
            gpu::BufferUsage::transfer_source});
        const auto local_light_texture = material_texture_owner->texture({
            4U, local_light_count, 1U, 1U, 1U, gpu::Format::rgba32_float,
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination});
        if (!local_light_staging || !local_light_texture)
            return Result<SceneRenderImage>::failure(!local_light_staging
                ? local_light_staging.error() : local_light_texture.error());
        if (auto written = device.write_buffer(local_light_staging.value(), 0U,
                bytes(local_light_upload)); !written)
            return Result<SceneRenderImage>::failure(written.error());
        transition(stream, local_light_staging.value(),
            device_ir::ResourceState::undefined, device_ir::ResourceState::copy_source);
        transition(stream, local_light_texture.value(),
            device_ir::ResourceState::undefined, device_ir::ResourceState::copy_destination);
        stream.append(device_ir::CmdCopyBufferToTexture{
            local_light_staging.value(), local_light_texture.value(), 0U, 0U});
        material_textures.local_lights_texture = local_light_texture.value();
        // One RGBA32F texture carries the nine signed SH coefficients in the
        // upper-left 3x3 of mip zero and the GGX-prefiltered panorama chain.
        // Mip zero otherwise keeps source radiance; the shader falls back to
        // mip one when a bilinear footprint would overlap the SH payload.
        constexpr std::uint32_t kShExtent = 3U;
        constexpr std::uint64_t kRowPitchAlignment = 256U;
        constexpr std::uint64_t kPlacementAlignment = 512U;
        constexpr std::uint64_t kEnvironmentUploadLimit = 128ULL * 1024ULL * 1024ULL;
        const SceneRenderEnvironmentPrefilteredMipChain* environment_mips =
            request.environment.has_value() &&
                request.environment->specular_environment_mips.has_value()
            ? &*request.environment->specular_environment_mips : nullptr;
        const bool has_specular_mips = environment_mips != nullptr;
        const std::uint32_t texture_width = environment_mips != nullptr
            ? environment_mips->levels.front().width : kShExtent;
        const std::uint32_t texture_height = environment_mips != nullptr
            ? environment_mips->levels.front().height : kShExtent;
        const std::uint32_t texture_mip_count = environment_mips != nullptr
            ? static_cast<std::uint32_t>(environment_mips->levels.size()) : 1U;
        struct EnvironmentMipLayout {
            std::uint64_t offset = 0U;
            std::uint32_t row_pitch = 0U;
        };
        std::vector<EnvironmentMipLayout> environment_layouts;
        std::uint64_t environment_upload_bytes = 0U;
        std::uint32_t mip_width = texture_width;
        std::uint32_t mip_height = texture_height;
        for (std::uint32_t mip = 0U; mip < texture_mip_count; ++mip) {
            const std::uint64_t row_bytes = static_cast<std::uint64_t>(mip_width) *
                sizeof(float) * 4U;
            const std::uint64_t row_pitch =
                (row_bytes + kRowPitchAlignment - 1U) & ~(kRowPitchAlignment - 1U);
            environment_upload_bytes =
                (environment_upload_bytes + kPlacementAlignment - 1U) &
                ~(kPlacementAlignment - 1U);
            if (row_pitch > std::numeric_limits<std::uint32_t>::max() ||
                (mip_height != 0U && row_pitch >
                    (kEnvironmentUploadLimit - environment_upload_bytes) / mip_height)) {
                return Result<SceneRenderImage>::failure(invalid(
                    "specular environment upload exceeds its 128 MiB bound"));
            }
            environment_layouts.push_back({environment_upload_bytes,
                static_cast<std::uint32_t>(row_pitch)});
            environment_upload_bytes += row_pitch * mip_height;
            mip_width = std::max(1U, mip_width / 2U);
            mip_height = std::max(1U, mip_height / 2U);
        }
        if (environment_upload_bytes == 0U ||
            environment_upload_bytes > kEnvironmentUploadLimit ||
            environment_upload_bytes > std::numeric_limits<std::size_t>::max()) {
            return Result<SceneRenderImage>::failure(invalid(
                "environment texture upload exceeds its bounded staging size"));
        }
        std::vector<std::uint8_t> environment_upload(
            static_cast<std::size_t>(environment_upload_bytes), 0U);
        mip_width = texture_width;
        mip_height = texture_height;
        for (std::uint32_t mip = 0U; mip < texture_mip_count; ++mip) {
            const auto& layout = environment_layouts[mip];
            const SceneRenderEnvironmentMipLevel* source_level = environment_mips != nullptr
                ? &environment_mips->levels[mip] : nullptr;
            for (std::uint32_t y = 0U; y < mip_height; ++y) {
                for (std::uint32_t x = 0U; x < mip_width; ++x) {
                    std::array<float, 4U> texel{};
                    if (source_level != nullptr) {
                        const auto& radiance = source_level->linear_rgb[
                            static_cast<std::size_t>(y) * mip_width + x];
                        texel = {radiance[0U], radiance[1U], radiance[2U], 1.0F};
                    }
                    if (mip == 0U && x < kShExtent && y < kShExtent) {
                        const std::size_t coefficient_index =
                            static_cast<std::size_t>(y) * kShExtent + x;
                        const core::Vec3d coefficient = request.environment.has_value()
                            ? request.environment->diffuse_irradiance_over_pi_sh[coefficient_index]
                            : core::Vec3d{};
                        texel[0] = static_cast<float>(coefficient.x);
                        texel[1] = static_cast<float>(coefficient.y);
                        texel[2] = static_cast<float>(coefficient.z);
                        texel[3] = x == 0U && y == 0U && request.environment.has_value()
                            ? 1.0F
                            : x == 2U && y == 2U && has_specular_mips
                                ? 1.0F : 0.0F;
                    }
                    const std::uint64_t destination_offset = layout.offset +
                        static_cast<std::uint64_t>(y) * layout.row_pitch +
                        static_cast<std::uint64_t>(x) * sizeof(texel);
                    std::memcpy(environment_upload.data() + destination_offset,
                        texel.data(), sizeof(texel));
                }
            }
            mip_width = std::max(1U, mip_width / 2U);
            mip_height = std::max(1U, mip_height / 2U);
        }
        const auto environment_staging = owned.buffer({environment_upload.size(),
            gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
        const auto environment_texture = material_texture_owner->texture({texture_width,
            texture_height, 1U, 1U, texture_mip_count, gpu::Format::rgba32_float,
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination});
        if (!environment_staging || !environment_texture)
            return Result<SceneRenderImage>::failure(!environment_staging
                ? environment_staging.error() : environment_texture.error());
        if (auto written = device.write_buffer(environment_staging.value(), 0U,
                bytes(environment_upload)); !written)
            return Result<SceneRenderImage>::failure(written.error());
        transition(stream, environment_staging.value(),
            device_ir::ResourceState::undefined, device_ir::ResourceState::copy_source);
        transition(stream, environment_texture.value(),
            device_ir::ResourceState::undefined, device_ir::ResourceState::copy_destination);
        for (std::uint32_t mip = 0U; mip < texture_mip_count; ++mip) {
            stream.append(device_ir::CmdCopyBufferToTexture{
                environment_staging.value(), environment_texture.value(),
                environment_layouts[mip].offset, mip});
        }
        material_textures.environment_texture = environment_texture.value();
        if (has_specular_mips) {
            const auto& lut_pixels = split_sum_brdf_lut_pixels();
            const auto lut_upload_bytes = bytes(lut_pixels);
            const auto lut_staging = owned.buffer({lut_upload_bytes.size(),
                gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
            const auto lut_texture = material_texture_owner->texture({
                kSplitSumBrdfLutExtent, kSplitSumBrdfLutExtent, 1U, 1U, 1U,
                gpu::Format::rgba32_float, gpu::TextureDimension::texture_2d,
                gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination});
            if (!lut_staging || !lut_texture)
                return Result<SceneRenderImage>::failure(!lut_staging
                    ? lut_staging.error() : lut_texture.error());
            if (auto written = device.write_buffer(
                    lut_staging.value(), 0U, lut_upload_bytes); !written)
                return Result<SceneRenderImage>::failure(written.error());
            transition(stream, lut_staging.value(),
                device_ir::ResourceState::undefined,
                device_ir::ResourceState::copy_source);
            transition(stream, lut_texture.value(),
                device_ir::ResourceState::undefined,
                device_ir::ResourceState::copy_destination);
            stream.append(device_ir::CmdCopyBufferToTexture{
                lut_staging.value(), lut_texture.value(), 0U, 0U});
            material_textures.split_sum_brdf_lut = lut_texture.value();
        }
    }

    const auto sampler_for_draw = [&](std::size_t object_index,
            const production::LookdevSubmeshDraw& draw, std::size_t texture_slot)
            -> Result<gpu::SamplerHandle> {
        const auto state = transform_for_draw(object_index, draw);
        if (!state) return Result<gpu::SamplerHandle>::failure(state.error());
        if (texture_slot >= state.value().samplers.size()) {
            return Result<gpu::SamplerHandle>::failure(invalid(
                "scene draw sampler slot is outside the material texture table"));
        }
        const auto identity = state.value().samplers[texture_slot];
        if (identity == 0U)
            return Result<gpu::SamplerHandle>::success(program.sampler);
        const auto found = material_textures.samplers.find(identity);
        if (found == material_textures.samplers.end()) {
            return Result<gpu::SamplerHandle>::failure({core::ErrorCode::not_found,
                "scene draw sampler handle is unavailable"});
        }
        return Result<gpu::SamplerHandle>::success(found->second);
    };

    const auto light_view = shadow_view_projection(request).value();
    const auto desired_mesh_identity = active_cache != nullptr
        ? mesh_cache_identity(request) : assets::Sha256Digest{};
    bool mesh_cache_hit = false;
    std::unique_ptr<Resources> pending_mesh_resources;
    std::vector<SceneDrawMeshBuffers> meshes;
    Resources* mesh_resource_owner = &owned;
    if (active_cache != nullptr) {
        if (active_cache->mesh_resources != nullptr &&
            active_cache->mesh_identity == desired_mesh_identity) {
            if (active_cache->mesh_resources->submitted) {
                if (auto idle = device.wait_idle(); !idle)
                    return Result<SceneRenderImage>::failure(idle.error());
                active_cache->mesh_resources->submitted = false;
            }
            meshes = active_cache->meshes;
            mesh_cache_hit = true;
            mesh_resource_owner = active_cache->mesh_resources.get();
        } else {
            active_cache->mesh_resources.reset();
            active_cache->meshes.clear();
            active_cache->mesh_identity = {};
            pending_mesh_resources = std::make_unique<Resources>(device);
            mesh_resource_owner = pending_mesh_resources.get();
        }
    }
    if (!mesh_cache_hit) {
        meshes.resize(request.objects.size());
        for (std::size_t object_index = 0U;
             object_index < request.objects.size(); ++object_index) {
            const auto& object = request.objects[object_index];
            std::vector<render::GpuMaterialConstants> submesh_materials;
            submesh_materials.reserve(object.draw_package.draws.size());
            for (const auto& draw : object.draw_package.draws)
                submesh_materials.push_back(draw.gpu_constants);
            const auto mesh = production::pack_pbr_mesh_upload(
                object.mesh, submesh_materials);
            if (!mesh) return Result<SceneRenderImage>::failure(mesh.error());
            const auto model = model_matrix(object.world_transform).value();
            const auto light_from_local = multiply(light_view, model);
            auto& object_stream = meshes[object_index];
            object_stream.draws.reserve(object.draw_package.draws.size());
            std::vector<ScenePbrUploadVertex> object_vertices;
            std::vector<std::uint32_t> object_indices;
            std::size_t maximum_output_vertices = 0U;
            std::size_t maximum_output_indices = 0U;
            const auto unmapped_vertex = std::numeric_limits<std::uint32_t>::max();
            std::vector<std::uint32_t> vertex_remap(
                mesh.value().vertices.size(), unmapped_vertex);
            std::vector<std::uint32_t> touched_vertices;
            std::size_t maximum_unique_vertices = 0U;
            for (const auto& draw : object.draw_package.draws) {
                const auto draw_vertex_bound = std::min<std::size_t>(
                    draw.index_count, mesh.value().vertices.size());
                maximum_unique_vertices = std::max(maximum_unique_vertices,
                    draw_vertex_bound);
                maximum_output_vertices += draw_vertex_bound;
                maximum_output_indices += draw.index_count;
            }
            object_vertices.reserve(maximum_output_vertices);
            object_indices.reserve(maximum_output_indices);
            touched_vertices.reserve(maximum_unique_vertices);
            for (const auto& draw : object.draw_package.draws) {
                const auto transforms = transform_for_draw(object_index, draw);
                if (!transforms)
                    return Result<SceneRenderImage>::failure(transforms.error());
                const auto& material = draw.gpu_constants;
                std::array<float, 4U> camera_clearcoat{
                    static_cast<float>(request.camera.position.x),
                    static_cast<float>(request.camera.position.y),
                    static_cast<float>(request.camera.position.z),
                    material.clearcoat_factors[0U]};
                std::array<float, 4U> surface_factors = material.surface_factors;
                if (texture_id(material, 2U) == 0U) surface_factors[2U] = 0.0F;
                const auto& channel_mappings = transforms.value().channel_mappings;
                const std::array<float, 4U> material_flags{
                    texture_id(material, 3U) != 0U ? 1.0F : 0.0F,
                    channel_mappings[0U][0U], channel_mappings[0U][1U],
                    channel_mappings[0U][2U]};
                const std::array<float, 4U> channel_mapping_a{
                    channel_mappings[0U][3U], channel_mappings[1U][2U],
                    channel_mappings[1U][1U], channel_mappings[2U][0U]};
                const std::array<float, 4U> channel_mapping_b{
                    channel_mappings[2U][1U], channel_mappings[2U][2U],
                    channel_mappings[3U][0U], channel_mappings[4U][0U]};
                const std::array<float, 4U> channel_mapping_c{
                    channel_mappings[4U][1U], channel_mappings[4U][2U],
                    transforms.value().sheen_weight, transforms.value().anisotropy};
                if (draw.first_index > mesh.value().indices.size() ||
                    draw.index_count > mesh.value().indices.size() - draw.first_index) {
                    return Result<SceneRenderImage>::failure({core::ErrorCode::stale_data,
                        "scene material draw index range differs from its packed mesh"});
                }
                const auto max_first_index = static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max());
                if (object_indices.size() > max_first_index ||
                    draw.index_count > max_first_index - object_indices.size()) {
                    return Result<SceneRenderImage>::failure(invalid(
                        "combined transformed index stream exceeds indexed-draw bounds"));
                }
                const auto first_index = static_cast<std::uint32_t>(
                    object_indices.size());
                touched_vertices.clear();
                for (std::uint32_t offset = 0U; offset < draw.index_count; ++offset) {
                    const auto source_index = mesh.value().indices[
                        static_cast<std::size_t>(draw.first_index) + offset];
                    if (source_index >= mesh.value().vertices.size())
                        return Result<SceneRenderImage>::failure({core::ErrorCode::stale_data,
                            "scene material draw references a missing packed vertex"});
                    if (vertex_remap[source_index] == unmapped_vertex) {
                        if (object_vertices.size() >= static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max())) {
                            return Result<SceneRenderImage>::failure(invalid(
                                "combined transformed vertex stream exceeds indexed-draw bounds"));
                        }
                        const auto& source = mesh.value().vertices[source_index];
                        const auto& source_mesh_vertex =
                            object.mesh.vertices[source_index];
                        std::array<std::array<float, 2U>, 5U> transformed_uvs{};
                        for (std::size_t slot = 0U; slot < transformed_uvs.size(); ++slot) {
                            std::optional<core::Vec2d> source_uv;
                            const auto uv_set = transforms.value().uv_sets[slot];
                            if (uv_set == object.uv0_set) {
                                source_uv = source_mesh_vertex.uv;
                            } else if (object.uv1_set.has_value() &&
                                       uv_set == *object.uv1_set) {
                                source_uv = source_mesh_vertex.uv1;
                            } else {
                                const auto additional = std::lower_bound(
                                    object.mesh.additional_uv_set_ids.begin(),
                                    object.mesh.additional_uv_set_ids.end(), uv_set);
                                if (additional !=
                                    object.mesh.additional_uv_set_ids.end() &&
                                    *additional == uv_set) {
                                    const auto additional_index =
                                        static_cast<std::size_t>(std::distance(
                                            object.mesh.additional_uv_set_ids.begin(),
                                            additional));
                                    if (additional_index <
                                        source_mesh_vertex.additional_uvs.size()) {
                                        source_uv = source_mesh_vertex.additional_uvs[
                                            additional_index];
                                    }
                                }
                            }
                            if (!source_uv.has_value() &&
                                texture_id(draw.gpu_constants, slot) != 0U) {
                                return Result<SceneRenderImage>::failure({
                                    core::ErrorCode::unsupported,
                                    "scene material references UV set " +
                                        std::to_string(uv_set) +
                                        " without a matching vertex stream"});
                            }
                            const auto transformed = transform_scene_uv(
                                source_uv.value_or(core::Vec2d{}),
                                transforms.value().transforms[slot]);
                            if (!transformed)
                                return Result<SceneRenderImage>::failure(
                                    transformed.error());
                            transformed_uvs[slot] = transformed.value();
                        }
                        std::array<float, 4U> tangent = source.tangent;
                        if (texture_id(draw.gpu_constants, 2U) != 0U) {
                            const auto& normal_transform = transforms.value().transforms[2U];
                            const double determinant = normal_transform[0U] *
                                normal_transform[3U] - normal_transform[1U] *
                                normal_transform[2U];
                            if (!std::isfinite(determinant) || determinant == 0.0)
                                return Result<SceneRenderImage>::failure(invalid(
                                    "normal-map UV transform is singular or overflows"));
                            const double direction_scale = std::max(
                                std::abs(normal_transform[3U]),
                                std::abs(normal_transform[2U]));
                            if (!std::isfinite(direction_scale) || direction_scale == 0.0)
                                return Result<SceneRenderImage>::failure(invalid(
                                    "normal-map UV transform has no usable tangent direction"));
                            const core::Vec3d normal{source.normal[0U],
                                source.normal[1U], source.normal[2U]};
                            const core::Vec3d source_tangent{source.tangent[0U],
                                source.tangent[1U], source.tangent[2U]};
                            const auto bitangent = core::cross(normal,
                                source_tangent).normalized() * source.tangent[3U];
                            const double orientation = determinant < 0.0 ? -1.0 : 1.0;
                            // The inverse UV Jacobian's first column maps the
                            // tangent into the transformed texture-U direction.
                            const auto transformed_tangent = (
                                source_tangent * (normal_transform[3U] / direction_scale) -
                                bitangent * (normal_transform[2U] / direction_scale)) *
                                orientation;
                            const auto normalized_tangent = transformed_tangent.normalized();
                            if (!normalized_tangent.finite())
                                return Result<SceneRenderImage>::failure(invalid(
                                    "normal-map UV transform collapses its tangent frame"));
                            // The shader flips sampled Y for DirectX Y-. Reverse
                            // handedness to encode the authored OpenGL Y+ basis.
                            const double convention_orientation =
                                transforms.value().open_gl_y_plus_normal ? -1.0 : 1.0;
                            tangent = {static_cast<float>(normalized_tangent.x),
                                static_cast<float>(normalized_tangent.y),
                                static_cast<float>(normalized_tangent.z),
                                static_cast<float>(source.tangent[3U] * orientation *
                                    convention_orientation)};
                        }
                        const auto light_position = transform_position(
                            light_from_local, source.position);
                        if (!std::all_of(light_position.begin(), light_position.end(),
                                [](float value) { return std::isfinite(value); })) {
                            return Result<SceneRenderImage>::failure(invalid(
                                "directional shadow vertex overflows float32"));
                        }
                        const auto authored_color = source_mesh_vertex.color.value_or(
                            core::Vec4d{1.0, 1.0, 1.0, 1.0});
                        const std::array<float, 4U> vertex_color{
                            static_cast<float>(authored_color.x),
                            static_cast<float>(authored_color.y),
                            static_cast<float>(authored_color.z),
                            static_cast<float>(authored_color.w)};
                        object_vertices.push_back({source.position, source.normal,
                            transformed_uvs[0U], tangent, light_position,
                            transformed_uvs[1U], transformed_uvs[2U],
                            transformed_uvs[3U], transformed_uvs[4U],
                            camera_clearcoat, surface_factors, material_flags,
                            vertex_color, channel_mapping_a, channel_mapping_b,
                            channel_mapping_c});
                        vertex_remap[source_index] = static_cast<std::uint32_t>(
                            object_vertices.size() - 1U);
                        touched_vertices.push_back(source_index);
                    }
                    object_indices.push_back(vertex_remap[source_index]);
                }
                object_stream.draws.push_back({first_index, draw.index_count});
                for (const auto source_index : touched_vertices)
                    vertex_remap[source_index] = unmapped_vertex;
            }
            const auto vertex_data = bytes(object_vertices);
            const auto index_data = bytes(object_indices);
            const auto vertex = mesh_resource_owner->buffer({vertex_data.size(),
                gpu::MemoryClass::upload, gpu::BufferUsage::vertex});
            const auto index = mesh_resource_owner->buffer({index_data.size(),
                gpu::MemoryClass::upload, gpu::BufferUsage::index});
            if (!vertex || !index)
                return Result<SceneRenderImage>::failure(
                    !vertex ? vertex.error() : index.error());
            if (auto write = device.write_buffer(vertex.value(), 0U, vertex_data); !write)
                return Result<SceneRenderImage>::failure(write.error());
            if (auto write = device.write_buffer(index.value(), 0U, index_data); !write)
                return Result<SceneRenderImage>::failure(write.error());
            object_stream.buffers = {vertex.value(), index.value(),
                vertex_data.size(), index_data.size()};
        }
    }
    const std::uint32_t shadow_extent = std::min(kShadowMapExtent,
        device.capabilities().max_texture_dimension_2d);
    if (shadow_extent == 0U)
        return Result<SceneRenderImage>::failure({core::ErrorCode::unsupported,
            "D3D12 device reports no usable shadow-map extent"});
    const bool needs_internal_display = !target.has_value();
    const bool target_cache_allowed = active_cache != nullptr &&
        scene_target_allocation_estimate(request.width, request.height,
            shadow_extent, needs_internal_display) <= kSceneTargetCacheBudgetBytes;
    SceneRenderTargets scene_targets;
    bool target_cache_hit = false;
    std::unique_ptr<Resources> pending_target_resources;
    Resources* target_resource_owner = &owned;
    if (target_cache_allowed && active_cache->target_resources != nullptr &&
        active_cache->targets.matches(request.width, request.height, shadow_extent) &&
        active_cache->targets.states_known) {
        scene_targets = active_cache->targets;
        target_resource_owner = active_cache->target_resources.get();
        target_cache_hit = true;
    } else if (target_cache_allowed) {
        active_cache->target_resources.reset();
        active_cache->targets = {};
        pending_target_resources = std::make_unique<Resources>(device);
        target_resource_owner = pending_target_resources.get();
    } else if (active_cache != nullptr) {
        active_cache->target_resources.reset();
        active_cache->targets = {};
    }

    const auto create_scene_targets = [&](Resources& owner) -> Result<SceneRenderTargets> {
        SceneRenderTargets created;
        created.width = request.width;
        created.height = request.height;
        created.shadow_extent = shadow_extent;
        const auto hdr = owner.texture({request.width, request.height, 1U, 1U, 1U,
            gpu::Format::rgba32_float, gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::color_attachment | gpu::TextureUsage::sampled |
                gpu::TextureUsage::transfer_source});
        const auto depth = owner.texture({request.width, request.height, 1U, 1U, 1U,
            gpu::Format::depth32_float, gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::depth_attachment | gpu::TextureUsage::transfer_source});
        const auto shadow_color = owner.texture({shadow_extent, shadow_extent, 1U, 1U, 1U,
            gpu::Format::rgba32_float, gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::color_attachment | gpu::TextureUsage::sampled,
            {1.0F, 1.0F, 1.0F, 1.0F}, 1.0F});
        const auto shadow_depth = owner.texture({shadow_extent, shadow_extent, 1U, 1U, 1U,
            gpu::Format::depth32_float, gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::depth_attachment, {0.0F, 0.0F, 0.0F, 1.0F}, 1.0F});
        if (!hdr || !depth || !shadow_color || !shadow_depth)
            return Result<SceneRenderTargets>::failure(!hdr ? hdr.error() :
                !depth ? depth.error() : !shadow_color ? shadow_color.error() :
                shadow_depth.error());
        created.hdr = hdr.value();
        created.depth = depth.value();
        created.shadow_color = shadow_color.value();
        created.shadow_depth = shadow_depth.value();
        return Result<SceneRenderTargets>::success(std::move(created));
    };
    if (!target_cache_hit) {
        auto created = create_scene_targets(*target_resource_owner);
        if (!created) return Result<SceneRenderImage>::failure(created.error());
        scene_targets = std::move(created.value());
    }
    if (needs_internal_display && !scene_targets.display) {
        const auto display_texture = target_resource_owner->texture({
            request.width, request.height, 1U, 1U, 1U, gpu::Format::rgba32_float,
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source});
        if (!display_texture)
            return Result<SceneRenderImage>::failure(display_texture.error());
        scene_targets.display = display_texture.value();
        scene_targets.display_state = device_ir::ResourceState::undefined;
        if (target_cache_hit) active_cache->targets.display = scene_targets.display;
    }
    const gpu::TextureHandle hdr = scene_targets.hdr;
    const gpu::TextureHandle depth = scene_targets.depth;
    const gpu::TextureHandle shadow_color = scene_targets.shadow_color;
    const gpu::TextureHandle shadow_depth = scene_targets.shadow_depth;
    const gpu::TextureHandle display = target.has_value() ? *target : scene_targets.display;

    render_graph::GraphBuilder graph_builder;
    auto add_graph_texture = [&](std::string name, gpu::Format format, bool imported,
                                 std::uint32_t width = 0U, std::uint32_t height = 0U,
                                 std::uint32_t mip_levels = 1U) {
        render_graph::ResourceDesc descriptor;
        descriptor.name = std::move(name);
        descriptor.kind = render_graph::ResourceKind::texture;
        descriptor.imported = imported;
        descriptor.width = width == 0U ? request.width : width;
        descriptor.height = height == 0U ? request.height : height;
        descriptor.depth = 1U;
        descriptor.layers = 1U;
        descriptor.mip_levels = mip_levels;
        descriptor.format = format;
        return graph_builder.create_resource(std::move(descriptor));
    };
    auto add_graph_buffer = [&](std::string name, std::uint64_t size) {
        render_graph::ResourceDesc descriptor;
        descriptor.name = std::move(name);
        descriptor.kind = render_graph::ResourceKind::buffer;
        descriptor.imported = true;
        descriptor.bytes = size;
        return graph_builder.create_resource(std::move(descriptor));
    };
    const auto graph_shadow_color = add_graph_texture("scene.shadow-map",
        gpu::Format::rgba32_float, false, shadow_extent, shadow_extent);
    const auto graph_shadow_depth = add_graph_texture("scene.shadow-depth",
        gpu::Format::depth32_float, false, shadow_extent, shadow_extent);
    const auto graph_hdr = add_graph_texture("scene.hdr", gpu::Format::rgba32_float, false);
    const auto graph_depth = add_graph_texture("scene.depth", gpu::Format::depth32_float, false);
    const auto graph_display = add_graph_texture("scene.display",
        target.has_value() ? gpu::Format::bgra8_unorm : gpu::Format::rgba32_float,
        target.has_value());
    if (!graph_shadow_color || !graph_shadow_depth || !graph_hdr || !graph_depth || !graph_display)
        return Result<SceneRenderImage>::failure(!graph_shadow_color ? graph_shadow_color.error() :
            !graph_shadow_depth ? graph_shadow_depth.error() : !graph_hdr ? graph_hdr.error() :
            !graph_depth ? graph_depth.error() : graph_display.error());
    using render_graph::ResourceId;
    std::optional<ResourceId> graph_temporal_history;
    std::optional<ResourceId> graph_temporal_motion;
    std::optional<ResourceId> graph_temporal_output;
    ResourceId graph_linear_hdr = graph_hdr.value();
    if (request.temporal.has_value()) {
        const auto output = add_graph_texture("scene.temporal-resolved",
            gpu::Format::rgba32_float, true);
        if (!output) return Result<SceneRenderImage>::failure(output.error());
        graph_temporal_output = output.value();
        graph_linear_hdr = output.value();
        if (request.temporal->history_color) {
            const auto history = add_graph_texture("scene.temporal-history",
                gpu::Format::rgba32_float, true);
            if (!history) return Result<SceneRenderImage>::failure(history.error());
            graph_temporal_history = history.value();
        }
        if (request.temporal->motion_vectors) {
            const auto motion = add_graph_texture(
                "scene.temporal-motion-vectors", gpu::Format::rg32_float, true);
            if (!motion) return Result<SceneRenderImage>::failure(motion.error());
            graph_temporal_motion = motion.value();
        }
    }
    std::optional<ResourceId> graph_frame_target;
    if (frame_target.has_value()) {
        const auto added = add_graph_texture(
            "desktop.back-buffer", gpu::Format::bgra8_unorm, true);
        if (!added) return Result<SceneRenderImage>::failure(added.error());
        graph_frame_target = added.value();
    }
    std::optional<ResourceId> graph_gpu_timing_readback;
    if (gpu_timestamp_readback.has_value()) {
        const auto added = add_graph_buffer("scene.gpu-timing-readback",
            2U * sizeof(std::uint64_t));
        if (!added) return Result<SceneRenderImage>::failure(added.error());
        graph_gpu_timing_readback = added.value();
    }

    std::map<ResourceId, gpu::TextureHandle> graph_textures{
        {graph_shadow_color.value(), shadow_color},
        {graph_shadow_depth.value(), shadow_depth},
        {graph_hdr.value(), hdr}, {graph_depth.value(), depth},
        {graph_display.value(), display}};
    if (request.temporal.has_value()) {
        if (graph_temporal_history.has_value())
            graph_textures.emplace(*graph_temporal_history,
                request.temporal->history_color);
        if (graph_temporal_motion.has_value())
            graph_textures.emplace(*graph_temporal_motion,
                request.temporal->motion_vectors);
        graph_textures.emplace(*graph_temporal_output,
            request.temporal->resolved_color);
    }
    std::map<ResourceId, gpu::BufferHandle> graph_buffers;
    std::set<ResourceId> graph_index_resources;
    std::vector<std::pair<ResourceId, ResourceId>> graph_mesh_resources;
    graph_mesh_resources.reserve(meshes.size());
    for (std::size_t object_index = 0U; object_index < meshes.size(); ++object_index) {
        const auto& mesh = meshes[object_index].buffers;
        const std::string name = "scene.mesh." + std::to_string(object_index);
        const auto vertex = add_graph_buffer(name + ".vertices", mesh.vertex_bytes);
        const auto index = add_graph_buffer(name + ".indices", mesh.index_bytes);
        if (!vertex || !index)
            return Result<SceneRenderImage>::failure(!vertex ? vertex.error() : index.error());
        graph_buffers.emplace(vertex.value(), mesh.vertices);
        graph_buffers.emplace(index.value(), mesh.indices);
        graph_index_resources.insert(index.value());
        graph_mesh_resources.emplace_back(vertex.value(), index.value());
    }
    if (graph_gpu_timing_readback.has_value()) {
        graph_buffers.emplace(*graph_gpu_timing_readback, *gpu_timestamp_readback);
    }
    std::map<ResourceId, device_ir::ResourceState> graph_states;
    if (target.has_value()) graph_states.emplace(graph_display.value(), target_initial_state);
    if (request.temporal.has_value()) {
        if (graph_temporal_history.has_value())
            graph_states.emplace(*graph_temporal_history,
                request.temporal->history_initial_state);
        if (graph_temporal_motion.has_value())
            graph_states.emplace(*graph_temporal_motion,
                request.temporal->motion_initial_state);
        graph_states.emplace(*graph_temporal_output,
            request.temporal->resolved_initial_state);
    }
    if (target_cache_hit) {
        graph_states.emplace(graph_shadow_color.value(), scene_targets.shadow_color_state);
        graph_states.emplace(graph_shadow_depth.value(), scene_targets.shadow_depth_state);
        graph_states.emplace(graph_hdr.value(), scene_targets.hdr_state);
        graph_states.emplace(graph_depth.value(), scene_targets.depth_state);
        if (!target.has_value())
            graph_states.emplace(graph_display.value(), scene_targets.display_state);
    }
    if (mesh_cache_hit) {
        for (const auto& [vertex, index] : graph_mesh_resources) {
            graph_states.emplace(vertex, device_ir::ResourceState::vertex_read);
            graph_states.emplace(index, device_ir::ResourceState::index_read);
        }
    }
    if (graph_frame_target.has_value()) {
        graph_textures.emplace(*graph_frame_target, *frame_target);
        graph_states.emplace(*graph_frame_target, device_ir::ResourceState::present);
    }
    std::map<gpu::TextureHandle, ResourceId> graph_material_resources;
    std::set<gpu::TextureHandle> cached_material_handles;
    if (material_texture_cache_hit) {
        for (const auto& [asset, handle] : material_textures.source_textures) {
            static_cast<void>(asset);
            cached_material_handles.insert(handle);
        }
        cached_material_handles.insert(material_textures.neutral.begin(),
            material_textures.neutral.end());
        cached_material_handles.insert(material_textures.environment_texture);
        cached_material_handles.insert(material_textures.local_lights_texture);
        if (material_textures.split_sum_brdf_lut)
            cached_material_handles.insert(material_textures.split_sum_brdf_lut);
    }
    std::uint32_t material_resource_index = 0U;
    const auto add_material_resource = [&](gpu::TextureHandle handle) -> Result<ResourceId> {
        const auto existing = graph_material_resources.find(handle);
        if (existing != graph_material_resources.end()) return Result<ResourceId>::success(existing->second);
        const auto descriptor = device.describe_texture(handle);
        if (!descriptor) return Result<ResourceId>::failure(descriptor.error());
        const auto& actual = descriptor.value();
        const auto added = add_graph_texture("scene.material." + std::to_string(material_resource_index++),
            actual.format, true, actual.width, actual.height, actual.mip_levels);
        if (!added) return Result<ResourceId>::failure(added.error());
        graph_material_resources.emplace(handle, added.value());
        graph_textures.emplace(added.value(), handle);
        // Newly uploaded textures start in copy-destination; cached textures
        // retain shader-read. The graph transitions each first use.
        graph_states.emplace(added.value(), cached_material_handles.contains(handle)
            ? device_ir::ResourceState::shader_read
            : device_ir::ResourceState::copy_destination);
        return Result<ResourceId>::success(added.value());
    };
    std::set<ResourceId> pbr_material_resources;
    std::set<ResourceId> shadow_material_resources;
    for (const auto& object : request.objects) {
        for (const auto& draw : object.draw_package.draws) {
            const auto& material = draw.gpu_constants;
            for (std::uint32_t slot = 0U; slot < 5U; ++slot) {
                const auto id = texture_id(material, slot);
                const auto texture = id != 0U
                    ? material_textures.source_textures.at(id)
                    : material_textures.neutral[slot];
                const auto resource = add_material_resource(texture);
                if (!resource) return Result<SceneRenderImage>::failure(resource.error());
                pbr_material_resources.insert(resource.value());
                if (slot == 0U) shadow_material_resources.insert(resource.value());
            }
        }
    }
    const auto sh_resource = add_material_resource(
        material_textures.environment_texture);
    if (!sh_resource) return Result<SceneRenderImage>::failure(sh_resource.error());
    pbr_material_resources.insert(sh_resource.value());
    const auto local_light_resource = add_material_resource(
        material_textures.local_lights_texture);
    if (!local_light_resource)
        return Result<SceneRenderImage>::failure(local_light_resource.error());
    pbr_material_resources.insert(local_light_resource.value());
    if (material_textures.split_sum_brdf_lut) {
        const auto brdf_resource = add_material_resource(
            material_textures.split_sum_brdf_lut);
        if (!brdf_resource)
            return Result<SceneRenderImage>::failure(brdf_resource.error());
        pbr_material_resources.insert(brdf_resource.value());
    }

    using render_graph::Access;
    using render_graph::PipelineStage;
    using render_graph::PassDesc;
    using render_graph::ResourceUsage;
    std::vector<ResourceUsage> shadow_usages{
        ResourceUsage{graph_shadow_color.value(), Access::write, PipelineStage::color_output},
        ResourceUsage{graph_shadow_depth.value(), Access::write, PipelineStage::depth_stencil}};
    for (const auto& [vertex, index] : graph_mesh_resources) {
        shadow_usages.push_back({vertex, Access::read, PipelineStage::vertex});
        shadow_usages.push_back({index, Access::read, PipelineStage::vertex});
    }
    for (const auto resource : shadow_material_resources)
        shadow_usages.push_back({resource, Access::read, PipelineStage::fragment});
    const PassDesc shadow_pass{"scene.shadow", std::move(shadow_usages)};
    std::vector<ResourceUsage> pbr_usages{
        ResourceUsage{graph_shadow_color.value(), Access::read, PipelineStage::fragment},
        ResourceUsage{graph_hdr.value(), Access::write, PipelineStage::color_output},
        ResourceUsage{graph_depth.value(), Access::write, PipelineStage::depth_stencil}};
    for (const auto& [vertex, index] : graph_mesh_resources) {
        pbr_usages.push_back({vertex, Access::read, PipelineStage::vertex});
        pbr_usages.push_back({index, Access::read, PipelineStage::vertex});
    }
    for (const auto resource : pbr_material_resources)
        pbr_usages.push_back({resource, Access::read, PipelineStage::fragment});
    const PassDesc pbr_pass{"scene.pbr", std::move(pbr_usages)};
    std::optional<PassDesc> temporal_pass;
    if (request.temporal.has_value()) {
        std::vector<ResourceUsage> temporal_usages{
            ResourceUsage{graph_hdr.value(), Access::read, PipelineStage::compute},
            ResourceUsage{*graph_temporal_output, Access::write,
                PipelineStage::compute}};
        if (graph_temporal_history.has_value())
            temporal_usages.push_back({*graph_temporal_history, Access::read,
                PipelineStage::compute});
        if (graph_temporal_motion.has_value())
            temporal_usages.push_back({*graph_temporal_motion, Access::read,
                PipelineStage::compute});
        temporal_pass = PassDesc{"scene.temporal-resolve",
            std::move(temporal_usages)};
    }
    const PassDesc tone_map_pass{"scene.tonemap", {
        ResourceUsage{graph_linear_hdr, Access::read, PipelineStage::fragment},
        ResourceUsage{graph_display.value(), Access::write, PipelineStage::color_output}}};
    const auto graph_shadow = graph_builder.add_pass(shadow_pass);
    const auto graph_pbr = graph_builder.add_pass(pbr_pass);
    std::optional<render_graph::PassId> graph_temporal;
    if (temporal_pass.has_value()) {
        const auto pass = graph_builder.add_pass(*temporal_pass);
        if (!pass) return Result<SceneRenderImage>::failure(pass.error());
        graph_temporal = pass.value();
    }
    const auto graph_tonemap = graph_builder.add_pass(tone_map_pass);
    if (!graph_shadow || !graph_pbr || !graph_tonemap)
        return Result<SceneRenderImage>::failure(!graph_shadow ? graph_shadow.error() :
            !graph_pbr ? graph_pbr.error() : graph_tonemap.error());
    std::optional<render_graph::PassId> graph_capture_readback;
    if (!target.has_value()) {
        const auto pass = graph_builder.add_pass({"scene.capture-readback", {
            ResourceUsage{graph_linear_hdr, Access::read, PipelineStage::copy},
            ResourceUsage{graph_depth.value(), Access::read, PipelineStage::copy},
            ResourceUsage{graph_display.value(), Access::read, PipelineStage::copy}}});
        if (!pass) return Result<SceneRenderImage>::failure(pass.error());
        graph_capture_readback = pass.value();
    }
    std::optional<render_graph::PassId> graph_target_copy;
    if (target.has_value() && !frame_target.has_value()) {
        const auto pass = graph_builder.add_pass({"scene.target-copy-source", {
            ResourceUsage{graph_display.value(), Access::read, PipelineStage::copy}}});
        if (!pass) return Result<SceneRenderImage>::failure(pass.error());
        graph_target_copy = pass.value();
    }
    std::optional<render_graph::PassId> graph_desktop_copy;
    std::optional<render_graph::PassId> graph_desktop_present;
    if (graph_frame_target.has_value()) {
        const auto copy = graph_builder.add_pass({"desktop.copy-authored-scene", {
            ResourceUsage{graph_display.value(), Access::read, PipelineStage::copy},
            ResourceUsage{*graph_frame_target, Access::write, PipelineStage::copy}}});
        if (!copy) return Result<SceneRenderImage>::failure(copy.error());
        graph_desktop_copy = copy.value();
        const auto present = graph_builder.add_pass({"desktop.present", {
            ResourceUsage{*graph_frame_target, Access::write, PipelineStage::present}}});
        if (!present) return Result<SceneRenderImage>::failure(present.error());
        graph_desktop_present = present.value();
    }
    std::optional<render_graph::PassId> graph_gpu_timing_resolve;
    if (graph_gpu_timing_readback.has_value()) {
        const auto pass = graph_builder.add_pass({"scene.gpu-timing-resolve", {
            ResourceUsage{*graph_gpu_timing_readback, Access::write,
                PipelineStage::copy}}});
        if (!pass) return Result<SceneRenderImage>::failure(pass.error());
        graph_gpu_timing_resolve = pass.value();
    }
    auto compiled_graph = graph_builder.compile();
    if (!compiled_graph) return Result<SceneRenderImage>::failure(compiled_graph.error());
    std::vector<render_graph::PassId> declared_order{
        graph_shadow.value(), graph_pbr.value()};
    if (graph_temporal) declared_order.push_back(*graph_temporal);
    declared_order.push_back(graph_tonemap.value());
    if (graph_capture_readback) declared_order.push_back(*graph_capture_readback);
    if (graph_target_copy) declared_order.push_back(*graph_target_copy);
    if (graph_desktop_copy) declared_order.push_back(*graph_desktop_copy);
    if (graph_desktop_present) declared_order.push_back(*graph_desktop_present);
    if (graph_gpu_timing_resolve) declared_order.push_back(*graph_gpu_timing_resolve);
    if (compiled_graph.value().pass_order != declared_order)
        return Result<SceneRenderImage>::failure(invalid(
            "direct scene render graph compiled to an unexpected pass order"));

    std::map<render_graph::PassId, PassDesc> graph_passes{
        {graph_shadow.value(), shadow_pass},
        {graph_pbr.value(), pbr_pass}, {graph_tonemap.value(), tone_map_pass}};
    if (graph_temporal.has_value())
        graph_passes.emplace(*graph_temporal, *temporal_pass);
    if (graph_capture_readback) graph_passes.emplace(*graph_capture_readback, PassDesc{
        "scene.capture-readback", {
            ResourceUsage{graph_linear_hdr, Access::read, PipelineStage::copy},
            ResourceUsage{graph_depth.value(), Access::read, PipelineStage::copy},
            ResourceUsage{graph_display.value(), Access::read, PipelineStage::copy}}});
    if (graph_target_copy) graph_passes.emplace(*graph_target_copy, PassDesc{
        "scene.target-copy-source", {ResourceUsage{graph_display.value(), Access::read, PipelineStage::copy}}});
    if (graph_desktop_copy) graph_passes.emplace(*graph_desktop_copy, PassDesc{
        "desktop.copy-authored-scene", {
            ResourceUsage{graph_display.value(), Access::read, PipelineStage::copy},
            ResourceUsage{*graph_frame_target, Access::write, PipelineStage::copy}}});
    if (graph_desktop_present) graph_passes.emplace(*graph_desktop_present, PassDesc{
        "desktop.present", {
            ResourceUsage{*graph_frame_target, Access::write, PipelineStage::present}}});
    if (graph_gpu_timing_resolve) graph_passes.emplace(*graph_gpu_timing_resolve,
        PassDesc{"scene.gpu-timing-resolve", {
            ResourceUsage{*graph_gpu_timing_readback, Access::write,
                PipelineStage::copy}}});
    detail::RenderGraphDispatch::ResourceBindings graph_bindings;
    for (const auto& [resource, handle] : graph_textures) {
        graph_bindings.emplace(resource, device_ir::ResourceHandle{handle});
    }
    for (const auto& [resource, handle] : graph_buffers) {
        graph_bindings.emplace(resource, device_ir::ResourceHandle{handle});
    }
    auto graph_dispatch = detail::RenderGraphDispatch::create(
        std::move(compiled_graph).value(), std::move(graph_passes),
        std::move(graph_bindings),
        std::move(graph_states), std::move(graph_index_resources));
    if (!graph_dispatch) return Result<SceneRenderImage>::failure(graph_dispatch.error());
    auto begin_graph_pass = [&](render_graph::PassId pass_id) {
        return graph_dispatch.value().record_pass(pass_id, stream);
    };

    if (capture_gpu_timing) stream.append(device_ir::CmdWriteTimestamp{0U});
    if (auto begun = begin_graph_pass(graph_shadow.value()); !begun)
        return Result<SceneRenderImage>::failure(begun.error());
    stream.append(device_ir::CmdBeginRendering{shadow_color, shadow_depth});
    stream.append(device_ir::CmdClearColor{shadow_color, {1.0F, 1.0F, 1.0F, 1.0F}});
    stream.append(device_ir::CmdClearDepth{shadow_depth, 1.0F});
    stream.append(device_ir::CmdSetViewport{0.0F, 0.0F,
        static_cast<float>(shadow_extent), static_cast<float>(shadow_extent), 0.0F, 1.0F});
    stream.append(device_ir::CmdSetScissor{0, 0, shadow_extent, shadow_extent});
    stream.append(device_ir::CmdBindPipeline{program.shadow_pipeline});
    for (std::size_t i = 0U; i < request.objects.size(); ++i) {
        const auto& object = request.objects[i];
        const auto model = model_matrix(object.world_transform).value();
        if (meshes[i].draws.size() != object.draw_package.draws.size())
            return Result<SceneRenderImage>::failure({core::ErrorCode::stale_data,
                "scene shadow draw ranges differ from the prepared material package"});
        stream.append(device_ir::CmdBindVertexBuffer{meshes[i].buffers.vertices, 0U,
            static_cast<std::uint32_t>(sizeof(ScenePbrUploadVertex))});
        stream.append(device_ir::CmdBindIndexBuffer{meshes[i].buffers.indices, 0U,
            device_ir::IndexFormat::uint32});
        for (std::size_t draw_index = 0U;
             draw_index < object.draw_package.draws.size(); ++draw_index) {
            const auto& draw = object.draw_package.draws[draw_index];
            if (draw.gpu_constants.alpha_factors[1U] ==
                static_cast<float>(render::AlphaMode::blended)) {
                // The current shadow target stores opaque depth only. Treating
                // a blended surface as opaque would invent a solid occluder.
                continue;
            }
            const auto& draw_range = meshes[i].draws[draw_index];
            const auto& material = draw.gpu_constants;
            const auto draw_sampler = sampler_for_draw(i, draw, 0U);
            if (!draw_sampler)
                return Result<SceneRenderImage>::failure(draw_sampler.error());
            std::array<float, 36U> constants{};
            std::copy(light_view.begin(), light_view.end(), constants.begin());
            std::copy(model.begin(), model.end(), constants.begin() + 16);
            constants[32U] = material.alpha_factors[0U];
            constants[33U] = material.base_color_factor[3U];
            constants[34U] = material.alpha_factors[1U] == 1.0F ? 1.0F : 0.0F;
            const auto draw_material_state = transform_for_draw(i, draw);
            if (!draw_material_state)
                return Result<SceneRenderImage>::failure(
                    draw_material_state.error());
            constants[35U] =
                draw_material_state.value().channel_mappings[0U][3U];
            const auto base_color_id = texture_id(material, 0U);
            const auto base_color = base_color_id != 0U
                ? material_textures.source_textures.at(base_color_id)
                : material_textures.neutral[0U];
            stream.append(device_ir::CmdBindResources{{{0U, 0U, base_color,
                draw_sampler.value()}}});
            stream.append(device_ir::CmdPushConstants{push_bytes(constants)});
            stream.append(device_ir::CmdDrawIndexed{draw_range.index_count, 1U,
                draw_range.first_index, 0, 0U});
        }
    }
    stream.append(device_ir::CmdEndRendering{});

    if (auto begun = begin_graph_pass(graph_pbr.value()); !begun)
        return Result<SceneRenderImage>::failure(begun.error());
    stream.append(device_ir::CmdBeginRendering{hdr, depth});
    stream.append(device_ir::CmdClearColor{hdr, {static_cast<float>(request.background_linear.x),
        static_cast<float>(request.background_linear.y), static_cast<float>(request.background_linear.z), 1.0F}});
    stream.append(device_ir::CmdClearDepth{depth, 1.0F});
    stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, static_cast<float>(request.width), static_cast<float>(request.height), 0.0F, 1.0F});
    stream.append(device_ir::CmdSetScissor{0, 0, request.width, request.height});
    const auto view = view_projection(request).value();
    const auto light = render::pack_light_for_gpu(request.light).value();
    struct OrderedDraw {
        std::size_t object_index = 0U;
        std::size_t draw_index = 0U;
        bool blended = false;
        double view_depth = 0.0;
    };
    std::vector<OrderedDraw> draw_order;
    const auto camera_forward =
        (request.camera.target - request.camera.position).normalized();
    for (std::size_t object_index = 0U;
         object_index < request.objects.size(); ++object_index) {
        const auto& object = request.objects[object_index];
        if (meshes[object_index].draws.size() !=
            object.draw_package.draws.size())
            return Result<SceneRenderImage>::failure({core::ErrorCode::stale_data,
                "scene PBR draw ranges differ from the prepared material package"});
        for (std::size_t draw_index = 0U;
             draw_index < object.draw_package.draws.size(); ++draw_index) {
            const auto& draw = object.draw_package.draws[draw_index];
            const bool blended = draw.gpu_constants.alpha_factors[1U] ==
                static_cast<float>(render::AlphaMode::blended);
            double view_depth = 0.0;
            if (blended) {
                if (draw.index_count == 0U ||
                    draw.first_index > object.mesh.indices.size() ||
                    draw.index_count > object.mesh.indices.size() - draw.first_index) {
                    return Result<SceneRenderImage>::failure({
                        core::ErrorCode::stale_data,
                        "blended scene draw has an invalid source index range"});
                }
                core::Vec3d centroid{};
                for (std::uint32_t offset = 0U; offset < draw.index_count; ++offset) {
                    const auto source_index = object.mesh.indices[
                        static_cast<std::size_t>(draw.first_index) + offset];
                    if (source_index >= object.mesh.vertices.size()) {
                        return Result<SceneRenderImage>::failure({
                            core::ErrorCode::stale_data,
                            "blended scene draw references a missing source vertex"});
                    }
                    const auto& vertex = object.mesh.vertices[source_index];
                    const auto world = object.world_transform.translation +
                        object.world_transform.rotation.rotate(
                            core::componentwise_multiply(
                                object.world_transform.scale, vertex.position));
                    centroid = centroid + world;
                }
                centroid = centroid *
                    (1.0 / static_cast<double>(draw.index_count));
                view_depth = core::dot(
                    centroid - request.camera.position, camera_forward);
                if (!std::isfinite(view_depth)) {
                    return Result<SceneRenderImage>::failure(invalid(
                        "blended scene draw depth is not finite"));
                }
            }
            draw_order.push_back({object_index, draw_index, blended, view_depth});
        }
    }
    std::stable_sort(draw_order.begin(), draw_order.end(),
        [](const OrderedDraw& left, const OrderedDraw& right) {
            if (left.blended != right.blended) return !left.blended;
            return left.blended && left.view_depth > right.view_depth;
        });

    stream.append(device_ir::CmdBindPipeline{program.pbr_pipeline});
    bool blended_pipeline_bound = false;
    for (const auto& ordered : draw_order) {
        const std::size_t i = ordered.object_index;
        const std::size_t draw_index = ordered.draw_index;
        const auto& object = request.objects[i];
        if (ordered.blended && !blended_pipeline_bound) {
            stream.append(device_ir::CmdBindPipeline{
                program.pbr_blended_pipeline});
            blended_pipeline_bound = true;
        }
        stream.append(device_ir::CmdBindVertexBuffer{meshes[i].buffers.vertices, 0U,
            static_cast<std::uint32_t>(sizeof(ScenePbrUploadVertex))});
        stream.append(device_ir::CmdBindIndexBuffer{meshes[i].buffers.indices, 0U,
            device_ir::IndexFormat::uint32});
        {
            const auto& draw = object.draw_package.draws[draw_index];
            const auto& draw_range = meshes[i].draws[draw_index];
            std::array<gpu::SamplerHandle, 5U> draw_samplers{};
            for (std::size_t slot = 0U; slot < draw_samplers.size(); ++slot) {
                const auto sampler = sampler_for_draw(i, draw, slot);
                if (!sampler)
                    return Result<SceneRenderImage>::failure(sampler.error());
                draw_samplers[slot] = sampler.value();
            }
            std::array<float, 48U> constants{};
            std::copy(view.begin(), view.end(), constants.begin());
            const auto model = model_matrix(object.world_transform).value();
            std::copy(model.begin(), model.end(), constants.begin() + 16);
            const auto& material = draw.gpu_constants;
            std::copy(material.base_color_factor.begin(), material.base_color_factor.end(), constants.begin() + 32);
            std::copy(material.emissive_radiance_and_f0.begin(), material.emissive_radiance_and_f0.end(), constants.begin() + 36);
            std::copy(light.direction_and_intensity.begin(), light.direction_and_intensity.end(), constants.begin() + 40);
            constants[43U] = material.alpha_factors[1U] == 1.0F ? -material.alpha_factors[0U] : 1.0F;
            std::copy_n(light.radiance_and_range.begin(), 3U, constants.begin() + 44);
            constants[47U] = material.clearcoat_factors[1U];
            device_ir::CmdBindResources bindings;
            for (std::uint32_t slot = 0U; slot < 5U; ++slot) {
                const auto id = texture_id(material, slot);
                const auto sampled_texture = id != 0U
                    ? material_textures.source_textures.at(id)
                    : material_textures.neutral[slot];
                bindings.bindings.push_back({0U, slot, sampled_texture,
                    draw_samplers[slot]});
            }
            bindings.bindings.push_back({0U, 5U, shadow_color, std::nullopt});
            bindings.bindings.push_back({0U, 6U,
                material_textures.environment_texture, std::nullopt});
            bindings.bindings.push_back({0U, 7U,
                material_textures.split_sum_brdf_lut
                    ? material_textures.split_sum_brdf_lut
                    : material_textures.environment_texture,
                std::nullopt});
            bindings.bindings.push_back({0U, 8U,
                material_textures.local_lights_texture, std::nullopt});
            stream.append(std::move(bindings)); stream.append(device_ir::CmdPushConstants{push_bytes(constants)});
            stream.append(device_ir::CmdDrawIndexed{draw_range.index_count, 1U,
                draw_range.first_index, 0, 0U});
            ++result.draw_count;
        }
    }
    stream.append(device_ir::CmdEndRendering{});
    if (graph_temporal.has_value()) {
        if (auto begun = begin_graph_pass(*graph_temporal); !begun)
            return Result<SceneRenderImage>::failure(begun.error());
        const auto& temporal = *request.temporal;
        const auto history_color = temporal.history_color
            ? temporal.history_color : hdr;
        // The shader skips this binding whenever history is absent or zero
        // motion is asserted. Current HDR is a safe descriptor-only fallback;
        // no fallback pixels are read as history or motion.
        const auto motion_vectors = temporal.motion_vectors
            ? temporal.motion_vectors : hdr;
        stream.append(device_ir::CmdBindPipeline{program.temporal_pipeline});
        stream.append(device_ir::CmdBindResources{{
            {0U, 0U, hdr, std::nullopt},
            {0U, 1U, history_color, std::nullopt},
            {0U, 2U, motion_vectors, std::nullopt},
            {0U, 3U, temporal.resolved_color, std::nullopt},
        }});
        const std::array<float, 8U> temporal_float_constants{
            temporal.feedback, 0.0F, 1.0F, 0.0F,
            temporal.constant_motion_pixels.has_value()
                ? static_cast<float>(temporal.constant_motion_pixels->x) : 0.0F,
            temporal.constant_motion_pixels.has_value()
                ? static_cast<float>(temporal.constant_motion_pixels->y) : 0.0F,
            1.0F / static_cast<float>(request.width),
            1.0F / static_cast<float>(request.height)};
        const std::array<std::uint32_t, 4U> temporal_flags{
            temporal.history_color ? 1U : 0U,
            temporal.camera_cut ? 1U : 0U,
            static_cast<std::uint32_t>(request.source_revision.value()),
            temporal.zero_motion ? 1U :
                (temporal.constant_motion_pixels.has_value() &&
                 !temporal.motion_vectors ? 2U : 0U)};
        std::array<std::uint8_t, 48U> temporal_constants{};
        std::memcpy(temporal_constants.data(), temporal_float_constants.data(),
            sizeof(temporal_float_constants));
        std::memcpy(temporal_constants.data() + sizeof(temporal_float_constants),
            temporal_flags.data(), sizeof(temporal_flags));
        stream.append(device_ir::CmdPushConstants{
            std::vector<std::uint8_t>(temporal_constants.begin(),
                temporal_constants.end())});
        stream.append(device_ir::CmdDispatch{
            (request.width + 7U) / 8U, (request.height + 7U) / 8U, 1U});
        stream.append(device_ir::CmdUavBarrier{
            device_ir::ResourceHandle{temporal.resolved_color}});
    }
    if (auto begun = begin_graph_pass(graph_tonemap.value()); !begun)
        return Result<SceneRenderImage>::failure(begun.error());
    stream.append(device_ir::CmdBeginRendering{display, std::nullopt});
    stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, static_cast<float>(request.width), static_cast<float>(request.height), 0.0F, 1.0F});
    stream.append(device_ir::CmdSetScissor{0, 0, request.width, request.height});
    stream.append(device_ir::CmdBindPipeline{program.tone_pipeline});
    stream.append(device_ir::CmdBindVertexBuffer{meshes.front().buffers.vertices, 0U,
        static_cast<std::uint32_t>(sizeof(ScenePbrUploadVertex))});
    const auto tone_map_source = request.temporal.has_value()
        ? request.temporal->resolved_color : hdr;
    stream.append(device_ir::CmdBindResources{{
        {0U, 0U, tone_map_source, program.sampler}}});
    const float fxaa_enabled = request.antialiasing == SceneRenderAntialiasing::fxaa
        ? 1.0F : 0.0F;
    stream.append(device_ir::CmdPushConstants{push_bytes(std::array<float, 4U>{
        static_cast<float>(request.camera.exposure_ev), fxaa_enabled, 0.0F, 0.0F})});
    stream.append(device_ir::CmdDraw{3U, 1U, 0U, 0U}); stream.append(device_ir::CmdEndRendering{});
    if (graph_capture_readback) {
        if (auto begun = begin_graph_pass(*graph_capture_readback); !begun)
            return Result<SceneRenderImage>::failure(begun.error());
    }
    if (graph_target_copy) {
        if (auto begun = begin_graph_pass(*graph_target_copy); !begun)
            return Result<SceneRenderImage>::failure(begun.error());
    }
    if (graph_desktop_copy) {
        if (auto begun = begin_graph_pass(*graph_desktop_copy); !begun)
            return Result<SceneRenderImage>::failure(begun.error());
        stream.append(device_ir::CmdCopyTexture{display, *frame_target});
    }
    if (graph_desktop_present) {
        if (auto begun = begin_graph_pass(*graph_desktop_present); !begun)
            return Result<SceneRenderImage>::failure(begun.error());
    }
    if (graph_gpu_timing_resolve) {
        stream.append(device_ir::CmdWriteTimestamp{1U});
        if (auto begun = begin_graph_pass(*graph_gpu_timing_resolve); !begun)
            return Result<SceneRenderImage>::failure(begun.error());
        stream.append(device_ir::CmdResolveTimestamps{
            *gpu_timestamp_readback, 0U, 2U, 0U});
    }
    if (auto finished = graph_dispatch.value().finish(); !finished)
        return Result<SceneRenderImage>::failure(finished.error());
    result.render_graph_passes = graph_dispatch.value().recorded_passes();
    // submit may have queued native work even if its final signal fails.
    Resources* active_mesh_resources = nullptr;
    Resources* active_material_texture_resources = nullptr;
    Resources* active_target_resources = target_resource_owner != &owned
        ? target_resource_owner : nullptr;
    if (active_cache != nullptr) {
        active_mesh_resources = pending_mesh_resources != nullptr
            ? pending_mesh_resources.get() : active_cache->mesh_resources.get();
        active_material_texture_resources = pending_material_texture_resources != nullptr
            ? pending_material_texture_resources.get()
            : active_cache->material_texture_resources.get();
    }
    owned.submitted = true;
    if (active_cache != nullptr) active_cache->resources->submitted = true;
    if (active_mesh_resources != nullptr) active_mesh_resources->submitted = true;
    if (active_material_texture_resources != nullptr)
        active_material_texture_resources->submitted = true;
    if (active_target_resources != nullptr) {
        active_target_resources->submitted = true;
        scene_targets.states_known = false;
        if (target_cache_hit) active_cache->targets.states_known = false;
    }
    const auto serial = device.submit(stream);
    if (!serial) return Result<SceneRenderImage>::failure(serial.error());
    result.submission_serial = serial.value();
    if (wait_for_completion) {
        if (auto waited = device.wait(serial.value()); !waited)
            return Result<SceneRenderImage>::failure(waited.error());
    }
    if (capture_gpu_timing) {
        const auto timestamp_bytes = device.read_buffer(
            *gpu_timestamp_readback, 0U, 2U * sizeof(std::uint64_t));
        if (!timestamp_bytes) {
            return Result<SceneRenderImage>::failure(timestamp_bytes.error());
        }
        if (timestamp_bytes.value().size() != 2U * sizeof(std::uint64_t)) {
            return Result<SceneRenderImage>::failure(invalid(
                "GPU timestamp readback returned an unexpected byte count"));
        }
        std::array<std::uint64_t, 2U> timestamps{};
        std::memcpy(timestamps.data(), timestamp_bytes.value().data(),
            sizeof(timestamps));
        if (timestamps[1U] < timestamps[0U]) {
            return Result<SceneRenderImage>::failure(invalid(
                "GPU scene timestamp interval moved backwards"));
        }
        const long double elapsed_nanoseconds =
            static_cast<long double>(timestamps[1U] - timestamps[0U]) *
            1'000'000'000.0L /
            static_cast<long double>(gpu_timestamp_frequency);
        constexpr long double uint64_exclusive_limit =
            18446744073709551616.0L;
        const long double rounded_elapsed_nanoseconds =
            std::round(elapsed_nanoseconds);
        if (!std::isfinite(elapsed_nanoseconds) || elapsed_nanoseconds < 0.0L ||
            rounded_elapsed_nanoseconds >= uint64_exclusive_limit) {
            return Result<SceneRenderImage>::failure(invalid(
                "GPU scene timestamp duration is outside the receipt range"));
        }
        result.gpu_timing = SceneRenderGpuTiming{
            static_cast<std::uint64_t>(rounded_elapsed_nanoseconds),
            gpu_timestamp_frequency};
    }
    owned.submitted = false;
    if (active_cache != nullptr) active_cache->resources->submitted = false;
    if (active_mesh_resources != nullptr) active_mesh_resources->submitted = false;
    if (active_material_texture_resources != nullptr)
        active_material_texture_resources->submitted = false;
    if (active_target_resources != nullptr)
        active_target_resources->submitted = false;
    scene_targets.shadow_color_state = device_ir::ResourceState::shader_read;
    scene_targets.shadow_depth_state = device_ir::ResourceState::depth_write;
    scene_targets.hdr_state = (target.has_value() || request.temporal.has_value())
        ? device_ir::ResourceState::shader_read
        : device_ir::ResourceState::copy_source;
    scene_targets.depth_state = target.has_value()
        ? device_ir::ResourceState::depth_write
        : device_ir::ResourceState::copy_source;
    if (!target.has_value())
        scene_targets.display_state = device_ir::ResourceState::copy_source;
    scene_targets.states_known = true;
    if (request.temporal.has_value()) {
        result.temporal_output_state = target.has_value()
            ? device_ir::ResourceState::shader_read
            : device_ir::ResourceState::copy_source;
    }
    if (pending_mesh_resources != nullptr) {
        active_cache->mesh_identity = desired_mesh_identity;
        active_cache->meshes = std::move(meshes);
        active_cache->mesh_resources = std::move(pending_mesh_resources);
    }
    if (pending_material_texture_resources != nullptr) {
        active_cache->material_texture_identity = desired_material_texture_identity;
        active_cache->material_textures = std::move(material_textures);
        active_cache->material_texture_resources =
            std::move(pending_material_texture_resources);
    }
    if (pending_target_resources != nullptr) {
        active_cache->targets = scene_targets;
        active_cache->target_resources = std::move(pending_target_resources);
    } else if (target_cache_hit) {
        active_cache->targets = scene_targets;
    }
    if (target.has_value()) return Result<SceneRenderImage>::success(std::move(result));
    auto display_pixels = device.read_texture_rgba32f(display);
    if (!display_pixels)
        return Result<SceneRenderImage>::failure(display_pixels.error());
    auto display_values = std::move(display_pixels).value();
    result.srgb_rgba.reserve(display_values.size());
    for (float value : display_values) {
        if (!std::isfinite(value) || value < -0.0001F || value > 1.0001F)
            return Result<SceneRenderImage>::failure(invalid("GPU scene display readback is outside finite sRGB bounds"));
        result.srgb_rgba.push_back(static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F)));
    }
    std::vector<float>().swap(display_values);

    const auto linear_hdr_texture = request.temporal.has_value()
        ? request.temporal->resolved_color : hdr;
    auto hdr_pixels = device.read_texture_rgba32f(linear_hdr_texture);
    if (!hdr_pixels)
        return Result<SceneRenderImage>::failure(hdr_pixels.error());
    auto depth_pixels = device.read_texture_depth32f(depth);
    if (!depth_pixels)
        return Result<SceneRenderImage>::failure(depth_pixels.error());
    result.linear_hdr_rgba = std::move(hdr_pixels.value());
    result.depth = std::move(depth_pixels.value());
    for (float value : result.linear_hdr_rgba) if (!std::isfinite(value))
        return Result<SceneRenderImage>::failure(invalid("GPU scene HDR readback contains nonfinite pixels"));
    for (float value : result.depth) if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
        return Result<SceneRenderImage>::failure(invalid("GPU scene depth readback is invalid"));
    result.pixel_digest = assets::sha256(result.srgb_rgba);
    return Result<SceneRenderImage>::success(std::move(result));
}

} // namespace

Result<SceneRenderImage> render_scene(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory) {
    return render_scene(device, request, shader_directory, false);
}

Result<SceneRenderImage> render_scene(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    bool capture_gpu_timing) {
    return render_scene_impl(device, request, shader_directory, std::nullopt,
        device_ir::ResourceState::undefined, std::nullopt, nullptr, nullptr, true,
        capture_gpu_timing);
}

Result<SceneRenderImage> render_scene_with_cache(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    std::optional<gpu::TextureHandle> target,
    device_ir::ResourceState target_initial_state,
    std::optional<gpu::TextureHandle> frame_target,
    bool wait_for_completion,
    bool capture_gpu_timing) {
    if (cache.impl_ == nullptr)
        cache.impl_ = std::make_unique<SceneRenderCache::Impl>();
    return render_scene_impl(device, request, shader_directory, target,
        target_initial_state, frame_target, &cache.impl_->program,
        &cache.impl_->generation, wait_for_completion, capture_gpu_timing);
}

Result<SceneRenderImage> render_scene_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory) {
    return render_scene_cached(device, cache, request, shader_directory, false);
}

Result<SceneRenderImage> render_scene_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    bool capture_gpu_timing) {
    return render_scene_with_cache(device, cache, request, shader_directory,
        std::nullopt, device_ir::ResourceState::undefined, std::nullopt, true,
        capture_gpu_timing);
}

namespace {
SceneRenderReceipt receipt_from_image(const SceneRenderImage& image) {
    SceneRenderReceipt receipt;
    receipt.width = image.width;
    receipt.height = image.height;
    receipt.source_revision = image.source_revision;
    receipt.source_digest = image.source_digest;
    receipt.request_digest = image.request_digest;
    receipt.submission_serial = image.submission_serial;
    receipt.environment_digest = image.environment_digest;
    receipt.environment_color_space = image.environment_color_space;
    receipt.antialiasing = image.antialiasing;
    receipt.shader_digests = image.shader_digests;
    receipt.temporal_output_state = image.temporal_output_state;
    receipt.device = image.device;
    receipt.draw_count = image.draw_count;
    receipt.render_graph_passes = image.render_graph_passes;
    receipt.limitations = image.limitations;
    return receipt;
}
} // namespace

Result<SceneRenderReceipt> render_scene_to_texture(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle target,
    device_ir::ResourceState target_initial_state) {
    const auto rendered = render_scene_impl(device, request, shader_directory, target,
        target_initial_state);
    if (!rendered) return Result<SceneRenderReceipt>::failure(rendered.error());
    return Result<SceneRenderReceipt>::success(receipt_from_image(rendered.value()));
}

Result<SceneRenderReceipt> render_scene_to_texture_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle target,
    device_ir::ResourceState target_initial_state) {
    const auto rendered = render_scene_with_cache(device, cache, request,
        shader_directory, target, target_initial_state, std::nullopt, true, false);
    if (!rendered) return Result<SceneRenderReceipt>::failure(rendered.error());
    return Result<SceneRenderReceipt>::success(receipt_from_image(rendered.value()));
}

Result<SceneRenderReceipt> render_scene_to_texture_with_frame_handoff(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle scene_target,
    device_ir::ResourceState scene_target_initial_state,
    gpu::TextureHandle frame_target) {
    const auto rendered = render_scene_impl(device, request, shader_directory,
        scene_target, scene_target_initial_state, frame_target);
    if (!rendered) return Result<SceneRenderReceipt>::failure(rendered.error());
    return Result<SceneRenderReceipt>::success(receipt_from_image(rendered.value()));
}

Result<SceneRenderReceipt> render_scene_to_texture_with_frame_handoff_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle scene_target,
    device_ir::ResourceState scene_target_initial_state,
    gpu::TextureHandle frame_target) {
    const auto rendered = render_scene_with_cache(device, cache, request,
        shader_directory, scene_target, scene_target_initial_state, frame_target, true,
        false);
    if (!rendered) return Result<SceneRenderReceipt>::failure(rendered.error());
    return Result<SceneRenderReceipt>::success(receipt_from_image(rendered.value()));
}

Result<SceneRenderReceipt> render_scene_to_texture_with_frame_handoff_cached_async(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle scene_target,
    device_ir::ResourceState scene_target_initial_state,
    gpu::TextureHandle frame_target) {
    const auto rendered = render_scene_with_cache(device, cache, request,
        shader_directory, scene_target, scene_target_initial_state, frame_target, false,
        false);
    if (!rendered) return Result<SceneRenderReceipt>::failure(rendered.error());
    return Result<SceneRenderReceipt>::success(receipt_from_image(rendered.value()));
}

} // namespace carto::d3d12
