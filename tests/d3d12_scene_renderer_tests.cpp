#include "project_document_access.hpp"

#include <carto/d3d12/scene_renderer.hpp>
#include <carto/d3d12/scene_anisotropy.hpp>
#include <carto/d3d12/scene_sheen.hpp>
#include <carto/d3d12/scene_camera_navigation.hpp>
#include <carto/d3d12/temporal_sampling.hpp>
#include <carto/d3d12/viewport_input.hpp>
#include <carto/geometry/primitives.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {
using namespace carto;

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-scene-textures-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
template <typename T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void take(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
d3d12::SceneRenderRequest request();
void rebind(d3d12::SceneRenderObject& object);
d3d12::SceneRenderTexture texture(std::uint64_t id, std::array<std::uint8_t, 4U> rgba,
    assets::TextureSemantic semantic);
assets::Sha256Digest hash(std::string_view text) {
    return assets::sha256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
std::vector<std::uint8_t> one_pixel_tga() {
    std::vector<std::uint8_t> bytes(22U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = 1U;
    bytes[14U] = 1U;
    bytes[16U] = 32U;
    bytes[17U] = 8U;
    bytes[18U] = 0x20U;
    bytes[19U] = 0x40U;
    bytes[20U] = 0xe0U;
    bytes[21U] = 0xffU;
    return bytes;
}
std::vector<std::uint8_t> equirectangular_png_with_srgb_metadata();
std::vector<std::uint8_t> sparse_alpha_tga() {
    std::vector<std::uint8_t> bytes(18U + 4U * 4U * 4U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = 4U;
    bytes[14U] = 4U;
    bytes[16U] = 32U;
    bytes[17U] = 8U;
    bytes[18U + 3U] = 255U;
    return bytes;
}
geometry::EditableMesh authored_box() {
    auto mesh = take(geometry::make_box({1.0, 1.0, 1.0}));
    const auto topology = take(mesh.topology());
    std::vector<attributes::AttributeValue> uvs, normals;
    for (const auto& corner : topology.corners) {
        const auto* face = mesh.find_face(corner.face);
        const auto a = mesh.find_vertex(face->vertices[0U])->position;
        const auto b = mesh.find_vertex(face->vertices[1U])->position;
        const auto c = mesh.find_vertex(face->vertices[2U])->position;
        const auto normal = core::cross(b - a, c - a).normalized();
        const auto position = mesh.find_vertex(corner.vertex)->position;
        normals.emplace_back(normal);
        if (std::abs(normal.x) > 0.9) uvs.emplace_back(core::Vec2d{position.z + 0.5, position.y + 0.5});
        else if (std::abs(normal.y) > 0.9) uvs.emplace_back(core::Vec2d{position.x + 0.5, position.z + 0.5});
        else uvs.emplace_back(core::Vec2d{position.x + 0.5, position.y + 0.5});
    }
    attributes::AttributeSet payload({{attributes::AttributeDomain::corner, topology.corners.size()},
        {attributes::AttributeDomain::face, mesh.faces_sorted().size()}});
    require(static_cast<bool>(payload.add_layer({{"uv.fixture", attributes::AttributeDomain::corner,
        attributes::AttributeType::vec2}, std::move(uvs)})), "fixture authored UVs");
    require(static_cast<bool>(payload.add_layer({{"cartographer.normal0", attributes::AttributeDomain::corner,
        attributes::AttributeType::vec3}, std::move(normals)})), "fixture authored hard-edge normals");
    require(static_cast<bool>(payload.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
        attributes::AttributeType::uint32}, std::vector<attributes::AttributeValue>(mesh.faces_sorted().size(), std::uint32_t{1U})})), "fixture authored material regions");
    attributes::UvSetTable sets;
    require(static_cast<bool>(sets.add({1U, "Fixture UV0", "uv.fixture", true, true, false, 1U})), "fixture UV identity");
    require(static_cast<bool>(mesh.set_attribute_payload(std::move(payload), std::move(sets))), "fixture mesh payload");
    return mesh;
}
project::ProjectDocument room_project() {
    auto document = take(project::ProjectDocument::create("Synthetic direct-light room acceptance fixture"));
    auto access = project::testing::access(document);
    project::MaterialCatalog catalog;
    const std::array<core::Vec4d, 5U> colors{{{0.55, 0.48, 0.36, 1.0}, {0.12, 0.055, 0.025, 1.0},
        {0.7, 0.72, 0.69, 1.0}, {0.025, 0.16, 0.2, 1.0}, {0.7, 0.5, 0.16, 1.0}}};
    for (std::size_t i = 0U; i < colors.size(); ++i) {
        render::MaterialAsset material{i + 1U, "Authored fixture surface " + std::to_string(i), {}, core::Revision{1U}};
        material.definition.base_color_factor = colors[i]; material.definition.roughness = i == 4U ? 0.24 : 0.72;
        material.definition.metallic = i == 4U ? 0.8 : 0.0;
        require(static_cast<bool>(catalog.insert_material(material)), "fixture source material");
    }
    const auto add = [&](std::string name, core::Vec3d position, core::Vec3d size, std::uint64_t material) {
        const auto mesh = take(access.add_mesh(authored_box()));
        core::Transform transform; transform.translation = position; transform.scale = size;
        const auto object_id = take(access.create_object(std::move(name), transform));
        require(static_cast<bool>(access.attach_mesh(object_id, mesh)), "fixture mesh binding");
        require(static_cast<bool>(catalog.insert_assignment({{mesh, 1U}, material, {}})), "fixture region assignment");
    };
    add("Floor", {0.0, -0.1, 0.0}, {6.0, 0.2, 6.0}, 1U);
    add("Back wall", {0.0, 1.5, -3.0}, {6.0, 3.0, 0.16}, 1U);
    add("Left wall", {-3.0, 1.5, 0.0}, {0.16, 3.0, 6.0}, 1U);
    add("Headboard", {-0.7, 0.9, -2.65}, {2.3, 1.6, 0.15}, 2U);
    add("Bed frame", {-0.7, 0.25, -1.3}, {2.2, 0.4, 2.8}, 2U);
    add("Mattress", {-0.7, 0.6, -1.3}, {2.15, 0.35, 2.75}, 3U);
    add("Cover", {-0.7, 0.8, -0.85}, {2.18, 0.08, 1.8}, 4U);
    add("Left pillow", {-1.25, 0.88, -2.1}, {0.8, 0.18, 0.5}, 3U);
    add("Right pillow", {-0.25, 0.88, -2.1}, {0.8, 0.18, 0.5}, 3U);
    add("Nightstand", {1.15, 0.45, -2.1}, {0.65, 0.9, 0.7}, 2U);
    add("Lamp base", {1.15, 0.95, -2.1}, {0.4, 0.08, 0.4}, 5U);
    add("Lamp stem", {1.15, 1.15, -2.1}, {0.06, 0.4, 0.06}, 5U);
    add("Lamp shade", {1.15, 1.4, -2.1}, {0.55, 0.35, 0.55}, 3U);
    add("Cabinet", {2.3, 0.6, -0.3}, {0.9, 1.2, 1.5}, 2U);
    require(static_cast<bool>(document.set_material_catalog(std::move(catalog))), "fixture catalog admission");
    require(static_cast<bool>(document.validate()), "whole saved fixture validates");
    return document;
}
project::ProjectDocument parameterized_graph_project() {
    auto document = room_project();
    auto catalog = document.material_catalog();
    project::MaterialGraph graph;
    graph.identity = "fixture/parameterized";
    graph.revision = core::Revision{1U};
    project::MaterialGraphNode roughness;
    roughness.id = 1U;
    roughness.kind = project::MaterialGraphNodeKind::parameter_scalar;
    roughness.value_type = project::MaterialGraphValueType::scalar;
    roughness.parameter_name = "roughness";
    project::MaterialGraphNode surface;
    surface.id = 2U;
    surface.kind = project::MaterialGraphNodeKind::surface_input;
    surface.value_type = project::MaterialGraphValueType::scalar;
    surface.surface_input = project::MaterialSurfaceInput::authored_mask;
    project::MaterialGraphNode combined_roughness;
    combined_roughness.id = 3U;
    combined_roughness.kind = project::MaterialGraphNodeKind::add;
    combined_roughness.value_type = project::MaterialGraphValueType::scalar;
    combined_roughness.inputs = {roughness.id, surface.id};
    project::MaterialGraphNode color;
    color.id = 4U;
    color.kind = project::MaterialGraphNodeKind::constant_color;
    color.value_type = project::MaterialGraphValueType::color;
    color.constants = {0.6, 0.4, 0.2, 1.0};
    project::MaterialGraphNode alpha;
    alpha.id = 5U;
    alpha.constants[0U] = 1.0;
    project::MaterialGraphNode metallic;
    metallic.id = 6U;
    metallic.constants[0U] = 0.0;
    graph.nodes = {roughness, surface, combined_roughness, color, alpha, metallic};
    graph.outputs.base_color = color.id;
    graph.outputs.alpha = alpha.id;
    graph.outputs.roughness = combined_roughness.id;
    graph.outputs.metallic = metallic.id;
    require(static_cast<bool>(catalog.insert_material_graph(graph)) &&
            static_cast<bool>(catalog.bind_material_graph(1U, graph.identity)) &&
            static_cast<bool>(document.set_material_catalog(std::move(catalog))),
        "fixture binds a graph with one explicit roughness input");
    return document;
}

project::ProjectDocument opacity_graph_project(
    double alpha, std::optional<double> opacity, render::AlphaMode mode) {
    auto document = room_project();
    auto catalog = document.material_catalog();
    auto material = catalog.materials().at(1U);
    material.definition.alpha_mode = mode;
    material.definition.alpha_cutoff = 0.375;
    material.revision = material.revision.next();
    take(catalog.replace_material(material));
    project::MaterialGraph graph;
    graph.identity = "fixture/opacity";
    graph.revision = core::Revision{1U};
    project::MaterialGraphNode color;
    color.id = 1U;
    color.kind = project::MaterialGraphNodeKind::constant_color;
    color.value_type = project::MaterialGraphValueType::color;
    color.constants = {0.6, 0.4, 0.2, 1.0};
    project::MaterialGraphNode alpha_node;
    alpha_node.id = 2U;
    alpha_node.constants[0U] = alpha;
    project::MaterialGraphNode roughness;
    roughness.id = 3U;
    roughness.constants[0U] = 0.5;
    project::MaterialGraphNode metallic;
    metallic.id = 4U;
    graph.nodes = {color, alpha_node, roughness, metallic};
    graph.outputs.base_color = color.id;
    graph.outputs.alpha = alpha_node.id;
    graph.outputs.roughness = roughness.id;
    graph.outputs.metallic = metallic.id;
    if (opacity.has_value()) {
        project::MaterialGraphNode opacity_node;
        opacity_node.id = 5U;
        opacity_node.constants[0U] = *opacity;
        graph.nodes.push_back(opacity_node);
        graph.outputs.opacity = opacity_node.id;
    }
    take(catalog.insert_material_graph(graph));
    take(catalog.bind_material_graph(1U, graph.identity));
    take(document.set_material_catalog(std::move(catalog)));
    return document;
}

void project_graph_opacity_preserves_coverage_policy_and_source() {
    d3d12::SceneRenderCamera camera;
    camera.position = {5.0, 3.1, 5.5};
    camera.target = {-0.3, 0.8, -1.0};
    const auto prepare = [&](const project::ProjectDocument& document) {
        return d3d12::prepare_project_scene_render(
            "graph-opacity", document, camera, 96U, 96U);
    };
    for (const auto mode : {render::AlphaMode::opaque,
            render::AlphaMode::masked, render::AlphaMode::blended}) {
        for (const double opacity : {0.0, 0.5, 1.0}) {
            const auto document = opacity_graph_project(0.5, opacity, mode);
            const auto before = document.serialize();
            const auto prepared = take(prepare(document));
            std::size_t bound_materials = 0U;
            for (const auto& object : prepared.objects) {
                for (const auto& binding : object.materials) {
                    if (binding.current_material.id != 1U) continue;
                    ++bound_materials;
                    const auto& material = binding.current_material.definition;
                    require(material.base_color_factor.w == 0.5 * opacity &&
                            material.alpha_mode == mode && material.alpha_cutoff == 0.375,
                        "graph coverage composes without changing authored alpha policy");
                }
            }
            require(bound_materials == 3U && document.serialize() == before,
                "shared graph materials lower consistently without mutating the project");
            const auto reopened = take(project::ProjectDocument::deserialize(before));
            require(take(take(prepare(reopened)).digest()) == take(prepared.digest()),
                "opacity source and prepared request identity survive saved-project reopen");
        }
    }
    const auto absent = take(prepare(opacity_graph_project(
        0.5, std::nullopt, render::AlphaMode::masked)));
    require(absent.objects.front().materials.front().current_material.definition
            .base_color_factor.w == 0.5,
        "graphs without opacity retain their existing alpha result");
    for (const double alpha : {0.0, 1.0}) {
        const auto endpoint = take(prepare(opacity_graph_project(
            alpha, 0.5, render::AlphaMode::blended)));
        require(endpoint.objects.front().materials.front().current_material.definition
                .base_color_factor.w == alpha * 0.5,
            "zero and one alpha retain exact multiplicative endpoint behavior");
    }
    const auto invalid_legacy_alpha = prepare(opacity_graph_project(
        2.0, std::nullopt, render::AlphaMode::opaque));
    require(!invalid_legacy_alpha &&
            invalid_legacy_alpha.error().code == core::ErrorCode::validation_failed,
        "missing opacity and opaque mode cannot hide an invalid graph alpha");

    // Products inside [0, 1] must not hide invalid individual operands.
    for (const auto values : {std::array{-0.5, -0.5}, std::array{2.0, 0.25},
            std::array{0.25, 2.0}, std::array{0.0, -0.5}, std::array{-0.5, 0.0}}) {
        const auto document = opacity_graph_project(
            values[0U], values[1U], render::AlphaMode::blended);
        const auto before = document.serialize();
        const auto refused = prepare(document);
        require(!refused && refused.error().code == core::ErrorCode::validation_failed &&
                document.serialize() == before,
            "invalid alpha or opacity fails before publishing a prepared scene");
    }

    auto parameterized = opacity_graph_project(0.5, 0.5, render::AlphaMode::blended);
    auto catalog = parameterized.material_catalog();
    auto graph = catalog.material_graphs().at("fixture/opacity");
    graph.revision = graph.revision.next();
    auto& opacity = graph.nodes.back();
    opacity.kind = project::MaterialGraphNodeKind::parameter_scalar;
    opacity.constants = {};
    opacity.parameter_name = "opacity";
    take(catalog.replace_material_graph(graph));
    take(catalog.bind_material_graph(1U, graph.identity));
    take(parameterized.set_material_catalog(catalog));
    const auto missing = prepare(parameterized);
    require(!missing && missing.error().code == core::ErrorCode::not_found,
        "missing opacity input cannot silently become fully opaque");
    project::MaterialGraphEvaluationInputs inputs;
    project::MaterialGraphValue value;
    value.components[0U] = 0.5;
    inputs.parameters.emplace("opacity", value);
    const auto graph_digest = take(graph.canonical_digest());
    take(catalog.set_material_graph_inputs(
        1U, graph.identity, graph.revision, graph_digest, inputs));
    take(parameterized.set_material_catalog(catalog));
    const auto saved_request = take(prepare(parameterized));
    require(saved_request.objects.front().materials.front().current_material.definition
            .base_color_factor.w == 0.25,
        "durable typed opacity inputs reach composed material coverage");
    const auto reopened = take(project::ProjectDocument::deserialize(parameterized.serialize()));
    require(take(take(prepare(reopened)).digest()) == take(saved_request.digest()),
        "durable opacity parameters survive serialization and exact request replay");
    inputs.parameters.at("opacity").components[0U] = 0.75;
    d3d12::SceneRenderMaterialGraphInputs override_inputs;
    override_inputs.emplace(graph.identity,
        d3d12::SceneRenderMaterialGraphInputBinding{graph.revision, graph_digest, inputs});
    const auto overridden = take(d3d12::prepare_project_scene_render(
        "graph-opacity", parameterized, camera, 96U, 96U, {},
        {{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}}, override_inputs));
    require(overridden.objects.front().materials.front().current_material.definition
            .base_color_factor.w == 0.375 &&
            take(overridden.digest()) != take(saved_request.digest()),
        "transient opacity changes coverage and request identity");
    auto invalid_inputs = override_inputs;
    invalid_inputs.at(graph.identity).inputs.parameters.at("opacity").components[0U] = 2.0;
    const auto invalid_parameter = d3d12::prepare_project_scene_render(
        "graph-opacity", parameterized, camera, 96U, 96U, {},
        {{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}}, invalid_inputs);
    require(!invalid_parameter &&
            invalid_parameter.error().code == core::ErrorCode::validation_failed,
        "typed but out-of-range transient opacity is refused before lowering");
    auto wrong_digest = override_inputs;
    wrong_digest.at(graph.identity).graph_digest = hash("different-opacity-source");
    const auto digest_mismatch = d3d12::prepare_project_scene_render(
        "graph-opacity", parameterized, camera, 96U, 96U, {},
        {{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}}, wrong_digest);
    require(!digest_mismatch && digest_mismatch.error().code == core::ErrorCode::stale_data,
        "same-revision opacity inputs still require the exact graph digest");
    override_inputs.at(graph.identity).graph_revision = core::Revision{1U};
    const auto stale = d3d12::prepare_project_scene_render(
        "graph-opacity", parameterized, camera, 96U, 96U, {},
        {{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}}, override_inputs);
    require(!stale && stale.error().code == core::ErrorCode::stale_data,
        "opacity cannot bypass graph input source pinning");

    using OutputMember = std::uint64_t project::MaterialGraphOutputs::*;
    for (const OutputMember output : {&project::MaterialGraphOutputs::transmission,
            &project::MaterialGraphOutputs::subsurface,
            &project::MaterialGraphOutputs::displacement}) {
        auto document = opacity_graph_project(1.0, 0.5, render::AlphaMode::blended);
        auto unsupported_catalog = document.material_catalog();
        auto unsupported_graph = unsupported_catalog.material_graphs().at("fixture/opacity");
        unsupported_graph.revision = unsupported_graph.revision.next();
        unsupported_graph.outputs.*output = 5U;
        take(unsupported_catalog.replace_material_graph(unsupported_graph));
        take(unsupported_catalog.bind_material_graph(1U, unsupported_graph.identity));
        take(document.set_material_catalog(std::move(unsupported_catalog)));
        const auto refused = prepare(document);
        require(!refused && refused.error().code == core::ErrorCode::unsupported,
            "opacity never silently substitutes for unsupported physical material outputs");
    }
}

void project_graph_neutral_physical_outputs_preserve_source() {
    using OutputMember = std::uint64_t project::MaterialGraphOutputs::*;
    const auto prepare = [](const project::ProjectDocument& document) {
        return d3d12::prepare_project_scene_render(
            "graph-neutral-physical", document, {}, 96U, 96U);
    };
    for (const OutputMember output : {&project::MaterialGraphOutputs::transmission,
            &project::MaterialGraphOutputs::subsurface,
            &project::MaterialGraphOutputs::displacement}) {
        for (const double value : {0.0, -0.0, 0.5, -0.5, 1.0e-100}) {
            auto document = opacity_graph_project(1.0, 1.0, render::AlphaMode::opaque);
            const auto baseline = take(prepare(document));
            auto catalog = document.material_catalog();
            auto graph = catalog.material_graphs().at("fixture/opacity");
            graph.revision = graph.revision.next();
            project::MaterialGraphNode physical;
            physical.id = 6U;
            physical.constants[0U] = value;
            graph.nodes.push_back(physical);
            graph.outputs.*output = physical.id;
            take(catalog.replace_material_graph(graph));
            take(catalog.bind_material_graph(1U, graph.identity));
            take(document.set_material_catalog(catalog));
            const auto before = document.serialize();
            const auto result = prepare(document);
            require(document.serialize() == before, "physical output admission never edits authored graphs");
            if (value != 0.0) {
                require(!result && result.error().code == core::ErrorCode::unsupported,
                    "even sub-float32 nonzero physical effects require real implementation");
                continue;
            }
            const auto& prepared = take(result);
            require(take(prepared.digest()) != take(baseline.digest()),
                "neutral explicit physical outputs retain their own graph provenance");
            const auto reopened = take(project::ProjectDocument::deserialize(before));
            require(take(take(prepare(reopened)).digest()) == take(prepared.digest()),
                "neutral physical output identities survive exact saved-project replay");
            require(prepared.objects.front().materials.front().current_material.definition
                    .base_color_factor.w == 1.0,
                "zero transmission is not substituted with transparency");

            // A durable zero-valued parameter is admissible, but a transient
            // nonzero override must still refuse even after that preparation.
            graph.revision = graph.revision.next();
            graph.nodes.back().kind = project::MaterialGraphNodeKind::parameter_scalar;
            graph.nodes.back().constants = {};
            graph.nodes.back().parameter_name = "physical";
            take(catalog.replace_material_graph(graph));
            take(catalog.bind_material_graph(1U, graph.identity));
            take(document.set_material_catalog(catalog));
            require(!prepare(document), "missing neutral parameters cannot be invented as zero");
            project::MaterialGraphEvaluationInputs inputs;
            project::MaterialGraphValue input;
            input.components[0U] = 0.0;
            inputs.parameters.emplace("physical", input);
            const auto digest = take(graph.canonical_digest());
            take(catalog.set_material_graph_inputs(1U, graph.identity, graph.revision, digest, inputs));
            take(document.set_material_catalog(catalog));
            take(prepare(document));
            inputs.parameters.at("physical").components[0U] = 1.0e-100;
            d3d12::SceneRenderMaterialGraphInputs override_inputs;
            override_inputs.emplace(graph.identity,
                d3d12::SceneRenderMaterialGraphInputBinding{graph.revision, digest, inputs});
            const auto changed = d3d12::prepare_project_scene_render(
                "graph-neutral-physical", document, {}, 96U, 96U, {},
                {{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}}, override_inputs);
            require(!changed && changed.error().code == core::ErrorCode::unsupported,
                "nonzero transient physical output cannot reuse neutral admission");
        }
    }
}

project::ProjectDocument sheen_graph_project(double weight) {
    auto document = opacity_graph_project(1.0, std::nullopt, render::AlphaMode::opaque);
    auto catalog = document.material_catalog();
    auto graph = catalog.material_graphs().at("fixture/opacity");
    graph.revision = graph.revision.next();
    project::MaterialGraphNode sheen;
    sheen.id = 5U;
    sheen.constants[0U] = weight;
    graph.nodes.push_back(sheen);
    graph.outputs.sheen = sheen.id;
    take(catalog.replace_material_graph(graph));
    take(catalog.bind_material_graph(1U, graph.identity));
    take(document.set_material_catalog(std::move(catalog)));
    return document;
}

void scene_sheen_reference_is_bounded_and_reciprocal() {
    const auto exact = take(d3d12::evaluate_scene_sheen_reference(
        0.5, 1.0, 0.0, 0.5, 0.5, 0.5));
    require(exact.base_layer_weight == 0.5 && std::abs(exact.sheen_brdf -
            std::sqrt(0.75) / (4.0 * std::acos(-1.0))) < 1.0e-12,
        "Charlie and Neubelt factors match the analytical roughness-one reference");
    for (const double roughness : {0.0, 0.5, 1.0}) {
        for (const double n_dot_l : {0.0, 1.0e-8, 0.5, 1.0}) {
            for (const double n_dot_v : {0.0, 1.0e-8, 0.5, 1.0}) {
                const auto value = take(d3d12::evaluate_scene_sheen_reference(
                    1.0, roughness, 0.0, n_dot_l, n_dot_v, 0.5));
                const auto reversed = take(d3d12::evaluate_scene_sheen_reference(
                    1.0, roughness, 0.0, n_dot_v, n_dot_l, 0.5));
                require(std::isfinite(value.sheen_brdf) && value.sheen_brdf >= 0.0 &&
                        value.sheen_brdf == reversed.sheen_brdf,
                    "sheen remains finite and reciprocal at grazing angles and zero roughness");
                if (n_dot_l == 0.0 || n_dot_v == 0.0)
                    require(value.sheen_brdf == 0.0, "sheen does not leak across a closed hemisphere");
            }
        }
    }
    const auto metal = take(d3d12::evaluate_scene_sheen_reference(
        1.0, 0.5, 1.0, 0.5, 0.5, 0.5));
    const auto absent = take(d3d12::evaluate_scene_sheen_reference(
        0.0, 0.5, 0.0, 0.5, 0.5, 0.5));
    require(metal.base_layer_weight == 1.0 && metal.sheen_brdf == 0.0 &&
            absent.base_layer_weight == 1.0 && absent.sheen_brdf == 0.0,
        "metals and zero sheen retain the complete existing base lobe");
    for (std::size_t index = 0U; index < 6U; ++index) {
        for (const double bad : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN()}) {
            std::array<double, 6U> inputs{0.5, 0.5, 0.0, 0.5, 0.5, 0.5};
            inputs[index] = bad;
            require(!d3d12::evaluate_scene_sheen_reference(inputs[0U], inputs[1U],
                    inputs[2U], inputs[3U], inputs[4U], inputs[5U]),
                "each sheen factor and cosine rejects invalid data independently");
        }
    }
}

void project_graph_sheen_binds_source_and_refuses_missing_ibl() {
    const auto prepare = [](const project::ProjectDocument& document) {
        return d3d12::prepare_project_scene_render(
            "graph-sheen", document, {}, 96U, 96U);
    };
    for (const double weight : {0.0, 0.5, 1.0}) {
        const auto document = sheen_graph_project(weight);
        const auto before = document.serialize();
        const auto prepared = take(prepare(document));
        std::size_t affected = 0U;
        for (const auto& object : prepared.objects) {
            for (const auto& binding : object.materials) {
                if (binding.current_material.id == 1U) {
                    ++affected;
                    require(binding.sheen_weight == weight,
                        "shared graph sheen reaches each matching draw binding");
                } else {
                    require(binding.sheen_weight == 0.0,
                        "sheen cannot leak onto an unrelated material");
                }
            }
        }
        require(affected == 3U && document.serialize() == before,
            "sheen preparation leaves the exact saved source unchanged");
        const auto reopened = take(project::ProjectDocument::deserialize(before));
        require(take(take(prepare(reopened)).digest()) == take(prepared.digest()),
            "saved sheen graph reproduces exact prepared request identity");
        auto environment_request = prepared;
        d3d12::SceneRenderEnvironment environment;
        environment.source_digest = hash("sheen-environment-refusal-fixture");
        environment_request.environment = environment;
        const auto admitted = environment_request.validate();
        require(weight == 0.0 ? static_cast<bool>(admitted) :
                (!admitted && admitted.error().code == core::ErrorCode::unsupported),
            "only nonzero sheen refuses the unavailable environment integration");
    }
    for (const double invalid_weight : {-0.5, 1.5}) {
        const auto result = prepare(sheen_graph_project(invalid_weight));
        require(!result && result.error().code == core::ErrorCode::validation_failed,
            "evaluated sheen must be within the shader's declared weight bounds");
    }
    auto prepared = take(prepare(sheen_graph_project(0.5)));
    const auto original_digest = take(prepared.digest());
    prepared.objects.front().materials.front().sheen_weight = 0.75;
    require(take(prepared.digest()) != original_digest,
        "scene-only sheen participates in request identity independently of generic material artifacts");
    for (const double invalid_weight : {-0.5, 1.5, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        prepared.objects.front().materials.front().sheen_weight = invalid_weight;
        require(!prepared.validate() && !prepared.digest(),
            "direct scene callers cannot bypass finite range admission for sheen");
    }
    auto stale_document = sheen_graph_project(0.5);
    auto catalog = stale_document.material_catalog();
    auto graph = catalog.material_graphs().at("fixture/opacity");
    graph.nodes.back().constants[0U] = 0.75;
    graph.revision = graph.revision.next();
    take(catalog.replace_material_graph(graph));
    take(stale_document.set_material_catalog(std::move(catalog)));
    const auto stale = prepare(stale_document);
    require(!stale && stale.error().code == core::ErrorCode::stale_data,
        "sheen graph changes require an explicit source rebind");
}

project::ProjectDocument anisotropy_graph_project(double strength) {
    auto document = opacity_graph_project(1.0, std::nullopt, render::AlphaMode::opaque);
    auto catalog = document.material_catalog();
    auto graph = catalog.material_graphs().at("fixture/opacity");
    graph.revision = graph.revision.next();
    project::MaterialGraphNode anisotropy;
    anisotropy.id = 5U;
    anisotropy.constants[0U] = strength;
    graph.nodes.push_back(anisotropy);
    graph.outputs.anisotropy = anisotropy.id;
    take(catalog.replace_material_graph(graph));
    take(catalog.bind_material_graph(1U, graph.identity));
    take(document.set_material_catalog(std::move(catalog)));
    return document;
}

void scene_anisotropy_reference_preserves_axes_and_zero_boundary() {
    const core::Vec3d view{0.0, 0.0, 1.0};
    const core::Vec3d light{0.6, 0.0, 0.8};
    const auto zero = take(d3d12::evaluate_scene_anisotropy_reference(0.5, 0.0, view, light));
    const auto near_zero = take(d3d12::evaluate_scene_anisotropy_reference(0.5, 1.0e-8, view, light));
    require(zero.mixed_kernel == zero.legacy_kernel &&
            std::abs(near_zero.mixed_kernel - zero.mixed_kernel) < 1.0e-6,
        "anisotropy preserves legacy zero and approaches it continuously");
    const auto tangent = take(d3d12::evaluate_scene_anisotropy_reference(0.5, 1.0, view, light));
    const auto bitangent = take(d3d12::evaluate_scene_anisotropy_reference(0.5, -1.0, view, light));
    const auto swapped = take(d3d12::evaluate_scene_anisotropy_reference(
        0.5, -1.0, view, {0.0, 0.6, 0.8}));
    require(tangent.mixed_kernel > bitangent.mixed_kernel &&
            std::abs(tangent.mixed_kernel - swapped.mixed_kernel) < 1.0e-12,
        "signed anisotropy swaps broad and narrow authored tangent axes");
    const auto normal = take(d3d12::evaluate_scene_anisotropy_reference(0.5, 1.0, view, view));
    require(std::abs(normal.mixed_kernel - 4.0 / std::acos(-1.0)) < 1.0e-12,
        "normal-incidence anisotropic GGX matches its analytical distribution and visibility");
    for (const double roughness : {0.0, 0.5, 1.0}) {
        for (const double strength : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            for (const core::Vec3d grazing : {core::Vec3d{1.0, 0.0, 1.0e-10}.normalized(),
                    core::Vec3d{0.3, 0.4, std::sqrt(0.75)}}) {
                const auto forward = take(d3d12::evaluate_scene_anisotropy_reference(
                    roughness, strength, grazing, light));
                const auto reverse = take(d3d12::evaluate_scene_anisotropy_reference(
                    roughness, strength, light, grazing));
                require(std::isfinite(forward.mixed_kernel) && forward.mixed_kernel >= 0.0 &&
                        std::abs(forward.mixed_kernel - reverse.mixed_kernel) < 1.0e-10,
                    "anisotropic kernel remains finite and reciprocal across roughness and grazing probes");
            }
        }
    }
    require(take(d3d12::evaluate_scene_anisotropy_reference(
                0.5, 1.0, view, {0.0, 0.0, -1.0})).mixed_kernel == 0.0,
        "anisotropic direct reflection does not leak into the opposite hemisphere");
    for (const double bad : {-1.1, 1.1, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()})
        require(!d3d12::evaluate_scene_anisotropy_reference(0.5, bad, view, light),
            "invalid signed anisotropy refuses rather than clamping");
    for (const double bad : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()})
        require(!d3d12::evaluate_scene_anisotropy_reference(bad, 0.5, view, light),
            "invalid anisotropic roughness refuses");
    for (const auto bad : {core::Vec3d{}, core::Vec3d{0.0, 0.0, 2.0},
            core::Vec3d{std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0}})
        require(!d3d12::evaluate_scene_anisotropy_reference(0.5, 0.5, bad, light) &&
                !d3d12::evaluate_scene_anisotropy_reference(0.5, 0.5, view, bad),
            "the numerical reference never invents missing or invalid directions");
}

