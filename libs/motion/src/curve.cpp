#include <carto/motion/curve.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace carto::motion {
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

bool valid_interpolation(InterpolationKind kind) {
    switch (kind) {
    case InterpolationKind::linear:
    case InterpolationKind::smoothstep:
    case InterpolationKind::cubic_bezier:
        return true;
    }
    return false;
}

assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

} // namespace

std::string_view interpolation_kind_name(InterpolationKind kind) noexcept {
    switch (kind) {
    case InterpolationKind::linear: return "linear";
    case InterpolationKind::smoothstep: return "smoothstep";
    case InterpolationKind::cubic_bezier: return "cubic-bezier";
    }
    return "unknown";
}

core::Result<void> CurveKey::validate() const {
    if (auto result = time.validate(); !result) return result;
    if (!std::isfinite(value) || !std::isfinite(tangent_in) ||
        !std::isfinite(tangent_out)) {
        return core::Result<void>::failure(invalid(
            "curve key values and tangents must be finite"));
    }
    return core::Result<void>::success();
}

bool CurveKey::operator<(const CurveKey& other) const noexcept {
    return std::tie(time.tick, time.ticks_per_second, value, tangent_in, tangent_out) <
        std::tie(other.time.tick, other.time.ticks_per_second, other.value,
            other.tangent_in, other.tangent_out);
}

core::Result<void> KeyedCurve::validate() const {
    if (!id) {
        return core::Result<void>::failure(invalid(
            "keyed curve requires a stable identity"));
    }
    if (timebase_ticks_per_second == 0U ||
        timebase_ticks_per_second > kMaxTicksPerSecond) {
        return core::Result<void>::failure(invalid(
            "keyed curve timebase is out of range"));
    }
    if (!valid_interpolation(interpolation)) {
        return core::Result<void>::failure(validation(
            "keyed curve interpolation kind is unknown"));
    }
    if (keys.empty() || keys.size() > kMaxKeys) {
        return core::Result<void>::failure(validation(
            "keyed curve requires between one and its bounded capacity of keys"));
    }
    for (const CurveKey& key : keys) {
        if (auto result = key.validate(); !result) return result;
        if (key.time.ticks_per_second != timebase_ticks_per_second) {
            return core::Result<void>::failure(validation(
                "curve key timebase must match the curve timebase"));
        }
    }
    for (std::size_t index = 1U; index < keys.size(); ++index) {
        if (keys[index - 1U].time.tick >= keys[index].time.tick) {
            return core::Result<void>::failure(validation(
                "curve keys must be strictly increasing in tick"));
        }
    }
    return core::Result<void>::success();
}

core::Result<double> KeyedCurve::sample(character::RationalTime time) const {
    if (auto result = validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (time.ticks_per_second != timebase_ticks_per_second) {
        return core::Result<double>::failure(invalid(
            "sample time must be on the curve timebase"));
    }
    // Exact integer tick semantics: a request on a key tick returns the
    // stored value bit-for-bit, before any interpolation math.
    for (const CurveKey& key : keys) {
        if (key.time.tick == time.tick) {
            return core::Result<double>::success(key.value);
        }
    }
    if (time.tick <= keys.front().time.tick) {
        return core::Result<double>::success(keys.front().value);
    }
    if (time.tick >= keys.back().time.tick) {
        return core::Result<double>::success(keys.back().value);
    }
    std::size_t upper = 1U;
    while (keys[upper].time.tick < time.tick) ++upper;
    const CurveKey& before = keys[upper - 1U];
    const CurveKey& after = keys[upper];
    const double span_ticks = static_cast<double>(after.time.tick - before.time.tick);
    const double offset_ticks = static_cast<double>(time.tick - before.time.tick);
    const double t = offset_ticks / span_ticks;
    double shaped = t;
    switch (interpolation) {
    case InterpolationKind::linear:
        shaped = t;
        break;
    case InterpolationKind::smoothstep:
        shaped = t * t * (3.0 - 2.0 * t);
        break;
    case InterpolationKind::cubic_bezier: {
        // Hermite form with endpoint slopes in value units per second. The
        // interval length comes from correctly-rounded rational-to-double
        // conversion; tangents are validated finite so the derivative stays
        // finite everywhere on the segment.
        const double start_seconds = character::to_seconds(before.time).seconds;
        const double end_seconds = character::to_seconds(after.time).seconds;
        const double span_seconds = end_seconds - start_seconds;
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        const double h10 = t3 - 2.0 * t2 + t;
        const double h01 = -2.0 * t3 + 3.0 * t2;
        const double h11 = t3 - t2;
        shaped = 0.0;
        return core::Result<double>::success(
            h00 * before.value + h10 * span_seconds * before.tangent_out +
            h01 * after.value + h11 * span_seconds * after.tangent_in);
    }
    }
    return core::Result<double>::success(
        before.value + (after.value - before.value) * shaped);
}

std::string KeyedCurve::serialize() const {
    std::ostringstream output;
    output << "CARTO_KEYED_CURVE 1\n";
    output << "ID " << id.value << '\n';
    output << "TIMEBASE " << timebase_ticks_per_second << '\n';
    output << "INTERPOLATION " << interpolation_kind_name(interpolation) << '\n';
    output << "KEYS " << keys.size() << '\n';
    output << std::setprecision(17);
    for (const CurveKey& key : keys) {
        output << "KEY " << key.time.tick << ' ' << key.time.ticks_per_second << ' '
               << key.value << ' ' << key.tangent_in << ' ' << key.tangent_out << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> KeyedCurve::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<KeyedCurve> KeyedCurve::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_KEYED_CURVE" || version != kSchemaVersion) {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve framing or schema version is invalid"));
    }
    KeyedCurve curve;
    std::string token;
    if (!(input >> token) || token != "ID" || !(input >> curve.id.value)) {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve identity is malformed"));
    }
    if (!(input >> token) || token != "TIMEBASE" ||
        !(input >> curve.timebase_ticks_per_second)) {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve timebase is malformed"));
    }
    std::string kind;
    if (!(input >> token) || token != "INTERPOLATION" || !(input >> kind)) {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve interpolation is malformed"));
    }
    if (kind == "linear") curve.interpolation = InterpolationKind::linear;
    else if (kind == "smoothstep") curve.interpolation = InterpolationKind::smoothstep;
    else if (kind == "cubic-bezier") curve.interpolation = InterpolationKind::cubic_bezier;
    else {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve interpolation kind is unknown"));
    }
    std::uint64_t key_count = 0U;
    if (!(input >> token) || token != "KEYS" || !(input >> key_count) ||
        key_count > kMaxKeys || key_count == 0U) {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve key count is malformed or exceeds its bound"));
    }
    curve.keys.reserve(static_cast<std::size_t>(key_count));
    for (std::uint64_t index = 0U; index < key_count; ++index) {
        CurveKey key;
        if (!(input >> token) || token != "KEY" ||
            !(input >> key.time.tick >> key.time.ticks_per_second >> key.value >>
                key.tangent_in >> key.tangent_out)) {
            return core::Result<KeyedCurve>::failure(parse_error(
                "keyed curve key payload is malformed"));
        }
        curve.keys.push_back(key);
    }
    if (!(input >> token) || token != "END") {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve terminator is missing"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<KeyedCurve>::failure(parse_error(
            "keyed curve text contains trailing unbound data"));
    }
    if (auto result = curve.validate(); !result) {
        return core::Result<KeyedCurve>::failure(result.error());
    }
    return core::Result<KeyedCurve>::success(std::move(curve));
}

