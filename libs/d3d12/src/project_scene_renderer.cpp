#include <carto/d3d12/scene_renderer.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace carto::d3d12 {
namespace {

// Apply the same aggregate ceiling to graph nodes and caller-supplied values.
constexpr std::size_t kMaxProjectGraphEvaluationItems = 262'144U;

core::Result<gpu::AddressMode> native_address_mode(
    project::SamplerAddressMode mode) {
    switch (mode) {
    case project::SamplerAddressMode::repeat:
        return core::Result<gpu::AddressMode>::success(gpu::AddressMode::repeat);
    case project::SamplerAddressMode::mirrored_repeat:
        return core::Result<gpu::AddressMode>::success(gpu::AddressMode::mirror);
    case project::SamplerAddressMode::clamp_to_edge:
        return core::Result<gpu::AddressMode>::success(gpu::AddressMode::clamp_edge);
    }
    return core::Result<gpu::AddressMode>::failure({core::ErrorCode::unsupported,
        "native scene rendering cannot lower this sampler address mode"});
}

core::Result<gpu::Filter> native_filter(project::SamplerFilter filter) {
    switch (filter) {
    case project::SamplerFilter::nearest:
        return core::Result<gpu::Filter>::success(gpu::Filter::nearest);
    case project::SamplerFilter::linear:
        return core::Result<gpu::Filter>::success(gpu::Filter::linear);
    }
    return core::Result<gpu::Filter>::failure({core::ErrorCode::unsupported,
        "native scene rendering cannot lower this sampler filter"});
}

core::Result<gpu::SamplerDesc> native_sampler_desc(
    const project::SamplerIntent& intent) {
    if (auto valid = intent.validate(); !valid)
        return core::Result<gpu::SamplerDesc>::failure(valid.error());
    const auto u = native_address_mode(intent.address_u);
    const auto v = native_address_mode(intent.address_v);
    const auto w = native_address_mode(intent.address_w);
    const auto minification = native_filter(intent.minification);
    const auto magnification = native_filter(intent.magnification);
    const auto mip = native_filter(intent.mip);
    if (!u || !v || !w || !minification || !magnification || !mip) {
        const core::Diagnostic& diagnostic = !u ? u.error() : !v ? v.error() :
            !w ? w.error() : !minification ? minification.error() :
            !magnification ? magnification.error() : mip.error();
        return core::Result<gpu::SamplerDesc>::failure(diagnostic);
    }
    gpu::SamplerDesc result;
    result.u = u.value();
    result.v = v.value();
    result.w = w.value();
    result.min_filter = minification.value();
    result.mag_filter = magnification.value();
    result.mip_filter = mip.value() == gpu::Filter::nearest
        ? gpu::MipFilter::nearest : gpu::MipFilter::linear;
    return core::Result<gpu::SamplerDesc>::success(result);
}

core::Result<void> collect_material_sampler(
    const project::MaterialCatalog& catalog,
    const render::StandardMaterial& material,
    std::map<render::SamplerId, gpu::SamplerDesc>& output) {
    const std::array<const render::TextureRef*, 5U> bindings{
        &material.base_color, &material.metallic_roughness, &material.normal,
        &material.occlusion, &material.emissive};
    for (const auto* binding : bindings) {
        if (!static_cast<bool>(*binding)) continue;
        if (binding->sampler == 0U || output.contains(binding->sampler)) continue;
        const auto authored = catalog.samplers().find(binding->sampler);
        if (authored == catalog.samplers().end()) {
            return core::Result<void>::failure({core::ErrorCode::stale_data,
                "material sampler intent is missing from the current catalog"});
        }
        const auto descriptor = native_sampler_desc(authored->second);
        if (!descriptor)
            return core::Result<void>::failure(descriptor.error());
        output.emplace(binding->sampler, descriptor.value());
    }
    return core::Result<void>::success();
}

core::Result<void> apply_evaluated_material_graph(
    const project::MaterialGraph& graph,
    const project::MaterialGraphEvaluation& evaluation,
    render::StandardMaterial& material,
    double& sheen_weight) {
    const auto& outputs = graph.outputs;
    if (outputs.normal != 0U || outputs.transmission != 0U ||
        outputs.subsurface != 0U ||
        outputs.anisotropy != 0U || outputs.displacement != 0U) {
        return core::Result<void>::failure({core::ErrorCode::unsupported,
            "native scene rendering cannot lower normal, transmission, subsurface, "
            "anisotropy, or displacement material graph outputs"});
    }

    const auto& base_color = evaluation.node_values.at(outputs.base_color);
    const auto& alpha = evaluation.node_values.at(outputs.alpha);
    const auto& roughness = evaluation.node_values.at(outputs.roughness);
    const auto& metallic = evaluation.node_values.at(outputs.metallic);
    // Opacity is additional coverage, not transmission or an alpha-mode
    // override. Validate each operand before composition: invalid negative or
    // above-one inputs must not cancel into an apparently valid final alpha.
    const double opacity = outputs.opacity == 0U ? 1.0 :
        evaluation.node_values.at(outputs.opacity).components[0U];
    const auto coverage_in_range = [](double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    };
    if (!coverage_in_range(alpha.components[0U]) || !coverage_in_range(opacity)) {
        return core::Result<void>::failure({core::ErrorCode::validation_failed,
            "native scene material graph alpha and opacity must each be in [0, 1]"});
    }
    sheen_weight = outputs.sheen == 0U ? 0.0 :
        evaluation.node_values.at(outputs.sheen).components[0U];
    if (!coverage_in_range(sheen_weight)) {
        return core::Result<void>::failure({core::ErrorCode::validation_failed,
            "native scene material graph sheen must be in [0, 1]"});
    }
    material.base_color_factor = {base_color.components[0U], base_color.components[1U],
        base_color.components[2U], alpha.components[0U] * opacity};
    material.roughness = roughness.components[0U];
    material.metallic = metallic.components[0U];

    if (outputs.occlusion != 0U) {
        material.occlusion_strength =
            evaluation.node_values.at(outputs.occlusion).components[0U];
    }
    if (outputs.emissive != 0U) {
        const auto& emissive = evaluation.node_values.at(outputs.emissive);
        if (emissive.components[3U] != 1.0) {
            return core::Result<void>::failure({core::ErrorCode::unsupported,
                "native scene rendering requires opaque material-graph emissive color"});
        }
        material.emissive_factor = {
            emissive.components[0U], emissive.components[1U], emissive.components[2U]};
        material.emissive_strength = 1.0;
    }
    if (outputs.clearcoat != 0U) {
        material.clearcoat = evaluation.node_values.at(outputs.clearcoat).components[0U];
    }
    if (outputs.clearcoat_roughness != 0U) {
        material.clearcoat_roughness =
            evaluation.node_values.at(outputs.clearcoat_roughness).components[0U];
    }
    return core::Result<void>::success();
}

struct ResolvedProjectLights {
    bool has_visible_source = false;
    std::optional<render::PbrLight> directional;
    std::vector<SceneRenderLocalLight> local;
};

core::Result<ResolvedProjectLights> resolve_project_authored_lights(
    const project::ProjectDocument& document) {
    using Output = core::Result<ResolvedProjectLights>;
    const auto& authored_lights = document.presentation_catalog().lights();
    if (authored_lights.empty()) return Output::success({});

    constexpr std::size_t kMaxDirectionalLights = 64U;
    constexpr std::size_t kMaxLocalLights = 64U;
    constexpr double kDirectionalAlignmentTolerance = 1.0e-10;
    ResolvedProjectLights result;
    std::optional<core::Vec3d> direction_to_light;
    core::Vec3d directional_radiance{};
    std::size_t active_directional_lights = 0U;
    for (const auto& [object_id, authored] : authored_lights) {
        const auto* object = document.scene().find(object_id);
        if (object == nullptr) {
            return Output::failure({core::ErrorCode::stale_data,
                "authored scene light is no longer bound to a scene object"});
        }
        if (!object->visible) continue;
        result.has_visible_source = true;

        if (authored.normal_state != project::LightNormalState::steady ||
            authored.failure_state == project::LightFailureState::intermittent) {
            return Output::failure({core::ErrorCode::unsupported,
                "direct scene capture cannot choose a switchable or intermittent authored light state"});
        }
        // A known-dark source contributes no analytic light and must not fall
        // through to the caller's transient key-light approximation.
        if (authored.failure_state == project::LightFailureState::dark) continue;
        const core::Vec3d source_radiance =
            authored.linear_color * authored.intensity;
        if (!source_radiance.finite()) {
            return Output::failure({core::ErrorCode::validation_failed,
                "authored light radiance exceeds finite capture bounds"});
        }
        if (source_radiance.x == 0.0 && source_radiance.y == 0.0 &&
            source_radiance.z == 0.0) {
            continue;
        }
        const auto world = document.scene().world_transform(object_id);
        if (!world) return Output::failure(world.error());
        if (!world.value().finite()) {
            return Output::failure({core::ErrorCode::validation_failed,
                "authored light transform is non-finite"});
        }
        if (authored.color_temperature_kelvin.has_value()) {
            return Output::failure({core::ErrorCode::unsupported,
                "direct scene capture cannot lower authored color-temperature intent"});
        }
        if (authored.kind == project::AuthoredLightKind::directional) {
            if (!authored.casts_shadows) {
                return Output::failure({core::ErrorCode::unsupported,
                    "direct scene capture cannot lower shadow-disabled directional light"});
            }
            if (active_directional_lights >= kMaxDirectionalLights) {
                return Output::failure({core::ErrorCode::validation_failed,
                    "direct scene capture exceeds the 64 active authored directional-light bound"});
            }
            ++active_directional_lights;
            // Authored objects emit along local -Z, so the PBR direction
            // toward the source is the transformed local +Z axis.
            const auto toward_light = world.value().rotation.normalized()
                .rotate({0.0, 0.0, 1.0}).normalized();
            if (!toward_light.finite()) {
                return Output::failure({core::ErrorCode::validation_failed,
                    "authored directional-light orientation cannot be represented"});
            }
            if (direction_to_light.has_value() &&
                core::dot(*direction_to_light, toward_light) <
                    1.0 - kDirectionalAlignmentTolerance) {
                return Output::failure({core::ErrorCode::unsupported,
                    "direct scene capture supports only co-directional authored key lights"});
            }
            if (!direction_to_light.has_value()) direction_to_light = toward_light;
            directional_radiance = directional_radiance + source_radiance;
            if (!directional_radiance.finite()) {
                return Output::failure({core::ErrorCode::validation_failed,
                    "authored directional-light radiance exceeds finite capture bounds"});
            }
            continue;
        }
        if (authored.kind == project::AuthoredLightKind::point ||
            authored.kind == project::AuthoredLightKind::spot) {
            if (authored.casts_shadows &&
                authored.kind == project::AuthoredLightKind::point) {
                return Output::failure({core::ErrorCode::unsupported,
                    "direct scene capture cannot provide point-light cube shadows"});
            }
            if (result.local.size() >= kMaxLocalLights) {
                return Output::failure({core::ErrorCode::validation_failed,
                    "direct scene capture exceeds the 64 active authored point/spot-light bound"});
            }
            SceneRenderLocalLight local;
            local.object = object_id;
            local.kind = authored.kind == project::AuthoredLightKind::point
                ? SceneRenderLocalLightKind::point : SceneRenderLocalLightKind::spot;
            local.position = world.value().translation;
            local.emission_direction = world.value().rotation.normalized()
                .rotate({0.0, 0.0, -1.0}).normalized();
            local.radiance = source_radiance;
            local.range = authored.range;
            local.inner_cone_radians = authored.inner_cone_radians;
            local.outer_cone_radians = authored.outer_cone_radians;
            local.casts_shadows = authored.casts_shadows;
            if (!local.position.finite() ||
                (local.kind == SceneRenderLocalLightKind::spot &&
                 !local.emission_direction.finite())) {
                return Output::failure({core::ErrorCode::validation_failed,
                    "authored point/spot-light world pose cannot be represented"});
            }
            result.local.push_back(std::move(local));
            continue;
        }
        return Output::failure({core::ErrorCode::unsupported,
            "direct scene capture cannot lower active area-light intent"});
    }
    if (direction_to_light.has_value())
        result.directional = render::PbrLight{*direction_to_light, directional_radiance};
    return Output::success(std::move(result));
}

} // namespace

