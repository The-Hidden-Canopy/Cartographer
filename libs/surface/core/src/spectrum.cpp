#include <carto/surface/spectrum.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::surface {

namespace {

constexpr double kEpsilon = 1e-12;
constexpr double kValidationTolerance = 1e-7;
constexpr std::size_t kMaximumSpectrumVertices = 4096U;
constexpr std::size_t kMaximumSpectrumModes = 4096U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool valid_hex_digest(std::string_view value) {
    return value.size() == 64U && assets::Sha256Digest::from_hex(value).has_value();
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::string digest_hex(std::string_view value) {
    return digest_text(value).hex();
}

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

struct EdgeKey {
    std::uint32_t first = 0U;
    std::uint32_t second = 0U;

    [[nodiscard]] bool operator<(const EdgeKey& other) const noexcept {
        return first < other.first || (first == other.first && second < other.second);
    }
};

struct TriangleContribution {
    std::array<std::uint32_t, 3> vertices{};
    double area = 0.0;
};

double cotangent(core::Vec3d left, core::Vec3d right, double cross_length) {
    if (!std::isfinite(cross_length) || cross_length <= kEpsilon) return 0.0;
    const double value = core::dot(left, right) / cross_length;
    if (!std::isfinite(value)) return 0.0;
    // Clamping negative cotangents makes the authoring graph a non-negative
    // conductance graph.  That keeps the normalized operator PSD and makes
    // invalid/obtuse local geometry fail closed rather than emit fake modes.
    return std::max(0.0, value);
}

void add_edge(
    std::map<EdgeKey, double>& edges,
    std::uint32_t first,
    std::uint32_t second,
    double weight) {
    if (first == second || !finite_nonnegative(weight) || weight <= 0.0) return;
    if (first > second) std::swap(first, second);
    edges[EdgeKey{first, second}] += weight;
}

struct GraphData {
    std::vector<std::uint32_t> global_vertices;
    std::vector<double> masses;
    std::map<EdgeKey, double> edges;
};

void sort_graph_vertices(GraphData& graph) {
    std::vector<std::size_t> order(graph.global_vertices.size());
    std::iota(order.begin(), order.end(), 0U);
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return graph.global_vertices[left] < graph.global_vertices[right];
    });

    std::vector<std::uint32_t> old_to_new(order.size(), 0U);
    std::vector<std::uint32_t> sorted_vertices(order.size());
    std::vector<double> sorted_masses(order.size());
    for (std::size_t next = 0U; next < order.size(); ++next) {
        old_to_new[order[next]] = static_cast<std::uint32_t>(next);
        sorted_vertices[next] = graph.global_vertices[order[next]];
        sorted_masses[next] = graph.masses[order[next]];
    }

    std::map<EdgeKey, double> sorted_edges;
    for (const auto& [edge, weight] : graph.edges) {
        std::uint32_t first = old_to_new[edge.first];
        std::uint32_t second = old_to_new[edge.second];
        if (first > second) std::swap(first, second);
        sorted_edges[EdgeKey{first, second}] += weight;
    }
    graph.global_vertices = std::move(sorted_vertices);
    graph.masses = std::move(sorted_masses);
    graph.edges = std::move(sorted_edges);
}

