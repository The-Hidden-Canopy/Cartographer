#pragma once
#include <carto/production/authoring_viewport.hpp>
#include <carto/project/transaction.hpp>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {
using namespace carto;
inline void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T take(core::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
inline void okay(core::Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
template <class Edit> void mutate(project::ProjectDocument &document, Edit edit) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("cartographer-viewport-" + std::to_string(stamp) + ".log");
    {
        journal::Journal journal(path);
        if (document.revision() != core::Revision{}) {
            const std::string checkpoint = document.serialize();
            static_cast<void>(take(journal.append(
                {{},
                 document.revision(),
                 "viewport.test.checkpoint",
                 std::span<const std::uint8_t>{
                     reinterpret_cast<const std::uint8_t *>(checkpoint.data()), checkpoint.size()},
                 std::nullopt})));
        }
        auto transaction =
            take(project::ProjectTransaction::begin(document, journal, "viewport-test"));
        edit(transaction);
        static_cast<void>(take(transaction.commit("viewport.test.edit")));
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
inline void set_materials(project::ProjectDocument &document, project::MaterialCatalog catalog) {
    mutate(document,
           [&](auto &transaction) { okay(transaction.set_material_catalog(std::move(catalog))); });
}
inline geometry::EditableMesh triangle() {
    geometry::EditableMesh mesh;
    okay(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {-1.0, -1.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, -1.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U},
          {geometry::VertexId{1U}, geometry::VertexId{2U}, geometry::VertexId{3U}}}}));
    attributes::AttributeSet payload({{attributes::AttributeDomain::face, 1U}});
    okay(payload.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                             attributes::AttributeType::uint32},
                            {std::uint32_t{1U}}}));
    okay(mesh.set_attribute_payload(std::move(payload), {}));
    return mesh;
}
struct Fixture {
    project::ProjectDocument document;
    scene::ObjectId front, back, camera, light;
    std::uint64_t front_mesh{}, back_mesh{};
};
inline Fixture fixture() {
    Fixture f{take(project::ProjectDocument::create("Viewport gate one")), {}, {}, {}, {}, 0U, 0U};
    mutate(f.document, [&](auto &transaction) {
        f.front_mesh = take(transaction.add_mesh(triangle()));
        f.back_mesh = take(transaction.add_mesh(triangle()));
        f.front = take(transaction.create_object("Front red"));
        f.back = take(transaction.create_object("Back blue", {{0.0, 0.0, -1.0}}));
        okay(transaction.attach_mesh(f.front, f.front_mesh));
        okay(transaction.attach_mesh(f.back, f.back_mesh));
        f.camera = take(transaction.create_object("Review camera", {{0.0, 0.0, 3.0}}));
        f.light = take(transaction.create_object("Directional key"));
        project::PresentationCatalog presentation;
        project::AuthoredCamera camera;
        camera.object = f.camera;
        camera.identity = "viewport.camera";
        camera.bookmark_identity = "viewport.review";
        camera.projection = project::CameraProjection::orthographic;
        camera.orthographic_height = 4.0;
        camera.active_for_review = true;
        okay(presentation.insert_camera(camera));
        project::AuthoredLight light;
        light.object = f.light;
        light.identity = "viewport.key";
        light.kind = project::AuthoredLightKind::directional;
        light.intensity = 8.0;
        light.range = 0.0;
        okay(presentation.insert_light(light));
        okay(transaction.set_presentation_catalog(presentation));
        project::MaterialCatalog catalog;
        render::MaterialAsset red;
        red.id = 1U;
        red.name = "Red";
        red.definition.base_color_factor = {0.7, 0.1, 0.05, 1.0};
        red.definition.roughness = 0.45;
        render::MaterialAsset blue = red;
        blue.id = 2U;
        blue.name = "Blue";
        blue.definition.base_color_factor = {0.05, 0.1, 0.7, 1.0};
        okay(catalog.insert_material(red));
        okay(catalog.insert_material(blue));
        okay(catalog.insert_assignment({{f.front_mesh, 1U}, 1U, {}}));
        okay(catalog.insert_assignment({{f.back_mesh, 1U}, 2U, {}}));
        okay(transaction.set_material_catalog(catalog));
    });
    okay(f.document.validate());
    return f;
}
inline production::AuthoringViewportRequest request(const Fixture &f) {
    production::AuthoringViewportRequest r;
    r.width = 64U;
    r.height = 64U;
    r.camera = f.camera;
    // Existing BRDF/material parity fixtures isolate direct light. Spatial
    // shadow behavior has its own occluder/receiver acceptance workload.
    r.shadows.enabled = false;
    return r;
}
inline void set_fixture_lights(Fixture &f, const std::vector<project::AuthoredLight> &lights) {
    project::PresentationCatalog catalog;
    for (const auto &[id, camera] : f.document.presentation_catalog().cameras()) {
        static_cast<void>(id);
        okay(catalog.insert_camera(camera));
    }
    for (const auto &light : lights) okay(catalog.insert_light(light));
    mutate(f.document, [&](auto &transaction) { okay(transaction.set_presentation_catalog(catalog)); });
}
inline std::size_t center(const production::AuthoringViewportImage &image) {
    return static_cast<std::size_t>(image.height / 2U) * image.width + image.width / 2U;
}

inline project::MaterialGraph authoring_graph_fixture() {
    using Kind = project::MaterialGraphNodeKind;
    using Type = project::MaterialGraphValueType;
    const auto scalar = [](std::uint64_t id, double value) {
        project::MaterialGraphNode n;
        n.id = id;
        n.kind = Kind::constant_scalar;
        n.constants[0] = value;
        return n;
    };
    const auto parameter = [](std::uint64_t id, const char *name,
                              bool color = false) {
        project::MaterialGraphNode n;
        n.id = id;
        n.kind = color ? Kind::parameter_color : Kind::parameter_scalar;
        n.value_type = color ? Type::color : Type::scalar;
        n.parameter_name = name;
        return n;
    };
    const auto arithmetic = [](std::uint64_t id, Kind kind, Type type,
                               std::vector<std::uint64_t> inputs) {
        project::MaterialGraphNode n;
        n.id = id;
        n.kind = kind;
        n.value_type = type;
        n.inputs = std::move(inputs);
        return n;
    };
    project::MaterialGraphNode color;
    color.id = 2;
    color.kind = Kind::constant_color;
    color.value_type = Type::color;
    color.constants = {.05, .8, .1, 1};
    project::MaterialGraph graph;
    graph.identity = "materials/viewport/arithmetic";
    graph.revision = core::Revision{1};
    graph.nodes = {parameter(1, "base_color", true),
                   color,
                   scalar(3, .25),
                   arithmetic(4, Kind::mix, Type::color, {1, 2, 3}),
                   parameter(5, "alpha"),
                   parameter(6, "roughness"),
                   scalar(7, .5),
                   arithmetic(8, Kind::multiply, Type::scalar, {6, 7}),
                   parameter(9, "metallic"),
                   scalar(10, .2),
                   arithmetic(11, Kind::add, Type::scalar, {9, 10}),
                   parameter(12, "emissive", true),
                   parameter(13, "clearcoat"),
                   parameter(14, "clearcoat_roughness")};
    graph.outputs.base_color = 4;
    graph.outputs.alpha = 5;
    graph.outputs.roughness = 8;
    graph.outputs.metallic = 11;
    graph.outputs.emissive = 12;
    graph.outputs.clearcoat = 13;
    graph.outputs.clearcoat_roughness = 14;
    return graph;
}

} // namespace
