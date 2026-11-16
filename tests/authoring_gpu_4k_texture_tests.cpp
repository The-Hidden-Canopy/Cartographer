#include "authoring_surface_fixture.hpp"
#include <carto/d3d12/authoring_viewport.hpp>
#include <chrono>
#include <iostream>

// A real 4K texture, not a 4K render target with tiny texture fixtures.
int main() {
    try {
        static_cast<void>(&pixels); // Other surface tests use this shared fixture helper.
        constexpr std::uint32_t extent = 4096;
        std::vector<std::uint8_t> encoded(18U + std::size_t(extent) * extent * 4U, 0U);
        encoded[2] = 2; // uncompressed, 32-bit, top-left TGA
        encoded[12] = encoded[14] = 0;
        encoded[13] = encoded[15] = 16;
        encoded[16] = 32;
        encoded[17] = 0x28;
        for (std::size_t p = 18; p < encoded.size(); p += 4) {
            encoded[p] = 25; encoded[p + 1] = 90;
            encoded[p + 2] = 220; encoded[p + 3] = 255;
        }
        project::TextureSourceRecord texture;
        texture.id = 71;
        texture.name = "4K texture fixture";
        texture.relative_locator = "textures/four-k.tga";
        texture.content_digest = assets::sha256(encoded);
        texture.format = assets::TextureSourceFormat::tga;
        texture.width = texture.height = extent;
        texture.provenance = "Synthetic 4K GPU admission fixture";
        auto library = std::make_shared<production::AuthoringTextureLibrary>();
        okay(library->admit(texture, encoded));
        encoded.clear();

        auto f = fixture();
        mutate(f.document, [&](auto &tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle())); });
        auto catalog = f.document.material_catalog();
        okay(catalog.insert_texture(texture));
        auto material = catalog.materials().at(1);
        material.revision = material.revision.next();
        material.definition.base_color = {texture.id, 9};
        okay(catalog.replace_material(material));
        auto graph = authoring_graph_fixture();
        okay(catalog.insert_material_graph(graph));
        okay(catalog.bind_material_graph(material.id, graph.identity));
        set_materials(f.document, catalog);
        auto r = request(f);
        r.textures = library;
        auto source = std::make_shared<const project::ProjectDocument>(f.document);
        auto renderer = take(d3d12::AuthoringViewport::create());
        const auto start = std::chrono::steady_clock::now();
        const auto cold = take(renderer->render(source, r));
        const auto wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        check(cold.material_resident_bytes > 85ULL * 1024 * 1024,
              "4K full mip chain is resident");
        check(cold.material_upload_bytes > 0, "4K texture uploaded");
        const auto cpu = take(production::render_authoring_viewport(f.document, r));
        const auto p = center(cpu);
        check(cpu.objects[p] == f.front && cold.image.objects[p] == f.front,
              "4K texture fixture covers the sampled pixel");
        const auto expected = cpu.linear_hdr[p], actual = cold.image.linear_hdr[p];
        for (auto [a, b] : {std::pair{expected.x, actual.x}, std::pair{expected.y, actual.y},
                            std::pair{expected.z, actual.z}})
            check(std::isfinite(b) && std::abs(a - b) < 2e-4 + std::abs(a) * 1e-3,
                  "4K texture GPU color matches CPU reference");
        const auto warm = take(renderer->render(source, r));
        check(warm.material_upload_bytes == 0, "4K texture is reused on warm frame");
        check(cold.image.display_rgba8 == warm.image.display_rgba8,
              "4K texture produces stable displayed pixels");
        // The same source has different linear and sRGB semantic mip artifacts.
        // Two full 4K chains exceed the bounded material payload admission.
        material.revision = material.revision.next();
        material.definition.occlusion = {texture.id, 9};
        okay(catalog.replace_material(material));
        auto over_budget = f.document;
        set_materials(over_budget, catalog);
        const auto refused = renderer->render(
            std::make_shared<const project::ProjectDocument>(over_budget), r);
        check(!refused && refused.error().message.find("GPU material residency byte budget") !=
                              std::string::npos,
              "two 4K semantic chains must fail the material budget explicitly");
        const auto recovered = take(renderer->render(source, r));
        check(recovered.material_upload_bytes == 0 &&
                  recovered.image.display_rgba8 == warm.image.display_rgba8,
              "rejected replacement preserves the last good 4K texture");
        std::cout << "4K texture mip upload and warm reuse passed; device=" << cold.device_name
                  << "; cold_wall_ms=" << wall_ms
                  << "; upload_bytes=" << cold.material_upload_bytes
                  << "; logical_material_resident_bytes=" << cold.material_resident_bytes
                  << "; warm_upload_bytes=" << warm.material_upload_bytes << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
