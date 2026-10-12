#include <carto/production/bake_graph.hpp>

#include "normal_map_projection.hpp"
#include "ambient_occlusion_projection.hpp"

#include <carto/render/tangent.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::production {

namespace {

constexpr std::string_view kRecipeMagic = "CARTO_BAKE_RECIPE 1";
constexpr std::string_view kArtifactMagic = "CARTO_BAKE_NORMAL_ARTIFACT";
constexpr std::string_view kTileCacheEntryMagic = "CARTO_BAKE_NORMAL_TILE_CACHE";
constexpr std::uint32_t kTileCacheEntryVersion = 1U;
constexpr std::string_view kAmbientOcclusionRecipeMagic = "CARTO_BAKE_AO_RECIPE 1";
constexpr std::string_view kAmbientOcclusionArtifactMagic = "CARTO_BAKE_AO_ARTIFACT";
constexpr std::uint32_t kAmbientOcclusionArtifactVersion = 1U;
constexpr std::string_view kBentNormalArtifactMagic = "CARTO_BAKE_BENT_NORMAL_ARTIFACT";
constexpr std::uint32_t kBentNormalArtifactVersion = 1U;
constexpr std::string_view kBakePaddingArtifactMagic = "CARTO_BAKE_PADDING_ARTIFACT";
constexpr std::uint32_t kBakePaddingArtifactVersion = 1U;
constexpr std::uint32_t kBakePaddingArtifactVersionTyped = 2U;
constexpr std::size_t kMaxIdentityBytes = 1024U;
constexpr std::size_t kMaxArtifactBytes =
    static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL + 32ULL * 1024ULL);
constexpr std::size_t kMaxBakePaddingArtifactBytes =
    static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL + 64ULL * 1024ULL);
constexpr std::size_t kMaxTileCacheEntryBytes =
    static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL + 64ULL * 1024ULL);
constexpr std::size_t kMaxBakeDiagnostics = 64U;
constexpr std::size_t kMaxProjectionMissSamples = 16U;
constexpr std::size_t kMaxBakeTangentVertices = 2'000'000U;
constexpr double kGeometryEpsilon = 1e-12;
constexpr std::size_t kBvhLeafTriangles = 8U;

std::string_view normal_algorithm(BakeNormalSpace normal_space) {
    switch (normal_space) {
    case BakeNormalSpace::tangent: return kBakeNormalAlgorithm;
    case BakeNormalSpace::object: return kBakeObjectNormalAlgorithm;
    }
    return {};
}

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

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
                byte != '\r' && byte != '\n';
        });
}

std::string encode_hex(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

std::optional<std::string> decode_hex(
    std::string_view value,
    std::size_t maximum_bytes = kMaxIdentityBytes) {
    if (value.size() % 2U != 0U || value.size() / 2U > maximum_bytes) {
        return std::nullopt;
    }
    const auto nibble = [](char digit) -> std::optional<std::uint8_t> {
        if (digit >= '0' && digit <= '9') {
            return static_cast<std::uint8_t>(digit - '0');
        }
        if (digit >= 'a' && digit <= 'f') {
            return static_cast<std::uint8_t>(10 + digit - 'a');
        }
        return std::nullopt;
    };
    std::string decoded;
    decoded.reserve(value.size() / 2U);
    for (std::size_t index = 0U; index < value.size(); index += 2U) {
        const auto high = nibble(value[index]);
        const auto low = nibble(value[index + 1U]);
        if (!high || !low) return std::nullopt;
        decoded.push_back(static_cast<char>((*high << 4U) | *low));
    }
    return decoded;
}

std::string double_bits_hex(double value) {
    constexpr char digits[] = "0123456789abcdef";
    std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    std::string encoded(16U, '0');
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        const std::size_t shift = (encoded.size() - index - 1U) * 4U;
        encoded[index] = digits[(bits >> shift) & 0x0fU];
    }
    return encoded;
}

std::optional<double> double_from_bits_hex(std::string_view value) {
    if (value.size() != 16U) return std::nullopt;
    std::uint64_t bits = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), bits, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return std::bit_cast<double>(bits);
}

std::optional<std::uint64_t> parse_u64(std::string_view value) {
    if (value.empty()) return std::nullopt;
    std::uint64_t parsed_value = 0U;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), parsed_value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed_value;
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    const auto parsed = parse_u64(value);
    if (!parsed || *parsed > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*parsed);
}

assets::Sha256Digest hash_bytes(std::span<const std::uint8_t> bytes) {
    return assets::sha256(bytes);
}

assets::Sha256Digest hash_text(std::string_view text) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    return hash_bytes(std::span<const std::uint8_t>(data, text.size()));
}

core::Result<assets::Sha256Digest> normal_map_settings_digest(
    const BakeRecipe& recipe,
    BakeNormalSpace normal_space) {
    const std::string_view algorithm = normal_algorithm(normal_space);
    if (algorithm.empty()) {
        return core::Result<assets::Sha256Digest>::failure(
            core::Diagnostic(core::ErrorCode::invalid_argument,
                "bake normal space is not supported"));
    }
    const auto recipe_settings = recipe.settings_digest();
    if (!recipe_settings) {
        return core::Result<assets::Sha256Digest>::failure(recipe_settings.error());
    }
    if (normal_space == BakeNormalSpace::tangent) return recipe_settings;
    return core::Result<assets::Sha256Digest>::success(hash_text(
        std::string(algorithm) + "\n" + recipe_settings.value().hex()));
}

core::Result<assets::Sha256Digest> bake_padding_settings_digest(
    const BakePaddingRecipe& recipe) {
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return core::Result<assets::Sha256Digest>::failure(recipe_digest.error());
    return core::Result<assets::Sha256Digest>::success(hash_text(
        std::string(kBakePaddingAlgorithm) + "\n" + recipe_digest.value().hex()));
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

void append_digest(
    std::vector<std::uint8_t>& bytes,
    const assets::Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
}

struct ByteReader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0U;

    bool read_u32(std::uint32_t& value) {
        if (bytes.size() - offset < 4U) return false;
        value = 0U;
        for (unsigned index = 0U; index < 4U; ++index) {
            value |= static_cast<std::uint32_t>(bytes[offset++]) << (index * 8U);
        }
        return true;
    }

    bool read_u64(std::uint64_t& value) {
        if (bytes.size() - offset < 8U) return false;
        value = 0U;
        for (unsigned index = 0U; index < 8U; ++index) {
            value |= static_cast<std::uint64_t>(bytes[offset++]) << (index * 8U);
        }
        return true;
    }

    bool read_string(std::string& value, std::size_t maximum) {
        std::uint64_t length = 0U;
        if (!read_u64(length) || length > maximum ||
            length > static_cast<std::uint64_t>(bytes.size() - offset)) {
            return false;
        }
        const std::size_t count = static_cast<std::size_t>(length);
        value.assign(reinterpret_cast<const char*>(bytes.data() + offset), count);
        offset += count;
        return true;
    }

    bool read_digest(assets::Sha256Digest& digest) {
        if (bytes.size() - offset < digest.bytes.size()) return false;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            digest.bytes.size(), digest.bytes.begin());
        offset += digest.bytes.size();
        return true;
    }

    bool read_payload(std::vector<std::uint8_t>& payload, std::size_t maximum) {
        std::uint64_t length = 0U;
        if (!read_u64(length) || length > maximum ||
            length > static_cast<std::uint64_t>(bytes.size() - offset)) {
            return false;
        }
        const std::size_t count = static_cast<std::size_t>(length);
        payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
        return true;
    }
};

std::vector<std::uint8_t> canonical_bake_tile_dependency_bytes(
    const BakeTileDependencyKey& key) {
    std::vector<std::uint8_t> canonical;
    append_string(canonical, "CARTO_BAKE_TILE_DEPENDENCY 1");
    append_string(canonical, key.source_identity);
    append_u64(canonical, key.source_revision.value());
    append_digest(canonical, key.source_geometry_digest);
    append_string(canonical, key.target_identity);
    append_u64(canonical, key.target_uv_set);
    append_u32(canonical, key.tile_u);
    append_u32(canonical, key.tile_v);
    append_digest(canonical, key.target_geometry_digest);
    append_digest(canonical, key.target_tile_digest);
    append_digest(canonical, key.settings_digest);
    return canonical;
}

std::vector<std::uint8_t> canonical_mesh_bytes(
    const geometry::EditableMesh& mesh,
    std::optional<attributes::UvSetId> uv_set) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, uv_set ? "carto.bake.target-mesh.v1" : "carto.bake.source-mesh.v1");

    const auto vertices = mesh.vertices_sorted();
    append_u64(bytes, static_cast<std::uint64_t>(vertices.size()));
    for (const geometry::Vertex& vertex : vertices) {
        append_u64(bytes, vertex.id.value);
        append_double(bytes, vertex.position.x == 0.0 ? 0.0 : vertex.position.x);
        append_double(bytes, vertex.position.y == 0.0 ? 0.0 : vertex.position.y);
        append_double(bytes, vertex.position.z == 0.0 ? 0.0 : vertex.position.z);
    }

    const auto faces = mesh.faces_sorted();
    append_u64(bytes, static_cast<std::uint64_t>(faces.size()));
    for (const geometry::Face& face : faces) {
        append_u64(bytes, face.id.value);
        append_u64(bytes, static_cast<std::uint64_t>(face.vertices.size()));
        for (const geometry::VertexId vertex : face.vertices) {
            append_u64(bytes, vertex.value);
        }
    }

    if (uv_set) {
        const attributes::UvSetDescriptor* descriptor = mesh.uv_sets().find(*uv_set);
        const attributes::AttributeLayer* layer = descriptor == nullptr
            ? nullptr : mesh.attributes().find(descriptor->layer_id);
        const auto topology = mesh.topology();
        if (descriptor == nullptr || layer == nullptr || !topology) return {};
        append_u64(bytes, descriptor->id);
        append_u32(bytes, descriptor->tile_policy_version);
        append_u64(bytes, static_cast<std::uint64_t>(topology.value().corners.size()));
        for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
            const geometry::CornerRecord& corner = topology.value().corners[index];
            const auto* uv = std::get_if<core::Vec2d>(&layer->values[index]);
            if (uv == nullptr) return {};
            append_u64(bytes, corner.id.value);
            append_u64(bytes, corner.face.value);
            append_u64(bytes, corner.vertex.value);
            append_double(bytes, uv->x == 0.0 ? 0.0 : uv->x);
            append_double(bytes, uv->y == 0.0 ? 0.0 : uv->y);
        }
    }
    return bytes;
}

struct Triangle {
    std::array<core::Vec3d, 3U> positions{};
    std::array<core::Vec3d, 3U> normals{};
    std::array<core::Vec2d, 3U> uvs{};
    std::array<geometry::VertexId, 3U> vertex_ids{};
    core::Vec3d geometric_normal{};
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

core::Result<std::vector<Triangle>> collect_triangles(
    const geometry::EditableMesh& mesh,
    std::optional<attributes::UvSetId> uv_set) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<std::vector<Triangle>>::failure(result.error());
    }
    if (mesh.vertex_count() == 0U || mesh.face_count() == 0U) {
        return core::Result<std::vector<Triangle>>::failure(
            validation("bake mesh must contain vertices and faces"));
    }
    const auto compiled = mesh.compile();
    const auto topology = mesh.topology();
    if (!compiled || !topology) {
        return core::Result<std::vector<Triangle>>::failure(
            compiled ? topology.error() : compiled.error());
    }

    std::map<geometry::VertexId, core::Vec3d> normals;
    for (std::size_t index = 0U; index < compiled.value().vertex_ids.size(); ++index) {
        normals.emplace(compiled.value().vertex_ids[index], compiled.value().normals[index]);
    }

    std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec2d> uv_by_face_vertex;
    if (uv_set) {
        const attributes::UvSetDescriptor* descriptor = mesh.uv_sets().find(*uv_set);
        if (descriptor == nullptr) {
            return core::Result<std::vector<Triangle>>::failure(
                validation("bake target references a missing UV set"));
        }
        const attributes::AttributeLayer* layer = mesh.attributes().find(descriptor->layer_id);
        if (layer == nullptr ||
            layer->descriptor.type != attributes::AttributeType::vec2 ||
            layer->values.size() != topology.value().corners.size()) {
            return core::Result<std::vector<Triangle>>::failure(
                validation("bake target UV set is not a complete corner-domain vec2 layer"));
        }
        for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
            const auto* value = std::get_if<core::Vec2d>(&layer->values[index]);
            if (value == nullptr || !value->finite()) {
                return core::Result<std::vector<Triangle>>::failure(
                    validation("bake target UV set contains a non-finite coordinate"));
            }
            uv_by_face_vertex.emplace(
                std::make_pair(topology.value().corners[index].face,
                    topology.value().corners[index].vertex), *value);
        }
    }

    std::vector<Triangle> triangles;
    for (const geometry::Face& face : mesh.faces_sorted()) {
        if (face.vertices.size() < 3U) {
            return core::Result<std::vector<Triangle>>::failure(
                validation("bake mesh contains a face with fewer than three vertices"));
        }
        std::vector<core::Vec3d> positions;
        std::vector<core::Vec3d> vertex_normals;
        std::vector<core::Vec2d> uvs;
        positions.reserve(face.vertices.size());
        vertex_normals.reserve(face.vertices.size());
        if (uv_set) uvs.reserve(face.vertices.size());
        for (const geometry::VertexId id : face.vertices) {
            const geometry::Vertex* vertex = mesh.find_vertex(id);
            const auto normal = normals.find(id);
            if (vertex == nullptr || normal == normals.end()) {
                return core::Result<std::vector<Triangle>>::failure(
                    validation("bake mesh face references a missing vertex or normal"));
            }
            positions.push_back(vertex->position);
            vertex_normals.push_back(normal->second);
            if (uv_set) {
                const auto uv = uv_by_face_vertex.find(std::make_pair(face.id, id));
                if (uv == uv_by_face_vertex.end()) {
                    return core::Result<std::vector<Triangle>>::failure(
                        validation("bake target face is missing a corner UV coordinate"));
                }
                uvs.push_back(uv->second);
            }
        }
        for (std::size_t fan = 1U; fan + 1U < face.vertices.size(); ++fan) {
            if (triangles.size() >= kMaxBakeSourceTriangles) {
                return core::Result<std::vector<Triangle>>::failure(
                    validation("bake mesh exceeds the CPU reference triangle limit"));
            }
            const std::array<std::size_t, 3U> indices{0U, fan, fan + 1U};
            Triangle triangle;
            triangle.face = face.id;
            triangle.fan_index = static_cast<std::uint32_t>(fan - 1U);
            for (std::size_t corner = 0U; corner < indices.size(); ++corner) {
                triangle.positions[corner] = positions[indices[corner]];
                triangle.normals[corner] = vertex_normals[indices[corner]];
                triangle.vertex_ids[corner] = face.vertices[indices[corner]];
                if (uv_set) triangle.uvs[corner] = uvs[indices[corner]];
            }
            const core::Vec3d geometric = core::cross(
                triangle.positions[1U] - triangle.positions[0U],
                triangle.positions[2U] - triangle.positions[0U]);
            triangle.geometric_normal = geometric.normalized();
            if (!triangle.geometric_normal.finite() || triangle.geometric_normal.length() <= 0.0) {
                return core::Result<std::vector<Triangle>>::failure(
                    validation("bake mesh contains a degenerate triangle"));
            }
            triangle.bounds_min = component_min(
                triangle.positions[0U],
                component_min(triangle.positions[1U], triangle.positions[2U]));
            triangle.bounds_max = component_max(
                triangle.positions[0U],
                component_max(triangle.positions[1U], triangle.positions[2U]));
            triangles.push_back(std::move(triangle));
        }
    }
    if (triangles.empty()) {
        return core::Result<std::vector<Triangle>>::failure(
            validation("bake mesh contains no triangles"));
    }
    return core::Result<std::vector<Triangle>>::success(std::move(triangles));
}

template <typename Report>
void report_diagnostic(
    Report& report,
    std::string code,
    std::string message,
    bool is_error,
    std::optional<geometry::FaceId> target_face = std::nullopt,
    std::optional<geometry::FaceId> source_face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt);

template <typename Report>
core::Result<std::vector<std::array<core::Vec4d, 3U>>> generate_target_tangent_frames(
    const geometry::EditableMesh& mesh,
    const std::vector<Triangle>& triangles,
    attributes::UvSetId uv_set,
    Report& report) {
    if (triangles.empty()) {
        return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::failure(
            validation("tangent generation requires target triangles"));
    }

    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::uint32_t>
        tangent_index_by_vertex_uv;
    std::vector<render::TangentVertex> tangent_vertices;
    tangent_vertices.reserve(std::min(
        triangles.size() * 3U, kMaxBakeTangentVertices));
    std::vector<std::uint32_t> tangent_indices;
    tangent_indices.reserve(triangles.size() * 3U);
    std::vector<std::array<std::uint32_t, 3U>> corner_indices(triangles.size());

    for (std::size_t triangle_index = 0U;
         triangle_index < triangles.size(); ++triangle_index) {
        const Triangle& triangle = triangles[triangle_index];
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const core::Vec2d uv = triangle.uvs[corner];
            const auto canonical_bits = [](double value) {
                return std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value);
            };
            const auto key = std::make_tuple(
                triangle.vertex_ids[corner].value,
                canonical_bits(uv.x),
                canonical_bits(uv.y));
            auto found = tangent_index_by_vertex_uv.find(key);
            if (found == tangent_index_by_vertex_uv.end()) {
                if (tangent_vertices.size() >= kMaxBakeTangentVertices) {
                    return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::failure(
                        validation("target tangent vertices exceed the deterministic budget"));
                }
                const std::uint32_t index = static_cast<std::uint32_t>(tangent_vertices.size());
                tangent_vertices.push_back({
                    triangle.positions[corner], triangle.normals[corner], uv});
                found = tangent_index_by_vertex_uv.emplace(key, index).first;
            }
            corner_indices[triangle_index][corner] = found->second;
            tangent_indices.push_back(found->second);
        }
    }

    const auto tangents = render::generate_tangents(tangent_vertices, tangent_indices);
    if (!tangents) {
        return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::failure(
            tangents.error());
    }

    const auto topology = mesh.topology();
    const attributes::UvSetDescriptor* uv_descriptor = mesh.uv_sets().find(uv_set);
    const attributes::AttributeLayer* uv_layer = uv_descriptor == nullptr
        ? nullptr : mesh.attributes().find(uv_descriptor->layer_id);
    if (!topology || uv_layer == nullptr ||
        uv_layer->descriptor.type != attributes::AttributeType::vec2 ||
        uv_layer->values.size() != topology.value().corners.size()) {
        return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::failure(
            validation("target tangent diagnostics require the bound corner UV layer"));
    }
    const auto find_half_edge = [&](geometry::HalfEdgeId id)
        -> const geometry::HalfEdgeRecord* {
        const auto found = std::lower_bound(
            topology.value().half_edges.begin(), topology.value().half_edges.end(), id,
            [](const geometry::HalfEdgeRecord& record, geometry::HalfEdgeId sought) {
                return record.id < sought;
            });
        return found != topology.value().half_edges.end() && found->id == id
            ? &*found : nullptr;
    };
    const auto uv_at_corner = [&](geometry::CornerId id)
        -> core::Result<core::Vec2d> {
        const auto found = std::lower_bound(
            topology.value().corners.begin(), topology.value().corners.end(), id,
            [](const geometry::CornerRecord& record, geometry::CornerId sought) {
                return record.id < sought;
            });
        if (found == topology.value().corners.end() || found->id != id) {
            return core::Result<core::Vec2d>::failure(
                validation("target tangent diagnostics found a stale corner reference"));
        }
        const std::size_t index = static_cast<std::size_t>(std::distance(
            topology.value().corners.begin(), found));
        const auto* uv = std::get_if<core::Vec2d>(&uv_layer->values[index]);
        if (uv == nullptr || !uv->finite()) {
            return core::Result<core::Vec2d>::failure(
                validation("target tangent diagnostics found an invalid corner UV"));
        }
        return core::Result<core::Vec2d>::success(*uv);
    };
    const auto uv_at_vertex = [&](const geometry::HalfEdgeRecord& half_edge,
        geometry::VertexId vertex) -> core::Result<core::Vec2d> {
        if (half_edge.origin == vertex) return uv_at_corner(half_edge.corner);
        if (half_edge.destination != vertex) {
            return core::Result<core::Vec2d>::failure(
                validation("target tangent diagnostics found a nonincident edge vertex"));
        }
        const geometry::HalfEdgeRecord* next = find_half_edge(half_edge.next);
        if (next == nullptr || next->face != half_edge.face || next->origin != vertex) {
            return core::Result<core::Vec2d>::failure(
                validation("target tangent diagnostics found a stale next half-edge"));
        }
        return uv_at_corner(next->corner);
    };
    const auto tangent_for = [&](geometry::VertexId vertex, core::Vec2d uv)
        -> core::Result<core::Vec4d> {
        const auto canonical_bits = [](double value) {
            return std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value);
        };
        const auto key = std::make_tuple(
            vertex.value, canonical_bits(uv.x), canonical_bits(uv.y));
        const auto index = tangent_index_by_vertex_uv.find(key);
        if (index == tangent_index_by_vertex_uv.end()) {
            return core::Result<core::Vec4d>::failure(
                validation("target tangent diagnostics could not resolve an edge tangent"));
        }
        return core::Result<core::Vec4d>::success(tangents.value()[index->second]);
    };
    for (const geometry::EdgeRecord& edge : topology.value().edges) {
        if (!edge.second_half_edge.has_value()) continue;
        const geometry::HalfEdgeRecord* first_half_edge = find_half_edge(edge.first_half_edge);
        const geometry::HalfEdgeRecord* second_half_edge =
            find_half_edge(edge.second_half_edge.value());
        if (first_half_edge == nullptr || second_half_edge == nullptr) {
            return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::failure(
                validation("target tangent diagnostics found a stale manifold edge"));
        }
        const geometry::FaceId first_face = first_half_edge->face;
        const geometry::FaceId second_face = second_half_edge->face;
        bool uv_seam = false;
        bool tangent_discontinuity = false;
        const std::array<geometry::VertexId, 2U> edge_vertices{edge.first, edge.second};
        std::array<core::Vec2d, 2U> first_uvs{};
        std::array<core::Vec2d, 2U> second_uvs{};
        for (std::size_t endpoint = 0U; endpoint < edge_vertices.size(); ++endpoint) {
            const geometry::VertexId vertex = edge_vertices[endpoint];
            const auto first_uv = uv_at_vertex(*first_half_edge, vertex);
            const auto second_uv = uv_at_vertex(*second_half_edge, vertex);
            if (!first_uv || !second_uv) {
                const auto& error = !first_uv ? first_uv.error() : second_uv.error();
                return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::failure(error);
            }
            first_uvs[endpoint] = first_uv.value();
            second_uvs[endpoint] = second_uv.value();
            uv_seam = uv_seam || first_uv.value().x != second_uv.value().x ||
                first_uv.value().y != second_uv.value().y;
        }
        if (!uv_seam) continue;
        for (std::size_t endpoint = 0U; endpoint < edge_vertices.size(); ++endpoint) {
            const auto first_tangent = tangent_for(edge_vertices[endpoint], first_uvs[endpoint]);
            const auto second_tangent = tangent_for(edge_vertices[endpoint], second_uvs[endpoint]);
            if (!first_tangent || !second_tangent) {
                const auto& error = !first_tangent
                    ? first_tangent.error() : second_tangent.error();
                return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::failure(error);
            }
            const core::Vec3d first_direction{
                first_tangent.value().x,
                first_tangent.value().y,
                first_tangent.value().z};
            const core::Vec3d second_direction{
                second_tangent.value().x,
                second_tangent.value().y,
                second_tangent.value().z};
            tangent_discontinuity = tangent_discontinuity ||
                core::dot(first_direction, second_direction) < 0.999999 ||
                (first_tangent.value().w < 0.0) != (second_tangent.value().w < 0.0);
        }
        if (uv_seam && tangent_discontinuity) {
            std::ostringstream message;
            message << "target UV seam at edge " << edge.id.value
                    << " changes tangent direction or handedness between faces "
                    << first_face.value << " and " << second_face.value;
            report_diagnostic(report, "target_tangent_discontinuity", message.str(), false,
                first_face, std::nullopt, std::nullopt, std::nullopt);
        }
    }

    std::vector<std::array<core::Vec4d, 3U>> result(triangles.size());
    for (std::size_t triangle_index = 0U;
         triangle_index < triangles.size(); ++triangle_index) {
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            result[triangle_index][corner] = tangents.value()[
                corner_indices[triangle_index][corner]];
        }
    }
    return core::Result<std::vector<std::array<core::Vec4d, 3U>>>::success(
        std::move(result));
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

