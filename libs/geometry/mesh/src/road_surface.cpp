#include <carto/geometry/road_surface.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace carto::geometry {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_identity(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > CurvePath::kMaxIdentityBytes ||
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

core::Vec3d horizontal(core::Vec3d value) noexcept {
    return {value.x, 0.0, value.z};
}

double horizontal_distance(core::Vec3d left, core::Vec3d right) noexcept {
    return std::hypot(left.x - right.x, left.z - right.z);
}

bool same_position(core::Vec3d left, core::Vec3d right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

core::Result<void> validate_design(
    double road_width_meters,
    double shoulder_width_meters,
    double maximum_grade_ratio,
    double minimum_curve_radius_meters,
    std::uint32_t samples_per_segment) {
    if (!std::isfinite(road_width_meters) || road_width_meters <= 0.0 ||
        road_width_meters > 100.0 || !std::isfinite(shoulder_width_meters) ||
        shoulder_width_meters < 0.0 || shoulder_width_meters > 25.0) {
        return core::Result<void>::failure(validation(
            "road surface width or shoulder exceeds the supported design bounds"));
    }
    if (!std::isfinite(maximum_grade_ratio) || maximum_grade_ratio < 0.0 ||
        maximum_grade_ratio > 1.0 || !std::isfinite(minimum_curve_radius_meters) ||
        minimum_curve_radius_meters <= 0.0 || minimum_curve_radius_meters > 1.0e9) {
        return core::Result<void>::failure(validation(
            "road surface grade or curve radius exceeds the supported design bounds"));
    }
    if (samples_per_segment == 0U ||
        samples_per_segment > CurvePath::kMaxSamplesPerSegment) {
        return core::Result<void>::failure(invalid(
            "road surface sampling density is outside its supported bound"));
    }
    if (minimum_curve_radius_meters <= road_width_meters * 0.5 + shoulder_width_meters) {
        return core::Result<void>::failure(validation(
            "minimum curve radius must exceed the road half-width plus shoulder envelope"));
    }
    return core::Result<void>::success();
}

double sampled_radius(
    core::Vec3d first,
    core::Vec3d middle,
    core::Vec3d last) noexcept {
    const double a = horizontal_distance(first, middle);
    const double b = horizontal_distance(middle, last);
    const double c = horizontal_distance(first, last);
    const double twice_area = std::abs(
        (middle.x - first.x) * (last.z - first.z) -
        (middle.z - first.z) * (last.x - first.x));
    if (a <= 1.0e-10 || b <= 1.0e-10) return 0.0;
    if (twice_area <= 1.0e-12 * std::max(1.0, a * b)) {
        return std::numeric_limits<double>::infinity();
    }
    return a * b * c / (2.0 * twice_area);
}

} // namespace

core::Result<void> RoadSurfaceMesh::validate() const {
    if (!valid_identity(centerline_identity) || centerline_revision.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "road surface centerline identity or revision is invalid"));
    }
    if (auto result = validate_design(road_width_meters, shoulder_width_meters,
            maximum_grade_ratio, minimum_curve_radius_meters, samples_per_segment); !result) {
        return result;
    }
    if (stations.size() < 2U || stations.size() > kMaxStations) {
        return core::Result<void>::failure(validation(
            "road surface station count is outside its supported bound"));
    }
    for (std::size_t index = 0U; index < stations.size(); ++index) {
        const auto& station = stations[index];
        if (!station.center.finite() || !station.shoulder_left.finite() ||
            !station.road_left.finite() || !station.road_right.finite() ||
            !station.shoulder_right.finite()) {
            return core::Result<void>::failure(validation(
                "road surface contains a non-finite cross-section"));
        }
        if (index > 0U && horizontal_distance(stations[index - 1U].center,
                station.center) <= 1.0e-10) {
            return core::Result<void>::failure(validation(
                "road surface contains a degenerate horizontal station interval"));
        }
    }
    return core::Result<void>::success();
}

