#pragma once

#include <carto/character/autorig_constraints.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <compare>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::character {

// Scores are transparent ranking evidence, never calibrated confidence or an
// acceptance decision. The caller supplies the policy weights and bounds.
struct AutorigCandidatePolicy final {
    static constexpr std::size_t kHardMaxCandidates = 1'024U;
    static constexpr std::size_t kHardMaxJointsPerCandidate = 1'024U;
    static constexpr std::size_t kHardMaxPairChecks = 4'000'000U;

    std::size_t max_candidates = 64U;
    std::size_t max_joints_per_candidate = 256U;
    double minimum_joint_separation = 0.0;
    double minimum_bone_clearance = 0.0;
    double minimum_length_to_mesh_ratio = 0.05;
    double maximum_length_to_mesh_ratio = 4.0;
    double part_coverage_weight = 0.35;
    double nonadjacent_clearance_weight = 0.25;
    double length_reasonableness_weight = 0.25;
    double joint_budget_weight = 0.15;
    // Bilateral reflection is an opt-in ranking preference. A caller that
    // expects authored asymmetry should leave this at zero.
    double bilateral_reflection_weight = 0.0;
    // Bend-arc smoothness is review evidence and stays out of ranking unless
    // a caller explicitly assigns it weight.
    double bend_arc_regularity_weight = 0.0;
    double branch_coverage_weight = 0.0;
    double skeleton_self_clearance_weight = 0.0;
    // Opt-in score for candidate bone centerlines that cross source triangles
    // away from their authored anchor endpoints. This is geometric evidence,
    // not a solid-containment or volumetric bone-collision test.
    double source_surface_crossing_weight = 0.0;
    // Counts only explicitly authored hinge/slider port hints. It is a
    // structural cost proxy, never an estimate of user effort.
    double control_cost_weight = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AutorigCandidateJointId final {
    std::uint64_t value = 0U;
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const AutorigCandidateJointId&) const noexcept = default;
};

// Preview-local identity only; it cannot be installed as a Rig JointId.
struct AutorigCandidateJoint final {
    AutorigCandidateJointId id;
    std::optional<AutorigCandidateJointId> parent;
    std::string part_id;
    std::string landmark_id;
    std::string semantic_role;
    AutorigPartSide side = AutorigPartSide::unspecified;
    geometry::VertexId source_vertex;
    core::Vec3d position;
    AutorigConstraintProvenance provenance;
};

struct AutorigCandidateScore final {
    double part_coverage = 0.0;
    double nonadjacent_clearance = 0.0;
    double length_reasonableness = 0.0;
    double joint_budget = 0.0;
    double bilateral_reflection_fit = 0.0;
    double bend_arc_regularity = 0.0;
    double branch_coverage = 0.0;
    double skeleton_self_clearance = 0.0;
    double source_surface_clearance = 0.0;
    double control_cost_fraction = 0.0;
    double ranking_score = 0.0;
    double skeleton_to_mesh_diagonal = 0.0;
    std::size_t evaluated_nonadjacent_pairs = 0U;
    std::size_t close_nonadjacent_pairs = 0U;
    std::size_t evaluated_bilateral_pairs = 0U;
    std::size_t evaluated_bend_arc_transitions = 0U;
    std::size_t evaluated_nonadjacent_bones = 0U;
    std::size_t overlapping_nonadjacent_bones = 0U;
    std::size_t evaluated_source_surface_bones = 0U;
    std::size_t source_surface_crossing_bones = 0U;
    std::size_t evaluated_control_links = 0U;
};

struct AutorigMechanicalConnectionProposal final {
    std::string port_hint_id;
    AutorigCandidateJointId parent_joint;
    AutorigCandidateJointId child_joint;
    std::string parent_port_landmark_id;
    std::string child_port_landmark_id;
    geometry::VertexId parent_port_vertex;
    geometry::VertexId child_port_vertex;
    kinematic::JointKind kind = kinematic::JointKind::fixed;
    core::Vec3d axis{};
    double lower_limit = 0.0;
    double upper_limit = 0.0;
    AutorigConstraintProvenance provenance;
};

