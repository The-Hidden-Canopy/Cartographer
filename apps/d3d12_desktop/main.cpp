#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <carto/d3d12/device.hpp>
#include <carto/d3d12/swapchain.hpp>
#include <carto/device_ir/command.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace {

constexpr wchar_t kWindowClass[] = L"CartographerD3D12Window";

struct WindowState {
    std::unique_ptr<carto::d3d12::D3D12Device> device;
    std::unique_ptr<carto::d3d12::D3D12Swapchain> swapchain;
};

WindowState* state(HWND window) noexcept {
    return reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* window_state = state(window);
    switch (message) {
    case WM_SIZE:
        if (window_state != nullptr && window_state->swapchain != nullptr &&
            w_param != SIZE_MINIMIZED) {
            const auto width = static_cast<std::uint32_t>(LOWORD(l_param));
            const auto height = static_cast<std::uint32_t>(HIWORD(l_param));
            static_cast<void>(window_state->swapchain->resize(width, height));
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (RegisterClassW(&window_class) == 0U) return 1;

    HWND window = CreateWindowExW(
        0U,
        kWindowClass,
        L"Cartographer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        800,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) return 1;

    auto device = carto::d3d12::D3D12Device::create();
    if (!device) {
        MessageBoxA(window, device.error().message.c_str(), "Cartographer D3D12", MB_ICONERROR);
        return 1;
    }
    WindowState window_state;
    window_state.device.reset(device.value().release());
    auto swapchain = carto::d3d12::D3D12Swapchain::create(
        *window_state.device,
        carto::d3d12::D3D12SwapchainOptions{
            reinterpret_cast<std::uintptr_t>(window), 1280U, 800U, 2U,
            carto::gpu::Format::bgra8_unorm, false});
    if (!swapchain) {
        MessageBoxA(window, swapchain.error().message.c_str(), "Cartographer D3D12", MB_ICONERROR);
        return 1;
    }
    window_state.swapchain.reset(swapchain.value().release());
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window_state));
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        const auto back_buffer = window_state.swapchain->acquire_back_buffer();
        if (!back_buffer) {
            MessageBoxA(window, back_buffer.error().message.c_str(),
                        "Cartographer D3D12", MB_ICONERROR);
            break;
        }
        carto::device_ir::DeviceCommandStream frame;
        frame.append(carto::device_ir::CmdTransitionResource{
            carto::device_ir::ResourceHandle{back_buffer.value()},
            carto::device_ir::ResourceState::present,
            carto::device_ir::ResourceState::render_target});
        frame.append(carto::device_ir::CmdBeginRendering{back_buffer.value(), std::nullopt});
        frame.append(carto::device_ir::CmdClearColor{
            back_buffer.value(), {0.035F, 0.045F, 0.065F, 1.0F}});
        frame.append(carto::device_ir::CmdEndRendering{});
        frame.append(carto::device_ir::CmdTransitionResource{
            carto::device_ir::ResourceHandle{back_buffer.value()},
            carto::device_ir::ResourceState::render_target,
            carto::device_ir::ResourceState::present});
        const auto serial = window_state.device->submit(frame);
        if (!serial) {
            static_cast<void>(window_state.device->destroy_texture(back_buffer.value()));
            MessageBoxA(window, serial.error().message.c_str(),
                        "Cartographer D3D12", MB_ICONERROR);
            break;
        }
        if (auto waited = window_state.device->wait(serial.value()); !waited) {
            static_cast<void>(window_state.device->destroy_texture(back_buffer.value()));
            MessageBoxA(window, waited.error().message.c_str(),
                        "Cartographer D3D12", MB_ICONERROR);
            break;
        }
        static_cast<void>(window_state.device->destroy_texture(back_buffer.value()));
        if (auto presented = window_state.swapchain->present(true); !presented) {
            MessageBoxA(window, presented.error().message.c_str(),
                        "Cartographer D3D12", MB_ICONERROR);
            break;
        }
    }
    return 0;
}
