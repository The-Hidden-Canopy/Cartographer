#include <carto/assets/blob_store.hpp>
#include <carto/project/surface_policy_admission.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::string digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

struct Fixture {
    carto::surface::SurfaceRepresentationTree tree;
    std::vector<carto::surface::RepresentationInheritanceRecord> inheritance;
    std::vector<carto::surface::RepresentationViewPolicyBinding> views;
    std::vector<carto::project::RepresentationMaterialBinding> materials;
    carto::project::MaterialCatalog catalog;
    std::vector<carto::surface::RepresentationConstraintBinding> constraints;
    std::vector<carto::surface::RepresentationFracturePolicyBinding> fractures;
    std::vector<carto::geometry::FaceId> source_faces{{1U}, {2U}, {3U}, {4U}};
    carto::project::RepresentationPolicyOutcomeDigests expected;
};

Fixture make_fixture() {
    using namespace carto;
    Fixture fixture;
    const surface::SurfaceDomainId domain{1U};
    const surface::RepresentationId root{10U};
    const surface::RepresentationId child{11U};
    surface::SurfaceErrorEnvelope child_error;
    child_error.max_position_error = 0.05;
    require(static_cast<bool>(fixture.tree.add({root, domain, std::nullopt, {child},
        surface::SurfaceRepresentationKind::authored, {}, digest("admission-root")})),
        "representation root is valid");
    require(static_cast<bool>(fixture.tree.add({child, domain, root, {},
        surface::SurfaceRepresentationKind::simplified_geometry, child_error,
        digest("admission-child")})), "representation child is valid");
    fixture.inheritance.push_back({child, root,
        {surface::RepresentationPolicyField::view,
         surface::RepresentationPolicyField::material,
         surface::RepresentationPolicyField::constraint,
         surface::RepresentationPolicyField::fracture}, {}});

    surface::SurfaceQualityPolicy view;
    fixture.views.push_back({root, view});

    render::MaterialAsset material;
    material.id = 7U;
    material.name = "Admission material";
    material.revision = core::Revision{1U};
    material.definition.roughness = 0.4;
    require(static_cast<bool>(fixture.catalog.insert_material(material)),
        "catalog material is valid");
    require(static_cast<bool>(fixture.catalog.insert_assignment({{90U, 2U}, 7U, std::nullopt})),
        "catalog region assignment is valid");
    fixture.materials.push_back({root, {{90U, 2U}}});
    fixture.materials.push_back({child, {{90U, 2U}}});

    fixture.constraints.push_back({root, {
        {30U, surface::SurfaceConstraintMetric::max_position_error, 0.1}}});
    fixture.fractures.push_back({root,
        surface::SurfaceFractureMode::balanced_face_id_order, 2U});

    const auto view_outcome = surface::evaluate_inherited_view_policy(
        fixture.tree, fixture.inheritance, fixture.views, child);
    const auto material_outcome = project::evaluate_inherited_material_policy(
        fixture.tree, fixture.inheritance, fixture.materials, fixture.catalog, child);
    const auto constraint_outcome = surface::evaluate_inherited_constraint_policy(
        fixture.tree, fixture.inheritance, fixture.constraints, child);
    const auto fracture_outcome = surface::evaluate_inherited_fracture_policy(
        fixture.tree, fixture.inheritance, fixture.fractures, fixture.source_faces, child);
    require(view_outcome && material_outcome && constraint_outcome && fracture_outcome,
        "all source policy outcomes can be computed");
    fixture.expected = {view_outcome.value().digest, material_outcome.value().digest,
        constraint_outcome.value().digest, fracture_outcome.value().digest};
    return fixture;
}

void admission_recomputes_all_current_policy_receipts() {
    using namespace carto;
    auto fixture = make_fixture();
    const auto admitted = project::admit_current_inherited_representation(
        fixture.tree, fixture.inheritance, fixture.views, fixture.materials,
        fixture.catalog, fixture.constraints, fixture.fractures, fixture.source_faces,
        surface::RepresentationId{11U}, fixture.expected);
    require(static_cast<bool>(admitted), "current digest-bound policies are admitted");
    require(admitted.value().representation_digest == digest("admission-child"),
        "admission binds the current representation digest");
    require(admitted.value().catalog_digest == fixture.catalog.canonical_digest().value().hex(),
        "admission binds the current catalog digest");
    const auto repeated = project::admit_current_inherited_representation(
        fixture.tree, fixture.inheritance, fixture.views, fixture.materials,
        fixture.catalog, fixture.constraints, fixture.fractures, fixture.source_faces,
        surface::RepresentationId{11U}, fixture.expected);
    require(repeated && repeated.value().digest == admitted.value().digest,
        "admission receipt is deterministic");

    auto stale_outcomes = fixture.expected;
    stale_outcomes.view = digest("old-view-outcome");
    require(!project::admit_current_inherited_representation(
        fixture.tree, fixture.inheritance, fixture.views, fixture.materials,
        fixture.catalog, fixture.constraints, fixture.fractures, fixture.source_faces,
        surface::RepresentationId{11U}, stale_outcomes),
        "stale outcome digest is refused");

    surface::SurfaceRepresentationTree changed_tree;
    const surface::SurfaceDomainId domain{1U};
    require(static_cast<bool>(changed_tree.add({surface::RepresentationId{10U}, domain,
        std::nullopt, {surface::RepresentationId{11U}},
        surface::SurfaceRepresentationKind::authored, {}, digest("admission-root")})),
        "changed-tree root is valid");
    surface::SurfaceErrorEnvelope changed_error;
    changed_error.max_position_error = 0.05;
    require(static_cast<bool>(changed_tree.add({surface::RepresentationId{11U}, domain,
        surface::RepresentationId{10U}, {},
        surface::SurfaceRepresentationKind::simplified_geometry,
        changed_error,
        digest("changed-admission-child")})), "changed-tree child is valid");
    require(!project::admit_current_inherited_representation(
        changed_tree, fixture.inheritance, fixture.views, fixture.materials,
        fixture.catalog, fixture.constraints, fixture.fractures, fixture.source_faces,
        surface::RepresentationId{11U}, fixture.expected),
        "outcomes for a prior representation revision are refused");

    auto rejecting_constraints = fixture.constraints;
    rejecting_constraints.front().constraints.front().maximum = 0.01;
    require(!project::admit_current_inherited_representation(
        fixture.tree, fixture.inheritance, fixture.views, fixture.materials,
        fixture.catalog, rejecting_constraints, fixture.fractures, fixture.source_faces,
        surface::RepresentationId{11U}, fixture.expected),
        "a current but rejected constraint outcome blocks admission");
}

} // namespace

int main() {
    admission_recomputes_all_current_policy_receipts();
    std::cout << "PASS current inherited policy admission\n";
    return 0;
}
