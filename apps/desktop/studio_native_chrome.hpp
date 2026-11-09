#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace carto::studio {

[[nodiscard]] inline bool system_high_contrast_enabled() noexcept {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

inline void apply_native_caption(HWND window, bool high_contrast) noexcept {
    if (window == nullptr) return;
    const BOOL dark = high_contrast ? FALSE : TRUE;
    if (FAILED(DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark))))
        static_cast<void>(DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(19), &dark, sizeof(dark)));
    const COLORREF caption = high_contrast ? 0xffffffffUL : RGB(7, 11, 18);
    const COLORREF text = high_contrast ? 0xffffffffUL : RGB(230, 236, 250);
    static_cast<void>(DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(35), &caption, sizeof(caption)));
    static_cast<void>(DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(36), &text, sizeof(text)));
}

} // namespace carto::studio
