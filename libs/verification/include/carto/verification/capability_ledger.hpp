#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/production/prepared_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::verification {

// One truthful feature admission: the named capability is supported for one
// exact admitted product, bound to the admission receipt digest that admitted
// it. A claim never carries unsupported evidence; explicit refusal is always
// recorded as a CapabilityNonClaim instead.
struct CapabilityClaim {
    std::uint64_t sequence = 0U;
    std::string feature;
    production::PreparedCapabilityEvidence evidence =
        production::PreparedCapabilityEvidence::preserved_source;
    bool required = false;
    assets::Sha256Digest admission_receipt_digest;
    std::string detail;

    [[nodiscard]] core::Result<void> validate() const;
};

// Explicit truthful non-claim: the named feature is NOT supported by the
// admitted product. Non-claims are first-class ledger records with a bounded
// reason and the same admission receipt binding; unsupported capability is
// never represented by silence or by a bare flag.
struct CapabilityNonClaim {
    std::uint64_t sequence = 0U;
    std::string feature;
    assets::Sha256Digest admission_receipt_digest;
    std::string reason;

    [[nodiscard]] core::Result<void> validate() const;
};

// Query view binding the latest claim, the latest non-claim, and their
// admission receipts for one feature. Both sides remain inspectable and the
// highest-sequence record decides the current verdict.
struct CapabilityQueryView {
    std::string feature;
    std::optional<CapabilityClaim> latest_claim;
    std::optional<CapabilityNonClaim> latest_non_claim;

    [[nodiscard]] bool declared() const noexcept {
        return latest_claim.has_value() || latest_non_claim.has_value();
    }
    // The current verdict is supported only when the newest record is a
    // claim; an unsubscribed feature and a superseded claim both read back
    // as not currently supported.
    [[nodiscard]] bool currently_supported() const noexcept;
};

// Append-only ledger of capability claims and explicit non-claims. Records
// are engine-neutral and runtime-handle-free; every record is bound to the
// digest of the admission receipt that produced it. Reimport or runtime
// receipts are referenced only as digest strings.
class CapabilityLedger final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxRecords = 4096U;
    static constexpr std::size_t kMaxFeatureBytes = 256U;
    static constexpr std::size_t kMaxDetailBytes = 1024U;
    static constexpr std::size_t kMaxReasonBytes = 1024U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;

    // Append-only: a later record for the same feature supersedes earlier
    // ones in query verdicts but never removes or rewrites them.
    [[nodiscard]] core::Result<void> claim(CapabilityClaim claim);
    [[nodiscard]] core::Result<void> non_claim(CapabilityNonClaim non_claim);

    // Converts composed neutral capability results into truthful ledger
    // records: unsupported evidence becomes an explicit non-claim with the
    // capability detail as its reason; every other evidence class becomes a
    // claim. All records bind one admission receipt digest.
    [[nodiscard]] core::Result<void> record_prepared_capabilities(
        const std::vector<production::PreparedCapabilityResult>& capabilities,
        const assets::Sha256Digest& admission_receipt_digest);

    // Binds claim + non-claim + receipt for one feature; nullopt when the
    // feature was never declared (so callers can distinguish "not supported"
    // from "never asked").
    [[nodiscard]] std::optional<CapabilityQueryView> query(
        std::string_view feature) const;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CapabilityLedger> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

    [[nodiscard]] const std::vector<CapabilityClaim>& claims() const noexcept {
        return claims_;
    }
    [[nodiscard]] const std::vector<CapabilityNonClaim>& non_claims() const noexcept {
        return non_claims_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return claims_.size() + non_claims_.size();
    }

private:
    std::uint64_t next_sequence_ = 1U;
    std::vector<CapabilityClaim> claims_;
    std::vector<CapabilityNonClaim> non_claims_;
};

} // namespace carto::verification
