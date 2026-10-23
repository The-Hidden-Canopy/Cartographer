#include <carto/production/bake_graph.hpp>

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
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::production {
namespace {

constexpr std::string_view kCurvatureRecipeMagic = "CARTO_CURVATURE_RECIPE 1";
constexpr std::string_view kCurvatureArtifactMagic = "CARTO_BAKE_CURVATURE_ARTIFACT";
constexpr std::uint32_t kCurvatureArtifactVersion = 1U;
constexpr std::size_t kMaxCurvatureRecipeBytes = 32U * 1024U;
constexpr std::size_t kMaxCurvatureArtifactBytes =
    static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL + 128ULL * 1024ULL);
constexpr std::size_t kMaxCurvatureMissSamples = 16U;
constexpr double kCurvatureEpsilon = 1e-12;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest hash_bytes(std::span<const std::uint8_t> bytes) {
    return assets::sha256(bytes);
}

assets::Sha256Digest hash_text(std::string_view text) {
    return hash_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_double(std::vector<std::uint8_t>& bytes, double value) {
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

void append_string(std::vector<std::uint8_t>& bytes, std::string_view value) {
    append_u64(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_digest(std::vector<std::uint8_t>& bytes, const assets::Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
}

std::string hex_encode(std::string_view value) {
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

std::optional<std::string> hex_decode(std::string_view text, std::size_t maximum_bytes) {
    if (text.size() % 2U != 0U || text.size() / 2U > maximum_bytes) return std::nullopt;
    const auto nibble = [](char value) -> std::optional<std::uint8_t> {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(10 + value - 'a');
        }
        return std::nullopt;
    };
    std::string result;
    result.reserve(text.size() / 2U);
    for (std::size_t index = 0U; index < text.size(); index += 2U) {
        const auto high = nibble(text[index]);
        const auto low = nibble(text[index + 1U]);
        if (!high || !low) return std::nullopt;
        result.push_back(static_cast<char>((*high << 4U) | *low));
    }
    return result;
}

std::string double_bits_hex(double value) {
    constexpr char digits[] = "0123456789abcdef";
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    std::string result(16U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const std::size_t shift = (result.size() - index - 1U) * 4U;
        result[index] = digits[(bits >> shift) & 0x0fU];
    }
    return result;
}

std::optional<double> parse_double_bits(std::string_view text) {
    if (text.size() != 16U) return std::nullopt;
    std::uint64_t bits = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), bits, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return std::bit_cast<double>(bits);
}

struct ByteReader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0U;

    bool read_u32(std::uint32_t& value) {
        if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
        value = 0U;
        for (unsigned index = 0U; index < 4U; ++index) {
            value |= static_cast<std::uint32_t>(bytes[offset++]) << (index * 8U);
        }
        return true;
    }

    bool read_u64(std::uint64_t& value) {
        if (offset > bytes.size() || bytes.size() - offset < 8U) return false;
        value = 0U;
        for (unsigned index = 0U; index < 8U; ++index) {
            value |= static_cast<std::uint64_t>(bytes[offset++]) << (index * 8U);
        }
        return true;
    }

    bool read_string(std::string& value, std::size_t maximum) {
        std::uint64_t length = 0U;
        if (!read_u64(length) || length > maximum ||
            length > static_cast<std::uint64_t>(bytes.size() - offset)) return false;
        const auto count = static_cast<std::size_t>(length);
        value.assign(reinterpret_cast<const char*>(bytes.data() + offset), count);
        offset += count;
        return true;
    }

    bool read_digest(assets::Sha256Digest& digest) {
        if (offset > bytes.size() || bytes.size() - offset < digest.bytes.size()) return false;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            digest.bytes.size(), digest.bytes.begin());
        offset += digest.bytes.size();
        return true;
    }

    bool read_payload(std::vector<std::uint8_t>& payload, std::size_t maximum) {
        std::uint64_t length = 0U;
        if (!read_u64(length) || length > maximum ||
            length > static_cast<std::uint64_t>(bytes.size() - offset)) return false;
        const auto count = static_cast<std::size_t>(length);
        payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
        return true;
    }
};

std::vector<std::uint8_t> canonical_artifact_bytes(const CurvatureMapArtifact& artifact) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kCurvatureArtifactMagic);
    append_u32(bytes, kCurvatureArtifactVersion);
    append_string(bytes, artifact.recipe.serialize());
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, artifact.projected_texels);
    append_u64(bytes, artifact.projection_misses);
    append_u32(bytes, static_cast<std::uint32_t>(artifact.projection_miss_samples.size()));
    for (const BakeProjectionMissSample& sample : artifact.projection_miss_samples) {
        append_u64(bytes, sample.target_face.value);
        append_u32(bytes, sample.texel_x);
        append_u32(bytes, sample.texel_y);
    }
    append_u32(bytes, artifact.projection_miss_samples_truncated ? 1U : 0U);
    append_u64(bytes, artifact.clamped_texels);
    append_double(bytes, artifact.minimum_signed_curvature);
    append_double(bytes, artifact.maximum_signed_curvature);
    append_double(bytes, artifact.maximum_quantization_error);
    append_u64(bytes, static_cast<std::uint64_t>(artifact.rgba8.size()));
    bytes.insert(bytes.end(), artifact.rgba8.begin(), artifact.rgba8.end());
    return bytes;
}

void add_error(
    CurvatureExecutionReport& report,
    std::string code,
    std::string message,
    std::optional<geometry::FaceId> face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    if (report.diagnostics.size() < 64U) {
        report.diagnostics.push_back(BakeDiagnostic{
            std::move(code), std::move(message), true, face, std::nullopt, x, y});
    } else if (report.diagnostics.back().code != "diagnostics_truncated") {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated",
            "additional curvature diagnostics were omitted after the bounded report limit",
            true, face, std::nullopt, x, y};
    }
}

