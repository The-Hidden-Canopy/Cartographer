#include <carto/surface/qem.hpp>

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

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
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

struct EdgeKey {
    std::uint32_t first = 0U;
    std::uint32_t second = 0U;

    [[nodiscard]] bool operator<(const EdgeKey& other) const noexcept {
        return first < other.first || (first == other.first && second < other.second);
    }
};

struct Triangle {
    geometry::FaceId face;
    std::array<std::uint32_t, 3> vertices{};
};

struct Quadric {
    double values[4][4]{};

    Quadric& operator+=(const Quadric& other) noexcept {
        for (auto& row : values) {
            const std::size_t row_index = static_cast<std::size_t>(&row - &values[0]);
            for (std::size_t column = 0U; column < 4U; ++column) {
                row[column] += other.values[row_index][column];
            }
        }
        return *this;
    }
};

Quadric operator+(Quadric left, const Quadric& right) noexcept {
    left += right;
    return left;
}

void add_plane(Quadric& quadric, core::Vec3d normal, double offset) {
    const std::array<double, 4> plane{normal.x, normal.y, normal.z, offset};
    for (std::size_t row = 0U; row < 4U; ++row) {
        for (std::size_t column = 0U; column < 4U; ++column) {
            quadric.values[row][column] += plane[row] * plane[column];
        }
    }
}

double evaluate(const Quadric& quadric, core::Vec3d position) {
    const std::array<double, 4> point{position.x, position.y, position.z, 1.0};
    double result = 0.0;
    for (std::size_t row = 0U; row < 4U; ++row) {
        for (std::size_t column = 0U; column < 4U; ++column) {
            result += point[row] * quadric.values[row][column] * point[column];
        }
    }
    return result;
}

bool solve_optimal_position(const Quadric& quadric, core::Vec3d& result) {
    double matrix[3][4]{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            matrix[row][column] = quadric.values[row][column];
        }
        matrix[row][3U] = -quadric.values[row][3U];
    }
    for (std::size_t pivot = 0U; pivot < 3U; ++pivot) {
        std::size_t best = pivot;
        for (std::size_t row = pivot + 1U; row < 3U; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[best][pivot])) best = row;
        }
        if (std::abs(matrix[best][pivot]) <= kEpsilon) return false;
        if (best != pivot) {
            for (std::size_t column = pivot; column < 4U; ++column) {
                std::swap(matrix[best][column], matrix[pivot][column]);
            }
        }
        const double divisor = matrix[pivot][pivot];
        for (std::size_t column = pivot; column < 4U; ++column) {
            matrix[pivot][column] /= divisor;
        }
        for (std::size_t row = 0U; row < 3U; ++row) {
            if (row == pivot) continue;
            const double factor = matrix[row][pivot];
            for (std::size_t column = pivot; column < 4U; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
            }
        }
    }
    result = {matrix[0][3U], matrix[1][3U], matrix[2][3U]};
    return result.finite();
}

double triangle_area(
    const std::vector<core::Vec3d>& positions,
    const std::array<std::uint32_t, 3>& vertices) {
    return core::cross(
        positions[vertices[1U]] - positions[vertices[0U]],
        positions[vertices[2U]] - positions[vertices[0U]]).length() * 0.5;
}

std::array<std::uint32_t, 3> sorted_triangle(
    std::array<std::uint32_t, 3> vertices) {
    std::sort(vertices.begin(), vertices.end());
    return vertices;
}

bool degenerate(const std::array<std::uint32_t, 3>& vertices) {
    return vertices[0U] == vertices[1U] || vertices[0U] == vertices[2U] ||
        vertices[1U] == vertices[2U];
}

void add_edge(
    std::map<EdgeKey, std::uint32_t>& incidence,
    std::uint32_t first,
    std::uint32_t second) {
    if (first > second) std::swap(first, second);
    if (first != second) ++incidence[EdgeKey{first, second}];
}

struct Candidate {
    EdgeKey edge;
    core::Vec3d position;
    double cost = 0.0;
};

