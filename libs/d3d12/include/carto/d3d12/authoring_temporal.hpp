#pragma once
#include <carto/assets/blob_store.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/production/authoring_viewport.hpp>
#include <carto/render/temporal_reprojection.hpp>

namespace carto::d3d12 {
struct AuthoringTemporalEpoch {
    assets::Sha256Digest source;
    core::Revision revision;
    std::uint32_t width = 0, height = 0;
};
struct alignas(16) AuthoringTemporalConstants {
    std::array<float, 4> feedback_depth_clip{};
    std::array<std::uint32_t, 4> extent_flags{};
};
static_assert(sizeof(AuthoringTemporalConstants) == 32);
// Builds the actual seven-SRV/one-UAV compute contract. No device/queue exists
// here. Sources/extent/revision must agree before history is eligible.
struct AuthoringTemporalKernelPlan {
    AuthoringTemporalConstants constants;
    gpu::PipelineDesc pipeline;
    std::array<std::uint32_t, 3> groups{};
};
[[nodiscard]] core::Result<AuthoringTemporalKernelPlan>
prepare_authoring_temporal(const AuthoringTemporalEpoch &current,
                           const AuthoringTemporalEpoch &history, bool history_valid,
                           bool camera_cut, const render::TemporalReprojectionSettings & = {});
[[nodiscard]] ShaderSource authoring_temporal_shader();
// Numeric test/transport helpers. Native integration must produce these on GPU,
// not read current frames to the host just to upload this data again.
[[nodiscard]] core::Result<std::vector<std::uint32_t>>
pack_temporal_identity_atlas(std::span<const render::TemporalSurface>, std::uint32_t width,
                             std::uint32_t height);
[[nodiscard]] core::Result<std::vector<std::array<float, 4>>>
pack_temporal_previous_samples(std::span<const render::TemporalPreviousSample>, std::uint32_t width,
                               std::uint32_t height);

using AuthoringMatrix = std::array<float, 16>;
[[nodiscard]] core::Result<AuthoringMatrix>
authoring_view_projection(const production::AuthoringView &, double aspect);
struct AuthoringTemporalDraw {
    std::uint64_t object = 0;
    core::Transform transform;
    std::span<const geometry::FaceId> faces;
};
struct alignas(16) AuthoringMotionConstants {
    AuthoringMatrix inverse_view_projection{}, previous_view_projection{};
    std::array<float, 4> current_depth_row{}, previous_depth_row{};
    std::array<std::uint32_t, 4> extent_draws{};
};
static_assert(sizeof(AuthoringMotionConstants) == 176);
enum class AuthoringTemporalReset {
    none,
    initial,
    source,
    extent,
    session,
    lens,
    camera_cut,
    quality,
    lighting,
    moving_shadow
};
struct AuthoringTemporalHostInput {
    AuthoringTemporalEpoch epoch;
    production::AuthoringView view;
    std::span<const AuthoringTemporalDraw> draws;
    assets::Sha256Digest lighting, quality;
    std::uint64_t session = 0;
    bool camera_cut = false, spatial_shadows = false;
    render::TemporalReprojectionSettings settings;
};
class AuthoringTemporalHostState;
struct AuthoringTemporalHostPlan {
    AuthoringMotionConstants motion;
    AuthoringTemporalKernelPlan resolve;
    std::vector<std::uint32_t> draw_words;
    std::shared_ptr<const std::vector<std::uint32_t>> face_words;
    assets::Sha256Digest draw_digest, face_digest;
    AuthoringTemporalReset reset = AuthoringTemporalReset::initial;

  private:
    struct Candidate;
    std::shared_ptr<const Candidate> candidate;
    friend class AuthoringTemporalHostState;
};
// Same-queue submission is the publication point; no history state is committed
// by preparation alone. The renderer invalidates on failed submission/resolve.
class AuthoringTemporalHostState final {
  public:
    AuthoringTemporalHostState() = default;
    AuthoringTemporalHostState(const AuthoringTemporalHostState &) = delete;
    AuthoringTemporalHostState &operator=(const AuthoringTemporalHostState &) = delete;
    [[nodiscard]] core::Result<AuthoringTemporalHostPlan>
    prepare(const AuthoringTemporalHostInput &) const;
    [[nodiscard]] core::Result<void> commit(const AuthoringTemporalHostPlan &);
    void invalidate();

  private:
    std::shared_ptr<const AuthoringTemporalHostPlan::Candidate> previous_;
    std::uint64_t generation_ = 0;
};
[[nodiscard]] ShaderSource authoring_motion_shader();
[[nodiscard]] ShaderSource authoring_packed_temporal_shader();
} // namespace carto::d3d12
