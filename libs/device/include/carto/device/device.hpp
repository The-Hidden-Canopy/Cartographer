#pragma once

#include <carto/core/result.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/gpu/lifetime.hpp>
#include <carto/gpu/rhi.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::device {

enum class BackendKind {
    cpu,
    d3d12,
    vulkan,
};

struct DeviceIdentity {
    BackendKind backend = BackendKind::cpu;
    std::string adapter_name;
    std::uint32_t vendor_id = 0U;
    std::uint32_t device_id = 0U;
    std::string stable_adapter_id;
    std::uint64_t local_memory_budget = 0U;
    std::string feature_level;
    std::string driver;
};

enum class BinaryFormat {
    dxil,
    spirv,
    cpu_ir,
};

struct ShaderBinary {
    std::string debug_name;
    gpu::ShaderStage stage = gpu::ShaderStage::none;
    std::string entry_point = "main";
    BinaryFormat format = BinaryFormat::cpu_ir;
    std::string source_digest;
    // SHA-256 identity of the compiler invocation recipe. This is deliberately
    // not presented as a hash of a compiler executable unless a backend can
    // provide that stronger identity.
    std::string compiler_digest;
    std::vector<std::uint8_t> bytes;
    // SHA-256 identity of the compiled binary when one exists. CPU reference
    // shaders may leave this empty because their bytecode is an explicit
    // local reference representation rather than a portable GPU artifact.
    std::string binary_digest;
};

[[nodiscard]] core::Result<void> validate(const ShaderBinary& binary);

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual BackendKind backend() const noexcept = 0;
    [[nodiscard]] virtual const DeviceIdentity& identity() const noexcept = 0;
    [[nodiscard]] virtual const gpu::DeviceCapabilities& capabilities() const noexcept = 0;

    [[nodiscard]] virtual core::Result<gpu::BufferHandle> create_buffer(
        const gpu::BufferDesc& descriptor) = 0;
    [[nodiscard]] virtual core::Result<gpu::TextureHandle> create_texture(
        const gpu::TextureDesc& descriptor) = 0;
    [[nodiscard]] virtual core::Result<gpu::SamplerHandle> create_sampler(
        const gpu::SamplerDesc& descriptor) = 0;
    [[nodiscard]] virtual core::Result<gpu::ShaderHandle> create_shader(
        const ShaderBinary& binary) = 0;
    [[nodiscard]] virtual core::Result<gpu::PipelineHandle> create_pipeline(
        const gpu::PipelineDesc& descriptor) = 0;

    [[nodiscard]] virtual core::Result<void> destroy_buffer(gpu::BufferHandle handle) = 0;
    [[nodiscard]] virtual core::Result<void> destroy_texture(gpu::TextureHandle handle) = 0;
    [[nodiscard]] virtual core::Result<void> destroy_sampler(gpu::SamplerHandle handle) = 0;
    [[nodiscard]] virtual core::Result<void> destroy_shader(gpu::ShaderHandle handle) = 0;
    [[nodiscard]] virtual core::Result<void> destroy_pipeline(gpu::PipelineHandle handle) = 0;

    [[nodiscard]] virtual core::Result<void> write_buffer(
        gpu::BufferHandle handle,
        std::uint64_t offset,
        std::span<const std::uint8_t> bytes) = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::uint8_t>> read_buffer(
        gpu::BufferHandle handle,
        std::uint64_t offset,
        std::uint64_t bytes) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<float>> read_texture_rgba32f(
        gpu::TextureHandle handle) const = 0;

    [[nodiscard]] virtual core::Result<gpu::SubmissionSerial> submit(
        const device_ir::DeviceCommandStream& stream,
        gpu::QueueType queue = gpu::QueueType::graphics) = 0;
    [[nodiscard]] virtual gpu::SubmissionSerial completed_serial() const noexcept = 0;
    [[nodiscard]] virtual core::Result<void> wait(gpu::SubmissionSerial serial) = 0;
};

} // namespace carto::device
