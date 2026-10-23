#include <carto/d3d12/viewport_input.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void navigation_and_dpi_state_is_bounded() {
    using carto::d3d12::ViewportInputState;
    using carto::d3d12::ViewportNavigationKey;

    ViewportInputState input;
    require(input.dpi() == 96U && input.dpi_scale() == 1.0,
        "navigation starts at the standard DPI scale");
    require(!input.move_pointer(10, 10), "pointer motion without capture is ignored");
    require(input.begin_orbit(100, 50), "primary press begins an orbit drag");
    require(!input.begin_orbit(100, 50), "duplicate press does not reset a captured drag");
    require(input.move_pointer(140, 70), "captured pointer motion updates the view");
    require(input.yaw_degrees() == 10.0 && input.pitch_degrees() == 5.0,
        "pointer delta maps to bounded orbit angles");

    require(input.move_pointer(
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::min()),
        "extreme signed pointer coordinates are handled");
    require(std::isfinite(input.yaw_degrees()) &&
            input.yaw_degrees() >= -180.0 && input.yaw_degrees() < 180.0,
        "yaw wraps into a finite canonical range");
    require(input.pitch_degrees() == -89.0,
        "pitch clamps before reaching a pole");
    require(input.end_orbit() && !input.dragging(), "release ends the captured drag");
    require(!input.end_orbit() && !input.move_pointer(0, 0),
        "post-release motion cannot mutate the view");

    require(input.adjust(ViewportNavigationKey::yaw_right),
        "keyboard can nudge yaw");
    require(input.adjust(ViewportNavigationKey::pitch_down),
        "keyboard can nudge pitch");
    require(input.adjust(ViewportNavigationKey::zoom_in),
        "keyboard can zoom in");
    require(input.zoom_wheel(120), "mouse wheel zooms in");
    require(input.zoom() > 1.0 && input.zoom() <= ViewportInputState::kMaximumZoom,
        "keyboard and wheel zoom remain bounded");
    require(input.zoom_wheel(std::numeric_limits<std::int32_t>::max()) &&
            input.zoom() == ViewportInputState::kMaximumZoom,
        "hostile positive wheel deltas clamp to maximum zoom");
    require(input.zoom_wheel(std::numeric_limits<std::int32_t>::min()) &&
            input.zoom() == ViewportInputState::kMinimumZoom,
        "hostile negative wheel deltas clamp to minimum zoom");

    require(input.set_dpi(144U) && input.dpi() == 144U && input.dpi_scale() == 1.5,
        "per-monitor DPI updates the scale");
    require(input.set_dpi(ViewportInputState::kMinimumDpi),
        "minimum supported DPI is admitted");
    require(input.set_dpi(ViewportInputState::kMaximumDpi),
        "maximum supported DPI is admitted");
    require(!input.set_dpi(0U) && !input.set_dpi(ViewportInputState::kMaximumDpi + 1U) &&
            input.dpi() == ViewportInputState::kMaximumDpi,
        "invalid DPI values are refused without changing state");
}

} // namespace

int main() {
    try {
        navigation_and_dpi_state_is_bounded();
        std::cout << "PASS D3D12 viewport input bounds and DPI state\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL D3D12 viewport input: " << error.what() << '\n';
        return 1;
    }
}