void project_graph_anisotropy_requires_source_bound_tangents() {
    const auto prepare = [](const project::ProjectDocument& document) {
        return d3d12::prepare_project_scene_render("graph-anisotropy", document, {}, 97U, 97U);
    };
    for (const double strength : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
        const auto document = anisotropy_graph_project(strength);
        const auto before = document.serialize();
        const auto prepared = take(prepare(document));
        std::size_t affected = 0U;
        for (const auto& object : prepared.objects) {
            for (const auto& binding : object.materials) {
                if (binding.current_material.id == 1U) {
                    ++affected;
                    require(binding.anisotropy == strength && object.uv0_set == render::kUv0 &&
                            object.mesh.vertices.front().uv.has_value() &&
                            object.mesh.vertices.front().tangent.has_value(),
                        "shared graph anisotropy uses the explicit primary UV0 tangent frame");
                } else {
                    require(binding.anisotropy == 0.0, "anisotropy does not bleed to unrelated materials");
                }
            }
        }
        require(affected == 3U && document.serialize() == before,
            "anisotropic preparation preserves all authored source bytes");
        const auto reopened = take(project::ProjectDocument::deserialize(before));
        require(take(take(prepare(reopened)).digest()) == take(prepared.digest()),
            "anisotropy and tangent selection survive exact saved-project replay");
    }
    for (const double strength : {0.0, -1.0, 1.0}) {
        auto missing_uv = anisotropy_graph_project(strength);
        const auto mesh_id = missing_uv.meshes().begin()->first;
        auto mesh = missing_uv.meshes().begin()->second;
        auto payload = mesh.attributes();
        take(payload.remove_layer("uv.fixture"));
        take(mesh.set_attribute_payload(std::move(payload), {}));
        take(project::testing::access(missing_uv).replace_mesh(mesh_id, mesh));
        const auto result = prepare(missing_uv);
        require(strength == 0.0 ? static_cast<bool>(result) :
                (!result && result.error().code == core::ErrorCode::not_found),
            "only nonzero anisotropy requires authored UV0 instead of neutral upload fallback");
    }
    for (const double strength : {-1.01, 1.01}) {
        const auto result = prepare(anisotropy_graph_project(strength));
        require(!result && result.error().code == core::ErrorCode::validation_failed,
            "graph anisotropy outside [-1, 1] refuses before rendering");
    }
    const auto valid = take(prepare(anisotropy_graph_project(0.5)));
    auto changed = valid;
    changed.objects.front().materials.front().anisotropy = -0.5;
    require(take(changed.digest()) != take(valid.digest()),
        "anisotropy sign participates in exact request identity");
    for (const double bad : {-1.01, 1.01, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        changed.objects.front().materials.front().anisotropy = bad;
        require(!changed.validate() && !changed.digest(), "raw requests retain signed anisotropy admission");
    }
    auto no_tangents = valid;
    for (auto& vertex : no_tangents.objects.front().mesh.vertices) vertex.tangent.reset();
    rebind(no_tangents.objects.front());
    const auto tangent_refusal = no_tangents.validate();
    require(!tangent_refusal && tangent_refusal.error().code == core::ErrorCode::unsupported,
        "direct requests cannot replace authored anisotropic tangents with neutral upload axes");
    auto wrong_uv = valid;
    wrong_uv.objects.front().uv0_set = 2U;
    require(!wrong_uv.validate(), "non-UV0 primary streams cannot silently rotate the anisotropy convention");
    auto with_environment = valid;
    d3d12::SceneRenderEnvironment environment;
    environment.source_digest = hash("anisotropy-environment-refusal");
    with_environment.environment = environment;
    const auto environment_refusal = with_environment.validate();
    require(!environment_refusal && environment_refusal.error().code == core::ErrorCode::unsupported,
        "isotropic environment prefilters cannot silently stand in for anisotropic IBL");
    auto with_normal = valid;
    with_normal.objects.front().materials.front().current_material.definition.normal.asset = 99U;
    rebind(with_normal.objects.front());
    const auto missing_normal = with_normal.validate();
    require(!missing_normal && missing_normal.error().code == core::ErrorCode::not_found,
        "admitting anisotropic normal maps never invents missing texture sources");

    const auto normal_texture = texture(99U, {128U, 128U, 255U, 255U}, assets::TextureSemantic::normal);
    for (const auto normal_uv : {render::kUv0, render::UvSetId{2U}}) {
        for (const bool same_object : {false, true}) {
            auto document = take(project::ProjectDocument::create("Anisotropy tangent-scope fixture"));
            auto access = project::testing::access(document);
            auto mesh = authored_box();
            auto payload = mesh.attributes();
            const auto primary_uv_values = payload.find("uv.fixture")->values;
            take(payload.add_layer({{"uv.second", attributes::AttributeDomain::corner,
                attributes::AttributeType::vec2}, primary_uv_values}));
            auto uv_sets = mesh.uv_sets();
            take(uv_sets.add({2U, "Second UV", "uv.second", false, true, false, 1U}));
            take(mesh.set_attribute_payload(payload, uv_sets));
            const auto separate_mesh = mesh;
            if (same_object) {
                auto regions = *payload.find("cartographer.material_slot");
                regions.values.back() = std::uint32_t{2U};
                take(payload.remove_layer("cartographer.material_slot"));
                take(payload.add_layer(std::move(regions)));
                take(mesh.set_attribute_payload(std::move(payload), uv_sets));
            }
            const auto first_mesh = take(access.add_mesh(mesh));
            const auto first_object = take(access.create_object("Anisotropic object"));
            take(access.attach_mesh(first_object, first_mesh));
            auto second_mesh = first_mesh;
            if (!same_object) {
                second_mesh = take(access.add_mesh(separate_mesh));
                const auto second_object = take(access.create_object("Unrelated normal-mapped object"));
                take(access.attach_mesh(second_object, second_mesh));
            }
            project::MaterialCatalog catalog;
            render::MaterialAsset anisotropic{1U, "Anisotropic fixture", {}, core::Revision{1U}};
            render::MaterialAsset normal_mapped{2U, "Normal-mapped fixture", {}, core::Revision{1U}};
            normal_mapped.definition.normal.asset = normal_texture.asset;
            normal_mapped.definition.normal.uv_set = normal_uv;
            normal_mapped.definition.normal.transform.rotation = 0.25;
            normal_mapped.definition.normal.transform.scale = {-1.0, 1.0};
            project::TextureSourceRecord source;
            source.id = normal_texture.asset;
            source.name = "Normal source fixture";
            source.relative_locator = "fixtures/normal.tga";
            source.content_digest = normal_texture.current_source_digest;
            source.format = assets::TextureSourceFormat::tga;
            source.width = 1U;
            source.height = 1U;
            source.provenance = "Synthetic tangent-scope regression source";
            take(catalog.insert_texture(source));
            take(catalog.insert_material(anisotropic));
            take(catalog.insert_material(normal_mapped));
            const auto graph = anisotropy_graph_project(1.0).material_catalog()
                .material_graphs().at("fixture/opacity");
            take(catalog.insert_material_graph(graph));
            take(catalog.bind_material_graph(1U, graph.identity));
            take(catalog.insert_assignment({{first_mesh, 1U}, 1U, {}}));
            take(catalog.insert_assignment({{second_mesh, same_object ? 2U : 1U}, 2U, {}}));
            take(document.set_material_catalog(std::move(catalog)));
            const auto result = d3d12::prepare_project_scene_render(
                "tangent-scope", document, {}, 97U, 97U, {normal_texture});
            require(same_object && normal_uv != render::kUv0 ?
                    (!result && result.error().code == core::ErrorCode::unsupported) :
                    static_cast<bool>(result),
                "UV1 tangent conflicts are object-scoped; rotated/reflected UV0 normal maps on other materials remain valid");
        }
    }
}

void scene_anisotropy_frame_transport_preserves_orientation() {
    const core::Vec3d geometric{0.0, 0.0, 1.0};
    const core::Vec3d tangent{1.0, 0.0, 0.0};
    const auto close = [](core::Vec3d a, core::Vec3d b) {
        return (a - b).length() < 1.0e-10;
    };
    for (const double handedness : {-1.0, 1.0}) {
        for (const auto normal : {geometric, tangent, core::Vec3d{0.0, 1.0, 0.0},
                core::Vec3d{0.0, 0.0, -1.0}, core::Vec3d{0.3, 0.4, std::sqrt(0.75)},
                core::Vec3d{1.0, 0.0, 1.0e-10}.normalized(),
                core::Vec3d{1.0e-8, 0.0, -1.0}.normalized(),
                core::Vec3d{2.0e-6, 3.0e-6, -1.0}.normalized()}) {
            const auto frame = take(d3d12::transport_scene_anisotropy_frame(
                geometric, tangent, handedness, normal));
            require(frame.tangent.finite() && frame.bitangent.finite() &&
                    std::abs(frame.tangent.length_squared() - 1.0) < 1.0e-10 &&
                    std::abs(frame.bitangent.length_squared() - 1.0) < 1.0e-10 &&
                    std::abs(core::dot(frame.tangent, normal)) < 1.0e-10 &&
                    std::abs(core::dot(frame.bitangent, normal)) < 1.0e-10 &&
                    close(core::cross(frame.tangent, frame.bitangent), normal * handedness),
                "transport retains a finite orthonormal frame and authored handedness at tilted and antiparallel normals");
        }
    }
    const auto aligned = take(d3d12::transport_scene_anisotropy_frame(geometric, tangent, 1.0, geometric));
    const auto quarter_turn = take(d3d12::transport_scene_anisotropy_frame(geometric, tangent, 1.0, tangent));
    const auto antipodal = take(d3d12::transport_scene_anisotropy_frame(
        geometric, tangent, 1.0, geometric * -1.0));
    require(close(aligned.tangent, tangent) && close(quarter_turn.tangent, geometric * -1.0) &&
            close(quarter_turn.bitangent, {0.0, 1.0, 0.0}) && close(antipodal.tangent, tangent) &&
            close(antipodal.bitangent, {0.0, -1.0, 0.0}),
        "closed-form quarter turns and deterministic antipodal orientation match the transport contract");
    const auto tilted = core::Vec3d{0.3, 0.4, std::sqrt(0.75)};
    const auto frame = take(d3d12::transport_scene_anisotropy_frame(geometric, tangent, 1.0, tilted));
    const auto restored = take(d3d12::transport_scene_anisotropy_frame(tilted, frame.tangent, 1.0, geometric));
    const auto closed_form = tangent - (geometric + tilted) *
        (core::dot(tangent, tilted) / (1.0 + core::dot(geometric, tilted)));
    require(close(restored.tangent, tangent) && close(frame.tangent, closed_form),
        "shortest transport is reversible and matches the independent positive-hemisphere formula");
    for (const auto bad : {core::Vec3d{}, core::Vec3d{2.0, 0.0, 0.0},
            core::Vec3d{std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0}}) {
        require(!d3d12::transport_scene_anisotropy_frame(bad, tangent, 1.0, geometric) &&
                !d3d12::transport_scene_anisotropy_frame(geometric, bad, 1.0, geometric) &&
                !d3d12::transport_scene_anisotropy_frame(geometric, tangent, 1.0, bad),
            "frame transport rejects malformed unit-vector inputs");
    }
    require(!d3d12::transport_scene_anisotropy_frame(geometric, geometric, 1.0, geometric) &&
            !d3d12::transport_scene_anisotropy_frame(geometric, tangent, 0.0, geometric) &&
            !d3d12::transport_scene_anisotropy_frame(geometric, tangent,
                std::numeric_limits<double>::quiet_NaN(), geometric),
        "frame transport rejects nonorthogonal axes and invalid handedness");
}

void project_graph_anisotropic_normal_maps_preserve_source_boundaries() {
    const auto normal_texture = texture(99U, {180U, 80U, 240U, 255U}, assets::TextureSemantic::normal);
    const auto make_document = [&](const render::TextureRef& normal, double strength = 0.7) {
        auto document = anisotropy_graph_project(strength);
        auto catalog = document.material_catalog();
        project::TextureSourceRecord source;
        source.id = normal_texture.asset;
        source.name = "Anisotropic normal source";
        source.relative_locator = "fixtures/anisotropic-normal.tga";
        source.content_digest = normal_texture.current_source_digest;
        source.format = assets::TextureSourceFormat::tga;
        source.width = 1U;
        source.height = 1U;
        source.provenance = "Locally authored synthetic regression texture";
        take(catalog.insert_texture(source));
        auto material = catalog.materials().at(1U);
        material.definition.normal = normal;
        material.revision = material.revision.next();
        take(catalog.replace_material(material));
        take(document.set_material_catalog(std::move(catalog)));
        return document;
    };
    const auto prepare = [&](const project::ProjectDocument& document) {
        return d3d12::prepare_project_scene_render(
            "graph-anisotropic-normal", document, {}, 97U, 97U, {normal_texture});
    };
    render::TextureRef normal;
    normal.asset = normal_texture.asset;
    normal.transform.scale = {2.0, 0.75};
    normal.transform.offset = {0.25, -0.5};
    normal.transform.pivot = {0.2, 0.9};
    for (const auto convention : {render::NormalConvention::directx_y_minus,
            render::NormalConvention::opengl_y_plus}) {
        normal.normal_convention = convention;
        const auto document = make_document(normal);
        const auto before = document.serialize();
        const auto prepared = take(prepare(document));
        std::size_t affected = 0U;
        for (const auto& object : prepared.objects) {
            for (const auto& binding : object.materials) {
                if (binding.current_material.id == 1U) {
                    ++affected;
                    require(binding.current_material.definition.normal == normal && binding.anisotropy == 0.7 &&
                            object.uv0_set == render::kUv0 && object.mesh.vertices.front().tangent.has_value(),
                        "saved graph instances retain exact normal binding, convention and explicit tangent source");
                }
            }
        }
        const auto reopened = take(project::ProjectDocument::deserialize(before));
        require(affected == 3U && document.serialize() == before &&
                take(take(prepare(reopened)).digest()) == take(prepared.digest()),
            "normal-mapped anisotropy survives save/reopen without authoring or binding mutation");
        auto stale_texture = prepared;
        stale_texture.textures.front().current_source_digest = hash("different-normal-source");
        require(!stale_texture.validate() && !stale_texture.digest(),
            "anisotropic normal-map admission still refuses stale texture bytes");
        auto stale_material = prepared;
        stale_material.objects.front().materials.front().current_material.definition.normal.normal_convention =
            convention == render::NormalConvention::directx_y_minus
                ? render::NormalConvention::opengl_y_plus : render::NormalConvention::directx_y_minus;
        require(!stale_material.validate() && !stale_material.digest(),
            "normal convention changes require source-bound material recompilation");
    }
    for (const auto rotation : {0.25, -0.25, 2.0 * std::acos(-1.0)}) {
        auto rotated = normal;
        rotated.transform.rotation = rotation;
        const auto refused = prepare(make_document(rotated));
        require(!refused && refused.error().code == core::ErrorCode::unsupported &&
                static_cast<bool>(prepare(make_document(rotated, 0.0))),
            "nonzero authored rotations refuse only for anisotropy; no approximate identity inference");
    }
    for (const auto scale : {core::Vec2d{-1.0, 1.0}, core::Vec2d{1.0, -1.0}, core::Vec2d{-1.0, -1.0}}) {
        auto reflected = normal;
        reflected.transform.scale = scale;
        const auto refused = prepare(make_document(reflected));
        require(!refused && refused.error().code == core::ErrorCode::unsupported &&
                static_cast<bool>(prepare(make_document(reflected, 0.0))),
            "reflected normal-map transforms retain their legacy path but refuse anisotropic axis ambiguity");
    }
    auto wrong_uv = normal;
    wrong_uv.uv_set = 2U;
    require(!prepare(make_document(wrong_uv)), "normal maps cannot select an unauthored anisotropic tangent stream");
    auto raw = take(prepare(make_document(normal)));
    raw.objects.front().materials.front().current_material.definition.normal.transform.rotation = 0.1;
    rebind(raw.objects.front());
    const auto raw_refusal = raw.validate();
    require(!raw_refusal && raw_refusal.error().code == core::ErrorCode::unsupported,
        "direct scene callers cannot bypass anisotropic normal-map orientation admission");
}

void project_scene_texture_loader_binds_verified_sources_and_fails_closed() {
    TempDirectory temporary;
    const auto project_path = temporary.path() / "scene.carto";
    const auto blob_root = temporary.path() / ".carto-assets" / "source-blobs";
    const auto encoded = one_pixel_tga();
    const auto admission = take(assets::inspect_texture_source(encoded, "image/x-tga"));
    assets::BlobStore store(blob_root);
    const auto blob = take(store.put(encoded, "image/x-tga"));
    project::TextureSourceRecord source;
    source.id = 501U;
    source.name = "Authored fixture texture";
    source.relative_locator = store.path_for(blob.digest)
        .lexically_relative(temporary.path()).generic_string();
    source.content_digest = admission.source.digest;
    source.format = admission.format;
    source.width = admission.width;
    source.height = admission.height;
    source.provenance = "verified native scene texture fixture";

    auto document = room_project();
    auto catalog = document.material_catalog();
    require(static_cast<bool>(catalog.insert_texture(source)),
        "fixture source record is admitted to the material catalog");
    auto material = catalog.materials().at(1U);
    material.definition.base_color.asset = source.id;
    material.revision = core::Revision{2U};
    require(static_cast<bool>(catalog.replace_material(material)) &&
        static_cast<bool>(document.set_material_catalog(std::move(catalog))),
        "fixture binds an authored base-color texture");

    auto loaded = take(d3d12::load_project_scene_textures(project_path, document));
    require(loaded.size() == 1U && loaded.front().asset == source.id &&
        loaded.front().current_source_digest == source.content_digest &&
        loaded.front().mips.semantic == assets::TextureSemantic::base_color &&
        loaded.front().mips.color_space == assets::ColorSpace::srgb &&
        loaded.front().mips.levels.size() == 1U &&
        loaded.front().mips.levels.front().pixels ==
            std::vector<std::uint8_t>({0xe0U, 0x40U, 0x20U, 0xffU}),
        "loader verifies and decodes the exact project source into semantic mips");
    const auto request = take(d3d12::prepare_project_scene_render(
        "fixture-room", document, {}, 64U, 64U, loaded));
    require(request.textures.size() == 1U,
        "verified project pixels reach the native render request");
    auto mapped_channel_document = document;
    auto mapped_channel_catalog = mapped_channel_document.material_catalog();
    auto mapped_channel_material = mapped_channel_catalog.materials().at(1U);
    mapped_channel_material.definition.base_color.channels.rgba = {
        render::TextureChannel::green, render::TextureChannel::blue,
        render::TextureChannel::one_minus_red,
        render::TextureChannel::constant_one};
    mapped_channel_material.revision = core::Revision{3U};
    require(static_cast<bool>(mapped_channel_catalog.replace_material(
            mapped_channel_material)) &&
        static_cast<bool>(mapped_channel_document.set_material_catalog(
            std::move(mapped_channel_catalog))),
        "fixture authors a current per-channel base-color mapping");
    const auto mapped_channel_request = d3d12::prepare_project_scene_render(
        "fixture-room", mapped_channel_document, {}, 64U, 64U, loaded);
    require(mapped_channel_request &&
        take(mapped_channel_request.value().digest()) != take(request.digest()) &&
        std::any_of(mapped_channel_request.value().objects.begin(),
            mapped_channel_request.value().objects.end(),
            [](const d3d12::SceneRenderObject& object) {
                return std::any_of(object.materials.begin(), object.materials.end(),
                    [](const d3d12::SceneRenderMaterialBinding& binding) {
                        return binding.current_material.definition.base_color.channels.rgba[0U] ==
                            render::TextureChannel::green;
                    });
            }),
        "saved-scene preparation preserves and identities supported channel mappings");
    auto masked_channel_document = document;
    auto masked_channel_catalog = masked_channel_document.material_catalog();
    auto masked_channel_material = masked_channel_catalog.materials().at(1U);
    masked_channel_material.definition.alpha_mode = render::AlphaMode::masked;
    masked_channel_material.definition.base_color.channels.rgba[3U] =
        render::TextureChannel::one_minus_alpha;
    masked_channel_material.revision = core::Revision{3U};
    require(static_cast<bool>(masked_channel_catalog.replace_material(
            masked_channel_material)) &&
        static_cast<bool>(masked_channel_document.set_material_catalog(
            std::move(masked_channel_catalog))),
        "fixture authors a masked material with a non-alpha alpha selector");
    const auto unsupported_masked_channel = d3d12::prepare_project_scene_render(
        "fixture-room", masked_channel_document, {}, 64U, 64U, loaded);
    require(!unsupported_masked_channel &&
        unsupported_masked_channel.error().code == core::ErrorCode::unsupported &&
        unsupported_masked_channel.error().message.find("mip coverage") !=
            std::string::npos,
        "masked non-alpha selectors refuse until their derived mips preserve selected-channel coverage");
    auto constant_masked_document = document;
    auto constant_masked_catalog = constant_masked_document.material_catalog();
    auto constant_masked_material = constant_masked_catalog.materials().at(1U);
    constant_masked_material.definition.alpha_mode = render::AlphaMode::masked;
    constant_masked_material.definition.base_color.channels.rgba[3U] =
        render::TextureChannel::constant_one;
    constant_masked_material.revision = core::Revision{3U};
    require(static_cast<bool>(constant_masked_catalog.replace_material(
            constant_masked_material)) &&
        static_cast<bool>(constant_masked_document.set_material_catalog(
            std::move(constant_masked_catalog))),
        "fixture authors a fully-covered constant masked alpha selector");
    require(static_cast<bool>(d3d12::prepare_project_scene_render(
            "fixture-room", constant_masked_document, {}, 64U, 64U, loaded)),
        "constant mapped alpha does not require source-channel mip coverage preservation");
    const auto textured_catalog = document.material_catalog();
    const auto texture_assignment = std::find_if(
        textured_catalog.assignments().begin(), textured_catalog.assignments().end(),
        [](const auto& entry) { return entry.second.material == 1U; });
    require(texture_assignment != textured_catalog.assignments().end(),
        "fixture resolves a mesh assigned to the textured material");
    auto multi_uv_document = document;
    using MeshAssetId = decltype(texture_assignment->first.mesh_asset);
    std::vector<MeshAssetId> multi_uv_mesh_assets;
    for (const auto& [assignment_key, assignment] : textured_catalog.assignments()) {
        if (assignment.material == 1U && std::find(multi_uv_mesh_assets.begin(),
                multi_uv_mesh_assets.end(), assignment_key.mesh_asset) ==
                multi_uv_mesh_assets.end()) {
            multi_uv_mesh_assets.push_back(assignment_key.mesh_asset);
        }
    }
    for (const auto mesh_asset : multi_uv_mesh_assets) {
        auto multi_uv_mesh = multi_uv_document.meshes().at(mesh_asset);
        auto multi_uv_payload = multi_uv_mesh.attributes();
        const auto* primary_uv_layer = multi_uv_payload.find("uv.fixture");
        require(primary_uv_layer != nullptr,
            "fixture exposes its source-bound primary UV layer");
        const auto primary_uv_values = primary_uv_layer->values;
        const auto make_uv_values = [&](double scale, double offset) {
            std::vector<attributes::AttributeValue> values;
            values.reserve(primary_uv_values.size());
            for (const auto& value : primary_uv_values) {
                const auto* uv = std::get_if<core::Vec2d>(&value);
                require(uv != nullptr, "fixture primary UV values remain vec2 data");
                values.emplace_back(core::Vec2d{uv->x * scale + offset,
                    uv->y * scale - offset});
            }
            return values;
        };
        require(static_cast<bool>(multi_uv_payload.add_layer({
                {"uv.material1", attributes::AttributeDomain::corner,
                    attributes::AttributeType::vec2}, make_uv_values(0.5, 0.125)})) &&
            static_cast<bool>(multi_uv_payload.add_layer({
                {"uv.material2", attributes::AttributeDomain::corner,
                    attributes::AttributeType::vec2}, make_uv_values(1.5, -0.25)})) &&
            static_cast<bool>(multi_uv_payload.add_layer({
                {"uv.material3", attributes::AttributeDomain::corner,
                    attributes::AttributeType::vec2}, make_uv_values(0.75, 0.375)})),
            "fixture authors three additional corner-domain UV streams");
        auto multi_uv_sets = multi_uv_mesh.uv_sets();
        require(static_cast<bool>(multi_uv_sets.add(
                {2U, "Material UV 2", "uv.material1", false, true, false, 1U})) &&
            static_cast<bool>(multi_uv_sets.add(
                {3U, "Material UV 3", "uv.material2", false, true, false, 1U})) &&
            static_cast<bool>(multi_uv_sets.add(
                {4U, "Material UV 4", "uv.material3", false, true, false, 1U})) &&
            static_cast<bool>(multi_uv_mesh.set_attribute_payload(
                std::move(multi_uv_payload), std::move(multi_uv_sets))) &&
            static_cast<bool>(project::testing::access(multi_uv_document).replace_mesh(
                mesh_asset, std::move(multi_uv_mesh))),
            "fixture binds all UV streams to the same mesh revision");
    }
    auto multi_uv_catalog = multi_uv_document.material_catalog();
    auto metallic_source = source;
    metallic_source.id = 502U;
    metallic_source.name = "Authored metallic-roughness fixture texture";
    auto emissive_source = source;
    emissive_source.id = 503U;
    emissive_source.name = "Authored emissive fixture texture";
    require(static_cast<bool>(multi_uv_catalog.insert_texture(metallic_source)) &&
        static_cast<bool>(multi_uv_catalog.insert_texture(emissive_source)),
        "fixture records distinct current texture assets for independent slots");
    auto multi_uv_material = multi_uv_catalog.materials().at(1U);
    multi_uv_material.definition.base_color.uv_set = 2U;
    multi_uv_material.definition.metallic_roughness.asset = 502U;
    multi_uv_material.definition.metallic_roughness.uv_set = 3U;
    multi_uv_material.definition.emissive.asset = 503U;
    multi_uv_material.definition.emissive.uv_set = 4U;
    multi_uv_material.revision = core::Revision{3U};
    require(static_cast<bool>(multi_uv_catalog.replace_material(multi_uv_material)) &&
        static_cast<bool>(multi_uv_document.set_material_catalog(
            std::move(multi_uv_catalog))),
        "fixture binds three material slots to distinct authored UV sets");
    const auto multi_uv_textures = take(d3d12::load_project_scene_textures(
        project_path, multi_uv_document));
    const auto multi_uv_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", multi_uv_document, {}, 64U, 64U, multi_uv_textures));
    const auto target_scene_object = std::find_if(
        multi_uv_request.objects.begin(), multi_uv_request.objects.end(),
        [&](const auto& candidate) {
            return std::any_of(candidate.materials.begin(), candidate.materials.end(),
                [](const auto& material_binding) {
                    return material_binding.current_material.definition.base_color.asset ==
                        501U;
                });
        });
    require(target_scene_object != multi_uv_request.objects.end() &&
        target_scene_object->uv0_set == 2U && target_scene_object->uv1_set == 3U &&
        target_scene_object->mesh.additional_uv_set_ids ==
            std::vector<attributes::UvSetId>{4U},
        "project preparation binds three authored slot UV sets through the extended artifact");
    auto missing_uv_textured = document;
    auto missing_uv_mesh = missing_uv_textured.meshes().at(
        texture_assignment->first.mesh_asset);
    auto missing_uv_payload = missing_uv_mesh.attributes();
    require(static_cast<bool>(missing_uv_payload.remove_layer("uv.fixture")) &&
        static_cast<bool>(missing_uv_mesh.set_attribute_payload(
            std::move(missing_uv_payload), {})) &&
        static_cast<bool>(project::testing::access(missing_uv_textured).replace_mesh(
            texture_assignment->first.mesh_asset, std::move(missing_uv_mesh))),
        "fixture removes UV0 from a currently textured material source");
    const auto missing_uv_textured_result = d3d12::prepare_project_scene_render(
        "fixture-room", missing_uv_textured, {}, 64U, 64U, loaded);
    require(!missing_uv_textured_result &&
        missing_uv_textured_result.error().code == core::ErrorCode::not_found &&
        missing_uv_textured_result.error().message.find("UV set") != std::string::npos,
        "texture sampling refuses a mesh without its referenced authored UV set");

    const auto rejects_ignored_binding_setting = [&](auto edit,
            std::string_view diagnostic, std::string_view message) {
        auto candidate = document;
        auto candidate_catalog = candidate.material_catalog();
        auto candidate_material = candidate_catalog.materials().at(1U);
        edit(candidate_material.definition.base_color);
        candidate_material.revision = core::Revision{3U};
        require(static_cast<bool>(candidate_catalog.replace_material(candidate_material)) &&
            static_cast<bool>(candidate.set_material_catalog(std::move(candidate_catalog))),
            "fixture authors a current material texture setting");
        const auto rejected = d3d12::prepare_project_scene_render(
            "fixture-room", candidate, {}, 64U, 64U, loaded);
        require(!rejected && rejected.error().code == core::ErrorCode::unsupported &&
            rejected.error().message.find(diagnostic) != std::string::npos,
            message);
    };
    auto missing_authored_uv = document;
    auto missing_uv_catalog = missing_authored_uv.material_catalog();
    auto missing_uv_material = missing_uv_catalog.materials().at(1U);
    missing_uv_material.definition.base_color.uv_set = 2U;
    missing_uv_material.revision = core::Revision{3U};
    require(static_cast<bool>(missing_uv_catalog.replace_material(missing_uv_material)) &&
        static_cast<bool>(missing_authored_uv.set_material_catalog(
            std::move(missing_uv_catalog))),
        "fixture requests a UV identity absent from the source mesh");
    const auto missing_authored_uv_result = d3d12::prepare_project_scene_render(
        "fixture-room", missing_authored_uv, {}, 64U, 64U, loaded);
    require(!missing_authored_uv_result &&
        missing_authored_uv_result.error().code == core::ErrorCode::not_found &&
        missing_authored_uv_result.error().message.find("UV set") != std::string::npos,
        "project adapter refuses a missing authored UV set without UV0 fallback");
    rejects_ignored_binding_setting([](render::TextureRef& binding) {
        binding.interpretation = render::TextureInterpretation::linear;
    }, "color interpretation",
        "a base-color texture cannot silently use a conflicting linear interpretation");

    auto sampler_document = document;
    auto sampler_catalog = sampler_document.material_catalog();
    const project::SamplerIntent repeat_mixed{
        .id = 7U,
        .name = "Authored repeat mixed filter",
        .address_u = project::SamplerAddressMode::repeat,
        .address_v = project::SamplerAddressMode::repeat,
        .address_w = project::SamplerAddressMode::clamp_to_edge,
        .minification = project::SamplerFilter::nearest,
        .magnification = project::SamplerFilter::linear,
        .mip = project::SamplerFilter::nearest,
        .revision = core::Revision{1U},
    };
    require(static_cast<bool>(sampler_catalog.insert_sampler(repeat_mixed)),
        "fixture admits an authored repeat sampler with distinct filter modes");
    auto sampler_material = sampler_catalog.materials().at(1U);
    sampler_material.definition.base_color.sampler = repeat_mixed.id;
    sampler_material.revision = core::Revision{3U};
    const auto sampler_source_digest = take(
        production::material_source_digest(sampler_material));
    require(static_cast<bool>(sampler_catalog.replace_material(sampler_material)) &&
        static_cast<bool>(sampler_document.set_material_catalog(
            std::move(sampler_catalog))),
        "fixture binds the authored sampler to the current base-color material");
    const auto sampler_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", sampler_document, {}, 64U, 64U, loaded));
    const auto& native_sampler = sampler_request.samplers.at(repeat_mixed.id);
    require(sampler_request.samplers.size() == 1U &&
        native_sampler.u == gpu::AddressMode::repeat &&
        native_sampler.v == gpu::AddressMode::repeat &&
        native_sampler.w == gpu::AddressMode::clamp_edge &&
        native_sampler.min_filter == gpu::Filter::nearest &&
        native_sampler.mag_filter == gpu::Filter::linear &&
        native_sampler.mip_filter == gpu::MipFilter::nearest &&
        take(sampler_request.digest()) != take(request.digest()) &&
        std::any_of(sampler_request.objects.begin(), sampler_request.objects.end(),
            [&](const d3d12::SceneRenderObject& rendered) {
                return std::any_of(rendered.materials.begin(), rendered.materials.end(),
                    [&](const d3d12::SceneRenderMaterialBinding& binding) {
                        return binding.material.source_digest == sampler_source_digest;
                    });
            }),
        "project sampler fields reach D3D12 while material source identity stays exact");

    auto transformed_document = document;
    auto transformed_catalog = transformed_document.material_catalog();
    auto transformed_material = transformed_catalog.materials().at(1U);
    transformed_material.definition.base_color.transform.scale = {0.75, 0.5};
    transformed_material.definition.base_color.transform.offset = {0.1, 0.2};
    transformed_material.definition.base_color.transform.rotation = 0.25;
    transformed_material.definition.base_color.transform.pivot = {0.25, 0.75};
    transformed_material.revision = core::Revision{3U};
    require(static_cast<bool>(transformed_catalog.replace_material(transformed_material)) &&
        static_cast<bool>(transformed_document.set_material_catalog(std::move(transformed_catalog))),
        "fixture authors a rotated pivoted affine texture transform");
    const auto scene_material_artifact = take(d3d12::compile_scene_render_material_artifact(
        "fixture/material/scene-uv", transformed_material));
    require(scene_material_artifact.source_digest ==
            take(production::material_source_digest(transformed_material)) &&
        static_cast<bool>(scene_material_artifact.validate()) &&
        !production::compile_material_artifact("fixture/material/generic-uv", transformed_material),
        "D3D12 scene artifact preserves transformed source identity while generic GPU compilation stays strict");
    const auto transformed_request = d3d12::prepare_project_scene_render(
        "fixture-room", transformed_document, {}, 64U, 64U, loaded);
    require(transformed_request &&
        take(transformed_request.value().digest()) != take(request.digest()),
        "rotated pivoted texture affine reaches and changes the prepared render identity");

    auto normal_document = document;
    auto normal_catalog = normal_document.material_catalog();
    auto normal_material = normal_catalog.materials().at(1U);
    normal_material.definition.base_color = {};
    normal_material.definition.normal.asset = source.id;
    normal_material.definition.normal.normal_convention = render::NormalConvention::opengl_y_plus;
    normal_material.revision = core::Revision{3U};
    require(static_cast<bool>(normal_catalog.replace_material(normal_material)) &&
        static_cast<bool>(normal_document.set_material_catalog(std::move(normal_catalog))),
        "fixture authors an OpenGL-convention normal source");
    const auto normal_textures = take(d3d12::load_project_scene_textures(
        project_path, normal_document));
    const auto open_gl_normal_artifact = take(d3d12::compile_scene_render_material_artifact(
        "fixture/material/opengl-normal", normal_material));
    const auto open_gl_normal_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", normal_document, {}, 64U, 64U, normal_textures));
    auto directx_normal_document = document;
    auto directx_normal_catalog = directx_normal_document.material_catalog();
    auto directx_normal_material = directx_normal_catalog.materials().at(1U);
    directx_normal_material.definition.base_color = {};
    directx_normal_material.definition.normal.asset = source.id;
    directx_normal_material.definition.normal.normal_convention =
        render::NormalConvention::directx_y_minus;
    directx_normal_material.revision = core::Revision{3U};
    require(static_cast<bool>(directx_normal_catalog.replace_material(
                directx_normal_material)) &&
            static_cast<bool>(directx_normal_document.set_material_catalog(
                std::move(directx_normal_catalog))),
        "fixture creates an equivalent DirectX-convention normal source");
    const auto directx_normal_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", directx_normal_document, {}, 64U, 64U, normal_textures));
    require(open_gl_normal_artifact.source_digest ==
            take(production::material_source_digest(normal_material)) &&
        static_cast<bool>(open_gl_normal_artifact.validate()) &&
        !production::compile_material_artifact(
            "fixture/material/generic-opengl-normal", normal_material) &&
        take(open_gl_normal_request.digest()) != take(directx_normal_request.digest()),
        "scene adapter preserves OpenGL source identity while generic compilation stays strict");

    auto conflicted_document = document;
    auto conflicted_catalog = conflicted_document.material_catalog();
    auto conflicted_material = conflicted_catalog.materials().at(1U);
    conflicted_material.definition.normal.asset = source.id;
    conflicted_material.revision = core::Revision{3U};
    require(static_cast<bool>(conflicted_catalog.replace_material(conflicted_material)) &&
        static_cast<bool>(conflicted_document.set_material_catalog(std::move(conflicted_catalog))),
        "fixture reuses one source in a second semantic slot");
    const auto conflict = d3d12::load_project_scene_textures(project_path, conflicted_document);
    require(!conflict && conflict.error().code == core::ErrorCode::unsupported,
        "one pixel interpretation is not silently reused across incompatible slots");

    TempDirectory missing_store;
    const auto missing = d3d12::load_project_scene_textures(
        missing_store.path() / "scene.carto", document);
    require(!missing && missing.error().code == core::ErrorCode::not_found,
        "a missing project-local blob cannot be substituted with an external or default texture");

    const auto source_path = store.path_for(blob.digest);
    {
        std::ofstream corrupted(source_path, std::ios::binary | std::ios::trunc);
        corrupted << "stale source bytes";
    }
    const auto stale = d3d12::load_project_scene_textures(project_path, document);
    require(!stale,
        "modified project blob fails content-digest verification instead of becoming fallback pixels");

    TempDirectory profiled_texture_project;
    const auto profiled_encoded = equirectangular_png_with_srgb_metadata();
    const auto profiled_admission = take(assets::inspect_texture_source(
        profiled_encoded, "image/png"));
    assets::BlobStore profiled_store(
        profiled_texture_project.path() / ".carto-assets" / "source-blobs");
    const auto profiled_blob = take(profiled_store.put(profiled_encoded, "image/png"));
    project::TextureSourceRecord profiled_source;
    profiled_source.id = 502U;
    profiled_source.name = "Profile-bearing fixture texture";
    profiled_source.relative_locator = profiled_store.path_for(profiled_blob.digest)
        .lexically_relative(profiled_texture_project.path()).generic_string();
    profiled_source.content_digest = profiled_admission.source.digest;
    profiled_source.format = profiled_admission.format;
    profiled_source.width = profiled_admission.width;
    profiled_source.height = profiled_admission.height;
    profiled_source.provenance = "verified color-metadata refusal fixture";
    auto profiled_document = room_project();
    auto profiled_catalog = profiled_document.material_catalog();
    require(static_cast<bool>(profiled_catalog.insert_texture(profiled_source)),
        "fixture admits a bound profile-bearing texture source");
    auto profiled_material = profiled_catalog.materials().at(1U);
    profiled_material.definition.base_color.asset = profiled_source.id;
    profiled_material.revision = core::Revision{2U};
    require(static_cast<bool>(profiled_catalog.replace_material(profiled_material)) &&
        static_cast<bool>(profiled_document.set_material_catalog(std::move(profiled_catalog))),
        "fixture binds a profile-bearing source to a saved material");
    const auto profile_refused = d3d12::load_project_scene_textures(
        profiled_texture_project.path() / "scene.carto", profiled_document);
    require(!profile_refused && profile_refused.error().code == core::ErrorCode::unsupported,
        "saved material texture loading refuses embedded color metadata before pixel use");
}

void masked_scene_textures_preserve_authored_alpha_coverage() {
    TempDirectory temporary;
    const auto project_path = temporary.path() / "masked-scene.carto";
    const auto encoded = sparse_alpha_tga();
    const auto admission = take(assets::inspect_texture_source(encoded, "image/x-tga"));
    assets::BlobStore store(temporary.path() / ".carto-assets" / "source-blobs");
    const auto blob = take(store.put(encoded, "image/x-tga"));

    project::TextureSourceRecord source;
    source.id = 777U;
    source.name = "Sparse authored cutout";
    source.relative_locator = store.path_for(blob.digest)
        .lexically_relative(temporary.path()).generic_string();
    source.content_digest = admission.source.digest;
    source.format = admission.format;
    source.width = admission.width;
    source.height = admission.height;
    source.provenance = "verified alpha-coverage fixture";

    auto document = room_project();
    auto catalog = document.material_catalog();
    require(static_cast<bool>(catalog.insert_texture(source)),
        "fixture admits a verified base-color source");
    auto masked = catalog.materials().at(1U);
    masked.definition.base_color.asset = source.id;
    masked.definition.base_color_factor.w = 0.8;
    masked.definition.alpha_mode = render::AlphaMode::masked;
    masked.definition.alpha_cutoff = 0.5;
    masked.revision = core::Revision{2U};
    require(static_cast<bool>(catalog.replace_material(masked)) &&
        static_cast<bool>(document.set_material_catalog(catalog)),
        "fixture binds the sparse texture to a masked material");

    const auto loaded = take(d3d12::load_project_scene_textures(
        project_path, document));
    require(loaded.size() == 1U && loaded.front().mips.levels.size() == 3U,
        "masked source builds a complete bounded mip chain");
    const auto count_covered = [](const assets::Rgba8MipLevel& level,
            std::uint8_t cutoff) {
        std::size_t covered = 0U;
        for (std::size_t offset = 3U; offset < level.pixels.size(); offset += 4U) {
            if (level.pixels[offset] >= cutoff) ++covered;
        }
        return covered;
    };
    require(count_covered(loaded.front().mips.levels[1U], 160U) == 1U &&
        count_covered(loaded.front().mips.levels[2U], 160U) == 1U,
        "every masked mip level preserves at least one source-covered texel");

    auto opaque_document = document;
    auto opaque_catalog = opaque_document.material_catalog();
    auto opaque = opaque_catalog.materials().at(1U);
    opaque.definition.alpha_mode = render::AlphaMode::opaque;
    opaque.revision = core::Revision{3U};
    require(static_cast<bool>(opaque_catalog.replace_material(opaque)) &&
        static_cast<bool>(opaque_document.set_material_catalog(std::move(opaque_catalog))),
        "fixture returns the same source to opaque material use");
    const auto opaque_mips = take(d3d12::load_project_scene_textures(
        project_path, opaque_document));
    require(count_covered(opaque_mips.front().mips.levels[1U], 128U) == 0U,
        "opaque materials retain ordinary averaged alpha mips");

    auto no_coverage_document = document;
    auto no_coverage_catalog = no_coverage_document.material_catalog();
    auto no_coverage = no_coverage_catalog.materials().at(2U);
    no_coverage.definition.base_color.asset = source.id;
    no_coverage.definition.base_color_factor.w = 0.2;
    no_coverage.definition.alpha_mode = render::AlphaMode::masked;
    no_coverage.definition.alpha_cutoff = 0.25;
    no_coverage.revision = core::Revision{2U};
    require(static_cast<bool>(no_coverage_catalog.replace_material(no_coverage)) &&
        static_cast<bool>(no_coverage_document.set_material_catalog(
            std::move(no_coverage_catalog))),
        "fixture binds a masked use whose factor can never pass its cutoff");
    require(static_cast<bool>(d3d12::load_project_scene_textures(
            project_path, no_coverage_document)),
        "a masked material with no possible alpha coverage does not add a false mip constraint");

    auto incompatible_document = document;
    auto incompatible_catalog = incompatible_document.material_catalog();
    auto incompatible = incompatible_catalog.materials().at(2U);
    incompatible.definition.base_color.asset = source.id;
    incompatible.definition.alpha_mode = render::AlphaMode::masked;
    incompatible.definition.alpha_cutoff = 0.25;
    incompatible.revision = core::Revision{2U};
    require(static_cast<bool>(incompatible_catalog.replace_material(incompatible)) &&
        static_cast<bool>(incompatible_document.set_material_catalog(
            std::move(incompatible_catalog))),
        "fixture binds the same source to a second masked cutoff");
    const auto conflict = d3d12::load_project_scene_textures(
        project_path, incompatible_document);
    require(!conflict && conflict.error().code == core::ErrorCode::unsupported,
        "one source cannot silently choose between incompatible masked alpha cutoffs");
}

void project_adapter_preserves_source_and_rejects_missing_authoring() {
    const auto document = room_project(); const auto before = document.serialize();
    d3d12::SceneRenderCamera camera; camera.position = {5.0, 3.1, 5.5}; camera.target = {-0.3, 0.8, -1.0};
    const auto prepared = take(d3d12::prepare_project_scene_render("fixture-room", document, camera, 640U, 400U));
    require(prepared.objects.size() == 14U && prepared.source_revision == document.revision(), "all authored room objects prepare");
    require(prepared.light.direction_to_light.x == 0.3 &&
            prepared.light.direction_to_light.y == 0.7 &&
            prepared.light.direction_to_light.z == 1.0 &&
            prepared.light.radiance.x == 3.0 && prepared.light.radiance.y == 3.0 &&
            prepared.light.radiance.z == 3.0,
        "projects without visible authored lights retain the caller's explicit key light");
    require(prepared.source_digest == hash(before) && document.serialize() == before, "preparation binds whole canonical project without mutation");
    auto unassigned = document; require(static_cast<bool>(unassigned.set_material_catalog({})), "fixture removes assignments");
    require(!d3d12::prepare_project_scene_render("fixture-room", unassigned, camera, 640U, 400U), "no default material fills missing source assignment");
    auto graphed = document; auto graph_catalog = graphed.material_catalog();
    project::MaterialGraph graph; graph.identity = "fixture/surface"; graph.revision = core::Revision{1U};
    project::MaterialGraphNode color; color.id = 1U; color.kind = project::MaterialGraphNodeKind::constant_color;
    color.value_type = project::MaterialGraphValueType::color; color.constants = {1.0, 0.0, 0.0, 1.0};
    project::MaterialGraphNode scalar; scalar.id = 2U; scalar.constants[0U] = 1.0;
    graph.nodes = {color, scalar}; graph.outputs.base_color = 1U;
    graph.outputs.alpha = 2U; graph.outputs.roughness = 2U; graph.outputs.metallic = 2U;
    graph.outputs.occlusion = 2U; graph.outputs.emissive = 1U;
    graph.outputs.clearcoat = 2U; graph.outputs.clearcoat_roughness = 2U;
    require(static_cast<bool>(graph_catalog.insert_material_graph(graph)) &&
        static_cast<bool>(graph_catalog.bind_material_graph(1U, graph.identity)) &&
        static_cast<bool>(graphed.set_material_catalog(std::move(graph_catalog))), "fixture binds valid authored graph");
    const auto graph_prepared = take(d3d12::prepare_project_scene_render(
        "fixture-room", graphed, camera, 640U, 400U));
    const auto graph_material = std::find_if(graph_prepared.objects.front().materials.begin(),
        graph_prepared.objects.front().materials.end(), [](const auto& binding) {
            return binding.source_identity.find("/graph/") != std::string::npos;
        });
    const auto graph_color = graph_material == graph_prepared.objects.front().materials.end()
        ? core::Vec4d{} : graph_material->current_material.definition.base_color_factor;
    require(graph_material != graph_prepared.objects.front().materials.end() &&
            graph_color.x == 1.0 && graph_color.y == 0.0 &&
            graph_color.z == 0.0 && graph_color.w == 1.0 &&
            graph_material->current_material.definition.roughness == 1.0 &&
            graph_material->current_material.definition.metallic == 1.0 &&
            graph_material->current_material.definition.occlusion_strength == 1.0 &&
            graph_material->current_material.definition.emissive_factor.x == 1.0 &&
            graph_material->current_material.definition.emissive_factor.y == 0.0 &&
            graph_material->current_material.definition.emissive_strength == 1.0 &&
            graph_material->current_material.definition.clearcoat == 1.0 &&
            graph_material->current_material.definition.clearcoat_roughness == 1.0,
        "bound constant graph values reach the prepared PBR material");
    const auto desktop_graph = d3d12::prepare_project_scene_render(
        "desktop-" + std::string(48U, 'a'), graphed, camera, 640U, 400U);
    require(static_cast<bool>(desktop_graph),
        "valid desktop project identities must support graph-bound materials within artifact identity bounds");
    const auto another_desktop_graph = take(d3d12::prepare_project_scene_render(
        "desktop-" + std::string(48U, 'b'), graphed, camera, 640U, 400U));
    const auto& first_identity = desktop_graph.value().objects.front().materials.front().source_identity;
    require(first_identity.size() <= 128U && first_identity !=
        another_desktop_graph.objects.front().materials.front().source_identity,
        "bounded graph identities preserve distinct full project contexts without truncation");
    const auto repeated_desktop_graph = take(d3d12::prepare_project_scene_render(
        "desktop-" + std::string(48U, 'a'), graphed, camera, 640U, 400U));
    require(take(repeated_desktop_graph.digest()) == take(desktop_graph.value().digest()),
        "bounded graph identity generation is deterministic");

    auto stale_graph = graphed;
    auto stale_catalog = stale_graph.material_catalog();
    auto edited_graph = stale_catalog.material_graphs().at(graph.identity);
    edited_graph.revision = core::Revision{2U};
    edited_graph.nodes.front().constants[0U] = 0.25;
    require(static_cast<bool>(stale_catalog.replace_material_graph(edited_graph)) &&
            static_cast<bool>(stale_graph.set_material_catalog(std::move(stale_catalog))),
        "fixture changes a graph without rebinding its pinned source receipt");
    const auto stale_graph_result = d3d12::prepare_project_scene_render(
        "fixture-room", stale_graph, camera, 640U, 400U);
    require(!stale_graph_result &&
            stale_graph_result.error().code == core::ErrorCode::stale_data,
        "stale graph binding is refused instead of rendering old or current values implicitly");

    auto missing_input_graph = graphed;
    auto input_catalog = missing_input_graph.material_catalog();
    auto parameterized_graph = input_catalog.material_graphs().at(graph.identity);
    parameterized_graph.revision = core::Revision{2U};
    auto& alpha_node = parameterized_graph.nodes.at(1U);
    alpha_node.kind = project::MaterialGraphNodeKind::parameter_scalar;
    alpha_node.constants = {};
    alpha_node.parameter_name = "opacity";
    require(static_cast<bool>(input_catalog.replace_material_graph(parameterized_graph)) &&
            static_cast<bool>(input_catalog.bind_material_graph(1U, graph.identity)) &&
            static_cast<bool>(missing_input_graph.set_material_catalog(std::move(input_catalog))),
        "fixture explicitly rebinds a graph that now needs a caller input");
    const auto missing_input_result = d3d12::prepare_project_scene_render(
        "fixture-room", missing_input_graph, camera, 640U, 400U);
    require(!missing_input_result &&
            missing_input_result.error().code == core::ErrorCode::not_found,
        "unprovided graph parameters are refused without a fabricated render value");
    project::MaterialGraphEvaluationInputs supplied_inputs;
    project::MaterialGraphValue opacity_input;
    opacity_input.type = project::MaterialGraphValueType::scalar;
    opacity_input.components[0U] = 0.35;
    supplied_inputs.parameters.emplace("opacity", opacity_input);
    const auto& bound_parameterized_graph =
        missing_input_graph.material_catalog().material_graphs().at(graph.identity);
    const auto bound_parameterized_graph_digest =
        take(bound_parameterized_graph.canonical_digest());
    d3d12::SceneRenderMaterialGraphInputs supplied_graph_inputs;
    supplied_graph_inputs.emplace(graph.identity,
        d3d12::SceneRenderMaterialGraphInputBinding{
            bound_parameterized_graph.revision,
            bound_parameterized_graph_digest,
            supplied_inputs});
    const auto supplied_input_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", missing_input_graph, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
        supplied_graph_inputs));
    const auto supplied_material = std::find_if(
        supplied_input_request.objects.front().materials.begin(),
        supplied_input_request.objects.front().materials.end(), [](const auto& binding) {
            return binding.source_identity.find("/graph/") != std::string::npos;
        });
    require(supplied_material != supplied_input_request.objects.front().materials.end() &&
        supplied_material->current_material.definition.base_color_factor.w == 0.35 &&
        supplied_material->current_material.definition.roughness == 0.35 &&
        supplied_material->current_material.definition.metallic == 0.35,
        "explicit transient graph parameters reach the saved-scene PBR material");
    auto changed_graph_inputs = supplied_graph_inputs;
    changed_graph_inputs.at(graph.identity).inputs.parameters.at(
        "opacity").components[0U] = 0.75;
    const auto changed_input_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", missing_input_graph, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
        std::move(changed_graph_inputs)));
    require(take(supplied_input_request.digest()) != take(changed_input_request.digest()),
        "saved-scene request identity binds the exact transient graph input values");

    auto persistent_input_project = parameterized_graph_project();
    auto persistent_catalog = persistent_input_project.material_catalog();
    const auto persistent_graph = persistent_catalog.material_graphs().at(
        "fixture/parameterized");
    const auto persistent_graph_digest = take(persistent_graph.canonical_digest());
    project::MaterialGraphEvaluationInputs persistent_inputs;
    project::MaterialGraphValue roughness_input;
    roughness_input.type = project::MaterialGraphValueType::scalar;
    roughness_input.components[0U] = 0.25;
    persistent_inputs.parameters.emplace("roughness", roughness_input);
    project::MaterialGraphValue mask_input;
    mask_input.type = project::MaterialGraphValueType::scalar;
    mask_input.components[0U] = 0.25;
    persistent_inputs.surface_inputs.emplace(
        project::MaterialSurfaceInput::authored_mask, mask_input);
    auto wrong_persistent_inputs = persistent_inputs;
    wrong_persistent_inputs.parameters.at("roughness").type =
        project::MaterialGraphValueType::color;
    require(!persistent_catalog.set_material_graph_inputs(1U,
                persistent_graph.identity, persistent_graph.revision,
                persistent_graph_digest, wrong_persistent_inputs) &&
            persistent_catalog.material_graph_bindings().at(1U).inputs.parameters.empty(),
        "mistyped saved input values are rejected without changing the material binding");
    require(static_cast<bool>(persistent_catalog.set_material_graph_inputs(1U,
                persistent_graph.identity, persistent_graph.revision,
                persistent_graph_digest, persistent_inputs)) &&
            static_cast<bool>(persistent_input_project.set_material_catalog(
                std::move(persistent_catalog))),
        "exact typed parameter and surface inputs are durably attached to the graph binding");
    const auto persistent_input_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", persistent_input_project, camera, 640U, 400U));
    const auto persistent_material = std::find_if(
        persistent_input_request.objects.front().materials.begin(),
        persistent_input_request.objects.front().materials.end(), [](const auto& binding) {
            return binding.source_identity.find("/graph/") != std::string::npos;
        });
    require(persistent_material != persistent_input_request.objects.front().materials.end() &&
            persistent_material->current_material.definition.roughness == 0.5,
        "saved-scene D3D12 adapter evaluates durable binding values when no request override is supplied");
    auto partial_input_project = persistent_input_project;
    auto partial_catalog = partial_input_project.material_catalog();
    auto partial_inputs = persistent_inputs;
    partial_inputs.surface_inputs.clear();
    require(static_cast<bool>(partial_catalog.set_material_graph_inputs(1U,
                persistent_graph.identity, persistent_graph.revision,
                persistent_graph_digest, partial_inputs)) &&
            static_cast<bool>(partial_input_project.set_material_catalog(
                std::move(partial_catalog))),
        "a deliberately partial graph input set can remain authored without defaulting missing values");
    const auto partial_input_result = d3d12::prepare_project_scene_render(
        "fixture-room", partial_input_project, camera, 640U, 400U);
    require(!partial_input_result &&
            partial_input_result.error().code == core::ErrorCode::not_found,
        "missing durable surface input is refused instead of being replaced with a fallback");
    auto stale_persistent_project = persistent_input_project;
    auto stale_persistent_catalog = stale_persistent_project.material_catalog();
    auto stale_persistent_graph = stale_persistent_catalog.material_graphs().at(
        persistent_graph.identity);
    stale_persistent_graph.revision = stale_persistent_graph.revision.next();
    stale_persistent_graph.nodes.back().constants[0U] = 0.8;
    require(static_cast<bool>(stale_persistent_catalog.replace_material_graph(
                stale_persistent_graph)) &&
            static_cast<bool>(stale_persistent_project.set_material_catalog(
                std::move(stale_persistent_catalog))),
        "fixture edits a source while preserving its prior durable graph input binding");
    const auto stale_persistent_result = d3d12::prepare_project_scene_render(
        "fixture-room", stale_persistent_project, camera, 640U, 400U);
    require(!stale_persistent_result &&
            stale_persistent_result.error().code == core::ErrorCode::stale_data,
        "saved values pinned to an edited graph source cannot drive the current capture");
    d3d12::SceneRenderMaterialGraphInputs persistent_override;
    persistent_inputs.parameters.at("roughness").components[0U] = 0.75;
    persistent_override.emplace("fixture/parameterized",
        d3d12::SceneRenderMaterialGraphInputBinding{
            persistent_graph.revision, persistent_graph_digest, persistent_inputs});
    const auto persistent_override_request = take(d3d12::prepare_project_scene_render(
        "fixture-room", persistent_input_project, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}}, persistent_override));
    require(take(persistent_input_request.digest()) !=
                take(persistent_override_request.digest()),
        "an exact per-request value set overrides persisted inputs and changes request identity");
    auto partial_request_inputs = persistent_inputs;
    partial_request_inputs.surface_inputs.clear();
    d3d12::SceneRenderMaterialGraphInputs partial_request_override;
    partial_request_override.emplace("fixture/parameterized",
        d3d12::SceneRenderMaterialGraphInputBinding{
            persistent_graph.revision, persistent_graph_digest, partial_request_inputs});
    const auto partial_request_result = d3d12::prepare_project_scene_render(
        "fixture-room", persistent_input_project, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
        std::move(partial_request_override));
    require(!partial_request_result &&
            partial_request_result.error().code == core::ErrorCode::not_found,
        "request input overrides replace the complete saved set instead of merging missing keys");
    d3d12::SceneRenderMaterialGraphInputs unknown_graph_inputs;
    unknown_graph_inputs.emplace("missing/graph",
        d3d12::SceneRenderMaterialGraphInputBinding{});
    const auto unknown_graph_input_result = d3d12::prepare_project_scene_render(
        "fixture-room", missing_input_graph, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
        std::move(unknown_graph_inputs));
    require(!unknown_graph_input_result &&
            unknown_graph_input_result.error().code == core::ErrorCode::stale_data,
        "transient inputs for a foreign graph source fail closed");
    auto stale_graph_inputs = supplied_graph_inputs;
    stale_graph_inputs.at(graph.identity).graph_revision = core::Revision{1U};
    const auto stale_graph_input_result = d3d12::prepare_project_scene_render(
        "fixture-room", missing_input_graph, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
        std::move(stale_graph_inputs));
    require(!stale_graph_input_result &&
            stale_graph_input_result.error().code == core::ErrorCode::stale_data,
        "transient inputs pinned to another graph revision fail closed");
    auto stale_graph_digest_inputs = supplied_graph_inputs;
    stale_graph_digest_inputs.at(graph.identity).graph_digest = hash("stale-graph-source");
    const auto stale_graph_digest_result = d3d12::prepare_project_scene_render(
        "fixture-room", missing_input_graph, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
        std::move(stale_graph_digest_inputs));
    require(!stale_graph_digest_result &&
            stale_graph_digest_result.error().code == core::ErrorCode::stale_data,
        "transient inputs pinned to another graph digest fail closed");

    d3d12::SceneRenderMaterialGraphInputs excessive_graph_inputs;
    project::MaterialGraphValue bounded_graph_input;
    bounded_graph_input.type = project::MaterialGraphValueType::scalar;
    for (std::size_t graph_index = 0U; graph_index < 64U; ++graph_index) {
        d3d12::SceneRenderMaterialGraphInputBinding binding;
        for (std::size_t parameter_index = 0U; parameter_index <
                project::MaterialGraph::kMaxNodes; ++parameter_index) {
            binding.inputs.parameters.emplace(
                "parameter-" + std::to_string(parameter_index), bounded_graph_input);
        }
        excessive_graph_inputs.emplace(
            "missing/graph/" + std::to_string(graph_index), std::move(binding));
    }
    d3d12::SceneRenderMaterialGraphInputBinding overflowing_graph_binding;
    overflowing_graph_binding.inputs.parameters.emplace("parameter-overflow",
        bounded_graph_input);
    excessive_graph_inputs.emplace("missing/graph/overflow",
        std::move(overflowing_graph_binding));
    const auto aggregate_graph_input_result = d3d12::prepare_project_scene_render(
        "fixture-room", missing_input_graph, camera, 640U, 400U, {},
        render::PbrLight{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
        std::move(excessive_graph_inputs));
    require(!aggregate_graph_input_result &&
            aggregate_graph_input_result.error().code == core::ErrorCode::validation_failed,
        "aggregate graph inputs exceeding the evaluation work bound fail before graph lookup");

    auto unsupported_output_graph = graphed;
    auto unsupported_catalog = unsupported_output_graph.material_catalog();
    auto richer_graph = unsupported_catalog.material_graphs().at(graph.identity);
    richer_graph.revision = core::Revision{2U};
    richer_graph.outputs.transmission = 2U;
    require(static_cast<bool>(unsupported_catalog.replace_material_graph(richer_graph)) &&
            static_cast<bool>(unsupported_catalog.bind_material_graph(1U, graph.identity)) &&
            static_cast<bool>(unsupported_output_graph.set_material_catalog(
                std::move(unsupported_catalog))),
        "fixture pins a graph with a channel beyond the current PBR adapter");
    const auto unsupported_output_result = d3d12::prepare_project_scene_render(
        "fixture-room", unsupported_output_graph, camera, 640U, 400U);
    require(!unsupported_output_result &&
            unsupported_output_result.error().code == core::ErrorCode::unsupported,
        "unrepresentable graph outputs fail closed");
    d3d12::SceneRenderTexture foreign_texture; foreign_texture.asset = 99U;
    foreign_texture.current_source_digest = hash("foreign-source");
    require(!d3d12::prepare_project_scene_render("fixture-room", document, camera, 640U, 400U, {foreign_texture}),
            "texture identity must resolve in the actual project catalog");
    auto missing_uv = document; auto changed = missing_uv.meshes().begin()->second;
    auto payload = changed.attributes(); require(static_cast<bool>(payload.remove_layer("uv.fixture")), "fixture removes UV coordinates");
    require(static_cast<bool>(changed.set_attribute_payload(std::move(payload), {})), "fixture removes UV identity");
    require(static_cast<bool>(project::testing::access(missing_uv).replace_mesh(missing_uv.meshes().begin()->first, changed)), "source model retains material regions without UVs");
    const auto untextured_without_uv = d3d12::prepare_project_scene_render(
        "fixture-room", missing_uv, camera, 640U, 400U);
    require(untextured_without_uv && untextured_without_uv.value().textures.empty(),
        "texture-free materials do not require or fabricate authored UVs");
    changed = document.meshes().begin()->second;
    require(static_cast<bool>(changed.set_attribute_payload({}, {})), "fixture removes UV/regions");
    // Removing an assigned region is itself correctly refused by the model.
    auto orphaned = document;
    require(!project::testing::access(orphaned).replace_mesh(orphaned.meshes().begin()->first, changed), "source model refuses orphaned assigned regions");
    const auto reopened = take(project::ProjectDocument::deserialize(before));
    const auto reopened_request = take(d3d12::prepare_project_scene_render("fixture-room", reopened, camera, 640U, 400U));
    require(take(reopened_request.digest()) == take(prepared.digest()), "save/reopen reproduces exact render request");
}

