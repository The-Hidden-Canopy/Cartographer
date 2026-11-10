#define CARTO_D3D12_SHELL_TESTING 1
#include "../apps/desktop/carto_d3d12_shell.hpp"

#include <chrono>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

struct TestThumbnail {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba8;
    carto::assets::Sha256Digest pixel_digest;
};
static_assert(requires(carto::desktop::D3D12Shell& shell, const carto::assets::DecodedRgba8Texture& image,
                       const TestThumbnail& thumbnail, const carto::assets::Sha256Digest& digest,
                       std::span<const std::uint8_t> pixels, std::string& error) {
    { shell.set_reference_image_texture(image, error) } -> std::same_as<carto::desktop::D3D12Shell::TextureUploadResult>;
    { shell.set_scene_capture_texture(1, 1, pixels, digest, error) } -> std::same_as<carto::desktop::D3D12Shell::TextureUploadResult>;
    { shell.set_source_reimport_thumbnail_texture(0, thumbnail, error) } -> std::same_as<carto::desktop::D3D12Shell::TextureUploadResult>;
    { shell.scene_capture_texture_id() } -> std::same_as<ImTextureID>;
    { shell.source_reimport_thumbnail_texture_id(0) } -> std::same_as<ImTextureID>;
    shell.clear_scene_capture_texture(false);
    shell.clear_source_reimport_thumbnail_texture(0);
    shell.clear_authoring_viewport_texture();
    shell.cancel_authoring_viewport_upload();
    { shell.authoring_viewport_format_matches() } -> std::same_as<bool>;
});

// No ImGui context or window is created in this headless ownership test.
void ImGui_ImplDX12_Shutdown() {}
void ImGui_ImplDX12_NewFrame() { std::abort(); }
void ImGui_ImplDX12_RenderDrawData(ImDrawData*, ID3D12GraphicsCommandList*) { std::abort(); }
bool ImGui_ImplDX12_HasUncertainRetirement() { return false; }
bool ImGui_ImplDX12_PreparePlatformShutdown() { return true; }
namespace ImGui {
ImGuiContext* GetCurrentContext() { return nullptr; }
ImGuiPlatformIO& GetPlatformIO() { std::abort(); }
}
carto::core::Result<carto::d3d12::ResidentGpuTiming> carto::d3d12::ResidentTimingReceipt::resolve() const {
    return carto::core::Result<carto::d3d12::ResidentGpuTiming>::failure(
        {carto::core::ErrorCode::invalid_state, "timing receipt is not used by this test"});
}

namespace carto::desktop {
struct D3D12ShellTestAccess {
    static bool attach(D3D12Shell& shell, const std::shared_ptr<d3d12::D3D12Device>& device) {
        shell.device_ = device;
        shell.native_ = static_cast<ID3D12Device*>(device->native_device_handle());
        shell.queue_ = static_cast<ID3D12CommandQueue*>(device->native_graphics_queue_handle());
        if (FAILED(shell.native_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&shell.fence_))))
            return false;
        shell.event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return shell.event_ != nullptr;
    }
    static void mark_present_pending(D3D12Shell& shell) { shell.present_pending_ = true; }
    static std::uint64_t shutdown_marker(const D3D12Shell& shell) { return shell.shutdown_marker_; }
    static bool present_pending(const D3D12Shell& shell) { return shell.present_pending_; }
    static bool begin_texture_frame(D3D12Shell& shell, const std::shared_ptr<const void>& lease) {
        if (FAILED(shell.native_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&shell.texture_fence_))) ||
            !shell.retired_textures_->begin_frame()) return false;
        shell.texture_frame_open_ = true;
        shell.reference_.emplace();
        shell.reference_->descriptor = 4U;
        shell.reference_->lease = lease;
        shell.descriptor_pool_->used[4U] = true;
        return true;
    }
    static void retire_reference(D3D12Shell& shell) { shell.retire_texture(shell.reference_); }
    static void collect(D3D12Shell& shell) { shell.collect_retired_textures(); }
    static bool descriptor_used(const D3D12Shell& shell) { return shell.descriptor_pool_->used[4U]; }
};
} // namespace carto::desktop

