#include <carto/geometry/curve_path.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace carto::geometry {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

constexpr std::uint32_t kAdaptiveMaxDepth = 20U;
constexpr double kAdaptiveToleranceFactor = 0.01;
constexpr double kAdaptiveScaleFloor = 0.1;

struct LengthTable final {
    std::vector<double> parameters;
    std::vector<double> cumulative;
};

bool valid_identity(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > CurvePath::kMaxIdentityBytes) return false;
    for (const char value : identity) {
        const bool ascii_letter = (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if (!ascii_letter && !digit && value != '.' && value != '_' && value != '-') {
            return false;
        }
    }
    return identity.front() != '.' && identity.front() != '-' && identity.front() != '_';
}

bool bounded_point(core::Vec3d point) noexcept {
    return point.finite() &&
        std::abs(point.x) <= CurvePath::kMaxCoordinateMagnitude &&
        std::abs(point.y) <= CurvePath::kMaxCoordinateMagnitude &&
        std::abs(point.z) <= CurvePath::kMaxCoordinateMagnitude;
}

bool same_point(core::Vec3d left, core::Vec3d right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

core::Vec3d evaluate(const CubicBezierSegment& segment, double parameter) noexcept {
    const double inverse = 1.0 - parameter;
    const double inverse_squared = inverse * inverse;
    const double parameter_squared = parameter * parameter;
    return segment.p0 * (inverse_squared * inverse) +
        segment.p1 * (3.0 * inverse_squared * parameter) +
        segment.p2 * (3.0 * inverse * parameter_squared) +
        segment.p3 * (parameter_squared * parameter);
}

core::Vec3d derivative(const CubicBezierSegment& segment, double parameter) noexcept {
    const double inverse = 1.0 - parameter;
    return (segment.p1 - segment.p0) * (3.0 * inverse * inverse) +
        (segment.p2 - segment.p1) * (6.0 * inverse * parameter) +
        (segment.p3 - segment.p2) * (3.0 * parameter * parameter);
}

core::Result<core::Vec3d> sample_tangent(
    const CubicBezierSegment& segment,
    double parameter,
    double step) {
    auto tangent = derivative(segment, parameter).normalized();
    if (tangent.finite()) return core::Result<core::Vec3d>::success(tangent);

    if (parameter < 1.0) {
        const double next = std::min(1.0, parameter + step);
        tangent = (evaluate(segment, next) - evaluate(segment, parameter)).normalized();
        if (tangent.finite()) return core::Result<core::Vec3d>::success(tangent);
    }
    if (parameter > 0.0) {
        const double previous = std::max(0.0, parameter - step);
        tangent = (evaluate(segment, parameter) - evaluate(segment, previous)).normalized();
        if (tangent.finite()) return core::Result<core::Vec3d>::success(tangent);
    }
    return core::Result<core::Vec3d>::failure(validation(
        "curve sample has no finite tangent direction"));
}

double perpendicular_distance(core::Vec3d point, core::Vec3d start, core::Vec3d chord,
    double chord_length) noexcept {
    if (chord_length <= 1.0e-12) return (point - start).length();
    const core::Vec3d offset = point - start;
    const core::Vec3d cross{
        offset.y * chord.z - offset.z * chord.y,
        offset.z * chord.x - offset.x * chord.z,
        offset.x * chord.y - offset.y * chord.x};
    return cross.length() / chord_length;
}

core::Result<void> flatten_curve(const CubicBezierSegment& segment, double tolerance,
    std::uint32_t depth, double parameter_start, double parameter_end,
    std::vector<std::pair<double, core::Vec3d>>& points) {
    const core::Vec3d chord = segment.p3 - segment.p0;
    const double chord_length = chord.length();
    const double error = std::max(
        perpendicular_distance(segment.p1, segment.p0, chord, chord_length),
        perpendicular_distance(segment.p2, segment.p0, chord, chord_length));
    if (error <= tolerance || chord_length <= tolerance) {
        points.emplace_back(parameter_end, segment.p3);
        return core::Result<void>::success();
    }
    if (depth >= kAdaptiveMaxDepth) {
        return core::Result<void>::failure(validation(
            "curve segment exceeds its adaptive subdivision bound"));
    }
    const core::Vec3d q0 = (segment.p0 + segment.p1) * 0.5;
    const core::Vec3d q1 = (segment.p1 + segment.p2) * 0.5;
    const core::Vec3d q2 = (segment.p2 + segment.p3) * 0.5;
    const core::Vec3d r0 = (q0 + q1) * 0.5;
    const core::Vec3d r1 = (q1 + q2) * 0.5;
    const core::Vec3d mid = (r0 + r1) * 0.5;
    const CubicBezierSegment left{segment.p0, q0, r0, mid};
    const CubicBezierSegment right{mid, r1, q2, segment.p3};
    const double parameter_mid =
        parameter_start + (parameter_end - parameter_start) * 0.5;
    if (auto result = flatten_curve(left, tolerance, depth + 1U, parameter_start,
            parameter_mid, points); !result) {
        return result;
    }
    return flatten_curve(right, tolerance, depth + 1U, parameter_mid,
        parameter_end, points);
}

core::Result<LengthTable> build_length_table(const CubicBezierSegment& segment,
    double tolerance) {
    std::vector<std::pair<double, core::Vec3d>> points;
    points.emplace_back(0.0, segment.p0);
    if (auto result = flatten_curve(segment, tolerance, 0U, 0.0, 1.0, points);
        !result) {
        return core::Result<LengthTable>::failure(result.error());
    }
    LengthTable table;
    table.parameters.reserve(points.size());
    table.cumulative.reserve(points.size());
    table.parameters.push_back(0.0);
    table.cumulative.push_back(0.0);
    for (std::size_t index = 1U; index < points.size(); ++index) {
        table.parameters.push_back(points[index].first);
        table.cumulative.push_back(table.cumulative.back() +
            (points[index].second - points[index - 1U].second).length());
    }
    return core::Result<LengthTable>::success(std::move(table));
}

double parameter_at_length(const LengthTable& table, double length) noexcept {
    const auto upper =
        std::lower_bound(table.cumulative.begin(), table.cumulative.end(), length);
    if (upper == table.cumulative.begin()) return table.parameters.front();
    if (upper == table.cumulative.end()) return table.parameters.back();
    const std::size_t index = static_cast<std::size_t>(upper - table.cumulative.begin());
    const double span = table.cumulative[index] - table.cumulative[index - 1U];
    const double fraction =
        span > 0.0 ? (length - table.cumulative[index - 1U]) / span : 0.0;
    return table.parameters[index - 1U] +
        fraction * (table.parameters[index] - table.parameters[index - 1U]);
}

bool read_unsigned(std::istream& input, std::uint64_t& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value,
        std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() &&
        std::isfinite(value);
}

} // namespace