void project_scene_adapter_wires_unambiguous_authored_colors() {
    auto document = room_project();
    const auto uncolored = take(d3d12::prepare_project_scene_render(
        "fixture-room", document, {}, 64U, 64U));
    require(!uncolored.objects.empty() && std::all_of(
            uncolored.objects.front().mesh.vertices.begin(),
            uncolored.objects.front().mesh.vertices.end(),
            [](const auto& vertex) { return !vertex.color.has_value(); }),
        "uncolored project meshes retain the renderer's implicit white default");

    const auto scene_objects = document.scene().objects_sorted();
    require(!scene_objects.empty() && scene_objects.front().mesh_asset.has_value(),
        "fixture exposes a mesh-bound scene object");
    const auto mesh_asset = *scene_objects.front().mesh_asset;
    auto mesh = document.meshes().at(mesh_asset);
    const auto topology = take(mesh.topology());
    const core::Vec4d authored_color{0.2, 0.8, 0.3, 0.6};
    std::vector<attributes::AttributeValue> corner_colors(
        topology.corners.size(), authored_color);
    auto payload = mesh.attributes();
    require(static_cast<bool>(payload.add_layer({
            {"color.fixture", attributes::AttributeDomain::corner,
                attributes::AttributeType::color4}, std::move(corner_colors)})),
        "fixture adds one authored corner color layer");
    require(static_cast<bool>(mesh.set_attribute_payload(
            std::move(payload), mesh.uv_sets())) &&
        static_cast<bool>(project::testing::access(document).replace_mesh(
            mesh_asset, std::move(mesh))),
        "fixture binds the authored color layer to the project mesh");

    const auto before_prepare = document.serialize();
    const auto colored = take(d3d12::prepare_project_scene_render(
        "fixture-room", document, {}, 64U, 64U));
    const auto colored_object = std::find_if(colored.objects.begin(), colored.objects.end(),
        [&](const auto& value) { return value.object == scene_objects.front().id; });
    const auto has_authored_color = [&](const core::Vec4d& color) {
        return color.x == authored_color.x && color.y == authored_color.y &&
            color.z == authored_color.z && color.w == authored_color.w;
    };
    require(colored_object != colored.objects.end() &&
        !colored_object->mesh.vertices.empty() && std::all_of(
            colored_object->mesh.vertices.begin(), colored_object->mesh.vertices.end(),
            [&](const auto& vertex) {
                return vertex.color.has_value() && has_authored_color(*vertex.color);
            }),
        "saved-project adapter transfers authored corner colors to compiled render vertices");
    require(document.serialize() == before_prepare,
        "color-aware scene preparation leaves the saved project unchanged");

    auto vertex_document = room_project();
    const auto vertex_scene_objects = vertex_document.scene().objects_sorted();
    require(!vertex_scene_objects.empty() && vertex_scene_objects.front().mesh_asset.has_value(),
        "vertex-color fixture exposes a mesh-bound scene object");
    const auto vertex_mesh_asset = *vertex_scene_objects.front().mesh_asset;
    auto vertex_mesh = vertex_document.meshes().at(vertex_mesh_asset);
    auto vertex_payload = vertex_mesh.attributes();
    std::vector<attributes::AttributeValue> vertex_colors(
        vertex_mesh.vertices_sorted().size(), authored_color);
    require(static_cast<bool>(vertex_payload.add_layer({
            {"color.vertex.fixture", attributes::AttributeDomain::vertex,
                attributes::AttributeType::color4}, std::move(vertex_colors)})) &&
        static_cast<bool>(vertex_mesh.set_attribute_payload(
            std::move(vertex_payload), vertex_mesh.uv_sets())) &&
        static_cast<bool>(project::testing::access(vertex_document).replace_mesh(
            vertex_mesh_asset, std::move(vertex_mesh))),
        "fixture binds an authored vertex-domain color layer");
    const auto vertex_colored = take(d3d12::prepare_project_scene_render(
        "fixture-room", vertex_document, {}, 64U, 64U));
    const auto vertex_colored_object = std::find_if(vertex_colored.objects.begin(),
        vertex_colored.objects.end(), [&](const auto& value) {
            return value.object == vertex_scene_objects.front().id;
        });
    require(vertex_colored_object != vertex_colored.objects.end() &&
        !vertex_colored_object->mesh.vertices.empty() && std::all_of(
            vertex_colored_object->mesh.vertices.begin(),
            vertex_colored_object->mesh.vertices.end(), [&](const auto& vertex) {
                return vertex.color.has_value() && has_authored_color(*vertex.color);
            }),
        "saved-project adapter transfers authored vertex colors to compiled render vertices");

    auto ambiguous_document = document;
    auto ambiguous_mesh = ambiguous_document.meshes().at(mesh_asset);
    auto ambiguous_payload = ambiguous_mesh.attributes();
    std::vector<attributes::AttributeValue> alternate_colors(
        topology.corners.size(), core::Vec4d{0.9, 0.1, 0.2, 1.0});
    require(static_cast<bool>(ambiguous_payload.add_layer({
            {"color.alternate", attributes::AttributeDomain::corner,
                attributes::AttributeType::color4}, std::move(alternate_colors)})) &&
        static_cast<bool>(ambiguous_mesh.set_attribute_payload(
            std::move(ambiguous_payload), ambiguous_mesh.uv_sets())) &&
        static_cast<bool>(project::testing::access(ambiguous_document).replace_mesh(
            mesh_asset, std::move(ambiguous_mesh))),
        "fixture binds a second authored color layer");
    const auto ambiguous = d3d12::prepare_project_scene_render(
        "fixture-room", ambiguous_document, {}, 64U, 64U);
    require(!ambiguous && ambiguous.error().code == core::ErrorCode::unsupported,
        "multiple authored color layers refuse instead of selecting one silently");

    auto invalid_domain_document = document;
    auto invalid_domain_mesh = invalid_domain_document.meshes().at(mesh_asset);
    auto invalid_domain_payload = invalid_domain_mesh.attributes();
    std::vector<attributes::AttributeValue> face_colors(
        invalid_domain_mesh.faces_sorted().size(), core::Vec4d{1.0, 1.0, 1.0, 1.0});
    require(static_cast<bool>(invalid_domain_payload.add_layer({
            {"color.face", attributes::AttributeDomain::face,
                attributes::AttributeType::color4}, std::move(face_colors)})) &&
        static_cast<bool>(invalid_domain_mesh.set_attribute_payload(
            std::move(invalid_domain_payload), invalid_domain_mesh.uv_sets())) &&
        static_cast<bool>(project::testing::access(invalid_domain_document).replace_mesh(
            mesh_asset, std::move(invalid_domain_mesh))),
        "fixture binds a wrong-domain color4 layer");
    const auto invalid_domain = d3d12::prepare_project_scene_render(
        "fixture-room", invalid_domain_document, {}, 64U, 64U);
    require(!invalid_domain && invalid_domain.error().code == core::ErrorCode::unsupported,
        "unsupported color4 domains fail closed before render preparation");
}

void authored_project_camera_resolves_world_pose_and_projection() {
    auto document = room_project();
    core::Transform transform;
    transform.translation = {3.0, 2.0, 1.0};
    const double half_angle = 0.7853981633974483;
    transform.rotation = {0.0, std::sin(half_angle), 0.0, std::cos(half_angle)};
    transform.scale = {2.0, 3.0, 4.0};
    const auto camera_object = take(project::testing::access(document).create_object(
        "Saved perspective camera", transform));
    project::AuthoredCamera authored;
    authored.object = camera_object;
    authored.identity = "fixture-perspective-camera";
    authored.vertical_fov_radians = 0.9;
    authored.near_plane = 0.02;
    authored.far_plane = 500.0;
    authored.revision = core::Revision{1U};
    auto catalog = document.presentation_catalog();
    require(static_cast<bool>(catalog.insert_camera(authored)) &&
        static_cast<bool>(document.set_presentation_catalog(std::move(catalog))),
        "fixture binds a saved perspective camera to its scene object");

    const auto resolved = take(d3d12::resolve_project_scene_render_camera(
        document, camera_object, 1.25));
    require(resolved.position.x == 3.0 && resolved.position.y == 2.0 &&
        resolved.position.z == 1.0 && resolved.target.x == 2.0 &&
        resolved.target.y == 2.0 && resolved.target.z == 1.0 &&
        resolved.up.x == 0.0 && resolved.up.y == 1.0 && resolved.up.z == 0.0 &&
        resolved.vertical_fov == authored.vertical_fov_radians &&
        resolved.near_plane == authored.near_plane &&
        resolved.far_plane == authored.far_plane && resolved.exposure_ev == 1.25,
        "resolved camera uses authored projection and current world pose while ignoring object scale");
    const auto absent = d3d12::resolve_project_scene_render_camera(
        document, scene::ObjectId{999999U});
    require(!absent && absent.error().code == core::ErrorCode::not_found,
        "camera lookup refuses an object without a current authored camera record");

    auto orthographic_document = room_project();
    const auto orthographic_object = take(
        project::testing::access(orthographic_document).create_object(
            "Saved orthographic camera"));
    project::AuthoredCamera orthographic;
    orthographic.object = orthographic_object;
    orthographic.identity = "fixture-orthographic-camera";
    orthographic.projection = project::CameraProjection::orthographic;
    orthographic.orthographic_height = 4.5;
    orthographic.revision = core::Revision{1U};
    auto orthographic_catalog = orthographic_document.presentation_catalog();
    require(static_cast<bool>(orthographic_catalog.insert_camera(orthographic)) &&
        static_cast<bool>(orthographic_document.set_presentation_catalog(
            std::move(orthographic_catalog))),
        "fixture binds an authored orthographic camera");
    const auto resolved_orthographic = take(d3d12::resolve_project_scene_render_camera(
        orthographic_document, orthographic_object));
    require(resolved_orthographic.projection ==
                d3d12::SceneRenderProjection::orthographic &&
            resolved_orthographic.orthographic_height ==
                orthographic.orthographic_height,
        "authored orthographic projection and vertical extent are preserved");
    auto orthographic_request = request();
    orthographic_request.camera = resolved_orthographic;
    require(static_cast<bool>(orthographic_request.validate()) &&
            static_cast<bool>(orthographic_request.digest()),
        "bounded authored orthographic camera admits with request identity");
}

