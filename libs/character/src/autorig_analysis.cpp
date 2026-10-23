#include <carto/character/autorig_analysis.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace carto::character {
namespace {

constexpr double kMaxCoordinate = 1.0e9;
using Cell = std::array<std::int64_t, 3U>;
using Neighbor = std::pair<geometry::VertexId, double>;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

double component(const core::Vec3d& value, AutorigAxis axis) noexcept {
    switch (axis) {
    case AutorigAxis::x: return value.x;
    case AutorigAxis::y: return value.y;
    case AutorigAxis::z: return value.z;
    }
    return 0.0;
}

core::Vec3d reflect(
    core::Vec3d value,
    AutorigAxis axis,
    double plane_offset) noexcept {
    switch (axis) {
    case AutorigAxis::x: value.x = 2.0 * plane_offset - value.x; break;
    case AutorigAxis::y: value.y = 2.0 * plane_offset - value.y; break;
    case AutorigAxis::z: value.z = 2.0 * plane_offset - value.z; break;
    }
    return value;
}

double distance(const core::Vec3d& first, const core::Vec3d& second) noexcept {
    return std::hypot(first.x - second.x, first.y - second.y, first.z - second.z);
}

bool quantize(const core::Vec3d& point, double cell_size, Cell& result) noexcept {
    const std::array coordinates{point.x, point.y, point.z};
    constexpr double limit = static_cast<double>(
        std::numeric_limits<std::int64_t>::max() - 2);
    for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
        const double quotient = std::floor(coordinates[axis] / cell_size);
        if (!std::isfinite(quotient) || std::abs(quotient) > limit) return false;
        result[axis] = static_cast<std::int64_t>(quotient);
    }
    return true;
}

assets::Sha256Digest analysis_digest(const AutorigFeatureAnalysis& analysis) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "CARTO_AUTORIG_FEATURE_ANALYSIS_V1\n"
        << analysis.source.asset_id << ' ' << analysis.source.content_digest.hex() << ' '
        << analysis.mesh_revision.value() << ' '
        << static_cast<unsigned>(analysis.symmetry_axis) << ' '
        << analysis.symmetry_plane_offset << ' ' << analysis.symmetry_tolerance << ' '
        << analysis.mesh_geometry_digest.hex() << '\n';
    for (const auto& item : analysis.components) {
        canonical << "C " << item.component_index << ' ' << item.representative.value
            << ' ' << item.vertex_count << '\n';
    }
    for (const auto& item : analysis.vertices) {
        canonical << "V " << item.vertex.value << ' ' << item.component_index << ' '
            << item.valence << ' ' << item.incident_face_count << ' '
            << item.boundary << '\n';
    }
    for (const auto& field : analysis.geodesic_fields) {
        canonical << "G " << field.seed.value << '\n';
        for (const auto& item : field.distances) {
            canonical << item.vertex.value << ' ' << item.reachable << ' '
                << item.distance << '\n';
        }
    }
    for (const auto& item : analysis.symmetry_pairs) {
        canonical << "S " << item.negative_side.value << ' '
            << item.positive_side.value << ' ' << item.positional_error << ' '
            << item.topology_compatible << '\n';
    }
    const auto write_ids = [&canonical](char tag,
        const std::vector<geometry::VertexId>& ids) {
        canonical << tag;
        for (const auto id : ids) canonical << ' ' << id.value;
        canonical << '\n';
    };
    write_ids('L', analysis.centerline_vertices);
    write_ids('A', analysis.ambiguous_vertices);
    write_ids('U', analysis.unmatched_vertices);
    const auto bytes = canonical.str();
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

