#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/time.hpp>
#include <carto/core/result.hpp>
#include <carto/motion/curve.hpp>

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace carto::motion {

// Dope-sheet and graph-editor operations over one keyed curve (ch13 "keys,
// tangents, retime"). Every operation is pure: it validates the source,
// returns a new curve plus a receipt binding source and result digests, and
// refuses without partial edits. Keys are addressed by their exact tick on
// the curve timebase, never by array position, so a selection stays valid
// across edits that reorder nothing.

enum class CurveEditKind : std::uint8_t {
    insert_key,
    delete_keys,
    offset_keys,
    scale_keys,
    set_tangents,
};

[[nodiscard]] std::string_view curve_edit_kind_name(CurveEditKind kind) noexcept;

enum class TangentMode : std::uint8_t {
    // Zero slope on both sides.
    flat,
    // In-slope toward the previous key, out-slope toward the next key; end
    // keys reuse their only neighbour slope.
    linear,
    // Catmull-Rom style: one slope from the previous to the next key; end
    // keys are flat so the curve never overshoots its range.
    auto_smooth,
};

[[nodiscard]] std::string_view tangent_mode_name(TangentMode mode) noexcept;

struct CurveEditReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U;

    CurveEditKind kind = CurveEditKind::insert_key;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest result_digest;
    std::uint64_t affected_key_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CurveEditReceipt> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct CurveEditResult {
    KeyedCurve curve;
    CurveEditReceipt receipt;
};

// Inserts one key. A key already at that tick is refused unless `replace`.
[[nodiscard]] core::Result<CurveEditResult> insert_key(
    const KeyedCurve& curve,
    const CurveKey& key,
    bool replace = false);

// Deletes the keys at the given ticks. Every tick must name an existing key
// and at least one key must remain.
[[nodiscard]] core::Result<CurveEditResult> delete_keys(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks);

// Moves the selected keys by delta_ticks. Refuses a zero delta, a selection
// that would land on an unselected key, or a tick that overflows.
[[nodiscard]] core::Result<CurveEditResult> offset_keys(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks,
    std::int64_t delta_ticks);

// Retimes the selected keys about pivot_tick by numerator/denominator
// (strictly positive). Scaled ticks round half away from zero; tangents are
// divided by the scale so each key keeps its shape in value-per-second. A
// retime that collapses two keys onto one tick or onto an unselected key is
// refused.
[[nodiscard]] core::Result<CurveEditResult> scale_keys(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks,
    std::int64_t pivot_tick,
    std::uint32_t numerator,
    std::uint32_t denominator);

// Rewrites the tangents of the selected keys under one mode.
[[nodiscard]] core::Result<CurveEditResult> set_tangents(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks,
    TangentMode mode);

} // namespace carto::motion
