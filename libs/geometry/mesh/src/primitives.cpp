#include <carto/geometry/primitives.hpp>

#include <string>
#include <utility>
#include <vector>

namespace carto::geometry {

namespace {

using core::Diagnostic;
using core::ErrorCode;

core::Result<void> require_positive_dimensions(core::Vec3d dimensions, const char* label) {
    if (!dimensions.finite() || dimensions.x <= 0.0 || dimensions.y <= 0.0 ||
        dimensions.z <= 0.0) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_argument,
            std::string(label) + " dimensions must be finite and strictly positive"));
    }
    return core::Result<void>::success();
}

core::Result<EditableMesh> build_mesh(
    const std::vector<core::Vec3d>& positions,
    const std::vector<std::vector<std::size_t>>& faces) {
    EditableMesh mesh;
    std::vector<VertexId> vertices;
    vertices.reserve(positions.size());
    for (const auto position : positions) {
        auto added = mesh.add_vertex(position);
        if (!added) {
            return core::Result<EditableMesh>::failure(added.error());
        }
        vertices.push_back(added.value());
    }
    for (const auto& face : faces) {
        std::vector<VertexId> ids;
        ids.reserve(face.size());
        for (const std::size_t index : face) {
            if (index >= vertices.size()) {
                return core::Result<EditableMesh>::failure(Diagnostic(
                    ErrorCode::invalid_state,
                    "primitive face references an out-of-range generated vertex"));
            }
            ids.push_back(vertices[index]);
        }
        auto added = mesh.add_face(std::move(ids));
        if (!added) {
            return core::Result<EditableMesh>::failure(
                added.error().with_context("generated primitive face"));
        }
    }
    return core::Result<EditableMesh>::success(std::move(mesh));
}

} // namespace

core::Result<EditableMesh> make_box(core::Vec3d size) {
    if (auto result = require_positive_dimensions(size, "box"); !result) {
        return core::Result<EditableMesh>::failure(result.error());
    }
    const core::Vec3d half{size.x * 0.5, size.y * 0.5, size.z * 0.5};
    const std::vector<core::Vec3d> positions = {
        {-half.x, -half.y, -half.z},
        {half.x, -half.y, -half.z},
        {half.x, half.y, -half.z},
        {-half.x, half.y, -half.z},
        {-half.x, -half.y, half.z},
        {half.x, -half.y, half.z},
        {half.x, half.y, half.z},
        {-half.x, half.y, half.z},
    };
    const std::vector<std::vector<std::size_t>> faces = {
        {0, 3, 2, 1},
        {4, 5, 6, 7},
        {0, 1, 5, 4},
        {3, 7, 6, 2},
        {0, 4, 7, 3},
        {1, 2, 6, 5},
    };
    return build_mesh(positions, faces);
}

core::Result<EditableMesh> make_plane(double width, double depth) {
    if (auto result = require_positive_dimensions({width, 1.0, depth}, "plane"); !result) {
        return core::Result<EditableMesh>::failure(result.error());
    }
    const double half_width = width * 0.5;
    const double half_depth = depth * 0.5;
    const std::vector<core::Vec3d> positions = {
        {-half_width, 0.0, -half_depth},
        {half_width, 0.0, -half_depth},
        {half_width, 0.0, half_depth},
        {-half_width, 0.0, half_depth},
    };
    // Viewed from +Y, this winding produces an upward normal.
    const std::vector<std::vector<std::size_t>> faces = {{0, 3, 2, 1}};
    return build_mesh(positions, faces);
}

} // namespace carto::geometry
