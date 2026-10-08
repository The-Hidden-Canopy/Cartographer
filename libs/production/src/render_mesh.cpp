#include <carto/production/render_mesh.hpp>

#include <carto/geometry/compiled_mesh.hpp>
#include <carto/project/project.hpp>
#include <carto/render/tangent.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::production {
namespace {

constexpr std::array<std::uint8_t, 12U> kMagic{
    'C', 'A', 'R', 'T', 'O', '_', 'R', 'M', 'E', 'S', 'H', 0U};
constexpr std::uint32_t kFlagUvs = 1U << 0U;
constexpr std::uint32_t kFlagTangents = 1U << 1U;
constexpr std::uint32_t kFlagMaterialSlots = 1U << 2U;
constexpr std::uint32_t kFlagUv1 = 1U << 3U;
constexpr std::uint32_t kFlagColor = 1U << 4U;
constexpr std::uint32_t kKnownFlagsV1 = kFlagUvs | kFlagTangents | kFlagMaterialSlots;
constexpr std::uint32_t kKnownFlags =
    kKnownFlagsV1 | kFlagUv1 | kFlagColor;
constexpr std::size_t kHeaderBytes = 80U;
constexpr std::size_t kBaseVertexBytes = 56U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic not_found(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

bool safe_layer_id(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x21U && byte <= 0x7eU;
        });
}

bool safe_project_identity(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
        });
}

std::optional<std::size_t> encoded_size(
    std::uint32_t vertex_count,
    std::uint32_t index_count,
    std::uint32_t submesh_count,
    std::uint32_t flags,
    std::uint32_t version) {
    if (vertex_count == 0U || vertex_count > kMaxRenderMeshArtifactVertices ||
        index_count < 3U || index_count > kMaxRenderMeshArtifactIndices ||
        index_count % 3U != 0U || submesh_count == 0U ||
        submesh_count > kMaxRenderMeshArtifactSubmeshes) {
        return std::nullopt;
    }
    const std::uint32_t known_flags = version == 1U
        ? kKnownFlagsV1
        : version == kRenderMeshArtifactFormatVersion ? kKnownFlags : 0U;
    if (known_flags == 0U || (flags & ~known_flags) != 0U ||
        ((flags & kFlagTangents) != 0U && (flags & kFlagUvs) == 0U)) {
        return std::nullopt;
    }

    std::uint64_t vertex_bytes = kBaseVertexBytes;
    if ((flags & kFlagUvs) != 0U) vertex_bytes += 16U;
    if ((flags & kFlagUv1) != 0U) vertex_bytes += 16U;
    if ((flags & kFlagColor) != 0U) vertex_bytes += 32U;
    if ((flags & kFlagTangents) != 0U) vertex_bytes += 32U;
    const std::uint64_t submesh_bytes =
        8U + (((flags & kFlagMaterialSlots) != 0U) ? 4U : 0U);
    const std::uint64_t total = static_cast<std::uint64_t>(kHeaderBytes) +
        static_cast<std::uint64_t>(vertex_count) * vertex_bytes +
        static_cast<std::uint64_t>(index_count) * sizeof(std::uint32_t) +
        static_cast<std::uint64_t>(index_count / 3U) * sizeof(std::uint64_t) +
        static_cast<std::uint64_t>(submesh_count) * submesh_bytes;
    if (total > kMaxRenderMeshArtifactBytes ||
        total > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(total);
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_f64(std::vector<std::uint8_t>& output, double value) {
    append_u64(output, std::bit_cast<std::uint64_t>(value));
}

bool read_u32(std::span<const std::uint8_t> bytes, std::size_t& cursor, std::uint32_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(value)) return false;
    value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[cursor++]) << shift;
    }
    return true;
}

bool read_u64(std::span<const std::uint8_t> bytes, std::size_t& cursor, std::uint64_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(value)) return false;
    value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[cursor++]) << shift;
    }
    return true;
}

bool read_f64(std::span<const std::uint8_t> bytes, std::size_t& cursor, double& value) {
    std::uint64_t encoded = 0U;
    if (!read_u64(bytes, cursor, encoded)) return false;
    value = std::bit_cast<double>(encoded);
    return true;
}

struct RenderVertexKey {
    std::uint64_t vertex_id = 0U;
    std::uint64_t uv0_u = 0U;
    std::uint64_t uv0_v = 0U;
    std::uint64_t uv1_u = 0U;
    std::uint64_t uv1_v = 0U;
    std::uint64_t color_r = 0U;
    std::uint64_t color_g = 0U;
    std::uint64_t color_b = 0U;
    std::uint64_t color_a = 0U;

    [[nodiscard]] auto operator<=>(const RenderVertexKey&) const noexcept = default;
};

std::uint64_t canonical_f64_bits(double value) {
    return value == 0.0 ? 0U : std::bit_cast<std::uint64_t>(value);
}