core::Result<SceneRenderCamera> resolve_project_scene_render_camera(
    const project::ProjectDocument& document,
    scene::ObjectId camera_object,
    double exposure_ev) {
    using Output = core::Result<SceneRenderCamera>;
    if (auto valid = document.validate(); !valid) return Output::failure(valid.error());
    const auto authored = document.presentation_catalog().cameras().find(camera_object);
    if (authored == document.presentation_catalog().cameras().end()) {
        return Output::failure({core::ErrorCode::not_found,
            "selected authored camera is absent from the current presentation catalog"});
    }
    if (auto valid = authored->second.validate(); !valid)
        return Output::failure(valid.error());
    if (!std::isfinite(exposure_ev) || std::abs(exposure_ev) > 20.0 ||
        (authored->second.projection == project::CameraProjection::perspective &&
         authored->second.vertical_fov_radians > 3.13) ||
        (authored->second.projection == project::CameraProjection::orthographic &&
         authored->second.orthographic_height > 1.0e8) ||
        authored->second.near_plane < 0.0001 ||
        authored->second.far_plane > 1.0e8) {
        return Output::failure({core::ErrorCode::validation_failed,
            "authored camera values exceed the direct scene-render bounds"});
    }
    const auto world = document.scene().world_transform(authored->second.object);
    if (!world) return Output::failure(world.error());
    if (!world.value().finite()) {
        return Output::failure({core::ErrorCode::validation_failed,
            "authored camera object has a non-finite world transform"});
    }

    const auto orientation = world.value().rotation.normalized();
    const auto forward = orientation.rotate({0.0, 0.0, -1.0}).normalized();
    const auto up = orientation.rotate({0.0, 1.0, 0.0}).normalized();
    const auto right = core::cross(forward, up).normalized();
    SceneRenderCamera result;
    result.position = world.value().translation;
    result.target = result.position + forward;
    result.up = up;
    result.projection = authored->second.projection ==
        project::CameraProjection::orthographic
        ? SceneRenderProjection::orthographic
        : SceneRenderProjection::perspective;
    result.vertical_fov = authored->second.vertical_fov_radians;
    result.orthographic_height = authored->second.orthographic_height;
    result.near_plane = authored->second.near_plane;
    result.far_plane = authored->second.far_plane;
    result.exposure_ev = exposure_ev;
    if (!result.position.finite() || !result.target.finite() ||
        !forward.finite() || !up.finite() || !right.finite()) {
        return Output::failure({core::ErrorCode::validation_failed,
            "authored camera world basis cannot be represented by the direct renderer"});
    }
    return Output::success(result);
}

