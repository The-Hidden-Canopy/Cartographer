#include "authoring_shadow_fixture.hpp"
#include <carto/render/lighting.hpp>
#include <iostream>

namespace {
void sampling_contract() {
    auto f = shadow_fixture();
    auto r = shadow_request(f);
    const auto direction = core::Vec3d{.4, 0, .8}.normalized();
    auto frame = *take(production::fit_directional_shadow(f.document, r, direction));
    const auto n = r.shadows.resolution;
    std::vector<float> depth(std::size_t(n) * n, .5F);
    depth[std::size_t(n / 2) * n + n / 2] = 1;
    // Known light-local receiver maps to center UV and normalized depth .7.
    const auto world =
        frame.view.position +
        frame.view.rotation.rotate(
            {0, 0, -(frame.view.near_plane + .7 * (frame.view.far_plane - frame.view.near_plane))});
    for (unsigned radius : {0U, 1U, 2U}) {
        frame.settings.radius = radius;
        const auto actual =
            take(production::sample_directional_shadow(frame, depth, world, direction, direction));
        const auto expected =
            take(render::evaluate_shadow_reference(depth, n, n, .5F, .5F, .7F, {radius, .0005F}));
        check(std::abs(actual - expected.visibility) < 1e-7,
              "spatial PCF matches existing kernel convention across supported radii");
    }
    check(take(production::sample_directional_shadow(frame, depth, {1e5, 1e5, 1e5}, direction,
                                                     direction)) == 1,
          "outside fitted volume is lit rather than clamped into a false shadow");
    auto orbit = r;
    orbit.view.position = {5, -7, 9};
    const auto other = *take(production::fit_directional_shadow(f.document, orbit, direction));
    check(other.view.position.x == frame.view.position.x &&
              other.view.position.y == frame.view.position.y &&
              other.view.orthographic_height == frame.view.orthographic_height,
          "camera orbit does not change the light texel grid");
    r.shadows.resolution = 63;
    check(!production::fit_directional_shadow(f.document, r, direction),
          "invalid shadow quality fails visibly");
    r.shadows.resolution = 128;
    r.shadows.slope_bias = -1;
    check(!production::render_authoring_viewport(f.document, r),
          "invalid shadow bias fails before rendering");
}
void scene_contract() {
    auto f = shadow_fixture();
    auto r = shadow_request(f);
    const auto shadowed = take(production::render_authoring_viewport(f.document, r));
    auto unlit = r;
    unlit.shadows.enabled = false;
    const auto direct = take(production::render_authoring_viewport(f.document, unlit));
    unsigned dark = 0, lit = 0;
    for (std::size_t p = 0; p < direct.objects.size(); ++p) {
        check(direct.objects[p] == shadowed.objects[p] && direct.depth[p] == shadowed.depth[p],
              "shadows preserve authored picking/depth");
        if (direct.objects[p] != f.back)
            continue;
        dark += shadowed.linear_hdr[p].x < direct.linear_hdr[p].x * .1;
        lit += std::abs(shadowed.linear_hdr[p].x - direct.linear_hdr[p].x) < 1e-9;
    }
    check(dark > 15 && lit > 500, "occluder casts a spatially bounded shadow on visible receiver");
    r.transform_overrides[f.front] = {{2, 0, 0}, {}, {.35, .35, 1}};
    const auto moved = take(production::render_authoring_viewport(f.document, r));
    check(moved.display_rgba8 != shadowed.display_rgba8,
          "moving occluder changes visible lighting");
    auto masked = f;
    auto mask_request = shadow_request(masked);
    mask_request.textures = mask_shadow_caster(masked);
    const auto cutout = take(production::render_authoring_viewport(masked.document, mask_request));
    unsigned reopened_light = 0;
    for (std::size_t p = 0; p < direct.objects.size(); ++p)
        if (direct.objects[p] == f.back && shadowed.objects[p] == f.back &&
            cutout.objects[p] == f.back && cutout.linear_hdr[p].x > shadowed.linear_hdr[p].x + .1)
            ++reopened_light;
    check(reopened_light > 5, "decoded alpha mask opens spatial lighting on the receiver");
    const auto reopened = take(project::ProjectDocument::deserialize(f.document.serialize()));
    check(take(production::render_authoring_viewport(reopened, shadow_request(f))).display_rgba8 ==
              shadowed.display_rgba8,
          "saved authored scene reproduces shadow lighting");
    project::PresentationCatalog presentation;
    okay(presentation.insert_camera(f.document.presentation_catalog().cameras().at(f.camera)));
    auto light = f.document.presentation_catalog().lights().at(f.light);
    light.casts_shadows = false;
    light.revision = light.revision.next();
    okay(presentation.insert_light(light));
    mutate(f.document, [&](auto &tx) { okay(tx.set_presentation_catalog(presentation)); });
    r = shadow_request(f);
    check(take(production::render_authoring_viewport(f.document, r)).display_rgba8 ==
              direct.display_rgba8,
          "authored casts_shadows controls spatial shadows");
}
void sloped_plane_without_occluder() {
    auto f=shadow_fixture();
    auto catalog=f.document.material_catalog();
    auto caster=catalog.materials().at(1);caster.revision=caster.revision.next();
    caster.definition.alpha_mode=render::AlphaMode::masked;caster.definition.base_color_factor.w=0;
    okay(catalog.replace_material(caster));set_materials(f.document,std::move(catalog));
    auto request=shadow_request(f);
    auto baseline=request;baseline.shadows.enabled=false;
    const auto direct=take(production::render_authoring_viewport(f.document,baseline));
    for(unsigned radius:{0U,1U,2U}) {
        request.shadows.radius=radius;
        const auto shadowed=take(production::render_authoring_viewport(f.document,request));
        unsigned checked=0;
        for(std::size_t p=0;p<direct.objects.size();++p) if(direct.objects[p]==f.back) {
            check(std::abs(shadowed.linear_hdr[p].x-direct.linear_hdr[p].x)<1e-9,
                  "sloped plane with no occluder must not acquire PCF self-shadow acne");++checked;
        }
        check(checked>1000,"sloped-plane regression covers a substantial receiver");
    }
}
} // namespace
int main() {
    try {
        sampling_contract();
        scene_contract();
        sloped_plane_without_occluder();
        std::cout << "directional shadow fitting, PCF, spatial occlusion, motion, picking and "
                     "reopen passed (CPU only)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