core::Result<GraphData> build_graph(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceSpectrumOptions& options) {
    std::set<geometry::FaceId> domain_faces(domain.source_faces.begin(), domain.source_faces.end());
    std::vector<std::uint32_t> selected_triangles;
    selected_triangles.reserve(domain.source_faces.size());
    for (std::size_t triangle = 0U; triangle < mesh.triangle_faces.size(); ++triangle) {
        if (domain_faces.contains(mesh.triangle_faces[triangle])) {
            selected_triangles.push_back(static_cast<std::uint32_t>(triangle));
        }
    }
    if (selected_triangles.empty()) {
        return core::Result<GraphData>::failure(validation(
            "surface spectrum domain contains no source triangles"));
    }

    std::vector<std::int32_t> local_index(mesh.positions.size(), -1);
    GraphData result;
    std::vector<TriangleContribution> triangles;
    triangles.reserve(selected_triangles.size());
    for (const std::uint32_t triangle : selected_triangles) {
        const std::size_t offset = static_cast<std::size_t>(triangle) * 3U;
        const std::uint32_t a = mesh.indices[offset];
        const std::uint32_t b = mesh.indices[offset + 1U];
        const std::uint32_t c = mesh.indices[offset + 2U];
        const core::Vec3d ab = mesh.positions[b] - mesh.positions[a];
        const core::Vec3d ac = mesh.positions[c] - mesh.positions[a];
        const double cross_length = core::cross(ab, ac).length();
        const double area = cross_length * 0.5;
        if (!finite_positive(area)) continue;

        for (const std::uint32_t vertex : {a, b, c}) {
            if (local_index[vertex] < 0) {
                local_index[vertex] = static_cast<std::int32_t>(result.global_vertices.size());
                result.global_vertices.push_back(vertex);
                result.masses.push_back(0.0);
            }
        }
        triangles.push_back({{
            static_cast<std::uint32_t>(local_index[a]),
            static_cast<std::uint32_t>(local_index[b]),
            static_cast<std::uint32_t>(local_index[c]),
        }, area});
        result.masses[static_cast<std::size_t>(local_index[a])] += area / 3.0;
        result.masses[static_cast<std::size_t>(local_index[b])] += area / 3.0;
        result.masses[static_cast<std::size_t>(local_index[c])] += area / 3.0;

        const double cot_a = cotangent(
            mesh.positions[b] - mesh.positions[a],
            mesh.positions[c] - mesh.positions[a],
            cross_length);
        const double cot_b = cotangent(
            mesh.positions[a] - mesh.positions[b],
            mesh.positions[c] - mesh.positions[b],
            cross_length);
        const double cot_c = cotangent(
            mesh.positions[a] - mesh.positions[c],
            mesh.positions[b] - mesh.positions[c],
            cross_length);
        add_edge(result.edges, triangles.back().vertices[1U], triangles.back().vertices[2U], cot_a * 0.5);
        add_edge(result.edges, triangles.back().vertices[0U], triangles.back().vertices[2U], cot_b * 0.5);
        add_edge(result.edges, triangles.back().vertices[0U], triangles.back().vertices[1U], cot_c * 0.5);
    }

    if (triangles.empty() || result.global_vertices.empty()) {
        return core::Result<GraphData>::failure(validation(
            "surface spectrum domain contains only degenerate triangles"));
    }
    if (result.global_vertices.size() > options.max_vertices) {
        return core::Result<GraphData>::failure(unsupported(
            "surface spectrum vertex count exceeds the bounded CPU reference cap"));
    }
    sort_graph_vertices(result);
    for (const double mass : result.masses) {
        if (!finite_positive(mass) || mass <= options.minimum_mass) {
            return core::Result<GraphData>::failure(validation(
                "surface spectrum mass matrix contains an invalid or underflowed entry"));
        }
    }
    return core::Result<GraphData>::success(std::move(result));
}

std::vector<double> normalized_laplacian(const GraphData& graph) {
    const std::size_t count = graph.global_vertices.size();
    std::vector<double> matrix(count * count, 0.0);
    std::vector<double> degree(count, 0.0);
    for (const auto& [edge, weight] : graph.edges) {
        degree[edge.first] += weight;
        degree[edge.second] += weight;
        matrix[static_cast<std::size_t>(edge.first) * count + edge.second] -=
            weight / std::sqrt(graph.masses[edge.first] * graph.masses[edge.second]);
        matrix[static_cast<std::size_t>(edge.second) * count + edge.first] -=
            weight / std::sqrt(graph.masses[edge.first] * graph.masses[edge.second]);
    }
    for (std::size_t index = 0U; index < count; ++index) {
        matrix[index * count + index] = degree[index] / graph.masses[index];
    }
    return matrix;
}

struct EigenSystem {
    std::vector<double> values;
    std::vector<double> vectors; // column-major by logical mode: row * count + mode
};

