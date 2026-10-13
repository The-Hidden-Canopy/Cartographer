#include <carto/editor/editor_camera.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace carto::editor {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool valid_projection(EditorProjection projection) noexcept {
    return projection == EditorProjection::orthographic ||
        projection == EditorProjection::perspective;
}

double wrap_angle(double angle) noexcept {
    const double wrapped = std::remainder(angle, kTwoPi);
    return wrapped == -kPi ? kPi : wrapped;
}

double view_span(const EditorCamera& camera) noexcept {
    if (camera.projection() == EditorProjection::orthographic) {
        return camera.orthographic_height();
    }
    return 2.0 * camera.distance() * std::tan(camera.vertical_fov() * 0.5);
}

} // namespace

bool EditorCamera::valid() const noexcept {
    return valid_projection(projection_) && target_.finite() &&
        std::isfinite(distance_) && distance_ >= kMinDistance &&
        distance_ <= kMaxDistance && std::isfinite(yaw_) &&
        std::abs(yaw_) <= 1.0e12 && std::isfinite(pitch_) &&
        pitch_ >= -kMaxPitch && pitch_ <= kMaxPitch &&
        std::isfinite(orthographic_height_) &&
        orthographic_height_ >= kMinOrthographicHeight &&
        orthographic_height_ <= kMaxOrthographicHeight &&
        std::isfinite(vertical_fov_) && vertical_fov_ >= kMinVerticalFov &&
        vertical_fov_ <= kMaxVerticalFov;
}

core::Result<void> EditorCamera::validate() const {
    if (!valid()) {
        return core::Result<void>::failure(invalid(
            "editor camera state is non-finite or outside its bounded domain"));
    }
    return core::Result<void>::success();
}

EditorCamera::Axes EditorCamera::axes() const noexcept {
    if (!valid()) return {};
    const double cos_pitch = std::cos(pitch_);
    const core::Vec3d forward{
        std::sin(yaw_) * cos_pitch,
        std::sin(pitch_),
        -std::cos(yaw_) * cos_pitch,
    };
    const core::Vec3d world_up{0.0, 1.0, 0.0};
    const core::Vec3d right = core::cross(forward, world_up).normalized();
    const core::Vec3d up = core::cross(right, forward).normalized();
    if (!right.finite() || !up.finite() || !forward.finite()) return {};
    return {right, up, forward};
}

core::Vec3d EditorCamera::position() const noexcept {
    if (!valid()) return {};
    return target_ - axes().forward * distance_;
}

void EditorCamera::set_projection(EditorProjection projection) noexcept {
    if (!valid() || !valid_projection(projection) || projection == projection_) return;
    const double span = view_span(*this);
    if (!std::isfinite(span) || span <= 0.0) return;
    if (projection == EditorProjection::orthographic) {
        orthographic_height_ = std::clamp(
            span, kMinOrthographicHeight, kMaxOrthographicHeight);
    } else {
        const double denominator = 2.0 * std::tan(vertical_fov_ * 0.5);
        if (!std::isfinite(denominator) || denominator <= 0.0) return;
        distance_ = std::clamp(span / denominator, kMinDistance, kMaxDistance);
    }
    projection_ = projection;
}

bool EditorCamera::orbit(double dx_pixels, double dy_pixels) noexcept {
    if (!valid() || !std::isfinite(dx_pixels) || !std::isfinite(dy_pixels) ||
        std::abs(dx_pixels) > 1.0e7 || std::abs(dy_pixels) > 1.0e7) {
        return false;
    }
    const double next_yaw = wrap_angle(yaw_ + dx_pixels * kOrbitRadiansPerPixel);
    const double next_pitch = std::clamp(
        pitch_ - dy_pixels * kOrbitRadiansPerPixel, -kMaxPitch, kMaxPitch);
    if (!std::isfinite(next_yaw) || !std::isfinite(next_pitch)) return false;
    yaw_ = next_yaw;
    pitch_ = next_pitch;
    return true;
}

bool EditorCamera::pan(
    double dx_pixels, double dy_pixels, double viewport_height_pixels) noexcept {
    if (!valid() || !std::isfinite(dx_pixels) || !std::isfinite(dy_pixels) ||
        !std::isfinite(viewport_height_pixels) || viewport_height_pixels <= 0.0 ||
        std::abs(dx_pixels) > 1.0e7 || std::abs(dy_pixels) > 1.0e7) {
        return false;
    }
    const double span = view_span(*this);
    const double world_per_pixel = span / viewport_height_pixels;
    if (!std::isfinite(world_per_pixel) || world_per_pixel <= 0.0) return false;
    const auto camera_axes = axes();
    const core::Vec3d next_target = target_ - camera_axes.right *
            (dx_pixels * world_per_pixel) + camera_axes.up *
            (dy_pixels * world_per_pixel);
    if (!next_target.finite()) return false;
    target_ = next_target;
    return true;
}

