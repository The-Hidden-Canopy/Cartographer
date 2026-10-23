#include <carto/assets/blob_store.hpp>
#include <carto/project/surface_material_policy.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

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

carto::project::MaterialCatalog catalog_fixture() {
    using namespace carto;
    project::MaterialCatalog catalog;
    render::MaterialAsset material;
    material.id = 7U;
    material.name = "Catalog-bound";
    material.revision = core::Revision{1U};
    material.definition.roughness = 0.6;
    require(static_cast<bool>(catalog.insert_material(material)), "material is admitted");

    project::MaterialVariant variant;
    variant.id = 8U;
    variant.name = "Catalog variant";
    variant.base_material = material.id;
    variant.overrides.roughness = 0.25;
    variant.revision = core::Revision{1U};
    require(static_cast<bool>(catalog.insert_variant(variant)), "variant is admitted");
    require(static_cast<bool>(catalog.insert_assignment({{900U, 4U}, 7U, 8U})),
        "source region receives catalog binding");
    require(static_cast<bool>(catalog.insert_assignment({{900U, 5U}, 7U, std::nullopt})),
        "alternate source region receives catalog binding");
    require(static_cast<bool>(catalog.validate()), "material catalog validates");
    return catalog;
}

carto::surface::SurfaceRepresentationTree tree_fixture() {
    using namespace carto::surface;
    SurfaceRepresentationTree tree;
    const SurfaceDomainId domain{3U};
    const RepresentationId root{10U};
    const RepresentationId child{11U};
    require(static_cast<bool>(tree.add({root, domain, std::nullopt, {child},
        SurfaceRepresentationKind::authored, {}, digest("material-root")})),
        "root is admitted");
    require(static_cast<bool>(tree.add({child, domain, root, {},
        SurfaceRepresentationKind::simplified_geometry, {}, digest("material-child")})),
        "child is admitted");
    return tree;
}

void catalog_bound_material_inheritance_is_deterministic() {
    using namespace carto;
    const auto catalog = catalog_fixture();
    const auto tree = tree_fixture();
    const std::vector<surface::RepresentationInheritanceRecord> inheritance{{
        surface::RepresentationId{11U}, surface::RepresentationId{10U},
        {surface::RepresentationPolicyField::material}, {}}};
    const std::vector<project::RepresentationMaterialBinding> bindings{
        {surface::RepresentationId{10U}, {{900U, 4U}}},
        {surface::RepresentationId{11U}, {{900U, 4U}}},
    };
    const auto first = project::evaluate_inherited_material_policy(
        tree, inheritance, bindings, catalog, surface::RepresentationId{11U});
    const auto second = project::evaluate_inherited_material_policy(
        tree, inheritance, bindings, catalog, surface::RepresentationId{11U});
    require(static_cast<bool>(first) && static_cast<bool>(second),
        "inherited material policy resolves");
    require(first.value().resolved_materials.size() == 1U,
        "resolved binding count is stable");
    require(first.value().resolved_materials.front().material == 7U &&
        first.value().resolved_materials.front().variant ==
            std::optional<project::MaterialVariantId>{8U},
        "material and variant match catalog assignment");
    require(first.value().digest == second.value().digest,
        "repeated policy evaluation has the same digest");
    require(first.value().catalog_digest == catalog.canonical_digest().value().hex(),
        "outcome binds the catalog digest");

    const std::vector<surface::RepresentationInheritanceRecord> overridden{{
        surface::RepresentationId{11U}, surface::RepresentationId{10U}, {},
        {surface::RepresentationPolicyField::material}}};
    const std::vector<project::RepresentationMaterialBinding> override_bindings{
        {surface::RepresentationId{10U}, {{900U, 4U}}},
        {surface::RepresentationId{11U}, {{900U, 5U}}},
    };
    const auto override_result = project::evaluate_inherited_material_policy(
        tree, overridden, override_bindings, catalog, surface::RepresentationId{11U});
    require(static_cast<bool>(override_result), "explicit catalog material override resolves");
    require(override_result.value().resolved_materials.front().source_region.region_id == 5U &&
        override_result.value().digest != first.value().digest,
        "material override is bound to its distinct catalog assignment");

    auto mismatched = bindings;
    mismatched.back().source_regions = {{900U, 5U}};
    require(!project::evaluate_inherited_material_policy(
        tree, inheritance, mismatched, catalog, surface::RepresentationId{11U}),
        "child cannot silently replace an inherited region set");

    auto missing_region = bindings;
    missing_region.front().source_regions = {{900U, 99U}};
    require(!project::evaluate_inherited_material_policy(
        tree, inheritance, missing_region, catalog, surface::RepresentationId{11U}),
        "missing catalog assignment is refused");

    require(!project::evaluate_inherited_material_policy(
        tree, inheritance, {}, catalog, surface::RepresentationId{11U}),
        "missing root binding is refused");
    const std::vector<surface::RepresentationInheritanceRecord> unsupported{{
        surface::RepresentationId{11U}, surface::RepresentationId{10U}, {}, {}}};
    require(!project::evaluate_inherited_material_policy(
        tree, unsupported, bindings, catalog, surface::RepresentationId{11U}),
        "missing material inheritance declaration is refused");

    auto invalid_catalog = catalog;
    require(static_cast<bool>(invalid_catalog.replace_assignment(
        {{900U, 4U}, 7U, project::MaterialVariantId{999U}})),
        "fixture can represent a stale variant reference");
    require(!project::evaluate_inherited_material_policy(
        tree, inheritance, bindings, invalid_catalog, surface::RepresentationId{11U}),
        "stale or missing catalog variant is refused");
}

} // namespace

int main() {
    catalog_bound_material_inheritance_is_deterministic();
    std::cout << "PASS catalog-bound material inheritance is deterministic\n";
    return 0;
}