struct RayHit {
    double distance = std::numeric_limits<double>::infinity();
    std::array<double, 3U> barycentric{};
    std::size_t triangle_index = 0U;
    bool front_facing = true;
};

struct RayCastResult {
    std::optional<RayHit> hit;
    std::optional<RayHit> rejected_backface;
};

class TriangleBvh final {
public:
    explicit TriangleBvh(const std::vector<Triangle>& triangles)
        : triangles_(triangles) {
        order_.resize(triangles_.size());
        for (std::size_t index = 0U; index < order_.size(); ++index) order_[index] = index;
        nodes_.reserve(triangles_.size() * 2U);
        if (!order_.empty()) static_cast<void>(build(0U, order_.size()));
    }

    [[nodiscard]] RayCastResult cast(
        core::Vec3d origin,
        core::Vec3d direction,
        double maximum_distance,
        BakeBackfacePolicy backface_policy) const {
        if (nodes_.empty()) return {};
        std::array<std::size_t, 128U> stack{};
        std::size_t stack_size = 1U;
        stack[0U] = 0U;
        std::optional<RayHit> best;
        std::optional<RayHit> rejected_backface;
        while (stack_size > 0U) {
            const BvhNode& node = nodes_[stack[--stack_size]];
            const double current_limit = best
                ? std::min(maximum_distance, best->distance + kGeometryEpsilon)
                : maximum_distance;
            if (!ray_intersects_bounds(origin, direction, node, current_limit)) continue;
            if (!node.leaf()) {
                // Push right first so traversal visits the stable left child first.
                if (stack_size + 2U <= stack.size()) {
                    stack[stack_size++] = node.right;
                    stack[stack_size++] = node.left;
                }
                continue;
            }
            for (std::size_t item = node.begin; item < node.end; ++item) {
                const std::size_t triangle_index = order_[item];
                const auto hit = intersect_triangle(
                    origin, direction, triangles_[triangle_index], maximum_distance);
                if (!hit) continue;
                const RayHit candidate{
                    hit->distance, hit->barycentric, triangle_index, hit->front_facing};
                const auto precedes = [this](const RayHit& left, const RayHit& right) {
                    return left.distance < right.distance - kGeometryEpsilon ||
                        (std::abs(left.distance - right.distance) <= kGeometryEpsilon &&
                            triangle_precedes(left.triangle_index, right.triangle_index));
                };
                if (backface_policy == BakeBackfacePolicy::front_only &&
                    !candidate.front_facing) {
                    if (!rejected_backface || precedes(candidate, *rejected_backface)) {
                        rejected_backface = candidate;
                    }
                    continue;
                }
                if (!best || precedes(candidate, *best)) {
                    best = candidate;
                }
            }
        }
        return {best, rejected_backface};
    }

private:
    struct Intersection {
        double distance = 0.0;
        std::array<double, 3U> barycentric{};
        bool front_facing = true;
    };

    [[nodiscard]] bool triangle_precedes(std::size_t first, std::size_t second) const {
        const Triangle& left = triangles_[first];
        const Triangle& right = triangles_[second];
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
        if (end - begin <= kBvhLeafTriangles) return node_index;

        const core::Vec3d extent = node.bounds_max - node.bounds_min;
        std::size_t axis = 0U;
        if (extent.y > extent.x && extent.y >= extent.z) axis = 1U;
        else if (extent.z > extent.x && extent.z > extent.y) axis = 2U;
        const auto centroid = [this, axis](std::size_t index) {
            const Triangle& triangle = triangles_[index];
            const double first = axis == 0U ? triangle.bounds_min.x
                : axis == 1U ? triangle.bounds_min.y : triangle.bounds_min.z;
            const double last = axis == 0U ? triangle.bounds_max.x
                : axis == 1U ? triangle.bounds_max.y : triangle.bounds_max.z;
            return first + (last - first) * 0.5;
        };
        std::stable_sort(order_.begin() + static_cast<std::ptrdiff_t>(begin),
            order_.begin() + static_cast<std::ptrdiff_t>(end),
            [this, &centroid](std::size_t left, std::size_t right) {
                const double left_center = centroid(left);
                const double right_center = centroid(right);
                if (left_center != right_center) return left_center < right_center;
                return triangle_precedes(left, right);
            });
        const std::size_t middle = begin + (end - begin) / 2U;
        const std::size_t left = build(begin, middle);
        const std::size_t right = build(middle, end);
        nodes_[node_index].left = left;
        nodes_[node_index].right = right;
        return node_index;
    }

    [[nodiscard]] static bool ray_intersects_bounds(
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
        const Triangle& triangle,
        double maximum_distance) {
        const core::Vec3d edge_one = triangle.positions[1U] - triangle.positions[0U];
        const core::Vec3d edge_two = triangle.positions[2U] - triangle.positions[0U];
        const core::Vec3d p = core::cross(direction, edge_two);
        const double determinant = core::dot(edge_one, p);
        if (std::abs(determinant) <= kGeometryEpsilon) {
            return std::nullopt;
        }
        const double inverse = 1.0 / determinant;
        const core::Vec3d from_first = origin - triangle.positions[0U];
        const double u = core::dot(from_first, p) * inverse;
        if (u < -kGeometryEpsilon || u > 1.0 + kGeometryEpsilon) return std::nullopt;
        const core::Vec3d q = core::cross(from_first, edge_one);
        const double v = core::dot(direction, q) * inverse;
        if (v < -kGeometryEpsilon || u + v > 1.0 + kGeometryEpsilon) return std::nullopt;
        const double distance = core::dot(edge_two, q) * inverse;
        if (distance < 0.0 || distance > maximum_distance) return std::nullopt;
        return Intersection{distance, {1.0 - u - v, u, v}, determinant > 0.0};
    }

    const std::vector<Triangle>& triangles_;
    std::vector<std::size_t> order_;
    std::vector<BvhNode> nodes_;
};

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

std::uint8_t encode_normal_component(double value) {
    const double scaled = std::clamp(value * 0.5 + 0.5, 0.0, 1.0) * 255.0;
    return static_cast<std::uint8_t>(std::clamp(std::lround(scaled), 0L, 255L));
}

std::vector<std::uint8_t> canonical_artifact_bytes(
    const NormalMapArtifact& artifact) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kArtifactMagic);
    append_u32(bytes, kBakeArtifactVersion);
    const std::string recipe = artifact.recipe.serialize();
    append_string(bytes, recipe);
    append_u32(bytes, static_cast<std::uint32_t>(artifact.normal_space));
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, artifact.projected_texels);
    append_u64(bytes, artifact.projection_misses);
    append_u32(bytes, static_cast<std::uint32_t>(artifact.projection_miss_samples.size()));
    append_u32(bytes, artifact.projection_miss_samples_truncated ? 1U : 0U);
    for (const BakeProjectionMissSample& sample : artifact.projection_miss_samples) {
        append_u64(bytes, sample.target_face.value);
        append_u32(bytes, sample.texel_x);
        append_u32(bytes, sample.texel_y);
    }
    append_u64(bytes, static_cast<std::uint64_t>(artifact.rgba8.size()));
    bytes.insert(bytes.end(), artifact.rgba8.begin(), artifact.rgba8.end());
    return bytes;
}

std::vector<std::uint8_t> canonical_bake_padding_artifact_bytes(
    const BakePaddingArtifact& artifact) {
    std::vector<std::uint8_t> bytes;
    const bool legacy_normal_map =
        artifact.recipe.input_kind == BakePaddingInputKind::normal_map;
    append_string(bytes, kBakePaddingArtifactMagic);
    append_u32(bytes, legacy_normal_map ? kBakePaddingArtifactVersion :
        kBakePaddingArtifactVersionTyped);
    append_string(bytes, artifact.recipe.binding.serialize());
    append_u32(bytes, static_cast<std::uint32_t>(artifact.recipe.input_normal_space));
    if (!legacy_normal_map) {
        append_u32(bytes, static_cast<std::uint32_t>(artifact.recipe.input_kind));
    }
    append_digest(bytes, artifact.recipe.input_artifact_digest);
    append_u32(bytes, artifact.recipe.padding_texels);
    append_string(bytes, artifact.algorithm);
    append_string(bytes, artifact.evidence_class);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.padded_texels);
    append_u64(bytes, artifact.byte_size);
    append_u64(bytes, static_cast<std::uint64_t>(artifact.rgba8.size()));
    bytes.insert(bytes.end(), artifact.rgba8.begin(), artifact.rgba8.end());
    return bytes;
}

std::vector<std::uint8_t> canonical_tile_cache_entry_bytes(
    const BakeTileArtifactCacheEntry& entry) {
    const auto artifact_bytes = entry.artifact.serialize();
    if (!artifact_bytes) return {};
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kTileCacheEntryMagic);
    append_u32(bytes, kTileCacheEntryVersion);
    append_string(bytes, entry.dependencies.source_identity);
    append_u64(bytes, entry.dependencies.source_revision.value());
    append_string(bytes, entry.dependencies.target_identity);
    append_u64(bytes, entry.dependencies.target_revision.value());
    append_u64(bytes, entry.dependencies.target_uv_set);
    append_u32(bytes, entry.dependencies.tile_u);
    append_u32(bytes, entry.dependencies.tile_v);
    append_digest(bytes, entry.dependencies.source_geometry_digest);
    append_digest(bytes, entry.dependencies.target_geometry_digest);
    append_digest(bytes, entry.dependencies.target_tile_digest);
    append_digest(bytes, entry.dependencies.settings_digest);
    append_digest(bytes, entry.dependencies.dependency_digest);
    append_u64(bytes, static_cast<std::uint64_t>(artifact_bytes.value().size()));
    bytes.insert(bytes.end(), artifact_bytes.value().begin(), artifact_bytes.value().end());
    return bytes;
}

core::Result<void> validate_tile_cache_entry_binding(
    const BakeTileArtifactCacheEntry& entry) {
    if (auto result = entry.artifact.validate(); !result) return result;
    const BakeTileDependencyKey& key = entry.dependencies;
    const BakeRecipe& recipe = entry.artifact.recipe;
    if (!safe_identity(key.source_identity) || !safe_identity(key.target_identity) ||
        key.source_identity != recipe.source.identity ||
        key.source_revision != recipe.source.revision ||
        key.target_identity != recipe.target.identity ||
        key.target_revision != recipe.target.revision ||
        key.target_uv_set != recipe.target_uv_set ||
        key.tile_u != recipe.tile_u || key.tile_v != recipe.tile_v ||
        key.source_geometry_digest != recipe.source.content_digest ||
        key.settings_digest != entry.artifact.settings_digest ||
        key.source_geometry_digest.is_zero() || key.target_geometry_digest.is_zero() ||
        key.target_tile_digest.is_zero() || key.settings_digest.is_zero() ||
        key.dependency_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "bake tile cache key does not match its source artifact binding"));
    }
    const auto canonical_dependencies = canonical_bake_tile_dependency_bytes(key);
    if (hash_bytes(canonical_dependencies) != key.dependency_digest) {
        return core::Result<void>::failure(validation(
            "bake tile cache dependency digest verification failed"));
    }
    return core::Result<void>::success();
}

template <typename Report>
void report_diagnostic(
    Report& report,
    std::string code,
    std::string message,
    bool is_error,
    std::optional<geometry::FaceId> target_face,
    std::optional<geometry::FaceId> source_face,
    std::optional<std::uint32_t> x,
    std::optional<std::uint32_t> y) {
    if (report.diagnostics.size() < kMaxBakeDiagnostics) {
        report.diagnostics.push_back(BakeDiagnostic{
            std::move(code), std::move(message), is_error, target_face, source_face, x, y});
    } else if (report.diagnostics.back().code == "diagnostics_truncated") {
        report.diagnostics.back().is_error = report.diagnostics.back().is_error || is_error;
    } else {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated",
            "additional bake diagnostics were omitted after the bounded report limit",
            is_error,
            target_face,
            std::nullopt,
            x,
            y};
    }
}

void report_error(
    BakeExecutionReport& report,
    std::string code,
    std::string message,
    std::optional<geometry::FaceId> target_face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    report_diagnostic(report, std::move(code), std::move(message), true,
        target_face, std::nullopt, x, y);
}

void report_warning(
    BakeExecutionReport& report,
    std::string code,
    std::string message,
    geometry::FaceId target_face,
    geometry::FaceId source_face,
    std::uint32_t x,
    std::uint32_t y) {
    report_diagnostic(report, std::move(code), std::move(message), false,
        target_face, source_face, x, y);
}

struct BakePaddingInputView {
    BakePaddingInputKind kind = BakePaddingInputKind::normal_map;
    BakeRecipe binding;
    BakeNormalSpace normal_space = BakeNormalSpace::tangent;
    assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::span<const std::uint8_t> rgba8;
};

core::Result<BakePaddingInputView> bake_padding_input_view(
    const NormalMapArtifact& input);
core::Result<BakePaddingInputView> bake_padding_input_view(
    const AmbientOcclusionMapArtifact& input);
core::Result<BakePaddingInputView> bake_padding_input_view(
    const BentNormalMapArtifact& input);
core::Result<BakePaddingInputView> bake_padding_input_view(
    const CurvatureMapArtifact& input);
core::Result<BakePaddingInputView> bake_padding_input_view(
    const ThicknessMapArtifact& input);
core::Result<BakePaddingInputView> bake_padding_input_view(
    const HeightDisplacementMapArtifact& input);
core::Result<BakePaddingInputView> bake_padding_input_view(BakePaddingInputRef input);

} // namespace

core::Result<void> BakeMeshBinding::validate() const {
    if (!safe_identity(identity) || revision.value() == 0U || content_digest.is_zero()) {
        return core::Result<void>::failure(
            invalid("bake mesh binding requires a stable identity, nonzero revision, and digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> BakeRecipe::validate() const {
    if (!safe_identity(identity)) {
        return core::Result<void>::failure(invalid("bake recipe identity is invalid"));
    }
    if (auto result = source.validate(); !result) return result;
    if (auto result = target.validate(); !result) return result;
    if (target_uv_set == 0U) {
        return core::Result<void>::failure(invalid("bake recipe requires a stable target UV-set id"));
    }
    if (width == 0U || height == 0U || width > kMaxBakeTextureExtent ||
        height > kMaxBakeTextureExtent ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
            kMaxBakeTextureTexels) {
        return core::Result<void>::failure(
            invalid("bake texture dimensions exceed the CPU reference limit"));
    }
    if (tile_u > 9U || tile_v > 99U) {
        return core::Result<void>::failure(invalid("bake recipe UDIM tile is outside the supported range"));
    }
    if (normal_convention != BakeNormalConvention::opengl_y_plus &&
        normal_convention != BakeNormalConvention::directx_y_minus) {
        return core::Result<void>::failure(invalid("bake normal convention is not supported"));
    }
    if (backface_policy != BakeBackfacePolicy::front_only &&
        backface_policy != BakeBackfacePolicy::both_sides) {
        return core::Result<void>::failure(invalid("bake backface policy is not supported"));
    }
    if (!std::isfinite(projection_half_extent) || projection_half_extent <= 0.0 ||
        projection_half_extent > 1'000'000.0) {
        return core::Result<void>::failure(
            invalid("bake projection half extent must be finite and positive"));
    }
    return core::Result<void>::success();
}

std::string BakeRecipe::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << kRecipeMagic << '\n'
           << "identity=" << encode_hex(identity) << '\n'
           << "source.identity=" << encode_hex(source.identity) << '\n'
           << "source.revision=" << source.revision.value() << '\n'
           << "source.digest=" << source.content_digest.hex() << '\n'
           << "target.identity=" << encode_hex(target.identity) << '\n'
           << "target.revision=" << target.revision.value() << '\n'
           << "target.digest=" << target.content_digest.hex() << '\n'
           << "target.uv_set=" << target_uv_set << '\n'
           << "target.width=" << width << '\n'
           << "target.height=" << height << '\n'
           << "tile.u=" << tile_u << '\n'
           << "tile.v=" << tile_v << '\n'
           << "normal.convention="
           << (normal_convention == BakeNormalConvention::opengl_y_plus
                   ? "opengl_y_plus" : "directx_y_minus") << '\n'
           << "projection.backface="
           << (backface_policy == BakeBackfacePolicy::front_only ? "front_only" : "both_sides")
           << '\n'
           << "projection.half_extent_bits=" << double_bits_hex(projection_half_extent) << '\n'
           << "policy.reject_projection_misses=" << (reject_projection_misses ? "1" : "0")
           << '\n';
    const std::string serialized = output.str();
    return serialized.size() <= kMaxBakeRecipeBytes ? serialized : std::string{};
}

core::Result<assets::Sha256Digest> BakeRecipe::settings_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::ostringstream settings;
    settings << kBakeNormalAlgorithm << '\n'
             << target_uv_set << '\n' << width << '\n' << height << '\n'
             << tile_u << '\n' << tile_v << '\n'
             << (normal_convention == BakeNormalConvention::opengl_y_plus
                     ? "opengl_y_plus" : "directx_y_minus") << '\n'
             << (backface_policy == BakeBackfacePolicy::front_only
                     ? "front_only" : "both_sides") << '\n'
             << double_bits_hex(projection_half_extent) << '\n'
             << (reject_projection_misses ? '1' : '0') << '\n';
    return core::Result<assets::Sha256Digest>::success(hash_text(settings.str()));
}

core::Result<assets::Sha256Digest> BakeRecipe::canonical_digest() const {
    const std::string serialized = serialize();
    if (serialized.empty()) {
        return core::Result<assets::Sha256Digest>::failure(
            invalid("cannot digest an invalid or oversized bake recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(serialized));
}

core::Result<BakeRecipe> BakeRecipe::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxBakeRecipeBytes) {
        return core::Result<BakeRecipe>::failure(invalid("bake recipe byte size is invalid"));
    }
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || line != kRecipeMagic) {
        return core::Result<BakeRecipe>::failure(validation("bake recipe header is unsupported"));
    }
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<BakeRecipe>::failure(
                validation("bake recipe contains a malformed or duplicate field"));
        }
    }
    if (fields.size() != 16U) {
        return core::Result<BakeRecipe>::failure(
            validation("bake recipe does not contain the exact supported field set"));
    }
    const auto field = [&fields](std::string_view name) -> std::optional<std::string_view> {
        const auto found = fields.find(std::string(name));
        if (found == fields.end()) return std::nullopt;
        return found->second;
    };
    const auto identity = field("identity");
    const auto source_identity = field("source.identity");
    const auto source_revision = field("source.revision");
    const auto source_digest = field("source.digest");
    const auto target_identity = field("target.identity");
    const auto target_revision = field("target.revision");
    const auto target_digest = field("target.digest");
    const auto uv_set = field("target.uv_set");
    const auto width_value = field("target.width");
    const auto height_value = field("target.height");
    const auto tile_u_value = field("tile.u");
    const auto tile_v_value = field("tile.v");
    const auto convention = field("normal.convention");
    const auto backface = field("projection.backface");
    const auto half_extent = field("projection.half_extent_bits");
    const auto reject_misses = field("policy.reject_projection_misses");
    if (!identity || !source_identity || !source_revision || !source_digest ||
        !target_identity || !target_revision || !target_digest || !uv_set ||
        !width_value || !height_value || !tile_u_value || !tile_v_value ||
        !convention || !backface || !half_extent || !reject_misses) {
        return core::Result<BakeRecipe>::failure(validation("bake recipe is missing a required field"));
    }
    const auto decoded_identity = decode_hex(*identity);
    const auto decoded_source_identity = decode_hex(*source_identity);
    const auto decoded_target_identity = decode_hex(*target_identity);
    const auto parsed_source_revision = parse_u64(*source_revision);
    const auto parsed_target_revision = parse_u64(*target_revision);
    const auto parsed_uv_set = parse_u64(*uv_set);
    const auto parsed_width = parse_u32(*width_value);
    const auto parsed_height = parse_u32(*height_value);
    const auto parsed_tile_u = parse_u32(*tile_u_value);
    const auto parsed_tile_v = parse_u32(*tile_v_value);
    const auto parsed_half_extent = double_from_bits_hex(*half_extent);
    const auto parsed_source_digest = assets::Sha256Digest::from_hex(*source_digest);
    const auto parsed_target_digest = assets::Sha256Digest::from_hex(*target_digest);
    if (!decoded_identity || !decoded_source_identity || !decoded_target_identity ||
        !parsed_source_revision || !parsed_target_revision || !parsed_uv_set ||
        !parsed_width || !parsed_height || !parsed_tile_u || !parsed_tile_v ||
        !parsed_half_extent || !parsed_source_digest || !parsed_target_digest ||
        (*reject_misses != "0" && *reject_misses != "1")) {
        return core::Result<BakeRecipe>::failure(validation("bake recipe field encoding is invalid"));
    }

    BakeRecipe recipe;
    recipe.identity = *decoded_identity;
    recipe.source = {*decoded_source_identity, core::Revision{*parsed_source_revision},
        parsed_source_digest.value()};
    recipe.target = {*decoded_target_identity, core::Revision{*parsed_target_revision},
        parsed_target_digest.value()};
    recipe.target_uv_set = *parsed_uv_set;
    recipe.width = *parsed_width;
    recipe.height = *parsed_height;
    recipe.tile_u = *parsed_tile_u;
    recipe.tile_v = *parsed_tile_v;
    recipe.projection_half_extent = *parsed_half_extent;
    recipe.reject_projection_misses = *reject_misses == "1";
    if (*convention == "opengl_y_plus") {
        recipe.normal_convention = BakeNormalConvention::opengl_y_plus;
    } else if (*convention == "directx_y_minus") {
        recipe.normal_convention = BakeNormalConvention::directx_y_minus;
    } else {
        return core::Result<BakeRecipe>::failure(validation("bake normal convention is unknown"));
    }
    if (*backface == "front_only") {
        recipe.backface_policy = BakeBackfacePolicy::front_only;
    } else if (*backface == "both_sides") {
        recipe.backface_policy = BakeBackfacePolicy::both_sides;
    } else {
        return core::Result<BakeRecipe>::failure(validation("bake backface policy is unknown"));
    }
    if (auto result = recipe.validate(); !result) {
        return core::Result<BakeRecipe>::failure(result.error());
    }
    if (recipe.serialize() != text) {
        return core::Result<BakeRecipe>::failure(
            validation("bake recipe serialization is not canonical"));
    }
    return core::Result<BakeRecipe>::success(std::move(recipe));
}

