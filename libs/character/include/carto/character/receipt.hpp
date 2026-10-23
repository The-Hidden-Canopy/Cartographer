#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace carto::character {

// Derivation receipt for one character product. It binds the producer build,
// the exact source (namespace, revision, digest), the recipe, the format
// schema, the backend/precision identity and the output digest. Wall time is
// deliberately absent from the canonical form: determinism and identity come
// from revisions and digests, never from a clock.
struct CharacterProductReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;

    std::string producer_build;
    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest recipe_digest;
    std::uint32_t format_schema_major = 0U;
    std::uint32_t format_schema_minor = 0U;
    std::string backend_identity;
    std::string precision_identity;
    assets::Sha256Digest output_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

enum class GateStatus : std::uint8_t {
    pass,
    fail,
    hold,
    not_run,
};

[[nodiscard]] std::string_view gate_status_name(GateStatus status) noexcept;

// Stub of the release promotion receipt. Individual subsystem leads record
// PASS/FAIL/HOLD/NOT_RUN per gate; the full gate ledger and the single
// promotion authority live in the verification subsystem. Even the stub
// fails closed: a NOT_RUN required gate can never read as a pass here, and
// the signer string is mandatory.
struct CharacterPromotionReceipt {
    static constexpr std::size_t kMaxLedgerRefBytes = 256U;
    static constexpr std::size_t kMaxGates = 256U;
    static constexpr std::size_t kMaxGateIdBytes = 128U;
    static constexpr std::size_t kMaxSignerBytes = 256U;

    std::string gate_ledger_ref;
    // Sorted by gate id; every recorded gate carries an explicit status.
    std::map<std::string, GateStatus> gate_statuses;
    std::string signer;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
