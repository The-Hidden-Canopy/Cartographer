#include <carto/production/multires.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace carto::production {
namespace {

constexpr std::string_view kAlgorithm = "carto.multires.cpu.triangle-midpoint-detail-map.v4";
constexpr std::string_view kEvidenceClass = "carto.cpu-reference";
using EdgeKey = std::pair<geometry::VertexId, geometry::VertexId>;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

assets::Sha256Digest hash_text(std::string_view text) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    return assets::sha256(std::span<const std::uint8_t>(bytes, text.size()));
}

std::string hex_u64(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16U, '0');
    for (std::size_t i = 0U; i < result.size(); ++i) {
        const std::size_t shift = (result.size() - i - 1U) * 4U;
        result[i] = digits[(value >> shift) & 0x0fU];
    }
    return result;
}

std::string hex_text(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::string hex_f64(double value) {
    return hex_u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}

std::string canonical_levels(const std::vector<MultiresHierarchyLevel>& levels) {
    std::string result = "CARTO_MULTIRES_LEVELS 1\n";
    result += "LEVELS " + std::to_string(levels.size()) + "\n";
    for (const MultiresHierarchyLevel& level : levels) {
        result += "LEVEL " + std::to_string(level.level) + " " +
            std::to_string(level.vertices.size()) + " " +
            std::to_string(level.faces.size()) + "\n";
        for (const MultiresHierarchyVertex& vertex : level.vertices) {
            result += "VERTEX " + std::to_string(vertex.id.value) + " " +
                hex_f64(vertex.position.x) + " " + hex_f64(vertex.position.y) + " " +
                hex_f64(vertex.position.z) + "\n";
        }
        for (const MultiresHierarchyFace& face : level.faces) {
            result += "FACE " + std::to_string(face.id.value) + " " +
                std::to_string(face.source_face.value);
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const SculptSurfaceAnchor& anchor = face.source_corners[corner];
                result += " " + std::to_string(face.vertices[corner].value) + " " +
                    std::to_string(anchor.face.value);
                for (const geometry::VertexId vertex : anchor.vertices) {
                    result += " " + std::to_string(vertex.value);
                }
                for (const double weight : anchor.barycentric) {
                    result += " " + hex_f64(weight);
                }
            }
            result.push_back('\n');
        }
    }
    return result;
}

std::string canonical_detail_classifications(
    const std::vector<MultiresDetailClassification>& classifications) {
    std::string result = "CARTO_MULTIRES_DETAIL_CLASSIFICATIONS 2\n";
    result += "RECORDS " + std::to_string(classifications.size()) + "\n";
    for (const MultiresDetailClassification& item : classifications) {
        result += "DETAIL " + std::to_string(item.source_face.value);
        for (const geometry::VertexId vertex : item.source_vertices) {
            result += " " + std::to_string(vertex.value);
        }
        result += " " + hex_f64(item.estimated_frequency) + " " +
            hex_f64(item.rms_normal_displacement) + " " +
            hex_f64(item.rms_tangential_displacement) + " " +
            hex_f64(item.runtime_importance) + " " +
            (item.boundary_sensitive ? "1" : "0") + " " +
            std::to_string(static_cast<std::uint32_t>(item.target));
        for (const core::Vec3d sample : item.displacement_vectors) {
            result += " " + hex_f64(sample.x) + " " + hex_f64(sample.y) + " " +
                hex_f64(sample.z);
        }
        result += " " + hex_f64(item.object_space_normal.x) + " " +
            hex_f64(item.object_space_normal.y) + " " +
            hex_f64(item.object_space_normal.z);
        result.push_back('\n');
    }
    return result;
}

std::string canonical_source_reconstruction_errors(
    const std::vector<MultiresLevelSourceError>& errors) {
    std::string result = "CARTO_MULTIRES_SOURCE_RECONSTRUCTION_ERRORS 1\n";
    result += "LEVELS " + std::to_string(errors.size()) + "\n";
    for (const MultiresLevelSourceError& error : errors) {
        result += "SOURCE_ERROR " + std::to_string(error.level) + " " +
            std::to_string(error.sample_count) + " " +
            hex_f64(error.maximum_anchor_reconstruction_error) + " " +
            hex_f64(error.rms_anchor_reconstruction_error) + "\n";
    }
    return result;
}

std::string artifact_canonical(
    const BakeMeshBinding& source,
    const std::string& algorithm,
    const std::string& evidence_class,
    const assets::Sha256Digest& settings_digest,
    const assets::Sha256Digest& output_digest,
    std::uint64_t byte_size) {
    return "CARTO_MULTIRES_ARTIFACT 1\nSOURCE " + hex_text(source.identity) + " " +
        std::to_string(source.revision.value()) + " " + source.content_digest.hex() +
        "\nALGORITHM " + hex_text(algorithm) + "\nEVIDENCE " +
        hex_text(evidence_class) + "\nSETTINGS " + settings_digest.hex() +
        "\nOUTPUT " + output_digest.hex() + "\nBYTES " + std::to_string(byte_size) + "\n";
}

EdgeKey edge_key(geometry::VertexId a, geometry::VertexId b) {
    return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}

std::optional<SculptSurfaceAnchor> interpolate_anchor(
    const SculptSurfaceAnchor& a,
    const SculptSurfaceAnchor& b) {
    if (!a.validate() || !b.validate() || a.face != b.face || a.vertices != b.vertices) {
        return std::nullopt;
    }
    SculptSurfaceAnchor result = a;
    for (std::size_t i = 0U; i < result.barycentric.size(); ++i) {
        result.barycentric[i] = a.barycentric[i] * 0.5 + b.barycentric[i] * 0.5;
    }
    if (!result.validate()) return std::nullopt;
    return result;
}

