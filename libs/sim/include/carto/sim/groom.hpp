#pragma once

#include <carto/character/bindings.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/time.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/sim/caps.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::sim {

// One groom guide: a polyline of local offsets rooted at the attachment
// point. All guides in one bake share the same point count (the codec and
// the caps are uniform); per-guide point counts up to
// SimCaps::kMaxGuidePoints, at least two (root plus one free point).
struct GuideSpec {
    std::vector<core::Vec3d> rest_offsets;
};

// Root motion sample for one frame. Frames must share one timebase, be
// strictly monotonic, and advance by a uniform tick count per frame; root
// transforms between samples are interpolated deterministically (linear
// position, normalized linear quaternion blend) at fixed substep fractions.
struct RootMotionSample {
    character::RationalTime time;
    core::Vec3d position{};
    core::Quaternion orientation = core::Quaternion::identity();
};

// Spring/damper parameters for guide secondary motion. Root points are
// infinite-mass (driven by the root motion); every other point is a point
// mass chained to its parent with a Hookean segment spring plus gravity and
// per-substep velocity damping, integrated with semi-implicit Euler at a
// fixed substep count.
struct GroomSpringParams {
    double stiffness = 5.0;
    double damping = 0.02;
    double mass_per_point_kg = 1.0e-3;
    core::Vec3d gravity{0.0, -9.81, 0.0};
    std::uint32_t substeps = 4U;
};

struct GroomBakeRequest {
    // Authored groom binding; the request's guide count must not exceed the
    // binding's authored guide table, preserving source correspondence.
    character::GroomBinding binding;
    assets::Sha256Digest binding_digest;
    std::vector<GuideSpec> guides;
    std::vector<RootMotionSample> root_motion;
    GroomSpringParams spring;
};

// Immutable baked guide trajectory: one position per guide point per frame,
// frame-major (`((frame * guide_count) + guide) * points_per_guide + point`).
struct GroomGuideCache {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxPositions =
        SimCaps::kMaxGuides * SimCaps::kMaxGuidePoints * SimCaps::kMaxFrames;

    assets::Sha256Digest binding_digest;
    std::uint32_t guide_count = 0U;
    std::uint32_t points_per_guide = 0U;
    std::uint32_t frame_count = 0U;
    std::uint32_t ticks_per_frame = 1U;
    std::uint32_t ticks_per_second = 0U;
    std::vector<core::Vec3d> positions;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct GroomSimReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;

    std::string backend_identity;
    std::string precision_identity;
    assets::Sha256Digest source_binding_digest;
    std::uint32_t guide_count = 0U;
    std::uint32_t points_per_guide = 0U;
    std::uint32_t frames_executed = 0U;
    std::uint32_t substeps_per_frame = 0U;
    // Mean per-point displacement between the final two frames: how settled
    // the guides were when the bake ended.
    double settle_residual = 0.0;
    assets::Sha256Digest cache_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct GroomBakeResult {
    GroomGuideCache cache;
    GroomSimReceipt receipt;
};

// Bounded CPU guide-spring bake. Fails closed beyond SimCaps and on
// non-finite, non-monotonic or non-uniform root motion.
[[nodiscard]] core::Result<GroomBakeResult> bake_groom(
    const GroomBakeRequest& request);

} // namespace carto::sim
