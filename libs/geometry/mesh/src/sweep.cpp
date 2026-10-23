#include <carto/geometry/sweep.hpp>

#include <algorithm>
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

constexpr double kFrameEpsilon = 1.0e-12;
constexpr double kJointTangentDotMinimum = 1.0 - 1.0e-8;
constexpr double kMaxProfileCoordinate = 1.0e4;
constexpr double kMaxScale = 100.0;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_identity(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > SweepSource::kMaxIdentityBytes) return false;
    for (const char value : identity) {
        const bool letter = (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if (!letter && !digit && value != '.' && value != '_' && value != '-') return false;
    }
    return identity.front() != '.' && identity.front() != '-' && identity.front() != '_';
}

bool valid_cap_policy(SweepCapPolicy policy) noexcept {
    return policy == SweepCapPolicy::open || policy == SweepCapPolicy::both ||
        policy == SweepCapPolicy::start || policy == SweepCapPolicy::end;
}

double cross2(core::Vec2d a, core::Vec2d b, core::Vec2d c) noexcept {
    return (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
}

double polygon_area_twice(const std::vector<core::Vec2d>& points) noexcept {
    double area = 0.0;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto& current = points[index];
        const auto& next = points[(index + 1U) % points.size()];
        area += current.x * next.y - next.x * current.y;
    }
    return area;
}

int orientation_sign(double value) noexcept {
    constexpr double kEpsilon = 1.0e-12;
    if (value > kEpsilon) return 1;
    if (value < -kEpsilon) return -1;
    return 0;
}

bool on_segment(core::Vec2d a, core::Vec2d b, core::Vec2d point) noexcept {
    constexpr double kEpsilon = 1.0e-12;
    return point.x >= std::min(a.x, b.x) - kEpsilon &&
        point.x <= std::max(a.x, b.x) + kEpsilon &&
        point.y >= std::min(a.y, b.y) - kEpsilon &&
        point.y <= std::max(a.y, b.y) + kEpsilon;
}

