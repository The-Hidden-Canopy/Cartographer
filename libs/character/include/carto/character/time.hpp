#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace carto::character {

// Exact rational authoring time: an integer tick count on a positive integer
// ticks-per-second timebase. All comparison and arithmetic here is exact
// integer arithmetic; no float enters any operation on this type.
//
// Conversion policy (documented contract, ch04 "Precision and interpolation"):
// - rational -> double seconds uses IEEE-754 correctly-rounded division.
//   The receipt bounds the absolute error by half an ulp of the rounded
//   result; the reported seconds are always the nearest double to the exact
//   rational value.
// - double seconds -> rational uses round-to-nearest-tick with ties to even
//   (default IEEE rounding mode). The receipt bounds the absolute error by
//   half a tick, expressed in seconds; non-finite or overflowing input fails
//   closed. The caller owns any error already present in the double input
//   itself; the receipt only covers the tick-grid rounding.
struct RationalTime {
    std::int64_t tick = 0;
    std::uint32_t ticks_per_second = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SecondsConversion {
    double seconds = 0.0;
    // Absolute error bound (seconds) of `seconds` against the exact rational
    // value. Always finite and non-negative.
    double max_abs_error = 0.0;
};

struct TimeConversion {
    RationalTime time;
    // Absolute error bound (seconds) of `time` against the input double.
    double max_abs_error = 0.0;
};

[[nodiscard]] core::Result<void> validate_monotonic(
    std::span<const RationalTime> times);

[[nodiscard]] std::strong_ordering compare(RationalTime left, RationalTime right) noexcept;
[[nodiscard]] core::Result<RationalTime> add(RationalTime left, RationalTime right);
[[nodiscard]] core::Result<RationalTime> subtract(RationalTime left, RationalTime right);
[[nodiscard]] bool same_timebase(RationalTime left, RationalTime right) noexcept;

[[nodiscard]] SecondsConversion to_seconds(RationalTime time) noexcept;
[[nodiscard]] core::Result<TimeConversion> from_seconds(
    double seconds,
    std::uint32_t ticks_per_second);

// Canonical reference codec: fixed field order, integer-only payload, no
// locale-sensitive formatting. Bounded so hostile inputs cannot drive
// allocation before validation.
[[nodiscard]] std::string serialize(RationalTime time);
[[nodiscard]] core::Result<RationalTime> deserialize_rational_time(std::string_view text);
[[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest(RationalTime time);

} // namespace carto::character
