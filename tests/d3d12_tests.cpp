#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <carto/d3d12/device.hpp>
#include <carto/d3d12/swapchain.hpp>
#include <carto/d3d12/texture_upload.hpp>
#include <carto/production/render_mesh.hpp>

#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>

#include <dxgi.h>

#include <algorithm>
#include <cstdint>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

void d3d12_device_reports_native_identity_and_owns_resources() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    if (!created) {
        throw TestFailure("D3D12 create: " + created.error().message);
    }
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.backend() == device::BackendKind::d3d12);
    REQUIRE(!device.identity().adapter_name.empty());
    REQUIRE(!device.identity().stable_adapter_id.empty());
    REQUIRE(device.capabilities().graphics_queue);
    REQUIRE(device.capabilities().compute_queue);
    REQUIRE(device.capabilities().copy_queue);

    const auto selected = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        device.identity().stable_adapter_id,
        false,
        false,
    });
    if (!selected) {
        throw TestFailure("D3D12 explicit adapter selection: " + selected.error().message);
    }
    REQUIRE(selected);
    REQUIRE(selected.value()->identity().stable_adapter_id ==
            device.identity().stable_adapter_id);

    const auto buffer = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::device_local,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(buffer);
    const auto upload = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    REQUIRE(upload);
    const auto readback = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(readback);
    const std::vector<std::uint8_t> bytes = {1U, 2U, 3U, 5U};
    REQUIRE(device.write_buffer(upload.value(), 0U, bytes));

    device_ir::DeviceCommandStream copy_stream;
    copy_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    copy_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    copy_stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, static_cast<std::uint64_t>(bytes.size())});
    const auto copy_serial = device.submit(copy_stream, gpu::QueueType::copy);
    if (!copy_serial) {
        throw TestFailure("D3D12 copy submit: " + copy_serial.error().message);
    }
    REQUIRE(copy_serial);
    REQUIRE(device.wait(copy_serial.value()));
    const auto copied = device.read_buffer(readback.value(), 0U, bytes.size());
    REQUIRE(copied && copied.value() == bytes);

    device_ir::DeviceCommandStream rejected_stream;
    rejected_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::copy_source,
        device_ir::ResourceState::vertex_read,
    });
    rejected_stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, static_cast<std::uint64_t>(bytes.size())});
    const auto rejected = device.submit(rejected_stream, gpu::QueueType::copy);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::validation_failed);

    device_ir::DeviceCommandStream replay_stream;
    replay_stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, static_cast<std::uint64_t>(bytes.size())});
    const auto replay_serial = device.submit(replay_stream, gpu::QueueType::copy);
    REQUIRE(replay_serial);
    REQUIRE(device.wait(replay_serial.value()));
    const auto replayed = device.read_buffer(readback.value(), 0U, bytes.size());
    REQUIRE(replayed && replayed.value() == bytes);

    const auto texture = device.create_texture(gpu::TextureDesc{
        4U,
        4U,
        1U,
        1U,
        1U,
        gpu::Format::rgba8_unorm,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment,
        {0.1F, 0.2F, 0.3F, 1.0F},
        1.0F,
    });
    REQUIRE(texture);

    device_ir::DeviceCommandStream clear_stream;
    clear_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    clear_stream.append(device_ir::CmdBeginRendering{texture.value(), std::nullopt});
    clear_stream.append(device_ir::CmdClearColor{
        texture.value(), {0.1F, 0.2F, 0.3F, 1.0F}});
    clear_stream.append(device_ir::CmdEndRendering{});
    const auto clear_serial = device.submit(clear_stream, gpu::QueueType::graphics);
    REQUIRE(clear_serial);
    REQUIRE(device.wait(clear_serial.value()));

    REQUIRE(device.destroy_texture(texture.value()));
    REQUIRE(device.destroy_buffer(upload.value()));
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.destroy_buffer(buffer.value()));
    REQUIRE(!device.destroy_buffer(buffer.value()));
}

void d3d12_lifetimes_recycle_descriptors_and_defer_resource_destruction() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();

    gpu::SamplerDesc sampler_descriptor{};
    sampler_descriptor.anisotropy = true;
    sampler_descriptor.max_anisotropy = 17.0F;
    const auto rejected_sampler = device.create_sampler(sampler_descriptor);
    REQUIRE(!rejected_sampler);
    REQUIRE(rejected_sampler.error().code == core::ErrorCode::invalid_argument);

    sampler_descriptor.anisotropy = false;
    sampler_descriptor.max_anisotropy = 1.0F;
    for (std::size_t iteration = 0U; iteration < 2050U; ++iteration) {
        const auto sampler = device.create_sampler(sampler_descriptor);
        REQUIRE(sampler);
        REQUIRE(device.destroy_sampler(sampler.value()));
    }

    const auto upload = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    const auto readback = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(upload && readback);
    const std::vector<std::uint8_t> bytes = {9U, 8U, 7U};
    REQUIRE(device.write_buffer(upload.value(), 0U, bytes));

    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, 3U});
    const auto serial = device.submit(stream, gpu::QueueType::copy);
    REQUIRE(serial);
    REQUIRE(device.destroy_buffer(upload.value()));
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.wait(serial.value()));
    REQUIRE(!device.read_buffer(readback.value(), 0U, 3U));

    const auto live_at_device_teardown = device.create_sampler(sampler_descriptor);
    REQUIRE(live_at_device_teardown);
}

void d3d12_device_loss_is_fail_closed_and_recoverable() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, false});
    REQUIRE(created);
    auto& device = *created.value();
    device.record_external_device_loss(
        "test device-loss injection", static_cast<std::uint32_t>(DXGI_ERROR_DEVICE_REMOVED));
    REQUIRE(device.device_lost());

    const auto rejected_buffer = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    REQUIRE(!rejected_buffer);
    REQUIRE(rejected_buffer.error().code == core::ErrorCode::invalid_state);

    device_ir::DeviceCommandStream stream;
    const auto rejected_submit = device.submit(stream, gpu::QueueType::graphics);
    REQUIRE(!rejected_submit);
    REQUIRE(rejected_submit.error().code == core::ErrorCode::invalid_state);

    const auto recovered = device.recover();
    REQUIRE(recovered);
    REQUIRE(!recovered.value()->device_lost());
    const auto buffer = recovered.value()->create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    REQUIRE(buffer);
    REQUIRE(recovered.value()->destroy_buffer(buffer.value()));
}

void d3d12_captures_native_timestamp_queries() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().timestamp_queries);
    REQUIRE(device.timestamp_frequency(gpu::QueueType::graphics) != 0U);
    REQUIRE(device.timestamp_frequency(gpu::QueueType::compute) != 0U);

    const auto readback = device.create_buffer(gpu::BufferDesc{
        2U * sizeof(std::uint64_t),
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(readback);

    const auto capture = [&device, readback](gpu::QueueType queue, bool initialize) {
        device_ir::DeviceCommandStream stream;
        if (initialize) {
            stream.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{readback.value()},
                device_ir::ResourceState::undefined,
                device_ir::ResourceState::copy_destination,
            });
        }
        stream.append(device_ir::CmdWriteTimestamp{0U});
        stream.append(device_ir::CmdWriteTimestamp{1U});
        stream.append(device_ir::CmdResolveTimestamps{
            readback.value(), 0U, 2U, 0U});
        const auto serial = device.submit(stream, queue);
        REQUIRE(serial);
        REQUIRE(device.wait(serial.value()));
        const auto bytes = device.read_buffer(
            readback.value(), 0U, 2U * sizeof(std::uint64_t));
        REQUIRE(bytes);
        std::uint64_t timestamps[2U]{};
        std::memcpy(timestamps, bytes.value().data(), sizeof(timestamps));
        REQUIRE(timestamps[1U] >= timestamps[0U]);
    };
    capture(gpu::QueueType::graphics, true);
    capture(gpu::QueueType::compute, false);
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.debug_receipts().empty());
}

