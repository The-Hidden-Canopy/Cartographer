#include <carto/character/time.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

// Portable 64x64 -> 128 unsigned multiply (no compiler-specific intrinsics so
// the contract builds under any C++23 toolchain).
struct U128 {
    std::uint64_t hi = 0U;
    std::uint64_t lo = 0U;
};

U128 multiply(std::uint64_t left, std::uint64_t right) {
    constexpr std::uint64_t kMask = 0xFFFFFFFFULL;
    const std::uint64_t left_lo = left & kMask;
    const std::uint64_t left_hi = left >> 32U;
    const std::uint64_t right_lo = right & kMask;
    const std::uint64_t right_hi = right >> 32U;
    const std::uint64_t cross_lo = left_lo * right_hi + left_hi * right_lo;
    U128 result;
    result.lo = left_lo * right_lo;
    result.hi = left_hi * right_hi + (cross_lo >> 32U);
    const std::uint64_t carry =
        ((result.lo >> 32U) + (cross_lo & kMask)) >> 32U;
    result.hi += carry;
    result.lo += (cross_lo & kMask) << 32U;
    return result;
}

int compare_u128(U128 left, U128 right) noexcept {
    if (left.hi != right.hi) return left.hi < right.hi ? -1 : 1;
    if (left.lo != right.lo) return left.lo < right.lo ? -1 : 1;
    return 0;
}

std::uint64_t magnitude(std::int64_t value) noexcept {
    return value < 0
        ? static_cast<std::uint64_t>(-(value + 1)) + 1U
        : static_cast<std::uint64_t>(value);
}

bool add_overflows(std::int64_t left, std::int64_t right) noexcept {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    return (right > 0 && left > kMax - right) ||
        (right < 0 && left < kMin - right);
}

core::Result<std::uint64_t> lcm_timebase(std::uint32_t left, std::uint32_t right) {
    const std::uint64_t gcd = std::gcd<std::uint64_t>(left, right);
    return core::Result<std::uint64_t>::success(
        (static_cast<std::uint64_t>(left) / gcd) * right);
}

// Scales `tick` by `factor` with an exactness guarantee: fails closed when
// the scaled value does not fit int64.
core::Result<std::int64_t> rescale(std::int64_t tick, std::uint64_t factor) {
    const U128 product = multiply(magnitude(tick), factor);
    if (product.hi != 0U || product.lo >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return core::Result<std::int64_t>::failure(invalid(
            "rational time rescale overflows the exact integer range"));
    }
    const std::int64_t scaled = static_cast<std::int64_t>(product.lo);
    return core::Result<std::int64_t>::success(tick < 0 ? -scaled : scaled);
}

double ulp_of(double value) noexcept {
    if (value == 0.0) return 0.0;
    return std::abs(std::nextafter(value, std::numeric_limits<double>::infinity()) - value);
}

} // namespace

