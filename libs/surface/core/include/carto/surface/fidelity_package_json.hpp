#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/surface/core.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace carto::surface {

// This is the producer-side manifest boundary for the VANTA evidence package.
// Cartographer owns authoring and compilation; VANTA remains responsible for
// admission, asset publication, and runtime ownership. The manifest contains
// identity and measured metadata only. Closure payload bytes stay in the
// separately-addressed artifacts referenced by their SHA-256 digests.
inline constexpr std::uint32_t kVantaFidelityPackageJsonFormatVersion = 1U;
inline constexpr std::size_t kMaxVantaFidelityPackageJsonBytes = 16U * 1024U * 1024U;

// Validate a compiled Cartographer package and serialize the exact JSON
// contract consumed by vanta::render::parse_fidelity_package(). The returned
// package_digest is the VANTA canonical digest, not the internal Cartographer
// package digest stored in VantaFidelityPackage::digest.
[[nodiscard]] core::Result<std::string> serialize_vanta_fidelity_package_json(
    const VantaFidelityPackage& package,
    const geometry::CompiledMesh& source);

} // namespace carto::surface