bool same_vec3(core::Vec3d left, core::Vec3d right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

RenderVertexKey render_vertex_key(const RenderMeshVertex& vertex) {
    return {
        vertex.source_vertex.value,
        vertex.uv.has_value() ? canonical_f64_bits(vertex.uv->x) : 0U,
        vertex.uv.has_value() ? canonical_f64_bits(vertex.uv->y) : 0U,
        vertex.uv1.has_value() ? canonical_f64_bits(vertex.uv1->x) : 0U,
        vertex.uv1.has_value() ? canonical_f64_bits(vertex.uv1->y) : 0U,
        vertex.color.has_value() ? canonical_f64_bits(vertex.color->x) : 0U,
        vertex.color.has_value() ? canonical_f64_bits(vertex.color->y) : 0U,
        vertex.color.has_value() ? canonical_f64_bits(vertex.color->z) : 0U,
        vertex.color.has_value() ? canonical_f64_bits(vertex.color->w) : 0U,
    };
}

bool finite_array(std::span<const float> values) {
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

std::optional<float> checked_float(double value) {
    constexpr double lowest = static_cast<double>(std::numeric_limits<float>::lowest());
    constexpr double highest = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < lowest || value > highest) return std::nullopt;
    const float converted = static_cast<float>(value);
    if (!std::isfinite(converted)) return std::nullopt;
    return converted;
}

core::Result<RenderMeshBounds> calculate_bounds(
    const std::vector<RenderMeshVertex>& vertices,
    std::span<const std::uint32_t> indices) {
    if (vertices.empty() || indices.empty()) {
        return core::Result<RenderMeshBounds>::failure(validation(
            "render-mesh bounds require referenced vertices"));
    }
    RenderMeshBounds result;
    for (const std::uint32_t index : indices) {
        if (index >= vertices.size() || !vertices[index].position.finite()) {
            return core::Result<RenderMeshBounds>::failure(validation(
                "render-mesh bounds contain an invalid vertex reference"));
        }
        result.aabb.include(vertices[index].position);
    }
    if (!result.aabb.valid()) {
        return core::Result<RenderMeshBounds>::failure(validation(
            "render-mesh AABB is invalid"));
    }
    result.sphere_center = {
        (result.aabb.minimum.x + result.aabb.maximum.x) * 0.5,
        (result.aabb.minimum.y + result.aabb.maximum.y) * 0.5,
        (result.aabb.minimum.z + result.aabb.maximum.z) * 0.5,
    };
    double radius_squared = 0.0;
    for (const std::uint32_t index : indices) {
        const core::Vec3d offset = vertices[index].position - result.sphere_center;
        radius_squared = std::max(radius_squared, offset.length_squared());
    }
    if (!std::isfinite(radius_squared) || radius_squared < 0.0) {
        return core::Result<RenderMeshBounds>::failure(validation(
            "render-mesh bounding sphere is invalid"));
    }
    result.sphere_radius = std::sqrt(radius_squared);
    if (auto validation_result = result.validate(); !validation_result) {
        return core::Result<RenderMeshBounds>::failure(validation_result.error());
    }
    return core::Result<RenderMeshBounds>::success(result);
}

} // namespace

core::Result<void> RenderMeshBounds::validate() const {
    if (!aabb.valid() || !sphere_center.finite() || !std::isfinite(sphere_radius) ||
        sphere_radius < 0.0 || sphere_center.x < aabb.minimum.x ||
        sphere_center.x > aabb.maximum.x || sphere_center.y < aabb.minimum.y ||
        sphere_center.y > aabb.maximum.y || sphere_center.z < aabb.minimum.z ||
        sphere_center.z > aabb.maximum.z) {
        return core::Result<void>::failure(validation(
            "render-mesh bounds are non-finite or internally inconsistent"));
    }
    return core::Result<void>::success();
}

core::Result<void> RenderMeshArtifact::validate() const {
    if (source_revision.exhausted() || mesh_revision.exhausted() || source_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "render-mesh artifact requires valid source and mesh identity"));
    }
    if (vertices.empty() || vertices.size() > kMaxRenderMeshArtifactVertices ||
        indices.size() < 3U || indices.size() > kMaxRenderMeshArtifactIndices ||
        indices.size() % 3U != 0U || triangle_faces.size() != indices.size() / 3U ||
        submeshes.empty() || submeshes.size() > kMaxRenderMeshArtifactSubmeshes) {
        return core::Result<void>::failure(validation(
            "render-mesh artifact exceeds its bounds or has inconsistent topology"));
    }

    const bool has_uvs = vertices.front().uv.has_value();
    const bool has_uv1 = vertices.front().uv1.has_value();
    const bool has_color = vertices.front().color.has_value();
    const bool has_tangents = vertices.front().tangent.has_value();
    if (has_tangents && !has_uvs) {
        return core::Result<void>::failure(validation(
            "render-mesh tangents require authored UV coordinates"));
    }
    std::map<geometry::VertexId, std::pair<core::Vec3d, core::Vec3d>> source_vertex_data;
    for (const RenderMeshVertex& vertex : vertices) {
        const double normal_length = vertex.normal.length();
        if (!vertex.source_vertex || !vertex.position.finite() || !vertex.normal.finite() ||
            !std::isfinite(normal_length) || std::abs(normal_length - 1.0) > 1e-6 ||
            vertex.uv.has_value() != has_uvs || vertex.tangent.has_value() != has_tangents ||
            vertex.uv1.has_value() != has_uv1 || vertex.color.has_value() != has_color ||
            (vertex.uv.has_value() && !vertex.uv->finite()) ||
            (vertex.uv1.has_value() && !vertex.uv1->finite()) ||
            (vertex.color.has_value() &&
             (!vertex.color->finite() || vertex.color->x < 0.0 || vertex.color->x > 1.0 ||
              vertex.color->y < 0.0 || vertex.color->y > 1.0 ||
              vertex.color->z < 0.0 || vertex.color->z > 1.0 ||
              vertex.color->w < 0.0 || vertex.color->w > 1.0))) {
            return core::Result<void>::failure(validation(
                "render-mesh vertex is invalid or has inconsistent attributes"));
        }
        const auto [source_data, inserted] = source_vertex_data.emplace(
            vertex.source_vertex, std::make_pair(vertex.position, vertex.normal));
        if (!inserted && (!same_vec3(source_data->second.first, vertex.position) ||
                          !same_vec3(source_data->second.second, vertex.normal))) {
            return core::Result<void>::failure(validation(
                "UV-split render vertices disagree about their source vertex geometry"));
        }
        if (vertex.tangent.has_value()) {
            const core::Vec4d tangent = *vertex.tangent;
            const core::Vec3d tangent_direction{tangent.x, tangent.y, tangent.z};
            if (!tangent.finite() || (tangent.w != -1.0 && tangent.w != 1.0) ||
                std::abs(tangent_direction.length() - 1.0) > 1e-5 ||
                std::abs(core::dot(vertex.normal, tangent_direction)) > 1e-5) {
                return core::Result<void>::failure(validation(
                    "render-mesh tangent frame is not finite and orthonormal"));
            }
        }
    }
    std::map<RenderVertexKey, std::uint32_t> canonical_vertices;
    std::uint32_t next_canonical_vertex = 0U;
    for (std::size_t triangle = 0U; triangle < triangle_faces.size(); ++triangle) {
        if (!triangle_faces[triangle]) {
            return core::Result<void>::failure(validation(
                "render-mesh artifact has a zero source-face identity"));
        }
        const std::size_t offset = triangle * 3U;
        const std::uint32_t first = indices[offset];
        const std::uint32_t second = indices[offset + 1U];
        const std::uint32_t third = indices[offset + 2U];
        if (first >= vertices.size() || second >= vertices.size() || third >= vertices.size() ||
            first == second || first == third || second == third) {
            return core::Result<void>::failure(validation(
                "render-mesh artifact contains an invalid triangle index"));
        }
        const core::Vec3d edge_one = vertices[second].position - vertices[first].position;
        const core::Vec3d edge_two = vertices[third].position - vertices[first].position;
        const core::Vec3d area_vector = core::cross(edge_one, edge_two);
        const double area_squared = area_vector.length_squared();
        if (!area_vector.finite() || !std::isfinite(area_squared) || area_squared <= 4e-24) {
            return core::Result<void>::failure(validation(
                "render-mesh artifact contains a degenerate triangle"));
        }
        for (const std::uint32_t vertex_index : {first, second, third}) {
            const RenderVertexKey key = render_vertex_key(vertices[vertex_index]);
            const auto [canonical, inserted] = canonical_vertices.emplace(
                key, next_canonical_vertex);
            if (inserted) {
                if (vertex_index != next_canonical_vertex) {
                    return core::Result<void>::failure(validation(
                        "render-mesh vertices are not in deterministic first-use order"));
                }
                ++next_canonical_vertex;
            } else if (vertex_index != canonical->second) {
                return core::Result<void>::failure(validation(
                    "render-mesh artifact redundantly splits an identical source/UV vertex"));
            }
        }
    }
    if (canonical_vertices.size() != vertices.size()) {
        return core::Result<void>::failure(validation(
            "render-mesh artifact contains unreferenced vertices"));
    }

    const bool has_material_slots = submeshes.front().material_slot.has_value();
    std::uint64_t next_index = 0U;
    std::optional<std::uint32_t> previous_slot;
    for (const RenderMeshSubmesh& submesh : submeshes) {
        if (submesh.index_count == 0U || submesh.index_count % 3U != 0U ||
            submesh.first_index != next_index ||
            submesh.material_slot.has_value() != has_material_slots) {
            return core::Result<void>::failure(validation(
                "render-mesh submesh ranges must be contiguous, non-empty, and consistent"));
        }
        if (has_material_slots) {
            if (previous_slot.has_value() &&
                submesh.material_slot.value() <= previous_slot.value()) {
                return core::Result<void>::failure(validation(
                    "render-mesh material slots must be unique and sorted"));
            }
            previous_slot = submesh.material_slot;
        }
        next_index += submesh.index_count;
    }
    if (next_index != indices.size() || (!has_material_slots && submeshes.size() != 1U)) {
        return core::Result<void>::failure(validation(
            "render-mesh submeshes do not cover the complete index stream"));
    }

    const std::uint32_t flags = (has_uvs ? kFlagUvs : 0U) |
        (has_uv1 ? kFlagUv1 : 0U) |
        (has_color ? kFlagColor : 0U) |
        (has_tangents ? kFlagTangents : 0U) |
        (has_material_slots ? kFlagMaterialSlots : 0U);
    if (!encoded_size(
            static_cast<std::uint32_t>(vertices.size()),
            static_cast<std::uint32_t>(indices.size()),
            static_cast<std::uint32_t>(submeshes.size()), flags,
            kRenderMeshArtifactFormatVersion)) {
        return core::Result<void>::failure(validation(
            "render-mesh artifact exceeds the serialized byte budget"));
    }
    return core::Result<void>::success();
}

