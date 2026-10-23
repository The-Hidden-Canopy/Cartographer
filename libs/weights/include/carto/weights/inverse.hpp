#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::weights {

// Native inverse deformation (Chapter 09, KnownRigFit case): recover per-joint
// posed transforms from a rest mesh, a posed mesh and a bound weight layer.
// The solver runs a bounded Gauss-Seidel alternation; each joint update is a
// closed-form weighted rigid fit (unit scale) against the current residual of
// the other joints. This is a deterministic cpu_reference implementation with
// an explicit convergence receipt; no claim is made about device parity or
// unknown-rig skeleton inference, and cancellation/iteration exhaustion is
// reported as INCOMPLETE-quality evidence, never as a silent partial write.

struct InverseSolveOptions {
    static constexpr std::uint32_t kMaxIterations = 256U;

    // Gauss-Seidel alternation bound.
    std::uint32_t max_iterations = 32U;
    // Convergence threshold on the largest joint transform delta per sweep.
    double tolerance = 1e-9;
    // Minimum influence weight for a vertex to participate in a joint fit.
    double influence_epsilon = 1e-6;

    [[nodiscard]] core::Result<void> validate() const;
};

struct InverseSolveReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;

    assets::Sha256Digest input_binding_digest;
    std::uint32_t max_iterations = 0U;
    std::uint32_t iterations_used = 0U;
    // False when the iteration bound was hit without meeting the tolerance:
    // the result is the best-valid iterand, explicitly marked incomplete.
    bool converged = false;
    double initial_rms_residual = 0.0;
    double final_rms_residual = 0.0;
    // Largest translation / rotation delta in the final sweep.
    double final_translation_delta = 0.0;
    double final_rotation_delta = 0.0;
    std::uint64_t solved_joint_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<InverseSolveReceipt> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct InverseSolveOutput {
    // Recovered posed transforms by joint id; identity for untouched joints.
    std::map<kinematic::JointId, core::Transform> joint_transforms;
    InverseSolveReceipt receipt;
};

// Recovers joint transforms such that the weight-blended deformation of
// `rest_positions` best matches `posed_positions` in the least-squares sense.
// `rest_positions` and `posed_positions` must have identical canonical
// ordering (the correspondence is the vertex index, matching the binding).
[[nodiscard]] core::Result<InverseSolveOutput> recover_joint_transforms(
    const kinematic::SkinBinding& binding,
    const kinematic::KinematicGraph& graph,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const InverseSolveOptions& options);

enum class InverseHypothesisStatus : std::uint8_t {
    rejected,
    incomplete,
    converged,
};

// One caller-supplied unknown-rig hypothesis. The evaluator treats each graph
// and binding as a proposal; it never infers anatomy or writes to either one.
struct InverseRigHypothesisInput {
    std::uint64_t id = 0U;
    const kinematic::KinematicGraph* graph = nullptr;
    const kinematic::SkinBinding* binding = nullptr;
};

struct InverseRigHypothesisResult {
    std::uint64_t id = 0U;
    std::uint32_t input_ordinal = 0U;
    InverseHypothesisStatus status = InverseHypothesisStatus::rejected;
    std::optional<assets::Sha256Digest> graph_digest;
    std::optional<assets::Sha256Digest> binding_digest;
    std::optional<assets::Sha256Digest> hypothesis_digest;
    std::optional<double> final_rms_residual;
    std::optional<InverseSolveReceipt> solve_receipt;
    std::string diagnostic;
};

struct InverseRigHypothesisOptions {
    static constexpr std::size_t kHardMaxHypotheses = 16U;
    static constexpr std::size_t kHardMaxObservationVertices = 1'048'576U;
    static constexpr std::uint64_t kHardMaxSolverWorkUnits = 100'000'000U;

    std::size_t max_hypotheses = 8U;
    std::uint64_t max_solver_work_units = 25'000'000U;
    InverseSolveOptions solver;

    [[nodiscard]] core::Result<void> validate() const;
};

struct InverseRigHypothesisSearch {
    // Caller-supplied source revision asserted by each candidate binding.
    assets::Sha256Digest source_mesh_digest;
    assets::Sha256Digest observation_digest;
    std::vector<InverseRigHypothesisResult> ranked_hypotheses;
};