void d3d12_uses_compute_queue_shader_states() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    const auto sampled = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled,
    });
    const auto storage = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage,
    });
    REQUIRE(sampled && storage);
    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{sampled.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_read,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{storage.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    const auto serial = device.submit(stream, gpu::QueueType::compute);
    REQUIRE(serial);
    REQUIRE(device.wait(serial.value()));
    REQUIRE(device.debug_receipts().empty());
    REQUIRE(device.destroy_texture(storage.value()));
    REQUIRE(device.destroy_texture(sampled.value()));
}

void d3d12_reuses_multiple_in_flight_command_contexts() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    device_ir::DeviceCommandStream stream;
    std::array<gpu::SubmissionSerial, 8U> serials{};
    for (auto& serial : serials) {
        const auto submitted = device.submit(stream, gpu::QueueType::graphics);
        REQUIRE(submitted);
        serial = submitted.value();
    }
    for (const auto serial : serials) REQUIRE(device.wait(serial));
    REQUIRE(device.debug_receipts().empty());
}

template <typename T>
std::vector<std::uint8_t> bytes_of(const T& value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    return std::vector<std::uint8_t>(begin, begin + sizeof(T));
}

template <typename T>
std::span<const std::uint8_t> bytes_of_vector(const std::vector<T>& values) {
    return {
        reinterpret_cast<const std::uint8_t*>(values.data()),
        values.size() * sizeof(T),
    };
}

struct D3d12PbrFixture {
    carto::production::RenderMeshArtifact artifact;
    carto::production::PbrMeshUpload upload;
};

D3d12PbrFixture authored_pbr_fixture() {
    using namespace carto;

    geometry::EditableMesh mesh;
    const auto v0 = mesh.add_vertex({-0.75, -0.75, 0.0});
    const auto v1 = mesh.add_vertex({0.75, -0.75, 0.0});
    const auto v2 = mesh.add_vertex({0.0, 0.75, 0.0});
    REQUIRE(v0 && v1 && v2);
    REQUIRE(mesh.add_face({v0.value(), v1.value(), v2.value()}));

    const auto topology = mesh.topology();
    REQUIRE(topology && topology.value().corners.size() == 3U);
    const std::map<geometry::VertexId, core::Vec2d> coordinates{
        {v0.value(), {0.0, 1.0}},
        {v1.value(), {1.0, 1.0}},
        {v2.value(), {0.5, 0.0}},
    };
    std::vector<attributes::AttributeValue> uv_values;
    uv_values.reserve(topology.value().corners.size());
    for (const auto& corner : topology.value().corners) {
        uv_values.emplace_back(coordinates.at(corner.vertex));
    }
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, topology.value().corners.size()},
    });
    REQUIRE(attributes.add_layer({
        {"uv.native-pbr", attributes::AttributeDomain::corner,
         attributes::AttributeType::vec2},
        std::move(uv_values),
    }));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({1U, "Native PBR UV0", "uv.native-pbr", false, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));

    constexpr std::string_view source_text = "cartographer-d3d12-authored-pbr-fixture-v1";
    const auto source_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(source_text.data()), source_text.size()});
    const production::AuthoritativeSource source{
        "fixture/native-pbr", core::Revision{1U}, source_digest};
    const auto artifact = production::compile_render_mesh_artifact(
        source,
        mesh,
        production::RenderMeshCompileOptions{
            .uv_set = 1U,
            .uv1_set = std::nullopt,
            .generate_tangents = true,
            .material_slot_layer = std::nullopt,
            .vertex_color_layer = std::nullopt,
        });
    if (!artifact) {
        throw TestFailure("compile authored PBR fixture: " + artifact.error().message);
    }
    const auto upload = production::pack_pbr_mesh_upload(artifact.value());
    if (!upload) {
        throw TestFailure("pack authored PBR fixture: " + upload.error().message);
    }
    return {artifact.value(), upload.value()};
}

void d3d12_compiles_dxil_draws_indexed_and_reads_texture() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().anisotropy);
    REQUIRE(device.capabilities().sampler_compare);
    REQUIRE(device.capabilities().max_texture_dimension_2d >= 16384U);
    REQUIRE(device.capabilities().max_sampler_anisotropy >= 16.0F);

    const auto oversized_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_oversized_source",
        gpu::ShaderStage::vertex,
        "main",
        "d3d12_oversized_source.hlsl",
        std::string(16U * 1024U * 1024U + 1U, ' '),
        true,
        false,
    });
    REQUIRE(!oversized_source);
    REQUIRE(oversized_source.error().code == core::ErrorCode::invalid_argument);

    const auto vertex_shader = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_test_vertex",
        gpu::ShaderStage::vertex,
        "main",
        "d3d12_test_vertex.hlsl",
        R"(
            struct Input { float4 position : POSITION; };
            struct Output { float4 position : SV_POSITION; };
            Output main(Input input) {
                Output output;
                output.position = input.position;
                return output;
            }
        )",
        true,
        false,
    });
    if (!vertex_shader) {
        throw TestFailure("DXC vertex compile: " + vertex_shader.error().message);
    }
    const auto fragment_shader = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_test_fragment",
        gpu::ShaderStage::fragment,
        "main",
        "d3d12_test_fragment.hlsl",
        R"(
            cbuffer PushConstants : register(b0) { float4 color; };
            float4 main() : SV_TARGET { return color; }
        )",
        true,
        false,
    });
    if (!fragment_shader) {
        throw TestFailure("DXC fragment compile: " + fragment_shader.error().message);
    }
    REQUIRE(vertex_shader.value().format == device::BinaryFormat::dxil);
    REQUIRE(fragment_shader.value().format == device::BinaryFormat::dxil);
    REQUIRE(vertex_shader.value().bytes.size() > 32U);
    REQUIRE(fragment_shader.value().bytes.size() > 32U);

    const auto vertex = device.create_shader(vertex_shader.value());
    const auto fragment = device.create_shader(fragment_shader.value());
    REQUIRE(vertex && fragment);

    gpu::PipelineDesc pipeline_descriptor;
    pipeline_descriptor.debug_name = "d3d12_indexed_triangle";
    pipeline_descriptor.shaders = {vertex.value(), fragment.value()};
    pipeline_descriptor.cull_mode = gpu::CullMode::none;
    pipeline_descriptor.color_format = gpu::Format::rgba32_float;
    pipeline_descriptor.depth_format = gpu::Format::depth32_float;
    pipeline_descriptor.push_constant_bytes = 16U;
    const auto pipeline = device.create_pipeline(pipeline_descriptor);
    if (!pipeline) {
        std::string message = "D3D12 PSO: " + pipeline.error().message;
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        const auto append_prefix = [&message](const char* label, const auto& bytes) {
            message += " | " + std::string(label) + "=";
            const std::size_t count = std::min<std::size_t>(16U, bytes.size());
            for (std::size_t index = 0U; index < count; ++index) {
                constexpr char digits[] = "0123456789abcdef";
                const auto byte = bytes[index];
                message += digits[(byte >> 4U) & 0x0fU];
                message += digits[byte & 0x0fU];
            }
        };
        append_prefix("vs", vertex_shader.value().bytes);
        append_prefix("ps", fragment_shader.value().bytes);
        throw TestFailure(message);
    }

    const std::array<std::array<float, 4U>, 3U> vertices = {{
        {{-0.8F, -0.8F, 0.25F, 1.0F}},
        {{0.8F, -0.8F, 0.25F, 1.0F}},
        {{0.0F, 0.8F, 0.25F, 1.0F}},
    }};
    const std::array<std::uint32_t, 3U> indices = {0U, 1U, 2U};
    const auto vertex_buffer = device.create_buffer(gpu::BufferDesc{
        sizeof(vertices), gpu::MemoryClass::upload,
        gpu::BufferUsage::vertex | gpu::BufferUsage::transfer_source});
    const auto index_buffer = device.create_buffer(gpu::BufferDesc{
        sizeof(indices), gpu::MemoryClass::upload,
        gpu::BufferUsage::index | gpu::BufferUsage::transfer_source});
    REQUIRE(vertex_buffer && index_buffer);
    REQUIRE(device.write_buffer(vertex_buffer.value(), 0U, bytes_of(vertices)));
    REQUIRE(device.write_buffer(index_buffer.value(), 0U, bytes_of(indices)));

    const auto color = device.create_texture(gpu::TextureDesc{
        32U, 32U, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source});
    const auto depth = device.create_texture(gpu::TextureDesc{
        32U, 32U, 1U, 1U, 1U, gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment});
    REQUIRE(color && depth);

    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{color.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{depth.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{index_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::index_read});
    stream.append(device_ir::CmdBeginRendering{color.value(), depth.value()});
    stream.append(device_ir::CmdClearColor{color.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    stream.append(device_ir::CmdClearDepth{depth.value(), 1.0F});
    stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    stream.append(device_ir::CmdBindPipeline{pipeline.value()});
    stream.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 16U});
    stream.append(device_ir::CmdBindIndexBuffer{
        index_buffer.value(), 0U, device_ir::IndexFormat::uint32});
    const std::array<float, 4U> draw_color = {1.0F, 0.25F, 0.0F, 1.0F};
    stream.append(device_ir::CmdPushConstants{bytes_of(draw_color)});
    stream.append(device_ir::CmdDrawIndexed{3U, 1U, 0U, 0, 0U});
    stream.append(device_ir::CmdEndRendering{});
    const auto serial = device.submit(stream, gpu::QueueType::graphics);
    if (!serial) throw TestFailure("D3D12 indexed draw: " + serial.error().message);
    REQUIRE(device.wait(serial.value()));

    const auto pixels = device.read_texture_rgba32f(color.value());
    if (!pixels) throw TestFailure("D3D12 texture readback: " + pixels.error().message);
    REQUIRE(pixels.value().size() == 32U * 32U * 4U);
    const std::size_t center = (16U * 32U + 16U) * 4U;
    REQUIRE(pixels.value()[center] > 0.8F);
    REQUIRE(pixels.value()[center + 1U] > 0.1F);
    REQUIRE(pixels.value()[center + 3U] > 0.8F);
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
    REQUIRE(device.debug_receipts().empty());

    REQUIRE(device.destroy_texture(depth.value()));
    REQUIRE(device.destroy_texture(color.value()));
    REQUIRE(device.destroy_buffer(index_buffer.value()));
    REQUIRE(device.destroy_buffer(vertex_buffer.value()));
    REQUIRE(device.destroy_pipeline(pipeline.value()));
    REQUIRE(device.destroy_shader(fragment.value()));
    REQUIRE(device.destroy_shader(vertex.value()));
}