core::Result<RoadSurfaceMesh> build_road_surface(
    const CurvePath& centerline,
    double road_width_meters,
    double shoulder_width_meters,
    double maximum_grade_ratio,
    double minimum_curve_radius_meters,
    std::uint32_t samples_per_segment) {
    if (auto result = centerline.validate(); !result) {
        return core::Result<RoadSurfaceMesh>::failure(
            result.error().with_context("road surface centerline"));
    }
    if (auto result = validate_design(road_width_meters, shoulder_width_meters,
            maximum_grade_ratio, minimum_curve_radius_meters, samples_per_segment); !result) {
        return core::Result<RoadSurfaceMesh>::failure(result.error());
    }
    if (centerline.segments.size() > (RoadSurfaceMesh::kMaxStations - 1U) /
            static_cast<std::size_t>(samples_per_segment)) {
        return core::Result<RoadSurfaceMesh>::failure(invalid(
            "road surface request exceeds its bounded station budget"));
    }

    auto sampled = centerline.sample_uniform(samples_per_segment);
    if (!sampled) {
        return core::Result<RoadSurfaceMesh>::failure(
            sampled.error().with_context("road surface sampling"));
    }
    std::vector<core::Vec3d> centers;
    centers.reserve(centerline.segments.size() *
        static_cast<std::size_t>(samples_per_segment) + 1U);
    for (const auto& sample : sampled.value()) {
        if (sample.segment_index > 0U && sample.parameter == 0.0 &&
            !centers.empty() && same_position(centers.back(), sample.position)) {
            continue;
        }
        centers.push_back(sample.position);
    }
    if (centers.size() < 2U || centers.size() > RoadSurfaceMesh::kMaxStations) {
        return core::Result<RoadSurfaceMesh>::failure(validation(
            "road centerline does not produce a supported station sequence"));
    }

    for (std::size_t index = 1U; index < centers.size(); ++index) {
        const double run = horizontal_distance(centers[index - 1U], centers[index]);
        if (!std::isfinite(run) || run <= 1.0e-10) {
            return core::Result<RoadSurfaceMesh>::failure(validation(
                "road centerline contains a zero-length horizontal interval"));
        }
        const double grade = std::abs(centers[index].y - centers[index - 1U].y) / run;
        if (!std::isfinite(grade) || grade > maximum_grade_ratio + 1.0e-10) {
            return core::Result<RoadSurfaceMesh>::failure(validation(
                "sampled road grade exceeds the authored maximum"));
        }
    }
    for (std::size_t index = 1U; index + 1U < centers.size(); ++index) {
        const double radius = sampled_radius(
            centers[index - 1U], centers[index], centers[index + 1U]);
        if (radius <= 0.0 || radius + 1.0e-9 < minimum_curve_radius_meters) {
            return core::Result<RoadSurfaceMesh>::failure(validation(
                "sampled road curvature is tighter than the authored minimum radius"));
        }
    }

    RoadSurfaceMesh product;
    product.centerline_identity = centerline.source_identity;
    product.centerline_revision = centerline.source_revision;
    product.road_width_meters = road_width_meters;
    product.shoulder_width_meters = shoulder_width_meters;
    product.maximum_grade_ratio = maximum_grade_ratio;
    product.minimum_curve_radius_meters = minimum_curve_radius_meters;
    product.samples_per_segment = samples_per_segment;
    product.stations.reserve(centers.size());
    const double road_half_width = road_width_meters * 0.5;
    const double shoulder_extent = road_half_width + shoulder_width_meters;
    constexpr double kMaximumMiterScale = 4.0;
    for (std::size_t index = 0U; index < centers.size(); ++index) {
        core::Vec3d incoming{};
        core::Vec3d outgoing{};
        if (index > 0U) {
            incoming = horizontal(centers[index] - centers[index - 1U]).normalized();
        }
        if (index + 1U < centers.size()) {
            outgoing = horizontal(centers[index + 1U] - centers[index]).normalized();
        }
        if (index == 0U) incoming = outgoing;
        if (index + 1U == centers.size()) outgoing = incoming;
        if (!incoming.finite() || !outgoing.finite()) {
            return core::Result<RoadSurfaceMesh>::failure(validation(
                "road surface station has no stable horizontal tangent"));
        }
        const core::Vec3d incoming_left{-incoming.z, 0.0, incoming.x};
        const core::Vec3d outgoing_left{-outgoing.z, 0.0, outgoing.x};
        core::Vec3d miter = (incoming_left + outgoing_left).normalized();
        if (!miter.finite()) {
            return core::Result<RoadSurfaceMesh>::failure(validation(
                "road centerline contains a reversing tangent that cannot form a safe join"));
        }
        double miter_scale = 1.0;
        if (index > 0U && index + 1U < centers.size()) {
            const double denominator = core::dot(miter, outgoing_left);
            if (!std::isfinite(denominator) || denominator <= 1.0e-6) {
                return core::Result<RoadSurfaceMesh>::failure(validation(
                    "road centerline join is degenerate"));
            }
            miter_scale = 1.0 / denominator;
            if (!std::isfinite(miter_scale) || miter_scale > kMaximumMiterScale) {
                return core::Result<RoadSurfaceMesh>::failure(validation(
                    "road centerline turn exceeds the bounded miter join"));
            }
        }
        const core::Vec3d shoulder_offset = miter * (shoulder_extent * miter_scale);
        const core::Vec3d road_offset = miter * (road_half_width * miter_scale);
        product.stations.push_back(RoadSurfaceStation{
            centers[index], centers[index] + shoulder_offset,
            centers[index] + road_offset, centers[index] - road_offset,
            centers[index] - shoulder_offset});
    }
    if (auto result = product.validate(); !result) {
        return core::Result<RoadSurfaceMesh>::failure(
            result.error().with_context("generated road surface"));
    }
    return core::Result<RoadSurfaceMesh>::success(std::move(product));
}

} // namespace carto::geometry