struct AutorigCandidate final {
    std::vector<AutorigCandidateJoint> joints;
    std::vector<AutorigMechanicalConnectionProposal> mechanical_connections;
    std::vector<std::string> unrepresented_parts;
    std::vector<std::string> selected_landmark_ids;
    AutorigCandidateScore score;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AutorigCandidateSet final {
    MeshAssetRef source;
    core::Revision mesh_revision;
    assets::Sha256Digest mesh_geometry_digest;
    assets::Sha256Digest analysis_digest;
    assets::Sha256Digest constraints_digest;
    assets::Sha256Digest policy_digest;
    // Every authored anchor-choice combination is included, or generation
    // fails without a partial set when a hard budget would be exceeded.
    std::vector<AutorigCandidate> ranked_candidates;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// Re-analyzes the current mesh, validates exact source-bound constraints, and
// creates deterministic preview alternatives by selecting one authored
// landmark per represented part. Unanchored parts remain explicit omissions.
// No anatomy, physical joint type, control graph, or riggability verdict is
// inferred; no mesh, project, or rig is mutated.
[[nodiscard]] core::Result<AutorigCandidateSet> generate_autorig_candidates(
    const geometry::EditableMesh& mesh,
    const AutorigAnalysisRequest& analysis_request,
    const AutorigConstraintSet& constraints,
    const AutorigCandidatePolicy& policy = {});

// The review session owns only speculative copies of the exact source and
// authored constraints. Selection, locks, edits, and replanning never write
// to a ProjectDocument. `accept_review` records a human choice; it is not a
// durable rig commit and its candidate IDs remain preview-local.
struct AutorigReviewAcceptance final {
    MeshAssetRef source;
    core::Revision mesh_revision;
    assets::Sha256Digest mesh_geometry_digest;
    assets::Sha256Digest constraints_digest;
    assets::Sha256Digest policy_digest;
    assets::Sha256Digest candidate_set_digest;
    assets::Sha256Digest candidate_digest;
    AutorigConstraintProvenance reviewer_decision;
    assets::Sha256Digest digest;

    [[nodiscard]] assets::Sha256Digest canonical_digest() const;
    [[nodiscard]] core::Result<void> validate() const;
};

class AutorigCandidateReview final {
public:
    [[nodiscard]] static core::Result<AutorigCandidateReview> begin(
        const geometry::EditableMesh& mesh,
        const AutorigAnalysisRequest& analysis_request,
        const AutorigConstraintSet& constraints,
        const AutorigCandidatePolicy& policy = {});

    [[nodiscard]] core::Result<void> select_candidate(
        const assets::Sha256Digest& candidate_digest);
    [[nodiscard]] core::Result<void> lock_landmark(
        std::string_view part_id,
        std::string_view landmark_id);
    [[nodiscard]] core::Result<void> edit_landmark(
        AutorigLandmarkConstraint replacement);
    [[nodiscard]] core::Result<void> replan(
        const geometry::EditableMesh& current_mesh);
    [[nodiscard]] core::Result<AutorigReviewAcceptance> accept_review(
        AutorigConstraintProvenance reviewer_decision) const;

    [[nodiscard]] const AutorigCandidateSet& candidates() const noexcept {
        return candidates_;
    }
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>&
    locked_landmarks() const noexcept { return locked_landmarks_; }
    [[nodiscard]] const std::optional<assets::Sha256Digest>&
    selected_candidate_digest() const noexcept { return selected_candidate_digest_; }

private:
    AutorigCandidateReview(
        geometry::EditableMesh mesh,
        AutorigAnalysisRequest request,
        AutorigConstraintSet constraints,
        AutorigCandidatePolicy policy,
        AutorigCandidateSet candidates)
        : mesh_(std::move(mesh)),
          request_(std::move(request)),
          constraints_(std::move(constraints)),
          policy_(policy),
          candidates_(std::move(candidates)) {}

    geometry::EditableMesh mesh_;
    AutorigAnalysisRequest request_;
    AutorigConstraintSet constraints_;
    AutorigCandidatePolicy policy_;
    AutorigCandidateSet candidates_;
    std::map<std::string, std::string, std::less<>> locked_landmarks_;
    std::optional<assets::Sha256Digest> selected_candidate_digest_;
};

} // namespace carto::character
