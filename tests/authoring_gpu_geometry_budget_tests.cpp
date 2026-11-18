#include "authoring_surface_fixture.hpp"
#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/mesh_digest.hpp>
#include <iostream>

namespace {
constexpr std::uint64_t prepared_limit = 64ULL * 1024 * 1024;
constexpr std::uint64_t triangle_bytes = 3 * sizeof(production::RenderMeshVertex) + 3 * sizeof(std::uint32_t);

geometry::EditableMesh islands(std::uint64_t first, std::uint64_t count) {
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    std::vector<attributes::AttributeValue> uv, slots;
    vertices.reserve(static_cast<std::size_t>(count * 3));
    faces.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        // The visible anchor is supplied by fixture(). These triangles exercise
        // preparation/storage admission without a large raster-overdraw workload.
        const double x = 10 + double(first + i) * .002;
        const auto v = i * 3 + 1;
        vertices.push_back({geometry::VertexId{v}, {x, 10, 0}});
        vertices.push_back({geometry::VertexId{v + 1}, {x + .001, 10, 0}});
        vertices.push_back({geometry::VertexId{v + 2}, {x, 10.001, 0}});
        faces.push_back({geometry::FaceId{i + 1}, {geometry::VertexId{v}, geometry::VertexId{v + 1}, geometry::VertexId{v + 2}}});
        uv.insert(uv.end(), {core::Vec2d{0, 0}, core::Vec2d{1, 0}, core::Vec2d{0, 1}});
        slots.emplace_back(std::uint32_t{1});
    }
    geometry::EditableMesh mesh;
    okay(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    attributes::AttributeSet payload({{attributes::AttributeDomain::face, static_cast<std::size_t>(count)},
                                     {attributes::AttributeDomain::corner, static_cast<std::size_t>(count * 3)}});
    okay(payload.add_layer({{"uv.test", attributes::AttributeDomain::corner, attributes::AttributeType::vec2}, std::move(uv)}));
    okay(payload.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face, attributes::AttributeType::uint32}, std::move(slots)}));
    attributes::UvSetTable sets;
    okay(sets.add({9, "Explicit UV", "uv.test", true, true, false, 1}));
    okay(mesh.set_attribute_payload(std::move(payload), std::move(sets)));
    return mesh;
}

Fixture textured_islands(std::uint64_t count, const Pixels& color) {
    auto f = fixture();
    mutate(f.document, [&](auto& tx) {
        okay(tx.remove_object(f.back));
        auto catalog = f.document.material_catalog();
        okay(catalog.insert_texture(color.record));
        render::MaterialAsset material;
        material.id = 3;
        material.name = "Independent textured geometry-budget fixture";
        material.definition.base_color = {color.record.id, 9};
        okay(catalog.insert_material(material));
        for (std::uint64_t first = 0; first < count; first += 2400) {
            const auto id = take(tx.add_mesh(islands(first, std::min<std::uint64_t>(2400, count - first))));
            const auto object = take(tx.create_object("Offscreen textured islands"));
            okay(tx.attach_mesh(object, id));
            okay(catalog.insert_assignment({{id, 1}, 3, {}}));
        }
        okay(tx.set_material_catalog(std::move(catalog)));
    });
    okay(f.document.validate());
    return f;
}

