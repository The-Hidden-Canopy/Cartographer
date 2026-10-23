#include <carto/project/surface_policy_admission.hpp>

#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::project {

namespace {

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic refused(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

std::string digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

} // namespace

core::Result<RepresentationPolicyAdmission> admit_current_inherited_representation(
    const surface::SurfaceRepresentationTree& tree,
    const std::vector<surface::RepresentationInheritanceRecord>& inheritance,
    const std::vector<surface::RepresentationViewPolicyBinding>& view_bindings,
    const std::vector<RepresentationMaterialBinding>& material_bindings,
    const MaterialCatalog& catalog,
    const std::vector<surface::RepresentationConstraintBinding>& constraint_bindings,
    const std::vector<surface::RepresentationFracturePolicyBinding>& fracture_bindings,
    const std::vector<geometry::FaceId>& source_faces,
    surface::RepresentationId representation,
    const RepresentationPolicyOutcomeDigests& expected_outcomes) {
    const auto view = surface::evaluate_inherited_view_policy(
        tree, inheritance, view_bindings, representation);
    if (!view) return core::Result<RepresentationPolicyAdmission>::failure(view.error());
    const auto material = evaluate_inherited_material_policy(
        tree, inheritance, material_bindings, catalog, representation);
    if (!material) return core::Result<RepresentationPolicyAdmission>::failure(material.error());
    const auto constraint = surface::evaluate_inherited_constraint_policy(
        tree, inheritance, constraint_bindings, representation);
    if (!constraint) return core::Result<RepresentationPolicyAdmission>::failure(constraint.error());
    const auto fracture = surface::evaluate_inherited_fracture_policy(
        tree, inheritance, fracture_bindings, source_faces, representation);
    if (!fracture) return core::Result<RepresentationPolicyAdmission>::failure(fracture.error());

    if (!view.value().accepted) {
        return core::Result<RepresentationPolicyAdmission>::failure(
            refused("current view policy outcome rejects the representation"));
    }
    if (!constraint.value().accepted) {
        return core::Result<RepresentationPolicyAdmission>::failure(
            refused("current representation constraints are violated"));
    }
    const RepresentationPolicyOutcomeDigests current{
        view.value().digest, material.value().digest,
        constraint.value().digest, fracture.value().digest};
    if (current.view != expected_outcomes.view ||
        current.material != expected_outcomes.material ||
        current.constraint != expected_outcomes.constraint ||
        current.fracture != expected_outcomes.fracture) {
        return core::Result<RepresentationPolicyAdmission>::failure(
            stale("policy outcomes are stale or do not bind the current inputs"));
    }
    std::ostringstream canonical;
    canonical << "surface-inherited-policy-admission-v1\n"
              << "representation=" << representation.value << '\n'
              << "representation_digest=" << view.value().representation_digest << '\n'
              << "catalog_digest=" << material.value().catalog_digest << '\n'
              << "view=" << current.view << '\n'
              << "material=" << current.material << '\n'
              << "constraint=" << current.constraint << '\n'
              << "fracture=" << current.fracture << '\n';
    RepresentationPolicyAdmission admission{
        representation, view.value().representation_digest,
        material.value().catalog_digest, current, digest_text(canonical.str())};
    return core::Result<RepresentationPolicyAdmission>::success(std::move(admission));
}

} // namespace carto::project
