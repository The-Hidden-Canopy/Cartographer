#pragma once

#include <carto/device/device.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace carto::device_cpu {

struct CpuDeviceOptions {
    std::uint32_t max_texture_dimension_2d = 16384U;
    std::uint64_t max_allocation_bytes = 512ULL * 1024ULL * 1024ULL;
};

class CpuDevice final : public device::Device {
public:
    explicit CpuDevice(CpuDeviceOptions options = {});

    [[nodiscard]] device::BackendKind backend() const noexcept override;
    [[nodiscard]] const device::DeviceIdentity& identity() const noexcept override;
    [[nodiscard]] const gpu::DeviceCapabilities& capabilities() const noexcept override;

    [[nodiscard]] core::Result<gpu::BufferHandle> create_buffer(
        const gpu::BufferDesc& descriptor) override;
    [[nodiscard]] core::Result<gpu::TextureHandle> create_texture(
        const gpu::TextureDesc& descriptor) override;
    [[nodiscard]] core::Result<gpu::SamplerHandle> create_sampler(
        const gpu::SamplerDesc& descriptor) override;
    [[nodiscard]] core::Result<gpu::ShaderHandle> create_shader(
        const device::ShaderBinary& binary) override;
    [[nodiscard]] core::Result<gpu::PipelineHandle> create_pipeline(
        const gpu::PipelineDesc& descriptor) override;

    [[nodiscard]] core::Result<void> destroy_buffer(gpu::BufferHandle handle) override;
    [[nodiscard]] core::Result<void> destroy_texture(gpu::TextureHandle handle) override;
    [[nodiscard]] core::Result<void> destroy_sampler(gpu::SamplerHandle handle) override;
    [[nodiscard]] core::Result<void> destroy_shader(gpu::ShaderHandle handle) override;
    [[nodiscard]] core::Result<void> destroy_pipeline(gpu::PipelineHandle handle) override;

    [[nodiscard]] core::Result<void> write_buffer(
        gpu::BufferHandle handle,
        std::uint64_t offset,
        std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> read_buffer(
        gpu::BufferHandle handle,
        std::uint64_t offset,
        std::uint64_t bytes) const override;
    [[nodiscard]] core::Result<std::vector<float>> read_texture_rgba32f(
        gpu::TextureHandle handle) const override;

    [[nodiscard]] core::Result<gpu::SubmissionSerial> submit(
        const device_ir::DeviceCommandStream& stream,
        gpu::QueueType queue = gpu::QueueType::graphics) override;
    [[nodiscard]] gpu::SubmissionSerial completed_serial() const noexcept override;
    [[nodiscard]] core::Result<void> wait(gpu::SubmissionSerial serial) override;

private:
    struct BufferResource {
        gpu::BufferDesc descriptor;
        std::vector<std::uint8_t> bytes;
    };

    struct TextureResource {
        gpu::TextureDesc descriptor;
        std::vector<float> color;
        std::vector<float> depth;
    };

    struct ShaderResource {
        device::ShaderBinary binary;
        gpu::ShaderDesc descriptor;
    };

    struct PipelineResource {
        gpu::PipelineDesc descriptor;
        bool samples_rgba32f = false;
    };

    struct SamplerResource {
        gpu::SamplerDesc descriptor;
    };

    struct Viewport {
        float x = 0.0F;
        float y = 0.0F;
        float width = 1.0F;
        float height = 1.0F;
        float min_depth = 0.0F;
        float max_depth = 1.0F;
    };

    struct Scissor {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::uint32_t width = 1U;
        std::uint32_t height = 1U;
    };

    struct ExecutionState {
        std::optional<gpu::TextureHandle> color_target;
        std::optional<gpu::TextureHandle> depth_target;
        std::optional<gpu::PipelineHandle> pipeline;
        std::optional<device_ir::CmdBindVertexBuffer> vertex_buffer;
        std::optional<device_ir::CmdBindIndexBuffer> index_buffer;
        std::optional<device_ir::ResourceBinding> sampled_texture;
        Viewport viewport;
        Scissor scissor;
        std::array<float, 4U> push_color{1.0F, 1.0F, 1.0F, 1.0F};
    };

    struct ClipVertex {
        std::array<float, 4U> position{};
        std::array<float, 2U> uv{0.5F, 0.5F};
    };

    [[nodiscard]] core::Result<void> preflight(
        const device_ir::DeviceCommandStream& stream) const;
    [[nodiscard]] core::Result<void> draw_indexed(
        const device_ir::CmdDrawIndexed& command,
        ExecutionState& state);
    [[nodiscard]] core::Result<void> rasterize_triangle(
        ExecutionState& state,
        const std::array<ClipVertex, 3U>& vertices);
    [[nodiscard]] std::array<float, 4U> sample_rgba32f(
        const TextureResource& texture,
        const gpu::SamplerDesc& sampler,
        float u,
        float v) const;

    CpuDeviceOptions options_;
    device::DeviceIdentity identity_;
    gpu::DeviceCapabilities capabilities_;
    gpu::Registry<gpu::BufferTag, BufferResource> buffers_;
    gpu::Registry<gpu::TextureTag, TextureResource> textures_;
    gpu::Registry<gpu::SamplerTag, SamplerResource> samplers_;
    gpu::Registry<gpu::ShaderTag, ShaderResource> shaders_;
    gpu::Registry<gpu::PipelineTag, PipelineResource> pipelines_;
    std::map<device_ir::ResourceHandle, device_ir::ResourceState> resource_states_;
    std::map<std::uint32_t, std::uint64_t> timestamps_;
    gpu::SubmissionSerial submitted_serial_ = 0U;
    gpu::SubmissionSerial completed_serial_ = 0U;
    gpu::SubmissionSerial signaled_serial_ = 0U;
};

} // namespace carto::device_cpu
