#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/dependency.hpp>
#include <carto/production/prepared_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

inline constexpr std::uint32_t kRuntimeSnapshotVersion = 1U;
inline constexpr std::uint32_t kPreparedRevisionDeltaVersion = 1U;
inline constexpr std::uint32_t kReimportPlanVersion = 1U;
inline constexpr std::uint32_t kReimportReceiptVersion = 1U;

inline constexpr std::size_t kMaxRuntimeSnapshotBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxRuntimeSnapshotStates = 65'536U;
inline constexpr std::size_t kMaxPreparedDeltaBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxPreparedDeltaChanges = 65'536U;
inline constexpr std::size_t kMaxReimportPlanBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxReimportPlanEntries = 65'536U;
inline constexpr std::size_t kMaxReimportReceiptBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxReimportDecisions = 4'096U;

// Runtime-reported condition of one admitted entity's game-owned state,
// relative to the baseline captured when it was admitted.
enum class RuntimeConditionKind : std::uint8_t {
    intact,
    damaged,
    looted,
    damaged_and_looted,
};

[[nodiscard]] const char* runtime_condition_kind_name(
    RuntimeConditionKind condition) noexcept;

// Per-source-identity runtime state: the 'C' leg of the three-way merge. All
// digests are produced by the destination adapter and are opaque to
// Cartographer; presence and parentage are the only structural facts. A
// destroyed entity keeps a tombstone (present=false) so an authored re-add is
// recognized as resurrection instead of a fresh create.
struct RuntimeSourceState {
    PreparedSourceEntityKind source_kind = PreparedSourceEntityKind::scene_node;
    std::uint64_t source_primary = 0U;
    std::uint64_t source_secondary = 0U;
    bool present = true;
    RuntimeConditionKind condition = RuntimeConditionKind::intact;
    // Current game-owned state digest and the baseline captured at admission.
    assets::Sha256Digest state_digest;
    assets::Sha256Digest admitted_state_digest;
    // Current runtime placement digest and the admitted baseline.
    assets::Sha256Digest placement_digest;
    assets::Sha256Digest admitted_placement_digest;
    // Current parent expressed as a stable source node identity (nodes only).
    std::optional<std::uint64_t> parent_source;
    // Optional spatial footprint for region-scoped reconciliation.
    std::optional<SpatialTileRange> footprint;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// The live world seen through stable source identities. It carries no runtime
// handles, no engine types, and no mutation authority; runtime/game state is
// fenced in by digest and only ever reported, never written.
class RuntimeWorldSnapshot final {
public:
    static constexpr std::uint32_t kSchemaVersion = kRuntimeSnapshotVersion;
    static constexpr std::size_t kMaxSerializedBytes = kMaxRuntimeSnapshotBytes;
    static constexpr std::size_t kMaxStates = kMaxRuntimeSnapshotStates;

    std::string source_namespace;
    core::Revision live_revision;
    // Canonically sorted and unique by source identity.
    std::vector<RuntimeSourceState> states;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<RuntimeWorldSnapshot> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

    [[nodiscard]] const RuntimeSourceState* find(
        PreparedSourceEntityKind source_kind,
        std::uint64_t source_primary,
        std::uint64_t source_secondary) const noexcept;
};

// Stable identity shared by delta, plan, and receipt records. The source side
// is the merge key; the prepared side pins the exact prepared entity.
struct PreparedEntityRef {
    PreparedSourceEntityKind source_kind = PreparedSourceEntityKind::scene_node;
    std::uint64_t source_primary = 0U;
    std::uint64_t source_secondary = 0U;
    PreparedEntityKind prepared_kind = PreparedEntityKind::node;
    std::uint64_t prepared_primary = 0U;
    std::uint64_t prepared_secondary = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const PreparedEntityRef&) const noexcept = default;
};

// Aspect bits describing what an authored change touched. Added/removed
// records carry no aspects; changed records carry at least one.
enum PreparedChangeAspect : std::uint32_t {
    changed_node_transform = 1U << 0U,
    changed_node_hierarchy = 1U << 1U,
    changed_node_visibility = 1U << 2U,
    changed_node_name = 1U << 3U,
    changed_node_asset_ref = 1U << 4U,
    changed_asset_products = 1U << 5U,
    changed_asset_channels = 1U << 6U,
    changed_asset_regions = 1U << 7U,
    changed_binding_material = 1U << 8U,
    changed_binding_variant = 1U << 9U,
    changed_correspondence_target = 1U << 10U,
};

