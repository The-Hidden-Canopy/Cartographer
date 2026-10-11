#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/verification/capability_ledger.hpp>
#include <carto/verification/regression_gate.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::verification {

// One named regression gate result inside a pipeline evidence report.
struct NamedGateOutcome {
    static constexpr std::size_t kMaxIdentityBytes = 256U;

    std::string gate_identity;
    RegressionGateOutcome outcome;

    [[nodiscard]] core::Result<void> validate() const;
};

// Whole-pipeline evidence report (doc 190): one deterministic, digest-bound,
// serializable record that assembles source identity and digest, preparation
// profile digest, capability claims and explicit non-claims, admission
// receipt digests, correspondence counts, and gate outcomes. It contains no
// runtime handles, no engine types, and no mutation authority; runtime and
// game-owned state stays fenced out by construction.
struct PipelineEvidenceReport {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxReceiptDigests = 1024U;
    static constexpr std::size_t kMaxGateOutcomes = 64U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string profile_identity;
    assets::Sha256Digest profile_digest;
    std::vector<CapabilityClaim> capability_claims;
    std::vector<CapabilityNonClaim> capability_non_claims;
    // Unique, sorted admission receipt digests referenced by the claims and
    // non-claims above; reimport or runtime receipts appear only as digests.
    std::vector<assets::Sha256Digest> admission_receipt_digests;
    std::vector<CorrespondenceCountExpectation> correspondence_counts;
    std::vector<NamedGateOutcome> gate_outcomes;
    assets::Sha256Digest report_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<PipelineEvidenceReport> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Assembles one deterministic report from a composed prepared scene, a
// capability ledger, and named gate outcomes. Receipt digests are extracted
// from the ledger; correspondence counts are tallied from the envelope; the
// report digest is bound before the report is returned.
[[nodiscard]] core::Result<PipelineEvidenceReport> assemble_pipeline_evidence_report(
    const production::PreparedSceneEnvelope& prepared,
    const CapabilityLedger& ledger,
    std::span<const NamedGateOutcome> gate_outcomes);

} // namespace carto::verification