core::Result<assets::Sha256Digest> bake_mesh_source_digest(
    const geometry::EditableMesh& mesh) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const auto canonical = canonical_mesh_bytes(mesh, std::nullopt);
    if (canonical.empty()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("bake source mesh canonicalization failed"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_bytes(canonical));
}

core::Result<assets::Sha256Digest> bake_mesh_target_digest(
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set) {
    if (uv_set == 0U) {
        return core::Result<assets::Sha256Digest>::failure(invalid("bake target UV-set id is zero"));
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const attributes::UvSetDescriptor* descriptor = mesh.uv_sets().find(uv_set);
    const attributes::AttributeLayer* layer = descriptor == nullptr
        ? nullptr : mesh.attributes().find(descriptor->layer_id);
    if (descriptor == nullptr || layer == nullptr ||
        layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("bake target requires the named corner-domain vec2 UV set"));
    }
    const auto canonical = canonical_mesh_bytes(mesh, uv_set);
    if (canonical.empty()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("bake target mesh canonicalization failed"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_bytes(canonical));
}

core::Result<assets::Sha256Digest> bake_mesh_target_tile_digest(
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set,
    std::uint32_t tile_u,
    std::uint32_t tile_v) {
    if (uv_set == 0U || tile_u > 9U || tile_v > 99U) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "bake tile digest requires a supported UV set and UDIM address"));
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const attributes::UvSetDescriptor* descriptor = mesh.uv_sets().find(uv_set);
    const attributes::AttributeLayer* layer = descriptor == nullptr
        ? nullptr : mesh.attributes().find(descriptor->layer_id);
    if (descriptor == nullptr || layer == nullptr ||
        layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "bake tile digest requires a corner-domain vec2 UV set"));
    }
    const auto topology = mesh.topology();
    if (!topology) return core::Result<assets::Sha256Digest>::failure(topology.error());
    if (layer->values.size() != topology.value().corners.size()) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "bake tile UV layer cardinality does not match mesh topology"));
    }

    std::map<std::pair<geometry::FaceId, geometry::VertexId>, geometry::CornerId>
        corner_by_face_vertex;
    std::map<geometry::CornerId, core::Vec2d> uv_by_corner;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const geometry::CornerRecord& corner = topology.value().corners[index];
        const auto* uv = std::get_if<core::Vec2d>(&layer->values[index]);
        if (uv == nullptr || !uv->finite() ||
            !corner_by_face_vertex.emplace(
                std::make_pair(corner.face, corner.vertex), corner.id).second ||
            !uv_by_corner.emplace(corner.id, *uv).second) {
            return core::Result<assets::Sha256Digest>::failure(validation(
                "bake tile UV layer contains malformed or ambiguous corner data"));
        }
    }

    const double tile_left = static_cast<double>(tile_u);
    const double tile_bottom = static_cast<double>(tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;
    std::vector<std::uint8_t> canonical;
    append_string(canonical, "CARTO_BAKE_TARGET_TILE 1");
    append_u64(canonical, uv_set);
    append_u32(canonical, tile_u);
    append_u32(canonical, tile_v);
    std::uint64_t included_faces = 0U;
    std::uint64_t total_triangles = 0U;
    const std::size_t face_count_offset = canonical.size();
    append_u64(canonical, 0U);
    for (const geometry::Face& face : mesh.faces_sorted()) {
        if (face.vertices.size() < 3U) {
            return core::Result<assets::Sha256Digest>::failure(validation(
                "bake tile target contains a face with fewer than three vertices"));
        }
        const std::uint64_t face_triangles =
            static_cast<std::uint64_t>(face.vertices.size() - 2U);
        if (face_triangles > kMaxBakeSourceTriangles - total_triangles) {
            return core::Result<assets::Sha256Digest>::failure(unsupported(
                "bake tile target exceeds the bounded triangle workload"));
        }
        total_triangles += face_triangles;
        std::vector<core::Vec2d> face_uvs;
        face_uvs.reserve(face.vertices.size());
        for (const geometry::VertexId vertex : face.vertices) {
            const auto corner = corner_by_face_vertex.find(std::make_pair(face.id, vertex));
            if (corner == corner_by_face_vertex.end()) {
                return core::Result<assets::Sha256Digest>::failure(validation(
                    "bake tile target face is missing a stable UV corner"));
            }
            const core::Vec2d uv = uv_by_corner.at(corner->second);
            face_uvs.push_back(uv);
        }
        bool face_uses_tile = false;
        for (std::size_t fan = 1U; fan + 1U < face_uvs.size(); ++fan) {
            const std::array<core::Vec2d, 3U> triangle{
                face_uvs[0U], face_uvs[fan], face_uvs[fan + 1U]};
            const double minimum_u = std::min({
                triangle[0U].x, triangle[1U].x, triangle[2U].x});
            const double maximum_u = std::max({
                triangle[0U].x, triangle[1U].x, triangle[2U].x});
            const double minimum_v = std::min({
                triangle[0U].y, triangle[1U].y, triangle[2U].y});
            const double maximum_v = std::max({
                triangle[0U].y, triangle[1U].y, triangle[2U].y});
            if (maximum_u <= tile_left || minimum_u >= tile_right ||
                maximum_v <= tile_bottom || minimum_v >= tile_top) {
                continue;
            }
            if (std::abs(edge_value(triangle[0U], triangle[1U], triangle[2U])) <=
                kGeometryEpsilon) {
                return core::Result<assets::Sha256Digest>::failure(validation(
                    "bake tile dependency contains a degenerate target UV triangle"));
            }
            const bool all_in_tile = std::all_of(triangle.begin(), triangle.end(),
                [tile_left, tile_right, tile_bottom, tile_top](core::Vec2d uv) {
                    return uv.x >= tile_left && uv.x <= tile_right &&
                        uv.y >= tile_bottom && uv.y <= tile_top;
                });
            if (!all_in_tile) {
                return core::Result<assets::Sha256Digest>::failure(unsupported(
                    "bake tile dependency cannot bind a target triangle that crosses UDIM boundaries"));
            }
            face_uses_tile = true;
        }
        if (!face_uses_tile) continue;

        append_u64(canonical, face.id.value);
        append_u64(canonical, static_cast<std::uint64_t>(face.vertices.size()));
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const geometry::VertexId vertex_id = face.vertices[index];
            const geometry::Vertex* vertex = mesh.find_vertex(vertex_id);
            if (vertex == nullptr || !vertex->position.finite()) {
                return core::Result<assets::Sha256Digest>::failure(validation(
                    "bake tile target face references an invalid vertex"));
            }
            append_u64(canonical, vertex_id.value);
            append_double(canonical, vertex->position.x);
            append_double(canonical, vertex->position.y);
            append_double(canonical, vertex->position.z);
            append_double(canonical, face_uvs[index].x);
            append_double(canonical, face_uvs[index].y);
        }
        ++included_faces;
    }
    if (included_faces == 0U) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "bake tile dependency has no target faces"));
    }
    for (std::uint32_t byte = 0U; byte < 64U; byte += 8U) {
        canonical[face_count_offset + byte / 8U] = static_cast<std::uint8_t>(
            (included_faces >> byte) & 0xffU);
    }
    return core::Result<assets::Sha256Digest>::success(hash_bytes(canonical));
}

core::Result<BakeTileDependencyKey> make_bake_tile_dependency_key(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    BakeNormalSpace normal_space) {
    if (auto result = validate_bake_sources(recipe, source, target); !result) {
        return core::Result<BakeTileDependencyKey>::failure(result.error());
    }
    const auto source_geometry = bake_mesh_source_digest(source);
    const auto target_geometry = bake_mesh_source_digest(target);
    const auto target_tile = bake_mesh_target_tile_digest(
        target, recipe.target_uv_set, recipe.tile_u, recipe.tile_v);
    const auto settings = normal_map_settings_digest(recipe, normal_space);
    if (!source_geometry || !target_geometry || !target_tile || !settings) {
        return core::Result<BakeTileDependencyKey>::failure(
            !source_geometry ? source_geometry.error() :
            !target_geometry ? target_geometry.error() :
            !target_tile ? target_tile.error() : settings.error());
    }

    BakeTileDependencyKey key;
    key.source_identity = recipe.source.identity;
    key.source_revision = recipe.source.revision;
    key.target_identity = recipe.target.identity;
    key.target_revision = recipe.target.revision;
    key.target_uv_set = recipe.target_uv_set;
    key.tile_u = recipe.tile_u;
    key.tile_v = recipe.tile_v;
    key.source_geometry_digest = source_geometry.value();
    key.target_geometry_digest = target_geometry.value();
    key.target_tile_digest = target_tile.value();
    key.settings_digest = settings.value();
    key.dependency_digest = hash_bytes(canonical_bake_tile_dependency_bytes(key));
    return core::Result<BakeTileDependencyKey>::success(std::move(key));
}

core::Result<BakeTileArtifactCacheEntry> make_bake_tile_cache_entry(
    const NormalMapArtifact& artifact,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    if (auto result = artifact.validate(); !result) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(result.error());
    }
    if (auto result = validate_bake_sources(artifact.recipe, source, target); !result) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(result.error());
    }
    const auto dependencies = make_bake_tile_dependency_key(
        artifact.recipe, source, target, artifact.normal_space);
    if (!dependencies) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(dependencies.error());
    }
    BakeTileArtifactCacheEntry entry;
    entry.dependencies = dependencies.value();
    entry.artifact = artifact;
    if (auto result = entry.refresh_digest(); !result) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(result.error());
    }
    return core::Result<BakeTileArtifactCacheEntry>::success(std::move(entry));
}

core::Result<NormalMapArtifact> reuse_bake_tile_artifact(
    const BakeTileArtifactCacheEntry& entry,
    const BakeRecipe& current_recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target) {
    if (auto result = entry.validate(); !result) {
        return core::Result<NormalMapArtifact>::failure(result.error());
    }
    const auto current_dependencies = make_bake_tile_dependency_key(
        current_recipe, current_source, current_target, entry.artifact.normal_space);
    if (!current_dependencies) {
        return core::Result<NormalMapArtifact>::failure(current_dependencies.error());
    }
    if (!entry.dependencies.same_dependencies(current_dependencies.value())) {
        return core::Result<NormalMapArtifact>::failure(stale(
            "cached bake tile dependencies do not match the current source and target"));
    }
    NormalMapArtifact rebound = entry.artifact;
    rebound.recipe = current_recipe;
    if (auto result = rebound.refresh_digests(); !result) {
        return core::Result<NormalMapArtifact>::failure(result.error());
    }
    if (auto result = rebound.validate(); !result) {
        return core::Result<NormalMapArtifact>::failure(result.error());
    }
    return core::Result<NormalMapArtifact>::success(std::move(rebound));
}