core::Result<RenderMeshBounds> RenderMeshArtifact::bounds() const {
    if (auto result = validate(); !result) {
        return core::Result<RenderMeshBounds>::failure(result.error());
    }
    return calculate_bounds(vertices, indices);
}

core::Result<std::vector<RenderMeshBounds>> RenderMeshArtifact::submesh_bounds() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<RenderMeshBounds>>::failure(result.error());
    }
    std::vector<RenderMeshBounds> result;
    result.reserve(submeshes.size());
    const std::span<const std::uint32_t> all_indices{indices};
    for (const RenderMeshSubmesh& submesh : submeshes) {
        const auto bounds_result = calculate_bounds(
            vertices,
            all_indices.subspan(submesh.first_index, submesh.index_count));
        if (!bounds_result) {
            return core::Result<std::vector<RenderMeshBounds>>::failure(
                bounds_result.error());
        }
        result.push_back(bounds_result.value());
    }
    return core::Result<std::vector<RenderMeshBounds>>::success(std::move(result));
}

core::Result<std::vector<std::uint8_t>> RenderMeshArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    const bool has_uvs = vertices.front().uv.has_value();
    const bool has_uv1 = vertices.front().uv1.has_value();
    const bool has_color = vertices.front().color.has_value();
    const bool has_tangents = vertices.front().tangent.has_value();
    const bool has_material_slots = submeshes.front().material_slot.has_value();
    const std::uint32_t flags = (has_uvs ? kFlagUvs : 0U) |
        (has_uv1 ? kFlagUv1 : 0U) |
        (has_color ? kFlagColor : 0U) |
        (has_tangents ? kFlagTangents : 0U) |
        (has_material_slots ? kFlagMaterialSlots : 0U);
    const auto size = encoded_size(
        static_cast<std::uint32_t>(vertices.size()),
        static_cast<std::uint32_t>(indices.size()),
        static_cast<std::uint32_t>(submeshes.size()), flags,
        kRenderMeshArtifactFormatVersion);
    if (!size.has_value()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "render-mesh artifact exceeds the serialized byte budget"));
    }

    std::vector<std::uint8_t> output;
    output.reserve(size.value());
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    append_u32(output, kRenderMeshArtifactFormatVersion);
    append_u32(output, flags);
    append_u64(output, source_revision.value());
    append_u64(output, mesh_revision.value());
    output.insert(output.end(), source_digest.bytes.begin(), source_digest.bytes.end());
    append_u32(output, static_cast<std::uint32_t>(vertices.size()));
    append_u32(output, static_cast<std::uint32_t>(indices.size()));
    append_u32(output, static_cast<std::uint32_t>(submeshes.size()));
    for (const RenderMeshVertex& vertex : vertices) {
        append_u64(output, vertex.source_vertex.value);
        append_f64(output, vertex.position.x);
        append_f64(output, vertex.position.y);
        append_f64(output, vertex.position.z);
        append_f64(output, vertex.normal.x);
        append_f64(output, vertex.normal.y);
        append_f64(output, vertex.normal.z);
        if (has_uvs) {
            append_f64(output, vertex.uv->x);
            append_f64(output, vertex.uv->y);
        }
        if (has_uv1) {
            append_f64(output, vertex.uv1->x);
            append_f64(output, vertex.uv1->y);
        }
        if (has_color) {
            append_f64(output, vertex.color->x);
            append_f64(output, vertex.color->y);
            append_f64(output, vertex.color->z);
            append_f64(output, vertex.color->w);
        }
        if (has_tangents) {
            append_f64(output, vertex.tangent->x);
            append_f64(output, vertex.tangent->y);
            append_f64(output, vertex.tangent->z);
            append_f64(output, vertex.tangent->w);
        }
    }
    for (const std::uint32_t index : indices) append_u32(output, index);
    for (const geometry::FaceId face : triangle_faces) append_u64(output, face.value);
    for (const RenderMeshSubmesh& submesh : submeshes) {
        if (has_material_slots) append_u32(output, submesh.material_slot.value());
        append_u32(output, submesh.first_index);
        append_u32(output, submesh.index_count);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(output));
}