std::string read_d3d12_shader(const char* name) {
#ifdef CARTO_D3D12_SHADER_DIR
    const std::filesystem::path path = std::filesystem::path(CARTO_D3D12_SHADER_DIR) / name;
#else
    const std::filesystem::path path = std::filesystem::path("libs/d3d12/shaders") / name;
#endif
    std::ifstream input(path, std::ios::binary);
    if (!input) throw TestFailure("unable to read shader source: " + path.string());
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void d3d12_executes_builtin_shader_suite_and_dispatches_compute() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().timestamp_queries);
    const std::uint64_t graphics_timestamp_frequency =
        device.timestamp_frequency(gpu::QueueType::graphics);
    REQUIRE(graphics_timestamp_frequency != 0U);

    const auto compile_and_create = [&device](
        const char* name,
        gpu::ShaderStage stage,
        const char* entry_point) -> gpu::ShaderHandle {
        const auto source = d3d12::compile_hlsl(d3d12::ShaderSource{
            name,
            stage,
            entry_point,
            name,
            read_d3d12_shader(name),
            true,
            false,
        });
        if (!source) {
            throw TestFailure(std::string("DXC built-in shader ") + name + ": " +
                              source.error().message);
        }
        const auto handle = device.create_shader(source.value());
        if (!handle) {
            throw TestFailure(std::string("D3D12 built-in shader ") + name + ": " +
                              handle.error().message);
        }
        return handle.value();
    };

    const auto pbr_vertex = compile_and_create(
        "cartographer_pbr.hlsl", gpu::ShaderStage::vertex, "VSMain");
    const auto pbr_fragment = compile_and_create(
        "cartographer_pbr.hlsl", gpu::ShaderStage::fragment, "PSMain");
    const auto shadow_vertex = compile_and_create(
        "cartographer_shadow.hlsl", gpu::ShaderStage::vertex, "VSMain");
    const auto shadow_fragment = compile_and_create(
        "cartographer_shadow.hlsl", gpu::ShaderStage::fragment, "PSMain");
    const auto temporal_compute = compile_and_create(
        "cartographer_temporal.hlsl", gpu::ShaderStage::compute, "CSMain");
    const auto tone_vertex = compile_and_create(
        "cartographer_tonemap.hlsl", gpu::ShaderStage::vertex, "VSMain");
    const auto tone_fragment = compile_and_create(
        "cartographer_tonemap.hlsl", gpu::ShaderStage::fragment, "PSMain");

    gpu::PipelineDesc pbr_descriptor;
    pbr_descriptor.debug_name = "cartographer_builtin_pbr";
    pbr_descriptor.shaders = {pbr_vertex, pbr_fragment};
    pbr_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 4U, gpu::ShaderStage::fragment}};
    pbr_descriptor.sampled_texture_count = 5U;
    pbr_descriptor.cull_mode = gpu::CullMode::none;
    pbr_descriptor.color_format = gpu::Format::rgba32_float;
    pbr_descriptor.push_constant_bytes = 232U;
    pbr_descriptor.vertex_stride_bytes = 52U;
    pbr_descriptor.vertex_attributes = {
        gpu::VertexAttribute{"POSITION", 0U, gpu::VertexFormat::float4, 0U, 0U},
        gpu::VertexAttribute{"NORMAL", 0U, gpu::VertexFormat::float3, 16U, 0U},
        gpu::VertexAttribute{"TEXCOORD", 0U, gpu::VertexFormat::float2, 28U, 0U},
        gpu::VertexAttribute{"TANGENT", 0U, gpu::VertexFormat::float4, 36U, 0U},
    };
    auto oversized_pbr_descriptor = pbr_descriptor;
    oversized_pbr_descriptor.debug_name = "cartographer_oversized_pbr";
    oversized_pbr_descriptor.push_constant_bytes = 236U;
    const auto oversized_pbr_pipeline = device.create_pipeline(oversized_pbr_descriptor);
    REQUIRE(!oversized_pbr_pipeline);
    REQUIRE(oversized_pbr_pipeline.error().message.find("64 DWORDs") !=
            std::string::npos);
    const auto pbr_pipeline = device.create_pipeline(pbr_descriptor);
    if (!pbr_pipeline) {
        throw TestFailure("D3D12 built-in PBR pipeline: " + pbr_pipeline.error().message);
    }

    gpu::PipelineDesc shadow_descriptor;
    shadow_descriptor.debug_name = "cartographer_builtin_shadow";
    shadow_descriptor.shaders = {shadow_vertex, shadow_fragment};
    shadow_descriptor.cull_mode = gpu::CullMode::none;
    shadow_descriptor.color_format = gpu::Format::rgba32_float;
    shadow_descriptor.depth_format = gpu::Format::depth32_float;
    shadow_descriptor.depth_test = true;
    shadow_descriptor.depth_write = true;
    shadow_descriptor.push_constant_bytes = 128U;
    shadow_descriptor.vertex_stride_bytes = production::kPbrUploadVertexStrideBytes;
    shadow_descriptor.vertex_attributes = {
        gpu::VertexAttribute{"POSITION", 0U, gpu::VertexFormat::float4, 0U, 0U},
    };
    const auto shadow_pipeline = device.create_pipeline(shadow_descriptor);
    if (!shadow_pipeline) {
        throw TestFailure("D3D12 built-in shadow pipeline: " +
                          shadow_pipeline.error().message);
    }

    gpu::PipelineDesc tone_descriptor;
    tone_descriptor.debug_name = "cartographer_builtin_tonemap";
    tone_descriptor.shaders = {tone_vertex, tone_fragment};
    tone_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::fragment}};
    tone_descriptor.cull_mode = gpu::CullMode::none;
    tone_descriptor.color_format = gpu::Format::rgba32_float;
    tone_descriptor.depth_test = false;
    tone_descriptor.push_constant_bytes = 16U;
    const auto tone_pipeline = device.create_pipeline(tone_descriptor);
    if (!tone_pipeline) {
        throw TestFailure("D3D12 built-in tone-map pipeline: " + tone_pipeline.error().message);
    }

    const auto compute_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_storage_compute",
        gpu::ShaderStage::compute,
        "main",
        "d3d12_storage_compute.hlsl",
        R"(
            RWTexture2D<float4> output : register(u0);
            [numthreads(8, 8, 1)]
            void main(uint3 dispatch_id : SV_DISPATCHTHREADID) {
                if (dispatch_id.x < 4 && dispatch_id.y < 4) {
                    output[dispatch_id.xy] = float4(0.25, 0.5, 0.75, 1.0);
                }
            }
        )",
        true,
        false,
    });
    REQUIRE(compute_source);
    const auto compute_shader = device.create_shader(compute_source.value());
    REQUIRE(compute_shader);
    gpu::PipelineDesc compute_descriptor;
    compute_descriptor.debug_name = "d3d12_storage_compute";
    compute_descriptor.shaders = {compute_shader.value()};
    compute_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute}};
    compute_descriptor.depth_test = false;
    const auto compute_pipeline = device.create_pipeline(compute_descriptor);
    if (!compute_pipeline) {
        throw TestFailure("D3D12 compute pipeline: " + compute_pipeline.error().message);
    }
    gpu::PipelineDesc temporal_descriptor;
    temporal_descriptor.debug_name = "cartographer_builtin_temporal";
    temporal_descriptor.shaders = {temporal_compute};
    temporal_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::compute},
    };
    temporal_descriptor.sampled_texture_count = 3U;
    temporal_descriptor.depth_test = false;
    temporal_descriptor.push_constant_bytes = 48U;
    const auto temporal_pipeline = device.create_pipeline(temporal_descriptor);
    if (!temporal_pipeline) {
        throw TestFailure("D3D12 built-in temporal pipeline: " +
                          temporal_pipeline.error().message);
    }

    const auto output = device.create_texture(gpu::TextureDesc{
        4U,
        4U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    REQUIRE(output);
    device_ir::DeviceCommandStream compute_stream;
    compute_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{output.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    compute_stream.append(device_ir::CmdBindPipeline{compute_pipeline.value()});
    compute_stream.append(device_ir::CmdBindResources{
        {device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{output.value()}, std::nullopt}}});
    compute_stream.append(device_ir::CmdDispatch{1U, 1U, 1U});
    compute_stream.append(device_ir::CmdUavBarrier{device_ir::ResourceHandle{output.value()}});
    const auto serial = device.submit(compute_stream, gpu::QueueType::compute);
    if (!serial) throw TestFailure("D3D12 compute submit: " + serial.error().message);
    REQUIRE(device.wait(serial.value()));
    const auto pixels = device.read_texture_rgba32f(output.value());
    if (!pixels) throw TestFailure("D3D12 compute readback: " + pixels.error().message);
    REQUIRE(pixels.value().size() == 4U * 4U * 4U);
    REQUIRE(pixels.value()[0U] > 0.24F && pixels.value()[0U] < 0.26F);
    REQUIRE(pixels.value()[1U] > 0.49F && pixels.value()[1U] < 0.51F);
    REQUIRE(pixels.value()[2U] > 0.74F && pixels.value()[2U] < 0.76F);
    REQUIRE(pixels.value()[3U] > 0.99F);

    gpu::SamplerDesc sampler_descriptor;
    sampler_descriptor.min_filter = gpu::Filter::linear;
    sampler_descriptor.mag_filter = gpu::Filter::linear;
    sampler_descriptor.mip_filter = gpu::MipFilter::nearest;
    const auto sampler = device.create_sampler(sampler_descriptor);
    REQUIRE(sampler);

    const std::array<std::array<std::uint8_t, 4U>, 5U> material_texels = {{
        {{255U, 128U, 64U, 255U}},   // base color, sRGB
        {{0U, 128U, 255U, 255U}},    // metallic/roughness: B metallic, G roughness
        {{255U, 0U, 255U, 255U}},    // tangent normal, DirectX Y-
        {{32U, 255U, 255U, 255U}},   // occlusion in R
        {{255U, 64U, 0U, 255U}},      // emissive, sRGB
    }};
    const std::array<gpu::Format, 5U> material_formats = {{
        gpu::Format::rgba8_srgb,
        gpu::Format::rgba8_unorm,
        gpu::Format::rgba8_unorm,
        gpu::Format::rgba8_unorm,
        gpu::Format::rgba8_srgb,
    }};
    std::array<gpu::BufferHandle, 5U> material_uploads{};
    std::array<gpu::TextureHandle, 5U> material_textures{};
    for (std::size_t index = 0U; index < material_texels.size(); ++index) {
        const auto upload = device.create_buffer(gpu::BufferDesc{
            256U,
            gpu::MemoryClass::upload,
            gpu::BufferUsage::transfer_source,
        });
        const auto texture = device.create_texture(gpu::TextureDesc{
            1U,
            1U,
            1U,
            1U,
            1U,
            material_formats.at(index),
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
        });
        REQUIRE(upload && texture);
        material_uploads.at(index) = upload.value();
        material_textures.at(index) = texture.value();
        REQUIRE(device.write_buffer(
            material_uploads.at(index), 0U, material_texels.at(index)));
    }
    device_ir::DeviceCommandStream upload_stream;
    for (std::size_t index = 0U; index < material_texels.size(); ++index) {
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{material_uploads.at(index)},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_source,
        });
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{material_textures.at(index)},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_destination,
        });
        upload_stream.append(device_ir::CmdCopyBufferToTexture{
            material_uploads.at(index), material_textures.at(index), 0U});
    }
    const auto upload_serial = device.submit(upload_stream, gpu::QueueType::copy);
    REQUIRE(upload_serial);
    REQUIRE(device.wait(upload_serial.value()));
    for (const auto upload : material_uploads) REQUIRE(device.destroy_buffer(upload));

    const D3d12PbrFixture pbr_fixture = authored_pbr_fixture();
    const auto& vertices = pbr_fixture.upload.vertices;
    const auto& indices = pbr_fixture.upload.indices;
    REQUIRE(vertices.size() == 3U && indices.size() == 3U);
    REQUIRE(pbr_fixture.upload.source_revision == pbr_fixture.artifact.source_revision);
    REQUIRE(pbr_fixture.upload.source_digest == pbr_fixture.artifact.source_digest);
    const auto vertex_buffer = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(vertices.size()) * sizeof(production::PbrUploadVertex),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::vertex | gpu::BufferUsage::transfer_source,
    });
    const auto index_buffer = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(indices.size()) * sizeof(std::uint32_t),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::index | gpu::BufferUsage::transfer_source,
    });
    const auto pbr_target = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment |
            gpu::TextureUsage::transfer_source,
    });
    const auto depth_target = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment,
    });
    const auto tone_target = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    });
    const auto pbr_timestamps = device.create_buffer(gpu::BufferDesc{
        2U * sizeof(std::uint64_t),
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(vertex_buffer && index_buffer && pbr_target && depth_target && tone_target &&
            pbr_timestamps);
    REQUIRE(device.write_buffer(vertex_buffer.value(), 0U, bytes_of_vector(vertices)));
    REQUIRE(device.write_buffer(index_buffer.value(), 0U, bytes_of_vector(indices)));

    std::array<float, 58U> pbr_constants{};
    for (std::size_t matrix = 0U; matrix < 2U; ++matrix) {
        for (std::size_t diagonal = 0U; diagonal < 4U; ++diagonal) {
            pbr_constants[matrix * 16U + diagonal * 5U] = 1.0F;
        }
    }
    pbr_constants[34U] = 2.0F;
    pbr_constants[35U] = 0.0F;
    pbr_constants[36U] = 1.0F;
    pbr_constants[37U] = 1.0F;
    pbr_constants[38U] = 1.0F;
    pbr_constants[39U] = 1.0F;
    pbr_constants[40U] = 0.2F;
    pbr_constants[41U] = 0.1F;
    pbr_constants[43U] = 0.08F;
    pbr_constants[44U] = 1.0F;
    pbr_constants[45U] = 1.0F;
    pbr_constants[46U] = 1.0F;
    pbr_constants[47U] = 1.0F;
    pbr_constants[48U] = 0.0F;
    pbr_constants[49U] = 0.0F;
    pbr_constants[50U] = 1.0F;
    pbr_constants[51U] = 1.0F;
    pbr_constants[52U] = 4.0F;
    pbr_constants[53U] = 4.0F;
    pbr_constants[54U] = 4.0F;
    pbr_constants[55U] = 1.0F;
    pbr_constants[56U] = 0.75F;
    pbr_constants[57U] = 0.2F;
    const std::array<float, 4U> tone_constants = {0.0F, 0.0F, 0.0F, 0.0F};

    device_ir::DeviceCommandStream render_stream;
    for (const auto texture : material_textures) {
        render_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture},
            device_ir::ResourceState::copy_destination,
            device_ir::ResourceState::shader_read,
        });
    }
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{index_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::index_read,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{pbr_target.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{depth_target.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{tone_target.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{pbr_timestamps.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    render_stream.append(device_ir::CmdWriteTimestamp{0U});
    render_stream.append(device_ir::CmdBeginRendering{pbr_target.value(), depth_target.value()});
    render_stream.append(device_ir::CmdClearColor{pbr_target.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    render_stream.append(device_ir::CmdClearDepth{depth_target.value(), 1.0F});
    render_stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    render_stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    render_stream.append(device_ir::CmdBindPipeline{pbr_pipeline.value()});
    render_stream.append(device_ir::CmdBindVertexBuffer{
        vertex_buffer.value(), 0U, production::kPbrUploadVertexStrideBytes});
    render_stream.append(device_ir::CmdBindIndexBuffer{
        index_buffer.value(), 0U, device_ir::IndexFormat::uint32});
    render_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{material_textures.at(0U)}, sampler.value()},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{material_textures.at(1U)}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 2U, device_ir::ResourceHandle{material_textures.at(2U)}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 3U, device_ir::ResourceHandle{material_textures.at(3U)}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 4U, device_ir::ResourceHandle{material_textures.at(4U)}, std::nullopt},
    }});
    render_stream.append(device_ir::CmdPushConstants{bytes_of(pbr_constants)});
    render_stream.append(device_ir::CmdDrawIndexed{
        static_cast<std::uint32_t>(indices.size()), 1U, 0U, 0, 0U});
    render_stream.append(device_ir::CmdEndRendering{});
    render_stream.append(device_ir::CmdWriteTimestamp{1U});
    render_stream.append(device_ir::CmdResolveTimestamps{
        pbr_timestamps.value(), 0U, 2U, 0U});
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{pbr_target.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    render_stream.append(device_ir::CmdBeginRendering{tone_target.value(), std::nullopt});
    render_stream.append(device_ir::CmdClearColor{tone_target.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    render_stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    render_stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    render_stream.append(device_ir::CmdBindPipeline{tone_pipeline.value()});
    render_stream.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 52U});
    render_stream.append(device_ir::CmdBindResources{{device_ir::ResourceBinding{
        0U, 0U, device_ir::ResourceHandle{pbr_target.value()}, sampler.value()}}});
    render_stream.append(device_ir::CmdPushConstants{bytes_of(tone_constants)});
    render_stream.append(device_ir::CmdDraw{3U, 1U, 0U, 0U});
    render_stream.append(device_ir::CmdEndRendering{});
    const auto render_serial = device.submit(render_stream, gpu::QueueType::graphics);
    if (!render_serial) throw TestFailure("D3D12 PBR/tone-map submit: " +
                                          render_serial.error().message);
    REQUIRE(device.wait(render_serial.value()));
    const auto timestamp_bytes = device.read_buffer(
        pbr_timestamps.value(), 0U, 2U * sizeof(std::uint64_t));
    REQUIRE(timestamp_bytes && timestamp_bytes.value().size() == 2U * sizeof(std::uint64_t));
    std::array<std::uint64_t, 2U> timestamp_values{};
    std::memcpy(timestamp_values.data(), timestamp_bytes.value().data(),
                timestamp_bytes.value().size());
    REQUIRE(timestamp_values[1U] >= timestamp_values[0U]);
    const long double pbr_gpu_nanoseconds =
        static_cast<long double>(timestamp_values[1U] - timestamp_values[0U]) *
        1'000'000'000.0L / static_cast<long double>(graphics_timestamp_frequency);
    REQUIRE(pbr_gpu_nanoseconds >= 0.0L);
    REQUIRE(pbr_fixture.upload.resident_bytes() ==
            vertices.size() * sizeof(production::PbrUploadVertex) +
                indices.size() * sizeof(std::uint32_t));
    std::cout << "D3D12 authored PBR fixture: "
              << static_cast<std::uint64_t>(pbr_gpu_nanoseconds)
              << " gpu ns, " << pbr_fixture.upload.resident_bytes()
              << " vertex/index bytes\n";
    const auto pbr_pixels = device.read_texture_rgba32f(pbr_target.value());
    const auto tone_pixels = device.read_texture_rgba32f(tone_target.value());
    REQUIRE(pbr_pixels && tone_pixels);
    const std::size_t center = (16U * 32U + 16U) * 4U;
    REQUIRE(pbr_pixels.value()[center] > 0.0F);
    REQUIRE(pbr_pixels.value()[center + 3U] > 0.9F);
    REQUIRE(tone_pixels.value()[center] > 0.0F && tone_pixels.value()[center] <= 1.01F);
    REQUIRE(tone_pixels.value()[center + 3U] > 0.9F);

    const auto shadow_color = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment,
        {0.0F, 0.0F, 0.0F, 1.0F},
        1.0F,
    });
    const auto shadow_depth = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment,
        {0.0F, 0.0F, 0.0F, 1.0F},
        1.0F,
    });
    REQUIRE(shadow_color && shadow_depth);
    std::array<float, 32U> shadow_constants{};
    for (std::size_t matrix = 0U; matrix < 2U; ++matrix) {
        for (std::size_t diagonal = 0U; diagonal < 4U; ++diagonal) {
            shadow_constants[matrix * 16U + diagonal * 5U] = 1.0F;
        }
    }
    device_ir::DeviceCommandStream shadow_stream;
    shadow_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{shadow_color.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    shadow_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{shadow_depth.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write,
    });
    shadow_stream.append(device_ir::CmdBeginRendering{
        shadow_color.value(), shadow_depth.value()});
    shadow_stream.append(device_ir::CmdClearColor{
        shadow_color.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    shadow_stream.append(device_ir::CmdClearDepth{shadow_depth.value(), 1.0F});
    shadow_stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    shadow_stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    shadow_stream.append(device_ir::CmdBindPipeline{shadow_pipeline.value()});
    shadow_stream.append(device_ir::CmdBindVertexBuffer{
        vertex_buffer.value(), 0U, production::kPbrUploadVertexStrideBytes});
    shadow_stream.append(device_ir::CmdPushConstants{bytes_of(shadow_constants)});
    shadow_stream.append(device_ir::CmdDraw{3U, 1U, 0U, 0U});
    shadow_stream.append(device_ir::CmdEndRendering{});
    const auto shadow_serial = device.submit(shadow_stream, gpu::QueueType::graphics);
    if (!shadow_serial) {
        throw TestFailure("D3D12 shadow submit: " + shadow_serial.error().message);
    }
    REQUIRE(device.wait(shadow_serial.value()));
    const auto shadow_pixels = device.read_texture_depth32f(shadow_depth.value());
    REQUIRE(shadow_pixels);
    const std::size_t shadow_center = 16U * 32U + 16U;
    REQUIRE(shadow_pixels.value()[shadow_center] < 0.99F);
    REQUIRE(device.destroy_texture(shadow_depth.value()));
    REQUIRE(device.destroy_texture(shadow_color.value()));

    const auto temporal_current = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.2F, 0.4F, 0.6F, 1.0F},
        1.0F,
    });
    const auto temporal_history = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.8F, 0.6F, 0.4F, 1.0F},
        1.0F,
    });
    const auto temporal_motion = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rg32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.0F, 0.0F, 0.0F, 0.0F},
        1.0F,
    });
    const auto temporal_resolved = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    REQUIRE(temporal_current && temporal_history && temporal_motion && temporal_resolved);

    device_ir::DeviceCommandStream temporal_setup;
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_current.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_history.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_motion.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    temporal_setup.append(device_ir::CmdBeginRendering{temporal_current.value(), std::nullopt});
    temporal_setup.append(device_ir::CmdClearColor{
        temporal_current.value(), {0.2F, 0.4F, 0.6F, 1.0F}});
    temporal_setup.append(device_ir::CmdEndRendering{});
    temporal_setup.append(device_ir::CmdBeginRendering{temporal_history.value(), std::nullopt});
    temporal_setup.append(device_ir::CmdClearColor{
        temporal_history.value(), {0.8F, 0.6F, 0.4F, 1.0F}});
    temporal_setup.append(device_ir::CmdEndRendering{});
    temporal_setup.append(device_ir::CmdBeginRendering{temporal_motion.value(), std::nullopt});
    temporal_setup.append(device_ir::CmdClearColor{
        temporal_motion.value(), {0.0F, 0.0F, 0.0F, 0.0F}});
    temporal_setup.append(device_ir::CmdEndRendering{});
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_current.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_history.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_motion.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    const auto temporal_setup_serial = device.submit(temporal_setup, gpu::QueueType::graphics);
    if (!temporal_setup_serial) {
        throw TestFailure("D3D12 temporal setup submit: " +
                          temporal_setup_serial.error().message);
    }
    REQUIRE(device.wait(temporal_setup_serial.value()));

    const std::array<float, 8U> temporal_float_constants = {
        0.5F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.125F, 0.125F,
    };
    const std::array<std::uint32_t, 4U> temporal_flags = {1U, 0U, 0U, 0U};
    std::array<std::uint8_t, 48U> temporal_constants{};
    std::memcpy(temporal_constants.data(), temporal_float_constants.data(),
                sizeof(temporal_float_constants));
    std::memcpy(temporal_constants.data() + sizeof(temporal_float_constants),
                temporal_flags.data(), sizeof(temporal_flags));
    device_ir::DeviceCommandStream temporal_stream;
    temporal_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_resolved.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    temporal_stream.append(device_ir::CmdBindPipeline{temporal_pipeline.value()});
    temporal_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{temporal_current.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{temporal_history.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 2U, device_ir::ResourceHandle{temporal_motion.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 3U, device_ir::ResourceHandle{temporal_resolved.value()}, std::nullopt},
    }});
    temporal_stream.append(device_ir::CmdPushConstants{bytes_of(temporal_constants)});
    temporal_stream.append(device_ir::CmdDispatch{1U, 1U, 1U});
    temporal_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{temporal_resolved.value()}});
    const auto temporal_serial = device.submit(temporal_stream, gpu::QueueType::compute);
    if (!temporal_serial) {
        throw TestFailure("D3D12 temporal submit: " + temporal_serial.error().message);
    }
    REQUIRE(device.wait(temporal_serial.value()));
    const auto temporal_pixels = device.read_texture_rgba32f(temporal_resolved.value());
    REQUIRE(temporal_pixels);
    const std::size_t temporal_center = (4U * 8U + 4U) * 4U;
    REQUIRE(temporal_pixels.value()[temporal_center] > 0.49F &&
            temporal_pixels.value()[temporal_center] < 0.51F);
    REQUIRE(temporal_pixels.value()[temporal_center + 1U] > 0.49F &&
            temporal_pixels.value()[temporal_center + 1U] < 0.51F);
    REQUIRE(temporal_pixels.value()[temporal_center + 2U] > 0.49F &&
            temporal_pixels.value()[temporal_center + 2U] < 0.51F);
    REQUIRE(temporal_pixels.value()[temporal_center + 3U] > 0.99F);
    REQUIRE(device.destroy_texture(temporal_resolved.value()));
    REQUIRE(device.destroy_texture(temporal_motion.value()));
    REQUIRE(device.destroy_texture(temporal_history.value()));
    REQUIRE(device.destroy_texture(temporal_current.value()));

    REQUIRE(device.destroy_texture(tone_target.value()));
    REQUIRE(device.destroy_texture(depth_target.value()));
    REQUIRE(device.destroy_texture(pbr_target.value()));
    for (const auto texture : material_textures) REQUIRE(device.destroy_texture(texture));
    REQUIRE(device.destroy_buffer(pbr_timestamps.value()));
    REQUIRE(device.destroy_buffer(index_buffer.value()));
    REQUIRE(device.destroy_buffer(vertex_buffer.value()));
    REQUIRE(device.destroy_sampler(sampler.value()));
    REQUIRE(device.destroy_texture(output.value()));
    REQUIRE(device.destroy_pipeline(compute_pipeline.value()));
    REQUIRE(device.destroy_pipeline(temporal_pipeline.value()));
    REQUIRE(device.destroy_shader(compute_shader.value()));
    REQUIRE(device.destroy_pipeline(tone_pipeline.value()));
    REQUIRE(device.destroy_pipeline(pbr_pipeline.value()));
    REQUIRE(device.destroy_pipeline(shadow_pipeline.value()));
    REQUIRE(device.destroy_shader(tone_fragment));
    REQUIRE(device.destroy_shader(tone_vertex));
    REQUIRE(device.destroy_shader(temporal_compute));
    REQUIRE(device.destroy_shader(shadow_fragment));
    REQUIRE(device.destroy_shader(shadow_vertex));
    REQUIRE(device.destroy_shader(pbr_fragment));
    REQUIRE(device.destroy_shader(pbr_vertex));
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 built-in shader debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
}