void project_scene_adapter_wires_supported_authored_lights_and_refuses_fallbacks() {
    const auto make_document = [](project::AuthoredLight authored,
                                  core::Transform transform) {
        auto document = room_project();
        const auto object = take(project::testing::access(document).create_object(
            "Authored renderer light", transform));
        authored.object = object;
        authored.identity = "fixture.renderer-light";
        if (authored.kind == project::AuthoredLightKind::directional)
            authored.range = 0.0;
        auto catalog = document.presentation_catalog();
        require(static_cast<bool>(catalog.insert_light(std::move(authored))) &&
                static_cast<bool>(document.set_presentation_catalog(std::move(catalog))),
            "fixture binds an authored light to its scene object");
        return document;
    };
    const render::PbrLight caller_light{{1.0, 0.0, 0.0}, {99.0, 98.0, 97.0}};
    core::Transform directional_transform;
    const double half_quarter_turn = 0.7853981633974483;
    directional_transform.rotation = {
        0.0, std::sin(half_quarter_turn), 0.0, std::cos(half_quarter_turn)};
    project::AuthoredLight key;
    key.kind = project::AuthoredLightKind::directional;
    key.linear_color = {0.5, 0.25, 1.0};
    key.intensity = 4.0;
    auto directional_document = make_document(key, directional_transform);
    const auto directional = take(d3d12::prepare_project_scene_render(
        "fixture-room", directional_document, {}, 64U, 64U, {}, caller_light));
    require(std::abs(directional.light.direction_to_light.x - 1.0) < 1.0e-12 &&
            std::abs(directional.light.direction_to_light.z) < 1.0e-12 &&
            directional.light.radiance.x == 2.0 &&
            directional.light.radiance.y == 1.0 &&
            directional.light.radiance.z == 4.0,
        "authored directional world pose and linear photometry override the transient key light");

    project::AuthoredLight point;
    point.kind = project::AuthoredLightKind::point;
    const auto unsupported_point = d3d12::prepare_project_scene_render(
        "fixture-room", make_document(point, {}), {}, 64U, 64U, {}, caller_light);
    require(!unsupported_point &&
            unsupported_point.error().code == core::ErrorCode::unsupported,
        "shadow-casting point-light intent refuses instead of rendering with the unrelated caller key light");

    project::AuthoredLight point_no_shadows = point;
    point_no_shadows.casts_shadows = false;
    core::Transform point_transform;
    point_transform.translation = {1.0, 2.0, 3.0};
    const auto prepared_point = take(d3d12::prepare_project_scene_render(
        "fixture-room", make_document(point_no_shadows, point_transform), {},
        64U, 64U, {}, caller_light));
    require(prepared_point.local_lights.size() == 1U &&
            prepared_point.local_lights.front().kind ==
                d3d12::SceneRenderLocalLightKind::point &&
            !prepared_point.local_lights.front().casts_shadows &&
            prepared_point.local_lights.front().position.x == 1.0 &&
            prepared_point.local_lights.front().position.y == 2.0 &&
            prepared_point.local_lights.front().position.z == 3.0 &&
            prepared_point.light.radiance.x == 0.0,
        "unshadowed authored point source reaches the bounded local-light request");
    auto moved_point = prepared_point;
    moved_point.local_lights.front().position.x += 0.5;
    require(moved_point.validate() &&
            take(moved_point.digest()) != take(prepared_point.digest()),
        "local-light pose is validated and included in request identity");

    project::AuthoredLight spot = point_no_shadows;
    spot.kind = project::AuthoredLightKind::spot;
    spot.inner_cone_radians = 0.2;
    spot.outer_cone_radians = 0.5;
    const auto prepared_spot = take(d3d12::prepare_project_scene_render(
        "fixture-room", make_document(spot, directional_transform), {},
        64U, 64U, {}, caller_light));
    require(prepared_spot.local_lights.size() == 1U &&
            prepared_spot.local_lights.front().kind ==
                d3d12::SceneRenderLocalLightKind::spot &&
            !prepared_spot.local_lights.front().casts_shadows &&
            prepared_spot.local_lights.front().inner_cone_radians == 0.2 &&
            prepared_spot.local_lights.front().outer_cone_radians == 0.5 &&
            std::abs(prepared_spot.local_lights.front().emission_direction.x + 1.0) <
                1.0e-12,
        "unshadowed authored spot source preserves world emission direction and cone");

    project::AuthoredLight shadowed_spot = spot;
    shadowed_spot.casts_shadows = true;
    const auto prepared_shadowed_spot = d3d12::prepare_project_scene_render(
        "fixture-room", make_document(shadowed_spot, directional_transform), {},
        64U, 64U, {}, caller_light);
    require(prepared_shadowed_spot &&
            prepared_shadowed_spot.value().local_lights.size() == 1U &&
            prepared_shadowed_spot.value().local_lights.front().casts_shadows &&
            prepared_shadowed_spot.value().light.radiance.x == 0.0,
        "saved single-spot shadow intent reaches capture without reusing caller key-light energy");

    project::AuthoredLight area = point_no_shadows;
    area.kind = project::AuthoredLightKind::area;
    area.area_width = 1.0;
    area.area_height = 1.0;
    const auto unsupported_area = d3d12::prepare_project_scene_render(
        "fixture-room", make_document(area, {}), {}, 64U, 64U, {}, caller_light);
    require(!unsupported_area &&
            unsupported_area.error().code == core::ErrorCode::unsupported,
        "area-light intent remains explicit until an area emitter is implemented");

    project::AuthoredLight switchable = key;
    switchable.normal_state = project::LightNormalState::switchable;
    const auto unresolved_state = d3d12::prepare_project_scene_render(
        "fixture-room", make_document(switchable, {}), {}, 64U, 64U, {}, caller_light);
    require(!unresolved_state &&
            unresolved_state.error().code == core::ErrorCode::unsupported,
        "switchable authored state refuses without an explicit state resolution");

    project::AuthoredLight temperature_tagged = key;
    temperature_tagged.color_temperature_kelvin = 2'700.0;
    const auto unresolved_temperature = d3d12::prepare_project_scene_render(
        "fixture-room", make_document(temperature_tagged, {}), {}, 64U, 64U, {}, caller_light);
    require(!unresolved_temperature &&
            unresolved_temperature.error().code == core::ErrorCode::unsupported,
        "temperature-tagged sources refuse until color-temperature conversion is defined");

    project::AuthoredLight no_shadows = key;
    no_shadows.casts_shadows = false;
    const auto unsupported_shadow_mode = d3d12::prepare_project_scene_render(
        "fixture-room", make_document(no_shadows, {}), {}, 64U, 64U, {}, caller_light);
    require(!unsupported_shadow_mode &&
            unsupported_shadow_mode.error().code == core::ErrorCode::unsupported,
        "shadow-disabled authored intent refuses rather than using the renderer's shadowed key path");

    project::AuthoredLight dark_point = point;
    dark_point.failure_state = project::LightFailureState::dark;
    const auto known_dark = take(d3d12::prepare_project_scene_render(
        "fixture-room", make_document(dark_point, {}), {}, 64U, 64U, {}, caller_light));
    require(known_dark.light.radiance.x == 0.0 &&
            known_dark.light.radiance.y == 0.0 &&
            known_dark.light.radiance.z == 0.0,
        "known-dark authored fixtures suppress the caller fallback");

    project::AuthoredLight black_point = point;
    black_point.linear_color = {};
    const auto zero_radiance = take(d3d12::prepare_project_scene_render(
        "fixture-room", make_document(black_point, {}), {}, 64U, 64U, {}, caller_light));
    require(zero_radiance.light.radiance.x == 0.0 &&
            zero_radiance.light.radiance.y == 0.0 &&
            zero_radiance.light.radiance.z == 0.0,
        "zero-radiance authored fixtures do not trigger unsupported-light approximation");

    auto divergent = directional_document;
    core::Transform second_transform;
    const auto second_object = take(project::testing::access(divergent).create_object(
        "Divergent authored renderer light", second_transform));
    project::AuthoredLight second = key;
    second.object = second_object;
    second.identity = "fixture.renderer-light-secondary";
    auto divergent_catalog = divergent.presentation_catalog();
    require(static_cast<bool>(divergent_catalog.insert_light(std::move(second))) &&
            static_cast<bool>(divergent.set_presentation_catalog(
                std::move(divergent_catalog))),
        "fixture binds a second directional source");
    const auto divergent_result = d3d12::prepare_project_scene_render(
        "fixture-room", divergent, {}, 64U, 64U, {}, caller_light);
    require(!divergent_result &&
            divergent_result.error().code == core::ErrorCode::unsupported,
        "divergent authored directions fail closed at the current single-direction GPU boundary");
}

production::RenderMeshArtifact triangle(bool sloped = false) {
    production::RenderMeshArtifact mesh;
    mesh.source_revision = core::Revision{7U}; mesh.mesh_revision = core::Revision{2U};
    mesh.source_digest = hash("scene-renderer-authored-fixture-v1");
    const double h = std::sqrt(0.5);
    const core::Vec3d normal = sloped ? core::Vec3d{h, 0.0, h} : core::Vec3d{0.0, 0.0, 1.0};
    const core::Vec4d tangent = sloped ? core::Vec4d{h, 0.0, -h, 1.0} : core::Vec4d{1.0, 0.0, 0.0, 1.0};
    mesh.vertices = {
        {{1U}, {-0.7, -0.7, sloped ? 0.7 : 0.0}, normal, core::Vec2d{0.0, 1.0}, {}, {}, tangent},
        {{2U}, {0.7, -0.7, sloped ? -0.7 : 0.0}, normal, core::Vec2d{1.0, 1.0}, {}, {}, tangent},
        {{3U}, {0.0, 0.7, 0.0}, normal, core::Vec2d{0.5, 0.0}, {}, {}, tangent}};
    mesh.indices = {0U, 1U, 2U}; mesh.triangle_faces = {{1U}};
    mesh.submeshes = {{std::nullopt, 0U, 3U}};
    require(static_cast<bool>(mesh.validate()), "authored test triangle validates");
    return mesh;
}
void diffuse_irradiance_sh_file_is_bounded_and_digest_bound() {
    TempDirectory temporary;
    const auto path = temporary.path() / "environment.sh";
    std::string encoded = "CARTO_DIFFUSE_IRRADIANCE_SH_V1\n";
    for (std::size_t i = 0U; i < 9U; ++i) encoded += "0 0 0\n";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        require(static_cast<bool>(output), "fixture writes diffuse irradiance SH");
    }
    const auto environment = take(d3d12::load_scene_render_environment_sh(path));
    require(environment.source_digest == hash(encoded) &&
        std::all_of(environment.diffuse_irradiance_over_pi_sh.begin(),
            environment.diffuse_irradiance_over_pi_sh.end(), [](core::Vec3d value) {
                return value.x == 0.0 && value.y == 0.0 && value.z == 0.0;
            }), "SH loader preserves all coefficients and exact file identity");
    const auto write_text = [&](std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        require(static_cast<bool>(output), "fixture writes malformed SH source");
    };
    write_text("CARTO_DIFFUSE_IRRADIANCE_SH_V0\n");
    require(!d3d12::load_scene_render_environment_sh(path), "unsupported SH schema refuses");
    std::string trailing = "CARTO_DIFFUSE_IRRADIANCE_SH_V1\n";
    for (std::size_t i = 0U; i < 9U; ++i) trailing += "0 0 0 ";
    trailing += "extra\n";
    write_text(trailing);
    require(!d3d12::load_scene_render_environment_sh(path), "trailing SH values refuse");
    std::string overbound = "CARTO_DIFFUSE_IRRADIANCE_SH_V1\n1000001 0 0\n";
    for (std::size_t i = 1U; i < 9U; ++i) overbound += "0 0 0\n";
    write_text(overbound);
    require(!d3d12::load_scene_render_environment_sh(path),
        "finite but over-bound SH coefficient refuses");
    write_text("CARTO_DIFFUSE_IRRADIANCE_SH_V1\n" + std::string(4097U, '0'));
    require(!d3d12::load_scene_render_environment_sh(path), "over-budget SH input refuses");
}
std::vector<std::uint8_t> equirectangular_tga(
    std::uint16_t width,
    std::uint16_t height,
    std::array<std::uint8_t, 4U> rgba) {
    std::vector<std::uint8_t> encoded(18U +
        static_cast<std::size_t>(width) * height * 4U, 0U);
    encoded[2U] = 2U;
    encoded[12U] = static_cast<std::uint8_t>(width & 0xffU);
    encoded[13U] = static_cast<std::uint8_t>(width >> 8U);
    encoded[14U] = static_cast<std::uint8_t>(height & 0xffU);
    encoded[15U] = static_cast<std::uint8_t>(height >> 8U);
    encoded[16U] = 32U;
    encoded[17U] = 0x28U;
    for (std::size_t offset = 18U; offset < encoded.size(); offset += 4U) {
        encoded[offset] = rgba[2U];
        encoded[offset + 1U] = rgba[1U];
        encoded[offset + 2U] = rgba[0U];
        encoded[offset + 3U] = rgba[3U];
    }
    return encoded;
}

void append_test_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::uint32_t test_png_crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void append_test_png_chunk(
    std::vector<std::uint8_t>& bytes,
    std::string_view type,
    std::span<const std::uint8_t> payload) {
    append_test_be32(bytes, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crc_offset = bytes.size();
    bytes.insert(bytes.end(), type.begin(), type.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    append_test_be32(bytes, test_png_crc32(
        std::span<const std::uint8_t>{bytes}.subspan(crc_offset)));
}

std::vector<std::uint8_t> equirectangular_png_with_srgb_metadata() {
    std::vector<std::uint8_t> bytes{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    std::array<std::uint8_t, 13U> ihdr{};
    ihdr[3U] = 16U;
    ihdr[7U] = 8U;
    ihdr[8U] = 8U;
    ihdr[9U] = 6U;
    append_test_png_chunk(bytes, "IHDR", ihdr);
    constexpr std::array<std::uint8_t, 1U> srgb_intent{0U};
    append_test_png_chunk(bytes, "sRGB", srgb_intent);
    constexpr std::array<std::uint8_t, 1U> opaque_idat{0U};
    append_test_png_chunk(bytes, "IDAT", opaque_idat);
    append_test_png_chunk(bytes, "IEND", {});
    return bytes;
}

std::vector<std::uint8_t> equirectangular_seam_tga() {
    constexpr std::uint16_t width = 16U;
    constexpr std::uint16_t height = 8U;
    auto encoded = equirectangular_tga(width, height, {0U, 0U, 0U, 255U});
    for (std::uint16_t y = 0U; y < height; ++y) {
        const std::size_t row = 18U + static_cast<std::size_t>(y) * width * 4U;
        encoded[row + 2U] = 255U; // First longitude column: red.
        const std::size_t last = row + static_cast<std::size_t>(width - 1U) * 4U;
        encoded[last] = 255U; // Last longitude column: blue.
    }
    return encoded;
}
std::vector<std::uint8_t> constant_radiance_hdr(
    std::uint32_t width,
    std::uint32_t height,
    std::array<float, 3U> linear_rgb) {
    const float maximum = std::max({linear_rgb[0U], linear_rgb[1U], linear_rgb[2U]});
    require(std::isfinite(maximum) && maximum > 0.0F,
        "HDR fixture radiance is finite and nonzero");
    int exponent = 0;
    const float mantissa = std::frexp(maximum, &exponent);
    const float channel_scale = mantissa * 256.0F / maximum;
    const std::array<std::uint8_t, 4U> rgbe{
        static_cast<std::uint8_t>(std::clamp(linear_rgb[0U] * channel_scale, 0.0F, 255.0F)),
        static_cast<std::uint8_t>(std::clamp(linear_rgb[1U] * channel_scale, 0.0F, 255.0F)),
        static_cast<std::uint8_t>(std::clamp(linear_rgb[2U] * channel_scale, 0.0F, 255.0F)),
        static_cast<std::uint8_t>(exponent + 128)};
    std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
        std::to_string(height) + " +X " + std::to_string(width) + "\n";
    std::vector<std::uint8_t> encoded(header.begin(), header.end());
    for (std::uint32_t row = 0U; row < height; ++row) {
        encoded.push_back(2U);
        encoded.push_back(2U);
        encoded.push_back(static_cast<std::uint8_t>(width >> 8U));
        encoded.push_back(static_cast<std::uint8_t>(width & 0xffU));
        for (const std::uint8_t channel : rgbe) {
            encoded.push_back(static_cast<std::uint8_t>(128U + width));
            encoded.push_back(channel);
        }
    }
    return encoded;
}
std::vector<std::uint8_t> legacy_radiance_hdr(
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t>& pixel_stream) {
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
        std::to_string(height) + " +X " + std::to_string(width) + "\n";
    std::vector<std::uint8_t> encoded(header.begin(), header.end());
    encoded.insert(encoded.end(), pixel_stream.begin(), pixel_stream.end());
    return encoded;
}
std::vector<std::uint8_t> radiance_hdr_with_hot_last_column(
    std::uint32_t width,
    std::uint32_t height) {
    require(width > 0U && width <= 128U && height > 0U,
        "odd-size HDR fixture fits one bounded literal packet per channel");
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
        std::to_string(height) + " +X " + std::to_string(width) + "\n";
    std::vector<std::uint8_t> encoded(header.begin(), header.end());
    for (std::uint32_t y = 0U; y < height; ++y) {
        encoded.insert(encoded.end(), {2U, 2U,
            static_cast<std::uint8_t>(width >> 8U),
            static_cast<std::uint8_t>(width & 0xffU)});
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            encoded.push_back(static_cast<std::uint8_t>(width));
            for (std::uint32_t x = 0U; x < width; ++x) {
                const std::array<std::uint8_t, 4U> pixel = x + 1U == width
                    ? std::array<std::uint8_t, 4U>{200U, 0U, 0U, 135U}
                    : std::array<std::uint8_t, 4U>{128U, 0U, 0U, 129U};
                encoded.push_back(pixel[channel]);
            }
        }
    }
    return encoded;
}
void equirectangular_environment_derives_digest_bound_sh() {
    TempDirectory temporary;
    const auto path = temporary.path() / "environment.tga";
    const auto write = [&](const std::vector<std::uint8_t>& encoded) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
        require(static_cast<bool>(output), "fixture writes equirectangular image");
    };
    const auto opaque = equirectangular_tga(16U, 8U, {64U, 128U, 192U, 255U});
    write(opaque);
    const auto linear = take(d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear));
    constexpr double kY00Integral = 3.544907701811032;
    const double linear_red = 64.0 / 255.0;
    require(linear.source_digest == assets::sha256(opaque) &&
        linear.source_color_space == assets::ColorSpace::linear &&
        linear.specular_environment_mips.has_value() &&
        linear.specular_environment_mips->levels.front().width == 16U &&
        linear.specular_environment_mips->levels.front().height == 8U &&
        std::abs(linear.diffuse_irradiance_over_pi_sh[0U].x -
            linear_red * kY00Integral) < 1.0e-6,
        "uniform linear panorama derives source-bound diffuse and specular inputs");
    for (const auto& level : linear.specular_environment_mips->levels) {
        for (const auto& pixel : level.linear_rgb) {
            require(std::abs(pixel[0U] - 64.0F / 255.0F) < 1.0e-5F &&
                std::abs(pixel[1U] - 128.0F / 255.0F) < 1.0e-5F &&
                std::abs(pixel[2U] - 192.0F / 255.0F) < 1.0e-5F,
                "GGX prefilter preserves a constant linear LDR environment at every roughness");
        }
    }
    auto linear_request = request();
    linear_request.environment = linear;
    auto relabeled_environment = linear;
    relabeled_environment.source_color_space = assets::ColorSpace::srgb;
    relabeled_environment.specular_environment_mips.reset();
    auto relabeled_request = request();
    relabeled_request.environment = relabeled_environment;
    require(take(linear_request.digest()) != take(relabeled_request.digest()),
        "explicit image color-space interpretation participates in request identity");
    auto stale_mips = linear;
    stale_mips.specular_environment_mips->source_digest = hash("different panorama bytes");
    auto stale_mip_request = request();
    stale_mip_request.environment = stale_mips;
    require(!stale_mip_request.validate(),
        "specular mip artifacts from another source fail closed");
    auto duplicate_terminal_mip = linear;
    const auto terminal_mip =
        duplicate_terminal_mip.specular_environment_mips->levels.back();
    duplicate_terminal_mip.specular_environment_mips->levels.push_back(
        terminal_mip);
    require(!duplicate_terminal_mip.specular_environment_mips->refresh_digest(),
        "noncanonical duplicate terminal environment mip refuses");
    auto refreshed_mips = linear;
    auto& changed_mip_pixel = refreshed_mips.specular_environment_mips->levels[0U].linear_rgb[0U][0U];
    changed_mip_pixel += 0.01F;
    take(refreshed_mips.specular_environment_mips->refresh_digest());
    auto refreshed_mip_request = request();
    refreshed_mip_request.environment = refreshed_mips;
    require(take(refreshed_mip_request.digest()) != take(linear_request.digest()),
        "environment mip artifact content participates in request identity");
    const auto srgb = take(d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::srgb));
    const double encoded_red = 64.0 / 255.0;
    const double decoded_red = encoded_red <= 0.04045
        ? encoded_red / 12.92
        : std::pow((encoded_red + 0.055) / 1.055, 2.4);
    require(srgb.source_color_space == assets::ColorSpace::srgb &&
        srgb.specular_environment_mips.has_value() &&
        srgb.specular_environment_mips->source_color_space == assets::ColorSpace::srgb &&
        std::abs(srgb.diffuse_irradiance_over_pi_sh[0U].x -
            decoded_red * kY00Integral) < 1.0e-6 &&
        srgb.diffuse_irradiance_over_pi_sh[0U].x <
            linear.diffuse_irradiance_over_pi_sh[0U].x,
        "explicit sRGB interpretation converts source channels to linear light");
    write(equirectangular_tga(8U, 8U, {64U, 128U, 192U, 255U}));
    require(!d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear),
        "non-equirectangular aspect ratio refuses");
    write(equirectangular_tga(16U, 8U, {64U, 128U, 192U, 0U}));
    require(!d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear),
        "transparent environment pixels refuse instead of flattening alpha");
    require(!d3d12::load_scene_render_environment_equirectangular(
        path, static_cast<assets::ColorSpace>(255U)),
        "unknown environment color-space value refuses");

    write(equirectangular_png_with_srgb_metadata());
    const auto profile_refused = d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::srgb);
    require(!profile_refused &&
            profile_refused.error().code == core::ErrorCode::unsupported,
        "environment conversion refuses embedded color metadata it does not transform");
}
void radiance_hdr_environment_preserves_linear_specular_mips() {
    TempDirectory temporary;
    const auto path = temporary.path() / "environment.hdr";
    const auto encoded = constant_radiance_hdr(16U, 8U, {4.0F, 0.25F, 0.125F});
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
        require(static_cast<bool>(output), "fixture writes linear Radiance source");
    }
    const auto environment = take(d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear));
    require(environment.source_digest == assets::sha256(encoded) &&
        environment.specular_environment_mips.has_value() &&
        environment.specular_environment_mips->levels.front().width == 16U &&
        environment.specular_environment_mips->levels.front().height == 8U &&
        std::abs(environment.specular_environment_mips->levels.front().linear_rgb[0U][0U] -
            4.0F) < 0.001F &&
        std::abs(environment.specular_environment_mips->levels.front().linear_rgb[0U][1U] -
            0.25F) < 0.001F,
        "Radiance source bytes produce digest-bound linear reflection mips");
    for (const auto& level : environment.specular_environment_mips->levels) {
        for (const auto& pixel : level.linear_rgb) {
            require(std::abs(pixel[0U] - 4.0F) < 0.001F &&
                std::abs(pixel[1U] - 0.25F) < 0.001F &&
                std::abs(pixel[2U] - 0.125F) < 0.001F,
                "GGX prefilter preserves a constant HDR environment at every roughness");
        }
    }
    auto input = request();
    input.environment = environment;
    const auto initial_digest = take(input.digest());
    auto stale = environment;
    stale.specular_environment_mips->source_digest = hash("another HDR source");
    input.environment = stale;
    require(!input.validate(), "HDR mips from a different source fail closed");
    auto changed = environment;
    changed.specular_environment_mips->levels[0U].linear_rgb[0U][0U] = 8.0F;
    take(changed.specular_environment_mips->refresh_digest());
    input.environment = changed;
    require(take(input.digest()) != initial_digest,
        "linear HDR mip artifact content participates in request identity");
    require(!d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::srgb),
        "Radiance RGBE refuses an sRGB interpretation");
    const auto odd_encoded = radiance_hdr_with_hot_last_column(30U, 15U);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(odd_encoded.data()),
            static_cast<std::streamsize>(odd_encoded.size()));
        require(static_cast<bool>(output), "fixture writes odd-size linear Radiance source");
    }
    const auto odd_environment = take(d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear));
    require(odd_environment.specular_environment_mips.has_value(),
        "eligible odd-size panorama retains reflection mips");
    const auto& odd_mips = *odd_environment.specular_environment_mips;
    require(odd_mips.levels.size() == 5U &&
        odd_mips.levels.back().width == 1U && odd_mips.levels.back().height == 1U &&
        odd_mips.levels.front().linear_rgb[14U * 30U + 29U][0U] > 90.0F,
        "odd-sized panorama's final source column remains available to prefiltering");
    float brightest_prefiltered_red = 0.0F;
    for (std::size_t level = 1U; level < odd_mips.levels.size(); ++level) {
        for (const auto& pixel : odd_mips.levels[level].linear_rgb)
            brightest_prefiltered_red = std::max(brightest_prefiltered_red, pixel[0U]);
    }
    require(brightest_prefiltered_red > 1.0F,
        "GGX roughness levels carry the odd source's bright final column into reflections");

    const std::vector<std::uint8_t> rgb{128U, 8U, 4U, 131U};
    std::vector<std::uint8_t> legacy_pixels = rgb;
    legacy_pixels.insert(legacy_pixels.end(), {1U, 1U, 1U, 127U});
    const auto legacy_encoded = legacy_radiance_hdr(16U, 8U, legacy_pixels);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(legacy_encoded.data()),
            static_cast<std::streamsize>(legacy_encoded.size()));
        require(static_cast<bool>(output), "fixture writes legacy RGBE run source");
    }
    const auto legacy_environment = take(d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear));
    require(legacy_environment.specular_environment_mips.has_value(),
        "eligible legacy RGBE run retains HDR reflection mips");
    const auto& modern_chain = *environment.specular_environment_mips;
    const auto& legacy_chain = *legacy_environment.specular_environment_mips;
    bool matching_mips = legacy_chain.levels.size() == modern_chain.levels.size();
    for (std::size_t level = 0U; matching_mips && level < modern_chain.levels.size(); ++level) {
        matching_mips = legacy_chain.levels[level].width == modern_chain.levels[level].width &&
            legacy_chain.levels[level].height == modern_chain.levels[level].height &&
            legacy_chain.levels[level].linear_rgb == modern_chain.levels[level].linear_rgb;
    }
    require(matching_mips &&
        std::abs(legacy_environment.diffuse_irradiance_over_pi_sh[0U].x -
            environment.diffuse_irradiance_over_pi_sh[0U].x) < 1.0e-6,
        "legacy image-wide repeat across scanlines matches modern RLE radiance");

    const auto small_flat = legacy_radiance_hdr(2U, 1U,
        {128U, 8U, 4U, 131U, 128U, 8U, 4U, 131U});
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(small_flat.data()),
            static_cast<std::streamsize>(small_flat.size()));
        require(static_cast<bool>(output), "fixture writes small flat RGBE source");
    }
    const auto small_environment = take(d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear));
    require(!small_environment.specular_environment_mips.has_value() &&
        std::abs(small_environment.diffuse_irradiance_over_pi_sh[0U].x -
            environment.diffuse_irradiance_over_pi_sh[0U].x) < 1.0e-6,
        "small flat legacy RGBE remains available for bounded diffuse SH");

    const auto rejects_legacy_payload = [&](std::vector<std::uint8_t> payload,
                                             std::string_view reason) {
        const auto malformed = legacy_radiance_hdr(16U, 8U, payload);
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(malformed.data()),
                static_cast<std::streamsize>(malformed.size()));
            require(static_cast<bool>(output), "fixture writes malformed legacy RGBE source");
        }
        require(!d3d12::load_scene_render_environment_equirectangular(
            path, assets::ColorSpace::linear), reason);
    };
    rejects_legacy_payload({1U, 1U, 1U, 1U},
        "legacy RGBE repeat without a previous literal refuses");
    rejects_legacy_payload({128U, 8U, 4U, 131U, 1U, 1U, 1U, 128U},
        "legacy RGBE repeat beyond the remaining image refuses");
    rejects_legacy_payload({128U, 8U}, "truncated legacy RGBE literal refuses");
    auto trailing_legacy = rgb;
    trailing_legacy.insert(trailing_legacy.end(), {1U, 1U, 1U, 127U, 9U});
    rejects_legacy_payload(std::move(trailing_legacy),
        "trailing legacy RGBE data refuses");
}
void rebind(d3d12::SceneRenderObject& object) {
    std::vector<production::LookdevMaterialBinding> bindings;
    for (auto& binding : object.materials) {
        binding.material = take(d3d12::compile_scene_render_material_artifact(
            binding.source_identity, binding.current_material));
        bindings.push_back({binding.material_slot, binding.material});
    }
    object.draw_package = take(production::compile_lookdev_draw_package(object.mesh, bindings));
}
d3d12::SceneRenderObject object(std::uint64_t id, core::Vec4d color, bool sloped = false) {
    d3d12::SceneRenderObject result;
    result.object = {id}; result.mesh = triangle(sloped);
    render::MaterialAsset material{id, "authored fixture material", {}, core::Revision{3U}};
    material.definition.base_color_factor = color; material.definition.roughness = 0.65;
    result.materials.push_back({std::nullopt, "fixture/material/" + std::to_string(id), material, {}});
    rebind(result); return result;
}
d3d12::SceneRenderRequest request() {
    d3d12::SceneRenderRequest result;
    result.source_revision = core::Revision{7U}; result.source_digest = hash("scene-renderer-authored-fixture-v1");
    result.width = 96U; result.height = 96U; result.camera.position = {0.0, 0.0, 3.0};
    result.camera.near_plane = 0.1; result.camera.far_plane = 20.0;
    result.light = {{0.0, 0.0, 1.0}, {3.0, 3.0, 3.0}};
    result.objects.push_back(object(1U, {0.7, 0.1, 0.03, 1.0}));
    return result;
}
d3d12::SceneRenderTexture texture(std::uint64_t id, std::array<std::uint8_t, 4U> rgba,
                                  assets::TextureSemantic semantic = assets::TextureSemantic::base_color) {
    std::vector<std::uint8_t> tga(18U, 0U);
    tga[2U] = 2U; tga[12U] = 1U; tga[14U] = 1U; tga[16U] = 32U; tga[17U] = 0x28U;
    tga.insert(tga.end(), {rgba[2U], rgba[1U], rgba[0U], rgba[3U]});
    const auto decoded = take(assets::decode_tga_rgba8(tga));
    auto mips = take(assets::generate_rgba8_mips(decoded, {semantic, assets::expected_color_space(semantic)}));
    return {id, decoded.admission.source.digest, std::move(mips)};
}
d3d12::SceneRenderTexture texture_grid(std::uint64_t id,
    assets::TextureSemantic semantic = assets::TextureSemantic::base_color) {
    constexpr std::array<std::array<std::uint8_t, 4U>, 4U> pixels{{
        {255U, 16U, 16U, 255U}, {16U, 255U, 16U, 255U},
        {16U, 16U, 255U, 255U}, {255U, 255U, 16U, 255U}}};
    std::vector<std::uint8_t> tga(18U, 0U);
    tga[2U] = 2U; tga[12U] = 2U; tga[14U] = 2U;
    tga[16U] = 32U; tga[17U] = 0x28U;
    for (const auto& rgba : pixels)
        tga.insert(tga.end(), {rgba[2U], rgba[1U], rgba[0U], rgba[3U]});
    const auto decoded = take(assets::decode_tga_rgba8(tga));
    auto mips = take(assets::generate_rgba8_mips(decoded,
        {semantic, assets::expected_color_space(semantic)}));
    return {id, decoded.admission.source.digest, std::move(mips)};
}
void rejects_stale_foreign_and_unsupported_inputs() {
    const auto valid = request(); require(static_cast<bool>(valid.validate()), "valid capture admits");
    const auto refused = [&](auto mutate, std::string_view label) {
        auto bad = valid; mutate(bad); require(!bad.validate(), label); require(!bad.digest(), label);
    };
    auto temporal_reset = valid;
    temporal_reset.temporal = d3d12::SceneRenderTemporalInputs{};
    temporal_reset.temporal->resolved_color = {3U, 1U};
    temporal_reset.temporal->camera_cut = true;
    require(static_cast<bool>(temporal_reset.validate()),
        "first-frame temporal reset admits without stale history or motion");
    auto jittered = valid;
    jittered.subpixel_jitter_pixels = {0.25, -0.5};
    require(static_cast<bool>(jittered.validate()) &&
            take(jittered.digest()) != take(valid.digest()),
        "bounded output-pixel jitter is valid and participates in request identity");
    refused([](auto& r) { r.subpixel_jitter_pixels.x = 0.5001; },
        "subpixel jitter above half a pixel is refused");
    auto temporal_static = temporal_reset;
    temporal_static.temporal->camera_cut = false;
    temporal_static.temporal->history_color = {1U, 1U};
    temporal_static.temporal->history_initial_state =
        device_ir::ResourceState::shader_read;
    temporal_static.temporal->history_content_digest = hash("temporal-history-content");
    temporal_static.temporal->history_request_digest = hash("temporal-history-request");
    temporal_static.temporal->history_source_revision = valid.source_revision;
    temporal_static.temporal->history_source_digest = valid.source_digest;
    temporal_static.temporal->zero_motion = true;
    temporal_static.temporal->feedback = 0.75F;
    require(static_cast<bool>(temporal_static.validate()) &&
            static_cast<bool>(temporal_static.digest()),
        "explicit static-camera history admits with provenance and zero-motion assertion");
    auto temporal_moving = temporal_static;
    temporal_moving.temporal->zero_motion = false;
    temporal_moving.temporal->motion_vectors = {2U, 1U};
    temporal_moving.temporal->motion_initial_state =
        device_ir::ResourceState::shader_read;
    temporal_moving.temporal->motion_vectors_digest = hash("temporal-motion-content");
    temporal_moving.temporal->motion_history_request_digest =
        temporal_moving.temporal->history_request_digest;
    require(static_cast<bool>(temporal_moving.validate()),
        "explicit current-to-history motion texture admits");
    auto temporal_constant_motion = temporal_static;
    temporal_constant_motion.temporal->zero_motion = false;
    temporal_constant_motion.temporal->constant_motion_pixels =
        core::Vec2d{0.375, -0.25};
    temporal_constant_motion.temporal->motion_history_request_digest =
        temporal_constant_motion.temporal->history_request_digest;
    require(static_cast<bool>(temporal_constant_motion.validate()) &&
            static_cast<bool>(temporal_constant_motion.digest()),
        "explicit uniform subpixel current-to-history offset admits with prior-frame binding");
    const auto temporal_identity = take(temporal_static.digest());
    auto changed_temporal_history = temporal_static;
    changed_temporal_history.temporal->history_content_digest =
        hash("different-temporal-history-content");
    require(take(changed_temporal_history.digest()) != temporal_identity,
        "temporal history content identity participates in request digest");
    require(take(temporal_constant_motion.digest()) != temporal_identity,
        "constant subpixel motion participates in request identity");
    auto changed_constant_motion = temporal_constant_motion;
    changed_constant_motion.temporal->constant_motion_pixels->x += 0.125;
    require(take(changed_constant_motion.digest()) !=
            take(temporal_constant_motion.digest()),
        "changing the uniform subpixel offset changes request identity");
    const auto refuse_temporal = [&](auto mutate, std::string_view label) {
        auto bad = temporal_static;
        mutate(bad);
        require(!bad.validate() && !bad.digest(), label);
    };
    refuse_temporal([](auto& r) {
        r.temporal->history_request_digest = {};
    }, "temporal history without previous request identity refused");
    refuse_temporal([](auto& r) {
        r.temporal->history_initial_state =
            device_ir::ResourceState::undefined;
    }, "history texture without declared incoming state refused");
    refuse_temporal([](auto& r) {
        r.temporal->zero_motion = false;
    }, "temporal history without motion source refused");
    const auto refuse_constant_motion = [&](auto mutate, std::string_view label) {
        auto bad = temporal_constant_motion;
        mutate(bad);
        require(!bad.validate() && !bad.digest(), label);
    };
    refuse_constant_motion([](auto& r) {
        r.temporal->constant_motion_pixels->x =
            std::numeric_limits<double>::quiet_NaN();
    }, "nonfinite constant temporal motion refused");
    refuse_constant_motion([](auto& r) {
        r.temporal->constant_motion_pixels->y =
            static_cast<double>(r.height) + 0.25;
    }, "constant temporal motion outside the frame bound refused");
    refuse_constant_motion([](auto& r) {
        r.temporal->motion_history_request_digest = hash("foreign-prior-frame");
    }, "constant temporal motion bound to another prior frame refused");
    refuse_constant_motion([](auto& r) {
        r.temporal->zero_motion = true;
    }, "zero-motion assertion cannot be combined with a nonzero constant offset");
    refuse_constant_motion([](auto& r) {
        r.temporal->camera_cut = true;
    }, "camera cut cannot reuse a constant-offset history frame");
    refuse_temporal([](auto& r) {
        r.temporal->feedback = std::numeric_limits<float>::quiet_NaN();
    }, "nonfinite temporal feedback refused");
    refuse_temporal([](auto& r) { r.temporal->feedback = 1.01F; },
        "temporal feedback above one refused");
    refuse_temporal([](auto& r) {
        r.temporal->history_source_digest = hash("stale-foreign-scene");
    }, "history from a stale or foreign scene source refused");
    refuse_temporal([](auto& r) {
        r.temporal->resolved_color = r.temporal->history_color;
    }, "temporal output aliasing history refused");
    refuse_temporal([](auto& r) { r.temporal->camera_cut = true; },
        "camera-cut reset cannot silently carry old history");
    refused([](auto& r) {
        r.temporal = d3d12::SceneRenderTemporalInputs{};
        r.temporal->resolved_color = {3U, 1U};
        r.temporal->motion_vectors = {2U, 1U};
        r.temporal->motion_vectors_digest = hash("orphan-motion");
    }, "motion without valid history refused");
    refused([](auto& r) {
        r.temporal = d3d12::SceneRenderTemporalInputs{};
        r.temporal->resolved_color = {3U, 1U};
        r.temporal->constant_motion_pixels = core::Vec2d{0.25, 0.0};
    }, "constant motion without valid history refused");
    refused([](auto& r) {
        r.temporal = d3d12::SceneRenderTemporalInputs{};
        r.temporal->resolved_color = {3U, 1U};
        r.temporal->history_color = {1U, 1U};
        r.temporal->history_content_digest = hash("history");
        r.temporal->history_request_digest = hash("prior-request");
        r.temporal->history_source_revision = r.source_revision;
        r.temporal->history_source_digest = r.source_digest;
        r.temporal->motion_vectors = {2U, 1U};
        r.temporal->motion_vectors_digest = hash("motion");
        r.temporal->motion_history_request_digest = hash("different-prior-request");
    }, "motion field bound to different history frame refused");
    refused([](auto& r) { r.source_digest = {}; }, "missing source digest refused");
    refused([](auto& r) { r.source_revision = core::Revision{8U}; }, "stale project refused");
    refused([](auto& r) { r.objects[0U].mesh.source_digest = hash("foreign-project"); }, "foreign project mesh refused");
    refused([](auto& r) { r.objects.push_back(r.objects[0U]); }, "duplicate object identity refused");
    refused([](auto& r) { r.objects[0U].mesh.vertices[0U].position.x += 0.1; }, "changed mesh content refused");
    refused([](auto& r) { r.objects[0U].materials[0U].current_material.definition.roughness = 0.2; }, "same-revision material edit refused");
    refused([](auto& r) { r.objects[0U].materials[0U].source_identity = "foreign/material"; }, "material source identity swap refused");
    refused([](auto& r) { r.objects[0U].materials.clear(); }, "missing material refused");
    refused([](auto& r) { r.objects[0U].world_transform.scale.x = 0.0; }, "singular transform refused");
    refused([](auto& r) { r.objects[0U].world_transform.scale.z = 1.0e-30; }, "unstable transform refused");
    refused([](auto& r) { r.objects[0U].world_transform.translation = {1.0e12, 1.0e12, 1.0e12}; }, "float32 world geometry collapse refused");
    refused([](auto& r) { r.camera.target = r.camera.position; }, "zero view direction refused");
    refused([](auto& r) { r.camera.up = {0.0, 0.0, 1.0}; }, "parallel camera up refused");
    refused([](auto& r) { r.camera.near_plane = r.camera.far_plane; }, "invalid clip range refused");
    refused([](auto& r) {
        r.camera.projection = static_cast<d3d12::SceneRenderProjection>(2U);
    }, "unknown scene camera projection refused");
    refused([](auto& r) {
        r.camera.projection = d3d12::SceneRenderProjection::orthographic;
        r.camera.orthographic_height = 0.0;
    }, "zero orthographic vertical extent refused");
    refused([](auto& r) { r.camera.exposure_ev = std::numeric_limits<double>::quiet_NaN(); }, "nonfinite exposure refused");
    refused([](auto& r) { r.width = 16384U; r.height = 16384U; }, "pixel allocation budget refused");
    auto eight_k = valid;
    eight_k.width = 8192U;
    eight_k.height = 4320U;
    require(static_cast<bool>(eight_k.validate()) &&
            static_cast<bool>(eight_k.digest()),
        "bounded 8K UHD scene request admits without allocating render targets");
    auto over_eight_k = eight_k;
    over_eight_k.height = 4321U;
    require(!over_eight_k.validate() && !over_eight_k.digest(),
        "scene request above the 8K UHD pixel bound refuses");
    auto over_dimension = valid;
    over_dimension.width = d3d12::kSceneRenderMaximumDimension + 1U;
    require(!over_dimension.validate() && !over_dimension.digest(),
        "scene request above the 8K axis bound refuses");
    refused([](auto& r) { r.light.direction_to_light = {}; }, "zero light direction refused");
    refused([](auto& r) { r.light.radiance.x = -1.0; }, "negative light radiance refused");
    auto local_point = valid;
    local_point.local_lights.push_back({scene::ObjectId{77U},
        d3d12::SceneRenderLocalLightKind::point, {0.0, 0.0, 1.0}, {},
        {1.0, 1.0, 1.0}, 10.0, 0.0, 0.0});
    require(static_cast<bool>(local_point.validate()),
        "bounded point-light request remains valid");
    auto shadowed_spot_request = local_point;
    auto& shadowed_spot_input = shadowed_spot_request.local_lights.front();
    shadowed_spot_input.kind = d3d12::SceneRenderLocalLightKind::spot;
    shadowed_spot_input.emission_direction = {0.0, 0.0, -1.0};
    shadowed_spot_input.inner_cone_radians = 0.2;
    shadowed_spot_input.outer_cone_radians = 0.5;
    shadowed_spot_input.range = 5.0;
    shadowed_spot_input.casts_shadows = true;
    shadowed_spot_request.light.radiance = {};
    require(static_cast<bool>(shadowed_spot_request.validate()) &&
            take(shadowed_spot_request.digest()) != take(local_point.digest()),
        "one bounded spotlight shadow is admitted and participates in capture identity");
    auto point_shadow = local_point;
    point_shadow.local_lights.front().casts_shadows = true;
    point_shadow.light.radiance = {};
    require(!point_shadow.validate() && !point_shadow.digest(),
        "point-light shadow intent refuses instead of borrowing a spotlight projection");
    auto key_and_spot_shadow = shadowed_spot_request;
    key_and_spot_shadow.light.radiance = {1.0, 0.0, 0.0};
    require(!key_and_spot_shadow.validate() && !key_and_spot_shadow.digest(),
        "one shadow map refuses simultaneous key-light and spotlight shadowing");
    auto multiple_spot_shadows = shadowed_spot_request;
    auto second_spot_shadow = multiple_spot_shadows.local_lights.front();
    second_spot_shadow.object = {78U};
    multiple_spot_shadows.local_lights.push_back(second_spot_shadow);
    require(!multiple_spot_shadows.validate() && !multiple_spot_shadows.digest(),
        "more than one shadow-casting spotlight refuses before rendering");
    auto short_spot_shadow = shadowed_spot_request;
    short_spot_shadow.local_lights.front().range = 0.015;
    require(!short_spot_shadow.validate() && !short_spot_shadow.digest(),
        "spot shadow range below the stable near-plane bound refuses");
    auto narrow_cone_spot_shadow = shadowed_spot_request;
    narrow_cone_spot_shadow.local_lights.front().inner_cone_radians = 0.0;
    narrow_cone_spot_shadow.local_lights.front().outer_cone_radians = 0.005;
    require(!narrow_cone_spot_shadow.validate() && !narrow_cone_spot_shadow.digest(),
        "spot shadow projection below the minimum cone bound refuses");
    const auto refused_local = [&](auto mutate, std::string_view label) {
        auto bad = local_point;
        mutate(bad);
        require(!bad.validate(), label);
        require(!bad.digest(), label);
    };
    refused_local([](auto& r) { r.local_lights[0U].object = {}; },
        "local light without stable source identity refused");
    refused_local([](auto& r) {
        r.local_lights[0U].kind =
            static_cast<d3d12::SceneRenderLocalLightKind>(2U);
    }, "unknown local-light kind refused");
    refused_local([](auto& r) {
        r.local_lights[0U].position.x = std::numeric_limits<double>::max();
    }, "local-light position that overflows float32 refused");
    refused_local([](auto& r) { r.local_lights[0U].range = 0.0; },
        "zero-range local light refused");
    refused_local([](auto& r) { r.local_lights[0U].range = 1.0e6 + 1.0; },
        "out-of-bound local-light range refused");
    refused_local([](auto& r) { r.local_lights[0U].radiance.x = -1.0; },
        "negative local-light radiance refused");
    refused_local([](auto& r) {
        r.local_lights[0U].radiance.x = std::numeric_limits<double>::max();
    }, "local-light radiance that overflows float32 refused");
    refused_local([](auto& r) {
        r.local_lights[0U].radiance.x =
            d3d12::kSceneRenderMaximumLocalLightIntensity * 2.0;
    }, "local-light intensity above the inverse-square HDR bound refused");
    refused_local([](auto& r) {
        r.local_lights[0U].kind = d3d12::SceneRenderLocalLightKind::spot;
        r.local_lights[0U].inner_cone_radians = 0.2;
        r.local_lights[0U].outer_cone_radians = 0.5;
        r.local_lights[0U].emission_direction = {};
    }, "zero spot direction refused");
    refused_local([](auto& r) {
        r.local_lights[0U].kind = d3d12::SceneRenderLocalLightKind::spot;
        r.local_lights[0U].inner_cone_radians = 0.6;
        r.local_lights[0U].outer_cone_radians = 0.5;
    }, "reversed spot cone refused");
    refused_local([](auto& r) {
        r.local_lights[0U].kind = d3d12::SceneRenderLocalLightKind::spot;
        r.local_lights[0U].inner_cone_radians = 0.2;
        r.local_lights[0U].outer_cone_radians = 0.5;
        r.local_lights[0U].emission_direction.x =
            std::numeric_limits<double>::infinity();
    }, "nonfinite spot direction refused");
    refused_local([](auto& r) {
        r.local_lights[0U].inner_cone_radians = 0.1;
    }, "point light carrying a spot cone refused");
    auto large_direction_spot = local_point;
    large_direction_spot.local_lights[0U].kind =
        d3d12::SceneRenderLocalLightKind::spot;
    large_direction_spot.local_lights[0U].emission_direction =
        {0.0, 0.0, -1.0e300};
    large_direction_spot.local_lights[0U].inner_cone_radians = 0.2;
    large_direction_spot.local_lights[0U].outer_cone_radians = 0.5;
    require(static_cast<bool>(large_direction_spot.validate()),
        "finite spot direction magnitude is canonicalized for the float GPU table");
    auto duplicate_local_sources = local_point;
    duplicate_local_sources.local_lights.push_back(
        duplicate_local_sources.local_lights.front());
    require(!duplicate_local_sources.validate(),
        "duplicate local-light source identity refused");
    auto too_many_local_lights = valid;
    for (std::uint64_t index = 0U; index < 65U; ++index) {
        auto light = local_point.local_lights.front();
        light.object = scene::ObjectId{100U + index};
        too_many_local_lights.local_lights.push_back(light);
    }
    require(!too_many_local_lights.validate() &&
            !too_many_local_lights.digest(),
        "local-light table beyond the 64-source bound refused");
    auto vertex_colored = valid;
    for (auto& vertex : vertex_colored.objects[0U].mesh.vertices)
        vertex.color = core::Vec4d{0.2, 1.0, 0.3, 1.0};
    rebind(vertex_colored.objects[0U]);
    require(static_cast<bool>(vertex_colored.validate()) &&
        take(vertex_colored.digest()) != take(valid.digest()),
        "authored vertex colors admit and participate in request identity");
    auto blended_material = valid;
    blended_material.objects[0U].materials[0U].current_material.definition.alpha_mode =
        render::AlphaMode::blended;
    rebind(blended_material.objects[0U]);
    require(static_cast<bool>(blended_material.validate()) &&
        take(blended_material.digest()) != take(valid.digest()),
        "straight-alpha material mode is admitted and request-identity bound");
    constexpr std::uint64_t id = (1ULL << 40U) + 13U;
    auto textured = valid;
    textured.objects[0U].materials[0U].current_material.definition.base_color.asset = id;
    rebind(textured.objects[0U]);
    require(!textured.validate(), "bound texture missing fails closed, no previous/default image");
    textured.textures.push_back(texture(id, {64U, 128U, 255U, 255U}));
    require(static_cast<bool>(textured.validate()), "full-width 64-bit texture identity admits");
    auto transformed = textured;
    auto& transformed_binding = transformed.objects[0U].materials[0U].current_material.definition.base_color;
    transformed_binding.transform.scale = {0.5, 0.75};
    transformed_binding.transform.offset = {0.125, -0.25};
    rebind(transformed.objects[0U]);
    require(static_cast<bool>(transformed.validate()) &&
        take(transformed.digest()) != take(textured.digest()),
        "supported affine texture transforms validate and participate in request identity");
    auto secondary_uv = textured;
    secondary_uv.objects[0U].uv1_set = 2U;
    for (auto& vertex : secondary_uv.objects[0U].mesh.vertices) {
        vertex.uv1 = core::Vec2d{vertex.uv->x * 2.0, 1.0 - vertex.uv->y};
    }
    secondary_uv.objects[0U].materials[0U].current_material.definition
        .base_color.uv_set = 2U;
    rebind(secondary_uv.objects[0U]);
    require(static_cast<bool>(secondary_uv.validate()) &&
        take(secondary_uv.digest()) != take(textured.digest()),
        "an explicitly mapped secondary UV stream is admitted and identity-bound");
    auto tertiary_uv = textured;
    tertiary_uv.objects[0U].mesh.additional_uv_set_ids = {3U};
    for (auto& vertex : tertiary_uv.objects[0U].mesh.vertices) {
        vertex.additional_uvs = {core::Vec2d{1.0 - vertex.uv->y,
            vertex.uv->x * 0.5}};
    }
    tertiary_uv.objects[0U].materials[0U].current_material.definition
        .base_color.uv_set = 3U;
    rebind(tertiary_uv.objects[0U]);
    require(static_cast<bool>(tertiary_uv.validate()) &&
        take(tertiary_uv.digest()) != take(textured.digest()),
        "an identity-bound additional UV stream is admitted beyond UV1");
    auto secondary_normal_uv = secondary_uv;
    auto& secondary_normal_material = secondary_normal_uv.objects[0U].materials[0U]
        .current_material.definition;
    secondary_normal_material.normal.asset = id + 1U;
    secondary_normal_material.normal.uv_set = 2U;
    secondary_normal_uv.textures.push_back(texture(id + 1U,
        {128U, 128U, 255U, 255U}, assets::TextureSemantic::normal));
    rebind(secondary_normal_uv.objects[0U]);
    const auto secondary_normal_refusal = secondary_normal_uv.validate();
    require(!secondary_normal_refusal &&
        secondary_normal_refusal.error().code == core::ErrorCode::unsupported &&
        secondary_normal_refusal.error().message.find("primary tangent UV") !=
            std::string::npos,
        "a normal map cannot use a secondary UV stream with primary-stream tangents");
    auto sampled = textured;
    sampled.objects[0U].materials[0U].current_material.definition.base_color.sampler = 7U;
    rebind(sampled.objects[0U]);
    gpu::SamplerDesc repeat_sampler;
    repeat_sampler.min_filter = gpu::Filter::nearest;
    repeat_sampler.mag_filter = gpu::Filter::nearest;
    repeat_sampler.mip_filter = gpu::MipFilter::nearest;
    repeat_sampler.u = gpu::AddressMode::repeat;
    repeat_sampler.v = gpu::AddressMode::repeat;
    sampled.samplers.emplace(7U, repeat_sampler);
    require(static_cast<bool>(sampled.validate()) &&
        take(sampled.digest()) != take(textured.digest()),
        "authored sampler descriptors validate and participate in request identity");
    auto missing_sampler = sampled;
    missing_sampler.samplers.clear();
    require(!missing_sampler.validate(),
        "bound non-default sampler without a current descriptor refuses");
    auto unused_sampler = sampled;
    unused_sampler.samplers.emplace(8U, gpu::SamplerDesc{});
    require(!unused_sampler.validate(),
        "sampler descriptors unused by visible material draws refuse");
    auto comparison_sampler = sampled;
    comparison_sampler.samplers.at(7U).compare = true;
    require(!comparison_sampler.validate(),
        "comparison samplers refuse when the PBR shader only performs ordinary samples");
    auto invalid_sampler_enum = sampled;
    invalid_sampler_enum.samplers.at(7U).mag_filter =
        static_cast<gpu::Filter>(255U);
    require(!invalid_sampler_enum.validate(),
        "unknown native sampler enum values refuse before request identity or GPU submission");
    auto per_slot_samplers = sampled;
    auto& per_slot_material =
        per_slot_samplers.objects[0U].materials[0U].current_material.definition;
    per_slot_material.metallic_roughness.asset = id + 1U;
    per_slot_material.metallic_roughness.sampler = 8U;
    per_slot_samplers.textures.push_back(texture(id + 1U,
        {255U, 128U, 64U, 255U}, assets::TextureSemantic::metallic_roughness));
    per_slot_samplers.samplers.emplace(8U, repeat_sampler);
    rebind(per_slot_samplers.objects[0U]);
    require(static_cast<bool>(per_slot_samplers.validate()),
        "one material draw admits independent current samplers for texture slots");
    auto missing_second_slot_sampler = per_slot_samplers;
    missing_second_slot_sampler.samplers.erase(8U);
    require(!missing_second_slot_sampler.validate(),
        "a missing descriptor for a non-base-color texture slot refuses");
    auto swapped_samplers = per_slot_samplers;
    swapped_samplers.objects[0U].materials[0U].current_material.definition
        .base_color.sampler = 8U;
    swapped_samplers.objects[0U].materials[0U].current_material.definition
        .metallic_roughness.sampler = 7U;
    rebind(swapped_samplers.objects[0U]);
    require(static_cast<bool>(swapped_samplers.validate()) &&
        take(swapped_samplers.digest()) != take(per_slot_samplers.digest()),
        "sampler-to-slot assignments participate in request identity");
    auto rotated = transformed;
    rotated.objects[0U].materials[0U].current_material.definition.base_color.transform.rotation = 0.1;
    rotated.objects[0U].materials[0U].current_material.definition.base_color.transform.pivot = {0.25, 0.75};
    rebind(rotated.objects[0U]);
    require(static_cast<bool>(rotated.validate()) &&
        take(rotated.digest()) != take(transformed.digest()),
        "rotated pivoted affine transforms validate and participate in request identity");
    auto overflowing_uv = transformed;
    overflowing_uv.objects[0U].materials[0U].current_material.definition.base_color.transform.scale.x = 1.0e300;
    rebind(overflowing_uv.objects[0U]);
    const auto overflow_refusal = overflowing_uv.validate();
    require(!overflow_refusal && overflow_refusal.error().code == core::ErrorCode::validation_failed &&
        overflow_refusal.error().message.find("float32") != std::string::npos,
        "UV transform values that cannot be represented in shader float32 are refused");
    auto conflicting_uv = transformed;
    auto& conflicting_material = conflicting_uv.objects[0U].materials[0U].current_material.definition;
    conflicting_material.metallic_roughness.asset = id + 1U;
    conflicting_material.metallic_roughness.transform.offset.x += 0.25;
    rebind(conflicting_uv.objects[0U]);
    conflicting_uv.textures.push_back(texture(id + 1U,
        {180U, 90U, 30U, 255U}, assets::TextureSemantic::metallic_roughness));
    require(static_cast<bool>(conflicting_uv.validate()) &&
        take(conflicting_uv.digest()) != take(transformed.digest()),
        "bound material slots preserve independent source-bound UV transforms");
    auto bad = textured; bad.textures[0U].current_source_digest = hash("replaced-source");
    require(!bad.validate(), "valid old mip chain refused for new source");
    bad = textured; bad.textures[0U].mips.levels[0U].pixels[0U] ^= 1U;
    require(!bad.validate(), "tampered mip pixels refused");
    bad = textured; bad.textures.push_back(bad.textures[0U]);
    require(!bad.validate(), "duplicate texture identity refused");
    bad = textured; bad.textures[0U].asset = 13U;
    require(!bad.validate(), "texture low-word alias refused");
    bad = textured; bad.textures[0U] = texture(id, {128U, 128U, 255U, 255U}, assets::TextureSemantic::normal);
    require(!bad.validate(), "wrong texture slot semantic refused");
    bad = valid; bad.textures = textured.textures;
    require(!bad.validate(), "unused texture input refused");
    auto moved = valid; moved.camera.position.x = 0.2;
    require(take(moved.digest()) != take(valid.digest()), "camera participates in request identity");
    auto orthographic = valid;
    orthographic.camera.projection = d3d12::SceneRenderProjection::orthographic;
    orthographic.camera.orthographic_height = 2.0;
    require(static_cast<bool>(orthographic.validate()) &&
            take(orthographic.digest()) != take(valid.digest()),
        "orthographic projection and vertical extent participate in request identity");
    auto changed_orthographic_height = orthographic;
    changed_orthographic_height.camera.orthographic_height = 3.0;
    require(take(changed_orthographic_height.digest()) !=
            take(orthographic.digest()),
        "orthographic extent changes invalidate request identity");
    auto overbound_orthographic = orthographic;
    overbound_orthographic.camera.orthographic_height = 1.0e8 + 1.0;
    require(!overbound_orthographic.validate() &&
            !overbound_orthographic.digest(),
        "orthographic vertical extent above renderer bounds refuses");
    auto unused_perspective_extent = valid;
    unused_perspective_extent.camera.orthographic_height = 1.0e9;
    require(static_cast<bool>(unused_perspective_extent.validate()) &&
            static_cast<bool>(unused_perspective_extent.digest()),
        "perspective projection ignores the unused orthographic extent bound");
    auto with_environment = valid;
    d3d12::SceneRenderEnvironment environment;
    environment.source_digest = hash("fixture diffuse irradiance SH");
    environment.diffuse_irradiance_over_pi_sh[0U] = {1.772453850905516, 0.886226925452758, 0.0};
    with_environment.environment = environment;
    require(static_cast<bool>(with_environment.validate()) &&
        take(with_environment.digest()) != take(valid.digest()),
        "explicit diffuse IBL coefficients participate in request identity");
    with_environment.environment->source_digest = {};
    require(!with_environment.validate(), "missing environment identity fails closed");
}

