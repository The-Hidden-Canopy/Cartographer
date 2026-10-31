#include <carto/production/uv_planar_projection.hpp>

#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace carto::production {
namespace {
core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}
core::Vec2d plane_coordinate(core::Vec3d point, UvProjectionPlane plane) {
    switch (plane) {
    case UvProjectionPlane::xy:
        return {point.x, point.y};
    case UvProjectionPlane::yz:
        return {point.y, point.z};
    case UvProjectionPlane::zx:
        return {point.z, point.x};
    }
    return {};
}
double signed_area(core::Vec2d a, core::Vec2d b, core::Vec2d c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
} // namespace

core::Result<geometry::EditableMesh> project_uv_planar(const geometry::EditableMesh &source,
                                                       const UvPlanarProjection &settings) {
    using Result = core::Result<geometry::EditableMesh>;
    auto fail = [](std::string message) { return Result::failure(invalid(std::move(message))); };
    if (static_cast<std::uint32_t>(settings.plane) >
            static_cast<std::uint32_t>(UvProjectionPlane::zx) ||
        !settings.origin.finite() || !settings.offset.finite() || !std::isfinite(settings.scale) ||
        settings.scale <= 0.0 || settings.output_uv_set == 0U || settings.output_name.empty() ||
        settings.output_layer_id.empty() ||
        settings.output_layer_id.starts_with("cartographer.uv_seam.") ||
        source.uv_sets().find(settings.output_uv_set) ||
        source.attributes().find(settings.output_layer_id) ||
        source.attributes().find(attributes::uv_seam_attribute_id(settings.output_uv_set)))
        return fail("invalid UV projection settings or output identity collision");
    if (auto valid = source.validate(); !valid)
        return Result::failure(valid.error());
    const auto topology = source.topology();
    if (!topology)
        return Result::failure(topology.error());
    if (topology.value().corners.empty() || topology.value().corners.size() > 100000U)
        return fail("UV projection corner count is outside its supported bound");

    std::map<geometry::VertexId, core::Vec2d> projected;
    for (const auto &corner : topology.value().corners) {
        if (projected.contains(corner.vertex))
            continue;
        const auto *vertex = source.find_vertex(corner.vertex);
        if (!vertex || !vertex->position.finite())
            return fail("UV projection source vertex is invalid");
        const auto p = plane_coordinate(vertex->position, settings.plane);
        const core::Vec2d uv{(p.x - settings.origin.x) * settings.scale + settings.offset.x,
                             (p.y - settings.origin.y) * settings.scale + settings.offset.y};
        if (!uv.finite())
            return fail("UV projection generated a nonfinite coordinate");
        projected.emplace(corner.vertex, uv);
    }
    const auto faces = source.faces_sorted();
    if (faces.empty())
        return fail("UV projection requires triangles");
    for (const auto &face : faces) {
        if (face.vertices.size() != 3U)
            return fail("UV projection supports triangles only");
        const auto a = projected.find(face.vertices[0]), b = projected.find(face.vertices[1]),
                   c = projected.find(face.vertices[2]);
        if (a == projected.end() || b == projected.end() || c == projected.end())
            return fail("UV projection face vertex is missing");
        const double area = signed_area(a->second, b->second, c->second);
        if (!std::isfinite(area) || std::abs(area) <= 1e-14)
            return fail("UV projection would produce a degenerate triangle");
    }
    std::vector<attributes::AttributeValue> values;
    values.reserve(topology.value().corners.size());
    for (const auto &corner : topology.value().corners)
        values.emplace_back(projected.at(corner.vertex));
    auto attrs = source.attributes();
    auto sets = source.uv_sets();
    if (auto result = attrs.set_domain_count(attributes::AttributeDomain::corner, values.size());
        !result)
        return Result::failure(result.error());
    if (auto result =
            attrs.add_layer({{settings.output_layer_id, attributes::AttributeDomain::corner,
                              attributes::AttributeType::vec2},
                             std::move(values)});
        !result)
        return Result::failure(result.error());
    if (auto result = sets.add({settings.output_uv_set, settings.output_name,
                                settings.output_layer_id, false, false, false, 1U});
        !result)
        return Result::failure(result.error());
    auto output = source;
    if (auto result = output.set_attribute_payload(std::move(attrs), std::move(sets)); !result)
        return Result::failure(result.error());
    return Result::success(std::move(output));
}
} // namespace carto::production
