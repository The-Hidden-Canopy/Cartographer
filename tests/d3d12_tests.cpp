#include <carto/d3d12/device.hpp>

#include <cstdint>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
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

template <typename T>
std::vector<std::uint8_t> bytes_of(const T& value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    return std::vector<std::uint8_t>(begin, begin + sizeof(T));
}

void d3d12_compiles_dxil_draws_indexed_and_reads_texture() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();

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
    REQUIRE(device.debug_receipts().empty());

    REQUIRE(device.destroy_texture(depth.value()));
    REQUIRE(device.destroy_texture(color.value()));
    REQUIRE(device.destroy_buffer(index_buffer.value()));
    REQUIRE(device.destroy_buffer(vertex_buffer.value()));
    REQUIRE(device.destroy_pipeline(pipeline.value()));
    REQUIRE(device.destroy_shader(fragment.value()));
    REQUIRE(device.destroy_shader(vertex.value()));
}

} // namespace

int main() {
    try {
        d3d12_device_reports_native_identity_and_owns_resources();
        std::cout << "PASS D3D12 native identity and resource ownership\n";
        d3d12_compiles_dxil_draws_indexed_and_reads_texture();
        std::cout << "PASS D3D12 DXIL indexed draw and texture readback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL D3D12 native identity and resource ownership: "
                  << error.what() << '\n';
        return 1;
    }
}