void d3d12_accepts_8k_temporal_resolve() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().max_texture_dimension_2d >= 8192U);
    const auto temporal_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "cartographer_8k_temporal",
        gpu::ShaderStage::compute,
        "CSMain",
        "cartographer_temporal.hlsl",
        read_d3d12_shader("cartographer_temporal.hlsl"),
        true,
        false,
    });
    REQUIRE(temporal_source);
    const auto temporal_shader = device.create_shader(temporal_source.value());
    REQUIRE(temporal_shader);
    gpu::PipelineDesc temporal_descriptor;
    temporal_descriptor.debug_name = "cartographer_8k_temporal";
    temporal_descriptor.shaders = {temporal_shader.value()};
    temporal_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::compute},
    };
    temporal_descriptor.sampled_texture_count = 3U;
    temporal_descriptor.depth_test = false;
    temporal_descriptor.push_constant_bytes = 48U;
    const auto temporal_pipeline = device.create_pipeline(temporal_descriptor);
    REQUIRE(temporal_pipeline);

    const auto probe_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "cartographer_8k_probe",
        gpu::ShaderStage::compute,
        "CSMain",
        "cartographer_8k_probe.hlsl",
        R"(
            Texture2D<float4> source_color : register(t0);
            RWTexture2D<float4> probe_color : register(u0);

            [numthreads(1, 1, 1)]
            void CSMain(uint3 dispatch_id : SV_DISPATCHTHREADID) {
                uint width;
                uint height;
                source_color.GetDimensions(width, height);
                probe_color[uint2(0, 0)] =
                    source_color.Load(int3(width / 2, height / 2, 0));
            }
        )",
        true,
        false,
    });
    REQUIRE(probe_source);
    const auto probe_shader = device.create_shader(probe_source.value());
    REQUIRE(probe_shader);
    gpu::PipelineDesc probe_descriptor;
    probe_descriptor.debug_name = "cartographer_8k_probe";
    probe_descriptor.shaders = {probe_shader.value()};
    probe_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
    };
    probe_descriptor.sampled_texture_count = 1U;
    probe_descriptor.depth_test = false;
    const auto probe_pipeline = device.create_pipeline(probe_descriptor);
    REQUIRE(probe_pipeline);

    const auto temporal_current = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rgba16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.2F, 0.4F, 0.6F, 1.0F},
        1.0F,
    });
    const auto temporal_history = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rgba16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.8F, 0.6F, 0.4F, 1.0F},
        1.0F,
    });
    const auto temporal_motion = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rg16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.0F, 0.0F, 0.0F, 0.0F},
        1.0F,
    });
    const auto temporal_resolved = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rgba16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::storage,
        {0.0F, 0.0F, 0.0F, 1.0F},
        1.0F,
    });
    const auto probe = device.create_texture(gpu::TextureDesc{
        1U,
        1U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    REQUIRE(temporal_current && temporal_history && temporal_motion && temporal_resolved && probe);

    device_ir::DeviceCommandStream setup;
    const auto clear_input = [&setup](gpu::TextureHandle texture, std::array<float, 4U> color) {
        setup.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::render_target,
        });
        setup.append(device_ir::CmdBeginRendering{texture, std::nullopt});
        setup.append(device_ir::CmdClearColor{texture, color});
        setup.append(device_ir::CmdEndRendering{});
        setup.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture},
            device_ir::ResourceState::render_target,
            device_ir::ResourceState::shader_read,
        });
    };
    clear_input(temporal_current.value(), {0.2F, 0.4F, 0.6F, 1.0F});
    clear_input(temporal_history.value(), {0.8F, 0.6F, 0.4F, 1.0F});
    clear_input(temporal_motion.value(), {0.0F, 0.0F, 0.0F, 0.0F});
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_resolved.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{probe.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    const auto setup_serial = device.submit(setup, gpu::QueueType::graphics);
    REQUIRE(setup_serial);
    REQUIRE(device.wait(setup_serial.value()));

    const std::array<float, 8U> temporal_float_constants = {
        0.5F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F / 8192.0F, 1.0F / 8192.0F,
    };
    const std::array<std::uint32_t, 4U> temporal_flags = {1U, 0U, 0U, 0U};
    std::array<std::uint8_t, 48U> temporal_constants{};
    std::memcpy(temporal_constants.data(), temporal_float_constants.data(),
                sizeof(temporal_float_constants));
    std::memcpy(temporal_constants.data() + sizeof(temporal_float_constants),
                temporal_flags.data(), sizeof(temporal_flags));
    device_ir::DeviceCommandStream temporal_stream;
    temporal_stream.append(device_ir::CmdBindPipeline{temporal_pipeline.value()});
    temporal_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{temporal_current.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{temporal_history.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 2U, device_ir::ResourceHandle{temporal_motion.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 3U, device_ir::ResourceHandle{temporal_resolved.value()}, std::nullopt},
    }});
    temporal_stream.append(device_ir::CmdPushConstants{bytes_of(temporal_constants)});
    temporal_stream.append(device_ir::CmdDispatch{1024U, 1024U, 1U});
    temporal_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{temporal_resolved.value()}});
    temporal_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_resolved.value()},
        device_ir::ResourceState::shader_write,
        device_ir::ResourceState::shader_read,
    });
    const auto temporal_serial = device.submit(temporal_stream, gpu::QueueType::compute);
    REQUIRE(temporal_serial);
    REQUIRE(device.wait(temporal_serial.value()));

    device_ir::DeviceCommandStream probe_stream;
    probe_stream.append(device_ir::CmdBindPipeline{probe_pipeline.value()});
    probe_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{temporal_resolved.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{probe.value()}, std::nullopt},
    }});
    probe_stream.append(device_ir::CmdDispatch{1U, 1U, 1U});
    probe_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{probe.value()}});
    const auto probe_serial = device.submit(probe_stream, gpu::QueueType::compute);
    if (!probe_serial) {
        throw TestFailure("D3D12 8K probe submit: " + probe_serial.error().message);
    }
    REQUIRE(probe_serial);
    REQUIRE(device.wait(probe_serial.value()));
    const auto probe_pixels = device.read_texture_rgba32f(probe.value());
    REQUIRE(probe_pixels);
    REQUIRE(probe_pixels.value().size() == 4U);
    REQUIRE(probe_pixels.value().at(0U) > 0.48F && probe_pixels.value().at(0U) < 0.52F);
    REQUIRE(probe_pixels.value().at(1U) > 0.48F && probe_pixels.value().at(1U) < 0.52F);
    REQUIRE(probe_pixels.value().at(2U) > 0.48F && probe_pixels.value().at(2U) < 0.52F);
    REQUIRE(probe_pixels.value().at(3U) > 0.99F);

    REQUIRE(device.destroy_texture(probe.value()));
    REQUIRE(device.destroy_texture(temporal_resolved.value()));
    REQUIRE(device.destroy_texture(temporal_motion.value()));
    REQUIRE(device.destroy_texture(temporal_history.value()));
    REQUIRE(device.destroy_texture(temporal_current.value()));
    REQUIRE(device.destroy_pipeline(probe_pipeline.value()));
    REQUIRE(device.destroy_shader(probe_shader.value()));
    REQUIRE(device.destroy_pipeline(temporal_pipeline.value()));
    REQUIRE(device.destroy_shader(temporal_shader.value()));
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 8K temporal debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
}