core::Result<RenderMeshArtifact> RenderMeshArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxRenderMeshArtifactBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "render-mesh artifact header or size is invalid"));
    }

    std::size_t cursor = kMagic.size();
    std::uint32_t version = 0U;
    std::uint32_t flags = 0U;
    std::uint64_t source_revision = 0U;
    std::uint64_t mesh_revision = 0U;
    if (!read_u32(bytes, cursor, version) || !read_u32(bytes, cursor, flags) ||
        !read_u64(bytes, cursor, source_revision) ||
        !read_u64(bytes, cursor, mesh_revision)) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "render-mesh artifact header is truncated"));
    }
    const std::uint32_t known_flags = version == 1U
        ? kKnownFlagsV1
        : version == kRenderMeshArtifactFormatVersion ? kKnownFlags : 0U;
    if (known_flags == 0U || (flags & ~known_flags) != 0U ||
        ((flags & kFlagTangents) != 0U && (flags & kFlagUvs) == 0U)) {
        return core::Result<RenderMeshArtifact>::failure(validation(
            "render-mesh artifact version or flags are unsupported"));
    }

    RenderMeshArtifact artifact;
    artifact.source_revision = core::Revision{source_revision};
    artifact.mesh_revision = core::Revision{mesh_revision};
    if (bytes.size() - cursor < artifact.source_digest.bytes.size()) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "render-mesh artifact source digest is truncated"));
    }
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                artifact.source_digest.bytes.size(), artifact.source_digest.bytes.begin());
    cursor += artifact.source_digest.bytes.size();

    std::uint32_t vertex_count = 0U;
    std::uint32_t index_count = 0U;
    std::uint32_t submesh_count = 0U;
    if (!read_u32(bytes, cursor, vertex_count) || !read_u32(bytes, cursor, index_count) ||
        !read_u32(bytes, cursor, submesh_count)) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "render-mesh artifact counts are truncated"));
    }
    const auto expected = encoded_size(
        vertex_count, index_count, submesh_count, flags, version);
    if (!expected.has_value() || expected.value() != bytes.size()) {
        return core::Result<RenderMeshArtifact>::failure(validation(
            "render-mesh artifact counts disagree with its bounded payload"));
    }

    artifact.vertices.reserve(vertex_count);
    for (std::uint32_t index = 0U; index < vertex_count; ++index) {
        RenderMeshVertex vertex;
        std::uint64_t source_vertex = 0U;
        if (!read_u64(bytes, cursor, source_vertex) ||
            !read_f64(bytes, cursor, vertex.position.x) ||
            !read_f64(bytes, cursor, vertex.position.y) ||
            !read_f64(bytes, cursor, vertex.position.z) ||
            !read_f64(bytes, cursor, vertex.normal.x) ||
            !read_f64(bytes, cursor, vertex.normal.y) ||
            !read_f64(bytes, cursor, vertex.normal.z)) {
            return core::Result<RenderMeshArtifact>::failure(invalid(
                "render-mesh artifact vertex data is truncated"));
        }
        vertex.source_vertex = geometry::VertexId{source_vertex};
        if ((flags & kFlagUvs) != 0U) {
            core::Vec2d uv;
            if (!read_f64(bytes, cursor, uv.x) || !read_f64(bytes, cursor, uv.y)) {
                return core::Result<RenderMeshArtifact>::failure(invalid(
                    "render-mesh artifact UV data is truncated"));
            }
            vertex.uv = uv;
        }
        if ((flags & kFlagUv1) != 0U) {
            core::Vec2d uv1;
            if (!read_f64(bytes, cursor, uv1.x) || !read_f64(bytes, cursor, uv1.y)) {
                return core::Result<RenderMeshArtifact>::failure(invalid(
                    "render-mesh artifact UV1 data is truncated"));
            }
            vertex.uv1 = uv1;
        }
        if ((flags & kFlagColor) != 0U) {
            core::Vec4d color;
            if (!read_f64(bytes, cursor, color.x) || !read_f64(bytes, cursor, color.y) ||
                !read_f64(bytes, cursor, color.z) || !read_f64(bytes, cursor, color.w)) {
                return core::Result<RenderMeshArtifact>::failure(invalid(
                    "render-mesh artifact vertex-color data is truncated"));
            }
            vertex.color = color;
        }
        if ((flags & kFlagTangents) != 0U) {
            core::Vec4d tangent;
            if (!read_f64(bytes, cursor, tangent.x) || !read_f64(bytes, cursor, tangent.y) ||
                !read_f64(bytes, cursor, tangent.z) || !read_f64(bytes, cursor, tangent.w)) {
                return core::Result<RenderMeshArtifact>::failure(invalid(
                    "render-mesh artifact tangent data is truncated"));
            }
            vertex.tangent = tangent;
        }
        artifact.vertices.push_back(std::move(vertex));
    }

    artifact.indices.reserve(index_count);
    for (std::uint32_t index = 0U; index < index_count; ++index) {
        std::uint32_t value = 0U;
        if (!read_u32(bytes, cursor, value)) {
            return core::Result<RenderMeshArtifact>::failure(invalid(
                "render-mesh artifact index data is truncated"));
        }
        artifact.indices.push_back(value);
    }
    artifact.triangle_faces.reserve(index_count / 3U);
    for (std::uint32_t index = 0U; index < index_count / 3U; ++index) {
        std::uint64_t value = 0U;
        if (!read_u64(bytes, cursor, value)) {
            return core::Result<RenderMeshArtifact>::failure(invalid(
                "render-mesh artifact face identities are truncated"));
        }
        artifact.triangle_faces.push_back(geometry::FaceId{value});
    }
    artifact.submeshes.reserve(submesh_count);
    for (std::uint32_t index = 0U; index < submesh_count; ++index) {
        RenderMeshSubmesh submesh;
        if ((flags & kFlagMaterialSlots) != 0U) {
            std::uint32_t slot = 0U;
            if (!read_u32(bytes, cursor, slot)) {
                return core::Result<RenderMeshArtifact>::failure(invalid(
                    "render-mesh artifact material slots are truncated"));
            }
            submesh.material_slot = slot;
        }
        if (!read_u32(bytes, cursor, submesh.first_index) ||
            !read_u32(bytes, cursor, submesh.index_count)) {
            return core::Result<RenderMeshArtifact>::failure(invalid(
                "render-mesh artifact submesh ranges are truncated"));
        }
        artifact.submeshes.push_back(submesh);
    }
    if (cursor != bytes.size()) {
        return core::Result<RenderMeshArtifact>::failure(validation(
            "render-mesh artifact contains trailing data"));
    }
    if (auto result = artifact.validate(); !result) {
        return core::Result<RenderMeshArtifact>::failure(result.error());
    }
    return core::Result<RenderMeshArtifact>::success(std::move(artifact));
}