assets::Sha256Digest geometry_digest(
    const std::vector<geometry::Vertex>& vertices,
    const std::vector<geometry::Face>& faces) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "CARTO_AUTORIG_INPUT_MESH_V1\n";
    for (const auto& vertex : vertices) {
        canonical << "V " << vertex.id.value << ' ' << vertex.position.x << ' '
            << vertex.position.y << ' ' << vertex.position.z << '\n';
    }
    for (const auto& face : faces) {
        canonical << "F " << face.id.value;
        for (const auto vertex : face.vertices) canonical << ' ' << vertex.value;
        canonical << '\n';
    }
    const auto bytes = canonical.str();
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

} // namespace

core::Result<void> AutorigAnalysisBudget::validate() const {
    if (max_vertices == 0U || max_vertices > kHardMaxVertices ||
        max_edges == 0U || max_edges > kHardMaxEdges ||
        max_geodesic_seeds > kHardMaxGeodesicSeeds ||
        max_symmetry_candidates == 0U ||
        max_symmetry_candidates > kHardMaxSymmetryCandidates) {
        return core::Result<void>::failure(invalid(
            "autorig analysis budget is zero or exceeds a hard bound"));
    }
    return core::Result<void>::success();
}

core::Result<void> AutorigAnalysisRequest::validate() const {
    if (auto checked = source.validate(); !checked) return checked;
    if (symmetry_axis > AutorigAxis::z || !std::isfinite(symmetry_plane_offset) ||
        std::abs(symmetry_plane_offset) > kMaxCoordinate ||
        !std::isfinite(symmetry_tolerance) || symmetry_tolerance <= 0.0 ||
        symmetry_tolerance > kMaxCoordinate) {
        return core::Result<void>::failure(invalid(
            "autorig symmetry plane or tolerance is invalid"));
    }
    if (auto checked = budget.validate(); !checked) return checked;
    if (geodesic_seeds.size() > budget.max_geodesic_seeds) {
        return core::Result<void>::failure(invalid(
            "autorig geodesic seed count exceeds the request budget"));
    }
    std::set<std::uint64_t> seeds;
    for (const auto seed : geodesic_seeds) {
        if (!seed || !seeds.insert(seed.value).second) {
            return core::Result<void>::failure(invalid(
                "autorig geodesic seeds must be nonzero and unique"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> AutorigFeatureAnalysis::validate() const {
    if (auto checked = source.validate(); !checked) return checked;
    if (symmetry_axis > AutorigAxis::z || !std::isfinite(symmetry_plane_offset) ||
        !std::isfinite(symmetry_tolerance) || symmetry_tolerance <= 0.0 ||
        mesh_geometry_digest.is_zero() || analysis_digest.is_zero() || vertices.empty() ||
        components.empty() ||
        vertices.size() > AutorigAnalysisBudget::kHardMaxVertices) {
        return core::Result<void>::failure(validation(
            "autorig analysis receipt has invalid source, bounds, or digest"));
    }
    std::set<std::uint64_t> vertex_ids;
    std::vector<std::size_t> component_sizes(components.size(), 0U);
    std::vector<geometry::VertexId> component_representatives(components.size());
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        const auto& item = vertices[index];
        if (!item.vertex || item.component_index >= components.size() ||
            (index != 0U && vertices[index - 1U].vertex >= item.vertex) ||
            !vertex_ids.insert(item.vertex.value).second) {
            return core::Result<void>::failure(validation(
                "autorig vertex features are invalid or unsorted"));
        }
        if (component_sizes[item.component_index] == 0U) {
            component_representatives[item.component_index] = item.vertex;
        }
        ++component_sizes[item.component_index];
    }
    for (std::size_t index = 0U; index < components.size(); ++index) {
        const auto& item = components[index];
        if (item.component_index != index || !item.representative ||
            item.vertex_count == 0U || item.vertex_count != component_sizes[index] ||
            item.representative != component_representatives[index]) {
            return core::Result<void>::failure(validation(
                "autorig component features are invalid"));
        }
    }
    if (geodesic_fields.size() > AutorigAnalysisBudget::kHardMaxGeodesicSeeds) {
        return core::Result<void>::failure(validation(
            "autorig geodesic field count exceeds its hard bound"));
    }
    geometry::VertexId previous_seed;
    for (const auto& field : geodesic_fields) {
        if (!vertex_ids.contains(field.seed.value) || field.distances.size() != vertices.size() ||
            (previous_seed && !(previous_seed < field.seed))) {
            return core::Result<void>::failure(validation(
                "autorig geodesic field is incomplete or has a missing seed"));
        }
        previous_seed = field.seed;
        for (std::size_t index = 0U; index < field.distances.size(); ++index) {
            const auto& item = field.distances[index];
            if (item.vertex != vertices[index].vertex || !std::isfinite(item.distance) ||
                item.distance < 0.0 || (!item.reachable && item.distance != 0.0)) {
                return core::Result<void>::failure(validation(
                    "autorig geodesic distances are invalid or unsorted"));
            }
        }
    }
    std::set<std::uint64_t> paired_vertices;
    std::pair<geometry::VertexId, geometry::VertexId> previous_pair{};
    for (std::size_t index = 0U; index < symmetry_pairs.size(); ++index) {
        const auto& pair = symmetry_pairs[index];
        if (!vertex_ids.contains(pair.negative_side.value) ||
            !vertex_ids.contains(pair.positive_side.value) ||
            pair.negative_side == pair.positive_side ||
            !std::isfinite(pair.positional_error) || pair.positional_error < 0.0 ||
            pair.positional_error > symmetry_tolerance ||
            !paired_vertices.insert(pair.negative_side.value).second ||
            !paired_vertices.insert(pair.positive_side.value).second ||
            (index != 0U &&
                !(previous_pair < std::pair{pair.negative_side, pair.positive_side}))) {
            return core::Result<void>::failure(validation(
                "autorig symmetry pair is invalid"));
        }
        previous_pair = {pair.negative_side, pair.positive_side};
    }
    const auto validate_classification = [&vertex_ids](
        const std::vector<geometry::VertexId>& ids) -> core::Result<void> {
        for (std::size_t index = 0U; index < ids.size(); ++index) {
            if (!vertex_ids.contains(ids[index].value) ||
                (index != 0U && ids[index - 1U] >= ids[index])) {
                return core::Result<void>::failure(validation(
                    "autorig vertex classification is invalid or unsorted"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto checked = validate_classification(centerline_vertices); !checked) return checked;
    if (auto checked = validate_classification(ambiguous_vertices); !checked) return checked;
    if (auto checked = validate_classification(unmatched_vertices); !checked) return checked;
    std::set<std::uint64_t> classified_vertices;
    const auto add_classification = [&classified_vertices, &paired_vertices](
        const std::vector<geometry::VertexId>& ids) -> bool {
        for (const auto id : ids) {
            if (paired_vertices.contains(id.value) ||
                !classified_vertices.insert(id.value).second) return false;
        }
        return true;
    };
    if (!add_classification(centerline_vertices) ||
        !add_classification(ambiguous_vertices) ||
        !add_classification(unmatched_vertices)) {
        return core::Result<void>::failure(validation(
            "autorig symmetry classifications overlap"));
    }
    return core::Result<void>::success();
}

core::Result<AutorigFeatureAnalysis> analyze_autorig_features(
    const geometry::EditableMesh& mesh,
    const AutorigAnalysisRequest& request) {
    if (auto checked = request.validate(); !checked) {
        return core::Result<AutorigFeatureAnalysis>::failure(checked.error());
    }
    if (mesh.revision() != request.expected_mesh_revision) {
        return core::Result<AutorigFeatureAnalysis>::failure(stale(
            "autorig analysis mesh revision is stale"));
    }
    if (auto checked = mesh.validate(); !checked) {
        return core::Result<AutorigFeatureAnalysis>::failure(checked.error());
    }
    const auto vertices = mesh.vertices_sorted();
    if (vertices.empty() || vertices.size() > request.budget.max_vertices ||
        vertices.size() > AutorigAnalysisBudget::kHardMaxVertices) {
        return core::Result<AutorigFeatureAnalysis>::failure(invalid(
            "autorig mesh vertex count is empty or exceeds its budget"));
    }
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<AutorigFeatureAnalysis>::failure(topology.error());
    }
    if (topology.value().edges.size() > request.budget.max_edges ||
        topology.value().edges.size() > AutorigAnalysisBudget::kHardMaxEdges) {
        return core::Result<AutorigFeatureAnalysis>::failure(invalid(
            "autorig mesh edge count exceeds its budget"));
    }

    std::map<geometry::VertexId, core::Vec3d> positions;
    std::map<geometry::VertexId, std::vector<Neighbor>> adjacency;
    std::set<geometry::VertexId> boundary_vertices;
    std::map<geometry::VertexId, std::uint32_t> face_counts;
    for (const auto& vertex : vertices) {
        if (!vertex.position.finite() || std::abs(vertex.position.x) > kMaxCoordinate ||
            std::abs(vertex.position.y) > kMaxCoordinate ||
            std::abs(vertex.position.z) > kMaxCoordinate) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig mesh contains a non-finite or over-range vertex"));
        }
        positions.emplace(vertex.id, vertex.position);
        adjacency.emplace(vertex.id, std::vector<Neighbor>{});
        face_counts.emplace(vertex.id, 0U);
    }
    for (const auto& edge : topology.value().edges) {
        const auto first = positions.find(edge.first);
        const auto second = positions.find(edge.second);
        if (first == positions.end() || second == positions.end()) {
            return core::Result<AutorigFeatureAnalysis>::failure(validation(
                "autorig topology contains an edge with a missing endpoint"));
        }
        const double edge_length = distance(first->second, second->second);
        if (!std::isfinite(edge_length) || edge_length <= 0.0) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig geodesic graph contains an invalid edge length"));
        }
        adjacency[edge.first].emplace_back(edge.second, edge_length);
        adjacency[edge.second].emplace_back(edge.first, edge_length);
        if (!edge.second_half_edge.has_value()) {
            boundary_vertices.insert(edge.first);
            boundary_vertices.insert(edge.second);
        }
    }
    for (auto& [vertex, neighbors] : adjacency) {
        static_cast<void>(vertex);
        std::sort(neighbors.begin(), neighbors.end(),
            [](const Neighbor& left, const Neighbor& right) {
                return left.first < right.first;
            });
        if (neighbors.size() > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig vertex valence exceeds the report range"));
        }
    }
    for (const auto& face : mesh.faces_sorted()) {
        for (const auto vertex : face.vertices) {
            auto found = face_counts.find(vertex);
            if (found == face_counts.end() ||
                found->second == std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<AutorigFeatureAnalysis>::failure(validation(
                    "autorig face references a missing vertex or exceeds the count range"));
            }
            ++found->second;
        }
    }

    AutorigFeatureAnalysis analysis;
    analysis.source = request.source;
    analysis.mesh_revision = mesh.revision();
    analysis.symmetry_axis = request.symmetry_axis;
    analysis.symmetry_plane_offset = request.symmetry_plane_offset;
    analysis.symmetry_tolerance = request.symmetry_tolerance;
    analysis.mesh_geometry_digest = geometry_digest(vertices, mesh.faces_sorted());

    std::map<geometry::VertexId, std::uint32_t> component_index;
    std::set<geometry::VertexId> visited;
    for (const auto& vertex : vertices) {
        if (visited.contains(vertex.id)) continue;
        if (analysis.components.size() >= std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig connected component count exceeds the report range"));
        }
        const auto component = static_cast<std::uint32_t>(analysis.components.size());
        std::queue<geometry::VertexId> pending;
        pending.push(vertex.id);
        visited.insert(vertex.id);
        std::size_t component_size = 0U;
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop();
            component_index.emplace(current, component);
            ++component_size;
            for (const auto& [neighbor, ignored_length] : adjacency.at(current)) {
                static_cast<void>(ignored_length);
                if (visited.insert(neighbor).second) pending.push(neighbor);
            }
        }
        analysis.components.push_back({component, vertex.id, component_size});
    }
    analysis.vertices.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        const auto& neighbors = adjacency.at(vertex.id);
        analysis.vertices.push_back({
            vertex.id,
            component_index.at(vertex.id),
            static_cast<std::uint32_t>(neighbors.size()),
            face_counts.at(vertex.id),
            boundary_vertices.contains(vertex.id),
        });
    }

    auto seeds = request.geodesic_seeds;
    std::sort(seeds.begin(), seeds.end());
    for (std::size_t index = 0U; index < seeds.size(); ++index) {
        if (!positions.contains(seeds[index])) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig geodesic seed does not exist in the source mesh"));
        }
        if (index != 0U && seeds[index - 1U] == seeds[index]) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig geodesic seed is duplicated"));
        }
    }
    for (const auto seed : seeds) {
        std::map<geometry::VertexId, double> distances;
        using QueueEntry = std::pair<double, geometry::VertexId>;
        std::priority_queue<QueueEntry, std::vector<QueueEntry>,
            std::greater<QueueEntry>> pending;
        distances.emplace(seed, 0.0);
        pending.emplace(0.0, seed);
        while (!pending.empty()) {
            const auto [current_distance, current] = pending.top();
            pending.pop();
            if (distances.at(current) != current_distance) continue;
            for (const auto& [neighbor, edge_length] : adjacency.at(current)) {
                const double candidate = current_distance + edge_length;
                if (!std::isfinite(candidate)) {
                    return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                        "autorig geodesic accumulation overflowed"));
                }
                const auto found = distances.find(neighbor);
                if (found == distances.end() || candidate < found->second) {
                    distances.insert_or_assign(neighbor, candidate);
                    pending.emplace(candidate, neighbor);
                }
            }
        }
        AutorigGeodesicField field;
        field.seed = seed;
        field.distances.reserve(vertices.size());
        for (const auto& vertex : vertices) {
            const auto found = distances.find(vertex.id);
            field.distances.push_back(found == distances.end()
                ? AutorigGeodesicDistance{vertex.id, 0.0, false}
                : AutorigGeodesicDistance{vertex.id, found->second, true});
        }
        analysis.geodesic_fields.push_back(std::move(field));
    }

    std::map<Cell, std::vector<std::size_t>> spatial_index;
    std::vector<bool> on_centerline(vertices.size(), false);
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        if (std::abs(component(positions.at(vertices[index].id), request.symmetry_axis) -
                request.symmetry_plane_offset) <= request.symmetry_tolerance) {
            on_centerline[index] = true;
            analysis.centerline_vertices.push_back(vertices[index].id);
        }
        Cell cell{};
        if (!quantize(positions.at(vertices[index].id), request.symmetry_tolerance, cell)) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig symmetry spatial index exceeds its numeric range"));
        }
        spatial_index[cell].push_back(index);
    }

    std::vector<std::vector<std::size_t>> candidates(vertices.size());
    std::size_t candidate_evaluations = 0U;
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        if (on_centerline[index]) continue;
        const auto reflected = reflect(
            positions.at(vertices[index].id),
            request.symmetry_axis,
            request.symmetry_plane_offset);
        Cell center{};
        if (!quantize(reflected, request.symmetry_tolerance, center)) {
            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                "autorig reflected symmetry position exceeds its numeric range"));
        }
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const Cell key{center[0] + dx, center[1] + dy, center[2] + dz};
                    const auto bucket = spatial_index.find(key);
                    if (bucket == spatial_index.end()) continue;
                    for (const auto candidate_index : bucket->second) {
                        if (++candidate_evaluations > request.budget.max_symmetry_candidates) {
                            return core::Result<AutorigFeatureAnalysis>::failure(invalid(
                                "autorig symmetry candidate work exceeds its budget"));
                        }
                        if (candidate_index == index || on_centerline[candidate_index]) continue;
                        const auto candidate_id = vertices[candidate_index].id;
                        const auto error = distance(reflected, positions.at(candidate_id));
                        if (error <= request.symmetry_tolerance) {
                            candidates[index].push_back(candidate_index);
                        }
                    }
                }
            }
        }
        std::sort(candidates[index].begin(), candidates[index].end(),
            [&vertices](std::size_t left, std::size_t right) {
                return vertices[left].id < vertices[right].id;
            });
    }

    std::vector<std::size_t> incoming(vertices.size(), 0U);
    for (const auto& matches : candidates) {
        for (const auto candidate : matches) ++incoming[candidate];
    }
    std::vector<bool> matched(vertices.size(), false);
    std::set<std::size_t> ambiguous;
    std::set<std::size_t> unmatched;
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        if (on_centerline[index] || matched[index]) continue;
        if (candidates[index].size() > 1U ||
            (candidates[index].empty() && incoming[index] != 0U)) {
            ambiguous.insert(index);
            for (const auto candidate : candidates[index]) ambiguous.insert(candidate);
            continue;
        }
        if (candidates[index].empty()) {
            unmatched.insert(index);
            continue;
        }
        const auto candidate = candidates[index].front();
        if (candidates[candidate].size() != 1U ||
            candidates[candidate].front() != index) {
            ambiguous.insert(index);
            ambiguous.insert(candidate);
            continue;
        }
        matched[index] = true;
        matched[candidate] = true;
        const auto index_coordinate = component(
            positions.at(vertices[index].id), request.symmetry_axis);
        const auto candidate_coordinate = component(
            positions.at(vertices[candidate].id), request.symmetry_axis);
        const std::size_t negative_index = index_coordinate < request.symmetry_plane_offset
            ? index : candidate;
        const std::size_t positive_index = negative_index == index ? candidate : index;
        const auto mirrored = reflect(
            positions.at(vertices[negative_index].id),
            request.symmetry_axis,
            request.symmetry_plane_offset);
        const auto error = distance(mirrored, positions.at(vertices[positive_index].id));
        const auto& negative_feature = analysis.vertices[negative_index];
        const auto& positive_feature = analysis.vertices[positive_index];
        analysis.symmetry_pairs.push_back({
            vertices[negative_index].id,
            vertices[positive_index].id,
            error,
            negative_feature.valence == positive_feature.valence &&
                negative_feature.incident_face_count == positive_feature.incident_face_count &&
                negative_feature.boundary == positive_feature.boundary,
        });
        static_cast<void>(candidate_coordinate);
    }
    for (const auto index : ambiguous) {
        if (!on_centerline[index] && !matched[index]) {
            analysis.ambiguous_vertices.push_back(vertices[index].id);
        }
    }
    for (const auto index : unmatched) {
        if (!on_centerline[index] && !matched[index] && !ambiguous.contains(index)) {
            analysis.unmatched_vertices.push_back(vertices[index].id);
        }
    }
    std::sort(analysis.symmetry_pairs.begin(), analysis.symmetry_pairs.end(),
        [](const AutorigSymmetryPair& left, const AutorigSymmetryPair& right) {
            return std::tie(left.negative_side, left.positive_side) <
                std::tie(right.negative_side, right.positive_side);
        });
    analysis.analysis_digest = analysis_digest(analysis);
    if (auto checked = analysis.validate(); !checked) {
        return core::Result<AutorigFeatureAnalysis>::failure(checked.error());
    }
    return core::Result<AutorigFeatureAnalysis>::success(std::move(analysis));
}

} // namespace carto::character
