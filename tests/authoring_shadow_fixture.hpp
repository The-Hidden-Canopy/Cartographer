#pragma once
#include "authoring_surface_fixture.hpp"
#include <cmath>

namespace {
inline Fixture shadow_fixture() {
    auto f = fixture();
    mutate(f.document, [&](auto &tx) {
        okay(tx.set_object_transform(f.front, {{.3, 0, 0}, {}, {.35, .35, 1}}));
        okay(tx.set_object_transform(f.back, {{0, 0, -1}, {}, {2, 2, 1}}));
        okay(tx.set_object_transform(f.light, {{}, {0, std::sin(.3), 0, std::cos(.3)}}));
    });
    auto catalog = f.document.material_catalog();
    for (auto id : {1U, 2U}) {
        auto m = catalog.materials().at(id);
        m.revision = m.revision.next();
        m.definition.base_color_factor = {.7, .7, .7, 1};
        m.definition.roughness = .8;
        okay(catalog.replace_material(m));
    }
    set_materials(f.document, std::move(catalog));
    return f;
}
inline production::AuthoringViewportRequest shadow_request(const Fixture &f) {
    auto r = request(f);
    r.width = r.height = 96;
    r.shadows.enabled = true;
    r.shadows.resolution = 128;
    return r;
}
inline std::shared_ptr<production::AuthoringTextureLibrary> mask_shadow_caster(Fixture &f) {
    mutate(f.document, [&](auto &tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle())); });
    const auto mask = pixels(81, 2, 1, {{255, 255, 255, 0}, {255, 255, 255, 255}});
    auto library = std::make_shared<production::AuthoringTextureLibrary>();
    okay(library->admit(mask.record, mask.encoded));
    auto c = f.document.material_catalog();
    okay(c.insert_texture(mask.record));
    auto material = c.materials().at(1);
    material.revision = material.revision.next();
    material.definition.base_color = {81, 9};
    material.definition.alpha_mode = render::AlphaMode::masked;
    okay(c.replace_material(material));
    set_materials(f.document, std::move(c));
    return library;
}
} // namespace
