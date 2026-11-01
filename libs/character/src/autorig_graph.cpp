#include <carto/character/autorig_graph.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace carto::character {
namespace {
core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}
}

core::Result<AutorigPreparedGraph> prepare_accepted_autorig_graph(
    const geometry::EditableMesh& current_mesh,
    const AutorigAnalysisRequest& current_request,
    const AutorigCandidateSet& candidates,
    const AutorigReviewAcceptance& acceptance,
    const std::vector<AutorigFinalNode>& authored_nodes) {
    using Result = core::Result<AutorigPreparedGraph>;
    if (auto checked = candidates.validate(); !checked) return Result::failure(checked.error());
    if (auto checked = acceptance.validate(); !checked) return Result::failure(checked.error());
    if (!(acceptance.source == candidates.source) ||
        acceptance.mesh_revision != candidates.mesh_revision ||
        acceptance.mesh_geometry_digest != candidates.mesh_geometry_digest ||
        acceptance.constraints_digest != candidates.constraints_digest ||
        acceptance.policy_digest != candidates.policy_digest ||
        acceptance.candidate_set_digest != candidates.digest) {
        return Result::failure(invalid("autorig acceptance does not bind the supplied candidate set"));
    }
    auto analysis = analyze_autorig_features(current_mesh, current_request);
    if (!analysis) return Result::failure(analysis.error());
    if (!(analysis.value().source == candidates.source) ||
        analysis.value().mesh_revision != candidates.mesh_revision ||
        analysis.value().mesh_geometry_digest != candidates.mesh_geometry_digest ||
        analysis.value().analysis_digest != candidates.analysis_digest) {
        return Result::failure(invalid("autorig source or analysis has changed since review"));
    }
    const auto selected = std::find_if(candidates.ranked_candidates.begin(),
        candidates.ranked_candidates.end(), [&acceptance](const auto& candidate) {
            return candidate.digest == acceptance.candidate_digest;
        });
    if (selected == candidates.ranked_candidates.end())
        return Result::failure(invalid("accepted candidate is absent from the supplied set"));
    if (!selected->unrepresented_parts.empty() || !selected->mechanical_connections.empty())
        return Result::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "final graph cannot retain omitted parts or mechanical port evidence"));
    if (authored_nodes.size() != selected->joints.size())
        return Result::failure(invalid("supply exactly one final node for each preview joint"));

    std::map<AutorigCandidateJointId, const AutorigFinalNode*> by_preview;
    std::set<kinematic::JointId> durable_ids;
    for (const auto& node : authored_nodes) {
        if (!node.preview_joint || !node.durable_joint ||
            !by_preview.emplace(node.preview_joint, &node).second ||
            !durable_ids.insert(node.durable_joint).second)
            return Result::failure(invalid("preview and durable joint mappings must be nonzero and unique"));
    }
    kinematic::KinematicGraph graph;
    std::map<AutorigCandidateJointId, core::Transform> world_rest;
    for (const auto& joint : selected->joints) {
        const auto found = by_preview.find(joint.id);
        if (found == by_preview.end())
            return Result::failure(invalid("final node mapping omits a preview joint"));
        std::optional<kinematic::JointId> parent;
        if (joint.parent) {
            const auto parent_node = by_preview.find(*joint.parent);
            if (parent_node == by_preview.end())
                return Result::failure(invalid("final node mapping omits a preview parent"));
            parent = parent_node->second->durable_joint;
        }
        const auto& node = *found->second;
        const auto world = joint.parent
            ? world_rest.at(*joint.parent).combine(node.local_rest) : node.local_rest;
        if (!world.finite())
            return Result::failure(invalid("final rest transform is not finite"));
        const double scale = std::max({1.0, std::abs(joint.position.x),
            std::abs(joint.position.y), std::abs(joint.position.z)});
        const double tolerance = 1.0e-7 +
            16.0 * std::numeric_limits<double>::epsilon() * scale;
        if (std::abs(world.translation.x - joint.position.x) > tolerance ||
            std::abs(world.translation.y - joint.position.y) > tolerance ||
            std::abs(world.translation.z - joint.position.z) > tolerance)
            return Result::failure(invalid("final rest position differs from the reviewed anchor"));
        world_rest.emplace(joint.id, world);
        auto inserted = graph.insert_node({node.durable_joint, parent, node.local_rest,
            node.kind, node.node_class, node.semantic_type});
        if (!inserted) return Result::failure(inserted.error());
    }
    auto graph_digest = graph.canonical_digest();
    if (!graph_digest) return Result::failure(graph_digest.error());
    return Result::success({std::move(graph), graph_digest.value(), acceptance.digest});
}
} // namespace carto::character
