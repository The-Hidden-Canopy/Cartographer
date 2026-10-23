#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace carto::editor {

enum class EditorProjection : std::uint8_t {
    orthographic,
    perspective,
};

struct EditorRay final {
    core::Vec3d origin{};
    core::Vec3d direction{0.0, 0.0, -1.0};
};

struct EditorProjectionMarker final {
    core::Vec2d screen_position{};
    double view_depth = 0.0;
    bool in_front = false;
    bool inside_viewport = false;
};

// Backend-neutral editor navigation state. This is UI state, not scene truth:
// it has no object IDs, project references, world-camera authority, or command
// mutation path. A renderer may consume the derived axes/position, while an
// authoring command must still be submitted through the normal transaction
// boundary.
class EditorCamera final {
public:
    static constexpr double kMinDistance = 0.05;
    static constexpr double kMaxDistance = 1.0e9;
    static constexpr double kMinOrthographicHeight = 1.0e-6;
    static constexpr double kMaxOrthographicHeight = 1.0e9;
    static constexpr double kMinVerticalFov = 1.0e-3;
    static constexpr double kMaxVerticalFov = 3.140592653589793;
    static constexpr double kMaxPitch = 1.5533430342749532; // 89 degrees
    static constexpr double kOrbitRadiansPerPixel = 0.005;
    static constexpr double kDollyStepsPerExponent = 0.1;

    [[nodiscard]] EditorProjection projection() const noexcept { return projection_; }
    [[nodiscard]] core::Vec3d target() const noexcept { return target_; }
    [[nodiscard]] double distance() const noexcept { return distance_; }
    [[nodiscard]] double yaw() const noexcept { return yaw_; }
    [[nodiscard]] double pitch() const noexcept { return pitch_; }
    [[nodiscard]] double orthographic_height() const noexcept {
        return orthographic_height_;
    }
    [[nodiscard]] double vertical_fov() const noexcept { return vertical_fov_; }

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;

    struct Axes final {
        core::Vec3d right{1.0, 0.0, 0.0};
        core::Vec3d up{0.0, 1.0, 0.0};
        core::Vec3d forward{0.0, 0.0, -1.0};
    };

    [[nodiscard]] Axes axes() const noexcept;
    [[nodiscard]] core::Vec3d position() const noexcept;

    // World-space pick ray through a pixel position, using the same span and
    // FOV conventions as the renderer-facing navigation math. Pixels use +x
    // right and +y down; pixel coordinates outside the viewport are projected
    // rather than rejected. Invalid camera state, non-finite input, or a
    // zero-sized viewport fail closed with a Diagnostic.
    [[nodiscard]] core::Result<EditorRay> ray_for_pixel(
        core::Vec2d pixel, core::Vec2d viewport_size) const;
    // Projects an exact world point into viewport-local pixel coordinates.
    // Near/far clipping is not inferred; the result reports only front-facing
    // and 2D viewport inclusion for lightweight authoring markers.
    [[nodiscard]] core::Result<EditorProjectionMarker> project_marker(
        core::Vec3d world_point, core::Vec2d viewport_size) const;

    // Switching projection preserves the view span at the orbit target.
    void set_projection(EditorProjection projection) noexcept;

    // Pixel deltas use +x right and +y down. Invalid input leaves the camera
    // unchanged and returns false.
    [[nodiscard]] bool orbit(double dx_pixels, double dy_pixels) noexcept;
    [[nodiscard]] bool pan(
        double dx_pixels, double dy_pixels, double viewport_height_pixels) noexcept;
    // Positive steps zoom in / move closer.
    [[nodiscard]] bool dolly(double steps) noexcept;
    // Moves camera and target together in (right, up, forward) coordinates.
    // Fly is intentionally unavailable in orthographic projection.
    [[nodiscard]] bool fly(core::Vec3d local, double seconds) noexcept;
    // Fits a sphere in the current projection for the given viewport aspect.
    [[nodiscard]] bool frame(
        core::Vec3d centre, double radius, double aspect) noexcept;

    // Stable, local-only camera state. It contains no project path or runtime
    // handle and is safe to persist as UI preferences.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<EditorCamera> deserialize(
        std::string_view text);

private:
    EditorProjection projection_{EditorProjection::perspective};
    core::Vec3d target_{};
    double distance_{10.0};
    double yaw_{0.0};
    double pitch_{0.0};
    double orthographic_height_{2.0};
    double vertical_fov_{0.7853981633974483}; // 45 degrees
};

} // namespace carto::editor