core::Result<SceneRenderRequest> prepare_project_scene_render(
    std::string_view project_identity, const project::ProjectDocument& document,
    SceneRenderCamera camera, std::uint32_t width, std::uint32_t height,
    std::vector<SceneRenderTexture> textures, render::PbrLight light,
    SceneRenderMaterialGraphInputs graph_inputs) {
    using Output = core::Result<SceneRenderRequest>;
    if (auto valid = document.validate(); !valid) return Output::failure(valid.error());
    SceneRenderRequest request;
    request.source_revision = document.revision();
    const auto canonical = document.serialize();
    request.source_digest = assets::sha256({reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    request.camera = camera; request.width = width; request.height = height; request.light = light;
    const auto authored_lights = resolve_project_authored_lights(document);
    if (!authored_lights) return Output::failure(authored_lights.error());
    if (authored_lights.value().has_visible_source) {
        request.light = authored_lights.value().directional.value_or(
            render::PbrLight{{0.0, 0.0, 1.0}, {}});
        request.local_lights = authored_lights.value().local;
    }
    request.textures = std::move(textures);
    const auto& catalog = document.material_catalog();
    std::size_t project_graph_evaluation_input_values = 0U;
    if (graph_inputs.size() > 1024U) {
        return Output::failure({core::ErrorCode::validation_failed,
            "scene material graph input map exceeds 1024 graphs"});
    }
    for (const auto& [identity, binding] : graph_inputs) {
        static_cast<void>(identity);
        const auto add_input_count = [&project_graph_evaluation_input_values](
            std::size_t count) {
            if (count > kMaxProjectGraphEvaluationItems -
                    project_graph_evaluation_input_values) {
                return false;
            }
            project_graph_evaluation_input_values += count;
            return true;
        };
        if (!add_input_count(binding.inputs.parameters.size()) ||
            !add_input_count(binding.inputs.surface_inputs.size())) {
            return Output::failure({core::ErrorCode::validation_failed,
                "scene material graph input values exceed the aggregate evaluation bound"});
        }
    }
    for (const auto& [identity, binding] : graph_inputs) {
        const auto graph = catalog.material_graphs().find(identity);
        if (graph == catalog.material_graphs().end() ||
            graph->second.identity != identity) {
            return Output::failure({core::ErrorCode::stale_data,
                "scene material graph inputs reference a missing graph source"});
        }
        const auto graph_digest = graph->second.canonical_digest();
        if (!graph_digest || binding.graph_revision != graph->second.revision ||
            binding.graph_digest != graph_digest.value()) {
            return Output::failure({core::ErrorCode::stale_data,
                "scene material graph inputs are bound to a different source revision"});
        }
        if (auto valid = binding.inputs.validate(); !valid) {
            return Output::failure(valid.error().with_context(
                "scene material graph input binding"));
        }
    }
    struct GraphMaterialFactors {
        render::StandardMaterial definition;
        assets::Sha256Digest digest;
        double sheen_weight = 0.0;
    };
    std::size_t project_graph_evaluation_nodes = 0U;
    std::map<render::MaterialId, GraphMaterialFactors> graph_material_factors;
    std::set<std::string> used_graph_inputs;
    for (const auto& texture : request.textures) {
        const auto current = catalog.textures().find(texture.asset);
        if (current == catalog.textures().end() || current->second.content_digest != texture.current_source_digest ||
            texture.mips.source.digest != current->second.content_digest || texture.mips.levels.empty() ||
            texture.mips.levels.front().width != current->second.width || texture.mips.levels.front().height != current->second.height)
            return Output::failure({core::ErrorCode::stale_data, "render texture differs from the current project source record"});
    }
    for (const auto& object : document.scene().objects_sorted()) {
        if (!object.visible || !object.mesh_asset.has_value()) continue;
        if (request.objects.size() >= 4096U)
            return Output::failure({core::ErrorCode::validation_failed, "project scene exceeds 4096 capture objects"});
        const auto& mesh = document.meshes().at(*object.mesh_asset);
        std::optional<std::string> vertex_color_layer;
        for (const auto& [layer_id, layer] : mesh.attributes().layers()) {
            if (layer.descriptor.type != attributes::AttributeType::color4) continue;
            if (layer.descriptor.domain != attributes::AttributeDomain::vertex &&
                layer.descriptor.domain != attributes::AttributeDomain::corner) {
                return Output::failure({core::ErrorCode::unsupported,
                    "native scene rendering requires vertex- or corner-domain color4 data"});
            }
            if (vertex_color_layer.has_value()) {
                return Output::failure({core::ErrorCode::unsupported,
                    "native scene rendering cannot choose between multiple authored color4 layers"});
            }
            vertex_color_layer = layer_id;
        }
        const bool condition_regions = mesh.attributes().find("condition.material_region") != nullptr;
        const bool generic_regions = mesh.attributes().find("cartographer.material_slot") != nullptr;
        if (condition_regions == generic_regions)
            return Output::failure({core::ErrorCode::validation_failed, "render mesh needs exactly one authored material-region layer"});
        const bool has_uv0 = mesh.uv_sets().find(render::kUv0) != nullptr;
        production::RenderMeshCompileOptions options;
        if (has_uv0) options.uv_set = render::kUv0;
        options.generate_tangents = has_uv0;
        options.vertex_color_layer = vertex_color_layer;
        options.material_slot_layer = condition_regions ? "condition.material_region" : "cartographer.material_slot";
        const auto compiled = production::compile_render_mesh_artifact(project_identity, document, *object.mesh_asset, options);
        if (!compiled) return Output::failure(compiled.error());
        SceneRenderObject rendered;
        rendered.object = object.id; rendered.mesh = compiled.value();
        const auto world = document.scene().world_transform(object.id);
        if (!world) return Output::failure(world.error());
        rendered.world_transform = world.value();
        std::vector<production::LookdevMaterialBinding> bindings;
        for (const auto& submesh : rendered.mesh.submeshes) {
            if (!submesh.material_slot.has_value())
                return Output::failure({core::ErrorCode::validation_failed, "render submesh has no authored material region"});
            const project::MaterialRegionKey key{*object.mesh_asset, *submesh.material_slot};
            const auto assignment = catalog.assignments().find(key);
            if (assignment == catalog.assignments().end())
                return Output::failure({core::ErrorCode::not_found, "render material region has no current assignment"});
            const auto& authored = assignment->second;
            const auto resolved = catalog.resolve_material(authored.material, authored.variant);
            if (!resolved) return Output::failure(resolved.error());
            auto material = catalog.materials().at(authored.material);
            material.definition = resolved.value();
            double sheen_weight = 0.0;
            std::string identity = "project/" + std::string(project_identity) + "/material/" + std::to_string(material.id);
            if (authored.variant.has_value()) identity += "/variant/" + std::to_string(*authored.variant);

            const auto graph_binding = catalog.material_graph_bindings().find(authored.material);
            if (graph_binding != catalog.material_graph_bindings().end()) {
                if (authored.variant.has_value()) {
                    return Output::failure({core::ErrorCode::unsupported,
                        "native scene rendering does not combine graph bindings with material variants"});
                }
                auto factors = graph_material_factors.find(authored.material);
                if (factors == graph_material_factors.end()) {
                    const auto graph = catalog.material_graphs().find(
                        graph_binding->second.graph_identity);
                    if (graph == catalog.material_graphs().end()) {
                        return Output::failure({core::ErrorCode::stale_data,
                            "bound material graph source is missing"});
                    }
                    const auto graph_digest = graph->second.canonical_digest();
                    if (!graph_digest || graph_binding->second.source_revision != graph->second.revision ||
                        graph_binding->second.source_digest != graph_digest.value()) {
                        return Output::failure({core::ErrorCode::stale_data,
                            "bound material graph source has changed since the explicit binding"});
                    }
                    if (graph->second.nodes.size() >
                        kMaxProjectGraphEvaluationItems - project_graph_evaluation_nodes) {
                        return Output::failure({core::ErrorCode::unsupported,
                            "scene material graphs exceed the aggregate CPU evaluation bound"});
                    }
                    project_graph_evaluation_nodes += graph->second.nodes.size();
                    const auto supplied_inputs = graph_inputs.find(graph->first);
                    const auto& evaluation_inputs = supplied_inputs == graph_inputs.end()
                        ? graph_binding->second.inputs : supplied_inputs->second.inputs;
                    if (supplied_inputs == graph_inputs.end()) {
                        const std::size_t bound_input_count =
                            evaluation_inputs.parameters.size() +
                            evaluation_inputs.surface_inputs.size();
                        if (bound_input_count > kMaxProjectGraphEvaluationItems -
                                project_graph_evaluation_input_values) {
                            return Output::failure({core::ErrorCode::validation_failed,
                                "persistent scene material graph inputs exceed the aggregate evaluation bound"});
                        }
                        project_graph_evaluation_input_values += bound_input_count;
                        if (auto valid = project::validate_material_graph_input_subset(
                                graph->second, evaluation_inputs); !valid) {
                            return Output::failure(valid.error().with_context(
                                "persistent scene material graph inputs"));
                        }
                    }
                    const auto evaluation = project::evaluate_material_graph(
                        graph->second, evaluation_inputs);
                    if (!evaluation) {
                        return Output::failure(evaluation.error().with_context(
                            "native scene material graph evaluation"));
                    }
                    if (supplied_inputs != graph_inputs.end()) {
                        used_graph_inputs.insert(graph->first);
                    }
                    if (auto applied = apply_evaluated_material_graph(
                            graph->second, evaluation.value(), material.definition,
                            sheen_weight); !applied) {
                        return Output::failure(applied.error());
                    }
                    const std::string graph_context =
                        "carto.d3d12.scene-material-graph-evaluation.v1\n" +
                        graph_digest.value().hex() + "\n" +
                        evaluation.value().inputs_digest.hex();
                    const auto evaluated_graph_digest = assets::sha256({
                        reinterpret_cast<const std::uint8_t*>(graph_context.data()),
                        graph_context.size()});
                    factors = graph_material_factors.emplace(authored.material,
                        GraphMaterialFactors{material.definition, evaluated_graph_digest,
                            sheen_weight}).first;
                } else {
                    material.definition = factors->second.definition;
                    sheen_weight = factors->second.sheen_weight;
                }
                identity += "/graph/" + factors->second.digest.hex();
            }

            if (identity.size() > 128U) {
                // The desktop project identity plus an exact graph digest can
                // exceed the artifact bound. Hash the complete context rather
                // than truncate project/material/variant/graph lineage.
                const std::string context = "carto.scene-material-identity.v1\n" + identity;
                const auto digest = assets::sha256({
                    reinterpret_cast<const std::uint8_t*>(context.data()), context.size()});
                identity = (graph_binding == catalog.material_graph_bindings().end()
                    ? "scene/material/" : "scene/graph/") + digest.hex();
            }
            if (auto collected = collect_material_sampler(
                    catalog, material.definition, request.samplers); !collected) {
                return Output::failure(collected.error());
            }
            const auto artifact = compile_scene_render_material_artifact(identity, material);
            if (!artifact) return Output::failure(artifact.error());
            rendered.materials.push_back({submesh.material_slot, identity, material,
                artifact.value(), sheen_weight});
            bindings.push_back({submesh.material_slot, artifact.value()});
        }
        std::set<render::UvSetId> requested_uv_sets;
        std::optional<render::UvSetId> normal_uv_set;
        for (const auto& binding : rendered.materials) {
            const auto& material = binding.current_material.definition;
            const std::array<const render::TextureRef*, 5U> texture_slots{
                &material.base_color, &material.metallic_roughness,
                &material.normal, &material.occlusion, &material.emissive};
            for (std::size_t slot = 0U; slot < texture_slots.size(); ++slot) {
                if (!static_cast<bool>(*texture_slots[slot])) continue;
                requested_uv_sets.insert(texture_slots[slot]->uv_set);
                if (slot == 2U) {
                    if (normal_uv_set.has_value() &&
                        *normal_uv_set != texture_slots[slot]->uv_set) {
                        return Output::failure({core::ErrorCode::unsupported,
                            "native scene object cannot use multiple normal-map UV sets with one tangent stream"});
                    }
                    normal_uv_set = texture_slots[slot]->uv_set;
                }
            }
        }
        if (requested_uv_sets.size() > production::kMaxRenderMeshArtifactUvStreams) {
            return Output::failure({core::ErrorCode::unsupported,
                "native scene mesh artifacts exceed the authored UV-stream bound"});
        }
        for (const auto uv_set : requested_uv_sets) {
            if (mesh.uv_sets().find(uv_set) == nullptr) {
                return Output::failure({core::ErrorCode::not_found,
                    "bound material references missing authored UV set " +
                        std::to_string(uv_set)});
            }
        }
        std::optional<render::UvSetId> primary_uv_set;
        if (normal_uv_set.has_value()) {
            primary_uv_set = normal_uv_set;
        } else if (!requested_uv_sets.empty()) {
            primary_uv_set = *requested_uv_sets.begin();
        } else if (has_uv0) {
            primary_uv_set = render::kUv0;
        }
        std::optional<render::UvSetId> secondary_uv_set;
        std::vector<render::UvSetId> additional_uv_sets;
        for (const auto uv_set : requested_uv_sets) {
            if (primary_uv_set == uv_set) continue;
            if (!secondary_uv_set.has_value()) secondary_uv_set = uv_set;
            else additional_uv_sets.push_back(uv_set);
        }
        auto material_options = options;
        material_options.uv_set = primary_uv_set;
        material_options.uv1_set = secondary_uv_set;
        material_options.additional_uv_sets = additional_uv_sets;
        material_options.generate_tangents = primary_uv_set.has_value();
        if (material_options.uv_set != options.uv_set ||
            material_options.uv1_set != options.uv1_set ||
            material_options.additional_uv_sets != options.additional_uv_sets ||
            material_options.generate_tangents != options.generate_tangents) {
            const auto material_mesh = production::compile_render_mesh_artifact(
                project_identity, document, *object.mesh_asset, material_options);
            if (!material_mesh) return Output::failure(material_mesh.error());
            rendered.mesh = material_mesh.value();
        }
        rendered.uv0_set = primary_uv_set.value_or(render::kUv0);
        rendered.uv1_set = secondary_uv_set;
        const auto package = production::compile_lookdev_draw_package(rendered.mesh, bindings);
        if (!package) return Output::failure(package.error());
        rendered.draw_package = package.value(); request.objects.push_back(std::move(rendered));
    }
    if (used_graph_inputs.size() != graph_inputs.size()) {
        return Output::failure({core::ErrorCode::stale_data,
            "scene material graph inputs were not consumed by a visible bound material"});
    }
    if (auto valid = request.validate(); !valid) return Output::failure(valid.error());
    return Output::success(std::move(request));
}
} // namespace carto::d3d12
