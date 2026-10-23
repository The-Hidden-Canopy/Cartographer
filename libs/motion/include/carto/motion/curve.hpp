#pragma once

#include <carto/character/motion_document.hpp>
#include <carto/character/time.hpp>
#include <carto/motion/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::motion {

enum class InterpolationKind : std::uint8_t {
    linear,
    smoothstep,
    cubic_bezier,
};

[[nodiscard]] std::string_view interpolation_kind_name(InterpolationKind kind) noexcept;

// One authored key on a scalar curve. Time is exact rational authoring time on
// the curve timebase; no float enters key identity or ordering. Tangents are
// Hermite slopes in value units per second and are used only by
// cubic_bezier; they must always be finite so tangent editing can never
// produce non-finite derivatives.
struct CurveKey {
    character::RationalTime time;
    double value = 0.0;
    double tangent_in = 0.0;
    double tangent_out = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const CurveKey& other) const noexcept {
        return time.tick == other.time.tick &&
            time.ticks_per_second == other.time.ticks_per_second &&
            value == other.value && tangent_in == other.tangent_in &&
            tangent_out == other.tangent_out;
    }
    [[nodiscard]] bool operator<(const CurveKey& other) const noexcept;
};

// A deterministic keyed scalar curve over exact rational time. Keys are
// sorted by tick and unique within one timebase. Sampling clamps to the
// outer keys outside the key range, returns the exact stored value at a key
// tick, and is a pure function of the curve bytes and the requested tick:
// identical inputs produce identical doubles on every platform.
class KeyedCurve final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxKeys = 4'096U;
    static constexpr std::uint32_t kMaxTicksPerSecond =
        character::MotionDocument::kMaxTicksPerSecond;

    MotionCurveId id;
    std::uint32_t timebase_ticks_per_second = 0U;
    InterpolationKind interpolation = InterpolationKind::linear;
    std::vector<CurveKey> keys;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<double> sample(character::RationalTime time) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<KeyedCurve> deserialize(std::string_view text);
};

// Evidence for a cross-timebase rescale of a keyed curve. Key times are
// re-expressed on the new timebase; when every key time scales exactly the
// rescale is flagged exact and the error bound is zero, otherwise the bound
// covers the tick-grid rounding of the conversion.
struct TimebaseRescaleReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    assets::Sha256Digest source_digest;
    assets::Sha256Digest result_digest;
    std::uint32_t source_ticks_per_second = 0U;
    std::uint32_t result_ticks_per_second = 0U;
    bool exact = false;
    double max_abs_time_error_seconds = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Rescales a validated curve to a new ticks-per-second timebase. The result
// curve keeps the source interpolation kind and values; key times are mapped
// through exact rational scaling when representable and through documented
// round-to-nearest-tick conversion otherwise. The source curve is untouched.
[[nodiscard]] core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>> rescale_timebase(
    const KeyedCurve& source,
    std::uint32_t new_ticks_per_second);

} // namespace carto::motion