LRESULT CALLBACK d3d12_test_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void d3d12_exercises_hidden_swapchain_resize_and_present() {
    using namespace carto;

    auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto device = std::move(created.value());
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"CartographerD3D12AcceptanceWindow";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = d3d12_test_window_proc;
    window_class.lpszClassName = class_name;
    const ATOM atom = RegisterClassW(&window_class);
    REQUIRE(atom != 0U);
    HWND window = CreateWindowExW(
        0U,
        class_name,
        L"Cartographer D3D12 acceptance",
        WS_POPUP,
        0,
        0,
        1280,
        800,
        nullptr,
        nullptr,
        instance,
        nullptr);
    REQUIRE(window != nullptr);

    {
        auto created_swapchain = d3d12::D3D12Swapchain::create(
            *device,
            d3d12::D3D12SwapchainOptions{
                reinterpret_cast<std::uintptr_t>(window),
                1280U,
                800U,
                2U,
                gpu::Format::bgra8_unorm,
                false});
        REQUIRE(created_swapchain);
        auto swapchain = std::move(created_swapchain.value());
        const auto render_frame = [&]() {
            const auto back_buffer = swapchain->acquire_back_buffer();
            REQUIRE(back_buffer);
            device_ir::DeviceCommandStream frame;
            frame.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{back_buffer.value()},
                device_ir::ResourceState::present,
                device_ir::ResourceState::render_target,
            });
            frame.append(device_ir::CmdBeginRendering{back_buffer.value(), std::nullopt});
            frame.append(device_ir::CmdClearColor{
                back_buffer.value(), {0.035F, 0.045F, 0.065F, 1.0F}});
            frame.append(device_ir::CmdEndRendering{});
            frame.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{back_buffer.value()},
                device_ir::ResourceState::render_target,
                device_ir::ResourceState::present,
            });
            const auto serial = device->submit(frame, gpu::QueueType::graphics);
            REQUIRE(serial);
            REQUIRE(device->wait(serial.value()));
            REQUIRE(device->destroy_texture(back_buffer.value()));
            REQUIRE(swapchain->present(true));
        };

        render_frame();
        REQUIRE(swapchain->resize(640U, 480U));
        REQUIRE(swapchain->width() == 640U);
        REQUIRE(swapchain->height() == 480U);
        render_frame();
        REQUIRE(!swapchain->resize(0U, 480U));
    }
    DestroyWindow(window);
    REQUIRE(UnregisterClassW(class_name, instance) != 0);
    REQUIRE(device->debug_receipts().empty());
}

