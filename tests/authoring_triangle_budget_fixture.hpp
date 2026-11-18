#pragma once
#include "authoring_viewport_fixture.hpp"

namespace {
// Repeated authored objects exercise the scene total independently of the
// per-mesh, geometry-cache and primitive-ID bounds. Each mesh has 2,048 faces.
inline Fixture triangle_budget_fixture() {
    auto f = fixture();
    constexpr std::uint64_t cells = 32, stride = cells + 1;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    for (std::uint64_t y = 0; y <= cells; ++y)
        for (std::uint64_t x = 0; x <= cells; ++x)
            vertices.push_back({geometry::VertexId{vertices.size() + 1},
                {-1.0 + 2.0 * double(x) / cells, -1.0 + 2.0 * double(y) / cells, 0}});
    for (std::uint64_t y = 0; y < cells; ++y)
        for (std::uint64_t x = 0; x < cells; ++x) {
            const auto a = y * stride + x + 1, b = a + 1, c = a + stride, d = c + 1;
            for (const auto ids : {std::array{a, b, d}, std::array{a, d, c}})
                faces.push_back({geometry::FaceId{faces.size() + 1},
                    {geometry::VertexId{ids[0]}, geometry::VertexId{ids[1]}, geometry::VertexId{ids[2]}}});
        }
    geometry::EditableMesh grid;
    okay(grid.insert_bulk(vertices, faces));
    attributes::AttributeSet payload({{attributes::AttributeDomain::face, faces.size()}});
    okay(payload.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
        attributes::AttributeType::uint32},
        std::vector<attributes::AttributeValue>(faces.size(), std::uint32_t{1})}));
    okay(grid.set_attribute_payload(std::move(payload), {}));
    mutate(f.document, [&](auto& tx) {
        okay(tx.replace_mesh(f.front_mesh, std::move(grid)));
        okay(tx.remove_object(f.back));
        for (unsigned i = 1; i < 128; ++i) {
            const auto object = take(tx.create_object("Scene triangle budget instance"));
            okay(tx.attach_mesh(object, f.front_mesh));
        }
    });
    okay(f.document.validate());
    return f;
}

inline project::ProjectDocument one_triangle_over_scene_budget(const Fixture& f) {
    auto overflow = f.document;
    mutate(overflow, [&](auto& tx) {
        const auto object = take(tx.create_object("One triangle over scene budget", {{0, 0, -1}}));
        okay(tx.attach_mesh(object, f.back_mesh));
    });
    return overflow;
}
} // namespace
