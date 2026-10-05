#include <carto/surface/distribution.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::surface {

namespace {

constexpr std::size_t kMaximumDistributionBins = 4096U;
constexpr double kProbabilityTolerance = 1e-6;
constexpr double kPi = 3.141592653589793238462643383279502884;

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

double xlogx(double value) {
    return value > 0.0 ? value * std::log(value) : 0.0;
}

double normalized_bin_distance(std::size_t first, std::size_t second, std::size_t count) {
    if (count <= 1U) return 0.0;
    return static_cast<double>(first > second ? first - second : second - first) /
        static_cast<double>(count - 1U);
}

core::Result<void> validate_distribution_content(const SurfaceDistribution& distribution) {
    if (distribution.probabilities.empty() ||
        distribution.probabilities.size() > kMaximumDistributionBins) {
        return core::Result<void>::failure(validation(
            "surface distribution contains an empty or oversized vector"));
    }
    double total = 0.0;
    for (const double probability : distribution.probabilities) {
        if (!finite_nonnegative(probability)) {
            return core::Result<void>::failure(validation(
                "surface distribution contains an invalid probability"));
        }
        total += probability;
    }
    if (!finite_positive(total) || std::abs(total - 1.0) > kProbabilityTolerance) {
        return core::Result<void>::failure(validation(
            "surface distribution probabilities are not normalized"));
    }
    return core::Result<void>::success();
}

double sinkhorn_cost(
    const std::vector<double>& first,
    const std::vector<double>& second,
    const SurfaceDistributionMetricsOptions& options) {
    const std::size_t count = first.size();
    const double regularization = options.sinkhorn_regularization;
    std::vector<double> kernel(count * count, 0.0);
    for (std::size_t row = 0U; row < count; ++row) {
        for (std::size_t column = 0U; column < count; ++column) {
            kernel[row * count + column] = std::exp(
                -normalized_bin_distance(row, column, count) / regularization);
        }
    }

    std::vector<double> u(count, 1.0);
    std::vector<double> v(count, 1.0);
    for (std::uint32_t iteration = 0U; iteration < options.sinkhorn_iterations; ++iteration) {
        for (std::size_t row = 0U; row < count; ++row) {
            double denominator = 0.0;
            for (std::size_t column = 0U; column < count; ++column) {
                denominator += kernel[row * count + column] * v[column];
            }
            u[row] = denominator > std::numeric_limits<double>::min()
                ? first[row] / denominator : 0.0;
        }
        for (std::size_t column = 0U; column < count; ++column) {
            double denominator = 0.0;
            for (std::size_t row = 0U; row < count; ++row) {
                denominator += kernel[row * count + column] * u[row];
            }
            v[column] = denominator > std::numeric_limits<double>::min()
                ? second[column] / denominator : 0.0;
        }
    }

    double cost = 0.0;
    for (std::size_t row = 0U; row < count; ++row) {
        for (std::size_t column = 0U; column < count; ++column) {
            const double transport = u[row] * kernel[row * count + column] * v[column];
            cost += transport * normalized_bin_distance(row, column, count);
        }
    }
    return cost;
}

} // namespace

core::Result<void> SurfaceDistributionOptions::validate() const {
    if (max_bins == 0U || max_bins > kMaximumDistributionBins) {
        return core::Result<void>::failure(invalid(
            "surface distribution bin cap is outside bounded limits"));
    }
    return core::Result<void>::success();
}

std::string SurfaceDistributionOptions::canonical() const {
    return "surface-distribution-options-v1;max_bins=" + std::to_string(max_bins);
}

assets::Sha256Digest SurfaceDistributionOptions::digest() const {
    return digest_text(canonical());
}

std::string SurfaceDistribution::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "surface-distribution-v1;probabilities=";
    for (const double probability : probabilities) output << probability << ',';
    return output.str();
}

