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

// Adapts a validated geometry-kernel edit receipt into the source-index
// mappings consumed by the generic attribute transfer layer. The revision
// binding prevents a receipt from being applied to a different source or
// destination state, and unknown source identities fail closed.
[[nodiscard]] core::Result<attributes::TopologyProvenance> provenance_from_receipt(
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& destination,
    const geometry::TopologyEditReceipt& receipt);

// Applies a two-vertex weld and transfers supported attribute domains from
// the exact source topology. The mesh is unchanged when transfer is refused.
[[nodiscard]] core::Result<geometry::TopologyEditReceipt>
merge_vertices_with_attribute_transfer(
    geometry::EditableMesh& mesh,
    geometry::VertexId target,
    geometry::VertexId source,
    geometry::VertexMergeMode mode);

// Applies a bounded single-face knife cut and transfers supported mesh
// attributes. Edge-interior endpoints receive position-weighted vertex and
// corner values; generated path interiors are explicitly invalidated.
[[nodiscard]] core::Result<geometry::TopologyEditReceipt>
knife_cut_with_attribute_transfer(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    const std::vector<core::Vec3d>& path);

} // namespace carto::mesh_attributes
