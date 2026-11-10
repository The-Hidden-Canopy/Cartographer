#include <bit>
#include <carto/d3d12/authoring_temporal.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace carto;
void check(bool value, const char *s) {
    if (!value)
        throw std::runtime_error(s);
}
template <class T> T take(core::Result<T> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
    return std::move(r.value());
}
} // namespace
void host_lifecycle() {
    using namespace carto;
    using Reset = d3d12::AuthoringTemporalReset;
    d3d12::AuthoringTemporalHostState state, foreign;
    std::vector<geometry::FaceId> faces{{0xfedcba9876543210ULL}};
    std::vector<d3d12::AuthoringTemporalDraw> draws{{0xabcdef1234567890ULL, {}, faces}};
    d3d12::AuthoringTemporalHostInput in;
    in.epoch = {assets::sha256(std::array<std::uint8_t, 1>{7}), core::Revision{1}, 64, 32};
    in.view.position = {0, 0, 5};
    in.view.rotation = {0, 0, 0, 1};
    in.view.far_plane = 100;
    in.draws = draws;
    in.session = 1;
    auto first = take(state.prepare(in));
    check(first.reset == Reset::initial && !first.resolve.constants.extent_flags[2],
          "first native history is invalid");
    check(first.motion.extent_draws == std::array<unsigned, 4>{64, 32, 256, 1} &&
              first.resolve.pipeline.sampled_texture_count == 4,
          "packed native kernel bindings");
    check((*first.face_words)[0] == 0x76543210 && (*first.face_words)[1] == 0xfedcba98 &&
              first.draw_words[0] == 0x34567890 && first.draw_words[1] == 0xabcdef12,
          "palette keeps full identity64");
    auto concurrent = take(state.prepare(in));
    check(bool(state.commit(first)), "submit publishes native history");
    check(!state.commit(concurrent) && !foreign.commit(first), "stale and foreign commits reject");
    auto stable = take(state.prepare(in));
    check(stable.reset == Reset::none && stable.resolve.constants.extent_flags[2] &&
              stable.face_words == first.face_words,
          "stable history and topology palette reuse");
    auto failed = in;
    failed.epoch.width = 0;
    check(!state.prepare(failed), "bad preparation rejected");
    check(take(state.prepare(in)).reset == Reset::none,
          "failed preparation preserves prior history");
    auto expect = [&](const d3d12::AuthoringTemporalHostInput &changed, Reset r) {
        auto p = take(state.prepare(changed));
        check(p.reset == r && !p.resolve.constants.extent_flags[2], "native reset rejects history");
    };
    auto changed = in;
    changed.epoch.source.bytes[0] ^= 1;
    expect(changed, Reset::source);
    changed = in;
    changed.epoch.revision = core::Revision{2};
    expect(changed, Reset::source);
    changed = in;
    changed.epoch.width = 63;
    expect(changed, Reset::extent);
    changed = in;
    changed.session = 2;
    expect(changed, Reset::session);
    changed = in;
    changed.view.orthographic_height = 11;
    expect(changed, Reset::lens);
    changed = in;
    changed.quality.bytes[0] = 1;
    expect(changed, Reset::quality);
    changed = in;
    changed.lighting.bytes[0] = 1;
    expect(changed, Reset::lighting);
    changed = in;
    changed.camera_cut = true;
    expect(changed, Reset::camera_cut);
    changed = in;
    changed.view.position.x = 20;
    expect(changed, Reset::camera_cut);
    // Transient rigid affine motion includes mirrored/nonuniform scales, independent of source
    // edits.
    draws[0].transform.translation = {.1, .2, .3};
    draws[0].transform.scale = {-2, 3, .5};
    auto moving = take(state.prepare(in));
    check(moving.reset == Reset::none && moving.draw_words[20] == 1,
          "rigid motion supports native reprojection");
    const auto transform_point = [&](std::array<double, 4> p) {
        std::array<double, 4> out{};
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned r = 0; r < 4; ++r)
                out[r] += std::bit_cast<float>(moving.draw_words[4 + c * 4 + r]) * p[c];
        return out;
    };
    const auto old = transform_point({-.3, 1.1, .5, 1});
    check(std::abs(old[0] - .2) < 1e-6 && std::abs(old[1] - .3) < 1e-6 &&
              std::abs(old[2] - .4) < 1e-6,
          "nonuniform mirrored world-to-old-world matrix");
    changed = in;
    changed.spatial_shadows = true;
    expect(changed, Reset::moving_shadow);
    draws[0].transform.scale.x = 0;
    check(!state.prepare(in), "singular native motion rejected");
    draws[0].transform = {};
    draws[0].object = 9;
    auto unsupported = take(state.prepare(in));
    check(unsupported.draw_words[20] == 0, "unmatched identity cannot fabricate prior motion");
    draws[0].object = 0xabcdef1234567890ULL;
    state.invalidate();
    check(!state.commit(stable) && take(state.prepare(in)).reset == Reset::initial,
          "suspend/failure invalidates pending candidates");
    for (const auto projection :
         {project::CameraProjection::orthographic, project::CameraProjection::perspective}) {
        in.view.projection = projection;
        auto p = take(state.prepare(in));
        const auto vp = take(d3d12::authoring_view_projection(in.view, 2));
        const std::array<double, 4> world{.2, .3, 0, 1};
        std::array<double, 4> clip{}, back{};
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned r = 0; r < 4; ++r)
                clip[r] += vp[c * 4 + r] * world[c];
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned r = 0; r < 4; ++r)
                back[r] += p.motion.inverse_view_projection[c * 4 + r] * clip[c];
        for (unsigned r = 0; r < 3; ++r)
            check(std::abs(back[r] / back[3] - world[r]) < 2e-4,
                  "GPU unprojection matrix matches raster camera");
        const auto projected = take(in.view.project({.2, .3, 0}, 64, 32));
        check(std::abs((clip[0] / clip[3] * .5 + .5) * 64 - projected.x) < 1e-4 &&
                  std::abs((.5 - clip[1] / clip[3] * .5) * 32 - projected.y) < 1e-4,
              "camera matrix matches authoring project convention");
    }
    check(!take(d3d12::compile_hlsl(d3d12::authoring_motion_shader())).bytes.empty(),
          "actual GPU motion producer DXIL compiles without device");
    check(!take(d3d12::compile_hlsl(d3d12::authoring_packed_temporal_shader())).bytes.empty(),
          "actual packed temporal resolve DXIL compiles without device");
}
int main() {
    try {
        host_lifecycle();
        d3d12::AuthoringTemporalEpoch epoch{assets::sha256(std::array<std::uint8_t, 1>{42}),
                                            core::Revision{7}, 9, 5};
        const auto plan = take(d3d12::prepare_authoring_temporal(epoch, epoch, true, false));
        check(plan.groups == std::array<unsigned, 3>{2, 1, 1} &&
                  plan.pipeline.sampled_texture_count == 7 &&
                  plan.pipeline.descriptor_bindings.size() == 8 &&
                  plan.pipeline.push_constant_bytes == 32,
              "temporal kernel has bounded explicit compute bindings/dispatch");
        gpu::ShaderRegistry pipeline_shaders;
        const auto handle=take(pipeline_shaders.insert({"temporal.contract",gpu::ShaderStage::compute,"CSMain",{1}}));
        auto validate_plan=[&](gpu::PipelineDesc descriptor) {
            descriptor.shaders={handle};
            check(descriptor.color_format==gpu::Format::unknown &&
                      descriptor.depth_format==gpu::Format::unknown,
                  "temporal compute plans do not invent attachment formats");
            check(bool(gpu::validate(descriptor,std::array{gpu::ShaderStage::compute})),
                  "temporal plan satisfies actual shared pipeline contract");
        };
        validate_plan(plan.pipeline);
        d3d12::AuthoringTemporalHostState contract_host;
        const std::vector<geometry::FaceId> contract_faces{{1}};
        const std::vector<d3d12::AuthoringTemporalDraw> contract_draws{{1,{},contract_faces}};
        d3d12::AuthoringTemporalHostInput contract_input;
        contract_input.epoch=epoch;contract_input.draws=contract_draws;
        contract_input.view.position={0,0,5};
        validate_plan(take(contract_host.prepare(contract_input)).resolve.pipeline);
        check(plan.constants.extent_flags[2] == 1 && plan.constants.extent_flags[3] == 0,
              "matching history is eligible");
        auto old = epoch;
        old.revision = old.revision.next();
        check(take(d3d12::prepare_authoring_temporal(epoch, old, true, false))
                      .constants.extent_flags[2] == 0,
              "revision rejects GPU history");
        old = epoch;
        old.source.bytes[0] ^= 1;
        check(take(d3d12::prepare_authoring_temporal(epoch, old, true, false))
                      .constants.extent_flags[2] == 0,
              "source digest rejects equal-revision GPU history");
        old = epoch;
        old.width = 8;
        check(take(d3d12::prepare_authoring_temporal(epoch, old, true, false))
                      .constants.extent_flags[2] == 0,
              "resize rejects GPU history");
        check(take(d3d12::prepare_authoring_temporal(epoch, epoch, true, true))
                      .constants.extent_flags[3] == 1,
              "camera cut reaches shader contract");
        auto invalid = epoch;
        invalid.width = 4097;
        check(!d3d12::prepare_authoring_temporal(invalid, epoch, false, false),
              "four-wide identity atlas cannot exceed D3D12 dimension limit");
        invalid = epoch;
        invalid.width = 4096;
        invalid.height = 4096;
        check(!d3d12::prepare_authoring_temporal(invalid, epoch, false, false),
              "temporal pixel budget is bounded");
        const std::vector<render::TemporalSurface> identity{
            {0xfedcba9876543210ULL, 0x0123456789abcdefULL}, {1, 2}};
        const auto packed = take(d3d12::pack_temporal_identity_atlas(identity, 2, 1));
        check(packed == std::vector<std::uint32_t>{0x76543210, 0xfedcba98, 0x89abcdef, 0x01234567,
                                                   1, 0, 2, 0},
              "object/face64 identity remains exact unsigned32 words rather than float24 aliases");
        const std::vector<render::TemporalPreviousSample> motion{
            {.25, .75, 2, true}, {1e300, 0, 2, true}, {0, 0, 0, false}};
        const auto samples = take(d3d12::pack_temporal_previous_samples(motion, 3, 1));
        check(samples[0] == std::array<float, 4>{.25, .75, 2, 1} && samples[1][3] == 0 &&
                  samples[2][3] == 0,
              "subpixel motion preserves unsupported/overflow rejection");
        const auto shader = take(d3d12::compile_hlsl(d3d12::authoring_temporal_shader()));
        check(!shader.bytes.empty() && shader.stage == gpu::ShaderStage::compute,
              "actual temporal HLSL compiles to DXIL without GPU");
        std::cout << "CPU temporal GPU preparation: exact64-bit identity, "
                     "source/revision/cut/extent rejection, subpixel motion packing and "
                     "actual7-SRV HLSL compilation passed; native/GPU execution pending\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
