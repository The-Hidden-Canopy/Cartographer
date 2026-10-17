#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::verification {

// A donor class describes why another repository is mentioned in a public
// promotion record. It is evidence metadata only; it never grants access to
// the donor repository or to a runtime service.
enum class DonorClass : std::uint8_t {
    promote_implementation = 0U,
    borrow_pattern = 1U,
    greenfield = 2U,
};

enum class PromotionDisposition : std::uint8_t {
    not_promoted = 0U,
    architecture_only = 1U,
    reimplemented = 2U,
    copied_adapted = 3U,
};

enum class PromotionStatus : std::uint8_t {
    observed = 0U,
    planned = 1U,
    adapting = 2U,
    accepted = 3U,
    rejected = 4U,
};

// A donor identity is deliberately logical and bounded. It cannot carry an
// absolute path, URL, credential, runtime handle, or arbitrary command.
struct DonorEvidence {
    std::string repository_id;
    // Lower-case 40-character Git object identity when one was verified.
    std::string commit;
    // Required when commit is absent; silent or guessed fallback is invalid.
    std::string commit_absence_reason;
    // Relative, logical source references only. These are not copied source
    // and are never resolved by this module.
    std::vector<std::string> logical_references;

    [[nodiscard]] core::Result<void> validate() const;
};

// One public-safe donor/promotion decision. The record preserves both the
// positive decision and the rejected/non-claim boundary so a later reader
// cannot mistake an architecture reference for promoted runtime code.
struct DonorPromotionReceipt {
    static constexpr std::size_t kMaxPromotionIdBytes = 128U;
    static constexpr std::size_t kMaxTargetModuleBytes = 128U;
    static constexpr std::size_t kMaxLicenseBasisBytes = 512U;
    static constexpr std::size_t kMaxDecisionBytes = 2048U;
    static constexpr std::size_t kMaxListEntries = 64U;
    static constexpr std::size_t kMaxListItemBytes = 512U;

    std::uint64_t sequence = 0U;
    std::string promotion_id;
    std::string target_module;
    DonorClass donor_class = DonorClass::greenfield;
    PromotionDisposition disposition = PromotionDisposition::not_promoted;
    PromotionStatus status = PromotionStatus::planned;
    DonorEvidence donor;
    std::string license_basis;
    std::vector<std::string> semantic_changes;
    std::vector<std::string> removed_authorities;
    std::vector<std::string> tests;
    std::vector<std::string> fixtures;
    std::vector<std::string> non_claims;
    std::string decision;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<DonorPromotionReceipt> deserialize(
        std::string_view text);
};

// Append-only promotion evidence. It is intentionally separate from the
// authoring domain and has no state-transition, permission, network, or
// runtime mutation surface.
class DonorPromotionLedger final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxRecords = 512U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;

    [[nodiscard]] core::Result<void> append(DonorPromotionReceipt receipt);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<DonorPromotionLedger> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

    [[nodiscard]] const std::vector<DonorPromotionReceipt>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    std::uint64_t next_sequence_ = 1U;
    std::vector<DonorPromotionReceipt> records_;
};

} // namespace carto::verification
