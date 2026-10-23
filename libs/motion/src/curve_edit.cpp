#include <carto/motion/curve_edit.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace carto::motion {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic conflict(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

core::Diagnostic missing(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Result<std::set<std::int64_t>> selection_of(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks) {
    if (ticks.empty() || ticks.size() > KeyedCurve::kMaxKeys) {
        return core::Result<std::set<std::int64_t>>::failure(invalid(
            "curve edit requires a bounded non-empty key selection"));
    }
    std::set<std::int64_t> selection(ticks.begin(), ticks.end());
    if (selection.size() != ticks.size()) {
        return core::Result<std::set<std::int64_t>>::failure(invalid(
            "curve edit selection repeats a key"));
    }
    for (const std::int64_t tick : selection) {
        const bool present = std::any_of(curve.keys.begin(), curve.keys.end(),
            [tick](const CurveKey& key) { return key.time.tick == tick; });
        if (!present) {
            return core::Result<std::set<std::int64_t>>::failure(missing(
                "curve edit selection names a tick with no key")
                .with_context("tick=" + std::to_string(tick)));
        }
    }
    return core::Result<std::set<std::int64_t>>::success(std::move(selection));
}

core::Result<CurveEditResult> seal(
    const KeyedCurve& source,
    KeyedCurve result,
    CurveEditKind kind,
    std::uint64_t affected) {
    std::sort(result.keys.begin(), result.keys.end(),
        [](const CurveKey& a, const CurveKey& b) { return a.time.tick < b.time.tick; });
    if (auto valid = result.validate(); !valid) {
        return core::Result<CurveEditResult>::failure(valid.error());
    }
    const auto source_digest = source.canonical_digest();
    const auto result_digest = result.canonical_digest();
    if (!source_digest) return core::Result<CurveEditResult>::failure(source_digest.error());
    if (!result_digest) return core::Result<CurveEditResult>::failure(result_digest.error());
    CurveEditResult edited;
    edited.curve = std::move(result);
    edited.receipt.kind = kind;
    edited.receipt.source_digest = source_digest.value();
    edited.receipt.result_digest = result_digest.value();
    edited.receipt.affected_key_count = affected;
    return core::Result<CurveEditResult>::success(std::move(edited));
}

double seconds_between(std::int64_t from, std::int64_t to, std::uint32_t ticks_per_second) {
    return (static_cast<double>(to) - static_cast<double>(from)) /
        static_cast<double>(ticks_per_second);
}

// Applies new ticks to the selected keys, refusing collisions. `retimed`
// maps an old selected tick to its new tick.
core::Result<KeyedCurve> retime(
    const KeyedCurve& curve,
    const std::map<std::int64_t, std::int64_t>& retimed,
    double tangent_divisor) {
    std::set<std::int64_t> occupied;
    for (const CurveKey& key : curve.keys) {
        if (!retimed.contains(key.time.tick)) occupied.insert(key.time.tick);
    }
    KeyedCurve result = curve;
    for (CurveKey& key : result.keys) {
        const auto found = retimed.find(key.time.tick);
        if (found == retimed.end()) continue;
        if (!occupied.insert(found->second).second) {
            return core::Result<KeyedCurve>::failure(conflict(
                "curve retime would place two keys on one tick")
                .with_context("tick=" + std::to_string(found->second)));
        }
        key.time.tick = found->second;
        key.tangent_in /= tangent_divisor;
        key.tangent_out /= tangent_divisor;
    }
    return core::Result<KeyedCurve>::success(std::move(result));
}

} // namespace

std::string_view curve_edit_kind_name(CurveEditKind kind) noexcept {
    switch (kind) {
    case CurveEditKind::insert_key: return "insert_key";
    case CurveEditKind::delete_keys: return "delete_keys";
    case CurveEditKind::offset_keys: return "offset_keys";
    case CurveEditKind::scale_keys: return "scale_keys";
    case CurveEditKind::set_tangents: return "set_tangents";
    }
    return "unknown";
}

std::string_view tangent_mode_name(TangentMode mode) noexcept {
    switch (mode) {
    case TangentMode::flat: return "flat";
    case TangentMode::linear: return "linear";
    case TangentMode::auto_smooth: return "auto_smooth";
    }
    return "unknown";
}

core::Result<void> CurveEditReceipt::validate() const {
    if (source_digest.is_zero() || result_digest.is_zero()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "curve edit receipt must bind source and result digests"));
    }
    return core::Result<void>::success();
}

std::string CurveEditReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_CURVE_EDIT_RECEIPT " << kSchemaVersion << '\n';
    output << "KIND " << curve_edit_kind_name(kind) << '\n';
    output << "SOURCE " << source_digest.hex() << '\n';
    output << "RESULT " << result_digest.hex() << '\n';
    output << "AFFECTED " << affected_key_count << '\n';
    output << "END\n";
    return output.str();
}

