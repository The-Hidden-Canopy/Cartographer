#pragma once

#include <carto/device/device.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace carto::d3d12 {

struct D3D12DeviceOptions {
    std::string adapter_id;
    bool allow_software_adapter = false;
    bool enable_debug_layer = false;
};

struct ShaderSource {
    std::string debug_name;
    gpu::ShaderStage stage = gpu::ShaderStage::none;
    std::string entry_point = "main";
    std::string source_name = "cartographer.hlsl";
    std::string source;
    bool optimize = true;
    bool debug_info = false;
};

// Compiles HLSL through the installed DirectX Shader Compiler runtime. The
// compiler is loaded dynamically so the public D3D12 target does not acquire a
// hard runtime dependency on a workstation SDK installation.
[[nodiscard]] core::Result<device::ShaderBinary> compile_hlsl(
    const ShaderSource& source);

class D3D12Device final : public device::Device {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<D3D12Device>> create(
        D3D12DeviceOptions options = {});
    ~D3D12Device() override;

    D3D12Device(const D3D12Device&) = delete;
    D3D12Device& operator=(const D3D12Device&) = delete;

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

    // Waits for every queue submission currently owned by this device. This
    // is required before native swapchain resize/replacement, where DXGI
    // rejects outstanding references to its back buffers.
    [[nodiscard]] core::Result<void> wait_idle();

    // Adopts a borrowed DXGI back-buffer resource into the generational
    // texture registry. The device takes its own COM reference; the caller
    // remains responsible for transitioning the returned texture between
    // present and render_target states and destroying the handle after use.
    [[nodiscard]] core::Result<gpu::TextureHandle> import_swapchain_texture(
        void* native_resource,
        const gpu::TextureDesc& descriptor);

    [[nodiscard]] core::Result<gpu::SubmissionSerial> submit(
        const device_ir::DeviceCommandStream& stream,
        gpu::QueueType queue = gpu::QueueType::graphics) override;
    [[nodiscard]] gpu::SubmissionSerial completed_serial() const noexcept override;
    [[nodiscard]] core::Result<void> wait(gpu::SubmissionSerial serial) override;

    // Debug-layer and device-removal evidence is retained as bounded textual
    // receipts. Recovery is explicit: it creates a fresh device and does not
    // silently claim that GPU resources survived removal.
    [[nodiscard]] const std::vector<std::string>& debug_receipts() const noexcept;
    [[nodiscard]] bool device_lost() const noexcept;
    void record_external_device_loss(
        const char* operation,
        std::uint32_t native_status);
    [[nodiscard]] core::Result<std::unique_ptr<D3D12Device>> recover() const;

    // Opaque bridge for optional native presentation code. The returned
    // interfaces are borrowed and remain valid only while this device lives.
    [[nodiscard]] void* native_device_handle() const noexcept;
    [[nodiscard]] void* native_graphics_queue_handle() const noexcept;

private:
    struct Impl;

    explicit D3D12Device(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace carto::d3d12
