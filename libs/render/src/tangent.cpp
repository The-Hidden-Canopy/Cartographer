#include <carto/render/tangent.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace carto::render {

namespace {

core::Result<std::vector<core::Vec4d>> invalid(std::string message) {
    return core::Result<std::vector<core::Vec4d>>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

core::Result<std::vector<core::Vec4d>> validation(std::string message) {
    return core::Result<std::vector<core::Vec4d>>::failure(core::Diagnostic(
        core::ErrorCode::validation_failed, std::move(message)));
}

} // namespace

core::Result<std::vector<core::Vec4d>> generate_tangents(
    std::span<const TangentVertex> vertices,
    std::span<const std::uint32_t> indices) {
    if (vertices.empty() || indices.empty() || indices.size() % 3U != 0U) {
        return invalid("tangent generation requires indexed triangle vertices");
    }
    for (const TangentVertex& vertex : vertices) {
        if (!vertex.position.finite() || !vertex.normal.finite() || !vertex.uv.finite()) {
            return invalid("tangent generation inputs must be finite");
        }
        if (!vertex.normal.normalized().finite()) {
            return validation("tangent generation requires non-degenerate normals");
        }
    }

    std::vector<core::Vec3d> tangent_sum(vertices.size());
    std::vector<core::Vec3d> bitangent_sum(vertices.size());
    for (std::size_t index = 0U; index < indices.size(); index += 3U) {
        const std::uint32_t ia = indices[index];
        const std::uint32_t ib = indices[index + 1U];
        const std::uint32_t ic = indices[index + 2U];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size() ||
            ia == ib || ia == ic || ib == ic) {
            return invalid("tangent generation index is outside a valid triangle");
        }
        const TangentVertex& a = vertices[ia];
        const TangentVertex& b = vertices[ib];
        const TangentVertex& c = vertices[ic];
        const core::Vec3d edge_one = b.position - a.position;
        const core::Vec3d edge_two = c.position - a.position;
        const core::Vec2d uv_one{b.uv.x - a.uv.x, b.uv.y - a.uv.y};
        const core::Vec2d uv_two{c.uv.x - a.uv.x, c.uv.y - a.uv.y};
        const double determinant = uv_one.x * uv_two.y - uv_one.y * uv_two.x;
        if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-12) {
            return validation("tangent generation rejects degenerate UV triangles");
        }
        const double reciprocal = 1.0 / determinant;
        const core::Vec3d tangent =
            (edge_one * uv_two.y - edge_two * uv_one.y) * reciprocal;
        const core::Vec3d bitangent =
            (edge_two * uv_one.x - edge_one * uv_two.x) * reciprocal;
        if (!tangent.finite() || !bitangent.finite()) {
            return validation("tangent generation produced non-finite frame data");
        }
        tangent_sum[ia] = tangent_sum[ia] + tangent;
        tangent_sum[ib] = tangent_sum[ib] + tangent;
        tangent_sum[ic] = tangent_sum[ic] + tangent;
        bitangent_sum[ia] = bitangent_sum[ia] + bitangent;
        bitangent_sum[ib] = bitangent_sum[ib] + bitangent;
        bitangent_sum[ic] = bitangent_sum[ic] + bitangent;
    }

    std::vector<core::Vec4d> result;
    result.reserve(vertices.size());
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        const core::Vec3d normal = vertices[index].normal.normalized();
        const core::Vec3d tangent =
            (tangent_sum[index] - normal * core::dot(normal, tangent_sum[index])).normalized();
        if (!normal.finite() || !tangent.finite() || !bitangent_sum[index].finite()) {
            return validation("tangent generation produced a degenerate frame");
        }
        const double handedness = core::dot(core::cross(normal, tangent), bitangent_sum[index]);
        if (!std::isfinite(handedness) || std::abs(handedness) <= 1e-12) {
            return validation("tangent generation produced an ambiguous handedness");
        }
        result.push_back({tangent.x, tangent.y, tangent.z, handedness < 0.0 ? -1.0 : 1.0});
    }
    return core::Result<std::vector<core::Vec4d>>::success(std::move(result));
}

} // namespace carto::render
