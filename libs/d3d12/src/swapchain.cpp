#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <carto/d3d12/swapchain.hpp>
#include <carto/device_ir/command.hpp>

#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace carto::d3d12 {

namespace {

using Microsoft::WRL::ComPtr;
using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic native_failure(const char* operation, HRESULT status) {
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex
            << static_cast<unsigned long>(status);
    return Diagnostic(ErrorCode::invalid_state, message.str());
}

bool is_device_removed_hresult(HRESULT status) {
    return status == DXGI_ERROR_DEVICE_REMOVED || status == DXGI_ERROR_DEVICE_RESET ||
        status == DXGI_ERROR_DRIVER_INTERNAL_ERROR || status == DXGI_ERROR_DEVICE_HUNG;
}

void record_device_loss(D3D12Device& device, const char* operation, HRESULT status) {
    if (is_device_removed_hresult(status)) {
        device.record_external_device_loss(operation, static_cast<std::uint32_t>(status));
    }
}

core::Result<void> synchronize_graphics_queue(D3D12Device& device) {
    if (auto idle = device.wait_idle(); !idle) return idle;
    device_ir::DeviceCommandStream marker;
    const auto serial = device.submit(marker, gpu::QueueType::graphics);
    if (!serial) return core::Result<void>::failure(serial.error());
    return device.wait(serial.value());
}

DXGI_FORMAT native_format(gpu::Format format) {
    switch (format) {
    case gpu::Format::rgba8_unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case gpu::Format::rgba8_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case gpu::Format::bgra8_unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case gpu::Format::bgra8_srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace

struct D3D12Swapchain::Impl {
    D3D12Device* device = nullptr;
    D3D12SwapchainOptions options;
    ComPtr<IDXGISwapChain3> swapchain;
};

core::Result<std::unique_ptr<D3D12Swapchain>> D3D12Swapchain::create(
    D3D12Device& device,
    D3D12SwapchainOptions options) {
    if (options.window_handle == 0U || options.width == 0U || options.height == 0U ||
        options.buffer_count < 2U || options.buffer_count > 8U ||
        native_format(options.format) == DXGI_FORMAT_UNKNOWN) {
        return core::Result<std::unique_ptr<D3D12Swapchain>>::failure(invalid(
            "D3D12 swapchain requires a window, bounded extent, 2-8 buffers, and an 8-bit format"));
    }
    if (device.device_lost()) {
        return core::Result<std::unique_ptr<D3D12Swapchain>>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 swapchain cannot attach to a lost device"));
    }
    auto implementation = std::make_unique<Impl>();
    implementation->device = &device;
    implementation->options = options;

    ComPtr<IDXGIFactory6> factory;
    HRESULT status = CreateDXGIFactory2(0U, IID_PPV_ARGS(&factory));
    if (FAILED(status)) {
        record_device_loss(device, "CreateDXGIFactory2", status);
        return core::Result<std::unique_ptr<D3D12Swapchain>>::failure(
            native_failure("CreateDXGIFactory2", status));
    }
    auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(
        device.native_graphics_queue_handle());
    if (native_queue == nullptr) {
        return core::Result<std::unique_ptr<D3D12Swapchain>>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 swapchain has no native graphics queue"));
    }
    DXGI_SWAP_CHAIN_DESC1 descriptor{};
    descriptor.Width = options.width;
    descriptor.Height = options.height;
    descriptor.Format = native_format(options.format);
    descriptor.Stereo = FALSE;
    descriptor.SampleDesc.Count = 1U;
    descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    descriptor.BufferCount = options.buffer_count;
    descriptor.Scaling = DXGI_SCALING_STRETCH;
    descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    descriptor.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    descriptor.Flags = options.allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;

    ComPtr<IDXGISwapChain1> base_swapchain;
    status = factory->CreateSwapChainForHwnd(
        native_queue,
        reinterpret_cast<HWND>(options.window_handle),
        &descriptor,
        nullptr,
        nullptr,
        &base_swapchain);
    if (FAILED(status)) {
        record_device_loss(device, "IDXGIFactory2::CreateSwapChainForHwnd", status);
        return core::Result<std::unique_ptr<D3D12Swapchain>>::failure(
            native_failure("IDXGIFactory2::CreateSwapChainForHwnd", status));
    }
    static_cast<void>(factory->MakeWindowAssociation(
        reinterpret_cast<HWND>(options.window_handle), DXGI_MWA_NO_ALT_ENTER));
    status = base_swapchain.As(&implementation->swapchain);
    if (FAILED(status)) {
        record_device_loss(device, "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)", status);
        return core::Result<std::unique_ptr<D3D12Swapchain>>::failure(
            native_failure("IDXGISwapChain1::QueryInterface(IDXGISwapChain3)", status));
    }
    return core::Result<std::unique_ptr<D3D12Swapchain>>::success(
        std::unique_ptr<D3D12Swapchain>(new D3D12Swapchain(std::move(implementation))));
}

D3D12Swapchain::D3D12Swapchain(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
D3D12Swapchain::~D3D12Swapchain() {
    if (impl_ != nullptr && impl_->device != nullptr && !impl_->device->device_lost()) {
        static_cast<void>(synchronize_graphics_queue(*impl_->device));
    }
}

core::Result<void> D3D12Swapchain::resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U) {
        return core::Result<void>::failure(invalid(
            "D3D12 swapchain resize requires a non-zero extent"));
    }
    if (impl_->device->device_lost()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 swapchain cannot resize a lost device"));
    }
    if (auto synchronized = synchronize_graphics_queue(*impl_->device); !synchronized) {
        return synchronized;
    }
    const HRESULT status = impl_->swapchain->ResizeBuffers(
        impl_->options.buffer_count,
        width,
        height,
        native_format(impl_->options.format),
        impl_->options.allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U);
    if (FAILED(status)) {
        record_device_loss(*impl_->device, "IDXGISwapChain3::ResizeBuffers", status);
        return core::Result<void>::failure(
            native_failure("IDXGISwapChain3::ResizeBuffers", status));
    }
    impl_->options.width = width;
    impl_->options.height = height;
    return core::Result<void>::success();
}

core::Result<gpu::TextureHandle> D3D12Swapchain::acquire_back_buffer() {
    if (impl_->device->device_lost()) {
        return core::Result<gpu::TextureHandle>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 swapchain cannot acquire from a lost device"));
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    const HRESULT status = impl_->swapchain->GetBuffer(
        impl_->swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&resource));
    if (FAILED(status)) {
        record_device_loss(*impl_->device, "IDXGISwapChain3::GetBuffer", status);
        return core::Result<gpu::TextureHandle>::failure(
            native_failure("IDXGISwapChain3::GetBuffer", status));
    }
    return impl_->device->import_swapchain_texture(
        resource.Get(),
        gpu::TextureDesc{
            impl_->options.width,
            impl_->options.height,
            1U,
            1U,
            1U,
            impl_->options.format,
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::color_attachment,
        });
}

core::Result<void> D3D12Swapchain::present(bool vsync) {
    if (impl_->device->device_lost()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 swapchain cannot present on a lost device"));
    }
    const UINT flags = !vsync && impl_->options.allow_tearing
        ? DXGI_PRESENT_ALLOW_TEARING : 0U;
    const HRESULT status = impl_->swapchain->Present(vsync ? 1U : 0U, flags);
    if (FAILED(status)) {
        record_device_loss(*impl_->device, "IDXGISwapChain3::Present", status);
        return core::Result<void>::failure(native_failure("IDXGISwapChain3::Present", status));
    }
    return core::Result<void>::success();
}

std::uint32_t D3D12Swapchain::current_back_buffer_index() const noexcept {
    return impl_->swapchain->GetCurrentBackBufferIndex();
}

std::uint32_t D3D12Swapchain::width() const noexcept { return impl_->options.width; }
std::uint32_t D3D12Swapchain::height() const noexcept { return impl_->options.height; }
std::uint32_t D3D12Swapchain::buffer_count() const noexcept {
    return impl_->options.buffer_count;
}
gpu::Format D3D12Swapchain::format() const noexcept { return impl_->options.format; }

} // namespace carto::d3d12
