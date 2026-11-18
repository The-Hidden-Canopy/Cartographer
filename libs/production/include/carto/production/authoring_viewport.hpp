#pragma once

#include <carto/production/authoring_environment.hpp>
#include <carto/production/authoring_material.hpp>
#include <carto/project/project.hpp>
#include <carto/render/material.hpp>
#include <carto/render/temporal_reprojection.hpp>

#include <map>
#include <vector>

namespace carto::production {

// CPU reference attachments use doubles and multiple history copies. Keep that
// path at its established 1M limit; native temporal targets have a separate
// bounded 4K budget. 128 bytes/pixel reserves <=1 GiB for native frame/history
// attachments at the admitted extent (material/source budgets are separate).
inline constexpr std::uint64_t kAuthoringCpuMaxPixels = 1'048'576ULL;
inline constexpr std::uint64_t kAuthoringNativeMaxPixels = 3840ULL * 2160ULL;
inline constexpr std::uint32_t kAuthoringNativeMaxDimension = 4096;
inline constexpr std::uint64_t kAuthoringNativeAttachmentBudget = 1024ULL * 1024ULL * 1024ULL;
// Aggregate visible-instance work, shared by reference and native rendering.
// Detailed instanced fur on a pair of reusable components can coexist;
// per-mesh, primitive-ID, geometry
// cache and material-residency bounds remain independent admission gates.
inline constexpr std::uint64_t kAuthoringMaxSceneTriangles = 4'194'304ULL;
// Native material draws are separately bounded; instanced detailed component
// pairs may share geometry while retaining independent transforms and regions.
inline constexpr std::uint64_t kAuthoringNativeMaxDraws = 4'096ULL;
[[nodiscard]] constexpr bool valid_authoring_native_extent(std::uint32_t w, std::uint32_t h) noexcept {
    const auto pixels = std::uint64_t(w) * h;
    return w && h && w <= kAuthoringNativeMaxDimension && h <= kAuthoringNativeMaxDimension &&
        pixels <= kAuthoringNativeMaxPixels && pixels <= kAuthoringNativeAttachmentBudget / 128ULL;
}


// A bounded reference rasterizer for the authoring viewport. This is CPU
// execution, not a GPU renderer or a complete lighting implementation.
struct AuthoringView {
    core::Vec3d position{0.0, -1000.0, 0.0};
    core::Quaternion rotation{0.7071067811865475, 0.0, 0.0, 0.7071067811865475};
    project::CameraProjection projection = project::CameraProjection::orthographic;
    double vertical_fov_radians = 1.0471975511965976;
    double orthographic_height = 10.0;
    double near_plane = 0.1;
    double far_plane = 2000.0;
    // Zero uses the target pixel aspect. A host displaying a reduced image
    // can retain the exact canvas aspect despite integer target rounding.
    double aspect_ratio = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    // Camera convention: right +X, up +Y, forward -Z. Output depth is
    // positive camera distance. Outside near/far is rejected, never flattened.
    [[nodiscard]] core::Result<core::Vec3d> project(core::Vec3d world, std::uint32_t width,
                                                    std::uint32_t height) const;
};

struct AuthoringViewportRequest {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    AuthoringView view;
    // When set, use the saved camera and its scene transform instead of view.
    std::optional<scene::ObjectId> camera;
    // Exposure belongs exclusively to this final display operation. HDR
    // radiance, materials and lights remain unexposed.
    double exposure_ev = 0.0;
    render::ToneMapMode tone_mode = render::ToneMapMode::aces_fitted;
    // With an authored camera, use its durable display intent. Explicit
    // reference sweeps may leave this false and use the values above.
    bool camera_display = false;
    std::map<scene::ObjectId, core::Transform> transform_overrides;
    // Decoded digest-bound source pixels are derived data, never project truth.
    // Hosts populate this library from the saved project's content-addressed store.
    std::shared_ptr<const AuthoringTextureLibrary> textures;
    // Derived viewport quality, not a second authored light source. The saved
    // light's casts_shadows flag still owns whether it casts a shadow.
    struct Shadows {
        bool enabled = true;
        std::uint32_t resolution = 512, radius = 1;
        double depth_bias = 0.0005, slope_bias = 0.001;
        [[nodiscard]] core::Result<void> validate() const;
    } shadows;
    // Optional immutable derived product; its content digest must match the
    // saved environment. Never overrides the catalog's source or orientation.
    std::shared_ptr<const EnvironmentLighting> environment_lighting;
    // Disposable native history quality. The stateless CPU renderer remains a
    // current-frame oracle; AuthoringTemporalHistory is its explicit companion.
    struct Temporal {
        bool enabled = false, camera_cut = false;
        std::uint64_t session = 0;
        render::TemporalReprojectionSettings settings;
    } temporal;
};

struct DirectionalShadowFrame {
    AuthoringView view;
    AuthoringViewportRequest::Shadows settings;
};
// Fits visible authored mesh bounds, including transient object transforms.
// Independent of the camera: orbiting does not move the light-space texel grid.
[[nodiscard]] core::Result<std::optional<DirectionalShadowFrame>>
fit_directional_shadow(const project::ProjectDocument &, const AuthoringViewportRequest &,
                       core::Vec3d direction_to_light);
// Validated map storage is normalized [0,1], as in the existing shadow-PCF
// reference. Edge taps clamp; receivers outside the fitted volume are lit.
[[nodiscard]] core::Result<double> sample_directional_shadow(const DirectionalShadowFrame &,
                                                             std::span<const float> depths,
                                                             core::Vec3d world,
                                                             core::Vec3d geometric_normal,
                                                             core::Vec3d direction_to_light);

struct AuthoringViewportImage {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    core::Revision source_revision;
    AuthoringView view;
    std::vector<core::Vec3d> linear_hdr;
    std::vector<double> depth;
    std::vector<scene::ObjectId> objects;
    std::vector<geometry::FaceId> faces;
    // Transparent background leaves authoring grid/reference overlays visible.
    std::vector<std::uint8_t> display_rgba8;
    std::uint64_t sample_visits = 0U;
    double host_milliseconds = 0.0;
    std::uint64_t attachment_bytes = 0U;
    std::vector<std::string> diagnostics;
};

struct AuthoringDisplay {
    double exposure_ev;
    render::ToneMapMode tone_mode;
};
[[nodiscard]] core::Result<AuthoringDisplay>
resolve_authoring_display(const project::ProjectDocument& document,
                          const AuthoringViewportRequest& request);

// Zero extent produces an empty suspended image. Unsupported visual material
// features fail visibly; they are never silently rendered as constant factors.
// The document remains the only project truth; buffers are disposable products.
[[nodiscard]] core::Result<AuthoringViewportImage>
render_authoring_viewport(const project::ProjectDocument &document,
                          const AuthoringViewportRequest &request);

} // namespace carto::production
