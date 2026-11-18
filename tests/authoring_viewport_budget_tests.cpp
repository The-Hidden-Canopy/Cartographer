#include <carto/production/authoring_viewport.hpp>
#include "authoring_triangle_budget_fixture.hpp"
#include <iostream>
#include <limits>
using namespace carto::production;
static_assert(valid_authoring_native_extent(3840, 2160));
static_assert(valid_authoring_native_extent(2160, 3840));
static_assert(!valid_authoring_native_extent(3840, 2161));
static_assert(!valid_authoring_native_extent(4097, 1));
static_assert(!valid_authoring_native_extent(0, 2160));
static_assert(!valid_authoring_native_extent(std::numeric_limits<unsigned>::max(), std::numeric_limits<unsigned>::max()));
static_assert(kAuthoringNativeMaxPixels <= kAuthoringNativeAttachmentBudget / 128ULL);
static_assert(kAuthoringCpuMaxPixels == 1048576ULL);
int main() try {
    const auto f = triangle_budget_fixture();
    const auto original = f.document.serialize();
    auto r = request(f);
    r.width = r.height = 1;
    const auto accepted = take(render_authoring_viewport(f.document, r));
    check(accepted.display_rgba8[center(accepted) * 4 + 3] != 0,
          "CPU reference renders the exact aggregate instance-triangle boundary");
    const auto overflow = one_triangle_over_scene_budget(f);
    const auto overflow_bytes = overflow.serialize();
    const auto refused = render_authoring_viewport(overflow, r);
    check(!refused && refused.error().message.find("triangle budget exceeded") != std::string::npos,
          "CPU reference rejects exactly one triangle above the scene bound");
    check(take(render_authoring_viewport(f.document, r)).display_rgba8 == accepted.display_rgba8,
          "CPU reference recovers the accepted scene after a rejected input");
    check(original == f.document.serialize() && overflow_bytes == overflow.serialize(),
          "reference admission and rejection never mutate either source project");
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
