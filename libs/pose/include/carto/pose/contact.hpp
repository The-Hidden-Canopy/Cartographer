#pragma once

#include <carto/character/constraints_record.hpp>
#include <carto/character/time.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/pose/fk.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace carto::pose {

// Contact-consistency verification over a sampled rational time window
// (ch10 multi-contact / contact continuity). The pinned joint is re-evaluated
// on EVERY intermediate sample — interpolating only start and end is
// explicitly inadequate — and the report carries the worst residual observed.
//
// The caller supplies the base pose per sample through `base_pose_at` (the
// authored-animation FK base for that frame; owned upstream by the motion
// subsystem). The callback must be deterministic: same rational time, same
// locals, so repeated evaluations of one request are bit-identical.
//
// The sample grid is exact integer arithmetic on the shared rational
// timebase: (window_end - window_start) must divide by (samples - 1)
// exactly, otherwise the request fails closed rather than silently
// jittering the grid.

struct ContactHoldRequest {
    static constexpr std::uint32_t kMaxSamples = 100'000U;

    // The contact constraints under test; all must exist, target joints, and
    // cover the whole window with their validity intervals.
    std::vector<character::ConstraintId> contacts;
    character::RationalTime window_start;
    character::RationalTime window_end;
    // Sample count including both endpoints; must be >= 2.
    std::uint32_t samples = 2U;
};

struct ContactHoldReport {
    character::RationalTime window_start;
    character::RationalTime window_end;
    std::uint32_t sample_count = 0U;
    // Worst residual per contact across the window, keyed by constraint id.
    std::map<character::ConstraintId, double> worst_residuals;
    // Worst residual observed across all contacts and samples.
    double worst_residual = 0.0;
    // True when every contact stayed within its authored tolerance on every
    // sample. False means slipping/penetration/unreachable motion was
    // observed; the worst residuals name where.
    bool held = false;
};

[[nodiscard]] core::Result<ContactHoldReport> evaluate_contact_hold(
    const character::ConstraintGraph& constraints,
    const kinematic::KinematicGraph& graph,
    const std::map<character::ConstraintId, core::Vec3d>& pin_targets,
    const std::function<core::Result<LocalPose>(character::RationalTime)>& base_pose_at,
    const ContactHoldRequest& request);

} // namespace carto::pose
