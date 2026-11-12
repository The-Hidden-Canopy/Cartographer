#include "authoring_viewport_fixture.hpp"
#include "material_bake_two_pass_reference.hpp"
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
using Kind = project::MaterialGraphNodeKind;
using Type = project::MaterialGraphValueType;
void near(double actual, double expected) {
    check(std::abs(actual - expected) < 1e-12,
          "constant graph result differs from explicit expectation");
}
project::MaterialGraphNode &node(project::MaterialGraph &graph,
                                 std::uint64_t id) {
    return *std::find_if(graph.nodes.begin(), graph.nodes.end(),
                         [&](const auto &n) { return n.id == id; });
}
project::MaterialCatalog bound_catalog(const Fixture &f,
                                       project::MaterialGraph graph) {
    auto catalog = f.document.material_catalog();
    for (const auto &n : graph.nodes)
        if (n.kind == Kind::texture_sample &&
            !catalog.textures().contains(n.texture_asset)) {
            project::TextureSourceRecord texture;
            texture.id = n.texture_asset;
            texture.name = "Synthetic source record";
            texture.relative_locator = "textures/synthetic.tga";
            const std::array<std::uint8_t, 4> pixel{255, 0, 0, 255};
            texture.content_digest = assets::sha256(pixel);
            texture.format = assets::TextureSourceFormat::tga;
            texture.width = texture.height = 1;
            texture.provenance = "Independent synthetic test pixel";
            okay(catalog.insert_texture(texture));
        }
    okay(catalog.insert_material_graph(graph));
    okay(catalog.bind_material_graph(1, graph.identity));
    okay(catalog.validate());
    return catalog;
}
void factors_parameters_and_variants() {
    auto f = fixture();
    const auto original = take(production::resolve_authoring_material(
        f.document.material_catalog(), 1));
    near(original.base_color_factor.x, .7);
    auto graph = authoring_graph_fixture();
    auto catalog = bound_catalog(f, graph);
    const auto encoded = catalog.serialize();
    const auto resolved =
        take(production::resolve_authoring_material(catalog, 1));
    near(resolved.base_color_factor.x, .5375);
    near(resolved.base_color_factor.y, .275);
    near(resolved.base_color_factor.z, .0625);
    near(resolved.roughness, .225);
    near(resolved.metallic, .2);
    near(resolved.base_color_factor.w, 1);
    check(catalog.serialize() == encoded,
          "evaluation must not mutate authored graph or material");

    project::MaterialVariant variant;
    variant.id = 9;
    variant.name = "Resolved parameter inputs";
    variant.base_material = 1;
    auto &v = variant.overrides;
    v.base_color_factor = core::Vec4d{.2, .4, .8, .6};
    v.roughness = .8;
    v.metallic = .3;
    v.emissive_factor = core::Vec3d{2, 3, 4};
    v.emissive_strength = 5;
    v.dielectric_f0 = .12;
    v.clearcoat = .4;
    v.clearcoat_roughness = .7;
    okay(catalog.insert_variant(variant));
    const auto varied =
        take(production::resolve_authoring_material(catalog, 1, 9));
    near(varied.base_color_factor.x, .1625);
    near(varied.base_color_factor.y, .5);
    near(varied.base_color_factor.z, .625);
    near(varied.base_color_factor.w, .6);
    near(varied.roughness, .4);
    near(varied.metallic, .5);
    near(varied.clearcoat, .4);
    near(varied.clearcoat_roughness, .7);
    near(varied.dielectric_f0, .12);
    near(varied.emissive_factor.x, 10);
    near(varied.emissive_factor.y, 15);
    near(varied.emissive_factor.z, 20);
    near(varied.emissive_strength, 1);
    const auto emission = take(render::evaluate_pbr_reference(
        varied, {{0, 0, 1}, {0, 0, 1}}, {{0, 0, 1}, {}}));
    near(emission.hdr_color.x, 10);
    near(emission.hdr_color.z, 20);
    check(!production::resolve_authoring_material(catalog, 1, 999),
          "unknown variants fail");
    check(!production::resolve_authoring_material(catalog, 999),
          "unknown materials fail");
    // Optional graph channels preserve material factors when not routed.
    graph.outputs.emissive = graph.outputs.clearcoat =
        graph.outputs.clearcoat_roughness = 0;
    node(graph, 6).parameter_name = "dielectric_f0";
    okay(catalog.replace_material_graph(graph));
    okay(catalog.bind_material_graph(1, graph.identity));
    const auto partial =
        take(production::resolve_authoring_material(catalog, 1, 9));
    near(partial.emissive_strength, 5);
    near(partial.emissive_factor.x, 2);
    near(partial.clearcoat, .4);
    near(partial.roughness, .06);
}
void stale_source_order_and_ranges() {
    auto f = fixture();
    auto graph = authoring_graph_fixture();
    auto catalog = bound_catalog(f, graph);
    node(graph, 2).constants[0] = .3;
    okay(catalog.replace_material_graph(graph));
    auto stale = production::resolve_authoring_material(catalog, 1);
    check(!stale && stale.error().code == core::ErrorCode::stale_data,
          "matching revision with changed digest must require explicit rebind");
    okay(catalog.bind_material_graph(1, graph.identity));
    auto first = take(production::resolve_authoring_material(catalog, 1));
    graph.revision = graph.revision.next();
    std::reverse(graph.nodes.begin(), graph.nodes.end());
    okay(catalog.replace_material_graph(graph));
    check(!production::resolve_authoring_material(catalog, 1),
          "changed graph revision is stale until rebound");
    okay(catalog.bind_material_graph(1, graph.identity));
    auto reordered = take(production::resolve_authoring_material(catalog, 1));
    near(reordered.base_color_factor.x, first.base_color_factor.x);
    auto roundtrip =
        take(project::MaterialCatalog::deserialize(catalog.serialize()));
    near(take(production::resolve_authoring_material(roundtrip, 1)).roughness,
         first.roughness);

    auto material = catalog.materials().at(1);
    material.definition.base_color_factor = {.3, .3, .3, .4};
    material.revision = material.revision.next();
    okay(catalog.replace_material(material));
    node(graph, 2).constants = {.4, .4, .4, .8};
    node(graph, 3).constants[0] = 2;
    okay(catalog.replace_material_graph(graph));
    okay(catalog.bind_material_graph(1, graph.identity));
    const auto extrapolated =
        take(production::resolve_authoring_material(catalog, 1));
    near(extrapolated.base_color_factor.x, .5);
    near(extrapolated.base_color_factor.w, .4);
    node(graph, 7).constants[0] = 3;
    okay(catalog.replace_material_graph(graph));
    okay(catalog.bind_material_graph(1, graph.identity));
    const auto invalid = production::resolve_authoring_material(catalog, 1);
    check(!invalid, "out-of-range roughness must fail, not clamp silently");
}
void unsupported_and_budget() {
    auto f = fixture();
    auto graph = authoring_graph_fixture();
    const auto rejects = [&](project::MaterialGraph candidate,
                             const char *message) {
        auto catalog = bound_catalog(f, candidate);
        const auto before = catalog.serialize();
        const auto result = production::resolve_authoring_material(catalog, 1);
        check(!result && result.error().message.find(
                             "authoring material graph") != std::string::npos,
              message);
        check(before == catalog.serialize(),
              "failed execution preserves authored source");
    };
    node(graph, 1).parameter_name = "unbound.color";
    rejects(graph, "unbound parameter must fail");
    graph = authoring_graph_fixture();
    graph.outputs.occlusion = 3;
    rejects(graph, "unsupported output must fail");
    graph = authoring_graph_fixture();
    project::MaterialGraphNode extra;
    extra.id = 99;
    extra.kind = Kind::surface_input;
    extra.surface_input = project::MaterialSurfaceInput::curvature;
    graph.nodes.push_back(extra);
    rejects(graph, "surface input must not be rendered as zero");
    graph = authoring_graph_fixture();
    extra.kind = Kind::unsupported;
    extra.surface_input = project::MaterialSurfaceInput::unknown;
    extra.unsupported_identity = "synthetic-unsupported";
    graph.nodes.push_back(extra);
    rejects(graph, "disconnected unsupported nodes must fail visibly");
    graph = authoring_graph_fixture();
    extra = {};
    extra.id = 99;
    extra.kind = Kind::texture_sample;
    extra.value_type = Type::color;
    extra.texture_asset = 7;
    extra.inputs = {3, 7};
    graph.nodes.push_back(extra);
    rejects(graph, "texture nodes must not claim pixel execution");
    graph = authoring_graph_fixture();
    for (std::uint64_t id = 15; id <= 128; ++id) {
        extra = {};
        extra.id = id;
        extra.constants[0] = .5;
        graph.nodes.push_back(extra);
    }
    check(
        take(production::resolve_authoring_material(bound_catalog(f, graph), 1))
                .roughness == .225,
        "128-node bound remains executable");
    extra.id = 129;
    graph.nodes.push_back(extra);
    rejects(graph, "129-node graph exceeds execution budget");
    graph = authoring_graph_fixture();
    extra = {};
    extra.id = 15;
    extra.constants[0] = 1e9;
    graph.nodes.push_back(extra);
    extra = {};
    extra.id = 16;
    extra.kind = Kind::multiply;
    extra.inputs = {15, 15};
    graph.nodes.push_back(extra);
    rejects(graph,
            "excessive intermediates fail before producing material constants");
}
void pixels_and_lifecycle() {
    auto f = fixture();
    auto r = request(f);
    r.exposure_ev =
        -4; // Keep this orthographic specular fixture off the display shoulder.
    const auto plain =
        take(production::render_authoring_viewport(f.document, r));
    auto graph = authoring_graph_fixture();
    auto catalog = bound_catalog(f, graph);
    set_materials(f.document, catalog);
    const auto graphed =
        take(production::render_authoring_viewport(f.document, r));
    check(graphed.display_rgba8 != plain.display_rgba8,
          "executed graph must change visible pixels");
    auto resolved = take(production::resolve_authoring_material(catalog, 1));
    okay(catalog.unbind_material_graph(1));
    auto material = catalog.materials().at(1);
    material.definition = resolved;
    material.revision = material.revision.next();
    okay(catalog.replace_material(material));
    auto explicit_document = f.document;
    set_materials(explicit_document, catalog);
    const auto explicit_pixels =
        take(production::render_authoring_viewport(explicit_document, r));
    check(
        graphed.display_rgba8 == explicit_pixels.display_rgba8 &&
            graphed.objects == explicit_pixels.objects,
        "graph pixels equal independently resolved standard-material factors");
    for (std::size_t i = 0; i < graphed.linear_hdr.size(); ++i)
        check((graphed.linear_hdr[i] - explicit_pixels.linear_hdr[i])
                      .length_squared() == 0,
              "constant graph and explicit material have identical full HDR "
              "attachments");
    const auto path =
        std::filesystem::temp_directory_path() /
        ("cartographer-graph-lifecycle-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".carto");
    static_cast<void>(take(f.document.save_atomic(path)));
    application::ApplicationSession session;
    ui::UiController controller(session);
    static_cast<void>(
        take(controller.dispatch(application::OpenProjectAction{path})));
    check(take(session.render_authoring_viewport(
                   r, session.snapshot().project_revision))
                  .display_rgba8 == graphed.display_rgba8,
          "bound graph appearance survives initial session reopen");
    auto snapshot = session.snapshot();
    auto old_digest = take(graph.canonical_digest());
    graph.revision = graph.revision.next();
    node(graph, 2).constants = {.1, .2, .9, 1};
    static_cast<void>(
        take(controller.dispatch(application::SetMaterialGraphAction{
            snapshot.project_revision, core::Revision{1}, old_digest, graph})));
    const auto stale = session.render_authoring_viewport(
        r, session.snapshot().project_revision);
    check(!stale && stale.error().code == core::ErrorCode::stale_data,
          "edited graph must not silently adopt a stale binding");
    snapshot = session.snapshot();
    static_cast<void>(
        take(controller.dispatch(application::SetMaterialGraphBindingAction{
            snapshot.project_revision, 1, snapshot.materials.front().revision,
            graph.identity, graph.revision, take(graph.canonical_digest())})));
    const auto changed = take(session.render_authoring_viewport(
        r, session.snapshot().project_revision));
    check(changed.display_rgba8 != graphed.display_rgba8,
          "explicit rebind activates graph edit in viewport");
    check((changed.linear_hdr[center(changed)] -
           graphed.linear_hdr[center(graphed)])
                  .length_squared() > 0,
          "rebound graph changes unexposed radiance, not only display state");
    static_cast<void>(take(controller.dispatch(application::UndoAction{})));
    check(!session.render_authoring_viewport(
              r, session.snapshot().project_revision),
          "undo rebind restores stale binding state");
    static_cast<void>(take(controller.dispatch(application::UndoAction{})));
    check(take(session.render_authoring_viewport(
                   r, session.snapshot().project_revision))
                  .display_rgba8 == graphed.display_rgba8,
          "undo graph edit restores original visible material");
    static_cast<void>(take(controller.dispatch(application::RedoAction{})));
    static_cast<void>(take(controller.dispatch(application::RedoAction{})));
    static_cast<void>(take(
        controller.dispatch(application::SaveProjectAction{std::nullopt})));
    application::ApplicationSession reopened;
    ui::UiController reopened_ui(reopened);
    static_cast<void>(
        take(reopened_ui.dispatch(application::OpenProjectAction{path})));
    check(
        take(reopened.render_authoring_viewport(
                 r, reopened.snapshot().project_revision))
                .display_rgba8 == changed.display_rgba8,
        "graph edit/rebind survives redo and disk save/reopen without changing "
        "appearance");
}
void material_bake_matches_two_pass_reference() {
    const auto f = fixture();
    auto graph = authoring_graph_fixture();
    node(graph, 6).parameter_name = "uv.u";
    // Spatially varying HDR emission makes the last texel determine a global
    // scale. A partial-scale or row-local quantizer cannot match this oracle.
    node(graph, 12).kind = Kind::constant_color;
    node(graph, 12).parameter_name.clear();
    node(graph, 12).constants = {2, 10, 4, 1};
    project::MaterialGraphNode other;
    other.id = 15;
    other.kind = Kind::constant_color;
    other.value_type = Type::color;
    other.constants = {20, 1, 0, 1};
    graph.nodes.push_back(other);
    project::MaterialGraphNode mixed;
    mixed.id = 16;
    mixed.kind = Kind::mix;
    mixed.value_type = Type::color;
    mixed.inputs = {12, 15, 6};
    graph.nodes.push_back(mixed);
    graph.outputs.emissive = 16;
    const auto catalog = bound_catalog(f, graph);
    const auto before = catalog.serialize();
    const auto varying = take(production::compile_authoring_material(catalog, 1));
    const auto plain = take(production::compile_authoring_material(
        f.document.material_catalog(), 1));
    for (const auto* program : {&plain, &varying}) {
        for (const std::uint32_t resolution : {4U, 16U, 64U}) {
            const auto expected = take(production::testing::two_pass_material_bake_reference(
                *program, resolution));
            const auto actual = take(production::bake_authoring_material(*program, resolution));
            check(actual.program_digest == expected.program_digest && actual.uv == expected.uv &&
                    actual.normal_mapping == expected.normal_mapping,
                "single-evaluation bake preserves source and normal binding");
            for (std::size_t map = 0; map < actual.maps.size(); ++map)
                check(actual.maps[map].width == expected.maps[map].width &&
                        actual.maps[map].height == expected.maps[map].height &&
                        actual.maps[map].pixels == expected.maps[map].pixels,
                    "single-evaluation bake must match every old quantized map byte");
            check(actual.factors.emissive_strength == expected.factors.emissive_strength &&
                    actual.factors.alpha_mode == expected.factors.alpha_mode &&
                    actual.factors.alpha_cutoff == expected.factors.alpha_cutoff &&
                    actual.factors.dielectric_f0 == expected.factors.dielectric_f0 &&
                    actual.factors.base_color == expected.factors.base_color &&
                    actual.factors.metallic_roughness == expected.factors.metallic_roughness &&
                    actual.factors.normal == expected.factors.normal &&
                    actual.factors.occlusion == expected.factors.occlusion &&
                    actual.factors.emissive == expected.factors.emissive,
                "single-evaluation bake preserves global emission scale and center factors");
        }
        for (const std::uint32_t bad : {0U, 3U, 7U})
            check(!production::bake_authoring_material(*program, bad),
                "single-evaluation bake retains resolution admission");
    }
    check(catalog.serialize() == before, "baking never changes authored material sources");
    production::AuthoringMaterialProgram empty;
    check(!production::bake_authoring_material(empty, 4),
        "missing executable material programs remain refused");
}
} // namespace
int main() {
    try {
        material_bake_matches_two_pass_reference();
        factors_parameters_and_variants();
        stale_source_order_and_ranges();
        unsupported_and_budget();
        pixels_and_lifecycle();
        std::cout << "CPU material graph execution passed: factors/variants, "
                     "arithmetic, bounded failures, full-image parity, "
                     "edit/rebind/undo/redo/save/reopen; no GPU device\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
