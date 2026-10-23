#pragma once

#include <carto/surface/core.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::surface {

// The first native spectrum implementation is intentionally a bounded CPU
// reference.  It provides deterministic evidence for later partial/sparse
// solvers without making a GPU or external linear-algebra dependency part of
// the public authoring contract.
struct SurfaceSpectrumOptions {
    std::uint32_t max_vertices = 512U;
    std::uint32_t max_modes = 16U;
    std::uint32_t max_jacobi_sweeps = 64U;
    double convergence_tolerance = 1e-10;
    double minimum_mass = 1e-12;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceSpectrumMode {
    double eigenvalue = 0.0;
    std::vector<double> coefficients;
    double signal_coefficient = 0.0;
    double signal_energy = 0.0;

    [[nodiscard]] core::Result<void> validate(std::size_t vertex_count) const;
};

struct SurfaceSpectrum {
    SurfaceDomainId domain;
    core::Revision source_revision;
    std::string source_content_digest;
    std::vector<geometry::VertexId> vertex_ids;
    std::vector<double> masses;
    std::vector<SurfaceSpectrumMode> modes;
    double total_signal_energy = 0.0;
    double smoothness_energy = 0.0;
    std::string algorithm_version = "cartographer-surface-spectrum/0.1";
    std::string digest;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CompiledMesh& mesh,
        const SurfaceDomain& source_domain) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Build a mass-normalized graph spectrum for a scalar signal sampled at the
// compiled mesh vertices.  The signal is not persisted; the receipt stores
// only its graph-Fourier coefficients and aggregate energies.
[[nodiscard]] core::Result<SurfaceSpectrum> build_surface_spectrum(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    std::span<const double> signal,
    const SurfaceSpectrumOptions& options = {});

} // namespace carto::surface