core::Result<void> PbrMeshUpload::validate() const {
    if (source_revision.exhausted() || mesh_revision.exhausted() || source_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "PBR upload requires valid source and mesh identity"));
    }
    if (vertices.empty() || vertices.size() > kMaxRenderMeshArtifactVertices ||
        indices.size() < 3U || indices.size() > kMaxRenderMeshArtifactIndices ||
        indices.size() % 3U != 0U || submeshes.empty() ||
        submeshes.size() > kMaxRenderMeshArtifactSubmeshes) {
        return core::Result<void>::failure(validation(
            "PBR upload exceeds its bounds or has inconsistent topology"));
    }
    for (const PbrUploadVertex& vertex : vertices) {
        if (!finite_array(vertex.position) || !finite_array(vertex.normal) ||
            !finite_array(vertex.uv0) || !finite_array(vertex.tangent) ||
            vertex.position[3U] != 1.0F ||
            (vertex.tangent[3U] != -1.0F && vertex.tangent[3U] != 1.0F)) {
            return core::Result<void>::failure(validation(
                "PBR upload contains invalid or non-finite vertex data"));
        }
        const core::Vec3d normal{
            vertex.normal[0U], vertex.normal[1U], vertex.normal[2U]};
        const core::Vec3d tangent{
            vertex.tangent[0U], vertex.tangent[1U], vertex.tangent[2U]};
        if (std::abs(normal.length() - 1.0) > 1e-4 ||
            std::abs(tangent.length() - 1.0) > 1e-4 ||
            std::abs(core::dot(normal, tangent)) > 1e-4) {
            return core::Result<void>::failure(validation(
                "PBR upload normal and tangent frame is not orthonormal"));
        }
    }
    for (const std::uint32_t index : indices) {
        if (index >= vertices.size()) {
            return core::Result<void>::failure(validation(
                "PBR upload contains an out-of-range index"));
        }
    }

    const bool has_material_slots = submeshes.front().material_slot.has_value();
    std::uint64_t next_index = 0U;
    std::optional<std::uint32_t> previous_slot;
    for (const RenderMeshSubmesh& submesh : submeshes) {
        if (submesh.index_count == 0U || submesh.index_count % 3U != 0U ||
            submesh.first_index != next_index ||
            submesh.material_slot.has_value() != has_material_slots) {
            return core::Result<void>::failure(validation(
                "PBR upload submesh ranges are not contiguous and consistent"));
        }
        if (has_material_slots) {
            if (previous_slot.has_value() &&
                submesh.material_slot.value() <= previous_slot.value()) {
                return core::Result<void>::failure(validation(
                    "PBR upload material slots are not unique and sorted"));
            }
            previous_slot = submesh.material_slot;
        }
        next_index += submesh.index_count;
    }
    if (next_index != indices.size() || (!has_material_slots && submeshes.size() != 1U)) {
        return core::Result<void>::failure(validation(
            "PBR upload submeshes do not cover the complete index stream"));
    }
    return core::Result<void>::success();
}

std::uint64_t PbrMeshUpload::resident_bytes() const noexcept {
    return static_cast<std::uint64_t>(vertices.size()) * sizeof(PbrUploadVertex) +
        static_cast<std::uint64_t>(indices.size()) * sizeof(std::uint32_t);
}

