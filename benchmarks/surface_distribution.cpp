#include <carto/surface/distribution.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
    constexpr std::size_t bins = 256U;
    std::vector<double> left;
    std::vector<double> right;
    left.reserve(bins);
    right.reserve(bins);
    for (std::size_t index = 0U; index < bins; ++index) {
        left.push_back(0.2 + static_cast<double>((index * 17U) % 31U));
        right.push_back(0.2 + static_cast<double>((index * 29U + 7U) % 37U));
    }
    const auto left_distribution = carto::surface::make_surface_distribution(left);
    const auto right_distribution = carto::surface::make_surface_distribution(right);
    if (!left_distribution || !right_distribution) return 1;

    carto::surface::SurfaceDistributionMetricsOptions options;
    options.max_bins = 256U;
    options.sinkhorn_iterations = 64U;
    const auto start = std::chrono::steady_clock::now();
    const auto metrics = carto::surface::compare_surface_distributions(
        left_distribution.value(), right_distribution.value(), options);
    const auto stop = std::chrono::steady_clock::now();
    if (!metrics) return 1;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(stop - start).count();
    std::cout << std::fixed << std::setprecision(4)
              << "stage,bins,elapsed_ms,js_divergence,fisher_rao,wasserstein,sinkhorn,digest\n"
              << "surface_distribution," << bins << ',' << elapsed_ms << ','
              << metrics.value().jensen_shannon_divergence << ','
              << metrics.value().fisher_rao_distance << ','
              << metrics.value().wasserstein_1d << ','
              << metrics.value().sinkhorn_transport << ','
              << metrics.value().digest << '\n';
    return 0;
}
