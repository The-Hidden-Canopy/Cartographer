#pragma once
#include "authoring_viewport_fixture.hpp"
#include <numbers>
namespace {
inline project::AuthoredEnvironment asymmetric_environment() {
    project::AuthoredEnvironment e;
    e.identity = "environment.synthetic";
    e.width = 32;
    e.height = 16;
    for (unsigned y = 0; y < e.height; ++y)
        for (unsigned x = 0; x < e.width; ++x) {
            const double theta = std::numbers::pi * (y + .5) / e.height;
            const double phi = 2 * std::numbers::pi * ((x + .5) / e.width - .5);
            const core::Vec3d d{std::sin(theta) * std::cos(phi), std::cos(theta),
                                std::sin(theta) * std::sin(phi)};
            e.linear_radiance.push_back({.2 + 12 * std::pow(std::max(0., d.x), 32),
                                         .1 + .8 * std::max(0., d.y),
                                         .3 + 6 * std::pow(std::max(0., d.z), 16)});
        }
    return e;
}
inline Fixture environment_fixture(const project::AuthoredEnvironment &e) {
    auto f = fixture();
    project::PresentationCatalog catalog;
    okay(catalog.insert_camera(f.document.presentation_catalog().cameras().at(f.camera)));
    okay(catalog.set_environment(e));
    mutate(f.document, [&](auto &tx) { okay(tx.set_presentation_catalog(catalog)); });
    auto materials = f.document.material_catalog();
    auto m = materials.materials().at(1);
    m.revision = m.revision.next();
    m.definition.base_color_factor = {1, 1, 1, 1};
    m.definition.metallic = 1;
    m.definition.roughness = .1;
    okay(materials.replace_material(m));
    set_materials(f.document, std::move(materials));
    return f;
}
} // namespace