core::Result<void> CurvePath::validate() const {
    if (!valid_identity(source_identity) || source_revision.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "curve source identity or revision is invalid"));
    }
    if (segments.empty() || segments.size() > kMaxSegments) {
        return core::Result<void>::failure(validation(
            "curve segment count is outside its supported bound"));
    }
    for (std::size_t index = 0U; index < segments.size(); ++index) {
        const auto& segment = segments[index];
        if (!bounded_point(segment.p0) || !bounded_point(segment.p1) ||
            !bounded_point(segment.p2) || !bounded_point(segment.p3)) {
            return core::Result<void>::failure(validation(
                "curve segment contains a non-finite or out-of-range control point"));
        }
        if (same_point(segment.p0, segment.p1) && same_point(segment.p0, segment.p2) &&
            same_point(segment.p0, segment.p3)) {
            return core::Result<void>::failure(validation(
                "curve segment cannot be a single repeated point"));
        }
        if (index > 0U && !same_point(segments[index - 1U].p3, segment.p0)) {
            return core::Result<void>::failure(validation(
                "adjacent curve segments must share their endpoint exactly"));
        }
    }
    return core::Result<void>::success();
}

core::Result<std::vector<CurveSample>> CurvePath::sample_uniform(
    std::uint32_t samples_per_segment) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<CurveSample>>::failure(result.error());
    }
    if (samples_per_segment == 0U || samples_per_segment > kMaxSamplesPerSegment ||
        segments.size() > kMaxSamples / (static_cast<std::size_t>(samples_per_segment) + 1U)) {
        return core::Result<std::vector<CurveSample>>::failure(invalid(
            "curve sampling request exceeds its bounded sample budget"));
    }

    std::vector<CurveSample> samples;
    samples.reserve(segments.size() * (static_cast<std::size_t>(samples_per_segment) + 1U));
    const double step = 1.0 / static_cast<double>(samples_per_segment);
    for (std::size_t segment_index = 0U; segment_index < segments.size(); ++segment_index) {
        const auto& segment = segments[segment_index];
        for (std::uint32_t sample_index = 0U;
             sample_index <= samples_per_segment; ++sample_index) {
            const double parameter = sample_index == samples_per_segment
                ? 1.0 : static_cast<double>(sample_index) * step;
            const auto tangent = sample_tangent(segment, parameter, step);
            if (!tangent) {
                return core::Result<std::vector<CurveSample>>::failure(
                    tangent.error().with_context("curve sampling"));
            }
            samples.push_back(CurveSample{
                evaluate(segment, parameter), tangent.value(),
                static_cast<std::uint32_t>(segment_index), parameter});
        }
    }
    return core::Result<std::vector<CurveSample>>::success(std::move(samples));
}

