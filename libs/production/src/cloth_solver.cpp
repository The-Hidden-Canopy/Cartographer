#include <carto/production/cloth_solver.hpp>
#include <carto/production/artifact_store.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::production {
namespace {

constexpr std::string_view kAlgorithm = "carto.cloth.cpu.xpbd-reference.v5";
constexpr std::string_view kEvidenceClass = "carto.cpu-reference";
constexpr double kMinimumRestLength = 1e-12;
constexpr std::size_t kMaxClothSerializedBytes =
    static_cast<std::size_t>(kMaxClothSimulationArtifactBytes) + 4096U;
using EdgeKey = std::pair<geometry::VertexId, geometry::VertexId>;
using EdgePairKey = std::pair<EdgeKey, EdgeKey>;
using CellKey = std::array<std::int64_t, 3>;
constexpr std::uint64_t kSelfCollisionCellBuildOperationsPerVertex = 29U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
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

std::optional<std::string> unhex_text(std::string_view encoded, std::size_t maximum_bytes) {
    if (encoded.empty() || encoded.size() % 2U != 0U || encoded.size() / 2U > maximum_bytes) {
        return std::nullopt;
    }
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(encoded.size() / 2U);
    for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
        const int high = digit(encoded[index]);
        const int low = digit(encoded[index + 1U]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

std::optional<std::uint64_t> parse_u64(std::string_view value) {
    if (value.empty()) return std::nullopt;
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<double> parse_hex_f64(std::string_view value) {
    if (value.size() != 16U) return std::nullopt;
    std::uint64_t bits = 0U;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), bits, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    const double parsed = std::bit_cast<double>(bits);
    if (!std::isfinite(parsed)) return std::nullopt;
    return parsed;
}

std::string hex_f64(double value) {
    return hex_u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}

EdgeKey edge_key(geometry::VertexId first, geometry::VertexId second) {
    return first < second ? EdgeKey{first, second} : EdgeKey{second, first};
}

EdgePairKey edge_pair_key(EdgeKey first, EdgeKey second) {
    return first < second ? EdgePairKey{first, second} : EdgePairKey{second, first};
}

double distance(core::Vec3d first, core::Vec3d second) {
    return (first - second).length();
}

struct DistanceConstraint {
    std::size_t first = 0U;
    std::size_t second = 0U;
    double rest_length = 0.0;
    double compliance = 0.0;
    double lambda = 0.0;
    core::Vec3d rest_direction{};
};

struct PinConstraint {
    std::size_t vertex = 0U;
    core::Vec3d target{};
    double compliance = 0.0;
    core::Vec3d lambda{};
};

struct ClothTriangle {
    geometry::FaceId source_face;
    std::array<std::size_t, 3> vertex_indices{};
    std::array<geometry::VertexId, 3> vertex_ids{};
    geometry::VertexId component_key;
};

struct ClothCollisionEdge {
    EdgeKey key;
    std::array<std::size_t, 2> vertex_indices{};
};

struct ClothBounds {
    core::Vec3d minimum{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    core::Vec3d maximum{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
};

struct ClothBvhNode {
    ClothBounds bounds;
    std::size_t left = std::numeric_limits<std::size_t>::max();
    std::size_t right = std::numeric_limits<std::size_t>::max();
    std::size_t begin = 0U;
    std::size_t count = 0U;

    [[nodiscard]] bool leaf() const noexcept { return count != 0U; }
};

struct PreparedCloth {
    std::vector<geometry::Vertex> source_vertices;
    std::map<geometry::VertexId, std::size_t> vertex_indices;
    std::vector<double> inverse_masses;
    std::vector<DistanceConstraint> stretch;
    std::vector<DistanceConstraint> shear;
    std::vector<DistanceConstraint> bend;
    std::vector<DistanceConstraint> seams;
    std::vector<PinConstraint> pins;
    std::set<EdgeKey> self_collision_exclusions;
    std::set<EdgeKey> self_collision_seam_vertex_pairs;
    std::set<EdgePairKey> self_collision_edge_exclusions;
    std::vector<ClothTriangle> triangles;
    std::vector<ClothCollisionEdge> collision_edges;
    std::vector<geometry::VertexId> component_keys;
    std::vector<std::size_t> triangle_order;
    std::vector<ClothBvhNode> self_collision_bvh;
    std::vector<std::size_t> edge_order;
    std::vector<ClothBvhNode> self_collision_edge_bvh;
    const std::vector<GarmentCollisionCapsule>* capsules = nullptr;
    double thickness = 0.0;
};

void include_point(ClothBounds& bounds, core::Vec3d point) {
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

void include_bounds(ClothBounds& target, const ClothBounds& source) {
    include_point(target, source.minimum);
    include_point(target, source.maximum);
}

double triangle_centroid_axis(
    const ClothTriangle& triangle,
    const std::vector<geometry::Vertex>& vertices,
    std::size_t axis) {
    const auto coordinate = [axis](core::Vec3d value) {
        if (axis == 0U) return value.x;
        if (axis == 1U) return value.y;
        return value.z;
    };
    return coordinate(vertices[triangle.vertex_indices[0]].position) / 3.0 +
        coordinate(vertices[triangle.vertex_indices[1]].position) / 3.0 +
        coordinate(vertices[triangle.vertex_indices[2]].position) / 3.0;
}

std::size_t build_collision_bvh_node(
    PreparedCloth& prepared,
    std::size_t begin,
    std::size_t end) {
    const std::size_t node_index = prepared.self_collision_bvh.size();
    prepared.self_collision_bvh.emplace_back();
    const std::size_t count = end - begin;
    if (count <= 4U) {
        prepared.self_collision_bvh[node_index].begin = begin;
        prepared.self_collision_bvh[node_index].count = count;
        return node_index;
    }

    ClothBounds centroid_bounds;
    for (std::size_t order = begin; order < end; ++order) {
        const ClothTriangle& triangle = prepared.triangles[prepared.triangle_order[order]];
        const core::Vec3d centroid =
            prepared.source_vertices[triangle.vertex_indices[0]].position * (1.0 / 3.0) +
            prepared.source_vertices[triangle.vertex_indices[1]].position * (1.0 / 3.0) +
            prepared.source_vertices[triangle.vertex_indices[2]].position * (1.0 / 3.0);
        include_point(centroid_bounds, centroid);
    }
    const std::array<double, 3> extents{
        centroid_bounds.maximum.x - centroid_bounds.minimum.x,
        centroid_bounds.maximum.y - centroid_bounds.minimum.y,
        centroid_bounds.maximum.z - centroid_bounds.minimum.z};
    std::size_t axis = 0U;
    if (extents[1] > extents[axis]) axis = 1U;
    if (extents[2] > extents[axis]) axis = 2U;
    const std::size_t middle = begin + count / 2U;
    std::nth_element(prepared.triangle_order.begin() + static_cast<std::ptrdiff_t>(begin),
        prepared.triangle_order.begin() + static_cast<std::ptrdiff_t>(middle),
        prepared.triangle_order.begin() + static_cast<std::ptrdiff_t>(end),
        [&prepared, axis](std::size_t first, std::size_t second) {
            const ClothTriangle& a = prepared.triangles[first];
            const ClothTriangle& b = prepared.triangles[second];
            const double first_centroid = triangle_centroid_axis(a, prepared.source_vertices, axis);
            const double second_centroid = triangle_centroid_axis(b, prepared.source_vertices, axis);
            if (first_centroid != second_centroid) return first_centroid < second_centroid;
            if (a.source_face != b.source_face) return a.source_face < b.source_face;
            if (a.vertex_ids != b.vertex_ids) return a.vertex_ids < b.vertex_ids;
            return first < second;
        });
    const std::size_t left = build_collision_bvh_node(prepared, begin, middle);
    const std::size_t right = build_collision_bvh_node(prepared, middle, end);
    prepared.self_collision_bvh[node_index].left = left;
    prepared.self_collision_bvh[node_index].right = right;
    return node_index;
}

core::Result<void> build_collision_bvh(PreparedCloth& prepared) {
    if (prepared.triangles.empty() ||
        prepared.triangles.size() > kMaxClothSelfCollisionTriangles) {
        return core::Result<void>::failure(unsupported(
            "cloth self-collision triangle count exceeds its bounded BVH capacity"));
    }
    ClothBounds centroid_bounds;
    for (const ClothTriangle& triangle : prepared.triangles) {
        const core::Vec3d centroid =
            prepared.source_vertices[triangle.vertex_indices[0]].position * (1.0 / 3.0) +
            prepared.source_vertices[triangle.vertex_indices[1]].position * (1.0 / 3.0) +
            prepared.source_vertices[triangle.vertex_indices[2]].position * (1.0 / 3.0);
        if (!centroid.finite()) {
            return core::Result<void>::failure(validation(
                "cloth self-collision BVH centroid is not finite"));
        }
        include_point(centroid_bounds, centroid);
    }
    const std::array<double, 3> centroid_extents{
        centroid_bounds.maximum.x - centroid_bounds.minimum.x,
        centroid_bounds.maximum.y - centroid_bounds.minimum.y,
        centroid_bounds.maximum.z - centroid_bounds.minimum.z};
    if (std::any_of(centroid_extents.begin(), centroid_extents.end(),
            [](double extent) { return !std::isfinite(extent); })) {
        return core::Result<void>::failure(validation(
            "cloth self-collision BVH centroid extent is not finite"));
    }
    prepared.triangle_order.resize(prepared.triangles.size());
    for (std::size_t i = 0U; i < prepared.triangle_order.size(); ++i) {
        prepared.triangle_order[i] = i;
    }
    prepared.self_collision_bvh.reserve(prepared.triangles.size() * 2U - 1U);
    static_cast<void>(build_collision_bvh_node(
        prepared, 0U, prepared.triangle_order.size()));
    return core::Result<void>::success();
}

double edge_centroid_axis(
    const ClothCollisionEdge& edge,
    const std::vector<geometry::Vertex>& vertices,
    std::size_t axis) {
    const core::Vec3d midpoint =
        vertices[edge.vertex_indices[0]].position * 0.5 +
        vertices[edge.vertex_indices[1]].position * 0.5;
    if (axis == 0U) return midpoint.x;
    if (axis == 1U) return midpoint.y;
    return midpoint.z;
}

std::size_t build_collision_edge_bvh_node(
    PreparedCloth& prepared,
    std::size_t begin,
    std::size_t end) {
    const std::size_t node_index = prepared.self_collision_edge_bvh.size();
    prepared.self_collision_edge_bvh.emplace_back();
    const std::size_t count = end - begin;
    if (count <= 4U) {
        prepared.self_collision_edge_bvh[node_index].begin = begin;
        prepared.self_collision_edge_bvh[node_index].count = count;
        return node_index;
    }

    ClothBounds centroid_bounds;
    for (std::size_t order = begin; order < end; ++order) {
        const ClothCollisionEdge& edge = prepared.collision_edges[
            prepared.edge_order[order]];
        const core::Vec3d midpoint =
            prepared.source_vertices[edge.vertex_indices[0]].position * 0.5 +
            prepared.source_vertices[edge.vertex_indices[1]].position * 0.5;
        include_point(centroid_bounds, midpoint);
    }
    const std::array<double, 3> extents{
        centroid_bounds.maximum.x - centroid_bounds.minimum.x,
        centroid_bounds.maximum.y - centroid_bounds.minimum.y,
        centroid_bounds.maximum.z - centroid_bounds.minimum.z};
    std::size_t axis = 0U;
    if (extents[1] > extents[axis]) axis = 1U;
    if (extents[2] > extents[axis]) axis = 2U;
    const std::size_t middle = begin + count / 2U;
    std::nth_element(prepared.edge_order.begin() + static_cast<std::ptrdiff_t>(begin),
        prepared.edge_order.begin() + static_cast<std::ptrdiff_t>(middle),
        prepared.edge_order.begin() + static_cast<std::ptrdiff_t>(end),
        [&prepared, axis](std::size_t first, std::size_t second) {
            const ClothCollisionEdge& a = prepared.collision_edges[first];
            const ClothCollisionEdge& b = prepared.collision_edges[second];
            const double first_centroid = edge_centroid_axis(
                a, prepared.source_vertices, axis);
            const double second_centroid = edge_centroid_axis(
                b, prepared.source_vertices, axis);
            if (first_centroid != second_centroid) return first_centroid < second_centroid;
            return a.key < b.key;
        });
    const std::size_t left = build_collision_edge_bvh_node(prepared, begin, middle);
    const std::size_t right = build_collision_edge_bvh_node(prepared, middle, end);
    prepared.self_collision_edge_bvh[node_index].left = left;
    prepared.self_collision_edge_bvh[node_index].right = right;
    return node_index;
}

core::Result<void> build_collision_edge_bvh(PreparedCloth& prepared) {
    if (prepared.collision_edges.empty() ||
        prepared.collision_edges.size() > kMaxClothSelfCollisionEdges) {
        return core::Result<void>::failure(unsupported(
            "cloth self-collision edge count exceeds its bounded BVH capacity"));
    }
    ClothBounds centroid_bounds;
    for (const ClothCollisionEdge& edge : prepared.collision_edges) {
        const core::Vec3d midpoint =
            prepared.source_vertices[edge.vertex_indices[0]].position * 0.5 +
            prepared.source_vertices[edge.vertex_indices[1]].position * 0.5;
        if (!midpoint.finite()) {
            return core::Result<void>::failure(validation(
                "cloth self-collision edge BVH centroid is not finite"));
        }
        include_point(centroid_bounds, midpoint);
    }
    const std::array<double, 3> extents{
        centroid_bounds.maximum.x - centroid_bounds.minimum.x,
        centroid_bounds.maximum.y - centroid_bounds.minimum.y,
        centroid_bounds.maximum.z - centroid_bounds.minimum.z};
    if (std::any_of(extents.begin(), extents.end(),
            [](double extent) { return !std::isfinite(extent); })) {
        return core::Result<void>::failure(validation(
            "cloth self-collision edge BVH extent is not finite"));
    }
    prepared.edge_order.resize(prepared.collision_edges.size());
    for (std::size_t i = 0U; i < prepared.edge_order.size(); ++i) {
        prepared.edge_order[i] = i;
    }
    prepared.self_collision_edge_bvh.reserve(prepared.collision_edges.size() * 2U - 1U);
    static_cast<void>(build_collision_edge_bvh_node(
        prepared, 0U, prepared.edge_order.size()));
    return core::Result<void>::success();
}

core::Result<DistanceConstraint> make_distance_constraint(
    geometry::VertexId first,
    geometry::VertexId second,
    double rest_length,
    double compliance,
    const std::map<geometry::VertexId, std::size_t>& indices,
    const std::vector<geometry::Vertex>& vertices,
    bool allow_zero_rest = false) {
    if (!first || !second || first == second || !std::isfinite(rest_length) ||
        rest_length < 0.0 || (!allow_zero_rest && rest_length <= kMinimumRestLength) ||
        !std::isfinite(compliance) || compliance < 0.0) {
        return core::Result<DistanceConstraint>::failure(validation(
            "cloth distance constraint has invalid identities or rest parameters"));
    }
    const auto a = indices.find(first);
    const auto b = indices.find(second);
    if (a == indices.end() || b == indices.end()) {
        return core::Result<DistanceConstraint>::failure(validation(
            "cloth constraint references a missing source vertex"));
    }
    const core::Vec3d difference = vertices[a->second].position - vertices[b->second].position;
    const double current_length = difference.length();
    const core::Vec3d direction = current_length > kMinimumRestLength ?
        difference * (1.0 / current_length) : core::Vec3d{1.0, 0.0, 0.0};
    return core::Result<DistanceConstraint>::success(DistanceConstraint{
        a->second, b->second, rest_length, compliance, 0.0, direction});
}

core::Result<PreparedCloth> prepare_cloth(
    const GarmentAsset& garment,
    const geometry::EditableMesh& base_mesh,
    bool enable_self_collision) {
    if (auto result = garment.validate(base_mesh); !result) {
        return core::Result<PreparedCloth>::failure(result.error());
    }
    if (base_mesh.vertex_count() == 0U || base_mesh.vertex_count() > kMaxClothVertices ||
        base_mesh.face_count() == 0U || base_mesh.face_count() > kMaxClothFaces) {
        return core::Result<PreparedCloth>::failure(unsupported(
            "cloth source mesh exceeds the CPU reference topology bound"));
    }
    PreparedCloth prepared;
    prepared.source_vertices = base_mesh.vertices_sorted();
    prepared.capsules = &garment.collision_capsules;
    prepared.thickness = garment.fabric.thickness;
    prepared.inverse_masses.resize(prepared.source_vertices.size(), 0.0);
    for (std::size_t i = 0U; i < prepared.source_vertices.size(); ++i) {
        prepared.vertex_indices.emplace(prepared.source_vertices[i].id, i);
    }
    std::vector<double> masses(prepared.source_vertices.size(), 0.0);
    std::vector<std::size_t> component_parent(prepared.source_vertices.size());
    for (std::size_t i = 0U; i < component_parent.size(); ++i) component_parent[i] = i;
    const auto component_root = [&component_parent](std::size_t vertex) {
        std::size_t root = vertex;
        while (component_parent[root] != root) root = component_parent[root];
        while (component_parent[vertex] != vertex) {
            const std::size_t next = component_parent[vertex];
            component_parent[vertex] = root;
            vertex = next;
        }
        return root;
    };
    const auto join_components = [&component_parent, &component_root](
        std::size_t first, std::size_t second) {
        const std::size_t first_root = component_root(first);
        const std::size_t second_root = component_root(second);
        if (first_root != second_root) {
            const std::size_t lower = std::min(first_root, second_root);
            const std::size_t upper = std::max(first_root, second_root);
            component_parent[upper] = lower;
        }
    };
    const auto faces = base_mesh.faces_sorted();
    std::map<EdgeKey, std::vector<geometry::VertexId>> opposite_vertices;
    std::set<EdgeKey> shear_pairs;
    std::set<EdgeKey> bend_pairs;
    for (const geometry::Face& face : faces) {
        const std::size_t last = face.vertices.size() - 1U;
        for (std::size_t triangle = 1U; triangle < last; ++triangle) {
            const std::array<geometry::VertexId, 3> ids{
                face.vertices[0], face.vertices[triangle], face.vertices[triangle + 1U]};
            const auto i0 = prepared.vertex_indices.find(ids[0]);
            const auto i1 = prepared.vertex_indices.find(ids[1]);
            const auto i2 = prepared.vertex_indices.find(ids[2]);
            if (i0 == prepared.vertex_indices.end() || i1 == prepared.vertex_indices.end() ||
                i2 == prepared.vertex_indices.end()) {
                return core::Result<PreparedCloth>::failure(validation(
                    "cloth source face references a missing vertex"));
            }
            const core::Vec3d a = prepared.source_vertices[i0->second].position;
            const core::Vec3d b = prepared.source_vertices[i1->second].position;
            const core::Vec3d c = prepared.source_vertices[i2->second].position;
            const double area = core::cross(b - a, c - a).length() * 0.5;
            if (!std::isfinite(area) || area <= 1e-15) {
                return core::Result<PreparedCloth>::failure(validation(
                    "cloth source contains a degenerate or non-finite surface triangle"));
            }
            const double vertex_mass = garment.fabric.areal_density * area / 3.0;
            for (const std::size_t index : {i0->second, i1->second, i2->second}) {
                masses[index] += vertex_mass;
            }
            if (enable_self_collision) {
                if (prepared.triangles.size() >= kMaxClothSelfCollisionTriangles) {
                    return core::Result<PreparedCloth>::failure(unsupported(
                        "cloth self-collision triangle count exceeds its bounded BVH capacity"));
                }
                prepared.triangles.push_back(ClothTriangle{
                    face.id, {i0->second, i1->second, i2->second}, ids, {}});
                join_components(i0->second, i1->second);
                join_components(i1->second, i2->second);
                join_components(i2->second, i0->second);
            }
            prepared.self_collision_exclusions.insert(edge_key(ids[0], ids[1]));
            prepared.self_collision_exclusions.insert(edge_key(ids[1], ids[2]));
            prepared.self_collision_exclusions.insert(edge_key(ids[2], ids[0]));
            opposite_vertices[edge_key(ids[0], ids[1])].push_back(ids[2]);
            opposite_vertices[edge_key(ids[1], ids[2])].push_back(ids[0]);
            opposite_vertices[edge_key(ids[2], ids[0])].push_back(ids[1]);
        }
        for (std::size_t diagonal = 2U; diagonal + 1U < face.vertices.size(); ++diagonal) {
            shear_pairs.insert(edge_key(face.vertices[0], face.vertices[diagonal]));
        }
    }
    for (std::size_t i = 0U; i < masses.size(); ++i) {
        if (!std::isfinite(masses[i]) || masses[i] <= 0.0) {
            return core::Result<PreparedCloth>::failure(validation(
                "cloth source contains an isolated vertex or non-finite mass"));
        }
        prepared.inverse_masses[i] = 1.0 / masses[i];
        if (!std::isfinite(prepared.inverse_masses[i])) {
            return core::Result<PreparedCloth>::failure(validation(
                "cloth source vertex inverse mass is not finite"));
        }
    }
    const auto topology = base_mesh.topology();
    if (!topology) return core::Result<PreparedCloth>::failure(topology.error());
    for (const geometry::EdgeRecord& edge : topology.value().edges) {
        const double rest_length = distance(
            prepared.source_vertices[prepared.vertex_indices.at(edge.first)].position,
            prepared.source_vertices[prepared.vertex_indices.at(edge.second)].position);
        auto constraint = make_distance_constraint(edge.first, edge.second, rest_length,
            garment.fabric.stretch_compliance, prepared.vertex_indices,
            prepared.source_vertices);
        if (!constraint) return core::Result<PreparedCloth>::failure(constraint.error());
        prepared.stretch.push_back(std::move(constraint).value());
    }
    for (const EdgeKey& pair : shear_pairs) {
        const double rest_length = distance(
            prepared.source_vertices[prepared.vertex_indices.at(pair.first)].position,
            prepared.source_vertices[prepared.vertex_indices.at(pair.second)].position);
        auto constraint = make_distance_constraint(pair.first, pair.second, rest_length,
            garment.fabric.shear_compliance, prepared.vertex_indices,
            prepared.source_vertices);
        if (!constraint) return core::Result<PreparedCloth>::failure(constraint.error());
        prepared.shear.push_back(std::move(constraint).value());
    }
    for (const auto& [edge, opposites] : opposite_vertices) {
        static_cast<void>(edge);
        if (opposites.size() > 2U) {
            return core::Result<PreparedCloth>::failure(unsupported(
                "cloth bend constraints require manifold triangle adjacency"));
        }
        if (opposites.size() == 2U && opposites[0] != opposites[1]) {
            bend_pairs.insert(edge_key(opposites[0], opposites[1]));
        }
    }
    for (const EdgeKey& pair : bend_pairs) {
        const double rest_length = distance(
            prepared.source_vertices[prepared.vertex_indices.at(pair.first)].position,
            prepared.source_vertices[prepared.vertex_indices.at(pair.second)].position);
        auto constraint = make_distance_constraint(pair.first, pair.second, rest_length,
            garment.fabric.bend_compliance, prepared.vertex_indices,
            prepared.source_vertices);
        if (!constraint) return core::Result<PreparedCloth>::failure(constraint.error());
        prepared.bend.push_back(std::move(constraint).value());
    }
    for (const GarmentSeam& seam : garment.seams) {
        for (std::size_t i = 0U; i < seam.first_chain.size(); ++i) {
            auto constraint = make_distance_constraint(
                seam.first_chain[i], seam.second_chain[i], 0.0, seam.compliance,
                prepared.vertex_indices, prepared.source_vertices, true);
            if (!constraint) return core::Result<PreparedCloth>::failure(constraint.error());
            prepared.seams.push_back(std::move(constraint).value());
            if (enable_self_collision) {
                join_components(prepared.vertex_indices.at(seam.first_chain[i]),
                    prepared.vertex_indices.at(seam.second_chain[i]));
                const EdgeKey seam_pair = edge_key(
                    seam.first_chain[i], seam.second_chain[i]);
                prepared.self_collision_exclusions.insert(seam_pair);
                prepared.self_collision_seam_vertex_pairs.insert(seam_pair);
                if (i > 0U) {
                    prepared.self_collision_edge_exclusions.insert(edge_pair_key(
                        edge_key(seam.first_chain[i - 1U], seam.first_chain[i]),
                        edge_key(seam.second_chain[i - 1U], seam.second_chain[i])));
                }
            }
        }
    }
    for (const GarmentPin& pin : garment.pins) {
        const auto index = prepared.vertex_indices.find(pin.vertex);
        if (index == prepared.vertex_indices.end()) {
            return core::Result<PreparedCloth>::failure(validation(
                "cloth pin references a missing source vertex"));
        }
        prepared.pins.push_back(PinConstraint{
            index->second, pin.target_position, pin.compliance, {}});
    }
    for (const GarmentAttachment& attachment : garment.attachments) {
        const auto index = prepared.vertex_indices.find(attachment.vertex);
        if (index == prepared.vertex_indices.end()) {
            return core::Result<PreparedCloth>::failure(validation(
                "cloth attachment references a missing source vertex"));
        }
        prepared.pins.push_back(PinConstraint{
            index->second, attachment.target_position, attachment.compliance, {}});
    }
    const std::uint64_t constraint_count =
        static_cast<std::uint64_t>(prepared.stretch.size()) +
        static_cast<std::uint64_t>(prepared.shear.size()) +
        static_cast<std::uint64_t>(prepared.bend.size()) +
        static_cast<std::uint64_t>(prepared.seams.size()) +
        static_cast<std::uint64_t>(prepared.pins.size());
    if (constraint_count > kMaxClothConstraints) {
        return core::Result<PreparedCloth>::failure(unsupported(
            "cloth source exceeds the CPU reference constraint count bound"));
    }
    if (enable_self_collision) {
        if (topology.value().edges.size() > kMaxClothSelfCollisionEdges) {
            return core::Result<PreparedCloth>::failure(unsupported(
                "cloth self-collision edge count exceeds its bounded BVH capacity"));
        }
        prepared.collision_edges.reserve(topology.value().edges.size());
        for (const geometry::EdgeRecord& edge : topology.value().edges) {
            const auto first = prepared.vertex_indices.find(edge.first);
            const auto second = prepared.vertex_indices.find(edge.second);
            if (first == prepared.vertex_indices.end() ||
                second == prepared.vertex_indices.end()) {
                return core::Result<PreparedCloth>::failure(validation(
                    "cloth collision edge references a missing source vertex"));
            }
            prepared.collision_edges.push_back(ClothCollisionEdge{
                edge_key(edge.first, edge.second), {first->second, second->second}});
        }
        prepared.component_keys.resize(prepared.source_vertices.size());
        for (std::size_t i = 0U; i < prepared.source_vertices.size(); ++i) {
            prepared.component_keys[i] =
                prepared.source_vertices[component_root(i)].id;
        }
        for (ClothTriangle& triangle : prepared.triangles) {
            triangle.component_key = prepared.component_keys[triangle.vertex_indices[0]];
        }
        if (auto result = build_collision_bvh(prepared); !result) {
            return core::Result<PreparedCloth>::failure(result.error());
        }
        if (auto result = build_collision_edge_bvh(prepared); !result) {
            return core::Result<PreparedCloth>::failure(result.error());
        }
    }
    return core::Result<PreparedCloth>::success(std::move(prepared));
}

core::Vec3d capsule_normal(core::Vec3d point, const GarmentCollisionCapsule& capsule,
                           core::Vec3d closest) {
    const core::Vec3d delta = point - closest;
    const double length = delta.length();
    if (length > kMinimumRestLength) return delta * (1.0 / length);
    const core::Vec3d axis = capsule.end - capsule.start;
    if (axis.length_squared() <= kMinimumRestLength * kMinimumRestLength) {
        return {1.0, 0.0, 0.0};
    }
    const core::Vec3d normalized_axis = axis.normalized();
    const core::Vec3d absolute{
        std::abs(normalized_axis.x), std::abs(normalized_axis.y), std::abs(normalized_axis.z)};
    const core::Vec3d basis = absolute.x <= absolute.y && absolute.x <= absolute.z ?
        core::Vec3d{1.0, 0.0, 0.0} :
        (absolute.y <= absolute.z ? core::Vec3d{0.0, 1.0, 0.0} :
            core::Vec3d{0.0, 0.0, 1.0});
    return core::cross(normalized_axis, basis).normalized();
}

bool project_capsules(
    std::vector<core::Vec3d>& positions,
    const PreparedCloth& prepared,
    double& maximum_correction) {
    for (core::Vec3d& position : positions) {
        for (const GarmentCollisionCapsule& capsule : *prepared.capsules) {
            const core::Vec3d axis = capsule.end - capsule.start;
            const double axis_length_squared = axis.length_squared();
            const double t = axis_length_squared <= kMinimumRestLength * kMinimumRestLength ?
                0.0 : std::clamp(core::dot(position - capsule.start, axis) /
                    axis_length_squared, 0.0, 1.0);
            const core::Vec3d closest = capsule.start + axis * t;
            const double separation = distance(position, closest);
            if (!std::isfinite(separation)) return false;
            const double effective_radius = capsule.radius + prepared.thickness;
            if (separation < effective_radius) {
                const core::Vec3d normal = capsule_normal(position, capsule, closest);
                if (!normal.finite()) return false;
                const core::Vec3d corrected = closest + normal * effective_radius;
                const double correction = distance(corrected, position);
                if (!corrected.finite() || !std::isfinite(correction)) return false;
                maximum_correction = std::max(maximum_correction, correction);
                position = corrected;
            }
        }
    }
    return true;
}

bool project_distance_constraint(
    DistanceConstraint& constraint,
    std::vector<core::Vec3d>& positions,
    const std::vector<double>& inverse_masses,
    double delta_time) {
    core::Vec3d difference = positions[constraint.first] - positions[constraint.second];
    double current_length = difference.length();
    if (!std::isfinite(current_length)) return false;
    core::Vec3d direction = current_length > kMinimumRestLength ?
        difference * (1.0 / current_length) : constraint.rest_direction;
    if (!direction.finite()) return false;
    if (current_length <= kMinimumRestLength && constraint.rest_length == 0.0) return true;
    const double first_weight = inverse_masses[constraint.first];
    const double second_weight = inverse_masses[constraint.second];
    const double alpha = constraint.compliance / (delta_time * delta_time);
    const double denominator = first_weight + second_weight + alpha;
    if (!std::isfinite(alpha) || !std::isfinite(denominator) || denominator <= 0.0) return false;
    const double delta_lambda = (-(current_length - constraint.rest_length) -
        alpha * constraint.lambda) / denominator;
    constraint.lambda += delta_lambda;
    positions[constraint.first] = positions[constraint.first] +
        direction * (first_weight * delta_lambda);
    positions[constraint.second] = positions[constraint.second] -
        direction * (second_weight * delta_lambda);
    return positions[constraint.first].finite() && positions[constraint.second].finite() &&
        std::isfinite(constraint.lambda);
}

bool project_pin(
    PinConstraint& pin,
    std::vector<core::Vec3d>& positions,
    const std::vector<double>& inverse_masses,
    double delta_time) {
    const double alpha = pin.compliance / (delta_time * delta_time);
    const double denominator = inverse_masses[pin.vertex] + alpha;
    if (!std::isfinite(alpha) || !std::isfinite(denominator) || denominator <= 0.0) return false;
    const core::Vec3d error = pin.target - positions[pin.vertex];
    const core::Vec3d delta_lambda =
        (error - pin.lambda * alpha) * (1.0 / denominator);
    pin.lambda = pin.lambda + delta_lambda;
    positions[pin.vertex] = positions[pin.vertex] +
        delta_lambda * inverse_masses[pin.vertex];
    return positions[pin.vertex].finite() && pin.lambda.finite();
}

template <typename Constraints>
bool project_constraints(
    Constraints& constraints,
    std::vector<core::Vec3d>& positions,
    const std::vector<double>& inverse_masses,
    double delta_time) {
    for (DistanceConstraint& constraint : constraints) {
        if (!project_distance_constraint(constraint, positions, inverse_masses, delta_time)) {
            return false;
        }
    }
    return true;
}

bool checked_multiply(std::uint64_t first, std::uint64_t second, std::uint64_t& product) {
    if (first != 0U && second > std::numeric_limits<std::uint64_t>::max() / first) return false;
    product = first * second;
    return true;
}

core::Result<std::uint64_t> validate_work_budget(
    const PreparedCloth& prepared,
    const ClothSolverSettings& settings) {
    const std::uint64_t vertex_count = prepared.source_vertices.size();
    std::uint64_t output_samples = 0U;
    if (!checked_multiply(vertex_count, settings.frame_count, output_samples) ||
        output_samples > kMaxClothSimulationSamples ||
        output_samples > (kMaxClothSimulationArtifactBytes - 4096U) / 96U) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth frame sequence exceeds its bounded sample or byte capacity"));
    }
    const std::uint64_t constraints = prepared.stretch.size() + prepared.shear.size() +
        prepared.bend.size() + prepared.seams.size() + prepared.pins.size();
    std::uint64_t collision_checks = 0U;
    if (!checked_multiply(vertex_count,
            static_cast<std::uint64_t>(prepared.capsules->size()), collision_checks) ||
        collision_checks > std::numeric_limits<std::uint64_t>::max() - constraints) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth collision work estimate overflows its bounded counter"));
    }
    std::uint64_t work_per_iteration = constraints + collision_checks;
    if (settings.enable_self_collision) {
        std::uint64_t neighborhood_operations = 0U;
        if (!checked_multiply(vertex_count,
                kSelfCollisionCellBuildOperationsPerVertex, neighborhood_operations) ||
            neighborhood_operations >
                std::numeric_limits<std::uint64_t>::max() - work_per_iteration) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth self-collision neighborhood work exceeds its bounded counter"));
        }
        work_per_iteration += neighborhood_operations;
        if (prepared.self_collision_bvh.size() >
            std::numeric_limits<std::uint64_t>::max() - work_per_iteration) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth self-collision BVH work exceeds its bounded counter"));
        }
        work_per_iteration += static_cast<std::uint64_t>(prepared.self_collision_bvh.size());
        if (prepared.self_collision_edge_bvh.size() >
            std::numeric_limits<std::uint64_t>::max() - work_per_iteration) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth self-collision edge BVH work exceeds its bounded counter"));
        }
        work_per_iteration += static_cast<std::uint64_t>(
            prepared.self_collision_edge_bvh.size());
        if (prepared.collision_edges.size() >
            std::numeric_limits<std::uint64_t>::max() - work_per_iteration) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth self-collision edge refit work exceeds its bounded counter"));
        }
        work_per_iteration += static_cast<std::uint64_t>(prepared.collision_edges.size());
    }
    std::uint64_t per_substep = 0U;
    if (!checked_multiply(work_per_iteration,
            static_cast<std::uint64_t>(settings.projection_iterations), per_substep) ||
        per_substep > std::numeric_limits<std::uint64_t>::max() - vertex_count * 2U) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth projection work estimate exceeds its bounded counter"));
    }
    per_substep += vertex_count * 2U;
    std::uint64_t substeps = 0U;
    std::uint64_t total_work = 0U;
    if (!checked_multiply(settings.frame_count, settings.substeps_per_frame, substeps) ||
        !checked_multiply(substeps, per_substep, total_work) ||
        total_work > kMaxClothProjectionOperations) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth simulation exceeds the CPU projection work budget"));
    }
    std::uint64_t verification_per_frame = 0U;
    if (collision_checks > std::numeric_limits<std::uint64_t>::max() - constraints) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth frame verification work exceeds its bounded counter"));
    }
    verification_per_frame = constraints + collision_checks;
    if (verification_per_frame > std::numeric_limits<std::uint64_t>::max() - vertex_count) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth frame verification work exceeds its bounded counter"));
    }
    verification_per_frame += vertex_count;
    std::uint64_t verification_work = 0U;
    if (!checked_multiply(settings.frame_count, verification_per_frame, verification_work) ||
        verification_work > std::numeric_limits<std::uint64_t>::max() - total_work) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth frame verification work exceeds its bounded counter"));
    }
    total_work += verification_work;
    if (total_work > kMaxClothProjectionOperations) {
        return core::Result<std::uint64_t>::failure(unsupported(
            "cloth simulation exceeds the CPU projection work budget"));
    }
    if (settings.enable_self_collision) {
        std::uint64_t final_neighborhood_work = 0U;
        std::uint64_t final_bvh_build_work = 0U;
        std::uint64_t final_edge_bvh_build_work = 0U;
        std::uint64_t triangle_count = static_cast<std::uint64_t>(prepared.triangles.size());
        std::uint64_t edge_count = static_cast<std::uint64_t>(prepared.collision_edges.size());
        std::uint64_t levels = 1U;
        std::uint64_t edge_levels = 1U;
        for (std::uint64_t remaining = triangle_count; remaining > 1U; remaining >>= 1U) {
            ++levels;
        }
        for (std::uint64_t remaining = edge_count; remaining > 1U; remaining >>= 1U) {
            ++edge_levels;
        }
        if (!checked_multiply(vertex_count,
                kSelfCollisionCellBuildOperationsPerVertex, final_neighborhood_work) ||
            !checked_multiply(triangle_count, levels, final_bvh_build_work) ||
            !checked_multiply(edge_count, edge_levels, final_edge_bvh_build_work) ||
            final_neighborhood_work >
                std::numeric_limits<std::uint64_t>::max() - prepared.self_collision_bvh.size()) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth final self-collision verification work exceeds its bounded counter"));
        }
        final_neighborhood_work += static_cast<std::uint64_t>(
            prepared.self_collision_bvh.size());
        if (final_neighborhood_work > std::numeric_limits<std::uint64_t>::max() -
                prepared.self_collision_edge_bvh.size()) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth final self-collision verification work exceeds its bounded counter"));
        }
        final_neighborhood_work += static_cast<std::uint64_t>(
            prepared.self_collision_edge_bvh.size());
        if (final_neighborhood_work >
            std::numeric_limits<std::uint64_t>::max() - edge_count) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth final self-collision verification work exceeds its bounded counter"));
        }
        final_neighborhood_work += edge_count;
        if (final_bvh_build_work >
                std::numeric_limits<std::uint64_t>::max() - final_edge_bvh_build_work ||
            final_bvh_build_work + final_edge_bvh_build_work >
                std::numeric_limits<std::uint64_t>::max() - final_neighborhood_work ||
            final_neighborhood_work + final_bvh_build_work + final_edge_bvh_build_work >
                std::numeric_limits<std::uint64_t>::max() / settings.frame_count) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth final self-collision verification work exceeds its bounded counter"));
        }
        const std::uint64_t verification_self_collision_work =
            (final_neighborhood_work + final_bvh_build_work + final_edge_bvh_build_work) *
            settings.frame_count;
        if (verification_self_collision_work >
            std::numeric_limits<std::uint64_t>::max() - total_work) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth final self-collision verification work exceeds its bounded counter"));
        }
        total_work += verification_self_collision_work;
        if (total_work > kMaxClothProjectionOperations) {
            return core::Result<std::uint64_t>::failure(unsupported(
                "cloth simulation exceeds the CPU projection work budget"));
        }
    }
    return core::Result<std::uint64_t>::success(total_work);
}