inline constexpr std::uint32_t kKnownPreparedChangeAspects =
    changed_node_transform | changed_node_hierarchy | changed_node_visibility |
    changed_node_name | changed_node_asset_ref | changed_asset_products |
    changed_asset_channels | changed_asset_regions | changed_binding_material |
    changed_binding_variant | changed_correspondence_target;

struct PreparedEntityChange {
    PreparedEntityRef entity;
    std::uint32_t aspects = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Deterministic diff of two prepared-scene revisions keyed by stable source
// identity. Same envelope pair always yields the same byte-identical delta.
class PreparedRevisionDelta final {
public:
    static constexpr std::uint32_t kSchemaVersion = kPreparedRevisionDeltaVersion;
    static constexpr std::size_t kMaxSerializedBytes = kMaxPreparedDeltaBytes;
    static constexpr std::size_t kMaxChanges = kMaxPreparedDeltaChanges;

    std::string source_namespace;
    core::Revision base_revision;
    core::Revision incoming_revision;
    assets::Sha256Digest base_digest;
    assets::Sha256Digest incoming_digest;
    // Each list is canonically sorted and unique by source identity.
    std::vector<PreparedEntityChange> added;
    std::vector<PreparedEntityChange> removed;
    std::vector<PreparedEntityChange> changed;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<PreparedRevisionDelta> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Computes the authored leg (A->B) of the three-way merge. Both envelopes
// must share one source namespace; every difference is classified by stable
// source identity and, for changes, by aspect.
[[nodiscard]] core::Result<PreparedRevisionDelta> diff_prepared_revisions(
    const PreparedSceneEnvelope& base,
    const PreparedSceneEnvelope& incoming);

// Spatially filters an authored delta for streaming worlds. An entity whose
// runtime footprint is known and does not intersect the dirty region drops
// out of the delta; entities without a footprint are global and stay dirty.
// Incompatible grids or levels fail closed through SpatialTileRange.
[[nodiscard]] core::Result<PreparedRevisionDelta> filter_delta_to_region(
    const PreparedRevisionDelta& delta,
    const RuntimeWorldSnapshot& live,
    const SpatialTileRange& dirty_region);

// Who owns the contested truth for one source identity after reconciliation:
// authoring (carto), runtime/game state (game), or both at once (conflict).
enum class PropertyOwnership : std::uint8_t {
    carto_owned,
    game_owned,
    conflict,
};

[[nodiscard]] const char* property_ownership_name(
    PropertyOwnership ownership) noexcept;

struct PropertyOwnershipRecord {
    PreparedSourceEntityKind source_kind = PreparedSourceEntityKind::scene_node;
    std::uint64_t source_primary = 0U;
    std::uint64_t source_secondary = 0U;
    PropertyOwnership ownership = PropertyOwnership::carto_owned;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// Classifies property ownership for the union of identities across the
// three legs. Authored change plus runtime drift is a conflict; runtime drift
// alone is game-owned (preserved, never reset by reimport); anything else is
// carto-owned. Result is canonically sorted by source identity.
[[nodiscard]] core::Result<std::vector<PropertyOwnershipRecord>>
classify_property_ownership(
    const PreparedSceneEnvelope& base,
    const PreparedSceneEnvelope& incoming,
    const RuntimeWorldSnapshot& live);

// Why a three-way merge could not proceed silently. Kinds follow the runtime
// drift that collided with an authored change; precedence when several drifts
// coincide is destroyed > hierarchy_reparent > moved > damaged > looted, so
// the classification is deterministic.
enum class ReimportConflictKind : std::uint8_t {
    destroyed,
    moved,
    damaged,
    looted,
    hierarchy_reparent,
};

[[nodiscard]] const char* reimport_conflict_kind_name(
    ReimportConflictKind kind) noexcept;

struct ReimportConflict {
    ReimportConflictKind kind = ReimportConflictKind::destroyed;
    PreparedEntityRef entity;
    // Per-leg evidence digests; zero marks a leg where the entity is absent.
    assets::Sha256Digest base_record_digest;
    assets::Sha256Digest incoming_record_digest;
    assets::Sha256Digest live_state_digest;
    std::string detail;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// One staged, non-mutating action. Creates/updates/deletes are carto-owned
// unless noted; destruction-driven deletes are game-owned.
struct ReimportPlanEntry {
    PreparedEntityRef entity;
    // Update aspects (see PreparedChangeAspect); zero for creates and deletes.
    std::uint32_t aspects = 0U;
    PropertyOwnership ownership = PropertyOwnership::carto_owned;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// The staged result of reconciliation. It records what an importer would do,
// nothing more: creating, updating, or deleting prepared entities, plus the
// conflicts that demand an operator decision. Destroyed entities are never
// resurrected by construction; an authored change to a destroyed entity
// becomes a destroyed conflict instead of a staged create/update.
class ReimportPlan final {
public:
    static constexpr std::uint32_t kSchemaVersion = kReimportPlanVersion;
    static constexpr std::size_t kMaxSerializedBytes = kMaxReimportPlanBytes;
    static constexpr std::size_t kMaxEntries = kMaxReimportPlanEntries;

