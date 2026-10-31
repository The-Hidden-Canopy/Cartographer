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
    frame_all,
};

// Platform-neutral navigation state for the native D3D12 viewport shell.
class ViewportInputState final {
public:
    static constexpr std::uint32_t kMinimumDpi = 24U;
    static constexpr std::uint32_t kMaximumDpi = 960U;
    static constexpr double kMinimumZoom = 0.125;
    static constexpr double kMaximumZoom = 16.0;
    static constexpr double kMaximumPanLogicalPixels = 32'768.0;

    [[nodiscard]] bool begin_orbit(std::int32_t x, std::int32_t y) noexcept {
        return begin_drag(DragMode::orbit, x, y);
    }

    [[nodiscard]] bool begin_pan(std::int32_t x, std::int32_t y) noexcept {
        return begin_drag(DragMode::pan, x, y);
    }

    [[nodiscard]] bool move_pointer(std::int32_t x, std::int32_t y) noexcept {
        if (drag_mode_ == DragMode::none) return false;
        const double delta_x = static_cast<double>(x) - static_cast<double>(last_x_);
        const double delta_y = static_cast<double>(y) - static_cast<double>(last_y_);
        last_x_ = x;
        last_y_ = y;
        if (delta_x == 0.0 && delta_y == 0.0) return false;
        const double logical_delta_x = delta_x / dpi_scale();
        const double logical_delta_y = delta_y / dpi_scale();
        if (drag_mode_ == DragMode::orbit) {
            yaw_degrees_ = wrap_degrees(yaw_degrees_ + logical_delta_x * 0.25);
            pitch_degrees_ = std::clamp(
                pitch_degrees_ + logical_delta_y * 0.25, -89.0, 89.0);
            return true;
        }
        const double next_pan_x = std::clamp(
            pan_x_logical_pixels_ - logical_delta_x,
            -kMaximumPanLogicalPixels, kMaximumPanLogicalPixels);
        const double next_pan_y = std::clamp(
            pan_y_logical_pixels_ + logical_delta_y,
            -kMaximumPanLogicalPixels, kMaximumPanLogicalPixels);
        const bool changed = next_pan_x != pan_x_logical_pixels_ ||
            next_pan_y != pan_y_logical_pixels_;
        pan_x_logical_pixels_ = next_pan_x;
        pan_y_logical_pixels_ = next_pan_y;
        return changed;
    }

    [[nodiscard]] bool end_orbit() noexcept {
        if (drag_mode_ != DragMode::orbit) return false;
        drag_mode_ = DragMode::none;
        return true;
    }

    [[nodiscard]] bool end_pan() noexcept {
        if (drag_mode_ != DragMode::pan) return false;
        drag_mode_ = DragMode::none;
        return true;
    }

    [[nodiscard]] bool end_drag() noexcept {
        if (drag_mode_ == DragMode::none) return false;
        drag_mode_ = DragMode::none;
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
        case ViewportNavigationKey::frame_all: {
            const bool changed = yaw_degrees_ != 0.0 || pitch_degrees_ != 0.0 ||
                zoom_ != 1.0 || pan_x_logical_pixels_ != 0.0 ||
                pan_y_logical_pixels_ != 0.0;
            yaw_degrees_ = 0.0;
            pitch_degrees_ = 0.0;
            zoom_ = 1.0;
            pan_x_logical_pixels_ = 0.0;
            pan_y_logical_pixels_ = 0.0;
            return changed;
        }
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
    [[nodiscard]] double pan_x_logical_pixels() const noexcept {
        return pan_x_logical_pixels_;
    }
    [[nodiscard]] double pan_y_logical_pixels() const noexcept {
        return pan_y_logical_pixels_;
    }
    [[nodiscard]] std::uint32_t dpi() const noexcept { return dpi_; }
    [[nodiscard]] double dpi_scale() const noexcept {
        return static_cast<double>(dpi_) / 96.0;
    }
    [[nodiscard]] bool dragging() const noexcept {
        return drag_mode_ != DragMode::none;
    }

private:
    enum class DragMode : std::uint8_t { none, orbit, pan };

    [[nodiscard]] bool begin_drag(
        DragMode mode, std::int32_t x, std::int32_t y) noexcept {
        if (drag_mode_ != DragMode::none) return false;
        drag_mode_ = mode;
        last_x_ = x;
        last_y_ = y;
        return true;
    }

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
    double pan_x_logical_pixels_ = 0.0;
    double pan_y_logical_pixels_ = 0.0;
    std::uint32_t dpi_ = 96U;
    std::int32_t last_x_ = 0;
    std::int32_t last_y_ = 0;
    DragMode drag_mode_ = DragMode::none;
};

} // namespace carto::d3d12