bool spatial_cell(core::Vec3d position, double cell_width, CellKey& cell) {
    // Keeping coordinates within 2^52 preserves neighboring-cell resolution
    // when positions and the cell width are represented as doubles.
    constexpr long double kMaximumCellCoordinate = 4'503'599'627'370'496.0L;
    const std::array<double, 3> coordinates{position.x, position.y, position.z};
    for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
        if (!std::isfinite(coordinates[axis])) return false;
        const long double scaled = static_cast<long double>(coordinates[axis]) /
            static_cast<long double>(cell_width);
        if (!std::isfinite(scaled)) return false;
        const long double floored = std::floor(scaled);
        if (floored < -kMaximumCellCoordinate || floored > kMaximumCellCoordinate) return false;
        cell[axis] = static_cast<std::int64_t>(floored);
    }
    return true;
}

struct TriangleClosestPoint {
    core::Vec3d position;
    std::array<double, 3> barycentric{};
};

struct SegmentClosestPoints {
    core::Vec3d first;
    core::Vec3d second;
    double first_parameter = 0.0;
    double second_parameter = 0.0;
};

bool closest_point_on_triangle(
    core::Vec3d point,
    core::Vec3d a,
    core::Vec3d b,
    core::Vec3d c,
    TriangleClosestPoint& result) {
    const core::Vec3d ab = b - a;
    const core::Vec3d ac = c - a;
    const core::Vec3d ap = point - a;
    const double d1 = core::dot(ab, ap);
    const double d2 = core::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        result = {a, {1.0, 0.0, 0.0}};
        return true;
    }

    const core::Vec3d bp = point - b;
    const double d3 = core::dot(ab, bp);
    const double d4 = core::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        result = {b, {0.0, 1.0, 0.0}};
        return true;
    }

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double denominator = d1 - d3;
        if (!std::isfinite(denominator) || denominator <= 0.0) return false;
        const double v = d1 / denominator;
        result = {a + ab * v, {1.0 - v, v, 0.0}};
        return result.position.finite();
    }

    const core::Vec3d cp = point - c;
    const double d5 = core::dot(ab, cp);
    const double d6 = core::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        result = {c, {0.0, 0.0, 1.0}};
        return true;
    }

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double denominator = d2 - d6;
        if (!std::isfinite(denominator) || denominator <= 0.0) return false;
        const double w = d2 / denominator;
        result = {a + ac * w, {1.0 - w, 0.0, w}};
        return result.position.finite();
    }

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double first = d4 - d3;
        const double second = d5 - d6;
        const double denominator = first + second;
        if (!std::isfinite(denominator) || denominator <= 0.0) return false;
        const double w = first / denominator;
        result = {b + (c - b) * w, {0.0, 1.0 - w, w}};
        return result.position.finite();
    }

    const double denominator = va + vb + vc;
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1e-30) return false;
    const double inverse = 1.0 / denominator;
    const double v = vb * inverse;
    const double w = vc * inverse;
    const double u = 1.0 - v - w;
    result = {a + ab * v + ac * w, {u, v, w}};
    return result.position.finite() && std::isfinite(u) && std::isfinite(v) &&
        std::isfinite(w);
}