void add_miss_diagnostic(
    CurvatureExecutionReport& report,
    bool is_error,
    geometry::FaceId face,
    std::uint32_t x,
    std::uint32_t y) {
    if (report.diagnostics.size() < 64U) {
        report.diagnostics.push_back(BakeDiagnostic{
            "projection_miss",
            "bounded high-resolution source projection missed a covered target texel",
            is_error, face, std::nullopt, x, y});
    } else if (report.diagnostics.back().code != "diagnostics_truncated") {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated",
            "additional curvature diagnostics were omitted after the bounded report limit",
            true, face, std::nullopt, x, y};
    }
}

using VertexPair = std::pair<std::uint64_t, std::uint64_t>;

VertexPair ordered_edge(geometry::VertexId first, geometry::VertexId second) {
    return first.value < second.value
        ? VertexPair{first.value, second.value} : VertexPair{second.value, first.value};
}

struct CurvatureTriangle {
    std::array<geometry::VertexId, 3U> vertices{};
    std::array<core::Vec3d, 3U> positions{};
    std::array<core::Vec3d, 3U> normals{};
    std::array<core::Vec2d, 3U> uvs{};
    std::array<double, 3U> curvature{};
    geometry::FaceId face;
    std::uint32_t fan_index = 0U;
    core::Vec3d bounds_min{};
    core::Vec3d bounds_max{};
};

core::Vec3d component_min(core::Vec3d left, core::Vec3d right) {
    return {std::min(left.x, right.x), std::min(left.y, right.y), std::min(left.z, right.z)};
}

core::Vec3d component_max(core::Vec3d left, core::Vec3d right) {
    return {std::max(left.x, right.x), std::max(left.y, right.y), std::max(left.z, right.z)};
}

struct CurvatureAccumulation {
    double one_third_area = 0.0;
    core::Vec3d normal{};
    core::Vec3d laplacian{};
};

core::Result<std::vector<CurvatureTriangle>> collect_source_curvature(
    const geometry::EditableMesh& mesh) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<std::vector<CurvatureTriangle>>::failure(result.error());
    }
    const auto compiled = mesh.compile();
    if (!compiled) return core::Result<std::vector<CurvatureTriangle>>::failure(compiled.error());
    std::map<std::uint64_t, core::Vec3d> positions;
    for (const geometry::Vertex& vertex : mesh.vertices_sorted()) {
        positions.emplace(vertex.id.value, vertex.position);
    }
    std::map<std::uint64_t, core::Vec3d> normals;
    for (std::size_t index = 0U; index < compiled.value().vertex_ids.size(); ++index) {
        normals.emplace(compiled.value().vertex_ids[index].value, compiled.value().normals[index]);
    }

    std::map<std::uint64_t, CurvatureAccumulation> accumulated;
    std::map<VertexPair, double> cotangent_weights;
    std::vector<CurvatureTriangle> triangles;
    for (const geometry::Face& face : mesh.faces_sorted()) {
        if (face.vertices.size() < 3U) {
            return core::Result<std::vector<CurvatureTriangle>>::failure(
                validation("curvature source contains a face with fewer than three vertices"));
        }
        for (std::size_t fan = 1U; fan + 1U < face.vertices.size(); ++fan) {
            if (triangles.size() >= kMaxBakeSourceTriangles) {
                return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                    "curvature source exceeds the CPU reference triangle limit"));
            }
            CurvatureTriangle triangle;
            triangle.face = face.id;
            triangle.fan_index = static_cast<std::uint32_t>(fan - 1U);
            triangle.vertices = {face.vertices[0U], face.vertices[fan], face.vertices[fan + 1U]};
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const auto position = positions.find(triangle.vertices[corner].value);
                const auto normal = normals.find(triangle.vertices[corner].value);
                if (position == positions.end() || normal == normals.end() ||
                    !normal->second.finite() || normal->second.length() <= 0.0) {
                    return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                        "curvature source triangle references invalid geometry or normals"));
                }
                triangle.positions[corner] = position->second;
                triangle.normals[corner] = normal->second;
            }
            const core::Vec3d edge_one = triangle.positions[1U] - triangle.positions[0U];
            const core::Vec3d edge_two = triangle.positions[2U] - triangle.positions[0U];
            const core::Vec3d cross = core::cross(edge_one, edge_two);
            const double twice_area = cross.length();
            if (!std::isfinite(twice_area) || twice_area <= kCurvatureEpsilon) {
                return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                    "curvature source triangulation contains a degenerate triangle"));
            }
            const double area = twice_area * 0.5;
            for (const geometry::VertexId vertex : triangle.vertices) {
                accumulated[vertex.value].one_third_area += area / 3.0;
            }
            const double cotangent_zero = core::dot(
                triangle.positions[1U] - triangle.positions[0U],
                triangle.positions[2U] - triangle.positions[0U]) / twice_area;
            const double cotangent_one = core::dot(
                triangle.positions[0U] - triangle.positions[1U],
                triangle.positions[2U] - triangle.positions[1U]) / twice_area;
            const double cotangent_two = core::dot(
                triangle.positions[0U] - triangle.positions[2U],
                triangle.positions[1U] - triangle.positions[2U]) / twice_area;
            if (!std::isfinite(cotangent_zero) || !std::isfinite(cotangent_one) ||
                !std::isfinite(cotangent_two)) {
                return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                    "curvature source cotangent weight is non-finite"));
            }
            cotangent_weights[ordered_edge(triangle.vertices[1U], triangle.vertices[2U])] +=
                cotangent_zero;
            cotangent_weights[ordered_edge(triangle.vertices[0U], triangle.vertices[2U])] +=
                cotangent_one;
            cotangent_weights[ordered_edge(triangle.vertices[0U], triangle.vertices[1U])] +=
                cotangent_two;
            triangle.bounds_min = component_min(triangle.positions[0U],
                component_min(triangle.positions[1U], triangle.positions[2U]));
            triangle.bounds_max = component_max(triangle.positions[0U],
                component_max(triangle.positions[1U], triangle.positions[2U]));
            triangles.push_back(std::move(triangle));
        }
    }
    if (triangles.empty()) {
        return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
            "curvature source contains no triangles"));
    }

    for (const auto& [edge, weight] : cotangent_weights) {
        const auto first_position = positions.find(edge.first);
        const auto second_position = positions.find(edge.second);
        if (first_position == positions.end() || second_position == positions.end()) {
            return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                "curvature edge references a missing source vertex"));
        }
        const core::Vec3d delta = second_position->second - first_position->second;
        accumulated[edge.first].laplacian = accumulated[edge.first].laplacian + delta * weight;
        accumulated[edge.second].laplacian = accumulated[edge.second].laplacian - delta * weight;
    }

    std::map<std::uint64_t, double> signed_curvatures;
    for (auto& [vertex, accumulation] : accumulated) {
        const auto normal = normals.find(vertex);
        if (normal == normals.end() || accumulation.one_third_area <= kCurvatureEpsilon) {
            return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                "curvature source vertex has no valid area or normal"));
        }
        accumulation.normal = normal->second.normalized();
        // Cotangent Laplacian over the barycentric dual area. The minus sign makes
        // outward-oriented convex regions positive; the factor yields mean curvature.
        const double signed_mean_curvature = -0.25 * core::dot(
            accumulation.laplacian, accumulation.normal) / accumulation.one_third_area;
        if (!std::isfinite(signed_mean_curvature)) {
            return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                "curvature source produced a non-finite signed mean curvature"));
        }
        signed_curvatures.emplace(vertex, signed_mean_curvature);
    }
    for (CurvatureTriangle& triangle : triangles) {
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            triangle.curvature[corner] = signed_curvatures.at(triangle.vertices[corner].value);
        }
    }
    return core::Result<std::vector<CurvatureTriangle>>::success(std::move(triangles));
}

