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
    if (!project.validate() || project.schema_version() < 7U) {
        std::cerr << "installed project target does not expose schema v7 or newer\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