core::Result<CurveEditReceipt> CurveEditReceipt::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<CurveEditReceipt>::failure(parse_error(
            "curve edit receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != "CARTOGRAPHER_CURVE_EDIT_RECEIPT" ||
        !(input >> version) || version != kSchemaVersion) {
        return core::Result<CurveEditReceipt>::failure(parse_error(
            "curve edit receipt framing is invalid"));
    }
    CurveEditReceipt receipt;
    std::string kind_token;
    std::string encoded_source;
    std::string encoded_result;
    std::string token;
    if (!(input >> token) || token != "KIND" || !(input >> kind_token) ||
        !(input >> token) || token != "SOURCE" || !(input >> encoded_source) ||
        !(input >> token) || token != "RESULT" || !(input >> encoded_result) ||
        !(input >> token) || token != "AFFECTED" ||
        !(input >> receipt.affected_key_count) ||
        !(input >> token) || token != "END") {
        return core::Result<CurveEditReceipt>::failure(parse_error(
            "curve edit receipt body is invalid"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CurveEditReceipt>::failure(parse_error(
            "curve edit receipt has trailing data"));
    }
    if (kind_token == "insert_key") receipt.kind = CurveEditKind::insert_key;
    else if (kind_token == "delete_keys") receipt.kind = CurveEditKind::delete_keys;
    else if (kind_token == "offset_keys") receipt.kind = CurveEditKind::offset_keys;
    else if (kind_token == "scale_keys") receipt.kind = CurveEditKind::scale_keys;
    else if (kind_token == "set_tangents") receipt.kind = CurveEditKind::set_tangents;
    else {
        return core::Result<CurveEditReceipt>::failure(parse_error(
            "curve edit receipt contains an unknown edit kind"));
    }
    const auto source = assets::Sha256Digest::from_hex(encoded_source);
    const auto result = assets::Sha256Digest::from_hex(encoded_result);
    if (!source || !result) {
        return core::Result<CurveEditReceipt>::failure(parse_error(
            "curve edit receipt digests are malformed"));
    }
    receipt.source_digest = source.value();
    receipt.result_digest = result.value();
    if (auto valid = receipt.validate(); !valid) {
        return core::Result<CurveEditReceipt>::failure(valid.error());
    }
    return core::Result<CurveEditReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> CurveEditReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string text = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
}

core::Result<CurveEditResult> insert_key(
    const KeyedCurve& curve,
    const CurveKey& key,
    bool replace) {
    if (auto result = curve.validate(); !result) {
        return core::Result<CurveEditResult>::failure(result.error());
    }
    if (auto result = key.validate(); !result) {
        return core::Result<CurveEditResult>::failure(result.error());
    }
    if (key.time.ticks_per_second != curve.timebase_ticks_per_second) {
        return core::Result<CurveEditResult>::failure(invalid(
            "inserted key must be on the curve timebase"));
    }
    KeyedCurve result = curve;
    const auto existing = std::find_if(result.keys.begin(), result.keys.end(),
        [&key](const CurveKey& entry) { return entry.time.tick == key.time.tick; });
    if (existing != result.keys.end()) {
        if (!replace) {
            return core::Result<CurveEditResult>::failure(conflict(
                "a key already exists at that tick"));
        }
        *existing = key;
    } else {
        if (result.keys.size() >= KeyedCurve::kMaxKeys) {
            return core::Result<CurveEditResult>::failure(conflict(
                "keyed curve is at its key capacity"));
        }
        result.keys.push_back(key);
    }
    return seal(curve, std::move(result), CurveEditKind::insert_key, 1U);
}

core::Result<CurveEditResult> delete_keys(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks) {
    if (auto result = curve.validate(); !result) {
        return core::Result<CurveEditResult>::failure(result.error());
    }
    const auto selection = selection_of(curve, ticks);
    if (!selection) return core::Result<CurveEditResult>::failure(selection.error());
    if (selection.value().size() >= curve.keys.size()) {
        return core::Result<CurveEditResult>::failure(conflict(
            "deleting every key would leave an empty curve"));
    }
    KeyedCurve result = curve;
    result.keys.erase(
        std::remove_if(result.keys.begin(), result.keys.end(),
            [&](const CurveKey& key) { return selection.value().contains(key.time.tick); }),
        result.keys.end());
    return seal(curve, std::move(result), CurveEditKind::delete_keys, selection.value().size());
}

core::Result<CurveEditResult> offset_keys(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks,
    std::int64_t delta_ticks) {
    if (auto result = curve.validate(); !result) {
        return core::Result<CurveEditResult>::failure(result.error());
    }
    if (delta_ticks == 0) {
        return core::Result<CurveEditResult>::failure(invalid(
            "curve key offset must be non-zero"));
    }
    const auto selection = selection_of(curve, ticks);
    if (!selection) return core::Result<CurveEditResult>::failure(selection.error());
    std::map<std::int64_t, std::int64_t> retimed;
    for (const std::int64_t tick : selection.value()) {
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
        if ((delta_ticks > 0 && tick > kMax - delta_ticks) ||
            (delta_ticks < 0 && tick < kMin - delta_ticks)) {
            return core::Result<CurveEditResult>::failure(invalid(
                "curve key offset overflows the tick range"));
        }
        retimed.emplace(tick, tick + delta_ticks);
    }
    auto result = retime(curve, retimed, 1.0);
    if (!result) return core::Result<CurveEditResult>::failure(result.error());
    return seal(curve, std::move(result.value()), CurveEditKind::offset_keys,
        selection.value().size());
}

core::Result<CurveEditResult> scale_keys(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks,
    std::int64_t pivot_tick,
    std::uint32_t numerator,
    std::uint32_t denominator) {
    if (auto result = curve.validate(); !result) {
        return core::Result<CurveEditResult>::failure(result.error());
    }
    if (numerator == 0U || denominator == 0U) {
        return core::Result<CurveEditResult>::failure(invalid(
            "curve retime scale must be strictly positive"));
    }
    const auto selection = selection_of(curve, ticks);
    if (!selection) return core::Result<CurveEditResult>::failure(selection.error());
    const double scale = static_cast<double>(numerator) / static_cast<double>(denominator);
    constexpr double kTickLimit = 9.0e18;
    std::map<std::int64_t, std::int64_t> retimed;
    for (const std::int64_t tick : selection.value()) {
        const double offset = static_cast<double>(tick) - static_cast<double>(pivot_tick);
        const double scaled = static_cast<double>(pivot_tick) + std::round(offset * scale);
        if (!std::isfinite(scaled) || std::abs(scaled) > kTickLimit) {
            return core::Result<CurveEditResult>::failure(invalid(
                "curve retime overflows the tick range"));
        }
        retimed.emplace(tick, static_cast<std::int64_t>(scaled));
    }
    auto result = retime(curve, retimed, scale);
    if (!result) return core::Result<CurveEditResult>::failure(result.error());
    return seal(curve, std::move(result.value()), CurveEditKind::scale_keys,
        selection.value().size());
}

core::Result<CurveEditResult> set_tangents(
    const KeyedCurve& curve,
    const std::vector<std::int64_t>& ticks,
    TangentMode mode) {
    if (auto result = curve.validate(); !result) {
        return core::Result<CurveEditResult>::failure(result.error());
    }
    if (mode != TangentMode::flat && mode != TangentMode::linear &&
        mode != TangentMode::auto_smooth) {
        return core::Result<CurveEditResult>::failure(invalid(
            "tangent mode is not supported"));
    }
    const auto selection = selection_of(curve, ticks);
    if (!selection) return core::Result<CurveEditResult>::failure(selection.error());
    const std::uint32_t tps = curve.timebase_ticks_per_second;
    KeyedCurve result = curve;
    const std::size_t count = curve.keys.size();
    for (std::size_t index = 0U; index < count; ++index) {
        const CurveKey& key = curve.keys[index];
        if (!selection.value().contains(key.time.tick)) continue;
        CurveKey& edited = result.keys[index];
        const CurveKey* previous = index > 0U ? &curve.keys[index - 1U] : nullptr;
        const CurveKey* next = index + 1U < count ? &curve.keys[index + 1U] : nullptr;
        const auto slope = [tps](const CurveKey& from, const CurveKey& to) {
            return (to.value - from.value) / seconds_between(from.time.tick, to.time.tick, tps);
        };
        switch (mode) {
        case TangentMode::flat:
            edited.tangent_in = 0.0;
            edited.tangent_out = 0.0;
            break;
        case TangentMode::linear: {
            const double in = previous != nullptr ? slope(*previous, key) :
                (next != nullptr ? slope(key, *next) : 0.0);
            const double out = next != nullptr ? slope(key, *next) : in;
            edited.tangent_in = in;
            edited.tangent_out = out;
            break;
        }
        case TangentMode::auto_smooth: {
            const double smooth = previous != nullptr && next != nullptr ?
                slope(*previous, *next) : 0.0;
            edited.tangent_in = smooth;
            edited.tangent_out = smooth;
            break;
        }
        }
        if (!std::isfinite(edited.tangent_in) || !std::isfinite(edited.tangent_out)) {
            return core::Result<CurveEditResult>::failure(invalid(
                "tangent rewrite produced a non-finite slope"));
        }
    }
    return seal(curve, std::move(result), CurveEditKind::set_tangents,
        selection.value().size());
}

} // namespace carto::motion
