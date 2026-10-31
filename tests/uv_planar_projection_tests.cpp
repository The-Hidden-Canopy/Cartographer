#include <carto/production/uv_planar_projection.hpp>
#include <carto/attributes/uv.hpp>

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

geometry::EditableMesh square(production::UvProjectionPlane plane) {
    geometry::EditableMesh mesh;
    auto position = [plane](double u, double v) {
        switch (plane) {
        case production::UvProjectionPlane::xy:
            return core::Vec3d{u, v, 3};
        case production::UvProjectionPlane::yz:
            return core::Vec3d{3, u, v};
        case production::UvProjectionPlane::zx:
            return core::Vec3d{v, 3, u};
        }
        return core::Vec3d{};
    };
    CHECK(mesh.insert_bulk(
        {{geometry::VertexId{1}, position(1, 2)},
         {geometry::VertexId{2}, position(3, 2)},
         {geometry::VertexId{3}, position(3, 4)},
         {geometry::VertexId{4}, position(1, 4)}},
        {{geometry::FaceId{1},
          {geometry::VertexId{1}, geometry::VertexId{2}, geometry::VertexId{3}}},
         {geometry::FaceId{2},
          {geometry::VertexId{1}, geometry::VertexId{3}, geometry::VertexId{4}}}}));
    return mesh;
}

production::UvPlanarProjection settings(production::UvProjectionPlane plane) {
    return {plane, {1, 2}, 0.5, {0.25, 0.5}, 8, "Projected", "uv.projected"};
}

