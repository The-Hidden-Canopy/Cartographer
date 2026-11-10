#include <carto/d3d12/device.hpp>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

bool ImGui_ImplDX12_TestWindowRetirement(ID3D12CommandQueue* queue, bool present_pending);
bool ImGui_ImplDX12_TestPlatformShutdown(ID3D12CommandQueue* queue, ID3D12Fence* blocker);

struct DescriptorFixture {
    ID3D12DescriptorHeap* heap = nullptr;
    unsigned allocations = 0, frees = 0;
};

int main(int argc, char** argv) {
    using namespace carto;
    auto created = d3d12::D3D12Device::create({{}, true, false, std::chrono::seconds(5)});
    if (!created) {
        std::cout << "SKIP: D3D12 device unavailable: " << created.error().message << '\n';
        return 77;
    }
    auto& device = *created.value();
    std::cout << "D3D12 adapter: " << (device.is_software_adapter() ? "software" : "hardware") << '\n';
    auto* native = static_cast<ID3D12Device*>(device.native_device_handle());
    auto* queue = static_cast<ID3D12CommandQueue*>(device.native_graphics_queue_handle());
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(native->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
        std::cerr << "descriptor heap creation failed\n"; return 1;
    }
    DescriptorFixture fixture{heap.Get()};
    ImGui::CreateContext();
    ImGui_ImplDX12_InitInfo info;
    info.Device = native; info.CommandQueue = queue; info.NumFramesInFlight = 3;
    info.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM; info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = heap.Get(); info.UserData = &fixture;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* data, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        auto& value = *static_cast<DescriptorFixture*>(data->UserData);
        ++value.allocations; *cpu = value.heap->GetCPUDescriptorHandleForHeapStart();
        *gpu = value.heap->GetGPUDescriptorHandleForHeapStart();
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* data, D3D12_CPU_DESCRIPTOR_HANDLE,
                                  D3D12_GPU_DESCRIPTOR_HANDLE) {
        ++static_cast<DescriptorFixture*>(data->UserData)->frees;
    };
    if (!ImGui_ImplDX12_Init(&info)) { std::cerr << "backend initialization failed\n"; return 1; }
    if (!ImGui_ImplDX12_CreateDeviceObjects()) { std::cerr << "backend device objects failed\n"; return 1; }

    if (argc == 2) {
        const bool presentation = std::string_view(argv[1]) == "presentation";
        const bool platform = std::string_view(argv[1]) == "platform";
        if (!presentation && !platform && std::string_view(argv[1]) != "window") return 1;
        if (presentation) {
            if (!ImGui_ImplDX12_TestWindowRetirement(queue, true) || ImGui_ImplDX12_HasUncertainRetirement()) return 1;
            ImGui_ImplDX12_Shutdown();
            ImGui::DestroyContext();
            std::cout << "post-present queue marker retired application-owned work\n";
            return 0;
        }
        Microsoft::WRL::ComPtr<ID3D12Fence> gate;
        if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))) return 1;
        struct ReleaseGate {
            ID3D12Fence* fence;
            ~ReleaseGate() { if (fence) static_cast<void>(fence->Signal(1)); }
        } release_gate{gate.Get()};
        if (FAILED(queue->Wait(gate.Get(), 1))) return 1;
        if (platform) {
            const bool ready = ImGui_ImplDX12_TestPlatformShutdown(queue, gate.Get());
            if (!ready || ImGui_ImplDX12_HasUncertainRetirement()) return 1;
            release_gate.fence = nullptr;
            ImGui_ImplDX12_Shutdown();
            ImGui::DestroyContext();
            std::cout << "platform shutdown waited for its existing post-present queue marker\n";
            return 0;
        }
        bool rejected = false;
        const auto start = std::chrono::steady_clock::now();
        try { static_cast<void>(ImGui_ImplDX12_TestWindowRetirement(queue, presentation)); }
        catch (const std::runtime_error&) { rejected = true; }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (!rejected || !ImGui_ImplDX12_HasUncertainRetirement() ||
            (!presentation && (elapsed < std::chrono::seconds(4) || elapsed > std::chrono::seconds(8))))
            return 1;
        bool shutdown_refused = false;
        try { ImGui_ImplDX12_Shutdown(); }
        catch (const std::runtime_error&) { shutdown_refused = true; }
        if (!shutdown_refused) return 1;
        if (FAILED(gate->Signal(1))) return 1;
        release_gate.fence = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Fence> drained;
        if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drained))) ||
            FAILED(queue->Signal(drained.Get(), 1))) return 1;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return 1;
        const HRESULT registered = drained->SetEventOnCompletion(1, event);
        const DWORD waited = SUCCEEDED(registered) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
        CloseHandle(event);
        if (waited != WAIT_OBJECT_0 || drained->GetCompletedValue() < 1) return 1;
        std::cout << "window retirement timeout retained\n";
        return 0;
    }

    ImTextureData ready;
    ready.Create(ImTextureFormat_RGBA32, 2, 2);
    ImGui_ImplDX12_UpdateTexture(&ready);
    if (ready.Status != ImTextureStatus_OK) { std::cerr << "normal upload did not finish\n"; return 1; }
    ready.UnusedFrames = 4;
    ready.SetStatus(ImTextureStatus_WantDestroy);
    ImGui_ImplDX12_UpdateTexture(&ready);
    if (ready.BackendUserData != nullptr || fixture.frees != 1) {
        std::cerr << "normal texture destroy failed: status=" << int(ready.Status) << " frees=" << fixture.frees << '\n'; return 1;
    }

    Microsoft::WRL::ComPtr<ID3D12Fence> blocker;
    if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)))) return 1;
    struct ReleaseFence {
        ID3D12Fence* fence;
        ~ReleaseFence() { if (fence) static_cast<void>(fence->Signal(1)); }
    } release{blocker.Get()};
    if (FAILED(queue->Wait(blocker.Get(), 1))) return 1;
    ImTextureData pending;
    pending.Create(ImTextureFormat_RGBA32, 2, 2);
    bool timed_out = false;
    const auto start = std::chrono::steady_clock::now();
    try { ImGui_ImplDX12_UpdateTexture(&pending); }
    catch (const std::runtime_error&) { timed_out = true; }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!timed_out || elapsed < std::chrono::seconds(4) || elapsed > std::chrono::seconds(8) ||
        pending.Status == ImTextureStatus_OK || !pending.BackendUserData ||
        !ImGui_ImplDX12_HasUncertainRetirement() || fixture.frees != 1) return 1;
    bool shutdown_refused = false;
    try { ImGui_ImplDX12_Shutdown(); }
    catch (const std::runtime_error&) { shutdown_refused = true; }
    if (!shutdown_refused || fixture.frees != 1) return 1;

    if (FAILED(blocker->Signal(1))) return 1;
    release.fence = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> drained;
    if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drained))) ||
        FAILED(queue->Signal(drained.Get(), 1))) return 1;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 1;
    const HRESULT registered = drained->SetEventOnCompletion(1, event);
    const DWORD waited = SUCCEEDED(registered) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0 || drained->GetCompletedValue() < 1) return 1;
    std::cout << "ImGui texture upload retained its descriptor and refused unsafe shutdown after timeout\n";
    return 0;
}