double edge_value(core::Vec2d first, core::Vec2d second, core::Vec2d point) {
    return (second.x - first.x) * (point.y - first.y) -
        (second.y - first.y) * (point.x - first.x);
}

bool top_left_edge(core::Vec2d first, core::Vec2d second) {
    const double dy = second.y - first.y;
    const double dx = second.x - first.x;
    return dy > 0.0 || (dy == 0.0 && dx < 0.0);
}

std::uint32_t pixel_floor(double value, std::uint32_t extent) {
    const double floored = std::floor(value);
    if (floored <= 0.0) return 0U;
    const double last = static_cast<double>(extent - 1U);
    if (floored >= last) return extent - 1U;
    return static_cast<std::uint32_t>(floored);
}

core::Result<std::vector<CurvatureTriangle>> collect_target_triangles(
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<std::vector<CurvatureTriangle>>::failure(result.error());
    }
    const auto compiled = mesh.compile();
    const auto topology = mesh.topology();
    if (!compiled || !topology) {
        return core::Result<std::vector<CurvatureTriangle>>::failure(
            compiled ? topology.error() : compiled.error());
    }
    const attributes::UvSetDescriptor* descriptor = mesh.uv_sets().find(uv_set);
    const attributes::AttributeLayer* layer = descriptor == nullptr
        ? nullptr : mesh.attributes().find(descriptor->layer_id);
    if (descriptor == nullptr || layer == nullptr ||
        layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2 ||
        layer->values.size() != topology.value().corners.size()) {
        return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
            "curvature target requires a complete corner-domain vec2 UV set"));
    }

    std::map<std::uint64_t, core::Vec3d> positions;
    for (const geometry::Vertex& vertex : mesh.vertices_sorted()) {
        positions.emplace(vertex.id.value, vertex.position);
    }
    std::map<std::uint64_t, core::Vec3d> normals;
    for (std::size_t index = 0U; index < compiled.value().vertex_ids.size(); ++index) {
        normals.emplace(compiled.value().vertex_ids[index].value, compiled.value().normals[index]);
    }
    std::map<std::pair<std::uint64_t, std::uint64_t>, core::Vec2d> uv_by_face_vertex;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto* uv = std::get_if<core::Vec2d>(&layer->values[index]);
        const geometry::CornerRecord& corner = topology.value().corners[index];
        if (uv == nullptr || !uv->finite()) {
            return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                "curvature target UV set contains a non-finite coordinate"));
        }
        uv_by_face_vertex.emplace(
            std::make_pair(corner.face.value, corner.vertex.value), *uv);
    }

    std::vector<CurvatureTriangle> triangles;
    for (const geometry::Face& face : mesh.faces_sorted()) {
        if (face.vertices.size() < 3U) {
            return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                "curvature target contains a face with fewer than three vertices"));
        }
        for (std::size_t fan = 1U; fan + 1U < face.vertices.size(); ++fan) {
            if (triangles.size() >= kMaxBakeSourceTriangles) {
                return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                    "curvature target exceeds the CPU reference triangle limit"));
            }
            CurvatureTriangle triangle;
            triangle.face = face.id;
            triangle.fan_index = static_cast<std::uint32_t>(fan - 1U);
            triangle.vertices = {face.vertices[0U], face.vertices[fan], face.vertices[fan + 1U]};
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const std::uint64_t vertex_id = triangle.vertices[corner].value;
                const auto position = positions.find(vertex_id);
                const auto normal = normals.find(vertex_id);
                const auto uv = uv_by_face_vertex.find({face.id.value, vertex_id});
                if (position == positions.end() || normal == normals.end() ||
                    uv == uv_by_face_vertex.end() || !normal->second.finite() ||
                    normal->second.length() <= 0.0) {
                    return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                        "curvature target triangle references missing position, normal, or UV data"));
                }
                triangle.positions[corner] = position->second;
                triangle.normals[corner] = normal->second;
                triangle.uvs[corner] = uv->second;
            }
            const core::Vec3d cross = core::cross(
                triangle.positions[1U] - triangle.positions[0U],
                triangle.positions[2U] - triangle.positions[0U]);
            if (!cross.finite() || cross.length() <= kCurvatureEpsilon) {
                return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
                    "curvature target triangulation contains a degenerate triangle"));
            }
            triangle.bounds_min = component_min(triangle.positions[0U],
                component_min(triangle.positions[1U], triangle.positions[2U]));
            triangle.bounds_max = component_max(triangle.positions[0U],
                component_max(triangle.positions[1U], triangle.positions[2U]));
            triangles.push_back(std::move(triangle));
        }
    }
    if (triangles.empty()) {
        return core::Result<std::vector<CurvatureTriangle>>::failure(validation(
            "curvature target contains no triangles"));
    }
    return core::Result<std::vector<CurvatureTriangle>>::success(std::move(triangles));
}

