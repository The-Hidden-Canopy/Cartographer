#pragma once

#include <carto/core/result.hpp>
#include <carto/providers/registry.hpp>

namespace carto::io {

// Registers the in-process OBJ importer and exporter through the same bounded
// provider lifecycle used by future external adapters.
[[nodiscard]] core::Result<void> register_builtin_obj_providers(
    providers::Registry& registry);

} // namespace carto::io