core::Result<std::vector<BakeTileCoordinate>> enumerate_bake_tiles(
    const geometry::EditableMesh& target,
    attributes::UvSetId target_uv_set) {
    if (target_uv_set == 0U) {
        return core::Result<std::vector<BakeTileCoordinate>>::failure(invalid(
            "bake tile schedule requires a stable target UV-set id"));
    }
    const auto triangles = collect_triangles(target, target_uv_set);
    if (!triangles) {
        return core::Result<std::vector<BakeTileCoordinate>>::failure(
            triangles.error());
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> tiles;
    std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> face_tiles;
    for (const Triangle& triangle : triangles.value()) {
        const double center_u = (triangle.uvs[0U].x + triangle.uvs[1U].x +
            triangle.uvs[2U].x) / 3.0;
        const double center_v = (triangle.uvs[0U].y + triangle.uvs[1U].y +
            triangle.uvs[2U].y) / 3.0;
        if (!std::isfinite(center_u) || !std::isfinite(center_v) ||
            center_u < 0.0 || center_v < 0.0 || center_u >= 10.0 ||
            center_v >= 100.0) {
            return core::Result<std::vector<BakeTileCoordinate>>::failure(
                validation("bake target face maps outside the supported UDIM range"));
        }
        const auto tile_u = static_cast<std::uint32_t>(std::floor(center_u));
        const auto tile_v = static_cast<std::uint32_t>(std::floor(center_v));
        const double left = static_cast<double>(tile_u);
        const double bottom = static_cast<double>(tile_v);
        const double right = left + 1.0;
        const double top = bottom + 1.0;
        for (const core::Vec2d uv : triangle.uvs) {
            if (uv.x < left || uv.x > right || uv.y < bottom || uv.y > top) {
                return core::Result<std::vector<BakeTileCoordinate>>::failure(
                    unsupported("target face " + std::to_string(triangle.face.value) +
                        " crosses a UDIM boundary and cannot be scheduled independently"));
            }
        }

        const std::pair<std::uint32_t, std::uint32_t> tile{tile_u, tile_v};
        const auto [face_entry, inserted] = face_tiles.emplace(
            triangle.face.value, tile);
        if (!inserted && face_entry->second != tile) {
            return core::Result<std::vector<BakeTileCoordinate>>::failure(
                unsupported("target face " + std::to_string(triangle.face.value) +
                    " spans multiple UDIM tiles"));
        }
        tiles.insert(tile);
        if (tiles.size() > kMaxBakeTileScheduleTiles) {
            return core::Result<std::vector<BakeTileCoordinate>>::failure(
                unsupported("bake tile schedule exceeds its bounded tile count"));
        }
    }
    if (tiles.empty()) {
        return core::Result<std::vector<BakeTileCoordinate>>::failure(validation(
            "bake tile schedule contains no covered target faces"));
    }

    std::vector<BakeTileCoordinate> result;
    result.reserve(tiles.size());
    for (const auto& [tile_u, tile_v] : tiles) {
        result.push_back(BakeTileCoordinate{tile_u, tile_v});
    }
    return core::Result<std::vector<BakeTileCoordinate>>::success(std::move(result));
}

core::Result<BakeTileScheduleResult> schedule_normal_map_tiles_cpu(
    const BakeRecipe& base_recipe,
    BakeNormalSpace normal_space,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::span<const BakeTileArtifactCacheEntry> reusable_tiles) {
    if (auto result = base_recipe.validate(); !result) {
        return core::Result<BakeTileScheduleResult>::failure(result.error());
    }
    if (auto result = validate_bake_sources(base_recipe, source, target); !result) {
        return core::Result<BakeTileScheduleResult>::failure(result.error());
    }
    const auto tiles = enumerate_bake_tiles(target, base_recipe.target_uv_set);
    if (!tiles) {
        return core::Result<BakeTileScheduleResult>::failure(tiles.error());
    }

    BakeTileScheduleResult schedule;
    schedule.entries.reserve(tiles.value().size());
    for (const BakeTileCoordinate tile : tiles.value()) {
        BakeRecipe tile_recipe = base_recipe;
        tile_recipe.tile_u = tile.tile_u;
        tile_recipe.tile_v = tile.tile_v;
        tile_recipe.identity = base_recipe.identity + "/tile/" +
            std::to_string(tile.tile_u) + "/" + std::to_string(tile.tile_v);
        if (auto result = tile_recipe.validate(); !result) {
            return core::Result<BakeTileScheduleResult>::failure(
                result.error().with_context("UDIM tile " +
                    std::to_string(tile.tile_u) + "," +
                    std::to_string(tile.tile_v)));
        }
        const auto dependencies = make_bake_tile_dependency_key(
            tile_recipe, source, target, normal_space);
        if (!dependencies) {
            return core::Result<BakeTileScheduleResult>::failure(
                dependencies.error().with_context("UDIM tile " +
                    std::to_string(tile.tile_u) + "," +
                    std::to_string(tile.tile_v)));
        }

        const BakeTileArtifactCacheEntry* cached = nullptr;
        for (const BakeTileArtifactCacheEntry& candidate : reusable_tiles) {
            if (!candidate.dependencies.same_dependencies(dependencies.value())) continue;
            if (cached != nullptr) {
                return core::Result<BakeTileScheduleResult>::failure(invalid(
                    "multiple cache entries match one scheduled bake tile"));
            }
            cached = &candidate;
        }
        if (cached != nullptr) {
            const auto reused = reuse_bake_tile_artifact(
                *cached, tile_recipe, source, target);
            if (!reused) {
                return core::Result<BakeTileScheduleResult>::failure(
                    reused.error().with_context("UDIM tile " +
                        std::to_string(tile.tile_u) + "," +
                        std::to_string(tile.tile_v)));
            }
            schedule.entries.push_back(BakeTileScheduleEntry{
                tile, reused.value(), true});
            ++schedule.reused_tiles;
            continue;
        }

        const BakeExecutionReport report = normal_space == BakeNormalSpace::tangent
            ? compile_tangent_normal_map_cpu(tile_recipe, source, target)
            : compile_object_space_normal_map_cpu(tile_recipe, source, target);
        if (!report.succeeded() || !report.artifact.has_value()) {
            const auto diagnostic = std::find_if(
                report.diagnostics.begin(), report.diagnostics.end(),
                [](const BakeDiagnostic& value) { return value.is_error; });
            const std::string detail = diagnostic == report.diagnostics.end()
                ? "CPU normal-map tile compilation failed without a diagnostic"
                : diagnostic->code + ": " + diagnostic->message;
            return core::Result<BakeTileScheduleResult>::failure(
                validation(detail).with_context("UDIM tile " +
                    std::to_string(tile.tile_u) + "," +
                    std::to_string(tile.tile_v)));
        }
        schedule.entries.push_back(BakeTileScheduleEntry{
            tile, *report.artifact, false});
        ++schedule.compiled_tiles;
    }
    return core::Result<BakeTileScheduleResult>::success(std::move(schedule));
}

core::Result<BakeMeshBinding> bind_bake_source(
    std::string identity,
    const geometry::EditableMesh& mesh) {
    if (!safe_identity(identity)) {
        return core::Result<BakeMeshBinding>::failure(invalid("bake source identity is invalid"));
    }
    const auto digest = bake_mesh_source_digest(mesh);
    if (!digest) return core::Result<BakeMeshBinding>::failure(digest.error());
    BakeMeshBinding binding{std::move(identity), mesh.revision(), digest.value()};
    if (auto result = binding.validate(); !result) {
        return core::Result<BakeMeshBinding>::failure(result.error());
    }
    return core::Result<BakeMeshBinding>::success(std::move(binding));
}

core::Result<BakeMeshBinding> bind_bake_target(
    std::string identity,
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set) {
    if (!safe_identity(identity)) {
        return core::Result<BakeMeshBinding>::failure(invalid("bake target identity is invalid"));
    }
    const auto digest = bake_mesh_target_digest(mesh, uv_set);
    if (!digest) return core::Result<BakeMeshBinding>::failure(digest.error());
    BakeMeshBinding binding{std::move(identity), mesh.revision(), digest.value()};
    if (auto result = binding.validate(); !result) {
        return core::Result<BakeMeshBinding>::failure(result.error());
    }
    return core::Result<BakeMeshBinding>::success(std::move(binding));
}

core::Result<void> validate_bake_sources(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    if (auto result = recipe.validate(); !result) return result;
    if (source.revision() != recipe.source.revision ||
        target.revision() != recipe.target.revision) {
        return core::Result<void>::failure(stale("bake source or target revision is stale"));
    }
    const auto source_digest = bake_mesh_source_digest(source);
    const auto target_digest = bake_mesh_target_digest(target, recipe.target_uv_set);
    if (!source_digest || !target_digest) {
        return core::Result<void>::failure(
            !source_digest ? source_digest.error() : target_digest.error());
    }
    if (source_digest.value() != recipe.source.content_digest ||
        target_digest.value() != recipe.target.content_digest) {
        return core::Result<void>::failure(stale("bake source or target content digest is stale"));
    }
    return core::Result<void>::success();
}

core::Result<ProductionCookKey> bake_production_cook_key(
    const BakeRecipe& recipe,
    std::string platform_profile) {
    return bake_production_cook_key(
        recipe, BakeNormalSpace::tangent, std::move(platform_profile));
}

core::Result<ProductionCookKey> bake_production_cook_key(
    const BakeRecipe& recipe,
    BakeNormalSpace normal_space,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const std::string_view algorithm = normal_algorithm(normal_space);
    if (algorithm.empty()) {
        return core::Result<ProductionCookKey>::failure(
            invalid("bake normal space is not supported"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) {
        return core::Result<ProductionCookKey>::failure(recipe_digest.error());
    }
    ProductionCookKey key;
    key.source = AuthoritativeSource{
        recipe.source.identity, recipe.source.revision, recipe.source.content_digest};
    key.kind = ArtifactKind::baked_texture;
    key.form = ExecutionForm::offline;
    key.algorithm = std::string(algorithm);
    key.settings_digest = normal_space == BakeNormalSpace::tangent
        ? recipe_digest.value()
        : hash_text(std::string(algorithm) + "\n" + recipe_digest.value().hex());
    key.platform_profile = std::move(platform_profile);
    key.representation_kind = normal_space == BakeNormalSpace::tangent
        ? "tangent-normal-rgba8-v1" : "object-normal-rgba8-v1";
    key.spatial_scope = "uv-set:" + std::to_string(recipe.target_uv_set) +
        ";target:" + recipe.target.identity + ";revision:" +
        std::to_string(recipe.target.revision.value()) + ";digest:" +
        recipe.target.content_digest.hex();
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> bake_padding_cook_key(
    const BakePaddingRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto settings = recipe.canonical_digest();
    if (!settings) return core::Result<ProductionCookKey>::failure(settings.error());
    ProductionCookKey key;
    key.source = AuthoritativeSource{
        recipe.binding.source.identity,
        recipe.binding.source.revision,
        recipe.binding.source.content_digest};
    key.kind = ArtifactKind::baked_texture;
    key.form = ExecutionForm::offline;
    key.algorithm = std::string(kBakePaddingAlgorithm);
    key.settings_digest = settings.value();
    key.platform_profile = std::move(platform_profile);
    switch (recipe.input_kind) {
    case BakePaddingInputKind::normal_map:
        key.representation_kind = "normal-map-rgba8-uv-island-padding-v1";
        break;
    case BakePaddingInputKind::ambient_occlusion:
        key.representation_kind = "ambient-occlusion-rgba8-uv-island-padding-v2";
        break;
    case BakePaddingInputKind::bent_normal:
        key.representation_kind = "bent-normal-rgba8-uv-island-padding-v2";
        break;
    case BakePaddingInputKind::curvature:
        key.representation_kind = "curvature-rgba8-uv-island-padding-v2";
        break;
    case BakePaddingInputKind::thickness:
        key.representation_kind = "thickness-rgba8-uv-island-padding-v2";
        break;
    case BakePaddingInputKind::height_displacement:
        key.representation_kind = "height-displacement-rgba8-uv-island-padding-v2";
        break;
    }
    key.spatial_scope = "uv-set:" + std::to_string(recipe.binding.target_uv_set) +
        ";tile:" + std::to_string(recipe.binding.tile_u) + "," +
        std::to_string(recipe.binding.tile_v) + ";target:" +
        recipe.binding.target.identity + ";revision:" +
        std::to_string(recipe.binding.target.revision.value()) + ";digest:" +
        recipe.binding.target.content_digest.hex();
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_bake_padding_graph_node(
    ProductionCookGraph& graph,
    const BakePaddingRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_padding_cook_key(recipe, platform_profile);
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (recipe.input_kind == BakePaddingInputKind::normal_map) {
        const auto input_key = bake_production_cook_key(
            recipe.binding, recipe.input_normal_space, std::move(platform_profile));
        if (!input_key) return core::Result<ProductionCookKey>::failure(input_key.error());
        if (std::find(dependencies.begin(), dependencies.end(), input_key.value()) ==
            dependencies.end()) {
            dependencies.push_back(input_key.value());
        }
    } else {
        const auto input_scope_key = bake_production_cook_key(
            recipe.binding, platform_profile);
        if (!input_scope_key) {
            return core::Result<ProductionCookKey>::failure(input_scope_key.error());
        }
        const bool has_typed_bake_dependency = std::any_of(
            dependencies.begin(), dependencies.end(), [&key, &input_scope_key](
                const ProductionCookKey& dependency) {
                return dependency.kind == ArtifactKind::baked_texture &&
                    dependency.source == key.value().source &&
                    dependency.spatial_scope == input_scope_key.value().spatial_scope;
            });
        if (!has_typed_bake_dependency) {
            return core::Result<ProductionCookKey>::failure(validation(
                "typed bake padding requires an explicit same-source, same-scope baked-texture dependency"));
        }
    }
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<ProductionCookKey> register_bake_graph_node(
    ProductionCookGraph& graph,
    const BakeRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_production_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<ProductionCookKey> register_bake_graph_node(
    ProductionCookGraph& graph,
    const BakeRecipe& recipe,
    BakeNormalSpace normal_space,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_production_cook_key(
        recipe, normal_space, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<void> publish_normal_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const NormalMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = validate_bake_sources(
            artifact.recipe, current_source, current_target); !result) {
        return result;
    }
    const auto expected_key = bake_production_cook_key(
        artifact.recipe, artifact.normal_space, std::move(platform_profile));
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "normal-map receipt does not match the current bake recipe"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const AuthoritativeSource current_authoritative_source{
        artifact.recipe.source.identity,
        artifact.recipe.source.revision,
        artifact.recipe.source.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_authoritative_source, policy);
}

core::Result<NormalMapArtifact> read_normal_map(
    const ProductionArtifactStore& store,
    const BakeRecipe& recipe,
    BakeNormalSpace normal_space,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = validate_bake_sources(recipe, current_source, current_target); !result) {
        return core::Result<NormalMapArtifact>::failure(result.error());
    }
    const auto key = bake_production_cook_key(
        recipe, normal_space, std::move(platform_profile));
    if (!key) return core::Result<NormalMapArtifact>::failure(key.error());
    const AuthoritativeSource current_authoritative_source{
        recipe.source.identity, recipe.source.revision, recipe.source.content_digest};
    const auto payload = store.read(key.value(), current_authoritative_source, policy);
    if (!payload) return core::Result<NormalMapArtifact>::failure(payload.error());
    auto artifact = NormalMapArtifact::deserialize(payload.value());
    if (!artifact) return artifact;
    if (artifact.value().recipe != recipe || artifact.value().normal_space != normal_space) {
        return core::Result<NormalMapArtifact>::failure(validation(
            "stored normal-map artifact does not match the requested bake recipe"));
    }
    return artifact;
}

core::Result<void> publish_bake_padding(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BakePaddingArtifact& artifact,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    return publish_bake_padding(store, std::move(receipt), artifact,
        BakePaddingInputRef{std::cref(input)}, current_source, current_target,
        policy, std::move(platform_profile));
}

core::Result<void> publish_bake_padding(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BakePaddingArtifact& artifact,
    BakePaddingInputRef input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = validate_bake_padding_sources(
            artifact, std::move(input), current_source, current_target); !result) {
        return result;
    }
    const auto expected_key = bake_padding_cook_key(
        artifact.recipe, std::move(platform_profile));
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "bake padding receipt does not match the current recipe"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const AuthoritativeSource current_authoritative_source{
        artifact.recipe.binding.source.identity,
        artifact.recipe.binding.source.revision,
        artifact.recipe.binding.source.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_authoritative_source, policy);
}

core::Result<BakePaddingArtifact> read_bake_padding(
    const ProductionArtifactStore& store,
    const BakePaddingRecipe& recipe,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    return read_bake_padding(store, recipe, BakePaddingInputRef{std::cref(input)},
        current_source, current_target, policy, std::move(platform_profile));
}

core::Result<BakePaddingArtifact> read_bake_padding(
    const ProductionArtifactStore& store,
    const BakePaddingRecipe& recipe,
    BakePaddingInputRef input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<BakePaddingArtifact>::failure(result.error());
    }
    const auto view = bake_padding_input_view(input);
    if (!view) return core::Result<BakePaddingArtifact>::failure(view.error());
    if (view.value().binding != recipe.binding ||
        view.value().normal_space != recipe.input_normal_space ||
        view.value().kind != recipe.input_kind ||
        view.value().artifact_digest != recipe.input_artifact_digest) {
        return core::Result<BakePaddingArtifact>::failure(stale(
            "bake padding read input does not match its recipe"));
    }
    if (auto result = validate_bake_sources(recipe.binding, current_source, current_target); !result) {
        return core::Result<BakePaddingArtifact>::failure(result.error());
    }
    const auto key = bake_padding_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<BakePaddingArtifact>::failure(key.error());
    const AuthoritativeSource current_authoritative_source{
        recipe.binding.source.identity, recipe.binding.source.revision,
        recipe.binding.source.content_digest};
    const auto payload = store.read(key.value(), current_authoritative_source, policy);
    if (!payload) return core::Result<BakePaddingArtifact>::failure(payload.error());
    auto artifact = BakePaddingArtifact::deserialize(payload.value());
    if (!artifact) return artifact;
    if (artifact.value().recipe != recipe) {
        return core::Result<BakePaddingArtifact>::failure(validation(
            "stored bake padding artifact does not match the requested recipe"));
    }
    if (auto result = validate_bake_padding_sources(
            artifact.value(), input, current_source, current_target); !result) {
        return core::Result<BakePaddingArtifact>::failure(result.error());
    }
    return artifact;
}

core::Result<void> NormalMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    const std::string_view expected_algorithm = normal_algorithm(normal_space);
    if (expected_algorithm.empty() || algorithm != expected_algorithm) {
        return core::Result<void>::failure(invalid("bake artifact algorithm is unsupported"));
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(recipe.width) * recipe.height * 4ULL;
    if (rgba8.size() != expected_bytes || covered_texels >
            static_cast<std::uint64_t>(recipe.width) * recipe.height ||
        projected_texels + projection_misses != covered_texels ||
        projected_texels > covered_texels ||
        projection_miss_samples.size() > kMaxProjectionMissSamples ||
        projection_miss_samples.size() > projection_misses ||
        (projection_misses == 0U &&
            (!projection_miss_samples.empty() || projection_miss_samples_truncated)) ||
        (projection_misses != 0U &&
            (recipe.reject_projection_misses || projection_miss_samples.empty() ||
                projection_miss_samples_truncated !=
                    (projection_miss_samples.size() < projection_misses)))) {
        return core::Result<void>::failure(
            validation("bake artifact dimensions or coverage accounting are inconsistent"));
    }
    const auto settings = normal_map_settings_digest(recipe, normal_space);
    if (!settings) return core::Result<void>::failure(settings.error());
    settings_digest = settings.value();
    output_digest = hash_bytes(rgba8);
    artifact_digest = hash_bytes(canonical_artifact_bytes(*this));
    return core::Result<void>::success();
}

core::Result<void> NormalMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::string_view expected_algorithm = normal_algorithm(normal_space);
    if (expected_algorithm.empty() || algorithm != expected_algorithm ||
        settings_digest.is_zero() ||
        output_digest.is_zero() || artifact_digest.is_zero()) {
        return core::Result<void>::failure(validation("bake artifact identity or digests are invalid"));
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(recipe.width) * recipe.height * 4ULL;
    if (rgba8.size() != expected_bytes || covered_texels == 0U ||
        covered_texels > static_cast<std::uint64_t>(recipe.width) * recipe.height ||
        projected_texels + projection_misses != covered_texels ||
        projected_texels > covered_texels ||
        projection_miss_samples.size() > kMaxProjectionMissSamples ||
        projection_miss_samples.size() > projection_misses ||
        (projection_misses == 0U &&
            (!projection_miss_samples.empty() || projection_miss_samples_truncated)) ||
        (projection_misses != 0U &&
            (recipe.reject_projection_misses || projection_miss_samples.empty() ||
                projection_miss_samples_truncated !=
                    (projection_miss_samples.size() < projection_misses)))) {
        return core::Result<void>::failure(
            validation("bake artifact dimensions or coverage accounting are invalid"));
    }
    std::uint64_t opaque_texels = 0U;
    for (std::size_t pixel = 0U; pixel < rgba8.size(); pixel += 4U) {
        const std::uint8_t alpha = rgba8[pixel + 3U];
        if (alpha == 255U) {
            ++opaque_texels;
        } else if (alpha != 0U || rgba8[pixel] != 128U ||
            rgba8[pixel + 1U] != 128U || rgba8[pixel + 2U] != 255U) {
            return core::Result<void>::failure(
                validation("bake artifact coverage alpha or uncovered texel value is invalid"));
        }
    }
    if (opaque_texels != projected_texels) {
        return core::Result<void>::failure(
            validation("bake artifact alpha coverage disagrees with projected texel count"));
    }
    for (std::size_t index = 0U; index < projection_miss_samples.size(); ++index) {
        const BakeProjectionMissSample& sample = projection_miss_samples[index];
        if (!sample.target_face || sample.texel_x >= recipe.width ||
            sample.texel_y >= recipe.height ||
            std::find(projection_miss_samples.begin(), projection_miss_samples.begin() +
                    static_cast<std::ptrdiff_t>(index), sample) !=
                projection_miss_samples.begin() + static_cast<std::ptrdiff_t>(index)) {
            return core::Result<void>::failure(
                validation("bake artifact projection miss sample is invalid or duplicated"));
        }
    }
    const auto settings = normal_map_settings_digest(recipe, normal_space);
    if (!settings || settings.value() != settings_digest ||
        hash_bytes(rgba8) != output_digest ||
        hash_bytes(canonical_artifact_bytes(*this)) != artifact_digest) {
        return core::Result<void>::failure(validation("bake artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> NormalMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_artifact_bytes(*this);
    append_digest(bytes, artifact_digest);
    if (bytes.size() > kMaxArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            validation("bake artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<NormalMapArtifact> NormalMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxArtifactBytes) {
        return core::Result<NormalMapArtifact>::failure(
            invalid("bake artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string recipe_text;
    std::string algorithm_value;
    std::uint32_t encoded_normal_space = 0U;
    if (!reader.read_string(magic, kArtifactMagic.size()) || magic != kArtifactMagic ||
        !reader.read_u32(version) || version != kBakeArtifactVersion ||
        !reader.read_string(recipe_text, kMaxBakeRecipeBytes) ||
        !reader.read_u32(encoded_normal_space) ||
        encoded_normal_space > static_cast<std::uint32_t>(BakeNormalSpace::object) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<NormalMapArtifact>::failure(
            validation("bake artifact header or recipe is malformed"));
    }
    const auto recipe = BakeRecipe::deserialize(recipe_text);
    if (!recipe) return core::Result<NormalMapArtifact>::failure(recipe.error());
    NormalMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.normal_space = static_cast<BakeNormalSpace>(encoded_normal_space);
    artifact.algorithm = std::move(algorithm_value);
    std::uint32_t sample_count = 0U;
    std::uint32_t samples_truncated = 0U;
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_u64(artifact.projected_texels) ||
        !reader.read_u64(artifact.projection_misses) ||
        !reader.read_u32(sample_count) || sample_count > kMaxProjectionMissSamples ||
        !reader.read_u32(samples_truncated) || samples_truncated > 1U) {
        return core::Result<NormalMapArtifact>::failure(
            validation("bake artifact receipt metadata is malformed"));
    }
    artifact.projection_miss_samples_truncated = samples_truncated == 1U;
    artifact.projection_miss_samples.reserve(sample_count);
    for (std::uint32_t index = 0U; index < sample_count; ++index) {
        std::uint64_t target_face = 0U;
        std::uint32_t texel_x = 0U;
        std::uint32_t texel_y = 0U;
        if (!reader.read_u64(target_face) || !reader.read_u32(texel_x) ||
            !reader.read_u32(texel_y)) {
            return core::Result<NormalMapArtifact>::failure(
                validation("bake artifact projection miss sample is truncated"));
        }
        artifact.projection_miss_samples.push_back(
            BakeProjectionMissSample{geometry::FaceId{target_face}, texel_x, texel_y});
    }
    if (!reader.read_payload(artifact.rgba8,
            static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<NormalMapArtifact>::failure(
            validation("bake artifact payload is truncated, oversized, or has trailing bytes"));
    }
    if (auto result = artifact.validate(); !result) {
        return core::Result<NormalMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<NormalMapArtifact>::failure(
            validation("bake artifact serialization is not canonical"));
    }
    return core::Result<NormalMapArtifact>::success(std::move(artifact));
}

namespace {

struct BakePaddingEdgeUse {
    geometry::FaceId face;
    core::Vec2d first_uv;
    core::Vec2d second_uv;
};

core::Result<std::map<geometry::FaceId, std::uint32_t>> bake_uv_island_labels(
    const geometry::EditableMesh& mesh,
    attributes::UvSetId uv_set) {
    if (mesh.face_count() == 0U || mesh.face_count() > kMaxBakePaddingFaces) {
        return core::Result<std::map<geometry::FaceId, std::uint32_t>>::failure(
            unsupported("bake padding target face count is outside its supported bound"));
    }
    const auto topology = mesh.topology();
    const attributes::UvSetDescriptor* descriptor = mesh.uv_sets().find(uv_set);
    const attributes::AttributeLayer* layer = descriptor == nullptr
        ? nullptr : mesh.attributes().find(descriptor->layer_id);
    if (!topology || descriptor == nullptr || layer == nullptr ||
        layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2 ||
        layer->values.size() != topology.value().corners.size()) {
        return core::Result<std::map<geometry::FaceId, std::uint32_t>>::failure(
            validation("bake padding requires a complete corner-domain UV set"));
    }

    std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec2d> uv_by_face_vertex;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto* uv = std::get_if<core::Vec2d>(&layer->values[index]);
        const geometry::CornerRecord& corner = topology.value().corners[index];
        if (uv == nullptr || !uv->finite() ||
            !uv_by_face_vertex.emplace(std::make_pair(corner.face, corner.vertex), *uv).second) {
            return core::Result<std::map<geometry::FaceId, std::uint32_t>>::failure(
                validation("bake padding target has invalid or ambiguous corner UV data"));
        }
    }

    const std::vector<geometry::Face> faces = mesh.faces_sorted();
    std::map<geometry::FaceId, geometry::FaceId> parent;
    std::map<std::pair<geometry::VertexId, geometry::VertexId>,
        std::vector<BakePaddingEdgeUse>> uses_by_edge;
    for (const geometry::Face& face : faces) {
        if (face.vertices.size() < 3U || !parent.emplace(face.id, face.id).second) {
            return core::Result<std::map<geometry::FaceId, std::uint32_t>>::failure(
                validation("bake padding target contains an invalid face identity"));
        }
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const geometry::VertexId from = face.vertices[index];
            const geometry::VertexId to = face.vertices[(index + 1U) % face.vertices.size()];
            const auto from_uv = uv_by_face_vertex.find({face.id, from});
            const auto to_uv = uv_by_face_vertex.find({face.id, to});
            if (from_uv == uv_by_face_vertex.end() || to_uv == uv_by_face_vertex.end()) {
                return core::Result<std::map<geometry::FaceId, std::uint32_t>>::failure(
                    validation("bake padding target face is missing a corner UV"));
            }
            const bool forward = from < to;
            const auto key = forward ? std::make_pair(from, to) : std::make_pair(to, from);
            uses_by_edge[key].push_back(BakePaddingEdgeUse{
                face.id, forward ? from_uv->second : to_uv->second,
                forward ? to_uv->second : from_uv->second});
        }
    }

    auto find_root = [&parent](auto&& self, geometry::FaceId face) -> geometry::FaceId {
        geometry::FaceId& value = parent.at(face);
        if (value != face) value = self(self, value);
        return value;
    };
    for (const auto& [edge, uses] : uses_by_edge) {
        static_cast<void>(edge);
        if (uses.size() > 2U) {
            return core::Result<std::map<geometry::FaceId, std::uint32_t>>::failure(
                unsupported("bake padding rejects non-manifold UV edge fans"));
        }
        if (uses.size() != 2U) continue;
        const BakePaddingEdgeUse& left_use = uses[0U];
        const BakePaddingEdgeUse& right_use = uses[1U];
        if (left_use.first_uv.x != right_use.first_uv.x ||
            left_use.first_uv.y != right_use.first_uv.y ||
            left_use.second_uv.x != right_use.second_uv.x ||
            left_use.second_uv.y != right_use.second_uv.y) {
            continue;
        }
        geometry::FaceId left = find_root(find_root, left_use.face);
        geometry::FaceId right = find_root(find_root, right_use.face);
        if (left != right) {
            if (right < left) std::swap(left, right);
            parent[right] = left;
        }
    }

    std::map<geometry::FaceId, std::uint32_t> label_by_root;
    std::map<geometry::FaceId, std::uint32_t> labels;
    for (const geometry::Face& face : faces) {
        const geometry::FaceId root = find_root(find_root, face.id);
        auto label = label_by_root.find(root);
        if (label == label_by_root.end()) {
            const std::uint32_t next = static_cast<std::uint32_t>(label_by_root.size() + 1U);
            label = label_by_root.emplace(root, next).first;
        }
        labels.emplace(face.id, label->second);
    }
    return core::Result<std::map<geometry::FaceId, std::uint32_t>>::success(
        std::move(labels));
}

core::Result<std::vector<std::uint32_t>> bake_padding_surface_labels(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& target) {
    const auto face_islands = bake_uv_island_labels(target, recipe.target_uv_set);
    if (!face_islands) {
        return core::Result<std::vector<std::uint32_t>>::failure(face_islands.error());
    }
    const auto triangles = collect_triangles(target, recipe.target_uv_set);
    if (!triangles) {
        return core::Result<std::vector<std::uint32_t>>::failure(triangles.error());
    }
    const std::size_t texel_count = static_cast<std::size_t>(recipe.width) * recipe.height;
    std::vector<std::uint32_t> labels(texel_count, 0U);
    std::uint64_t raster_candidates = 0U;
    const double tile_left = static_cast<double>(recipe.tile_u);
    const double tile_bottom = static_cast<double>(recipe.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;

    for (Triangle triangle : triangles.value()) {
        const double minimum_u = std::min({triangle.uvs[0U].x, triangle.uvs[1U].x,
            triangle.uvs[2U].x});
        const double maximum_u = std::max({triangle.uvs[0U].x, triangle.uvs[1U].x,
            triangle.uvs[2U].x});
        const double minimum_v = std::min({triangle.uvs[0U].y, triangle.uvs[1U].y,
            triangle.uvs[2U].y});
        const double maximum_v = std::max({triangle.uvs[0U].y, triangle.uvs[1U].y,
            triangle.uvs[2U].y});
        if (maximum_u <= tile_left || minimum_u >= tile_right ||
            maximum_v <= tile_bottom || minimum_v >= tile_top) {
            continue;
        }
        const bool all_in_tile = std::all_of(triangle.uvs.begin(), triangle.uvs.end(),
            [tile_left, tile_right, tile_bottom, tile_top](core::Vec2d uv) {
                return uv.x >= tile_left && uv.x <= tile_right &&
                    uv.y >= tile_bottom && uv.y <= tile_top;
            });
        if (!all_in_tile) {
            return core::Result<std::vector<std::uint32_t>>::failure(unsupported(
                "bake padding rejects target triangles that cross a UV tile boundary"));
        }
        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kGeometryEpsilon) {
            return core::Result<std::vector<std::uint32_t>>::failure(validation(
                "bake padding target contains a degenerate UV triangle"));
        }
        if (area < 0.0) {
            std::swap(triangle.uvs[1U], triangle.uvs[2U]);
            area = -area;
        }
        const auto face_label = face_islands.value().find(triangle.face);
        if (face_label == face_islands.value().end()) {
            return core::Result<std::vector<std::uint32_t>>::failure(validation(
                "bake padding triangle has no stable UV-island identity"));
        }
        const double local_min_u = std::clamp(minimum_u - tile_left, 0.0, 1.0);
        const double local_max_u = std::clamp(maximum_u - tile_left, 0.0, 1.0);
        const double local_min_v = std::clamp(minimum_v - tile_bottom, 0.0, 1.0);
        const double local_max_v = std::clamp(maximum_v - tile_bottom, 0.0, 1.0);
        const std::uint32_t min_x = pixel_floor(
            std::floor(local_min_u * recipe.width), recipe.width);
        const std::uint32_t max_x = pixel_floor(
            std::floor(local_max_u * recipe.width), recipe.width);
        const std::uint32_t min_y = pixel_floor(
            std::floor(local_min_v * recipe.height), recipe.height);
        const std::uint32_t max_y = pixel_floor(
            std::floor(local_max_v * recipe.height), recipe.height);
        const double edge_epsilon = std::max(1.0, area) * 1e-14;
        for (std::uint32_t y = min_y; y <= max_y; ++y) {
            for (std::uint32_t x = min_x; x <= max_x; ++x) {
                if (++raster_candidates > kMaxBakeRasterCandidates) {
                    return core::Result<std::vector<std::uint32_t>>::failure(unsupported(
                        "bake padding raster exceeded its deterministic work budget"));
                }
                const core::Vec2d sample{
                    tile_left + (static_cast<double>(x) + 0.5) / recipe.width,
                    tile_bottom + (static_cast<double>(y) + 0.5) / recipe.height};
                const double edge_zero = edge_value(
                    triangle.uvs[1U], triangle.uvs[2U], sample);
                const double edge_one = edge_value(
                    triangle.uvs[2U], triangle.uvs[0U], sample);
                const double edge_two = edge_value(
                    triangle.uvs[0U], triangle.uvs[1U], sample);
                const auto inside = [edge_epsilon](double edge, bool inclusive) {
                    return edge > edge_epsilon ||
                        (std::abs(edge) <= edge_epsilon && inclusive);
                };
                if (!inside(edge_zero, top_left_edge(triangle.uvs[1U], triangle.uvs[2U])) ||
                    !inside(edge_one, top_left_edge(triangle.uvs[2U], triangle.uvs[0U])) ||
                    !inside(edge_two, top_left_edge(triangle.uvs[0U], triangle.uvs[1U]))) {
                    continue;
                }
                const std::size_t texel = static_cast<std::size_t>(y) * recipe.width + x;
                if (labels[texel] != 0U) {
                    return core::Result<std::vector<std::uint32_t>>::failure(validation(
                        "bake padding target UV islands overlap at a covered texel"));
                }
                labels[texel] = face_label->second;
            }
        }
    }
    return core::Result<std::vector<std::uint32_t>>::success(std::move(labels));
}

BakePaddingInputView make_bake_padding_input_view(
    BakePaddingInputKind kind,
    const BakeRecipe& binding,
    BakeNormalSpace normal_space,
    const assets::Sha256Digest& artifact_digest,
    std::uint64_t covered_texels,
    const std::vector<std::uint8_t>& rgba8) {
    return BakePaddingInputView{
        kind, binding, normal_space, artifact_digest, covered_texels, rgba8};
}

core::Result<BakePaddingInputView> bake_padding_input_view(
    const NormalMapArtifact& input) {
    if (auto result = input.validate(); !result) {
        return core::Result<BakePaddingInputView>::failure(result.error());
    }
    return core::Result<BakePaddingInputView>::success(make_bake_padding_input_view(
        BakePaddingInputKind::normal_map, input.recipe, input.normal_space,
        input.artifact_digest, input.covered_texels, input.rgba8));
}

core::Result<BakePaddingInputView> bake_padding_input_view(
    const AmbientOcclusionMapArtifact& input) {
    if (auto result = input.validate(); !result) {
        return core::Result<BakePaddingInputView>::failure(result.error());
    }
    return core::Result<BakePaddingInputView>::success(make_bake_padding_input_view(
        BakePaddingInputKind::ambient_occlusion, input.recipe.binding,
        BakeNormalSpace::tangent, input.artifact_digest,
        input.covered_texels, input.rgba8));
}

core::Result<BakePaddingInputView> bake_padding_input_view(
    const BentNormalMapArtifact& input) {
    if (auto result = input.validate(); !result) {
        return core::Result<BakePaddingInputView>::failure(result.error());
    }
    return core::Result<BakePaddingInputView>::success(make_bake_padding_input_view(
        BakePaddingInputKind::bent_normal, input.recipe.binding,
        BakeNormalSpace::tangent, input.artifact_digest,
        input.covered_texels, input.rgba8));
}

core::Result<BakePaddingInputView> bake_padding_input_view(
    const CurvatureMapArtifact& input) {
    if (auto result = input.validate(); !result) {
        return core::Result<BakePaddingInputView>::failure(result.error());
    }
    return core::Result<BakePaddingInputView>::success(make_bake_padding_input_view(
        BakePaddingInputKind::curvature, input.recipe.binding,
        BakeNormalSpace::tangent, input.artifact_digest,
        input.covered_texels, input.rgba8));
}

core::Result<BakePaddingInputView> bake_padding_input_view(
    const ThicknessMapArtifact& input) {
    if (auto result = input.validate(); !result) {
        return core::Result<BakePaddingInputView>::failure(result.error());
    }
    return core::Result<BakePaddingInputView>::success(make_bake_padding_input_view(
        BakePaddingInputKind::thickness, input.recipe.binding,
        BakeNormalSpace::tangent, input.artifact_digest,
        input.covered_texels, input.rgba8));
}

core::Result<BakePaddingInputView> bake_padding_input_view(
    const HeightDisplacementMapArtifact& input) {
    if (auto result = input.validate(); !result) {
        return core::Result<BakePaddingInputView>::failure(result.error());
    }
    return core::Result<BakePaddingInputView>::success(make_bake_padding_input_view(
        BakePaddingInputKind::height_displacement, input.recipe.binding,
        BakeNormalSpace::tangent, input.artifact_digest,
        input.covered_texels, input.rgba8));
}

core::Result<BakePaddingInputView> bake_padding_input_view(
    BakePaddingInputRef input) {
    return std::visit([](const auto& wrapped) {
        return bake_padding_input_view(wrapped.get());
    }, input);
}

} // namespace

core::Result<void> BakePaddingRecipe::validate() const {
    if (auto result = binding.validate(); !result) return result;
    if ((input_normal_space != BakeNormalSpace::tangent &&
            input_normal_space != BakeNormalSpace::object) ||
        static_cast<std::uint32_t>(input_kind) >
            static_cast<std::uint32_t>(BakePaddingInputKind::height_displacement) ||
        input_artifact_digest.is_zero() || padding_texels == 0U ||
        padding_texels > kMaxBakePaddingTexels) {
        return core::Result<void>::failure(validation(
            "bake padding input identity or radius is outside its supported bounds"));
    }
    if (input_kind != BakePaddingInputKind::normal_map &&
        input_normal_space != BakeNormalSpace::tangent) {
        return core::Result<void>::failure(validation(
            "non-normal bake padding inputs must use the tangent default normal-space field"));
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> BakePaddingRecipe::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::vector<std::uint8_t> canonical;
    append_string(canonical, input_kind == BakePaddingInputKind::normal_map ?
        "CARTO_BAKE_PADDING_RECIPE 1" : "CARTO_BAKE_PADDING_RECIPE 2");
    append_string(canonical, binding.serialize());
    append_u32(canonical, static_cast<std::uint32_t>(input_normal_space));
    if (input_kind != BakePaddingInputKind::normal_map) {
        append_u32(canonical, static_cast<std::uint32_t>(input_kind));
    }
    append_digest(canonical, input_artifact_digest);
    append_u32(canonical, padding_texels);
    return core::Result<assets::Sha256Digest>::success(hash_bytes(canonical));
}

core::Result<void> BakePaddingArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != kBakePaddingAlgorithm || evidence_class != "carto.cpu-reference") {
        return core::Result<void>::failure(invalid(
            "bake padding algorithm or evidence class is unsupported"));
    }
    const std::uint64_t texels =
        static_cast<std::uint64_t>(recipe.binding.width) * recipe.binding.height;
    if (rgba8.size() != texels * 4ULL || padded_texels > texels) {
        return core::Result<void>::failure(validation(
            "bake padding output size or fill count is inconsistent"));
    }
    byte_size = static_cast<std::uint64_t>(rgba8.size());
    const auto settings = bake_padding_settings_digest(recipe);
    if (!settings) return core::Result<void>::failure(settings.error());
    settings_digest = settings.value();
    output_digest = hash_bytes(rgba8);
    artifact_digest = hash_bytes(canonical_bake_padding_artifact_bytes(*this));
    return core::Result<void>::success();
}

core::Result<void> BakePaddingArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != kBakePaddingAlgorithm || evidence_class != "carto.cpu-reference" ||
        settings_digest.is_zero() || output_digest.is_zero() || artifact_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "bake padding artifact identity or digests are invalid"));
    }
    const std::uint64_t texels =
        static_cast<std::uint64_t>(recipe.binding.width) * recipe.binding.height;
    if (rgba8.size() != texels * 4ULL || byte_size != rgba8.size() ||
        padded_texels > texels) {
        return core::Result<void>::failure(validation(
            "bake padding artifact dimensions or fill count are invalid"));
    }
    const auto settings = bake_padding_settings_digest(recipe);
    if (!settings || settings.value() != settings_digest ||
        hash_bytes(rgba8) != output_digest ||
        hash_bytes(canonical_bake_padding_artifact_bytes(*this)) != artifact_digest) {
        return core::Result<void>::failure(validation(
            "bake padding artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> BakePaddingArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_bake_padding_artifact_bytes(*this);
    append_digest(bytes, artifact_digest);
    if (bytes.size() > kMaxBakePaddingArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "bake padding artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<BakePaddingArtifact> BakePaddingArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxBakePaddingArtifactBytes) {
        return core::Result<BakePaddingArtifact>::failure(invalid(
            "bake padding artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string binding_text;
    std::uint32_t input_normal_space = 0U;
    std::uint32_t input_kind = static_cast<std::uint32_t>(BakePaddingInputKind::normal_map);
    std::uint32_t padding_texels = 0U;
    BakePaddingArtifact artifact;
    if (!reader.read_string(magic, kBakePaddingArtifactMagic.size()) ||
        magic != kBakePaddingArtifactMagic || !reader.read_u32(version) ||
        (version != kBakePaddingArtifactVersion &&
            version != kBakePaddingArtifactVersionTyped) ||
        !reader.read_string(binding_text, kMaxBakeRecipeBytes) ||
        !reader.read_u32(input_normal_space) ||
        input_normal_space > static_cast<std::uint32_t>(BakeNormalSpace::object)) {
        return core::Result<BakePaddingArtifact>::failure(validation(
            "bake padding artifact header or receipt metadata is malformed"));
    }
    if (version == kBakePaddingArtifactVersionTyped &&
        (!reader.read_u32(input_kind) ||
            input_kind > static_cast<std::uint32_t>(BakePaddingInputKind::height_displacement))) {
        return core::Result<BakePaddingArtifact>::failure(validation(
            "typed bake padding artifact input kind is malformed"));
    }
    if (!reader.read_digest(artifact.recipe.input_artifact_digest) ||
        !reader.read_u32(padding_texels) ||
        !reader.read_string(artifact.algorithm, 256U) ||
        !reader.read_string(artifact.evidence_class, 256U) ||
        !reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.padded_texels) ||
        !reader.read_u64(artifact.byte_size)) {
        return core::Result<BakePaddingArtifact>::failure(validation(
            "bake padding artifact header or receipt metadata is malformed"));
    }
    const auto binding = BakeRecipe::deserialize(binding_text);
    if (!binding) return core::Result<BakePaddingArtifact>::failure(binding.error());
    artifact.recipe.binding = binding.value();
    artifact.recipe.input_normal_space = static_cast<BakeNormalSpace>(input_normal_space);
    artifact.recipe.padding_texels = padding_texels;
    artifact.recipe.input_kind = static_cast<BakePaddingInputKind>(input_kind);
    if (!reader.read_payload(artifact.rgba8,
            static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<BakePaddingArtifact>::failure(validation(
            "bake padding artifact payload is truncated, oversized, or has trailing bytes"));
    }
    if (auto result = artifact.validate(); !result) {
        return core::Result<BakePaddingArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<BakePaddingArtifact>::failure(validation(
            "bake padding artifact serialization is not canonical"));
    }
    return core::Result<BakePaddingArtifact>::success(std::move(artifact));
}

core::Result<BakePaddingRecipe> make_bake_padding_recipe(
    const NormalMapArtifact& input,
    std::uint32_t padding_texels) {
    return make_bake_padding_recipe(BakePaddingInputRef{std::cref(input)}, padding_texels);
}

core::Result<BakePaddingRecipe> make_bake_padding_recipe(
    BakePaddingInputRef input,
    std::uint32_t padding_texels) {
    const auto view = bake_padding_input_view(std::move(input));
    if (!view) return core::Result<BakePaddingRecipe>::failure(view.error());
    BakePaddingRecipe recipe{
        view.value().binding, view.value().normal_space,
        view.value().artifact_digest, padding_texels, view.value().kind};
    if (auto result = recipe.validate(); !result) {
        return core::Result<BakePaddingRecipe>::failure(result.error());
    }
    return core::Result<BakePaddingRecipe>::success(std::move(recipe));
}

core::Result<BakePaddingArtifact> compile_bake_padding_cpu(
    const BakePaddingRecipe& recipe,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    return compile_bake_padding_cpu(recipe,
        BakePaddingInputRef{std::cref(input)}, source, target);
}

core::Result<BakePaddingArtifact> compile_bake_padding_cpu(
    const BakePaddingRecipe& recipe,
    BakePaddingInputRef input,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<BakePaddingArtifact>::failure(result.error());
    }
    const auto view = bake_padding_input_view(std::move(input));
    if (!view) return core::Result<BakePaddingArtifact>::failure(view.error());
    const BakePaddingInputView& input_view = view.value();
    if (input_view.binding != recipe.binding ||
        input_view.normal_space != recipe.input_normal_space ||
        input_view.kind != recipe.input_kind ||
        input_view.artifact_digest != recipe.input_artifact_digest) {
        return core::Result<BakePaddingArtifact>::failure(stale(
            "bake padding input artifact does not match its recipe"));
    }
    if (auto result = validate_bake_sources(recipe.binding, source, target); !result) {
        return core::Result<BakePaddingArtifact>::failure(result.error());
    }
    auto surface = bake_padding_surface_labels(recipe.binding, target);
    if (!surface) return core::Result<BakePaddingArtifact>::failure(surface.error());
    std::vector<std::uint32_t> surface_labels = std::move(surface).value();
    const std::size_t texels = surface_labels.size();
    const std::uint64_t surface_texels = static_cast<std::uint64_t>(std::count_if(
        surface_labels.begin(), surface_labels.end(),
        [](std::uint32_t label) { return label != 0U; }));
    if (surface_texels != input_view.covered_texels) {
        return core::Result<BakePaddingArtifact>::failure(stale(
            "bake padding UV coverage differs from the input artifact"));
    }

    constexpr std::uint8_t kUnreached = std::numeric_limits<std::uint8_t>::max();
    std::vector<std::uint8_t> distance(texels, kUnreached);
    std::vector<std::uint32_t> queue;
    std::vector<std::uint8_t> padded_pixels(
        input_view.rgba8.begin(), input_view.rgba8.end());
    queue.reserve(texels);
    for (std::size_t texel = 0U; texel < texels; ++texel) {
        const std::uint8_t alpha = input_view.rgba8[texel * 4U + 3U];
        if (surface_labels[texel] != 0U) {
            distance[texel] = 0U;
            if (alpha == 255U) {
                queue.push_back(static_cast<std::uint32_t>(texel));
            } else if (alpha != 0U) {
                return core::Result<BakePaddingArtifact>::failure(validation(
                    "bake padding input contains an invalid coverage alpha"));
            }
        } else if (alpha != 0U) {
            return core::Result<BakePaddingArtifact>::failure(validation(
                "bake padding input marks an uncovered UV texel as projected"));
        }
    }
    if (queue.empty()) {
        return core::Result<BakePaddingArtifact>::failure(validation(
            "bake padding requires at least one projected texel"));
    }

    std::uint64_t padded_texel_count = 0U;
    std::size_t queue_read = 0U;
    const std::int64_t width = static_cast<std::int64_t>(recipe.binding.width);
    const std::int64_t height = static_cast<std::int64_t>(recipe.binding.height);
    while (queue_read < queue.size()) {
        const std::uint32_t current = queue[queue_read++];
        const std::uint8_t current_distance = distance[current];
        if (current_distance >= recipe.padding_texels) continue;
        const std::int64_t x = static_cast<std::int64_t>(current % recipe.binding.width);
        const std::int64_t y = static_cast<std::int64_t>(current / recipe.binding.width);
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const std::int64_t next_x = x + dx;
                const std::int64_t next_y = y + dy;
                if (next_x < 0 || next_x >= width || next_y < 0 || next_y >= height) {
                    continue;
                }
                const std::size_t next = static_cast<std::size_t>(next_y) *
                    recipe.binding.width + static_cast<std::size_t>(next_x);
                if (surface_labels[next] != 0U || distance[next] != kUnreached) continue;
                surface_labels[next] = surface_labels[current];
                distance[next] = static_cast<std::uint8_t>(current_distance + 1U);
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    padded_pixels[next * 4U + channel] =
                        padded_pixels[static_cast<std::size_t>(current) * 4U + channel];
                }
                queue.push_back(static_cast<std::uint32_t>(next));
                ++padded_texel_count;
            }
        }
    }

    BakePaddingArtifact artifact;
    artifact.recipe = recipe;
    artifact.padded_texels = padded_texel_count;
    artifact.rgba8 = std::move(padded_pixels);
    if (auto result = artifact.refresh_digests(); !result) {
        return core::Result<BakePaddingArtifact>::failure(result.error());
    }
    return core::Result<BakePaddingArtifact>::success(std::move(artifact));
}

core::Result<void> validate_bake_padding_sources(
    const BakePaddingArtifact& artifact,
    const NormalMapArtifact& input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target) {
    return validate_bake_padding_sources(artifact,
        BakePaddingInputRef{std::cref(input)}, current_source, current_target);
}

core::Result<void> validate_bake_padding_sources(
    const BakePaddingArtifact& artifact,
    BakePaddingInputRef input,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target) {
    if (auto result = artifact.validate(); !result) return result;
    const auto expected = compile_bake_padding_cpu(
        artifact.recipe, std::move(input), current_source, current_target);
    if (!expected) return core::Result<void>::failure(expected.error());
    if (expected.value().artifact_digest != artifact.artifact_digest) {
        return core::Result<void>::failure(validation(
            "bake padding artifact differs from its current deterministic inputs"));
    }
    return core::Result<void>::success();
}

core::Result<void> BakeTileArtifactCacheEntry::validate() const {
    if (auto result = validate_tile_cache_entry_binding(*this); !result) return result;
    if (entry_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "bake tile cache entry digest is missing"));
    }
    const auto canonical = canonical_tile_cache_entry_bytes(*this);
    if (canonical.empty() || hash_bytes(canonical) != entry_digest) {
        return core::Result<void>::failure(validation(
            "bake tile cache entry digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<void> BakeTileArtifactCacheEntry::refresh_digest() {
    if (auto result = validate_tile_cache_entry_binding(*this); !result) return result;
    const auto canonical = canonical_tile_cache_entry_bytes(*this);
    if (canonical.empty() || canonical.size() > kMaxTileCacheEntryBytes) {
        return core::Result<void>::failure(validation(
            "bake tile cache entry is empty or exceeds its serialized size limit"));
    }
    entry_digest = hash_bytes(canonical);
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> BakeTileArtifactCacheEntry::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_tile_cache_entry_bytes(*this);
    if (bytes.empty() || bytes.size() + entry_digest.bytes.size() >
            kMaxTileCacheEntryBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "bake tile cache entry exceeds its serialized size limit"));
    }
    append_digest(bytes, entry_digest);
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<BakeTileArtifactCacheEntry> BakeTileArtifactCacheEntry::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxTileCacheEntryBytes) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(invalid(
            "bake tile cache entry byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    BakeTileArtifactCacheEntry entry;
    std::uint64_t source_revision = 0U;
    std::uint64_t target_revision = 0U;
    std::uint64_t target_uv_set = 0U;
    std::vector<std::uint8_t> artifact_bytes;
    if (!reader.read_string(magic, 64U) || magic != kTileCacheEntryMagic ||
        !reader.read_u32(version) || version != kTileCacheEntryVersion ||
        !reader.read_string(entry.dependencies.source_identity, kMaxIdentityBytes) ||
        !reader.read_u64(source_revision) ||
        !reader.read_string(entry.dependencies.target_identity, kMaxIdentityBytes) ||
        !reader.read_u64(target_revision) || !reader.read_u64(target_uv_set) ||
        !reader.read_u32(entry.dependencies.tile_u) ||
        !reader.read_u32(entry.dependencies.tile_v) ||
        !reader.read_digest(entry.dependencies.source_geometry_digest) ||
        !reader.read_digest(entry.dependencies.target_geometry_digest) ||
        !reader.read_digest(entry.dependencies.target_tile_digest) ||
        !reader.read_digest(entry.dependencies.settings_digest) ||
        !reader.read_digest(entry.dependencies.dependency_digest) ||
        !reader.read_payload(artifact_bytes, kMaxArtifactBytes)) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(validation(
            "bake tile cache entry header or dependency key is malformed"));
    }
    entry.dependencies.source_revision = core::Revision{source_revision};
    entry.dependencies.target_revision = core::Revision{target_revision};
    entry.dependencies.target_uv_set = target_uv_set;
    if (!reader.read_digest(entry.entry_digest) || reader.offset != bytes.size()) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(validation(
            "bake tile cache entry is truncated, oversized, or has trailing bytes"));
    }
    const auto artifact = NormalMapArtifact::deserialize(artifact_bytes);
    if (!artifact) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(artifact.error());
    }
    entry.artifact = artifact.value();
    if (auto result = entry.validate(); !result) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(result.error());
    }
    const auto canonical = entry.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<BakeTileArtifactCacheEntry>::failure(validation(
            "bake tile cache entry serialization is not canonical"));
    }
    return core::Result<BakeTileArtifactCacheEntry>::success(std::move(entry));
}

static BakeExecutionReport compile_normal_map_cpu_reference(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    BakeNormalSpace normal_space,
    std::stop_token stop_token = {}) {
    BakeExecutionReport report;
    const auto cancellation_requested = [&]() {
        if (!stop_token.stop_requested()) return false;
        report_error(report, "bake_cancelled", "normal-map bake was cancelled");
        return true;
    };
    if (cancellation_requested()) return report;
    if (normal_algorithm(normal_space).empty()) {
        report_error(report, "normal_space_invalid", "requested normal-map space is unsupported");
        return report;
    }
    if (auto result = validate_bake_sources(recipe, source, target); !result) {
        report_error(report, "bake_input_invalid", result.error().message);
        return report;
    }
    const auto source_triangles = collect_triangles(source, std::nullopt);
    const auto target_triangles = collect_triangles(target, recipe.target_uv_set);
    if (!source_triangles || !target_triangles) {
        const auto& error = !source_triangles ? source_triangles.error() : target_triangles.error();
        report_error(report, "bake_mesh_invalid", error.message);
        return report;
    }
    std::vector<std::array<core::Vec4d, 3U>> target_tangent_frames;
    if (normal_space == BakeNormalSpace::tangent) {
        const auto generated = generate_target_tangent_frames(
            target, target_triangles.value(), recipe.target_uv_set, report);
        if (!generated) {
            report_error(report, "target_tangent_frame_invalid", generated.error().message);
            return report;
        }
        target_tangent_frames = std::move(generated).value();
    }
    if (cancellation_requested()) return report;

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(recipe.width) * recipe.height * 4U, 0U);
    for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
        if ((index & 0xffffU) == 0U && cancellation_requested()) return report;
        pixels[index] = 128U;
        pixels[index + 1U] = 128U;
        pixels[index + 2U] = 255U;
    }
    std::vector<bool> occupied(static_cast<std::size_t>(recipe.width) * recipe.height, false);
    std::vector<geometry::FaceId> owners(occupied.size());
    const TriangleBvh bvh(source_triangles.value());
    if (cancellation_requested()) return report;
    std::uint64_t raster_candidates = 0U;
    std::vector<BakeProjectionMissSample> miss_samples;
    bool miss_samples_truncated = false;
    const double tile_left = static_cast<double>(recipe.tile_u);
    const double tile_bottom = static_cast<double>(recipe.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;

    for (std::size_t triangle_index = 0U;
         triangle_index < target_triangles.value().size(); ++triangle_index) {
        if (cancellation_requested()) return report;
        Triangle triangle = target_triangles.value()[triangle_index];
        std::array<core::Vec4d, 3U> tangents{};
        if (normal_space == BakeNormalSpace::tangent) {
            tangents = target_tangent_frames[triangle_index];
        }
        const double minimum_u = std::min({triangle.uvs[0U].x, triangle.uvs[1U].x,
            triangle.uvs[2U].x});
        const double maximum_u = std::max({triangle.uvs[0U].x, triangle.uvs[1U].x,
            triangle.uvs[2U].x});
        const double minimum_v = std::min({triangle.uvs[0U].y, triangle.uvs[1U].y,
            triangle.uvs[2U].y});
        const double maximum_v = std::max({triangle.uvs[0U].y, triangle.uvs[1U].y,
            triangle.uvs[2U].y});
        if (maximum_u <= tile_left || minimum_u >= tile_right ||
            maximum_v <= tile_bottom || minimum_v >= tile_top) {
            continue;
        }
        const bool all_in_tile = std::all_of(triangle.uvs.begin(), triangle.uvs.end(),
            [tile_left, tile_right, tile_bottom, tile_top](core::Vec2d uv) {
                return uv.x >= tile_left && uv.x <= tile_right &&
                    uv.y >= tile_bottom && uv.y <= tile_top;
            });
        if (!all_in_tile) {
            report_error(report, "cross_tile_uv_triangle",
                "CPU reference bake requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }

        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kGeometryEpsilon) {
            report_error(report, "degenerate_target_uv",
                "target triangle has a degenerate UV chart", triangle.face);
            continue;
        }
        if (area < 0.0) {
            std::swap(triangle.positions[1U], triangle.positions[2U]);
            std::swap(triangle.normals[1U], triangle.normals[2U]);
            std::swap(triangle.uvs[1U], triangle.uvs[2U]);
            std::swap(tangents[1U], tangents[2U]);
            area = -area;
        }

        const double local_min_u = std::clamp(minimum_u - tile_left, 0.0, 1.0);
        const double local_max_u = std::clamp(maximum_u - tile_left, 0.0, 1.0);
        const double local_min_v = std::clamp(minimum_v - tile_bottom, 0.0, 1.0);
        const double local_max_v = std::clamp(maximum_v - tile_bottom, 0.0, 1.0);
        const std::uint32_t min_x = pixel_floor(
            std::floor(local_min_u * recipe.width), recipe.width);
        const std::uint32_t max_x = pixel_floor(
            std::floor(local_max_u * recipe.width), recipe.width);
        const std::uint32_t min_y = pixel_floor(
            std::floor(local_min_v * recipe.height), recipe.height);
        const std::uint32_t max_y = pixel_floor(
            std::floor(local_max_v * recipe.height), recipe.height);
        const double edge_epsilon = std::max(1.0, area) * 1e-14;

        for (std::uint32_t y = min_y; y <= max_y; ++y) {
            if (cancellation_requested()) return report;
            for (std::uint32_t x = min_x; x <= max_x; ++x) {
                if ((x & 63U) == 0U && cancellation_requested()) return report;
                ++raster_candidates;
                if (raster_candidates > kMaxBakeRasterCandidates) {
                    report_error(report, "raster_work_budget_exceeded",
                        "target UV rasterization exceeded its deterministic work budget",
                        triangle.face);
                    return report;
                }
                const core::Vec2d sample{
                    tile_left + (static_cast<double>(x) + 0.5) / recipe.width,
                    tile_bottom + (static_cast<double>(y) + 0.5) / recipe.height};
                const double edge_zero = edge_value(triangle.uvs[1U], triangle.uvs[2U], sample);
                const double edge_one = edge_value(triangle.uvs[2U], triangle.uvs[0U], sample);
                const double edge_two = edge_value(triangle.uvs[0U], triangle.uvs[1U], sample);
                const auto inside = [edge_epsilon](double edge, bool inclusive) {
                    return edge > edge_epsilon ||
                        (std::abs(edge) <= edge_epsilon && inclusive);
                };
                if (!inside(edge_zero, top_left_edge(triangle.uvs[1U], triangle.uvs[2U])) ||
                    !inside(edge_one, top_left_edge(triangle.uvs[2U], triangle.uvs[0U])) ||
                    !inside(edge_two, top_left_edge(triangle.uvs[0U], triangle.uvs[1U]))) {
                    continue;
                }
                ++report.target_texels_visited;
                const std::size_t texel = static_cast<std::size_t>(y) * recipe.width + x;
                if (occupied[texel]) {
                    std::ostringstream message;
                    message << "target UV triangles overlap at texel " << x << ',' << y
                            << " (previous face " << owners[texel].value << ')';
                    report_error(report, "overlapping_uv_islands", message.str(), triangle.face, x, y);
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
                core::Vec3d tangent{};
                double handedness = 1.0;
                if (normal_space == BakeNormalSpace::tangent) {
                    tangent = {
                        tangents[0U].x * barycentric[0U] +
                            tangents[1U].x * barycentric[1U] +
                            tangents[2U].x * barycentric[2U],
                        tangents[0U].y * barycentric[0U] +
                            tangents[1U].y * barycentric[1U] +
                            tangents[2U].y * barycentric[2U],
                        tangents[0U].z * barycentric[0U] +
                            tangents[1U].z * barycentric[1U] +
                            tangents[2U].z * barycentric[2U]};
                    tangent = (tangent - target_normal *
                        core::dot(target_normal, tangent)).normalized();
                    handedness = tangents[0U].w * barycentric[0U] +
                        tangents[1U].w * barycentric[1U] +
                        tangents[2U].w * barycentric[2U];
                }
                if (!target_position.finite() || !target_normal.finite() ||
                    target_normal.length() <= 0.0 ||
                    (normal_space == BakeNormalSpace::tangent &&
                        (!tangent.finite() || tangent.length() <= 0.0 ||
                            std::abs(handedness) <= kGeometryEpsilon))) {
                    report_error(report, "target_shading_frame_invalid",
                        "target interpolation produced a degenerate shading frame",
                        triangle.face, x, y);
                    continue;
                }
                const core::Vec3d bitangent = normal_space == BakeNormalSpace::tangent
                    ? core::cross(target_normal, tangent) *
                        (handedness < 0.0 ? -1.0 : 1.0)
                    : core::Vec3d{};
                const core::Vec3d ray_direction = target_normal * -1.0;
                const core::Vec3d ray_origin = target_position +
                    target_normal * recipe.projection_half_extent;
                const RayCastResult projection = bvh.cast(ray_origin, ray_direction,
                    recipe.projection_half_extent * 2.0, recipe.backface_policy);
                if (!projection.hit) {
                    if (projection.rejected_backface) {
                        ++report.projection_backface_rejections;
                        const Triangle& rejected = source_triangles.value()[
                            projection.rejected_backface->triangle_index];
                        report_warning(report, "projection_backface_rejected",
                            "front-only projection rejected the nearest bounded source backface",
                            triangle.face, rejected.face, x, y);
                    }
                    ++report.projection_misses;
                    if (miss_samples.size() < kMaxProjectionMissSamples) {
                        miss_samples.push_back(BakeProjectionMissSample{triangle.face, x, y});
                        report_diagnostic(report,
                            "projection_miss",
                            "bounded source projection missed a covered target texel",
                            recipe.reject_projection_misses,
                            triangle.face,
                            std::nullopt,
                            x,
                            y);
                    } else {
                        miss_samples_truncated = true;
                    }
                    continue;
                }
                const RayHit& hit = *projection.hit;
                if (!hit.front_facing) {
                    ++report.projection_backface_hits;
                    const Triangle& selected = source_triangles.value()[hit.triangle_index];
                    report_warning(report, "projection_backface_hit",
                        "two-sided projection selected a source backface",
                        triangle.face, selected.face, x, y);
                }
                if (hit.distance >= recipe.projection_half_extent - kGeometryEpsilon) {
                    const Triangle& selected = source_triangles.value()[hit.triangle_index];
                    report_warning(report, "projection_cage_intersection",
                        "selected source hit lies beyond the target plane along the bounded projection ray",
                        triangle.face, selected.face, x, y);
                }
                ++report.projected_texels;
                const Triangle& source_triangle = source_triangles.value()[hit.triangle_index];
                core::Vec3d source_normal = (
                    source_triangle.normals[0U] * hit.barycentric[0U] +
                    source_triangle.normals[1U] * hit.barycentric[1U] +
                    source_triangle.normals[2U] * hit.barycentric[2U]).normalized();
                if (!source_normal.finite() || source_normal.length() <= 0.0) {
                    report_error(report, "source_shading_normal_invalid",
                        "source projection hit has a degenerate interpolated normal",
                        triangle.face, x, y);
                    continue;
                }
                const std::size_t pixel = texel * 4U;
                if (normal_space == BakeNormalSpace::tangent) {
                    double green = core::dot(source_normal, bitangent);
                    if (recipe.normal_convention == BakeNormalConvention::directx_y_minus) {
                        green = -green;
                    }
                    pixels[pixel] = encode_normal_component(
                        core::dot(source_normal, tangent));
                    pixels[pixel + 1U] = encode_normal_component(green);
                    pixels[pixel + 2U] = encode_normal_component(
                        core::dot(source_normal, target_normal));
                } else {
                    pixels[pixel] = encode_normal_component(source_normal.x);
                    pixels[pixel + 1U] = encode_normal_component(source_normal.y);
                    pixels[pixel + 2U] = encode_normal_component(source_normal.z);
                }
                pixels[pixel + 3U] = 255U;
            }
        }
    }

    if (cancellation_requested()) return report;
    if (std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        if (miss_samples.size() < report.projection_misses && recipe.reject_projection_misses) {
            report_error(report, "projection_misses_truncated",
                "additional projection misses were omitted from the bounded diagnostics list");
        }
        return report;
    }
    if (report.covered_texels == 0U) {
        report_error(report, "target_uv_no_coverage",
            "selected target UV tile contains no rasterized texels");
        return report;
    }
    if (recipe.reject_projection_misses && report.projection_misses != 0U) {
        report_error(report, "projection_misses_rejected",
            "recipe rejects one or more bounded source projection misses");
        return report;
    }
    if (cancellation_requested()) return report;

    NormalMapArtifact artifact;
    artifact.recipe = recipe;
    artifact.normal_space = normal_space;
    artifact.algorithm = std::string(normal_algorithm(normal_space));
    artifact.covered_texels = report.covered_texels;
    artifact.projected_texels = report.projected_texels;
    artifact.projection_misses = report.projection_misses;
    artifact.projection_miss_samples = std::move(miss_samples);
    artifact.projection_miss_samples_truncated = miss_samples_truncated;
    artifact.rgba8 = std::move(pixels);
    if (auto result = artifact.refresh_digests(); !result) {
        report_error(report, "artifact_validation_failed", result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        report_error(report, "artifact_validation_failed", result.error().message);
        return report;
    }
    if (cancellation_requested()) return report;
    report.artifact = std::move(artifact);
    return report;
}

namespace detail {

NormalMapBakeWork prepare_normal_map_work(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    BakeNormalSpace normal_space) {
    NormalMapBakeWork work;
    work.recipe = recipe;
    work.normal_space = normal_space;
    if (normal_algorithm(normal_space).empty()) {
        report_error(work.report, "normal_space_invalid",
            "requested normal-map space is unsupported");
        return work;
    }
    if (auto result = validate_bake_sources(recipe, source, target); !result) {
        report_error(work.report, "bake_input_invalid", result.error().message);
        return work;
    }
    const auto source_triangles = collect_triangles(source, std::nullopt);
    const auto target_triangles = collect_triangles(target, recipe.target_uv_set);
    if (!source_triangles || !target_triangles) {
        const auto& error = !source_triangles ? source_triangles.error() : target_triangles.error();
        report_error(work.report, "bake_mesh_invalid", error.message);
        return work;
    }
    const auto settings_digest = normal_map_settings_digest(recipe, normal_space);
    if (!settings_digest) {
        report_error(work.report, "bake_settings_invalid", settings_digest.error().message);
        return work;
    }
    work.settings_digest = settings_digest.value();
    std::vector<std::array<core::Vec4d, 3U>> target_tangent_frames;
    if (normal_space == BakeNormalSpace::tangent) {
        const auto generated = generate_target_tangent_frames(
            target, target_triangles.value(), recipe.target_uv_set, work.report);
        if (!generated) {
            report_error(work.report, "target_tangent_frame_invalid", generated.error().message);
            return work;
        }
        target_tangent_frames = std::move(generated).value();
    }
    work.source_triangles.reserve(source_triangles.value().size());
    for (const Triangle& triangle : source_triangles.value()) {
        work.source_triangles.push_back(NormalMapProjectionTriangle{
            triangle.face.value, triangle.fan_index, triangle.positions, triangle.normals});
    }
    work.pixels.assign(
        static_cast<std::size_t>(recipe.width) * recipe.height * 4U, 0U);
    for (std::size_t index = 0U; index < work.pixels.size(); index += 4U) {
        work.pixels[index] = 128U;
        work.pixels[index + 1U] = 128U;
        work.pixels[index + 2U] = 255U;
    }

    std::vector<bool> occupied(static_cast<std::size_t>(recipe.width) * recipe.height, false);
    std::vector<geometry::FaceId> owners(occupied.size());
    std::uint64_t raster_candidates = 0U;
    const double tile_left = static_cast<double>(recipe.tile_u);
    const double tile_bottom = static_cast<double>(recipe.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;

    for (std::size_t triangle_index = 0U;
         triangle_index < target_triangles.value().size(); ++triangle_index) {
        Triangle triangle = target_triangles.value()[triangle_index];
        std::array<core::Vec4d, 3U> tangents{};
        if (normal_space == BakeNormalSpace::tangent) {
            tangents = target_tangent_frames[triangle_index];
        }
        const double minimum_u = std::min({triangle.uvs[0U].x, triangle.uvs[1U].x,
            triangle.uvs[2U].x});
        const double maximum_u = std::max({triangle.uvs[0U].x, triangle.uvs[1U].x,
            triangle.uvs[2U].x});
        const double minimum_v = std::min({triangle.uvs[0U].y, triangle.uvs[1U].y,
            triangle.uvs[2U].y});
        const double maximum_v = std::max({triangle.uvs[0U].y, triangle.uvs[1U].y,
            triangle.uvs[2U].y});
        if (maximum_u <= tile_left || minimum_u >= tile_right ||
            maximum_v <= tile_bottom || minimum_v >= tile_top) {
            continue;
        }
        const bool all_in_tile = std::all_of(triangle.uvs.begin(), triangle.uvs.end(),
            [tile_left, tile_right, tile_bottom, tile_top](core::Vec2d uv) {
                return uv.x >= tile_left && uv.x <= tile_right &&
                    uv.y >= tile_bottom && uv.y <= tile_top;
            });
        if (!all_in_tile) {
            report_error(work.report, "cross_tile_uv_triangle",
                "CPU reference bake requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }

        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kGeometryEpsilon) {
            report_error(work.report, "degenerate_target_uv",
                "target triangle has a degenerate UV chart", triangle.face);
            continue;
        }
        if (area < 0.0) {
            std::swap(triangle.positions[1U], triangle.positions[2U]);
            std::swap(triangle.normals[1U], triangle.normals[2U]);
            std::swap(triangle.uvs[1U], triangle.uvs[2U]);
            std::swap(tangents[1U], tangents[2U]);
            area = -area;
        }

        const double local_min_u = std::clamp(minimum_u - tile_left, 0.0, 1.0);
        const double local_max_u = std::clamp(maximum_u - tile_left, 0.0, 1.0);
        const double local_min_v = std::clamp(minimum_v - tile_bottom, 0.0, 1.0);
        const double local_max_v = std::clamp(maximum_v - tile_bottom, 0.0, 1.0);
        const std::uint32_t min_x = pixel_floor(
            std::floor(local_min_u * recipe.width), recipe.width);
        const std::uint32_t max_x = pixel_floor(
            std::floor(local_max_u * recipe.width), recipe.width);
        const std::uint32_t min_y = pixel_floor(
            std::floor(local_min_v * recipe.height), recipe.height);
        const std::uint32_t max_y = pixel_floor(
            std::floor(local_max_v * recipe.height), recipe.height);
        const double edge_epsilon = std::max(1.0, area) * 1e-14;

        for (std::uint32_t y = min_y; y <= max_y; ++y) {
            for (std::uint32_t x = min_x; x <= max_x; ++x) {
                ++raster_candidates;
                if (raster_candidates > kMaxBakeRasterCandidates) {
                    report_error(work.report, "raster_work_budget_exceeded",
                        "target UV rasterization exceeded its deterministic work budget",
                        triangle.face);
                    return work;
                }
                const core::Vec2d sample{
                    tile_left + (static_cast<double>(x) + 0.5) / recipe.width,
                    tile_bottom + (static_cast<double>(y) + 0.5) / recipe.height};
                const double edge_zero = edge_value(triangle.uvs[1U], triangle.uvs[2U], sample);
                const double edge_one = edge_value(triangle.uvs[2U], triangle.uvs[0U], sample);
                const double edge_two = edge_value(triangle.uvs[0U], triangle.uvs[1U], sample);
                const auto inside = [edge_epsilon](double edge, bool inclusive) {
                    return edge > edge_epsilon ||
                        (std::abs(edge) <= edge_epsilon && inclusive);
                };
                if (!inside(edge_zero, top_left_edge(triangle.uvs[1U], triangle.uvs[2U])) ||
                    !inside(edge_one, top_left_edge(triangle.uvs[2U], triangle.uvs[0U])) ||
                    !inside(edge_two, top_left_edge(triangle.uvs[0U], triangle.uvs[1U]))) {
                    continue;
                }
                ++work.report.target_texels_visited;
                const std::size_t texel = static_cast<std::size_t>(y) * recipe.width + x;
                if (occupied[texel]) {
                    std::ostringstream message;
                    message << "target UV triangles overlap at texel " << x << ',' << y
                            << " (previous face " << owners[texel].value << ')';
                    report_error(work.report, "overlapping_uv_islands", message.str(),
                        triangle.face, x, y);
                    continue;
                }
                occupied[texel] = true;
                owners[texel] = triangle.face;
                ++work.report.covered_texels;

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
                core::Vec3d tangent{};
                double handedness = 1.0;
                if (normal_space == BakeNormalSpace::tangent) {
                    tangent = {
                        tangents[0U].x * barycentric[0U] +
                            tangents[1U].x * barycentric[1U] +
                            tangents[2U].x * barycentric[2U],
                        tangents[0U].y * barycentric[0U] +
                            tangents[1U].y * barycentric[1U] +
                            tangents[2U].y * barycentric[2U],
                        tangents[0U].z * barycentric[0U] +
                            tangents[1U].z * barycentric[1U] +
                            tangents[2U].z * barycentric[2U]};
                    tangent = (tangent - target_normal *
                        core::dot(target_normal, tangent)).normalized();
                    handedness = tangents[0U].w * barycentric[0U] +
                        tangents[1U].w * barycentric[1U] +
                        tangents[2U].w * barycentric[2U];
                }
                if (!target_position.finite() || !target_normal.finite() ||
                    target_normal.length() <= 0.0 ||
                    (normal_space == BakeNormalSpace::tangent &&
                        (!tangent.finite() || tangent.length() <= 0.0 ||
                            std::abs(handedness) <= kGeometryEpsilon))) {
                    report_error(work.report, "target_shading_frame_invalid",
                        "target interpolation produced a degenerate shading frame",
                        triangle.face, x, y);
                    continue;
                }
                if (work.samples.size() >= kMaxNormalMapProjectionSamples) {
                    report_error(work.report, "projection_sample_budget_exceeded",
                        "D3D12 bake exceeds the bounded projection sample count",
                        triangle.face, x, y);
                    return work;
                }
                const core::Vec3d bitangent = normal_space == BakeNormalSpace::tangent
                    ? core::cross(target_normal, tangent) *
                        (handedness < 0.0 ? -1.0 : 1.0)
                    : core::Vec3d{};
                work.samples.push_back(NormalMapProjectionSample{
                    triangle.face,
                    x,
                    y,
                    texel,
                    target_position + target_normal * recipe.projection_half_extent,
                    target_normal * -1.0,
                    target_normal,
                    tangent,
                    bitangent,
                });
            }
        }
    }
    return work;
}

std::vector<NormalMapProjectionOutcome> cast_normal_map_cpu_reference(
    const NormalMapBakeWork& work,
    const geometry::EditableMesh& source) {
    std::vector<NormalMapProjectionOutcome> outcomes;
    outcomes.reserve(work.samples.size());
    const auto source_triangles = collect_triangles(source, std::nullopt);
    if (!source_triangles) return outcomes;
    const TriangleBvh bvh(source_triangles.value());
    for (const NormalMapProjectionSample& sample : work.samples) {
        const RayCastResult projection = bvh.cast(
            sample.ray_origin,
            sample.ray_direction,
            work.recipe.projection_half_extent * 2.0,
            work.recipe.backface_policy);
        NormalMapProjectionOutcome outcome;
        if (projection.hit) {
            outcome.hit = true;
            outcome.triangle_index = projection.hit->triangle_index;
            outcome.backface = !projection.hit->front_facing;
            outcome.distance = projection.hit->distance;
            outcome.barycentric = projection.hit->barycentric;
        }
        if (projection.rejected_backface) {
            outcome.rejected_backface_triangle_index =
                projection.rejected_backface->triangle_index;
        }
        outcomes.push_back(outcome);
    }
    return outcomes;
}

BakeExecutionReport finalize_normal_map_work(
    NormalMapBakeWork work,
    std::span<const NormalMapProjectionOutcome> outcomes) {
    if (outcomes.size() != work.samples.size()) {
        report_error(work.report, "projection_result_count_invalid",
            "projection backend did not return one result for every covered texel");
        return std::move(work.report);
    }
    for (std::size_t index = 0U; index < work.samples.size(); ++index) {
        const NormalMapProjectionSample& sample = work.samples[index];
        const NormalMapProjectionOutcome& outcome = outcomes[index];
        if (!outcome.hit) {
            if (outcome.rejected_backface_triangle_index.has_value()) {
                const std::size_t rejected_index = *outcome.rejected_backface_triangle_index;
                if (rejected_index >= work.source_triangles.size()) {
                    report_error(work.report, "projection_result_invalid",
                        "projection backend returned an invalid rejected-backface identity",
                        sample.target_face, sample.texel_x, sample.texel_y);
                    continue;
                }
                ++work.report.projection_backface_rejections;
                report_warning(work.report, "projection_backface_rejected",
                    "front-only projection rejected the nearest bounded source backface",
                    sample.target_face,
                    geometry::FaceId{work.source_triangles[rejected_index].face_id},
                    sample.texel_x,
                    sample.texel_y);
            }
            ++work.report.projection_misses;
            if (work.miss_samples.size() < kMaxProjectionMissSamples) {
                work.miss_samples.push_back(BakeProjectionMissSample{
                    sample.target_face, sample.texel_x, sample.texel_y});
                report_diagnostic(work.report,
                    "projection_miss",
                    "bounded source projection missed a covered target texel",
                    work.recipe.reject_projection_misses,
                    sample.target_face,
                    std::nullopt,
                    sample.texel_x,
                    sample.texel_y);
            } else {
                work.miss_samples_truncated = true;
            }
            continue;
        }
        if (outcome.triangle_index >= work.source_triangles.size() ||
            !std::isfinite(outcome.distance) || outcome.distance <= 0.0 ||
            outcome.distance > work.recipe.projection_half_extent * 2.0 ||
            !std::all_of(outcome.barycentric.begin(), outcome.barycentric.end(),
                [](double value) { return std::isfinite(value) && value >= -1e-6 && value <= 1.000001; }) ||
            std::abs(outcome.barycentric[0U] + outcome.barycentric[1U] +
                outcome.barycentric[2U] - 1.0) > 1e-5) {
            report_error(work.report, "projection_result_invalid",
                "projection backend returned a malformed bounded hit",
                sample.target_face, sample.texel_x, sample.texel_y);
            continue;
        }
        if (outcome.backface) {
            ++work.report.projection_backface_hits;
            report_warning(work.report, "projection_backface_hit",
                "two-sided projection selected a source backface",
                sample.target_face,
                geometry::FaceId{work.source_triangles[outcome.triangle_index].face_id},
                sample.texel_x,
                sample.texel_y);
        }
        if (outcome.distance >= work.recipe.projection_half_extent - kGeometryEpsilon) {
            report_warning(work.report, "projection_cage_intersection",
                "selected source hit lies beyond the target plane along the bounded projection ray",
                sample.target_face,
                geometry::FaceId{work.source_triangles[outcome.triangle_index].face_id},
                sample.texel_x,
                sample.texel_y);
        }
        ++work.report.projected_texels;
        const NormalMapProjectionTriangle& source_triangle =
            work.source_triangles[outcome.triangle_index];
        const core::Vec3d source_normal = (
            source_triangle.normals[0U] * outcome.barycentric[0U] +
            source_triangle.normals[1U] * outcome.barycentric[1U] +
            source_triangle.normals[2U] * outcome.barycentric[2U]).normalized();
        if (!source_normal.finite() || source_normal.length() <= 0.0) {
            report_error(work.report, "source_shading_normal_invalid",
                "source projection hit has a degenerate interpolated normal",
                sample.target_face, sample.texel_x, sample.texel_y);
            continue;
        }
        const std::size_t pixel = sample.pixel_index * 4U;
        if (work.normal_space == BakeNormalSpace::tangent) {
            double green = core::dot(source_normal, sample.bitangent);
            if (work.recipe.normal_convention == BakeNormalConvention::directx_y_minus) {
                green = -green;
            }
            work.pixels[pixel] = encode_normal_component(
                core::dot(source_normal, sample.tangent));
            work.pixels[pixel + 1U] = encode_normal_component(green);
            work.pixels[pixel + 2U] = encode_normal_component(
                core::dot(source_normal, sample.target_normal));
        } else {
            work.pixels[pixel] = encode_normal_component(source_normal.x);
            work.pixels[pixel + 1U] = encode_normal_component(source_normal.y);
            work.pixels[pixel + 2U] = encode_normal_component(source_normal.z);
        }
        work.pixels[pixel + 3U] = 255U;
    }

    if (std::any_of(work.report.diagnostics.begin(), work.report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        if (work.miss_samples.size() < work.report.projection_misses &&
            work.recipe.reject_projection_misses) {
            report_error(work.report, "projection_misses_truncated",
                "additional projection misses were omitted from the bounded diagnostics list");
        }
        return std::move(work.report);
    }
    if (work.report.covered_texels == 0U) {
        report_error(work.report, "target_uv_no_coverage",
            "selected target UV tile contains no rasterized texels");
        return std::move(work.report);
    }
    if (work.recipe.reject_projection_misses && work.report.projection_misses != 0U) {
        report_error(work.report, "projection_misses_rejected",
            "recipe rejects one or more bounded source projection misses");
        return std::move(work.report);
    }

    NormalMapArtifact artifact;
    artifact.recipe = work.recipe;
    artifact.normal_space = work.normal_space;
    artifact.algorithm = std::string(normal_algorithm(work.normal_space));
    artifact.covered_texels = work.report.covered_texels;
    artifact.projected_texels = work.report.projected_texels;
    artifact.projection_misses = work.report.projection_misses;
    artifact.projection_miss_samples = std::move(work.miss_samples);
    artifact.projection_miss_samples_truncated = work.miss_samples_truncated;
    artifact.rgba8 = std::move(work.pixels);
    if (auto result = artifact.refresh_digests(); !result) {
        report_error(work.report, "artifact_validation_failed", result.error().message);
        return std::move(work.report);
    }
    if (artifact.settings_digest != work.settings_digest) {
        report_error(work.report, "artifact_settings_drift",
            "normal-map artifact settings differ from the projection request settings");
        return std::move(work.report);
    }
    if (auto result = artifact.validate(); !result) {
        report_error(work.report, "artifact_validation_failed", result.error().message);
        return std::move(work.report);
    }
    work.report.artifact = std::move(artifact);
    return std::move(work.report);
}

} // namespace detail

BakeExecutionReport compile_tangent_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    return compile_tangent_normal_map_cpu(recipe, source, target, {});
}

BakeExecutionReport compile_tangent_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token) {
    return compile_normal_map_cpu_reference(
        recipe, source, target, BakeNormalSpace::tangent, stop_token);
}

BakeExecutionReport compile_object_space_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    return compile_object_space_normal_map_cpu(recipe, source, target, {});
}

BakeExecutionReport compile_object_space_normal_map_cpu(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token) {
    return compile_normal_map_cpu_reference(
        recipe, source, target, BakeNormalSpace::object, stop_token);
}

} // namespace carto::production

namespace carto::production {
namespace {

std::vector<std::uint8_t> canonical_ambient_occlusion_bytes(
    const AmbientOcclusionMapArtifact& artifact) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kAmbientOcclusionArtifactMagic);
    append_u32(bytes, kAmbientOcclusionArtifactVersion);
    append_string(bytes, artifact.recipe.serialize());
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, artifact.rays_cast);
    append_u64(bytes, artifact.occluded_rays);
    append_double(bytes, artifact.sampling_standard_error_upper_bound);
    append_u64(bytes, static_cast<std::uint64_t>(artifact.rgba8.size()));
    bytes.insert(bytes.end(), artifact.rgba8.begin(), artifact.rgba8.end());
    return bytes;
}

std::vector<std::uint8_t> canonical_bent_normal_bytes(
    const BentNormalMapArtifact& artifact) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kBentNormalArtifactMagic);
    append_u32(bytes, kBentNormalArtifactVersion);
    append_string(bytes, artifact.recipe.serialize());
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, artifact.rays_cast);
    append_u64(bytes, artifact.occluded_rays);
    append_double(bytes, artifact.sampling_standard_error_upper_bound);
    append_u64(bytes, static_cast<std::uint64_t>(artifact.rgba8.size()));
    bytes.insert(bytes.end(), artifact.rgba8.begin(), artifact.rgba8.end());
    return bytes;
}