std::optional<geometry::FaceId> take_face_id(
    std::uint64_t& next,
    bool& exhausted) {
    if (exhausted) return std::nullopt;
    const geometry::FaceId result{next};
    if (next == std::numeric_limits<std::uint64_t>::max()) exhausted = true;
    else ++next;
    return result;
}

std::optional<geometry::VertexId> take_vertex_id(
    std::uint64_t& next,
    bool& exhausted) {
    if (exhausted) return std::nullopt;
    const geometry::VertexId result{next};
    if (next == std::numeric_limits<std::uint64_t>::max()) exhausted = true;
    else ++next;
    return result;
}

bool fan_triangulation_preserves_face_orientation(
    const geometry::Face& face,
    const geometry::EditableMesh& mesh) {
    if (face.vertices.size() < 3U) return false;
    const geometry::Vertex* origin = mesh.find_vertex(face.vertices[0]);
    if (origin == nullptr) return false;

    core::Vec3d polygon_normal{};
    std::vector<core::Vec3d> triangle_normals;
    triangle_normals.reserve(face.vertices.size() - 2U);
    for (std::size_t index = 1U; index + 1U < face.vertices.size(); ++index) {
        const geometry::Vertex* first = mesh.find_vertex(face.vertices[index]);
        const geometry::Vertex* second = mesh.find_vertex(face.vertices[index + 1U]);
        if (first == nullptr || second == nullptr) return false;
        const core::Vec3d normal = core::cross(
            first->position - origin->position,
            second->position - origin->position);
        if (!normal.finite()) return false;
        triangle_normals.push_back(normal);
        polygon_normal = polygon_normal + normal;
        if (!polygon_normal.finite()) return false;
    }

    const double polygon_normal_length = polygon_normal.length();
    if (!std::isfinite(polygon_normal_length) || polygon_normal_length == 0.0) return false;
    constexpr double kOrientationTolerance =
        64.0 * std::numeric_limits<double>::epsilon();
    for (const core::Vec3d normal : triangle_normals) {
        const double triangle_normal_length = normal.length();
        const double alignment = core::dot(normal, polygon_normal);
        if (!std::isfinite(triangle_normal_length) || triangle_normal_length == 0.0 ||
            !std::isfinite(alignment) ||
            alignment <= kOrientationTolerance * triangle_normal_length * polygon_normal_length) {
            return false;
        }
    }
    return true;
}

bool valid_detail_target(MultiresDetailTarget target) {
    switch (target) {
    case MultiresDetailTarget::unchanged:
    case MultiresDetailTarget::geometry:
    case MultiresDetailTarget::displacement:
    case MultiresDetailTarget::normal_field:
        return true;
    }
    return false;
}

bool zero_vector(core::Vec3d value) {
    return value.x == 0.0 && value.y == 0.0 && value.z == 0.0;
}

bool unit_vector(core::Vec3d value) {
    return value.finite() && std::abs(value.length_squared() - 1.0) <= 1e-9;
}