bool EditorCamera::dolly(double steps) noexcept {
    if (!valid() || !std::isfinite(steps) || std::abs(steps) > 1.0e4) return false;
    const double exponent = std::clamp(
        -steps * kDollyStepsPerExponent, -60.0, 60.0);
    const double factor = std::exp(exponent);
    if (!std::isfinite(factor) || factor <= 0.0) return false;
    if (projection_ == EditorProjection::orthographic) {
        const double next = orthographic_height_ * factor;
        if (!std::isfinite(next)) return false;
        orthographic_height_ = std::clamp(
            next, kMinOrthographicHeight, kMaxOrthographicHeight);
    } else {
        const double next = distance_ * factor;
        if (!std::isfinite(next)) return false;
        distance_ = std::clamp(next, kMinDistance, kMaxDistance);
    }
    return true;
}

bool EditorCamera::fly(core::Vec3d local, double seconds) noexcept {
    if (!valid() || projection_ != EditorProjection::perspective ||
        !local.finite() || !std::isfinite(seconds) || seconds < 0.0 ||
        seconds > 3600.0 || local.length_squared() > 1.0e12) {
        return false;
    }
    const auto camera_axes = axes();
    const core::Vec3d world_velocity = camera_axes.right * local.x +
        camera_axes.up * local.y + camera_axes.forward * local.z;
    const core::Vec3d delta = world_velocity * seconds;
    if (!delta.finite()) return false;
    target_ = target_ + delta;
    return target_.finite();
}

bool EditorCamera::frame(core::Vec3d centre, double radius, double aspect) noexcept {
    if (!valid() || !centre.finite() || !std::isfinite(radius) || radius <= 0.0 ||
        !std::isfinite(aspect) || aspect <= 0.0 || aspect > 1.0e6) {
        return false;
    }
    if (projection_ == EditorProjection::orthographic) {
        const double required_height = std::max(2.0 * radius, 2.0 * radius / aspect);
        if (!std::isfinite(required_height) ||
            required_height > kMaxOrthographicHeight) return false;
        target_ = centre;
        orthographic_height_ = std::clamp(
            required_height, kMinOrthographicHeight, kMaxOrthographicHeight);
        return true;
    }

    const double vertical_half_angle = vertical_fov_ * 0.5;
    const double horizontal_half_angle =
        std::atan(std::tan(vertical_half_angle) * aspect);
    const double half_angle = std::min(vertical_half_angle, horizontal_half_angle);
    const double required_distance = radius / std::sin(half_angle) * 1.05;
    if (!std::isfinite(required_distance) || required_distance > kMaxDistance) {
        return false;
    }
    target_ = centre;
    distance_ = std::clamp(required_distance, kMinDistance, kMaxDistance);
    return true;
}

std::string EditorCamera::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << (projection_ == EditorProjection::orthographic ? "ortho" : "persp")
           << ' ' << target_.x << ' ' << target_.y << ' ' << target_.z
           << ' ' << distance_ << ' ' << yaw_ << ' ' << pitch_ << ' '
           << orthographic_height_ << ' ' << vertical_fov_;
    return output.str();
}

std::optional<EditorCamera> EditorCamera::deserialize(std::string_view text) {
    if (text.empty() || text.size() > 1024U) return std::nullopt;
    std::istringstream input{std::string(text)};
    std::string projection;
    EditorCamera camera;
    if (!(input >> projection >> camera.target_.x >> camera.target_.y >>
            camera.target_.z >> camera.distance_ >> camera.yaw_ >> camera.pitch_ >>
            camera.orthographic_height_ >> camera.vertical_fov_)) {
        return std::nullopt;
    }
    if (projection == "ortho") {
        camera.projection_ = EditorProjection::orthographic;
    } else if (projection == "persp") {
        camera.projection_ = EditorProjection::perspective;
    } else {
        return std::nullopt;
    }
    if (std::string extra; input >> extra) return std::nullopt;
    return camera.valid() ? std::optional<EditorCamera>{camera} : std::nullopt;
}

} // namespace carto::editor
