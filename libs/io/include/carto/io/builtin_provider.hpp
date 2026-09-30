#pragma once

#include <carto/core/result.hpp>
#include <carto/providers/registry.hpp>

namespace carto::io {

// Registers the in-process OBJ importer and exporter through the same bounded
// provider lifecycle used by future external adapters.
[[nodiscard]] core::Result<void> register_builtin_obj_providers(
    providers::Registry& registry);

// Registers the deterministic glTF export profile through the same lifecycle
// gate.  The profile is neutral on the Cartographer side and is shaped for
// VANTA's bounded public glTF importer contract without linking VANTA.
[[nodiscard]] core::Result<void> register_builtin_gltf_providers(
    providers::Registry& registry);

[[nodiscard]] core::Result<void> register_builtin_ply_providers(
    providers::Registry& registry);

[[nodiscard]] core::Result<void> register_builtin_stl_providers(
    providers::Registry& registry);

} // namespace carto::io