core::Result<std::vector<MultiresDetailClassification>> classify_multires_detail(
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const MultiresSettings& settings) {
    if (auto result = settings.validate(); !result) {
        return core::Result<std::vector<MultiresDetailClassification>>::failure(result.error());
    }
    if (auto result = sculpted.validate(base_mesh); !result) {
        return core::Result<std::vector<MultiresDetailClassification>>::failure(result.error());
    }
    const auto compiled = base_mesh.compile();
    if (!compiled) return core::Result<std::vector<MultiresDetailClassification>>::failure(
        compiled.error());
    const auto topology = base_mesh.topology();
    if (!topology) return core::Result<std::vector<MultiresDetailClassification>>::failure(
        topology.error());

    std::map<geometry::VertexId, std::size_t> compiled_indices;
    for (std::size_t index = 0U; index < compiled.value().vertex_ids.size(); ++index) {
        compiled_indices.emplace(compiled.value().vertex_ids[index], index);
    }
    std::map<geometry::VertexId, core::Vec3d> evaluated_positions;
    for (const geometry::Vertex& vertex : sculpted.vertices) {
        evaluated_positions.emplace(vertex.id, vertex.position);
    }
    std::set<EdgeKey> boundary_edges;
    for (const geometry::EdgeRecord& edge : topology.value().edges) {
        if (!edge.second_half_edge.has_value()) {
            boundary_edges.insert(edge_key(edge.first, edge.second));
        }
    }

    const auto faces = base_mesh.faces_sorted();
    std::vector<MultiresDetailClassification> result;
    result.reserve(compiled.value().triangle_faces.size());
    for (const geometry::Face& face : faces) {
        if (!fan_triangulation_preserves_face_orientation(face, base_mesh)) {
            return core::Result<std::vector<MultiresDetailClassification>>::failure(unsupported(
                "multires detail classification requires valid deterministic source fan triangles"));
        }
        for (std::size_t triangle = 1U; triangle + 1U < face.vertices.size(); ++triangle) {
            const std::array<geometry::VertexId, 3> ids{
                face.vertices[0], face.vertices[triangle], face.vertices[triangle + 1U]};
            std::array<core::Vec3d, 3> positions{};
            std::array<core::Vec3d, 3> deformed_triangle_positions{};
            std::array<core::Vec3d, 3> source_normals{};
            std::array<double, 3> normal_displacements{};
            std::array<double, 3> edge_lengths{};
            double normal_energy = 0.0;
            double tangential_energy = 0.0;
            bool boundary_sensitive = false;
            for (std::size_t corner = 0U; corner < ids.size(); ++corner) {
                const auto source_index = compiled_indices.find(ids[corner]);
                const auto evaluated = evaluated_positions.find(ids[corner]);
                const geometry::Vertex* source_vertex = base_mesh.find_vertex(ids[corner]);
                if (source_index == compiled_indices.end() || evaluated == evaluated_positions.end() ||
                    source_vertex == nullptr) {
                    return core::Result<std::vector<MultiresDetailClassification>>::failure(stale(
                        "multires detail classification source vertex is missing"));
                }
                positions[corner] = source_vertex->position;
                deformed_triangle_positions[corner] = evaluated->second;
                const core::Vec3d normal = compiled.value().normals[source_index->second].normalized();
                if (!normal.finite()) {
                    return core::Result<std::vector<MultiresDetailClassification>>::failure(validation(
                        "multires detail classification source normal is degenerate"));
                }
                source_normals[corner] = normal;
                const core::Vec3d displacement = evaluated->second - source_vertex->position;
                normal_displacements[corner] = core::dot(displacement, normal);
                const core::Vec3d tangent = displacement - normal * normal_displacements[corner];
                normal_energy += normal_displacements[corner] * normal_displacements[corner];
                tangential_energy += tangent.length_squared();
            }
            for (std::size_t edge = 0U; edge < 3U; ++edge) {
                const std::size_t next = (edge + 1U) % 3U;
                edge_lengths[edge] = (positions[edge] - positions[next]).length();
                if (!std::isfinite(edge_lengths[edge]) || edge_lengths[edge] <= 0.0) {
                    return core::Result<std::vector<MultiresDetailClassification>>::failure(validation(
                        "multires detail classification contains a degenerate source edge"));
                }
                boundary_sensitive = boundary_sensitive ||
                    boundary_edges.contains(edge_key(ids[edge], ids[next]));
            }
            const double rms_normal = std::sqrt(normal_energy / 3.0);
            const double rms_tangential = std::sqrt(tangential_energy / 3.0);
            const double edge_scale = (edge_lengths[0] + edge_lengths[1] + edge_lengths[2]) / 3.0;
            double variation_energy = 0.0;
            for (std::size_t edge = 0U; edge < 3U; ++edge) {
                const std::size_t next = (edge + 1U) % 3U;
                const double difference = normal_displacements[edge] - normal_displacements[next];
                variation_energy += difference * difference;
            }
            const double rms_variation = std::sqrt(variation_energy / 3.0);
            const double frequency = rms_normal <= settings.minimum_detail_displacement ? 0.0 :
                rms_variation / (edge_scale * rms_normal);
            if (!std::isfinite(rms_normal) || !std::isfinite(rms_tangential) ||
                !std::isfinite(frequency)) {
                return core::Result<std::vector<MultiresDetailClassification>>::failure(validation(
                    "multires detail classification produced non-finite metrics"));
            }

            MultiresDetailTarget target = MultiresDetailTarget::unchanged;
            const bool has_detail = rms_normal > settings.minimum_detail_displacement ||
                rms_tangential > settings.minimum_detail_displacement;
            if (has_detail) {
                if (rms_tangential > settings.minimum_detail_displacement ||
                    settings.runtime_importance >= settings.geometry_importance_threshold ||
                    (settings.preserve_boundary_geometry && boundary_sensitive)) {
                    target = MultiresDetailTarget::geometry;
                } else if (frequency <= settings.displacement_frequency_threshold) {
                    target = MultiresDetailTarget::displacement;
                } else {
                    target = MultiresDetailTarget::normal_field;
                }
            }
            MultiresDetailClassification classification{
                face.id, ids, frequency, rms_normal, rms_tangential,
                settings.runtime_importance, boundary_sensitive, target, {}, {}};
            if (target == MultiresDetailTarget::displacement) {
                for (std::size_t corner = 0U; corner < ids.size(); ++corner) {
                    classification.displacement_vectors[corner] =
                        source_normals[corner] * normal_displacements[corner];
                }
            } else if (target == MultiresDetailTarget::normal_field) {
                const core::Vec3d normal = core::cross(
                    deformed_triangle_positions[1] - deformed_triangle_positions[0],
                    deformed_triangle_positions[2] - deformed_triangle_positions[0]).normalized();
                if (!unit_vector(normal)) {
                    return core::Result<std::vector<MultiresDetailClassification>>::failure(
                        validation("multires normal-field triangle is degenerate"));
                }
                classification.object_space_normal = normal;
            }
            result.push_back(std::move(classification));
        }
    }
    return core::Result<std::vector<MultiresDetailClassification>>::success(std::move(result));
}

