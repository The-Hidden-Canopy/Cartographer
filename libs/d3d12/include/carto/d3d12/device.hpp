#pragma once

#include <carto/device/device.hpp>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <memory>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace carto::d3d12 {

namespace detail {
enum class ContextFenceAdmission { ready, pending, invalid_completion };
[[nodiscard]] constexpr ContextFenceAdmission context_fence_admission(
    gpu::SubmissionSerial submitted, gpu::SubmissionSerial completed) noexcept {
    if (completed == std::numeric_limits<gpu::SubmissionSerial>::max())
        return ContextFenceAdmission::invalid_completion;
    return submitted == 0 || completed >= submitted ? ContextFenceAdmission::ready
                                                     : ContextFenceAdmission::pending;
}
// Bookkeeping for one executed submission whose queue signal is unconfirmed.
// This state does not itself submit, synchronize, or own GPU resources.
class UnsignaledExecution final {
public:
    [[nodiscard]] bool active() const noexcept { return serial_ != 0; }
    [[nodiscard]] bool marker_confirmed() const noexcept { return marker_confirmed_; }
    [[nodiscard]] gpu::SubmissionSerial serial() const noexcept { return serial_; }
    [[nodiscard]] gpu::QueueType queue() const noexcept { return queue_; }
    [[nodiscard]] std::size_t context() const noexcept { return context_; }
    [[nodiscard]] bool arm(gpu::SubmissionSerial serial, gpu::QueueType queue,
                           std::size_t context) noexcept {
        if (active() || serial == 0) return false;
        serial_ = serial; queue_ = queue; context_ = context;
        marker_confirmed_ = false; return true;
    }
    [[nodiscard]] bool recovered(gpu::SubmissionSerial serial, gpu::QueueType queue) noexcept {
        if (!active() || marker_confirmed_ || queue != queue_ || serial <= serial_ ||
            serial == std::numeric_limits<gpu::SubmissionSerial>::max()) return false;
        serial_ = serial; marker_confirmed_ = true; return true;
    }
    [[nodiscard]] bool observe_completion(gpu::SubmissionSerial completed) noexcept {
        if (!active() || !marker_confirmed_ ||
            completed == std::numeric_limits<gpu::SubmissionSerial>::max() || completed < serial_)
            return false;
        clear(); return true;
    }
    // Called only after the original submission's own Signal succeeded.
    void original_signal_confirmed() noexcept { clear(); }
private:
    void clear() noexcept { serial_ = 0; marker_confirmed_ = false; }
    gpu::SubmissionSerial serial_ = 0;
    gpu::QueueType queue_ = gpu::QueueType::graphics;
    std::size_t context_ = 0;
    bool marker_confirmed_ = false;
};
} // namespace detail

struct ReadbackHostTiming {
    double map_ms = 0, allocation_ms = 0, copy_ms = 0, unmap_ms = 0;
};

struct QueueClockCalibration {
    std::uint64_t gpu_ticks = 0;
    std::chrono::steady_clock::time_point host_epoch;
    double uncertainty_ms = 0;
};
struct SubmissionHostTiming {
    double validated = 0, context_ready = 0, encoded = 0, returned = 0;
    double context_wait_ms = 0;
};

struct D3D12DeviceOptions {
    std::string adapter_id;
    bool allow_software_adapter = false;
    bool enable_debug_layer = false;
    // Bounds each synchronous device wait, including the complete wait_idle
    // operation. Pending work remains owned when this deadline expires.
    std::chrono::milliseconds fence_wait_timeout = std::chrono::seconds(60);
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
// CPU-only compiler/cache support; these never create a device or queue.
[[nodiscard]] core::Result<std::string> dxc_runtime_fingerprint();
[[nodiscard]] core::Result<void> validate_dxil(std::span<const std::uint8_t> bytes);

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
    [[nodiscard]] bool is_software_adapter() const noexcept;

