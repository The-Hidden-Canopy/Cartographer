#include <carto/production/preparation_operation.hpp>
#include <carto/production/preparation_job.hpp>
#include <carto/project/project.hpp>
#include <carto/world/model.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>

int main() {
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

    return EXIT_SUCCESS;
}
