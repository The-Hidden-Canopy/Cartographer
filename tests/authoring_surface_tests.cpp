#include "authoring_surface_fixture.hpp"
#include <algorithm>
#include <carto/attributes/uv.hpp>
#include <carto/ui/ui.hpp>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using Kind = project::MaterialGraphNodeKind;
using Type = project::MaterialGraphValueType;
using Program = production::AuthoringMaterialProgram;
void near(double actual, double expected, double tolerance = 1e-10) {
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error("surface sample mismatch: " + std::to_string(actual) +
                                 " expected " + std::to_string(expected));
}
project::MaterialCatalog mapped_catalog(const Pixels &p, render::TextureRef ref = {}) {
    auto catalog = fixture().document.material_catalog();
    okay(catalog.insert_texture(p.record));
    auto material = catalog.materials().at(1);
    material.definition.base_color_factor = {1, 1, 1, 1};
    ref.asset = p.record.id;
    material.definition.base_color = ref;
    material.revision = material.revision.next();
    okay(catalog.replace_material(material));
    return catalog;
}
void replace(project::MaterialCatalog &c, const render::StandardMaterial &m) {
    auto asset = c.materials().at(1);
    asset.definition = m;
    asset.revision = asset.revision.next();
    okay(c.replace_material(asset));
}
Program compile(const project::MaterialCatalog &c, const production::AuthoringTextureLibrary &l) {
    return take(production::compile_authoring_material(c, 1, {}, &l));
}
production::AuthoringMaterialSample at(const Program &p, double u, double v = 0.5,
                                       core::Vec2d dx = {}, core::Vec2d dy = {}) {
    return take(p.evaluate({{u, v}, dx, dy}));
}
void sampling_and_failure_atomicity() {
    auto p = pixels(1, 2, 1, {{0, 0, 0, 255}, {255, 255, 255, 255}});
    production::AuthoringTextureLibrary l;
    okay(l.admit(p.record, p.encoded));
    auto c = mapped_catalog(p);
    auto program = compile(c, l);
    near(at(program, .5).material.base_color_factor.x,
         .5); // linear-light bilinear, not sRGB .5 decoded
    auto mip = at(program, .23, .5, {2, 0});
    near(mip.material.base_color_factor.x, std::pow((188.0 / 255 + .055) / 1.055, 2.4));
    check(program.texture_bytes() == 12 && l.resident_bytes() == 20,
          "decoded and mip budgets counted separately");
    auto malformed = p.encoded;
    malformed.pop_back();
    check(!l.admit(p.record, malformed), "truncated replacement fails");
    auto wrong = p.record;
    wrong.width = 3;
    check(!l.admit(wrong, p.encoded), "wrong extent fails");
    wrong = p.record;
    wrong.content_digest = assets::sha256(malformed);
    check(!l.admit(wrong, p.encoded), "wrong digest fails");
    near(at(compile(c, l), .5).material.base_color_factor.x, .5);
    auto changed = pixels(1, 2, 1, {{255, 0, 0, 255}, {255, 0, 0, 255}});
    changed.record.revision = p.record.revision.next();
    okay(l.admit(changed.record, changed.encoded));
    check(!production::compile_authoring_material(c, 1, {}, &l),
          "old authored source cannot silently use replacement");
    near(at(program, .5).material.base_color_factor.x,
         .5); // old immutable program remains safe
    c = mapped_catalog(changed);
    near(at(compile(c, l), .5).material.base_color_factor.x, 1);
    check(!program.evaluate({{std::numeric_limits<double>::infinity(), 0}, {}, {}}),
          "nonfinite UV rejected");
    check(!program.evaluate({{1e7, 0}, {}, {}}), "coordinate bound enforced");
}
void sampler_identity_transform_and_data() {
    auto p = pixels(2, 2, 1, {{128, 64, 32, 255}, {255, 128, 64, 255}});
    production::AuthoringTextureLibrary l;
    okay(l.admit(p.record, p.encoded));
    auto c = mapped_catalog(p);
    project::SamplerIntent sampler;
    sampler.id = 7;
    sampler.name = "Independent nearest";
    sampler.minification = sampler.magnification = sampler.mip = project::SamplerFilter::nearest;
    okay(c.insert_sampler(sampler));
    auto m = c.materials().at(1).definition;
    m.base_color.sampler = 7;
    m.base_color.uv_set = 9;
    replace(c, m);
    auto program = compile(c, l);
    check(program.uv_set() == 9, "UV identity retained rather than first layer");
    near(at(program, 1.25).material.base_color_factor.x,
         std::pow((128.0 / 255 + .055) / 1.055, 2.4));
    sampler.id = 8;
    sampler.address_u = project::SamplerAddressMode::clamp_to_edge;
    okay(c.insert_sampler(sampler));
    m.base_color.sampler = 8;
    replace(c, m);
    near(at(compile(c, l), 1.25).material.base_color_factor.x, 1);
    sampler.id = 9;
    sampler.address_u = project::SamplerAddressMode::mirrored_repeat;
    okay(c.insert_sampler(sampler));
    m.base_color.sampler = 9;
    replace(c, m);
    near(at(compile(c, l), 1.25).material.base_color_factor.x, 1);
    m.base_color.interpretation = render::TextureInterpretation::linear;
    m.base_color.transform.offset = {.5, 0};
    replace(c, m);
    near(at(compile(c, l), .25).material.base_color_factor.x, 1);
    m.base_color.transform = {};
    m.base_color.channels.rgba[0] = render::TextureChannel::one_minus_green;
    replace(c, m);
    near(at(compile(c, l), .25).material.base_color_factor.x, 1 - 64.0 / 255);
    m.metallic_roughness = m.base_color;
    m.metallic_roughness.interpretation = render::TextureInterpretation::srgb;
    replace(c, m);
    check(!production::compile_authoring_material(c, 1, {}, &l), "data sRGB binding rejected");
    m.metallic_roughness.interpretation = render::TextureInterpretation::linear;
    m.metallic_roughness.channels = {};
    m.roughness = .8;
    m.metallic = .5;
    replace(c, m);
    auto data = at(compile(c, l), .25).material;
    near(data.roughness, .8 * 64 / 255);
    near(data.metallic, .5 * 32 / 255);
    m.metallic_roughness.uv_set = 1;
    replace(c, m);
    check(!production::compile_authoring_material(c, 1, {}, &l), "mixed UV sets fail explicitly");
}
project::MaterialGraph surface_graph(std::uint64_t texture = 0, std::uint64_t uv = 1) {
    auto g = authoring_graph_fixture();
    g.identity = "materials/surface/reference";
    project::MaterialGraphNode u;
    u.id = 15;
    u.kind = Kind::parameter_scalar;
    u.parameter_name = "uv.u";
    auto v = u;
    v.id = 16;
    v.parameter_name = "uv.v";
    g.nodes.push_back(u);
    g.nodes.push_back(v);
    g.outputs.roughness = 15;
    if (texture) {
        project::MaterialGraphNode t;
        t.id = 17;
        t.kind = Kind::texture_sample;
        t.value_type = Type::color;
        t.texture_asset = texture;
        t.uv_set = uv;
        t.inputs = {15, 16};
        g.nodes.push_back(t);
        g.outputs.base_color = 17;
    }
    return g;
}
void executable_uv_graphs() {
    auto p = pixels(3, 1, 1, {{128, 64, 32, 255}});
    production::AuthoringTextureLibrary l;
    okay(l.admit(p.record, p.encoded));
    auto c = mapped_catalog(p);
    auto g = surface_graph(3);
    okay(c.insert_material_graph(g));
    okay(c.bind_material_graph(1, g.identity));
    auto program = compile(c, l);
    auto sampled = at(program, .35).material;
    near(sampled.roughness, .35);
    near(sampled.base_color_factor.x, std::pow((128.0 / 255 + .055) / 1.055, 2.4));
    check(!production::resolve_authoring_material(c, 1),
          "constant GPU compatibility resolver rejects surface graph");
    g.nodes[1].constants[0] = .8;
    okay(c.replace_material_graph(g));
    auto stale = production::compile_authoring_material(c, 1, {}, &l);
    check(!stale && stale.error().code == core::ErrorCode::stale_data,
          "surface graph requires explicit digest rebind");
    okay(c.bind_material_graph(1, g.identity));
    auto roundtrip = take(project::MaterialCatalog::deserialize(c.serialize()));
    near(at(compile(roundtrip, l), .35).material.roughness, .35);
    auto m = c.materials().at(1).definition;
    m.emissive = m.base_color;
    replace(c, m);
    check(!production::compile_authoring_material(c, 1, {}, &l),
          "ambiguous color/emission semantic binding fails");
    m.emissive = {};
    m.base_color.uv_set = 9;
    replace(c, m);
    check(!production::compile_authoring_material(c, 1, {}, &l),
          "graph UV binding cannot silently follow another set");
    c = fixture().document.material_catalog();
    g = surface_graph();
    okay(c.insert_material_graph(g));
    okay(c.bind_material_graph(1, g.identity));
    auto uv = take(production::compile_authoring_material(c, 1));
    near(at(uv, .2).material.roughness, .2);
    near(at(uv, .8).material.roughness, .8);
    check(uv.uv_set() == render::kUv0, "untextured UV builtins explicitly select UV0");
}
void masked_raster_normal_and_reopen() {
    auto f = fixture();
    mutate(f.document, [&](auto &tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle())); });
    auto p = pixels(4, 2, 1, {{255, 0, 0, 0}, {255, 0, 0, 255}});
    auto library = std::make_shared<production::AuthoringTextureLibrary>();
    okay(library->admit(p.record, p.encoded));
    auto c = f.document.material_catalog();
    okay(c.insert_texture(p.record));
    auto m = c.materials().at(1).definition;
    m.base_color = {4, 9};
    m.alpha_mode = render::AlphaMode::masked;
    replace(c, m);
    set_materials(f.document, c);
    auto r = request(f);
    r.textures = library;
    auto image = take(production::render_authoring_viewport(f.document, r));
    const std::size_t left = 40 * 64 + 26, right = 40 * 64 + 38;
    check(image.objects[left] == f.back && image.objects[right] == f.front,
          "masked texels preserve rear depth and picking identities");
    auto loaded = take(project::ProjectDocument::deserialize(f.document.serialize()));
    check(take(production::render_authoring_viewport(loaded, r)).display_rgba8 ==
              image.display_rgba8,
          "reopen preserves textured raster");
    check(!production::render_authoring_viewport(f.document, request(f)),
          "missing decoded pixels fail visibly");
    const auto root = std::filesystem::temp_directory_path() /
                      ("cartographer-surface-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto path = root / "textured.carto";
    assets::BlobStore store(root / ".carto-assets" / "source-blobs");
    static_cast<void>(take(store.put(p.encoded, "image/x-tga")));
    static_cast<void>(take(f.document.save_atomic(path)));
    application::ApplicationSession session;
    ui::UiController controller(session);
    static_cast<void>(take(controller.dispatch(application::OpenProjectAction{path})));
    const auto live_request = request(f); // application must load verified source bytes itself
    const auto native_request = take(session.prepare_authoring_viewport_request(
        live_request, session.snapshot().project_revision));
    check(bool(native_request.textures),
          "native request receives the same digest-verified source library");
    check(!session.prepare_authoring_viewport_request(live_request,
                                                      session.snapshot().project_revision.next()),
          "native texture admission rejects stale project revision");
    auto live =
        take(session.render_authoring_viewport(live_request, session.snapshot().project_revision));
    check(live.display_rgba8 == image.display_rgba8,
          "session loads digest-addressed texture pixels on reopen");
    auto snapshot = session.snapshot();
    auto edited = m;
    edited.base_color.transform.offset = {.5, 0};
    static_cast<void>(take(controller.dispatch(application::UpdateMaterialAction{
        snapshot.project_revision, 1, snapshot.materials.front().revision, edited})));
    const auto changed =
        take(session.render_authoring_viewport(live_request, session.snapshot().project_revision));
    check(changed.objects[left] == f.front && changed.objects[right] == f.back,
          "texture-transform edit updates mask and picking");
    static_cast<void>(take(controller.dispatch(application::UndoAction{})));
    check(take(session.render_authoring_viewport(live_request, session.snapshot().project_revision))
                  .display_rgba8 == image.display_rgba8,
          "textured undo restores appearance");
    static_cast<void>(take(controller.dispatch(application::RedoAction{})));
    static_cast<void>(take(controller.dispatch(application::SaveProjectAction{std::nullopt})));
    application::ApplicationSession reopened;
    ui::UiController reopened_ui(reopened);
    static_cast<void>(take(reopened_ui.dispatch(application::OpenProjectAction{path})));
    check(
        take(reopened.render_authoring_viewport(live_request, reopened.snapshot().project_revision))
                .display_rgba8 == changed.display_rgba8,
        "textured edit/redo/save/reopen preserves appearance");
    auto n = pixels(5, 1, 1, {{128, 220, 220, 255}});
    okay(library->admit(n.record, n.encoded));
    okay(c.insert_texture(n.record));
    m.base_color = {};
    m.alpha_mode = render::AlphaMode::opaque;
    m.normal = {5, 9};
    replace(c, m);
    set_materials(f.document, c);
    const auto dx = at(compile(c, *library), .5);
    check(dx.normal_mapped && dx.tangent_normal.y < 0, "DirectX green points negative tangent Y");
    m.normal.normal_convention = render::NormalConvention::opengl_y_plus;
    replace(c, m);
    set_materials(f.document, c);
    const auto gl = at(compile(c, *library), .5);
    near(gl.tangent_normal.y, -dx.tangent_normal.y);
    near(gl.tangent_normal.z, dx.tangent_normal.z);
    for (const core::Vec3d scale : {core::Vec3d{2, .5, 1}, core::Vec3d{-2, .5, 1}}) {
        r.transform_overrides[f.front] = {{}, {}, scale};
        const auto transformed = take(production::render_authoring_viewport(f.document, r));
        check(transformed.objects[center(transformed)] == f.front,
              "nonuniform/mirrored normal mapped raster retains hit");
        check(transformed.linear_hdr[center(transformed)].finite(),
              "transformed normal-map radiance remains finite");
        const auto tangent_normal = gl.tangent_normal;
        const core::Vec3d expected_normal{scale.x < 0 ? -tangent_normal.x : tangent_normal.x,
                                          -tangent_normal.y, tangent_normal.z};
        const auto expected = take(render::evaluate_pbr_reference(
            gl.material, {expected_normal, {0, 0, 1}}, {{0, 0, 1}, {8, 8, 8}}));
        near(transformed.linear_hdr[center(transformed)].x, expected.hdr_color.x);
        near(transformed.linear_hdr[center(transformed)].y, expected.hdr_color.y);
    }
}
void perspective_uv_raster() {
    auto f = fixture();
    mutate(f.document,
           [&](auto &tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle(1, true))); });
    auto c = f.document.material_catalog();
    auto g = authoring_graph_fixture();
    g.nodes[0].kind = Kind::constant_color;
    g.nodes[0].parameter_name.clear();
    g.nodes[0].constants = {0, 0, 0, 1};
    g.nodes[1].constants = {1, 0, 0, 1};
    g.nodes[2].kind = Kind::parameter_scalar;
    g.nodes[2].parameter_name = "uv.u";
    g.nodes[2].constants = {};
    g.outputs.emissive = 4;
    okay(c.insert_material_graph(g));
    okay(c.bind_material_graph(1, g.identity));
    set_materials(f.document, c);
    project::PresentationCatalog presentation;
    auto camera = f.document.presentation_catalog().cameras().at(f.camera);
    camera.projection = project::CameraProjection::perspective;
    okay(presentation.insert_camera(camera));
    mutate(f.document, [&](auto &tx) { okay(tx.set_presentation_catalog(presentation)); });
    auto r = request(f);
    auto image = take(production::render_authoring_viewport(f.document, r));
    const std::array<core::Vec3d, 3> projected{take(image.view.project({-1, -1, 0}, 64, 64)),
                                               take(image.view.project({1, -1, -1}, 64, 64)),
                                               take(image.view.project({0, 1, 0}, 64, 64))};
    unsigned compared = 0;
    double max_affine_error = 0;
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            const auto index = y * 64 + x;
            if (image.objects[index] != f.front)
                continue;
            // Independent area-coordinate formula, followed by homogeneous division.
            const auto &a = projected[0];
            const auto &b = projected[1];
            const auto &d = projected[2];
            const auto denominator = (b.y - d.y) * (a.x - d.x) + (d.x - b.x) * (a.y - d.y);
            const double wa =
                ((b.y - d.y) * (x + .5 - d.x) + (d.x - b.x) * (y + .5 - d.y)) / denominator;
            const double wb =
                ((d.y - a.y) * (x + .5 - d.x) + (a.x - d.x) * (y + .5 - d.y)) / denominator;
            const double wc = 1 - wa - wb;
            const double u = (wb / 4 + .5 * wc / 3) / (wa / 3 + wb / 4 + wc / 3);
            near(image.linear_hdr[index].x, u, 2e-12);
            near(image.linear_hdr[index].y, 0);
            max_affine_error = std::max(max_affine_error, std::abs(u - (wb + .5 * wc)));
            ++compared;
        }
    check(compared > 100 && max_affine_error > .02,
          "perspective test distinguishes rational UV interpolation from affine "
          "shortcut");
}
void gpu_packet_contract() {
    auto c = fixture().document.material_catalog();
    auto g = surface_graph();
    okay(c.insert_material_graph(g));
    okay(c.bind_material_graph(1, g.identity));
    const auto program = take(production::compile_authoring_material(c, 1));
    const auto packet = take(program.gpu_packet());
    check(packet.words.size() % 32 == 0 && packet.words[0][0] == 1 && packet.words[0][3] <= 32,
          "native packet version, row alignment and register bound");
    check(packet.words.size() * 16 <= 6656, "bounded native packet payload");
    // Decode the packed register machine independently of the CPU graph's
    // node-ID evaluator. This catches last-use reuse and output-register bugs.
    for (float u : {.1F, .3F, .7F, .9F}) {
        std::array<std::array<float, 4>, 32> regs{};
        for (unsigned i = 0; i < unsigned(packet.words[0][1]); ++i) {
            const auto code = packet.words[24 + i * 3], slots = packet.words[25 + i * 3];
            std::array<float, 4> value{};
            const unsigned op = unsigned(code[0]), dest = unsigned(slots[0]);
            if (op < 2)
                value = packet.words[26 + i * 3];
            else if (op == 2 || op == 3) {
                const auto &m = packet.material;
                switch (unsigned(code[2])) {
                case 0:
                    value = {float(m.base_color_factor.x), float(m.base_color_factor.y),
                             float(m.base_color_factor.z), float(m.base_color_factor.w)};
                    break;
                case 1:
                    value = {float(m.emissive_factor.x * m.emissive_strength),
                             float(m.emissive_factor.y * m.emissive_strength),
                             float(m.emissive_factor.z * m.emissive_strength), 1};
                    break;
                case 2:
                    value[0] = float(m.base_color_factor.w);
                    break;
                case 3:
                    value[0] = float(m.roughness);
                    break;
                case 4:
                    value[0] = float(m.metallic);
                    break;
                case 5:
                    value[0] = float(m.dielectric_f0);
                    break;
                case 6:
                    value[0] = float(m.clearcoat);
                    break;
                case 7:
                    value[0] = float(m.clearcoat_roughness);
                    break;
                case 8:
                    value[0] = u;
                    break;
                case 9:
                    value[0] = .5F;
                    break;
                }
            } else {
                const auto a = regs[unsigned(slots[1])], b = regs[unsigned(slots[2])];
                const float weight = op == 8 ? regs[unsigned(slots[3])][0] : 0;
                for (unsigned j = 0; j < (code[1] == 0 ? 1U : 4U); ++j)
                    value[j] = op == 6   ? a[j] + b[j]
                               : op == 7 ? a[j] * b[j]
                                         : a[j] * (1 - weight) + b[j] * weight;
            }
            regs[dest] = value;
        }
        const auto result = at(program, u).material;
        const auto outputs = packet.words[1];
        near(regs[unsigned(outputs[0])][0], result.base_color_factor.x, 1e-6);
        near(regs[unsigned(outputs[1])][0], result.base_color_factor.w, 1e-6);
        near(regs[unsigned(outputs[2])][0], result.roughness, 1e-6);
        near(regs[unsigned(outputs[3])][0], result.metallic, 1e-6);
    }
    std::reverse(g.nodes.begin(), g.nodes.end());
    okay(c.replace_material_graph(g));
    check(take(take(production::compile_authoring_material(c, 1)).gpu_packet()).digest ==
              packet.digest,
          "source vector order does not perturb GPU packet");
    const auto pressure = [](unsigned constants) {
        auto graph = surface_graph();
        graph.nodes.clear();
        graph.outputs = {};
        project::MaterialGraphNode color;
        color.id = 1;
        color.kind = Kind::constant_color;
        color.value_type = Type::color;
        color.constants = {.5, .5, .5, 1};
        graph.nodes.push_back(color);
        const auto scalar = [&](unsigned id, double v) {
            project::MaterialGraphNode n;
            n.id = id;
            n.constants[0] = v;
            graph.nodes.push_back(n);
        };
        scalar(2, 1);
        scalar(3, 0);
        scalar(6, .01);
        project::MaterialGraphNode uv;
        uv.id = 5;
        uv.kind = Kind::parameter_scalar;
        uv.parameter_name = "uv.u";
        graph.nodes.push_back(uv);
        for (unsigned i = 0; i < constants; ++i)
            scalar(10 + i, 1);
        std::uint64_t previous = 10;
        for (unsigned i = 1; i < constants; ++i) {
            project::MaterialGraphNode n;
            n.id = 100 + i;
            n.kind = Kind::add;
            n.inputs = {previous, 10U + i};
            graph.nodes.push_back(n);
            previous = n.id;
        }
        project::MaterialGraphNode output;
        output.id = 200;
        output.kind = Kind::multiply;
        output.inputs = {previous, 6};
        graph.nodes.push_back(output);
        graph.outputs.base_color = 1;
        graph.outputs.alpha = 2;
        graph.outputs.metallic = 3;
        graph.outputs.roughness = 200;
        return graph;
    };
    for (unsigned count : {26U, 34U}) {
        auto catalog = fixture().document.material_catalog();
        auto graph = pressure(count);
        okay(catalog.insert_material_graph(graph));
        okay(catalog.bind_material_graph(1, graph.identity));
        const auto compiled = take(production::compile_authoring_material(catalog, 1));
        near(at(compiled, .5).material.roughness, count * .01);
        const auto packed = compiled.gpu_packet();
        check(bool(packed) == (count == 26),
              "GPU register pressure accepted/rejected independently of CPU execution");
    }
    auto p = pixels(81, 1, 1, {{128, 64, 32, 255}});
    production::AuthoringTextureLibrary library;
    okay(library.admit(p.record, p.encoded));
    c = mapped_catalog(p);
    g = surface_graph(81);
    okay(c.insert_material_graph(g));
    okay(c.bind_material_graph(1, g.identity));
    const auto textured = take(compile(c, library).gpu_packet());
    check(textured.maps[0] && textured.maps[0]->color_space == assets::ColorSpace::srgb &&
              textured.words[4][0] == 1,
          "native packet retains typed sRGB mip source");
    check((unsigned(textured.words[0][2]) & 1) != 0,
          "explicit graph sample suppresses double base-color multiplication");
    auto snapshot = c.serialize();
    auto bad = p.encoded;
    bad.pop_back();
    check(!library.admit(p.record, bad), "packet source failed replacement rejects");
    check(c.serialize() == snapshot &&
              take(compile(c, library).gpu_packet()).digest == textured.digest,
          "failed admission retains prior good GPU packet and authored source");
}
} // namespace
int main() {
    try {
        sampling_and_failure_atomicity();
        sampler_identity_transform_and_data();
        executable_uv_graphs();
        masked_raster_normal_and_reopen();
        perspective_uv_raster();
        gpu_packet_contract();
        std::cout << "authoring surface CPU: linear sampling/mips, sampler/UV "
                     "identity, failure atomicity, executable UV/texture graphs, "
                     "mask/depth/picking, normal conventions and reopen passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