void d3d12_uploads_and_reads_specific_texture_mips() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    if (!created) {
        throw TestFailure("D3D12 mip device create: " + created.error().message);
    }
    auto& device = *created.value();

    const std::array<std::uint8_t, 16U> rgba{
        255U, 0U, 0U, 255U,
        0U, 255U, 0U, 255U,
        0U, 0U, 255U, 255U,
        255U, 255U, 255U, 255U,
    };
    std::vector<std::uint8_t> tga(18U, 0U);
    tga[2U] = 2U;
    tga[12U] = 2U;
    tga[14U] = 2U;
    tga[16U] = 32U;
    tga[17U] = 0x28U;
    for (std::size_t offset = 0U; offset < rgba.size(); offset += 4U) {
        tga.insert(tga.end(), {
            rgba[offset + 2U], rgba[offset + 1U], rgba[offset], rgba[offset + 3U]});
    }
    const auto decoded = assets::decode_tga_rgba8(tga);
    REQUIRE(decoded);
    const auto chain = assets::generate_rgba8_mips(decoded.value(), {
        .semantic = assets::TextureSemantic::data,
        .color_space = assets::ColorSpace::linear,
    });
    REQUIRE(chain);
    const auto plan = d3d12::build_texture_upload_plan(chain.value());
    REQUIRE(plan);
    REQUIRE(plan.value().footprints.size() == 2U);
    REQUIRE(plan.value().footprints[0U].offset == 0U);
    REQUIRE(plan.value().footprints[0U].row_pitch == 256U);
    REQUIRE(plan.value().footprints[1U].offset == 512U);
    REQUIRE(plan.value().staging_bytes.size() == 768U);
    REQUIRE(plan.value().resident_bytes == 20U);
    auto tampered_plan = plan.value();
    tampered_plan.staging_bytes.front() ^= 1U;
    REQUIRE(!tampered_plan.validate());

    const auto upload = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(plan.value().staging_bytes.size()),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    const auto readback = device.create_buffer(gpu::BufferDesc{
        256U,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    auto texture_descriptor = plan.value().texture;
    texture_descriptor.usage =
        texture_descriptor.usage | gpu::TextureUsage::transfer_source;
    const auto texture = device.create_texture(texture_descriptor);
    REQUIRE(upload && readback && texture);
    REQUIRE(device.write_buffer(upload.value(), 0U, plan.value().staging_bytes));

    device_ir::DeviceCommandStream upload_stream;
    upload_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    upload_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    REQUIRE(d3d12::append_texture_upload_copies(
        upload_stream, upload.value(), texture.value(), plan.value()));
    const auto upload_serial = device.submit(upload_stream, gpu::QueueType::copy);
    if (!upload_serial) {
        throw TestFailure("D3D12 mip upload submit: " + upload_serial.error().message);
    }
    REQUIRE(device.wait(upload_serial.value()));

    device_ir::DeviceCommandStream readback_stream;
    readback_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::copy_destination,
        device_ir::ResourceState::copy_source,
    });
    readback_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    readback_stream.append(device_ir::CmdCopyTextureToBuffer{
        texture.value(), readback.value(), 0U, 1U});
    const auto serial = device.submit(readback_stream, gpu::QueueType::copy);
    if (!serial) {
        throw TestFailure("D3D12 mip upload/readback submit: " + serial.error().message);
    }
    REQUIRE(device.wait(serial.value()));
    const auto& terminal_mip = chain.value().levels.back().pixels;
    const auto pixels = device.read_buffer(readback.value(), 0U, terminal_mip.size());
    REQUIRE(pixels);
    if (!std::equal(terminal_mip.begin(), terminal_mip.end(), pixels.value().begin())) {
        throw TestFailure(
            "D3D12 mip-1 readback mismatch: expected " +
            std::to_string(terminal_mip[0U]) + "," +
            std::to_string(terminal_mip[1U]) + "," +
            std::to_string(terminal_mip[2U]) + "," +
            std::to_string(terminal_mip[3U]) + " received " +
            std::to_string(pixels.value()[0U]) + "," +
            std::to_string(pixels.value()[1U]) + "," +
            std::to_string(pixels.value()[2U]) + "," +
            std::to_string(pixels.value()[3U]));
    }

    device_ir::DeviceCommandStream rejected_role_flip;
    rejected_role_flip.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::copy_source,
        device_ir::ResourceState::copy_destination,
    });
    rejected_role_flip.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::copy_destination,
        device_ir::ResourceState::copy_source,
    });
    const auto rejected_serial = device.submit(
        rejected_role_flip, gpu::QueueType::copy);
    REQUIRE(!rejected_serial);
    REQUIRE(rejected_serial.error().message.find("submission boundary") !=
            std::string::npos);

    REQUIRE(device.destroy_texture(texture.value()));
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.destroy_buffer(upload.value()));
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 mip upload debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
}

} // namespace

