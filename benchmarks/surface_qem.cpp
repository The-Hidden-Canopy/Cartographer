#include <carto/geometry/mesh.hpp>
#include <carto/surface/qem.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
    constexpr int side = 16;
    carto::geometry::EditableMesh mesh;
    std::vector<carto::geometry::VertexId> vertices;
    vertices.reserve(static_cast<std::size_t>(side * side));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const auto vertex = mesh.add_vertex({static_cast<double>(x), static_cast<double>(y),
                                                 0.03 * static_cast<double>((x + y) % 4)});
            if (!vertex) return 1;
            vertices.push_back(vertex.value());
        }
    }
    for (int y = 0; y < side - 1; ++y) {
        for (int x = 0; x < side - 1; ++x) {
            const auto at = [=](int px, int py) {
                return vertices[static_cast<std::size_t>(py * side + px)];
            };
            if (!mesh.add_face({at(x, y), at(x + 1, y), at(x + 1, y + 1)})) return 1;
            if (!mesh.add_face({at(x, y), at(x + 1, y + 1), at(x, y + 1)})) return 1;
        }
    }
    const auto compiled = mesh.compile();
    if (!compiled) return 1;
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{7001U}, "benchmark", "synthetic");
    if (!domain) return 1;
    carto::surface::SurfaceQemOptions options;
    options.target_triangles = 448U;
    options.max_collapses = 4U;

    const auto start = std::chrono::steady_clock::now();
    const auto result = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), options);
    const auto stop = std::chrono::steady_clock::now();
    if (!result) return 1;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(stop - start).count();
    std::cout << std::fixed << std::setprecision(4)
              << "stage,source_triangles,target_triangles,collapses,elapsed_ms,digest\n"
              << "surface_qem," << result.value().source_triangles << ','
              << result.value().target_triangles << ',' << result.value().collapses.size() << ','
              << elapsed_ms << ',' << result.value().digest << '\n';
    return 0;
}
