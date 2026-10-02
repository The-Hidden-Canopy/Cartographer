#pragma once

#include <carto/attributes/provenance.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>

namespace carto::mesh_attributes {

// Converts the geometry kernel's stable IDs into the domain ordering consumed
// by the generic attribute transfer layer. Positions and topology adjacency
// are deliberately not inferred here; only kernel-owned identity is carried.
[[nodiscard]] core::Result<attributes::StableTopologySnapshot>
stable_topology_snapshot(const geometry::EditableMesh& mesh);

// Derives transfer provenance across two validated mesh states. Created mesh
// elements receive empty source mappings and therefore require an explicit
// invalidate/duplicate policy at the attribute layer.
[[nodiscard]] core::Result<attributes::TopologyProvenance> derive_provenance(
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& destination);

} // namespace carto::mesh_attributes
