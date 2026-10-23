#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace carto::d3d12 {

enum class ViewportNavigationKey : std::uint8_t {
    yaw_left,
    yaw_right,
    pitch_up,
    pitch_down,
    zoom_in,
    zoom_out,
};

// Platform-neutral navigation state for the native D3D12 viewport shell.
class ViewportInputState final {
public:
    static constexpr std::uint32_t kMinimumDpi = 24U;
    static constexpr std::uint32_t kMaximumDpi = 960U;
    static constexpr double kMinimumZoom = 0.125;
    static constexpr double kMaximumZoom = 16.0;

    [[nodiscard]] bool begin_orbit(std::int32_t x, std::int32_t y) noexcept {
        if (dragging_) return false;
        dragging_ = true;
        last_x_ = x;
        last_y_ = y;
        return true;
    }

    [[nodiscard]] bool move_pointer(std::int32_t x, std::int32_t y) noexcept {
        if (!dragging_) return false;
        const double delta_x = static_cast<double>(x) - static_cast<double>(last_x_);
        const double delta_y = static_cast<double>(y) - static_cast<double>(last_y_);
        last_x_ = x;
        last_y_ = y;
        if (delta_x == 0.0 && delta_y == 0.0) return false;
        yaw_degrees_ = wrap_degrees(yaw_degrees_ + delta_x * 0.25);
        pitch_degrees_ = std::clamp(
            pitch_degrees_ + delta_y * 0.25, -89.0, 89.0);
        return true;
    }

    [[nodiscard]] bool end_orbit() noexcept {
        if (!dragging_) return false;
        dragging_ = false;
        return true;
    }

    [[nodiscard]] bool adjust(ViewportNavigationKey key) noexcept {
        switch (key) {
        case ViewportNavigationKey::yaw_left:
            yaw_degrees_ = wrap_degrees(yaw_degrees_ - 5.0);
            return true;
        case ViewportNavigationKey::yaw_right:
            yaw_degrees_ = wrap_degrees(yaw_degrees_ + 5.0);
            return true;
        case ViewportNavigationKey::pitch_up:
            pitch_degrees_ = std::clamp(pitch_degrees_ - 5.0, -89.0, 89.0);
            return true;
        case ViewportNavigationKey::pitch_down:
            pitch_degrees_ = std::clamp(pitch_degrees_ + 5.0, -89.0, 89.0);
            return true;
        case ViewportNavigationKey::zoom_in:
            return set_zoom(zoom_ * 1.1);
        case ViewportNavigationKey::zoom_out:
            return set_zoom(zoom_ / 1.1);
        }
        return false;
    }

    [[nodiscard]] bool zoom_wheel(std::int32_t wheel_delta) noexcept {
        if (wheel_delta == 0) return false;
        const double factor = std::pow(1.1, static_cast<double>(wheel_delta) / 120.0);
        return set_zoom(zoom_ * factor);
    }

    [[nodiscard]] bool set_dpi(std::uint32_t dpi) noexcept {
        if (dpi < kMinimumDpi || dpi > kMaximumDpi) return false;
        dpi_ = dpi;
        return true;
    }

    [[nodiscard]] double yaw_degrees() const noexcept { return yaw_degrees_; }
    [[nodiscard]] double pitch_degrees() const noexcept { return pitch_degrees_; }
    [[nodiscard]] double zoom() const noexcept { return zoom_; }
    [[nodiscard]] std::uint32_t dpi() const noexcept { return dpi_; }
    [[nodiscard]] double dpi_scale() const noexcept {
        return static_cast<double>(dpi_) / 96.0;
    }
    [[nodiscard]] bool dragging() const noexcept { return dragging_; }

private:
    [[nodiscard]] static double wrap_degrees(double degrees) noexcept {
        double wrapped = std::fmod(degrees + 180.0, 360.0);
        if (wrapped < 0.0) wrapped += 360.0;
        return wrapped - 180.0;
    }

    [[nodiscard]] bool set_zoom(double zoom) noexcept {
        const double bounded = std::clamp(zoom, kMinimumZoom, kMaximumZoom);
        if (bounded == zoom_) return false;
        zoom_ = bounded;
        return true;
    }

    double yaw_degrees_ = 0.0;
    double pitch_degrees_ = 0.0;
    double zoom_ = 1.0;
    std::uint32_t dpi_ = 96U;
    std::int32_t last_x_ = 0;
    std::int32_t last_y_ = 0;
    bool dragging_ = false;
};

} // namespace carto::d3d12
