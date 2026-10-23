#include <carto/surface/normal_closure.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::surface {

namespace {

constexpr double kEpsilon = 1e-12;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
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

struct NormalSample {
    core::Vec3d normal;
    double area = 0.0;
};

std::uint32_t normal_bin(core::Vec3d normal, core::Vec3d reference, std::uint32_t bins) {
    const double coordinate = std::clamp(0.5 * (core::dot(normal, reference) + 1.0), 0.0, 1.0);
    const auto index = static_cast<std::uint32_t>(coordinate * static_cast<double>(bins));
    return std::min(bins - 1U, index);
}

core::Result<std::pair<std::vector<NormalSample>, core::Vec3d>> source_samples(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain) {
    const std::set<geometry::FaceId> faces(domain.source_faces.begin(), domain.source_faces.end());
    std::vector<NormalSample> samples;
    core::Vec3d reference_sum{};
    for (std::size_t triangle = 0U; triangle < source.triangle_faces.size(); ++triangle) {
        if (!faces.contains(source.triangle_faces[triangle])) continue;
        const std::size_t offset = triangle * 3U;
        const auto& indices = source.indices;
        const core::Vec3d raw = core::cross(
            source.positions[indices[offset + 1U]] - source.positions[indices[offset]],
            source.positions[indices[offset + 2U]] - source.positions[indices[offset]]);
        const double area = raw.length() * 0.5;
        if (!finite_positive(area)) continue;
        const core::Vec3d normal = raw.normalized();
        samples.push_back({normal, area});
        reference_sum = reference_sum + raw;
    }
    if (samples.empty() || reference_sum.length_squared() <= kEpsilon) {
        return core::Result<std::pair<std::vector<NormalSample>, core::Vec3d>>::failure(
            validation("surface normal closure source has no stable normal samples"));
    }
    return core::Result<std::pair<std::vector<NormalSample>, core::Vec3d>>::success(
        {std::move(samples), reference_sum.normalized()});
}

core::Result<std::vector<NormalSample>> candidate_samples(
    const SurfaceQemResult& candidate) {
    std::vector<NormalSample> samples;
    samples.reserve(candidate.triangle_count());
    for (std::size_t triangle = 0U; triangle < candidate.triangle_count(); ++triangle) {
        const std::size_t offset = triangle * 3U;
        const std::array<std::uint32_t, 3> indices{
            candidate.indices[offset], candidate.indices[offset + 1U], candidate.indices[offset + 2U]};
        const core::Vec3d raw = core::cross(
            candidate.positions[indices[1U]] - candidate.positions[indices[0U]],
            candidate.positions[indices[2U]] - candidate.positions[indices[0U]]);
        const double area = raw.length() * 0.5;
        if (!finite_positive(area)) {
            return core::Result<std::vector<NormalSample>>::failure(validation(
                "surface normal closure candidate contains a degenerate triangle"));
        }
        samples.push_back({raw.normalized(), area});
    }
    return core::Result<std::vector<NormalSample>>::success(std::move(samples));
}

core::Result<SurfaceDistribution> distribution_from_samples(
    const std::vector<NormalSample>& samples,
    core::Vec3d reference,
    std::uint32_t bins) {
    std::vector<double> weights(bins, 0.0);
    for (const NormalSample& sample : samples) {
        weights[normal_bin(sample.normal, reference, bins)] += sample.area;
    }
    return make_surface_distribution(weights);
}

} // namespace

core::Result<void> SurfaceNormalClosureOptions::validate() const {
    if (normal_bins == 0U || normal_bins > 128U || max_source_triangles == 0U ||
        max_source_triangles > 65536U) {
        return core::Result<void>::failure(invalid(
            "surface normal closure options are outside bounded limits"));
    }
    if (auto valid = distribution_metrics.validate(); !valid) return valid;
    if (normal_bins > distribution_metrics.max_bins) {
        return core::Result<void>::failure(invalid(
            "surface normal bins exceed the distribution metric cap"));
    }
    return core::Result<void>::success();
}

std::string SurfaceNormalClosureOptions::canonical() const {
    std::ostringstream output;
    output << "surface-normal-closure-options-v1;bins=" << normal_bins
           << ";max_source=" << max_source_triangles
           << ";metrics=" << distribution_metrics.canonical();
    return output.str();
}

assets::Sha256Digest SurfaceNormalClosureOptions::digest() const {
    return digest_text(canonical());
}

std::string SurfaceNormalClosureResult::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-normal-closure-v1;domain=" << domain.value
           << ";revision=" << source_revision.value()
           << ";source_domain=" << source_domain_digest
           << ";candidate=" << candidate_digest
           << ";source_distribution=" << source_normal_distribution.digest
           << ";candidate_distribution=" << candidate_normal_distribution.digest
           << ";metrics=" << distribution_metrics.digest
           << ";source_area=" << source_area
           << ";candidate_area=" << candidate_area
           << ";relative_area_error=" << relative_area_error
           << ";options=" << options_digest
           << ";algorithm=" << algorithm_version;
    return output.str();
}

