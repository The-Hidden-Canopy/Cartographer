#pragma once

#include <carto/project/surface_material_policy.hpp>

namespace carto::project {

struct RepresentationPolicyOutcomeDigests {
    std::string view;
    std::string material;
    std::string constraint;
    std::string fracture;
};

struct RepresentationPolicyAdmission {
    surface::RepresentationId representation;
    std::string representation_digest;
    std::string catalog_digest;
    RepresentationPolicyOutcomeDigests outcomes;
    std::string digest;
};

[[nodiscard]] core::Result<RepresentationPolicyAdmission>
admit_current_inherited_representation(
    const surface::SurfaceRepresentationTree& tree,
    const std::vector<surface::RepresentationInheritanceRecord>& inheritance,
    const std::vector<surface::RepresentationViewPolicyBinding>& view_bindings,
    const std::vector<RepresentationMaterialBinding>& material_bindings,
    const MaterialCatalog& catalog,
    const std::vector<surface::RepresentationConstraintBinding>& constraint_bindings,
    const std::vector<surface::RepresentationFracturePolicyBinding>& fracture_bindings,
    const std::vector<geometry::FaceId>& source_faces,
    surface::RepresentationId representation,
    const RepresentationPolicyOutcomeDigests& expected_outcomes);

} // namespace carto::project