core::Result<PbrMeshUpload> pack_pbr_mesh_upload(const RenderMeshArtifact& artifact) {
    if (auto result = artifact.validate(); !result) {
        return core::Result<PbrMeshUpload>::failure(result.error());
    }

    PbrMeshUpload upload;
    upload.source_revision = artifact.source_revision;
    upload.mesh_revision = artifact.mesh_revision;
    upload.source_digest = artifact.source_digest;
    upload.indices = artifact.indices;
    upload.submeshes = artifact.submeshes;
    upload.vertices.reserve(artifact.vertices.size());

    for (const RenderMeshVertex& source : artifact.vertices) {
        if (!source.uv.has_value() || !source.tangent.has_value()) {
            return core::Result<PbrMeshUpload>::failure(validation(
                "native PBR upload requires authored UV0 and a tangent frame"));
        }
        const std::array<double, 13U> values{
            source.position.x, source.position.y, source.position.z, 1.0,
            source.normal.x, source.normal.y, source.normal.z,
            source.uv->x, source.uv->y,
            source.tangent->x, source.tangent->y, source.tangent->z,
            source.tangent->w,
        };
        std::array<float, 13U> converted{};
        for (std::size_t index = 0U; index < values.size(); ++index) {
            const auto value = checked_float(values[index]);
            if (!value.has_value()) {
                return core::Result<PbrMeshUpload>::failure(validation(
                    "render vertex cannot be represented as finite float32 PBR input"));
            }
            converted[index] = value.value();
        }
        upload.vertices.push_back(PbrUploadVertex{
            {converted[0U], converted[1U], converted[2U], converted[3U]},
            {converted[4U], converted[5U], converted[6U]},
            {converted[7U], converted[8U]},
            {converted[9U], converted[10U], converted[11U], converted[12U]},
        });
    }

    if (auto result = upload.validate(); !result) {
        return core::Result<PbrMeshUpload>::failure(result.error());
    }
    return core::Result<PbrMeshUpload>::success(std::move(upload));
}