struct BvhNode {
    core::Vec3d bounds_min{};
    core::Vec3d bounds_max{};
    std::size_t begin = 0U;
    std::size_t end = 0U;
    std::size_t left = std::numeric_limits<std::size_t>::max();
    std::size_t right = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] bool leaf() const noexcept {
        return left == std::numeric_limits<std::size_t>::max();
    }
};

struct CurvatureHit {
    double distance = std::numeric_limits<double>::infinity();
    std::array<double, 3U> barycentric{};
    std::size_t triangle_index = 0U;
};

class CurvatureBvh final {
public:
    explicit CurvatureBvh(const std::vector<CurvatureTriangle>& triangles)
        : triangles_(triangles) {
        order_.resize(triangles_.size());
        for (std::size_t index = 0U; index < order_.size(); ++index) order_[index] = index;
        nodes_.reserve(triangles_.size() * 2U);
        if (!order_.empty()) static_cast<void>(build(0U, order_.size()));
    }

    [[nodiscard]] std::optional<CurvatureHit> cast(
        core::Vec3d origin,
        core::Vec3d direction,
        double maximum_distance,
        BakeBackfacePolicy backface_policy) const {
        if (nodes_.empty()) return std::nullopt;
        std::array<std::size_t, 128U> pending{};
        std::size_t pending_size = 1U;
        pending[0U] = 0U;
        std::optional<CurvatureHit> best;
        while (pending_size > 0U) {
            const BvhNode& node = nodes_[pending[--pending_size]];
            const double maximum = best
                ? std::min(maximum_distance, best->distance + kCurvatureEpsilon)
                : maximum_distance;
            if (!intersects_bounds(origin, direction, node, maximum)) continue;
            if (!node.leaf()) {
                // Median splits keep traversal depth below this fixed stack bound.
                if (pending_size + 2U > pending.size()) return std::nullopt;
                pending[pending_size++] = node.right;
                pending[pending_size++] = node.left;
                continue;
            }
            for (std::size_t item = node.begin; item < node.end; ++item) {
                const std::size_t index = order_[item];
                const auto hit = intersect_triangle(
                    origin, direction, triangles_[index], maximum_distance, backface_policy);
                if (!hit) continue;
                if (!best || hit->distance < best->distance - kCurvatureEpsilon ||
                    (std::abs(hit->distance - best->distance) <= kCurvatureEpsilon &&
                        triangle_precedes(index, best->triangle_index))) {
                    best = CurvatureHit{hit->distance, hit->barycentric, index};
                }
            }
        }
        return best;
    }

private:
    struct Intersection {
        double distance = 0.0;
        std::array<double, 3U> barycentric{};
    };

    [[nodiscard]] bool triangle_precedes(std::size_t first, std::size_t second) const {
        const CurvatureTriangle& left = triangles_[first];
        const CurvatureTriangle& right = triangles_[second];
        return left.face < right.face ||
            (left.face == right.face && left.fan_index < right.fan_index);
    }

    [[nodiscard]] std::size_t build(std::size_t begin, std::size_t end) {
        BvhNode node;
        node.begin = begin;
        node.end = end;
        node.bounds_min = triangles_[order_[begin]].bounds_min;
        node.bounds_max = triangles_[order_[begin]].bounds_max;
        for (std::size_t index = begin + 1U; index < end; ++index) {
            node.bounds_min = component_min(node.bounds_min, triangles_[order_[index]].bounds_min);
            node.bounds_max = component_max(node.bounds_max, triangles_[order_[index]].bounds_max);
        }
        const std::size_t node_index = nodes_.size();
        nodes_.push_back(node);
        if (end - begin <= 8U) return node_index;

        const core::Vec3d extent = node.bounds_max - node.bounds_min;
        std::size_t axis = 0U;
        if (extent.y > extent.x && extent.y >= extent.z) axis = 1U;
        else if (extent.z > extent.x && extent.z > extent.y) axis = 2U;
        const auto centroid = [this, axis](std::size_t index) {
            const CurvatureTriangle& triangle = triangles_[index];
            const double first = axis == 0U ? triangle.bounds_min.x
                : axis == 1U ? triangle.bounds_min.y : triangle.bounds_min.z;
            const double last = axis == 0U ? triangle.bounds_max.x
                : axis == 1U ? triangle.bounds_max.y : triangle.bounds_max.z;
            return first + (last - first) * 0.5;
        };
        std::stable_sort(order_.begin() + static_cast<std::ptrdiff_t>(begin),
            order_.begin() + static_cast<std::ptrdiff_t>(end),
            [this, &centroid](std::size_t first, std::size_t second) {
                const double first_center = centroid(first);
                const double second_center = centroid(second);
                if (first_center != second_center) return first_center < second_center;
                return triangle_precedes(first, second);
            });
        const std::size_t middle = begin + (end - begin) / 2U;
        nodes_[node_index].left = build(begin, middle);
        nodes_[node_index].right = build(middle, end);
        return node_index;
    }