int main() {
    try {
        for (auto plane : {production::UvProjectionPlane::xy, production::UvProjectionPlane::yz,
                           production::UvProjectionPlane::zx}) {
            auto source = square(plane);
            const auto before = source.revision();
            auto output = production::project_uv_planar(source, settings(plane));
            CHECK(output);
            auto repeat = production::project_uv_planar(source, settings(plane));
            CHECK(repeat);
            CHECK(source.revision() == before && source.uv_sets().find(8) == nullptr);
            CHECK(output.value().uv_sets().find(8) != nullptr);
            CHECK(output.value().uv_sets().active_for_editing() == nullptr);
            CHECK(output.value().uv_sets().active_for_render() == nullptr);
            const auto *layer = output.value().attributes().find("uv.projected");
            const auto *repeated = repeat.value().attributes().find("uv.projected");
            CHECK(layer && repeated);
            const auto topo = source.topology();
            CHECK(topo);
            for (std::size_t i = 0; i < topo.value().corners.size(); ++i) {
                const auto uv = std::get<core::Vec2d>(layer->values[i]);
                const auto again = std::get<core::Vec2d>(repeated->values[i]);
                CHECK(uv.x == again.x && uv.y == again.y);
                switch (topo.value().corners[i].vertex.value) {
                case 1:
                    CHECK(uv.x == 0.25 && uv.y == 0.5);
                    break;
                case 2:
                    CHECK(uv.x == 1.25 && uv.y == 0.5);
                    break;
                case 3:
                    CHECK(uv.x == 1.25 && uv.y == 1.5);
                    break;
                case 4:
                    CHECK(uv.x == 0.25 && uv.y == 1.5);
                    break;
                default:
                    CHECK(false);
                }
            }
        }
        auto source = square(production::UvProjectionPlane::xy);
        auto before = source.revision();
        auto initial =
            production::project_uv_planar(source, settings(production::UvProjectionPlane::xy));
        CHECK(initial);
        auto with_seam = initial.value();
        auto old_attributes = with_seam.attributes();
        const auto old_topology = with_seam.topology();
        CHECK(old_topology);
        CHECK(old_attributes.set_domain_count(attributes::AttributeDomain::edge,
                                              old_topology.value().edges.size()));
        std::vector<attributes::AttributeValue> flags(old_topology.value().edges.size(), false);
        bool marked = false;
        for (std::size_t i = 0; i < old_topology.value().edges.size(); ++i) {
            if (old_topology.value().edges[i].second_half_edge.has_value()) {
                flags[i] = true;
                marked = true;
            }
        }
        CHECK(marked);
        CHECK(old_attributes.add_layer(
            {{attributes::uv_seam_attribute_id(8), attributes::AttributeDomain::edge,
              attributes::AttributeType::boolean},
             std::move(flags)}));
        auto old_sets = with_seam.uv_sets();
        CHECK(old_sets.set_active_for_editing(8));
        CHECK(old_sets.set_active_for_render(8));
        CHECK(with_seam.set_attribute_payload(std::move(old_attributes), std::move(old_sets)));
        auto second = settings(production::UvProjectionPlane::xy);
        second.output_uv_set = 9;
        second.output_layer_id = "uv.second";
        auto retained = production::project_uv_planar(with_seam, second);
        CHECK(retained);
        CHECK(retained.value().attributes().find("uv.projected") != nullptr);
        CHECK(retained.value().uv_sets().find(8) != nullptr &&
              retained.value().uv_sets().find(9) != nullptr);
        const auto *original_uv = with_seam.attributes().find("uv.projected");
        const auto *retained_uv = retained.value().attributes().find("uv.projected");
        const auto *original_seam =
            with_seam.attributes().find(attributes::uv_seam_attribute_id(8));
        const auto *retained_seam =
            retained.value().attributes().find(attributes::uv_seam_attribute_id(8));
        CHECK(original_uv && retained_uv && original_seam && retained_seam);
        for (std::size_t i = 0; i < original_uv->values.size(); ++i) {
            auto a = std::get<core::Vec2d>(original_uv->values[i]);
            auto b = std::get<core::Vec2d>(retained_uv->values[i]);
            CHECK(a.x == b.x && a.y == b.y);
        }
        for (std::size_t i = 0; i < original_seam->values.size(); ++i)
            CHECK(std::get<bool>(original_seam->values[i]) ==
                  std::get<bool>(retained_seam->values[i]));
        CHECK(retained.value().attributes().find(attributes::uv_seam_attribute_id(9)) == nullptr);
        CHECK(retained.value().uv_sets().active_for_editing()->id == 8);
        CHECK(retained.value().uv_sets().active_for_render()->id == 8);
        CHECK(!production::project_uv_planar(initial.value(),
                                             settings(production::UvProjectionPlane::xy)));
        auto bad = settings(production::UvProjectionPlane::xy);
        bad.output_layer_id = attributes::uv_seam_attribute_id(8);
        CHECK(!production::project_uv_planar(source, bad));
        bad = settings(production::UvProjectionPlane::xy);
        bad.plane = static_cast<production::UvProjectionPlane>(99);
        CHECK(!production::project_uv_planar(source, bad));
        bad = settings(production::UvProjectionPlane::xy);
        bad.scale = 0;
        CHECK(!production::project_uv_planar(source, bad));
        bad.scale = std::numeric_limits<double>::quiet_NaN();
        CHECK(!production::project_uv_planar(source, bad));
        bad = settings(production::UvProjectionPlane::xy);
        bad.origin.x = std::numeric_limits<double>::infinity();
        CHECK(!production::project_uv_planar(source, bad));
        bad = settings(production::UvProjectionPlane::xy);
        bad.scale = std::numeric_limits<double>::max();
        CHECK(!production::project_uv_planar(source, bad));
        CHECK(!production::project_uv_planar(source, settings(production::UvProjectionPlane::yz)));
        CHECK(source.revision() == before && source.uv_sets().find(8) == nullptr);
        geometry::EditableMesh noncoplanar;
        CHECK(noncoplanar.insert_bulk(
            {{geometry::VertexId{1}, {0, 0, 0}},
             {geometry::VertexId{2}, {1, 0, 0}},
             {geometry::VertexId{3}, {1, 1, 1}},
             {geometry::VertexId{4}, {0, 1, 0}}},
            {{geometry::FaceId{1},
              {geometry::VertexId{1}, geometry::VertexId{2}, geometry::VertexId{3}}},
             {geometry::FaceId{2},
              {geometry::VertexId{1}, geometry::VertexId{3}, geometry::VertexId{4}}}}));
        CHECK(production::project_uv_planar(noncoplanar,
                                            settings(production::UvProjectionPlane::xy)));
        geometry::EditableMesh quad;
        CHECK(quad.insert_bulk({{geometry::VertexId{1}, {0, 0, 0}},
                                {geometry::VertexId{2}, {1, 0, 0}},
                                {geometry::VertexId{3}, {1, 1, 0}},
                                {geometry::VertexId{4}, {0, 1, 0}}},
                               {{geometry::FaceId{1},
                                 {geometry::VertexId{1}, geometry::VertexId{2},
                                  geometry::VertexId{3}, geometry::VertexId{4}}}}));
        CHECK(!production::project_uv_planar(quad, settings(production::UvProjectionPlane::xy)));
        std::cout << "UV planar projection tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
