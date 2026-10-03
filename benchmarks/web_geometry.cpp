#include <carto/geometry/mesh.hpp>
#include <carto/web_geometry/compiler.hpp>
#include <carto/web_geometry/package.hpp>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using carto::core::Vec3d;
using carto::geometry::EditableMesh;

EditableMesh make_grid(std::size_t rows, std::size_t columns) {
    EditableMesh mesh;
    std::vector<carto::geometry::VertexId> vertices((rows + 1U) * (columns + 1U));
    for (std::size_t row = 0U; row <= rows; ++row) {
        for (std::size_t column = 0U; column <= columns; ++column) {
            const auto vertex = mesh.add_vertex(
                Vec3d{static_cast<double>(column), static_cast<double>(row), 0.0});
            if (!vertex) throw std::runtime_error(vertex.error().message);
            vertices[row * (columns + 1U) + column] = vertex.value();
        }
    }
    for (std::size_t row = 0U; row < rows; ++row) {
        for (std::size_t column = 0U; column < columns; ++column) {
            const std::size_t stride = columns + 1U;
            const auto lower_left = vertices[row * stride + column];
            const auto lower_right = vertices[row * stride + column + 1U];
            const auto upper_right = vertices[(row + 1U) * stride + column + 1U];
            const auto upper_left = vertices[(row + 1U) * stride + column];
            if (auto face = mesh.add_face(
                    {lower_left, lower_right, upper_right, upper_left});
                !face) {
                throw std::runtime_error(face.error().message);
            }
        }
    }
    return mesh;
}

template <typename Function>
double measure_microseconds(Function&& function, std::size_t iterations) {
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        function();
    }
    const auto finished = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(finished - started).count() /
        static_cast<double>(iterations);
}

} // namespace

int main() {
    try {
        constexpr std::size_t kRows = 32U;
        constexpr std::size_t kColumns = 32U;
        constexpr std::size_t kIterations = 10U;
        const auto editable = make_grid(kRows, kColumns);
        const auto compiled = editable.compile();
        if (!compiled) throw std::runtime_error(compiled.error().message);
        const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
            compiled.value());
        if (!provenance) throw std::runtime_error(provenance.error().message);

        carto::web_geometry::WebGeometryPackage package;
        const auto compile_once = [&]() {
            const auto result = carto::web_geometry::compile_web_geometry(
                compiled.value(), {}, {}, provenance.value());
            if (!result) throw std::runtime_error(result.error().message);
            package = result.value();
        };
        compile_once();
        const auto compile_mean_us = measure_microseconds(compile_once, kIterations);
        const auto serialize_mean_us = measure_microseconds([&]() {
            const auto result = carto::web_geometry::serialize_package(package);
            if (!result) throw std::runtime_error(result.error().message);
        }, kIterations);
        if (auto result = package.validate(); !result) {
            throw std::runtime_error(result.error().message);
        }
        std::cout << std::fixed << std::setprecision(3)
                  << "fixture,triangles,clusters,pages,payload_bytes,compile_mean_us,"
                     "serialize_mean_us,package_digest\n"
                  << "grid_32x32," << compiled.value().triangle_faces.size() << ','
                  << package.hierarchy.clusters.size() << ',' << package.pages.size() << ','
                  << package.payload.size() << ',' << compile_mean_us << ','
                  << serialize_mean_us << ',' << package.digest.hex() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "web geometry benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
