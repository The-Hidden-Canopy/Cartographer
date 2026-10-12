#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/representation/error_envelope.hpp>
#include <carto/representation/family.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::representation {

enum class EvidenceStatus : std::uint8_t {
    admitted,
    failed,
    stale,
};

[[nodiscard]] std::string_view evidence_status_name(EvidenceStatus status) noexcept;
[[nodiscard]] core::Result<EvidenceStatus> evidence_status_from_name(
    std::string_view name);

// Runtime-measured evidence for one family/variant on one hardware profile.
// Measurements are observations, never authoring truth: the ledger records
// what the runtime saw, and admission decisions consume it explicitly.
struct RepresentationRuntimeEvidence {
    RepresentationFamilyId family;
    RepresentationVariantId variant;
    std::string hardware_profile;
    assets::Sha256Digest profile_digest;
    core::Revision source_revision;
    EvidenceStatus status = EvidenceStatus::failed;
    std::uint64_t measured_resident_bytes = 0U;
    RepresentationErrorEnvelope observed_error;
    assets::Sha256Digest evidence_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Append-only runtime evidence ledger in the ProductionReceiptLedger idiom,
// standalone. Query never falls back: the latest record for a
// (family, variant, profile) key supersedes earlier ones, and a failed or
// stale latest record yields nullopt even when an older admitted record
// exists.
class RepresentationEvidenceLedger final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxRecords = 4096U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxProfileBytes = 128U;

    [[nodiscard]] core::Result<void> append(RepresentationRuntimeEvidence evidence);
    [[nodiscard]] core::Result<std::optional<RepresentationRuntimeEvidence>> query(
        RepresentationFamilyId family,
        RepresentationVariantId variant,
        std::string_view hardware_profile) const;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<RepresentationEvidenceLedger> deserialize(
        std::string_view text);

    [[nodiscard]] const std::vector<RepresentationRuntimeEvidence>& records()
        const noexcept {
        return records_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    std::vector<RepresentationRuntimeEvidence> records_;
};

} // namespace carto::representation