bool closest_points_on_segments(
    core::Vec3d first_start,
    core::Vec3d first_end,
    core::Vec3d second_start,
    core::Vec3d second_end,
    SegmentClosestPoints& result) {
    const core::Vec3d first_delta = first_end - first_start;
    const core::Vec3d second_delta = second_end - second_start;
    const double first_length = first_delta.length();
    const double second_length = second_delta.length();
    if (!first_start.finite() || !first_end.finite() || !second_start.finite() ||
        !second_end.finite() || !std::isfinite(first_length) ||
        !std::isfinite(second_length) || first_length <= kMinimumRestLength ||
        second_length <= kMinimumRestLength) {
        return false;
    }
    const core::Vec3d first_direction = first_delta * (1.0 / first_length);
    const core::Vec3d second_direction = second_delta * (1.0 / second_length);
    const core::Vec3d offset = first_start - second_start;
    const double direction_dot = core::dot(first_direction, second_direction);
    const double first_offset = core::dot(first_direction, offset);
    const double second_offset = core::dot(second_direction, offset);
    if (!std::isfinite(direction_dot) || !std::isfinite(first_offset) ||
        !std::isfinite(second_offset)) {
        return false;
    }

    const double denominator = 1.0 - direction_dot * direction_dot;
    double first_distance = denominator > 1e-14 ?
        (direction_dot * second_offset - first_offset) / denominator : 0.0;
    first_distance = std::clamp(first_distance, 0.0, first_length);
    double second_distance = direction_dot * first_distance + second_offset;
    if (second_distance < 0.0) {
        second_distance = 0.0;
        first_distance = std::clamp(-first_offset, 0.0, first_length);
    } else if (second_distance > second_length) {
        second_distance = second_length;
        first_distance = std::clamp(
            direction_dot * second_length - first_offset, 0.0, first_length);
    }
    result.first_parameter = first_distance / first_length;
    result.second_parameter = second_distance / second_length;
    result.first = first_start + first_direction * first_distance;
    result.second = second_start + second_direction * second_distance;
    return result.first.finite() && result.second.finite() &&
        std::isfinite(result.first_parameter) &&
        std::isfinite(result.second_parameter);
}

bool bounds_near_point(const ClothBounds& bounds, core::Vec3d point, double radius) {
    return point.x >= bounds.minimum.x - radius && point.x <= bounds.maximum.x + radius &&
        point.y >= bounds.minimum.y - radius && point.y <= bounds.maximum.y + radius &&
        point.z >= bounds.minimum.z - radius && point.z <= bounds.maximum.z + radius;
}

