#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/prepared_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

inline constexpr std::uint32_t kDestinationCapabilityProfileVersion = 1U;
inline constexpr std::size_t kMaxDestinationCapabilityProfileBytes =
    1U * 1024U * 1024U;
inline constexpr std::size_t kMaxDestinationCapabilities = 4'096U;

// One capability a destination attests it can realize. The evidence stays at
// the destination's own attestation level; Cartographer never upgrades
// evidence it did not produce. A capability the destination does not list is
// unsupported by definition, so entries here never carry unsupported evidence.
struct DestinationCapability {
    std::string feature;
    PreparedCapabilityEvidence evidence =
        PreparedCapabilityEvidence::derived_product;
    std::string detail;

    [[nodiscard]] core::Result<void> validate() const;
};

// Versioned, digest-bound description of what one runtime destination can
// realize from prepared products. It contains no runtime handles, no engine
// types, and no mutation authority; absence of a feature is a precise
// negative answer, not an assumption.
class DestinationCapabilityProfile final {
public:
    static constexpr std::uint32_t kSchemaVersion =
        kDestinationCapabilityProfileVersion;
    static constexpr std::size_t kMaxSerializedBytes =
        kMaxDestinationCapabilityProfileBytes;
    static constexpr std::size_t kMaxCapabilities = kMaxDestinationCapabilities;

    std::string identity;
    core::Revision profile_revision;
    // Canonically sorted and unique by feature.
    std::vector<DestinationCapability> capabilities;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<DestinationCapabilityProfile> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

    [[nodiscard]] const DestinationCapability* find(
        std::string_view feature) const noexcept;
};

struct RequestedCapability {
    std::string feature;
    bool required = true;

    [[nodiscard]] core::Result<void> validate() const;
};

// Answers a capability demand against the profile. Every requested feature
// produces exactly one PreparedCapabilityResult, in request order. Supported
// features reuse the destination's attested evidence; unsupported features
// carry explicit unsupported evidence with the requesting detail preserved in
// the text. An unsupported result is emitted with required=false because a
// required-and-unsupported pair is not a valid standalone record; callers that
// refuse on required demand must inspect the request list alongside.
[[nodiscard]] core::Result<std::vector<PreparedCapabilityResult>>
check_destination_capabilities(
    const DestinationCapabilityProfile& profile,
    std::span<const RequestedCapability> requested);

} // namespace carto::production
