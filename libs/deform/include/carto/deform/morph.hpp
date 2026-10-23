#pragma once

#include <carto/character/morph.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace carto::deform {

// Canonical little-endian delta blob codec version. Layout:
//   u32 version (must be 1), u32 reserved (must be 0), u64 count,
//   then count records of { u32 vertex_index, f64 dx, f64 dy, f64 dz }.
// Vertex indexes must be strictly increasing; encoding and decoding are the
// only producers and both fail closed on any deviation.
inline constexpr std::uint32_t kMorphDeltaBlobVersion = 1U;
inline constexpr std::uint64_t kMorphDeltaBlobHeaderBytes = 16U;
inline constexpr std::uint64_t kMorphDeltaBlobRecordBytes = 28U;

struct MorphDelta {
    std::uint32_t vertex_index = 0U;
    core::Vec3d delta{};
};

// Producer-side encoder; rejects non-finite deltas, duplicate or unordered
// indexes, and populations or sizes beyond the MorphTarget envelope.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_delta_blob(
    const std::vector<MorphDelta>& deltas);

[[nodiscard]] core::Result<std::vector<MorphDelta>> decode_delta_blob(
    std::span<const std::uint8_t> bytes,
    std::uint64_t expected_count);

// Caller-attested binding for one morph application. The target's declared
// source mesh digest and topology correspondence digest must match these
// attestations or the application refuses as stale data.
struct MorphApplicationInput {
    std::span<const core::Vec3d> bind_positions;
    const character::MorphTarget* target = nullptr;
    std::span<const std::uint8_t> delta_blob;
    assets::Sha256Digest mesh_content_digest;
    assets::Sha256Digest topology_correspondence_digest;
    double weight = 0.0; // must be finite and within [0, 1]
};

struct MorphApplicationResult {
    std::vector<core::Vec3d> positions;
    std::uint64_t displaced_points = 0U;
    // True exactly when the target's tangent_out policy demands tangent
    // recomputation; source_preserved positions flow through unchanged and
    // this flag stays false so callers never recompute silently.
    bool tangents_require_recompute = false;
    assets::Sha256Digest applied_delta_digest;
};

// Bounded, fail-closed morph application: blob envelope and digest verified
// against the record, correspondence and source mesh digests verified against
// the caller attestation, every decoded delta finite and within the declared
// precision bound, only the targeted points move, and the tangent policy is
// honored by explicit recompute demand.
[[nodiscard]] core::Result<MorphApplicationResult> apply_morph_target(
    const MorphApplicationInput& input);

// Deterministic digest of the policy-bearing fields of one morph target;
// recorded in deformation receipts as the stage policy digest.
[[nodiscard]] assets::Sha256Digest morph_policy_digest(
    const character::MorphTarget& target);

} // namespace carto::deform
