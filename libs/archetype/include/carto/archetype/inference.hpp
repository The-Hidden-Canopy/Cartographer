#pragma once

#include <carto/archetype/digest.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::archetype {

inline constexpr std::size_t kMaxObservationIdentityBytes = 256U;
inline constexpr std::size_t kMaxSourceNamespaceBytes = 256U;
inline constexpr std::size_t kMaxCandidatesPerProposal = 4'096U;
inline constexpr std::size_t kMaxObservationsPerCandidate = 65'536U;
inline constexpr std::size_t kMaxSerializedProposalBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxSerializedReceiptBytes = 1U * 1024U * 1024U;

// One observed assembly in source content. The observation identity is a
// stable source identity (for example a scene node or instance-field recipe
// identity), never a runtime handle or display name.
struct AssemblyObservation {
    std::string observation_identity;
    StructuralDigest structural;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct InferenceOptions {
    // Repetitions required before a structure becomes an archetype candidate.
    // Must be at least two: a unique structure is authoring truth, not a
    // repetition economy.
    std::uint32_t minimum_repetition = 2U;

    [[nodiscard]] core::Result<void> validate() const;
};

// A group of source observations that share one structural digest. Member
// identities are sorted and unique; member_evidence_digest binds them.
struct ArchetypeCandidate {
    StructuralDigest structural;
    std::vector<std::string> observation_identities; // sorted and unique
    std::uint64_t repetition_count = 0U;
    assets::Sha256Digest member_evidence_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// Deterministic evidence for one inference pass. The receipt binds the exact
// source observation set, the deduplication counts, and the candidate set;
// its digest is self-binding. Recomputing inference over the same source
// revision and observations reproduces this receipt exactly.
struct InferenceReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest; // over the full sorted observation set
    std::uint32_t observation_count = 0U;
    std::uint32_t unique_structure_count = 0U;
    std::uint32_t candidate_count = 0U;
    // Sum over candidates of (repetition_count - 1): the instances a game
    // no longer has to plumb separately when the archetype is reused.
    std::uint64_t deduplicated_instance_count = 0U;
    assets::Sha256Digest candidate_set_digest; // over the sorted candidate set
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Provider-neutral, immutable inference product. It proposes reusable
// archetypes; it contains no engine archetype types, no runtime handles, and
// no game-variant policy.
struct ArchetypeProposal {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    // Sorted strictly by structural value; at most one candidate per structure.
    std::vector<ArchetypeCandidate> candidates;
    InferenceReceipt receipt;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ArchetypeProposal> deserialize(
        std::string_view text);
};

// Detects repeated assemblies: observations are validated, canonicalized, and
// grouped by structural digest. Groups reaching the repetition threshold
// become candidates; everything else is counted but never proposed. The same
// observations in any input order produce byte-identical proposals.
[[nodiscard]] core::Result<ArchetypeProposal> infer_archetypes(
    std::string source_namespace,
    core::Revision source_revision,
    std::span<const AssemblyObservation> observations,
    const InferenceOptions& options = {});

} // namespace carto::archetype