    std::string source_namespace;
    core::Revision base_revision;
    core::Revision incoming_revision;
    core::Revision live_revision;
    assets::Sha256Digest base_digest;
    assets::Sha256Digest incoming_digest;
    assets::Sha256Digest live_snapshot_digest;
    // Each list is canonically sorted and unique by source identity, and the
    // four lists never share an identity.
    std::vector<ReimportPlanEntry> creates;
    std::vector<ReimportPlanEntry> updates;
    std::vector<ReimportPlanEntry> deletes;
    std::vector<ReimportConflict> conflicts;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ReimportPlan> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// The neutral three-way reconciliation. A = base admitted prepared revision,
// B = incoming prepared revision, C = live runtime snapshot. Namespaces must
// agree; every present live identity must resolve in the base envelope, while
// destroyed tombstones may outlive prepared removal. The plan is a staged
// record only and grants no mutation authority.
[[nodiscard]] core::Result<ReimportPlan> plan_reimport(
    const PreparedSceneEnvelope& base,
    const PreparedSceneEnvelope& incoming,
    const RuntimeWorldSnapshot& live);

// Operator verdict for one conflict. admit applies the incoming authored
// change (for destroyed conflicts this explicitly accepts resurrection);
// override preserves the game-owned runtime state and drops the authored
// change; skip stages nothing and leaves the conflict unresolved.
enum class ReimportOperatorDecision : std::uint8_t {
    admit,
    override_decision,
    skip,
};

[[nodiscard]] const char* reimport_operator_decision_name(
    ReimportOperatorDecision decision) noexcept;

struct ReimportOperatorChoice {
    PreparedSourceEntityKind source_kind = PreparedSourceEntityKind::scene_node;
    std::uint64_t source_primary = 0U;
    std::uint64_t source_secondary = 0U;
    ReimportOperatorDecision decision = ReimportOperatorDecision::skip;
    // Empty marks an automated policy decision rather than a human operator.
    std::string operator_identity;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ReimportDecisionRecord {
    ReimportConflictKind conflict_kind = ReimportConflictKind::destroyed;
    PreparedSourceEntityKind source_kind = PreparedSourceEntityKind::scene_node;
    std::uint64_t source_primary = 0U;
    std::uint64_t source_secondary = 0U;
    ReimportOperatorDecision decision = ReimportOperatorDecision::skip;
    // The exact three-way digests the decision was bound to.
    assets::Sha256Digest base_record_digest;
    assets::Sha256Digest incoming_record_digest;
    assets::Sha256Digest live_state_digest;
    std::string operator_identity;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// The durable, digest-bound record of how each conflict was decided. The
// receipt binds every decision to the base/incoming/live digests and to the
// exact plan, so it is only meaningful for that three-way state and never for
// a silently newer world.
class ReimportReceipt final {
public:
    static constexpr std::uint32_t kSchemaVersion = kReimportReceiptVersion;
    static constexpr std::size_t kMaxSerializedBytes = kMaxReimportReceiptBytes;
    static constexpr std::size_t kMaxDecisions = kMaxReimportDecisions;

    std::string source_namespace;
    core::Revision base_revision;
    core::Revision incoming_revision;
    core::Revision live_revision;
    assets::Sha256Digest base_digest;
    assets::Sha256Digest incoming_digest;
    assets::Sha256Digest live_snapshot_digest;
    assets::Sha256Digest plan_digest;
    // One record per plan conflict, canonically sorted by source identity.
    std::vector<ReimportDecisionRecord> decisions;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ReimportReceipt> deserialize(
        std::string_view text);
};

// Records operator decisions for exactly the conflicts of one plan. Missing
// or unknown choices fail closed; every recorded decision inherits the
// conflict's three leg digests.
[[nodiscard]] core::Result<ReimportReceipt> record_reimport_receipt(
    const ReimportPlan& plan,
    std::span<const ReimportOperatorChoice> choices);

} // namespace carto::production
