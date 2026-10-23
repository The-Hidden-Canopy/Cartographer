#include <carto/geometry/mesh.hpp>
#include <carto/surface/visibility_closure.hpp>

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
        compiled.value(), carto::surface::SurfaceDomainId{9003U}, "benchmark", "synthetic");
    if (!domain) return 1;
    carto::surface::SurfaceQemOptions qem_options;
    qem_options.target_triangles = 448U;
    qem_options.max_collapses = 4U;
    const auto candidate = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), qem_options);
    if (!candidate) return 1;

    carto::surface::SurfaceVisibilityClosureOptions options;
    options.visibility_bins = 16U;
    options.direction_count = 8U;
    options.max_distance = 100.0;
    options.distribution_metrics.max_bins = 16U;
    options.distribution_metrics.sinkhorn_iterations = 32U;
    const auto start = std::chrono::steady_clock::now();
    const auto result = carto::surface::evaluate_surface_visibility_closure(
        compiled.value(), domain.value(), candidate.value(), options);
    const auto stop = std::chrono::steady_clock::now();
    if (!result) return 1;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(stop - start).count();
    std::cout << std::fixed << std::setprecision(4)
              << "stage,bins,directions,source_average,candidate_average,average_error,js_divergence,elapsed_ms,digest\n"
              << "surface_visibility_closure," << options.visibility_bins << ','
              << options.direction_count << ',' << result.value().source_average_visibility << ','
              << result.value().candidate_average_visibility << ','
              << result.value().average_visibility_error << ','
              << result.value().distribution_metrics.jensen_shannon_divergence << ','
              << elapsed_ms << ',' << result.value().digest << '\n';
    return 0;
}
