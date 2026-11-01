#pragma once

#include <carto/character/autorig_graph.hpp>
#include <string_view>

namespace carto::character {

struct AutorigJointMapping final {
    AutorigCandidateJointId preview;
    std::optional<AutorigCandidateJointId> preview_parent;
    kinematic::JointId durable;
};

// Historical authoring evidence. The record binds a canonical graph to the
// human review assertion and preserves the preview-to-durable identity map.
// Validating a cold record verifies its internal integrity; it does not prove
// that the source mesh is still current or authenticate the claimed reviewer.
struct AcceptedAutorigGraphRecord final {
    static constexpr std::size_t kMaxSerializedBytes = 20U * 1024U * 1024U;
    AutorigReviewAcceptance acceptance;
    std::string graph_bytes;
    assets::Sha256Digest graph_digest;
    std::vector<AutorigJointMapping> joints;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<AcceptedAutorigGraphRecord> deserialize(std::string_view bytes);
};

// Performs the current-source checks and produces a canonical historical
// record. Persistence and transaction admission are the caller's authority.
[[nodiscard]] core::Result<AcceptedAutorigGraphRecord> make_accepted_autorig_graph_record(
    const geometry::EditableMesh& current_mesh,
    const AutorigAnalysisRequest& current_request,
    const AutorigCandidateSet& candidates,
    const AutorigReviewAcceptance& acceptance,
    const std::vector<AutorigFinalNode>& authored_nodes);

} // namespace carto::character