core::Result<void> SurfaceDistribution::refresh_digest() {
    if (auto valid = validate_distribution_content(*this); !valid) return valid;
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceDistribution::validate() const {
    if (auto valid = validate_distribution_content(*this); !valid) return valid;
    if (!valid_hex_digest(digest) || digest != digest_hex(canonical())) {
        return core::Result<void>::failure(validation(
            "surface distribution digest does not match canonical content"));
    }
    return core::Result<void>::success();
}

core::Result<SurfaceDistribution> make_surface_distribution(
    std::span<const double> nonnegative_weights,
    const SurfaceDistributionOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<SurfaceDistribution>::failure(valid.error());
    }
    if (nonnegative_weights.empty() || nonnegative_weights.size() > options.max_bins) {
        return core::Result<SurfaceDistribution>::failure(invalid(
            "surface distribution input is empty or exceeds its explicit bin cap"));
    }
    double total = 0.0;
    for (const double weight : nonnegative_weights) {
        if (!finite_nonnegative(weight)) {
            return core::Result<SurfaceDistribution>::failure(invalid(
                "surface distribution input must contain finite non-negative weights"));
        }
        total += weight;
    }
    if (!finite_positive(total)) {
        return core::Result<SurfaceDistribution>::failure(invalid(
            "surface distribution input must have positive total weight"));
    }

    SurfaceDistribution result;
    result.probabilities.reserve(nonnegative_weights.size());
    for (const double weight : nonnegative_weights) {
        result.probabilities.push_back(weight / total);
    }
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceDistribution>::failure(refreshed.error());
    }
    return core::Result<SurfaceDistribution>::success(std::move(result));
}

core::Result<void> SurfaceDistributionMetricsOptions::validate() const {
    if (max_bins == 0U || max_bins > 512U || sinkhorn_iterations == 0U ||
        sinkhorn_iterations > 1024U || !finite_positive(sinkhorn_regularization) ||
        sinkhorn_regularization < 1e-3 || sinkhorn_regularization > 1.0) {
        return core::Result<void>::failure(invalid(
            "surface distribution metric options are outside bounded limits"));
    }
    return core::Result<void>::success();
}

std::string SurfaceDistributionMetricsOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-distribution-metrics-options-v1;max_bins=" << max_bins
           << ";iterations=" << sinkhorn_iterations
           << ";regularization=" << sinkhorn_regularization;
    return output.str();
}

assets::Sha256Digest SurfaceDistributionMetricsOptions::digest() const {
    return digest_text(canonical());
}

std::string SurfaceDistributionMetrics::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-distribution-metrics-v1;a=" << distribution_a_digest
           << ";b=" << distribution_b_digest
           << ";js=" << jensen_shannon_divergence
           << ";fisher_rao=" << fisher_rao_distance
           << ";wasserstein=" << wasserstein_1d
           << ";emd=" << earth_movers_distance
           << ";sinkhorn=" << sinkhorn_transport
           << ";options=" << options_digest;
    return output.str();
}