    [[nodiscard]] static bool intersects_bounds(
        core::Vec3d origin,
        core::Vec3d direction,
        const BvhNode& node,
        double maximum_distance) {
        double minimum = 0.0;
        double maximum = maximum_distance;
        const std::array<double, 3U> origins{origin.x, origin.y, origin.z};
        const std::array<double, 3U> directions{direction.x, direction.y, direction.z};
        const std::array<double, 3U> lower{node.bounds_min.x, node.bounds_min.y,
            node.bounds_min.z};
        const std::array<double, 3U> upper{node.bounds_max.x, node.bounds_max.y,
            node.bounds_max.z};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (std::abs(directions[axis]) <= std::numeric_limits<double>::min()) {
                if (origins[axis] < lower[axis] || origins[axis] > upper[axis]) return false;
                continue;
            }
            const double reciprocal = 1.0 / directions[axis];
            double near_value = (lower[axis] - origins[axis]) * reciprocal;
            double far_value = (upper[axis] - origins[axis]) * reciprocal;
            if (near_value > far_value) std::swap(near_value, far_value);
            minimum = std::max(minimum, near_value);
            maximum = std::min(maximum, far_value);
            if (maximum < minimum) return false;
        }
        return true;
    }

    [[nodiscard]] static std::optional<Intersection> intersect_triangle(
        core::Vec3d origin,
        core::Vec3d direction,
        const CurvatureTriangle& triangle,
        double maximum_distance,
        BakeBackfacePolicy backface_policy) {
        const core::Vec3d edge_one = triangle.positions[1U] - triangle.positions[0U];
        const core::Vec3d edge_two = triangle.positions[2U] - triangle.positions[0U];
        const core::Vec3d p = core::cross(direction, edge_two);
        const double determinant = core::dot(edge_one, p);
        if (backface_policy == BakeBackfacePolicy::front_only) {
            if (determinant <= kCurvatureEpsilon) return std::nullopt;
        } else if (std::abs(determinant) <= kCurvatureEpsilon) {
            return std::nullopt;
        }
        const double inverse = 1.0 / determinant;
        const core::Vec3d from_first = origin - triangle.positions[0U];
        const double u = core::dot(from_first, p) * inverse;
        if (u < -kCurvatureEpsilon || u > 1.0 + kCurvatureEpsilon) return std::nullopt;
        const core::Vec3d q = core::cross(from_first, edge_one);
        const double v = core::dot(direction, q) * inverse;
        if (v < -kCurvatureEpsilon || u + v > 1.0 + kCurvatureEpsilon) {
            return std::nullopt;
        }
        const double distance = core::dot(edge_two, q) * inverse;
        if (distance < 0.0 || distance > maximum_distance) return std::nullopt;
        return Intersection{distance, {1.0 - u - v, u, v}};
    }

    const std::vector<CurvatureTriangle>& triangles_;
    std::vector<std::size_t> order_;
    std::vector<BvhNode> nodes_;
};

} // namespace



core::Result<void> CurvatureRecipe::validate() const {
    if (auto result = binding.validate(); !result) return result;
    if (!std::isfinite(normalization_scale) || normalization_scale <= 0.0 ||
        normalization_scale > 1'000'000.0) {
        return core::Result<void>::failure(invalid(
            "curvature normalization scale must be finite and positive"));
    }
    return core::Result<void>::success();
}

std::string CurvatureRecipe::serialize() const {
    if (!validate()) return {};
    const std::string text = std::string(kCurvatureRecipeMagic) + "\n" +
        "binding.hex=" + hex_encode(binding.serialize()) + "\n" +
        "normalization.scale.bits=" + double_bits_hex(normalization_scale) + "\n";
    return text.size() <= kMaxCurvatureRecipeBytes ? text : std::string{};
}

core::Result<assets::Sha256Digest> CurvatureRecipe::canonical_digest() const {
    const std::string text = serialize();
    if (text.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid or oversized curvature recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(text));
}

core::Result<CurvatureRecipe> CurvatureRecipe::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxCurvatureRecipeBytes) {
        return core::Result<CurvatureRecipe>::failure(invalid(
            "curvature recipe byte size is invalid"));
    }
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || line != kCurvatureRecipeMagic) {
        return core::Result<CurvatureRecipe>::failure(validation(
            "curvature recipe header is unsupported"));
    }
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<CurvatureRecipe>::failure(validation(
                "curvature recipe contains a malformed or duplicate field"));
        }
    }
    const auto binding_hex = fields.find("binding.hex");
    const auto scale_hex = fields.find("normalization.scale.bits");
    if (fields.size() != 2U || binding_hex == fields.end() || scale_hex == fields.end()) {
        return core::Result<CurvatureRecipe>::failure(validation(
            "curvature recipe does not contain the exact supported field set"));
    }
    const auto binding_text = hex_decode(binding_hex->second, kMaxBakeRecipeBytes);
    const auto scale = parse_double_bits(scale_hex->second);
    if (!binding_text || !scale) {
        return core::Result<CurvatureRecipe>::failure(validation(
            "curvature recipe binding or normalization scale is malformed"));
    }
    const auto binding = BakeRecipe::deserialize(*binding_text);
    if (!binding) return core::Result<CurvatureRecipe>::failure(binding.error());
    CurvatureRecipe recipe{binding.value(), *scale};
    if (auto result = recipe.validate(); !result) {
        return core::Result<CurvatureRecipe>::failure(result.error());
    }
    if (recipe.serialize() != text) {
        return core::Result<CurvatureRecipe>::failure(validation(
            "curvature recipe serialization is not canonical"));
    }
    return core::Result<CurvatureRecipe>::success(std::move(recipe));
}