core::Result<std::vector<MultiresHierarchyLevel>> build_levels(
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const MultiresSettings& settings) {
    if (auto result = settings.validate(); !result) {
        return core::Result<std::vector<MultiresHierarchyLevel>>::failure(result.error());
    }
    if (auto result = sculpted.validate(base_mesh); !result) {
        return core::Result<std::vector<MultiresHierarchyLevel>>::failure(result.error());
    }
    const auto source_faces = base_mesh.faces_sorted();
    if (source_faces.empty()) {
        return core::Result<std::vector<MultiresHierarchyLevel>>::failure(validation(
            "multires requires at least one source polygon"));
    }
    if (sculpted.vertices.size() > kMaxMultiresHierarchyVertices) {
        return core::Result<std::vector<MultiresHierarchyLevel>>::failure(unsupported(
            "multires source vertex count exceeds the hierarchy budget"));
    }

    std::uint64_t triangle_count = 0U;
    std::uint64_t max_face_id = 0U;
    for (const geometry::Face& face : source_faces) {
        if (face.vertices.size() < 3U) {
            return core::Result<std::vector<MultiresHierarchyLevel>>::failure(validation(
                "multires source polygon has fewer than three corners"));
        }
        if (!fan_triangulation_preserves_face_orientation(face, base_mesh)) {
            return core::Result<std::vector<MultiresHierarchyLevel>>::failure(unsupported(
                "multires source polygon cannot be represented by its deterministic fan without inverted or degenerate triangles"));
        }
        const std::uint64_t additions = static_cast<std::uint64_t>(face.vertices.size() - 2U);
        if (additions > kMaxMultiresHierarchyFaces ||
            triangle_count > kMaxMultiresHierarchyFaces - additions) {
            return core::Result<std::vector<MultiresHierarchyLevel>>::failure(unsupported(
                "multires source triangulation exceeds the hierarchy face budget"));
        }
        triangle_count += additions;
        max_face_id = std::max(max_face_id, face.id.value);
    }
    std::uint64_t total_face_records = triangle_count;
    std::uint64_t total_vertex_records = static_cast<std::uint64_t>(sculpted.vertices.size());
    if (total_face_records > kMaxMultiresHierarchyFaces ||
        total_vertex_records > kMaxMultiresHierarchyVertices) {
        return core::Result<std::vector<MultiresHierarchyLevel>>::failure(unsupported(
            "multires base level exceeds the hierarchy budget"));
    }

    std::uint64_t next_face_id = max_face_id;
    bool face_ids_exhausted = next_face_id == std::numeric_limits<std::uint64_t>::max();
    if (!face_ids_exhausted) ++next_face_id;
    std::vector<MultiresHierarchyLevel> levels;
    levels.reserve(static_cast<std::size_t>(settings.subdivision_levels) + 1U);
    MultiresHierarchyLevel base_level;
    base_level.level = 0U;
    base_level.vertices.reserve(sculpted.vertices.size());
    for (const geometry::Vertex& vertex : sculpted.vertices) {
        if (!vertex.position.finite()) {
            return core::Result<std::vector<MultiresHierarchyLevel>>::failure(validation(
                "multires source evaluation contains a non-finite position"));
        }
        base_level.vertices.push_back({vertex.id, vertex.position});
    }
    std::sort(base_level.vertices.begin(), base_level.vertices.end(),
        [](const auto& a, const auto& b) { return a.id < b.id; });

    std::map<geometry::VertexId, core::Vec3d> positions;
    for (const MultiresHierarchyVertex& vertex : base_level.vertices) {
        positions.emplace(vertex.id, vertex.position);
    }
    for (const geometry::Face& source_face : source_faces) {
        const std::size_t last = source_face.vertices.size() - 1U;
        for (std::size_t triangle = 1U; triangle < last; ++triangle) {
            std::array<geometry::VertexId, 3> vertices{
                source_face.vertices[0], source_face.vertices[triangle],
                source_face.vertices[triangle + 1U]};
            MultiresHierarchyFace face;
            if (triangle == 1U) face.id = source_face.id;
            else {
                const auto id = take_face_id(next_face_id, face_ids_exhausted);
                if (!id) return core::Result<std::vector<MultiresHierarchyLevel>>::failure(
                    unsupported("multires face identity space is exhausted"));
                face.id = *id;
            }
            face.source_face = source_face.id;
            face.vertices = vertices;
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const geometry::VertexId a = vertices[0];
                const geometry::VertexId b = vertices[1];
                const geometry::VertexId c = vertices[2];
                face.source_corners[corner] = SculptSurfaceAnchor{
                    source_face.id, {a, b, c},
                    {corner == 0U ? 1.0 : 0.0,
                     corner == 1U ? 1.0 : 0.0,
                     corner == 2U ? 1.0 : 0.0}};
            }
            base_level.faces.push_back(std::move(face));
        }
    }
    std::sort(base_level.faces.begin(), base_level.faces.end(),
        [](const auto& a, const auto& b) { return a.id < b.id; });
    levels.push_back(std::move(base_level));

    std::uint64_t max_vertex_id = 0U;
    for (const MultiresHierarchyVertex& vertex : levels.front().vertices) {
        max_vertex_id = std::max(max_vertex_id, vertex.id.value);
    }
    std::uint64_t next_vertex_id = max_vertex_id;
    bool vertex_ids_exhausted = next_vertex_id == std::numeric_limits<std::uint64_t>::max();
    if (!vertex_ids_exhausted) ++next_vertex_id;

    for (std::uint32_t level_index = 1U;
         level_index <= settings.subdivision_levels; ++level_index) {
        const MultiresHierarchyLevel& previous = levels.back();
        if (previous.faces.size() > kMaxMultiresHierarchyFaces / 4U ||
            total_face_records > kMaxMultiresHierarchyFaces -
                static_cast<std::uint64_t>(previous.faces.size()) * 4U) {
            return core::Result<std::vector<MultiresHierarchyLevel>>::failure(unsupported(
                "multires subdivision exceeds the hierarchy face budget"));
        }
        std::set<EdgeKey> edges;
        for (const MultiresHierarchyFace& face : previous.faces) {
            edges.insert(edge_key(face.vertices[0], face.vertices[1]));
            edges.insert(edge_key(face.vertices[1], face.vertices[2]));
            edges.insert(edge_key(face.vertices[2], face.vertices[0]));
        }
        const std::uint64_t next_level_vertex_count =
            static_cast<std::uint64_t>(previous.vertices.size()) +
            static_cast<std::uint64_t>(edges.size());
        if (next_level_vertex_count > kMaxMultiresHierarchyVertices ||
            total_vertex_records > kMaxMultiresHierarchyVertices - next_level_vertex_count) {
            return core::Result<std::vector<MultiresHierarchyLevel>>::failure(unsupported(
                "multires subdivision exceeds the hierarchy vertex budget"));
        }
        std::map<EdgeKey, geometry::VertexId> midpoint_ids;
        std::vector<MultiresHierarchyVertex> vertices = previous.vertices;
        vertices.reserve(vertices.size() + edges.size());
        for (const EdgeKey& edge : edges) {
            const auto a = positions.find(edge.first);
            const auto b = positions.find(edge.second);
            if (a == positions.end() || b == positions.end()) {
                return core::Result<std::vector<MultiresHierarchyLevel>>::failure(validation(
                    "multires face references a missing input vertex"));
            }
            const auto id = take_vertex_id(next_vertex_id, vertex_ids_exhausted);
            if (!id) return core::Result<std::vector<MultiresHierarchyLevel>>::failure(
                unsupported("multires vertex identity space is exhausted"));
            const core::Vec3d midpoint{
                a->second.x * 0.5 + b->second.x * 0.5,
                a->second.y * 0.5 + b->second.y * 0.5,
                a->second.z * 0.5 + b->second.z * 0.5};
            if (!midpoint.finite()) {
                return core::Result<std::vector<MultiresHierarchyLevel>>::failure(validation(
                    "multires midpoint calculation produced a non-finite position"));
            }
            midpoint_ids.emplace(edge, *id);
            vertices.push_back({*id, midpoint});
            positions.emplace(*id, midpoint);
        }
        std::sort(vertices.begin(), vertices.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });

        MultiresHierarchyLevel next_level;
        next_level.level = level_index;
        next_level.vertices = std::move(vertices);
        next_level.faces.reserve(previous.faces.size() * 4U);
        for (const MultiresHierarchyFace& parent : previous.faces) {
            const auto m01_id = midpoint_ids.at(edge_key(parent.vertices[0], parent.vertices[1]));
            const auto m12_id = midpoint_ids.at(edge_key(parent.vertices[1], parent.vertices[2]));
            const auto m20_id = midpoint_ids.at(edge_key(parent.vertices[2], parent.vertices[0]));
            const auto m01_anchor = interpolate_anchor(
                parent.source_corners[0], parent.source_corners[1]);
            const auto m12_anchor = interpolate_anchor(
                parent.source_corners[1], parent.source_corners[2]);
            const auto m20_anchor = interpolate_anchor(
                parent.source_corners[2], parent.source_corners[0]);
            if (!m01_anchor || !m12_anchor || !m20_anchor) {
                return core::Result<std::vector<MultiresHierarchyLevel>>::failure(validation(
                    "multires subdivision cannot preserve a common source triangle anchor"));
            }
            std::array<MultiresHierarchyFace, 4> children{};
            children[0] = {parent.id, parent.source_face,
                {parent.vertices[0], m01_id, m20_id},
                {parent.source_corners[0], *m01_anchor, *m20_anchor}};
            const auto child1 = take_face_id(next_face_id, face_ids_exhausted);
            const auto child2 = take_face_id(next_face_id, face_ids_exhausted);
            const auto child3 = take_face_id(next_face_id, face_ids_exhausted);
            if (!child1 || !child2 || !child3) {
                return core::Result<std::vector<MultiresHierarchyLevel>>::failure(unsupported(
                    "multires face identity space is exhausted"));
            }
            children[1] = {*child1, parent.source_face,
                {m01_id, parent.vertices[1], m12_id},
                {*m01_anchor, parent.source_corners[1], *m12_anchor}};
            children[2] = {*child2, parent.source_face,
                {m20_id, m12_id, parent.vertices[2]},
                {*m20_anchor, *m12_anchor, parent.source_corners[2]}};
            children[3] = {*child3, parent.source_face,
                {m01_id, m12_id, m20_id},
                {*m01_anchor, *m12_anchor, *m20_anchor}};
            next_level.faces.insert(next_level.faces.end(), children.begin(), children.end());
        }
        std::sort(next_level.faces.begin(), next_level.faces.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
        total_face_records += static_cast<std::uint64_t>(next_level.faces.size());
        total_vertex_records += static_cast<std::uint64_t>(next_level.vertices.size());
        levels.push_back(std::move(next_level));
    }
    return core::Result<std::vector<MultiresHierarchyLevel>>::success(std::move(levels));
}

