#include <carto/geometry/mesh.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using carto::core::Vec3d;
using carto::geometry::EditableMesh;
using carto::geometry::EdgeId;
using carto::geometry::FaceId;
using carto::geometry::VertexId;

constexpr std::size_t kWarmupIterations = 7U;
constexpr double kExtrudeDistance = 0.05;
constexpr double kInsetDistance = 0.05;
constexpr double kSplitFactor = 0.5;
constexpr double kSlideFactor = 0.25;

struct SizeCase {
    std::size_t rows;
    std::size_t columns;
    std::size_t iterations;
};

struct Fixture {
    EditableMesh mesh;
    FaceId face;
    VertexId vertex;
    EdgeId edge;
    EdgeId support_edge;
    bool has_interior_edge = false;
};

struct TimingSummary {
    double minimum_microseconds = 0.0;
    double median_microseconds = 0.0;
    double p95_microseconds = 0.0;
    double mean_microseconds = 0.0;
    std::size_t checksum = 0U;
};

struct KernelSpec {
    std::string_view name;
    bool (*run)(EditableMesh&, const Fixture&);
    bool requires_interior_edge = false;
};

[[nodiscard]] EditableMesh make_grid(std::size_t rows, std::size_t columns) {
    EditableMesh mesh;
    std::vector<VertexId> vertices((rows + 1U) * (columns + 1U));
    for (std::size_t row = 0U; row <= rows; ++row) {
        for (std::size_t column = 0U; column <= columns; ++column) {
            const auto vertex = mesh.add_vertex(Vec3d{
                static_cast<double>(column), static_cast<double>(row), 0.0});
            if (!vertex) throw std::runtime_error(vertex.error().message);
            vertices[row * (columns + 1U) + column] = vertex.value();
        }
    }

    for (std::size_t row = 0U; row < rows; ++row) {
        for (std::size_t column = 0U; column < columns; ++column) {
            const std::size_t stride = columns + 1U;
            const VertexId lower_left = vertices[row * stride + column];
            const VertexId lower_right = vertices[row * stride + column + 1U];
            const VertexId upper_right = vertices[(row + 1U) * stride + column + 1U];
            const VertexId upper_left = vertices[(row + 1U) * stride + column];
            if (auto face = mesh.add_face({lower_left, lower_right, upper_right, upper_left});
                !face) {
                throw std::runtime_error(face.error().message);
            }
        }
    }
    return mesh;
}

[[nodiscard]] Fixture make_fixture(const SizeCase& size) {
    Fixture fixture{make_grid(size.rows, size.columns), {}, {}, {}, {}, false};
    const auto faces = fixture.mesh.faces_sorted();
    const auto vertices = fixture.mesh.vertices_sorted();
    const auto topology = fixture.mesh.topology();
    if (!topology || faces.empty() || vertices.empty() || topology.value().edges.empty()) {
        throw std::runtime_error("benchmark fixture did not produce mesh topology");
    }

    fixture.face = faces.front().id;
    fixture.vertex = vertices.front().id;
    const auto support = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [vertex = fixture.vertex](const auto& edge) {
            return edge.first == vertex || edge.second == vertex;
        });
    if (support == topology.value().edges.end()) {
        throw std::runtime_error("benchmark fixture has no vertex support edge");
    }
    fixture.support_edge = support->id;
    const auto interior = std::find_if(
        topology.value().edges.begin(), topology.value().edges.end(),
        [](const auto& edge) { return edge.second_half_edge.has_value(); });
    fixture.edge = interior == topology.value().edges.end()
        ? topology.value().edges.front().id
        : interior->id;
    fixture.has_interior_edge = interior != topology.value().edges.end();
    if (auto result = fixture.mesh.validate(); !result) {
        throw std::runtime_error(result.error().message);
    }
    return fixture;
}

bool run_extrude(EditableMesh& mesh, const Fixture& fixture) {
    return static_cast<bool>(mesh.extrude_face(fixture.face, kExtrudeDistance));
}

bool run_inset(EditableMesh& mesh, const Fixture& fixture) {
    return static_cast<bool>(mesh.inset_face(fixture.face, kInsetDistance));
}

bool run_vertex_position(EditableMesh& mesh, const Fixture& fixture) {
    return static_cast<bool>(mesh.set_vertex_position(
        fixture.vertex, Vec3d{0.001, 0.002, 0.003}));
}

