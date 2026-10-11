#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/representation/error_envelope.hpp>
#include <carto/representation/family.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::representation {

// Neutral hardware profile capability: a stable profile identity plus the
// purposes the profile can embody and its resident-byte budget. It carries no
// device handles and no policy implementation.
struct HardwareProfileCapability {
    static constexpr std::size_t kMaxProfileIdentityBytes = 128U;

    std::string profile_identity;
    assets::Sha256Digest profile_digest;
    // Sorted and unique by underlying value so the canonical form is stable.
    std::vector<RepresentationPurpose> supported_purposes;
    std::uint64_t resident_budget_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool supports(RepresentationPurpose purpose) const noexcept;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Error/cost requirement a consumer asks for. Budgets are hard bounds: the
// selector picks the cheapest satisfying variant or refuses explicitly. It
// never relaxes these bounds to fall back to a weaker representation.
struct RepresentationCapabilityRequirement {
    RepresentationPurpose purpose = RepresentationPurpose::render;
    double max_position_error = 0.0;
    double max_silhouette_error = 0.0;
    double max_normal_error = 0.0;
    double max_radiance_error = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Resolved error envelope for one variant. Variants reference their envelope
// by digest; the selector consumes the resolved values so the requirement
// bounds can be checked explicitly.
struct RepresentationVariantEnvelope {
    RepresentationVariantId variant;
    RepresentationErrorEnvelope envelope;

    [[nodiscard]] core::Result<void> validate() const;
};

// Deterministic selection receipt. The selection digest binds the family
// digest, requirement digest, capability digest, and the chosen variant, so
// the same inputs always reproduce the same selection.
struct RepresentationSelection {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    RepresentationFamilyId family;
    RepresentationVariantId selected_variant;
    RepresentationPurpose purpose = RepresentationPurpose::render;
    std::string profile_identity;
    assets::Sha256Digest profile_digest;
    assets::Sha256Digest requirement_digest;
    assets::Sha256Digest capability_digest;
    assets::Sha256Digest family_digest;
    std::string algorithm = "cartographer-representation-selection/0.1";
    assets::Sha256Digest selection_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<RepresentationSelection> deserialize(
        std::string_view text);
};

// Selects the cheapest variant of `family` satisfying `requirement` under
// `capability`. Cheapest means lowest resident_bytes, tie-broken by lowest
// variant id, so the result is a pure function of the inputs.
//
// Refusal is explicit: a profile that does not support the requested purpose
// fails with ErrorCode::unsupported and no candidate is ever downgraded to
// fit. `certifications`, when non-empty, restricts eligibility to certified
// variants whose recorded source revision matches the family; an empty span
// makes every family variant eligible. `envelopes` must resolve every
// eligible variant's error envelope by id.
[[nodiscard]] core::Result<RepresentationSelection> select_variant(
    const RepresentationFamily& family,
    const HardwareProfileCapability& capability,
    const RepresentationCapabilityRequirement& requirement,
    std::span<const RepresentationCertification> certifications,
    std::span<const RepresentationVariantEnvelope> envelopes);

} // namespace carto::representation
