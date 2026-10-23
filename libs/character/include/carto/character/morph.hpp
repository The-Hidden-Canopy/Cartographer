#pragma once

#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>

namespace carto::character {

enum class TangentPolicy : std::uint8_t {
    none,
    source_preserved,
    recompute,
};

[[nodiscard]] std::string_view tangent_policy_name(TangentPolicy policy) noexcept;

// One corrective shape on a stable mesh topology. Deltas are never inline:
// the immutable delta blob is content-addressed, non-finite deltas are
// rejected at decode time, and the magnitude envelope is declared up front so
// a decoder can fail before touching unbounded memory.
struct MorphTarget {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::uint64_t kMaxDisplacedPoints = 4'194'304U;
    static constexpr std::uint64_t kMaxDeltaBlobBytes = 256ULL * 1024ULL * 1024ULL;

    MorphTargetId id;
    // Source mesh identity and content digest the deltas correspond to.
    std::uint64_t source_mesh_asset = 0U;
    assets::Sha256Digest source_mesh_digest;
    // Displacement population summary and the content-addressed delta blob.
    std::uint64_t displaced_point_count = 0U;
    assets::Sha256Digest delta_blob_digest;
    std::uint64_t delta_blob_bytes = 0U;
    // Stable point-identity correspondence between the source mesh and this
    // target's delta layout. Must exist: without it the deltas are indexes
    // into nothing, and index is never a stable identity.
    assets::Sha256Digest topology_correspondence_digest;
    TangentPolicy tangent_in = TangentPolicy::source_preserved;
    TangentPolicy tangent_out = TangentPolicy::recompute;
    // Declared maximum absolute per-component delta magnitude; decoders fail
    // closed on any larger or non-finite decoded value.
    double precision_bound = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