core::Result<void> refit_collision_bvh(
    const std::vector<core::Vec3d>& positions,
    PreparedCloth& prepared) {
    if (prepared.self_collision_bvh.empty()) {
        return core::Result<void>::failure(validation(
            "cloth vertex-face collision BVH is not initialized"));
    }
    for (std::size_t reverse = prepared.self_collision_bvh.size(); reverse > 0U; --reverse) {
        ClothBvhNode& node = prepared.self_collision_bvh[reverse - 1U];
        node.bounds = ClothBounds{};
        if (node.leaf()) {
            for (std::size_t offset = 0U; offset < node.count; ++offset) {
                const ClothTriangle& triangle = prepared.triangles[
                    prepared.triangle_order[node.begin + offset]];
                const core::Vec3d a = positions[triangle.vertex_indices[0]];
                const core::Vec3d b = positions[triangle.vertex_indices[1]];
                const core::Vec3d c = positions[triangle.vertex_indices[2]];
                const core::Vec3d cross = core::cross(b - a, c - a);
                const double area_twice = cross.length();
                if (!a.finite() || !b.finite() || !c.finite() ||
                    !std::isfinite(area_twice) || area_twice <= 2e-15) {
                    return core::Result<void>::failure(validation(
                        "cloth vertex-face collision encountered a degenerate triangle"));
                }
                include_point(node.bounds, a);
                include_point(node.bounds, b);
                include_point(node.bounds, c);
            }
        } else {
            include_bounds(node.bounds,
                prepared.self_collision_bvh[node.left].bounds);
            include_bounds(node.bounds,
                prepared.self_collision_bvh[node.right].bounds);
        }
    }
    return core::Result<void>::success();
}

core::Result<void> refit_collision_edge_bvh(
    const std::vector<core::Vec3d>& positions,
    PreparedCloth& prepared) {
    if (prepared.self_collision_edge_bvh.empty()) {
        return core::Result<void>::failure(validation(
            "cloth edge-edge collision BVH is not initialized"));
    }
    for (std::size_t reverse = prepared.self_collision_edge_bvh.size(); reverse > 0U; --reverse) {
        ClothBvhNode& node = prepared.self_collision_edge_bvh[reverse - 1U];
        node.bounds = ClothBounds{};
        if (node.leaf()) {
            for (std::size_t offset = 0U; offset < node.count; ++offset) {
                const ClothCollisionEdge& edge = prepared.collision_edges[
                    prepared.edge_order[node.begin + offset]];
                const core::Vec3d first = positions[edge.vertex_indices[0]];
                const core::Vec3d second = positions[edge.vertex_indices[1]];
                const double edge_length = distance(first, second);
                if (!first.finite() || !second.finite() || !std::isfinite(edge_length) ||
                    edge_length <= kMinimumRestLength) {
                    return core::Result<void>::failure(validation(
                        "cloth edge-edge collision encountered a degenerate edge"));
                }
                include_point(node.bounds, first);
                include_point(node.bounds, second);
            }
        } else {
            include_bounds(node.bounds,
                prepared.self_collision_edge_bvh[node.left].bounds);
            include_bounds(node.bounds,
                prepared.self_collision_edge_bvh[node.right].bounds);
        }
    }
    return core::Result<void>::success();
}

std::optional<bool> bounds_overlap_with_margin(
    const ClothBounds& first,
    const ClothBounds& second,
    double margin) {
    const std::array<double, 6> expanded{
        first.maximum.x + margin, first.maximum.y + margin, first.maximum.z + margin,
        second.maximum.x + margin, second.maximum.y + margin, second.maximum.z + margin};
    if (std::any_of(expanded.begin(), expanded.end(),
            [](double coordinate) { return !std::isfinite(coordinate); })) {
        return std::nullopt;
    }
    return !(expanded[0] < second.minimum.x || expanded[3] < first.minimum.x ||
        expanded[1] < second.minimum.y || expanded[4] < first.minimum.y ||
        expanded[2] < second.minimum.z || expanded[5] < first.minimum.z);
}

core::Vec3d canonical_normal(core::Vec3d normal) {
    normal = normal.normalized();
    const std::array<double, 3> components{normal.x, normal.y, normal.z};
    for (const double component : components) {
        if (std::abs(component) <= 1e-12) continue;
        return component < 0.0 ? normal * -1.0 : normal;
    }
    return normal;
}

core::Vec3d edge_contact_normal(
    const ClothCollisionEdge& first_edge,
    const ClothCollisionEdge& second_edge,
    const std::vector<core::Vec3d>& positions,
    const SegmentClosestPoints& closest,
    double separation) {
    const core::Vec3d difference = closest.first - closest.second;
    if (separation > 1e-12) return difference * (1.0 / separation);

    const core::Vec3d first_direction =
        (positions[first_edge.vertex_indices[1]] -
            positions[first_edge.vertex_indices[0]]).normalized();
    const core::Vec3d second_direction =
        (positions[second_edge.vertex_indices[1]] -
            positions[second_edge.vertex_indices[0]]).normalized();
    core::Vec3d normal = core::cross(first_direction, second_direction);
    if (!normal.finite() || normal.length_squared() <= 1e-24) {
        const std::array<double, 3> absolute{
            std::abs(first_direction.x), std::abs(first_direction.y),
            std::abs(first_direction.z)};
        core::Vec3d axis{1.0, 0.0, 0.0};
        if (absolute[1] < absolute[0] && absolute[1] <= absolute[2]) {
            axis = {0.0, 1.0, 0.0};
        } else if (absolute[2] < absolute[0] && absolute[2] < absolute[1]) {
            axis = {0.0, 0.0, 1.0};
        }
        normal = core::cross(first_direction, axis);
    }
    return canonical_normal(normal);
}

bool edges_share_vertex(const ClothCollisionEdge& first, const ClothCollisionEdge& second) {
    return first.key.first == second.key.first || first.key.first == second.key.second ||
        first.key.second == second.key.first || first.key.second == second.key.second;
}

bool edges_are_seam_adjacent(
    const ClothCollisionEdge& first,
    const ClothCollisionEdge& second,
    const PreparedCloth& prepared) {
    return prepared.self_collision_seam_vertex_pairs.contains(
            edge_key(first.key.first, second.key.first)) ||
        prepared.self_collision_seam_vertex_pairs.contains(
            edge_key(first.key.first, second.key.second)) ||
        prepared.self_collision_seam_vertex_pairs.contains(
            edge_key(first.key.second, second.key.first)) ||
        prepared.self_collision_seam_vertex_pairs.contains(
            edge_key(first.key.second, second.key.second));
}

bool topologically_excluded(
    std::size_t vertex,
    const ClothTriangle& triangle,
    const PreparedCloth& prepared) {
    const geometry::VertexId vertex_id = prepared.source_vertices[vertex].id;
    for (const geometry::VertexId triangle_vertex : triangle.vertex_ids) {
        if (vertex_id == triangle_vertex || prepared.self_collision_exclusions.contains(
                edge_key(vertex_id, triangle_vertex))) {
            return true;
        }
    }
    return false;
}

template <typename Visitor>
core::Result<void> visit_vertex_face_contacts(
    const std::vector<core::Vec3d>& positions,
    PreparedCloth& prepared,
    double minimum_separation,
    std::uint64_t& total_work,
    Visitor&& visitor) {
    if (auto result = refit_collision_bvh(positions, prepared); !result) {
        return result;
    }
    std::vector<std::size_t> stack;
    stack.reserve(64U);
    for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
        stack.clear();
        stack.push_back(0U);
        while (!stack.empty()) {
            if (total_work >= kMaxClothProjectionOperations) {
                return core::Result<void>::failure(unsupported(
                    "cloth vertex-face candidate work exceeds the CPU projection budget"));
            }
            ++total_work;
            const std::size_t node_index = stack.back();
            stack.pop_back();
            const ClothBvhNode& node = prepared.self_collision_bvh[node_index];
            if (!bounds_near_point(node.bounds, positions[vertex], minimum_separation)) continue;
            if (node.leaf()) {
                for (std::size_t offset = 0U; offset < node.count; ++offset) {
                    if (total_work >= kMaxClothProjectionOperations) {
                        return core::Result<void>::failure(unsupported(
                            "cloth vertex-face triangle work exceeds the CPU projection budget"));
                    }
                    ++total_work;
                    const ClothTriangle& triangle = prepared.triangles[
                        prepared.triangle_order[node.begin + offset]];
                    if (topologically_excluded(vertex, triangle, prepared)) continue;
                    const core::Vec3d a = positions[triangle.vertex_indices[0]];
                    const core::Vec3d b = positions[triangle.vertex_indices[1]];
                    const core::Vec3d c = positions[triangle.vertex_indices[2]];
                    TriangleClosestPoint closest;
                    if (!closest_point_on_triangle(positions[vertex], a, b, c, closest)) {
                        return core::Result<void>::failure(validation(
                            "cloth vertex-face closest-point projection is degenerate"));
                    }
                    const core::Vec3d difference = positions[vertex] - closest.position;
                    const double separation = difference.length();
                    if (!std::isfinite(separation)) {
                        return core::Result<void>::failure(validation(
                            "cloth vertex-face separation is not finite"));
                    }
                    if (separation >= minimum_separation) continue;

                    core::Vec3d normal;
                    if (separation > std::max(1e-12, minimum_separation * 1e-9)) {
                        normal = difference * (1.0 / separation);
                    } else {
                        normal = canonical_normal(core::cross(b - a, c - a));
                        const geometry::VertexId point_component = prepared.component_keys[vertex];
                        if (point_component != triangle.component_key) {
                            if (point_component < triangle.component_key) normal = normal * -1.0;
                        } else {
                            const geometry::VertexId minimum_triangle_id = std::min(
                                triangle.vertex_ids[0], std::min(
                                    triangle.vertex_ids[1], triangle.vertex_ids[2]));
                            if (prepared.source_vertices[vertex].id < minimum_triangle_id) {
                                normal = normal * -1.0;
                            }
                        }
                    }
                    if (!normal.finite() || normal.length_squared() <= 0.0) {
                        return core::Result<void>::failure(validation(
                            "cloth vertex-face contact normal is not finite"));
                    }
                    if (auto result = visitor(
                            vertex, triangle, closest, normal, separation); !result) {
                        return result;
                    }
                }
            } else {
                // Stack order makes the lower stable subtree run first.
                stack.push_back(node.right);
                stack.push_back(node.left);
            }
        }
    }
    return core::Result<void>::success();
}

