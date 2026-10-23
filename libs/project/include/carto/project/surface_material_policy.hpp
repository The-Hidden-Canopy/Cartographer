#pragma once

#include <carto/project/material_catalog.hpp>
#include <carto/surface/inheritance.hpp>

#include <string>
#include <vector>

namespace carto::project {

struct RepresentationMaterialBinding {
    surface::RepresentationId representation;
    std::vector<MaterialRegionKey> source_regions;
};

struct ResolvedRepresentationMaterial {
    MaterialRegionKey source_region;
    render::MaterialId material = 0U;
    std::optional<MaterialVariantId> variant;
};

struct RepresentationMaterialPolicyOutcome {
    surface::RepresentationId representation;
    std::string representation_digest;
    std::string catalog_digest;
    std::string inheritance_digest;
    std::string assignment_digest;
    std::string digest;
    std::vector<ResolvedRepresentationMaterial> resolved_materials;
};

[[nodiscard]] core::Result<RepresentationMaterialPolicyOutcome>
evaluate_inherited_material_policy(
    const surface::SurfaceRepresentationTree& tree,
    const std::vector<surface::RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationMaterialBinding>& bindings,
    const MaterialCatalog& catalog,
    surface::RepresentationId representation);

} // namespace carto::project
