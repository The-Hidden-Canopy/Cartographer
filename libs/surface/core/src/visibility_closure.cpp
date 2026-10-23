#include <carto/surface/visibility_closure.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::surface {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool finite_unit(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

bool valid_hex_digest(std::string_view value) {
    return value.size() == 64U && assets::Sha256Digest::from_hex(value).has_value();
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::string digest_hex(std::string_view value) {
    return digest_text(value).hex();
}

struct TriangleGeometry {
    core::Vec3d first;
    core::Vec3d second;
    core::Vec3d third;
    core::Vec3d normal;
    core::Vec3d centroid;
    double area = 0.0;
};

core::Result<std::vector<TriangleGeometry>> source_triangles(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain) {
    const std::set<geometry::FaceId> faces(domain.source_faces.begin(), domain.source_faces.end());
    std::vector<TriangleGeometry> result;
    for (std::size_t triangle = 0U; triangle < source.triangle_faces.size(); ++triangle) {
        if (!faces.contains(source.triangle_faces[triangle])) continue;
        const std::size_t offset = triangle * 3U;
        const auto a = source.positions[source.indices[offset]];
        const auto b = source.positions[source.indices[offset + 1U]];
        const auto c = source.positions[source.indices[offset + 2U]];
        const core::Vec3d raw = core::cross(b - a, c - a);
        const double area = raw.length() * 0.5;
        if (!finite_positive(area)) continue;
        result.push_back({a, b, c, raw.normalized(), (a + b + c) * (1.0 / 3.0), area});
    }
    if (result.empty()) {
        return core::Result<std::vector<TriangleGeometry>>::failure(validation(
            "surface visibility closure source contains no non-degenerate triangles"));
    }
    return core::Result<std::vector<TriangleGeometry>>::success(std::move(result));
}

core::Result<std::vector<TriangleGeometry>> candidate_triangles(
    const SurfaceQemResult& candidate) {
    std::vector<TriangleGeometry> result;
    result.reserve(candidate.triangle_count());
    for (std::size_t triangle = 0U; triangle < candidate.triangle_count(); ++triangle) {
        const std::size_t offset = triangle * 3U;
        const auto a = candidate.positions[candidate.indices[offset]];
        const auto b = candidate.positions[candidate.indices[offset + 1U]];
        const auto c = candidate.positions[candidate.indices[offset + 2U]];
        const core::Vec3d raw = core::cross(b - a, c - a);
        const double area = raw.length() * 0.5;
        if (!finite_positive(area)) {
            return core::Result<std::vector<TriangleGeometry>>::failure(validation(
                "surface visibility closure candidate contains a degenerate triangle"));
        }
        result.push_back({a, b, c, raw.normalized(), (a + b + c) * (1.0 / 3.0), area});
    }
    return core::Result<std::vector<TriangleGeometry>>::success(std::move(result));
}

bool occluded(
    core::Vec3d origin,
    core::Vec3d direction,
    const std::vector<TriangleGeometry>& triangles,
    std::size_t ignored,
    double epsilon,
    double max_distance) {
    for (std::size_t index = 0U; index < triangles.size(); ++index) {
        if (index == ignored) continue;
        const TriangleGeometry& triangle = triangles[index];
        const core::Vec3d edge_one = triangle.second - triangle.first;
        const core::Vec3d edge_two = triangle.third - triangle.first;
        const core::Vec3d cross_direction = core::cross(direction, edge_two);
        const double determinant = core::dot(edge_one, cross_direction);
        if (std::abs(determinant) <= epsilon) continue;
        const double inverse = 1.0 / determinant;
        const core::Vec3d origin_delta = origin - triangle.first;
        const double u = inverse * core::dot(origin_delta, cross_direction);
        if (u < 0.0 || u > 1.0) continue;
        const core::Vec3d cross_origin = core::cross(origin_delta, edge_one);
        const double v = inverse * core::dot(direction, cross_origin);
        if (v < 0.0 || u + v > 1.0) continue;
        const double distance = inverse * core::dot(edge_two, cross_origin);
        if (distance > epsilon && distance < max_distance) return true;
    }
    return false;
}

core::Vec3d tangent_for(core::Vec3d normal) {
    const core::Vec3d reference = std::abs(normal.z) < 0.9
        ? core::Vec3d{0.0, 0.0, 1.0}
        : core::Vec3d{0.0, 1.0, 0.0};
    return core::cross(reference, normal).normalized();
}

core::Result<std::pair<SurfaceDistribution, double>> visibility_distribution(
    const std::vector<TriangleGeometry>& triangles,
    const SurfaceVisibilityClosureOptions& options) {
    std::vector<double> weights(options.visibility_bins, 0.0);
    double total_area = 0.0;
    double weighted_visibility = 0.0;
    for (std::size_t triangle = 0U; triangle < triangles.size(); ++triangle) {
        const TriangleGeometry& geometry = triangles[triangle];
        const core::Vec3d tangent = tangent_for(geometry.normal);
        const core::Vec3d bitangent = core::cross(geometry.normal, tangent).normalized();
        std::uint32_t visible = 0U;
        for (std::uint32_t direction_index = 0U;
             direction_index < options.direction_count; ++direction_index) {
            const double azimuth = 2.0 * kPi * static_cast<double>(direction_index) /
                static_cast<double>(options.direction_count);
            const double cosine = 0.25 + 0.5 * static_cast<double>(direction_index % 2U);
            const double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));
            const core::Vec3d direction = geometry.normal * cosine +
                tangent * (std::cos(azimuth) * sine) +
                bitangent * (std::sin(azimuth) * sine);
            const core::Vec3d origin = geometry.centroid + geometry.normal * options.ray_epsilon;
            if (!occluded(origin, direction, triangles, triangle, options.ray_epsilon,
                          options.max_distance)) {
                ++visible;
            }
        }
        const double ratio = static_cast<double>(visible) /
            static_cast<double>(options.direction_count);
        const auto bin = std::min(
            options.visibility_bins - 1U,
            static_cast<std::uint32_t>(ratio * static_cast<double>(options.visibility_bins)));
        weights[bin] += geometry.area;
        total_area += geometry.area;
        weighted_visibility += geometry.area * ratio;
    }
    if (!finite_positive(total_area)) {
        return core::Result<std::pair<SurfaceDistribution, double>>::failure(validation(
            "surface visibility closure has no positive area"));
    }
    const auto distribution = make_surface_distribution(weights);
    if (!distribution) {
        return core::Result<std::pair<SurfaceDistribution, double>>::failure(distribution.error());
    }
    return core::Result<std::pair<SurfaceDistribution, double>>::success(
        {distribution.value(), weighted_visibility / total_area});
}

} // namespace

