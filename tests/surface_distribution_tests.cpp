#include <carto/surface/distribution.hpp>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            throw TestFailure(std::string("requirement failed: ") + #condition);            \
        }                                                                                   \
    } while (false)

void metrics_are_bounded_and_deterministic() {
    const std::vector<double> left_weights{1.0, 2.0, 0.0, 1.0};
    const std::vector<double> right_weights{0.0, 1.0, 2.0, 1.0};
    const auto left = carto::surface::make_surface_distribution(left_weights);
    const auto right = carto::surface::make_surface_distribution(right_weights);
    REQUIRE(left && right);
    REQUIRE(left.value().validate());
    REQUIRE(right.value().validate());

    carto::surface::SurfaceDistributionMetricsOptions options;
    options.sinkhorn_iterations = 96U;
    const auto first = carto::surface::compare_surface_distributions(
        left.value(), right.value(), options);
    const auto second = carto::surface::compare_surface_distributions(
        left.value(), right.value(), options);
    REQUIRE(first && second);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().validate(left.value(), right.value(), options));
    REQUIRE(first.value().jensen_shannon_divergence > 0.0);
    REQUIRE(first.value().fisher_rao_distance > 0.0);
    REQUIRE(first.value().wasserstein_1d > 0.0);
    REQUIRE(first.value().earth_movers_distance == first.value().wasserstein_1d);
    REQUIRE(std::isfinite(first.value().sinkhorn_transport));
}

void metrics_reject_invalid_supports_and_forgery() {
    const std::vector<double> values{1.0, 0.0, 0.0};
    const auto distribution = carto::surface::make_surface_distribution(values);
    REQUIRE(distribution);

    auto forged_distribution = distribution.value();
    forged_distribution.probabilities[0] = 0.5;
    REQUIRE(!forged_distribution.validate());

    const std::vector<double> negative{1.0, -1.0};
    REQUIRE(!carto::surface::make_surface_distribution(negative));
    const std::vector<double> zero{0.0, 0.0};
    REQUIRE(!carto::surface::make_surface_distribution(zero));
    const std::vector<double> non_finite{1.0, std::numeric_limits<double>::infinity()};
    REQUIRE(!carto::surface::make_surface_distribution(non_finite));

    carto::surface::SurfaceDistributionOptions invalid_distribution_options;
    invalid_distribution_options.max_bins = 0U;
    REQUIRE(!invalid_distribution_options.validate());

    carto::surface::SurfaceDistributionMetricsOptions invalid_metric_options;
    invalid_metric_options.sinkhorn_regularization = 1e-6;
    REQUIRE(!invalid_metric_options.validate());

    const std::vector<double> different_support{0.5, 0.5};
    const auto different = carto::surface::make_surface_distribution(different_support);
    REQUIRE(different);
    REQUIRE(!carto::surface::compare_surface_distributions(
        distribution.value(), different.value()));

    auto forged_metrics = carto::surface::compare_surface_distributions(
        distribution.value(), distribution.value());
    REQUIRE(forged_metrics);
    forged_metrics.value().digest[0] = forged_metrics.value().digest[0] == '0' ? '1' : '0';
    REQUIRE(!forged_metrics.value().validate(
        distribution.value(), distribution.value(), {}));
}

void metrics_reject_oversized_inputs() {
    std::vector<double> values(513U, 1.0);
    const auto distribution = carto::surface::make_surface_distribution(values);
    REQUIRE(distribution);
    REQUIRE(!carto::surface::compare_surface_distributions(
        distribution.value(), distribution.value()));

    carto::surface::SurfaceDistributionMetricsOptions options;
    options.max_bins = 512U;
    REQUIRE(options.validate());
    options.max_bins = 513U;
    REQUIRE(!options.validate());
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"distribution metrics are bounded and deterministic", metrics_are_bounded_and_deterministic},
        {"distribution metrics reject invalid supports and forgery", metrics_reject_invalid_supports_and_forgery},
        {"distribution metrics reject oversized inputs", metrics_reject_oversized_inputs},
    };
    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
