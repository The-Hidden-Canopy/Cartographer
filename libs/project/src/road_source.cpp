#include <carto/project/road_source.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace carto::project {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_identity(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > RoadSource::kMaxIdentityBytes ||
        identity.front() == '.' || identity.front() == '-' || identity.front() == '_') {
        return false;
    }
    return std::all_of(identity.begin(), identity.end(), [](char value) {
        const bool letter = (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        return letter || digit || value == '.' || value == '_' || value == '-';
    });
}

bool read_unsigned(std::istream& input, std::uint64_t& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value,
        std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size() &&
        std::isfinite(value);
}

template <typename Enum>
bool read_enum(std::istream& input, Enum& value, std::uint64_t max_value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > max_value) return false;
    value = static_cast<Enum>(encoded);
    return true;
}

} // namespace

core::Result<void> RoadSource::validate() const {
    if (!valid_identity(source_identity) || !valid_identity(centerline_identity) ||
        source_revision.value() == 0U || centerline_revision.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "road or centerline identity/revision is invalid"));
    }
    if (!std::isfinite(width_meters) || width_meters <= 0.0 ||
        width_meters > kMaxWidthMeters || !std::isfinite(shoulder_meters) ||
        shoulder_meters < 0.0 || shoulder_meters > kMaxShoulderMeters) {
        return core::Result<void>::failure(validation(
            "road width or shoulder is outside its supported bounds"));
    }
    if (!std::isfinite(maximum_grade_ratio) || maximum_grade_ratio < 0.0 ||
        maximum_grade_ratio > kMaxGradeRatio ||
        !std::isfinite(minimum_curve_radius_meters) ||
        minimum_curve_radius_meters <= 0.0 ||
        minimum_curve_radius_meters > kMaxCurveRadiusMeters) {
        return core::Result<void>::failure(validation(
            "road grade or curvature constraint is outside its supported bounds"));
    }
    if (static_cast<std::uint8_t>(surface_class) >
            static_cast<std::uint8_t>(RoadSurfaceClass::trail) ||
        static_cast<std::uint8_t>(function_class) >
            static_cast<std::uint8_t>(RoadFunctionClass::pedestrian) ||
        static_cast<std::uint8_t>(access_state) >
            static_cast<std::uint8_t>(RoadAccessState::closed) ||
        static_cast<std::uint8_t>(drainage_intent) >
            static_cast<std::uint8_t>(RoadDrainageIntent::swale_crossing)) {
        return core::Result<void>::failure(validation(
            "road source contains an unsupported semantic classification"));
    }
    return core::Result<void>::success();
}

core::Result<RoadCoverageBounds> RoadSource::coverage_bounds(
    const geometry::CurvePath& centerline) const {
    if (auto result = validate(); !result) {
        return core::Result<RoadCoverageBounds>::failure(result.error());
    }
    if (auto result = centerline.validate(); !result) {
        return core::Result<RoadCoverageBounds>::failure(result.error());
    }
    if (centerline.source_identity != centerline_identity ||
        centerline.source_revision != centerline_revision) {
        return core::Result<RoadCoverageBounds>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road coverage requires its exact authored centerline revision"));
    }

    const double lateral_extent = width_meters * 0.5 + shoulder_meters;
    RoadCoverageBounds bounds{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (const auto& segment : centerline.segments) {
        for (const auto point : {segment.p0, segment.p1, segment.p2, segment.p3}) {
            bounds.min_x = std::min(bounds.min_x, point.x - lateral_extent);
            bounds.min_y = std::min(bounds.min_y, point.y);
            bounds.min_z = std::min(bounds.min_z, point.z - lateral_extent);
            bounds.max_x = std::max(bounds.max_x, point.x + lateral_extent);
            bounds.max_y = std::max(bounds.max_y, point.y);
            bounds.max_z = std::max(bounds.max_z, point.z + lateral_extent);
        }
    }
    return core::Result<RoadCoverageBounds>::success(bounds);
}

std::string RoadSource::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "CARTOGRAPHER_ROAD_SOURCE " << kSchemaVersion << '\n';
    output << "IDENTITY " << std::quoted(source_identity) << ' '
           << source_revision.value() << '\n';
    output << "CENTERLINE " << std::quoted(centerline_identity) << ' '
           << centerline_revision.value() << '\n';
    output << "DESIGN " << width_meters << ' ' << shoulder_meters << ' '
           << maximum_grade_ratio << ' ' << minimum_curve_radius_meters << '\n';
    output << "CLASS " << static_cast<unsigned int>(surface_class) << ' '
           << static_cast<unsigned int>(function_class) << ' '
           << static_cast<unsigned int>(access_state) << ' '
           << static_cast<unsigned int>(drainage_intent) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<RoadSource> RoadSource::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RoadSource>::failure(invalid(
            "road source payload is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != "CARTOGRAPHER_ROAD_SOURCE" ||
        !read_unsigned(input, version) || version != kSchemaVersion) {
        return core::Result<RoadSource>::failure(validation(
            "road source magic or schema version is unsupported"));
    }

    RoadSource road;
    std::string record;
    std::uint64_t source_revision = 0U;
    std::uint64_t centerline_revision = 0U;
    if (!(input >> record) || record != "IDENTITY" ||
        !(input >> std::quoted(road.source_identity)) ||
        !read_unsigned(input, source_revision) || !(input >> record) ||
        record != "CENTERLINE" || !(input >> std::quoted(road.centerline_identity)) ||
        !read_unsigned(input, centerline_revision)) {
        return core::Result<RoadSource>::failure(validation(
            "road source identity records are invalid"));
    }
    road.source_revision = core::Revision{source_revision};
    road.centerline_revision = core::Revision{centerline_revision};
    if (!(input >> record) || record != "DESIGN" ||
        !read_double(input, road.width_meters) || !read_double(input, road.shoulder_meters) ||
        !read_double(input, road.maximum_grade_ratio) ||
        !read_double(input, road.minimum_curve_radius_meters)) {
        return core::Result<RoadSource>::failure(validation(
            "road source design record is invalid"));
    }
    if (!(input >> record) || record != "CLASS" ||
        !read_enum(input, road.surface_class, static_cast<std::uint64_t>(RoadSurfaceClass::trail)) ||
        !read_enum(input, road.function_class,
            static_cast<std::uint64_t>(RoadFunctionClass::pedestrian)) ||
        !read_enum(input, road.access_state,
            static_cast<std::uint64_t>(RoadAccessState::closed)) ||
        !read_enum(input, road.drainage_intent,
            static_cast<std::uint64_t>(RoadDrainageIntent::swale_crossing))) {
        return core::Result<RoadSource>::failure(validation(
            "road source classification record is invalid"));
    }
    if (!(input >> record) || record != "END") {
        return core::Result<RoadSource>::failure(validation(
            "road source is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RoadSource>::failure(validation(
            "road source contains data after END"));
    }
    if (auto result = road.validate(); !result) {
        return core::Result<RoadSource>::failure(result.error());
    }
    return core::Result<RoadSource>::success(std::move(road));
}

assets::Sha256Digest RoadSource::source_digest() const {
    const std::string encoded = serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return assets::sha256(std::span<const std::uint8_t>(bytes, encoded.size()));
}

} // namespace carto::project
