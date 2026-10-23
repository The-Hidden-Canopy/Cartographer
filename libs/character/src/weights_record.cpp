#include <carto/character/weights_record.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

std::string_view weight_provenance_name(WeightProvenance provenance) noexcept {
    switch (provenance) {
    case WeightProvenance::authored_paint: return "authored-paint";
    case WeightProvenance::automatic_proposal: return "automatic-proposal";
    case WeightProvenance::transferred: return "transferred";
    case WeightProvenance::imported: return "imported";
    }
    return "unknown";
}

std::string_view weight_staleness_name(WeightStaleness staleness) noexcept {
    switch (staleness) {
    case WeightStaleness::fresh: return "fresh";
    case WeightStaleness::stale_mesh_topology: return "stale-mesh-topology";
    case WeightStaleness::stale_joint_graph: return "stale-joint-graph";
    case WeightStaleness::invalid: return "invalid";
    }
    return "unknown";
}

core::Result<void> validate_influences(
    std::span<const kinematic::JointInfluence> influences) {
    if (influences.empty() || influences.size() > SkinWeightLayer::kMaxInfluencesPerVertex) {
        return core::Result<void>::failure(invalid(
            "a vertex requires between 1 and 8 skin weight influences"));
    }
    double sum = 0.0;
    for (std::size_t index = 0U; index < influences.size(); ++index) {
        const kinematic::JointInfluence& influence = influences[index];
        if (!influence.joint) {
            return core::Result<void>::failure(invalid(
                "a skin weight influence references the invalid joint"));
        }
        if (!std::isfinite(influence.weight) || influence.weight <= 0.0) {
            return core::Result<void>::failure(validation(
                "skin weight influences must be positive and finite"));
        }
        if (index > 0U && !(influences[index - 1U].joint < influence.joint)) {
            return core::Result<void>::failure(validation(
                "skin weight influences must be sorted by unique joint id"));
        }
        sum += influence.weight;
    }
    if (std::abs(sum - 1.0) > kinematic::SkinBinding::kWeightSumTolerance) {
        return core::Result<void>::failure(validation(
            "skin weight influences must be normalized to a sum of one"));
    }
    return core::Result<void>::success();
}

core::Result<void> SkinWeightLayer::validate() const {
    if (!layer_id || mesh_topology_digest.is_zero() || joint_graph_digest.is_zero() ||
        influenced_vertex_count == 0U || influenced_vertex_count > kMaxInfluencedVertices ||
        total_influence_count < influenced_vertex_count ||
        total_influence_count >
            influenced_vertex_count * static_cast<std::uint64_t>(kMaxInfluencesPerVertex) ||
        provenance_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "skin weight layer identity, digests, or influence population is invalid"));
    }
    if ((locks_digest.has_value() && locks_digest->is_zero()) ||
        (masks_digest.has_value() && masks_digest->is_zero())) {
        return core::Result<void>::failure(invalid(
            "skin weight layer locks and masks references must be content-addressed"));
    }
    if ((staleness == WeightStaleness::fresh) != (stale_since.value() == 0U)) {
        return core::Result<void>::failure(validation(
            "skin weight layer staleness and invalid revision do not agree"));
    }
    return core::Result<void>::success();
}

} // namespace carto::character