core::Result<void> RationalTime::validate() const {
    if (ticks_per_second == 0U) {
        return core::Result<void>::failure(invalid(
            "rational time requires a positive ticks-per-second"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_monotonic(std::span<const RationalTime> times) {
    for (const RationalTime& time : times) {
        if (auto result = time.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < times.size(); ++index) {
        if (times[index].ticks_per_second != times[index - 1U].ticks_per_second ||
            times[index].tick < times[index - 1U].tick) {
            return core::Result<void>::failure(validation(
                "rational time sequence must share one timebase and be non-decreasing"));
        }
    }
    return core::Result<void>::success();
}

bool same_timebase(RationalTime left, RationalTime right) noexcept {
    return left.ticks_per_second == right.ticks_per_second;
}

std::strong_ordering compare(RationalTime left, RationalTime right) noexcept {
    const int left_sign = (left.tick > 0) - (left.tick < 0);
    const int right_sign = (right.tick > 0) - (right.tick < 0);
    if (left_sign != right_sign) {
        return left_sign < right_sign ? std::strong_ordering::less
                                      : std::strong_ordering::greater;
    }
    if (left_sign == 0) return std::strong_ordering::equal;
    // Same non-zero sign: compare magnitudes exactly via 128-bit cross products.
    const int magnitude_order = compare_u128(
        multiply(magnitude(left.tick), right.ticks_per_second),
        multiply(magnitude(right.tick), left.ticks_per_second));
    if (magnitude_order == 0) return std::strong_ordering::equal;
    const bool less = left_sign > 0 ? magnitude_order < 0 : magnitude_order > 0;
    return less ? std::strong_ordering::less : std::strong_ordering::greater;
}

core::Result<RationalTime> add(RationalTime left, RationalTime right) {
    if (auto result = left.validate(); !result) return core::Result<RationalTime>::failure(result.error());
    if (auto result = right.validate(); !result) return core::Result<RationalTime>::failure(result.error());
    const auto timebase = lcm_timebase(left.ticks_per_second, right.ticks_per_second);
    if (timebase.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<RationalTime>::failure(invalid(
            "rational time operation requires a timebase beyond the admissible range"));
    }
    const auto left_scaled = rescale(
        left.tick, timebase.value() / left.ticks_per_second);
    if (!left_scaled) return core::Result<RationalTime>::failure(left_scaled.error());
    const auto right_scaled = rescale(
        right.tick, timebase.value() / right.ticks_per_second);
    if (!right_scaled) return core::Result<RationalTime>::failure(right_scaled.error());
    if (add_overflows(left_scaled.value(), right_scaled.value())) {
        return core::Result<RationalTime>::failure(invalid(
            "rational time addition overflows the exact integer range"));
    }
    RationalTime result{left_scaled.value() + right_scaled.value(),
        static_cast<std::uint32_t>(timebase.value())};
    if (auto valid = result.validate(); !valid) {
        return core::Result<RationalTime>::failure(valid.error());
    }
    return core::Result<RationalTime>::success(result);
}

core::Result<RationalTime> subtract(RationalTime left, RationalTime right) {
    if (auto result = left.validate(); !result) return core::Result<RationalTime>::failure(result.error());
    if (auto result = right.validate(); !result) return core::Result<RationalTime>::failure(result.error());
    const auto timebase = lcm_timebase(left.ticks_per_second, right.ticks_per_second);
    if (timebase.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<RationalTime>::failure(invalid(
            "rational time operation requires a timebase beyond the admissible range"));
    }
    const auto left_scaled = rescale(
        left.tick, timebase.value() / left.ticks_per_second);
    if (!left_scaled) return core::Result<RationalTime>::failure(left_scaled.error());
    const auto right_scaled = rescale(
        right.tick, timebase.value() / right.ticks_per_second);
    if (!right_scaled) return core::Result<RationalTime>::failure(right_scaled.error());
    if (add_overflows(left_scaled.value(), -right_scaled.value())) {
        return core::Result<RationalTime>::failure(invalid(
            "rational time subtraction overflows the exact integer range"));
    }
    RationalTime result{left_scaled.value() - right_scaled.value(),
        static_cast<std::uint32_t>(timebase.value())};
    if (auto valid = result.validate(); !valid) {
        return core::Result<RationalTime>::failure(valid.error());
    }
    return core::Result<RationalTime>::success(result);
}

SecondsConversion to_seconds(RationalTime time) noexcept {
    // IEEE-754 division is correctly rounded: the double is the nearest
    // representable value to the exact rational, so the absolute error is at
    // most half an ulp of the rounded result.
    const double seconds = static_cast<double>(time.tick) /
        static_cast<double>(time.ticks_per_second);
    return SecondsConversion{seconds, 0.5 * ulp_of(seconds)};
}

core::Result<TimeConversion> from_seconds(double seconds, std::uint32_t ticks_per_second) {
    if (!std::isfinite(seconds) || ticks_per_second == 0U) {
        return core::Result<TimeConversion>::failure(invalid(
            "seconds input must be finite and the timebase positive"));
    }
    const double scaled = seconds * static_cast<double>(ticks_per_second);
    if (!std::isfinite(scaled) ||
        std::abs(scaled) > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return core::Result<TimeConversion>::failure(invalid(
            "seconds input does not fit the rational tick range"));
    }
    // Round-to-nearest with ties to even under the default IEEE rounding mode.
    const double rounded = std::nearbyint(scaled);
    const auto tick = static_cast<std::int64_t>(rounded);
    // Error bound: at most half a tick of grid rounding plus the double
    // multiplication rounding, expressed in seconds.
    const double error_ticks = 0.5 + 0.5 * ulp_of(scaled);
    return core::Result<TimeConversion>::success(TimeConversion{
        RationalTime{tick, ticks_per_second}, error_ticks / ticks_per_second});
}

std::string serialize(RationalTime time) {
    std::ostringstream output;
    output << "CARTO_RATIONAL_TIME 1\n";
    output << "TICKS " << time.tick << ' ' << time.ticks_per_second << '\n';
    output << "END\n";
    return output.str();
}

core::Result<RationalTime> deserialize_rational_time(std::string_view text) {
    if (text.empty() || text.size() > 1U * 1024U) {
        return core::Result<RationalTime>::failure(parse_error(
            "rational time text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_RATIONAL_TIME" || version != 1U) {
        return core::Result<RationalTime>::failure(parse_error(
            "rational time framing or schema version is invalid"));
    }
    std::string token;
    RationalTime time;
    if (!(input >> token) || token != "TICKS" ||
        !(input >> time.tick >> time.ticks_per_second) ||
        !(input >> token) || token != "END") {
        return core::Result<RationalTime>::failure(parse_error(
            "rational time payload is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RationalTime>::failure(parse_error(
            "rational time text contains trailing unbound data"));
    }
    if (auto result = time.validate(); !result) {
        return core::Result<RationalTime>::failure(result.error());
    }
    return core::Result<RationalTime>::success(time);
}

core::Result<assets::Sha256Digest> canonical_digest(RationalTime time) {
    if (auto result = time.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize(time);
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

} // namespace carto::character
