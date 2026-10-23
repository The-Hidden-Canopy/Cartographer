#pragma once

#include <carto/biological/authoring.hpp>
#include <carto/geometry/mesh.hpp>

namespace carto::biological {

// Deterministic editable proxy geometry compiled from an explicit source
// profile. This compiler makes no anatomical-accuracy claim.
struct ProxyMeshProduct {
    AuthoringSourceIdentity source;
    StructureId structure;
    geometry::EditableMesh mesh;
    assets::Sha256Digest product_digest;

    [[nodiscard]] core::Result<void> validate_against(
        const BiologicalAuthoringDocument& document) const;
};

[[nodiscard]] core::Result<ProxyMeshProduct> compile_ellipsoid_proxy(
    const BiologicalAuthoringDocument& document,
    StructureId structure);

} // namespace carto::biological