core::Result<void> SurfaceVisibilityClosureOptions::validate() const {
    if (visibility_bins == 0U || visibility_bins > 128U || direction_count == 0U ||
        direction_count > 32U || max_source_triangles == 0U || max_source_triangles > 2048U ||
        !finite_positive(ray_epsilon) || ray_epsilon > 1.0 ||
        !finite_positive(max_distance) || max_distance > 1e6) {
        return core::Result<void>::failure(invalid(
            "surface visibility closure options are outside bounded limits"));
    }
    if (auto valid = distribution_metrics.validate(); !valid) return valid;
    if (visibility_bins > distribution_metrics.max_bins) {
        return core::Result<void>::failure(invalid(
            "surface visibility bins exceed the distribution metric cap"));
    }
    return core::Result<void>::success();
}

std::string SurfaceVisibilityClosureOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-visibility-closure-options-v1;bins=" << visibility_bins
           << ";directions=" << direction_count
           << ";max_source=" << max_source_triangles
           << ";epsilon=" << ray_epsilon
           << ";max_distance=" << max_distance
           << ";metrics=" << distribution_metrics.canonical();
    return output.str();
}

assets::Sha256Digest SurfaceVisibilityClosureOptions::digest() const {
    return digest_text(canonical());
}

std::string SurfaceVisibilityClosureResult::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-visibility-closure-v1;domain=" << domain.value
           << ";revision=" << source_revision.value()
           << ";source_domain=" << source_domain_digest
           << ";candidate=" << candidate_digest
           << ";source_distribution=" << source_visibility_distribution.digest
           << ";candidate_distribution=" << candidate_visibility_distribution.digest
           << ";metrics=" << distribution_metrics.digest
           << ";source_average=" << source_average_visibility
           << ";candidate_average=" << candidate_average_visibility
           << ";average_error=" << average_visibility_error
           << ";options=" << options_digest
           << ";algorithm=" << algorithm_version;
    return output.str();
}

