#pragma once

#include <carto/deform/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::deform {

struct InverseSkinHypothesis {
    std::string identity;
    SkinMethod method = SkinMethod::linear_blend;
    kinematic::SkinBinding binding;
    JointPoseSet poses;
};

struct InverseSkinHypothesisScore {
    std::string identity;
    assets::Sha256Digest hypothesis_digest;
    DeviationSummary observed_deviation;
    assets::Sha256Digest evaluated_positions_digest;
    bool evaluated = false;
    std::string evaluation_diagnostic;
};

struct InverseSkinHypothesisSearch {
    std::uint64_t source_mesh_asset = 0U;
    assets::Sha256Digest source_mesh_digest;
    assets::Sha256Digest bind_positions_digest;
    assets::Sha256Digest observed_positions_digest;
    assets::Sha256Digest candidate_set_digest;
    std::vector<InverseSkinHypothesisScore> ranked_candidates;
    // Every non-dominated candidate is retained; the evaluator never picks
    // a winner when candidates trade mean error against maximum error.
    std::vector<assets::Sha256Digest> pareto_frontier;
    bool ambiguous_frontier = false;

    [[nodiscard]] core::Result<void> validate() const;
};

struct LockedInverseSkinHypothesis {
    std::uint64_t source_mesh_asset = 0U;
    assets::Sha256Digest source_mesh_digest;
    assets::Sha256Digest bind_positions_digest;
    assets::Sha256Digest candidate_set_digest;
    assets::Sha256Digest hypothesis_digest;
    assets::Sha256Digest observed_positions_digest;
    std::string reviewer_decision;

    [[nodiscard]] core::Result<void> validate() const;
};

struct InverseSkinRerun {
    assets::Sha256Digest source_mesh_digest_before;
    assets::Sha256Digest source_mesh_digest_after;
    assets::Sha256Digest bind_positions_digest;
    assets::Sha256Digest hypothesis_digest;
    assets::Sha256Digest evaluated_positions_digest;
    DeviationSummary observed_deviation;
    std::vector<core::Vec3d> positions;
};

[[nodiscard]] core::Result<assets::Sha256Digest> inverse_skin_hypothesis_digest(
    const InverseSkinHypothesis& hypothesis);

// Evaluates an explicit bounded hypothesis set against observed positions.
// This is review evidence over caller-supplied rigs, not automatic anatomy or
// rig inference. All candidates and Pareto trade-offs remain inspectable.
[[nodiscard]] core::Result<InverseSkinHypothesisSearch> search_inverse_skin_hypotheses(
    std::uint64_t source_mesh_asset,
    assets::Sha256Digest source_mesh_digest,
    std::span<const core::Vec3d> bind_positions,
    std::span<const core::Vec3d> observed_positions,
    std::span<const InverseSkinHypothesis> hypotheses,
    double error_tolerance = 1e-6);

[[nodiscard]] core::Result<LockedInverseSkinHypothesis> lock_inverse_skin_hypothesis(
    const InverseSkinHypothesisSearch& search,
    const assets::Sha256Digest& hypothesis_digest,
    std::string reviewer_decision);

// Re-evaluates one locked hypothesis from the exact original source. Source
// digest equality is returned as evidence; no source mesh or project is edited.
[[nodiscard]] core::Result<InverseSkinRerun> rerun_locked_inverse_skin_hypothesis(
    const LockedInverseSkinHypothesis& locked,
    const InverseSkinHypothesis& hypothesis,
    std::span<const core::Vec3d> bind_positions,
    std::span<const core::Vec3d> observed_positions,
    assets::Sha256Digest current_source_mesh_digest);

} // namespace carto::deform
