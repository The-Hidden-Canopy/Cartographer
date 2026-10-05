#pragma once

#include <carto/surface/core.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::surface {

struct SurfaceDistributionOptions {
    std::uint32_t max_bins = 4096U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceDistribution {
    std::vector<double> probabilities;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

[[nodiscard]] core::Result<SurfaceDistribution> make_surface_distribution(
    std::span<const double> nonnegative_weights,
    const SurfaceDistributionOptions& options = {});

struct SurfaceDistributionMetricsOptions {
    std::uint32_t max_bins = 512U;
    std::uint32_t sinkhorn_iterations = 64U;
    double sinkhorn_regularization = 0.05;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceDistributionMetrics {
    std::string distribution_a_digest;
    std::string distribution_b_digest;
    double jensen_shannon_divergence = 0.0;
    double fisher_rao_distance = 0.0;
    double wasserstein_1d = 0.0;
    double earth_movers_distance = 0.0;
    double sinkhorn_transport = 0.0;
    std::string options_digest;
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const SurfaceDistribution& distribution_a,
        const SurfaceDistribution& distribution_b,
        const SurfaceDistributionMetricsOptions& options) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// The first public metric surface intentionally requires equal, ordered bins.
// It is therefore auditable for spectral bands and does not silently invent a
// correspondence between unrelated supports.
[[nodiscard]] core::Result<SurfaceDistributionMetrics> compare_surface_distributions(
    const SurfaceDistribution& distribution_a,
    const SurfaceDistribution& distribution_b,
    const SurfaceDistributionMetricsOptions& options = {});

} // namespace carto::surface