template <typename Visitor>
core::Result<void> visit_edge_edge_contacts(
    const std::vector<core::Vec3d>& positions,
    PreparedCloth& prepared,
    double minimum_separation,
    std::uint64_t& total_work,
    Visitor&& visitor) {
    if (auto result = refit_collision_edge_bvh(positions, prepared); !result) {
        return result;
    }
    std::vector<std::size_t> stack;
    std::vector<std::size_t> candidates;
    stack.reserve(64U);
    candidates.reserve(64U);
    for (std::size_t first_index = 0U;
         first_index < prepared.collision_edges.size(); ++first_index) {
        const ClothCollisionEdge& first_edge = prepared.collision_edges[first_index];
        ClothBounds first_bounds;
        include_point(first_bounds, positions[first_edge.vertex_indices[0]]);
        include_point(first_bounds, positions[first_edge.vertex_indices[1]]);
        stack.clear();
        candidates.clear();
        stack.push_back(0U);
        while (!stack.empty()) {
            if (total_work >= kMaxClothProjectionOperations) {
                return core::Result<void>::failure(unsupported(
                    "cloth edge-edge candidate work exceeds the CPU projection budget"));
            }
            ++total_work;
            const std::size_t node_index = stack.back();
            stack.pop_back();
            const ClothBvhNode& node = prepared.self_collision_edge_bvh[node_index];
            const auto node_overlap = bounds_overlap_with_margin(
                first_bounds, node.bounds, minimum_separation);
            if (!node_overlap.has_value()) {
                return core::Result<void>::failure(validation(
                    "cloth edge-edge bounds expansion is not finite"));
            }
            if (!*node_overlap) continue;
            if (node.leaf()) {
                for (std::size_t offset = 0U; offset < node.count; ++offset) {
                    if (total_work >= kMaxClothProjectionOperations) {
                        return core::Result<void>::failure(unsupported(
                            "cloth edge-edge leaf work exceeds the CPU projection budget"));
                    }
                    ++total_work;
                    const std::size_t second_index = prepared.edge_order[node.begin + offset];
                    if (second_index <= first_index) continue;
                    const ClothCollisionEdge& second_edge =
                        prepared.collision_edges[second_index];
                    ClothBounds second_bounds;
                    include_point(second_bounds, positions[second_edge.vertex_indices[0]]);
                    include_point(second_bounds, positions[second_edge.vertex_indices[1]]);
                    const auto edge_overlap = bounds_overlap_with_margin(
                        first_bounds, second_bounds, minimum_separation);
                    if (!edge_overlap.has_value()) {
                        return core::Result<void>::failure(validation(
                            "cloth edge-edge pair bounds expansion is not finite"));
                    }
                    if (*edge_overlap) candidates.push_back(second_index);
                }
            } else {
                // Stack order makes the lower stable subtree run first.
                stack.push_back(node.right);
                stack.push_back(node.left);
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (const std::size_t second_index : candidates) {
            const ClothCollisionEdge& second_edge = prepared.collision_edges[second_index];
            if (edges_share_vertex(first_edge, second_edge) ||
                edges_are_seam_adjacent(first_edge, second_edge, prepared) ||
                prepared.self_collision_edge_exclusions.contains(
                    edge_pair_key(first_edge.key, second_edge.key))) {
                continue;
            }
            if (total_work >= kMaxClothProjectionOperations) {
                return core::Result<void>::failure(unsupported(
                    "cloth edge-edge pair work exceeds the CPU projection budget"));
            }
            ++total_work;
            SegmentClosestPoints closest;
            if (!closest_points_on_segments(
                    positions[first_edge.vertex_indices[0]],
                    positions[first_edge.vertex_indices[1]],
                    positions[second_edge.vertex_indices[0]],
                    positions[second_edge.vertex_indices[1]], closest)) {
                return core::Result<void>::failure(validation(
                    "cloth edge-edge closest-point projection is degenerate"));
            }
            const double separation = distance(closest.first, closest.second);
            if (!std::isfinite(separation)) {
                return core::Result<void>::failure(validation(
                    "cloth edge-edge separation is not finite"));
            }
            if (separation >= minimum_separation) continue;
            const core::Vec3d normal = edge_contact_normal(
                first_edge, second_edge, positions, closest, separation);
            if (!normal.finite() || normal.length_squared() <= 0.0) {
                return core::Result<void>::failure(validation(
                    "cloth edge-edge contact normal is not finite"));
            }
            if (auto result = visitor(
                    first_edge, second_edge, closest, normal, separation); !result) {
                return result;
            }
        }
    }
    return core::Result<void>::success();
}

template <typename Visitor>
core::Result<void> visit_self_collision_pairs(
    const std::vector<core::Vec3d>& positions,
    const PreparedCloth& prepared,
    double minimum_separation,
    std::uint64_t& total_work,
    Visitor&& visitor) {
    std::map<CellKey, std::vector<std::size_t>> cells;
    for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
        CellKey cell{};
        if (!spatial_cell(positions[vertex], minimum_separation, cell)) {
            return core::Result<void>::failure(validation(
                "cloth self-collision position exceeds the spatial cell coordinate bound"));
        }
        auto& occupants = cells[cell];
        occupants.push_back(vertex);
        if (occupants.size() > kMaxClothSelfCollisionCellOccupancy) {
            return core::Result<void>::failure(unsupported(
                "cloth self-collision cell exceeds its 64-vertex neighborhood bound"));
        }
    }

    for (std::size_t first = 0U; first < positions.size(); ++first) {
        CellKey center{};
        if (!spatial_cell(positions[first], minimum_separation, center)) {
            return core::Result<void>::failure(validation(
                "cloth self-collision position exceeds the spatial cell coordinate bound"));
        }
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const CellKey neighbor{
                        center[0] + dx, center[1] + dy, center[2] + dz};
                    const auto cell = cells.find(neighbor);
                    if (cell == cells.end()) continue;
                    for (const std::size_t second : cell->second) {
                        if (second <= first) continue;
                        if (total_work >= kMaxClothProjectionOperations) {
                            return core::Result<void>::failure(unsupported(
                                "cloth self-collision candidate work exceeds the CPU projection budget"));
                        }
                        ++total_work;
                        if (prepared.self_collision_exclusions.contains(edge_key(
                                prepared.source_vertices[first].id,
                                prepared.source_vertices[second].id))) {
                            continue;
                        }
                        const double separation = distance(positions[first], positions[second]);
                        if (!std::isfinite(separation)) {
                            return core::Result<void>::failure(validation(
                                "cloth self-collision distance is not finite"));
                        }
                        if (separation < minimum_separation) {
                            if (auto result = visitor(first, second, separation); !result) {
                                return result;
                            }
                        }
                    }
                }
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> project_vertex_vertex_self_collision(
    std::vector<core::Vec3d>& positions,
    const PreparedCloth& prepared,
    std::uint64_t& total_work,
    double& maximum_correction) {
    const double minimum_separation = prepared.thickness * 2.0;
    return visit_self_collision_pairs(positions, prepared, minimum_separation, total_work,
        [&](std::size_t first, std::size_t second, double separation) {
            const double first_weight = prepared.inverse_masses[first];
            const double second_weight = prepared.inverse_masses[second];
            const double denominator = first_weight + second_weight;
            if (!std::isfinite(denominator)) {
                return core::Result<void>::failure(validation(
                    "cloth self-collision mass denominator is not finite"));
            }
            if (denominator <= 0.0) return core::Result<void>::success();
            const core::Vec3d difference = positions[first] - positions[second];
            const core::Vec3d normal = separation > kMinimumRestLength ?
                difference * (1.0 / separation) : core::Vec3d{1.0, 0.0, 0.0};
            const double correction = (minimum_separation - separation) / denominator;
            if (!normal.finite() || !std::isfinite(correction) || correction < 0.0) {
                return core::Result<void>::failure(validation(
                    "cloth self-collision projection is not finite"));
            }
            positions[first] = positions[first] + normal * (first_weight * correction);
            positions[second] = positions[second] - normal * (second_weight * correction);
            if (!positions[first].finite() || !positions[second].finite()) {
                return core::Result<void>::failure(validation(
                    "cloth self-collision projection produced a non-finite state"));
            }
            maximum_correction = std::max(maximum_correction,
                std::max(first_weight, second_weight) * correction);
            return core::Result<void>::success();
        });
}

core::Result<void> project_vertex_face_self_collision(
    std::vector<core::Vec3d>& positions,
    PreparedCloth& prepared,
    std::uint64_t& total_work,
    double& maximum_correction) {
    const double minimum_separation = prepared.thickness * 2.0;
    return visit_vertex_face_contacts(positions, prepared, minimum_separation, total_work,
        [&](std::size_t vertex, const ClothTriangle& triangle,
            const TriangleClosestPoint& closest, core::Vec3d normal, double separation) {
            const double penetration = minimum_separation - separation;
            const double vertex_weight = prepared.inverse_masses[vertex];
            double denominator = vertex_weight;
            for (std::size_t corner = 0U; corner < triangle.vertex_indices.size(); ++corner) {
                const double barycentric = closest.barycentric[corner];
                const double weight = prepared.inverse_masses[triangle.vertex_indices[corner]];
                denominator += barycentric * barycentric * weight;
            }
            if (!std::isfinite(penetration) || penetration <= 0.0 ||
                !std::isfinite(denominator)) {
                return core::Result<void>::failure(validation(
                    "cloth vertex-face contact projection has invalid weights"));
            }
            if (denominator <= 0.0) {
                return core::Result<void>::failure(validation(
                    "cloth vertex-face contact is pinned on both sides"));
            }
            const double delta_lambda = penetration / denominator;
            const core::Vec3d point_delta = normal * (vertex_weight * delta_lambda);
            positions[vertex] = positions[vertex] + point_delta;
            maximum_correction = std::max(maximum_correction, point_delta.length());
            for (std::size_t corner = 0U; corner < triangle.vertex_indices.size(); ++corner) {
                const std::size_t triangle_vertex = triangle.vertex_indices[corner];
                const double barycentric = closest.barycentric[corner];
                const double weight = prepared.inverse_masses[triangle_vertex];
                const core::Vec3d triangle_delta =
                    normal * (-barycentric * weight * delta_lambda);
                positions[triangle_vertex] = positions[triangle_vertex] + triangle_delta;
                maximum_correction = std::max(
                    maximum_correction, triangle_delta.length());
            }
            if (!positions[vertex].finite() ||
                std::any_of(triangle.vertex_indices.begin(), triangle.vertex_indices.end(),
                    [&positions](std::size_t index) { return !positions[index].finite(); })) {
                return core::Result<void>::failure(validation(
                    "cloth vertex-face contact projection produced a non-finite state"));
            }
            return core::Result<void>::success();
        });
}

core::Result<void> project_edge_edge_self_collision(
    std::vector<core::Vec3d>& positions,
    PreparedCloth& prepared,
    std::uint64_t& total_work,
    double& maximum_correction) {
    const double minimum_separation = prepared.thickness * 2.0;
    return visit_edge_edge_contacts(positions, prepared, minimum_separation, total_work,
        [&](const ClothCollisionEdge& first_edge, const ClothCollisionEdge& second_edge,
            const SegmentClosestPoints& closest, core::Vec3d normal, double separation) {
            const double penetration = minimum_separation - separation;
            const std::array<std::size_t, 4> vertices{
                first_edge.vertex_indices[0], first_edge.vertex_indices[1],
                second_edge.vertex_indices[0], second_edge.vertex_indices[1]};
            const std::array<double, 4> coefficients{
                1.0 - closest.first_parameter, closest.first_parameter,
                -(1.0 - closest.second_parameter), -closest.second_parameter};
            double denominator = 0.0;
            for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
                const double coefficient = coefficients[corner];
                denominator += coefficient * coefficient *
                    prepared.inverse_masses[vertices[corner]];
            }
            if (!std::isfinite(penetration) || penetration <= 0.0 ||
                !std::isfinite(denominator)) {
                return core::Result<void>::failure(validation(
                    "cloth edge-edge contact projection has invalid weights"));
            }
            if (denominator <= 0.0) {
                return core::Result<void>::failure(validation(
                    "cloth edge-edge contact is pinned on both sides"));
            }
            const double delta_lambda = penetration / denominator;
            if (!std::isfinite(delta_lambda)) {
                return core::Result<void>::failure(validation(
                    "cloth edge-edge contact correction is not finite"));
            }
            for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
                const std::size_t vertex = vertices[corner];
                const core::Vec3d delta = normal * (coefficients[corner] *
                    prepared.inverse_masses[vertex] * delta_lambda);
                positions[vertex] = positions[vertex] + delta;
                const double correction = delta.length();
                if (!delta.finite() || !positions[vertex].finite() ||
                    !std::isfinite(correction)) {
                    return core::Result<void>::failure(validation(
                        "cloth edge-edge contact projection produced a non-finite state"));
                }
                maximum_correction = std::max(maximum_correction, correction);
            }
            return core::Result<void>::success();
        });
}

core::Result<void> project_self_collision(
    std::vector<core::Vec3d>& positions,
    PreparedCloth& prepared,
    std::uint64_t& total_work,
    double& maximum_correction) {
    if (auto result = project_vertex_vertex_self_collision(
            positions, prepared, total_work, maximum_correction); !result) {
        return result;
    }
    if (auto result = project_vertex_face_self_collision(
            positions, prepared, total_work, maximum_correction); !result) {
        return result;
    }
    return project_edge_edge_self_collision(
        positions, prepared, total_work, maximum_correction);
}

core::Result<double> measure_frame_error(
    const std::vector<core::Vec3d>& positions,
    const GarmentAsset& garment,
    const ClothSolverSettings& settings,
    PreparedCloth& prepared,
    std::uint64_t& total_work,
    std::uint32_t frame_index) {
    double maximum_distance_error = 0.0;
    double maximum_pin_error = 0.0;
    double maximum_capsule_penetration = 0.0;
    double maximum_self_collision_penetration = 0.0;
    for (const auto* constraints : {&prepared.stretch, &prepared.shear,
             &prepared.bend, &prepared.seams}) {
        for (const DistanceConstraint& constraint : *constraints) {
            const double error = std::abs(distance(
                positions[constraint.first], positions[constraint.second]) -
                constraint.rest_length);
            if (!std::isfinite(error)) {
                return core::Result<double>::failure(validation(
                    "cloth frame " + std::to_string(frame_index) +
                    " constraint error is not finite"));
            }
            maximum_distance_error = std::max(maximum_distance_error, error);
        }
    }
    for (const PinConstraint& pin : prepared.pins) {
        const double error = distance(positions[pin.vertex], pin.target);
        if (!std::isfinite(error)) {
            return core::Result<double>::failure(validation(
                "cloth frame " + std::to_string(frame_index) +
                " pin error is not finite"));
        }
        maximum_pin_error = std::max(maximum_pin_error, error);
    }
    for (const core::Vec3d position : positions) {
        for (const GarmentCollisionCapsule& capsule : garment.collision_capsules) {
            const core::Vec3d axis = capsule.end - capsule.start;
            const double axis_length_squared = axis.length_squared();
            const double t = axis_length_squared <= kMinimumRestLength * kMinimumRestLength ?
                0.0 : std::clamp(core::dot(position - capsule.start, axis) /
                    axis_length_squared, 0.0, 1.0);
            const core::Vec3d closest = capsule.start + axis * t;
            const double clearance = distance(position, closest) -
                capsule.radius - garment.fabric.thickness;
            if (!std::isfinite(clearance)) {
                return core::Result<double>::failure(validation(
                    "cloth frame " + std::to_string(frame_index) +
                    " capsule distance is not finite"));
            }
            maximum_capsule_penetration = std::max(
                maximum_capsule_penetration, std::max(0.0, -clearance));
        }
    }
    if (settings.enable_self_collision) {
        const double minimum_separation = prepared.thickness * 2.0;
        if (auto result = visit_self_collision_pairs(
                positions, prepared, minimum_separation, total_work,
                [&](std::size_t, std::size_t, double separation) {
                    maximum_self_collision_penetration = std::max(
                        maximum_self_collision_penetration, minimum_separation - separation);
                    return core::Result<void>::success();
                }); !result) {
            return core::Result<double>::failure(result.error());
        }
        if (auto result = visit_vertex_face_contacts(
                positions, prepared, minimum_separation, total_work,
                [&](std::size_t, const ClothTriangle&,
                    const TriangleClosestPoint&, core::Vec3d, double separation) {
                    maximum_self_collision_penetration = std::max(
                        maximum_self_collision_penetration,
                        minimum_separation - separation);
                    return core::Result<void>::success();
                }); !result) {
            return core::Result<double>::failure(result.error());
        }
        if (auto result = visit_edge_edge_contacts(
                positions, prepared, minimum_separation, total_work,
                [&](const ClothCollisionEdge&, const ClothCollisionEdge&,
                    const SegmentClosestPoints&, core::Vec3d, double separation) {
                    maximum_self_collision_penetration = std::max(
                        maximum_self_collision_penetration,
                        minimum_separation - separation);
                    return core::Result<void>::success();
                }); !result) {
            return core::Result<double>::failure(result.error());
        }
        const double self_collision_tolerance = std::max(1e-9, minimum_separation * 0.05);
        if (maximum_self_collision_penetration > self_collision_tolerance) {
            return core::Result<double>::failure(validation(
                "cloth frame " + std::to_string(frame_index) +
                " self-collision penetration exceeds its thickness-relative tolerance"));
        }
    }
    const double maximum_error = std::max({maximum_distance_error, maximum_pin_error,
        maximum_capsule_penetration, maximum_self_collision_penetration});
    if (!std::isfinite(maximum_error) || maximum_error > 0.05) {
        return core::Result<double>::failure(validation(
            "cloth frame " + std::to_string(frame_index) + " solver error " +
            std::to_string(maximum_error) +
            " exceeds the declared 0.05 meter bound (distance=" +
            std::to_string(maximum_distance_error) + ", pin=" +
            std::to_string(maximum_pin_error) + ", capsule=" +
            std::to_string(maximum_capsule_penetration) + ", self=" +
            std::to_string(maximum_self_collision_penetration) + ")"));
    }
    return core::Result<double>::success(maximum_error);
}

struct SimulationOutput {
    std::vector<ClothSimulationFrame> frames;
    std::uint64_t completed_substeps = 0U;
    double maximum_constraint_error = 0.0;
    double maximum_collision_correction = 0.0;
    std::optional<std::uint32_t> settled_frame;
};

