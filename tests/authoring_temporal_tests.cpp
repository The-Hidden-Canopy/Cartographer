#include "authoring_viewport_fixture.hpp"
#include <carto/production/authoring_temporal.hpp>
#include <iostream>
#include <numeric>

namespace {
using Reject = render::TemporalRejection;
std::uint64_t accepted(const production::AuthoringTemporalFrame &f) { return f.counts[0]; }
void resolver_contract() {
    std::vector<core::Vec3d> current, history;
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 3; ++x)
            current.push_back({10. + 10 * x, 10. + 10 * x, 10. + 10 * x});
    history = current;
    std::vector<double> depth(9, 7), previous_depth(9, 2);
    std::vector<render::TemporalSurface> ids(9, {1, 1}), old_ids = ids;
    std::vector<render::TemporalPreviousSample> samples(9);
    samples[4] = {1.25, 1.25, 2, true};
    render::TemporalReprojectionInput input;
    input.width = input.height = 3;
    input.current_hdr = current;
    input.history_hdr = history;
    input.current_depth = depth;
    input.history_depth = previous_depth;
    input.current_surface = ids;
    input.history_surface = old_ids;
    input.previous = samples;
    input.current_revision = input.history_revision = core::Revision{7};
    input.history_valid = true;
    input.settings.feedback = .8;
    input.settings.clip_expansion = 0;
    auto result = take(render::resolve_temporal_reprojected_reference(input));
    check(std::abs(result.linear_hdr[4].x - 22) < 1e-10 && result.counts[0] == 1,
          "subpixel bilinear history must not round motion; compare against previous-camera depth");
    check(std::accumulate(result.counts.begin(), result.counts.end(), std::uint64_t{}) == 9,
          "one exclusive rejection/acceptance classification per pixel");
    old_ids[8] = {2, 1};
    result = take(render::resolve_temporal_reprojected_reference(input));
    check(result.rejection[4] == Reject::identity && result.linear_hdr[4].x == 20,
          "even one contributing bilinear identity mismatch rejects history without edge bleed");
    old_ids = ids;
    previous_depth[8] = 3;
    result = take(render::resolve_temporal_reprojected_reference(input));
    check(result.rejection[4] == Reject::depth && result.linear_hdr[4].x == 20,
          "disocclusion at one footprint tap rejects history");
    previous_depth[8] = 2;
    samples[4].x = -.01;
    check(take(render::resolve_temporal_reprojected_reference(input)).rejection[4] ==
              Reject::offscreen,
          "offscreen reprojection never clamps to a border history texel");
    samples[4] = {1, 1, 2, true};
    input.camera_cut = true;
    check(take(render::resolve_temporal_reprojected_reference(input))
                  .counts[unsigned(Reject::camera_cut)] == 9,
          "camera cut rejects all history");
    input.camera_cut = false;
    input.current_revision = core::Revision{8};
    check(take(render::resolve_temporal_reprojected_reference(input))
                  .counts[unsigned(Reject::revision)] == 9,
          "source revision rejects all history");
    input.current_revision = input.history_revision;
    input.content_changed = true;
    check(take(render::resolve_temporal_reprojected_reference(input))
                  .counts[unsigned(Reject::revision)] == 9,
          "source content identity cannot be replaced by equal revision numbers");
    input.content_changed = false;
    input.settings.feedback = 0;
    check(!take(render::resolve_temporal_reprojected_reference(input)).history_used(),
          "zero feedback disables history rather than reporting unused history as accepted");
    input.settings.feedback = .8;
    history[4] = {10000, 10000, 10000};
    result = take(render::resolve_temporal_reprojected_reference(input));
    check(result.linear_hdr[4].x < 30.01 && result.linear_hdr[4].x > 1,
          "same-surface neighborhood clipping bounds stale HDR without clamping HDR to display "
          "range");
    history[4] = {20, 20, 20};
    current[4] = {22, 22, 22};
    result = take(render::resolve_temporal_reprojected_reference(input));
    check(std::abs(result.linear_hdr[4].x - 20) < std::abs(current[4].x - 20),
          "eligible stable samples attenuate temporal noise");
    history[4].x = std::numeric_limits<double>::quiet_NaN();
    check(!render::resolve_temporal_reprojected_reference(input),
          "nonfinite history fails before publication");
    input.history_valid = false;
    input.history_hdr = {};
    input.history_depth = {};
    input.history_surface = {};
    check(!take(render::resolve_temporal_reprojected_reference(input)).history_used(),
          "initial frame never requires invented history attachments");
    input.width = std::numeric_limits<unsigned>::max();
    check(!render::resolve_temporal_reprojected_reference(input),
          "temporal extent allocation is bounded");
}
void authoring_contract() {
    auto f = fixture();
    auto r = request(f);
    production::AuthoringTemporalHistory history;
    const auto initial = take(history.render(f.document, r));
    check(initial.reset == production::AuthoringHistoryReset::initial && accepted(initial) == 0,
          "initial authored frame starts without history");
    const auto stable = take(history.render(f.document, r));
    check(stable.reset == production::AuthoringHistoryReset::none && accepted(stable) > 300,
          "real authored mesh reprojects stable visible surfaces");
    check(stable.image.objects == initial.image.objects &&
              stable.image.depth == initial.image.depth &&
              stable.image.faces == initial.image.faces,
          "temporal resolve preserves depth/picking/corner face identity");
    check(stable.history_attachment_bytes == 64 * 64 * 48,
          "bounded history attachment accounting excludes scratch/output/pose maps");
    check(std::abs(stable.cpu_prepare_ms + stable.cpu_resolve_ms + stable.cpu_display_ms +
                   stable.cpu_publish_ms - stable.cpu_total_ms) < 1e-6,
          "matched CPU intervals reconcile without counting overlapping medians");
    auto bad = take(production::render_authoring_viewport(f.document, r));
    bad.linear_hdr[center(bad)].x = std::numeric_limits<double>::quiet_NaN();
    check(!history.resolve(f.document, r, std::move(bad)),
          "failed current-frame replacement must not publish history");
    check(accepted(take(history.render(f.document, r))) == accepted(stable),
          "last good history survives failed resolve");
    auto revealing = r;
    revealing.transform_overrides[f.front] = {{1.2, 0, 0}};
    auto revealed_current = take(production::render_authoring_viewport(f.document, revealing));
    const auto revealed = take(history.resolve(f.document, revealing, revealed_current));
    std::uint64_t disoccluded = 0, uncovered = 0;
    for (std::size_t p = 0; p < revealed.image.objects.size(); ++p) {
        if (stable.image.objects[p] == f.front && revealed.image.objects[p] == f.back) {
            ++disoccluded;
            check((revealed.image.linear_hdr[p] - revealed_current.linear_hdr[p]).length() < 1e-12,
                  "revealed back surface rejects previous front color without a trail");
        }
        if (stable.image.objects[p] && !revealed.image.objects[p]) {
            ++uncovered;
            check(revealed.image.display_rgba8[p * 4 + 3] == 0,
                  "moving silhouette leaves transparent authoring background, not a trail");
        }
    }
    check(disoccluded > 50 && revealed.counts[unsigned(Reject::identity)] > 0,
          "actual moving authored occluder reveals a differently identified back surface");
    history.clear();
    static_cast<void>(take(history.render(f.document, r)));
    auto silhouette = revealing;
    silhouette.transform_overrides[f.back] = {{1.2, 0, -1}};
    const auto empty = take(history.render(f.document, silhouette));
    uncovered = 0;
    for (std::size_t p = 0; p < empty.image.objects.size(); ++p)
        if (stable.image.objects[p] && !empty.image.objects[p]) {
            ++uncovered;
            check(empty.image.display_rgba8[p * 4 + 3] == 0 &&
                      empty.image.linear_hdr[p].length_squared() == 0,
                  "moving all covering silhouettes exposes empty background without color/alpha "
                  "trails");
        }
    check(uncovered > 50, "silhouette sequence exercises newly uncovered background");
    std::cout << "CPU temporal trajectory: " << disoccluded << " disoccluded surface samples, "
              << uncovered << " uncovered background samples\n";
    history.clear();
    static_cast<void>(take(history.render(f.document, r)));
    auto exposed = r;
    exposed.exposure_ev = 2;
    const auto brighter = take(history.render(f.document, exposed));
    check(brighter.reset == production::AuthoringHistoryReset::none &&
              brighter.image.display_rgba8 != stable.image.display_rgba8,
          "exposure changes display without resetting unexposed history");
    for (std::size_t p = 0; p < brighter.image.linear_hdr.size(); ++p)
        check((brighter.image.linear_hdr[p] - stable.image.linear_hdr[p]).length() < 1e-8,
              "history never receives camera exposure");
    const auto encoded = take(render::encode_display_rgba8(
        brighter.image.linear_hdr,
        [&] {
            std::vector<std::uint8_t> c(brighter.image.objects.size());
            for (std::size_t p = 0; p < c.size(); ++p)
                c[p] = brighter.image.objects[p] ? 255 : 0;
            return c;
        }(),
        2, r.tone_mode));
    check(encoded == brighter.image.display_rgba8,
          "resolved HDR gets exactly one selected display transform");
    auto motion = r;
    motion.transform_overrides[f.front] = {{.02, 0, 0}, {}, {-1.3, .75, 1}};
    history.clear();
    static_cast<void>(take(history.render(f.document, motion)));
    motion.transform_overrides[f.front].translation.x += .015;
    const auto moved = take(history.render(f.document, motion));
    check(moved.reset == production::AuthoringHistoryReset::none && accepted(moved) > 200,
          "subpixel rigid motion supports mirrored and nonuniform object transforms");
    auto orbit = motion;
    orbit.camera.reset();
    orbit.view = moved.image.view;
    orbit.view.position.x += .02;
    const auto camera_motion = take(history.render(f.document, orbit));
    check(camera_motion.reset == production::AuthoringHistoryReset::none &&
              accepted(camera_motion) > 200,
          "orthographic camera movement uses actual prior projection");
    auto perspective = orbit;
    perspective.view.projection = project::CameraProjection::perspective;
    perspective.view.vertical_fov_radians = 1;
    check(take(history.render(f.document, perspective)).reset ==
              production::AuthoringHistoryReset::lens,
          "projection/lens edit resets incompatible history");
    perspective.view.position.z += .25;
    const auto dolly = take(history.render(f.document, perspective));
    check(dolly.reset == production::AuthoringHistoryReset::none && accepted(dolly) > 150,
          "perspective reprojection validates expected old depth after camera dolly");
    const auto cut = take(history.render(f.document, perspective, true));
    check(cut.reset == production::AuthoringHistoryReset::camera_cut && accepted(cut) == 0,
          "explicit camera discontinuity never blends");
    auto teleport = perspective;
    teleport.view.position.x += 100;
    check(take(history.render(f.document, teleport)).reset ==
              production::AuthoringHistoryReset::camera_cut,
          "large undeclared camera jumps reset conservatively");
    history.clear();
    auto shadowed = r;
    shadowed.shadows.enabled = true;
    shadowed.shadows.resolution = 64;
    static_cast<void>(take(history.render(f.document, shadowed)));
    shadowed.transform_overrides[f.front] = {{.02, 0, 0}};
    const auto moving_shadow = take(history.render(f.document, shadowed));
    check(moving_shadow.reset == production::AuthoringHistoryReset::lighting_motion &&
              accepted(moving_shadow) == 0,
          "unsupported moving shadow correspondence resets all receivers rather than trailing");
    auto light_motion = shadowed;
    light_motion.shadows.enabled = false;
    static_cast<void>(take(history.render(f.document, light_motion)));
    light_motion.transform_overrides[f.light] = {{}, {0, .1, 0, .995}};
    check(take(history.render(f.document, light_motion)).reset ==
              production::AuthoringHistoryReset::lighting_motion,
          "transient light edits reset radiance history even without shadows");
    auto resize = r;
    resize.width = 80;
    check(take(history.render(f.document, resize)).reset ==
              production::AuthoringHistoryReset::extent,
          "resize replaces incompatible history");
    auto suspended = resize;
    suspended.width = 0;
    check(take(history.render(f.document, suspended)).reset ==
              production::AuthoringHistoryReset::suspended,
          "minimized zero extent suspends temporal storage");
    check(take(history.render(f.document, r)).reset == production::AuthoringHistoryReset::initial,
          "resume after suspension cannot reuse an older image");
    auto other = fixture();
    check(other.document.revision() == f.document.revision(),
          "distinct source fixture has same revision counter");
    check(take(history.render(other.document, request(other))).reset ==
              production::AuthoringHistoryReset::source,
          "reopen/different source with equal revision still invalidates by canonical content");
    auto materials = other.document.material_catalog();
    auto material = materials.materials().at(1);
    material.definition.roughness = .8;
    material.revision = material.revision.next();
    okay(materials.replace_material(material));
    set_materials(other.document, materials);
    check(take(history.render(other.document, request(other))).reset ==
              production::AuthoringHistoryReset::source,
          "material/geometry/document revision invalidates derived temporal lighting");

    // A bounded CPU timing receipt only. No GPU/kernel/FPS or speedup inference.
    history.clear();
    std::vector<double> resolve_times, complete_times;
    for (unsigned sample = 0; sample < 35; ++sample) {
        const auto frame = take(history.render(f.document, r));
        if (sample >= 5) {
            resolve_times.push_back(frame.cpu_total_ms);
            complete_times.push_back(frame.image.host_milliseconds);
        }
    }
    std::sort(resolve_times.begin(), resolve_times.end());
    std::sort(complete_times.begin(), complete_times.end());
    const auto median = [](const auto &v) { return (v[14] + v[15]) * .5; };
    std::cout << "CPU reference timing only:64x64,2 triangles,shadows off,5 warmup,30 samples; "
              << "temporal_ms_p50=" << median(resolve_times) << ",p95=" << resolve_times[28]
              << "; raster_plus_temporal_host_ms_p50=" << median(complete_times)
              << ",p95=" << complete_times[28] << '\n';
}
} // namespace
int main() {
    try {
        resolver_contract();
        authoring_contract();
        std::cout << "CPU temporal reprojection: bilinear HDR, footprint identity/depth/offscreen "
                     "rejection, YCoCg clipping, source/camera/lens/extent invalidation and rigid "
                     "motion passed; GPU/native history pending\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
