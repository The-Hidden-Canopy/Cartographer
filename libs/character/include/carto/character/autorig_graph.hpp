#pragma once

#include <carto/character/autorig_candidates.hpp>
#include <carto/kinematic/graph.hpp>

#include <vector>

namespace carto::character {

// The artist supplies durable IDs and all final node properties. Rest
// transforms are local to the parent; they are not inferred from anchors.
// Their composed world positions must agree with the reviewed anchors.
struct AutorigFinalNode final {
    AutorigCandidateJointId preview_joint;
    kinematic::JointId durable_joint;
    core::Transform local_rest;
    kinematic::JointKind kind = kinematic::JointKind::fixed;
    kinematic::NodeClass node_class = kinematic::NodeClass::rigid;
    std::string semantic_type;
};

struct AutorigPreparedGraph final {
    kinematic::KinematicGraph graph;
    assets::Sha256Digest graph_digest;
    assets::Sha256Digest acceptance_digest;
};

// Prepare, but do not persist, a graph from an accepted current candidate.
// The caller must atomically persist both the graph and its acceptance
// provenance through the owning project transaction. Preview IDs are never
// used as durable IDs. Omitted parts and mechanical port evidence are refused
// because KinematicGraph cannot retain those facts.
[[nodiscard]] core::Result<AutorigPreparedGraph> prepare_accepted_autorig_graph(
    const geometry::EditableMesh& current_mesh,
    const AutorigAnalysisRequest& current_request,
    const AutorigCandidateSet& candidates,
    const AutorigReviewAcceptance& acceptance,
    const std::vector<AutorigFinalNode>& authored_nodes);

} // namespace carto::character
