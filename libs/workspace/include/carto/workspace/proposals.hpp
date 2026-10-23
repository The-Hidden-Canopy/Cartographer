#pragma once

#include <carto/workspace/commands.hpp>
#include <carto/workspace/session.hpp>
#include <carto/workspace/store.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/time.hpp>
#include <carto/journal/journal.hpp>

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::workspace {

// Typed AI assistance (ch20). Proposals are untrusted candidate payloads with
// explicit uncertainty and a recipe digest; they carry NO executable content
// and NO filesystem or host commands. A proposal only ever materializes as
// native workspace commands, so no proposal path can bypass validation: the
// command surface is the single mutation authority. LLM/network inference is
// never required; the bundled generators are deterministic local functions.

enum class ProposalKind : std::uint8_t {
    rig,
    skin,
    motion,
};

[[nodiscard]] std::string_view proposal_kind_name(ProposalKind kind) noexcept;

// The effect classes a proposal is permitted to exercise. Every operation in
// the sequence must map to a permitted effect, enforced at validation.
enum class ProposalEffect : std::uint8_t {
    rig_structure,
    rig_rest,
    rig_attachments,
    skin_weights,
    motion_keys,
    motion_policy,
    sim_bindings,
    face_morphs,
    review_gates,
};

[[nodiscard]] std::string_view proposal_effect_name(ProposalEffect effect) noexcept;

[[nodiscard]] WorkspaceDomain proposal_domain(ProposalKind kind) noexcept;
[[nodiscard]] ProposalEffect command_effect(WorkspaceCommandKind kind) noexcept;

// One honest uncertainty statement about one proposed operation.
struct UncertaintyDiagnostic {
    static constexpr std::size_t kMaxAspectBytes = 128U;
    static constexpr std::size_t kMaxNoteBytes = 256U;

    std::string aspect;
    double confidence = 0.0; // in [0, 1]; lower means less certain
    std::string note;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const UncertaintyDiagnostic&) const noexcept = default;
};

// A physically meaningful quality expectation the proposal declares up front,
// to be checked against measured evidence downstream, never self-scored.
struct ExpectedQualityMetric {
    static constexpr std::size_t kMaxMetricBytes = 128U;

    std::string metric;
    double value = 0.0;
    double tolerance = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const ExpectedQualityMetric&) const noexcept = default;
};

// The shared proposal envelope (ch20 "Typed proposal contract"). It pins the
// exact source revision and digest it was generated against, so a stale
// proposal fails closed at review time instead of editing changed source.
struct AuthoringProposal {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;
    static constexpr std::size_t kMaxProviderBytes = 256U;
    static constexpr std::size_t kMaxAssets = 256U;
    static constexpr std::size_t kMaxTargets = 1'024U;
    static constexpr std::size_t kMaxEffects = 16U;
    static constexpr std::size_t kMaxOperations = 256U;
    static constexpr std::size_t kMaxAssumptions = 64U;
    static constexpr std::size_t kMaxAssumptionBytes = 512U;
    static constexpr std::size_t kMaxMetrics = 64U;
    static constexpr std::size_t kMaxPolicyBytes = 256U;
    static constexpr std::size_t kMaxTargetIdBytes = 256U;

    journal::Uuid proposal_id;
    ProposalKind kind = ProposalKind::rig;
    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest objective_text_digest;
    std::string provider_build_id;
    // Sorted and unique.
    std::vector<assets::Sha256Digest> referenced_assets;
    // Stable id strings ("joint:3"), sorted and unique.
    std::vector<std::string> explicit_target_ids;
    // Sorted and unique.
    std::vector<ProposalEffect> permitted_effect_kinds;
    // Ordered: this is an operation sequence, not a set.
    std::vector<WorkspaceCommand> operation_sequence;
    std::vector<std::string> declared_assumptions;
    // Exactly one uncertainty entry per operation, same order.
    std::vector<UncertaintyDiagnostic> uncertainty_by_operation;
    std::vector<ExpectedQualityMetric> expected_quality_metrics;
    std::uint64_t max_vertex_changes = 0U;
    std::uint64_t max_joint_changes = 0U;
    std::uint64_t max_time_ms = 0U;
    std::uint64_t max_memory_bytes = 0U;
    assets::Sha256Digest internal_evidence_digest;
    std::string expiry_or_revalidation_policy;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<AuthoringProposal> deserialize(
        std::string_view text);
};

enum class ProposalDecision : std::uint8_t {
    accepted,
    rejected,
};

[[nodiscard]] std::string_view proposal_decision_name(ProposalDecision decision) noexcept;