std::vector<Candidate> build_candidates(
    const std::vector<Triangle>& triangles,
    const std::vector<Quadric>& quadrics,
    const std::vector<core::Vec3d>& positions,
    const std::vector<geometry::VertexId>& vertex_ids,
    bool preserve_boundary) {
    std::map<EdgeKey, std::uint32_t> incidence;
    for (const Triangle& triangle : triangles) {
        add_edge(incidence, triangle.vertices[0U], triangle.vertices[1U]);
        add_edge(incidence, triangle.vertices[1U], triangle.vertices[2U]);
        add_edge(incidence, triangle.vertices[2U], triangle.vertices[0U]);
    }

    std::vector<Candidate> candidates;
    candidates.reserve(incidence.size());
    for (const auto& [edge, count] : incidence) {
        if (preserve_boundary && count == 1U) continue;
        const Quadric combined = quadrics[edge.first] + quadrics[edge.second];
        core::Vec3d proposed{};
        if (!solve_optimal_position(combined, proposed)) {
            const double first_cost = evaluate(combined, positions[edge.first]);
            const double second_cost = evaluate(combined, positions[edge.second]);
            proposed = first_cost <= second_cost ? positions[edge.first] : positions[edge.second];
        }
        const double cost = evaluate(combined, proposed);
        if (proposed.finite() && finite_nonnegative(cost)) {
            candidates.push_back({edge, proposed, std::max(0.0, cost)});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& left, const Candidate& right) {
        if (left.cost != right.cost) return left.cost < right.cost;
        const auto left_first = vertex_ids[left.edge.first].value;
        const auto right_first = vertex_ids[right.edge.first].value;
        if (left_first != right_first) return left_first < right_first;
        return vertex_ids[left.edge.second].value < vertex_ids[right.edge.second].value;
    });
    return candidates;
}

bool acceptable_collapse(
    const std::vector<Triangle>& triangles,
    std::vector<core::Vec3d>& positions,
    const Candidate& candidate,
    std::uint32_t target_triangles,
    double minimum_area,
    std::vector<Triangle>& rewritten) {
    const std::uint32_t kept = candidate.edge.first;
    const std::uint32_t removed = candidate.edge.second;
    const core::Vec3d previous_position = positions[kept];
    positions[kept] = candidate.position;
    std::set<std::array<std::uint32_t, 3>> unique_triangles;
    rewritten.clear();
    bool duplicate = false;
    for (const Triangle& triangle : triangles) {
        Triangle next = triangle;
        for (std::uint32_t& vertex : next.vertices) {
            if (vertex == removed) vertex = kept;
        }
        if (degenerate(next.vertices)) continue;
        const auto key = sorted_triangle(next.vertices);
        if (!unique_triangles.insert(key).second) {
            duplicate = true;
            break;
        }
        if (triangle_area(positions, next.vertices) <= minimum_area) {
            positions[kept] = previous_position;
            return false;
        }
        rewritten.push_back(next);
    }
    const bool accepted = !duplicate && rewritten.size() == target_triangles;
    if (!accepted) positions[kept] = previous_position;
    return accepted;
}

} // namespace

core::Result<void> SurfaceQemOptions::validate() const {
    if (max_source_triangles == 0U || max_source_triangles > 65536U ||
        target_triangles == 0U || target_triangles > max_source_triangles ||
        max_collapses == 0U || max_collapses > 65536U ||
        !finite_positive(minimum_triangle_area) || minimum_triangle_area > 1.0) {
        return core::Result<void>::failure(invalid(
            "surface QEM options are outside bounded limits"));
    }
    return core::Result<void>::success();
}

std::string SurfaceQemOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-qem-options-v1;max_source=" << max_source_triangles
           << ";target=" << target_triangles
           << ";max_collapses=" << max_collapses
           << ";minimum_area=" << minimum_triangle_area
           << ";preserve_boundary=" << (preserve_boundary ? 1 : 0);
    return output.str();
}

assets::Sha256Digest SurfaceQemOptions::digest() const {
    return digest_text(canonical());
}

