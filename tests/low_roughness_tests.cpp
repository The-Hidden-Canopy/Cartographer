#include "authoring_surface_fixture.hpp"
#ifdef CARTO_LOW_ROUGHNESS_GPU
#include <carto/d3d12/authoring_viewport.hpp>
#endif
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>

namespace {
constexpr std::array<double, 4> roughnesses{.03, .055, .10, .16};

render::StandardMaterial wet_material(bool coat, double roughness) {
    render::StandardMaterial m;
    m.base_color_factor = {0, 0, 0, 1};
    m.dielectric_f0 = coat ? 0 : .04;
    m.roughness = coat ? .65 : roughness;
    m.clearcoat = coat ? 1 : 0;
    m.clearcoat_roughness = roughness;
    return m;
}

double response(bool coat, double roughness, double light_angle = 0) {
    const double angle = light_angle * std::numbers::pi / 180;
    const auto p = take(render::evaluate_pbr_reference(
        wet_material(coat, roughness), {}, {{std::sin(angle), 0, std::cos(angle)}, {1, 1, 1}}));
    const auto value = coat ? p.clearcoat_specular : p.direct_specular;
    check(value.finite() && value.x >= 0, "wet-surface reference must remain finite/nonnegative");
    return value.x;
}

void distinct_peaks(const std::array<double, 4>& peaks) {
    check(peaks[0] > peaks[1] * 1.5 && peaks[1] > peaks[2] * 3 &&
              peaks[2] > peaks[3] * 3,
          "authored low roughness values must produce distinct specular peaks");
}

void cpu_contract() {
    for (bool coat : {false, true}) {
        std::array<double, 4> peaks{};
        for (std::size_t i = 0; i < peaks.size(); ++i) {
            peaks[i] = response(coat, roughnesses[i]);
            std::cout << (coat ? "coat" : "base") << " r=" << roughnesses[i]
                      << " peak=" << peaks[i] << '\n';
        }
        distinct_peaks(peaks);
        check(response(coat, .055, .5) / peaks[1] < .3 &&
                  response(coat, .16, .5) / peaks[3] > .8,
              "wet and rougher lobes must have different angular widths");
        check(response(coat, 0) == response(coat, .03) &&
                  response(coat, .03) == response(coat, .045),
              "the minimum applies to perceptual roughness, with finite zero-roughness response");
        check(response(coat, .1, 120) == 0,
              "a light below the surface cannot create a specular lobe");
        check(std::isfinite(response(coat, 1)), "rough endpoint must remain finite");
    }
    // Analytic normal-incidence limit at a roughness unaffected by the repair.
    check(std::abs(response(false, .4) - .124339799290543) < 1e-10,
          "ordinary roughness must preserve its established response");
    for (bool coat : {false, true}) {
        auto m = wet_material(coat, .1);
        (coat ? m.clearcoat_roughness : m.roughness) =
            std::numeric_limits<double>::quiet_NaN();
        check(!render::evaluate_pbr_reference(m, {}, {{0, 0, 1}, {1, 1, 1}}),
              "invalid authored roughness must fail rather than being repaired by a floor");
    }
}

void environment_contract() {
    project::AuthoredEnvironment e;
    e.identity = "test.low-roughness-environment";
    e.width = 4;
    e.height = 2;
    e.linear_radiance.assign(8, {1, 1, 1});
    const auto lighting = take(production::compile_environment_lighting(e));
    const auto smooth = lighting->brdf(.5, .055), rough = lighting->brdf(.5, .16);
    std::cout << "environment smooth=" << smooth[0] << ',' << smooth[1]
              << " rough=" << rough[0] << ',' << rough[1] << '\n';
    check(std::abs(smooth[0] - rough[0]) + std::abs(smooth[1] - rough[1]) > 1e-4,
          "split-sum integration must retain low-roughness material differences");
    check((lighting->specular({0, 0, 1}, .03) - core::Vec3d{1, 1, 1}).length() < 1e-5,
          "sharper prefilter must preserve a constant environment's radiance");
}

#ifdef CARTO_LOW_ROUGHNESS_GPU
void gpu_contract() {
    auto f = fixture();
    mutate(f.document, [&](auto& tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle(9))); });
    production::AuthoringTextureLibrary textures;
    const auto white = pixels(1, 1, 1, {{255, 255, 255, 255}});
    okay(textures.admit(white.record, white.encoded));
    auto source_catalog = f.document.material_catalog();
    okay(source_catalog.insert_texture(white.record));
    set_materials(f.document, std::move(source_catalog));
    auto view = request(f);
    view.textures = take(textures.snapshot());
    // Odd extent puts a sample exactly at the plane's normal-incidence point.
    view.width = view.height = 65;
    auto gpu = take(d3d12::AuthoringViewport::create());
    const auto render_value = [&](const render::StandardMaterial& definition, bool parity = true) {
        auto catalog = f.document.material_catalog();
        auto m = catalog.materials().at(1);
        m.definition = definition;
        // Exercise the same per-fragment material path used by textured eyes/fur.
        // A neutral source keeps the independent numeric expectation unchanged.
        m.definition.base_color = {1, 9};
        m.revision = m.revision.next();
        okay(catalog.replace_material(m));
        set_materials(f.document, std::move(catalog));
        const auto native = take(gpu->render(f.document, view));
        const auto p = center(native.image);
        check(native.image.objects[p] == f.front,
              "wet-surface sample must retain visible front-object identity");
        const double b = native.image.linear_hdr[p].x;
        check(std::isfinite(b) && b > 0,
              "sharp wet-surface HDR must remain finite and positive");
        if (parity) {
            const auto cpu = take(production::render_authoring_viewport(f.document, view));
            const double a = cpu.linear_hdr[p].x;
            std::cout << native.device_name << " cpu=" << a << " gpu=" << b << '\n';
            check(cpu.objects[p] == f.front && std::isfinite(a) && a > 0,
                  "reference wet-surface sample must retain finite front-object radiance");
            check(std::abs(a - b) <= .005 * std::max(a, b),
                  "CPU and native GPU must agree on low-roughness linear HDR");
        } else {
            std::cout << native.device_name << " anisotropic GPU-only=" << b << '\n';
        }
        return b;
    };
    double isotropic_floor_peak = 0;
    for (bool coat : {false, true}) {
        std::array<double, 4> peaks{};
        for (std::size_t i = 0; i < peaks.size(); ++i)
            peaks[i] = render_value(wet_material(coat, roughnesses[i]));
        if (!coat) isotropic_floor_peak = peaks[0];
        distinct_peaks(peaks);
    }
    auto metal = wet_material(false, 0);
    metal.base_color_factor = {1, 1, 1, 1};
    metal.metallic = 1;
    check(render_value(metal) > 65504,
          "valid sharp metallic HDR must remain unclamped above the half-float limit");
    auto anisotropic = wet_material(false, .03);
    anisotropic.anisotropy = .5;
    const auto floor_peak = render_value(anisotropic, false);
    anisotropic.roughness = .045;
    const auto minimum_peak = render_value(anisotropic, false);
    anisotropic.roughness = .055;
    const auto wider_peak = render_value(anisotropic, false);
    check(floor_peak < isotropic_floor_peak * .25,
          "anisotropy fixture must actually exercise a stretched lobe");
    check(std::abs(floor_peak - minimum_peak) < 1e-5 * minimum_peak &&
              minimum_peak > wider_peak * 1.2,
          "anisotropic GPU lobe must use the same perceptual roughness floor");
}
#endif
} // namespace

int main(int argc, char** argv) try {
    std::cout << std::setprecision(17);
    if (argc == 1) cpu_contract();
    else if (argc == 2 && std::string_view(argv[1]) == "--environment") environment_contract();
#ifdef CARTO_LOW_ROUGHNESS_GPU
    else if (argc == 2 && std::string_view(argv[1]) == "--gpu") gpu_contract();
#endif
    else throw std::runtime_error("unsupported low-roughness test mode");
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
