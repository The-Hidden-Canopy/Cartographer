#pragma once

#include <carto/production/closure.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

// Describes where an elapsed-time value came from. In particular, GPU time
// may never be labeled as measured when it is only a host estimate.
enum class TimingEvidenceSource : std::uint8_t {
    none,
    host_clock,
    device_timestamp,
    worker_device_timestamp,
};

[[nodiscard]] const char* timing_evidence_source_name(
    TimingEvidenceSource source) noexcept;

struct CostEvidenceBasis {
    TimingEvidenceSource cook = TimingEvidenceSource::none;
    TimingEvidenceSource cpu = TimingEvidenceSource::none;
    TimingEvidenceSource gpu = TimingEvidenceSource::none;

    [[nodiscard]] core::Result<void> validate(const CostTelemetry& cost) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const CostEvidenceBasis&) const noexcept = default;
};

enum class CandidateDisposition : std::uint8_t {
    selected,
    rejected_policy,
    failed,
    stale,
    cancelled,
};

[[nodiscard]] const char* candidate_disposition_name(
    CandidateDisposition disposition) noexcept;

// Public, provider-neutral basis for a production choice. Policy internals
// remain opaque; their exact public threshold contract is bound by digest.
struct ProductionDecisionBasis {
    AuthoritativeSource source;
    ProductionDecisionContext context;
    std::string policy_identity;
    assets::Sha256Digest admission_policy_digest;
    std::string hardware_profile;
    std::uint32_t candidate_limit = 1U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const ProductionDecisionBasis&) const noexcept = default;
};

// Every considered product remains inspectable. A policy-rejected candidate
// can still carry valid derived output; it simply cannot be published under
// the bound decision. Non-selected records require a bounded reason code.
struct ProductionDecisionCandidate {
    ProductionArtifactReceipt artifact;
    CandidateDisposition disposition = CandidateDisposition::failed;
    CostEvidenceBasis cost_evidence;
    std::string reason_code;

    [[nodiscard]] core::Result<void> validate(
        const ProductionDecisionBasis& basis) const;
    [[nodiscard]] std::string canonical() const;
};

struct ProductionDecisionReceipt {
    static constexpr std::uint32_t max_candidates = 64U;
    static constexpr std::uint64_t max_serialized_bytes = 4U * 1024U * 1024U;

    ProductionDecisionBasis basis;
    ReceiptStatus status = ReceiptStatus::failed;
    std::optional<std::uint32_t> selected_index;
    std::vector<ProductionDecisionCandidate> candidates;
    assets::Sha256Digest decision_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] const ProductionDecisionCandidate* selected_candidate() const noexcept;
};

// Revalidates the exact selected product, source, public threshold policy,
// context, and hardware profile immediately before publication. It does not
// choose a candidate and therefore does not expose provider selection policy.
[[nodiscard]] core::Result<void> validate_decision_publication(
    const ProductionDecisionReceipt& decision,
    const AuthoritativeSource& current_source,
    const AdmissionPolicy& current_policy);

[[nodiscard]] core::Result<std::vector<std::uint8_t>>
serialize_production_decision(const ProductionDecisionReceipt& decision);

[[nodiscard]] core::Result<ProductionDecisionReceipt>
deserialize_production_decision(std::span<const std::uint8_t> bytes);

} // namespace carto::production
