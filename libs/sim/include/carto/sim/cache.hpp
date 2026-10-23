#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/sim/groom.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace carto::sim {

enum class CachePromotionPolicy : std::uint8_t {
    promote,
    hold,
    reject,
};

[[nodiscard]] std::string_view cache_promotion_policy_name(
    CachePromotionPolicy policy) noexcept;

// Promotion record for one baked cache. The record is digest-bound on both
// sides: to the content-addressed cache it certifies and to the exact source
// binding revision the cache was baked from. A cache whose bytes no longer
// match its receipt, or that was baked against a different binding digest,
// is rejected, never silently re-promoted.
struct CachePromotionRecord {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxReasonBytes = 512U;
    // Mean relative residual below which a completed bake may promote.
    static constexpr double kResidualPromotionThreshold = 1.0e-3;

    assets::Sha256Digest cache_digest;
    assets::Sha256Digest source_binding_digest;
    assets::Sha256Digest receipt_digest;
    CachePromotionPolicy policy = CachePromotionPolicy::hold;
    std::string reason;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// The evidence an automated promotion proposal may consider. All fields are
// producer-supplied; the convenience overloads below recompute the cache
// digest themselves so a tampered cache cannot present a matching receipt.
struct CachePromotionInput {
    assets::Sha256Digest cache_digest;
    assets::Sha256Digest source_binding_digest;
    assets::Sha256Digest receipt_digest;
    std::uint32_t steps_executed = 0U;
    std::uint32_t steps_recorded = 0U;
    double final_residual = 0.0;
};

// Deterministic policy evaluation. Incomplete bakes (executed != recorded)
// and non-finite residuals are rejected; completed bakes above the residual
// threshold are held with an actionable reason; the rest promote.
[[nodiscard]] core::Result<CachePromotionRecord> evaluate_cache_promotion(
    const CachePromotionInput& input);

// Convenience overloads that bind the receipt to the cache it certifies:
// recompute the cache digest, refuse on mismatch with the receipt, and check
// the source binding digests agree.
[[nodiscard]] core::Result<CachePromotionRecord> evaluate_groom_cache_promotion(
    const GroomGuideCache& cache,
    const GroomSimReceipt& receipt);

} // namespace carto::sim
