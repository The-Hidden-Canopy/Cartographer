#include <carto/production/material_binding.hpp>
#include <carto/production/authoring_light_state.hpp>
#include <carto/production/authoring_viewport.hpp>
#include <carto/production/render_mesh.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <tuple>

namespace carto::production {
namespace {
using core::Vec3d;
core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}
core::Quaternion inverse(core::Quaternion q) {
    q = q.normalized();
    return {-q.x, -q.y, -q.z, q.w};
}
Vec3d world_point(const core::Transform &t, Vec3d p) {
    return t.translation + t.rotation.rotate(core::componentwise_multiply(t.scale, p));
}
struct Vertex {
    Vec3d world;
    Vec3d normal;
    Vec3d camera;
    core::Vec2d uv;
    Vec3d tangent;
    double tangent_sign = 1;
};
Vertex mix(const Vertex &a, const Vertex &b, double t) {
    return {a.world * (1.0 - t) + b.world * t,   a.normal * (1.0 - t) + b.normal * t,
            a.camera * (1.0 - t) + b.camera * t, a.uv * (1 - t) + b.uv * t,
            a.tangent * (1 - t) + b.tangent * t, a.tangent_sign * (1 - t) + b.tangent_sign * t};
}
// Clip before perspective division. Interpolated varyings remain in world
// space until perspective-correct interpolation at the sample location.
std::vector<Vertex> clip(std::vector<Vertex> polygon, double plane, bool near) {
    std::vector<Vertex> result;
    if (polygon.empty())
        return result;
    Vertex previous = polygon.back();
    auto distance = [=](const Vertex &v) {
        return near ? -v.camera.z - plane : plane + v.camera.z;
    };
    for (const auto &current : polygon) {
        const double a = distance(previous), b = distance(current);
        if ((a >= 0.0) != (b >= 0.0))
            result.push_back(mix(previous, current, a / (a - b)));
        if (b >= 0.0)
            result.push_back(current);
        previous = current;
    }
    return result;
}
Vec3d screen(Vec3d camera, const AuthoringView &view, double width, double height) {
    const double span = view.projection == project::CameraProjection::perspective
                            ? -2.0 * camera.z * std::tan(view.vertical_fov_radians * 0.5)
                            : view.orthographic_height;
    const double aspect = view.aspect_ratio > 0.0 ? view.aspect_ratio : width / height;
    return {width * 0.5 + camera.x * width / (span * aspect),
            height * 0.5 - camera.y * height / span, -camera.z};
}
double edge(Vec3d a, Vec3d b, double x, double y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}
} // namespace

core::Result<AuthoringDisplay> resolve_authoring_display(const project::ProjectDocument &document,
                                                         const AuthoringViewportRequest &request) {
    AuthoringDisplay display{request.exposure_ev, request.tone_mode};
    if (request.camera_display) {
        if (!request.camera)
            return core::Result<AuthoringDisplay>::failure(
                invalid("camera display requires an authored camera"));
        const auto camera = document.presentation_catalog().cameras().find(*request.camera);
        if (camera == document.presentation_catalog().cameras().end())
            return core::Result<AuthoringDisplay>::failure(
                invalid("camera display source is missing"));
        display = {camera->second.exposure_ev, camera->second.tone_mode};
    }
    if (!std::isfinite(display.exposure_ev) || std::abs(display.exposure_ev) > 32.0 ||
        static_cast<unsigned>(display.tone_mode) > 2U)
        return core::Result<AuthoringDisplay>::failure(invalid("camera display intent is invalid"));
    return core::Result<AuthoringDisplay>::success(display);
}

core::Result<void> AuthoringView::validate() const {
    if (!position.finite() || !rotation.normalizable() || !std::isfinite(near_plane) ||
        !std::isfinite(far_plane) || near_plane <= 0.0 || far_plane <= near_plane ||
        !std::isfinite(aspect_ratio) || aspect_ratio < 0.0 || !std::isfinite(orthographic_height) ||
        orthographic_height <= 0.0 || !std::isfinite(vertical_fov_radians) ||
        vertical_fov_radians <= 0.0 || vertical_fov_radians >= std::numbers::pi ||
        (projection != project::CameraProjection::orthographic &&
         projection != project::CameraProjection::perspective)) {
        return core::Result<void>::failure(invalid("authoring viewport camera is invalid"));
    }
    return core::Result<void>::success();
}
core::Result<Vec3d> AuthoringView::project(Vec3d world, std::uint32_t width,
                                           std::uint32_t height) const {
    if (auto valid = validate(); !valid)
        return core::Result<Vec3d>::failure(valid.error());
    const Vec3d p = inverse(rotation).rotate(world - position);
    if (!world.finite() || width == 0U || height == 0U || -p.z < near_plane || -p.z > far_plane)
        return core::Result<Vec3d>::failure(invalid("point lies outside the camera depth range"));
    const auto projected = screen(p, *this, width, height);
    if (!projected.finite())
        return core::Result<Vec3d>::failure(invalid("camera projection overflow"));
    return core::Result<Vec3d>::success(projected);
}