void report_error(
    AmbientOcclusionExecutionReport& report,
    std::string code,
    std::string message,
    std::optional<geometry::FaceId> target_face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    if (report.diagnostics.size() < kMaxBakeDiagnostics) {
        report.diagnostics.push_back(BakeDiagnostic{
            std::move(code), std::move(message), true, target_face, std::nullopt, x, y});
    } else if (report.diagnostics.back().code != "diagnostics_truncated") {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated",
            "additional ambient-occlusion diagnostics were omitted after the bounded report limit",
            true,
            target_face,
            std::nullopt,
            x,
            y};
    }
}

std::uint64_t mix_ao_seed(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint32_t reverse_bits(std::uint32_t value) {
    value = ((value & 0x55555555U) << 1U) | ((value >> 1U) & 0x55555555U);
    value = ((value & 0x33333333U) << 2U) | ((value >> 2U) & 0x33333333U);
    value = ((value & 0x0f0f0f0fU) << 4U) | ((value >> 4U) & 0x0f0f0f0fU);
    value = ((value & 0x00ff00ffU) << 8U) | ((value >> 8U) & 0x00ff00ffU);
    return (value << 16U) | (value >> 16U);
}

} // namespace

core::Result<void> AmbientOcclusionRecipe::validate() const {
    if (auto result = binding.validate(); !result) return result;
    if (sample_count == 0U || sample_count > kMaxBakeAmbientOcclusionSamples ||
        !std::isfinite(max_distance) || max_distance <= 0.0 ||
        max_distance > 1'000'000.0 || !std::isfinite(origin_bias) ||
        origin_bias <= 0.0 || origin_bias >= max_distance || origin_bias > 1'000.0) {
        return core::Result<void>::failure(invalid(
            "ambient-occlusion sample count, distance, or ray bias is invalid"));
    }
    return core::Result<void>::success();
}

