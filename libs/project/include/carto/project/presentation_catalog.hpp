#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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

// Durable source intent only. Preview buffers, renderer handles, evaluated
// exposure, switch state, and runtime light simulation are derived/external.
class PresentationCatalog final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecords = 4'096U;

    [[nodiscard]] core::Result<void> insert_camera(AuthoredCamera camera);
    [[nodiscard]] core::Result<void> insert_light(AuthoredLight light);
    [[nodiscard]] const std::map<scene::ObjectId, AuthoredCamera>& cameras() const noexcept {
        return cameras_;
    }
    [[nodiscard]] const std::map<scene::ObjectId, AuthoredLight>& lights() const noexcept {
        return lights_;
    }
    [[nodiscard]] bool empty() const noexcept { return cameras_.empty() && lights_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PresentationCatalog> deserialize(std::string_view text);

private:
    std::map<scene::ObjectId, AuthoredCamera> cameras_;
    std::map<scene::ObjectId, AuthoredLight> lights_;
};

} // namespace carto::project