int main() {
    using namespace carto;
    auto created = d3d12::D3D12Device::create({{}, true, false, std::chrono::seconds(5)});
    if (!created) {
        std::cout << "SKIP: D3D12 device unavailable: " << created.error().message << '\n';
        return 77;
    }
    auto device = std::shared_ptr<d3d12::D3D12Device>(std::move(created.value()));
    std::weak_ptr<d3d12::D3D12Device> retained = device;
    auto* native = static_cast<ID3D12Device*>(device->native_device_handle());
    auto* queue = static_cast<ID3D12CommandQueue*>(device->native_graphics_queue_handle());
    Microsoft::WRL::ComPtr<ID3D12Fence> blocker;
    if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)))) return 1;
    struct ReleaseFence {
        ID3D12Fence* fence;
        ~ReleaseFence() { if (fence) static_cast<void>(fence->Signal(1)); }
    } release{blocker.Get()};
    if (FAILED(queue->Wait(blocker.Get(), 1))) return 1;
    bool rejected = false;
    {
        desktop::D3D12Shell shell;
        if (!desktop::D3D12ShellTestAccess::attach(shell, device)) return 1;
        device.reset();
        try { shell.shutdown(); }
        catch (const std::runtime_error&) { rejected = true; }
    }
    if (!rejected || retained.expired()) {
        std::cerr << "unconfirmed shell shutdown did not retain its device ownership\n";
        return 1;
    }
    if (FAILED(blocker->Signal(1))) return 1;
    release.fence = nullptr;
    auto resumed_device = retained.lock();
    if (!resumed_device) return 1;
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> texture_blocker;
        if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&texture_blocker))) ||
            FAILED(queue->Wait(texture_blocker.Get(), 1))) return 1;
        struct ReleaseTextureFence {
            ID3D12Fence* fence;
            ~ReleaseTextureFence() { if (fence) static_cast<void>(fence->Signal(1)); }
        } release_texture{texture_blocker.Get()};
        desktop::D3D12Shell shell;
        if (!desktop::D3D12ShellTestAccess::attach(shell, resumed_device)) return 1;
        auto lease = std::make_shared<int>(42);
        std::weak_ptr<int> lease_weak = lease;
        if (!desktop::D3D12ShellTestAccess::begin_texture_frame(shell, lease)) return 1;
        lease.reset();
        desktop::D3D12ShellTestAccess::retire_reference(shell);
        shell.finish_host_frame();
        desktop::D3D12ShellTestAccess::collect(shell);
        if (lease_weak.expired() || !desktop::D3D12ShellTestAccess::descriptor_used(shell)) return 1;
        if (FAILED(texture_blocker->Signal(1))) return 1;
        release_texture.fence = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            desktop::D3D12ShellTestAccess::collect(shell);
            if (!lease_weak.expired()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!lease_weak.expired() && std::chrono::steady_clock::now() < deadline);
        if (!lease_weak.expired() || desktop::D3D12ShellTestAccess::descriptor_used(shell)) return 1;
        shell.shutdown();
    }
    {
        desktop::D3D12Shell shell;
        if (!desktop::D3D12ShellTestAccess::attach(shell, resumed_device)) return 1;
        bool ready = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            ready = shell.prepare_shutdown();
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!ready && std::chrono::steady_clock::now() < deadline);
        if (!ready || shell.new_frame() || shell.render(nullptr)) return 1;
        shell.shutdown();
    }
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> present_blocker;
        if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_blocker))) ||
            FAILED(queue->Wait(present_blocker.Get(), 1))) return 1;
        ReleaseFence release_present{present_blocker.Get()};
        desktop::D3D12Shell shell;
        if (!desktop::D3D12ShellTestAccess::attach(shell, resumed_device)) return 1;
        desktop::D3D12ShellTestAccess::mark_present_pending(shell);
        if (shell.prepare_shutdown()) return 1;
        const auto marker = desktop::D3D12ShellTestAccess::shutdown_marker(shell);
        if (marker == 0 || shell.prepare_shutdown() ||
            desktop::D3D12ShellTestAccess::shutdown_marker(shell) != marker ||
            !desktop::D3D12ShellTestAccess::present_pending(shell)) return 1;
        if (FAILED(present_blocker->Signal(1))) return 1;
        release_present.fence = nullptr;
        bool ready = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            ready = shell.prepare_shutdown();
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!ready && std::chrono::steady_clock::now() < deadline);
        if (!ready) return 1;
        shell.shutdown();
    }
    std::cout << "shell retained native ownership after an unconfirmed queue retirement\n";
    return 0;
}