core::Result<void> CurvatureMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != kBakeCurvatureAlgorithm) {
        return core::Result<void>::failure(invalid("curvature artifact algorithm is unsupported"));
    }
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    if (rgba8.size() != texels * 4ULL || covered_texels == 0U ||
        covered_texels > texels || projected_texels + projection_misses != covered_texels ||
        projected_texels == 0U || projection_miss_samples.size() > kMaxCurvatureMissSamples ||
        projection_miss_samples.size() > projection_misses || clamped_texels > projected_texels) {
        return core::Result<void>::failure(validation(
            "curvature artifact dimensions or coverage accounting are inconsistent"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return core::Result<void>::failure(recipe_digest.error());
    settings_digest = hash_text(std::string(kBakeCurvatureAlgorithm) + "\n" +
        recipe_digest.value().hex());
    maximum_quantization_error = recipe.normalization_scale / 255.0;
    output_digest = hash_bytes(rgba8);
    artifact_digest = hash_bytes(canonical_artifact_bytes(*this));
    return core::Result<void>::success();
}

core::Result<void> CurvatureMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    if (algorithm != kBakeCurvatureAlgorithm || settings_digest.is_zero() ||
        output_digest.is_zero() || artifact_digest.is_zero() ||
        rgba8.size() != texels * 4ULL || covered_texels == 0U || covered_texels > texels ||
        projected_texels == 0U || projected_texels + projection_misses != covered_texels ||
        clamped_texels > projected_texels ||
        projection_miss_samples.size() > kMaxCurvatureMissSamples ||
        projection_miss_samples.size() > projection_misses ||
        !std::isfinite(minimum_signed_curvature) ||
        !std::isfinite(maximum_signed_curvature) ||
        minimum_signed_curvature > maximum_signed_curvature ||
        !std::isfinite(maximum_quantization_error) ||
        std::abs(maximum_quantization_error - recipe.normalization_scale / 255.0) > 1e-15) {
        return core::Result<void>::failure(validation(
            "curvature artifact metadata or coverage accounting is invalid"));
    }
    if (projection_misses == 0U) {
        if (!projection_miss_samples.empty() || projection_miss_samples_truncated) {
            return core::Result<void>::failure(validation(
                "curvature artifact records misses without missed texels"));
        }
    } else if (recipe.binding.reject_projection_misses || projection_miss_samples.empty() ||
        projection_miss_samples_truncated !=
            (projection_miss_samples.size() < projection_misses)) {
        return core::Result<void>::failure(validation(
            "curvature artifact miss evidence conflicts with its recipe"));
    }
    std::set<std::array<std::uint64_t, 3U>> unique_misses;
    for (const BakeProjectionMissSample& sample : projection_miss_samples) {
        if (!sample.target_face || sample.texel_x >= recipe.binding.width ||
            sample.texel_y >= recipe.binding.height ||
            !unique_misses.insert({sample.target_face.value, sample.texel_x,
                sample.texel_y}).second) {
            return core::Result<void>::failure(validation(
                "curvature artifact contains invalid or duplicate projection-miss evidence"));
        }
    }
    std::uint64_t opaque_texels = 0U;
    for (std::size_t pixel = 0U; pixel < rgba8.size(); pixel += 4U) {
        const std::uint8_t alpha = rgba8[pixel + 3U];
        if (alpha == 255U) {
            ++opaque_texels;
            if (rgba8[pixel] != rgba8[pixel + 1U] || rgba8[pixel] != rgba8[pixel + 2U]) {
                return core::Result<void>::failure(validation(
                    "curvature output must be grayscale"));
            }
        } else if (alpha != 0U || rgba8[pixel] != 128U ||
            rgba8[pixel + 1U] != 128U || rgba8[pixel + 2U] != 128U) {
            return core::Result<void>::failure(validation(
                "curvature uncovered texel encoding is invalid"));
        }
    }
    if (opaque_texels != projected_texels) {
        return core::Result<void>::failure(validation(
            "curvature alpha coverage disagrees with its receipt"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest || hash_text(std::string(kBakeCurvatureAlgorithm) + "\n" +
            recipe_digest.value().hex()) != settings_digest ||
        hash_bytes(rgba8) != output_digest ||
        hash_bytes(canonical_artifact_bytes(*this)) != artifact_digest) {
        return core::Result<void>::failure(validation(
            "curvature artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> CurvatureMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_artifact_bytes(*this);
    append_digest(bytes, artifact_digest);
    if (bytes.size() > kMaxCurvatureArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "curvature artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<CurvatureMapArtifact> CurvatureMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxCurvatureArtifactBytes) {
        return core::Result<CurvatureMapArtifact>::failure(invalid(
            "curvature artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string recipe_text;
    std::string algorithm_value;
    std::uint32_t miss_count = 0U;
    std::uint32_t truncated = 0U;
    std::uint64_t minimum_bits = 0U;
    std::uint64_t maximum_bits = 0U;
    std::uint64_t quantization_bits = 0U;
    if (!reader.read_string(magic, kCurvatureArtifactMagic.size()) ||
        magic != kCurvatureArtifactMagic || !reader.read_u32(version) ||
        version != kCurvatureArtifactVersion ||
        !reader.read_string(recipe_text, kMaxCurvatureRecipeBytes) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<CurvatureMapArtifact>::failure(validation(
            "curvature artifact header or recipe is malformed"));
    }
    const auto recipe = CurvatureRecipe::deserialize(recipe_text);
    if (!recipe) return core::Result<CurvatureMapArtifact>::failure(recipe.error());
    CurvatureMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.algorithm = std::move(algorithm_value);
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_u64(artifact.projected_texels) ||
        !reader.read_u64(artifact.projection_misses) ||
        !reader.read_u32(miss_count) || miss_count > kMaxCurvatureMissSamples) {
        return core::Result<CurvatureMapArtifact>::failure(validation(
            "curvature artifact metrics or miss count are malformed"));
    }
    for (std::uint32_t index = 0U; index < miss_count; ++index) {
        std::uint64_t face_id = 0U;
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        if (!reader.read_u64(face_id) || !reader.read_u32(x) || !reader.read_u32(y)) {
            return core::Result<CurvatureMapArtifact>::failure(validation(
                "curvature artifact projection-miss sample is truncated"));
        }
        artifact.projection_miss_samples.push_back({geometry::FaceId{face_id}, x, y});
    }
    if (!reader.read_u32(truncated) || truncated > 1U ||
        !reader.read_u64(artifact.clamped_texels) ||
        !reader.read_u64(minimum_bits) || !reader.read_u64(maximum_bits) ||
        !reader.read_u64(quantization_bits) ||
        !reader.read_payload(artifact.rgba8,
            static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<CurvatureMapArtifact>::failure(validation(
            "curvature artifact payload is truncated, oversized, or has trailing bytes"));
    }
    artifact.projection_miss_samples_truncated = truncated != 0U;
    artifact.minimum_signed_curvature = std::bit_cast<double>(minimum_bits);
    artifact.maximum_signed_curvature = std::bit_cast<double>(maximum_bits);
    artifact.maximum_quantization_error = std::bit_cast<double>(quantization_bits);
    if (auto result = artifact.validate(); !result) {
        return core::Result<CurvatureMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<CurvatureMapArtifact>::failure(validation(
            "curvature artifact serialization is not canonical"));
    }
    return core::Result<CurvatureMapArtifact>::success(std::move(artifact));
}

core::Result<ProductionCookKey> bake_curvature_cook_key(
    const CurvatureRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto base = bake_production_cook_key(recipe.binding, std::move(platform_profile));
    if (!base) return core::Result<ProductionCookKey>::failure(base.error());
    const auto digest = recipe.canonical_digest();
    if (!digest) return core::Result<ProductionCookKey>::failure(digest.error());
    ProductionCookKey key = base.value();
    key.algorithm = std::string(kBakeCurvatureAlgorithm);
    key.settings_digest = hash_text(std::string(kBakeCurvatureAlgorithm) + "\n" +
        digest.value().hex());
    key.representation_kind = "signed-mean-curvature-rgba8-v1";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_curvature_graph_node(
    ProductionCookGraph& graph,
    const CurvatureRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_curvature_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<void> publish_curvature_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const CurvatureMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = validate_bake_sources(
            artifact.recipe.binding, current_source, current_target); !result) {
        return result;
    }
    const auto expected_key = bake_curvature_cook_key(
        artifact.recipe, std::move(platform_profile));
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "curvature receipt does not match the current bake recipe"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const BakeMeshBinding& source_binding = artifact.recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_authoritative_source, policy);
}

core::Result<CurvatureMapArtifact> read_curvature_map(
    const ProductionArtifactStore& store,
    const CurvatureRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = validate_bake_sources(
            recipe.binding, current_source, current_target); !result) {
        return core::Result<CurvatureMapArtifact>::failure(result.error());
    }
    const auto key = bake_curvature_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<CurvatureMapArtifact>::failure(key.error());
    const BakeMeshBinding& source_binding = recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    const auto payload = store.read(key.value(), current_authoritative_source, policy);
    if (!payload) return core::Result<CurvatureMapArtifact>::failure(payload.error());
    auto artifact = CurvatureMapArtifact::deserialize(payload.value());
    if (!artifact) return artifact;
    if (artifact.value().recipe.serialize() != recipe.serialize()) {
        return core::Result<CurvatureMapArtifact>::failure(validation(
            "stored curvature artifact does not match the requested bake recipe"));
    }
    return artifact;
}

CurvatureExecutionReport compile_curvature_cpu(
    const CurvatureRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    CurvatureExecutionReport report;
    if (auto result = recipe.validate(); !result) {
        add_error(report, "curvature_recipe_invalid", result.error().message);
        return report;
    }
    if (auto result = validate_bake_sources(recipe.binding, source, target); !result) {
        add_error(report, "bake_input_invalid", result.error().message);
        return report;
    }
    auto source_triangles = collect_source_curvature(source);
    auto target_triangles = collect_target_triangles(target, recipe.binding.target_uv_set);
    if (!source_triangles || !target_triangles) {
        const auto& error = !source_triangles
            ? source_triangles.error() : target_triangles.error();
        add_error(report, "curvature_mesh_invalid", error.message);
        return report;
    }
    const std::size_t texel_count = static_cast<std::size_t>(recipe.binding.width) *
        recipe.binding.height;
    std::vector<std::uint8_t> pixels(texel_count * 4U, 0U);
    for (std::size_t pixel = 0U; pixel < pixels.size(); pixel += 4U) {
        pixels[pixel] = 128U;
        pixels[pixel + 1U] = 128U;
        pixels[pixel + 2U] = 128U;
    }
    std::vector<bool> occupied(texel_count, false);
    std::vector<geometry::FaceId> owners(texel_count);
    CurvatureBvh bvh(source_triangles.value());
    std::uint64_t raster_candidates = 0U;
    std::vector<BakeProjectionMissSample> miss_samples;
    bool miss_samples_truncated = false;
    double minimum_curvature = std::numeric_limits<double>::infinity();
    double maximum_curvature = -std::numeric_limits<double>::infinity();
    const double tile_left = static_cast<double>(recipe.binding.tile_u);
    const double tile_bottom = static_cast<double>(recipe.binding.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;

    for (CurvatureTriangle triangle : target_triangles.value()) {
        const double minimum_u = std::min({triangle.uvs[0U].x,
            triangle.uvs[1U].x, triangle.uvs[2U].x});
        const double maximum_u = std::max({triangle.uvs[0U].x,
            triangle.uvs[1U].x, triangle.uvs[2U].x});
        const double minimum_v = std::min({triangle.uvs[0U].y,
            triangle.uvs[1U].y, triangle.uvs[2U].y});
        const double maximum_v = std::max({triangle.uvs[0U].y,
            triangle.uvs[1U].y, triangle.uvs[2U].y});
        if (maximum_u <= tile_left || minimum_u >= tile_right ||
            maximum_v <= tile_bottom || minimum_v >= tile_top) continue;
        const bool all_in_tile = std::all_of(triangle.uvs.begin(), triangle.uvs.end(),
            [tile_left, tile_right, tile_bottom, tile_top](core::Vec2d uv) {
                return uv.x >= tile_left && uv.x <= tile_right &&
                    uv.y >= tile_bottom && uv.y <= tile_top;
            });
        if (!all_in_tile) {
            add_error(report, "cross_tile_uv_triangle",
                "curvature bake requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }
        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kCurvatureEpsilon) {
            add_error(report, "degenerate_target_uv",
                "curvature target triangle has a degenerate UV chart", triangle.face);
            continue;
        }
        if (area < 0.0) {
            std::swap(triangle.positions[1U], triangle.positions[2U]);
            std::swap(triangle.normals[1U], triangle.normals[2U]);
            std::swap(triangle.uvs[1U], triangle.uvs[2U]);
            area = -area;
        }
        const double local_min_u = std::clamp(minimum_u - tile_left, 0.0, 1.0);
        const double local_max_u = std::clamp(maximum_u - tile_left, 0.0, 1.0);
        const double local_min_v = std::clamp(minimum_v - tile_bottom, 0.0, 1.0);
        const double local_max_v = std::clamp(maximum_v - tile_bottom, 0.0, 1.0);
        const std::uint32_t min_x = pixel_floor(
            std::floor(local_min_u * recipe.binding.width), recipe.binding.width);
        const std::uint32_t max_x = pixel_floor(
            std::floor(local_max_u * recipe.binding.width), recipe.binding.width);
        const std::uint32_t min_y = pixel_floor(
            std::floor(local_min_v * recipe.binding.height), recipe.binding.height);
        const std::uint32_t max_y = pixel_floor(
            std::floor(local_max_v * recipe.binding.height), recipe.binding.height);
        const double edge_epsilon = std::max(1.0, area) * 1e-14;

        for (std::uint32_t y = min_y; y <= max_y; ++y) {
            for (std::uint32_t x = min_x; x <= max_x; ++x) {
                ++raster_candidates;
                if (raster_candidates > kMaxBakeRasterCandidates) {
                    add_error(report, "raster_work_budget_exceeded",
                        "curvature target rasterization exceeded its deterministic work budget",
                        triangle.face);
                    return report;
                }
                const core::Vec2d sample{
                    tile_left + (static_cast<double>(x) + 0.5) / recipe.binding.width,
                    tile_bottom + (static_cast<double>(y) + 0.5) / recipe.binding.height};
                const double edge_zero = edge_value(triangle.uvs[1U], triangle.uvs[2U], sample);
                const double edge_one = edge_value(triangle.uvs[2U], triangle.uvs[0U], sample);
                const double edge_two = edge_value(triangle.uvs[0U], triangle.uvs[1U], sample);
                const auto inside = [edge_epsilon](double edge, bool inclusive) {
                    return edge > edge_epsilon ||
                        (std::abs(edge) <= edge_epsilon && inclusive);
                };
                if (!inside(edge_zero, top_left_edge(triangle.uvs[1U], triangle.uvs[2U])) ||
                    !inside(edge_one, top_left_edge(triangle.uvs[2U], triangle.uvs[0U])) ||
                    !inside(edge_two, top_left_edge(triangle.uvs[0U], triangle.uvs[1U]))) continue;

                ++report.target_texels_visited;
                const std::size_t texel = static_cast<std::size_t>(y) *
                    recipe.binding.width + x;
                if (occupied[texel]) {
                    std::ostringstream message;
                    message << "target UV triangles overlap at texel " << x << ',' << y
                            << " (previous face " << owners[texel].value << ')';
                    add_error(report, "overlapping_uv_islands", message.str(), triangle.face, x, y);
                    continue;
                }
                occupied[texel] = true;
                owners[texel] = triangle.face;
                ++report.covered_texels;

                const std::array<double, 3U> barycentric{
                    edge_zero / area, edge_one / area, edge_two / area};
                const core::Vec3d target_position =
                    triangle.positions[0U] * barycentric[0U] +
                    triangle.positions[1U] * barycentric[1U] +
                    triangle.positions[2U] * barycentric[2U];
                const core::Vec3d target_normal = (
                    triangle.normals[0U] * barycentric[0U] +
                    triangle.normals[1U] * barycentric[1U] +
                    triangle.normals[2U] * barycentric[2U]).normalized();
                if (!target_position.finite() || !target_normal.finite() ||
                    target_normal.length() <= 0.0) {
                    add_error(report, "target_shading_frame_invalid",
                        "curvature target interpolation produced a degenerate frame",
                        triangle.face, x, y);
                    continue;
                }
                const double projection_extent = recipe.binding.projection_half_extent;
                const core::Vec3d ray_origin = target_position + target_normal * projection_extent;
                const auto hit = bvh.cast(ray_origin, target_normal * -1.0,
                    projection_extent * 2.0, recipe.binding.backface_policy);
                if (!hit) {
                    ++report.projection_misses;
                    if (miss_samples.size() < kMaxCurvatureMissSamples) {
                        miss_samples.push_back({triangle.face, x, y});
                    } else {
                        miss_samples_truncated = true;
                    }
                    add_miss_diagnostic(report, recipe.binding.reject_projection_misses,
                        triangle.face, x, y);
                    continue;
                }
                const CurvatureTriangle& source_triangle =
                    source_triangles.value()[hit->triangle_index];
                const double signed_curvature =
                    source_triangle.curvature[0U] * hit->barycentric[0U] +
                    source_triangle.curvature[1U] * hit->barycentric[1U] +
                    source_triangle.curvature[2U] * hit->barycentric[2U];
                if (!std::isfinite(signed_curvature)) {
                    add_error(report, "source_curvature_invalid",
                        "projected source curvature is non-finite", triangle.face, x, y);
                    continue;
                }
                ++report.projected_texels;
                minimum_curvature = std::min(minimum_curvature, signed_curvature);
                maximum_curvature = std::max(maximum_curvature, signed_curvature);
                if (std::abs(signed_curvature) > recipe.normalization_scale) {
                    ++report.clamped_texels;
                }
                const double normalized = std::clamp(
                    signed_curvature / recipe.normalization_scale, -1.0, 1.0);
                const auto encoded = static_cast<std::uint8_t>(std::clamp(
                    std::lround((normalized * 0.5 + 0.5) * 255.0), 0L, 255L));
                const std::size_t pixel = texel * 4U;
                pixels[pixel] = encoded;
                pixels[pixel + 1U] = encoded;
                pixels[pixel + 2U] = encoded;
                pixels[pixel + 3U] = 255U;
            }
        }
    }

    if (std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        return report;
    }
    if (report.covered_texels == 0U || report.projected_texels == 0U) {
        add_error(report, "curvature_projection_empty",
            "selected target UV tile produced no projected curvature texels");
        return report;
    }
    CurvatureMapArtifact artifact;
    artifact.recipe = recipe;
    artifact.covered_texels = report.covered_texels;
    artifact.projected_texels = report.projected_texels;
    artifact.projection_misses = report.projection_misses;
    artifact.projection_miss_samples = std::move(miss_samples);
    artifact.projection_miss_samples_truncated = miss_samples_truncated;
    artifact.clamped_texels = report.clamped_texels;
    artifact.minimum_signed_curvature = minimum_curvature;
    artifact.maximum_signed_curvature = maximum_curvature;
    artifact.rgba8 = std::move(pixels);
    if (auto result = artifact.refresh_digests(); !result) {
        add_error(report, "curvature_artifact_invalid", result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        add_error(report, "curvature_artifact_invalid", result.error().message);
        return report;
    }
    report.artifact = std::move(artifact);
    return report;
}

} // namespace carto::production
