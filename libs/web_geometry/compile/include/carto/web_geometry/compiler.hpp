#pragma once

#include <carto/attributes/set.hpp>
#include <carto/web_geometry/types.hpp>

namespace carto::web_geometry {

// Compiles a renderer-independent, deterministic leaf hierarchy. The input
// CompiledMesh is derived authoring data; this call never mutates it or the
// supplied attributes and allocates no GPU resources.
[[nodiscard]] core::Result<WebGeometryPackage> compile_web_geometry(
    const geometry::CompiledMesh& mesh,
    const attributes::AttributeSet& attributes,
    const WebGeometryCompileOptions& options,
    const ProvenanceContext& provenance);

} // namespace carto::web_geometry