void temporal_sampling_is_deterministic_and_memory_bounded() {
    require(d3d12::temporal_sample_limit == 16U,
        "temporal sample limit remains bounded");
    const auto first = d3d12::temporal_sample_jitter(0U);
    require(first.x == 0.0 && first.y == 0.0,
        "first temporal sample keeps the unjittered projection");
    const auto next = d3d12::temporal_sample_jitter(1U);
    const auto history_offset = d3d12::temporal_history_offset(first, next);
    require(history_offset.x == first.x - next.x &&
            history_offset.y == first.y - next.y,
        "history reprojection is the previous-minus-current sample offset");
    auto previous = first;
    for (std::uint32_t sample = 1U; sample < d3d12::temporal_sample_limit; ++sample) {
        const auto jitter = d3d12::temporal_sample_jitter(sample);
        const auto repeated = d3d12::temporal_sample_jitter(sample);
        require(jitter.finite() && std::abs(jitter.x) <= 0.5 &&
                std::abs(jitter.y) <= 0.5 &&
                jitter.x == repeated.x && jitter.y == repeated.y,
            "temporal jitter is finite, bounded, and deterministic");
        require(jitter.x != previous.x || jitter.y != previous.y,
            "adjacent Halton samples do not repeat");
        previous = jitter;
    }
    require(d3d12::temporal_history_textures_fit_budget(8192U, 1024U),
        "two RGBA32F history textures admit at the exact memory budget");
    require(!d3d12::temporal_history_textures_fit_budget(8192U, 1025U) &&
            !d3d12::temporal_history_textures_fit_budget(0U, 1U) &&
            !d3d12::temporal_history_textures_fit_budget(1U, 0U),
        "over-budget and empty temporal extents refuse");
}
d3d12::SceneRenderImage capture(d3d12::D3D12Device& device, const d3d12::SceneRenderRequest& input) {
    return take(d3d12::render_scene(device, input, CARTO_D3D12_SHADER_DIR));
}
void scene_render_uses_independent_material_slot_samplers(
    d3d12::D3D12Device& device) {
    auto input = request();
    constexpr assets::TextureAssetId base_color_id = 0x1'0000'0002ULL;
    constexpr assets::TextureAssetId metallic_roughness_id = 0x1'0000'0003ULL;
    auto& material = input.objects[0U].materials[0U].current_material.definition;
    material.base_color.asset = base_color_id;
    material.base_color.sampler = 7U;
    material.base_color.transform.offset = {0.75, 0.0};
    material.metallic_roughness.asset = metallic_roughness_id;
    material.metallic_roughness.sampler = 8U;
    material.metallic_roughness.transform.offset = {0.75, 0.0};
    input.textures.push_back(texture_grid(base_color_id));
    input.textures.push_back(texture_grid(metallic_roughness_id,
        assets::TextureSemantic::metallic_roughness));
    gpu::SamplerDesc repeat;
    repeat.u = gpu::AddressMode::repeat;
    repeat.v = gpu::AddressMode::repeat;
    gpu::SamplerDesc clamp;
    clamp.u = gpu::AddressMode::clamp_edge;
    clamp.v = gpu::AddressMode::clamp_edge;
    input.samplers.emplace(7U, repeat);
    input.samplers.emplace(8U, clamp);
    rebind(input.objects[0U]);
    require(static_cast<bool>(input.validate()),
        "distinct current samplers admit on base-color and metallic/roughness slots");

    const auto direct = capture(device, input);
    d3d12::SceneRenderCache cache;
    const auto cached = take(d3d12::render_scene_cached(
        device, cache, input, CARTO_D3D12_SHADER_DIR));
    require(cached.pixel_digest == direct.pixel_digest &&
        cached.linear_hdr_rgba == direct.linear_hdr_rgba,
        "cached per-slot sampler binding matches the fresh render");

    auto metallic_repeat = input;
    metallic_repeat.samplers.at(8U).u = gpu::AddressMode::repeat;
    metallic_repeat.samplers.at(8U).v = gpu::AddressMode::repeat;
    const auto repeat_pixels = capture(device, metallic_repeat);
    require(repeat_pixels.pixel_digest != direct.pixel_digest,
        "changing only metallic/roughness addressing changes rendered pixels");
}
void scene_render_cache_reuses_programs_meshes_materials_and_targets(
    d3d12::D3D12Device& device) {
    d3d12::SceneRenderCache cache;
    const auto input = request();
    require(cache.generation() == 0U, "new scene cache has no program generation");
    const auto first = take(d3d12::render_scene_cached(
        device, cache, input, CARTO_D3D12_SHADER_DIR));
    require(cache.generation() == 1U,
        "first cached render creates one persistent scene program set");
    const auto repeated = take(d3d12::render_scene_cached(
        device, cache, input, CARTO_D3D12_SHADER_DIR));
    require(cache.generation() == 1U &&
            repeated.shader_digests == first.shader_digests &&
            repeated.pixel_digest == first.pixel_digest &&
            repeated.linear_hdr_rgba == first.linear_hdr_rgba &&
            repeated.depth == first.depth,
        "reused scene targets preserve display, HDR, and depth pixels");
    const std::uint64_t graphics_timestamp_frequency =
        device.timestamp_frequency(gpu::QueueType::graphics);
    if (graphics_timestamp_frequency != 0U) {
        const auto measured = take(d3d12::render_scene_cached(
            device, cache, input, CARTO_D3D12_SHADER_DIR, true));
        require(measured.gpu_timing.has_value() &&
                measured.gpu_timing->timestamp_frequency_hz ==
                    graphics_timestamp_frequency &&
                cache.generation() == 1U &&
                measured.request_digest == repeated.request_digest &&
                measured.pixel_digest == repeated.pixel_digest,
            "cached timing captures graphics timestamps without changing scene identity or pixels");
    }
    auto camera_changed = input;
    camera_changed.camera.position.x += 0.25;
    const auto cached_view = take(d3d12::render_scene_cached(
        device, cache, camera_changed, CARTO_D3D12_SHADER_DIR));
    const auto reference_view = capture(device, camera_changed);
    require(cache.generation() == 1U &&
        cached_view.pixel_digest == reference_view.pixel_digest &&
        cached_view.linear_hdr_rgba == reference_view.linear_hdr_rgba &&
        cached_view.depth == reference_view.depth,
        "camera changes reuse targets and match a fresh display/HDR/depth render");
    auto resized = input;
    ++resized.width;
    ++resized.height;
    const auto cached_resize = take(d3d12::render_scene_cached(
        device, cache, resized, CARTO_D3D12_SHADER_DIR));
    const auto reference_resize = capture(device, resized);
    require(cache.generation() == 1U &&
        cached_resize.pixel_digest == reference_resize.pixel_digest &&
        cached_resize.linear_hdr_rgba == reference_resize.linear_hdr_rgba &&
        cached_resize.depth == reference_resize.depth,
        "extent changes replace cached targets and match a fresh display/HDR/depth render");
    auto transform_changed = input;
    transform_changed.objects.front().world_transform.translation.x += 0.2;
    const auto cached_transform = take(d3d12::render_scene_cached(
        device, cache, transform_changed, CARTO_D3D12_SHADER_DIR));
    const auto reference_transform = capture(device, transform_changed);
    require(cache.generation() == 1U &&
        cached_transform.pixel_digest == reference_transform.pixel_digest,
        "world-transform changes rebuild cached mesh inputs and match a fresh render");
    auto light_changed = input;
    light_changed.light.direction_to_light = {0.2, 0.4, 1.0};
    const auto cached_light = take(d3d12::render_scene_cached(
        device, cache, light_changed, CARTO_D3D12_SHADER_DIR));
    const auto reference_light = capture(device, light_changed);
    require(cache.generation() == 1U &&
        cached_light.pixel_digest == reference_light.pixel_digest,
        "light changes rebuild shadow-coordinate mesh inputs and match a fresh render");
    auto spot_shadow = input;
    spot_shadow.light.radiance = {};
    d3d12::SceneRenderLocalLight spot_shadow_source;
    spot_shadow_source.object = {77U};
    spot_shadow_source.kind = d3d12::SceneRenderLocalLightKind::spot;
    spot_shadow_source.position = {0.0, 0.0, 2.0};
    spot_shadow_source.emission_direction = {0.0, 0.0, -1.0};
    spot_shadow_source.range = 5.0;
    spot_shadow_source.inner_cone_radians = 0.2;
    spot_shadow_source.outer_cone_radians = 0.5;
    spot_shadow_source.casts_shadows = true;
    spot_shadow.local_lights = {spot_shadow_source};
    const auto cached_spot_shadow = take(d3d12::render_scene_cached(
        device, cache, spot_shadow, CARTO_D3D12_SHADER_DIR));
    auto moved_spot_shadow = spot_shadow;
    moved_spot_shadow.local_lights.front().position.x = 0.25;
    const auto cached_moved_spot = take(d3d12::render_scene_cached(
        device, cache, moved_spot_shadow, CARTO_D3D12_SHADER_DIR));
    const auto direct_moved_spot = capture(device, moved_spot_shadow);
    require(cached_moved_spot.request_digest != cached_spot_shadow.request_digest &&
        cached_moved_spot.pixel_digest == direct_moved_spot.pixel_digest &&
        cached_moved_spot.linear_hdr_rgba == direct_moved_spot.linear_hdr_rgba,
        "moving a shadow-casting spotlight invalidates cached projected vertices and matches a fresh render");
    constexpr std::uint64_t texture_id = (1ULL << 40U) + 13U;
    auto textured = input;
    textured.objects.front().materials.front().current_material.definition.base_color.asset =
        texture_id;
    rebind(textured.objects.front());
    textured.textures.push_back(texture_grid(texture_id));
    const auto cached_texture = take(d3d12::render_scene_cached(
        device, cache, textured, CARTO_D3D12_SHADER_DIR));
    const auto reference_texture = capture(device, textured);
    require(cache.generation() == 1U &&
        cached_texture.pixel_digest == reference_texture.pixel_digest,
        "source-bound material texture uploads match a fresh render");
    auto changed_texture = textured;
    changed_texture.textures[0U] = texture(texture_id, {220U, 30U, 40U, 255U});
    const auto cached_changed_texture = take(d3d12::render_scene_cached(
        device, cache, changed_texture, CARTO_D3D12_SHADER_DIR));
    const auto reference_changed_texture = capture(device, changed_texture);
    require(cache.generation() == 1U &&
        cached_changed_texture.pixel_digest == reference_changed_texture.pixel_digest,
        "changed source bytes rebuild material texture uploads and match a fresh render");
    auto transformed_texture = textured;
    transformed_texture.objects.front().materials.front().current_material.definition.base_color.transform.scale =
        {0.75, 0.5};
    rebind(transformed_texture.objects.front());
    const auto cached_uv_transform = take(d3d12::render_scene_cached(
        device, cache, transformed_texture, CARTO_D3D12_SHADER_DIR));
    const auto reference_uv_transform = capture(device, transformed_texture);
    require(cache.generation() == 1U &&
        cached_uv_transform.pixel_digest == reference_uv_transform.pixel_digest &&
        cached_uv_transform.request_digest == reference_uv_transform.request_digest &&
        cached_uv_transform.pixel_digest != cached_texture.pixel_digest,
        "per-texture UV streams change cached pixels and match a fresh render without rebuilding the pipeline");
    auto repeated_sampler = textured;
    auto& repeat_binding = repeated_sampler.objects.front().materials.front().
        current_material.definition.base_color;
    repeat_binding.sampler = 7U;
    repeat_binding.transform.offset.x = 1.25;
    rebind(repeated_sampler.objects.front());
    gpu::SamplerDesc repeat_descriptor;
    repeat_descriptor.min_filter = gpu::Filter::nearest;
    repeat_descriptor.mag_filter = gpu::Filter::nearest;
    repeat_descriptor.mip_filter = gpu::MipFilter::nearest;
    repeat_descriptor.u = gpu::AddressMode::repeat;
    repeat_descriptor.v = gpu::AddressMode::repeat;
    repeated_sampler.samplers.emplace(7U, repeat_descriptor);
    const auto cached_repeat_sampler = take(d3d12::render_scene_cached(
        device, cache, repeated_sampler, CARTO_D3D12_SHADER_DIR));
    const auto reference_repeat_sampler = capture(device, repeated_sampler);
    auto clamped_sampler = repeated_sampler;
    clamped_sampler.samplers.at(7U).u = gpu::AddressMode::clamp_edge;
    clamped_sampler.samplers.at(7U).v = gpu::AddressMode::clamp_edge;
    const auto cached_clamp_sampler = take(d3d12::render_scene_cached(
        device, cache, clamped_sampler, CARTO_D3D12_SHADER_DIR));
    const auto reference_clamp_sampler = capture(device, clamped_sampler);
    require(cache.generation() == 1U &&
        cached_repeat_sampler.pixel_digest == reference_repeat_sampler.pixel_digest &&
        cached_clamp_sampler.pixel_digest == reference_clamp_sampler.pixel_digest &&
        cached_repeat_sampler.pixel_digest != cached_clamp_sampler.pixel_digest &&
        cached_repeat_sampler.request_digest != cached_clamp_sampler.request_digest,
        "authored repeat/clamp samplers change out-of-range UV pixels and cached outputs match fresh renders");
    constexpr std::uint64_t normal_texture_id = texture_id + 1U;
    auto directx_normal = input;
    directx_normal.objects.front().materials.front().current_material.definition.normal.asset =
        normal_texture_id;
    rebind(directx_normal.objects.front());
    directx_normal.textures.push_back(texture_grid(normal_texture_id,
        assets::TextureSemantic::normal));
    const auto cached_directx_normal = take(d3d12::render_scene_cached(
        device, cache, directx_normal, CARTO_D3D12_SHADER_DIR));
    auto open_gl_normal = directx_normal;
    open_gl_normal.objects.front().materials.front().current_material.definition.normal.
        normal_convention = render::NormalConvention::opengl_y_plus;
    rebind(open_gl_normal.objects.front());
    const auto cached_open_gl_normal = take(d3d12::render_scene_cached(
        device, cache, open_gl_normal, CARTO_D3D12_SHADER_DIR));
    const auto reference_open_gl_normal = capture(device, open_gl_normal);
    require(cache.generation() == 1U &&
        cached_open_gl_normal.pixel_digest != cached_directx_normal.pixel_digest &&
        cached_open_gl_normal.pixel_digest == reference_open_gl_normal.pixel_digest &&
        cached_open_gl_normal.request_digest == reference_open_gl_normal.request_digest,
        "cached normal-map convention changes invalidate tangent streams and match a fresh render");
    auto environment_changed = changed_texture;
    d3d12::SceneRenderEnvironment environment;
    environment.source_digest = hash("cached scene environment");
    environment.diffuse_irradiance_over_pi_sh[0U] = {1.0, 1.0, 1.0};
    environment_changed.environment = environment;
    const auto cached_environment = take(d3d12::render_scene_cached(
        device, cache, environment_changed, CARTO_D3D12_SHADER_DIR));
    const auto reference_environment = capture(device, environment_changed);
    require(cache.generation() == 1U &&
        cached_environment.pixel_digest == reference_environment.pixel_digest,
        "changed diffuse SH inputs rebuild their cached texture and match a fresh render");
    cache.reset();
    require(cache.generation() == 0U,
        "reset retires cached program, mesh, material, and target resources before the device");
}

