#include <carto/d3d12/device.hpp>
#include <carto/device_ir/command.hpp>

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <iostream>
#include <vector>

int main() {
    using namespace carto;
    const auto zero = d3d12::D3D12Device::create({{}, true, false, std::chrono::milliseconds(0)});
    const auto excessive = d3d12::D3D12Device::create({{}, true, false, std::chrono::minutes(11)});
    if (zero || excessive || zero.error().code != core::ErrorCode::invalid_argument ||
        excessive.error().code != core::ErrorCode::invalid_argument) return 1;
    auto created = d3d12::D3D12Device::create({{}, true, false, std::chrono::seconds(5)});
    if (!created) {
        std::cout << "SKIP: D3D12 device unavailable: " << created.error().message << '\n';
        return 77;
    }
    auto& device = *created.value();
    auto* native = static_cast<ID3D12Device*>(device.native_device_handle());
    auto* queue = static_cast<ID3D12CommandQueue*>(device.native_graphics_queue_handle());
    Microsoft::WRL::ComPtr<ID3D12Fence> blocker;
    if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)))) return 1;
    struct ReleaseFence {
        ID3D12Fence* fence;
        ~ReleaseFence() { if (fence) static_cast<void>(fence->Signal(1)); }
    } release{blocker.Get()};
    if (FAILED(queue->Wait(blocker.Get(), 1))) return 1;

    const auto upload = device.create_buffer({64U, gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
    const auto readback = device.create_buffer({64U, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination});
    const std::vector<std::uint8_t> expected{9U, 8U, 7U, 6U};
    if (!upload || !readback || !device.write_buffer(upload.value(), 0U, expected)) return 1;
    device_ir::DeviceCommandStream copy;
    copy.append(device_ir::CmdTransitionResource{{upload.value()}, device_ir::ResourceState::undefined,
                                                  device_ir::ResourceState::copy_source});
    copy.append(device_ir::CmdTransitionResource{{readback.value()}, device_ir::ResourceState::undefined,
                                                  device_ir::ResourceState::copy_destination});
    copy.append(device_ir::CmdCopyBuffer{upload.value(), readback.value(), 0U, 0U, expected.size()});
    device_ir::DeviceCommandStream empty;
    const auto first = device.submit(copy, gpu::QueueType::graphics);
    const auto second = device.submit(empty, gpu::QueueType::graphics);
    const auto third = device.submit(empty, gpu::QueueType::graphics);
    if (!first || !second || !third) return 1;
    if (!device.destroy_buffer(upload.value())) return 1;
    const auto start = std::chrono::steady_clock::now();
    const auto stalled_wait = device.wait(first.value());
    const auto wait_elapsed = std::chrono::steady_clock::now() - start;
    if (stalled_wait || wait_elapsed < std::chrono::seconds(4) ||
        wait_elapsed > std::chrono::seconds(8)) {
        std::cerr << "pending device wait did not time out within its expected bound\n";
        return 1;
    }
    const auto context_start = std::chrono::steady_clock::now();
    const auto stalled_submit = device.submit(empty, gpu::QueueType::graphics);
    const auto context_elapsed = std::chrono::steady_clock::now() - context_start;
    if (stalled_submit || context_elapsed < std::chrono::seconds(4) ||
        context_elapsed > std::chrono::seconds(8)) {
        std::cerr << "pending allocator was not preserved behind a bounded wait\n";
        return 1;
    }
    if (FAILED(blocker->Signal(1))) return 1;
    release.fence = nullptr;
    if (!device.wait(third.value())) return 1;
    const auto copied = device.read_buffer(readback.value(), 0U, expected.size());
    if (!copied || copied.value() != expected || !device.destroy_buffer(readback.value())) {
        std::cerr << "in-flight upload did not survive deferred retirement\n";
        return 1;
    }
    const auto resumed = device.submit(empty, gpu::QueueType::graphics);
    if (!resumed || !device.wait(resumed.value()) || !device.wait_idle()) return 1;
    std::cout << "D3D12 " << (device.is_software_adapter() ? "software" : "hardware")
              << " queue survived timeout and resumed after confirmed retirement\n";
    return 0;
}