core::Result<void> SurfaceVisibilityClosureResult::refresh_digest() {
    if (!domain || source_revision.exhausted() || !valid_hex_digest(source_domain_digest) ||
        !valid_hex_digest(candidate_digest) || !valid_hex_digest(options_digest) ||
        !finite_unit(source_average_visibility) || !finite_unit(candidate_average_visibility) ||
        !finite_unit(average_visibility_error) || algorithm_version.empty()) {
        return core::Result<void>::failure(validation(
            "surface visibility closure cannot refresh incomplete content"));
    }
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceVisibilityClosureResult::validate(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& source_domain,
    const SurfaceQemResult& candidate,
    const SurfaceVisibilityClosureOptions& options) const {
    if (auto valid = source_domain.validate(source); !valid) return valid;
    if (auto valid = candidate.validate(source, source_domain); !valid) return valid;
    if (auto valid = options.validate(); !valid) return valid;
    if (domain != source_domain.id || source_revision != source_domain.source_revision ||
        source_domain_digest != source_domain.digest || candidate_digest != candidate.digest ||
        options_digest != options.digest().hex() || !valid_hex_digest(digest) ||
        digest != digest_hex(canonical()) || !finite_unit(source_average_visibility) ||
        !finite_unit(candidate_average_visibility) || !finite_unit(average_visibility_error) ||
        algorithm_version.empty()) {
        return core::Result<void>::failure(validation(
            "surface visibility closure identity or aggregate is invalid"));
    }
    if (auto valid = source_visibility_distribution.validate(); !valid) return valid;
    if (auto valid = candidate_visibility_distribution.validate(); !valid) return valid;
    if (auto valid = distribution_metrics.validate(
            source_visibility_distribution, candidate_visibility_distribution,
            options.distribution_metrics); !valid) return valid;
    if (source_visibility_distribution.probabilities.size() != options.visibility_bins ||
        candidate_visibility_distribution.probabilities.size() != options.visibility_bins) {
        return core::Result<void>::failure(validation(
            "surface visibility closure distribution cardinality is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<SurfaceVisibilityClosureResult> evaluate_surface_visibility_closure(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceQemResult& candidate,
    const SurfaceVisibilityClosureOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<SurfaceVisibilityClosureResult>::failure(valid.error());
    }
    if (auto valid = domain.validate(source); !valid) {
        return core::Result<SurfaceVisibilityClosureResult>::failure(valid.error());
    }
    if (auto valid = candidate.validate(source, domain); !valid) {
        return core::Result<SurfaceVisibilityClosureResult>::failure(valid.error());
    }
    const auto source_geometry = source_triangles(source, domain);
    if (!source_geometry) return core::Result<SurfaceVisibilityClosureResult>::failure(source_geometry.error());
    if (source_geometry.value().size() > options.max_source_triangles) {
        return core::Result<SurfaceVisibilityClosureResult>::failure(invalid(
            "surface visibility closure source exceeds the bounded triangle cap"));
    }
    const auto candidate_geometry = candidate_triangles(candidate);
    if (!candidate_geometry) return core::Result<SurfaceVisibilityClosureResult>::failure(candidate_geometry.error());
    const auto source_distribution = visibility_distribution(source_geometry.value(), options);
    const auto candidate_distribution = visibility_distribution(candidate_geometry.value(), options);
    if (!source_distribution || !candidate_distribution) {
        return core::Result<SurfaceVisibilityClosureResult>::failure(
            !source_distribution ? source_distribution.error() : candidate_distribution.error());
    }
    const auto metrics = compare_surface_distributions(
        source_distribution.value().first, candidate_distribution.value().first,
        options.distribution_metrics);
    if (!metrics) return core::Result<SurfaceVisibilityClosureResult>::failure(metrics.error());

    SurfaceVisibilityClosureResult result;
    result.domain = domain.id;
    result.source_revision = domain.source_revision;
    result.source_domain_digest = domain.digest;
    result.candidate_digest = candidate.digest;
    result.source_visibility_distribution = source_distribution.value().first;
    result.candidate_visibility_distribution = candidate_distribution.value().first;
    result.distribution_metrics = metrics.value();
    result.source_average_visibility = source_distribution.value().second;
    result.candidate_average_visibility = candidate_distribution.value().second;
    result.average_visibility_error = std::abs(
        result.candidate_average_visibility - result.source_average_visibility);
    result.options_digest = options.digest().hex();
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceVisibilityClosureResult>::failure(refreshed.error());
    }
    if (auto valid = result.validate(source, domain, candidate, options); !valid) {
        return core::Result<SurfaceVisibilityClosureResult>::failure(valid.error());
    }
    return core::Result<SurfaceVisibilityClosureResult>::success(std::move(result));
}

} // namespace carto::surface