void native_sampler_min_mag_filters_remain_independent(
    d3d12::D3D12Device& device) {
    constexpr std::uint64_t texture_identity = 901U;
    const auto capture_with_filters = [&](gpu::Filter minification,
                                          gpu::Filter magnification) {
        auto input = request();
        input.objects.front() = object(1U, {1.0, 1.0, 1.0, 1.0});
        auto& binding = input.objects.front().materials.front().
            current_material.definition.base_color;
        binding.asset = texture_identity;
        binding.sampler = 7U;
        rebind(input.objects.front());
        input.textures.push_back(texture_grid(texture_identity));
        gpu::SamplerDesc descriptor;
        descriptor.min_filter = minification;
        descriptor.mag_filter = magnification;
        descriptor.mip_filter = gpu::MipFilter::nearest;
        input.samplers.emplace(7U, descriptor);
        return capture(device, input);
    };
    const auto point = capture_with_filters(gpu::Filter::nearest,
        gpu::Filter::nearest);
    const auto linear_min = capture_with_filters(gpu::Filter::linear,
        gpu::Filter::nearest);
    const auto linear_mag = capture_with_filters(gpu::Filter::nearest,
        gpu::Filter::linear);
    require(linear_min.linear_hdr_rgba == point.linear_hdr_rgba &&
        linear_mag.linear_hdr_rgba != point.linear_hdr_rgba,
        "magnification and minification choices map to distinct D3D12 filter modes");
}

void native_material_channel_mappings_reach_shading(
    d3d12::D3D12Device& device) {
    using BindingSelector = render::TextureRef& (*)(render::StandardMaterial&);
    d3d12::SceneRenderCache cache;
    const auto verify_slot = [&](std::uint64_t texture_identity,
                                 assets::TextureSemantic semantic,
                                 BindingSelector select_binding,
                                 auto configure_material,
                                 std::array<render::TextureChannel, 4U> mapped_channels,
                                 std::string_view label) {
        auto identity_request = request();
        auto& identity_material = identity_request.objects.front().materials.front().
            current_material.definition;
        auto& identity_binding = select_binding(identity_material);
        identity_binding.asset = texture_identity;
        configure_material(identity_material);
        rebind(identity_request.objects.front());
        identity_request.textures.push_back(texture_grid(texture_identity, semantic));

        auto mapped_request = identity_request;
        auto& mapped_material = mapped_request.objects.front().materials.front().
            current_material.definition;
        select_binding(mapped_material).channels.rgba = mapped_channels;
        rebind(mapped_request.objects.front());

        const auto identity = take(d3d12::render_scene_cached(
            device, cache, identity_request, CARTO_D3D12_SHADER_DIR));
        const auto mapped = take(d3d12::render_scene_cached(
            device, cache, mapped_request, CARTO_D3D12_SHADER_DIR));
        const auto fresh = capture(device, mapped_request);
        require(cache.generation() == 1U &&
            mapped.request_digest != identity.request_digest &&
            mapped.linear_hdr_rgba != identity.linear_hdr_rgba &&
            mapped.pixel_digest != identity.pixel_digest &&
            mapped.linear_hdr_rgba == fresh.linear_hdr_rgba &&
            mapped.pixel_digest == fresh.pixel_digest,
            label);
    };

    verify_slot(913U, assets::TextureSemantic::base_color,
        +[](render::StandardMaterial& material) -> render::TextureRef& {
            return material.base_color;
        }, [](render::StandardMaterial&) {},
        {render::TextureChannel::green, render::TextureChannel::blue,
         render::TextureChannel::one_minus_red,
         render::TextureChannel::constant_one},
        "base-color remapping, inversion and constants reach cached native shading");
    verify_slot(914U, assets::TextureSemantic::metallic_roughness,
        +[](render::StandardMaterial& material) -> render::TextureRef& {
            return material.metallic_roughness;
        }, [](render::StandardMaterial& material) {
            material.metallic = 1.0;
            material.roughness = 1.0;
        },
        {render::TextureChannel::red, render::TextureChannel::blue,
         render::TextureChannel::green, render::TextureChannel::alpha},
        "metallic/roughness channel selection reaches cached native shading");
    verify_slot(915U, assets::TextureSemantic::normal,
        +[](render::StandardMaterial& material) -> render::TextureRef& {
            return material.normal;
        }, [](render::StandardMaterial& material) {
            material.normal_scale = 1.0;
        },
        {render::TextureChannel::blue, render::TextureChannel::red,
         render::TextureChannel::green, render::TextureChannel::alpha},
        "normal-map channel selection reaches cached native tangent-space shading");
    verify_slot(916U, assets::TextureSemantic::occlusion,
        +[](render::StandardMaterial& material) -> render::TextureRef& {
            return material.occlusion;
        }, [](render::StandardMaterial& material) {
            material.occlusion_strength = 1.0;
        },
        {render::TextureChannel::green, render::TextureChannel::red,
         render::TextureChannel::blue, render::TextureChannel::alpha},
        "occlusion channel selection reaches cached native shading");
    verify_slot(917U, assets::TextureSemantic::emissive,
        +[](render::StandardMaterial& material) -> render::TextureRef& {
            return material.emissive;
        }, [](render::StandardMaterial& material) {
            material.emissive_factor = {1.0, 1.0, 1.0};
            material.emissive_strength = 1.0;
        },
        {render::TextureChannel::green, render::TextureChannel::blue,
         render::TextureChannel::red, render::TextureChannel::alpha},
        "emissive channel selection reaches cached native shading");
}

void orthographic_projection_preserves_scale_under_depth_translation(
    d3d12::D3D12Device& device) {
    const auto coverage = [&](d3d12::SceneRenderProjection projection,
                              double object_z) {
        auto input = request();
        input.camera.projection = projection;
        input.camera.orthographic_height = 2.0;
        input.objects.front().world_transform.translation.z = object_z;
        const auto image = capture(device, input);
        return static_cast<std::size_t>(std::count_if(
            image.depth.begin(), image.depth.end(), [](float depth) {
                return depth < 1.0F;
            }));
    };
    const auto orthographic_near = coverage(
        d3d12::SceneRenderProjection::orthographic, 0.0);
    const auto orthographic_far = coverage(
        d3d12::SceneRenderProjection::orthographic, -1.0);
    const auto perspective_near = coverage(
        d3d12::SceneRenderProjection::perspective, 0.0);
    const auto perspective_far = coverage(
        d3d12::SceneRenderProjection::perspective, -1.0);
    require(orthographic_near > 0U && orthographic_near == orthographic_far,
        "orthographic projection preserves object screen size across camera depth");
    require(perspective_near > perspective_far,
        "perspective projection continues to shrink objects with camera depth");
}

void scene_depth_exposure_material_camera_and_lifetimes(d3d12::D3D12Device& device) {
    auto input = request();
    input.objects[0U].world_transform.translation.z = 0.4;
    auto back = object(2U, {0.02, 0.7, 0.05, 1.0}); back.world_transform.scale = {1.5, 1.5, 1.5};
    input.objects.push_back(back);
    const auto source_before = take(input.objects[0U].mesh.serialize());
    const auto identity_before = take(input.digest());
    const auto sentinel = take(device.create_buffer({16U, gpu::MemoryClass::upload, gpu::BufferUsage::vertex}));
    const auto first = capture(device, input);
    const std::size_t center = (48U * 96U + 48U) * 4U;
    require(first.draw_count == 2U && first.depth.size() == 96U * 96U && first.srgb_rgba.size() == 96U * 96U * 4U,
            "actual two-object HDR/depth/display capture");
    require(first.linear_hdr_rgba[center] > first.linear_hdr_rgba[center + 1U] * 3.0F && first.depth[center / 4U] < 1.0F,
            "front material and depth win");
    require(first.request_digest == identity_before && first.source_digest == input.source_digest &&
            first.source_revision == input.source_revision && first.pixel_digest == assets::sha256(first.srgb_rgba), "capture carries exact source/request/pixel identity");
    require(first.render_graph_passes == std::vector<std::string>{
        "scene.shadow", "scene.pbr", "scene.tonemap", "scene.capture-readback"},
        "offscreen receipt records render and capture-readback graph passes in execution order");
    require(first.device.backend == device::BackendKind::d3d12 && !first.device.adapter_name.empty() &&
            !first.shader_digests[0U].is_zero() && !first.shader_digests[2U].is_zero() &&
            !first.shader_digests[4U].is_zero() &&
            !first.shader_digests[6U].is_zero() && first.limitations.size() == 3U,
        "hardware, PBR/shadow/tone/temporal shader evidence and limitations retained");
    std::reverse(input.objects.begin(), input.objects.end());
    const auto reversed = capture(device, input);
    require(first.srgb_rgba == reversed.srgb_rgba && first.depth == reversed.depth, "depth occlusion independent of draw order");
    std::reverse(input.objects.begin(), input.objects.end());
    input.camera.exposure_ev = 1.0;
    const auto brighter = capture(device, input);
    require(first.linear_hdr_rgba == brighter.linear_hdr_rgba && brighter.srgb_rgba[center] > first.srgb_rgba[center],
            "exposure leaves scene-linear HDR unchanged");
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        core::Vec3d color{first.linear_hdr_rgba[center], first.linear_hdr_rgba[center + 1U], first.linear_hdr_rgba[center + 2U]};
        color = take(render::linear_to_srgb(take(render::tone_map(take(render::apply_exposure(color, 1.0)), render::ToneMapMode::aces_fitted))));
        const double expected = (channel == 0U ? color.x : channel == 1U ? color.y : color.z) * 255.0;
        require(std::abs(expected - brighter.srgb_rgba[center + channel]) <= 1.1, "tone-map GPU parity proves exposure applied once");
    }
    input.camera.exposure_ev = 0.0; input.camera.position.x = 0.5;
    const auto moved = capture(device, input);
    require(moved.pixel_digest != first.pixel_digest && moved.request_digest != first.request_digest, "camera changes actual pixels and identity");
    require(take(input.objects[0U].mesh.serialize()) == source_before, "capture never changes source geometry");
    require(static_cast<bool>(device.write_buffer(sentinel, 0U, std::array<std::uint8_t, 4U>{1U, 2U, 3U, 4U})), "renderer preserves caller-owned resources");
    require(static_cast<bool>(device.destroy_buffer(sentinel)), "caller still owns sentinel retirement");
    auto textured = request(); constexpr std::uint64_t id = (1ULL << 40U) + 13U;
    textured.objects[0U].materials[0U].current_material.definition.base_color.asset = id; rebind(textured.objects[0U]);
    textured.textures.push_back(texture(id, {20U, 80U, 255U, 255U}));
    const auto textured_image = capture(device, textured);
    require(textured_image.pixel_digest != capture(device, request()).pixel_digest, "source texture pixels reach PBR output");
    auto grid_textured = request();
    grid_textured.objects[0U].materials[0U].current_material.definition.base_color.asset = id;
    rebind(grid_textured.objects[0U]);
    grid_textured.textures.push_back(texture_grid(id));
    const auto identity_grid_image = capture(device, grid_textured);
    auto secondary_uv_grid = grid_textured;
    secondary_uv_grid.objects[0U].uv1_set = 2U;
    for (auto& vertex : secondary_uv_grid.objects[0U].mesh.vertices) {
        vertex.uv1 = core::Vec2d{vertex.uv->y, 1.0 - vertex.uv->x};
    }
    secondary_uv_grid.objects[0U].materials[0U].current_material.definition
        .base_color.uv_set = 2U;
    rebind(secondary_uv_grid.objects[0U]);
    const auto secondary_uv_image = capture(device, secondary_uv_grid);
    require(secondary_uv_image.request_digest != identity_grid_image.request_digest &&
        secondary_uv_image.pixel_digest != identity_grid_image.pixel_digest &&
        secondary_uv_image.linear_hdr_rgba != identity_grid_image.linear_hdr_rgba,
        "material sampling resolves its authored UV-set identity to the secondary vertex stream");
    auto tertiary_uv_grid = grid_textured;
    tertiary_uv_grid.objects[0U].mesh.additional_uv_set_ids = {3U};
    for (auto& vertex : tertiary_uv_grid.objects[0U].mesh.vertices) {
        vertex.additional_uvs = {core::Vec2d{1.0 - vertex.uv->y,
            vertex.uv->x * 0.5}};
    }
    tertiary_uv_grid.objects[0U].materials[0U].current_material.definition
        .base_color.uv_set = 3U;
    rebind(tertiary_uv_grid.objects[0U]);
    const auto tertiary_uv_image = capture(device, tertiary_uv_grid);
    require(tertiary_uv_image.request_digest != identity_grid_image.request_digest &&
        tertiary_uv_image.pixel_digest != identity_grid_image.pixel_digest &&
        tertiary_uv_image.linear_hdr_rgba != identity_grid_image.linear_hdr_rgba,
        "material sampling resolves an additional authored UV identity to its vertex stream");
    grid_textured.objects[0U].materials[0U].current_material.definition.base_color.transform.scale = {0.5, 0.5};
    rebind(grid_textured.objects[0U]);
    const auto transformed_grid_image = capture(device, grid_textured);
    require(transformed_grid_image.request_digest != identity_grid_image.request_digest &&
        transformed_grid_image.pixel_digest != identity_grid_image.pixel_digest &&
        transformed_grid_image.linear_hdr_rgba != identity_grid_image.linear_hdr_rgba,
        "per-texture UV scale changes PBR pixels and source-bound request identity");
    auto rotated_grid = grid_textured;
    rotated_grid.objects[0U].materials[0U].current_material.definition.base_color.transform.rotation =
        1.5707963267948966;
    rotated_grid.objects[0U].materials[0U].current_material.definition.base_color.transform.pivot =
        {0.5, 0.5};
    rebind(rotated_grid.objects[0U]);
    const auto rotated_grid_image = capture(device, rotated_grid);
    require(rotated_grid_image.request_digest != identity_grid_image.request_digest &&
        rotated_grid_image.pixel_digest != identity_grid_image.pixel_digest &&
        rotated_grid_image.linear_hdr_rgba != identity_grid_image.linear_hdr_rgba,
        "rotated pivoted UVs change PBR pixels and source-bound request identity");
    constexpr std::uint64_t metallic_roughness_id = id + 1U;
    auto independent_uvs = request();
    auto& independent_material = independent_uvs.objects[0U].materials[0U].current_material.definition;
    independent_material.base_color.asset = id;
    independent_material.base_color.transform.scale = {0.75, 0.5};
    independent_material.base_color.transform.rotation = 0.2;
    independent_material.base_color.transform.pivot = {0.5, 0.5};
    independent_material.metallic_roughness.asset = metallic_roughness_id;
    independent_material.metallic_roughness.transform.offset = {0.25, -0.125};
    independent_material.metallic = 1.0;
    independent_material.roughness = 0.35;
    rebind(independent_uvs.objects[0U]);
    independent_uvs.textures.push_back(texture_grid(id));
    independent_uvs.textures.push_back(texture_grid(metallic_roughness_id,
        assets::TextureSemantic::metallic_roughness));
    const auto independent_uv_image = capture(device, independent_uvs);
    auto identity_metallic_uvs = independent_uvs;
    auto& identity_metallic_binding = identity_metallic_uvs.objects[0U].materials[0U].
        current_material.definition;
    identity_metallic_binding.metallic_roughness.transform = {};
    rebind(identity_metallic_uvs.objects[0U]);
    const auto identity_metallic_uv_image = capture(device, identity_metallic_uvs);
    require(independent_uv_image.request_digest != identity_metallic_uv_image.request_digest &&
        independent_uv_image.pixel_digest != identity_metallic_uv_image.pixel_digest &&
        independent_uv_image.linear_hdr_rgba != identity_metallic_uv_image.linear_hdr_rgba,
        "metallic-roughness uses its own UV affine independently of base color");
    constexpr std::uint64_t normal_map_id = id + 2U;
    auto normal_uvs = request();
    normal_uvs.objects[0U].materials[0U].current_material.definition.normal.asset =
        normal_map_id;
    rebind(normal_uvs.objects[0U]);
    normal_uvs.textures.push_back(texture_grid(normal_map_id,
        assets::TextureSemantic::normal));
    const auto identity_normal_uv_image = capture(device, normal_uvs);
    auto& normal_binding = normal_uvs.objects[0U].materials[0U].
        current_material.definition.normal;
    normal_binding.transform.rotation = 0.4;
    normal_binding.transform.scale = {0.8, 0.6};
    rebind(normal_uvs.objects[0U]);
    const auto transformed_normal_uv_image = capture(device, normal_uvs);
    require(transformed_normal_uv_image.request_digest !=
            identity_normal_uv_image.request_digest &&
        transformed_normal_uv_image.pixel_digest != identity_normal_uv_image.pixel_digest &&
        transformed_normal_uv_image.linear_hdr_rgba != identity_normal_uv_image.linear_hdr_rgba,
        "normal-map UV affine changes sampling and the derived tangent frame");
    auto open_gl_normal_uvs = normal_uvs;
    open_gl_normal_uvs.objects[0U].materials[0U].current_material.definition.normal.
        normal_convention = render::NormalConvention::opengl_y_plus;
    rebind(open_gl_normal_uvs.objects[0U]);
    const auto open_gl_normal_image = capture(device, open_gl_normal_uvs);
    require(open_gl_normal_image.request_digest != transformed_normal_uv_image.request_digest &&
        open_gl_normal_image.pixel_digest != transformed_normal_uv_image.pixel_digest &&
        open_gl_normal_image.linear_hdr_rgba != transformed_normal_uv_image.linear_hdr_rgba,
        "OpenGL Y-plus normal maps reverse tangent handedness and change sampled normal pixels");
    auto environment_input = request();
    d3d12::SceneRenderEnvironment environment;
    environment.source_digest = hash("fixture uniform diffuse irradiance");
    environment.diffuse_irradiance_over_pi_sh[0U] =
        {3.544907701811032, 3.544907701811032, 3.544907701811032};
    environment_input.environment = environment;
    const auto environment_image = capture(device, environment_input);
    require(environment_image.environment_digest == environment.source_digest &&
        environment_image.request_digest != take(request().digest()) &&
        environment_image.linear_hdr_rgba != capture(device, request()).linear_hdr_rgba,
        "diffuse irradiance reaches native PBR pixels and remains source/request bound");
    textured.objects[0U].materials[0U].current_material.definition.alpha_mode = render::AlphaMode::masked; rebind(textured.objects[0U]);
    textured.textures[0U] = texture(id, {255U, 255U, 255U, 0U});
    const auto masked = capture(device, textured);
    require(masked.depth[center / 4U] == 1.0F && masked.linear_hdr_rgba[center] == 0.0F, "masked alpha discards color and depth");
    auto rejected = textured; rejected.textures.clear();
    const auto serial_before = device.completed_serial();
    require(!d3d12::render_scene(device, rejected, CARTO_D3D12_SHADER_DIR) && device.completed_serial() == serial_before,
            "invalid inputs rejected before GPU submission");
    require(!d3d12::render_scene(device, request(), "missing-scene-shader-directory"), "missing shader fails closed");
    require(device.debug_receipts().empty() && !device.device_lost(), "no native debug/device-loss errors");
}

void local_point_spot_lights_reach_native_pixels(d3d12::D3D12Device& device) {
    auto input = request();
    input.objects.front() = object(1U, {1.0, 1.0, 1.0, 1.0});
    input.light = {{0.0, 0.0, 1.0}, {}};
    const auto baseline = capture(device, input);
    const std::size_t center = (48U * 96U + 48U) * 4U;
    const float baseline_luminance = baseline.linear_hdr_rgba[center] +
        baseline.linear_hdr_rgba[center + 1U] +
        baseline.linear_hdr_rgba[center + 2U];

    d3d12::SceneRenderLocalLight point;
    point.object = {77U};
    point.kind = d3d12::SceneRenderLocalLightKind::point;
    point.position = {0.0, 0.0, 2.0};
    point.radiance = {4.0, 4.0, 4.0};
    point.range = 1000.0;
    input.local_lights = {point};
    const auto point_image = capture(device, input);
    const float point_luminance = point_image.linear_hdr_rgba[center] +
        point_image.linear_hdr_rgba[center + 1U] +
        point_image.linear_hdr_rgba[center + 2U];
    require(point_luminance > baseline_luminance,
        "point-light texture data contributes direct PBR energy at the illuminated surface");
    auto farther_point_request = input;
    farther_point_request.local_lights.front().position = {0.0, 0.0, 4.0};
    const auto farther_point_image = capture(device, farther_point_request);
    const float near_light_contribution = point_luminance - baseline_luminance;
    const float far_light_contribution =
        farther_point_image.linear_hdr_rgba[center] +
        farther_point_image.linear_hdr_rgba[center + 1U] +
        farther_point_image.linear_hdr_rgba[center + 2U] -
        baseline_luminance;
    require(far_light_contribution > 0.0F &&
            far_light_contribution < near_light_contribution * 0.32F &&
            far_light_contribution > near_light_contribution * 0.18F,
        "point-light contribution follows inverse-square distance before the range cutoff");
    auto near_field_request = input;
    near_field_request.local_lights.front().position = {0.0, 0.0, 0.001};
    const auto near_field_image = capture(device, near_field_request);
    require(std::all_of(near_field_image.linear_hdr_rgba.begin(),
                near_field_image.linear_hdr_rgba.end(),
                [](float value) { return std::isfinite(value); }) &&
            near_field_image.linear_hdr_rgba[center] > point_image.linear_hdr_rgba[center],
        "near-field inverse-square response stays finite under the 1 cm floor");
    auto beyond_range_request = farther_point_request;
    beyond_range_request.local_lights.front().range = 3.0;
    const auto beyond_range_image = capture(device, beyond_range_request);
    require(beyond_range_image.linear_hdr_rgba[center] ==
            baseline.linear_hdr_rgba[center],
        "point light outside its authored range contributes no direct energy");
    require(std::find(point_image.limitations.begin(), point_image.limitations.end(),
            "Point/spot lights are unshadowed; inverse-square intensity uses a 1 cm floor, quartic range fade, and uncalibrated linear units") !=
            point_image.limitations.end(),
        "capture reports the local-light shadow and photometric limitations");

    auto spot_request = input;
    spot_request.local_lights.front().kind =
        d3d12::SceneRenderLocalLightKind::spot;
    spot_request.local_lights.front().emission_direction = {0.0, 0.0, -1.0};
    spot_request.local_lights.front().inner_cone_radians = 0.2;
    spot_request.local_lights.front().outer_cone_radians = 0.5;
    const auto spot_image = capture(device, spot_request);
    require(spot_image.linear_hdr_rgba[center] > baseline.linear_hdr_rgba[center],
        "spot cone aligned toward the surface contributes direct PBR energy");

    auto shadow_control_request = spot_request;
    shadow_control_request.camera.position = {2.0, 0.0, 3.0};
    shadow_control_request.local_lights.front().position = {0.0, 0.0, 2.0};
    shadow_control_request.local_lights.front().range = 5.0;
    shadow_control_request.objects.push_back(object(2U, {0.7, 0.1, 0.03, 1.0}));
    shadow_control_request.objects.back().world_transform.translation = {0.0, 0.0, 1.0};
    shadow_control_request.objects.back().world_transform.scale = {0.15, 0.15, 0.15};
    const auto unshadowed_control = capture(device, shadow_control_request);
    auto shadowed_spot_request = shadow_control_request;
    shadowed_spot_request.local_lights.front().casts_shadows = true;
    const auto shadowed_spot_image = capture(device, shadowed_spot_request);
    const float unshadowed_center =
        unshadowed_control.linear_hdr_rgba[center] +
        unshadowed_control.linear_hdr_rgba[center + 1U] +
        unshadowed_control.linear_hdr_rgba[center + 2U];
    const float shadowed_center =
        shadowed_spot_image.linear_hdr_rgba[center] +
        shadowed_spot_image.linear_hdr_rgba[center + 1U] +
        shadowed_spot_image.linear_hdr_rgba[center + 2U];
    require(shadowed_center < unshadowed_center * 0.75F &&
            std::any_of(shadowed_spot_image.limitations.begin(),
                shadowed_spot_image.limitations.end(), [](const std::string& item) {
                    return item.find("One authored spotlight uses the scene shadow map") !=
                        std::string::npos;
                }),
        "spotlight shadow attenuates an occluded receiver and is identified in capture limitations");

    spot_request.local_lights.front().emission_direction = {0.0, 0.0, 1.0};
    const auto reversed_spot = capture(device, spot_request);
    require(reversed_spot.linear_hdr_rgba[center] ==
            baseline.linear_hdr_rgba[center],
        "spot cone directed away from the surface contributes no direct energy");

    spot_request.local_lights.front().emission_direction = {0.0, 0.0, -1.0e300};
    const auto magnitude_scaled_spot = capture(device, spot_request);
    require(magnitude_scaled_spot.pixel_digest == spot_image.pixel_digest,
        "spot upload normalizes finite direction magnitude before float conversion");
}

