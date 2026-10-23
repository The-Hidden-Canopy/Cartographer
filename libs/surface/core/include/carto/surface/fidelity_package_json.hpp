#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/surface/core.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace carto::surface {

// Producer-side manifest for a provider-neutral fidelity package. The
// manifest contains identity and measured metadata only. Closure payload
// bytes stay in separately addressed artifacts referenced by SHA-256 digest.
inline constexpr std::uint32_t kFidelityPackageJsonFormatVersion = 2U;
inline constexpr std::size_t kMaxFidelityPackageJsonBytes = 16U * 1024U * 1024U;

// Validate a compiled Cartographer package and serialize the exact JSON
// contract. The returned package_digest is the serialized contract digest,
// distinct from the internal Cartographer digest stored in FidelityPackage.
[[nodiscard]] core::Result<std::string> serialize_fidelity_package_json(
    const FidelityPackage& package,
    const geometry::CompiledMesh& source);

} // namespace carto::surface