int main() {
    try {
        d3d12_device_reports_native_identity_and_owns_resources();
        std::cout << "PASS D3D12 native identity and resource ownership\n";
        d3d12_lifetimes_recycle_descriptors_and_defer_resource_destruction();
        std::cout << "PASS D3D12 descriptor reuse and deferred destruction\n";
        d3d12_device_loss_is_fail_closed_and_recoverable();
        std::cout << "PASS D3D12 device-loss recovery boundary\n";
        d3d12_captures_native_timestamp_queries();
        std::cout << "PASS D3D12 native timestamp queries\n";
        d3d12_uses_compute_queue_shader_states();
        std::cout << "PASS D3D12 compute queue shader states\n";
        d3d12_reuses_multiple_in_flight_command_contexts();
        std::cout << "PASS D3D12 in-flight command contexts\n";
        d3d12_compiles_dxil_draws_indexed_and_reads_texture();
        std::cout << "PASS D3D12 DXIL indexed draw and texture readback\n";
        d3d12_uploads_and_reads_specific_texture_mips();
        std::cout << "PASS D3D12 mip-specific texture upload and readback\n";
        d3d12_executes_builtin_shader_suite_and_dispatches_compute();
        std::cout << "PASS D3D12 built-in PBR/tone-map/shadow/temporal and compute dispatch\n";
        d3d12_accepts_8k_temporal_resolve();
        std::cout << "PASS D3D12 8K temporal resolve acceptance\n";
        d3d12_exercises_hidden_swapchain_resize_and_present();
        std::cout << "PASS D3D12 hidden swapchain resize and present\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL D3D12 native identity and resource ownership: "
                  << error.what() << '\n';
        return 1;
    }
}
