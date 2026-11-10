#include "authoring_viewport_fixture.hpp"
#include <carto/geometry/primitives.hpp>
#include <carto/production/authoring_viewport.hpp>
#include <carto/production/authoring_light_state.hpp>
#include <carto/project/transaction.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace carto;
namespace {
void authored_light_states() {
    auto f = fixture();
    auto light = f.document.presentation_catalog().lights().at(f.light);
    check(take(production::authoring_light_is_active(light)), "steady light is active");
    const auto lit = take(production::render_authoring_viewport(f.document, request(f)));
    check(lit.linear_hdr[center(lit)].x > 0.0, "steady light illuminates the viewport");
    light.failure_state = project::LightFailureState::dark;
    light.color_temperature_kelvin = 2700.0;
    set_fixture_lights(f, {light});
    const auto dark = take(production::render_authoring_viewport(f.document, request(f)));
    check(dark.objects[center(dark)] && dark.linear_hdr[center(dark)].x == 0.0,
          "known-dark source contributes no light even with temperature intent");
    light.failure_state = project::LightFailureState::none;
    set_fixture_lights(f, {light});
    check(!production::render_authoring_viewport(f.document, request(f)),
          "active color temperature fails closed");
    light.color_temperature_kelvin.reset();
    for (const auto state : {project::LightNormalState::switchable,
                             project::LightNormalState::intermittent}) {
        light.normal_state = state; set_fixture_lights(f, {light});
        check(!production::render_authoring_viewport(f.document, request(f)),
              "ambiguous normal light state fails closed");
    }
    light.normal_state = project::LightNormalState::steady;
    light.failure_state = project::LightFailureState::intermittent;
    set_fixture_lights(f, {light});
    check(!production::render_authoring_viewport(f.document, request(f)),
          "intermittent failure fails closed");
}
void batch_display_contract() {
    std::vector<core::Vec3d> values;
    std::vector<std::uint8_t> coverage;
    for (std::size_t i = 0; i < 2048U; ++i) {
        const double x = std::exp2(double(i % 256U) / 16.0 - 12.0);
        values.push_back({i % 11U == 0U ? -x : x, x * .18, x * 4.0});
        coverage.push_back(i % 7U == 0U ? 0U : 255U);
    }
    for (auto mode : {render::ToneMapMode::aces_fitted, render::ToneMapMode::neutral,
                      render::ToneMapMode::linear_diagnostic})
        for (double ev : {-8.0, -2.0, 0.0, 2.0, 8.0}) {
            auto output = take(render::encode_display_rgba8(values, coverage, ev, mode));
            for (std::size_t p = 0; p < values.size(); ++p) {
                check(output[p * 4U + 3U] == coverage[p], "batch encoder preserves coverage");
                if (!coverage[p])
                    continue;
                const auto expected = take(render::linear_to_srgb(
                    take(render::tone_map(take(render::apply_exposure(values[p], ev)), mode))));
                const std::array<double, 3> rgb{expected.x, expected.y, expected.z};
                for (std::size_t c = 0; c < 3U; ++c)
                    check(std::abs(int(output[p * 4U + c]) - int(std::lround(rgb[c] * 255.0))) <= 1,
                          "batch display encoder matches scalar tone/exposure/color wedges");
            }
        }
    check(!render::encode_display_rgba8(values, {}, 0, render::ToneMapMode::neutral),
          "batch coverage mismatch fails");
}
void depth_hdr_and_display() {
    auto f = fixture();
    auto r = request(f);
    auto image = take(production::render_authoring_viewport(f.document, r));
    const auto p = center(image);
    check(image.objects[p] == f.front,
          "depth must select front object even though back object is drawn last");
    check(image.faces[p] == geometry::FaceId{1U}, "raster hit retains authored face ID");
    check(std::abs(image.depth[p] - 3.0) < 1e-12, "depth is positive camera distance");
    check(image.linear_hdr[p].x > 1.0, "HDR must survive unclamped");
    check(image.linear_hdr[p].x > image.linear_hdr[p].z, "authored material is visible");
    check(!image.objects[0] && image.display_rgba8[3U] == 0U, "background is transparent");
    for (auto mode : {render::ToneMapMode::aces_fitted, render::ToneMapMode::neutral,
                      render::ToneMapMode::linear_diagnostic}) {
        r.tone_mode = mode;
        r.exposure_ev = -2.0;
        auto exposed = take(production::render_authoring_viewport(f.document, r));
        check(exposed.linear_hdr[p].x == image.linear_hdr[p].x,
              "exposure must not alter HDR radiance");
        auto expected = take(render::linear_to_srgb(
            take(render::tone_map(take(render::apply_exposure(image.linear_hdr[p], -2.0)), mode))));
        check(std::abs(int(exposed.display_rgba8[p * 4U]) - int(std::lround(expected.x * 255.0))) <=
                  1,
              "exactly one exposure then selected tone mode then display encoding");
    }
    r = request(f);
    r.transform_overrides[f.front] = {{4.0, 0.0, 0.0}};
    auto moved = take(production::render_authoring_viewport(f.document, r));
    check(moved.objects[center(moved)] == f.back,
          "object motion replaces depth/identity rather than retaining stale "
          "pixels");
    r.width = 128U;
    r.height = 32U;
    auto resized = take(production::render_authoring_viewport(f.document, r));
    check(resized.objects.size() == 4096U, "resize reallocates all attachments consistently");
    r.width = 0U;
    check(take(production::render_authoring_viewport(f.document, r)).objects.empty(),
          "zero extent suspends rendering");
    r.width = 1048577U;
    check(!production::render_authoring_viewport(f.document, r),
          "oversized extent is rejected before allocation");
}
void transforms_and_sweeps() {
    auto f = fixture();
    auto r = request(f);
    for (double roughness : {0.05, 0.25, 0.7, 1.0})
        for (double metallic : {0.0, 0.5, 1.0}) {
            auto catalog = f.document.material_catalog();
            auto material = catalog.materials().at(1U);
            material.definition.roughness = roughness;
            material.definition.metallic = metallic;
            material.revision = material.revision.next();
            okay(catalog.replace_material(material));
            set_materials(f.document, catalog);
            for (core::Vec3d scale : {core::Vec3d{2.0, 0.5, 1.0}, core::Vec3d{-2.0, 0.5, 1.0}}) {
                core::Transform transform;
                transform.scale = scale;
                r.transform_overrides[f.front] = transform;
                auto image = take(production::render_authoring_viewport(f.document, r));
                render::PbrSurface surface;
                auto expected = take(render::evaluate_pbr_reference(
                    material.definition, surface, {{0.0, 0.0, 1.0}, {8.0, 8.0, 8.0}}));
                const auto p = center(image);
                check(image.objects[p] == f.front, "mirrored mesh retains front identity");
                check(std::abs(image.linear_hdr[p].x - expected.hdr_color.x) < 1e-10,
                      "reference BRDF agrees across material/transform sweep");
            }
        }
    r.transform_overrides[f.front].scale = {0.0, 1.0, 1.0};
    check(!production::render_authoring_viewport(f.document, r),
          "singular normal transform fails visibly");

    auto mesh = triangle();
    attributes::AttributeSet payload(
        {{attributes::AttributeDomain::corner, 3U}, {attributes::AttributeDomain::face, 1U}});
    okay(payload.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                             attributes::AttributeType::uint32},
                            {std::uint32_t{1U}}}));
    const core::Vec3d authored_normal{0.6, 0.0, 0.8};
    okay(payload.add_layer({{"cartographer.normal0", attributes::AttributeDomain::corner,
                             attributes::AttributeType::vec3},
                            {authored_normal, authored_normal, authored_normal}}));
    okay(mesh.set_attribute_payload(std::move(payload), {}));
    mutate(f.document, [&](auto &transaction) {
        okay(transaction.replace_mesh(f.front_mesh, std::move(mesh)));
    });
    r = request(f);
    for (double mirror : {1.0, -1.0}) {
        core::Transform transform;
        transform.scale = {mirror * 2.0, 0.5, 1.0};
        r.transform_overrides[f.front] = transform;
        auto image = take(production::render_authoring_viewport(f.document, r));
        render::PbrSurface surface;
        surface.normal =
            core::Vec3d{authored_normal.x / transform.scale.x, 0.0, authored_normal.z}.normalized();
        auto material = f.document.material_catalog().materials().at(1U).definition;
        auto expected = take(
            render::evaluate_pbr_reference(material, surface, {{0.0, 0.0, 1.0}, {8.0, 8.0, 8.0}}));
        check(std::abs(image.linear_hdr[center(image)].x - expected.hdr_color.x) < 1e-10,
              "authored corner normals use inverse transpose under nonuniform and "
              "mirrored scales");
    }
}
void clipping_and_failure() {
    auto f = fixture();
    auto r = request(f);
    r.camera.reset();
    r.view.position = {0.0, 0.0, 0.15};
    r.view.rotation = {};
    r.view.near_plane = 0.1;
    r.view.projection = project::CameraProjection::perspective;
    core::Transform tilted;
    tilted.rotation = {0.0, std::sin(0.5), 0.0, std::cos(0.5)};
    r.transform_overrides[f.front] = tilted;
    auto clipped = take(production::render_authoring_viewport(f.document, r));
    check(std::count(clipped.objects.begin(), clipped.objects.end(), f.front) > 0,
          "near-plane crossing is clipped, not dropped");
    for (std::size_t i = 0; i < clipped.depth.size(); ++i)
        if (clipped.objects[i])
            check(std::isfinite(clipped.depth[i]) && clipped.depth[i] >= 0.1 - 1e-10,
                  "clipped depth stays in range");
    r.camera = scene::ObjectId{999U};
    check(!production::render_authoring_viewport(f.document, r), "missing camera fails");
    r = request(f);
    r.exposure_ev = std::numeric_limits<double>::infinity();
    check(!production::render_authoring_viewport(f.document, r), "nonfinite exposure fails");
    auto catalog = f.document.material_catalog();
    auto m = catalog.materials().at(1U);
    m.definition.alpha_mode = render::AlphaMode::blended;
    m.revision = m.revision.next();
    okay(catalog.replace_material(m));
    set_materials(f.document, catalog);
    r = request(f);
    check(!production::render_authoring_viewport(f.document, r),
          "unsupported blending cannot silently substitute opaque shading");
}
void session_undo_and_reopen() {
    auto f = fixture();
    const auto root = std::filesystem::temp_directory_path() /
                      ("cartographer-viewport-session-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    static_cast<void>(take(f.document.save_atomic(root / "session.carto")));
    application::ApplicationSession session;
    ui::UiController controller(session);
    static_cast<void>(
        take(controller.dispatch(application::OpenProjectAction{root / "session.carto"})));
    auto r = request(f);
    auto snapshot = session.snapshot();
    auto original = take(session.render_authoring_viewport(r, snapshot.project_revision));
    core::Transform moved;
    moved.translation = {4.0, 0.0, 0.0};
    static_cast<void>(take(controller.dispatch(
        application::SetObjectTransformAction{f.front, moved, snapshot.project_revision})));
    check(!session.render_authoring_viewport(r, snapshot.project_revision),
          "stale live-session render request rejects after edit");
    auto changed = take(session.render_authoring_viewport(r, session.snapshot().project_revision));
    check(changed.objects[center(changed)] == f.back, "live authored transform reaches renderer");
    static_cast<void>(take(controller.dispatch(application::UndoAction{})));
    auto undone = take(session.render_authoring_viewport(r, session.snapshot().project_revision));
    check(undone.display_rgba8 == original.display_rgba8,
          "undo restores complete displayed appearance");
    static_cast<void>(take(controller.dispatch(application::RedoAction{})));
    auto redone = take(session.render_authoring_viewport(r, session.snapshot().project_revision));
    check(redone.display_rgba8 == changed.display_rgba8, "redo restores edited appearance");
    snapshot = session.snapshot();
    auto catalog = snapshot.presentation_catalog;
    auto camera = catalog.cameras().at(f.camera);
    camera.revision = camera.revision.next();
    camera.exposure_ev = -2;
    camera.tone_mode = render::ToneMapMode::neutral;
    okay(catalog.replace_camera(camera));
    static_cast<void>(take(controller.dispatch(application::SetPresentationCatalogAction{
        snapshot.project_generation, snapshot.project_revision, std::move(catalog)})));
    r.camera_display = true;
    auto displayed = take(session.render_authoring_viewport(r, session.snapshot().project_revision));
    check(std::equal(displayed.linear_hdr.begin(), displayed.linear_hdr.end(), redone.linear_hdr.begin(),
              [](auto a, auto b) { return (a-b).length_squared() == 0; }) &&
          displayed.display_rgba8 != redone.display_rgba8,
          "authored display edit changes bytes without exposing scene radiance twice");
    static_cast<void>(take(controller.dispatch(application::UndoAction{})));
    check(take(session.render_authoring_viewport(r, session.snapshot().project_revision)).display_rgba8 == redone.display_rgba8,
          "undo restores camera exposure and tone selection");
    static_cast<void>(take(controller.dispatch(application::RedoAction{})));
    static_cast<void>(take(controller.dispatch(application::SaveProjectAction{std::nullopt})));
    application::ApplicationSession reopened;
    ui::UiController reopened_ui(reopened);
    static_cast<void>(
        take(reopened_ui.dispatch(application::OpenProjectAction{root / "session.carto"})));
    check(take(reopened.render_authoring_viewport(r, reopened.snapshot().project_revision))
                  .display_rgba8 == displayed.display_rgba8,
          "live-session save/reopen preserves camera display intent and edited image");
}
void reopen_and_artifact(const std::filesystem::path &output) {
    auto f = fixture();
    auto r = request(f);
    auto before = take(production::render_authoring_viewport(f.document, r));
    auto reopened = take(project::ProjectDocument::deserialize(f.document.serialize()));
    auto after = take(production::render_authoring_viewport(reopened, r));
    check(before.display_rgba8 == after.display_rgba8 && before.objects == after.objects &&
              before.depth == after.depth,
          "save/reopen preserves the complete displayed image and hit buffers");
    auto catalog = f.document.material_catalog();
    auto m = catalog.materials().at(1U);
    m.definition.base_color_factor = {0.05, 0.8, 0.1, 1.0};
    m.revision = m.revision.next();
    okay(catalog.replace_material(m));
    set_materials(f.document, catalog);
    auto edited = take(production::render_authoring_viewport(f.document, r));
    check(edited.display_rgba8 != before.display_rgba8, "material edit invalidates visible pixels");
    if (!output.empty()) {
        std::filesystem::create_directories(output);
        take(f.document.save_atomic(output / "viewport-g1.carto"));
        auto loaded = take(project::ProjectDocument::load(output / "viewport-g1.carto"));
        check(take(production::render_authoring_viewport(loaded, r)).display_rgba8 ==
                  edited.display_rgba8,
              "disk reopen matches");
        std::ofstream ppm(output / "viewport-g1.ppm", std::ios::binary);
        ppm << "P6\n" << edited.width << ' ' << edited.height << "\n255\n";
        for (std::size_t p = 0; p < edited.objects.size(); ++p)
            ppm.write(reinterpret_cast<const char *>(&edited.display_rgba8[p * 4U]), 3);
        std::ofstream numeric(output / "viewport-g1.txt");
        const auto p = center(edited);
        numeric.precision(17);
        numeric << "CPU reference only\ncenter HDR " << edited.linear_hdr[p].x << ' '
                << edited.linear_hdr[p].y << ' ' << edited.linear_hdr[p].z << "\ndepth "
                << edited.depth[p] << "\nobject " << edited.objects[p].value << "\nsample visits "
                << edited.sample_visits << "\nattachment bytes " << edited.attachment_bytes
                << "\nhost render ms " << edited.host_milliseconds << '\n';
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        const auto start = std::chrono::steady_clock::now();
        batch_display_contract();
        authored_light_states();
        depth_hdr_and_display();
        transforms_and_sweeps();
        clipping_and_failure();
        session_undo_and_reopen();
        reopen_and_artifact(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        std::cout << "authoring viewport: depth/HDR/display, material sweeps, "
                     "transforms, clipping, budgets, edits and reopen passed; host ms="
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               start)
                         .count()
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