core::Result<std::vector<CurveSample>> CurvePath::sample_arc_length(
    double target_spacing) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<CurveSample>>::failure(result.error());
    }
    if (!std::isfinite(target_spacing) || target_spacing <= 0.0) {
        return core::Result<std::vector<CurveSample>>::failure(invalid(
            "curve arc-length spacing must be a positive finite value"));
    }

    std::vector<CurveSample> samples;
    for (std::size_t segment_index = 0U; segment_index < segments.size(); ++segment_index) {
        const auto& segment = segments[segment_index];
        const double scale = std::max({
            (segment.p1 - segment.p0).length(),
            (segment.p2 - segment.p0).length(),
            (segment.p3 - segment.p0).length()});
        const double tolerance = kAdaptiveToleranceFactor *
            std::min(target_spacing, kAdaptiveScaleFloor * scale);
        auto table = build_length_table(segment, tolerance);
        if (!table) {
            return core::Result<std::vector<CurveSample>>::failure(
                table.error().with_context("curve arc-length sampling"));
        }
        const double total_length = table.value().cumulative.back();
        if (!std::isfinite(total_length)) {
            return core::Result<std::vector<CurveSample>>::failure(validation(
                "curve arc-length integration produced a non-finite length"));
        }
        const bool shares_start =
            !samples.empty() && same_point(samples.back().position, segment.p0);
        const std::size_t remaining = kMaxSamples - samples.size();
        std::uint64_t stations = 0U;
        double station_spacing = total_length;
        if (total_length > 0.0) {
            const double quotient = total_length / target_spacing;
            if (!(quotient < static_cast<double>(kMaxSamples))) {
                return core::Result<std::vector<CurveSample>>::failure(invalid(
                    "curve arc-length sampling cannot honor the requested spacing "
                    "within its bounded sample budget"));
            }
            stations = static_cast<std::uint64_t>(std::ceil(quotient));
            const std::uint64_t needed = stations + 1U;
            if (needed - (shares_start ? 1U : 0U) > remaining) {
                return core::Result<std::vector<CurveSample>>::failure(invalid(
                    "curve arc-length sampling cannot honor the requested spacing "
                    "within its bounded sample budget"));
            }
            station_spacing = total_length / static_cast<double>(stations);
        }
        const double tangent_step = total_length > 0.0
            ? std::clamp(station_spacing / total_length, 1.0e-6, 1.0)
            : 1.0e-3;

        if (!shares_start) {
            const auto tangent = sample_tangent(segment, 0.0, tangent_step);
            if (!tangent) {
                return core::Result<std::vector<CurveSample>>::failure(
                    tangent.error().with_context("curve arc-length sampling"));
            }
            samples.push_back(CurveSample{segment.p0, tangent.value(),
                static_cast<std::uint32_t>(segment_index), 0.0});
        }
        for (std::uint64_t station = 1U; station <= stations; ++station) {
            const double parameter = station == stations
                ? 1.0
                : parameter_at_length(table.value(),
                      static_cast<double>(station) * station_spacing);
            const auto tangent = sample_tangent(segment, parameter, tangent_step);
            if (!tangent) {
                return core::Result<std::vector<CurveSample>>::failure(
                    tangent.error().with_context("curve arc-length sampling"));
            }
            samples.push_back(CurveSample{evaluate(segment, parameter), tangent.value(),
                static_cast<std::uint32_t>(segment_index), parameter});
        }
    }
    return core::Result<std::vector<CurveSample>>::success(std::move(samples));
}