core::Result<std::vector<MultiresLevelSourceError>> measure_source_reconstruction_errors(
    const std::vector<MultiresHierarchyLevel>& levels,
    const SculptEvaluationArtifact& sculpted) {
    std::map<geometry::VertexId, core::Vec3d> sculpt_positions;
    for (const geometry::Vertex& vertex : sculpted.vertices) {
        if (!vertex.id || !vertex.position.finite() ||
            !sculpt_positions.emplace(vertex.id, vertex.position).second) {
            return core::Result<std::vector<MultiresLevelSourceError>>::failure(validation(
                "multires source error measurement received invalid sculpt vertices"));
        }
    }

    const auto anchor_position = [&sculpt_positions](const SculptSurfaceAnchor& anchor)
        -> core::Result<core::Vec3d> {
        if (!anchor.validate()) return core::Result<core::Vec3d>::failure(validation(
            "multires source error measurement received an invalid anchor"));
        core::Vec3d result{};
        for (std::size_t corner = 0U; corner < anchor.vertices.size(); ++corner) {
            const auto source = sculpt_positions.find(anchor.vertices[corner]);
            if (source == sculpt_positions.end()) {
                return core::Result<core::Vec3d>::failure(stale(
                    "multires source error anchor references a missing sculpt vertex"));
            }
            result = result + source->second * anchor.barycentric[corner];
        }
        if (!result.finite()) return core::Result<core::Vec3d>::failure(validation(
            "multires source error anchor produced a non-finite position"));
        return core::Result<core::Vec3d>::success(result);
    };
    const auto error_distance = [](core::Vec3d first, core::Vec3d second) {
        return std::hypot(first.x - second.x, first.y - second.y, first.z - second.z);
    };
    const auto tolerance_for = [](core::Vec3d position) {
        const double scale = std::max({1.0, std::abs(position.x),
            std::abs(position.y), std::abs(position.z)});
        return scale * 1e-9;
    };

    std::vector<MultiresLevelSourceError> result;
    result.reserve(levels.size());
    for (std::size_t level_index = 0U; level_index < levels.size(); ++level_index) {
        const MultiresHierarchyLevel& level = levels[level_index];
        if (level.level != level_index || level.vertices.empty()) {
            return core::Result<std::vector<MultiresLevelSourceError>>::failure(validation(
                "multires source error levels are empty or unordered"));
        }
        std::map<geometry::VertexId, core::Vec3d> level_positions;
        for (const MultiresHierarchyVertex& vertex : level.vertices) {
            if (!vertex.id || !vertex.position.finite() ||
                !level_positions.emplace(vertex.id, vertex.position).second) {
                return core::Result<std::vector<MultiresLevelSourceError>>::failure(validation(
                    "multires source error level contains an invalid vertex"));
            }
        }

        std::map<geometry::VertexId, core::Vec3d> expected_positions;
        for (const MultiresHierarchyFace& face : level.faces) {
            for (std::size_t corner = 0U; corner < face.vertices.size(); ++corner) {
                if (!level_positions.contains(face.vertices[corner])) {
                    return core::Result<std::vector<MultiresLevelSourceError>>::failure(stale(
                        "multires source error face references a missing level vertex"));
                }
                const auto expected = anchor_position(face.source_corners[corner]);
                if (!expected) {
                    return core::Result<std::vector<MultiresLevelSourceError>>::failure(
                        expected.error());
                }
                const auto [found, inserted] = expected_positions.emplace(
                    face.vertices[corner], expected.value());
                if (!inserted) {
                    const double anchor_disagreement = error_distance(
                        found->second, expected.value());
                    if (!std::isfinite(anchor_disagreement) ||
                        anchor_disagreement > tolerance_for(found->second)) {
                        return core::Result<std::vector<MultiresLevelSourceError>>::failure(
                            validation("multires shared vertex has conflicting source anchors"));
                    }
                }
            }
        }

        double error_scale = 0.0;
        double scaled_squared_error_sum = 0.0;
        double maximum_error = 0.0;
        for (const auto& [vertex_id, position] : level_positions) {
            const auto anchored = expected_positions.find(vertex_id);
            const auto direct_source = sculpt_positions.find(vertex_id);
            if (anchored == expected_positions.end() && direct_source == sculpt_positions.end()) {
                return core::Result<std::vector<MultiresLevelSourceError>>::failure(stale(
                    "multires level vertex has no rebuildable sculpt source anchor"));
            }
            const core::Vec3d& expected = anchored != expected_positions.end() ?
                anchored->second : direct_source->second;
            const double error = error_distance(position, expected);
            if (!std::isfinite(error) || error > tolerance_for(expected)) {
                return core::Result<std::vector<MultiresLevelSourceError>>::failure(validation(
                    "multires level vertex exceeds its sculpt source reconstruction tolerance"));
            }
            maximum_error = std::max(maximum_error, error);
            if (error > error_scale) {
                const double ratio = error_scale / error;
                scaled_squared_error_sum = 1.0 + scaled_squared_error_sum * ratio * ratio;
                error_scale = error;
            } else if (error_scale > 0.0) {
                const double ratio = error / error_scale;
                scaled_squared_error_sum += ratio * ratio;
            }
        }
        const double rms_error = error_scale == 0.0 ? 0.0 : error_scale *
            std::sqrt(scaled_squared_error_sum /
                static_cast<double>(level_positions.size()));
        result.push_back(MultiresLevelSourceError{
            level.level, static_cast<std::uint64_t>(level_positions.size()),
            maximum_error, rms_error});
    }
    return core::Result<std::vector<MultiresLevelSourceError>>::success(std::move(result));
}

} // namespace

