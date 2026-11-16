#include "authoring_environment_fixture.hpp"
#include <carto/ui/ui.hpp>
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
void catalog_contract() {
    auto e = asymmetric_environment();
    project::PresentationCatalog c;
    okay(c.set_environment(e));
    const auto before = c.serialize();
    check(take(project::PresentationCatalog::deserialize(before)).serialize() == before,
          "linear HDR environment and orientation serialize canonically");
    auto bad = e;
    bad.linear_radiance[0].x = std::numeric_limits<double>::quiet_NaN();
    check(!c.set_environment(bad) && c.serialize() == before,
          "failed environment replacement preserves prior source");
    bad = e;
    bad.height = 100000;
    check(!c.set_environment(bad), "environment extent admission is bounded");
    const auto old = take(project::PresentationCatalog::deserialize(
        "CARTOGRAPHER_PRESENTATION_CATALOG 3\nCAMERAS 0\nLIGHTS 0\nREFERENCE_PLANES 0\nEND\n"));
    check(!old.environment(), "legacy v3 sources migrate without invented environment light");
    auto malformed = before;
    const auto marker = malformed.find("ENVIRONMENT_SOURCE");
    malformed.erase(marker);
    check(!project::PresentationCatalog::deserialize(malformed),
          "truncated environment fails visibly");
}
void lighting_contract() {
    auto constant = asymmetric_environment();
    std::fill(constant.linear_radiance.begin(), constant.linear_radiance.end(),
              core::Vec3d{4, 2, .5});
    auto uniform = take(production::compile_environment_lighting(constant));
    check(uniform->atlas().size_bytes() == 128 * 207 * 16,
          "environment product has a bounded explicit atlas budget");
    const auto high = take(production::compile_environment_lighting(
        constant, production::EnvironmentLightingQuality::high));
    check(high->atlas_width() == 256 && high->atlas_height() == 335 &&
              high->atlas().size_bytes() == 256U * 335U * 16U,
          "high quality has a bounded, explicitly sized atlas");
    check(!production::compile_environment_lighting(
              constant, static_cast<production::EnvironmentLightingQuality>(99)),
          "unknown quality tiers fail closed");
    for (const auto n : {core::Vec3d{1, 0, 0}, core::Vec3d{0, 1, 0}, core::Vec3d{0, 0, -1}}) {
        check((uniform->diffuse(n) - core::Vec3d{4, 2, .5}).length() < 1e-5,
              "constant environment conserves diffuse radiance E/pi in every direction");
        for (double roughness : {0., .1, .4, .8, 1.}) {
            check((uniform->specular(n, roughness) - core::Vec3d{4, 2, .5}).length() < 1e-5,
                  "normalized GGX prefilter conserves constant HDR radiance");
            check((high->specular(n, roughness) - core::Vec3d{4, 2, .5}).length() < 1e-5 &&
                      (high->diffuse(n) - core::Vec3d{4, 2, .5}).length() < 1e-5,
                  "high-quality atlas conserves constant radiance");
            for (double nv : {.01, .2, .7, 1.}) {
                const auto lut = uniform->brdf(nv, roughness);
                const auto high_lut = high->brdf(nv, roughness);
                check(std::isfinite(lut[0]) && std::isfinite(lut[1]) && lut[0] >= 0 &&
                          lut[1] >= 0 && lut[0] + lut[1] < 1.05,
                      "BRDF integral remains finite and energy-bounded over roughness/view-angle "
                      "sweep");
                check(std::abs(lut[0]-high_lut[0]) < 1e-7 &&
                          std::abs(lut[1]-high_lut[1]) < 1e-7,
                      "quality tiers preserve the same BRDF lookup");
            }
        }
    }
    auto e = asymmetric_environment();
    auto product = take(production::compile_environment_lighting(e));
    auto detail = e;
    detail.width = 256; detail.height = 128;
    detail.linear_radiance.assign(std::size_t(detail.width)*detail.height, core::Vec3d{.01,.01,.01});
    constexpr unsigned bright_x=128, bright_y=64;
    detail.linear_radiance[std::size_t(bright_y)*detail.width+bright_x] = {1000,1000,1000};
    const auto began = std::chrono::steady_clock::now();
    const auto detail_standard = take(production::compile_environment_lighting(
        detail, production::EnvironmentLightingQuality::standard));
    const auto standard_ms = std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-began).count();
    const auto high_start = std::chrono::steady_clock::now();
    const auto detail_high = take(production::compile_environment_lighting(
        detail, production::EnvironmentLightingQuality::high));
    const auto high_ms = std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-high_start).count();
    const double theta=std::numbers::pi*(bright_y+.5)/detail.height;
    const double phi=2*std::numbers::pi*((bright_x+.5)/detail.width-.5);
    const core::Vec3d direction{std::sin(theta)*std::cos(phi),std::cos(theta),
                                std::sin(theta)*std::sin(phi)};
    const auto standard_peak=detail_standard->specular(direction,0).x;
    const auto high_peak=detail_high->specular(direction,0).x;
    std::cout << "detail fixture: standard=" << standard_peak << " high=" << high_peak
              << "; standard_compile_ms=" << standard_ms << "; high_compile_ms=" << high_ms << '\n';
    check(high_peak > standard_peak * 1.05,
          "higher-quality atlas retains a narrow bright reflection better");
    check(product->diffuse({1, 0, 0}).x > product->diffuse({-1, 0, 0}).x * 2,
          "diffuse environment is spatially directional, not one+Z irradiance sample");
    check(product->specular({1, 0, 0}, .1).x > product->specular({1, 0, 0}, 1).x * 2,
          "roughness broadens asymmetric directional reflections");
    check(product->specular({0, 1, 0}, 1).y > product->specular({0, -1, 0}, 1).y * 2,
          "coarsest roughness retains zenith/nadir direction instead of collapsing to one row");
    render::StandardMaterial m;
    m.base_color_factor = {1, 1, 1, 1};
    m.metallic = 1;
    m.roughness = .1;
    const auto original =
        take(production::evaluate_environment_lighting(*product, e, m, {1, 0, 0}, {1, 0, 0}));
    check(original.x > 1 && original.x > original.z * 2,
          "mirror-like environment response retains unclamped HDR direction");
    auto rotated = e;
    rotated.rotation_radians = std::numbers::pi / 2;
    const auto after =
        take(production::evaluate_environment_lighting(*product, rotated, m, {1, 0, 0}, {1, 0, 0}));
    check(after.z > after.x * 2 && (after - original).length() > 1,
          "rotating asymmetric source changes reflected direction");
    rotated = e;
    rotated.intensity = 2;
    check((take(production::evaluate_environment_lighting(*product, rotated, m, {1, 0, 0},
                                                          {1, 0, 0})) -
           original * 2)
                  .length() < 1e-8,
          "environment intensity is applied exactly once outside convolution");
    check(take(production::environment_content_digest(rotated)) == product->source_digest(),
          "power/orientation edits reuse the derived convolution identity");
    check(take(production::evaluate_environment_lighting(*product, e, m, {1, 0, 0}, {1, 0, 0}, 0))
                  .x == original.x,
          "AO does not darken metallic specular environment or substitute for GI");
    m.metallic = 0;
    const auto no_ao =
        take(production::evaluate_environment_lighting(*product, e, m, {1, 0, 0}, {1, 0, 0}, 0));
    const auto ao =
        take(production::evaluate_environment_lighting(*product, e, m, {1, 0, 0}, {1, 0, 0}, 1));
    check(ao.x > no_ao.x, "AO modulates diffuse environment separately from specular");
    m.metallic = 1;
    m.roughness = 1;
    m.clearcoat = 1;
    m.clearcoat_roughness = .1;
    const auto polished_coat =
        take(production::evaluate_environment_lighting(*product, e, m, {1, 0, 0}, {1, 0, 0}));
    m.clearcoat_roughness = 1;
    const auto rough_coat =
        take(production::evaluate_environment_lighting(*product, e, m, {1, 0, 0}, {1, 0, 0}));
    check(polished_coat.x > rough_coat.x + .05,
          "clearcoat uses its own directional roughness level");
    check(!product->specular({1, 0, 0}, std::numeric_limits<double>::quiet_NaN()).finite(),
          "invalid roughness cannot trigger undefined atlas indexing");
}
void viewport_and_session(const std::filesystem::path &output) {
    auto e = asymmetric_environment();
    auto f = environment_fixture(e);
    auto r = request(f);
    r.environment_lighting = take(production::compile_environment_lighting(e));
    const auto original = take(production::render_authoring_viewport(f.document, r));
    check(original.linear_hdr[center(original)].z > 1,
          "environment alone lights authored geometry in linear HDR");
    auto exposed = r;
    exposed.exposure_ev = 1;
    const auto brighter = take(production::render_authoring_viewport(f.document, exposed));
    check(brighter.display_rgba8 != original.display_rgba8 &&
              std::equal(original.linear_hdr.begin(), original.linear_hdr.end(),
                         brighter.linear_hdr.begin(),
                         [](auto a, auto b) { return (a - b).length_squared() == 0; }),
          "camera exposure never enters environment convolution/lighting");
    auto changed = f.document.presentation_catalog();
    auto rotated = e;
    rotated.rotation_radians = std::numbers::pi / 2;
    rotated.revision = rotated.revision.next();
    okay(changed.set_environment(rotated));
    auto edited = f.document;
    mutate(edited, [&](auto &tx) { okay(tx.set_presentation_catalog(changed)); });
    const auto after = take(production::render_authoring_viewport(edited, r));
    check(after.display_rgba8 != original.display_rgba8 && after.objects == original.objects &&
              after.depth == original.depth,
          "environment edit changes appearance without corrupting geometry/picking");
    auto stale = e;
    stale.linear_radiance[0].x += 1;
    changed = f.document.presentation_catalog();
    okay(changed.set_environment(stale));
    auto other = f.document;
    mutate(other, [&](auto &tx) { okay(tx.set_presentation_catalog(changed)); });
    check(!production::render_authoring_viewport(other, r),
          "stale derived lighting cannot override changed authoritative pixels");
    const auto folder =
        output.empty()
            ? std::filesystem::temp_directory_path() /
                  ("cartographer-environment-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
            : output;
    std::filesystem::create_directories(folder);
    static_cast<void>(take(f.document.save_atomic(folder / "environment-cpu.carto")));
    application::ApplicationSession session;
    ui::UiController ui(session);
    static_cast<void>(
        take(ui.dispatch(application::OpenProjectAction{folder / "environment-cpu.carto"})));
    auto snap = session.snapshot();
    changed = snap.presentation_catalog;
    okay(changed.set_environment(rotated));
    static_cast<void>(take(ui.dispatch(application::SetPresentationCatalogAction{
        snap.project_generation, snap.project_revision, changed})));
    check(take(session.render_authoring_viewport(r, session.snapshot().project_revision))
                  .display_rgba8 == after.display_rgba8,
          "existing authoring action executes environment edit");
    static_cast<void>(take(ui.dispatch(application::UndoAction{})));
    check(take(session.render_authoring_viewport(r, session.snapshot().project_revision))
                  .display_rgba8 == original.display_rgba8,
          "environment undo restores authored appearance");
    static_cast<void>(take(ui.dispatch(application::RedoAction{})));
    static_cast<void>(take(ui.dispatch(application::SaveProjectAction{std::nullopt})));
    const auto reopened = take(project::ProjectDocument::load(folder / "environment-cpu.carto"));
    check(take(production::render_authoring_viewport(reopened, r)).display_rgba8 ==
              after.display_rgba8,
          "environment edit/redo/save/reopen preserves appearance");
    if (!output.empty()) {
        std::ofstream image(folder / "environment-cpu.ppm", std::ios::binary);
        image << "P6\n" << after.width << ' ' << after.height << "\n255\n";
        for (std::size_t p = 0; p < after.objects.size(); ++p)
            image.write(reinterpret_cast<const char *>(&after.display_rgba8[p * 4]), 3);
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        catalog_contract();
        lighting_contract();
        viewport_and_session(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        std::cout
            << "CPU environment: directional diffuse/specular, roughness/BRDF, HDR/rotation/AO, "
               "source admission and edit/undo/reopen passed; no GPU device\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