bool segments_intersect(
    core::Vec2d a, core::Vec2d b, core::Vec2d c, core::Vec2d d) noexcept {
    const auto orient = [](core::Vec2d p, core::Vec2d q, core::Vec2d r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    const int abc = orientation_sign(orient(a, b, c));
    const int abd = orientation_sign(orient(a, b, d));
    const int cda = orientation_sign(orient(c, d, a));
    const int cdb = orientation_sign(orient(c, d, b));
    if (abc != abd && cda != cdb && abc != 0 && abd != 0 && cda != 0 && cdb != 0) {
        return true;
    }
    return (abc == 0 && on_segment(a, b, c)) ||
        (abd == 0 && on_segment(a, b, d)) ||
        (cda == 0 && on_segment(c, d, a)) ||
        (cdb == 0 && on_segment(c, d, b));
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

core::Vec3d rotate_about_axis(
    core::Vec3d value, core::Vec3d axis, double sine, double cosine) noexcept {
    return value * cosine + core::cross(axis, value) * sine +
        axis * (core::dot(axis, value) * (1.0 - cosine));
}

struct Frame final {
    core::Vec3d tangent;
    core::Vec3d binormal;
    core::Vec3d normal;
};

core::Result<Frame> initial_frame(core::Vec3d tangent, core::Vec3d reference_up) {
    const auto projected_up = reference_up - tangent * core::dot(reference_up, tangent);
    const auto normal = projected_up.normalized();
    if (!normal.finite()) {
        return core::Result<Frame>::failure(validation(
            "sweep reference frame is parallel to the initial centerline tangent"));
    }
    const auto binormal = core::cross(tangent, normal).normalized();
    if (!binormal.finite()) {
        return core::Result<Frame>::failure(validation(
            "sweep reference frame could not produce an orthogonal basis"));
    }
    return core::Result<Frame>::success(Frame{tangent, binormal, normal});
}

core::Result<Frame> transport_frame(const Frame& previous, core::Vec3d tangent) {
    const double cosine = std::clamp(core::dot(previous.tangent, tangent), -1.0, 1.0);
    auto axis = core::cross(previous.tangent, tangent);
    const double sine = axis.length();
    if (!std::isfinite(sine)) {
        return core::Result<Frame>::failure(validation(
            "sweep frame transport encountered a non-finite tangent change"));
    }
    if (cosine < -1.0 + 1.0e-10) {
        return core::Result<Frame>::failure(validation(
            "sweep centerline contains an ambiguous near-180-degree reversal"));
    }

    core::Vec3d normal = previous.normal;
    if (sine > kFrameEpsilon) {
        axis = axis * (1.0 / sine);
        normal = rotate_about_axis(normal, axis, sine, cosine);
    }
    normal = (normal - tangent * core::dot(normal, tangent)).normalized();
    const auto binormal = core::cross(tangent, normal).normalized();
    if (!normal.finite() || !binormal.finite()) {
        return core::Result<Frame>::failure(validation(
            "sweep frame transport could not maintain an orthogonal basis"));
    }
    return core::Result<Frame>::success(Frame{tangent, binormal, normal});
}

} // namespace

core::Result<void> SweepSource::validate() const {
    if (!valid_identity(source_identity) || source_revision.value() == 0U ||
        !valid_identity(centerline_identity) || centerline_revision.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "sweep source or centerline identity/revision is invalid"));
    }
    if (profile.size() < 3U || profile.size() > kMaxProfilePoints) {
        return core::Result<void>::failure(validation(
            "sweep profile point count is outside its supported bound"));
    }
    const double reference_up_length = reference_up.length();
    if (!reference_up.finite() || !std::isfinite(reference_up_length) ||
        reference_up_length <= kFrameEpsilon) {
        return core::Result<void>::failure(validation(
            "sweep reference-up vector must be finite and nonzero"));
    }
    if (samples_per_segment == 0U ||
        samples_per_segment > CurvePath::kMaxSamplesPerSegment ||
        samples_per_segment > kMaxStations ||
        start_scale <= 0.0 || end_scale <= 0.0 ||
        start_scale > kMaxScale || end_scale > kMaxScale ||
        !std::isfinite(start_scale) || !std::isfinite(end_scale)) {
        return core::Result<void>::failure(validation(
            "sweep sampling or endpoint scale is outside its supported bound"));
    }
    if (!valid_cap_policy(cap_policy)) {
        return core::Result<void>::failure(invalid("sweep cap policy is unknown"));
    }
    for (const auto point : profile) {
        if (!point.finite() || std::abs(point.x) > kMaxProfileCoordinate ||
            std::abs(point.y) > kMaxProfileCoordinate) {
            return core::Result<void>::failure(validation(
                "sweep profile contains a non-finite or out-of-range point"));
        }
    }
    if (!(polygon_area_twice(profile) > kFrameEpsilon)) {
        return core::Result<void>::failure(validation(
            "sweep profile must have positive counter-clockwise area"));
    }
    for (std::size_t index = 0U; index < profile.size(); ++index) {
        const auto& current = profile[index];
        const auto& next = profile[(index + 1U) % profile.size()];
        const auto& after_next = profile[(index + 2U) % profile.size()];
        if (std::hypot(next.x - current.x, next.y - current.y) <= kFrameEpsilon ||
            cross2(current, next, after_next) <= kFrameEpsilon) {
            return core::Result<void>::failure(validation(
                "sweep profile must be strictly convex with distinct ordered points"));
        }
    }
    for (std::size_t first = 0U; first < profile.size(); ++first) {
        const std::size_t first_next = (first + 1U) % profile.size();
        for (std::size_t second = first + 1U; second < profile.size(); ++second) {
            const std::size_t second_next = (second + 1U) % profile.size();
            if (first == second || first_next == second || second_next == first) continue;
            if (segments_intersect(
                profile[first], profile[first_next], profile[second], profile[second_next])) {
                return core::Result<void>::failure(validation(
                    "sweep profile contains intersecting non-adjacent edges"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string SweepSource::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "CARTOGRAPHER_SWEEP_SOURCE " << kSchemaVersion << '\n';
    output << "IDENTITY " << std::quoted(source_identity) << ' '
           << source_revision.value() << '\n';
    output << "CENTERLINE " << std::quoted(centerline_identity) << ' '
           << centerline_revision.value() << '\n';
    output << "SAMPLING " << samples_per_segment << '\n';
    output << "FRAME " << reference_up.x << ' ' << reference_up.y << ' '
           << reference_up.z << '\n';
    output << "SCALE " << start_scale << ' ' << end_scale << '\n';
    output << "CAPS " << static_cast<unsigned>(cap_policy) << '\n';
    output << "PROFILE " << profile.size() << '\n';
    for (const auto point : profile) output << "POINT " << point.x << ' ' << point.y << '\n';
    return output.str();
}

core::Result<SweepSource> SweepSource::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source is empty or exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string marker;
    std::uint64_t version = 0U;
    if (!(input >> marker) || marker != "CARTOGRAPHER_SWEEP_SOURCE" ||
        !read_unsigned(input, version) || version != kSchemaVersion) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source header or schema version is unsupported"));
    }

    SweepSource source;
    std::string field;
    std::uint64_t source_revision = 0U;
    std::uint64_t centerline_revision = 0U;
    if (!(input >> field) || field != "IDENTITY" ||
        !(input >> std::quoted(source.source_identity)) ||
        !read_unsigned(input, source_revision) ||
        !(input >> field) || field != "CENTERLINE" ||
        !(input >> std::quoted(source.centerline_identity)) ||
        !read_unsigned(input, centerline_revision)) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source identity block is malformed"));
    }
    source.source_revision = core::Revision{source_revision};
    source.centerline_revision = core::Revision{centerline_revision};

    std::uint64_t sample_count = 0U;
    if (!(input >> field) || field != "SAMPLING" || !read_unsigned(input, sample_count) ||
        sample_count > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source sampling block is malformed"));
    }
    source.samples_per_segment = static_cast<std::uint32_t>(sample_count);
    if (!(input >> field) || field != "FRAME" ||
        !read_double(input, source.reference_up.x) ||
        !read_double(input, source.reference_up.y) ||
        !read_double(input, source.reference_up.z) ||
        !(input >> field) || field != "SCALE" ||
        !read_double(input, source.start_scale) || !read_double(input, source.end_scale)) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source frame or scale block is malformed"));
    }
    std::uint64_t encoded_caps = 0U;
    if (!(input >> field) || field != "CAPS" || !read_unsigned(input, encoded_caps) ||
        encoded_caps > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source cap block is malformed"));
    }
    source.cap_policy = static_cast<SweepCapPolicy>(encoded_caps);

    std::uint64_t profile_count = 0U;
    if (!(input >> field) || field != "PROFILE" || !read_unsigned(input, profile_count) ||
        profile_count > kMaxProfilePoints) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source profile block exceeds its bounded capacity"));
    }
    source.profile.reserve(static_cast<std::size_t>(profile_count));
    for (std::uint64_t index = 0U; index < profile_count; ++index) {
        core::Vec2d point;
        if (!(input >> field) || field != "POINT" ||
            !read_double(input, point.x) || !read_double(input, point.y)) {
            return core::Result<SweepSource>::failure(validation(
                "sweep source profile point is malformed"));
        }
        source.profile.push_back(point);
    }
    if (input >> field) {
        return core::Result<SweepSource>::failure(validation(
            "sweep source contains trailing data"));
    }
    if (auto result = source.validate(); !result) {
        return core::Result<SweepSource>::failure(result.error());
    }
    return core::Result<SweepSource>::success(std::move(source));
}

