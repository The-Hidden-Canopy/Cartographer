#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/collision/metrics.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::collision {

// Collision is a representation family, not a separate asset religion. Every
// certified variant shares one authored identity and correspondence while a
// runtime consumer selects the cheapest variant that satisfies its purpose,
// target capability, and error budget.
enum class CollisionRepresentationFamily : std::uint8_t {
    primitive,
    compound,
    convex_proxy,
    triangle_mesh,
};

[[nodiscard]] std::string_view collision_representation_family_name(
    CollisionRepresentationFamily family) noexcept;

// Per-consumer collision purpose. Different runtime consumers may select
// different certified variants of the same authored identity.
enum class CollisionPurpose : std::uint8_t {
    gameplay_query,
    projectile,
    vehicle,
    debris,
    character,
};

[[nodiscard]] std::string_view collision_purpose_name(
    CollisionPurpose purpose) noexcept;

enum class CollisionReceiptStatus : std::uint8_t {
    admitted,
    failed,
    stale,
    cancelled,
};

[[nodiscard]] std::string_view collision_receipt_status_name(
    CollisionReceiptStatus status) noexcept;

enum class CollisionCandidateDisposition : std::uint8_t {
    selected,
    rejected_policy,
    failed,
    stale,
    cancelled,
};

[[nodiscard]] std::string_view collision_candidate_disposition_name(
    CollisionCandidateDisposition disposition) noexcept;

// Public, provider-neutral basis for a certified-variant selection. It binds
// the exact source, algorithm, settings, target profile, and admission
// policy without exposing the private policy that interprets them. It mirrors
// the production decision-receipt pattern while staying standalone.
struct CollisionVariantBasis {
    std::string source_identity;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string algorithm;
    assets::Sha256Digest settings_digest;
    std::string target_profile_identity;
    assets::Sha256Digest target_profile_digest;
    std::string admission_policy_identity;
    assets::Sha256Digest admission_policy_digest;
    std::string hardware_profile;
    std::uint32_t candidate_limit = 4U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const CollisionVariantBasis&) const noexcept = default;
};

// Every considered representation remains inspectable. A policy-rejected
// candidate can still carry valid measured output; it simply cannot be
// certified under the bound selection. Non-selected records require a bounded
// reason code.
struct CollisionVariantCandidate {
    static constexpr std::size_t kMaxReasonBytes = 128U;

    CollisionRepresentationFamily family =
        CollisionRepresentationFamily::primitive;
    CollisionPurpose purpose = CollisionPurpose::gameplay_query;
    assets::Sha256Digest candidate_digest;
    CollisionErrorMetrics measured_error;
    std::uint64_t resident_cost_bytes = 0U;
    CollisionCandidateDisposition disposition = CollisionCandidateDisposition::failed;
    std::string reason_code;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(
        const CollisionVariantCandidate&) const noexcept = default;
};

// Certified-variant selection receipt. The selected candidate is revalidated
// against the basis at publication time; a failed selection never certifies a
// fallback and never replaces an earlier admitted selection by itself.
class CollisionVariantSelection final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxCandidates = 64U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;

    CollisionVariantBasis basis;
    CollisionReceiptStatus status = CollisionReceiptStatus::failed;
    std::optional<std::uint32_t> selected_index;
    std::vector<CollisionVariantCandidate> candidates;
    assets::Sha256Digest decision_digest;

    [[nodiscard]] core::Result<void> insert_candidate(CollisionVariantCandidate candidate);
    [[nodiscard]] const CollisionVariantCandidate* selected_candidate() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CollisionVariantSelection> deserialize(
        std::string_view text);
};

} // namespace carto::collision
