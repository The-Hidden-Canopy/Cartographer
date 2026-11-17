#include <carto/d3d12/device.hpp>
#include <carto/d3d12/authored_anisotropy.hpp>
#include <array>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
struct TestFailure : std::runtime_error {using std::runtime_error::runtime_error;};
#define REQUIRE(x) do {if(!(x)) throw TestFailure(#x);} while(false)
template<class T> std::vector<std::uint8_t> bytes_of(const T& v) {
    auto p=reinterpret_cast<const std::uint8_t*>(&v);return {p,p+sizeof(v)};
}
std::string shader_text() {
    std::ifstream input(std::string(CARTO_D3D12_SHADER_DIR)+"/cartographer_authored_anisotropy.hlsli");
    REQUIRE(input);return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void parity() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    std::cout << "D3D12 adapter: " << device.identity().adapter_name << "\n";

    const auto vertex_shader = d3d12::compile_hlsl(d3d12::ShaderSource{
        "authored_anisotropy_vertex",
        gpu::ShaderStage::vertex,
        "main",
        "authored_anisotropy_vertex.hlsl",
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
        "authored_anisotropy_fragment",
        gpu::ShaderStage::fragment,
        "main",
        "authored_anisotropy_fragment.hlsl",
        shader_text() + R"(
            cbuffer PushConstants : register(b0) { float4 factors; float4 view_dir; float4 light_dir; };
            float4 main() : SV_TARGET {
                return float4(carto_authored_anisotropy(factors.x,factors.y,factors.z,view_dir.xyz,light_dir.xyz),1.0);
            }
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
    pipeline_descriptor.debug_name = "authored_anisotropy_parity";
    pipeline_descriptor.shaders = {vertex.value(), fragment.value()};
    pipeline_descriptor.cull_mode = gpu::CullMode::none;
    pipeline_descriptor.color_format = gpu::Format::rgba32_float;
    pipeline_descriptor.depth_format = gpu::Format::depth32_float;
    pipeline_descriptor.push_constant_bytes = 48U;
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

    struct Case { float roughness, strength, rotation; core::Vec3d view, light; };
    const auto v = core::Vec3d{0.4,0.2,1}.normalized();
    const auto l = core::Vec3d{-0.3,0.5,1}.normalized();
    const std::array cases{
        Case{.5F,0,0,v,l}, Case{.5F,.8F,0,v,l}, Case{.5F,.8F,1.5707963F,v,l},
        Case{.5F,.8F,0,l,v}, Case{.5F,1,.7F,v,l}, Case{0,0,0,{0,0,1},{0,0,1}},
        Case{1,1,0,v,l}, Case{.1F,.9F,.3F,core::Vec3d{1,0,1e-6}.normalized(),core::Vec3d{-1,.1,1e-6}.normalized()},
        Case{.5F,.8F,0,core::Vec3d{1,0,1e-6}.normalized(),core::Vec3d{-1,0,1e-6}.normalized()},
        Case{.5F,.8F,0,{0,0,-1},l}, Case{.5F,.8F,0,v,{1,0,0}},
        Case{.5F,.8F,-.3F,{v.x,-v.y,v.z},{l.x,-l.y,l.z}}, Case{.5F,.8F,.3F,v,l}
    };
    for(const auto& item : cases) {
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
    if (&item == &cases.front()) {
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{index_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::index_read});
    }
    stream.append(device_ir::CmdBeginRendering{color.value(), depth.value()});
    stream.append(device_ir::CmdClearColor{color.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    stream.append(device_ir::CmdClearDepth{depth.value(), 1.0F});
    stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    stream.append(device_ir::CmdBindPipeline{pipeline.value()});
    stream.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 16U});
    stream.append(device_ir::CmdBindIndexBuffer{
        index_buffer.value(), 0U, device_ir::IndexFormat::uint32});
    const std::array<float,12U> draw_color = {item.roughness,item.strength,item.rotation,0,
        float(item.view.x),float(item.view.y),float(item.view.z),0,
        float(item.light.x),float(item.light.y),float(item.light.z),0};
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
    const auto reference=d3d12::evaluate_authored_anisotropy_reference(
        draw_color[0],draw_color[1],draw_color[2],
        {draw_color[4],draw_color[5],draw_color[6]}, {draw_color[8],draw_color[9],draw_color[10]});
    REQUIRE(reference);
    const std::array expected{reference.value().distribution,reference.value().visibility,reference.value().kernel};
    for(std::size_t channel=0;channel<3;++channel) {
        const double actual=pixels.value()[center+channel];
        if(!std::isfinite(actual) || std::abs(actual-expected[channel]) > 2e-4*std::max(1.0,std::abs(expected[channel])))
            throw TestFailure("GPU/CPU parity case="+std::to_string(&item-&cases.front())+" channel="+std::to_string(channel)+" actual="+std::to_string(actual)+" expected="+std::to_string(expected[channel]));
    }
    REQUIRE(pixels.value()[center+3U]==1.0F);
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
    REQUIRE(device.debug_receipts().empty());

    REQUIRE(device.destroy_texture(depth.value()));
    REQUIRE(device.destroy_texture(color.value()));
    }
    REQUIRE(device.destroy_buffer(index_buffer.value()));
    REQUIRE(device.destroy_buffer(vertex_buffer.value()));
    REQUIRE(device.destroy_pipeline(pipeline.value()));
    REQUIRE(device.destroy_shader(fragment.value()));
    REQUIRE(device.destroy_shader(vertex.value()));
}

int main(){try{parity();std::cout<<"PASS authored anisotropy GPU/CPU parity\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