bool run_vertex_slide(EditableMesh& mesh, const Fixture& fixture) {
    return static_cast<bool>(mesh.slide_vertex(
        fixture.vertex, fixture.support_edge, kSlideFactor));
}

bool run_delete(EditableMesh& mesh, const Fixture& fixture) {
    return static_cast<bool>(mesh.delete_face(fixture.face, false));
}

bool run_split(EditableMesh& mesh, const Fixture& fixture) {
    return static_cast<bool>(mesh.split_edge(fixture.edge, kSplitFactor));
}

bool run_dissolve(EditableMesh& mesh, const Fixture& fixture) {
    if (!fixture.has_interior_edge) return false;
    return static_cast<bool>(mesh.dissolve_edge(fixture.edge));
}

[[nodiscard]] TimingSummary benchmark(
    const Fixture& fixture,
    const KernelSpec& kernel,
    std::size_t iterations) {
    const std::size_t total = kWarmupIterations + iterations;
    std::vector<EditableMesh> workspaces;
    workspaces.reserve(total);
    for (std::size_t index = 0U; index < total; ++index) {
        workspaces.push_back(fixture.mesh);
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    std::size_t checksum = 0U;
    for (std::size_t index = 0U; index < total; ++index) {
        auto& workspace = workspaces[index];
        const auto started = std::chrono::steady_clock::now();
        const bool succeeded = kernel.run(workspace, fixture);
        const auto finished = std::chrono::steady_clock::now();
        if (!succeeded) {
            throw std::runtime_error(
                std::string(kernel.name) + " rejected its benchmark fixture");
        }
        if (auto result = workspace.validate(); !result) {
            throw std::runtime_error(
                std::string(kernel.name) + " produced invalid topology: " + result.error().message);
        }
        checksum += workspace.vertex_count() + workspace.face_count() +
            static_cast<std::size_t>(workspace.revision().value());
        if (index >= kWarmupIterations) {
            samples.push_back(std::chrono::duration<double, std::micro>(finished - started).count());
        }
    }

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double rank) {
        const double position = rank * static_cast<double>(samples.size() - 1U);
        const std::size_t lower = static_cast<std::size_t>(std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
        const double fraction = position - static_cast<double>(lower);
        return samples[lower] + (samples[upper] - samples[lower]) * fraction;
    };
    const double total_microseconds =
        std::accumulate(samples.begin(), samples.end(), 0.0);
    return TimingSummary{
        samples.front(), percentile(0.50), percentile(0.95),
        total_microseconds / static_cast<double>(samples.size()), checksum};
}

} // namespace

int main() {
    try {
        constexpr std::array<SizeCase, 4U> sizes = {
            SizeCase{1U, 1U, 2000U},
            SizeCase{4U, 4U, 1000U},
            SizeCase{8U, 8U, 250U},
            SizeCase{16U, 16U, 50U},
        };
        constexpr std::array<KernelSpec, 7U> kernels = {
            KernelSpec{"geometry.EditableMesh.extrude_face", run_extrude},
            KernelSpec{"geometry.EditableMesh.inset_face", run_inset},
            KernelSpec{"geometry.EditableMesh.set_vertex_position", run_vertex_position},
            KernelSpec{"geometry.EditableMesh.slide_vertex", run_vertex_slide},
            KernelSpec{"geometry.EditableMesh.delete_face", run_delete},
            KernelSpec{"geometry.EditableMesh.split_edge", run_split},
            KernelSpec{"geometry.EditableMesh.dissolve_edge", run_dissolve, true},
        };

        std::cout << "kernel,rows,columns,vertices,faces,iterations,min_us,median_us,p95_us,mean_us,checksum\n";
        std::cout << std::fixed << std::setprecision(3);
        for (const auto& size : sizes) {
            const Fixture fixture = make_fixture(size);
            for (const auto& kernel : kernels) {
                if (kernel.requires_interior_edge && !fixture.has_interior_edge) continue;
                const auto summary = benchmark(fixture, kernel, size.iterations);
                std::cout << kernel.name << ',' << size.rows << ',' << size.columns << ','
                    << fixture.mesh.vertex_count() << ',' << fixture.mesh.face_count() << ','
                    << size.iterations << ',' << summary.minimum_microseconds << ','
                    << summary.median_microseconds << ',' << summary.p95_microseconds << ','
                    << summary.mean_microseconds << ',' << summary.checksum << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
