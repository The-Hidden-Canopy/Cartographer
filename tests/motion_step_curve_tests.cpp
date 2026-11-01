#include <carto/motion/curve.hpp>
#include <carto/motion/curve_edit.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <string_view>

using namespace carto;
namespace {
class CommaPunctuation final : public std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};
void require(bool ok, std::string_view message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
motion::KeyedCurve make_step() {
    motion::KeyedCurve curve;
    curve.id = motion::MotionCurveId{9};
    curve.timebase_ticks_per_second = 30;
    curve.interpolation = motion::InterpolationKind::step;
    curve.keys = {{{-10, 30}, 0, 0, 0}, {{10, 30}, 1, 0, 0}, {{20, 30}, 3, 0, 0}};
    return curve;
}
void check_sample(const motion::KeyedCurve& curve, std::int64_t tick, double expected) {
    const auto sample = curve.sample({tick, 30});
    require(sample && sample.value() == expected, "step samples exact held value");
}
void replace_once(std::string& text, std::string_view old, std::string_view value) {
    const auto position = text.find(old);
    require(position != std::string::npos, "mutation target exists");
    text.replace(position, old.size(), value);
}
}

int main() {
    const auto curve = make_step();
    require(static_cast<bool>(curve.validate()), "valid step curve");
    for (auto tick : {-100LL, -10LL, -9LL, 0LL, 9LL}) check_sample(curve, tick, 0);
    for (auto tick : {10LL, 11LL, 19LL}) check_sample(curve, tick, 1);
    for (auto tick : {20LL, 21LL, 100LL}) check_sample(curve, tick, 3);
    require(!curve.sample({10, 60}), "different timebase is refused");

    const auto bytes = curve.serialize();
    require(bytes.starts_with("CARTO_KEYED_CURVE 2\n") &&
        bytes.find("INTERPOLATION step\n") != std::string::npos,
        "step uses version 2 with explicit interpolation");
    const auto decoded = motion::KeyedCurve::deserialize(bytes);
    require(decoded && decoded.value().serialize() == bytes &&
        decoded.value().canonical_digest().value() == curve.canonical_digest().value(),
        "step codec and digest round trip");
    std::string malformed = bytes;
    replace_once(malformed, "CARTO_KEYED_CURVE 2", "CARTO_KEYED_CURVE 1");
    require(!motion::KeyedCurve::deserialize(malformed), "v1 cannot claim step");
    malformed = bytes;
    replace_once(malformed, "INTERPOLATION step", "INTERPOLATION linear");
    require(!motion::KeyedCurve::deserialize(malformed), "v2 cannot masquerade as legacy interpolation");
    malformed = bytes;
    replace_once(malformed, "INTERPOLATION step", "INTERPOLATION unknown");
    require(!motion::KeyedCurve::deserialize(malformed), "unknown interpolation refused");
    malformed = bytes;
    replace_once(malformed, "CARTO_KEYED_CURVE 2", "CARTO_KEYED_CURVE 3");
    require(!motion::KeyedCurve::deserialize(malformed), "future version refused");
    require(!motion::KeyedCurve::deserialize(bytes + "EXTRA"), "trailing data refused");
    require(!motion::KeyedCurve::deserialize(bytes + "\n"), "noncanonical trailing whitespace refused");
    auto invalid_keys = curve;
    invalid_keys.keys[1].time.tick = invalid_keys.keys[0].time.tick;
    require(!motion::KeyedCurve::deserialize(invalid_keys.serialize()), "duplicate key tick refused");
    invalid_keys = curve;
    invalid_keys.keys[1].time.tick = -11;
    require(!motion::KeyedCurve::deserialize(invalid_keys.serialize()), "out-of-order key refused");
    invalid_keys = curve;
    invalid_keys.keys[1].time.ticks_per_second = 60;
    require(!motion::KeyedCurve::deserialize(invalid_keys.serialize()), "key timebase mismatch refused");

    auto fractional = curve;
    fractional.keys[1].value = 1.25;
    const auto canonical = fractional.serialize();
    const auto previous_locale = std::locale();
    std::locale::global(std::locale(previous_locale, new CommaPunctuation));
    const auto localized = fractional.serialize();
    const auto localized_read = motion::KeyedCurve::deserialize(canonical);
    std::locale::global(previous_locale);
    require(localized == canonical && localized_read &&
        localized_read.value().canonical_digest().value() == fractional.canonical_digest().value(),
        "v2 codec and digest do not depend on process locale");

    auto legacy = curve;
    legacy.interpolation = motion::InterpolationKind::linear;
    const std::string legacy_bytes =
        "CARTO_KEYED_CURVE 1\nID 9\nTIMEBASE 30\nINTERPOLATION linear\n"
        "KEYS 3\nKEY -10 30 0 0 0\nKEY 10 30 1 0 0\nKEY 20 30 3 0 0\nEND\n";
    require(legacy.serialize() == legacy_bytes, "legacy v1 bytes unchanged");
    const auto legacy_read = motion::KeyedCurve::deserialize(legacy_bytes);
    require(legacy_read && legacy_read.value().serialize() == legacy_bytes &&
        legacy_read.value().sample({0, 30}).value() == 0.5, "v1 linear semantics retained");

    const auto original_digest = curve.canonical_digest().value();
    const auto edited = motion::insert_key(curve, {{0, 30}, 2, 0, 0});
    require(edited && edited.value().receipt.validate() &&
        edited.value().receipt.source_digest == original_digest &&
        edited.value().receipt.result_digest == edited.value().curve.canonical_digest().value() &&
        curve.canonical_digest().value() == original_digest, "key edit is pure and digest bound");
    check_sample(edited.value().curve, -1, 0);
    check_sample(edited.value().curve, 0, 2);
    check_sample(edited.value().curve, 9, 2);
    check_sample(edited.value().curve, 10, 1);
    require(!motion::insert_key(curve, {{10, 30}, 4, 0, 0}), "duplicate tick refuses without replace");
    require(!motion::set_tangents(curve, {10}, motion::TangentMode::linear) &&
        curve.canonical_digest().value() == original_digest,
        "meaningless step tangent edits refuse without mutation");

    auto extreme = curve;
    extreme.keys = {{{std::numeric_limits<std::int64_t>::min(), 30}, -1, 0, 0},
                    {{std::numeric_limits<std::int64_t>::max(), 30}, 1, 0, 0}};
    check_sample(extreme, 0, -1);
    check_sample(extreme, std::numeric_limits<std::int64_t>::max(), 1);
    std::cout << "step curve tests passed\n";
}