    [[nodiscard]] core::Result<gpu::BufferHandle> create_buffer(
        const gpu::BufferDesc& descriptor) override;
    [[nodiscard]] core::Result<gpu::TextureHandle> create_texture(
        const gpu::TextureDesc& descriptor) override;
    [[nodiscard]] core::Result<gpu::TextureDesc> describe_texture(
        gpu::TextureHandle handle) const;
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
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> read_buffer_timed(
        gpu::BufferHandle handle, std::uint64_t offset, std::uint64_t bytes,
        ReadbackHostTiming& timing) const;
    // Borrows read-only mapped bytes for this callback only. The caller must
    // have waited for the writing submission. Do not retain the span, mutate
    // resources, or submit device work from the callback. Unmaps on failure.
    [[nodiscard]] core::Result<void> visit_readback(
        gpu::BufferHandle handle, std::uint64_t offset, std::uint64_t bytes,
        const std::function<core::Result<void>(std::span<const std::uint8_t>)>& visitor,
        ReadbackHostTiming& timing) const;
    [[nodiscard]] core::Result<std::vector<float>> read_texture_rgba32f(
        gpu::TextureHandle handle) const override;
    [[nodiscard]] core::Result<std::vector<float>> read_texture_depth32f(
        gpu::TextureHandle handle) const;

    // Waits for every queue submission currently owned by this device. This
    // is required before native swapchain resize/replacement, where DXGI
    // rejects outstanding references to its back buffers.
    [[nodiscard]] core::Result<void> wait_idle();

    // If submit executed but its queue signal failed, all device-owned
    // resources are conservatively quarantined. This attempts a later marker
    // on that same queue; a successful return means the marker was submitted,
    // not that it has completed. Caller must still wait for queue retirement.
    // This tracks device-owned submissions only, not work submitted directly
    // through borrowed native handles or any presentation/ImGui retirement.
    // Calls on a device, including use of borrowed handles, must be externally
    // serialized; the device does not provide concurrent submission locking.
    [[nodiscard]] core::Result<void> resolve_unsignaled_execution();

    // Adopts a borrowed DXGI back-buffer resource into the generational
    // texture registry. The device takes its own COM reference; the caller
    // remains responsible for transitioning the returned texture from present
    // into a supported use state (render_target or copy_destination), back to
    // present, and destroying the handle after use.
    [[nodiscard]] core::Result<gpu::TextureHandle> import_swapchain_texture(
        void* native_resource,
        const gpu::TextureDesc& descriptor);

    [[nodiscard]] core::Result<gpu::SubmissionSerial> submit(
        const device_ir::DeviceCommandStream& stream,
        gpu::QueueType queue = gpu::QueueType::graphics) override;
    // Empty success means the next command context is still in flight: no
    // context was consumed and no stream validation or encoding occurred.
    // Nonempty success is the usual submission serial. Other errors retain
    // their usual semantics, including quarantine after an uncertain Signal.
    // Device and borrowed native-handle operations must be externally
    // serialized. This avoids the context fence wait only; D3D12 driver calls
    // and normal encoding/submission are not guaranteed nonblocking.
    [[nodiscard]] core::Result<std::optional<gpu::SubmissionSerial>> try_submit(
        const device_ir::DeviceCommandStream& stream,
        gpu::QueueType queue = gpu::QueueType::graphics);
    // Highest serial returned by a successful submit on this device.
    [[nodiscard]] gpu::SubmissionSerial last_submitted_serial() const noexcept;
    [[nodiscard]] gpu::SubmissionSerial completed_serial() const noexcept override;
    [[nodiscard]] core::Result<gpu::SubmissionSerial> submit_timed(
        const device_ir::DeviceCommandStream& stream, SubmissionHostTiming& timing,
        gpu::QueueType queue = gpu::QueueType::graphics);
    [[nodiscard]] core::Result<void> wait(gpu::SubmissionSerial serial) override;

    // Returns the native timestamp frequency for a queue, or zero when the
    // adapter does not expose timestamp queries for that queue.
    [[nodiscard]] std::uint64_t timestamp_frequency(
        gpu::QueueType queue = gpu::QueueType::graphics) const noexcept;
    [[nodiscard]] core::Result<QueueClockCalibration> clock_calibration() const;

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
    // Borrowed resource for same-device presentation. Keep its owning target
    // lease alive and retain the declared resource state; this grants no
    // cross-device/shared-handle or synchronization authority.
    [[nodiscard]] core::Result<void*> native_texture_handle(gpu::TextureHandle texture) const;

private:
    struct Impl;

    [[nodiscard]] core::Result<std::vector<float>> read_texture_float32(
        gpu::TextureHandle handle,
        gpu::Format expected_format,
        std::uint32_t channel_count) const;

    explicit D3D12Device(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace carto::d3d12