core::Result<RenderMeshArtifact> compile_render_mesh_artifact(
    const AuthoritativeSource& source,
    const geometry::EditableMesh& mesh,
    const RenderMeshCompileOptions& options) {
    if (auto result = source.validate(); !result) {
        return core::Result<RenderMeshArtifact>::failure(result.error());
    }
    if (mesh.revision().exhausted()) {
        return core::Result<RenderMeshArtifact>::failure(validation(
            "render-mesh source revision is exhausted"));
    }
    if (options.generate_tangents && !options.uv_set.has_value()) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "tangent generation requires an explicit UV-set identity"));
    }
    if (options.uv1_set.has_value() &&
        (!options.uv_set.has_value() || options.uv1_set == options.uv_set)) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "UV1 requires a distinct explicit UV0-set identity"));
    }
    if (options.material_slot_layer.has_value() &&
        !safe_layer_id(options.material_slot_layer.value())) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "material-slot layer identity is empty, unsafe, or too long"));
    }
    if (options.vertex_color_layer.has_value() &&
        !safe_layer_id(options.vertex_color_layer.value())) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "vertex-color layer identity is empty, unsafe, or too long"));
    }

    const auto compiled_result = mesh.compile();
    if (!compiled_result) {
        return core::Result<RenderMeshArtifact>::failure(compiled_result.error());
    }
    const geometry::CompiledMesh& compiled = compiled_result.value();
    if (compiled.source_revision != mesh.revision() || !compiled.valid()) {
        return core::Result<RenderMeshArtifact>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "compiled mesh is not bound to the current editable-mesh revision"));
    }
    if (compiled.indices.size() < 3U ||
        compiled.indices.size() > kMaxRenderMeshArtifactIndices ||
        compiled.indices.size() % 3U != 0U) {
        return core::Result<RenderMeshArtifact>::failure(validation(
            "source mesh has no triangles or exceeds the render-mesh work bound"));
    }

    using CornerKey = std::pair<geometry::FaceId, geometry::VertexId>;
    using CornerUvs = std::map<CornerKey, core::Vec2d>;
    const auto load_uv_set = [&mesh](
        const std::optional<attributes::UvSetId> set_id,
        std::string_view stream_name) -> core::Result<CornerUvs> {
        CornerUvs result;
        if (!set_id.has_value()) {
            return core::Result<CornerUvs>::success(std::move(result));
        }
        if (set_id.value() == 0U) {
            return core::Result<CornerUvs>::failure(invalid(
                std::string(stream_name) + " UV-set identity must be non-zero"));
        }
        const auto* uv_set = mesh.uv_sets().find(set_id.value());
        if (uv_set == nullptr) {
            return core::Result<CornerUvs>::failure(not_found(
                std::string("requested ") + std::string(stream_name) +
                " UV set is not authored on this mesh"));
        }
        const auto* uv_layer = mesh.attributes().find(uv_set->layer_id);
        if (uv_layer == nullptr ||
            uv_layer->descriptor.domain != attributes::AttributeDomain::corner ||
            uv_layer->descriptor.type != attributes::AttributeType::vec2) {
            return core::Result<CornerUvs>::failure(validation(
                std::string("requested ") + std::string(stream_name) +
                " UV set is not backed by a corner-domain vec2 layer"));
        }
        const auto topology = mesh.topology();
        if (!topology) return core::Result<CornerUvs>::failure(topology.error());
        if (topology.value().corners.size() != uv_layer->values.size()) {
            return core::Result<CornerUvs>::failure(validation(
                std::string(stream_name) +
                " UV payload does not match stable source-corner ordering"));
        }
        for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
            const auto* uv = std::get_if<core::Vec2d>(&uv_layer->values[index]);
            if (uv == nullptr || !uv->finite()) {
                return core::Result<CornerUvs>::failure(validation(
                    std::string(stream_name) +
                    " UV payload contains a wrong-typed or non-finite value"));
            }
            const auto& corner = topology.value().corners[index];
            if (!result.emplace(CornerKey{corner.face, corner.vertex}, *uv).second) {
                return core::Result<CornerUvs>::failure(validation(
                    "source topology contains duplicate face-corner identity"));
            }
        }
        return core::Result<CornerUvs>::success(std::move(result));
    };

    auto uv0_result = load_uv_set(options.uv_set, "UV0");
    if (!uv0_result) return core::Result<RenderMeshArtifact>::failure(uv0_result.error());
    CornerUvs uv_by_corner = std::move(uv0_result).value();
    auto uv1_result = load_uv_set(options.uv1_set, "UV1");
    if (!uv1_result) return core::Result<RenderMeshArtifact>::failure(uv1_result.error());
    CornerUvs uv1_by_corner = std::move(uv1_result).value();

    std::map<CornerKey, core::Vec4d> color_by_corner;
    if (options.vertex_color_layer.has_value()) {
        const auto* layer = mesh.attributes().find(options.vertex_color_layer.value());
        if (layer == nullptr) {
            return core::Result<RenderMeshArtifact>::failure(not_found(
                "requested vertex-color layer is not authored on this mesh"));
        }
        if (layer->descriptor.type != attributes::AttributeType::color4 ||
            (layer->descriptor.domain != attributes::AttributeDomain::vertex &&
             layer->descriptor.domain != attributes::AttributeDomain::corner)) {
            return core::Result<RenderMeshArtifact>::failure(validation(
                "vertex-color layer must be vertex- or corner-domain color4 data"));
        }
        const auto valid_color = [](const core::Vec4d& color) {
            return color.finite() && color.x >= 0.0 && color.x <= 1.0 &&
                color.y >= 0.0 && color.y <= 1.0 &&
                color.z >= 0.0 && color.z <= 1.0 &&
                color.w >= 0.0 && color.w <= 1.0;
        };
        const auto topology = mesh.topology();
        if (!topology) return core::Result<RenderMeshArtifact>::failure(topology.error());
        if (layer->descriptor.domain == attributes::AttributeDomain::corner) {
            if (layer->values.size() != topology.value().corners.size()) {
                return core::Result<RenderMeshArtifact>::failure(validation(
                    "corner colors do not match stable source-corner ordering"));
            }
            for (std::size_t index = 0U; index < layer->values.size(); ++index) {
                const auto* color = std::get_if<core::Vec4d>(&layer->values[index]);
                if (color == nullptr || !valid_color(*color)) {
                    return core::Result<RenderMeshArtifact>::failure(validation(
                        "corner colors contain a wrong-typed or out-of-range value"));
                }
                const auto& corner = topology.value().corners[index];
                if (!color_by_corner.emplace(CornerKey{corner.face, corner.vertex}, *color).second) {
                    return core::Result<RenderMeshArtifact>::failure(validation(
                        "source topology contains duplicate face-corner identity"));
                }
            }
        } else {
            const auto vertices = mesh.vertices_sorted();
            if (layer->values.size() != vertices.size()) {
                return core::Result<RenderMeshArtifact>::failure(validation(
                    "vertex colors do not match stable source-vertex ordering"));
            }
            std::map<geometry::VertexId, core::Vec4d> color_by_vertex;
            for (std::size_t index = 0U; index < vertices.size(); ++index) {
                const auto* color = std::get_if<core::Vec4d>(&layer->values[index]);
                if (color == nullptr || !valid_color(*color)) {
                    return core::Result<RenderMeshArtifact>::failure(validation(
                        "vertex colors contain a wrong-typed or out-of-range value"));
                }
                color_by_vertex.emplace(vertices[index].id, *color);
            }
            for (const auto& corner : topology.value().corners) {
                const auto color = color_by_vertex.find(corner.vertex);
                if (color == color_by_vertex.end() ||
                    !color_by_corner.emplace(
                        CornerKey{corner.face, corner.vertex}, color->second).second) {
                    return core::Result<RenderMeshArtifact>::failure(validation(
                        "vertex colors cannot be mapped to stable source corners"));
                }
            }
        }
    }

    std::map<geometry::FaceId, std::uint32_t> material_slot_by_face;
    if (options.material_slot_layer.has_value()) {
        const auto* layer = mesh.attributes().find(options.material_slot_layer.value());
        if (layer == nullptr) {
            return core::Result<RenderMeshArtifact>::failure(not_found(
                "requested material-slot layer is not authored on this mesh"));
        }
        if (layer->descriptor.domain != attributes::AttributeDomain::face ||
            layer->descriptor.type != attributes::AttributeType::uint32) {
            return core::Result<RenderMeshArtifact>::failure(validation(
                "material-slot layer must be face-domain uint32 data"));
        }
        const auto faces = mesh.faces_sorted();
        if (faces.size() != layer->values.size()) {
            return core::Result<RenderMeshArtifact>::failure(validation(
                "material-slot values do not match stable source-face ordering"));
        }
        for (std::size_t index = 0U; index < faces.size(); ++index) {
            const auto* slot = std::get_if<std::uint32_t>(&layer->values[index]);
            if (slot == nullptr || !material_slot_by_face.emplace(faces[index].id, *slot).second) {
                return core::Result<RenderMeshArtifact>::failure(validation(
                    "material-slot payload contains a wrong-typed or duplicate face value"));
            }
        }
    }

    std::map<std::optional<std::uint32_t>, std::vector<std::size_t>> triangles_by_slot;
    for (std::size_t triangle = 0U; triangle < compiled.triangle_faces.size(); ++triangle) {
        const geometry::FaceId face = compiled.triangle_faces[triangle];
        if (!options.material_slot_layer.has_value()) {
            triangles_by_slot[std::nullopt].push_back(triangle);
            continue;
        }
        const auto slot = material_slot_by_face.find(face);
        if (slot == material_slot_by_face.end()) {
            return core::Result<RenderMeshArtifact>::failure(validation(
                "compiled triangle refers to a face missing its material slot"));
        }
        triangles_by_slot[slot->second].push_back(triangle);
    }
    if (triangles_by_slot.empty() || triangles_by_slot.size() > kMaxRenderMeshArtifactSubmeshes) {
        return core::Result<RenderMeshArtifact>::failure(validation(
            "source mesh exceeds the render-mesh submesh bound"));
    }

    RenderMeshArtifact artifact;
    artifact.source_revision = source.revision;
    artifact.mesh_revision = mesh.revision();
    artifact.source_digest = source.content_digest;
    artifact.indices.reserve(compiled.indices.size());
    artifact.triangle_faces.reserve(compiled.triangle_faces.size());
    std::map<RenderVertexKey, std::uint32_t> vertex_indices;

    for (const auto& [slot, triangles] : triangles_by_slot) {
        const std::uint32_t first_index = static_cast<std::uint32_t>(artifact.indices.size());
        for (const std::size_t triangle : triangles) {
            const geometry::FaceId face = compiled.triangle_faces[triangle];
            const std::size_t source_offset = triangle * 3U;
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const std::uint32_t source_index = compiled.indices[source_offset + corner];
                const geometry::VertexId source_vertex = compiled.vertex_ids[source_index];
                const CornerKey corner_key{face, source_vertex};
                const auto uv_iterator = options.uv_set.has_value()
                    ? uv_by_corner.find(corner_key)
                    : uv_by_corner.end();
                if (options.uv_set.has_value() && uv_iterator == uv_by_corner.end()) {
                    return core::Result<RenderMeshArtifact>::failure(validation(
                        "compiled triangle corner has no authored render UV"));
                }
                const auto uv1_iterator = options.uv1_set.has_value()
                    ? uv1_by_corner.find(corner_key)
                    : uv1_by_corner.end();
                if (options.uv1_set.has_value() && uv1_iterator == uv1_by_corner.end()) {
                    return core::Result<RenderMeshArtifact>::failure(validation(
                        "compiled triangle corner has no authored UV1"));
                }
                const auto color_iterator = options.vertex_color_layer.has_value()
                    ? color_by_corner.find(corner_key)
                    : color_by_corner.end();
                if (options.vertex_color_layer.has_value() &&
                    color_iterator == color_by_corner.end()) {
                    return core::Result<RenderMeshArtifact>::failure(validation(
                        "compiled triangle corner has no authored vertex color"));
                }
                const core::Vec2d uv = uv_iterator == uv_by_corner.end()
                    ? core::Vec2d{}
                    : uv_iterator->second;
                const core::Vec2d uv1 = uv1_iterator == uv1_by_corner.end()
                    ? core::Vec2d{}
                    : uv1_iterator->second;
                const core::Vec4d color = color_iterator == color_by_corner.end()
                    ? core::Vec4d{}
                    : color_iterator->second;
                const RenderVertexKey vertex_key{
                    source_vertex.value,
                    options.uv_set.has_value() ? canonical_f64_bits(uv.x) : 0U,
                    options.uv_set.has_value() ? canonical_f64_bits(uv.y) : 0U,
                    options.uv1_set.has_value() ? canonical_f64_bits(uv1.x) : 0U,
                    options.uv1_set.has_value() ? canonical_f64_bits(uv1.y) : 0U,
                    options.vertex_color_layer.has_value()
                        ? canonical_f64_bits(color.x) : 0U,
                    options.vertex_color_layer.has_value()
                        ? canonical_f64_bits(color.y) : 0U,
                    options.vertex_color_layer.has_value()
                        ? canonical_f64_bits(color.z) : 0U,
                    options.vertex_color_layer.has_value()
                        ? canonical_f64_bits(color.w) : 0U,
                };
                auto found = vertex_indices.find(vertex_key);
                if (found == vertex_indices.end()) {
                    if (artifact.vertices.size() >= kMaxRenderMeshArtifactVertices) {
                        return core::Result<RenderMeshArtifact>::failure(validation(
                            "UV seam expansion exceeds the render-mesh vertex bound"));
                    }
                    const core::Vec3d normal = compiled.normals[source_index].normalized();
                    if (!normal.finite()) {
                        return core::Result<RenderMeshArtifact>::failure(validation(
                            "render-mesh source has a degenerate vertex normal"));
                    }
                    const auto output_index = static_cast<std::uint32_t>(artifact.vertices.size());
                    RenderMeshVertex vertex{
                        source_vertex,
                        compiled.positions[source_index],
                        normal,
                        options.uv_set.has_value()
                            ? std::optional<core::Vec2d>{uv}
                            : std::nullopt,
                        options.uv1_set.has_value()
                            ? std::optional<core::Vec2d>{uv1}
                            : std::nullopt,
                        options.vertex_color_layer.has_value()
                            ? std::optional<core::Vec4d>{color}
                            : std::nullopt,
                        std::nullopt,
                    };
                    artifact.vertices.push_back(std::move(vertex));
                    found = vertex_indices.emplace(vertex_key, output_index).first;
                }
                artifact.indices.push_back(found->second);
            }
            artifact.triangle_faces.push_back(face);
        }
        const std::uint32_t index_count = static_cast<std::uint32_t>(artifact.indices.size()) -
            first_index;
        artifact.submeshes.push_back({slot, first_index, index_count});
    }

    if (options.generate_tangents) {
        std::vector<render::TangentVertex> tangent_vertices;
        tangent_vertices.reserve(artifact.vertices.size());
        for (const RenderMeshVertex& vertex : artifact.vertices) {
            tangent_vertices.push_back({vertex.position, vertex.normal, vertex.uv.value()});
        }
        const auto tangents = render::generate_tangents(tangent_vertices, artifact.indices);
        if (!tangents) {
            return core::Result<RenderMeshArtifact>::failure(tangents.error());
        }
        for (std::size_t index = 0U; index < artifact.vertices.size(); ++index) {
            artifact.vertices[index].tangent = tangents.value()[index];
        }
    }

    if (auto result = artifact.validate(); !result) {
        return core::Result<RenderMeshArtifact>::failure(result.error());
    }
    return core::Result<RenderMeshArtifact>::success(std::move(artifact));
}

