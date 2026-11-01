#pragma once
#include <carto/character/autorig_graph_record.hpp>
#include <carto/character/definition.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/pose/ik_fk_authoring.hpp>
#include <carto/weights/transfer_record.hpp>
#include <carto/weights/transfer.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {
class ProjectDocument;
// Pure preparation shared by journaled project transactions and the human
// application command boundary. It does not change the document or publish
// the returned bytes. External source provenance remains caller-asserted.
[[nodiscard]] core::Result<std::string> prepare_accepted_autorig_bundle(
    const ProjectDocument& document,
    const character::AutorigAnalysisRequest& request,
    const character::AutorigCandidateSet& candidates,
    const character::AutorigReviewAcceptance& acceptance,
    const std::vector<character::AutorigFinalNode>& nodes,
    const character::RigDefinition& rig,
    const kinematic::JointLimitSet& limits,
    const kinematic::SkinBinding& source_binding,
    const character::SkinWeightLayer& source_layer,
    const weights::WeightCorrespondence& correspondence,
    const weights::JointTransferMap& joint_map,
    const weights::WeightTransferOptions& options,
    const weights::WeightLocks* locks = nullptr);
// Historical project source: cold decode checks internal pins and references;
// it does not authenticate the reviewer or establish current mesh provenance.
struct AcceptedAutorigBundle {
    static constexpr std::size_t kMaxIkFkChains = pose::IkFkAuthoring::kMaxSetSize;
    // Caps repeated rig/graph/limit validation across all chains.
    static constexpr std::size_t kMaxIkFkValidationBytes = 64U * 1024U * 1024U;
    character::AcceptedAutorigGraphRecord reviewed;
    character::RigDefinition rig;
    kinematic::JointLimitSet limits;
    weights::TransferredWeightRecord weights;
    // Sorted by (root, mid, effector); no duplicate chains. Empty for v1.
    std::vector<pose::IkFkAuthoring> ik_fk;
};
[[nodiscard]] core::Result<AcceptedAutorigBundle> decode_accepted_autorig_bundle(std::string_view bytes);
// Reconfigures a current decoded bundle without re-running weight transfer.
// Existing chain identities cannot be removed here; explicit historical clear
// is a separate human-authorized decision. Returns canonical bytes only;
// publication still requires an admitted CAS.
[[nodiscard]] core::Result<std::string> prepare_autorig_ik_fk_configuration(
    const ProjectDocument& document, const AcceptedAutorigBundle& bundle,
    const std::vector<pose::IkFkAuthoring>& configurations);
// Pure historical recovery preparation. Validates internal record integrity,
// not mesh freshness, and strips only IK/FK authoring. It grants no authority
// to publish, evaluate a stale rig, or infer a replacement configuration.
[[nodiscard]] core::Result<std::string> prepare_historical_autorig_ik_fk_clear(
    const AcceptedAutorigBundle& bundle);
// Required before using the historical bundle with the current project: checks
// live source revision, geometry and target index bounds. Source content digest
// remains caller-asserted; this does not authenticate external source bytes.
[[nodiscard]] core::Result<void> validate_accepted_autorig_current(
    const ProjectDocument& document, const AcceptedAutorigBundle& bundle);
}
