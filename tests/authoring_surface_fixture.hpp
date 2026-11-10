#pragma once
#include "authoring_viewport_fixture.hpp"
#include <carto/attributes/uv.hpp>
namespace {
struct Pixels {
    project::TextureSourceRecord record;
    std::vector<std::uint8_t> encoded;
};
// Independent, uncompressed, top-left RGBA TGA fixtures; no repository assets.
Pixels pixels(std::uint64_t id, std::uint16_t width, std::uint16_t height,
              std::initializer_list<std::array<std::uint8_t, 4>> colors) {
    Pixels p;
    p.encoded.resize(18);
    p.encoded[2] = 2;
    p.encoded[12] = static_cast<std::uint8_t>(width);
    p.encoded[13] = static_cast<std::uint8_t>(width >> 8);
    p.encoded[14] = static_cast<std::uint8_t>(height);
    p.encoded[15] = static_cast<std::uint8_t>(height >> 8);
    p.encoded[16] = 32;
    p.encoded[17] = 0x28;
    check(colors.size() == std::size_t(width) * height, "synthetic fixture extent");
    for (const auto &c : colors)
        p.encoded.insert(p.encoded.end(), {c[2], c[1], c[0], c[3]});
    p.record.id = id;
    p.record.name = "Synthetic pixel source";
    p.record.relative_locator = "textures/synthetic.tga";
    p.record.content_digest = assets::sha256(p.encoded);
    p.record.format = assets::TextureSourceFormat::tga;
    p.record.width = width;
    p.record.height = height;
    p.record.provenance = "Independent numeric fixture";
    return p;
}
[[maybe_unused]] geometry::EditableMesh uv_triangle(std::uint64_t uv = 9, bool slanted = false) {
    auto mesh = triangle();
    if (slanted) {
        mesh = geometry::EditableMesh{};
        okay(mesh.insert_bulk(
            {{geometry::VertexId{1}, {-1, -1, 0}},
             {geometry::VertexId{2}, {1, -1, -1}},
             {geometry::VertexId{3}, {0, 1, 0}}},
            {{geometry::FaceId{1},
              {geometry::VertexId{1}, geometry::VertexId{2}, geometry::VertexId{3}}}}));
    }
    attributes::AttributeSet attrs(
        {{attributes::AttributeDomain::face, 1}, {attributes::AttributeDomain::corner, 3}});
    okay(attrs.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                           attributes::AttributeType::uint32},
                          {std::uint32_t{1}}}));
    okay(attrs.add_layer(
        {{"uv.test", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
         {core::Vec2d{0, 1}, core::Vec2d{1, 1}, core::Vec2d{.5, 0}}}));
    attributes::UvSetTable sets;
    okay(sets.add({uv, "Explicit UV", "uv.test", true, true, false, 1}));
    okay(mesh.set_attribute_payload(std::move(attrs), std::move(sets)));
    return mesh;
}

} // namespace