EigenSystem jacobi_eigensystem(
    std::vector<double> matrix,
    std::size_t count,
    std::uint32_t max_sweeps,
    double tolerance) {
    std::vector<double> vectors(count * count, 0.0);
    for (std::size_t index = 0U; index < count; ++index) {
        vectors[index * count + index] = 1.0;
    }

    for (std::uint32_t sweep = 0U; sweep < max_sweeps; ++sweep) {
        std::size_t p = 0U;
        std::size_t q = 0U;
        double largest = 0.0;
        for (std::size_t row = 0U; row < count; ++row) {
            for (std::size_t column = row + 1U; column < count; ++column) {
                const double magnitude = std::abs(matrix[row * count + column]);
                if (magnitude > largest) {
                    largest = magnitude;
                    p = row;
                    q = column;
                }
            }
        }
        if (largest <= tolerance || count < 2U) break;

        const double app = matrix[p * count + p];
        const double aqq = matrix[q * count + q];
        const double apq = matrix[p * count + q];
        const double tau = (aqq - app) / (2.0 * apq);
        const double t = std::copysign(
            1.0 / (std::abs(tau) + std::sqrt(1.0 + tau * tau)), tau);
        const double c = 1.0 / std::sqrt(1.0 + t * t);
        const double s = t * c;
        matrix[p * count + p] = app - t * apq;
        matrix[q * count + q] = aqq + t * apq;
        matrix[p * count + q] = 0.0;
        matrix[q * count + p] = 0.0;
        for (std::size_t index = 0U; index < count; ++index) {
            if (index == p || index == q) continue;
            const double aip = matrix[index * count + p];
            const double aiq = matrix[index * count + q];
            matrix[index * count + p] = c * aip - s * aiq;
            matrix[p * count + index] = matrix[index * count + p];
            matrix[index * count + q] = s * aip + c * aiq;
            matrix[q * count + index] = matrix[index * count + q];
        }
        for (std::size_t index = 0U; index < count; ++index) {
            const double vip = vectors[index * count + p];
            const double viq = vectors[index * count + q];
            vectors[index * count + p] = c * vip - s * viq;
            vectors[index * count + q] = s * vip + c * viq;
        }
    }

    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), 0U);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const double left_value = matrix[left * count + left];
        const double right_value = matrix[right * count + right];
        if (left_value != right_value) return left_value < right_value;
        return left < right;
    });

    EigenSystem result;
    result.values.reserve(count);
    result.vectors.assign(count * count, 0.0);
    for (std::size_t mode = 0U; mode < count; ++mode) {
        const std::size_t source = order[mode];
        result.values.push_back(matrix[source * count + source]);
        for (std::size_t row = 0U; row < count; ++row) {
            result.vectors[row * count + mode] = vectors[row * count + source];
        }
    }
    return result;
}

void canonicalize_mode(std::vector<double>& values) {
    for (const double value : values) {
        if (std::abs(value) <= kValidationTolerance) continue;
        if (value < 0.0) {
            for (double& entry : values) entry = -entry;
        }
        return;
    }
}

} // namespace

core::Result<void> SurfaceSpectrumOptions::validate() const {
    if (max_vertices == 0U || max_vertices > 4096U || max_modes == 0U ||
        max_modes > max_vertices || max_jacobi_sweeps == 0U ||
        max_jacobi_sweeps > 4096U || !finite_positive(convergence_tolerance) ||
        convergence_tolerance > 1.0 || !finite_positive(minimum_mass) ||
        minimum_mass > 1.0) {
        return core::Result<void>::failure(invalid("surface spectrum options are outside bounded limits"));
    }
    return core::Result<void>::success();
}

std::string SurfaceSpectrumOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-spectrum-options-v1;max_vertices=" << max_vertices
           << ";max_modes=" << max_modes
           << ";max_sweeps=" << max_jacobi_sweeps
           << ";tolerance=" << convergence_tolerance
           << ";minimum_mass=" << minimum_mass;
    return output.str();
}

assets::Sha256Digest SurfaceSpectrumOptions::digest() const {
    return digest_text(canonical());
}

core::Result<void> SurfaceSpectrumMode::validate(std::size_t vertex_count) const {
    if (!finite_nonnegative(eigenvalue) || coefficients.size() != vertex_count ||
        !std::isfinite(signal_coefficient) || !finite_nonnegative(signal_energy) ||
        std::abs(signal_energy - signal_coefficient * signal_coefficient) >
            kValidationTolerance * std::max(1.0, signal_energy)) {
        return core::Result<void>::failure(validation("surface spectrum mode is invalid"));
    }
    for (const double coefficient : coefficients) {
        if (!std::isfinite(coefficient)) {
            return core::Result<void>::failure(validation(
                "surface spectrum mode contains a non-finite coefficient"));
        }
    }
    return core::Result<void>::success();
}