core::Result<void> MultiresSettings::validate() const {
    if (subdivision_levels > kMaxMultiresSubdivisionLevels ||
        !std::isfinite(runtime_importance) || runtime_importance < 0.0 || runtime_importance > 1.0 ||
        !std::isfinite(geometry_importance_threshold) || geometry_importance_threshold < 0.0 ||
        geometry_importance_threshold > 1.0 ||
        !std::isfinite(displacement_frequency_threshold) ||
        displacement_frequency_threshold <= 0.0 || displacement_frequency_threshold > 1e9 ||
        !std::isfinite(minimum_detail_displacement) || minimum_detail_displacement <= 0.0 ||
        minimum_detail_displacement > 1e3) {
        return core::Result<void>::failure(invalid(
            "multires subdivision or detail classification settings exceed their bounds"));
    }
    return core::Result<void>::success();
}

std::string MultiresSettings::canonical() const {
    if (!validate()) return {};
    return "CARTO_MULTIRES_SETTINGS 2\nLEVELS " +
        std::to_string(subdivision_levels) + "\nIMPORTANCE " +
        hex_f64(runtime_importance) + "\nGEOMETRY_THRESHOLD " +
        hex_f64(geometry_importance_threshold) + "\nDISPLACEMENT_FREQUENCY " +
        hex_f64(displacement_frequency_threshold) + "\nMINIMUM_DISPLACEMENT " +
        hex_f64(minimum_detail_displacement) + "\nPRESERVE_BOUNDARY " +
        (preserve_boundary_geometry ? "1\n" : "0\n");
}

core::Result<assets::Sha256Digest> MultiresSettings::digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(canonical()));
}

std::string MultiresSurfaceHierarchy::canonical() const {
    const std::string output = canonical_levels(levels) +
        canonical_detail_classifications(detail_classifications) +
        canonical_source_reconstruction_errors(source_reconstruction_errors);
    return artifact_canonical(source, algorithm, evidence_class, settings_digest,
        output_digest, byte_size) + "SETTINGS_VALUE " + settings.canonical() + output;
}

