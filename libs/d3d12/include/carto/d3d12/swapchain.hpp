#pragma once

#include <carto/core/result.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/gpu/rhi.hpp>

#include <cstdint>
#include <memory>

namespace carto::d3d12 {

struct D3D12SwapchainOptions {
    std::uintptr_t window_handle = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t buffer_count = 2U;
    gpu::Format format = gpu::Format::bgra8_unorm;
    bool allow_tearing = false;
};

// Native presentation seam. Back buffers remain owned by DXGI; rendering
// resources and command submission remain owned by D3D12Device. This class
// deliberately exposes only lifecycle/present operations until a typed
// back-buffer import contract is added.
class D3D12Swapchain final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<D3D12Swapchain>> create(
        D3D12Device& device,
        D3D12SwapchainOptions options);
    ~D3D12Swapchain();

    D3D12Swapchain(const D3D12Swapchain&) = delete;
    D3D12Swapchain& operator=(const D3D12Swapchain&) = delete;

    [[nodiscard]] core::Result<void> resize(
        std::uint32_t width,
        std::uint32_t height);
    [[nodiscard]] core::Result<gpu::TextureHandle> acquire_back_buffer();
    [[nodiscard]] core::Result<void> present(bool vsync = true);
    [[nodiscard]] std::uint32_t current_back_buffer_index() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::uint32_t buffer_count() const noexcept;
    [[nodiscard]] gpu::Format format() const noexcept;

private:
    struct Impl;

    explicit D3D12Swapchain(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace carto::d3d12
