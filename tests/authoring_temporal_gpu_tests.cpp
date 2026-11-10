// GPU-only pending parity runner. Building it does not instantiate a GPU.
#include <algorithm>
#include <carto/d3d12/authoring_temporal.hpp>
#include <carto/device_ir/command.hpp>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace carto;
void check(bool v, const char *s) {
    if (!v)
        throw std::runtime_error(s);
}
template <class T> T take(core::Result<T> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
    return std::move(r.value());
}
void okay(core::Result<void> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
}
template <class T> std::span<const std::uint8_t> bytes(const std::vector<T> &v) {
    return {reinterpret_cast<const std::uint8_t *>(v.data()), v.size() * sizeof(T)};
}
struct Fixture {
    unsigned w = 9, h = 5;
    std::vector<core::Vec3d> current, history;
    std::vector<double> depth, old_depth;
    std::vector<render::TemporalSurface> ids, old_ids;
    std::vector<render::TemporalPreviousSample> motion;
    bool valid = true, cut = false, changed = false;
    Fixture() {
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                current.push_back({10. + x + y, 2. + x * .1, .5 + y * .1});
                history.push_back(current.back() + core::Vec3d{.2, .1, .05});
                depth.push_back(7);
                old_depth.push_back(2);
                ids.push_back({0xfedcba9876543210ULL, 0x0123456789abcdefULL});
                motion.push_back({x + .25, y + .25, 2, true});
            }
        old_ids = ids;
    }
    render::TemporalReprojectionInput input() const {
        render::TemporalReprojectionInput i;
        i.width = w;
        i.height = h;
        i.current_hdr = current;
        i.history_hdr = history;
        i.current_depth = depth;
        i.history_depth = old_depth;
        i.current_surface = ids;
        i.history_surface = old_ids;
        i.previous = motion;
        i.history_valid = valid;
        i.camera_cut = cut;
        i.content_changed = changed;
        i.current_revision = i.history_revision = core::Revision{7};
        return i;
    }
};
void compare(d3d12::D3D12Device &device, gpu::PipelineHandle pipeline, const Fixture &f,
             gpu::QueueType queue = gpu::QueueType::compute) {
    const auto input = f.input();
    const auto reference = take(render::resolve_temporal_reprojected_reference(input));
    d3d12::AuthoringTemporalEpoch epoch{assets::sha256(std::array<std::uint8_t, 1>{1}),
                                        core::Revision{7}, f.w, f.h};
    auto old = epoch;
    if (f.changed)
        old.source.bytes[0] ^= 1;
    const auto plan =
        take(d3d12::prepare_authoring_temporal(epoch, old, f.valid, f.cut, input.settings));
    device_ir::DeviceCommandStream commands;
    std::vector<gpu::TextureHandle> textures;
    std::vector<gpu::BufferHandle> buffers;
    const auto upload = [&](unsigned w, unsigned h, gpu::Format format,
                            std::span<const std::uint8_t> data, unsigned pixel_bytes) {
        const auto texture = take(device.create_texture(
            {w, h, 1, 1, 1, format, gpu::TextureDimension::texture_2d,
             gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination}));
        textures.push_back(texture);
        const unsigned row = w * pixel_bytes, pitch = (row + 255) & ~255U;
        std::vector<std::uint8_t> padded(std::size_t(pitch) * h);
        check(data.size() == std::size_t(row) * h, "GPU test upload extent");
        for (unsigned y = 0; y < h; ++y)
            std::copy_n(data.data() + std::size_t(row) * y, row,
                        padded.data() + std::size_t(pitch) * y);
        const auto staging = take(device.create_buffer(
            {padded.size(), gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source}));
        buffers.push_back(staging);
        okay(device.write_buffer(staging, 0, padded));
        commands.append(device_ir::CmdTransitionResource{
            {staging}, device_ir::ResourceState::undefined, device_ir::ResourceState::copy_source});
        commands.append(
            device_ir::CmdTransitionResource{{texture},
                                             device_ir::ResourceState::undefined,
                                             device_ir::ResourceState::copy_destination});
        commands.append(device_ir::CmdCopyBufferToTexture{staging, texture, 0});
        commands.append(device_ir::CmdTransitionResource{{texture},
                                                         device_ir::ResourceState::copy_destination,
                                                         device_ir::ResourceState::shader_read});
        return texture;
    };
    std::vector<std::array<float, 4>> current, history;
    std::vector<float> depth, old_depth;
    for (std::size_t p = 0; p < f.current.size(); ++p) {
        const auto a = f.current[p], b = f.history[p];
        current.push_back({float(a.x), float(a.y), float(a.z), f.ids[p][0] ? float(p + 1) : 0});
        history.push_back({float(b.x), float(b.y), float(b.z), 999});
        depth.push_back(float(f.depth[p]));
        old_depth.push_back(float(f.old_depth[p]));
    }
    const auto motion = take(d3d12::pack_temporal_previous_samples(f.motion, f.w, f.h));
    const auto identities = take(d3d12::pack_temporal_identity_atlas(f.ids, f.w, f.h));
    const auto prior_ids = take(d3d12::pack_temporal_identity_atlas(f.old_ids, f.w, f.h));
    const std::array<gpu::TextureHandle, 7> sampled{
        upload(f.w, f.h, gpu::Format::rgba32_float, bytes(current), 16),
        upload(f.w, f.h, gpu::Format::rgba32_float, bytes(history), 16),
        upload(f.w, f.h, gpu::Format::rgba32_float, bytes(motion), 16),
        upload(f.w, f.h, gpu::Format::r32_float, bytes(depth), 4),
        upload(f.w, f.h, gpu::Format::r32_float, bytes(old_depth), 4),
        upload(f.w * 4, f.h, gpu::Format::r32_uint, bytes(identities), 4),
        upload(f.w * 4, f.h, gpu::Format::r32_uint, bytes(prior_ids), 4)};
    const auto output = take(device.create_texture(
        {f.w, f.h, 1, 1, 1, gpu::Format::rgba32_float, gpu::TextureDimension::texture_2d,
         gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source}));
    textures.push_back(output);
    commands.append(device_ir::CmdTransitionResource{
        {output}, device_ir::ResourceState::undefined, device_ir::ResourceState::shader_write});
    commands.append(device_ir::CmdBindPipeline{pipeline});
    device_ir::CmdBindResources bindings;
    for (unsigned i = 0; i < 7; ++i)
        bindings.bindings.push_back({0, i, {sampled[i]}, std::nullopt});
    bindings.bindings.push_back({0, 7, {output}, std::nullopt});
    commands.append(std::move(bindings));
    std::vector<std::uint8_t> constants(sizeof(plan.constants));
    std::memcpy(constants.data(), &plan.constants, sizeof(plan.constants));
    commands.append(device_ir::CmdPushConstants{std::move(constants)});
    commands.append(device_ir::CmdDispatch{plan.groups[0], plan.groups[1], 1});
    commands.append(device_ir::CmdUavBarrier{{output}});
    okay(device.wait(take(device.submit(commands, queue))));
    const auto actual = take(device.read_texture_rgba32f(output));
    for (std::size_t p = 0; p < f.current.size(); ++p) {
        const auto expected = reference.linear_hdr[p];
        for (auto [c, e] :
             {std::pair{0U, expected.x}, std::pair{1U, expected.y}, std::pair{2U, expected.z}})
            check(std::isfinite(actual[p * 4 + c]) &&
                      std::abs(actual[p * 4 + c] - e) < 3e-5 + std::abs(e) * 1e-5,
                  "temporal GPU/CPU full image HDR parity");
        check(actual[p * 4 + 3] == current[p][3],
              "temporal pass preserves current picking alpha exactly");
    }
    for (const auto buffer : buffers)
        okay(device.destroy_buffer(buffer));
    for (const auto texture : textures)
        okay(device.destroy_texture(texture));
}
} // namespace
int main() {
    try {
        auto device = take(d3d12::D3D12Device::create({{}, false, true}));
        const auto shader = take(
            device->create_shader(take(d3d12::compile_hlsl(d3d12::authoring_temporal_shader()))));
        Fixture base;
        d3d12::AuthoringTemporalEpoch epoch{assets::sha256(std::array<std::uint8_t, 1>{1}),
                                            core::Revision{7}, base.w, base.h};
        auto desc = take(d3d12::prepare_authoring_temporal(epoch, epoch, true, false)).pipeline;
        desc.shaders = {shader};
        const auto pipeline = take(device->create_pipeline(desc));
        auto invalid = desc;
        invalid.sampled_texture_count = 8;
        invalid.descriptor_bindings.push_back({0, 8, gpu::ShaderStage::compute});
        check(!device->create_pipeline(invalid), "compute table extension rejects eighth SRV");
        invalid = desc;
        invalid.push_constant_bytes = 228;
        check(!device->create_pipeline(invalid), "compute table extension preserves64-DWORD limit");
        device_ir::DeviceCommandStream copy_dispatch;
        copy_dispatch.append(device_ir::CmdBindPipeline{pipeline});
        copy_dispatch.append(device_ir::CmdDispatch{1,1,1});
        const auto forbidden=device->submit(copy_dispatch,gpu::QueueType::copy);
        check(!forbidden && forbidden.error().message.find("graphics or compute queue")!=std::string::npos,
              "copy-only queue must still reject compute commands");
        compare(*device,pipeline,base,gpu::QueueType::graphics);
        compare(*device, pipeline, base);
        auto changed = base;
        changed.old_ids[14][0] ^= std::uint64_t{1} << 50;
        compare(*device, pipeline, changed);
        changed = base;
        changed.old_depth[14] = 8;
        compare(*device, pipeline, changed);
        changed = base;
        changed.motion[13].x = -.25;
        changed.motion[14].supported = false;
        compare(*device, pipeline, changed);
        changed = base;
        changed.history[14] = {1000, 1000, 1000};
        compare(*device, pipeline, changed);
        changed = base;
        changed.cut = true;
        compare(*device, pipeline, changed);
        changed = base;
        changed.changed = true;
        compare(*device, pipeline, changed);
        changed = base;
        changed.valid = false;
        compare(*device, pipeline, changed);
        changed = base;
        changed.ids[14] = {};
        compare(*device, pipeline, changed);
        okay(device->destroy_pipeline(pipeline));
        okay(device->destroy_shader(shader));
        for (const auto &m : device->debug_receipts())
            std::cerr << m << '\n';
        check(device->debug_receipts().empty(), "temporal kernel debug/lifetime receipts clean");
        std::cout << "GPU temporal offscreen kernel parity passed10 cases on compute/direct queues with exact64-bit "
                     "identities/subpixel/depth/clipping/cut/source rejection; native history "
                     "integration remains pending\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
