#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/assets/source_registry.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {

enum class CameraProjection : std::uint8_t { perspective, orthographic };
enum class CameraReviewPurpose : std::uint8_t {
    general, familiarity_baseline, silhouette_review, screenshot_evidence
};

struct AuthoredCamera {
    scene::ObjectId object;
    std::string identity;
    std::string bookmark_identity;
    std::string reference_image_identity;
    CameraProjection projection = CameraProjection::perspective;
    CameraReviewPurpose review_purpose = CameraReviewPurpose::general;
    double vertical_fov_radians = 1.0471975511965976;
    double orthographic_height = 10.0;
    double near_plane = 0.1;
    double far_plane = 10'000.0;
    bool active_for_review = false;
    core::Revision revision{};

    [[nodiscard]] core::Result<void> validate() const;
};

enum class AuthoredLightKind : std::uint8_t { directional, point, spot, area };
enum class LightNormalState : std::uint8_t { steady, switchable, intermittent };
enum class LightFailureState : std::uint8_t { none, dark, intermittent };

struct AuthoredLight {
    scene::ObjectId object;
    std::string identity;
    std::string semantic_class;
    // These are descriptive source references, never authorization subjects.
    std::string fixture_owner;
    std::string power_owner;
    std::string switch_owner;
    AuthoredLightKind kind = AuthoredLightKind::point;
    core::Vec3d linear_color{1.0, 1.0, 1.0};
    double intensity = 1.0;
    double range = 10.0;
    double inner_cone_radians = 0.0;
    double outer_cone_radians = 0.0;
    double area_width = 0.0;
    double area_height = 0.0;
    std::optional<double> color_temperature_kelvin;
    LightNormalState normal_state = LightNormalState::steady;
    LightFailureState failure_state = LightFailureState::none;
    bool casts_shadows = true;
    bool flicker_eligible = false;
    core::Revision revision{};

    [[nodiscard]] core::Result<void> validate() const;
};

enum class ReferencePlaneView : std::uint8_t {
    front, side, top, perspective, camera_pinned
};

struct ReferenceImageCalibration {
    core::Vec2d first_pixel;
    core::Vec2d second_pixel;
    double known_distance_meters = 0.0;

    [[nodiscard]] core::Result<void> validate(
        std::uint32_t image_width_pixels,
        std::uint32_t image_height_pixels) const;
    [[nodiscard]] core::Result<double> calibrated_width_meters(
        std::uint32_t image_width_pixels,
        std::uint32_t image_height_pixels) const;
};

// A review-only image plane. The locator is project-relative and the digest,
// asset generation, and optional observed bytes bind callers to exact source
// content. This record never loads an image, substitutes a fallback, or grants
// renderer/runtime authority.
struct ReferenceImagePlane {
    std::string identity;
    std::string source_asset_identity;
    std::string source_locator;
    assets::Sha256Digest source_digest;
    core::Revision source_generation{};
    std::uint32_t image_width_pixels = 0U;
    std::uint32_t image_height_pixels = 0U;
    ReferencePlaneView view = ReferencePlaneView::perspective;
    std::string view_set_identity;
    std::optional<scene::ObjectId> pinned_camera;
    core::Vec3d origin{};
    core::Vec3d normal{0.0, 0.0, 1.0};
    core::Vec3d up{0.0, 1.0, 0.0};
    double width_meters = 1.0;
    double opacity = 1.0;
    bool visible = true;
    bool locked = false;
    std::optional<ReferenceImageCalibration> calibration;
    core::Revision revision{1U};

    [[nodiscard]] core::Result<void> validate() const;
};

struct ReferenceImagePlaneCorners {
    core::Vec3d upper_left{};
    core::Vec3d upper_right{};
    core::Vec3d lower_right{};
    core::Vec3d lower_left{};
};

struct ReferenceImagePlanePlanCorners {
    core::Vec2d upper_left{};
    core::Vec2d upper_right{};
    core::Vec2d lower_right{};
    core::Vec2d lower_left{};
};

// Derives the image quad from the plane's centered origin, physical width,
// pixel aspect ratio, normal, and up vector. Image-right is cross(up, normal).
// This is geometry only; it does not resolve or render the source image.
[[nodiscard]] core::Result<ReferenceImagePlaneCorners>
reference_image_plane_corners(const ReferenceImagePlane& plane);

// Projects only top-view planes whose authored basis lies in the X/Z work
// surface. Front, side, perspective, and camera-pinned planes are not silently
// flattened into the plan view.
[[nodiscard]] core::Result<ReferenceImagePlanePlanCorners>
reference_image_plane_xz_corners(const ReferenceImagePlane& plane);

enum class ReferenceImageSourceState : std::uint8_t {
    missing, stale, verified
};

// This is a pure binding check. The caller must read the project-local source
// and supply its observed digest; absent bytes remain missing and are never
// replaced with generated or cached fallback content.
[[nodiscard]] ReferenceImageSourceState inspect_reference_image_source(
    const ReferenceImagePlane& plane,
    const assets::ProjectAssetRegistry& registry,
    std::optional<assets::Sha256Digest> observed_source_digest);

// Reads an exact, content-addressed image blob from the project-local source
// store. It checks the registry binding, canonical path, byte digest, and
// inspected dimensions; it does not decode pixels or synthesize a fallback.
[[nodiscard]] core::Result<std::vector<std::uint8_t>>
read_reference_image_source_bytes(
    const std::filesystem::path& project_root,
    const ReferenceImagePlane& plane,
    const assets::ProjectAssetRegistry& registry);

// Durable source intent only. Preview buffers, renderer handles, evaluated
// exposure, switch state, and runtime light simulation are derived/external.
class PresentationCatalog final {
public:
    static constexpr std::uint32_t kSchemaVersion = 2U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecords = 4'096U;

    [[nodiscard]] core::Result<void> insert_camera(AuthoredCamera camera);
    [[nodiscard]] core::Result<void> insert_light(AuthoredLight light);
    [[nodiscard]] core::Result<void> insert_reference_plane(ReferenceImagePlane plane);
    [[nodiscard]] const std::map<scene::ObjectId, AuthoredCamera>& cameras() const noexcept {
        return cameras_;
    }
    [[nodiscard]] const std::map<scene::ObjectId, AuthoredLight>& lights() const noexcept {
        return lights_;
    }
    [[nodiscard]] const std::map<std::string, ReferenceImagePlane>&
    reference_planes() const noexcept { return reference_planes_; }
    [[nodiscard]] bool empty() const noexcept {
        return cameras_.empty() && lights_.empty() && reference_planes_.empty();
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PresentationCatalog> deserialize(std::string_view text);

private:
    std::map<scene::ObjectId, AuthoredCamera> cameras_;
    std::map<scene::ObjectId, AuthoredLight> lights_;
    std::map<std::string, ReferenceImagePlane> reference_planes_;
};

} // namespace carto::project
