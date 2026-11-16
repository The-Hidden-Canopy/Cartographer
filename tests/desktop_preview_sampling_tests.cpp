#include "../apps/desktop/studio_rendered_preview.hpp"
#include "authoring_viewport_fixture.hpp"
#ifdef CARTO_PREVIEW_SAMPLING_GPU
#include <carto/d3d12/authoring_viewport.hpp>
#endif
#include <algorithm>
#include <iostream>
#include <limits>

int main() try {
    using namespace carto;
    const auto extent = [](double w, double h, int mode, bool cpu = false) {
        return studio::preview_render_extent(w, h, mode,
            cpu ? production::kAuthoringCpuMaxPixels : production::kAuthoringNativeMaxPixels,
            production::kAuthoringNativeMaxDimension);
    };
    auto f = fixture();
    geometry::EditableMesh hair;
    // 0.3 canvas pixels wide, between pixel centers. At 100% it vanishes
    // entirely; 200% resolves coverage within that same displayed footprint.
    okay(hair.insert_bulk(
        {{geometry::VertexId{1}, {-.05625, -1, 0}},
         {geometry::VertexId{2}, {-.0375, -1, 0}},
         {geometry::VertexId{3}, {-.0375, 1, 0}},
         {geometry::VertexId{4}, {-.05625, 1, 0}}},
        {{geometry::FaceId{1}, {geometry::VertexId{1}, geometry::VertexId{2}, geometry::VertexId{3}}},
         {geometry::FaceId{2}, {geometry::VertexId{1}, geometry::VertexId{3}, geometry::VertexId{4}}}}));
    attributes::AttributeSet attrs({{attributes::AttributeDomain::face, 2}});
    okay(attrs.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                           attributes::AttributeType::uint32}, {std::uint32_t{1}, std::uint32_t{1}}}));
    okay(hair.set_attribute_payload(std::move(attrs), {}));
    mutate(f.document, [&](auto& tx) {okay(tx.replace_mesh(f.front_mesh, std::move(hair)));});
    const auto source = f.document.serialize();
    auto r = request(f);
    auto old_frame = take(production::render_authoring_viewport(f.document, r));
    check(std::count(old_frame.objects.begin(), old_frame.objects.end(), f.front) == 0,
        "fixture no longer reproduces missing subpixel hair at 100%");
    const auto quality = extent(64, 64, 0);
    check(quality.has_value() && !quality->capped, "default fine-detail extent rejected");
    r.width = quality->width;
    r.height = quality->height;
    auto fine_frame = take(production::render_authoring_viewport(f.document, r));
    check(std::count(fine_frame.objects.begin(), fine_frame.objects.end(), f.front) > 40,
        "default preview sampling still loses subpixel strands");
    const auto p = std::size_t(64) * fine_frame.width + 62;
    check(fine_frame.objects[p] == f.front && fine_frame.linear_hdr[p].x > .1,
        "fine-detail sample lost the hair identity or actual shading");
#ifdef CARTO_PREVIEW_SAMPLING_GPU
    auto gpu = take(d3d12::AuthoringViewport::create());
    const auto native_old = take(gpu->render(f.document, request(f))).image;
    check(std::count(native_old.objects.begin(), native_old.objects.end(), f.front) == 0,
        "native fixture no longer reproduces missing subpixel hair at 100%");
    const auto native_fine = take(gpu->render(f.document, r)).image;
    check(std::count(native_fine.objects.begin(), native_fine.objects.end(), f.front) > 40 &&
        native_fine.objects[p] == f.front && native_fine.linear_hdr[p].x > .1,
        "native fine-detail preview still loses subpixel hair coverage/shading");
#endif
    check(f.document.serialize() == source, "preview quality mutated the source document");
    const auto native_4k = extent(1920, 1080, 0);
    check(native_4k && native_4k->width == 3840 && native_4k->height == 2160 && !native_4k->capped,
        "native fine-detail preview lost its admitted 4K extent");
    for (const auto [w,h] : {std::pair{3840.,2160.}, std::pair{1e200,1e200},
                             std::pair{1.,1e9}, std::pair{1e9,1.}}) {
        for (const bool cpu : {false,true}) {
            const auto limited = extent(w,h,0,cpu);
            const auto budget = cpu ? production::kAuthoringCpuMaxPixels : production::kAuthoringNativeMaxPixels;
            check(limited && limited->width && limited->height && limited->capped &&
                limited->width <= production::kAuthoringNativeMaxDimension &&
                limited->height <= production::kAuthoringNativeMaxDimension &&
                std::uint64_t(limited->width) * limited->height <= budget,
                "fine-detail preview escaped renderer memory/dimension budgets");
        }
    }
    check(extent(64,64,1)->width == 64 && extent(64,64,2)->width == 32 &&
          extent(64,64,3)->width == 16, "existing lower quality choices changed");
    check(!extent(64,64,-1) && !extent(64,64,4) && !extent(0,64,0) &&
          !extent(-1,64,0) && !extent(std::numeric_limits<double>::quiet_NaN(),64,0) &&
          !extent(64,std::numeric_limits<double>::infinity(),0) &&
          !extent(std::numeric_limits<double>::denorm_min(),1e200,0) &&
          !studio::preview_render_extent(64,64,0,0,4096) &&
          !studio::preview_render_extent(64,64,0,1024,0),
          "invalid quality/canvas input was admitted");
    std::cout << "fine-detail preview recovers subpixel hair within native/CPU budgets\n";
    return 0;
} catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
