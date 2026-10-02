#include <carto/device_cpu/cpu_device.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

template <typename T>
std::vector<std::uint8_t> bytes_of(const T& value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    return std::vector<std::uint8_t>(begin, begin + sizeof(T));
}

carto::gpu::BufferDesc transfer_buffer(std::uint64_t bytes) {
    return carto::gpu::BufferDesc{
        bytes,
        carto::gpu::MemoryClass::upload,
        carto::gpu::BufferUsage::vertex | carto::gpu::BufferUsage::index |
            carto::gpu::BufferUsage::transfer_source | carto::gpu::BufferUsage::transfer_destination,
    };
}

void cpu_reference_rasterizes_and_reads_back_a_triangle() {
    using namespace carto;

    device_cpu::CpuDevice device;
    REQUIRE(device.backend() == device::BackendKind::cpu);
    REQUIRE(device.identity().stable_adapter_id == "carto.cpu.reference.v1");
    REQUIRE(device.capabilities().graphics_queue);

    const auto vertex_buffer = device.create_buffer(transfer_buffer(3U * 4U * sizeof(float)));
    const auto occluded_vertex_buffer = device.create_buffer(transfer_buffer(3U * 4U * sizeof(float)));
    const auto index_buffer = device.create_buffer(transfer_buffer(3U * sizeof(std::uint32_t)));
    REQUIRE(vertex_buffer && occluded_vertex_buffer && index_buffer);

    const std::array<std::array<float, 4U>, 3U> vertices = {{
        {{-0.8F, -0.8F, 0.25F, 1.0F}},
        {{0.8F, -0.8F, 0.25F, 1.0F}},
        {{0.0F, 0.8F, 0.25F, 1.0F}},
    }};
    const std::array<std::uint32_t, 3U> indices = {0U, 1U, 2U};
    const std::array<std::array<float, 4U>, 3U> occluded_vertices = {{
        {{-0.8F, -0.8F, 0.75F, 1.0F}},
        {{0.8F, -0.8F, 0.75F, 1.0F}},
        {{0.0F, 0.8F, 0.75F, 1.0F}},
    }};
    REQUIRE(device.write_buffer(vertex_buffer.value(), 0U, bytes_of(vertices)));
    REQUIRE(device.write_buffer(occluded_vertex_buffer.value(), 0U, bytes_of(occluded_vertices)));
    REQUIRE(device.write_buffer(index_buffer.value(), 0U, bytes_of(indices)));

    const gpu::TextureDesc color_descriptor{
        16U,
        16U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    const gpu::TextureDesc depth_descriptor{
        16U,
        16U,
        1U,
        1U,
        1U,
        gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment | gpu::TextureUsage::transfer_source,
    };
    const auto color = device.create_texture(color_descriptor);
    const auto depth = device.create_texture(depth_descriptor);
    REQUIRE(color && depth);

    const auto vertex_shader = device.create_shader(device::ShaderBinary{
        "cpu_vertex",
        gpu::ShaderStage::vertex,
        "main",
        device::BinaryFormat::cpu_ir,
        "cpu-ir-v1",
        "cpu-reference",
        {'p', 'o', 's', '4'},
        {},
    });
    const auto fragment_shader = device.create_shader(device::ShaderBinary{
        "cpu_fragment",
        gpu::ShaderStage::fragment,
        "main",
        device::BinaryFormat::cpu_ir,
        "cpu-ir-v1",
        "cpu-reference",
        {'f', 'l', 'a', 't'},
        {},
    });
    REQUIRE(vertex_shader && fragment_shader);

    gpu::PipelineDesc pipeline_descriptor;
    pipeline_descriptor.debug_name = "cpu_triangle";
    pipeline_descriptor.shaders = {vertex_shader.value(), fragment_shader.value()};
    pipeline_descriptor.cull_mode = gpu::CullMode::none;
    pipeline_descriptor.color_format = gpu::Format::rgba32_float;
    pipeline_descriptor.depth_format = gpu::Format::depth32_float;
    pipeline_descriptor.push_constant_bytes = 16U;
    const auto pipeline = device.create_pipeline(pipeline_descriptor);
    REQUIRE(pipeline);

    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdBeginLabel{"cpu triangle"});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{color.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{depth.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{index_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::index_read,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{occluded_vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read,
    });
    stream.append(device_ir::CmdBeginRendering{color.value(), depth.value()});
    stream.append(device_ir::CmdClearColor{color.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    stream.append(device_ir::CmdClearDepth{depth.value(), 1.0F});
    stream.append(device_ir::CmdBindPipeline{pipeline.value()});
    stream.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 16U});
    stream.append(device_ir::CmdBindIndexBuffer{
        index_buffer.value(), 0U, device_ir::IndexFormat::uint32});
    const std::array<float, 4U> color_value = {1.0F, 0.25F, 0.0F, 1.0F};
    stream.append(device_ir::CmdPushConstants{bytes_of(color_value)});
    stream.append(device_ir::CmdDrawIndexed{3U, 1U, 0U, 0, 0U});
    stream.append(device_ir::CmdBindVertexBuffer{occluded_vertex_buffer.value(), 0U, 16U});
    const std::array<float, 4U> occluded_color = {0.0F, 0.0F, 1.0F, 1.0F};
    stream.append(device_ir::CmdPushConstants{bytes_of(occluded_color)});
    stream.append(device_ir::CmdDrawIndexed{3U, 1U, 0U, 0, 0U});
    stream.append(device_ir::CmdEndRendering{});
    stream.append(device_ir::CmdEndLabel{});

    REQUIRE(stream.validate());
    REQUIRE(stream.dump().find("draw_indexed") != std::string::npos);
    const auto serial = device.submit(stream);
    REQUIRE(serial && serial.value() == 1U);
    REQUIRE(device.completed_serial() == serial.value());
    REQUIRE(device.wait(serial.value()));

    const auto pixels = device.read_texture_rgba32f(color.value());
    REQUIRE(pixels && pixels.value().size() == 16U * 16U * 4U);
    const std::size_t center = (8U * 16U + 8U) * 4U;
    const std::size_t corner = 0U;
    REQUIRE(pixels.value()[center] > 0.9F);
    REQUIRE(pixels.value()[center + 1U] > 0.2F);
    REQUIRE(pixels.value()[center + 3U] > 0.9F);
    REQUIRE(pixels.value()[corner] < 0.01F);
    REQUIRE(pixels.value()[corner + 1U] < 0.01F);
}

void cpu_reference_copies_bytes_and_rejects_stale_handles() {
    using namespace carto;

    device_cpu::CpuDevice device;
    const auto source = device.create_buffer(transfer_buffer(8U));
    const auto destination = device.create_buffer(transfer_buffer(8U));
    REQUIRE(source && destination);
    const std::array<std::uint8_t, 8U> payload = {1U, 3U, 5U, 7U, 9U, 11U, 13U, 15U};
    REQUIRE(device.write_buffer(source.value(), 0U, payload));

    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{source.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{destination.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    stream.append(device_ir::CmdCopyBuffer{source.value(), destination.value(), 0U, 0U, 8U});
    const auto serial = device.submit(stream, gpu::QueueType::copy);
    REQUIRE(serial && serial.value() == 1U);
    const auto copied = device.read_buffer(destination.value(), 0U, 8U);
    REQUIRE(copied && copied.value() == std::vector<std::uint8_t>(payload.begin(), payload.end()));

    REQUIRE(device.destroy_buffer(source.value()));
    const auto stale = device.read_buffer(source.value(), 0U, 1U);
    REQUIRE(!stale);
    REQUIRE(stale.error().code == core::ErrorCode::stale_data);
    REQUIRE(!device.wait(0U));
}

void cpu_reference_uploads_and_samples_texture() {
    using namespace carto;

    device_cpu::CpuDevice device;
    const auto vertex_buffer = device.create_buffer(transfer_buffer(3U * 6U * sizeof(float)));
    const auto index_buffer = device.create_buffer(transfer_buffer(3U * sizeof(std::uint32_t)));
    const auto texture_upload = device.create_buffer(transfer_buffer(2U * 2U * 4U * sizeof(float)));
    const auto readback = device.create_buffer(gpu::BufferDesc{
        8U * 8U * 4U * sizeof(float),
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(vertex_buffer && index_buffer && texture_upload && readback);

    const std::array<std::array<float, 6U>, 3U> vertices = {{
        {{-0.8F, -0.8F, 0.25F, 1.0F, 1.4F, 1.4F}},
        {{0.8F, -0.8F, 0.25F, 1.0F, 1.4F, 1.4F}},
        {{0.0F, 0.8F, 0.25F, 1.0F, 1.4F, 1.4F}},
    }};
    const std::array<std::uint32_t, 3U> indices = {0U, 1U, 2U};
    const std::array<std::array<float, 4U>, 4U> texels = {{
        {{1.0F, 0.0F, 0.0F, 1.0F}},
        {{0.0F, 1.0F, 0.0F, 1.0F}},
        {{0.0F, 0.0F, 1.0F, 1.0F}},
        {{0.25F, 0.5F, 0.75F, 1.0F}},
    }};
    REQUIRE(device.write_buffer(vertex_buffer.value(), 0U, bytes_of(vertices)));
    REQUIRE(device.write_buffer(index_buffer.value(), 0U, bytes_of(indices)));
    REQUIRE(device.write_buffer(texture_upload.value(), 0U, bytes_of(texels)));

    const auto color = device.create_texture(gpu::TextureDesc{
        8U, 8U, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    });
    const auto depth = device.create_texture(gpu::TextureDesc{
        8U, 8U, 1U, 1U, 1U, gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment,
    });
    const auto sampled = device.create_texture(gpu::TextureDesc{
        2U, 2U, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
    });
    gpu::SamplerDesc sampler_descriptor;
    sampler_descriptor.min_filter = gpu::Filter::nearest;
    sampler_descriptor.mag_filter = gpu::Filter::nearest;
    sampler_descriptor.u = gpu::AddressMode::clamp_edge;
    sampler_descriptor.v = gpu::AddressMode::clamp_edge;
    const auto sampler = device.create_sampler(sampler_descriptor);
    REQUIRE(color && depth && sampled && sampler);

    const auto vertex_shader = device.create_shader(device::ShaderBinary{
        "cpu_sample_vertex", gpu::ShaderStage::vertex, "main", device::BinaryFormat::cpu_ir,
        "cpu-ir-v1", "cpu-reference", {'p', 'o', 's', '4'}, {},
    });
    const auto fragment_shader = device.create_shader(device::ShaderBinary{
        "cpu_sample_fragment", gpu::ShaderStage::fragment, "main", device::BinaryFormat::cpu_ir,
        "cpu-ir-v1", "cpu-reference", {'s', 'a', 'm', 'p', 'l', 'e'}, {},
    });
    REQUIRE(vertex_shader && fragment_shader);

    gpu::PipelineDesc pipeline_descriptor;
    pipeline_descriptor.debug_name = "cpu_texture_sample";
    pipeline_descriptor.shaders = {vertex_shader.value(), fragment_shader.value()};
    pipeline_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::fragment},
    };
    pipeline_descriptor.cull_mode = gpu::CullMode::none;
    pipeline_descriptor.color_format = gpu::Format::rgba32_float;
    pipeline_descriptor.depth_format = gpu::Format::depth32_float;
    pipeline_descriptor.push_constant_bytes = 16U;
    const auto pipeline = device.create_pipeline(pipeline_descriptor);
    REQUIRE(pipeline);

    device_ir::DeviceCommandStream wrong_state;
    device_ir::ResourceBinding wrong_state_binding;
    wrong_state_binding.resource = device_ir::ResourceHandle{sampled.value()};
    wrong_state_binding.sampler = sampler.value();
    wrong_state.append(device_ir::CmdBindResources{{wrong_state_binding}});
    const auto rejected = device.submit(wrong_state);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::validation_failed);

    device_ir::ResourceBinding sampled_binding;
    sampled_binding.resource = device_ir::ResourceHandle{sampled.value()};
    sampled_binding.sampler = sampler.value();
    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture_upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{sampled.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    stream.append(device_ir::CmdCopyBufferToTexture{
        texture_upload.value(), sampled.value(), 0U});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{sampled.value()},
        device_ir::ResourceState::copy_destination,
        device_ir::ResourceState::shader_read,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{color.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{depth.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{index_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::index_read,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    stream.append(device_ir::CmdBeginRendering{color.value(), depth.value()});
    stream.append(device_ir::CmdClearColor{color.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    stream.append(device_ir::CmdClearDepth{depth.value(), 1.0F});
    stream.append(device_ir::CmdBindPipeline{pipeline.value()});
    stream.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 6U * sizeof(float)});
    stream.append(device_ir::CmdBindIndexBuffer{
        index_buffer.value(), 0U, device_ir::IndexFormat::uint32});
    stream.append(device_ir::CmdBindResources{{sampled_binding}});
    const std::array<float, 4U> multiplier = {2.0F, 2.0F, 2.0F, 1.0F};
    stream.append(device_ir::CmdPushConstants{bytes_of(multiplier)});
    stream.append(device_ir::CmdDrawIndexed{3U, 1U, 0U, 0, 0U});
    stream.append(device_ir::CmdEndRendering{});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{color.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::copy_source,
    });
    stream.append(device_ir::CmdCopyTextureToBuffer{color.value(), readback.value(), 0U});

    const auto serial = device.submit(stream);
    REQUIRE(serial);
    REQUIRE(device.wait(serial.value()));
    const auto pixels = device.read_texture_rgba32f(color.value());
    REQUIRE(pixels);
    const std::size_t center = (4U * 8U + 4U) * 4U;
    REQUIRE(std::abs(pixels.value()[center] - 0.5F) < 0.001F);
    REQUIRE(std::abs(pixels.value()[center + 1U] - 1.0F) < 0.001F);
    REQUIRE(std::abs(pixels.value()[center + 2U] - 1.5F) < 0.001F);
    REQUIRE(std::abs(pixels.value()[center + 3U] - 1.0F) < 0.001F);

    const auto copied = device.read_buffer(
        readback.value(), static_cast<std::uint64_t>(center * sizeof(float)),
        4U * sizeof(float));
    REQUIRE(copied);
    std::array<float, 4U> copied_pixel{};
    std::memcpy(copied_pixel.data(), copied.value().data(), sizeof(copied_pixel));
    REQUIRE(std::abs(copied_pixel[0] - 0.5F) < 0.001F);
    REQUIRE(std::abs(copied_pixel[1] - 1.0F) < 0.001F);
    REQUIRE(std::abs(copied_pixel[2] - 1.5F) < 0.001F);
}

void cpu_reference_rejects_wrong_resource_state_and_presentation() {
    using namespace carto;

    device_cpu::CpuDevice device;
    const auto texture = device.create_texture(gpu::TextureDesc{
        4U,
        4U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    });
    REQUIRE(texture);

    device_ir::DeviceCommandStream clear_without_transition;
    clear_without_transition.append(device_ir::CmdClearColor{
        texture.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    const auto rejected = device.submit(clear_without_transition);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::validation_failed);

    device_ir::DeviceCommandStream present;
    present.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::present,
    });
    present.append(device_ir::CmdPresent{texture.value()});
    const auto unsupported = device.submit(present);
    REQUIRE(!unsupported);
    REQUIRE(unsupported.error().code == core::ErrorCode::unsupported);

    const auto foreign_shader = device.create_shader(device::ShaderBinary{
        "foreign",
        gpu::ShaderStage::vertex,
        "main",
        device::BinaryFormat::dxil,
        "external",
        "compiler",
        {'d', 'x', 'i', 'l'},
        {},
    });
    REQUIRE(!foreign_shader);
    REQUIRE(foreign_shader.error().code == core::ErrorCode::unsupported);
}

void command_ir_rejects_unbalanced_and_unsupported_shapes_before_execution() {
    using namespace carto;

    device_ir::DeviceCommandStream unbalanced;
    unbalanced.append(device_ir::CmdBeginRendering{
        gpu::TextureHandle{1U, 1U}, std::nullopt});
    REQUIRE(!unbalanced.validate());
    REQUIRE(unbalanced.validate().error().code == core::ErrorCode::validation_failed);

    device_ir::DeviceCommandStream invalid_draw;
    invalid_draw.append(device_ir::CmdDrawIndexed{4U, 1U, 0U, 0, 0U});
    REQUIRE(!invalid_draw.validate());
    REQUIRE(invalid_draw.validate().error().code == core::ErrorCode::validation_failed);

    device_ir::DeviceCommandStream invalid_clear;
    invalid_clear.append(device_ir::CmdClearDepth{
        gpu::TextureHandle{1U, 1U}, std::numeric_limits<float>::quiet_NaN()});
    REQUIRE(!invalid_clear.validate());
    REQUIRE(invalid_clear.validate().error().code == core::ErrorCode::invalid_argument);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests = {
        {"CPU reference rasterizes and reads back a triangle",
         cpu_reference_rasterizes_and_reads_back_a_triangle},
        {"CPU reference copies bytes and rejects stale handles",
         cpu_reference_copies_bytes_and_rejects_stale_handles},
        {"CPU reference uploads and samples a texture",
         cpu_reference_uploads_and_samples_texture},
        {"CPU reference rejects wrong states and presentation",
         cpu_reference_rejects_wrong_resource_state_and_presentation},
        {"command IR rejects unbalanced and invalid shapes",
         command_ir_rejects_unbalanced_and_unsupported_shapes_before_execution},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
