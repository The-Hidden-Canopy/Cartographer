#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/gpudef/deform_cases.hpp>
#include <carto/gpudef/evidence.hpp>
#include <carto/gpudef/throughput.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::gpudef {

// Capability-style summary receipt (spec ch17 honest-evidence enumeration).
// One entry per reported deformation feature states exactly which features
// carry measured packed-lane parity evidence and which remain CPU-reference
// only: the summary derives its claims from the receipts it is built from
// and can never upgrade a feature it was not given evidence for. All four
// deformation cases have CPU references in libs/deform, so a feature with no
// bound parity receipts is reported cpu_reference_only — never "none" and
// never anything stronger.

enum class FeatureEvidence : std::uint8_t {
    // Measured case-parity receipts bind this feature's packed lane against
    // its source-lane reference; latest receipt digest is attached.
    packed_parity,
    // The feature exists as a CPU reference, but no packed-lane parity
    // receipts were supplied to this summary.
    cpu_reference_only,
};

[[nodiscard]] std::string_view feature_evidence_name(
    FeatureEvidence evidence) noexcept;

struct FeatureCapability {
    DeformationCase feature = DeformationCase::linear_blend_skinning;
    FeatureEvidence evidence = FeatureEvidence::cpu_reference_only;
    std::uint32_t receipt_count = 0U;
    // Digest of the latest bound parity receipt (max canonical digest for a
    // deterministic total order); zero exactly when no receipt is bound.
    assets::Sha256Digest latest_receipt_digest{};

    [[nodiscard]] core::Result<void> validate() const;
};

struct CapabilitySummaryReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxFeatures = 16U;

    // Sorted by feature, unique, at least one entry.
    std::vector<FeatureCapability> features;
    // Worst tier across the bound parity evidence: native_device only when
    // at least one parity receipt is bound and every bound parity receipt is
    // native-device; cpu_reference otherwise. Never structural, never
    // integrated_product.
    EvidenceTier tier = EvidenceTier::cpu_reference;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<CapabilitySummaryReceipt> deserialize(
        std::string_view text);
};

// Builds a summary over the caller-reported feature set (deduplicated and
// sorted; every deformation case is a valid report target), binding the
// parity receipts actually supplied. Features without receipts stay
// cpu_reference_only. Fails closed when any supplied receipt is invalid.
[[nodiscard]] core::Result<CapabilitySummaryReceipt> summarize_capabilities(
    std::span<const DeformationCase> reported_features,
    std::span<const CaseParityReceipt> parity_receipts,
    std::span<const ThroughputReceipt> throughput_receipts = {});

[[nodiscard]] EvidenceTier effective_tier(
    const CapabilitySummaryReceipt& receipt) noexcept;

[[nodiscard]] core::Result<void> require_evidence_tier(
    const CapabilitySummaryReceipt& receipt,
    EvidenceTier requested);

} // namespace carto::gpudef
