#pragma once

#include <carto/surface/distribution.hpp>
#include <carto/surface/qem.hpp>

#include <cstdint>
#include <string>

namespace carto::surface {

struct SurfaceNormalClosureOptions {
    std::uint32_t normal_bins = 16U;
    std::uint32_t max_source_triangles = 4096U;
    SurfaceDistributionMetricsOptions distribution_metrics{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceNormalClosureResult {
    SurfaceDomainId domain;
    core::Revision source_revision;
    std::string source_domain_digest;
    std::string candidate_digest;
    SurfaceDistribution source_normal_distribution;
    SurfaceDistribution candidate_normal_distribution;
    SurfaceDistributionMetrics distribution_metrics;
    double source_area = 0.0;
    double candidate_area = 0.0;
    double relative_area_error = 0.0;
    std::string options_digest;
    std::string algorithm_version = "cartographer-surface-normal-closure/0.1";
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& source,
        const SurfaceDomain& source_domain,
        const SurfaceQemResult& candidate,
        const SurfaceNormalClosureOptions& options) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Compare source and candidate triangle-normal distributions using the source
// surface's area-weighted reference normal. This is a closure signal for a
// candidate compiler, not a full anisotropic NDF or PBR rendering evaluation.
[[nodiscard]] core::Result<SurfaceNormalClosureResult> evaluate_surface_normal_closure(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceQemResult& candidate,
    const SurfaceNormalClosureOptions& options = {});

} // namespace carto::surface