core::Result<void> SurfaceDistributionMetrics::refresh_digest() {
    if (!valid_hex_digest(distribution_a_digest) || !valid_hex_digest(distribution_b_digest) ||
        !finite_nonnegative(jensen_shannon_divergence) ||
        !finite_nonnegative(fisher_rao_distance) || !finite_nonnegative(wasserstein_1d) ||
        !finite_nonnegative(earth_movers_distance) || !finite_nonnegative(sinkhorn_transport) ||
        !valid_hex_digest(options_digest)) {
        return core::Result<void>::failure(validation(
            "surface distribution metric receipt is incomplete"));
    }
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceDistributionMetrics::validate(
    const SurfaceDistribution& distribution_a,
    const SurfaceDistribution& distribution_b,
    const SurfaceDistributionMetricsOptions& options) const {
    if (auto valid = distribution_a.validate(); !valid) return valid;
    if (auto valid = distribution_b.validate(); !valid) return valid;
    if (auto valid = options.validate(); !valid) return valid;
    if (distribution_a.probabilities.size() != distribution_b.probabilities.size() ||
        distribution_a.probabilities.empty() ||
        distribution_a.probabilities.size() > options.max_bins ||
        distribution_a_digest != distribution_a.digest ||
        distribution_b_digest != distribution_b.digest ||
        options_digest != options.digest().hex() ||
        !valid_hex_digest(digest) || digest != digest_hex(canonical()) ||
        jensen_shannon_divergence < -kProbabilityTolerance ||
        jensen_shannon_divergence > std::log(2.0) + kProbabilityTolerance ||
        fisher_rao_distance < -kProbabilityTolerance ||
        fisher_rao_distance > kPi + kProbabilityTolerance ||
        wasserstein_1d < -kProbabilityTolerance || wasserstein_1d > 1.0 + kProbabilityTolerance ||
        earth_movers_distance < -kProbabilityTolerance || earth_movers_distance > 1.0 + kProbabilityTolerance ||
        sinkhorn_transport < -kProbabilityTolerance || sinkhorn_transport > 1.0 + kProbabilityTolerance) {
        return core::Result<void>::failure(validation(
            "surface distribution metric receipt is invalid or mismatched"));
    }
    return core::Result<void>::success();
}

core::Result<SurfaceDistributionMetrics> compare_surface_distributions(
    const SurfaceDistribution& distribution_a,
    const SurfaceDistribution& distribution_b,
    const SurfaceDistributionMetricsOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<SurfaceDistributionMetrics>::failure(valid.error());
    }
    if (auto valid = distribution_a.validate(); !valid) {
        return core::Result<SurfaceDistributionMetrics>::failure(valid.error());
    }
    if (auto valid = distribution_b.validate(); !valid) {
        return core::Result<SurfaceDistributionMetrics>::failure(valid.error());
    }
    if (distribution_a.probabilities.size() != distribution_b.probabilities.size() ||
        distribution_a.probabilities.empty() ||
        distribution_a.probabilities.size() > options.max_bins) {
        return core::Result<SurfaceDistributionMetrics>::failure(invalid(
            "surface distribution comparison requires equal bounded bin counts"));
    }

    const std::size_t count = distribution_a.probabilities.size();
    double js = 0.0;
    double fisher_dot = 0.0;
    double cumulative_a = 0.0;
    double cumulative_b = 0.0;
    double wasserstein = 0.0;
    for (std::size_t index = 0U; index < count; ++index) {
        const double probability_a = distribution_a.probabilities[index];
        const double probability_b = distribution_b.probabilities[index];
        const double midpoint = 0.5 * (probability_a + probability_b);
        js += 0.5 * (xlogx(probability_a) - xlogx(midpoint)) +
            0.5 * (xlogx(probability_b) - xlogx(midpoint));
        fisher_dot += std::sqrt(probability_a * probability_b);
        cumulative_a += probability_a;
        cumulative_b += probability_b;
        wasserstein += std::abs(cumulative_a - cumulative_b);
    }
    if (count > 1U) wasserstein /= static_cast<double>(count - 1U);
    fisher_dot = std::clamp(fisher_dot, 0.0, 1.0);

    SurfaceDistributionMetrics result;
    result.distribution_a_digest = distribution_a.digest;
    result.distribution_b_digest = distribution_b.digest;
    result.jensen_shannon_divergence = std::max(0.0, js);
    result.fisher_rao_distance = 2.0 * std::acos(fisher_dot);
    result.wasserstein_1d = wasserstein;
    result.earth_movers_distance = wasserstein;
    result.sinkhorn_transport = sinkhorn_cost(
        distribution_a.probabilities, distribution_b.probabilities, options);
    result.options_digest = options.digest().hex();
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceDistributionMetrics>::failure(refreshed.error());
    }
    if (auto valid = result.validate(distribution_a, distribution_b, options); !valid) {
        return core::Result<SurfaceDistributionMetrics>::failure(valid.error());
    }
    return core::Result<SurfaceDistributionMetrics>::success(std::move(result));
}

} // namespace carto::surface