// A human's asserted selection of one preview candidate for an exact rerun.
// This record is review lineage only; reviewer_identity is not authenticated
// by the numerical library and the lock grants no project-write authority.
struct InverseRigHypothesisLock {
    static constexpr std::size_t kMaxReviewerBytes = 128U;
    static constexpr std::size_t kMaxJustificationBytes = 4096U;

    std::uint64_t hypothesis_id = 0U;
    InverseHypothesisStatus preview_status = InverseHypothesisStatus::rejected;
    assets::Sha256Digest source_mesh_digest;
    assets::Sha256Digest observation_digest;
    assets::Sha256Digest graph_digest;
    assets::Sha256Digest binding_digest;
    assets::Sha256Digest hypothesis_digest;
    InverseSolveReceipt preview_receipt;
    std::string reviewer_identity;
    std::string justification;
    assets::Sha256Digest selection_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_selection_digest();
};

struct InverseRigHypothesisRerun {
    assets::Sha256Digest selection_digest;
    assets::Sha256Digest observation_digest;
    assets::Sha256Digest hypothesis_digest;
    InverseHypothesisStatus status = InverseHypothesisStatus::rejected;
    InverseSolveOutput solution;
};

// Evaluates a bounded set of explicitly supplied rig/weight hypotheses against
// one rest/posed observation. Every input produces a result row, including
// rejected and non-converged candidates. Results are content-bound and ranked
// by residual with deterministic digest tie-breaks; they are review evidence,
// not an inferred or accepted rig. Re-running this function leaves the source
// graph, binding, and position arrays untouched.
[[nodiscard]] core::Result<InverseRigHypothesisSearch> evaluate_inverse_rig_hypotheses(
    std::span<const InverseRigHypothesisInput> hypotheses,
    const assets::Sha256Digest& source_mesh_digest,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const InverseRigHypothesisOptions& options);

// Locks one evaluated hypothesis to a caller-asserted reviewer and rationale.
// The lock is bound to the exact observation, graph, binding, and preview
// receipt. Rejected candidates cannot be locked; incomplete candidates remain
// explicitly incomplete if selected for another review pass.
[[nodiscard]] core::Result<InverseRigHypothesisLock> lock_inverse_rig_hypothesis(
    const InverseRigHypothesisSearch& search,
    std::span<const InverseRigHypothesisInput> hypotheses,
    std::uint64_t hypothesis_id,
    std::string reviewer_identity,
    std::string justification);

// Repeats the solver only when the selected graph, binding, and exact mesh
// observation still match the human's lock. The returned transforms remain a
// preview result and do not mutate or commit the source.
[[nodiscard]] core::Result<InverseRigHypothesisRerun> rerun_locked_inverse_rig_hypothesis(
    const InverseRigHypothesisLock& selection,
    const InverseRigHypothesisInput& hypothesis,
    const assets::Sha256Digest& source_mesh_digest,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const InverseRigHypothesisOptions& options);

struct WeightRefinementOptions {
    static constexpr std::uint32_t kMaxIterations = 128U;

    std::uint32_t max_iterations = 16U;
    // Gradient step size bound.
    double step_size = 0.05;
    // Weights below this after a step are pruned; residual mass is
    // redistributed deterministically.
    double prune_epsilon = 1e-6;

    [[nodiscard]] core::Result<void> validate() const;
};

// Honest quality receipt for the weight-refinement stub: it records exactly
// what the bounded gradient pass achieved and declares its maturity. A
// non-positive residual reduction is reported, not hidden.
struct WeightRefinementReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    assets::Sha256Digest input_binding_digest;
    assets::Sha256Digest output_binding_digest;
    std::uint32_t iterations_used = 0U;
    double step_size = 0.0;
    double initial_rms_residual = 0.0;
    double final_rms_residual = 0.0;
    bool improved = false;
    std::uint64_t adjusted_vertex_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<WeightRefinementReceipt> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct WeightRefinementOutput {
    kinematic::SkinBinding binding;
    WeightRefinementReceipt receipt;
};

// Bounded gradient refinement of existing weights against one posed
// observation, given known joint transforms. Declared stub: this is the
// first-pass local improvement stage of Chapter 09's weight solve, not the
// full alternating fit; corrective/nonrigid residuals remain explicit.
[[nodiscard]] core::Result<WeightRefinementOutput> refine_weights_gradient(
    const kinematic::SkinBinding& binding,
    const kinematic::KinematicGraph& graph,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const std::map<kinematic::JointId, core::Transform>& joint_transforms,
    const WeightRefinementOptions& options);

} // namespace carto::weights