core::Result<SimulationOutput> simulate(
    const GarmentAsset& garment,
    const geometry::EditableMesh& base_mesh,
    const ClothSolverSettings& settings) {
    if (auto result = settings.validate(); !result) {
        return core::Result<SimulationOutput>::failure(result.error());
    }
    auto prepared_result = prepare_cloth(
        garment, base_mesh, settings.enable_self_collision);
    if (!prepared_result) return core::Result<SimulationOutput>::failure(prepared_result.error());
    PreparedCloth prepared = std::move(prepared_result).value();
    const auto work_budget = validate_work_budget(prepared, settings);
    if (!work_budget) return core::Result<SimulationOutput>::failure(work_budget.error());
    std::uint64_t total_work = work_budget.value();
    std::vector<core::Vec3d> positions;
    std::vector<core::Vec3d> velocities(prepared.source_vertices.size());
    positions.reserve(prepared.source_vertices.size());
    for (const geometry::Vertex& vertex : prepared.source_vertices) {
        positions.push_back(vertex.position);
    }
    std::vector<core::Vec3d> last_frame_positions = positions;
    std::uint32_t stable_frame_count = 0U;
    const auto& inverse_masses = prepared.inverse_masses;
    const double delta_time = settings.frame_time_step /
        static_cast<double>(settings.substeps_per_frame);
    const double damping_factor = std::max(0.0, 1.0 - garment.fabric.damping * delta_time);
    SimulationOutput output;
    const std::uint64_t substep_count = static_cast<std::uint64_t>(settings.frame_count) *
        settings.substeps_per_frame;
    for (std::uint64_t substep = 0U; substep < substep_count; ++substep) {
        const std::vector<core::Vec3d> previous_positions = positions;
        for (DistanceConstraint& constraint : prepared.stretch) constraint.lambda = 0.0;
        for (DistanceConstraint& constraint : prepared.shear) constraint.lambda = 0.0;
        for (DistanceConstraint& constraint : prepared.bend) constraint.lambda = 0.0;
        for (DistanceConstraint& constraint : prepared.seams) constraint.lambda = 0.0;
        for (PinConstraint& pin : prepared.pins) pin.lambda = {};
        for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
            velocities[vertex] = (velocities[vertex] + settings.gravity * delta_time) * damping_factor;
            positions[vertex] = positions[vertex] + velocities[vertex] * delta_time;
            if (!velocities[vertex].finite() || !positions[vertex].finite()) {
                return core::Result<SimulationOutput>::failure(validation(
                    "cloth integration produced a non-finite state"));
            }
        }
        for (std::uint32_t iteration = 0U;
             iteration < settings.projection_iterations; ++iteration) {
            if (!project_constraints(prepared.stretch, positions, inverse_masses, delta_time) ||
                !project_constraints(prepared.shear, positions, inverse_masses, delta_time) ||
                !project_constraints(prepared.bend, positions, inverse_masses, delta_time) ||
                !project_constraints(prepared.seams, positions, inverse_masses, delta_time)) {
                return core::Result<SimulationOutput>::failure(validation(
                    "cloth XPBD distance projection produced a non-finite state"));
            }
            if (!project_capsules(positions, prepared, output.maximum_collision_correction)) {
                return core::Result<SimulationOutput>::failure(validation(
                    "cloth capsule collision projection produced a non-finite state"));
            }
            if (settings.enable_self_collision) {
                if (auto result = project_self_collision(positions, prepared, total_work,
                        output.maximum_collision_correction); !result) {
                    return core::Result<SimulationOutput>::failure(result.error());
                }
            }
            for (PinConstraint& pin : prepared.pins) {
                if (!project_pin(pin, positions, inverse_masses, delta_time)) {
                    return core::Result<SimulationOutput>::failure(validation(
                        "cloth attachment projection produced a non-finite state"));
                }
            }
        }
        for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
            velocities[vertex] = (positions[vertex] - previous_positions[vertex]) *
                (1.0 / delta_time);
            if (!velocities[vertex].finite()) {
                return core::Result<SimulationOutput>::failure(validation(
                    "cloth velocity reconstruction produced a non-finite state"));
            }
        }
        ++output.completed_substeps;
        if (output.completed_substeps % settings.substeps_per_frame == 0U) {
            const std::uint32_t frame_index = static_cast<std::uint32_t>(output.frames.size());
            const auto frame_error = measure_frame_error(positions, garment, settings,
                prepared, total_work, frame_index);
            if (!frame_error) {
                return core::Result<SimulationOutput>::failure(frame_error.error());
            }
            output.maximum_constraint_error = std::max(
                output.maximum_constraint_error, frame_error.value());
            ClothSimulationFrame frame;
            frame.index = frame_index;
            frame.vertices.reserve(prepared.source_vertices.size());
            for (std::size_t i = 0U; i < positions.size(); ++i) {
                const double movement = distance(positions[i], last_frame_positions[i]);
                if (!std::isfinite(movement)) {
                    return core::Result<SimulationOutput>::failure(validation(
                        "cloth frame displacement measurement is not finite"));
                }
                frame.maximum_frame_displacement = std::max(
                    frame.maximum_frame_displacement, movement);
                frame.vertices.push_back({prepared.source_vertices[i].id, positions[i]});
            }
            stable_frame_count = frame.maximum_frame_displacement <=
                    settings.settle_displacement_tolerance
                ? stable_frame_count + 1U : 0U;
            if (!output.settled_frame.has_value() &&
                stable_frame_count >= settings.settle_window_frames) {
                output.settled_frame = frame.index;
            }
            output.frames.push_back(std::move(frame));
            last_frame_positions = positions;
        }
    }
    if (output.frames.size() != settings.frame_count) {
        return core::Result<SimulationOutput>::failure(validation(
            "cloth solver did not produce one validated sample for every frame"));
    }
    if (settings.require_settled && !output.settled_frame.has_value()) {
        return core::Result<SimulationOutput>::failure(unsupported(
            "cloth simulation did not settle within its requested frame budget"));
    }
    return core::Result<SimulationOutput>::success(std::move(output));
}

std::string output_canonical(const ClothSimulationArtifact& artifact) {
    std::string result = "CARTO_CLOTH_OUTPUT 3\nSTEPS " +
        std::to_string(artifact.completed_substeps) + "\nERROR " +
        hex_f64(artifact.maximum_constraint_error) + "\nCOLLISION_CORRECTION " +
        hex_f64(artifact.maximum_collision_correction) + "\nFRAMES " +
        std::to_string(artifact.frames.size()) + "\nSETTLED " +
        (artifact.settled_frame.has_value()
            ? std::to_string(artifact.settled_frame.value()) : "none") + "\n";
    for (const ClothSimulationFrame& frame : artifact.frames) {
        result += "FRAME " + std::to_string(frame.index) + " VERTICES " +
            std::to_string(frame.vertices.size()) + " MOVEMENT " +
            hex_f64(frame.maximum_frame_displacement) + "\n";
        for (const geometry::Vertex& vertex : frame.vertices) {
            result += "VERTEX " + std::to_string(vertex.id.value) + " " +
                hex_f64(vertex.position.x) + " " + hex_f64(vertex.position.y) + " " +
                hex_f64(vertex.position.z) + "\n";
        }
    }
    return result;
}

std::string artifact_canonical(
    const BakeMeshBinding& source,
    const assets::Sha256Digest& garment_digest,
    std::string_view algorithm,
    std::string_view evidence_class,
    const assets::Sha256Digest& settings_digest,
    const assets::Sha256Digest& output_digest,
    std::uint64_t byte_size) {
    return "CARTO_CLOTH_ARTIFACT 3\nSOURCE " + hex_text(source.identity) + " " +
        std::to_string(source.revision.value()) + " " + source.content_digest.hex() +
        "\nGARMENT " + garment_digest.hex() + "\nALGORITHM " + hex_text(algorithm) +
        "\nEVIDENCE " + hex_text(evidence_class) + "\nSETTINGS " + settings_digest.hex() +
        "\nOUTPUT " + output_digest.hex() + "\nBYTES " + std::to_string(byte_size) + "\n";
}

core::Result<geometry::EditableMesh> make_rest_pose_mesh(
    const ClothSimulationFrame& frame,
    const geometry::EditableMesh& source_mesh) {
    const std::vector<geometry::Vertex> source_vertices = source_mesh.vertices_sorted();
    if (frame.vertices.size() != source_vertices.size()) {
        return core::Result<geometry::EditableMesh>::failure(validation(
            "settled cloth frame vertex count differs from its source mesh"));
    }
    geometry::MeshPatch patch;
    patch.expected_revision = source_mesh.revision();
    patch.vertex_changes.reserve(source_vertices.size());
    for (std::size_t index = 0U; index < source_vertices.size(); ++index) {
        const geometry::Vertex& source_vertex = source_vertices[index];
        const geometry::Vertex& frame_vertex = frame.vertices[index];
        if (source_vertex.id != frame_vertex.id || !frame_vertex.position.finite()) {
            return core::Result<geometry::EditableMesh>::failure(validation(
                "settled cloth frame identity or position differs from its source mesh"));
        }
        if (source_vertex.position.x != frame_vertex.position.x ||
            source_vertex.position.y != frame_vertex.position.y ||
            source_vertex.position.z != frame_vertex.position.z) {
            patch.vertex_changes.push_back(geometry::VertexChange{
                source_vertex.id, source_vertex.position, frame_vertex.position});
        }
    }

    geometry::EditableMesh baked_mesh = source_mesh;
    if (!patch.vertex_changes.empty()) {
        if (auto result = baked_mesh.apply_patch(patch); !result) {
            return core::Result<geometry::EditableMesh>::failure(result.error());
        }
    }
    if (auto result = baked_mesh.validate(); !result) {
        return core::Result<geometry::EditableMesh>::failure(result.error());
    }
    return core::Result<geometry::EditableMesh>::success(std::move(baked_mesh));
}

} // namespace

core::Result<void> ClothSolverSettings::validate() const {
    if (!std::isfinite(frame_time_step) || frame_time_step < 1e-5 || frame_time_step > 0.1 ||
        frame_count == 0U || frame_count > kMaxClothFrames ||
        substeps_per_frame == 0U || substeps_per_frame > kMaxClothSubsteps ||
        projection_iterations == 0U || projection_iterations > kMaxClothIterations ||
        !gravity.finite() || std::abs(gravity.x) > 1e3 || std::abs(gravity.y) > 1e3 ||
        std::abs(gravity.z) > 1e3 ||
        !std::isfinite(settle_displacement_tolerance) ||
        settle_displacement_tolerance <= 0.0 || settle_displacement_tolerance > 1e3 ||
        settle_window_frames == 0U || settle_window_frames > kMaxClothFrames ||
        (require_settled && settle_window_frames > frame_count)) {
        return core::Result<void>::failure(invalid(
            "cloth timestep, counts, or gravity are outside their supported bounds"));
    }
    return core::Result<void>::success();
}

std::string ClothSolverSettings::canonical() const {
    return "CARTO_CLOTH_SETTINGS 2\nTIME " + hex_f64(frame_time_step) + "\nFRAMES " +
        std::to_string(frame_count) + "\nSUBSTEPS " +
        std::to_string(substeps_per_frame) + "\nITERATIONS " +
        std::to_string(projection_iterations) + "\nGRAVITY " + hex_f64(gravity.x) + " " +
        hex_f64(gravity.y) + " " + hex_f64(gravity.z) + "\nSELF_COLLISION " +
        (enable_self_collision ? "1\n" : "0\n") + "SETTLE_TOLERANCE " +
        hex_f64(settle_displacement_tolerance) + "\nSETTLE_WINDOW " +
        std::to_string(settle_window_frames) + "\nREQUIRE_SETTLED " +
        (require_settled ? "1\n" : "0\n");
}

core::Result<assets::Sha256Digest> ClothSolverSettings::digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(canonical()));
}

std::string ClothSimulationArtifact::canonical() const {
    return artifact_canonical(source, garment_digest, algorithm, evidence_class,
        settings_digest, output_digest, byte_size) + "SETTINGS_VALUE " +
        settings.canonical() + output_canonical(*this);
}

core::Result<std::vector<std::uint8_t>> ClothSimulationArtifact::serialize() const {
    ClothSimulationArtifact refreshed = *this;
    if (auto result = refreshed.refresh_digests(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    const std::string text = canonical();
    if (refreshed.canonical() != text || text.size() > kMaxClothSerializedBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "cloth artifact serialization has stale digests or exceeds its byte bound"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(
        std::vector<std::uint8_t>(text.begin(), text.end()));
}

core::Result<ClothSimulationArtifact> ClothSimulationArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxClothSerializedBytes) {
        return core::Result<ClothSimulationArtifact>::failure(unsupported(
            "serialized cloth artifact is empty or exceeds its byte bound"));
    }
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream input(text);
    auto token = [&input](std::string& value) {
        return static_cast<bool>(input >> value);
    };
    auto expect = [&token](std::string_view expected, std::string& value) {
        return token(value) && value == expected;
    };
    auto read_u64 = [&token](std::uint64_t& value) {
        std::string encoded;
        if (!token(encoded)) return false;
        const auto parsed = parse_u64(encoded);
        if (!parsed) return false;
        value = *parsed;
        return true;
    };
    auto read_f64 = [&token](double& value) {
        std::string encoded;
        if (!token(encoded)) return false;
        const auto parsed = parse_hex_f64(encoded);
        if (!parsed) return false;
        value = *parsed;
        return true;
    };
    auto read_digest = [&token](assets::Sha256Digest& value) {
        std::string encoded;
        if (!token(encoded)) return false;
        const auto parsed = assets::Sha256Digest::from_hex(encoded);
        if (!parsed) return false;
        value = parsed.value();
        return true;
    };

    ClothSimulationArtifact artifact;
    std::string field;
    std::uint64_t version = 0U;
    if (!expect("CARTO_CLOTH_ARTIFACT", field) || !read_u64(version) || version != 3U) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth artifact header is invalid"));
    }
    std::string source_identity_hex;
    std::uint64_t source_revision = 0U;
    if (!expect("SOURCE", field) || !token(source_identity_hex) ||
        !read_u64(source_revision) || !read_digest(artifact.source.content_digest)) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth source binding is invalid"));
    }
    const auto source_identity = unhex_text(source_identity_hex, 1024U);
    if (!source_identity) return core::Result<ClothSimulationArtifact>::failure(validation(
        "serialized cloth source identity encoding is invalid"));
    artifact.source.identity = *source_identity;
    artifact.source.revision = core::Revision{source_revision};
    if (!expect("GARMENT", field) || !read_digest(artifact.garment_digest)) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth GarmentAsset binding is invalid"));
    }
    std::string algorithm_hex;
    std::string evidence_hex;
    if (!expect("ALGORITHM", field) || !token(algorithm_hex) ||
        !expect("EVIDENCE", field) || !token(evidence_hex) ||
        !expect("SETTINGS", field) || !read_digest(artifact.settings_digest) ||
        !expect("OUTPUT", field) || !read_digest(artifact.output_digest) ||
        !expect("BYTES", field) || !read_u64(artifact.byte_size)) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth artifact provenance is invalid"));
    }
    const auto algorithm = unhex_text(algorithm_hex, 128U);
    const auto evidence = unhex_text(evidence_hex, 128U);
    if (!algorithm || !evidence) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth algorithm or evidence encoding is invalid"));
    }
    artifact.algorithm = *algorithm;
    artifact.evidence_class = *evidence;
    if (!expect("SETTINGS_VALUE", field) ||
        !expect("CARTO_CLOTH_SETTINGS", field) || !read_u64(version) || version != 2U ||
        !expect("TIME", field) || !read_f64(artifact.settings.frame_time_step)) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth solver settings header is invalid"));
    }
    std::uint64_t frame_count = 0U;
    std::uint64_t substeps = 0U;
    std::uint64_t iterations = 0U;
    if (!expect("FRAMES", field) || !read_u64(frame_count) ||
        frame_count > std::numeric_limits<std::uint32_t>::max() ||
        !expect("SUBSTEPS", field) || !read_u64(substeps) ||
        substeps > std::numeric_limits<std::uint32_t>::max() ||
        !expect("ITERATIONS", field) || !read_u64(iterations) ||
        iterations > std::numeric_limits<std::uint32_t>::max() ||
        !expect("GRAVITY", field) || !read_f64(artifact.settings.gravity.x) ||
        !read_f64(artifact.settings.gravity.y) || !read_f64(artifact.settings.gravity.z)) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth solver settings values are invalid"));
    }
    artifact.settings.frame_count = static_cast<std::uint32_t>(frame_count);
    artifact.settings.substeps_per_frame = static_cast<std::uint32_t>(substeps);
    artifact.settings.projection_iterations = static_cast<std::uint32_t>(iterations);
    std::uint64_t self_collision = 0U;
    std::uint64_t settle_window = 0U;
    std::uint64_t require_settled = 0U;
    if (!expect("SELF_COLLISION", field) || !read_u64(self_collision) ||
        self_collision > 1U || !expect("SETTLE_TOLERANCE", field) ||
        !read_f64(artifact.settings.settle_displacement_tolerance) ||
        !expect("SETTLE_WINDOW", field) || !read_u64(settle_window) ||
        settle_window > std::numeric_limits<std::uint32_t>::max() ||
        !expect("REQUIRE_SETTLED", field) || !read_u64(require_settled) ||
        require_settled > 1U || !expect("CARTO_CLOTH_OUTPUT", field) ||
        !read_u64(version) || version != 3U || !expect("STEPS", field) ||
        !read_u64(artifact.completed_substeps) || !expect("ERROR", field) ||
        !read_f64(artifact.maximum_constraint_error) ||
        !expect("COLLISION_CORRECTION", field) ||
        !read_f64(artifact.maximum_collision_correction) ||
        !expect("FRAMES", field)) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth output metrics are invalid"));
    }
    artifact.settings.enable_self_collision = self_collision == 1U;
    artifact.settings.settle_window_frames = static_cast<std::uint32_t>(settle_window);
    artifact.settings.require_settled = require_settled == 1U;
    if (auto result = artifact.settings.validate(); !result) {
        return core::Result<ClothSimulationArtifact>::failure(result.error());
    }
    std::uint64_t output_frame_count = 0U;
    std::string settled_token;
    if (!read_u64(output_frame_count) || output_frame_count != artifact.settings.frame_count ||
        !expect("SETTLED", field) || !token(settled_token)) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth output frame count differs from its settings"));
    }
    if (settled_token != "none") {
        const auto settled_frame = parse_u64(settled_token);
        if (!settled_frame || *settled_frame > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<ClothSimulationArtifact>::failure(validation(
                "serialized cloth settled-frame index is invalid"));
        }
        artifact.settled_frame = static_cast<std::uint32_t>(*settled_frame);
    }
    artifact.frames.reserve(static_cast<std::size_t>(output_frame_count));
    std::uint64_t common_vertex_count = 0U;
    std::uint64_t total_samples = 0U;
    for (std::uint64_t frame_index = 0U; frame_index < output_frame_count; ++frame_index) {
        std::uint64_t encoded_index = 0U;
        std::uint64_t vertex_count = 0U;
        double frame_displacement = 0.0;
        if (!expect("FRAME", field) || !read_u64(encoded_index) ||
            encoded_index != frame_index || !expect("VERTICES", field) ||
            !read_u64(vertex_count) || vertex_count == 0U ||
            vertex_count > kMaxClothVertices ||
            (frame_index > 0U && vertex_count != common_vertex_count) ||
            !expect("MOVEMENT", field) || !read_f64(frame_displacement)) {
            return core::Result<ClothSimulationArtifact>::failure(validation(
                "serialized cloth frame index or vertex count is invalid"));
        }
        if (frame_index == 0U) {
            common_vertex_count = vertex_count;
            if (!checked_multiply(output_frame_count, common_vertex_count, total_samples) ||
                total_samples > kMaxClothSimulationSamples ||
                total_samples > (kMaxClothSimulationArtifactBytes - 4096U) / 96U) {
                return core::Result<ClothSimulationArtifact>::failure(unsupported(
                    "serialized cloth frame sequence exceeds its sample or byte bound"));
            }
        }
        ClothSimulationFrame frame;
        frame.index = static_cast<std::uint32_t>(frame_index);
        frame.maximum_frame_displacement = frame_displacement;
        frame.vertices.reserve(static_cast<std::size_t>(vertex_count));
        for (std::uint64_t index = 0U; index < vertex_count; ++index) {
            std::uint64_t vertex_id = 0U;
            geometry::Vertex vertex;
            if (!expect("VERTEX", field) || !read_u64(vertex_id) || vertex_id == 0U ||
                !read_f64(vertex.position.x) || !read_f64(vertex.position.y) ||
                !read_f64(vertex.position.z)) {
                return core::Result<ClothSimulationArtifact>::failure(validation(
                    "serialized cloth vertex record is invalid"));
            }
            vertex.id = geometry::VertexId{vertex_id};
            frame.vertices.push_back(vertex);
        }
        artifact.frames.push_back(std::move(frame));
    }
    input >> std::ws;
    if (!input.eof() || artifact.canonical() != text) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth artifact is non-canonical or has trailing data"));
    }
    const assets::Sha256Digest declared_settings_digest = artifact.settings_digest;
    const assets::Sha256Digest declared_output_digest = artifact.output_digest;
    const std::uint64_t declared_byte_size = artifact.byte_size;
    if (auto result = artifact.refresh_digests(); !result) {
        return core::Result<ClothSimulationArtifact>::failure(result.error());
    }
    if (artifact.settings_digest != declared_settings_digest ||
        artifact.output_digest != declared_output_digest ||
        artifact.byte_size != declared_byte_size) {
        return core::Result<ClothSimulationArtifact>::failure(validation(
            "serialized cloth artifact digest does not bind its settings and output"));
    }
    return core::Result<ClothSimulationArtifact>::success(std::move(artifact));
}