core::Result<void> MultiresSurfaceHierarchy::refresh_digests() {
    if (auto result = source.validate(); !result) return result;
    if (auto result = settings.validate(); !result) return result;
    if (algorithm != kAlgorithm || evidence_class != kEvidenceClass || levels.empty() ||
        levels.size() > static_cast<std::size_t>(kMaxMultiresSubdivisionLevels) + 1U ||
        detail_classifications.empty() ||
        detail_classifications.size() > kMaxMultiresHierarchyFaces ||
        source_reconstruction_errors.size() != levels.size()) {
        return core::Result<void>::failure(validation(
            "multires algorithm, evidence class, hierarchy, or detail list is invalid"));
    }
    const auto settings_hash = settings.digest();
    if (!settings_hash) return core::Result<void>::failure(settings_hash.error());
    settings_digest = settings_hash.value();
    for (const MultiresDetailClassification& item : detail_classifications) {
        if (!item.source_face ||
            std::any_of(item.source_vertices.begin(), item.source_vertices.end(),
                [](geometry::VertexId id) { return !id; }) ||
            !std::isfinite(item.estimated_frequency) || item.estimated_frequency < 0.0 ||
            !std::isfinite(item.rms_normal_displacement) ||
            item.rms_normal_displacement < 0.0 ||
            !std::isfinite(item.rms_tangential_displacement) ||
            item.rms_tangential_displacement < 0.0 ||
            item.runtime_importance != settings.runtime_importance ||
            !valid_detail_target(item.target)) {
            return core::Result<void>::failure(validation(
                "multires detail classification record is invalid"));
        }
        if (!item.object_space_normal.finite()) {
            return core::Result<void>::failure(validation(
                "multires detail field contains a non-finite normal sample"));
        }
        for (std::size_t corner = 0U; corner < item.source_vertices.size(); ++corner) {
            const core::Vec3d displacement = item.displacement_vectors[corner];
            if (!displacement.finite()) {
                return core::Result<void>::failure(validation(
                    "multires detail field contains a non-finite displacement sample"));
            }
            if (item.target == MultiresDetailTarget::displacement) {
                if (!zero_vector(item.object_space_normal)) {
                    return core::Result<void>::failure(validation(
                        "displacement detail must not contain a normal-field sample"));
                }
            } else if (item.target == MultiresDetailTarget::normal_field) {
                if (!zero_vector(displacement) || !unit_vector(item.object_space_normal)) {
                    return core::Result<void>::failure(validation(
                        "normal-field detail must contain one unit object-space normal"));
                }
            } else if (!zero_vector(displacement) || !zero_vector(item.object_space_normal)) {
                return core::Result<void>::failure(validation(
                    "non-migrated detail must not contain displacement or normal samples"));
            }
        }
    }
    for (std::size_t index = 0U; index < source_reconstruction_errors.size(); ++index) {
        const MultiresLevelSourceError& error = source_reconstruction_errors[index];
        if (error.level != index || error.sample_count == 0U ||
            error.sample_count != levels[index].vertices.size() ||
            !std::isfinite(error.maximum_anchor_reconstruction_error) ||
            error.maximum_anchor_reconstruction_error < 0.0 ||
            !std::isfinite(error.rms_anchor_reconstruction_error) ||
            error.rms_anchor_reconstruction_error < 0.0 ||
            error.rms_anchor_reconstruction_error > error.maximum_anchor_reconstruction_error) {
            return core::Result<void>::failure(validation(
                "multires source reconstruction error record is invalid"));
        }
    }
    const std::string output = canonical_levels(levels) +
        canonical_detail_classifications(detail_classifications) +
        canonical_source_reconstruction_errors(source_reconstruction_errors);
    if (output.size() > kMaxMultiresHierarchyBytes) {
        return core::Result<void>::failure(unsupported(
            "multires canonical output exceeds the byte budget"));
    }
    output_digest = hash_text(output);
    byte_size = static_cast<std::uint64_t>(output.size());
    artifact_digest = hash_text(artifact_canonical(source, algorithm, evidence_class,
        settings_digest, output_digest, byte_size));
    return core::Result<void>::success();
}