core::Result<void> SurfaceQemCollapse::validate() const {
    if (!kept_vertex || !removed_vertex || kept_vertex == removed_vertex ||
        !proposed_position.finite() || !finite_nonnegative(cost)) {
        return core::Result<void>::failure(validation("surface QEM collapse receipt is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceQemResult::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-qem-result-v1;domain=" << domain.value
           << ";revision=" << source_revision.value()
           << ";source=" << source_content_digest
           << ";source_triangles=" << source_triangles
           << ";target_triangles=" << target_triangles
           << ";target_reached=" << (target_reached ? 1 : 0)
           << ";vertices=";
    for (std::size_t index = 0U; index < vertex_ids.size(); ++index) {
        output << vertex_ids[index].value << ':' << positions[index].x << ','
               << positions[index].y << ',' << positions[index].z << ':'
               << normals[index].x << ',' << normals[index].y << ',' << normals[index].z << ';';
    }
    output << ";indices=";
    for (const std::uint32_t index : indices) output << index << ',';
    output << ";faces=";
    for (const geometry::FaceId face : triangle_faces) output << face.value << ',';
    output << ";collapses=";
    for (const SurfaceQemCollapse& collapse : collapses) {
        output << collapse.kept_vertex.value << ':' << collapse.removed_vertex.value << ':'
               << collapse.proposed_position.x << ',' << collapse.proposed_position.y << ','
               << collapse.proposed_position.z << ':' << collapse.cost << ';';
    }
    output << ";algorithm=" << algorithm_version;
    return output.str();
}

core::Result<void> SurfaceQemResult::refresh_digest() {
    if (!domain || source_revision.exhausted() || !valid_hex_digest(source_content_digest) ||
        source_triangles == 0U || target_triangles == 0U || !target_reached ||
        positions.empty() || positions.size() != normals.size() ||
        positions.size() != vertex_ids.size() || indices.empty() || indices.size() % 3U != 0U ||
        triangle_faces.size() != indices.size() / 3U || algorithm_version.empty()) {
        return core::Result<void>::failure(validation(
            "surface QEM result cannot refresh digest from incomplete content"));
    }
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceQemResult::validate(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& source_domain) const {
    if (!source.valid()) {
        return core::Result<void>::failure(validation("surface QEM source mesh is invalid"));
    }
    if (auto valid = source_domain.validate(source); !valid) return valid;
    if (domain != source_domain.id || source_revision != source_domain.source_revision ||
        source_content_digest != source_domain.source_content_digest || source_triangles == 0U ||
        target_triangles == 0U || target_triangles > source_triangles || !target_reached ||
        triangle_count() != target_triangles || positions.empty() ||
        positions.size() != normals.size() || positions.size() != vertex_ids.size() ||
        indices.empty() || indices.size() % 3U != 0U ||
        triangle_faces.size() != triangle_count() ||
        !std::is_sorted(vertex_ids.begin(), vertex_ids.end()) ||
        std::adjacent_find(vertex_ids.begin(), vertex_ids.end()) != vertex_ids.end() ||
        !std::is_sorted(triangle_faces.begin(), triangle_faces.end()) ||
        std::adjacent_find(triangle_faces.begin(), triangle_faces.end()) != triangle_faces.end() ||
        !valid_hex_digest(digest) || digest != digest_hex(canonical()) || algorithm_version.empty()) {
        return core::Result<void>::failure(validation("surface QEM result identity is invalid"));
    }

    std::set<geometry::VertexId> source_vertices(source.vertex_ids.begin(), source.vertex_ids.end());
    std::set<geometry::FaceId> source_faces(source_domain.source_faces.begin(), source_domain.source_faces.end());
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        if (!vertex_ids[index] || !source_vertices.contains(vertex_ids[index]) ||
            !positions[index].finite() || !normals[index].finite() ||
            normals[index].length_squared() <= kEpsilon) {
            return core::Result<void>::failure(validation("surface QEM output vertex is invalid"));
        }
    }
    for (const std::uint32_t index : indices) {
        if (index >= positions.size()) {
            return core::Result<void>::failure(validation("surface QEM index is out of range"));
        }
    }
    for (const geometry::FaceId face : triangle_faces) {
        if (!face || !source_faces.contains(face)) {
            return core::Result<void>::failure(validation(
                "surface QEM output face is outside the source domain"));
        }
    }
    for (const SurfaceQemCollapse& collapse : collapses) {
        if (auto valid = collapse.validate(); !valid) return valid;
    }
    return core::Result<void>::success();
}

core::Result<SurfaceQemResult> build_constrained_qem(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceQemOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<SurfaceQemResult>::failure(valid.error());
    }
    if (auto valid = domain.validate(mesh); !valid) {
        return core::Result<SurfaceQemResult>::failure(valid.error());
    }

    const std::set<geometry::FaceId> domain_faces(domain.source_faces.begin(), domain.source_faces.end());
    std::vector<Triangle> triangles;
    std::set<geometry::FaceId> admitted_faces;
    for (std::size_t triangle_index = 0U; triangle_index < mesh.triangle_faces.size(); ++triangle_index) {
        const geometry::FaceId face = mesh.triangle_faces[triangle_index];
        if (!domain_faces.contains(face)) continue;
        const std::size_t offset = triangle_index * 3U;
        Triangle triangle{face, {mesh.indices[offset], mesh.indices[offset + 1U], mesh.indices[offset + 2U]}};
        if (degenerate(triangle.vertices) || triangle_area(mesh.positions, triangle.vertices) <=
                options.minimum_triangle_area) {
            continue;
        }
        if (!admitted_faces.insert(face).second) {
            return core::Result<SurfaceQemResult>::failure(validation(
                "surface QEM requires unique source face identities"));
        }
        triangles.push_back(triangle);
    }
    if (triangles.empty()) {
        return core::Result<SurfaceQemResult>::failure(validation(
            "surface QEM domain contains no non-degenerate triangles"));
    }
    if (triangles.size() > options.max_source_triangles) {
        return core::Result<SurfaceQemResult>::failure(unsupported(
            "surface QEM source triangle count exceeds the bounded compiler cap"));
    }
    if (options.target_triangles > triangles.size()) {
        return core::Result<SurfaceQemResult>::failure(invalid(
            "surface QEM target exceeds the admitted source triangle count"));
    }

    std::vector<core::Vec3d> positions = mesh.positions;
    std::vector<Quadric> quadrics(mesh.positions.size());
    for (const Triangle& triangle : triangles) {
        const core::Vec3d raw = core::cross(
            positions[triangle.vertices[1U]] - positions[triangle.vertices[0U]],
            positions[triangle.vertices[2U]] - positions[triangle.vertices[0U]]);
        const double length = raw.length();
        const core::Vec3d normal = raw * (1.0 / length);
        const double offset = -core::dot(normal, positions[triangle.vertices[0U]]);
        for (const std::uint32_t vertex : triangle.vertices) add_plane(quadrics[vertex], normal, offset);
    }

    std::vector<SurfaceQemCollapse> collapses;
    std::vector<Triangle> rewritten;
    while (triangles.size() > options.target_triangles && collapses.size() < options.max_collapses) {
        const std::vector<Candidate> candidates = build_candidates(
            triangles, quadrics, positions, mesh.vertex_ids, options.preserve_boundary);
        bool accepted = false;
        for (const Candidate& candidate : candidates) {
            if (!acceptable_collapse(
                    triangles, positions, candidate, options.target_triangles,
                    options.minimum_triangle_area, rewritten)) {
                continue;
            }
            const std::uint32_t kept = candidate.edge.first;
            const std::uint32_t removed = candidate.edge.second;
            quadrics[kept] += quadrics[removed];
            triangles = rewritten;
            collapses.push_back({
                mesh.vertex_ids[kept], mesh.vertex_ids[removed], candidate.position, candidate.cost});
            accepted = true;
            break;
        }
        if (!accepted) {
            return core::Result<SurfaceQemResult>::failure(unsupported(
                "surface QEM constraints could not reach the exact target triangle count"));
        }
    }
    if (triangles.size() != options.target_triangles) {
        return core::Result<SurfaceQemResult>::failure(unsupported(
            "surface QEM collapse budget could not reach the exact target triangle count"));
    }

    std::set<std::uint32_t> used_vertices;
    for (const Triangle& triangle : triangles) {
        for (const std::uint32_t vertex : triangle.vertices) used_vertices.insert(vertex);
    }
    std::vector<std::uint32_t> ordered_vertices(used_vertices.begin(), used_vertices.end());
    std::sort(ordered_vertices.begin(), ordered_vertices.end(), [&](std::uint32_t left, std::uint32_t right) {
        return mesh.vertex_ids[left] < mesh.vertex_ids[right];
    });
    std::vector<std::uint32_t> remap(mesh.positions.size(), std::numeric_limits<std::uint32_t>::max());

    SurfaceQemResult result;
    result.domain = domain.id;
    result.source_revision = domain.source_revision;
    result.source_content_digest = domain.source_content_digest;
    result.source_triangles = static_cast<std::uint32_t>(admitted_faces.size());
    result.target_triangles = options.target_triangles;
    result.target_reached = true;
    result.collapses = std::move(collapses);
    result.positions.reserve(ordered_vertices.size());
    result.normals.assign(ordered_vertices.size(), {});
    result.vertex_ids.reserve(ordered_vertices.size());
    for (const std::uint32_t vertex : ordered_vertices) {
        remap[vertex] = static_cast<std::uint32_t>(result.positions.size());
        result.positions.push_back(positions[vertex]);
        result.vertex_ids.push_back(mesh.vertex_ids[vertex]);
    }
    std::sort(triangles.begin(), triangles.end(), [](const Triangle& left, const Triangle& right) {
        return left.face < right.face;
    });
    result.indices.reserve(triangles.size() * 3U);
    result.triangle_faces.reserve(triangles.size());
    for (const Triangle& triangle : triangles) {
        result.triangle_faces.push_back(triangle.face);
        for (const std::uint32_t vertex : triangle.vertices) {
            result.indices.push_back(remap[vertex]);
        }
        const core::Vec3d raw = core::cross(
            result.positions[result.indices[result.indices.size() - 2U]] -
                result.positions[result.indices[result.indices.size() - 3U]],
            result.positions[result.indices[result.indices.size() - 1U]] -
                result.positions[result.indices[result.indices.size() - 3U]]);
        for (const std::uint32_t index : {result.indices[result.indices.size() - 3U],
                                          result.indices[result.indices.size() - 2U],
                                          result.indices[result.indices.size() - 1U]}) {
            result.normals[index] = result.normals[index] + raw;
        }
    }
    for (core::Vec3d& normal : result.normals) normal = normal.normalized();
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceQemResult>::failure(refreshed.error());
    }
    if (auto valid = result.validate(mesh, domain); !valid) {
        return core::Result<SurfaceQemResult>::failure(valid.error());
    }
    return core::Result<SurfaceQemResult>::success(std::move(result));
}

} // namespace carto::surface
