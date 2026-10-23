#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/pose/fk.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace carto::pose {

// Bounded work budget for every pose solver (product invariant 6: no
// unbounded work). Iteration and tolerance bounds are deterministic: two runs
// with the same inputs, budget, and code take the same stopping path. The
// wall-clock cap is an optional host-defense bound; when it fires the result
// is still deterministic in pose but the stop reason is not reproducible
// across machines, so deterministic replay callers leave it disabled.
struct IkBudget {
    static constexpr std::uint32_t kMaxIterations = 100'000U;

    std::uint32_t max_iterations = 64U;
    double convergence_tolerance = 1e-9;
    // Damped-least-squares damping (lambda); larger damps harder near
    // singularities at the cost of convergence speed.
    double damping = 0.1;
    // 0.0 disables the wall-clock cap.
    double max_seconds = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Shared result shape for both solvers.
struct IkChainResult {
    // Solved local pose overrides for the chain joints, relative to the base
    // locals passed into the solver. Composing these over the base pose and
    // running FK reproduces the solved chain.
    LocalPose solved_locals;
    // Final distance between the effector and the requested target, in the
    // evaluation frame's scene units.
    double residual = 0.0;
    std::uint32_t iterations = 0U;
    bool converged = false;
};

// Analytic two-bone IK: `root` -> `mid` -> `effector`, solved in closed form
// for the effector position. The root translation is held fixed; root and mid
// local rotations are solved so the effector lands on `target` when the target
// is reachable. Unreachable targets are stretched along the aim direction and
// reported with converged == false (or refused, see allow_partial). An
// optional world-space pole vector selects the bend plane; without one the
// authored rest bend normal is preserved.
struct TwoBoneIkRequest {
    kinematic::JointId root;
    kinematic::JointId mid;
    kinematic::JointId effector;
    core::Vec3d target{};
    std::optional<core::Vec3d> pole_vector;
    IkBudget budget;
    // false (default): a non-converged solve is an honest failure carrying the
    // residual in the diagnostic context. true: returns the best pose as an
    // explicit proposal with converged == false (manual supremacy: a proposal
    // a human decision may accept, never a silent commit).
    bool allow_partial = false;
};

// Damped-least-squares Jacobian chain IK over rotational DOF (the CPU
// reference full-body style solver, ch10). `chain` lists joints root-first
// down one ancestor line; exactly one DOF column per joint axis (x/y/z) is
// active, clamped to the joint's enabled angular limits each iteration via
// the library Euler convention. When `limits` is null the chain is
// unclamped. Unreachable or non-converging requests fail closed (or return an
// explicit proposal when allow_partial is set), never a best-effort silence.
struct JacobianIkRequest {
    std::vector<kinematic::JointId> chain;
    core::Vec3d target{};
    IkBudget budget;
    bool allow_partial = false;
};

[[nodiscard]] core::Result<IkChainResult> solve_two_bone(
    const kinematic::KinematicGraph& graph,
    const LocalPose& base_locals,
    const kinematic::JointLimitSet* limits,
    const TwoBoneIkRequest& request);

[[nodiscard]] core::Result<IkChainResult> solve_chain_jacobian(
    const kinematic::KinematicGraph& graph,
    const LocalPose& base_locals,
    const kinematic::JointLimitSet* limits,
    const JacobianIkRequest& request);

} // namespace carto::pose