core::Result<RenderMeshArtifact> compile_render_mesh_artifact(
    std::string_view project_identity,
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset_id,
    const RenderMeshCompileOptions& options) {
    if (!safe_project_identity(project_identity)) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "project identity must be a bounded non-path identifier"));
    }
    if (mesh_asset_id == 0U) {
        return core::Result<RenderMeshArtifact>::failure(invalid(
            "render-mesh compilation requires a non-zero project mesh asset ID"));
    }
    if (auto result = document.validate(); !result) {
        return core::Result<RenderMeshArtifact>::failure(result.error());
    }
    const auto mesh = document.meshes().find(mesh_asset_id);
    if (mesh == document.meshes().end()) {
        return core::Result<RenderMeshArtifact>::failure(not_found(
            "requested render-mesh asset is not present in the project document"));
    }

    const std::string canonical_document = document.serialize();
    constexpr std::size_t max_project_bytes = 128U * 1024U * 1024U;
    if (canonical_document.empty() || canonical_document.size() > max_project_bytes) {
        return core::Result<RenderMeshArtifact>::failure(validation(
            "canonical project source exceeds the render-mesh source-digest bound"));
    }
    const auto source_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical_document.data()),
        canonical_document.size()});
    const AuthoritativeSource source{
        "project/" + std::string(project_identity),
        document.revision(),
        source_digest,
    };
    return compile_render_mesh_artifact(source, mesh->second, options);
}

} // namespace carto::production
