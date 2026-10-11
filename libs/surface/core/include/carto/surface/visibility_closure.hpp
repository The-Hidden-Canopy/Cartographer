#pragma once

#include <carto/surface/distribution.hpp>
#include <carto/surface/qem.hpp>

#include <cstdint>
#include <string>

namespace carto::surface {

struct SurfaceVisibilityClosureOptions {
    std::uint32_t visibility_bins = 16U;
    std::uint32_t direction_count = 8U;
    std::uint32_t max_source_triangles = 2048U;
    double ray_epsilon = 1e-6;
    double max_distance = 100.0;
    SurfaceDistributionMetricsOptions distribution_metrics{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceVisibilityClosureResult {
    SurfaceDomainId domain;
    core::Revision source_revision;
    std::string source_domain_digest;
    std::string candidate_digest;
    SurfaceDistribution source_visibility_distribution;
    SurfaceDistribution candidate_visibility_distribution;
    SurfaceDistributionMetrics distribution_metrics;
    double source_average_visibility = 0.0;
    double candidate_average_visibility = 0.0;
    double average_visibility_error = 0.0;
    std::string options_digest;
    std::string algorithm_version = "cartographer-surface-visibility-closure/0.1";
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source,
        const SurfaceDomain& source_domain,
        const SurfaceQemResult& candidate,
        const SurfaceVisibilityClosureOptions& options) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Compute a bounded hemisphere ray-visibility histogram for source and QEM
// candidate triangles. This is an explicit geometric occlusion reference, not
// a claim of full ambient occlusion, PBR, or renderer parity.
[[nodiscard]] core::Result<SurfaceVisibilityClosureResult> evaluate_surface_visibility_closure(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceQemResult& candidate,
    const SurfaceVisibilityClosureOptions& options = {});

} // namespace carto::surface
