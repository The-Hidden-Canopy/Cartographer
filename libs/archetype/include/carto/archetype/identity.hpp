#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::archetype {

inline constexpr std::uint32_t kIdentitySidecarSchemaVersion = 1U;
inline constexpr std::size_t kMaxSerializedSidecarBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxSidecarFields = 65'536U;
inline constexpr std::size_t kMaxLiveIdsPerField = 1'000'000U;
inline constexpr std::size_t kMaxDeadIdsPerField = 1'000'000U;
inline constexpr std::uint64_t kMaxTrackedOrdinal = 1'000'000'000U;
inline constexpr std::size_t kMaxIdCollisionAttempts = 1024U;

// Deterministic, content-derived instance identity for one ordinal of an
// instance field population. The same recipe digest, field identity, seed,
// and ordinal always derive the same non-zero identity across processes and
// platforms; the identity carries no runtime handle and no game state.
[[nodiscard]] std::uint64_t derive_instance_id(
    const assets::Sha256Digest& recipe_digest,
    std::uint64_t field_identity,
    std::uint64_t seed,
    std::uint64_t ordinal);

// Durable sidecar that keeps instance identities stable across cook and
// reimport. Reimport never resets persistent state: surviving ordinals keep
// their previous identities even when derivation inputs drift, and dead
// identities (suppressed instances) stay dead forever — they are never
// reissued to another ordinal, which is what prevents resurrection.
struct InstanceIdentitySidecar {
    static constexpr std::uint32_t kSchemaVersion = kIdentitySidecarSchemaVersion;

    struct FieldMap {
        world::InstanceFieldId field;
        std::uint64_t seed = 0U; // derivation seed observed at the last cook
        assets::Sha256Digest recipe_digest; // derivation recipe at the last cook
        // ordinal -> stable non-zero instance identity for live ordinals;
        // ordinals retired by population shrink are retained below the
        // current maximum so a later regrowth reuses the same identities.
        std::map<std::uint64_t, std::uint64_t> live_ids;
        // Sorted, unique identities that must never be reissued.
        std::vector<std::uint64_t> dead_ids;

        [[nodiscard]] core::Result<void> validate() const;
        [[nodiscard]] std::string canonical() const;
    };

    std::map<world::InstanceFieldId, FieldMap> fields;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<InstanceIdentitySidecar> deserialize(
        std::string_view text);
};

// Plans the instance identity map for one world instance field. With a null
// previous sidecar this is the initial cook: every ordinal derives its
// identity from the field's recipe, identity, and seed, skipping suppressed
// ordinals. With a previous sidecar this is a reimport: live ordinals keep
// their recorded identities, newly reachable ordinals derive fresh
// identities that deterministically avoid dead ones, and any identity found
// in the field's suppressed set transitions to dead and is never reissued.
// Reimporting an unchanged field over its own sidecar is an exact fixed
// point.
[[nodiscard]] core::Result<InstanceIdentitySidecar> reimport_instance_identities(
    const world::InstanceField& field,
    const InstanceIdentitySidecar* previous);

} // namespace carto::archetype
