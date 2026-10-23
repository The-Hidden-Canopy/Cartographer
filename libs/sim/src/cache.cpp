#include <carto/sim/cache.hpp>

#include <cmath>
#include <sstream>
#include <string>
#include <utility>

namespace carto::sim {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

core::Result<CachePromotionRecord> make_record(
    CachePromotionInput input,
    CachePromotionPolicy policy,
    std::string reason) {
    if (input.receipt_digest.is_zero()) {
        return core::Result<CachePromotionRecord>::failure(invalid(
            "promotion evaluation requires a content-addressed receipt digest"));
    }
    CachePromotionRecord record;
    record.cache_digest = input.cache_digest;
    record.source_binding_digest = input.source_binding_digest;
    record.receipt_digest = input.receipt_digest;
    record.policy = policy;
    record.reason = std::move(reason);
    if (auto result = record.validate(); !result) {
        return core::Result<CachePromotionRecord>::failure(result.error());
    }
    return core::Result<CachePromotionRecord>::success(std::move(record));
}

} // namespace

std::string_view cache_promotion_policy_name(CachePromotionPolicy policy) noexcept {
    switch (policy) {
    case CachePromotionPolicy::promote: return "promote";
    case CachePromotionPolicy::hold: return "hold";
    case CachePromotionPolicy::reject: return "reject";
    }
    return "unknown";
}

core::Result<void> CachePromotionRecord::validate() const {
    if (cache_digest.is_zero() || source_binding_digest.is_zero() ||
        receipt_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "promotion record must bind the cache, its source and its receipt"));
    }
    if (reason.empty() || reason.size() > kMaxReasonBytes) {
        return core::Result<void>::failure(validation(
            "promotion record requires a bounded actionable reason"));
    }
    return core::Result<void>::success();
}

std::string CachePromotionRecord::serialize() const {
    std::ostringstream output;
    output << "CARTO_SIM_CACHE_PROMOTION " << kSchemaVersion << '\n';
    output << "CACHE " << cache_digest.hex() << '\n';
    output << "SOURCE " << source_binding_digest.hex() << '\n';
    output << "RECEIPT " << receipt_digest.hex() << '\n';
    output << "POLICY " << cache_promotion_policy_name(policy) << '\n';
    output << "REASON " << reason << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CachePromotionRecord::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<CachePromotionRecord> evaluate_cache_promotion(
    const CachePromotionInput& input) {
    if (input.cache_digest.is_zero() || input.source_binding_digest.is_zero()) {
        return core::Result<CachePromotionRecord>::failure(invalid(
            "promotion evaluation requires content-addressed cache and source"));
    }
    if (!std::isfinite(input.final_residual)) {
        return make_record(
            input,
            CachePromotionPolicy::reject,
            "bake residual is not finite; refusing promotion");
    }
    if (input.steps_executed != input.steps_recorded) {
        return make_record(
            input,
            CachePromotionPolicy::reject,
            "bake is incomplete: executed steps differ from recorded steps");
    }
    if (input.steps_executed == 0U) {
        return make_record(
            input,
            CachePromotionPolicy::reject,
            "bake recorded no steps; refusing promotion");
    }
    if (input.final_residual > CachePromotionRecord::kResidualPromotionThreshold) {
        return make_record(
            input,
            CachePromotionPolicy::hold,
            "bake residual exceeds the promotion threshold; keep on hold "
            "for artist review");
    }
    return make_record(
        input,
        CachePromotionPolicy::promote,
        "bake completed within declared caps and the residual threshold");
}

core::Result<CachePromotionRecord> evaluate_groom_cache_promotion(
    const GroomGuideCache& cache,
    const GroomSimReceipt& receipt) {
    if (auto result = receipt.validate(); !result) {
        return core::Result<CachePromotionRecord>::failure(
            result.error().with_context("groom cache promotion"));
    }
    auto cache_digest = cache.canonical_digest();
    if (!cache_digest) {
        return core::Result<CachePromotionRecord>::failure(cache_digest.error());
    }
    auto receipt_digest = receipt.canonical_digest();
    if (!receipt_digest) {
        return core::Result<CachePromotionRecord>::failure(receipt_digest.error());
    }
    CachePromotionInput input;
    input.cache_digest = cache_digest.value();
    input.source_binding_digest = receipt.source_binding_digest;
    input.receipt_digest = receipt_digest.value();
    input.steps_executed = receipt.frames_executed;
    input.steps_recorded = cache.frame_count;
    input.final_residual = receipt.settle_residual;
    if (!(input.cache_digest == receipt.cache_digest) ||
        !(cache.binding_digest == receipt.source_binding_digest)) {
        return make_record(
            input,
            CachePromotionPolicy::reject,
            "cache bytes do not match the receipt; stale or tampered caches "
            "are rejected, never re-promoted");
    }
    return evaluate_cache_promotion(input);
}

} // namespace carto::sim