assets::Sha256Digest source_digest(const project::ProjectDocument& document) {
    const auto bytes = document.serialize();
    return assets::sha256({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

void same_image(const production::AuthoringViewportImage& a, const production::AuthoringViewportImage& b) {
    check(a.display_rgba8 == b.display_rgba8 && a.objects == b.objects && a.faces == b.faces &&
          a.depth == b.depth, "geometry-cache paths preserve all display/picking/depth attachments");
    check(a.linear_hdr.size() == b.linear_hdr.size(), "HDR attachment size is preserved");
    for (std::size_t i = 0; i < a.linear_hdr.size(); ++i)
        check(a.linear_hdr[i].x == b.linear_hdr[i].x && a.linear_hdr[i].y == b.linear_hdr[i].y &&
              a.linear_hdr[i].z == b.linear_hdr[i].z, "HDR radiance is preserved exactly");
}

void independent_geometry_budget(d3d12::AuthoringViewport& renderer) {
    const auto color = pixels(91, 1, 1, {{160, 180, 200, 255}});
    auto library = std::make_shared<production::AuthoringTextureLibrary>();
    okay(library->admit(color.record, color.encoded));
    // Every island has three distinct source vertices. The full selected
    // artifacts fit, whereas retaining another untextured copy does not.
    const auto count = prepared_limit / triangle_bytes - 1;
    check((count + 1) * triangle_bytes <= prepared_limit && count * triangle_bytes * 2 > prepared_limit,
          "fixture separates useful draw storage from duplicate preparation storage");
    const auto good = textured_islands(count, color);
    const auto before = source_digest(good.document);
    auto r = request(good);
    r.width = r.height = 32;
    r.textures = library;
    auto source = std::make_shared<const project::ProjectDocument>(good.document);
    const auto cold = take(renderer.render(source, r));
    check(cold.image.objects[center(cold.image)] == good.front,
          "accepted large textured scene still renders the visible authored anchor");
    const auto warm = take(renderer.render(good.document, r));
    same_image(cold.image, warm.image);
    check(warm.mesh_compile_count == 0 && warm.mesh_cache_hits > 0 && warm.geometry_upload_bytes == 0,
          "warm textured storage does not rebuild discarded layout-only geometry");

    const auto overflow = textured_islands(prepared_limit / triangle_bytes, color);
    const auto overflow_before = source_digest(overflow.document);
    const auto refused = renderer.render(std::make_shared<const project::ProjectDocument>(overflow.document), r);
    check(!refused && refused.error().message.find("prepared geometry byte budget exceeded") != std::string::npos,
          "actual draw artifacts beyond the unchanged 64-MiB bound are rejected");
    const auto recovered = take(renderer.render(good.document, r));
    same_image(cold.image, recovered.image);
    check(recovered.target_generation == cold.target_generation && recovered.geometry_upload_bytes == 0 &&
          recovered.mesh_compile_count == 0,
          "failed preparation preserves the previous target and warm geometry cache");
    check(source_digest(good.document) == before && source_digest(overflow.document) == overflow_before,
          "admission, rejection and recovery never mutate source documents");
    std::cout << "PASS textured storage within bound, actual overflow, warm reuse and recovery\n";
}

void shared_geometry_material_orders(d3d12::AuthoringViewport& renderer) {
    const auto color = pixels(92, 1, 1, {{160, 180, 200, 255}});
    auto library = std::make_shared<production::AuthoringTextureLibrary>();
    okay(library->admit(color.record, color.encoded));
    for (bool texture_first : {true, false}) {
        auto f = fixture();
        const auto shared = uv_triangle();
        mutate(f.document, [&](auto& tx) {
            okay(tx.replace_mesh(f.front_mesh, shared));
            okay(tx.replace_mesh(f.back_mesh, shared));
            auto back = f.document.scene().find(f.back)->local_transform;
            back.translation.x = 1;
            okay(tx.set_object_transform(f.back, back));
            auto catalog = f.document.material_catalog();
            okay(catalog.insert_texture(color.record));
            auto material = catalog.materials().at(texture_first ? 1 : 2);
            material.definition.base_color = {color.record.id, 9};
            material.revision = material.revision.next();
            okay(catalog.replace_material(material));
            okay(tx.set_material_catalog(std::move(catalog)));
        });
        check(project::canonical_mesh_source_digest(f.document.meshes().at(f.front_mesh)) ==
              project::canonical_mesh_source_digest(f.document.meshes().at(f.back_mesh)),
              "distinct source assets really share their geometry cache identity");
        const auto before = source_digest(f.document);
        auto r = request(f); r.textures = library;
        const auto cold = take(renderer.render(std::make_shared<const project::ProjectDocument>(f.document), r));
        const auto warm = take(renderer.render(std::make_shared<const project::ProjectDocument>(f.document), r));
        same_image(cold.image, warm.image);
        check(warm.mesh_compile_count == 0 && warm.geometry_upload_bytes == 0,
              "a base artifact used by any draw survives either material traversal order");
        check(source_digest(f.document) == before, "shared-geometry material traversal preserves source");
    }
    std::cout << "PASS shared geometry with textured/untextured materials in both orders\n";
}
}

int main() try {
    auto device = std::shared_ptr<d3d12::D3D12Device>(take(d3d12::D3D12Device::create({{}, false, true})));
    auto renderer = take(d3d12::AuthoringViewport::create_on_device(device));
    shared_geometry_material_orders(*renderer);
    independent_geometry_budget(*renderer);
    check(device->debug_receipts().empty(), "resource-bound checks leave native debug validation clean");
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
