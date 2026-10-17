#include <carto/production/bake_graph.hpp>
#include <carto/biological/authoring.hpp>
#include <carto/production_authoring/authoring.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/engineering/authoring.hpp>
#include <carto/production/preparation_operation.hpp>
#include <carto/production/preparation_job.hpp>
#include <carto/project/project.hpp>
#include <carto/spatial_field/scalar_field.hpp>
#include <carto/terrain/terrain_tile.hpp>
#include <carto/ui/ui.hpp>
#include <carto/world/model.hpp>

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>

int main() {
    const auto condition = carto::condition::make_condition_fixture();
    if (!condition || !condition.value().validate()) {
        std::cerr << "installed CONDITION target does not expose the public authoring boundary\n";
        return EXIT_FAILURE;
    }

    const auto engineering = carto::engineering::make_dv012_fixture();
    if (!engineering || !engineering.value().validate()) {
        std::cerr << "installed engineering target does not expose the public authoring boundary\n";
        return EXIT_FAILURE;
    }

    carto::production::ProductionCookGraph bake_graph;
    if (!bake_graph.validate()) {
        std::cerr << "installed production BakeGraph target does not expose the public compiler boundary\n";
        return EXIT_FAILURE;
    }

    carto::biological::BiologicalAuthoringDocument biological;
    biological.source = carto::biological::AuthoringSourceIdentity{
        "package/biological", 1U, carto::core::Revision{1U},
        carto::assets::sha256(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>("package-biological"), 18U})};
    biological.organism_family = "package-smoke";
    if (!biological.validate()) {
        std::cerr << "installed biological target does not expose the public authoring boundary\n";
        return EXIT_FAILURE;
    }

    carto::production_authoring::SculptStack sculpt;
    if (sculpt.mesh_asset != 0U || sculpt.validate()) {
        std::cerr << "installed production authoring target has an invalid default state\n";
        return EXIT_FAILURE;
    }

    carto::world::WorldModel world;
    const auto inserted = world.insert_area(carto::world::SemanticArea{
        .id = carto::world::AreaId{1U},
        .semantic_type = "package-smoke",
        .geometry = {},
        .parent = std::nullopt,
        .tags = {"public"},
    });
    if (!inserted || !world.validate()) {
        std::cerr << "installed world target is not consumable\n";
        return EXIT_FAILURE;
    }

    const carto::project::ProjectDocument project;
    if (!project.validate() || project.schema_version() !=
            carto::project::ProjectDocument::kSchemaVersion ||
        !project.material_catalog().empty() ||
        !project.source_namespace().starts_with("cartographer.project.")) {
        std::cerr << "installed project target does not expose the current schema\n";
        return EXIT_FAILURE;
    }

    const auto profile = carto::production::portable_preparation_profile();
    if (!profile.validate() || !profile.canonical_digest()) {
        std::cerr << "installed production target does not expose preparation\n";
        return EXIT_FAILURE;
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    if (ui.snapshot().market.status != carto::ui::MarketStatus::unconfigured) {
        std::cerr << "installed UI target does not expose the bounded market projection\n";
        return EXIT_FAILURE;
    }

    const auto terrain_source = carto::spatial_field::ScalarField2D::create(
        carto::terrain::terrain_tile_identity({0, 0}),
        carto::spatial_field::FieldSemantic::terrain_elevation,
        {0.0, 0.0}, 1.0, 2U, 2U, 0.0, 10.0, 0.0);
    if (!terrain_source) {
        std::cerr << "installed terrain source types are not consumable\n";
        return EXIT_FAILURE;
    }
    const auto terrain_product = carto::terrain::build_terrain_tile(
        terrain_source.value(), {0, 0});
    const auto bathymetry = carto::terrain::to_bathymetry_field(
        terrain_source.value(), "package-smoke.terrain-xy.v1");
    if (!terrain_product || !terrain_product.value().validate() || !bathymetry ||
        !bathymetry.value().validate()) {
        std::cerr << "installed terrain/hydro bridge is not consumable\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