std::string CurvePath::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "CARTOGRAPHER_CURVE_PATH " << kSchemaVersion << '\n';
    output << "IDENTITY " << std::quoted(source_identity) << ' '
           << source_revision.value() << '\n';
    output << "SEGMENTS " << segments.size() << '\n';
    for (const auto& segment : segments) {
        output << "SEGMENT";
        for (const auto point : {segment.p0, segment.p1, segment.p2, segment.p3}) {
            output << ' ' << point.x << ' ' << point.y << ' ' << point.z;
        }
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<CurvePath> CurvePath::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<CurvePath>::failure(invalid(
            "curve path payload is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || !read_unsigned(input, version) ||
        magic != "CARTOGRAPHER_CURVE_PATH" || version != kSchemaVersion) {
        return core::Result<CurvePath>::failure(validation(
            "curve path header or schema version is invalid"));
    }

    CurvePath path;
    std::string token;
    if (!(input >> token) || token != "IDENTITY" ||
        !(input >> std::quoted(path.source_identity)) ||
        !read_unsigned(input, version)) {
        return core::Result<CurvePath>::failure(validation(
            "curve path identity record is malformed"));
    }
    path.source_revision = core::Revision{version};

    std::uint64_t segment_count = 0U;
    if (!(input >> token) || token != "SEGMENTS" || !read_unsigned(input, segment_count) ||
        segment_count == 0U || segment_count > kMaxSegments) {
        return core::Result<CurvePath>::failure(validation(
            "curve path segment table is malformed or exceeds its bound"));
    }
    path.segments.reserve(static_cast<std::size_t>(segment_count));
    for (std::uint64_t index = 0U; index < segment_count; ++index) {
        if (!(input >> token) || token != "SEGMENT") {
            return core::Result<CurvePath>::failure(validation(
                "curve path is missing a segment record"));
        }
        CubicBezierSegment segment;
        std::array<core::Vec3d*, 4> points{
            &segment.p0, &segment.p1, &segment.p2, &segment.p3};
        for (core::Vec3d* point : points) {
            if (!read_double(input, point->x) || !read_double(input, point->y) ||
                !read_double(input, point->z)) {
                return core::Result<CurvePath>::failure(validation(
                    "curve path segment contains a malformed control point"));
            }
        }
        path.segments.push_back(segment);
    }
    if (!(input >> token) || token != "END") {
        return core::Result<CurvePath>::failure(validation(
            "curve path is missing its END record"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<CurvePath>::failure(validation(
            "curve path contains trailing unbound data"));
    }
    if (auto result = path.validate(); !result) {
        return core::Result<CurvePath>::failure(result.error());
    }
    return core::Result<CurvePath>::success(std::move(path));
}

} // namespace carto::geometry