static core::Result<AuthoringViewportImage>
render_reference(const project::ProjectDocument &document, const AuthoringViewportRequest &request,
                 bool depth_only) {
    using Result = core::Result<AuthoringViewportImage>;
    const auto started = std::chrono::steady_clock::now();
    AuthoringViewportImage image;
    image.width = request.width;
    image.height = request.height;
    image.source_revision = document.revision();
    image.view = request.view;
    if (request.width == 0U || request.height == 0U)
        return Result::success(std::move(image));
    if (auto valid = request.shadows.validate(); !valid)
        return Result::failure(valid.error());
    constexpr std::uint64_t max_pixels = 1'048'576U;
    constexpr std::uint64_t max_visits = 16'777'216U;
    const std::uint64_t count = std::uint64_t(request.width) * request.height;
    const auto display = resolve_authoring_display(document, request);
    if (!display)
        return Result::failure(display.error());
    if (count > max_pixels)
        return Result::failure(invalid("authoring viewport exceeds its pixel/exposure budget"));
    if (auto valid = document.validate(); !valid)
        return Result::failure(valid.error());
    std::shared_ptr<const EnvironmentLighting> environment;
    const auto &environment_source = document.presentation_catalog().environment();
    if (!depth_only && environment_source && environment_source->enabled &&
        environment_source->intensity > 0) {
        const auto key = environment_content_digest(*environment_source);
        if (!key)
            return Result::failure(key.error());
        environment = request.environment_lighting;
        if (environment && environment->source_digest() != key.value())
            return Result::failure(invalid("viewport environment product is stale"));
        if (!environment) {
            auto built = compile_environment_lighting(*environment_source);
            if (!built)
                return Result::failure(built.error());
            environment = std::move(built.value());
        }
    }
    const std::string canonical_document = document.serialize();
    if (canonical_document.empty() || canonical_document.size() > 128U * 1024U * 1024U)
        return Result::failure(invalid("viewport project source exceeds its byte budget"));
    const AuthoritativeSource source{
        "project/authoring.viewport", document.revision(),
        assets::sha256(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(canonical_document.data()),
            canonical_document.size()})};
    std::map<std::tuple<std::uint64_t, render::UvSetId, bool>, RenderMeshArtifact> compiled_meshes;
    std::map<std::pair<render::MaterialId, std::optional<project::MaterialVariantId>>,
             AuthoringMaterialProgram>
        programs;
    std::uint64_t geometry_bytes = 0;
    const auto admit_geometry = [&](const RenderMeshArtifact &mesh) {
        const auto bytes = mesh.vertices.size() * sizeof(RenderMeshVertex) +
                           mesh.indices.size() * sizeof(std::uint32_t) +
                           mesh.triangle_faces.size() * sizeof(geometry::FaceId) +
                           mesh.submeshes.size() * sizeof(RenderMeshSubmesh);
        if (bytes > 64U * 1024U * 1024U - geometry_bytes)
            return false;
        geometry_bytes += bytes;
        return true;
    };
    if (request.camera) {
        const auto camera = document.presentation_catalog().cameras().find(*request.camera);
        if (camera == document.presentation_catalog().cameras().end())
            return Result::failure(invalid("authored viewport camera does not exist"));
        const auto transform = document.scene().world_transform(*request.camera);
        if (!transform)
            return Result::failure(transform.error());
        image.view = {transform.value().translation,
                      transform.value().rotation,
                      camera->second.projection,
                      camera->second.vertical_fov_radians,
                      camera->second.orthographic_height,
                      camera->second.near_plane,
                      camera->second.far_plane};
        image.view.aspect_ratio = request.view.aspect_ratio;
    }
    if (auto valid = image.view.validate(); !valid)
        return Result::failure(valid.error());
    std::vector<render::PbrLight> lights;
    std::vector<bool> casts_shadows;
    for (const auto &[object, light] : document.presentation_catalog().lights()) {
        const auto *light_object = document.scene().find(object);
        if (!light_object || !light_object->visible)
            continue;
        const auto active = authoring_light_is_active(light);
        if (!active) return Result::failure(active.error());
        if (!active.value()) continue;
        if (light.kind != project::AuthoredLightKind::directional)
            return Result::failure(invalid("reference viewport supports directional lights only"));
        if (lights.size() == 4U)
            return Result::failure(invalid("reference viewport supports at most four lights"));
        auto transform = document.scene().world_transform(object);
        if (!transform)
            return Result::failure(transform.error());
        if (const auto override = request.transform_overrides.find(object);
            override != request.transform_overrides.end())
            transform = core::Result<core::Transform>::success(override->second);
        lights.push_back({transform.value().rotation.rotate({0.0, 0.0, 1.0}),
                          light.linear_color * light.intensity});
        casts_shadows.push_back(light.casts_shadows);
    }
    struct Shadow {
        DirectionalShadowFrame frame;
        std::vector<float> depth;
    };
    std::vector<std::optional<Shadow>> shadows(lights.size());
    if (!depth_only && request.shadows.enabled)
        for (std::size_t i = 0; i < lights.size(); ++i) {
            if (!casts_shadows[i])
                continue;
            auto fit = fit_directional_shadow(document, request, lights[i].direction_to_light);
            if (!fit)
                return Result::failure(fit.error());
            if (!fit.value())
                continue;
            auto shadow_request = request;
            shadow_request.camera.reset();
            shadow_request.camera_display = false;
            shadow_request.view = fit.value()->view;
            shadow_request.width = shadow_request.height = request.shadows.resolution;
            shadow_request.shadows.enabled = false;
            auto map = render_reference(document, shadow_request, true);
            if (!map)
                return Result::failure(map.error());
            Shadow shadow{*fit.value(), {}};
            shadow.depth.reserve(map.value().depth.size());
            for (const auto depth : map.value().depth)
                shadow.depth.push_back(
                    std::isfinite(depth) ? float(std::clamp((depth - shadow.frame.view.near_plane) /
                                                                (shadow.frame.view.far_plane -
                                                                 shadow.frame.view.near_plane),
                                                            0., 1.))
                                         : 1.F);
            shadows[i] = std::move(shadow);
        }
    image.diagnostics.push_back(
        "CPU reference: directional direct light, bounded material/UV/texture "
        "graph evaluation; "
        "scene-fitted directional shadow PCF and bounded directional environment; not native GPU "
        "execution "
        "evidence");
    if (lights.empty() && !environment)
        image.diagnostics.push_back("No authored directional light: only emission is visible");
    if (!depth_only)
        image.linear_hdr.resize(static_cast<std::size_t>(count));
    image.depth.assign(static_cast<std::size_t>(count), std::numeric_limits<double>::infinity());
    if (!depth_only) {
        image.objects.resize(static_cast<std::size_t>(count));
        image.faces.resize(static_cast<std::size_t>(count));
        image.display_rgba8.resize(static_cast<std::size_t>(count) * 4U);
    }
    image.attachment_bytes =
        count * (depth_only ? sizeof(double)
                            : sizeof(core::Vec3d) + sizeof(double) + sizeof(scene::ObjectId) +
                                  sizeof(geometry::FaceId) + 4U);
    const auto camera_inverse = inverse(image.view.rotation);
    std::uint64_t triangle_count = 0U;
    for (const auto &object : document.scene().objects_sorted()) {
        if (!object.visible || !object.mesh_asset)
            continue;
        const auto mesh = document.meshes().find(*object.mesh_asset);
        if (mesh == document.meshes().end())
            return Result::failure(invalid("viewport mesh asset is missing"));
        RenderMeshCompileOptions options;
        auto slots=authoring_material_slot_layer(mesh->second);
        if(!slots)return Result::failure(slots.error());
        options.material_slot_layer=slots.value();
        const auto base_key = std::tuple{*object.mesh_asset, render::UvSetId{0}, false};
        auto compiled = compiled_meshes.find(base_key);
        if (compiled == compiled_meshes.end()) {
            auto artifact = compile_render_mesh_artifact(source, mesh->second, options);
            if (!artifact)
                return Result::failure(artifact.error());
            if (!admit_geometry(artifact.value()))
                return Result::failure(invalid("reference geometry cache budget exceeded"));
            compiled = compiled_meshes.emplace(base_key, std::move(artifact.value())).first;
        }
        const auto &artifact = compiled->second;
        triangle_count += artifact.indices.size() / 3U;
        if (triangle_count > 65'536U)
            return Result::failure(invalid("reference viewport triangle budget exceeded"));
        auto transform = document.scene().world_transform(object.id);
        if (!transform)
            return Result::failure(transform.error());
        if (const auto override = request.transform_overrides.find(object.id);
            override != request.transform_overrides.end())
            transform = core::Result<core::Transform>::success(override->second);
        const auto &t = transform.value();
        if (!t.finite() || t.scale.x == 0.0 || t.scale.y == 0.0 || t.scale.z == 0.0)
            return Result::failure(invalid("viewport transform is singular or invalid"));
        for (const auto &submesh : artifact.submeshes) {
            AuthoringMaterialProgram program;
            const auto &catalog = document.material_catalog();
            const auto assignment = catalog.assignments().find(
                {*object.mesh_asset, submesh.material_slot.value_or(0U)});
            if (assignment != catalog.assignments().end()) {
                const auto key = std::pair{assignment->second.material, assignment->second.variant};
                auto ready = programs.find(key);
                if (ready == programs.end()) {
                    if (programs.size() == 32)
                        return Result::failure(
                            invalid("reference viewport material program budget exceeded"));
                    auto resolved = compile_authoring_material(catalog, key.first, key.second,
                                                               request.textures.get());
                    if (!resolved)
                        return Result::failure(resolved.error());
                    ready = programs.emplace(key, std::move(resolved.value())).first;
                }
                program = ready->second;
            }
            const RenderMeshArtifact *active = &artifact;
            const RenderMeshSubmesh *range = &submesh;
            if (const auto uv = program.uv_set()) {
                const auto key = std::tuple{*object.mesh_asset, *uv, program.normal_mapping()};
                auto ready = compiled_meshes.find(key);
                if (ready == compiled_meshes.end()) {
                    auto textured_options = options;
                    textured_options.uv_set = *uv;
                    textured_options.generate_tangents = program.normal_mapping();
                    auto textured =
                        compile_render_mesh_artifact(source, mesh->second, textured_options);
                    if (!textured)
                        return Result::failure(textured.error());
                    if (!admit_geometry(textured.value()))
                        return Result::failure(
                            invalid("reference UV/tangent geometry cache budget exceeded"));
                    ready = compiled_meshes.emplace(key, std::move(textured.value())).first;
                }
                active = &ready->second;
                const auto selected = std::find_if(
                    active->submeshes.begin(), active->submeshes.end(),
                    [&](const auto &s) { return s.material_slot == submesh.material_slot; });
                if (selected == active->submeshes.end())
                    return Result::failure(invalid("textured mesh material region is missing"));
                range = &*selected;
            }
            std::optional<AuthoringMaterialSample> uniform;
            if (!program.per_fragment()) {
                auto evaluated = program.evaluate({});
                if (!evaluated)
                    return Result::failure(evaluated.error());
                uniform = std::move(evaluated.value());
                if (uniform->material.alpha_mode == render::AlphaMode::masked &&
                    uniform->material.base_color_factor.w < uniform->material.alpha_cutoff)
                    continue;
            }
            for (std::size_t index = range->first_index;
                 index < range->first_index + range->index_count; index += 3U) {
                std::vector<Vertex> polygon;
                for (std::size_t corner = 0U; corner < 3U; ++corner) {
                    const auto &vertex = active->vertices[active->indices[index + corner]];
                    const Vec3d world = world_point(t, vertex.position);
                    const Vec3d normal =
                        t.rotation
                            .rotate({vertex.normal.x / t.scale.x, vertex.normal.y / t.scale.y,
                                     vertex.normal.z / t.scale.z})
                            .normalized();
                    const Vec3d camera = camera_inverse.rotate(world - image.view.position);
                    if (!world.finite() || !normal.finite() || !camera.finite())
                        return Result::failure(invalid("viewport transform overflow"));
                    Vec3d tangent{};
                    double sign = 1;
                    if (program.normal_mapping()) {
                        if (!vertex.tangent)
                            return Result::failure(
                                invalid("normal map requires authored-UV tangents"));
                        const auto &local = *vertex.tangent;
                        // Match the native vertex contract before interpolation;
                        // nonuniform scale must not weight longer tangents more.
                        tangent = t.rotation
                                      .rotate(core::componentwise_multiply(
                                          t.scale, {local.x, local.y, local.z}))
                                      .normalized();
                        sign = local.w *
                               ((t.scale.x < 0) ^ (t.scale.y < 0) ^ (t.scale.z < 0) ? -1 : 1);
                        if (!tangent.finite())
                            return Result::failure(
                                invalid("normal-map tangent transform overflow"));
                    }
                    polygon.push_back(
                        {world, normal, camera, vertex.uv.value_or(core::Vec2d{}), tangent, sign});
                }
                if (program.normal_mapping() &&
                    (polygon[0].tangent_sign != polygon[1].tangent_sign ||
                     polygon[0].tangent_sign != polygon[2].tangent_sign))
                    return Result::failure(
                        invalid("tangent handedness changes within a primitive"));
                polygon = clip(clip(std::move(polygon), image.view.near_plane, true),
                               image.view.far_plane, false);
                for (std::size_t fan = 1U; fan + 1U < polygon.size(); ++fan) {
                    const std::array<Vertex, 3> v{polygon[0], polygon[fan], polygon[fan + 1U]};
                    const std::array<Vec3d, 3> p{
                        screen(v[0].camera, image.view, image.width, image.height),
                        screen(v[1].camera, image.view, image.width, image.height),
                        screen(v[2].camera, image.view, image.width, image.height)};
                    if (!p[0].finite() || !p[1].finite() || !p[2].finite())
                        return Result::failure(invalid("viewport projection overflow"));
                    const double area = edge(p[0], p[1], p[2].x, p[2].y);
                    if (std::abs(area) < 1e-12)
                        continue;
                    const auto receiver_plane=core::cross(v[1].world-v[0].world,v[2].world-v[0].world).normalized();
                    const int x0 = static_cast<int>(std::clamp(
                        std::floor(std::min({p[0].x, p[1].x, p[2].x})), 0.0, double(image.width)));
                    const int x1 = static_cast<int>(std::clamp(
                        std::ceil(std::max({p[0].x, p[1].x, p[2].x})), 0.0, double(image.width)));
                    const int y0 = static_cast<int>(std::clamp(
                        std::floor(std::min({p[0].y, p[1].y, p[2].y})), 0.0, double(image.height)));
                    const int y1 = static_cast<int>(std::clamp(
                        std::ceil(std::max({p[0].y, p[1].y, p[2].y})), 0.0, double(image.height)));
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x) {
                            if (++image.sample_visits > max_visits)
                                return Result::failure(
                                    invalid("reference viewport sample budget exceeded"));
                            std::array<double, 3> w{edge(p[1], p[2], x + 0.5, y + 0.5) / area,
                                                    edge(p[2], p[0], x + 0.5, y + 0.5) / area,
                                                    edge(p[0], p[1], x + 0.5, y + 0.5) / area};
                            if (w[0] < -1e-10 || w[1] < -1e-10 || w[2] < -1e-10)
                                continue;
                            std::array<double, 3> reciprocal{1, 1, 1};
                            double denominator = 1;
                            if (image.view.projection == project::CameraProjection::perspective) {
                                for (std::size_t c = 0U; c < 3U; ++c)
                                    w[c] *= (reciprocal[c] = 1 / p[c].z);
                                const double sum = w[0] + w[1] + w[2];
                                denominator = sum;
                                for (auto &weight : w)
                                    weight /= sum;
                            }
                            const double depth = w[0] * p[0].z + w[1] * p[1].z + w[2] * p[2].z;
                            const auto pixel = static_cast<std::size_t>(y) * image.width +
                                               static_cast<std::size_t>(x);
                            if (depth >= image.depth[pixel])
                                continue;
                            Vec3d normal =
                                (v[0].normal * w[0] + v[1].normal * w[1] + v[2].normal * w[2])
                                    .normalized();
                            const Vec3d world =
                                v[0].world * w[0] + v[1].world * w[1] + v[2].world * w[2];
                            auto sampled = uniform;
                            if (!sampled) {
                                const auto uv = v[0].uv * w[0] + v[1].uv * w[1] + v[2].uv * w[2];
                                const auto gradient = [&](std::array<double, 3> bary) {
                                    core::Vec2d numerator{};
                                    double d = 0;
                                    for (unsigned c = 0; c < 3; ++c) {
                                        numerator = numerator + v[c].uv * (bary[c] * reciprocal[c]);
                                        d += bary[c] * reciprocal[c];
                                    }
                                    return (numerator - uv * d) * (1 / denominator);
                                };
                                auto evaluated = program.evaluate(
                                    {uv,
                                     gradient({(p[1].y - p[2].y) / area, (p[2].y - p[0].y) / area,
                                               (p[0].y - p[1].y) / area}),
                                     gradient({(p[2].x - p[1].x) / area, (p[0].x - p[2].x) / area,
                                               (p[1].x - p[0].x) / area})});
                                if (!evaluated)
                                    return Result::failure(evaluated.error());
                                sampled = std::move(evaluated.value());
                            }
                            const auto &material = sampled->material;
                            if (material.alpha_mode == render::AlphaMode::masked &&
                                material.base_color_factor.w < material.alpha_cutoff)
                                continue;
                            if (depth_only) {
                                image.depth[pixel] = depth;
                                continue;
                            }
                            if (sampled->normal_mapped) {
                                auto tangent =
                                    v[0].tangent * w[0] + v[1].tangent * w[1] + v[2].tangent * w[2];
                                tangent =
                                    (tangent - normal * core::dot(normal, tangent)).normalized();
                                if (tangent.length_squared() < 1e-12)
                                    return Result::failure(
                                        invalid("normal-map tangent frame is degenerate"));
                                const auto bitangent =
                                    core::cross(normal, tangent) * v[0].tangent_sign;
                                const auto n = sampled->tangent_normal;
                                normal =
                                    (tangent * n.x + bitangent * n.y + normal * n.z).normalized();
                            }
                            render::PbrSurface surface;
                            surface.normal = normal;
                            surface.view_direction =
                                image.view.projection == project::CameraProjection::perspective
                                    ? (image.view.position - world).normalized()
                                    : image.view.rotation.rotate({0.0, 0.0, 1.0});
                            Vec3d hdr = material.emissive_factor * material.emissive_strength;
                            if (environment) {
                                auto ibl = evaluate_environment_lighting(
                                    *environment, *environment_source, material, normal,
                                    surface.view_direction, sampled->ambient_occlusion);
                                if (!ibl)
                                    return Result::failure(ibl.error());
                                hdr = hdr + ibl.value();
                            }
                            for (std::size_t l = 0; l < lights.size(); ++l) {
                                auto light = lights[l];
                                if (shadows[l]) {
                                    auto visibility = sample_directional_shadow(
                                        shadows[l]->frame, shadows[l]->depth, world,
                                        receiver_plane, light.direction_to_light);
                                    if (!visibility)
                                        return Result::failure(visibility.error());
                                    light.radiance = light.radiance * visibility.value();
                                }
                                auto evaluated =
                                    render::evaluate_pbr_reference(material, surface, light);
                                if (!evaluated)
                                    return Result::failure(evaluated.error());
                                hdr =
                                    hdr + evaluated.value().hdr_color - evaluated.value().emissive;
                            }
                            if (!hdr.finite())
                                return Result::failure(invalid("viewport radiance overflow"));
                            image.depth[pixel] = depth;
                            image.objects[pixel] = object.id;
                            image.faces[pixel] = active->triangle_faces[index / 3U];
                            image.linear_hdr[pixel] = hdr;
                        }
                }
            }
        }
    }
    for (std::size_t pixel = 0U; pixel < image.linear_hdr.size(); ++pixel) {
        if (!image.objects[pixel])
            continue;
        const auto exposed =
            render::apply_exposure(image.linear_hdr[pixel], display.value().exposure_ev);
        if (!exposed)
            return Result::failure(exposed.error());
        const auto mapped = render::tone_map(exposed.value(), display.value().tone_mode);
        if (!mapped)
            return Result::failure(mapped.error());
        const auto encoded = render::linear_to_srgb(mapped.value());
        if (!encoded)
            return Result::failure(encoded.error());
        const std::array<double, 3> rgb{encoded.value().x, encoded.value().y, encoded.value().z};
        for (std::size_t c = 0U; c < 3U; ++c)
            image.display_rgba8[pixel * 4U + c] =
                static_cast<std::uint8_t>(std::lround(rgb[c] * 255.0));
        image.display_rgba8[pixel * 4U + 3U] = 255U;
    }
    image.host_milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return Result::success(std::move(image));
}
core::Result<AuthoringViewportImage>
render_authoring_viewport(const project::ProjectDocument &document,
                          const AuthoringViewportRequest &request) {
    return render_reference(document, request, false);
}
} // namespace carto::production