core::Result<void> SurfaceNormalClosureResult::refresh_digest() {
    if (!domain || source_revision.exhausted() || !valid_hex_digest(source_domain_digest) ||
        !valid_hex_digest(candidate_digest) || !valid_hex_digest(options_digest) ||
        !finite_positive(source_area) || !finite_positive(candidate_area) ||
        !finite_nonnegative(relative_area_error) || algorithm_version.empty()) {
        return core::Result<void>::failure(validation(
            "surface normal closure cannot refresh incomplete content"));
    }
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceNormalClosureResult::validate(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& source_domain,
    const SurfaceQemResult& candidate,
    const SurfaceNormalClosureOptions& options) const {
    if (auto valid = source_domain.validate(source); !valid) return valid;
    if (auto valid = candidate.validate(source, source_domain); !valid) return valid;
    if (auto valid = options.validate(); !valid) return valid;
    if (domain != source_domain.id || source_revision != source_domain.source_revision ||
        source_domain_digest != source_domain.digest || candidate_digest != candidate.digest ||
        options_digest != options.digest().hex() || !valid_hex_digest(digest) ||
        digest != digest_hex(canonical()) || !finite_positive(source_area) ||
        !finite_positive(candidate_area) || !finite_nonnegative(relative_area_error) ||
        algorithm_version.empty()) {
        return core::Result<void>::failure(validation(
            "surface normal closure identity or aggregate is invalid"));
    }
    if (auto valid = source_normal_distribution.validate(); !valid) return valid;
    if (auto valid = candidate_normal_distribution.validate(); !valid) return valid;
    if (auto valid = distribution_metrics.validate(
            source_normal_distribution, candidate_normal_distribution,
            options.distribution_metrics); !valid) return valid;
    if (source_normal_distribution.probabilities.size() != options.normal_bins ||
        candidate_normal_distribution.probabilities.size() != options.normal_bins) {
        return core::Result<void>::failure(validation(
            "surface normal closure distribution cardinality is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<SurfaceNormalClosureResult> evaluate_surface_normal_closure(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceQemResult& candidate,
    const SurfaceNormalClosureOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<SurfaceNormalClosureResult>::failure(valid.error());
    }
    if (auto valid = domain.validate(source); !valid) {
        return core::Result<SurfaceNormalClosureResult>::failure(valid.error());
    }
    if (auto valid = candidate.validate(source, domain); !valid) {
        return core::Result<SurfaceNormalClosureResult>::failure(valid.error());
    }
    const auto source_data = source_samples(source, domain);
    if (!source_data) return core::Result<SurfaceNormalClosureResult>::failure(source_data.error());
    if (source_data.value().first.size() > options.max_source_triangles) {
        return core::Result<SurfaceNormalClosureResult>::failure(invalid(
            "surface normal closure source exceeds the bounded triangle cap"));
    }
    const auto candidate_data = candidate_samples(candidate);
    if (!candidate_data) return core::Result<SurfaceNormalClosureResult>::failure(candidate_data.error());
    const auto source_distribution = distribution_from_samples(
        source_data.value().first, source_data.value().second, options.normal_bins);
    const auto candidate_distribution = distribution_from_samples(
        candidate_data.value(), source_data.value().second, options.normal_bins);
    if (!source_distribution || !candidate_distribution) {
        return core::Result<SurfaceNormalClosureResult>::failure(
            !source_distribution ? source_distribution.error() : candidate_distribution.error());
    }
    const auto metrics = compare_surface_distributions(
        source_distribution.value(), candidate_distribution.value(), options.distribution_metrics);
    if (!metrics) return core::Result<SurfaceNormalClosureResult>::failure(metrics.error());

    SurfaceNormalClosureResult result;
    result.domain = domain.id;
    result.source_revision = domain.source_revision;
    result.source_domain_digest = domain.digest;
    result.candidate_digest = candidate.digest;
    result.source_normal_distribution = source_distribution.value();
    result.candidate_normal_distribution = candidate_distribution.value();
    result.distribution_metrics = metrics.value();
    for (const NormalSample& sample : source_data.value().first) result.source_area += sample.area;
    for (const NormalSample& sample : candidate_data.value()) result.candidate_area += sample.area;
    result.relative_area_error = std::abs(result.candidate_area - result.source_area) /
        result.source_area;
    result.options_digest = options.digest().hex();
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceNormalClosureResult>::failure(refreshed.error());
    }
    if (auto valid = result.validate(source, domain, candidate, options); !valid) {
        return core::Result<SurfaceNormalClosureResult>::failure(valid.error());
    }
    return core::Result<SurfaceNormalClosureResult>::success(std::move(result));
}

} // namespace carto::surface
