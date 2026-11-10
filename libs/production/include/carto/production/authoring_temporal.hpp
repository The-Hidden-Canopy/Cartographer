#pragma once
#include <carto/production/authoring_viewport.hpp>
#include <carto/render/temporal_reprojection.hpp>

namespace carto::production {
enum class AuthoringHistoryReset {
    none,
    initial,
    suspended,
    extent,
    source,
    lens,
    camera_cut,
    lighting_motion,
    quality
};
struct AuthoringTemporalFrame {
    AuthoringViewportImage image;
    AuthoringHistoryReset reset = AuthoringHistoryReset::initial;
    std::array<std::uint64_t, static_cast<unsigned>(render::TemporalRejection::count)> counts{};
    std::uint64_t history_attachment_bytes = 0;
    // Sequential CPU intervals from one resolve call; total includes validation,
    // source hashing, reprojection preparation and the small return overhead.
    double cpu_prepare_ms = 0, cpu_resolve_ms = 0, cpu_display_ms = 0, cpu_publish_ms = 0,
           cpu_total_ms = 0;
};
// CPU correctness/reference path, deliberately separate from native GPU history.
// Uses existing authored geometry/camera and final display transform. History is
// disposable; it is not serialized or authoritative. No UI or GPU is instantiated.
class AuthoringTemporalHistory {
  public:
    [[nodiscard]] core::Result<AuthoringTemporalFrame>
    render(const project::ProjectDocument &, const AuthoringViewportRequest &,
           bool camera_cut = false, const render::TemporalReprojectionSettings & = {});
    // A numeric renderer may supply its current frame, but it must have matching
    // source revision/extent and positive camera-distance depth (not device Z).
    [[nodiscard]] core::Result<AuthoringTemporalFrame>
    resolve(const project::ProjectDocument &, const AuthoringViewportRequest &,
            AuthoringViewportImage, bool camera_cut = false,
            const render::TemporalReprojectionSettings & = {});
    void clear();

  private:
    bool valid_ = false;
    std::uint32_t width_ = 0, height_ = 0;
    core::Revision revision_;
    assets::Sha256Digest digest_;
    AuthoringView view_;
    AuthoringViewportRequest::Shadows shadows_;
    std::shared_ptr<const AuthoringTextureLibrary> textures_;
    std::map<scene::ObjectId, core::Transform> transforms_;
    std::vector<core::Vec3d> hdr_;
    std::vector<double> depth_;
    std::vector<render::TemporalSurface> surfaces_;
};
} // namespace carto::production