core::Result<void> ClothSimulationArtifact::refresh_digests() {
    if (auto result = source.validate(); !result) return result;
    if (algorithm != kAlgorithm || evidence_class != kEvidenceClass ||
        garment_digest.is_zero() || frames.empty() ||
        !std::isfinite(maximum_constraint_error) || maximum_constraint_error < 0.0 ||
        maximum_constraint_error > 0.05 ||
        !std::isfinite(maximum_collision_correction) || maximum_collision_correction < 0.0) {
        return core::Result<void>::failure(validation(
            "cloth artifact identity, metrics, or output are invalid"));
    }
    if (auto result = settings.validate(); !result) return result;
    if (frames.size() != settings.frame_count) {
        return core::Result<void>::failure(validation(
            "cloth artifact frame count differs from its settings"));
    }
    const std::size_t vertex_count = frames.front().vertices.size();
    if (vertex_count == 0U || vertex_count > kMaxClothVertices) {
        return core::Result<void>::failure(validation(
            "cloth artifact frame vertex count is outside its bound"));
    }
    std::uint64_t total_samples = 0U;
    if (!checked_multiply(static_cast<std::uint64_t>(frames.size()),
            static_cast<std::uint64_t>(vertex_count), total_samples) ||
        total_samples > kMaxClothSimulationSamples ||
        total_samples > (kMaxClothSimulationArtifactBytes - 4096U) / 96U) {
        return core::Result<void>::failure(unsupported(
            "cloth artifact frame sequence exceeds its sample or byte bound"));
    }
    std::uint64_t expected_substeps = 0U;
    if (!checked_multiply(settings.frame_count, settings.substeps_per_frame,
            expected_substeps) || completed_substeps != expected_substeps) {
        return core::Result<void>::failure(validation(
            "cloth artifact completed substep count differs from its settings"));
    }
    const auto settings_hash = settings.digest();
    if (!settings_hash) return core::Result<void>::failure(settings_hash.error());
    settings_digest = settings_hash.value();
    std::optional<std::uint32_t> derived_settled_frame;
    std::uint32_t stable_frame_count = 0U;
    for (std::size_t frame_index = 0U; frame_index < frames.size(); ++frame_index) {
        const ClothSimulationFrame& frame = frames[frame_index];
        if (frame.index != frame_index || frame.vertices.size() != vertex_count ||
            !std::isfinite(frame.maximum_frame_displacement) ||
            frame.maximum_frame_displacement < 0.0) {
            return core::Result<void>::failure(validation(
                "cloth frame indexes, vertex counts, or displacement metrics are invalid"));
        }
        stable_frame_count = frame.maximum_frame_displacement <=
                settings.settle_displacement_tolerance
            ? stable_frame_count + 1U : 0U;
        if (!derived_settled_frame.has_value() &&
            stable_frame_count >= settings.settle_window_frames) {
            derived_settled_frame = frame.index;
        }
        for (std::size_t vertex_index = 0U; vertex_index < frame.vertices.size();
             ++vertex_index) {
            const geometry::Vertex& vertex = frame.vertices[vertex_index];
            if (!vertex.id || !vertex.position.finite() ||
                (vertex_index > 0U &&
                    frame.vertices[vertex_index - 1U].id >= vertex.id) ||
                (frame_index > 0U &&
                    frames[0].vertices[vertex_index].id != vertex.id)) {
                return core::Result<void>::failure(validation(
                    "cloth frame vertices must be finite, sorted, unique, and source-stable"));
            }
        }
    }
    if (settled_frame != derived_settled_frame) {
        return core::Result<void>::failure(validation(
            "cloth settled-frame receipt does not match its per-frame movement metrics"));
    }
    if (settings.require_settled && !settled_frame.has_value()) {
        return core::Result<void>::failure(unsupported(
            "cloth artifact does not satisfy its required settle window"));
    }
    const std::string output = output_canonical(*this);
    if (output.size() > kMaxClothSimulationArtifactBytes) {
        return core::Result<void>::failure(unsupported(
            "cloth output exceeds the canonical byte bound"));
    }
    output_digest = hash_text(output);
    byte_size = static_cast<std::uint64_t>(output.size());
    artifact_digest = hash_text(artifact_canonical(source, garment_digest, algorithm,
        evidence_class, settings_digest, output_digest, byte_size));
    return core::Result<void>::success();
}

core::Result<void> ClothSimulationArtifact::validate(
    const GarmentAsset& garment,
    const geometry::EditableMesh& base_mesh) const {
    if (auto result = garment.validate(base_mesh); !result) return result;
    if (source != garment.source) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "cloth artifact source binding differs from its GarmentAsset"));
    }
    const auto current_garment_digest = garment.digest();
    if (!current_garment_digest || current_garment_digest.value() != garment_digest) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "cloth artifact GarmentAsset digest is stale"));
    }
    ClothSimulationArtifact refreshed = *this;
    if (auto result = refreshed.refresh_digests(); !result) return result;
    if (refreshed.settings_digest != settings_digest || refreshed.output_digest != output_digest ||
        refreshed.artifact_digest != artifact_digest || refreshed.byte_size != byte_size) {
        return core::Result<void>::failure(validation(
            "cloth artifact digest does not match its source, settings, and output"));
    }
    const auto rebuilt = simulate(garment, base_mesh, settings);
    if (!rebuilt) return core::Result<void>::failure(rebuilt.error());
    ClothSimulationArtifact expected = *this;
    expected.frames = rebuilt.value().frames;
    expected.completed_substeps = rebuilt.value().completed_substeps;
    expected.maximum_constraint_error = rebuilt.value().maximum_constraint_error;
    expected.maximum_collision_correction = rebuilt.value().maximum_collision_correction;
    expected.settled_frame = rebuilt.value().settled_frame;
    if (output_canonical(*this) != output_canonical(expected)) {
        return core::Result<void>::failure(validation(
            "cloth output differs from deterministic CPU reconstruction"));
    }
    return core::Result<void>::success();
}

std::string ClothRestPoseBakeReceipt::canonical() const {
    return "CARTO_CLOTH_REST_POSE_RECEIPT 1\nALGORITHM " + hex_text(algorithm) +
        "\nSOURCE " + hex_text(source.identity) + " " +
        std::to_string(source.revision.value()) + " " + source.content_digest.hex() +
        "\nGARMENT " + garment_digest.hex() + "\nSIMULATION " +
        simulation_artifact_digest.hex() + "\nFRAME " + std::to_string(frame_index) +
        "\nOUTPUT " + hex_text(output.identity) + " " +
        std::to_string(output.revision.value()) + " " + output.content_digest.hex() +
        "\nVERTICES " + std::to_string(vertex_count) + "\n";
}

