#include <carto/geometry/mesh.hpp>
#include <carto/surface/spectrum.hpp>

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
    constexpr int grid = 20;
    carto::geometry::EditableMesh mesh;
    std::vector<carto::geometry::VertexId> vertices;
    vertices.reserve(static_cast<std::size_t>(grid * grid));
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(grid - 1);
            const double fy = static_cast<double>(y) / static_cast<double>(grid - 1);
            const auto vertex = mesh.add_vertex({fx, fy, 0.08 * std::sin(fx * 6.0) * std::cos(fy * 5.0)});
            if (!vertex) return 1;
            vertices.push_back(vertex.value());
        }
    }
    for (int y = 0; y < grid - 1; ++y) {
        for (int x = 0; x < grid - 1; ++x) {
            const auto at = [=](int px, int py) {
                return vertices[static_cast<std::size_t>(py * grid + px)];
            };
            if (!mesh.add_face({at(x, y), at(x + 1, y), at(x + 1, y + 1)})) return 1;
            if (!mesh.add_face({at(x, y), at(x + 1, y + 1), at(x, y + 1)})) return 1;
        }
    }
    const auto compiled = mesh.compile();
    if (!compiled) return 1;
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{9001U}, "benchmark", "synthetic");
    if (!domain) return 1;

    std::vector<double> signal;
    signal.reserve(compiled.value().positions.size());
    for (const auto& position : compiled.value().positions) {
        signal.push_back(std::sin(position.x * 12.0) + 0.5 * std::cos(position.y * 9.0));
    }
    carto::surface::SurfaceSpectrumOptions options;
    options.max_vertices = 512U;
    options.max_modes = 12U;
    options.max_jacobi_sweeps = 48U;

    const auto start = std::chrono::steady_clock::now();
    const auto result = carto::surface::build_surface_spectrum(
        compiled.value(), domain.value(), signal, options);
    const auto stop = std::chrono::steady_clock::now();
    if (!result) return 1;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(stop - start).count();
    std::cout << std::fixed << std::setprecision(4)
              << "stage,vertices,modes,elapsed_ms,digest\n"
              << "surface_spectrum," << result.value().vertex_ids.size() << ','
              << result.value().modes.size() << ',' << elapsed_ms << ','
              << result.value().digest << '\n';
    return 0;
}