std::string AmbientOcclusionRecipe::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << kAmbientOcclusionRecipeMagic << '\n'
           << "binding.hex=" << encode_hex(binding.serialize()) << '\n'
           << "ao.samples=" << sample_count << '\n'
           << "ao.distance.bits=" << double_bits_hex(max_distance) << '\n'
           << "ao.origin_bias.bits=" << double_bits_hex(origin_bias) << '\n'
           << "ao.seed=" << seed << '\n';
    const std::string text = output.str();
    return text.size() <= kMaxBakeAmbientOcclusionRecipeBytes ? text : std::string{};
}

core::Result<assets::Sha256Digest> AmbientOcclusionRecipe::canonical_digest() const {
    const std::string text = serialize();
    if (text.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid or oversized ambient-occlusion recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(text));
}

core::Result<AmbientOcclusionRecipe> AmbientOcclusionRecipe::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxBakeAmbientOcclusionRecipeBytes) {
        return core::Result<AmbientOcclusionRecipe>::failure(invalid(
            "ambient-occlusion recipe byte size is invalid"));
    }
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || line != kAmbientOcclusionRecipeMagic) {
        return core::Result<AmbientOcclusionRecipe>::failure(validation(
            "ambient-occlusion recipe header is unsupported"));
    }
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<AmbientOcclusionRecipe>::failure(validation(
                "ambient-occlusion recipe contains a malformed or duplicate field"));
        }
    }
    if (fields.size() != 5U) {
        return core::Result<AmbientOcclusionRecipe>::failure(validation(
            "ambient-occlusion recipe does not contain the exact supported field set"));
    }
    const auto field = [&fields](std::string_view name) -> std::optional<std::string_view> {
        const auto found = fields.find(std::string(name));
        if (found == fields.end()) return std::nullopt;
        return found->second;
    };
    const auto binding_hex = field("binding.hex");
    const auto sample_count_value = field("ao.samples");
    const auto distance_bits = field("ao.distance.bits");
    const auto bias_bits = field("ao.origin_bias.bits");
    const auto seed_value = field("ao.seed");
    if (!binding_hex || !sample_count_value || !distance_bits || !bias_bits || !seed_value) {
        return core::Result<AmbientOcclusionRecipe>::failure(validation(
            "ambient-occlusion recipe is missing a required field"));
    }
    const auto binding_text = decode_hex(*binding_hex, kMaxBakeRecipeBytes);
    const auto sample_count = parse_u32(*sample_count_value);
    const auto max_distance = double_from_bits_hex(*distance_bits);
    const auto origin_bias = double_from_bits_hex(*bias_bits);
    const auto seed = parse_u64(*seed_value);
    if (!binding_text || !sample_count || !max_distance || !origin_bias || !seed) {
        return core::Result<AmbientOcclusionRecipe>::failure(validation(
            "ambient-occlusion recipe field encoding is invalid"));
    }
    const auto binding = BakeRecipe::deserialize(*binding_text);
    if (!binding) {
        return core::Result<AmbientOcclusionRecipe>::failure(binding.error());
    }
    AmbientOcclusionRecipe recipe{
        binding.value(), *sample_count, *max_distance, *origin_bias, *seed};
    if (auto result = recipe.validate(); !result) {
        return core::Result<AmbientOcclusionRecipe>::failure(result.error());
    }
    if (recipe.serialize() != text) {
        return core::Result<AmbientOcclusionRecipe>::failure(validation(
            "ambient-occlusion recipe serialization is not canonical"));
    }
    return core::Result<AmbientOcclusionRecipe>::success(std::move(recipe));
}