// One applied operation inside an accepted review.
struct ProposalApplication {
    std::uint32_t operation_index = 0U;
    WorkspaceCommandKind kind = WorkspaceCommandKind::rig_add_joint;
    core::Revision revision_before;
    core::Revision revision_after;
    assets::Sha256Digest command_digest;

    [[nodiscard]] bool operator==(const ProposalApplication&) const noexcept = default;
};

// The durable review record (manual supremacy, ch00 invariant 5). Accept
// applies the proposal through the exact same command surface and journaling
// as manual authoring; reject preserves source byte-for-byte.
struct ProposalReview {
    static constexpr std::size_t kMaxReviewerBytes = 256U;
    static constexpr std::size_t kMaxReasonBytes = 1'024U;
    static constexpr std::size_t kMaxApplications = AuthoringProposal::kMaxOperations;

    journal::Uuid proposal_id;
    assets::Sha256Digest proposal_digest;
    ProposalDecision decision = ProposalDecision::rejected;
    std::string reviewer;
    std::string reason;
    core::Revision review_revision;
    assets::Sha256Digest store_digest_before;
    assets::Sha256Digest store_digest_after;
    // Populated in order for accepted reviews only.
    std::vector<ProposalApplication> applications;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ProposalReview> deserialize(
        std::string_view text);
};

// ---- Typed proposal payloads from local generators ----

struct RigCandidateJoint {
    kinematic::JointId joint;
    std::optional<kinematic::JointId> parent;
    core::Transform rest = core::Transform::identity();
    kinematic::JointKind kind = kinematic::JointKind::ball;
    std::string semantic_type;
};

struct RigProposal {
    static constexpr std::size_t kMaxCandidates = 256U;

    std::vector<RigCandidateJoint> candidate_joints;
    assets::Sha256Digest recipe_digest;
    // One uncertainty entry per candidate, same order.
    std::vector<UncertaintyDiagnostic> uncertainty;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SkinCandidateStamp {
    std::uint32_t vertex = 0U;
    std::vector<kinematic::JointInfluence> influences;
};

struct SkinProposal {
    static constexpr std::size_t kMaxStamps = 65'536U;

    std::vector<SkinCandidateStamp> stamps;
    assets::Sha256Digest recipe_digest;
    std::vector<UncertaintyDiagnostic> uncertainty;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MotionCandidateKey {
    temporal::ChannelId channel;
    character::RationalTime time;
    double value = 0.0;
};

struct MotionProposal {
    static constexpr std::size_t kMaxKeys = 16'384U;

    std::vector<MotionCandidateKey> keys;
    assets::Sha256Digest recipe_digest;
    std::vector<UncertaintyDiagnostic> uncertainty;

    [[nodiscard]] core::Result<void> validate() const;
};

// Builds the shared envelope from a typed rig proposal. Every candidate
// becomes a native add-joint command; budgets and target ids are derived, not
// guessed.
[[nodiscard]] core::Result<AuthoringProposal> build_rig_proposal(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const RigProposal& proposal,
    const assets::Sha256Digest& objective_text_digest,
    std::string provider_build_id);

[[nodiscard]] core::Result<AuthoringProposal> build_skin_proposal(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const SkinProposal& proposal,
    const assets::Sha256Digest& objective_text_digest,
    std::string provider_build_id);

[[nodiscard]] core::Result<AuthoringProposal> build_motion_proposal(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const MotionProposal& proposal,
    const assets::Sha256Digest& objective_text_digest,
    std::string provider_build_id);

namespace local {

// Deterministic local generators (no model, no network). They produce typed
// proposals only; nothing here mutates source.

struct RigChainOptions {
    std::uint32_t joint_count = 4U; // bounded by RigProposal::kMaxCandidates
    core::Vec3d spacing{0.0, 0.1, 0.0}; // must be finite
};

// Proposes a straight joint chain rooted at the graph root (or a new root when
// the graph is empty). Joint identities continue the existing identity space.
// Confidence decays along the chain: longer spines are less certain.
[[nodiscard]] core::Result<RigProposal> propose_rig_chain(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const RigChainOptions& options);

struct UniformSkinOptions {
    kinematic::JointId default_joint;
};

// Proposes a single-influence assignment to the default joint for every
// influenced vertex that is not artist-locked. Honest about weakness: the
// uniform prior is a starting point, not a result.
[[nodiscard]] core::Result<SkinProposal> propose_uniform_skin(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const UniformSkinOptions& options);

struct StaticKeysOptions {
    character::RationalTime time;
    double value = 0.0;
};

// Proposes one static-pose key per existing keyframe time already present in
// the character channel: every authored time receives the proposed constant
// value. Requires the bundle to carry a channel.
[[nodiscard]] core::Result<MotionProposal> propose_static_keys(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const StaticKeysOptions& options);

} // namespace local

} // namespace carto::workspace
