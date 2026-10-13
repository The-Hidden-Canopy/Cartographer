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