void specular_environment_mips_reach_native_pixels(d3d12::D3D12Device& device) {
    TempDirectory temporary;
    const auto make_environment_request = [&](std::string_view name,
                                              std::array<std::uint8_t, 4U> color) {
        const auto path = temporary.path() / std::string(name);
        const auto encoded = equirectangular_tga(16U, 8U, color);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
        output.close();
        require(static_cast<bool>(output), "fixture writes specular panorama");
        auto environment = take(d3d12::load_scene_render_environment_equirectangular(
            path, assets::ColorSpace::linear));
        environment.diffuse_irradiance_over_pi_sh = {};
        auto input = request();
        input.objects.front().materials.front().current_material.definition.metallic = 1.0;
        input.objects.front().materials.front().current_material.definition.roughness = 0.2;
        rebind(input.objects.front());
        input.environment = std::move(environment);
        return input;
    };
    d3d12::SceneRenderCache cache;
    const auto red_request = make_environment_request("red.tga", {255U, 0U, 0U, 255U});
    const auto blue_request = make_environment_request("blue.tga", {0U, 0U, 255U, 255U});
    const auto red = take(d3d12::render_scene_cached(
        device, cache, red_request, CARTO_D3D12_SHADER_DIR));
    const auto blue = take(d3d12::render_scene_cached(
        device, cache, blue_request, CARTO_D3D12_SHADER_DIR));
    const auto blue_reference = capture(device, blue_request);
    require(red.environment_digest != blue.environment_digest &&
        red.linear_hdr_rgba != blue.linear_hdr_rgba &&
        blue.pixel_digest == blue_reference.pixel_digest,
        "source-bound reflections change metallic pixels and invalidate cached environment textures");

    auto smooth_request = red_request;
    smooth_request.light.radiance = {};
    smooth_request.objects.front().materials.front().current_material.definition.roughness = 0.1;
    rebind(smooth_request.objects.front());
    auto rough_request = smooth_request;
    rough_request.objects.front().materials.front().current_material.definition.roughness = 0.9;
    rebind(rough_request.objects.front());
    const auto smooth_reflection = take(d3d12::render_scene_cached(
        device, cache, smooth_request, CARTO_D3D12_SHADER_DIR));
    const auto rough_reflection = take(d3d12::render_scene_cached(
        device, cache, rough_request, CARTO_D3D12_SHADER_DIR));
    require(smooth_reflection.request_digest != rough_reflection.request_digest &&
        smooth_reflection.linear_hdr_rgba != rough_reflection.linear_hdr_rgba,
        "split-sum BRDF integration changes constant-environment reflection energy with roughness");
}
void blended_scene_draws_sort_back_to_front_without_writing_depth(
    d3d12::D3D12Device& device) {
    auto opaque_background = request();
    opaque_background.light.radiance = {};
    const auto background_image = capture(device, opaque_background);

    auto far_green = object(2U, {0.1, 0.8, 0.2, 1.0});
    far_green.world_transform.translation.z = 0.3;
    auto near_blue = object(3U, {0.1, 0.2, 0.9, 1.0});
    near_blue.world_transform.translation.z = 0.6;
    auto far_opaque = opaque_background;
    far_opaque.objects = {far_green};
    auto near_opaque = opaque_background;
    near_opaque.objects = {near_blue};
    const auto far_image = capture(device, far_opaque);
    const auto near_image = capture(device, near_opaque);

    auto transparent_scene = opaque_background;
    near_blue.materials.front().current_material.definition.base_color_factor.w = 0.5;
    near_blue.materials.front().current_material.definition.alpha_mode =
        render::AlphaMode::blended;
    rebind(near_blue);
    far_green.materials.front().current_material.definition.base_color_factor.w = 0.5;
    far_green.materials.front().current_material.definition.alpha_mode =
        render::AlphaMode::blended;
    rebind(far_green);
    // Deliberately submit near before far; the renderer must sort the draws.
    transparent_scene.objects.push_back(std::move(near_blue));
    transparent_scene.objects.push_back(std::move(far_green));
    const auto blended = capture(device, transparent_scene);
    const std::size_t center_pixel =
        (static_cast<std::size_t>(blended.height / 2U) * blended.width +
         blended.width / 2U) * 4U;
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const float expected = near_image.linear_hdr_rgba[center_pixel + channel] * 0.5F +
            far_image.linear_hdr_rgba[center_pixel + channel] * 0.25F +
            background_image.linear_hdr_rgba[center_pixel + channel] * 0.25F;
        require(std::abs(blended.linear_hdr_rgba[center_pixel + channel] - expected) <
                1.0e-4F,
            "transparent submeshes blend in back-to-front order in linear HDR");
    }
    require(std::abs(blended.depth[center_pixel / 4U] -
            background_image.depth[center_pixel / 4U]) < 1.0e-6F,
        "transparent draws test against opaque depth without replacing it");
}
void vertex_colors_reach_pbr_and_masked_alpha(d3d12::D3D12Device& device) {
    auto plain = request();
    plain.light.radiance = {};
    plain.objects.front().materials.front().current_material.definition
        .base_color_factor = {1.0, 1.0, 1.0, 1.0};
    rebind(plain.objects.front());
    const auto plain_image = capture(device, plain);

    auto tinted = plain;
    for (auto& vertex : tinted.objects.front().mesh.vertices)
        vertex.color = core::Vec4d{0.15, 0.9, 0.25, 1.0};
    rebind(tinted.objects.front());
    const auto tinted_image = capture(device, tinted);
    const std::size_t center_pixel =
        (static_cast<std::size_t>(tinted.height / 2U) * tinted.width +
         tinted.width / 2U) * 4U;
    require(tinted_image.linear_hdr_rgba[center_pixel] <
            plain_image.linear_hdr_rgba[center_pixel] * 0.25F &&
        tinted_image.linear_hdr_rgba[center_pixel + 1U] >
            tinted_image.linear_hdr_rgba[center_pixel],
        "linear authored vertex RGB multiplies the base-color response");

    auto masked = plain;
    for (auto& vertex : masked.objects.front().mesh.vertices)
        vertex.color = core::Vec4d{1.0, 1.0, 1.0, 0.0};
    masked.objects.front().materials.front().current_material.definition.alpha_mode =
        render::AlphaMode::masked;
    masked.objects.front().materials.front().current_material.definition.alpha_cutoff = 0.5;
    rebind(masked.objects.front());
    const auto masked_image = capture(device, masked);
    require(masked_image.depth[center_pixel / 4U] == 1.0F &&
        masked_image.linear_hdr_rgba[center_pixel] == 0.0F &&
        masked_image.linear_hdr_rgba[center_pixel + 1U] == 0.0F &&
        masked_image.linear_hdr_rgba[center_pixel + 2U] == 0.0F,
        "vertex alpha drives matching masked PBR discard and depth preservation");
}
void equirectangular_reflections_wrap_the_longitude_seam(
    d3d12::D3D12Device& device) {
    TempDirectory temporary;
    const auto path = temporary.path() / "seam.tga";
    const auto encoded = equirectangular_seam_tga();
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
        require(static_cast<bool>(output), "fixture writes longitude-seam panorama");
    }
    auto environment = take(d3d12::load_scene_render_environment_equirectangular(
        path, assets::ColorSpace::linear));
    environment.diffuse_irradiance_over_pi_sh = {};
    const auto make_request = [&](double camera_z) {
        auto input = request();
        auto& scene_object = input.objects.front();
        const double half_angle = 3.0 * 3.14159265358979323846 / 8.0;
        scene_object.world_transform.rotation = {
            0.0, std::sin(half_angle), 0.0, std::cos(half_angle)};
        auto& material = scene_object.materials.front().current_material;
        material.definition.base_color_factor = {0.8, 0.8, 0.8, 1.0};
        material.definition.metallic = 1.0;
        material.definition.roughness = 0.2;
        rebind(scene_object);
        input.camera.position = {3.0, 0.0, camera_z};
        input.camera.target = {0.0, 0.0, 0.0};
        input.light.radiance = {};
        input.environment = environment;
        return input;
    };
    const auto left = take(d3d12::render_scene(
        device, make_request(-0.001), CARTO_D3D12_SHADER_DIR));
    const auto right = take(d3d12::render_scene(
        device, make_request(0.001), CARTO_D3D12_SHADER_DIR));
    const std::size_t center =
        (static_cast<std::size_t>(left.height / 2U) * left.width + left.width / 2U) * 4U;
    const std::size_t center_depth =
        static_cast<std::size_t>(left.height / 2U) * left.width + left.width / 2U;
    require(left.depth[center_depth] < 1.0F && right.depth[center_depth] < 1.0F,
        "both seam-side views rasterize the metallic fixture at center");
    const float left_energy = left.linear_hdr_rgba[center] +
        left.linear_hdr_rgba[center + 1U] + left.linear_hdr_rgba[center + 2U];
    const float right_energy = right.linear_hdr_rgba[center] +
        right.linear_hdr_rgba[center + 1U] + right.linear_hdr_rgba[center + 2U];
    require(left_energy > 0.05F && right_energy > 0.05F,
        "seam comparison observes environment reflection radiance");
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        require(std::abs(left.linear_hdr_rgba[center + channel] -
                    right.linear_hdr_rgba[center + channel]) < 0.03F,
            "nearby directions across the panorama seam interpolate continuously");
    }
}
void hdr_environment_reflections_preserve_values_above_one(
    d3d12::D3D12Device& device) {
    TempDirectory temporary;
    const auto make_request = [&](std::string_view name,
                                  std::array<float, 3U> radiance) {
        const auto path = temporary.path() / std::string(name);
        const auto encoded = constant_radiance_hdr(16U, 8U, radiance);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
        output.close();
        require(static_cast<bool>(output), "fixture writes HDR reflection source");
        auto environment = take(d3d12::load_scene_render_environment_equirectangular(
            path, assets::ColorSpace::linear));
        environment.diffuse_irradiance_over_pi_sh = {};
        auto input = request();
        input.objects.front().materials.front().current_material.definition.metallic = 1.0;
        input.objects.front().materials.front().current_material.definition.roughness = 0.2;
        rebind(input.objects.front());
        input.environment = std::move(environment);
        return input;
    };
    const auto red_request = make_request("red.hdr", {4.0F, 0.0F, 0.0F});
    const auto blue_request = make_request("blue.hdr", {0.0F, 0.0F, 4.0F});
    d3d12::SceneRenderCache cache;
    const auto red = take(d3d12::render_scene_cached(
        device, cache, red_request, CARTO_D3D12_SHADER_DIR));
    const auto blue = take(d3d12::render_scene_cached(
        device, cache, blue_request, CARTO_D3D12_SHADER_DIR));
    const auto blue_reference = capture(device, blue_request);
    const float maximum_radiance = *std::max_element(
        red.linear_hdr_rgba.begin(), red.linear_hdr_rgba.end());
    require(red.environment_digest != blue.environment_digest &&
        red.linear_hdr_rgba != blue.linear_hdr_rgba && maximum_radiance > 1.0F &&
        blue.pixel_digest == blue_reference.pixel_digest,
        "Radiance reflection preserves HDR values and source changes invalidate the cached environment");
}
void transformed_normals_match_baked_geometry(d3d12::D3D12Device& device) {
    for (double mirror : {1.0, -1.0}) {
        auto input = request(); input.objects[0U] = object(1U, {0.6, 0.3, 0.1, 1.0}, true);
        input.objects[0U].materials[0U].current_material.definition.normal.asset = 51U;
        rebind(input.objects[0U]);
        input.textures.push_back(texture(51U, {180U, 80U, 240U, 255U}, assets::TextureSemantic::normal));
        input.objects[0U].world_transform.scale = {mirror * 1.7, 0.9, 0.4};
        input.objects[0U].world_transform.rotation = {0.0, std::sin(0.13), 0.0, std::cos(0.13)};
        auto baked = input; auto& baked_object = baked.objects[0U]; const auto transform = baked_object.world_transform;
        for (auto& vertex : baked_object.mesh.vertices) {
            vertex.position = transform.translation + transform.rotation.rotate(core::componentwise_multiply(transform.scale, vertex.position));
            vertex.normal = transform.rotation.rotate({vertex.normal.x / transform.scale.x,
                vertex.normal.y / transform.scale.y, vertex.normal.z / transform.scale.z}).normalized();
            const auto tangent = transform.rotation.rotate(core::componentwise_multiply(transform.scale,
                {vertex.tangent->x, vertex.tangent->y, vertex.tangent->z})).normalized();
            vertex.tangent = core::Vec4d{tangent.x, tangent.y, tangent.z, mirror * vertex.tangent->w};
        }
        baked_object.world_transform = {}; rebind(baked_object);
        const auto actual = capture(device, input); const auto expected = capture(device, baked);
        std::size_t compared = 0U;
        for (std::size_t pixel = 0U; pixel < actual.depth.size(); ++pixel) {
            if (actual.depth[pixel] == 1.0F || expected.depth[pixel] == 1.0F) continue;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
                require(std::abs(actual.linear_hdr_rgba[pixel * 4U + channel] - expected.linear_hdr_rgba[pixel * 4U + channel]) < 0.001F,
                        "nonuniform/mirrored model normals must match inverse-transpose baked geometry");
            ++compared;
        }
        require(compared > 100U, "normal comparison contains actual rasterized geometry");
    }
}
// Read BGRA8 through a GPU identity pass, not a second tone-map operation.
// This checks the caller-owned presentation texture's actual displayed values.
std::vector<float> probe_display_target(d3d12::D3D12Device& device, gpu::TextureHandle source,
                                        std::uint32_t width, std::uint32_t height,
                                        device_ir::ResourceState source_state =
                                            device_ir::ResourceState::copy_source) {
    constexpr std::string_view shader = R"(
        Texture2D<float4> image : register(t0);
        SamplerState image_sampler : register(s0);
        struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
        Output VSMain(uint vertex : SV_VertexID) {
            const float2 p[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
            const float2 uv[3] = {float2(0,1),float2(0,-1),float2(2,1)};
            Output result; result.position=float4(p[vertex],0,1); result.uv=uv[vertex]; return result;
        }
        float4 PSMain(Output input) : SV_TARGET { return image.Sample(image_sampler,input.uv); }
    )";
    const auto vertex = take(device.create_shader(take(d3d12::compile_hlsl({"display_probe_vs",
        gpu::ShaderStage::vertex, "VSMain", "display-probe.hlsl", std::string(shader)}))));
    const auto fragment = take(device.create_shader(take(d3d12::compile_hlsl({"display_probe_ps",
        gpu::ShaderStage::fragment, "PSMain", "display-probe.hlsl", std::string(shader)}))));
    gpu::PipelineDesc descriptor;
    descriptor.debug_name = "scene_display_identity_probe"; descriptor.shaders = {vertex, fragment};
    descriptor.descriptor_bindings = {{0U, 0U, gpu::ShaderStage::fragment}};
    descriptor.depth_test = false; descriptor.depth_write = false;
    descriptor.cull_mode = gpu::CullMode::none; descriptor.color_format = gpu::Format::rgba32_float;
    const auto pipeline = take(device.create_pipeline(descriptor));
    const auto sampler = take(device.create_sampler({}));
    const auto vertices = take(device.create_buffer({48U, gpu::MemoryClass::upload, gpu::BufferUsage::vertex}));
    const std::array<float, 12U> unused{};
    require(static_cast<bool>(device.write_buffer(vertices, 0U,
        {reinterpret_cast<const std::uint8_t*>(unused.data()), sizeof(unused)})), "probe vertex buffer write");
    const auto target = take(device.create_texture({width, height, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d, gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source}));
    device_ir::DeviceCommandStream commands;
    commands.append(device_ir::CmdTransitionResource{source, source_state, device_ir::ResourceState::shader_read});
    commands.append(device_ir::CmdTransitionResource{target, device_ir::ResourceState::undefined, device_ir::ResourceState::render_target});
    commands.append(device_ir::CmdTransitionResource{vertices, device_ir::ResourceState::undefined, device_ir::ResourceState::vertex_read});
    commands.append(device_ir::CmdBeginRendering{target, {}});
    commands.append(device_ir::CmdSetViewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1});
    commands.append(device_ir::CmdSetScissor{0, 0, width, height});
    commands.append(device_ir::CmdBindPipeline{pipeline});
    commands.append(device_ir::CmdBindVertexBuffer{vertices, 0U, 16U});
    commands.append(device_ir::CmdBindResources{{{0U, 0U, source, sampler}}});
    commands.append(device_ir::CmdDraw{3U}); commands.append(device_ir::CmdEndRendering{});
    commands.append(device_ir::CmdTransitionResource{source, device_ir::ResourceState::shader_read, source_state});
    require(static_cast<bool>(device.wait(take(device.submit(commands)))), "display probe executes");
    auto pixels = take(device.read_texture_rgba32f(target));
    require(static_cast<bool>(device.destroy_texture(target)) && static_cast<bool>(device.destroy_buffer(vertices)) &&
        static_cast<bool>(device.destroy_pipeline(pipeline)) && static_cast<bool>(device.destroy_sampler(sampler)) &&
        static_cast<bool>(device.destroy_shader(vertex)) && static_cast<bool>(device.destroy_shader(fragment)), "probe owns only its temporary resources");
    return pixels;
}
void direct_target_matches_capture_and_preserves_caller_ownership(d3d12::D3D12Device& device) {
    const auto input = request(); const auto expected = capture(device, input);
    gpu::TextureDesc descriptor{input.width, input.height, 1U, 1U, 1U, gpu::Format::bgra8_unorm,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source | gpu::TextureUsage::sampled};
    const auto target = take(device.create_texture(descriptor));
    const auto source_before = take(input.digest());
    const auto before_bad_state = device.completed_serial();
    require(!d3d12::render_scene_to_texture(device, input, CARTO_D3D12_SHADER_DIR,
        target, device_ir::ResourceState::copy_source) && device.completed_serial() == before_bad_state,
        "caller cannot misdeclare a fresh target as already initialized");
    auto receipt = take(d3d12::render_scene_to_texture(device, input, CARTO_D3D12_SHADER_DIR,
        target, device_ir::ResourceState::undefined));
    require(receipt.request_digest == expected.request_digest && receipt.source_digest == input.source_digest &&
        receipt.draw_count == expected.draw_count && receipt.shader_digests == expected.shader_digests,
        "direct target and capture bind identical source, draw, request and shader evidence");
    require(expected.render_graph_passes == std::vector<std::string>{
        "scene.shadow", "scene.pbr", "scene.tonemap", "scene.capture-readback"} &&
        receipt.render_graph_passes == std::vector<std::string>{
            "scene.shadow", "scene.pbr", "scene.tonemap", "scene.target-copy-source"},
        "capture and direct target report the bounded passes actually recorded");
    require(receipt.render_graph_passes == std::vector<std::string>{
        "scene.shadow", "scene.pbr", "scene.tonemap", "scene.target-copy-source"},
        "direct-target receipt includes the compiled output-state handoff pass");
    const auto pixels = probe_display_target(device, target, input.width, input.height);
    require(pixels.size() == expected.srgb_rgba.size(), "direct target pixel extent matches capture");
    for (std::size_t i = 0U; i < pixels.size(); ++i)
        require(std::abs(pixels[i] * 255.0F - expected.srgb_rgba[i]) <= 1.01F, "direct BGRA8 output matches offline capture within one quantization step");
    d3d12::SceneRenderCache cache;
    const auto cached_receipt = take(d3d12::render_scene_to_texture_cached(
        device, cache, input, CARTO_D3D12_SHADER_DIR, target,
        device_ir::ResourceState::copy_source));
    require(cache.generation() == 1U &&
        cached_receipt.shader_digests == receipt.shader_digests &&
        cached_receipt.request_digest == receipt.request_digest,
        "cached BGRA8 direct target reuses the matching scene program set");
    gpu::TextureDesc frame_descriptor{input.width, input.height, 1U, 1U, 1U,
        gpu::Format::bgra8_unorm, gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::sampled |
            gpu::TextureUsage::transfer_destination | gpu::TextureUsage::transfer_source};
    const auto frame_target = take(device.create_texture(frame_descriptor));
    device_ir::DeviceCommandStream initialize_frame_target;
    initialize_frame_target.append(device_ir::CmdTransitionResource{frame_target,
        device_ir::ResourceState::undefined, device_ir::ResourceState::present});
    require(static_cast<bool>(device.wait(take(device.submit(initialize_frame_target)))),
        "frame target begins in its declared present state");
    const auto before_integrated_frame = device.completed_serial();
    const auto integrated = take(d3d12::render_scene_to_texture_with_frame_handoff_cached(
        device, cache, input, CARTO_D3D12_SHADER_DIR, target,
        device_ir::ResourceState::copy_source, frame_target));
    require(cache.generation() == 1U,
        "integrated desktop handoff reuses the BGRA8 scene program set");
    require(device.completed_serial() == before_integrated_frame + 1U,
        "scene render, desktop copy, and present-state handoff use one submission");
    require(integrated.request_digest == expected.request_digest &&
        integrated.source_digest == input.source_digest &&
        integrated.draw_count == expected.draw_count,
        "integrated handoff retains the exact scene request and draw lineage");
    require(integrated.render_graph_passes == std::vector<std::string>{
        "scene.shadow", "scene.pbr", "scene.tonemap", "desktop.copy-authored-scene",
        "desktop.present"},
        "integrated frame receipt follows the compiled scene and presentation pass order");
    const auto integrated_pixels = probe_display_target(device, frame_target,
        input.width, input.height, device_ir::ResourceState::present);
    require(integrated_pixels.size() == expected.srgb_rgba.size(),
        "integrated frame pixel extent matches the offline scene capture");
    for (std::size_t i = 0U; i < integrated_pixels.size(); ++i)
        require(std::abs(integrated_pixels[i] * 255.0F - expected.srgb_rgba[i]) <= 1.01F,
            "integrated desktop frame preserves the direct scene pixels");
    const auto format_switch_capture = take(d3d12::render_scene_cached(
        device, cache, input, CARTO_D3D12_SHADER_DIR));
    require(cache.generation() == 2U &&
        format_switch_capture.pixel_digest == expected.pixel_digest,
        "changing the cached target format rebuilds its pipeline and preserves pixels");
    const auto refuse_frame_target = [&](gpu::TextureHandle invalid_target,
                                         std::string_view label) {
        const auto serial = device.completed_serial();
        require(!d3d12::render_scene_to_texture_with_frame_handoff(device, input,
            CARTO_D3D12_SHADER_DIR, target, device_ir::ResourceState::copy_source,
            invalid_target), label);
        require(device.completed_serial() == serial,
            "invalid integrated frame target is rejected before GPU submission");
    };
    refuse_frame_target(target, "scene/frame target alias refused");
    refuse_frame_target({}, "null integrated frame target refused");
    const auto wrong_frame_descriptor = [&frame_descriptor](auto mutate) {
        auto descriptor = frame_descriptor;
        mutate(descriptor);
        return descriptor;
    };
    const auto wrong_frame_extent = take(device.create_texture(
        wrong_frame_descriptor([](auto& descriptor) { ++descriptor.width; })));
    refuse_frame_target(wrong_frame_extent, "wrong integrated frame extent refused");
    require(static_cast<bool>(device.destroy_texture(wrong_frame_extent)),
        "invalid frame extent remains caller-owned");
    const auto wrong_frame_format = take(device.create_texture(
        wrong_frame_descriptor([](auto& descriptor) {
            descriptor.format = gpu::Format::rgba32_float;
        })));
    refuse_frame_target(wrong_frame_format, "wrong integrated frame format refused");
    require(static_cast<bool>(device.destroy_texture(wrong_frame_format)),
        "invalid frame format remains caller-owned");
    const auto wrong_frame_usage = take(device.create_texture(
        wrong_frame_descriptor([](auto& descriptor) {
            descriptor.usage = gpu::TextureUsage::color_attachment |
                gpu::TextureUsage::sampled;
        })));
    refuse_frame_target(wrong_frame_usage, "frame target without copy usage refused");
    require(static_cast<bool>(device.destroy_texture(wrong_frame_usage)),
        "invalid frame usage remains caller-owned");
    require(static_cast<bool>(device.destroy_texture(frame_target)),
        "integrated render leaves the caller frame target owned by its caller");
    auto moved = input; moved.camera.position.x = 0.5;
    receipt = take(d3d12::render_scene_to_texture(device, moved, CARTO_D3D12_SHADER_DIR,
        target, device_ir::ResourceState::copy_source));
    require(receipt.request_digest != expected.request_digest, "persistent target accepts a changed view");
    require(probe_display_target(device, target, input.width, input.height) != pixels, "changed view changes actual target pixels");
    const auto refuses = [&](gpu::TextureHandle handle, device_ir::ResourceState state, std::string_view label) {
        const auto before = device.completed_serial();
        require(!d3d12::render_scene_to_texture(device, input, CARTO_D3D12_SHADER_DIR, handle, state), label);
        require(before == device.completed_serial(), "invalid direct target never submits GPU work");
    };
    refuses({}, device_ir::ResourceState::undefined, "null target refused");
    refuses(target, device_ir::ResourceState::shader_read, "undeclared direct target initial state refused");
    const auto invalid_descriptor = [&](auto mutate, std::string_view label) {
        auto bad = descriptor; mutate(bad); const auto handle = take(device.create_texture(bad));
        refuses(handle, device_ir::ResourceState::undefined, label);
        require(static_cast<bool>(device.destroy_texture(handle)), "invalid target remains caller-owned");
    };
    invalid_descriptor([](auto& d) { d.width += 1U; }, "wrong target extent refused");
    invalid_descriptor([](auto& d) { d.format = gpu::Format::rgba32_float; }, "wrong target format refused");
    invalid_descriptor([](auto& d) { d.usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_source; }, "missing color attachment usage refused");
    invalid_descriptor([](auto& d) { d.usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::sampled; }, "missing copy-source usage refused");
    require(take(input.digest()) == source_before, "direct rendering never mutates source inputs");
    require(static_cast<bool>(device.describe_texture(target)) && static_cast<bool>(device.destroy_texture(target)),
            "valid target remains caller-owned after successful and failed renders");
    refuses(target, device_ir::ResourceState::copy_source, "stale target generation refused");
    require(device.debug_receipts().empty() && !device.device_lost(), "direct render target maintains clean native state");
}

void explicit_temporal_resolve_uses_bound_history_and_resets_on_cut(
    d3d12::D3D12Device& device) {
    const auto input = request();
    const auto current = capture(device, input);
    auto jittered_input = input;
    jittered_input.subpixel_jitter_pixels = {0.25, -0.25};
    const auto jittered_capture = capture(device, jittered_input);
    require(jittered_capture.request_digest != current.request_digest &&
            jittered_capture.srgb_rgba != current.srgb_rgba,
        "pixel-space projection jitter changes request identity and rendered coverage");
    const auto history = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::sampled |
            gpu::TextureUsage::transfer_source}));
    const auto resolved = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::sampled |
            gpu::TextureUsage::transfer_source}));

    auto previous_input = input;
    previous_input.light.radiance = {0.2, 0.3, 0.4};
    const auto previous = capture(device, previous_input);
    auto reset_request = previous_input;
    reset_request.temporal = d3d12::SceneRenderTemporalInputs{};
    reset_request.temporal->resolved_color = history;
    reset_request.temporal->camera_cut = true;
    auto reset = take(d3d12::render_scene(device, reset_request,
        CARTO_D3D12_SHADER_DIR));
    require(reset.linear_hdr_rgba == previous.linear_hdr_rgba &&
            reset.srgb_rgba == previous.srgb_rgba &&
            reset.temporal_output_state == device_ir::ResourceState::copy_source &&
            reset.render_graph_passes == std::vector<std::string>{
                "scene.shadow", "scene.pbr", "scene.temporal-resolve",
                "scene.tonemap", "scene.capture-readback"},
        "camera-cut reset copies current HDR through the declared temporal graph pass");

    std::vector<std::uint8_t> history_bytes;
    history_bytes.reserve(reset.linear_hdr_rgba.size() * sizeof(float));
    for (float value : reset.linear_hdr_rgba) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0U; shift < 32U; shift += 8U)
            history_bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
    }

    auto accumulation_request = input;
    accumulation_request.temporal = d3d12::SceneRenderTemporalInputs{};
    accumulation_request.temporal->history_color = history;
    accumulation_request.temporal->history_initial_state =
        device_ir::ResourceState::copy_source;
    accumulation_request.temporal->history_content_digest =
        assets::sha256(std::span<const std::uint8_t>{history_bytes});
    accumulation_request.temporal->history_request_digest = reset.request_digest;
    accumulation_request.temporal->history_source_revision = input.source_revision;
    accumulation_request.temporal->history_source_digest = input.source_digest;
    accumulation_request.temporal->resolved_color = resolved;
    accumulation_request.temporal->feedback = 0.75F;
    accumulation_request.temporal->zero_motion = true;
    const auto accumulated = take(d3d12::render_scene(device,
        accumulation_request, CARTO_D3D12_SHADER_DIR));
    float maximum_temporal_error = 0.0F;
    bool history_neighborhood_clamped = false;
    for (std::uint32_t y = 0U; y < input.height; ++y) {
        for (std::uint32_t x = 0U; x < input.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * input.width + x) * 4U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                float neighborhood_min = current.linear_hdr_rgba[pixel + channel];
                float neighborhood_max = neighborhood_min;
                for (int offset_y = -1; offset_y <= 1; ++offset_y) {
                    for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                        const int neighbor_x = static_cast<int>(x) + offset_x;
                        const int neighbor_y = static_cast<int>(y) + offset_y;
                        if (neighbor_x < 0 || neighbor_y < 0 ||
                            neighbor_x >= static_cast<int>(input.width) ||
                            neighbor_y >= static_cast<int>(input.height)) {
                            continue;
                        }
                        const std::size_t neighbor =
                            (static_cast<std::size_t>(neighbor_y) * input.width +
                             static_cast<std::size_t>(neighbor_x)) * 4U + channel;
                        const float sample = current.linear_hdr_rgba[neighbor];
                        if (!std::isfinite(sample)) continue;
                        neighborhood_min = std::min(neighborhood_min, sample);
                        neighborhood_max = std::max(neighborhood_max, sample);
                    }
                }
                const float history = previous.linear_hdr_rgba[pixel + channel];
                const float bounded_history = std::clamp(
                    history, neighborhood_min, neighborhood_max);
                history_neighborhood_clamped = history_neighborhood_clamped ||
                    bounded_history != history;
                const float expected = current.linear_hdr_rgba[pixel + channel] *
                    0.25F + bounded_history * 0.75F;
                maximum_temporal_error = std::max(maximum_temporal_error,
                    std::abs(accumulated.linear_hdr_rgba[pixel + channel] -
                        expected));
            }
        }
    }
    require(history_neighborhood_clamped && maximum_temporal_error < 0.001F,
        "temporal resolve blends scene-linear history within the current neighborhood bound");
    require(accumulated.temporal_output_state ==
                device_ir::ResourceState::copy_source &&
            accumulated.request_digest != current.request_digest &&
            accumulated.request_digest != reset.request_digest,
        "temporal output state and history-bound request identity are receipted");

    const auto motion_history = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination}));
    const auto motion_vectors = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::rg32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination}));
    const auto motion_vectors_y = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::rg32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination}));
    const auto motion_resolved = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::sampled |
            gpu::TextureUsage::transfer_source}));
    const auto motion_y_resolved_texture = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::sampled |
            gpu::TextureUsage::transfer_source}));
    const auto encode_float_bytes = [](std::span<const float> values) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(values.size() * sizeof(float));
        for (const float value : values) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned shift = 0U; shift < 32U; shift += 8U) {
                bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
            }
        }
        return bytes;
    };
    const auto upload_float_texture = [&](gpu::TextureHandle texture_handle,
            std::uint32_t channels, const std::vector<std::uint8_t>& tight_bytes) {
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(input.width) *
            channels * sizeof(float);
        const std::uint64_t row_pitch = (row_bytes + 255U) & ~std::uint64_t{255U};
        std::vector<std::uint8_t> staging(
            static_cast<std::size_t>(row_pitch * input.height), 0U);
        for (std::uint32_t row = 0U; row < input.height; ++row) {
            const std::size_t source_offset =
                static_cast<std::size_t>(row_bytes) * row;
            const std::size_t destination_offset =
                static_cast<std::size_t>(row_pitch) * row;
            std::copy_n(tight_bytes.begin() +
                    static_cast<std::ptrdiff_t>(source_offset),
                static_cast<std::size_t>(row_bytes),
                staging.begin() + static_cast<std::ptrdiff_t>(destination_offset));
        }
        const auto upload_buffer = take(device.create_buffer({staging.size(),
            gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source}));
        require(static_cast<bool>(device.write_buffer(upload_buffer, 0U,
                std::span<const std::uint8_t>{staging})),
            "temporal fixture writes aligned float texture staging");
        device_ir::DeviceCommandStream upload_stream;
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{upload_buffer},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_source});
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture_handle},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_destination});
        upload_stream.append(device_ir::CmdCopyBufferToTexture{
            upload_buffer, texture_handle, 0U, 0U});
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture_handle},
            device_ir::ResourceState::copy_destination,
            device_ir::ResourceState::shader_read});
        const auto upload_serial = take(device.submit(
            upload_stream, gpu::QueueType::graphics));
        require(static_cast<bool>(device.wait(upload_serial)),
            "temporal fixture waits for float texture upload");
        require(static_cast<bool>(device.destroy_buffer(upload_buffer)),
            "temporal fixture releases completed upload staging");
    };
    const auto motion_history_bytes =
        encode_float_bytes(std::span<const float>{current.linear_hdr_rgba});
    upload_float_texture(motion_history, 4U, motion_history_bytes);
    std::vector<float> motion_values(
        static_cast<std::size_t>(input.width) * input.height * 2U);
    for (std::size_t index = 0U; index < motion_values.size(); index += 2U) {
        motion_values[index] = 1.0F;
        motion_values[index + 1U] = 0.0F;
    }
    const auto motion_bytes =
        encode_float_bytes(std::span<const float>{motion_values});
    upload_float_texture(motion_vectors, 2U, motion_bytes);
    for (std::size_t index = 0U; index < motion_values.size(); index += 2U) {
        motion_values[index] = 0.0F;
        motion_values[index + 1U] = 1.0F;
    }
    const auto motion_y_bytes =
        encode_float_bytes(std::span<const float>{motion_values});
    upload_float_texture(motion_vectors_y, 2U, motion_y_bytes);

    auto motion_request = input;
    motion_request.temporal = d3d12::SceneRenderTemporalInputs{};
    motion_request.temporal->history_color = motion_history;
    motion_request.temporal->history_initial_state =
        device_ir::ResourceState::shader_read;
    motion_request.temporal->history_content_digest =
        assets::sha256(std::span<const std::uint8_t>{motion_history_bytes});
    motion_request.temporal->history_request_digest = current.request_digest;
    motion_request.temporal->history_source_revision = input.source_revision;
    motion_request.temporal->history_source_digest = input.source_digest;
    motion_request.temporal->motion_vectors = motion_vectors;
    motion_request.temporal->motion_initial_state =
        device_ir::ResourceState::shader_read;
    motion_request.temporal->motion_vectors_digest =
        assets::sha256(std::span<const std::uint8_t>{motion_bytes});
    motion_request.temporal->motion_history_request_digest =
        current.request_digest;
    motion_request.temporal->resolved_color = motion_resolved;
    motion_request.temporal->feedback = 0.75F;
    const auto motion_resolved_image = take(d3d12::render_scene(
        device, motion_request, CARTO_D3D12_SHADER_DIR));
    float maximum_motion_error = 0.0F;
    bool motion_sign_distinguishable = false;
    for (std::uint32_t y = 0U; y < input.height; ++y) {
        for (std::uint32_t x = 0U; x < input.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * input.width + x) * 4U;
            const bool history_in_bounds = x + 1U < input.width;
            const std::size_t positive_history = history_in_bounds
                ? pixel + 4U : pixel;
            const std::size_t negative_history = x > 0U ? pixel - 4U : pixel;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const float current_value = current.linear_hdr_rgba[pixel + channel];
                const float positive_value =
                    current.linear_hdr_rgba[positive_history + channel];
                const float negative_value =
                    current.linear_hdr_rgba[negative_history + channel];
                const float expected = history_in_bounds
                    ? current_value * 0.25F + positive_value * 0.75F
                    : current_value;
                maximum_motion_error = std::max(maximum_motion_error,
                    std::abs(motion_resolved_image.linear_hdr_rgba[pixel + channel] -
                        expected));
                if (history_in_bounds && x > 0U &&
                    std::abs(positive_value - negative_value) > 0.001F) {
                    motion_sign_distinguishable = true;
                }
            }
        }
    }
    require(motion_sign_distinguishable && maximum_motion_error < 0.001F,
        "positive current-to-history X motion samples the pixel to the right");

    auto subpixel_jitter_request = motion_request;
    subpixel_jitter_request.temporal->motion_vectors = {};
    subpixel_jitter_request.temporal->motion_initial_state =
        device_ir::ResourceState::undefined;
    subpixel_jitter_request.temporal->motion_vectors_digest = {};
    subpixel_jitter_request.temporal->constant_motion_pixels =
        core::Vec2d{0.25, 0.0};
    subpixel_jitter_request.temporal->resolved_initial_state =
        device_ir::ResourceState::copy_source;
    const auto subpixel_jitter = take(d3d12::render_scene(device,
        subpixel_jitter_request, CARTO_D3D12_SHADER_DIR));
    float maximum_subpixel_jitter_error = 0.0F;
    bool subpixel_jitter_is_distinguishable = false;
    for (std::uint32_t y = 0U; y < input.height; ++y) {
        for (std::uint32_t x = 0U; x < input.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * input.width + x) * 4U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                float expected = current.linear_hdr_rgba[pixel + channel];
                if (static_cast<double>(x) + 0.25 <=
                        static_cast<double>(input.width - 1U)) {
                    const std::size_t next_pixel = pixel + 4U;
                    float neighborhood_min = expected;
                    float neighborhood_max = expected;
                    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
                        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                            const int neighbor_x = static_cast<int>(x) + offset_x;
                            const int neighbor_y = static_cast<int>(y) + offset_y;
                            if (neighbor_x < 0 || neighbor_y < 0 ||
                                neighbor_x >= static_cast<int>(input.width) ||
                                neighbor_y >= static_cast<int>(input.height)) {
                                continue;
                            }
                            const std::size_t neighbor =
                                (static_cast<std::size_t>(neighbor_y) * input.width +
                                 static_cast<std::size_t>(neighbor_x)) * 4U + channel;
                            const float sample = current.linear_hdr_rgba[neighbor];
                            if (!std::isfinite(sample)) continue;
                            neighborhood_min = std::min(neighborhood_min, sample);
                            neighborhood_max = std::max(neighborhood_max, sample);
                        }
                    }
                    const float interpolated_history =
                        current.linear_hdr_rgba[pixel + channel] * 0.75F +
                        current.linear_hdr_rgba[next_pixel + channel] * 0.25F;
                    const float bounded_history = std::clamp(interpolated_history,
                        neighborhood_min, neighborhood_max);
                    subpixel_jitter_is_distinguishable =
                        subpixel_jitter_is_distinguishable ||
                        std::abs(bounded_history - current.linear_hdr_rgba[pixel + channel]) >
                            0.001F;
                    expected = current.linear_hdr_rgba[pixel + channel] * 0.25F +
                        bounded_history * 0.75F;
                }
                maximum_subpixel_jitter_error = std::max(
                    maximum_subpixel_jitter_error,
                    std::abs(subpixel_jitter.linear_hdr_rgba[pixel + channel] - expected));
            }
        }
    }
    require(subpixel_jitter_is_distinguishable &&
            maximum_subpixel_jitter_error < 0.001F,
        "quarter-pixel constant motion bilinearly reprojects history");

    auto motion_y_request = motion_request;
    motion_y_request.temporal->motion_vectors = motion_vectors_y;
    motion_y_request.temporal->motion_vectors_digest =
        assets::sha256(std::span<const std::uint8_t>{motion_y_bytes});
    motion_y_request.temporal->resolved_color = motion_y_resolved_texture;
    const auto motion_y_resolved_image = take(d3d12::render_scene(device,
        motion_y_request, CARTO_D3D12_SHADER_DIR));
    float maximum_motion_y_error = 0.0F;
    bool motion_y_sign_distinguishable = false;
    for (std::uint32_t y = 0U; y < input.height; ++y) {
        for (std::uint32_t x = 0U; x < input.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * input.width + x) * 4U;
            const bool history_in_bounds = y + 1U < input.height;
            const std::size_t positive_history = history_in_bounds
                ? pixel + static_cast<std::size_t>(input.width) * 4U : pixel;
            const std::size_t negative_history = y > 0U
                ? pixel - static_cast<std::size_t>(input.width) * 4U : pixel;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const float current_value = current.linear_hdr_rgba[pixel + channel];
                const float positive_value =
                    current.linear_hdr_rgba[positive_history + channel];
                const float negative_value =
                    current.linear_hdr_rgba[negative_history + channel];
                const float expected = history_in_bounds
                    ? current_value * 0.25F + positive_value * 0.75F
                    : current_value;
                maximum_motion_y_error = std::max(maximum_motion_y_error,
                    std::abs(motion_y_resolved_image.linear_hdr_rgba[pixel + channel] -
                        expected));
                if (history_in_bounds && y > 0U &&
                    std::abs(positive_value - negative_value) > 0.001F) {
                    motion_y_sign_distinguishable = true;
                }
            }
        }
    }
    require(motion_y_sign_distinguishable && maximum_motion_y_error < 0.001F,
        "positive current-to-history Y motion samples the row below in top-left pixel space");

    const auto direct_target = take(device.create_texture({input.width, input.height,
        1U, 1U, 1U, gpu::Format::bgra8_unorm,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source}));
    auto direct_temporal = accumulation_request;
    direct_temporal.temporal->history_initial_state =
        device_ir::ResourceState::shader_read;
    direct_temporal.temporal->resolved_initial_state =
        device_ir::ResourceState::copy_source;
    const auto direct_receipt = take(d3d12::render_scene_to_texture(device,
        direct_temporal, CARTO_D3D12_SHADER_DIR, direct_target,
        device_ir::ResourceState::undefined));
    require(direct_receipt.temporal_output_state ==
                device_ir::ResourceState::shader_read &&
            direct_receipt.request_digest != accumulated.request_digest,
        "direct-target receipt binds its temporal input state and output state");

    auto stale_motion = accumulation_request;
    stale_motion.temporal->zero_motion = false;
    stale_motion.temporal->history_initial_state =
        device_ir::ResourceState::shader_read;
    stale_motion.temporal->resolved_initial_state =
        device_ir::ResourceState::shader_read;
    stale_motion.temporal->motion_vectors = take(device.create_texture({1U, 1U,
        1U, 1U, 1U, gpu::Format::rg32_float,
        gpu::TextureDimension::texture_2d, gpu::TextureUsage::sampled}));
    stale_motion.temporal->motion_initial_state =
        device_ir::ResourceState::shader_read;
    stale_motion.temporal->motion_vectors_digest = hash("wrong-extent-motion");
    stale_motion.temporal->motion_history_request_digest =
        stale_motion.temporal->history_request_digest;
    const auto before_bad_extent = device.completed_serial();
    require(!d3d12::render_scene(device, stale_motion, CARTO_D3D12_SHADER_DIR) &&
            device.completed_serial() == before_bad_extent,
        "stale motion-vector extent is refused before GPU submission");

    auto wrong_motion_format = accumulation_request;
    wrong_motion_format.temporal->zero_motion = false;
    wrong_motion_format.temporal->history_initial_state =
        device_ir::ResourceState::shader_read;
    wrong_motion_format.temporal->resolved_initial_state =
        device_ir::ResourceState::shader_read;
    wrong_motion_format.temporal->motion_vectors = take(device.create_texture({
        input.width, input.height, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d, gpu::TextureUsage::sampled}));
    wrong_motion_format.temporal->motion_initial_state =
        device_ir::ResourceState::shader_read;
    wrong_motion_format.temporal->motion_vectors_digest =
        hash("wrong-format-motion");
    wrong_motion_format.temporal->motion_history_request_digest =
        wrong_motion_format.temporal->history_request_digest;
    const auto before_bad_format = device.completed_serial();
    require(!d3d12::render_scene(device, wrong_motion_format,
            CARTO_D3D12_SHADER_DIR) &&
            device.completed_serial() == before_bad_format,
        "wrong motion-vector format is refused before GPU submission");

    require(static_cast<bool>(device.destroy_texture(history)) &&
            static_cast<bool>(device.destroy_texture(resolved)) &&
            static_cast<bool>(device.destroy_texture(motion_history)) &&
            static_cast<bool>(device.destroy_texture(motion_vectors)) &&
            static_cast<bool>(device.destroy_texture(motion_vectors_y)) &&
            static_cast<bool>(device.destroy_texture(motion_resolved)) &&
            static_cast<bool>(device.destroy_texture(motion_y_resolved_texture)) &&
            static_cast<bool>(device.destroy_texture(direct_target)) &&
            static_cast<bool>(device.destroy_texture(
                stale_motion.temporal->motion_vectors)) &&
            static_cast<bool>(device.destroy_texture(
                wrong_motion_format.temporal->motion_vectors)),
        "temporal test destroys only its caller-owned fixtures");
}
void graph_occlusion_values_reach_native_pixels(d3d12::D3D12Device& device) {
    for (double occlusion : {0.0, 0.25, 1.0}) {
        auto document = room_project(); auto catalog = document.material_catalog();
        project::MaterialGraph graph; graph.identity = "fixture/occlusion"; graph.revision = core::Revision{1U};
        project::MaterialGraphNode color; color.id = 1U; color.kind = project::MaterialGraphNodeKind::constant_color;
        color.value_type = project::MaterialGraphValueType::color; color.constants = {1.0, 1.0, 1.0, 1.0};
        project::MaterialGraphNode one; one.id = 2U; one.constants[0U] = 1.0;
        project::MaterialGraphNode value; value.id = 3U; value.constants[0U] = occlusion;
        project::MaterialGraphNode zero; zero.id = 4U;
        graph.nodes = {color, one, value, zero}; graph.outputs.base_color = 1U;
        graph.outputs.alpha = 2U; graph.outputs.roughness = 2U;
        graph.outputs.metallic = 4U; graph.outputs.occlusion = 3U;
        require(static_cast<bool>(catalog.insert_material_graph(graph)) &&
            static_cast<bool>(catalog.bind_material_graph(1U, graph.identity)) &&
            static_cast<bool>(document.set_material_catalog(std::move(catalog))), "source graph occlusion fixture admits");
        const auto before = document.serialize();
        d3d12::SceneRenderCamera camera; camera.position = {0.0, 5.0, 3.0}; camera.target = {0.0, -0.1, 0.0};
        auto prepared = take(d3d12::prepare_project_scene_render("graph-occlusion", document, camera, 96U, 96U, {},
            {{0.0, 1.0, 0.0}, {0.0, 0.0, 0.0}}));
        std::erase_if(prepared.objects, [&](const auto& rendered) {
            return document.scene().find(rendered.object)->name != "Floor";
        });
        const auto image = capture(device, prepared);
        const std::size_t center = (48U * 96U + 48U) * 4U;
        require(image.depth[center / 4U] < 1.0F, "occlusion test covers actual floor geometry");
        for (std::size_t channel = 0U; channel < 3U; ++channel)
            require(std::abs(image.linear_hdr_rgba[center + channel] - 0.03 * occlusion) < 0.000001,
                "graph occlusion must affect native ambient pixels, not only prepared metadata");
        require(document.serialize() == before, "native graph rendering never edits the source project");
    }
    auto bound = request(); bound.light.radiance = {};
    auto& material = bound.objects[0U].materials[0U].current_material.definition;
    material.base_color_factor = {1.0, 1.0, 1.0, 1.0};
    material.occlusion.asset = 52U; material.occlusion_strength = 1.0;
    rebind(bound.objects[0U]); bound.textures.push_back(texture(52U, {0U, 0U, 0U, 255U}, assets::TextureSemantic::occlusion));
    const auto textured_image = capture(device, bound);
    const std::size_t center = (48U * 96U + 48U) * 4U;
    require(textured_image.linear_hdr_rgba[center] < 0.000001F,
        "authored occlusion map uses the sign-tagged texture branch");
    material.occlusion_strength = 0.0;
    rebind(bound.objects[0U]);
    const auto image = capture(device, bound);
    require(std::abs(image.linear_hdr_rgba[center] - 0.03) < 0.000001,
        "zero authored texture strength still disables the bound occlusion map");
}

