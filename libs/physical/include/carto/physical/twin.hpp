#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/physical/condition.hpp>
#include <carto/physical/geometry_evidence.hpp>
#include <carto/physical/ground_pressure.hpp>
#include <carto/physical/mass.hpp>
#include <carto/physical/surface_ledger.hpp>
#include <carto/physical/types.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace carto::physical {

// The neutral, versioned, digest-bound physical twin document for one source
// object. It binds geometry evidence, construction, derived mass, the surface
// ledger, and optional ground pressure evidence to an exact source revision
// and digest. It contains no engine types and no runtime handles.
//
// Game-owned condition state may be *carried alongside* the record so
// adapters can keep one object in flight, but it is NEVER serialized into
// the canonical document and NEVER enters its digest. See condition.hpp.
struct PhysicalTwinRecord {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;
    static constexpr std::size_t kMaxConditionStates = 65'536U;

    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    GeometryEvidence geometry;
    ConstructionProfile construction;
    MassDerivationReceipt mass;
    PhysicalSurfaceLedger ledger;
    std::optional<GroundPressureEvidence> ground_pressure;
    // Game-owned runtime wear state, explicitly fenced out of the canonical
    // document. Mutating these entries never changes canonical_digest().
    std::vector<SurfaceConditionState> condition_states;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PhysicalTwinRecord> deserialize(
        std::string_view text);
};

// Receipt emitted by twin acceptance. Conservation is checked against the
// previously accepted twin: when the source digest is unchanged (no authored
// change), derived mass and enclosed volume must not increase. Derived values
// are accepted only when they bind the exact evidence digests of the inputs
// they claim to derive from; drift is stale data and fails the evaluation.
struct PhysicalTwinAcceptanceReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxViolations = 64U;

    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest twin_digest;
    bool first_acceptance = true;
    std::optional<assets::Sha256Digest> prior_twin_digest;
    double prior_total_mass_kg = 0.0;
    double candidate_total_mass_kg = 0.0;
    double prior_volume_m3 = 0.0;
    double candidate_volume_m3 = 0.0;
    bool evidence_digests_bound = false;
    bool mass_conservation_observed = true;
    bool volume_conservation_observed = true;
    std::uint64_t game_owned_condition_records = 0U;
    std::vector<std::string> violations;
    bool accepted = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PhysicalTwinAcceptanceReceipt> deserialize(
        std::string_view text);
};

// Evaluates candidate acceptance against the previously accepted twin (null
// for the first acceptance). Fails closed with stale_data when derived mass
// binds evidence digests that do not match the candidate's exact inputs.
// Conservation violations do not throw; they are reported in the receipt and
// set accepted = false, leaving the last-good twin untouched by contract.
[[nodiscard]] core::Result<PhysicalTwinAcceptanceReceipt> evaluate_physical_twin_acceptance(
    const PhysicalTwinRecord& candidate,
    const PhysicalTwinRecord* prior);

} // namespace carto::physical
