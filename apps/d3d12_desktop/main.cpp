#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>

#include <carto/d3d12/device.hpp>
#include <carto/d3d12/swapchain.hpp>
#include <carto/d3d12/viewport_input.hpp>
#include <carto/device_ir/command.hpp>

#include <array>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"CartographerD3D12Window";

struct WindowState {
    std::unique_ptr<carto::d3d12::D3D12Device> device;
    std::unique_ptr<carto::d3d12::D3D12Swapchain> swapchain;
    carto::d3d12::ViewportInputState input;
    std::optional<std::string> window_error;
};

WindowState* state(HWND window) noexcept {
    return reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void update_navigation_title(HWND window) {
    const auto* window_state = state(window);
    if (window_state == nullptr) return;
    const auto& input = window_state->input;
    std::wostringstream title;
    title.imbue(std::locale::classic());
    title << L"Cartographer D3D12 | Drag: orbit, arrows: nudge, wheel: zoom | yaw "
          << std::fixed << std::setprecision(1) << input.yaw_degrees()
          << L"° pitch " << input.pitch_degrees()
          << L"° zoom " << input.zoom()
          << L"x | DPI " << input.dpi()
          << L" (" << std::setprecision(2) << input.dpi_scale() << L"x)";
    const std::wstring text = title.str();
    static_cast<void>(SetWindowTextW(window, text.c_str()));
}

std::optional<carto::d3d12::ViewportNavigationKey> navigation_key(WPARAM key) noexcept {
    using carto::d3d12::ViewportNavigationKey;
    switch (key) {
    case VK_LEFT: return ViewportNavigationKey::yaw_left;
    case VK_RIGHT: return ViewportNavigationKey::yaw_right;
    case VK_UP: return ViewportNavigationKey::pitch_up;
    case VK_DOWN: return ViewportNavigationKey::pitch_down;
    case VK_ADD:
    case VK_OEM_PLUS: return ViewportNavigationKey::zoom_in;
    case VK_SUBTRACT:
    case VK_OEM_MINUS: return ViewportNavigationKey::zoom_out;
    default: return std::nullopt;
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* window_state = state(window);
    switch (message) {
    case WM_SIZE:
        if (window_state != nullptr && window_state->swapchain != nullptr &&
            w_param != SIZE_MINIMIZED) {
            RECT client{};
            if (GetClientRect(window, &client) == 0) {
                window_state->window_error = "GetClientRect failed while resizing the D3D12 swapchain";
                return 0;
            }
            const auto width = static_cast<std::uint32_t>(client.right - client.left);
            const auto height = static_cast<std::uint32_t>(client.bottom - client.top);
            if (width != 0U && height != 0U) {
                const auto resized = window_state->swapchain->resize(width, height);
                if (!resized) window_state->window_error = resized.error().message;
            }
            update_navigation_title(window);
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(l_param);
        if (suggested == nullptr) return DefWindowProcW(window, message, w_param, l_param);
        if (window_state != nullptr &&
            !window_state->input.set_dpi(static_cast<std::uint32_t>(LOWORD(w_param)))) {
            window_state->window_error = "WM_DPICHANGED supplied an unsupported DPI value";
            return 0;
        }
        if (SetWindowPos(window, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE) == 0 && window_state != nullptr) {
            window_state->window_error = "SetWindowPos failed while applying WM_DPICHANGED bounds";
        }
        update_navigation_title(window);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (window_state != nullptr) {
            SetCapture(window);
            if (GetCapture() == window) {
                static_cast<void>(window_state->input.begin_orbit(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)));
                update_navigation_title(window);
            }
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (window_state != nullptr && window_state->input.dragging() &&
            window_state->input.move_pointer(
                GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param))) {
            update_navigation_title(window);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (window_state != nullptr && window_state->input.end_orbit()) {
            if (GetCapture() == window) ReleaseCapture();
            update_navigation_title(window);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (window_state != nullptr && window_state->input.end_orbit()) {
            update_navigation_title(window);
        }
        break;
    case WM_MOUSEWHEEL:
        if (window_state != nullptr && window_state->input.zoom_wheel(
                GET_WHEEL_DELTA_WPARAM(w_param))) {
            update_navigation_title(window);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (window_state != nullptr) {
            if (w_param == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            const auto action = navigation_key(w_param);
            if (action.has_value()) {
                const bool is_repeat = (static_cast<std::uint64_t>(l_param) &
                    (std::uint64_t{1U} << 30U)) != 0U;
                if (!is_repeat && window_state->input.adjust(*action)) {
                    update_navigation_title(window);
                }
                return 0;
            }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
        const DWORD error = GetLastError();
        const bool already_per_monitor_v2 = error == ERROR_ACCESS_DENIED &&
            AreDpiAwarenessContextsEqual(
                GetThreadDpiAwarenessContext(),
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0;
        if (!already_per_monitor_v2) {
            MessageBoxW(nullptr,
                L"Cartographer D3D12 requires per-monitor DPI awareness v2.",
                L"Cartographer D3D12", MB_ICONERROR | MB_OK);
            return 1;
        }
    }

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

    RECT initial_client{};
    if (GetClientRect(window, &initial_client) == 0 ||
        initial_client.right <= initial_client.left ||
        initial_client.bottom <= initial_client.top) {
        MessageBoxW(window, L"The initial D3D12 client area is invalid.",
                    L"Cartographer D3D12", MB_ICONERROR | MB_OK);
        DestroyWindow(window);
        return 1;
    }
    const auto initial_width = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(initial_client.right) - initial_client.left);
    const auto initial_height = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(initial_client.bottom) - initial_client.top);

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
            reinterpret_cast<std::uintptr_t>(window), initial_width, initial_height, 2U,
            carto::gpu::Format::bgra8_unorm, false});
    if (!swapchain) {
        MessageBoxA(window, swapchain.error().message.c_str(), "Cartographer D3D12", MB_ICONERROR);
        return 1;
    }
    window_state.swapchain.reset(swapchain.value().release());
    static_cast<void>(window_state.input.set_dpi(GetDpiForWindow(window)));
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window_state));
    update_navigation_title(window);
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
        if (window_state.window_error.has_value()) {
            MessageBoxA(window, window_state.window_error->c_str(),
                        "Cartographer D3D12 window", MB_ICONERROR);
            break;
        }
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
