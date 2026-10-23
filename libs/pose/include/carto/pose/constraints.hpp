#pragma once

#include <carto/character/constraints_record.hpp>
#include <carto/character/time.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/pose/fk.hpp>
#include <carto/pose/ik.hpp>

#include <cstdint>
#include <map>
#include <vector>

namespace carto::pose {

// Deterministic CPU evaluation of an authored character::ConstraintGraph over
// a FK base pose, following the ch10 ordering contract:
//
//   rest + authored animation -> FK base pose (supplied by the caller)
//     -> local constraints and authored joint limits   (stage: preprocess)
//     -> limb IK / aim / orientation objectives        (stage: pre_deformation)
//     -> prioritized constrained IK / contacts         (stage: post_deformation)
//     -> sustained contact state                       (stage: post_simulation)
//     -> final limit verification                      (receipt, below)
//
// Stage passes run in ConstraintStage order; within a stage, constraints run
// in depends_on topological order (ready set tie-broken by priority
// descending then id) and conflicts on one target referent resolve through
// the graph's declared ArbitrationRule. Constraints whose validity window
// does not contain the sampled time are inactive and produce no outcome.
//
// Referent policy (honest refusal, never silent fallback): only joint
// referents are solvable on the CPU reference path; a control referent fails
// closed with unsupported, because control-driven goals must be resolved to
// joint space by evaluate_controls before constraint evaluation.
//
// Per-type semantics:
// - ik_pin:  source joint is the chain root, target joint the effector; the
//            effector is pinned to context.pin_targets[id] with the bounded
//            Jacobian chain solver.
// - contact: same solving path as ik_pin; converged additionally requires the
//            final residual within the constraint's authored tolerance.
// - orient:  rotates the target joint so its world orientation matches the
//            source joint's world orientation at application time (look-at /
//            aim alignment without Euler subtraction across branch cuts).
// - fk_blend: blends the target joint's local pose toward
//            context.blend_targets[id] by context.blend_weights[id] in [0,1].
// - limit:   clamps the target joint's local pose to its authored
//            JointLimit; requires limits in the context.

struct ConstraintEvalContext {
    // Authored limits; required by limit constraints and final verification.
    // Null means the evaluation cannot honor limits and limit constraints
    // fail closed instead of guessing.
    const kinematic::JointLimitSet* limits = nullptr;
    // FK base pose local overrides (rest defaults for absent joints).
    LocalPose base_locals;
    // Sampled rational time; must share one timebase with every active
    // constraint's validity window.
    character::RationalTime time;
    // Solver budget for ik_pin / contact chains.
    IkBudget budget;
    // Per-constraint goals (ik_pin / contact: world position).
    std::map<character::ConstraintId, core::Vec3d> pin_targets;
    // fk_blend: goal local pose and blend weight per constraint.
    std::map<character::ConstraintId, core::Transform> blend_targets;
    std::map<character::ConstraintId, double> blend_weights;
};

struct ConstraintOutcome {
    double residual = 0.0;
    std::uint32_t iterations = 0U;
    bool converged = false;
    // True when an arbitration rule resolved this constraint away (it was not
    // evaluated); residual/iterations are then placeholders, not measurements.
    bool superseded = false;
};

struct PoseReceipt {
    // Final local pose after every active stage pass (base + applied
    // constraints), covering all joints the constraints touched.
    LocalPose solved_locals;
    // One outcome per active constraint, keyed by id (std::map order is the
    // canonical deterministic order).
    std::map<character::ConstraintId, ConstraintOutcome> outcomes;
    character::RationalTime sampled_time;
    // Number of stage passes that ran with at least one active constraint.
    std::uint32_t stage_passes = 0U;
    // True when every active constraint converged (superseded constraints do
    // not count against). False means the receipt is a proposal: commit is
    // refused until a human decision accepts it (manual supremacy).
    bool all_converged = false;
    // Final limit verification: joints whose composed local pose lies outside
    // enabled authored angular/linear limits under the library Euler
    // convention. Empty when no limits were supplied.
    std::vector<kinematic::JointId> limit_violations;
};

[[nodiscard]] core::Result<PoseReceipt> evaluate_constraints(
    const character::ConstraintGraph& constraints,
    const kinematic::KinematicGraph& graph,
    const ConstraintEvalContext& context);

} // namespace carto::pose