core::Result<void> AmbientOcclusionMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != kBakeAmbientOcclusionAlgorithm) {
        return core::Result<void>::failure(invalid(
            "ambient-occlusion artifact algorithm is unsupported"));
    }
    const auto settings = recipe.canonical_digest();
    if (!settings) return core::Result<void>::failure(settings.error());
    settings_digest = settings.value();
    sampling_standard_error_upper_bound = 0.5 / std::sqrt(
        static_cast<double>(recipe.sample_count));
    output_digest = hash_bytes(rgba8);
    artifact_digest = hash_bytes(canonical_ambient_occlusion_bytes(*this));
    return core::Result<void>::success();
}

core::Result<void> AmbientOcclusionMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t texels =
        static_cast<std::uint64_t>(recipe.binding.width) * recipe.binding.height;
    const std::uint64_t expected_rays =
        covered_texels * static_cast<std::uint64_t>(recipe.sample_count);
    const double expected_standard_error_upper_bound = 0.5 / std::sqrt(
        static_cast<double>(recipe.sample_count));
    if (algorithm != kBakeAmbientOcclusionAlgorithm || settings_digest.is_zero() ||
        output_digest.is_zero() || artifact_digest.is_zero() ||
        rgba8.size() != texels * 4ULL || covered_texels == 0U ||
        covered_texels > texels || rays_cast != expected_rays ||
        rays_cast > kMaxBakeAmbientOcclusionRays || occluded_rays > rays_cast ||
        !std::isfinite(sampling_standard_error_upper_bound) ||
        std::abs(sampling_standard_error_upper_bound -
            expected_standard_error_upper_bound) > 1e-15) {
        return core::Result<void>::failure(validation(
            "ambient-occlusion artifact metadata or coverage accounting is invalid"));
    }
    std::uint64_t opaque_texels = 0U;
    for (std::size_t pixel = 0U; pixel < rgba8.size(); pixel += 4U) {
        const std::uint8_t alpha = rgba8[pixel + 3U];
        if (alpha == 255U) {
            ++opaque_texels;
            if (rgba8[pixel] != rgba8[pixel + 1U] ||
                rgba8[pixel] != rgba8[pixel + 2U]) {
                return core::Result<void>::failure(validation(
                    "ambient-occlusion output must be grayscale"));
            }
        } else if (alpha != 0U || rgba8[pixel] != 255U ||
            rgba8[pixel + 1U] != 255U || rgba8[pixel + 2U] != 255U) {
            return core::Result<void>::failure(validation(
                "ambient-occlusion uncovered texel encoding is invalid"));
        }
    }
    if (opaque_texels != covered_texels) {
        return core::Result<void>::failure(validation(
            "ambient-occlusion alpha coverage disagrees with its receipt"));
    }
    const auto settings = recipe.canonical_digest();
    if (!settings || settings.value() != settings_digest ||
        hash_bytes(rgba8) != output_digest ||
        hash_bytes(canonical_ambient_occlusion_bytes(*this)) != artifact_digest) {
        return core::Result<void>::failure(validation(
            "ambient-occlusion artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> AmbientOcclusionMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_ambient_occlusion_bytes(*this);
    append_digest(bytes, artifact_digest);
    if (bytes.size() > kMaxArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "ambient-occlusion artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<AmbientOcclusionMapArtifact> AmbientOcclusionMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxArtifactBytes) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(invalid(
            "ambient-occlusion artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string recipe_text;
    std::string algorithm_value;
    std::uint64_t standard_error_bits = 0U;
    if (!reader.read_string(magic, kAmbientOcclusionArtifactMagic.size()) ||
        magic != kAmbientOcclusionArtifactMagic ||
        !reader.read_u32(version) || version != kAmbientOcclusionArtifactVersion ||
        !reader.read_string(recipe_text, kMaxBakeAmbientOcclusionRecipeBytes) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(validation(
            "ambient-occlusion artifact header or recipe is malformed"));
    }
    const auto recipe = AmbientOcclusionRecipe::deserialize(recipe_text);
    if (!recipe) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(recipe.error());
    }
    AmbientOcclusionMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.algorithm = std::move(algorithm_value);
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_u64(artifact.rays_cast) ||
        !reader.read_u64(artifact.occluded_rays) ||
        !reader.read_u64(standard_error_bits) ||
        !reader.read_payload(artifact.rgba8,
            static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(validation(
            "ambient-occlusion artifact payload is truncated, oversized, or has trailing bytes"));
    }
    artifact.sampling_standard_error_upper_bound =
        std::bit_cast<double>(standard_error_bits);
    if (auto result = artifact.validate(); !result) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(validation(
            "ambient-occlusion artifact serialization is not canonical"));
    }
    return core::Result<AmbientOcclusionMapArtifact>::success(std::move(artifact));
}

core::Result<void> BentNormalMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    if (rgba8.size() != texels * 4ULL || covered_texels == 0U ||
        covered_texels > texels ||
        rays_cast != covered_texels * static_cast<std::uint64_t>(recipe.sample_count) ||
        rays_cast > kMaxBakeAmbientOcclusionRays || occluded_rays > rays_cast) {
        return core::Result<void>::failure(validation(
            "bent-normal artifact dimensions or coverage accounting are inconsistent"));
    }
    if (algorithm != kBakeBentNormalAlgorithm) {
        return core::Result<void>::failure(invalid(
            "bent-normal artifact algorithm is unsupported"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return core::Result<void>::failure(recipe_digest.error());
    settings_digest = hash_text(std::string(kBakeBentNormalAlgorithm) + "\\n" +
        recipe_digest.value().hex());
    sampling_standard_error_upper_bound = 0.5 / std::sqrt(
        static_cast<double>(recipe.sample_count));
    output_digest = hash_bytes(rgba8);
    artifact_digest = hash_bytes(canonical_bent_normal_bytes(*this));
    return core::Result<void>::success();
}

core::Result<void> BentNormalMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    const std::uint64_t expected_rays =
        covered_texels * static_cast<std::uint64_t>(recipe.sample_count);
    const double expected_standard_error_upper_bound = 0.5 / std::sqrt(
        static_cast<double>(recipe.sample_count));
    if (algorithm != kBakeBentNormalAlgorithm || settings_digest.is_zero() ||
        output_digest.is_zero() || artifact_digest.is_zero() ||
        rgba8.size() != texels * 4ULL || covered_texels == 0U ||
        covered_texels > texels || rays_cast != expected_rays ||
        rays_cast > kMaxBakeAmbientOcclusionRays || occluded_rays > rays_cast ||
        !std::isfinite(sampling_standard_error_upper_bound) ||
        std::abs(sampling_standard_error_upper_bound -
            expected_standard_error_upper_bound) > 1e-15) {
        return core::Result<void>::failure(validation(
            "bent-normal artifact metadata or coverage accounting is invalid"));
    }
    std::uint64_t opaque_texels = 0U;
    for (std::size_t pixel = 0U; pixel < rgba8.size(); pixel += 4U) {
        const std::uint8_t alpha = rgba8[pixel + 3U];
        if (alpha == 255U) {
            ++opaque_texels;
            const double x = static_cast<double>(rgba8[pixel]) / 127.5 - 1.0;
            const double y = static_cast<double>(rgba8[pixel + 1U]) / 127.5 - 1.0;
            const double z = static_cast<double>(rgba8[pixel + 2U]) / 127.5 - 1.0;
            const double length_squared = x * x + y * y + z * z;
            if (!std::isfinite(length_squared) ||
                std::abs(length_squared - 1.0) > 0.04) {
                return core::Result<void>::failure(validation(
                    "bent-normal covered texel is not a normalized tangent-space direction"));
            }
        } else if (alpha != 0U || rgba8[pixel] != 128U ||
            rgba8[pixel + 1U] != 128U || rgba8[pixel + 2U] != 255U) {
            return core::Result<void>::failure(validation(
                "bent-normal uncovered texel encoding is invalid"));
        }
    }
    if (opaque_texels != covered_texels) {
        return core::Result<void>::failure(validation(
            "bent-normal alpha coverage disagrees with its receipt"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest || hash_text(std::string(kBakeBentNormalAlgorithm) + "\\n" +
            recipe_digest.value().hex()) != settings_digest ||
        hash_bytes(rgba8) != output_digest ||
        hash_bytes(canonical_bent_normal_bytes(*this)) != artifact_digest) {
        return core::Result<void>::failure(validation(
            "bent-normal artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> BentNormalMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_bent_normal_bytes(*this);
    append_digest(bytes, artifact_digest);
    if (bytes.size() > kMaxArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "bent-normal artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<BentNormalMapArtifact> BentNormalMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxArtifactBytes) {
        return core::Result<BentNormalMapArtifact>::failure(invalid(
            "bent-normal artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string recipe_text;
    std::string algorithm_value;
    std::uint64_t standard_error_bits = 0U;
    if (!reader.read_string(magic, kBentNormalArtifactMagic.size()) ||
        magic != kBentNormalArtifactMagic ||
        !reader.read_u32(version) || version != kBentNormalArtifactVersion ||
        !reader.read_string(recipe_text, kMaxBakeAmbientOcclusionRecipeBytes) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<BentNormalMapArtifact>::failure(validation(
            "bent-normal artifact header or recipe is malformed"));
    }
    const auto recipe = AmbientOcclusionRecipe::deserialize(recipe_text);
    if (!recipe) return core::Result<BentNormalMapArtifact>::failure(recipe.error());
    BentNormalMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.algorithm = std::move(algorithm_value);
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_u64(artifact.rays_cast) ||
        !reader.read_u64(artifact.occluded_rays) ||
        !reader.read_u64(standard_error_bits) ||
        !reader.read_payload(artifact.rgba8,
            static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<BentNormalMapArtifact>::failure(validation(
            "bent-normal artifact payload is truncated, oversized, or has trailing bytes"));
    }
    artifact.sampling_standard_error_upper_bound =
        std::bit_cast<double>(standard_error_bits);
    if (auto result = artifact.validate(); !result) {
        return core::Result<BentNormalMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<BentNormalMapArtifact>::failure(validation(
            "bent-normal artifact serialization is not canonical"));
    }
    return core::Result<BentNormalMapArtifact>::success(std::move(artifact));
}

} // namespace carto::production
namespace carto::production {

core::Result<ProductionCookKey> bake_ambient_occlusion_cook_key(
    const AmbientOcclusionRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto base = bake_production_cook_key(
        recipe.binding, std::move(platform_profile));
    if (!base) return core::Result<ProductionCookKey>::failure(base.error());
    const auto digest = recipe.canonical_digest();
    if (!digest) return core::Result<ProductionCookKey>::failure(digest.error());
    ProductionCookKey key = base.value();
    key.algorithm = std::string(kBakeAmbientOcclusionAlgorithm);
    key.settings_digest = digest.value();
    key.representation_kind = "ambient-occlusion-rgba8-v1";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_ambient_occlusion_graph_node(
    ProductionCookGraph& graph,
    const AmbientOcclusionRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_ambient_occlusion_cook_key(
        recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<ProductionCookKey> bake_bent_normal_cook_key(
    const AmbientOcclusionRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto base = bake_production_cook_key(
        recipe.binding, std::move(platform_profile));
    if (!base) return core::Result<ProductionCookKey>::failure(base.error());
    const auto digest = recipe.canonical_digest();
    if (!digest) return core::Result<ProductionCookKey>::failure(digest.error());
    ProductionCookKey key = base.value();
    key.algorithm = std::string(kBakeBentNormalAlgorithm);
    key.settings_digest = hash_text(std::string(kBakeBentNormalAlgorithm) + "\\n" +
        digest.value().hex());
    key.representation_kind = "bent-normal-tangent-rgba8-v1";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_bent_normal_graph_node(
    ProductionCookGraph& graph,
    const AmbientOcclusionRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_bent_normal_cook_key(
        recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<void> publish_ambient_occlusion_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const AmbientOcclusionMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = validate_bake_sources(
            artifact.recipe.binding, current_source, current_target); !result) {
        return result;
    }
    const auto expected_key = bake_ambient_occlusion_cook_key(
        artifact.recipe, std::move(platform_profile));
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "ambient-occlusion receipt does not match the current bake recipe"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const BakeMeshBinding& source = artifact.recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source.identity, source.revision, source.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_authoritative_source, policy);
}

core::Result<AmbientOcclusionMapArtifact> read_ambient_occlusion_map(
    const ProductionArtifactStore& store,
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = validate_bake_sources(
            recipe.binding, current_source, current_target); !result) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(result.error());
    }
    const auto key = bake_ambient_occlusion_cook_key(
        recipe, std::move(platform_profile));
    if (!key) return core::Result<AmbientOcclusionMapArtifact>::failure(key.error());
    const BakeMeshBinding& source = recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source.identity, source.revision, source.content_digest};
    const auto payload = store.read(key.value(), current_authoritative_source, policy);
    if (!payload) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(payload.error());
    }
    auto artifact = AmbientOcclusionMapArtifact::deserialize(payload.value());
    if (!artifact) return artifact;
    if (artifact.value().recipe.serialize() != recipe.serialize()) {
        return core::Result<AmbientOcclusionMapArtifact>::failure(validation(
            "stored ambient-occlusion artifact does not match the requested recipe"));
    }
    return artifact;
}

core::Result<void> publish_bent_normal_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BentNormalMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = validate_bake_sources(
            artifact.recipe.binding, current_source, current_target); !result) {
        return result;
    }
    const auto expected_key = bake_bent_normal_cook_key(
        artifact.recipe, std::move(platform_profile));
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "bent-normal receipt does not match the current bake recipe"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const BakeMeshBinding& source_binding = artifact.recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_authoritative_source, policy);
}

core::Result<BentNormalMapArtifact> read_bent_normal_map(
    const ProductionArtifactStore& store,
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = validate_bake_sources(
            recipe.binding, current_source, current_target); !result) {
        return core::Result<BentNormalMapArtifact>::failure(result.error());
    }
    const auto key = bake_bent_normal_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<BentNormalMapArtifact>::failure(key.error());
    const BakeMeshBinding& source_binding = recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    const auto payload = store.read(key.value(), current_authoritative_source, policy);
    if (!payload) return core::Result<BentNormalMapArtifact>::failure(payload.error());
    auto artifact = BentNormalMapArtifact::deserialize(payload.value());
    if (!artifact) return artifact;
    if (artifact.value().recipe.serialize() != recipe.serialize()) {
        return core::Result<BentNormalMapArtifact>::failure(validation(
            "stored bent-normal artifact does not match the requested bake recipe"));
    }
    return artifact;
}

static AmbientOcclusionExecutionReport compile_ambient_occlusion_cpu_reference(
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    BentNormalMapArtifact* bent_normal_artifact,
    const std::function<bool(geometry::FaceId, std::uint32_t, std::uint32_t,
        std::uint32_t, core::Vec3d, core::Vec3d, double)>& projection_query = {},
    std::vector<detail::AmbientOcclusionProjectionTriangle>* projection_triangles = nullptr,
    std::vector<detail::AmbientOcclusionProjectionRay>* projection_rays = nullptr) {
    AmbientOcclusionExecutionReport report;
    if (auto result = recipe.validate(); !result) {
        report_error(report, "bake_sampling_recipe_invalid", result.error().message);
        return report;
    }
    if (auto result = validate_bake_sources(
            recipe.binding, source, target); !result) {
        report_error(report, "bake_input_invalid", result.error().message);
        return report;
    }
    const auto source_triangles = collect_triangles(source, std::nullopt);
    const auto target_triangles = collect_triangles(
        target, recipe.binding.target_uv_set);
    if (!source_triangles || !target_triangles) {
        const auto& error = !source_triangles ? source_triangles.error() : target_triangles.error();
        report_error(report, "bake_mesh_invalid", error.message);
        return report;
    }
    if (projection_triangles != nullptr) {
        projection_triangles->clear();
        projection_triangles->reserve(source_triangles.value().size());
        for (const Triangle& triangle : source_triangles.value()) {
            projection_triangles->push_back(detail::AmbientOcclusionProjectionTriangle{
                triangle.face.value,
                triangle.fan_index,
                triangle.positions,
            });
        }
    }
    if (projection_rays != nullptr) projection_rays->clear();
    std::vector<std::array<core::Vec4d, 3U>> target_tangent_frames;
    if (bent_normal_artifact != nullptr) {
        const auto generated = generate_target_tangent_frames(
            target, target_triangles.value(), recipe.binding.target_uv_set, report);
        if (!generated) {
            report_error(report, "bent_normal_tangent_frame_invalid",
                generated.error().message);
            return report;
        }
        target_tangent_frames = std::move(generated).value();
    }

    const std::size_t texel_count = static_cast<std::size_t>(recipe.binding.width) *
        recipe.binding.height;
    std::vector<std::uint8_t> pixels(texel_count * 4U, 0U);
    for (std::size_t pixel = 0U; pixel < pixels.size(); pixel += 4U) {
        pixels[pixel] = 255U;
        pixels[pixel + 1U] = 255U;
        pixels[pixel + 2U] = 255U;
    }
    std::vector<std::uint8_t> bent_pixels;
    if (bent_normal_artifact != nullptr) {
        bent_pixels.resize(texel_count * 4U, 0U);
        for (std::size_t pixel = 0U; pixel < bent_pixels.size(); pixel += 4U) {
            bent_pixels[pixel] = 128U;
            bent_pixels[pixel + 1U] = 128U;
            bent_pixels[pixel + 2U] = 255U;
        }
    }
    std::vector<bool> occupied(texel_count, false);
    std::vector<geometry::FaceId> owners(texel_count);
    const TriangleBvh bvh(source_triangles.value());
    std::uint64_t raster_candidates = 0U;
    const double tile_left = static_cast<double>(recipe.binding.tile_u);
    const double tile_bottom = static_cast<double>(recipe.binding.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;
    constexpr double two_pi = 6.283185307179586476925286766559;

    for (std::size_t triangle_index = 0U;
         triangle_index < target_triangles.value().size(); ++triangle_index) {
        Triangle triangle = target_triangles.value()[triangle_index];
        std::array<core::Vec4d, 3U> target_tangents{};
        if (bent_normal_artifact != nullptr) {
            target_tangents = target_tangent_frames[triangle_index];
        }
        const double minimum_u = std::min({triangle.uvs[0U].x,
            triangle.uvs[1U].x, triangle.uvs[2U].x});
        const double maximum_u = std::max({triangle.uvs[0U].x,
            triangle.uvs[1U].x, triangle.uvs[2U].x});
        const double minimum_v = std::min({triangle.uvs[0U].y,
            triangle.uvs[1U].y, triangle.uvs[2U].y});
        const double maximum_v = std::max({triangle.uvs[0U].y,
            triangle.uvs[1U].y, triangle.uvs[2U].y});
        if (maximum_u <= tile_left || minimum_u >= tile_right ||
            maximum_v <= tile_bottom || minimum_v >= tile_top) {
            continue;
        }
        const bool all_in_tile = std::all_of(triangle.uvs.begin(), triangle.uvs.end(),
            [tile_left, tile_right, tile_bottom, tile_top](core::Vec2d uv) {
                return uv.x >= tile_left && uv.x <= tile_right &&
                    uv.y >= tile_bottom && uv.y <= tile_top;
            });
        if (!all_in_tile) {
            report_error(report, "cross_tile_uv_triangle",
                "CPU bake requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }
        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kGeometryEpsilon) {
            report_error(report, "degenerate_target_uv",
                "ambient-occlusion target triangle has a degenerate UV chart", triangle.face);
            continue;
        }
        if (area < 0.0) {
            std::swap(triangle.positions[1U], triangle.positions[2U]);
            std::swap(triangle.normals[1U], triangle.normals[2U]);
            std::swap(triangle.uvs[1U], triangle.uvs[2U]);
            std::swap(target_tangents[1U], target_tangents[2U]);
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
                    report_error(report, "raster_work_budget_exceeded",
                        "target UV rasterization exceeded its deterministic work budget",
                        triangle.face);
                    return report;
                }
                const core::Vec2d sample{
                    tile_left + (static_cast<double>(x) + 0.5) / recipe.binding.width,
                    tile_bottom + (static_cast<double>(y) + 0.5) / recipe.binding.height};
                const double edge_zero = edge_value(
                    triangle.uvs[1U], triangle.uvs[2U], sample);
                const double edge_one = edge_value(
                    triangle.uvs[2U], triangle.uvs[0U], sample);
                const double edge_two = edge_value(
                    triangle.uvs[0U], triangle.uvs[1U], sample);
                const auto inside = [edge_epsilon](double edge, bool inclusive) {
                    return edge > edge_epsilon ||
                        (std::abs(edge) <= edge_epsilon && inclusive);
                };
                if (!inside(edge_zero, top_left_edge(triangle.uvs[1U], triangle.uvs[2U])) ||
                    !inside(edge_one, top_left_edge(triangle.uvs[2U], triangle.uvs[0U])) ||
                    !inside(edge_two, top_left_edge(triangle.uvs[0U], triangle.uvs[1U]))) {
                    continue;
                }
                ++report.target_texels_visited;
                const std::size_t texel = static_cast<std::size_t>(y) *
                    recipe.binding.width + x;
                if (occupied[texel]) {
                    std::ostringstream message;
                    message << "target UV triangles overlap at texel " << x << ',' << y
                            << " (previous face " << owners[texel].value << ')';
                    report_error(report, "overlapping_uv_islands", message.str(),
                        triangle.face, x, y);
                    continue;
                }
                occupied[texel] = true;
                owners[texel] = triangle.face;
                ++report.covered_texels;

                const std::array<double, 3U> barycentric{
                    edge_zero / area, edge_one / area, edge_two / area};
                const core::Vec3d position =
                    triangle.positions[0U] * barycentric[0U] +
                    triangle.positions[1U] * barycentric[1U] +
                    triangle.positions[2U] * barycentric[2U];
                const core::Vec3d normal = (
                    triangle.normals[0U] * barycentric[0U] +
                    triangle.normals[1U] * barycentric[1U] +
                    triangle.normals[2U] * barycentric[2U]).normalized();
                if (!position.finite() || !normal.finite() || normal.length() <= 0.0) {
                    report_error(report, "target_shading_frame_invalid",
                        "ambient-occlusion target interpolation is degenerate",
                        triangle.face, x, y);
                    continue;
                }

                core::Vec3d target_tangent{};
                core::Vec3d target_bitangent{};
                if (bent_normal_artifact != nullptr) {
                    target_tangent = {
                        target_tangents[0U].x * barycentric[0U] +
                            target_tangents[1U].x * barycentric[1U] +
                            target_tangents[2U].x * barycentric[2U],
                        target_tangents[0U].y * barycentric[0U] +
                            target_tangents[1U].y * barycentric[1U] +
                            target_tangents[2U].y * barycentric[2U],
                        target_tangents[0U].z * barycentric[0U] +
                            target_tangents[1U].z * barycentric[1U] +
                            target_tangents[2U].z * barycentric[2U]};
                    target_tangent = (target_tangent - normal *
                        core::dot(normal, target_tangent)).normalized();
                    const double handedness = target_tangents[0U].w * barycentric[0U] +
                        target_tangents[1U].w * barycentric[1U] +
                        target_tangents[2U].w * barycentric[2U];
                    if (!target_tangent.finite() || target_tangent.length() <= 0.0 ||
                        !std::isfinite(handedness) || std::abs(handedness) <= kGeometryEpsilon) {
                        report_error(report, "bent_normal_tangent_frame_invalid",
                            "bent-normal target interpolation produced a degenerate tangent frame",
                            triangle.face, x, y);
                        continue;
                    }
                    target_bitangent = core::cross(normal, target_tangent) *
                        (handedness < 0.0 ? -1.0 : 1.0);
                }

                const core::Vec3d reference_axis = std::abs(normal.z) < 0.999
                    ? core::Vec3d{0.0, 0.0, 1.0}
                    : core::Vec3d{0.0, 1.0, 0.0};
                const core::Vec3d sample_tangent =
                    core::cross(reference_axis, normal).normalized();
                const core::Vec3d sample_bitangent =
                    core::cross(normal, sample_tangent).normalized();
                if (!sample_tangent.finite() || sample_tangent.length() <= 0.0 ||
                    !sample_bitangent.finite() || sample_bitangent.length() <= 0.0) {
                    report_error(report, "bake_sample_frame_invalid",
                        "ambient-occlusion sample frame is degenerate",
                        triangle.face, x, y);
                    continue;
                }

                const std::uint64_t seed_material = recipe.seed ^
                    (triangle.face.value * 0x9e3779b97f4a7c15ULL) ^
                    (static_cast<std::uint64_t>(x) << 32U) ^ y;
                const std::uint32_t scramble = static_cast<std::uint32_t>(
                    mix_ao_seed(seed_material) >> 32U);
                const core::Vec3d origin = position + normal * recipe.origin_bias;
                std::uint64_t occluded_here = 0U;
                core::Vec3d visible_direction_sum{};
                for (std::uint32_t ray = 0U; ray < recipe.sample_count; ++ray) {
                    if (report.rays_cast >= kMaxBakeAmbientOcclusionRays) {
                        report_error(report, "bake_ray_work_budget_exceeded",
                            "deterministic ambient-ray sampling exceeded its work budget",
                            triangle.face, x, y);
                        return report;
                    }
                    const double radial_sample =
                        (static_cast<double>(ray) + 0.5) / recipe.sample_count;
                    const double azimuth_sample = static_cast<double>(
                        reverse_bits(ray ^ scramble)) / 4'294'967'296.0;
                    const double radius = std::sqrt(radial_sample);
                    const double angle = two_pi * azimuth_sample;
                    const double tangent_x = radius * std::cos(angle);
                    const double tangent_y = radius * std::sin(angle);
                    const double normal_z = std::sqrt(1.0 - radial_sample);
                    const core::Vec3d direction = (
                        sample_tangent * tangent_x + sample_bitangent * tangent_y +
                        normal * normal_z
                    ).normalized();
                    if (!direction.finite() || direction.length() <= 0.0) {
                        report_error(report, "bake_ray_direction_invalid",
                            "ambient-occlusion sample generated an invalid direction",
                            triangle.face, x, y);
                        break;
                    }
                    ++report.rays_cast;
                    const bool is_occluded = projection_query
                        ? projection_query(triangle.face, x, y, ray, origin, direction,
                            recipe.max_distance)
                        : bvh.cast(origin, direction, recipe.max_distance,
                            BakeBackfacePolicy::both_sides).hit.has_value();
                    if (projection_rays != nullptr) {
                        projection_rays->push_back(detail::AmbientOcclusionProjectionRay{
                            triangle.face, x, y, ray, origin, direction});
                    }
                    if (is_occluded) {
                        ++report.occluded_rays;
                        ++occluded_here;
                    } else {
                        visible_direction_sum = visible_direction_sum + direction;
                    }
                }
                if (report.diagnostics.size() > 0U &&
                    report.diagnostics.back().is_error) {
                    return report;
                }
                const double visibility = 1.0 -
                    static_cast<double>(occluded_here) / recipe.sample_count;
                const auto encoded = static_cast<std::uint8_t>(std::clamp(
                    std::lround(std::clamp(visibility, 0.0, 1.0) * 255.0), 0L, 255L));
                const std::size_t pixel = texel * 4U;
                pixels[pixel] = encoded;
                pixels[pixel + 1U] = encoded;
                pixels[pixel + 2U] = encoded;
                pixels[pixel + 3U] = 255U;
                if (bent_normal_artifact != nullptr) {
                    core::Vec3d bent_direction = visible_direction_sum.normalized();
                    if (!bent_direction.finite() || bent_direction.length() <= 0.0) {
                        bent_direction = normal;
                    }
                    core::Vec3d local_direction{
                        core::dot(bent_direction, target_tangent),
                        core::dot(bent_direction, target_bitangent),
                        core::dot(bent_direction, normal)};
                    if (recipe.binding.normal_convention ==
                        BakeNormalConvention::directx_y_minus) {
                        local_direction.y = -local_direction.y;
                    }
                    local_direction = local_direction.normalized();
                    if (!local_direction.finite() || local_direction.length() <= 0.0) {
                        report_error(report, "bent_normal_direction_invalid",
                            "bent-normal accumulation produced an invalid direction",
                            triangle.face, x, y);
                        return report;
                    }
                    const auto encode_direction = [](double component) {
                        return static_cast<std::uint8_t>(std::clamp(
                            std::lround((std::clamp(component, -1.0, 1.0) * 0.5 + 0.5) *
                                255.0), 0L, 255L));
                    };
                    bent_pixels[pixel] = encode_direction(local_direction.x);
                    bent_pixels[pixel + 1U] = encode_direction(local_direction.y);
                    bent_pixels[pixel + 2U] = encode_direction(local_direction.z);
                    bent_pixels[pixel + 3U] = 255U;
                }
            }
        }
    }

    if (std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        return report;
    }
    if (report.covered_texels == 0U) {
        report_error(report, "target_uv_no_coverage",
            "selected target UV tile contains no output texels");
        return report;
    }

    if (bent_normal_artifact != nullptr) {
        BentNormalMapArtifact artifact;
        artifact.recipe = recipe;
        artifact.covered_texels = report.covered_texels;
        artifact.rays_cast = report.rays_cast;
        artifact.occluded_rays = report.occluded_rays;
        artifact.rgba8 = std::move(bent_pixels);
        if (auto result = artifact.refresh_digests(); !result) {
            report_error(report, "bent_normal_artifact_validation_failed", result.error().message);
            return report;
        }
        if (auto result = artifact.validate(); !result) {
            report_error(report, "bent_normal_artifact_validation_failed", result.error().message);
            return report;
        }
        *bent_normal_artifact = std::move(artifact);
        return report;
    }

    AmbientOcclusionMapArtifact artifact;
    artifact.recipe = recipe;
    artifact.covered_texels = report.covered_texels;
    artifact.rays_cast = report.rays_cast;
    artifact.occluded_rays = report.occluded_rays;
    artifact.rgba8 = std::move(pixels);
    if (auto result = artifact.refresh_digests(); !result) {
        report_error(report, "artifact_validation_failed", result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        report_error(report, "artifact_validation_failed", result.error().message);
        return report;
    }
    report.artifact = std::move(artifact);
    return report;
}

detail::AmbientOcclusionProjectionWork detail::prepare_ambient_occlusion_projection_work(
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    AmbientOcclusionProjectionWork work;
    work.validation = compile_ambient_occlusion_cpu_reference(
        recipe, source, target, nullptr,
        [](geometry::FaceId, std::uint32_t, std::uint32_t, std::uint32_t,
            core::Vec3d, core::Vec3d, double) { return false; },
        &work.source_triangles, &work.rays);
    return work;
}

AmbientOcclusionExecutionReport detail::compile_ambient_occlusion_from_projection(
    AmbientOcclusionProjectionWork work,
    std::span<const std::uint8_t> hit_flags) {
    AmbientOcclusionExecutionReport report = std::move(work.validation);
    if (!report.succeeded() || !report.artifact.has_value()) return report;
    if (work.rays.size() != hit_flags.size() ||
        work.rays.size() != report.artifact->rays_cast) {
        report_error(report, "d3d12_ao_projection_result_count_invalid",
            "D3D12 AO hit count does not match the prepared canonical ray count");
        return report;
    }

    AmbientOcclusionMapArtifact& artifact = *report.artifact;
    const std::uint32_t sample_count = artifact.recipe.sample_count;
    std::uint64_t occluded_rays = 0U;
    std::uint64_t texels_reconstructed = 0U;
    std::size_t ray_index = 0U;
    while (ray_index < work.rays.size()) {
        const detail::AmbientOcclusionProjectionRay& first = work.rays[ray_index];
        if (first.target_face.value == 0U ||
            first.texel_x >= artifact.recipe.binding.width ||
            first.texel_y >= artifact.recipe.binding.height) {
            report_error(report, "d3d12_ao_projection_identity_invalid",
                "prepared AO ray has an invalid target face or texel identity",
                first.target_face, first.texel_x, first.texel_y);
            return report;
        }
        const std::uint32_t x = first.texel_x;
        const std::uint32_t y = first.texel_y;
        const geometry::FaceId target_face = first.target_face;
        std::uint32_t samples_in_texel = 0U;
        std::uint32_t occluded_in_texel = 0U;
        while (ray_index < work.rays.size()) {
            const detail::AmbientOcclusionProjectionRay& ray = work.rays[ray_index];
            if (ray.target_face != target_face || ray.texel_x != x || ray.texel_y != y) {
                break;
            }
            if (ray.sample_index != samples_in_texel || hit_flags[ray_index] > 1U) {
                report_error(report, "d3d12_ao_projection_identity_invalid",
                    "prepared AO ray samples are duplicated, unordered, or malformed",
                    target_face, x, y);
                return report;
            }
            if (hit_flags[ray_index] != 0U) {
                ++occluded_in_texel;
                ++occluded_rays;
            }
            ++samples_in_texel;
            ++ray_index;
        }
        if (samples_in_texel != sample_count) {
            report_error(report, "d3d12_ao_projection_sample_count_invalid",
                "prepared AO target texel does not contain the requested sample count",
                target_face, x, y);
            return report;
        }
        const std::size_t pixel = (
            static_cast<std::size_t>(y) * artifact.recipe.binding.width + x) * 4U;
        if (pixel + 3U >= artifact.rgba8.size() || artifact.rgba8[pixel + 3U] != 255U) {
            report_error(report, "d3d12_ao_projection_coverage_invalid",
                "prepared AO ray identity points to an uncovered artifact texel",
                target_face, x, y);
            return report;
        }
        const double visibility = 1.0 - static_cast<double>(occluded_in_texel) /
            static_cast<double>(sample_count);
        const auto encoded = static_cast<std::uint8_t>(std::clamp(
            std::lround(std::clamp(visibility, 0.0, 1.0) * 255.0), 0L, 255L));
        artifact.rgba8[pixel] = encoded;
        artifact.rgba8[pixel + 1U] = encoded;
        artifact.rgba8[pixel + 2U] = encoded;
        ++texels_reconstructed;
    }
    if (texels_reconstructed != artifact.covered_texels) {
        report_error(report, "d3d12_ao_projection_coverage_invalid",
            "prepared AO rays do not reconstruct every covered target texel");
        return report;
    }
    artifact.occluded_rays = occluded_rays;
    report.occluded_rays = occluded_rays;
    if (auto result = artifact.refresh_digests(); !result) {
        report_error(report, "artifact_validation_failed", result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        report_error(report, "artifact_validation_failed", result.error().message);
    }
    return report;
}

AmbientOcclusionExecutionReport compile_ambient_occlusion_cpu(
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    return compile_ambient_occlusion_cpu_reference(recipe, source, target, nullptr);
}

BentNormalExecutionReport compile_bent_normal_cpu(
    const AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    BentNormalMapArtifact artifact;
    const auto execution = compile_ambient_occlusion_cpu_reference(
        recipe, source, target, &artifact);
    BentNormalExecutionReport report;
    report.diagnostics = execution.diagnostics;
    report.target_texels_visited = execution.target_texels_visited;
    report.covered_texels = execution.covered_texels;
    report.rays_cast = execution.rays_cast;
    report.occluded_rays = execution.occluded_rays;
    if (artifact.validate() && report.covered_texels > 0U &&
        std::none_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        report.artifact = std::move(artifact);
    }
    return report;
}

} // namespace carto::production