std::string SurfaceSpectrum::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-spectrum-v1;domain=" << domain.value
           << ";revision=" << source_revision.value()
           << ";source=" << source_content_digest
           << ";vertices=";
    for (const geometry::VertexId id : vertex_ids) output << id.value << ',';
    output << ";masses=";
    for (const double mass : masses) output << mass << ',';
    output << ";modes=";
    for (const SurfaceSpectrumMode& mode : modes) {
        output << mode.eigenvalue << ':' << mode.signal_coefficient << ':'
               << mode.signal_energy << ':';
        for (const double coefficient : mode.coefficients) output << coefficient << ',';
        output << ';';
    }
    output << ";total=" << total_signal_energy
           << ";smoothness=" << smoothness_energy
           << ";algorithm=" << algorithm_version;
    return output.str();
}

core::Result<void> SurfaceSpectrum::refresh_digest() {
    if (!domain || source_revision.exhausted() || !valid_hex_digest(source_content_digest) ||
        vertex_ids.empty() || vertex_ids.size() > kMaximumSpectrumVertices ||
        vertex_ids.size() != masses.size() || modes.empty() ||
        modes.size() > std::min(kMaximumSpectrumModes, vertex_ids.size()) ||
        algorithm_version.empty() || !finite_nonnegative(total_signal_energy) ||
        !finite_nonnegative(smoothness_energy)) {
        return core::Result<void>::failure(validation(
            "surface spectrum cannot refresh digest from incomplete content"));
    }
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceSpectrum::validate(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& source_domain) const {
    if (!mesh.valid()) {
        return core::Result<void>::failure(validation("surface spectrum source mesh is invalid"));
    }
    if (auto valid = source_domain.validate(mesh); !valid) {
        return valid;
    }
    if (domain != source_domain.id || source_revision != source_domain.source_revision ||
        source_content_digest != source_domain.source_content_digest ||
        vertex_ids.empty() || vertex_ids.size() > kMaximumSpectrumVertices ||
        vertex_ids.size() != masses.size() || modes.empty() ||
        modes.size() > std::min(kMaximumSpectrumModes, vertex_ids.size()) ||
        !std::is_sorted(vertex_ids.begin(), vertex_ids.end()) ||
        std::adjacent_find(vertex_ids.begin(), vertex_ids.end()) != vertex_ids.end() ||
        !valid_hex_digest(digest) || digest != digest_hex(canonical()) ||
        !finite_nonnegative(total_signal_energy) || !finite_nonnegative(smoothness_energy) ||
        algorithm_version.empty()) {
        return core::Result<void>::failure(validation("surface spectrum identity or digest is invalid"));
    }

    std::vector<geometry::VertexId> mesh_ids = mesh.vertex_ids;
    std::sort(mesh_ids.begin(), mesh_ids.end());
    if (std::adjacent_find(mesh_ids.begin(), mesh_ids.end()) != mesh_ids.end()) {
        return core::Result<void>::failure(validation("surface spectrum source vertex IDs are not unique"));
    }
    for (std::size_t index = 0U; index < vertex_ids.size(); ++index) {
        if (!vertex_ids[index] || !std::binary_search(mesh_ids.begin(), mesh_ids.end(), vertex_ids[index]) ||
            !finite_positive(masses[index])) {
            return core::Result<void>::failure(validation("surface spectrum vertex or mass is invalid"));
        }
    }
    for (std::size_t mode_index = 0U; mode_index < modes.size(); ++mode_index) {
        if (auto valid = modes[mode_index].validate(vertex_ids.size()); !valid) return valid;
        if (mode_index > 0U && modes[mode_index].eigenvalue + kValidationTolerance <
            modes[mode_index - 1U].eigenvalue) {
            return core::Result<void>::failure(validation(
                "surface spectrum eigenvalues are not sorted"));
        }
        double norm = 0.0;
        for (std::size_t vertex = 0U; vertex < vertex_ids.size(); ++vertex) {
            norm += masses[vertex] * modes[mode_index].coefficients[vertex] *
                modes[mode_index].coefficients[vertex];
        }
        if (!std::isfinite(norm) || std::abs(norm - 1.0) > 5e-5) {
            return core::Result<void>::failure(validation(
                "surface spectrum mode is not mass normalized"));
        }
        for (std::size_t previous = 0U; previous < mode_index; ++previous) {
            double overlap = 0.0;
            for (std::size_t vertex = 0U; vertex < vertex_ids.size(); ++vertex) {
                overlap += masses[vertex] * modes[previous].coefficients[vertex] *
                    modes[mode_index].coefficients[vertex];
            }
            if (!std::isfinite(overlap) || std::abs(overlap) > 5e-5) {
                return core::Result<void>::failure(validation(
                    "surface spectrum modes are not mass orthogonal"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<SurfaceSpectrum> build_surface_spectrum(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    std::span<const double> signal,
    const SurfaceSpectrumOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<SurfaceSpectrum>::failure(valid.error());
    }
    if (auto valid = domain.validate(mesh); !valid) {
        return core::Result<SurfaceSpectrum>::failure(valid.error());
    }
    if (signal.size() != mesh.positions.size()) {
        return core::Result<SurfaceSpectrum>::failure(invalid(
            "surface spectrum signal must align with compiled mesh vertices"));
    }
    for (const double value : signal) {
        if (!std::isfinite(value)) {
            return core::Result<SurfaceSpectrum>::failure(invalid(
                "surface spectrum signal contains a non-finite sample"));
        }
    }

    const auto graph = build_graph(mesh, domain, options);
    if (!graph) return core::Result<SurfaceSpectrum>::failure(graph.error());
    const std::size_t count = graph.value().global_vertices.size();
    const auto eigen = jacobi_eigensystem(
        normalized_laplacian(graph.value()), count,
        options.max_jacobi_sweeps, options.convergence_tolerance);

    SurfaceSpectrum result;
    result.domain = domain.id;
    result.source_revision = domain.source_revision;
    result.source_content_digest = domain.source_content_digest;
    result.masses = graph.value().masses;
    result.vertex_ids.reserve(count);
    for (const std::uint32_t global_vertex : graph.value().global_vertices) {
        result.vertex_ids.push_back(mesh.vertex_ids[global_vertex]);
        result.total_signal_energy += graph.value().masses[result.vertex_ids.size() - 1U] *
            signal[global_vertex] * signal[global_vertex];
    }
    for (const auto& [edge, weight] : graph.value().edges) {
        const double first_signal = signal[graph.value().global_vertices[edge.first]];
        const double second_signal = signal[graph.value().global_vertices[edge.second]];
        result.smoothness_energy += weight * (first_signal - second_signal) *
            (first_signal - second_signal);
    }

    const std::size_t mode_count = std::min<std::size_t>(options.max_modes, count);
    result.modes.reserve(mode_count);
    for (std::size_t mode_index = 0U; mode_index < mode_count; ++mode_index) {
        SurfaceSpectrumMode mode;
        mode.eigenvalue = std::max(0.0, eigen.values[mode_index]);
        mode.coefficients.resize(count);
        for (std::size_t vertex = 0U; vertex < count; ++vertex) {
            mode.coefficients[vertex] = eigen.vectors[vertex * count + mode_index] /
                std::sqrt(result.masses[vertex]);
        }
        canonicalize_mode(mode.coefficients);
        for (std::size_t vertex = 0U; vertex < count; ++vertex) {
            mode.signal_coefficient += result.masses[vertex] * mode.coefficients[vertex] *
                signal[graph.value().global_vertices[vertex]];
        }
        mode.signal_energy = mode.signal_coefficient * mode.signal_coefficient;
        result.modes.push_back(std::move(mode));
    }
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceSpectrum>::failure(refreshed.error());
    }
    if (auto valid = result.validate(mesh, domain); !valid) {
        return core::Result<SurfaceSpectrum>::failure(valid.error());
    }
    return core::Result<SurfaceSpectrum>::success(std::move(result));
}

} // namespace carto::surface
