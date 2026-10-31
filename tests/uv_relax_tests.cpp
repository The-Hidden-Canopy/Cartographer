#include <carto/production/uv_relax.hpp>
#include <carto/attributes/uv.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)
using namespace carto;

geometry::EditableMesh fan(bool seam = false, bool degenerate = false, bool split_uv = false,
                           bool concave = false, bool mirror = false) {
    geometry::EditableMesh mesh;
    CHECK(mesh.insert_bulk(
        {{geometry::VertexId{1}, {0, 0, 0}},
         {geometry::VertexId{2}, {1, 0, 0}},
         {geometry::VertexId{3}, {1, 1, 0}},
         {geometry::VertexId{4}, {0, 1, 0}},
         {geometry::VertexId{5}, {0.5, 0.5, 0}}},
        {{geometry::FaceId{1},
          {geometry::VertexId{1}, geometry::VertexId{2}, geometry::VertexId{5}}},
         {geometry::FaceId{2},
          {geometry::VertexId{2}, geometry::VertexId{3}, geometry::VertexId{5}}},
         {geometry::FaceId{3},
          {geometry::VertexId{3}, geometry::VertexId{4}, geometry::VertexId{5}}},
         {geometry::FaceId{4},
          {geometry::VertexId{4}, geometry::VertexId{1}, geometry::VertexId{5}}}}));
    const auto topology = mesh.topology();
    CHECK(topology);
    attributes::AttributeSet attrs(
        {{attributes::AttributeDomain::corner, topology.value().corners.size()},
         {attributes::AttributeDomain::edge, topology.value().edges.size()}});
    std::vector<attributes::AttributeValue> values;
    for (const auto &c : topology.value().corners) {
        switch (c.vertex.value) {
        case 1:
            values.emplace_back(core::Vec2d{0, 0});
            break;
        case 2:
            values.emplace_back(core::Vec2d{1, 0});
            break;
        case 3:
            values.emplace_back(core::Vec2d{1, 1});
            break;
        case 4:
            values.emplace_back(core::Vec2d{0, 1});
            break;
        default:
            values.emplace_back(degenerate
                                    ? core::Vec2d{0.5, 0}
                                    : (split_uv && c.face.value == 1 ? core::Vec2d{0.3, 0.4}
                                                                     : core::Vec2d{0.25, 0.4}));
        }
        if (concave) {
            switch (c.vertex.value) {
            case 1:
                values.back() = core::Vec2d{0, 0};
                break;
            case 2:
                values.back() = core::Vec2d{2, 0};
                break;
            case 3:
                values.back() = core::Vec2d{0.2, 0.2};
                break;
            case 4:
                values.back() = core::Vec2d{0, 2};
                break;
            default:
                values.back() = core::Vec2d{0.1, 0.1};
                break;
            }
        }
        if (mirror) {
            auto value = std::get<core::Vec2d>(values.back());
            value.x = -value.x;
            values.back() = value;
        }
    }
    CHECK(attrs.add_layer(
        {{"uv.source", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
         std::move(values)}));
    if (seam) {
        std::vector<attributes::AttributeValue> flags(topology.value().edges.size(), false);
        bool found = false;
        for (std::size_t i = 0; i < topology.value().edges.size(); ++i) {
            const auto &edge = topology.value().edges[i];
            if (edge.first.value == 1 && edge.second.value == 5) {
                flags[i] = true;
                found = true;
            }
        }
        CHECK(found);
        CHECK(attrs.add_layer(
            {{attributes::uv_seam_attribute_id(7), attributes::AttributeDomain::edge,
              attributes::AttributeType::boolean},
             std::move(flags)}));
    }
    attributes::UvSetTable sets;
    CHECK(sets.add({7, "Source", "uv.source", true, true, false, 1}));
    CHECK(mesh.set_attribute_payload(std::move(attrs), std::move(sets)));
    return mesh;
}

int main() {
    try {
        auto mesh = fan();
        auto before = mesh.revision();
        auto result = production::relax_uv_triangles(mesh, 7, 8, "Relaxed", "uv.relaxed", 1, 1.0);
        CHECK(result);
        auto again = production::relax_uv_triangles(mesh, 7, 8, "Relaxed", "uv.relaxed", 1, 1.0);
        CHECK(again);
        CHECK(mesh.revision() == before);
        CHECK(!mesh.uv_sets().find(8));
        CHECK(result.value().uv_sets().active_for_render()->id == 7);
        auto topo = mesh.topology();
        CHECK(topo);
        const auto *out = result.value().attributes().find("uv.relaxed");
        const auto *out2 = again.value().attributes().find("uv.relaxed");
        CHECK(out && out2);
        for (std::size_t i = 0; i < out->values.size(); ++i) {
            auto a = std::get<core::Vec2d>(out->values[i]);
            auto b = std::get<core::Vec2d>(out2->values[i]);
            CHECK(a.x == b.x && a.y == b.y);
        }
        for (std::size_t i = 0; i < topo.value().corners.size(); ++i) {
            const auto uv = std::get<core::Vec2d>(out->values[i]);
            if (topo.value().corners[i].vertex.value == 5)
                CHECK(std::abs(uv.x - 0.5) < 1e-12 && std::abs(uv.y - 0.5) < 1e-12);
            else {
                const auto *original = mesh.attributes().find("uv.source");
                const auto source = std::get<core::Vec2d>(original->values[i]);
                CHECK(uv.x == source.x && uv.y == source.y);
            }
        }
        auto twice = production::relax_uv_triangles(mesh, 7, 8, "Relaxed", "uv.relaxed", 2, 0.5);
        CHECK(twice);
        const auto *two = twice.value().attributes().find("uv.relaxed");
        for (std::size_t i = 0; i < topo.value().corners.size(); ++i)
            if (topo.value().corners[i].vertex.value == 5) {
                const auto uv = std::get<core::Vec2d>(two->values[i]);
                CHECK(std::abs(uv.x - 0.4375) < 1e-12 && std::abs(uv.y - 0.475) < 1e-12);
            }
        auto discontinuous = fan(false, false, true);
        auto divided =
            production::relax_uv_triangles(discontinuous, 7, 8, "Relaxed", "uv.relaxed", 3, 1);
        CHECK(divided);
        const auto *original_split = discontinuous.attributes().find("uv.source");
        const auto *divided_uv = divided.value().attributes().find("uv.relaxed");
        for (std::size_t i = 0; i < divided_uv->values.size(); ++i) {
            auto a = std::get<core::Vec2d>(original_split->values[i]);
            auto b = std::get<core::Vec2d>(divided_uv->values[i]);
            CHECK(a.x == b.x && a.y == b.y);
        }
        auto split = fan(true);
        auto pinned = production::relax_uv_triangles(split, 7, 8, "Relaxed", "uv.relaxed", 3, 1);
        CHECK(pinned);
        const auto *seam = pinned.value().attributes().find(attributes::uv_seam_attribute_id(8));
        CHECK(seam);
        for (std::size_t i = 0; i < seam->values.size(); ++i)
            CHECK(std::get<bool>(seam->values[i]) ==
                  std::get<bool>(
                      split.attributes().find(attributes::uv_seam_attribute_id(7))->values[i]));
        const auto *src = split.attributes().find("uv.source");
        const auto *dst = pinned.value().attributes().find("uv.relaxed");
        for (std::size_t i = 0; i < dst->values.size(); ++i) {
            auto a = std::get<core::Vec2d>(dst->values[i]);
            auto b = std::get<core::Vec2d>(src->values[i]);
            CHECK(a.x == b.x && a.y == b.y);
        }
        CHECK(!production::relax_uv_triangles(fan(false, true), 7, 8, "R", "uv.out", 1, 1));
        auto mirrored = fan(false, false, false, false, true);
        auto mirrored_result = production::relax_uv_triangles(mirrored, 7, 8, "R", "uv.out", 1, 1);
        CHECK(mirrored_result);
        auto mirror_topo = mirrored.topology();
        CHECK(mirror_topo);
        const auto *mirror_out = mirrored_result.value().attributes().find("uv.out");
        for (std::size_t i = 0; i < mirror_topo.value().corners.size(); ++i) {
            if (mirror_topo.value().corners[i].vertex.value == 5) {
                auto uv = std::get<core::Vec2d>(mirror_out->values[i]);
                CHECK(std::abs(uv.x + 0.5) < 1e-12 && std::abs(uv.y - 0.5) < 1e-12);
            }
        }
        auto concave = fan(false, false, false, true);
        const auto prior = concave.revision();
        CHECK(!production::relax_uv_triangles(concave, 7, 8, "R", "uv.out", 1, 1));
        CHECK(concave.revision() == prior && !concave.uv_sets().find(8));
        CHECK(!production::relax_uv_triangles(mesh, 7, 8, "R", "uv.out", 0, 1));
        CHECK(!production::relax_uv_triangles(mesh, 7, 8, "R", "uv.out", 101, 1));
        CHECK(!production::relax_uv_triangles(mesh, 7, 8, "R", "uv.out", 1, 1.1));
        CHECK(!production::relax_uv_triangles(mesh, 7, 8, "R", "uv.out", 1,
                                              std::numeric_limits<double>::quiet_NaN()));
        CHECK(!production::relax_uv_triangles(mesh, 7, 7, "R", "uv.out", 1, 1));
        CHECK(!production::relax_uv_triangles(mesh, 7, 8, "R", "uv.source", 1, 1));
        CHECK(!production::relax_uv_triangles(mesh, 7, 8, "R", attributes::uv_seam_attribute_id(8),
                                              1, 1));
        CHECK(!mesh.attributes().find(attributes::uv_seam_attribute_id(8)));
        std::cout << "UV relaxation tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