void graph_opacity_reaches_native_coverage(d3d12::D3D12Device& device) {
    for (const auto mode : {render::AlphaMode::opaque,
            render::AlphaMode::masked, render::AlphaMode::blended}) {
        const auto document = opacity_graph_project(0.5, 0.5, mode);
        const auto before = document.serialize();
        d3d12::SceneRenderCamera camera;
        camera.position = {0.0, 5.0, 3.0};
        camera.target = {0.0, -0.1, 0.0};
        const auto floor_request = [&](const project::ProjectDocument& source) {
            auto prepared = take(d3d12::prepare_project_scene_render(
                "graph-opacity", source, camera, 96U, 96U, {},
                {{0.0, 1.0, 0.0}, {0.0, 0.0, 0.0}}));
            std::erase_if(prepared.objects, [&](const auto& rendered) {
                return source.scene().find(rendered.object)->name != "Floor";
            });
            return prepared;
        };
        const auto prepared = floor_request(document);
        const auto actual = capture(device, prepared);
        auto explicit_material = document;
        auto catalog = explicit_material.material_catalog();
        auto material = catalog.materials().at(1U);
        material.definition.base_color_factor = {0.6, 0.4, 0.2, 0.25};
        material.definition.roughness = 0.5;
        material.definition.metallic = 0.0;
        material.revision = material.revision.next();
        take(catalog.unbind_material_graph(1U));
        take(catalog.replace_material(material));
        take(explicit_material.set_material_catalog(std::move(catalog)));
        const auto expected = capture(device, floor_request(explicit_material));
        require(actual.linear_hdr_rgba == expected.linear_hdr_rgba &&
                actual.depth == expected.depth,
            "graph opacity matches explicit factor coverage in the actual native shader path");
        const auto full_opacity = capture(device, floor_request(
            opacity_graph_project(0.5, 1.0, mode)));
        const std::size_t center = (48U * 96U + 48U) * 4U;
        if (mode == render::AlphaMode::opaque) {
            require(actual.depth[center / 4U] < 1.0F &&
                    actual.linear_hdr_rgba == full_opacity.linear_hdr_rgba,
                "authored opaque policy remains opaque rather than inferring a blend mode");
        } else if (mode == render::AlphaMode::masked) {
            require(actual.depth[center / 4U] == 1.0F &&
                    full_opacity.depth[center / 4U] < 1.0F,
                "graph opacity drives the existing authored cutoff and masked depth discard");
        } else {
            require(actual.depth[center / 4U] == 1.0F &&
                    actual.linear_hdr_rgba[center] > 0.0F &&
                    actual.linear_hdr_rgba[center] < full_opacity.linear_hdr_rgba[center],
                "graph opacity changes blended HDR coverage without writing opaque depth");
        }
        require(document.serialize() == before,
            "native opacity capture leaves all authored project bytes unchanged");
    }
}

void sheen_direct_lights_match_reference_and_invalidate_mesh_cache(
    d3d12::D3D12Device& device) {
    auto scene = request();
    scene.width = 97U;
    scene.height = 97U;
    scene.antialiasing = d3d12::SceneRenderAntialiasing::none;
    scene.light = {{0.6, 0.0, 0.8}, {3.0, 3.0, 3.0}};
    auto& binding = scene.objects.front().materials.front();
    binding.current_material.definition.base_color_factor = {0.6, 0.4, 0.2, 1.0};
    rebind(scene.objects.front());
    d3d12::SceneRenderCache cache;
    const auto render_cached = [&](const d3d12::SceneRenderRequest& input) {
        return take(d3d12::render_scene_cached(device, cache, input, CARTO_D3D12_SHADER_DIR));
    };
    const auto uncoated = render_cached(scene);
    const auto before = take(scene.digest());
    binding.sheen_weight = 1.0;
    const auto full = render_cached(scene);
    const auto fresh = capture(device, scene);
    require(take(scene.digest()) != before && full.linear_hdr_rgba == fresh.linear_hdr_rgba &&
            full.depth == fresh.depth && full.pixel_digest != uncoated.pixel_digest,
        "a sheen-only change rebuilds cached mesh payload and matches a fresh render");
    const auto reference = take(d3d12::evaluate_scene_sheen_reference(
        1.0, 0.65, 0.0, 0.8, 1.0, std::sqrt(0.9)));
    const std::size_t center = (48U * 97U + 48U) * 4U;
    const std::array<double, 3U> base{0.6, 0.4, 0.2};
    require(full.depth[center / 4U] < 1.0F,
        "odd-sized sheen fixture covers the exact camera-target surface sample");
    for (std::size_t channel = 0U; channel < base.size(); ++channel) {
        const double expected = base[channel] * (0.03 + reference.sheen_brdf * 0.8 * 3.0);
        require(std::abs(full.linear_hdr_rgba[center + channel] - expected) < 2.0e-4,
            "native directional sheen matches the CPU direct BRDF plus declared ambient");
    }
    binding.sheen_weight = 0.5;
    const auto half = render_cached(scene);
    for (std::size_t channel = 0U; channel < base.size(); ++channel)
        require(std::abs(half.linear_hdr_rgba[center + channel] -
                0.5F * (full.linear_hdr_rgba[center + channel] +
                    uncoated.linear_hdr_rgba[center + channel])) < 2.0e-5,
            "scalar sheen mixes linearly with the existing base at fixed surface factors");

    binding.sheen_weight = 1.0;
    auto local = scene;
    local.light.radiance = {};
    d3d12::SceneRenderLocalLight light;
    light.object = {901U};
    light.position = {3.0, 0.0, 4.0};
    light.emission_direction = {-0.6, 0.0, -0.8};
    light.range = 10.0;
    light.radiance = {3.0, 3.0, 3.0};
    light.inner_cone_radians = 0.2;
    light.outer_cone_radians = 0.6;
    light.casts_shadows = false;
    for (const auto kind : {d3d12::SceneRenderLocalLightKind::point,
            d3d12::SceneRenderLocalLightKind::spot}) {
        light.kind = kind;
        local.local_lights = {light};
        const auto image = render_cached(local);
        const double range_attenuation = (1.0 - 0.0625) * (1.0 - 0.0625) / 25.0;
        for (std::size_t channel = 0U; channel < base.size(); ++channel) {
            const double expected = base[channel] *
                (0.03 + reference.sheen_brdf * 0.8 * 3.0 * range_attenuation);
            require(std::abs(image.linear_hdr_rgba[center + channel] - expected) < 2.0e-4,
                "point and spot sheen apply existing distance and cone attenuation exactly once");
        }
    }
    binding.current_material.definition.metallic = 0.5;
    binding.sheen_weight = 0.0;
    rebind(scene.objects.front());
    const auto mixed_metal_base = render_cached(scene);
    binding.sheen_weight = 1.0;
    const auto mixed_metal_sheen = render_cached(scene);
    for (std::size_t channel = 0U; channel < base.size(); ++channel)
        require(std::abs(mixed_metal_sheen.linear_hdr_rgba[center + channel] -
                0.5F * (mixed_metal_base.linear_hdr_rgba[center + channel] +
                    full.linear_hdr_rgba[center + channel])) < 2.0e-5,
            "fractional metallic suppresses the mixture weight over the complete base lobe");
    binding.current_material.definition.metallic = 0.0;
    binding.current_material.definition.clearcoat = 0.5;
    binding.sheen_weight = 0.0;
    rebind(scene.objects.front());
    const auto coated_base = render_cached(scene);
    binding.sheen_weight = 1.0;
    const auto coated_sheen = render_cached(scene);
    const double coat_attenuation = 1.0 - 0.5 *
        (0.04 + 0.96 * std::pow(1.0 - std::sqrt(0.9), 5.0));
    for (std::size_t channel = 0U; channel < base.size(); ++channel)
        require(std::abs((coated_sheen.linear_hdr_rgba[center + channel] -
                    coated_base.linear_hdr_rgba[center + channel]) - coat_attenuation *
                (full.linear_hdr_rgba[center + channel] -
                    uncoated.linear_hdr_rgba[center + channel])) < 2.0e-4,
            "clearcoat attenuates both mixed base lobes while retaining its own outer reflection");
    binding.current_material.definition.clearcoat = 0.0;
    binding.current_material.definition.metallic = 1.0;
    rebind(scene.objects.front());
    const auto metal_sheen = render_cached(scene);
    binding.sheen_weight = 0.0;
    const auto metal_base = render_cached(scene);
    require(metal_sheen.linear_hdr_rgba == metal_base.linear_hdr_rgba,
        "full-metal shading remains unchanged by the suppressed sheen mixture");
    require(std::any_of(full.limitations.begin(), full.limitations.end(),
            [](const std::string& limitation) { return limitation.find("no sheen IBL") != std::string::npos; }),
        "native sheen receipt discloses the missing environment integration");
}

void anisotropy_direct_lights_match_reference_and_authored_axes(
    d3d12::D3D12Device& device) {
    auto scene = request();
    scene.width = 97U;
    scene.height = 97U;
    scene.antialiasing = d3d12::SceneRenderAntialiasing::none;
    scene.light = {{0.6, 0.0, 0.8}, {3.0, 3.0, 3.0}};
    auto& binding = scene.objects.front().materials.front();
    binding.current_material.definition.base_color_factor = {0.6, 0.4, 0.2, 1.0};
    binding.current_material.definition.metallic = 1.0;
    rebind(scene.objects.front());
    d3d12::SceneRenderCache cache;
    const auto render_cached = [&](const d3d12::SceneRenderRequest& input) {
        return take(d3d12::render_scene_cached(device, cache, input, CARTO_D3D12_SHADER_DIR));
    };
    const auto isotropic = render_cached(scene);
    const auto source_digest = scene.source_digest;
    const auto generic_material_digest = binding.material.artifact_digest;
    const std::size_t center = (48U * 97U + 48U) * 4U;
    const std::array<double, 3U> base{0.6, 0.4, 0.2};
    const double fresnel_power = std::pow(1.0 - std::sqrt(0.9), 5.0);
    for (const double strength : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
        binding.anisotropy = strength;
        const auto image = render_cached(scene);
        const auto reference = take(d3d12::evaluate_scene_anisotropy_reference(
            0.65, strength, {0.0, 0.0, 1.0}, {0.6, 0.0, 0.8}));
        require(image.depth[center / 4U] < 1.0F, "anisotropy reference sample covers actual geometry");
        for (std::size_t channel = 0U; channel < base.size(); ++channel) {
            const double fresnel = base[channel] + (1.0 - base[channel]) * fresnel_power;
            const double expected = 0.03 * base[channel] +
                fresnel * reference.mixed_kernel * 0.8 * 3.0;
            require(std::abs(image.linear_hdr_rgba[center + channel] - expected) < 2.0e-4,
                "native signed anisotropy matches the declared mixed specular kernel");
        }
        if (strength == 0.0)
            require(image.linear_hdr_rgba == isotropic.linear_hdr_rgba,
                "returning to zero anisotropy restores the exact legacy cached rendering");
    }
    const auto positive = render_cached(scene);
    binding.anisotropy = -1.0;
    const auto negative = render_cached(scene);
    const auto fresh = capture(device, scene);
    require(negative.linear_hdr_rgba == fresh.linear_hdr_rgba && negative.depth == fresh.depth &&
            std::abs(negative.linear_hdr_rgba[center] - positive.linear_hdr_rgba[center]) > 0.001F &&
            scene.source_digest == source_digest && binding.material.artifact_digest == generic_material_digest,
        "anisotropy-only sign changes invalidate uploaded axes without relying on generic artifact changes");
    auto rotated_frame = scene;
    rotated_frame.objects.front().materials.front().anisotropy = 1.0;
    for (auto& vertex : rotated_frame.objects.front().mesh.vertices)
        vertex.tangent = core::Vec4d{0.0, 1.0, 0.0, 1.0};
    rebind(rotated_frame.objects.front());
    const auto rotated = render_cached(rotated_frame);
    for (std::size_t channel = 0U; channel < base.size(); ++channel)
        require(std::abs(rotated.linear_hdr_rgba[center + channel] -
                negative.linear_hdr_rgba[center + channel]) < 2.0e-5,
            "rotating the authored tangent frame swaps the anisotropic highlight axes");

    auto local = scene;
    local.light.radiance = {};
    d3d12::SceneRenderLocalLight light;
    light.object = {902U};
    light.position = {3.0, 0.0, 4.0};
    light.emission_direction = {-0.6, 0.0, -0.8};
    light.radiance = {3.0, 3.0, 3.0};
    light.range = 10.0;
    light.inner_cone_radians = 0.2;
    light.outer_cone_radians = 0.6;
    light.casts_shadows = false;
    const auto reference = take(d3d12::evaluate_scene_anisotropy_reference(
        0.65, -1.0, {0.0, 0.0, 1.0}, {0.6, 0.0, 0.8}));
    const double range_attenuation = (1.0 - 0.0625) * (1.0 - 0.0625) / 25.0;
    for (const auto kind : {d3d12::SceneRenderLocalLightKind::point,
            d3d12::SceneRenderLocalLightKind::spot}) {
        light.kind = kind;
        local.local_lights = {light};
        const auto image = render_cached(local);
        for (std::size_t channel = 0U; channel < base.size(); ++channel) {
            const double fresnel = base[channel] + (1.0 - base[channel]) * fresnel_power;
            const double expected = 0.03 * base[channel] +
                fresnel * reference.mixed_kernel * 0.8 * 3.0 * range_attenuation;
            require(std::abs(image.linear_hdr_rgba[center + channel] - expected) < 2.0e-4,
                "point and spot paths use the same anisotropic kernel and existing attenuation");
        }
    }
    require(std::any_of(positive.limitations.begin(), positive.limitations.end(),
            [](const std::string& limitation) { return limitation.find("anisotropic IBL") != std::string::npos; }),
        "native anisotropy receipts disclose absent environment integration");
}

void anisotropic_normal_maps_match_transported_reference_and_cache(
    d3d12::D3D12Device& device) {
    auto scene = request();
    scene.width = 97U;
    scene.height = 97U;
    scene.antialiasing = d3d12::SceneRenderAntialiasing::none;
    const core::Vec3d world_view{0.0, 0.0, 1.0};
    const core::Vec3d world_light{0.48, 0.36, 0.8};
    scene.light = {world_light, {3.0, 3.0, 3.0}};
    auto& binding = scene.objects.front().materials.front();
    auto& material = binding.current_material.definition;
    material.base_color_factor = {0.6, 0.4, 0.2, 1.0};
    material.metallic = 1.0;
    material.normal.asset = 99U;
    material.normal_scale = 1.25;
    d3d12::SceneRenderCache cache;
    const auto render_cached = [&](const d3d12::SceneRenderRequest& input) {
        return take(d3d12::render_scene_cached(device, cache, input, CARTO_D3D12_SHADER_DIR));
    };
    const std::size_t center = (48U * 97U + 48U) * 4U;
    const std::array<double, 3U> base{0.6, 0.4, 0.2};
    const auto verify = [&](const d3d12::SceneRenderRequest& input,
                            const d3d12::SceneRenderImage& image, double attenuation) {
        const auto& factors = input.objects.front().materials.front();
        const auto& definition = factors.current_material.definition;
        const auto& pixels = input.textures.front().mips.levels.front().pixels;
        const double sign = definition.normal.normal_convention == render::NormalConvention::opengl_y_plus
            ? -1.0 : 1.0;
        // Decode the actual source-bound mip texel, including quantization and
        // existing convention/normal-scale semantics, rather than an ideal normal.
        const auto normal = core::Vec3d{
            (2.0 * pixels[0U] / 255.0 - 1.0) * definition.normal_scale,
            -(2.0 * pixels[1U] / 255.0 - 1.0) * definition.normal_scale * sign,
            2.0 * pixels[2U] / 255.0 - 1.0}.normalized();
        const auto frame = take(d3d12::transport_scene_anisotropy_frame(
            {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, sign, normal));
        const auto local = [&](core::Vec3d direction) {
            return core::Vec3d{core::dot(frame.tangent, direction),
                core::dot(frame.bitangent, direction), core::dot(normal, direction)};
        };
        const auto reference = take(d3d12::evaluate_scene_anisotropy_reference(
            definition.roughness, factors.anisotropy, local(world_view), local(world_light)));
        const auto half = (world_view + world_light).normalized();
        const double fresnel_power = std::pow(1.0 - core::dot(world_view, half), 5.0);
        require(image.depth[center / 4U] < 1.0F, "mapped-normal reference pixel covers geometry");
        for (std::size_t channel = 0U; channel < base.size(); ++channel) {
            const double fresnel = base[channel] + (1.0 - base[channel]) * fresnel_power;
            const double expected = 0.03 * base[channel] + fresnel * reference.mixed_kernel *
                std::max(0.0, core::dot(normal, world_light)) * 3.0 * attenuation;
            require(std::isfinite(image.linear_hdr_rgba[center + channel]) &&
                    std::abs(image.linear_hdr_rgba[center + channel] - expected) < 3.0e-4,
                "mapped signed anisotropy matches transported-frame direct-light reference pixels");
        }
    };
    for (const auto rgba : {std::array<std::uint8_t, 4U>{180U, 80U, 240U, 255U},
            std::array<std::uint8_t, 4U>{255U, 128U, 128U, 255U}}) {
        scene.textures = {texture(99U, rgba, assets::TextureSemantic::normal)};
        for (const auto convention : {render::NormalConvention::directx_y_minus,
                render::NormalConvention::opengl_y_plus}) {
            material.normal.normal_convention = convention;
            binding.anisotropy = 0.0;
            rebind(scene.objects.front());
            const auto isotropic = render_cached(scene);
            for (const double strength : {-1.0, -0.5, 0.5, 1.0, 0.0}) {
                binding.anisotropy = strength;
                const auto image = render_cached(scene);
                verify(scene, image, 1.0);
                if (strength == 0.0)
                    require(image.linear_hdr_rgba == isotropic.linear_hdr_rgba && image.depth == isotropic.depth,
                        "zero anisotropy restores exact mapped-normal legacy pixels after cache reuse");
            }
            binding.anisotropy = 0.7;
            const auto cached = render_cached(scene);
            const auto fresh = capture(device, scene);
            require(cached.linear_hdr_rgba == fresh.linear_hdr_rgba && cached.depth == fresh.depth,
                "normal-source and convention-only changes agree between reused and fresh resources");
        }
    }
    scene.textures = {texture(99U, {180U, 80U, 240U, 255U}, assets::TextureSemantic::normal)};
    material.normal.normal_convention = render::NormalConvention::directx_y_minus;
    rebind(scene.objects.front());
    const auto directx = render_cached(scene);
    const auto directx_digest = take(scene.digest());
    material.normal.normal_convention = render::NormalConvention::opengl_y_plus;
    rebind(scene.objects.front());
    const auto opengl = render_cached(scene);
    const auto opengl_fresh = capture(device, scene);
    require(take(scene.digest()) != directx_digest && directx.pixel_digest != opengl.pixel_digest &&
            opengl.linear_hdr_rgba == opengl_fresh.linear_hdr_rgba,
        "convention-only edits change the actual mapped normal and invalidate cached upload factors");
    verify(scene, opengl, 1.0);

    const auto original_normal = scene.textures.front();
    const auto unchanged_material_digest = binding.material.artifact_digest;
    scene.textures.front() = texture(99U, {64U, 190U, 240U, 255U}, assets::TextureSemantic::normal);
    const auto changed_normal = render_cached(scene);
    const auto changed_normal_fresh = capture(device, scene);
    require(binding.material.artifact_digest == unchanged_material_digest &&
            changed_normal.pixel_digest != opengl.pixel_digest &&
            changed_normal.linear_hdr_rgba == changed_normal_fresh.linear_hdr_rgba &&
            changed_normal.depth == changed_normal_fresh.depth,
        "normal-source-only changes refresh cached texture bytes at fixed material and anisotropy");
    verify(scene, changed_normal, 1.0);
    scene.textures.front() = original_normal;
    require(render_cached(scene).linear_hdr_rgba == opengl.linear_hdr_rgba,
        "restoring normal texture bytes restores exact cached mapped-anisotropy shading");

    material.normal.transform.scale = {2.0, 0.75};
    material.normal.transform.offset = {0.2, -0.5};
    material.normal.transform.pivot = {0.1, 0.9};
    rebind(scene.objects.front());
    const auto transformed = render_cached(scene);
    require(transformed.linear_hdr_rgba == opengl.linear_hdr_rgba,
        "positive axis-aligned normal transforms do not rotate anisotropy on a constant texel fixture");

    scene.light.radiance = {};
    d3d12::SceneRenderLocalLight light;
    light.object = {903U};
    light.position = {2.4, 1.8, 4.0};
    light.emission_direction = world_light * -1.0;
    light.radiance = {3.0, 3.0, 3.0};
    light.range = 10.0;
    light.inner_cone_radians = 0.2;
    light.outer_cone_radians = 0.6;
    for (const auto kind : {d3d12::SceneRenderLocalLightKind::point, d3d12::SceneRenderLocalLightKind::spot}) {
        light.kind = kind;
        scene.local_lights = {light};
        for (const double strength : {-1.0, 1.0}) {
            binding.anisotropy = strength;
            verify(scene, render_cached(scene), (1.0 - 0.0625) * (1.0 - 0.0625) / 25.0);
        }
    }
    require(std::any_of(transformed.limitations.begin(), transformed.limitations.end(),
            [](const std::string& limitation) { return limitation.find("antiparallel") != std::string::npos; }),
        "native receipt discloses bounded normal-map orientation and the antiparallel frame choice");
}

void viewport_camera_rebuilds_pan_from_immutable_base_and_resets_home() {
    d3d12::SceneRenderCamera base;
    base.position = {0.0, 0.0, 10.0};
    base.target = {1.0, 2.0, 3.0};
    d3d12::ViewportInputState input;
    require(input.begin_pan(40, 40) && input.move_pointer(60, 52),
        "pan input captures a bounded pointer delta");
    const auto first = take(d3d12::apply_viewport_camera_input(
        base, 10.0, 800U, input));
    const auto repeated = take(d3d12::apply_viewport_camera_input(
        base, 10.0, 800U, input));
    require(first.target.x == repeated.target.x &&
            first.target.y == repeated.target.y &&
            first.target.z == repeated.target.z &&
            first.position.x == repeated.position.x &&
            first.position.y == repeated.position.y &&
            first.position.z == repeated.position.z,
        "repeated frames rebuild the same pan from the immutable camera base");
    require(first.target.x != base.target.x || first.target.y != base.target.y ||
            first.target.z != base.target.z,
        "captured pan moves the rendered camera target");
    require(input.adjust(d3d12::ViewportNavigationKey::frame_all),
        "Home clears the captured pan");
    const auto reset = take(d3d12::apply_viewport_camera_input(
        base, 10.0, 800U, input));
    require(reset.target.x == base.target.x && reset.target.y == base.target.y &&
            reset.target.z == base.target.z,
        "Home restores the original unpanned target");
}

void orthographic_viewport_zoom_changes_projection_height() {
    d3d12::SceneRenderCamera base;
    base.position = {0.0, 0.0, 10.0};
    base.target = {};
    base.projection = d3d12::SceneRenderProjection::orthographic;
    base.orthographic_height = 8.0;
    d3d12::ViewportInputState input;
    require(input.adjust(d3d12::ViewportNavigationKey::zoom_in),
        "orthographic viewport accepts bounded zoom input");
    const auto zoomed = take(d3d12::apply_viewport_camera_input(
        base, 10.0, 800U, input));
    require(zoomed.orthographic_height < base.orthographic_height &&
            zoomed.position.z == base.position.z,
        "orthographic zoom changes visible height without dollying the camera");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
            take(room_project().save_atomic(argv[2])); return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) ==
                "--write-local-light-fixture") {
            auto document = room_project();
            core::Transform transform;
            transform.translation = {1.25, 2.4, -2.1};
            const auto light_object = take(
                project::testing::access(document).create_object(
                    "Saved point light", transform));
            project::AuthoredLight light;
            light.object = light_object;
            light.identity = "fixture-render-point";
            light.kind = project::AuthoredLightKind::point;
            light.linear_color = {0.5, 0.25, 1.0};
            light.intensity = 3.0;
            light.range = 6.0;
            light.casts_shadows = false;
            auto catalog = document.presentation_catalog();
            require(static_cast<bool>(catalog.insert_light(light)),
                "CLI fixture admits a saved unshadowed point light");
            core::Transform spot_transform;
            spot_transform.translation = {-1.0, 2.2, -1.5};
            const auto spot_object = take(
                project::testing::access(document).create_object(
                    "Saved spot light", spot_transform));
            project::AuthoredLight spot;
            spot.object = spot_object;
            spot.identity = "fixture-render-spot";
            spot.kind = project::AuthoredLightKind::spot;
            spot.linear_color = {1.0, 0.5, 0.25};
            spot.intensity = 2.0;
            spot.range = 8.0;
            spot.inner_cone_radians = 0.25;
            spot.outer_cone_radians = 0.5;
            spot.casts_shadows = true;
            require(static_cast<bool>(catalog.insert_light(spot)) &&
                static_cast<bool>(document.set_presentation_catalog(
                    std::move(catalog))),
                "CLI fixture admits a saved unshadowed spot light");
            take(document.save_atomic(argv[2]));
            std::ofstream identity_file(std::string(argv[2]) + ".light-id");
            identity_file << light_object.value;
            identity_file.close();
            require(static_cast<bool>(identity_file),
                "CLI fixture writes the point-light source identity");
            std::ofstream spot_identity_file(std::string(argv[2]) + ".spot-id");
            spot_identity_file << spot_object.value;
            spot_identity_file.close();
            require(static_cast<bool>(spot_identity_file),
                "CLI fixture writes the spot-light source identity");
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) ==
                "--write-authored-camera-fixture") {
            auto document = room_project();
            core::Transform transform;
            transform.translation = {0.0, 2.0, 6.0};
            const double half_angle = std::atan(1.0 / 3.0) * 0.5;
            transform.rotation = {std::sin(half_angle), 0.0, 0.0,
                std::cos(half_angle)};
            const auto camera_object = take(
                project::testing::access(document).create_object(
                    "Saved render camera", transform));
            project::AuthoredCamera camera;
            camera.object = camera_object;
            camera.identity = "fixture-render-camera";
            camera.vertical_fov_radians = 0.9;
            camera.near_plane = 0.02;
            camera.far_plane = 500.0;
            camera.revision = core::Revision{1U};
            auto catalog = document.presentation_catalog();
            core::Transform orthographic_transform;
            orthographic_transform.translation = {0.0, 2.0, 6.0};
            const auto orthographic_object = take(
                project::testing::access(document).create_object(
                    "Saved orthographic render camera", orthographic_transform));
            project::AuthoredCamera orthographic_camera;
            orthographic_camera.object = orthographic_object;
            orthographic_camera.identity = "fixture-orthographic-render-camera";
            orthographic_camera.projection = project::CameraProjection::orthographic;
            orthographic_camera.orthographic_height = 3.5;
            orthographic_camera.near_plane = 0.02;
            orthographic_camera.far_plane = 500.0;
            orthographic_camera.revision = core::Revision{1U};
            require(static_cast<bool>(catalog.insert_camera(camera)) &&
                static_cast<bool>(catalog.insert_camera(orthographic_camera)) &&
                static_cast<bool>(document.set_presentation_catalog(
                    std::move(catalog))),
                "CLI fixture admits authored perspective and orthographic cameras");
            take(document.save_atomic(argv[2]));
            std::ofstream identity_file(std::string(argv[2]) + ".camera-id");
            identity_file << camera_object.value;
            identity_file.close();
            require(static_cast<bool>(identity_file),
                "CLI fixture writes the camera object identity");
            std::ofstream orthographic_identity_file(
                std::string(argv[2]) + ".orthographic-camera-id");
            orthographic_identity_file << orthographic_object.value;
            orthographic_identity_file.close();
            require(static_cast<bool>(orthographic_identity_file),
                "CLI fixture writes the orthographic camera object identity");
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--write-parameterized-fixture") {
            take(parameterized_graph_project().save_atomic(argv[2])); return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "write-equirectangular") {
            const auto encoded = equirectangular_tga(
                8U, 4U, {64U, 128U, 192U, 255U});
            std::ofstream output(argv[2], std::ios::binary);
            output.write(reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::streamsize>(encoded.size()));
            output.close();
            require(static_cast<bool>(output),
                "fixture writes CLI equirectangular source");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--graph-opacity-only") {
            project_graph_opacity_preserves_coverage_policy_and_source();
            std::cout << "PASS graph opacity preparation and source boundaries (no GPU)\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--graph-sheen-only") {
            project_graph_neutral_physical_outputs_preserve_source();
            std::cout << "PASS exact-neutral physical outputs and nonzero refusal\n";
            scene_sheen_reference_is_bounded_and_reciprocal();
            project_graph_sheen_binds_source_and_refuses_missing_ibl();
            std::cout << "PASS graph sheen reference and source boundaries (no GPU)\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--graph-anisotropy-only") {
            scene_anisotropy_reference_preserves_axes_and_zero_boundary();
            project_graph_anisotropy_requires_source_bound_tangents();
            scene_anisotropy_frame_transport_preserves_orientation();
            project_graph_anisotropic_normal_maps_preserve_source_boundaries();
            std::cout << "PASS graph anisotropy reference and tangent boundaries (no GPU)\n";
            return 0;
        }
        temporal_sampling_is_deterministic_and_memory_bounded();
        std::cout << "PASS shared temporal sampling bounds and texture budget\n";
        viewport_camera_rebuilds_pan_from_immutable_base_and_resets_home();
        orthographic_viewport_zoom_changes_projection_height();
        std::cout << "PASS viewport camera base-state pan and projection zoom\n";
        // Keep the default full-suite gate, with an explicit diagnostic filter
        // to reproduce native failures independently of source-admission ones.
        if (!(argc == 2 && std::string_view(argv[1]) == "--native-only")) {
            diffuse_irradiance_sh_file_is_bounded_and_digest_bound();
            std::cout << "PASS diffuse irradiance SH file bounds and identity\n";
            equirectangular_environment_derives_digest_bound_sh();
            std::cout << "PASS equirectangular environment SH derivation\n";
            radiance_hdr_environment_preserves_linear_specular_mips();
            std::cout << "PASS linear Radiance HDR reflection mip binding\n";
            project_adapter_preserves_source_and_rejects_missing_authoring();
            std::cout << "PASS saved-project scene preparation, immutable source and reopen identity\n";
            project_graph_opacity_preserves_coverage_policy_and_source();
            std::cout << "PASS graph opacity composition, authored alpha policy and source pinning\n";
            project_graph_neutral_physical_outputs_preserve_source();
            std::cout << "PASS exact-neutral physical outputs and nonzero refusal\n";
            scene_sheen_reference_is_bounded_and_reciprocal();
            project_graph_sheen_binds_source_and_refuses_missing_ibl();
            std::cout << "PASS graph sheen reference, source identity and environment refusal\n";
            scene_anisotropy_reference_preserves_axes_and_zero_boundary();
            project_graph_anisotropy_requires_source_bound_tangents();
            scene_anisotropy_frame_transport_preserves_orientation();
            project_graph_anisotropic_normal_maps_preserve_source_boundaries();
            std::cout << "PASS graph anisotropy reference, source and tangent boundaries\n";
            authored_project_camera_resolves_world_pose_and_projection();
            std::cout << "PASS authored camera world-pose resolution and projection binding\n";
            project_scene_adapter_wires_supported_authored_lights_and_refuses_fallbacks();
            std::cout << "PASS saved-project directional/point/spot light wiring and refusal boundaries\n";
            project_scene_adapter_wires_unambiguous_authored_colors();
            std::cout << "PASS saved-project authored color wiring and ambiguity refusal\n";
            project_scene_texture_loader_binds_verified_sources_and_fails_closed();
            std::cout << "PASS project texture source verification, semantic mip binding and stale-byte refusal\n";
            masked_scene_textures_preserve_authored_alpha_coverage();
            std::cout << "PASS masked material source alpha coverage across semantic mips\n";
            rejects_stale_foreign_and_unsupported_inputs();
            std::cout << "PASS scene source/texture/camera/budget admission boundaries\n";
        }
        auto device = take(d3d12::D3D12Device::create({{}, false, true}));
        scene_render_uses_independent_material_slot_samplers(*device);
        std::cout << "PASS independent per-slot sampler pixels and cache parity\n";
        scene_render_cache_reuses_programs_meshes_materials_and_targets(*device);
        std::cout << "PASS cached scene program/mesh/material lifetime and pixel parity\n";
        native_material_channel_mappings_reach_shading(*device);
        std::cout << "PASS authored texture channel mappings reach native PBR shading\n";
        orthographic_projection_preserves_scale_under_depth_translation(*device);
        std::cout << "PASS perspective and orthographic projection pixel behavior\n";
        native_sampler_min_mag_filters_remain_independent(*device);
        std::cout << "PASS independent native sampler minification/magnification filters\n";
        scene_depth_exposure_material_camera_and_lifetimes(*device);
        std::cout << "PASS authored scene depth/HDR/material/texture/camera/exposure/lifetime GPU execution\n";
        local_point_spot_lights_reach_native_pixels(*device);
        std::cout << "PASS authored point/spot local lights reach native PBR pixels\n";
        specular_environment_mips_reach_native_pixels(*device);
        std::cout << "PASS source-bound roughness-mip environment reflections reach native PBR pixels\n";
        blended_scene_draws_sort_back_to_front_without_writing_depth(*device);
        std::cout << "PASS sorted straight-alpha scene blending and opaque depth preservation\n";
        vertex_colors_reach_pbr_and_masked_alpha(*device);
        std::cout << "PASS authored vertex color and masked vertex alpha reach native PBR\n";
        equirectangular_reflections_wrap_the_longitude_seam(*device);
        std::cout << "PASS equirectangular reflection longitude seam wraps continuously\n";
        hdr_environment_reflections_preserve_values_above_one(*device);
        std::cout << "PASS Radiance HDR reflections preserve above-one linear radiance\n";
        explicit_temporal_resolve_uses_bound_history_and_resets_on_cut(*device);
        std::cout << "PASS explicit D3D12 temporal history, reset, provenance and state wiring\n";
        direct_target_matches_capture_and_preserves_caller_ownership(*device);
        std::cout << "PASS direct GPU target pixel parity, stale-state refusal and caller ownership\n";
        graph_occlusion_values_reach_native_pixels(*device);
        std::cout << "PASS material graph occlusion reaches native pixels without source mutation\n";
        graph_opacity_reaches_native_coverage(*device);
        std::cout << "PASS material graph opacity reaches native masked and blended coverage\n";
        sheen_direct_lights_match_reference_and_invalidate_mesh_cache(*device);
        std::cout << "PASS direct sheen reference pixels and mesh-cache invalidation\n";
        anisotropy_direct_lights_match_reference_and_authored_axes(*device);
        std::cout << "PASS direct anisotropy reference pixels, axes and cache invalidation\n";
        anisotropic_normal_maps_match_transported_reference_and_cache(*device);
        std::cout << "PASS anisotropic normal-map frame, conventions and cached reference pixels\n";
        transformed_normals_match_baked_geometry(*device);
        require(device->debug_receipts().empty() && !device->device_lost(), "complete scene suite preserves clean native device state");
        std::cout << "PASS nonuniform and mirrored normal/tangent GPU parity\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL scene renderer: " << error.what() << '\n'; return 1; }
}
