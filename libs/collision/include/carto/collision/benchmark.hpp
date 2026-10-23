#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/collision/family.hpp>
#include <carto/collision/metrics.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace carto::collision {

// Evidence tiers stay strictly separated: structural validation, reference
// behavior, native backend evidence and full-game acceptance are distinct,
// and one is never silently promoted into another. Cartographer's neutral
// benchmark can only ever produce the first two tiers.
enum class CollisionBenchmarkEvidence : std::uint8_t {
    structural_validation,
    cpu_reference,
    native_backend_evidence,
    full_game_acceptance,
};

[[nodiscard]] std::string_view collision_benchmark_evidence_name(
    CollisionBenchmarkEvidence evidence) noexcept;

// Public basis binding benchmark evidence to its exact source, algorithm,
// settings, target profile, and hardware profile.
struct CollisionBenchmarkBasis {
    std::string source_identity;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string algorithm;
    assets::Sha256Digest settings_digest;
    std::string target_profile_identity;
    assets::Sha256Digest target_profile_digest;
    std::string hardware_profile;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(
        const CollisionBenchmarkBasis&) const noexcept = default;
};

// CPU reference timing and error evidence for one collision candidate.
// Host-clock timing is labeled as such and may never be reported as measured
// device time. Receipts claiming native or full-game evidence are invalid:
// neutral preparation cannot fabricate them.
struct CollisionBenchmarkReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1024U * 1024U;

    CollisionBenchmarkBasis basis;
    assets::Sha256Digest candidate_digest;
    CollisionErrorMetrics measured_error;
    double cpu_reference_milliseconds = 0.0;
    CollisionBenchmarkEvidence evidence = CollisionBenchmarkEvidence::cpu_reference;
    CollisionReceiptStatus status = CollisionReceiptStatus::failed;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CollisionBenchmarkReceipt> deserialize(
        std::string_view text);
};

// Promotion gate: the public threshold contract a benchmarked candidate must
// satisfy before it may replace the last-good selection.
struct CollisionPromotionGate {
    CollisionErrorThresholdPolicy error_thresholds;
    std::optional<double> max_cpu_reference_milliseconds;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct CollisionPromotionDecision {
    // The candidate digest that remains selected after the promotion attempt.
    assets::Sha256Digest selected_candidate_digest;
    // True when the new candidate passed the gate and was promoted.
    bool promoted_new_candidate = false;
    // True when the new candidate failed the gate and the previous last-good
    // selection was retained untouched.
    bool retained_last_good = false;
};

// Applies the gate to one benchmark receipt. A failed, stale, cancelled, or
// over-budget candidate never replaces the last-good selection; when no
// last-good exists the promotion fails closed. The returned decision always
// names the digest a consumer must keep or adopt.
[[nodiscard]] core::Result<CollisionPromotionDecision> promote_collision_candidate(
    const CollisionBenchmarkReceipt& candidate,
    const std::optional<assets::Sha256Digest>& last_good_candidate_digest,
    const CollisionPromotionGate& gate);

} // namespace carto::collision