core::Result<void> MultiresSurfaceHierarchy::validate(
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh) const {
    if (auto result = sculpted.validate(base_mesh); !result) return result;
    if (source != sculpted.stack.source) {
        return core::Result<void>::failure(stale(
            "multires source binding differs from its SculptStack evaluation"));
    }
    if (algorithm != kAlgorithm || evidence_class != kEvidenceClass) {
        return core::Result<void>::failure(validation(
            "multires algorithm or evidence class is not recognized"));
    }
    const auto rebuilt_levels = build_levels(sculpted, base_mesh, settings);
    if (!rebuilt_levels) return core::Result<void>::failure(rebuilt_levels.error());
    if (canonical_levels(levels) != canonical_levels(rebuilt_levels.value())) {
        return core::Result<void>::failure(validation(
            "multires levels differ from deterministic source reconstruction"));
    }
    const auto rebuilt_detail = classify_multires_detail(sculpted, base_mesh, settings);
    if (!rebuilt_detail) return core::Result<void>::failure(rebuilt_detail.error());
    if (canonical_detail_classifications(detail_classifications) !=
        canonical_detail_classifications(rebuilt_detail.value())) {
        return core::Result<void>::failure(validation(
            "multires detail classifications differ from source reconstruction"));
    }
    const auto rebuilt_errors = measure_source_reconstruction_errors(levels, sculpted);
    if (!rebuilt_errors) return core::Result<void>::failure(rebuilt_errors.error());
    if (canonical_source_reconstruction_errors(source_reconstruction_errors) !=
        canonical_source_reconstruction_errors(rebuilt_errors.value())) {
        return core::Result<void>::failure(validation(
            "multires source reconstruction errors differ from their anchors"));
    }
    MultiresSurfaceHierarchy rebuilt = *this;
    if (auto result = rebuilt.refresh_digests(); !result) return result;
    if (rebuilt.settings_digest != settings_digest || rebuilt.output_digest != output_digest ||
        rebuilt.artifact_digest != artifact_digest || rebuilt.byte_size != byte_size) {
        return core::Result<void>::failure(validation(
            "multires digest does not match its source, settings, and hierarchy"));
    }
    for (const MultiresHierarchyLevel& level : levels) {
        for (const MultiresHierarchyFace& face : level.faces) {
            for (const SculptSurfaceAnchor& anchor : face.source_corners) {
                if (anchor.face != face.source_face || !anchor.validate()) {
                    return core::Result<void>::failure(validation(
                        "multires corner anchor is malformed or names another source face"));
                }
                const auto resolved = resolve_sculpt_surface_anchor(base_mesh, anchor);
                if (!resolved) return core::Result<void>::failure(resolved.error());
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<MultiresSurfaceHierarchy> compile_multires_surface_hierarchy(
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const MultiresSettings& settings) {
    const auto levels = build_levels(sculpted, base_mesh, settings);
    if (!levels) {
        return core::Result<MultiresSurfaceHierarchy>::failure(levels.error());
    }
    const auto detail = classify_multires_detail(sculpted, base_mesh, settings);
    if (!detail) {
        return core::Result<MultiresSurfaceHierarchy>::failure(detail.error());
    }
    const auto source_errors = measure_source_reconstruction_errors(levels.value(), sculpted);
    if (!source_errors) {
        return core::Result<MultiresSurfaceHierarchy>::failure(source_errors.error());
    }
    MultiresSurfaceHierarchy result;
    result.source = sculpted.stack.source;
    result.settings = settings;
    result.levels = std::move(levels).value();
    result.detail_classifications = std::move(detail).value();
    result.source_reconstruction_errors = std::move(source_errors).value();
    if (auto refreshed = result.refresh_digests(); !refreshed) {
        return core::Result<MultiresSurfaceHierarchy>::failure(refreshed.error());
    }
    if (auto validated = result.validate(sculpted, base_mesh); !validated) {
        return core::Result<MultiresSurfaceHierarchy>::failure(validated.error());
    }
    return core::Result<MultiresSurfaceHierarchy>::success(std::move(result));
}

core::Result<ProductionCookKey> multires_production_cook_key(
    const MultiresSurfaceHierarchy& hierarchy,
    std::string platform_profile) {
    if (auto result = hierarchy.source.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    if (auto result = hierarchy.settings.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto settings_digest = hierarchy.settings.digest();
    if (!settings_digest) return core::Result<ProductionCookKey>::failure(settings_digest.error());
    if (hierarchy.algorithm != kAlgorithm || hierarchy.evidence_class != kEvidenceClass ||
        settings_digest.value() != hierarchy.settings_digest || hierarchy.output_digest.is_zero() ||
        hierarchy.artifact_digest.is_zero()) {
        return core::Result<ProductionCookKey>::failure(validation(
            "multires cook key requires a complete CPU-reference hierarchy"));
    }

    ProductionCookKey key;
    key.source = AuthoritativeSource{
        hierarchy.source.identity, hierarchy.source.revision, hierarchy.source.content_digest};
    key.kind = ArtifactKind::multires_hierarchy;
    key.form = ExecutionForm::offline;
    key.algorithm = hierarchy.algorithm;
    key.settings_digest = hierarchy.settings_digest;
    key.platform_profile = std::move(platform_profile);
    key.representation_kind = "multires.surface-hierarchy";
    key.spatial_scope = "whole-mesh";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<void> publish_multires_surface_hierarchy(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const MultiresSurfaceHierarchy& hierarchy,
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const AdmissionPolicy& policy) {
    if (auto result = hierarchy.validate(sculpted, base_mesh); !result) return result;
    const auto expected_key = multires_production_cook_key(
        hierarchy, receipt.key.platform_profile);
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "multires production receipt key differs from the hierarchy source or settings"));
    }
    if (receipt.quality.measured_error != 0.0) {
        return core::Result<void>::failure(validation(
            "exact multires triangle refinement must report zero representation error"));
    }
    const std::string payload = hierarchy.canonical();
    if (payload.empty() || payload.size() > kMaxMultiresArtifactBytes) {
        return core::Result<void>::failure(unsupported(
            "multires canonical artifact exceeds its publication bound"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
    const AuthoritativeSource current_source{
        hierarchy.source.identity, hierarchy.source.revision, hierarchy.source.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{bytes, payload.size()}, current_source, policy);
}

core::Result<MultiresSurfaceHierarchy> read_multires_surface_hierarchy(
    const ProductionArtifactStore& store,
    const SculptEvaluationArtifact& sculpted,
    const geometry::EditableMesh& base_mesh,
    const MultiresSettings& settings,
    std::string platform_profile,
    const AdmissionPolicy& policy) {
    const auto hierarchy = compile_multires_surface_hierarchy(sculpted, base_mesh, settings);
    if (!hierarchy) return hierarchy;
    const auto key = multires_production_cook_key(hierarchy.value(), std::move(platform_profile));
    if (!key) return core::Result<MultiresSurfaceHierarchy>::failure(key.error());
    const AuthoritativeSource current_source{
        hierarchy.value().source.identity, hierarchy.value().source.revision,
        hierarchy.value().source.content_digest};
    AdmissionPolicy bounded_policy = policy;
    if (!bounded_policy.max_output_bytes.has_value() ||
        *bounded_policy.max_output_bytes > kMaxMultiresArtifactBytes) {
        bounded_policy.max_output_bytes = kMaxMultiresArtifactBytes;
    }
    const auto payload = store.read(key.value(), current_source, bounded_policy);
    if (!payload) return core::Result<MultiresSurfaceHierarchy>::failure(payload.error());
    if (payload.value().empty() || payload.value().size() > kMaxMultiresArtifactBytes) {
        return core::Result<MultiresSurfaceHierarchy>::failure(validation(
            "stored multires artifact is empty or exceeds its publication bound"));
    }

    const std::string expected = hierarchy.value().canonical();
    const std::string_view stored(reinterpret_cast<const char*>(payload.value().data()),
        payload.value().size());
    if (payload.value().size() > kMaxMultiresArtifactBytes || stored != expected) {
        return core::Result<MultiresSurfaceHierarchy>::failure(validation(
            "stored multires artifact differs from deterministic source reconstruction"));
    }
    return hierarchy;
}

} // namespace carto::production