core::Result<SweptMeshProduct> build_sweep_mesh(
    const SweepSource& source,
    const CurvePath& centerline) {
    if (auto result = source.validate(); !result) {
        return core::Result<SweptMeshProduct>::failure(result.error());
    }
    if (auto result = centerline.validate(); !result) {
        return core::Result<SweptMeshProduct>::failure(
            result.error().with_context("sweep centerline"));
    }
    if (source.centerline_identity != centerline.source_identity ||
        source.centerline_revision != centerline.source_revision) {
        return core::Result<SweptMeshProduct>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep source does not match the exact centerline identity and revision"));
    }
    const std::size_t segment_count = centerline.segments.size();
    if (source.samples_per_segment > (SweepSource::kMaxStations - 1U) / segment_count) {
        return core::Result<SweptMeshProduct>::failure(validation(
            "sweep sampling request exceeds the station budget"));
    }
    const std::size_t station_budget =
        segment_count * static_cast<std::size_t>(source.samples_per_segment) + 1U;
    const std::size_t generated_vertices = station_budget * source.profile.size();
    const std::size_t cap_faces = source.cap_policy == SweepCapPolicy::both ? 2U :
        ((source.cap_policy == SweepCapPolicy::start ||
          source.cap_policy == SweepCapPolicy::end) ? 1U : 0U);
    const std::size_t generated_faces =
        (station_budget - 1U) * source.profile.size() + cap_faces;
    if (generated_vertices > SweepSource::kMaxGeneratedElements ||
        generated_faces > SweepSource::kMaxGeneratedElements) {
        return core::Result<SweptMeshProduct>::failure(validation(
            "sweep output exceeds the generated element budget"));
    }

    auto sampled = centerline.sample_uniform(source.samples_per_segment);
    if (!sampled) {
        return core::Result<SweptMeshProduct>::failure(
            sampled.error().with_context("sweep centerline sampling"));
    }
    std::vector<CurveSample> stations;
    stations.reserve(station_budget);
    for (const auto& sample : sampled.value()) {
        if (!stations.empty() &&
            sample.position.x == stations.back().position.x &&
            sample.position.y == stations.back().position.y &&
            sample.position.z == stations.back().position.z) {
            if (core::dot(stations.back().tangent, sample.tangent) <
                kJointTangentDotMinimum) {
                return core::Result<SweptMeshProduct>::failure(validation(
                    "sweep centerline contains a tangent-discontinuous segment joint"));
            }
            stations.back().tangent =
                (stations.back().tangent + sample.tangent).normalized();
            continue;
        }
        if (!stations.empty() &&
            (sample.position - stations.back().position).length() <= kFrameEpsilon) {
            return core::Result<SweptMeshProduct>::failure(validation(
                "sweep centerline contains consecutive coincident stations"));
        }
        stations.push_back(sample);
    }
    if (stations.size() < 2U || stations.size() > SweepSource::kMaxStations) {
        return core::Result<SweptMeshProduct>::failure(validation(
            "sweep station count is outside its supported bound"));
    }
    const std::size_t station_vertices = stations.size() * source.profile.size();
    const std::size_t station_faces = (stations.size() - 1U) * source.profile.size() + cap_faces;
    if (station_vertices > SweepSource::kMaxGeneratedElements ||
        station_faces > SweepSource::kMaxGeneratedElements) {
        return core::Result<SweptMeshProduct>::failure(validation(
            "sweep output exceeds the generated element budget"));
    }

    std::vector<Frame> frames;
    frames.reserve(stations.size());
    auto first_frame = initial_frame(stations.front().tangent.normalized(), source.reference_up);
    if (!first_frame) {
        return core::Result<SweptMeshProduct>::failure(first_frame.error());
    }
    frames.push_back(first_frame.value());
    for (std::size_t index = 1U; index < stations.size(); ++index) {
        const auto tangent = stations[index].tangent.normalized();
        if (!tangent.finite()) {
            return core::Result<SweptMeshProduct>::failure(validation(
                "sweep centerline contains a degenerate tangent"));
        }
        auto transported = transport_frame(frames.back(), tangent);
        if (!transported) {
            return core::Result<SweptMeshProduct>::failure(transported.error());
        }
        frames.push_back(transported.value());
    }

    EditableMesh mesh;
    std::vector<std::vector<VertexId>> rings;
    rings.reserve(stations.size());
    for (std::size_t station_index = 0U; station_index < stations.size(); ++station_index) {
        const double t = static_cast<double>(station_index) /
            static_cast<double>(stations.size() - 1U);
        const double scale = source.start_scale + (source.end_scale - source.start_scale) * t;
        std::vector<VertexId> ring;
        ring.reserve(source.profile.size());
        for (const auto point : source.profile) {
            const auto position = stations[station_index].position +
                (frames[station_index].binormal * point.x +
                 frames[station_index].normal * point.y) * scale;
            auto added = mesh.add_vertex(position);
            if (!added) {
                return core::Result<SweptMeshProduct>::failure(
                    added.error().with_context("sweep vertex generation"));
            }
            ring.push_back(added.value());
        }
        rings.push_back(std::move(ring));
    }

    for (std::size_t station_index = 0U; station_index + 1U < rings.size(); ++station_index) {
        for (std::size_t profile_index = 0U; profile_index < source.profile.size();
             ++profile_index) {
            const std::size_t next = (profile_index + 1U) % source.profile.size();
            auto added = mesh.add_face({
                rings[station_index][profile_index],
                rings[station_index + 1U][profile_index],
                rings[station_index + 1U][next],
                rings[station_index][next],
            });
            if (!added) {
                return core::Result<SweptMeshProduct>::failure(
                    added.error().with_context("sweep side-face generation"));
            }
        }
    }

    const bool cap_start = source.cap_policy == SweepCapPolicy::both ||
        source.cap_policy == SweepCapPolicy::start;
    const bool cap_end = source.cap_policy == SweepCapPolicy::both ||
        source.cap_policy == SweepCapPolicy::end;
    if (cap_start) {
        auto added = mesh.add_face(rings.front());
        if (!added) {
            return core::Result<SweptMeshProduct>::failure(
                added.error().with_context("sweep start cap generation"));
        }
    }
    if (cap_end) {
        auto end_cap = rings.back();
        std::reverse(end_cap.begin(), end_cap.end());
        auto added = mesh.add_face(std::move(end_cap));
        if (!added) {
            return core::Result<SweptMeshProduct>::failure(
                added.error().with_context("sweep end cap generation"));
        }
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<SweptMeshProduct>::failure(
            result.error().with_context("generated sweep mesh"));
    }
    return core::Result<SweptMeshProduct>::success(SweptMeshProduct{
        std::move(mesh), source.source_identity, source.source_revision,
        centerline.source_identity, centerline.source_revision});
}

} // namespace carto::geometry
