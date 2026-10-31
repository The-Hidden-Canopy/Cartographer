#include <carto/production/uv_relax.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::production {
namespace {
using geometry::FaceId;
using geometry::VertexId;
using Edge = std::pair<VertexId, VertexId>;
struct Use {
    std::size_t a;
    std::size_t b;
};
core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}
double area(core::Vec2d a, core::Vec2d b, core::Vec2d c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
bool same(core::Vec2d a, core::Vec2d b) { return a.x == b.x && a.y == b.y; }
} // namespace

core::Result<geometry::EditableMesh>
relax_uv_triangles(const geometry::EditableMesh &source, attributes::UvSetId source_uv_set,
                   attributes::UvSetId output_uv_set, std::string output_name,
                   std::string output_layer_id, std::uint32_t iterations, double strength) {
    using Result = core::Result<geometry::EditableMesh>;
    auto fail = [](std::string message) { return Result::failure(invalid(std::move(message))); };
    if (iterations == 0 || iterations > 100 || !std::isfinite(strength) || strength <= 0 ||
        strength > 1 || output_uv_set == 0 || output_uv_set == source_uv_set ||
        output_name.empty() || output_layer_id.empty() ||
        output_layer_id.starts_with("cartographer.uv_seam.") ||
        source.uv_sets().find(output_uv_set) || source.attributes().find(output_layer_id) ||
        source.attributes().find(attributes::uv_seam_attribute_id(output_uv_set)))
        return fail("invalid UV relaxation settings or output identity collision");
    if (auto valid = source.validate(); !valid)
        return Result::failure(valid.error());
    const auto topology = source.topology();
    if (!topology)
        return Result::failure(topology.error());
    const auto &corners = topology.value().corners;
    if (corners.empty() || corners.size() > 100000 ||
        corners.size() * static_cast<std::uint64_t>(iterations) > 2000000)
        return fail("UV relaxation work exceeds its supported bound");
    const auto *set = source.uv_sets().find(source_uv_set);
    const auto *layer = set ? source.attributes().find(set->layer_id) : nullptr;
    if (!layer || layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2 ||
        layer->values.size() != corners.size())
        return fail("UV relaxation requires a complete corner-domain vec2 set");
    const auto *seams = source.attributes().find(attributes::uv_seam_attribute_id(source_uv_set));
    if (seams && (seams->descriptor.domain != attributes::AttributeDomain::edge ||
                  seams->descriptor.type != attributes::AttributeType::boolean ||
                  seams->values.size() != topology.value().edges.size()))
        return fail("UV relaxation seam layer does not match edge topology");
    if (seams) {
        if (auto valid = seams->validate(topology.value().edges.size()); !valid)
            return Result::failure(valid.error());
    }
    std::map<std::pair<FaceId, VertexId>, std::size_t> lookup;
    std::vector<core::Vec2d> uv;
    uv.reserve(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto *value = std::get_if<core::Vec2d>(&layer->values[i]);
        if (!value || !value->finite() ||
            !lookup.emplace(std::make_pair(corners[i].face, corners[i].vertex), i).second)
            return fail("invalid or ambiguous UV corner");
        uv.push_back(*value);
    }
    std::map<Edge, std::vector<Use>> uses;
    std::vector<std::array<std::size_t, 3>> triangles;
    std::vector<double> original_area;
    for (const auto &face : source.faces_sorted()) {
        if (face.vertices.size() != 3)
            return fail("UV relaxation supports triangles only");
        std::array<std::size_t, 3> tri{};
        for (std::size_t j = 0; j < 3; ++j) {
            auto it = lookup.find({face.id, face.vertices[j]});
            if (it == lookup.end())
                return fail("UV relaxation face corner is missing");
            tri[j] = it->second;
        }
        const double signed_area = area(uv[tri[0]], uv[tri[1]], uv[tri[2]]);
        if (!std::isfinite(signed_area) || std::abs(signed_area) <= 1e-14)
            return fail("UV relaxation rejects degenerate source UV triangles");
        const auto *a = source.find_vertex(face.vertices[0]);
        const auto *b = source.find_vertex(face.vertices[1]);
        const auto *c = source.find_vertex(face.vertices[2]);
        if (!a || !b || !c)
            return fail("UV relaxation face vertex is missing");
        const auto x = b->position.x - a->position.x, y = b->position.y - a->position.y,
                   z = b->position.z - a->position.z;
        const auto u = c->position.x - a->position.x, v = c->position.y - a->position.y,
                   w = c->position.z - a->position.z;
        const double cx = y * w - z * v, cy = z * u - x * w, cz = x * v - y * u;
        const double world_area2 = cx * cx + cy * cy + cz * cz;
        if (!std::isfinite(world_area2) || world_area2 <= 1e-28)
            return fail("UV relaxation rejects degenerate world triangles");
        triangles.push_back(tri);
        original_area.push_back(signed_area);
        for (std::size_t j = 0; j < 3; ++j) {
            const auto from = face.vertices[j], to = face.vertices[(j + 1) % 3];
            const bool forward = from < to;
            uses[forward ? Edge{from, to} : Edge{to, from}].push_back(
                forward ? Use{tri[j], tri[(j + 1) % 3]} : Use{tri[(j + 1) % 3], tri[j]});
        }
    }
    if (triangles.empty())
        return fail("UV relaxation requires triangles");
    std::set<Edge> marked;
    if (seams)
        for (std::size_t i = 0; i < topology.value().edges.size(); ++i) {
            const auto *value = std::get_if<bool>(&seams->values[i]);
            if (!value)
                return fail("invalid seam value");
            if (*value)
                marked.emplace(topology.value().edges[i].first, topology.value().edges[i].second);
        }
    std::vector<std::size_t> parent(corners.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&parent](std::size_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto join = [&parent, &root](std::size_t a, std::size_t b) {
        a = root(a);
        b = root(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    };
    std::vector<Use> boundary;
    for (const auto &[edge, list] : uses) {
        if (list.size() > 2)
            return fail("UV relaxation rejects non-manifold edge fans");
        if (list.size() == 2 && !marked.contains(edge) && same(uv[list[0].a], uv[list[1].a]) &&
            same(uv[list[0].b], uv[list[1].b])) {
            join(list[0].a, list[1].a);
            join(list[0].b, list[1].b);
        } else
            for (const auto &use : list)
                boundary.push_back(use);
    }
    std::vector<bool> fixed(corners.size(), false);
    for (const auto &edge : boundary) {
        fixed[root(edge.a)] = true;
        fixed[root(edge.b)] = true;
    }
    std::vector<std::set<std::size_t>> neighbors(corners.size());
    for (const auto &tri : triangles)
        for (std::size_t j = 0; j < 3; ++j) {
            auto a = root(tri[j]), b = root(tri[(j + 1) % 3]);
            if (a == b)
                return fail("UV relaxation encountered a collapsed chart edge");
            neighbors[a].insert(b);
            neighbors[b].insert(a);
        }
    std::vector<core::Vec2d> current = uv, next = uv;
    for (std::uint32_t step = 0; step < iterations; ++step) {
        for (std::size_t i = 0; i < corners.size(); ++i)
            if (root(i) == i && !fixed[i]) {
                if (neighbors[i].empty())
                    return fail("UV relaxation encountered an isolated node");
                double x = 0, y = 0;
                for (auto n : neighbors[i]) {
                    x += current[n].x;
                    y += current[n].y;
                }
                const double count = static_cast<double>(neighbors[i].size());
                next[i] = {current[i].x * (1 - strength) + strength * x / count,
                           current[i].y * (1 - strength) + strength * y / count};
                if (!next[i].finite())
                    return fail("UV relaxation generated a nonfinite coordinate");
            }
        for (std::size_t i = 0; i < corners.size(); ++i)
            next[i] = next[root(i)];
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            const auto &t = triangles[i];
            const double a = area(next[t[0]], next[t[1]], next[t[2]]);
            if (!std::isfinite(a) || std::abs(a) <= 1e-14 || ((a > 0) != (original_area[i] > 0)))
                return fail("UV relaxation would degenerate or flip a triangle");
        }
        current = next;
    }
    auto attrs = source.attributes();
    auto sets = source.uv_sets();
    std::vector<attributes::AttributeValue> values;
    values.reserve(current.size());
    for (const auto &p : current)
        values.emplace_back(p);
    if (auto result = attrs.add_layer({{output_layer_id, attributes::AttributeDomain::corner,
                                        attributes::AttributeType::vec2},
                                       std::move(values)});
        !result)
        return Result::failure(result.error());
    if (seams) {
        auto copy = *seams;
        copy.descriptor.id = attributes::uv_seam_attribute_id(output_uv_set);
        if (auto result = attrs.add_layer(std::move(copy)); !result)
            return Result::failure(result.error());
    }
    if (auto result = sets.add({output_uv_set, std::move(output_name), std::move(output_layer_id),
                                false, false, false, set->tile_policy_version});
        !result)
        return Result::failure(result.error());
    auto output = source;
    if (auto result = output.set_attribute_payload(std::move(attrs), std::move(sets)); !result)
        return Result::failure(result.error());
    return Result::success(std::move(output));
}
} // namespace carto::production