core::Result<void> TimebaseRescaleReceipt::validate() const {
    if (source_ticks_per_second == 0U || result_ticks_per_second == 0U ||
        source_digest.is_zero() || result_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "rescale receipt requires timebases and content digests"));
    }
    if (!std::isfinite(max_abs_time_error_seconds) ||
        max_abs_time_error_seconds < 0.0) {
        return core::Result<void>::failure(invalid(
            "rescale receipt error bound must be finite and non-negative"));
    }
    if (exact && max_abs_time_error_seconds != 0.0) {
        return core::Result<void>::failure(validation(
            "an exact rescale carries a zero error bound"));
    }
    return core::Result<void>::success();
}

core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>> rescale_timebase(
    const KeyedCurve& source,
    std::uint32_t new_ticks_per_second) {
    if (auto result = source.validate(); !result) {
        return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::failure(
            result.error());
    }
    if (new_ticks_per_second == 0U || new_ticks_per_second > KeyedCurve::kMaxTicksPerSecond) {
        return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::failure(
            invalid("target timebase is out of range"));
    }
    const auto source_digest = source.canonical_digest();
    if (!source_digest) {
        return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::failure(
            source_digest.error());
    }

    KeyedCurve result = source;
    result.timebase_ticks_per_second = new_ticks_per_second;
    bool exact = true;
    double max_error = 0.0;
    for (CurveKey& key : result.keys) {
        const auto scaled = character::from_seconds(
            character::to_seconds(key.time).seconds, new_ticks_per_second);
        if (!scaled) {
            return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::failure(
                scaled.error());
        }
        // Exact integer check: the source tick is re-expressible on the new
        // grid only when tick * new / old lands on an integer tick.
        const long double numerator = static_cast<long double>(key.time.tick) *
            static_cast<long double>(new_ticks_per_second);
        const long double quotient = numerator / key.time.ticks_per_second;
        const long double nearest = std::round(quotient);
        if (std::fabs(quotient - nearest) <= 1e-18L &&
            std::fabs(quotient) <=
                static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
            key.time = character::RationalTime{
                static_cast<std::int64_t>(nearest), new_ticks_per_second};
        } else {
            exact = false;
            key.time = scaled.value().time;
            max_error = std::max(max_error, scaled.value().max_abs_error);
        }
    }
    if (auto result_valid = result.validate(); !result_valid) {
        return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::failure(
            result_valid.error());
    }
    const auto result_digest = result.canonical_digest();
    if (!result_digest) {
        return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::failure(
            result_digest.error());
    }
    TimebaseRescaleReceipt receipt{
        source_digest.value(), result_digest.value(),
        source.timebase_ticks_per_second, new_ticks_per_second,
        exact, exact ? 0.0 : max_error};
    if (auto receipt_valid = receipt.validate(); !receipt_valid) {
        return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::failure(
            receipt_valid.error());
    }
    return core::Result<std::pair<KeyedCurve, TimebaseRescaleReceipt>>::success(
        std::make_pair(std::move(result), std::move(receipt)));
}

} // namespace carto::motion