core::Result<void> ClothRestPoseBakeReceipt::refresh_digest() {
    if (algorithm != "carto.cloth.rest-pose-bake.v1" ||
        garment_digest.is_zero() || simulation_artifact_digest.is_zero() ||
        frame_index >= kMaxClothFrames || vertex_count == 0U ||
        vertex_count > kMaxClothVertices ||
        output.identity != "carto/cloth-rest-pose/" + simulation_artifact_digest.hex()) {
        return core::Result<void>::failure(validation(
            "cloth rest-pose receipt values are outside their supported bounds"));
    }
    if (auto result = source.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (auto result = output.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    receipt_digest = hash_text(canonical());
    return core::Result<void>::success();
}

core::Result<void> ClothRestPoseBakeReceipt::validate() const {
    ClothRestPoseBakeReceipt refreshed = *this;
    if (auto result = refreshed.refresh_digest(); !result) return result;
    if (refreshed.receipt_digest != receipt_digest) {
        return core::Result<void>::failure(validation(
            "cloth rest-pose receipt digest does not match its bindings"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> ClothRestPoseBakeReceipt::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    const std::string text = canonical() + "RECEIPT " + receipt_digest.hex() + "\n";
    if (text.size() > kMaxClothRestPoseReceiptBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(unsupported(
            "cloth rest-pose receipt exceeds its serialized byte bound"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(
        std::vector<std::uint8_t>(text.begin(), text.end()));
}

core::Result<ClothRestPoseBakeReceipt> ClothRestPoseBakeReceipt::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxClothRestPoseReceiptBytes) {
        return core::Result<ClothRestPoseBakeReceipt>::failure(invalid(
            "serialized cloth rest-pose receipt is empty or exceeds its byte bound"));
    }
    const std::string text(bytes.begin(), bytes.end());
    std::istringstream input(text);
    ClothRestPoseBakeReceipt receipt;
    std::string field;
    std::string version_token;
    std::string algorithm_hex;
    std::string source_identity_hex;
    std::string source_revision_token;
    std::string source_digest_hex;
    std::string garment_digest_hex;
    std::string simulation_digest_hex;
    std::string frame_index_token;
    std::string output_identity_hex;
    std::string output_revision_token;
    std::string output_digest_hex;
    std::string vertex_count_token;
    std::string receipt_digest_hex;
    if (!(input >> field >> version_token) || field != "CARTO_CLOTH_REST_POSE_RECEIPT" ||
        version_token != "1" || !(input >> field >> algorithm_hex) || field != "ALGORITHM" ||
        !(input >> field >> source_identity_hex >> source_revision_token >> source_digest_hex) ||
        field != "SOURCE" || !(input >> field >> garment_digest_hex) || field != "GARMENT" ||
        !(input >> field >> simulation_digest_hex) || field != "SIMULATION" ||
        !(input >> field >> frame_index_token) || field != "FRAME" ||
        !(input >> field >> output_identity_hex >> output_revision_token >> output_digest_hex) ||
        field != "OUTPUT" || !(input >> field >> vertex_count_token) || field != "VERTICES" ||
        !(input >> field >> receipt_digest_hex) || field != "RECEIPT") {
        return core::Result<ClothRestPoseBakeReceipt>::failure(validation(
            "serialized cloth rest-pose receipt fields are invalid"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ClothRestPoseBakeReceipt>::failure(validation(
            "serialized cloth rest-pose receipt contains trailing fields"));
    }
    const auto algorithm = unhex_text(algorithm_hex, 128U);
    const auto source_identity = unhex_text(source_identity_hex, 1024U);
    const auto output_identity = unhex_text(output_identity_hex, 1024U);
    const auto source_revision = parse_u64(source_revision_token);
    const auto output_revision = parse_u64(output_revision_token);
    const auto frame_index = parse_u64(frame_index_token);
    const auto vertex_count = parse_u64(vertex_count_token);
    const auto source_digest = assets::Sha256Digest::from_hex(source_digest_hex);
    const auto garment_digest = assets::Sha256Digest::from_hex(garment_digest_hex);
    const auto simulation_digest = assets::Sha256Digest::from_hex(simulation_digest_hex);
    const auto output_digest = assets::Sha256Digest::from_hex(output_digest_hex);
    const auto receipt_digest = assets::Sha256Digest::from_hex(receipt_digest_hex);
    if (!algorithm || !source_identity || !output_identity || !source_revision ||
        !output_revision || !frame_index || *frame_index > std::numeric_limits<std::uint32_t>::max() ||
        !vertex_count || !source_digest || !garment_digest || !simulation_digest ||
        !output_digest || !receipt_digest) {
        return core::Result<ClothRestPoseBakeReceipt>::failure(validation(
            "serialized cloth rest-pose receipt encoding is invalid"));
    }
    receipt.algorithm = *algorithm;
    receipt.source = BakeMeshBinding{
        *source_identity, core::Revision{*source_revision}, source_digest.value()};
    receipt.garment_digest = garment_digest.value();
    receipt.simulation_artifact_digest = simulation_digest.value();
    receipt.frame_index = static_cast<std::uint32_t>(*frame_index);
    receipt.output = BakeMeshBinding{
        *output_identity, core::Revision{*output_revision}, output_digest.value()};
    receipt.vertex_count = *vertex_count;
    receipt.receipt_digest = receipt_digest.value();
    if (auto result = receipt.validate(); !result) {
        return core::Result<ClothRestPoseBakeReceipt>::failure(result.error());
    }
    const auto serialized = receipt.serialize();
    if (!serialized || std::string(serialized.value().begin(), serialized.value().end()) != text) {
        return core::Result<ClothRestPoseBakeReceipt>::failure(validation(
            "serialized cloth rest-pose receipt is not canonical"));
    }
    return core::Result<ClothRestPoseBakeReceipt>::success(std::move(receipt));
}

core::Result<void> ClothRestPoseBakeResult::validate(
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& source_mesh) const {
    if (auto result = receipt.validate(); !result) return result;
    if (auto result = simulation.validate(garment, source_mesh); !result) return result;
    if (!simulation.settled_frame.has_value() ||
        receipt.source != simulation.source || receipt.source != garment.source ||
        receipt.garment_digest != simulation.garment_digest ||
        receipt.simulation_artifact_digest != simulation.artifact_digest ||
        receipt.frame_index != simulation.settled_frame.value() ||
        receipt.vertex_count != source_mesh.vertex_count() ||
        mesh.vertex_count() != receipt.vertex_count || mesh.face_count() != source_mesh.face_count()) {
        return core::Result<void>::failure(validation(
            "cloth rest-pose receipt does not match its settled simulation or source mesh"));
    }
    const auto frame = std::find_if(simulation.frames.begin(), simulation.frames.end(),
        [this](const ClothSimulationFrame& candidate) {
            return candidate.index == receipt.frame_index;
        });
    if (frame == simulation.frames.end()) {
        return core::Result<void>::failure(validation(
            "cloth rest-pose receipt selects a missing settled frame"));
    }
    const auto expected_mesh = make_rest_pose_mesh(*frame, source_mesh);
    if (!expected_mesh) return core::Result<void>::failure(expected_mesh.error());
    const auto expected_binding = bind_bake_source(receipt.output.identity, expected_mesh.value());
    const auto actual_binding = bind_bake_source(receipt.output.identity, mesh);
    if (!expected_binding || !actual_binding || expected_binding.value() != receipt.output ||
        actual_binding.value() != receipt.output) {
        return core::Result<void>::failure(validation(
            "cloth rest-pose mesh differs from its deterministic settled-frame reconstruction"));
    }
    return core::Result<void>::success();
}

core::Result<ClothRestPoseBakeResult> bake_cloth_settled_rest_pose(
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& source_mesh) {
    if (auto result = simulation.validate(garment, source_mesh); !result) {
        return core::Result<ClothRestPoseBakeResult>::failure(result.error());
    }
    if (!simulation.settled_frame.has_value()) {
        return core::Result<ClothRestPoseBakeResult>::failure(unsupported(
            "cloth simulation has no validated settled frame to bake"));
    }
    const auto frame = std::find_if(simulation.frames.begin(), simulation.frames.end(),
        [&simulation](const ClothSimulationFrame& candidate) {
            return candidate.index == simulation.settled_frame.value();
        });
    if (frame == simulation.frames.end()) {
        return core::Result<ClothRestPoseBakeResult>::failure(validation(
            "cloth simulation settle receipt selects a missing frame"));
    }
    auto baked_mesh = make_rest_pose_mesh(*frame, source_mesh);
    if (!baked_mesh) {
        return core::Result<ClothRestPoseBakeResult>::failure(baked_mesh.error());
    }
    const std::string output_identity =
        "carto/cloth-rest-pose/" + simulation.artifact_digest.hex();
    auto output_binding = bind_bake_source(output_identity, baked_mesh.value());
    if (!output_binding) {
        return core::Result<ClothRestPoseBakeResult>::failure(output_binding.error());
    }

    ClothRestPoseBakeResult result;
    result.mesh = std::move(baked_mesh).value();
    result.receipt.source = simulation.source;
    result.receipt.garment_digest = simulation.garment_digest;
    result.receipt.simulation_artifact_digest = simulation.artifact_digest;
    result.receipt.frame_index = simulation.settled_frame.value();
    result.receipt.output = std::move(output_binding).value();
    result.receipt.vertex_count = frame->vertices.size();
    if (auto refresh = result.receipt.refresh_digest(); !refresh) {
        return core::Result<ClothRestPoseBakeResult>::failure(refresh.error());
    }
    if (auto validation_result = result.validate(simulation, garment, source_mesh);
        !validation_result) {
        return core::Result<ClothRestPoseBakeResult>::failure(validation_result.error());
    }
    return core::Result<ClothRestPoseBakeResult>::success(std::move(result));
}

core::Result<void> stage_cloth_rest_pose_bake_receipt(
    project::ProjectTransaction& transaction,
    std::uint64_t output_mesh_asset,
    const ClothRestPoseBakeResult& result,
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& source_mesh) {
    if (output_mesh_asset == 0U || output_mesh_asset == garment.mesh_asset) {
        return core::Result<void>::failure(invalid(
            "cloth rest-pose receipt requires a distinct derived mesh asset"));
    }
    if (auto validation_result = result.validate(simulation, garment, source_mesh);
        !validation_result) {
        return validation_result;
    }
    const auto staged_mesh = transaction.staged_document().meshes().find(output_mesh_asset);
    if (staged_mesh == transaction.staged_document().meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "cloth rest-pose receipt target mesh is not staged in the project transaction"));
    }
    const auto staged_binding = bind_bake_source(
        result.receipt.output.identity, staged_mesh->second);
    if (!staged_binding || staged_binding.value() != result.receipt.output) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "staged mesh does not match the cloth rest-pose receipt output binding"));
    }
    const auto payload = result.receipt.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const auto digest = assets::sha256(std::span<const std::uint8_t>{payload.value()});
    std::string serialized_receipt(
        reinterpret_cast<const char*>(payload.value().data()), payload.value().size());
    return transaction.set_cloth_rest_pose_receipt(
        output_mesh_asset, std::move(serialized_receipt), digest);
}

core::Result<ProductionCookKey> cloth_rest_pose_bake_cook_key(
    const ClothSimulationArtifact& simulation,
    std::string platform_profile) {
    if (auto result = simulation.settings.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    if (!simulation.settled_frame.has_value()) {
        return core::Result<ProductionCookKey>::failure(unsupported(
            "cloth rest-pose cook key requires a settled simulation frame"));
    }
    ClothSimulationArtifact refreshed = simulation;
    if (auto result = refreshed.refresh_digests(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    if (refreshed.artifact_digest != simulation.artifact_digest) {
        return core::Result<ProductionCookKey>::failure(validation(
            "cloth rest-pose cook key source simulation digest is invalid"));
    }
    if (auto result = simulation.source.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    ProductionCookKey key;
    key.source = AuthoritativeSource{
        simulation.source.identity, simulation.source.revision,
        simulation.source.content_digest};
    key.kind = ArtifactKind::cloth_rest_pose;
    key.form = ExecutionForm::offline;
    key.algorithm = "carto.cloth.rest-pose-bake.v1";
    key.settings_digest = hash_text(
        "CARTO_CLOTH_REST_POSE_SETTINGS 1\nSIMULATION " +
        simulation.artifact_digest.hex() + "\nFRAME " +
        std::to_string(simulation.settled_frame.value()) + "\n");
    key.platform_profile = std::move(platform_profile);
    key.representation_kind = "cloth.rest-pose.static-mesh";
    key.spatial_scope = "whole-garment";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<void> publish_cloth_rest_pose_bake(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const ClothRestPoseBakeResult& result,
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_source_mesh,
    const AdmissionPolicy& policy) {
    if (auto validation_result = result.validate(simulation, garment, current_source_mesh);
        !validation_result) {
        return validation_result;
    }
    const auto expected_key = cloth_rest_pose_bake_cook_key(
        simulation, receipt.key.platform_profile);
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "cloth rest-pose production key differs from its source simulation or settled frame"));
    }
    if (receipt.quality.measured_error != simulation.maximum_constraint_error) {
        return core::Result<void>::failure(validation(
            "cloth rest-pose production error metric differs from its verified simulation"));
    }
    const auto payload = result.receipt.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const AuthoritativeSource current_source{
        result.receipt.source.identity, result.receipt.source.revision,
        result.receipt.source.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_source, policy);
}

core::Result<ClothRestPoseBakeResult> read_cloth_rest_pose_bake(
    const ProductionArtifactStore& store,
    const ClothSimulationArtifact& simulation,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_source_mesh,
    std::string platform_profile,
    const AdmissionPolicy& policy) {
    if (auto result = simulation.validate(garment, current_source_mesh); !result) {
        return core::Result<ClothRestPoseBakeResult>::failure(result.error());
    }
    const auto key = cloth_rest_pose_bake_cook_key(simulation, std::move(platform_profile));
    if (!key) return core::Result<ClothRestPoseBakeResult>::failure(key.error());
    const auto payload = store.read(key.value(), key.value().source, policy);
    if (!payload) return core::Result<ClothRestPoseBakeResult>::failure(payload.error());
    const auto stored_receipt = ClothRestPoseBakeReceipt::deserialize(
        std::span<const std::uint8_t>{payload.value()});
    if (!stored_receipt) {
        return core::Result<ClothRestPoseBakeResult>::failure(stored_receipt.error());
    }
    const auto rebuilt = bake_cloth_settled_rest_pose(
        simulation, garment, current_source_mesh);
    if (!rebuilt) return core::Result<ClothRestPoseBakeResult>::failure(rebuilt.error());
    if (stored_receipt.value().canonical() != rebuilt.value().receipt.canonical() ||
        stored_receipt.value().receipt_digest != rebuilt.value().receipt.receipt_digest) {
        return core::Result<ClothRestPoseBakeResult>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "stored cloth rest-pose receipt differs from deterministic reconstruction"));
    }
    return rebuilt;
}

core::Result<ClothSimulationArtifact> compile_cloth_cpu(
    const GarmentAsset& garment,
    const geometry::EditableMesh& base_mesh,
    const ClothSolverSettings& settings) {
    const auto garment_digest = garment.digest();
    if (!garment_digest) return core::Result<ClothSimulationArtifact>::failure(
        garment_digest.error());
    const auto simulation = simulate(garment, base_mesh, settings);
    if (!simulation) return core::Result<ClothSimulationArtifact>::failure(simulation.error());
    ClothSimulationArtifact artifact;
    artifact.source = garment.source;
    artifact.garment_digest = garment_digest.value();
    artifact.settings = settings;
    artifact.frames = simulation.value().frames;
    artifact.completed_substeps = simulation.value().completed_substeps;
    artifact.maximum_constraint_error = simulation.value().maximum_constraint_error;
    artifact.maximum_collision_correction = simulation.value().maximum_collision_correction;
    artifact.settled_frame = simulation.value().settled_frame;
    if (auto result = artifact.refresh_digests(); !result) {
        return core::Result<ClothSimulationArtifact>::failure(result.error());
    }
    if (auto result = artifact.validate(garment, base_mesh); !result) {
        return core::Result<ClothSimulationArtifact>::failure(result.error());
    }
    return core::Result<ClothSimulationArtifact>::success(std::move(artifact));
}

core::Result<ProductionCookKey> cloth_production_cook_key(
    const GarmentAsset& garment,
    const ClothSolverSettings& settings,
    std::string platform_profile) {
    if (auto result = settings.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto garment_digest = garment.digest();
    if (!garment_digest) {
        return core::Result<ProductionCookKey>::failure(garment_digest.error());
    }
    const auto settings_digest = settings.digest();
    if (!settings_digest) {
        return core::Result<ProductionCookKey>::failure(settings_digest.error());
    }
    const BakeMeshBinding& binding = garment.source;
    if (auto result = binding.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    ProductionCookKey key;
    key.source = AuthoritativeSource{
        binding.identity, binding.revision, binding.content_digest};
    key.kind = ArtifactKind::cloth_simulation;
    key.form = ExecutionForm::offline;
    key.algorithm = std::string(kAlgorithm);
    key.settings_digest = hash_text(
        "CARTO_CLOTH_COOK_SETTINGS 1\nGARMENT " + garment_digest.value().hex() +
        "\nSOLVER " + settings_digest.value().hex() + "\n");
    key.platform_profile = std::move(platform_profile);
    key.representation_kind = "cloth.animation.frame-sequence";
    key.spatial_scope = "whole-garment";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<void> publish_cloth_simulation(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const ClothSimulationArtifact& artifact,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_mesh,
    const AdmissionPolicy& policy) {
    if (auto result = artifact.validate(garment, current_mesh); !result) return result;
    const auto expected_key = cloth_production_cook_key(
        garment, artifact.settings, receipt.key.platform_profile);
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "cloth production receipt key differs from the artifact source, garment, or settings"));
    }
    if (receipt.quality.measured_error != artifact.maximum_constraint_error) {
        return core::Result<void>::failure(validation(
            "cloth production receipt error metric differs from the verified artifact"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const AuthoritativeSource current_source{
        artifact.source.identity, artifact.source.revision, artifact.source.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_source, policy);
}

core::Result<ClothSimulationArtifact> read_cloth_simulation(
    const ProductionArtifactStore& store,
    const GarmentAsset& garment,
    const geometry::EditableMesh& current_mesh,
    const ClothSolverSettings& settings,
    std::string platform_profile,
    const AdmissionPolicy& policy) {
    if (auto result = garment.validate(current_mesh); !result) {
        return core::Result<ClothSimulationArtifact>::failure(result.error());
    }
    const auto key = cloth_production_cook_key(garment, settings, std::move(platform_profile));
    if (!key) return core::Result<ClothSimulationArtifact>::failure(key.error());
    const AuthoritativeSource current_source{
        garment.source.identity, garment.source.revision, garment.source.content_digest};
    const auto payload = store.read(key.value(), current_source, policy);
    if (!payload) return core::Result<ClothSimulationArtifact>::failure(payload.error());
    const auto artifact = ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{payload.value()});
    if (!artifact) return core::Result<ClothSimulationArtifact>::failure(artifact.error());
    if (artifact.value().settings.canonical() != settings.canonical()) {
        return core::Result<ClothSimulationArtifact>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "stored cloth artifact solver settings differ from the requested cook"));
    }
    if (auto result = artifact.value().validate(garment, current_mesh); !result) {
        return core::Result<ClothSimulationArtifact>::failure(result.error());
    }
    return artifact;
}

} // namespace carto::production
