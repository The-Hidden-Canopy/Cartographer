#include <carto/production/bake_graph.hpp>

#include <carto/project/project.hpp>

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
#include <tuple>
#include <utility>
#include <vector>

namespace carto::production {
namespace {

constexpr std::string_view kHeightDisplacementRecipeMagic =
    "CARTO_HEIGHT_DISPLACEMENT_RECIPE 1";
constexpr std::string_view kHeightDisplacementArtifactMagic =
    "CARTO_BAKE_HEIGHT_DISPLACEMENT_ARTIFACT";
constexpr std::uint32_t kHeightDisplacementArtifactVersion = 1U;
constexpr std::size_t kMaxHeightDisplacementRecipeBytes = 32U * 1024U;
constexpr std::size_t kMaxHeightDisplacementArtifactBytes =
    static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL + 128ULL * 1024ULL);
constexpr std::string_view kThicknessRecipeMagic = "CARTO_THICKNESS_RECIPE 1";
constexpr std::string_view kThicknessArtifactMagic = "CARTO_BAKE_THICKNESS_ARTIFACT";
constexpr std::uint32_t kThicknessArtifactVersion = 1U;
constexpr std::size_t kMaxThicknessRecipeBytes = 32U * 1024U;
constexpr std::size_t kMaxThicknessArtifactBytes =
    static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL + 128ULL * 1024ULL);
constexpr std::string_view kWorldPositionRecipeMagic =
    "CARTO_WORLD_POSITION_RECIPE 1";
constexpr std::string_view kWorldPositionArtifactMagic =
    "CARTO_BAKE_WORLD_POSITION_ARTIFACT";
constexpr std::size_t kMaxWorldPositionRecipeBytes = 32U * 1024U;
constexpr std::size_t kMaxWorldPositionArtifactBytes =
    static_cast<std::size_t>(kMaxWorldPositionTexels * 32ULL + 128ULL * 1024ULL);
constexpr std::size_t kMaxBakeMissSamples = 16U;
constexpr std::size_t kBvhLeafTriangles = 8U;
constexpr double kBakeScalarEpsilon = 1e-12;

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

void append_string(std::vector<std::uint8_t>& bytes, std::string_view value) {
    append_u64(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_digest(std::vector<std::uint8_t>& bytes, const assets::Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
}

void append_sample(
    std::vector<std::uint8_t>& bytes,
    const BakeProjectionMissSample& sample) {
    append_u64(bytes, sample.target_face.value);
    append_u32(bytes, sample.texel_x);
    append_u32(bytes, sample.texel_y);
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

bool world_position_identity_is_safe(std::string_view value) {
    return !value.empty() && value.size() <= 1024U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
                byte != '\r' && byte != '\n';
        });
}

std::optional<std::uint64_t> parse_u64(std::string_view value) {
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

bool same_transform_bits(const core::Transform& left, const core::Transform& right) {
    const std::array<double, 10U> a{
        left.translation.x, left.translation.y, left.translation.z,
        left.rotation.x, left.rotation.y, left.rotation.z, left.rotation.w,
        left.scale.x, left.scale.y, left.scale.z};
    const std::array<double, 10U> b{
        right.translation.x, right.translation.y, right.translation.z,
        right.rotation.x, right.rotation.y, right.rotation.z, right.rotation.w,
        right.scale.x, right.scale.y, right.scale.z};
    for (std::size_t index = 0U; index < a.size(); ++index) {
        if (std::bit_cast<std::uint64_t>(a[index]) !=
            std::bit_cast<std::uint64_t>(b[index])) return false;
    }
    return true;
}

void store_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes[offset + shift / 8U] =
            static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

std::uint64_t load_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void store_double(std::vector<std::uint8_t>& bytes, std::size_t offset, double value) {
    store_u64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

std::vector<std::uint8_t> canonical_world_position_artifact_bytes(
    const WorldPositionMapArtifact& artifact,
    bool include_artifact_digest) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kWorldPositionArtifactMagic);
    append_u32(bytes, kWorldPositionArtifactVersion);
    append_string(bytes, artifact.recipe.serialize());
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, static_cast<std::uint64_t>(artifact.output_bytes.size()));
    bytes.insert(bytes.end(), artifact.output_bytes.begin(), artifact.output_bytes.end());
    if (include_artifact_digest) append_digest(bytes, artifact.artifact_digest);
    return bytes;
}

void add_world_position_error(
    WorldPositionExecutionReport& report,
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
            "diagnostics_truncated", "additional world-position diagnostics were omitted",
            true, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    }
}

std::vector<std::uint8_t> canonical_artifact_bytes(const ThicknessMapArtifact& artifact) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kThicknessArtifactMagic);
    append_u32(bytes, kThicknessArtifactVersion);
    append_string(bytes, artifact.recipe.serialize());
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, artifact.projected_texels);
    append_u64(bytes, artifact.thickness_hits);
    append_u64(bytes, artifact.projection_misses);
    append_u64(bytes, artifact.thickness_misses);
    append_u32(bytes, static_cast<std::uint32_t>(artifact.projection_miss_samples.size()));
    append_u32(bytes, artifact.projection_miss_samples_truncated ? 1U : 0U);
    for (const BakeProjectionMissSample& sample : artifact.projection_miss_samples) {
        append_sample(bytes, sample);
    }
    append_u32(bytes, static_cast<std::uint32_t>(artifact.thickness_miss_samples.size()));
    append_u32(bytes, artifact.thickness_miss_samples_truncated ? 1U : 0U);
    for (const BakeProjectionMissSample& sample : artifact.thickness_miss_samples) {
        append_sample(bytes, sample);
    }
    append_u64(bytes, std::bit_cast<std::uint64_t>(artifact.minimum_thickness));
    append_u64(bytes, std::bit_cast<std::uint64_t>(artifact.maximum_thickness));
    append_u64(bytes, std::bit_cast<std::uint64_t>(artifact.maximum_quantization_error));
    append_u64(bytes, static_cast<std::uint64_t>(artifact.rgba8.size()));
    bytes.insert(bytes.end(), artifact.rgba8.begin(), artifact.rgba8.end());
    return bytes;
}

std::vector<std::uint8_t> canonical_artifact_bytes(
    const HeightDisplacementMapArtifact& artifact) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kHeightDisplacementArtifactMagic);
    append_u32(bytes, kHeightDisplacementArtifactVersion);
    append_string(bytes, artifact.recipe.serialize());
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, artifact.projected_texels);
    append_u64(bytes, artifact.projection_misses);
    append_u32(bytes, static_cast<std::uint32_t>(artifact.projection_miss_samples.size()));
    append_u32(bytes, artifact.projection_miss_samples_truncated ? 1U : 0U);
    for (const BakeProjectionMissSample& sample : artifact.projection_miss_samples) {
        append_sample(bytes, sample);
    }
    append_u64(bytes, artifact.clamped_texels);
    append_u64(bytes, std::bit_cast<std::uint64_t>(artifact.minimum_signed_displacement));
    append_u64(bytes, std::bit_cast<std::uint64_t>(artifact.maximum_signed_displacement));
    append_u64(bytes, std::bit_cast<std::uint64_t>(artifact.maximum_quantization_error));
    append_u64(bytes, static_cast<std::uint64_t>(artifact.rgba8.size()));
    bytes.insert(bytes.end(), artifact.rgba8.begin(), artifact.rgba8.end());
    return bytes;
}

void add_diagnostic(
    ThicknessExecutionReport& report,
    std::string code,
    std::string message,
    bool is_error,
    std::optional<geometry::FaceId> face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    if (report.diagnostics.size() < 64U) {
        report.diagnostics.push_back(BakeDiagnostic{
            std::move(code), std::move(message), is_error, face, std::nullopt, x, y});
    } else if (report.diagnostics.back().code != "diagnostics_truncated") {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated", "additional thickness diagnostics were omitted",
            is_error, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    } else if (is_error) {
        report.diagnostics.back().is_error = true;
    }
}

void add_error(
    ThicknessExecutionReport& report,
    std::string code,
    std::string message,
    std::optional<geometry::FaceId> face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    add_diagnostic(report, std::move(code), std::move(message), true, face, x, y);
}

void add_height_diagnostic(
    HeightDisplacementExecutionReport& report,
    std::string code,
    std::string message,
    bool is_error,
    std::optional<geometry::FaceId> face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    if (report.diagnostics.size() < 64U) {
        report.diagnostics.push_back(BakeDiagnostic{
            std::move(code), std::move(message), is_error, face, std::nullopt, x, y});
    } else if (report.diagnostics.back().code != "diagnostics_truncated") {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated", "additional height/displacement diagnostics were omitted",
            is_error, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    } else if (is_error) {
        report.diagnostics.back().is_error = true;
    }
}

void add_height_error(
    HeightDisplacementExecutionReport& report,
    std::string code,
    std::string message,
    std::optional<geometry::FaceId> face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    add_height_diagnostic(report, std::move(code), std::move(message), true, face, x, y);
}

core::Vec3d component_min(core::Vec3d left, core::Vec3d right) {
    return {std::min(left.x, right.x), std::min(left.y, right.y), std::min(left.z, right.z)};
}

core::Vec3d component_max(core::Vec3d left, core::Vec3d right) {
    return {std::max(left.x, right.x), std::max(left.y, right.y), std::max(left.z, right.z)};
}

struct BakeTriangle {
    std::array<core::Vec3d, 3U> positions{};
    std::array<core::Vec3d, 3U> normals{};
    std::array<core::Vec2d, 3U> uvs{};
    core::Vec3d bounds_min{};
    core::Vec3d bounds_max{};
    geometry::FaceId face;
    std::uint32_t fan_index = 0U;
};

core::Result<std::vector<BakeTriangle>> collect_triangles(
    const geometry::EditableMesh& mesh,
    std::optional<attributes::UvSetId> uv_set) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<std::vector<BakeTriangle>>::failure(result.error());
    }
    if (mesh.vertex_count() == 0U || mesh.face_count() == 0U) {
        return core::Result<std::vector<BakeTriangle>>::failure(validation(
            "thickness mesh must contain vertices and faces"));
    }
    const auto compiled = mesh.compile();
    const auto topology = mesh.topology();
    if (!compiled || !topology) {
        return core::Result<std::vector<BakeTriangle>>::failure(
            compiled ? topology.error() : compiled.error());
    }

    std::map<geometry::VertexId, core::Vec3d> normals;
    for (std::size_t index = 0U; index < compiled.value().vertex_ids.size(); ++index) {
        const core::Vec3d normal = compiled.value().normals[index];
        if (!normal.finite() || normal.length() <= kBakeScalarEpsilon) {
            return core::Result<std::vector<BakeTriangle>>::failure(validation(
                "thickness mesh contains an invalid vertex normal"));
        }
        normals.emplace(compiled.value().vertex_ids[index], normal);
    }

    std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec2d> uv_by_face_vertex;
    if (uv_set) {
        const attributes::UvSetDescriptor* descriptor = mesh.uv_sets().find(*uv_set);
        const attributes::AttributeLayer* layer = descriptor == nullptr
            ? nullptr : mesh.attributes().find(descriptor->layer_id);
        if (descriptor == nullptr || layer == nullptr ||
            layer->descriptor.domain != attributes::AttributeDomain::corner ||
            layer->descriptor.type != attributes::AttributeType::vec2 ||
            layer->values.size() != topology.value().corners.size()) {
            return core::Result<std::vector<BakeTriangle>>::failure(validation(
                "thickness target requires a complete corner-domain vec2 UV set"));
        }
        for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
            const auto* uv = std::get_if<core::Vec2d>(&layer->values[index]);
            if (uv == nullptr || !uv->finite()) {
                return core::Result<std::vector<BakeTriangle>>::failure(validation(
                    "thickness target UV set contains a non-finite coordinate"));
            }
            uv_by_face_vertex.emplace(std::make_pair(
                topology.value().corners[index].face,
                topology.value().corners[index].vertex), *uv);
        }
    }

    std::vector<BakeTriangle> triangles;
    for (const geometry::Face& face : mesh.faces_sorted()) {
        if (face.vertices.size() < 3U) {
            return core::Result<std::vector<BakeTriangle>>::failure(validation(
                "thickness mesh contains a face with fewer than three vertices"));
        }
        for (std::size_t fan = 1U; fan + 1U < face.vertices.size(); ++fan) {
            if (triangles.size() >= kMaxBakeSourceTriangles) {
                return core::Result<std::vector<BakeTriangle>>::failure(validation(
                    "thickness mesh exceeds the CPU reference triangle limit"));
            }
            const std::array<geometry::VertexId, 3U> ids{
                face.vertices[0U], face.vertices[fan], face.vertices[fan + 1U]};
            BakeTriangle triangle;
            triangle.face = face.id;
            triangle.fan_index = static_cast<std::uint32_t>(fan - 1U);
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const geometry::Vertex* vertex = mesh.find_vertex(ids[corner]);
                const auto normal = normals.find(ids[corner]);
                if (vertex == nullptr || normal == normals.end()) {
                    return core::Result<std::vector<BakeTriangle>>::failure(validation(
                        "thickness face references a missing position or normal"));
                }
                triangle.positions[corner] = vertex->position;
                triangle.normals[corner] = normal->second;
                if (uv_set) {
                    const auto uv = uv_by_face_vertex.find({face.id, ids[corner]});
                    if (uv == uv_by_face_vertex.end()) {
                        return core::Result<std::vector<BakeTriangle>>::failure(validation(
                            "thickness target face is missing a corner UV coordinate"));
                    }
                    triangle.uvs[corner] = uv->second;
                }
            }
            const core::Vec3d cross = core::cross(
                triangle.positions[1U] - triangle.positions[0U],
                triangle.positions[2U] - triangle.positions[0U]);
            if (!cross.finite() || cross.length() <= kBakeScalarEpsilon) {
                return core::Result<std::vector<BakeTriangle>>::failure(validation(
                    "thickness mesh contains a degenerate triangle"));
            }
            triangle.bounds_min = component_min(triangle.positions[0U],
                component_min(triangle.positions[1U], triangle.positions[2U]));
            triangle.bounds_max = component_max(triangle.positions[0U],
                component_max(triangle.positions[1U], triangle.positions[2U]));
            triangles.push_back(std::move(triangle));
        }
    }
    if (triangles.empty()) {
        return core::Result<std::vector<BakeTriangle>>::failure(validation(
            "thickness mesh contains no triangles"));
    }
    return core::Result<std::vector<BakeTriangle>>::success(std::move(triangles));
}

struct BakeBvhNode {
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

struct BakeRayHit {
    double distance = std::numeric_limits<double>::infinity();
    std::array<double, 3U> barycentric{};
    std::size_t triangle_index = 0U;
};

class BakeBvh final {
public:
    explicit BakeBvh(const std::vector<BakeTriangle>& triangles)
        : triangles_(triangles) {
        order_.resize(triangles_.size());
        for (std::size_t index = 0U; index < order_.size(); ++index) order_[index] = index;
        nodes_.reserve(triangles_.size() * 2U);
        if (!order_.empty()) static_cast<void>(build(0U, order_.size()));
    }

    [[nodiscard]] std::optional<BakeRayHit> cast(
        core::Vec3d origin,
        core::Vec3d direction,
        double minimum_distance,
        double maximum_distance,
        BakeBackfacePolicy backface_policy) const {
        if (nodes_.empty()) return std::nullopt;
        std::optional<BakeRayHit> best;
        cast_node(0U, origin, direction, minimum_distance, maximum_distance,
            backface_policy, best);
        return best;
    }

private:
    struct Intersection {
        double distance = 0.0;
        std::array<double, 3U> barycentric{};
    };

    [[nodiscard]] bool triangle_precedes(std::size_t first, std::size_t second) const {
        const BakeTriangle& left = triangles_[first];
        const BakeTriangle& right = triangles_[second];
        return left.face < right.face ||
            (left.face == right.face && left.fan_index < right.fan_index);
    }

    [[nodiscard]] std::size_t build(std::size_t begin, std::size_t end) {
        BakeBvhNode node;
        node.begin = begin;
        node.end = end;
        node.bounds_min = triangles_[order_[begin]].bounds_min;
        node.bounds_max = triangles_[order_[begin]].bounds_max;
        for (std::size_t index = begin + 1U; index < end; ++index) {
            node.bounds_min = component_min(node.bounds_min,
                triangles_[order_[index]].bounds_min);
            node.bounds_max = component_max(node.bounds_max,
                triangles_[order_[index]].bounds_max);
        }
        const std::size_t node_index = nodes_.size();
        nodes_.push_back(node);
        if (end - begin <= kBvhLeafTriangles) return node_index;

        const core::Vec3d extent = node.bounds_max - node.bounds_min;
        std::size_t axis = 0U;
        if (extent.y > extent.x && extent.y >= extent.z) axis = 1U;
        else if (extent.z > extent.x && extent.z > extent.y) axis = 2U;
        const auto coordinate = [this, axis](std::size_t index) {
            const BakeTriangle& triangle = triangles_[index];
            const double minimum = axis == 0U ? triangle.bounds_min.x
                : axis == 1U ? triangle.bounds_min.y : triangle.bounds_min.z;
            const double maximum = axis == 0U ? triangle.bounds_max.x
                : axis == 1U ? triangle.bounds_max.y : triangle.bounds_max.z;
            return minimum + (maximum - minimum) * 0.5;
        };
        const auto first = order_.begin() + static_cast<std::ptrdiff_t>(begin);
        const auto middle_iterator = order_.begin() + static_cast<std::ptrdiff_t>(
            begin + (end - begin) / 2U);
        const auto last = order_.begin() + static_cast<std::ptrdiff_t>(end);
        std::nth_element(first, middle_iterator, last,
            [this, &coordinate](std::size_t left, std::size_t right) {
                const double left_center = coordinate(left);
                const double right_center = coordinate(right);
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

    [[nodiscard]] static bool intersects_bounds(
        core::Vec3d origin,
        core::Vec3d direction,
        const BakeBvhNode& node,
        double minimum_distance,
        double maximum_distance) {
        double near_distance = 0.0;
        double far_distance = maximum_distance;
        const std::array<double, 3U> origins{origin.x, origin.y, origin.z};
        const std::array<double, 3U> directions{direction.x, direction.y, direction.z};
        const std::array<double, 3U> lower{
            node.bounds_min.x, node.bounds_min.y, node.bounds_min.z};
        const std::array<double, 3U> upper{
            node.bounds_max.x, node.bounds_max.y, node.bounds_max.z};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (std::abs(directions[axis]) <= std::numeric_limits<double>::min()) {
                if (origins[axis] < lower[axis] || origins[axis] > upper[axis]) return false;
                continue;
            }
            const double reciprocal = 1.0 / directions[axis];
            double first = (lower[axis] - origins[axis]) * reciprocal;
            double second = (upper[axis] - origins[axis]) * reciprocal;
            if (first > second) std::swap(first, second);
            near_distance = std::max(near_distance, first);
            far_distance = std::min(far_distance, second);
            if (far_distance < near_distance) return false;
        }
        return far_distance >= minimum_distance;
    }

    [[nodiscard]] static std::optional<Intersection> intersect_triangle(
        core::Vec3d origin,
        core::Vec3d direction,
        const BakeTriangle& triangle,
        double minimum_distance,
        double maximum_distance,
        BakeBackfacePolicy backface_policy) {
        const core::Vec3d edge_one = triangle.positions[1U] - triangle.positions[0U];
        const core::Vec3d edge_two = triangle.positions[2U] - triangle.positions[0U];
        const core::Vec3d p = core::cross(direction, edge_two);
        const double determinant = core::dot(edge_one, p);
        if (backface_policy == BakeBackfacePolicy::front_only) {
            if (determinant <= kBakeScalarEpsilon) return std::nullopt;
        } else if (std::abs(determinant) <= kBakeScalarEpsilon) {
            return std::nullopt;
        }
        const double inverse = 1.0 / determinant;
        const core::Vec3d from_first = origin - triangle.positions[0U];
        const double u = core::dot(from_first, p) * inverse;
        if (u < -kBakeScalarEpsilon || u > 1.0 + kBakeScalarEpsilon) return std::nullopt;
        const core::Vec3d q = core::cross(from_first, edge_one);
        const double v = core::dot(direction, q) * inverse;
        if (v < -kBakeScalarEpsilon || u + v > 1.0 + kBakeScalarEpsilon) {
            return std::nullopt;
        }
        const double distance = core::dot(edge_two, q) * inverse;
        if (!std::isfinite(distance) || distance < minimum_distance ||
            distance > maximum_distance) return std::nullopt;
        return Intersection{distance, {1.0 - u - v, u, v}};
    }

    void cast_node(
        std::size_t node_index,
        core::Vec3d origin,
        core::Vec3d direction,
        double minimum_distance,
        double maximum_distance,
        BakeBackfacePolicy backface_policy,
        std::optional<BakeRayHit>& best) const {
        const BakeBvhNode& node = nodes_[node_index];
        const double limit = best
            ? std::min(maximum_distance, best->distance + kBakeScalarEpsilon)
            : maximum_distance;
        if (!intersects_bounds(origin, direction, node, minimum_distance, limit)) return;
        if (!node.leaf()) {
            cast_node(node.left, origin, direction, minimum_distance, maximum_distance,
                backface_policy, best);
            cast_node(node.right, origin, direction, minimum_distance, maximum_distance,
                backface_policy, best);
            return;
        }
        for (std::size_t item = node.begin; item < node.end; ++item) {
            const std::size_t triangle_index = order_[item];
            const auto hit = intersect_triangle(origin, direction,
                triangles_[triangle_index], minimum_distance, maximum_distance,
                backface_policy);
            if (!hit) continue;
            if (!best || hit->distance < best->distance - kBakeScalarEpsilon ||
                (std::abs(hit->distance - best->distance) <= kBakeScalarEpsilon &&
                    triangle_precedes(triangle_index, best->triangle_index))) {
                best = BakeRayHit{hit->distance, hit->barycentric, triangle_index};
            }
        }
    }

    const std::vector<BakeTriangle>& triangles_;
    std::vector<std::size_t> order_;
    std::vector<BakeBvhNode> nodes_;
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

} // namespace

core::Result<void> ThicknessRecipe::validate() const {
    if (auto result = binding.validate(); !result) return result;
    if (!std::isfinite(maximum_distance) || maximum_distance <= 0.0 ||
        maximum_distance > 1'000'000.0 || !std::isfinite(surface_bias) ||
        surface_bias <= 0.0 || surface_bias >= maximum_distance * 0.25) {
        return core::Result<void>::failure(invalid(
            "thickness distance and surface bias must be finite, positive, and bounded"));
    }
    return core::Result<void>::success();
}

std::string ThicknessRecipe::serialize() const {
    if (!validate()) return {};
    const std::string text = std::string(kThicknessRecipeMagic) + "\n" +
        "binding.hex=" + hex_encode(binding.serialize()) + "\n" +
        "maximum.distance.bits=" + double_bits_hex(maximum_distance) + "\n" +
        "surface.bias.bits=" + double_bits_hex(surface_bias) + "\n" +
        "reject.thickness.misses=" + (reject_thickness_misses ? "1\n" : "0\n");
    return text.size() <= kMaxThicknessRecipeBytes ? text : std::string{};
}

core::Result<assets::Sha256Digest> ThicknessRecipe::canonical_digest() const {
    const std::string text = serialize();
    if (text.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid or oversized thickness recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(text));
}

core::Result<ThicknessRecipe> ThicknessRecipe::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxThicknessRecipeBytes) {
        return core::Result<ThicknessRecipe>::failure(invalid(
            "thickness recipe byte size is invalid"));
    }
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || line != kThicknessRecipeMagic) {
        return core::Result<ThicknessRecipe>::failure(validation(
            "thickness recipe header is unsupported"));
    }
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<ThicknessRecipe>::failure(validation(
                "thickness recipe contains a malformed or duplicate field"));
        }
    }
    const auto binding_hex = fields.find("binding.hex");
    const auto distance_hex = fields.find("maximum.distance.bits");
    const auto bias_hex = fields.find("surface.bias.bits");
    const auto reject = fields.find("reject.thickness.misses");
    if (fields.size() != 4U || binding_hex == fields.end() ||
        distance_hex == fields.end() || bias_hex == fields.end() || reject == fields.end() ||
        (reject->second != "0" && reject->second != "1")) {
        return core::Result<ThicknessRecipe>::failure(validation(
            "thickness recipe does not contain the exact supported field set"));
    }
    const auto binding_text = hex_decode(binding_hex->second, kMaxBakeRecipeBytes);
    const auto distance = parse_double_bits(distance_hex->second);
    const auto bias = parse_double_bits(bias_hex->second);
    if (!binding_text || !distance || !bias) {
        return core::Result<ThicknessRecipe>::failure(validation(
            "thickness recipe binding or metric settings are malformed"));
    }
    const auto binding = BakeRecipe::deserialize(*binding_text);
    if (!binding) return core::Result<ThicknessRecipe>::failure(binding.error());
    ThicknessRecipe recipe{binding.value(), *distance, *bias, reject->second == "1"};
    if (auto result = recipe.validate(); !result) {
        return core::Result<ThicknessRecipe>::failure(result.error());
    }
    if (recipe.serialize() != text) {
        return core::Result<ThicknessRecipe>::failure(validation(
            "thickness recipe serialization is not canonical"));
    }
    return core::Result<ThicknessRecipe>::success(std::move(recipe));
}

core::Result<void> ThicknessMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != kBakeThicknessAlgorithm) {
        return core::Result<void>::failure(invalid(
            "thickness artifact algorithm is unsupported"));
    }
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    if (rgba8.size() != texels * 4ULL || covered_texels == 0U ||
        covered_texels > texels || projected_texels + projection_misses != covered_texels ||
        thickness_hits + thickness_misses != projected_texels || thickness_hits == 0U ||
        projection_miss_samples.size() > kMaxBakeMissSamples ||
        projection_miss_samples.size() > projection_misses ||
        thickness_miss_samples.size() > kMaxBakeMissSamples ||
        thickness_miss_samples.size() > thickness_misses) {
        return core::Result<void>::failure(validation(
            "thickness artifact dimensions or coverage accounting are inconsistent"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return core::Result<void>::failure(recipe_digest.error());
    settings_digest = hash_text(std::string(kBakeThicknessAlgorithm) + "\n" +
        recipe_digest.value().hex());
    maximum_quantization_error = recipe.maximum_distance / 510.0;
    output_digest = hash_bytes(rgba8);
    artifact_digest = hash_bytes(canonical_artifact_bytes(*this));
    return core::Result<void>::success();
}

core::Result<void> ThicknessMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    if (algorithm != kBakeThicknessAlgorithm || settings_digest.is_zero() ||
        output_digest.is_zero() || artifact_digest.is_zero() ||
        rgba8.size() != texels * 4ULL || covered_texels == 0U || covered_texels > texels ||
        projected_texels + projection_misses != covered_texels ||
        thickness_hits + thickness_misses != projected_texels || thickness_hits == 0U ||
        projection_miss_samples.size() > kMaxBakeMissSamples ||
        projection_miss_samples.size() > projection_misses ||
        thickness_miss_samples.size() > kMaxBakeMissSamples ||
        thickness_miss_samples.size() > thickness_misses ||
        !std::isfinite(minimum_thickness) || !std::isfinite(maximum_thickness) ||
        minimum_thickness < 0.0 || minimum_thickness > maximum_thickness ||
        maximum_thickness > recipe.maximum_distance ||
        !std::isfinite(maximum_quantization_error) ||
        std::abs(maximum_quantization_error - recipe.maximum_distance / 510.0) > 1e-15) {
        return core::Result<void>::failure(validation(
            "thickness artifact metadata or coverage accounting is invalid"));
    }
    const auto validate_misses = [this](
        const std::vector<BakeProjectionMissSample>& samples,
        bool truncated,
        std::uint64_t misses,
        bool rejects,
        std::string_view kind) -> core::Result<void> {
        if (misses == 0U) {
            if (!samples.empty() || truncated) {
                return core::Result<void>::failure(validation(
                    std::string(kind) + " miss evidence exists without missed texels"));
            }
            return core::Result<void>::success();
        }
        if (rejects || samples.empty() || truncated != (samples.size() < misses)) {
            return core::Result<void>::failure(validation(
                std::string(kind) + " miss evidence conflicts with the recipe"));
        }
        std::set<std::array<std::uint64_t, 3U>> unique;
        for (const BakeProjectionMissSample& sample : samples) {
            if (!sample.target_face || sample.texel_x >= recipe.binding.width ||
                sample.texel_y >= recipe.binding.height ||
                !unique.insert({sample.target_face.value, sample.texel_x,
                    sample.texel_y}).second) {
                return core::Result<void>::failure(validation(
                    std::string(kind) + " contains invalid or duplicate miss evidence"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_misses(projection_miss_samples,
            projection_miss_samples_truncated, projection_misses,
            recipe.binding.reject_projection_misses, "projection"); !result) return result;
    if (auto result = validate_misses(thickness_miss_samples,
            thickness_miss_samples_truncated, thickness_misses,
            recipe.reject_thickness_misses, "thickness"); !result) return result;
    for (const BakeProjectionMissSample& projection : projection_miss_samples) {
        if (std::any_of(thickness_miss_samples.begin(), thickness_miss_samples.end(),
                [&projection](const BakeProjectionMissSample& thickness) {
                    return projection.target_face == thickness.target_face &&
                        projection.texel_x == thickness.texel_x &&
                        projection.texel_y == thickness.texel_y;
                })) {
            return core::Result<void>::failure(validation(
                "thickness texel cannot be both a projection miss and an exit miss"));
        }
    }

    std::uint64_t opaque_texels = 0U;
    for (std::size_t pixel = 0U; pixel < rgba8.size(); pixel += 4U) {
        const std::uint8_t alpha = rgba8[pixel + 3U];
        if (alpha == 255U) {
            ++opaque_texels;
            if (rgba8[pixel] != rgba8[pixel + 1U] ||
                rgba8[pixel] != rgba8[pixel + 2U]) {
                return core::Result<void>::failure(validation(
                    "thickness output must be grayscale"));
            }
        } else if (alpha != 0U || rgba8[pixel] != 0U || rgba8[pixel + 1U] != 0U ||
            rgba8[pixel + 2U] != 0U) {
            return core::Result<void>::failure(validation(
                "thickness uncovered texel encoding is invalid"));
        }
    }
    if (opaque_texels != thickness_hits) {
        return core::Result<void>::failure(validation(
            "thickness alpha coverage disagrees with its receipt"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest || hash_text(std::string(kBakeThicknessAlgorithm) + "\n" +
            recipe_digest.value().hex()) != settings_digest ||
        hash_bytes(rgba8) != output_digest ||
        hash_bytes(canonical_artifact_bytes(*this)) != artifact_digest) {
        return core::Result<void>::failure(validation(
            "thickness artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> ThicknessMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_artifact_bytes(*this);
    append_digest(bytes, artifact_digest);
    if (bytes.size() > kMaxThicknessArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "thickness artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<ThicknessMapArtifact> ThicknessMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxThicknessArtifactBytes) {
        return core::Result<ThicknessMapArtifact>::failure(invalid(
            "thickness artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string recipe_text;
    std::string algorithm_value;
    std::uint32_t projection_sample_count = 0U;
    std::uint32_t projection_truncated = 0U;
    std::uint32_t thickness_sample_count = 0U;
    std::uint32_t thickness_truncated = 0U;
    std::uint64_t minimum_bits = 0U;
    std::uint64_t maximum_bits = 0U;
    std::uint64_t quantization_bits = 0U;
    if (!reader.read_string(magic, kThicknessArtifactMagic.size()) ||
        magic != kThicknessArtifactMagic || !reader.read_u32(version) ||
        version != kThicknessArtifactVersion ||
        !reader.read_string(recipe_text, kMaxThicknessRecipeBytes) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<ThicknessMapArtifact>::failure(validation(
            "thickness artifact header or recipe is malformed"));
    }
    const auto recipe = ThicknessRecipe::deserialize(recipe_text);
    if (!recipe) return core::Result<ThicknessMapArtifact>::failure(recipe.error());
    ThicknessMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.algorithm = std::move(algorithm_value);
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_u64(artifact.projected_texels) ||
        !reader.read_u64(artifact.thickness_hits) ||
        !reader.read_u64(artifact.projection_misses) ||
        !reader.read_u64(artifact.thickness_misses) ||
        !reader.read_u32(projection_sample_count) ||
        projection_sample_count > kMaxBakeMissSamples ||
        !reader.read_u32(projection_truncated) || projection_truncated > 1U) {
        return core::Result<ThicknessMapArtifact>::failure(validation(
            "thickness artifact metrics or projection miss count are malformed"));
    }
    for (std::uint32_t index = 0U; index < projection_sample_count; ++index) {
        std::uint64_t face_id = 0U;
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        if (!reader.read_u64(face_id) || !reader.read_u32(x) || !reader.read_u32(y)) {
            return core::Result<ThicknessMapArtifact>::failure(validation(
                "thickness projection miss sample is truncated"));
        }
        artifact.projection_miss_samples.push_back({geometry::FaceId{face_id}, x, y});
    }
    if (!reader.read_u32(thickness_sample_count) ||
        thickness_sample_count > kMaxBakeMissSamples ||
        !reader.read_u32(thickness_truncated) || thickness_truncated > 1U) {
        return core::Result<ThicknessMapArtifact>::failure(validation(
            "thickness exit miss count is malformed"));
    }
    for (std::uint32_t index = 0U; index < thickness_sample_count; ++index) {
        std::uint64_t face_id = 0U;
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        if (!reader.read_u64(face_id) || !reader.read_u32(x) || !reader.read_u32(y)) {
            return core::Result<ThicknessMapArtifact>::failure(validation(
                "thickness exit miss sample is truncated"));
        }
        artifact.thickness_miss_samples.push_back({geometry::FaceId{face_id}, x, y});
    }
    if (!reader.read_u64(minimum_bits) || !reader.read_u64(maximum_bits) ||
        !reader.read_u64(quantization_bits) ||
        !reader.read_payload(artifact.rgba8,
            static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<ThicknessMapArtifact>::failure(validation(
            "thickness artifact payload is truncated, oversized, or has trailing bytes"));
    }
    artifact.projection_miss_samples_truncated = projection_truncated != 0U;
    artifact.thickness_miss_samples_truncated = thickness_truncated != 0U;
    artifact.minimum_thickness = std::bit_cast<double>(minimum_bits);
    artifact.maximum_thickness = std::bit_cast<double>(maximum_bits);
    artifact.maximum_quantization_error = std::bit_cast<double>(quantization_bits);
    if (auto result = artifact.validate(); !result) {
        return core::Result<ThicknessMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<ThicknessMapArtifact>::failure(validation(
            "thickness artifact serialization is not canonical"));
    }
    return core::Result<ThicknessMapArtifact>::success(std::move(artifact));
}

core::Result<void> HeightDisplacementRecipe::validate() const {
    if (auto result = binding.validate(); !result) return result;
    if (!std::isfinite(normalization_distance) || normalization_distance <= 0.0 ||
        normalization_distance > 1'000'000.0) {
        return core::Result<void>::failure(invalid(
            "height/displacement normalization distance must be finite, positive, and bounded"));
    }
    return core::Result<void>::success();
}

std::string HeightDisplacementRecipe::serialize() const {
    if (!validate()) return {};
    const std::string text = std::string(kHeightDisplacementRecipeMagic) + "\n" +
        "binding.hex=" + hex_encode(binding.serialize()) + "\n" +
        "normalization.distance.bits=" + double_bits_hex(normalization_distance) + "\n" +
        "reject.clamped.samples=" + (reject_clamped_samples ? "1\n" : "0\n");
    return text.size() <= kMaxHeightDisplacementRecipeBytes ? text : std::string{};
}

core::Result<assets::Sha256Digest> HeightDisplacementRecipe::canonical_digest() const {
    const std::string text = serialize();
    if (text.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid or oversized height/displacement recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(text));
}

core::Result<HeightDisplacementRecipe> HeightDisplacementRecipe::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxHeightDisplacementRecipeBytes) {
        return core::Result<HeightDisplacementRecipe>::failure(invalid(
            "height/displacement recipe byte size is invalid"));
    }
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || line != kHeightDisplacementRecipeMagic) {
        return core::Result<HeightDisplacementRecipe>::failure(validation(
            "height/displacement recipe header is unsupported"));
    }
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<HeightDisplacementRecipe>::failure(validation(
                "height/displacement recipe contains a malformed or duplicate field"));
        }
    }
    const auto binding_hex = fields.find("binding.hex");
    const auto normalization_hex = fields.find("normalization.distance.bits");
    const auto reject = fields.find("reject.clamped.samples");
    if (fields.size() != 3U || binding_hex == fields.end() ||
        normalization_hex == fields.end() || reject == fields.end() ||
        (reject->second != "0" && reject->second != "1")) {
        return core::Result<HeightDisplacementRecipe>::failure(validation(
            "height/displacement recipe does not contain the exact supported field set"));
    }
    const auto binding_text = hex_decode(binding_hex->second, kMaxBakeRecipeBytes);
    const auto normalization = parse_double_bits(normalization_hex->second);
    if (!binding_text || !normalization) {
        return core::Result<HeightDisplacementRecipe>::failure(validation(
            "height/displacement recipe binding or normalization is malformed"));
    }
    const auto binding = BakeRecipe::deserialize(*binding_text);
    if (!binding) return core::Result<HeightDisplacementRecipe>::failure(binding.error());
    HeightDisplacementRecipe recipe{
        binding.value(), *normalization, reject->second == "1"};
    if (auto result = recipe.validate(); !result) {
        return core::Result<HeightDisplacementRecipe>::failure(result.error());
    }
    if (recipe.serialize() != text) {
        return core::Result<HeightDisplacementRecipe>::failure(validation(
            "height/displacement recipe serialization is not canonical"));
    }
    return core::Result<HeightDisplacementRecipe>::success(std::move(recipe));
}

core::Result<void> HeightDisplacementMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != kBakeHeightDisplacementAlgorithm) {
        return core::Result<void>::failure(invalid(
            "height/displacement artifact algorithm is unsupported"));
    }
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    if (rgba8.size() != texels * 4ULL || covered_texels == 0U ||
        covered_texels > texels || projected_texels + projection_misses != covered_texels ||
        projected_texels == 0U || projection_miss_samples.size() > kMaxBakeMissSamples ||
        projection_miss_samples.size() > projection_misses ||
        clamped_texels > projected_texels) {
        return core::Result<void>::failure(validation(
            "height/displacement artifact dimensions or coverage accounting are inconsistent"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return core::Result<void>::failure(recipe_digest.error());
    settings_digest = hash_text(std::string(kBakeHeightDisplacementAlgorithm) + "\n" +
        recipe_digest.value().hex());
    maximum_quantization_error = recipe.normalization_distance / 255.0;
    output_digest = hash_bytes(rgba8);
    artifact_digest = hash_bytes(canonical_artifact_bytes(*this));
    return core::Result<void>::success();
}

core::Result<void> HeightDisplacementMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t texels = static_cast<std::uint64_t>(
        recipe.binding.width) * recipe.binding.height;
    if (algorithm != kBakeHeightDisplacementAlgorithm || settings_digest.is_zero() ||
        output_digest.is_zero() || artifact_digest.is_zero() ||
        rgba8.size() != texels * 4ULL || covered_texels == 0U || covered_texels > texels ||
        projected_texels == 0U || projected_texels + projection_misses != covered_texels ||
        projection_miss_samples.size() > kMaxBakeMissSamples ||
        projection_miss_samples.size() > projection_misses || clamped_texels > projected_texels ||
        (recipe.reject_clamped_samples && clamped_texels > 0U) ||
        !std::isfinite(minimum_signed_displacement) ||
        !std::isfinite(maximum_signed_displacement) ||
        minimum_signed_displacement > maximum_signed_displacement ||
        (clamped_texels == 0U &&
            (minimum_signed_displacement < -recipe.normalization_distance ||
             maximum_signed_displacement > recipe.normalization_distance)) ||
        (clamped_texels > 0U &&
            minimum_signed_displacement >= -recipe.normalization_distance &&
            maximum_signed_displacement <= recipe.normalization_distance) ||
        !std::isfinite(maximum_quantization_error) ||
        std::abs(maximum_quantization_error - recipe.normalization_distance / 255.0) > 1e-15) {
        return core::Result<void>::failure(validation(
            "height/displacement artifact metadata or coverage accounting is invalid"));
    }
    if (projection_misses == 0U) {
        if (!projection_miss_samples.empty() || projection_miss_samples_truncated) {
            return core::Result<void>::failure(validation(
                "height/displacement miss evidence exists without missed texels"));
        }
    } else if (recipe.binding.reject_projection_misses || projection_miss_samples.empty() ||
        projection_miss_samples_truncated !=
            (projection_miss_samples.size() < projection_misses)) {
        return core::Result<void>::failure(validation(
            "height/displacement miss evidence conflicts with the recipe"));
    }
    std::set<std::array<std::uint64_t, 3U>> unique_misses;
    for (const BakeProjectionMissSample& sample : projection_miss_samples) {
        if (!sample.target_face || sample.texel_x >= recipe.binding.width ||
            sample.texel_y >= recipe.binding.height ||
            !unique_misses.insert({sample.target_face.value, sample.texel_x,
                sample.texel_y}).second) {
            return core::Result<void>::failure(validation(
                "height/displacement artifact contains invalid or duplicate miss evidence"));
        }
    }
    std::uint64_t opaque_texels = 0U;
    for (std::size_t pixel = 0U; pixel < rgba8.size(); pixel += 4U) {
        const std::uint8_t alpha = rgba8[pixel + 3U];
        if (alpha == 255U) {
            ++opaque_texels;
            if (rgba8[pixel] != rgba8[pixel + 1U] ||
                rgba8[pixel] != rgba8[pixel + 2U]) {
                return core::Result<void>::failure(validation(
                    "height/displacement output must be grayscale"));
            }
        } else if (alpha != 0U || rgba8[pixel] != 128U ||
            rgba8[pixel + 1U] != 128U || rgba8[pixel + 2U] != 128U) {
            return core::Result<void>::failure(validation(
                "height/displacement uncovered texel encoding is invalid"));
        }
    }
    if (opaque_texels != projected_texels) {
        return core::Result<void>::failure(validation(
            "height/displacement alpha coverage disagrees with its receipt"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest || hash_text(std::string(kBakeHeightDisplacementAlgorithm) + "\n" +
            recipe_digest.value().hex()) != settings_digest ||
        hash_bytes(rgba8) != output_digest ||
        hash_bytes(canonical_artifact_bytes(*this)) != artifact_digest) {
        return core::Result<void>::failure(validation(
            "height/displacement artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> HeightDisplacementMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes = canonical_artifact_bytes(*this);
    append_digest(bytes, artifact_digest);
    if (bytes.size() > kMaxHeightDisplacementArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "height/displacement artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<HeightDisplacementMapArtifact> HeightDisplacementMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxHeightDisplacementArtifactBytes) {
        return core::Result<HeightDisplacementMapArtifact>::failure(invalid(
            "height/displacement artifact byte size is invalid"));
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
    if (!reader.read_string(magic, kHeightDisplacementArtifactMagic.size()) ||
        magic != kHeightDisplacementArtifactMagic || !reader.read_u32(version) ||
        version != kHeightDisplacementArtifactVersion ||
        !reader.read_string(recipe_text, kMaxHeightDisplacementRecipeBytes) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<HeightDisplacementMapArtifact>::failure(validation(
            "height/displacement artifact header or recipe is malformed"));
    }
    const auto recipe = HeightDisplacementRecipe::deserialize(recipe_text);
    if (!recipe) return core::Result<HeightDisplacementMapArtifact>::failure(recipe.error());
    HeightDisplacementMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.algorithm = std::move(algorithm_value);
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_u64(artifact.projected_texels) ||
        !reader.read_u64(artifact.projection_misses) ||
        !reader.read_u32(miss_count) || miss_count > kMaxBakeMissSamples ||
        !reader.read_u32(truncated) || truncated > 1U) {
        return core::Result<HeightDisplacementMapArtifact>::failure(validation(
            "height/displacement artifact metrics or miss count are malformed"));
    }
    for (std::uint32_t index = 0U; index < miss_count; ++index) {
        std::uint64_t face_id = 0U;
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        if (!reader.read_u64(face_id) || !reader.read_u32(x) || !reader.read_u32(y)) {
            return core::Result<HeightDisplacementMapArtifact>::failure(validation(
                "height/displacement projection miss sample is truncated"));
        }
        artifact.projection_miss_samples.push_back({geometry::FaceId{face_id}, x, y});
    }
    if (!reader.read_u64(artifact.clamped_texels) ||
        !reader.read_u64(minimum_bits) || !reader.read_u64(maximum_bits) ||
        !reader.read_u64(quantization_bits) ||
        !reader.read_payload(artifact.rgba8,
            static_cast<std::size_t>(kMaxBakeTextureTexels * 4ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<HeightDisplacementMapArtifact>::failure(validation(
            "height/displacement artifact payload is truncated, oversized, or has trailing bytes"));
    }
    artifact.projection_miss_samples_truncated = truncated != 0U;
    artifact.minimum_signed_displacement = std::bit_cast<double>(minimum_bits);
    artifact.maximum_signed_displacement = std::bit_cast<double>(maximum_bits);
    artifact.maximum_quantization_error = std::bit_cast<double>(quantization_bits);
    if (auto result = artifact.validate(); !result) {
        return core::Result<HeightDisplacementMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<HeightDisplacementMapArtifact>::failure(validation(
            "height/displacement artifact serialization is not canonical"));
    }
    return core::Result<HeightDisplacementMapArtifact>::success(std::move(artifact));
}

core::Result<ProductionCookKey> bake_thickness_cook_key(
    const ThicknessRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto base = bake_production_cook_key(recipe.binding, std::move(platform_profile));
    if (!base) return core::Result<ProductionCookKey>::failure(base.error());
    const auto digest = recipe.canonical_digest();
    if (!digest) return core::Result<ProductionCookKey>::failure(digest.error());
    ProductionCookKey key = base.value();
    key.algorithm = std::string(kBakeThicknessAlgorithm);
    key.settings_digest = hash_text(std::string(kBakeThicknessAlgorithm) + "\n" +
        digest.value().hex());
    key.representation_kind = "ray-aligned-thickness-rgba8-v1";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_thickness_graph_node(
    ProductionCookGraph& graph,
    const ThicknessRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_thickness_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<void> publish_thickness_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const ThicknessMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = validate_bake_sources(
            artifact.recipe.binding, current_source, current_target); !result) {
        return result;
    }
    const auto expected_key = bake_thickness_cook_key(
        artifact.recipe, std::move(platform_profile));
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "thickness receipt does not match the current bake recipe"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const BakeMeshBinding& source_binding = artifact.recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_authoritative_source, policy);
}

core::Result<ThicknessMapArtifact> read_thickness_map(
    const ProductionArtifactStore& store,
    const ThicknessRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = validate_bake_sources(
            recipe.binding, current_source, current_target); !result) {
        return core::Result<ThicknessMapArtifact>::failure(result.error());
    }
    const auto key = bake_thickness_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<ThicknessMapArtifact>::failure(key.error());
    const BakeMeshBinding& source_binding = recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    const auto payload = store.read(key.value(), current_authoritative_source, policy);
    if (!payload) return core::Result<ThicknessMapArtifact>::failure(payload.error());
    auto artifact = ThicknessMapArtifact::deserialize(payload.value());
    if (!artifact) return artifact;
    if (artifact.value().recipe.serialize() != recipe.serialize()) {
        return core::Result<ThicknessMapArtifact>::failure(validation(
            "stored thickness artifact does not match the requested recipe"));
    }
    return artifact;
}

core::Result<ProductionCookKey> bake_height_displacement_cook_key(
    const HeightDisplacementRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto base = bake_production_cook_key(recipe.binding, std::move(platform_profile));
    if (!base) return core::Result<ProductionCookKey>::failure(base.error());
    const auto digest = recipe.canonical_digest();
    if (!digest) return core::Result<ProductionCookKey>::failure(digest.error());
    ProductionCookKey key = base.value();
    key.algorithm = std::string(kBakeHeightDisplacementAlgorithm);
    key.settings_digest = hash_text(std::string(kBakeHeightDisplacementAlgorithm) + "\n" +
        digest.value().hex());
    key.representation_kind = "signed-height-displacement-rgba8-v1";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_height_displacement_graph_node(
    ProductionCookGraph& graph,
    const HeightDisplacementRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = bake_height_displacement_cook_key(
        recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

core::Result<void> publish_height_displacement_map(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const HeightDisplacementMapArtifact& artifact,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = validate_bake_sources(
            artifact.recipe.binding, current_source, current_target); !result) {
        return result;
    }
    const auto expected_key = bake_height_displacement_cook_key(
        artifact.recipe, std::move(platform_profile));
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(validation(
            "height/displacement receipt does not match the current bake recipe"));
    }
    const auto payload = artifact.serialize();
    if (!payload) return core::Result<void>::failure(payload.error());
    const BakeMeshBinding& source_binding = artifact.recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    return store.publish(std::move(receipt),
        std::span<const std::uint8_t>{payload.value()}, current_authoritative_source, policy);
}

core::Result<HeightDisplacementMapArtifact> read_height_displacement_map(
    const ProductionArtifactStore& store,
    const HeightDisplacementRecipe& recipe,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy,
    std::string platform_profile) {
    if (auto result = validate_bake_sources(
            recipe.binding, current_source, current_target); !result) {
        return core::Result<HeightDisplacementMapArtifact>::failure(result.error());
    }
    const auto key = bake_height_displacement_cook_key(
        recipe, std::move(platform_profile));
    if (!key) return core::Result<HeightDisplacementMapArtifact>::failure(key.error());
    const BakeMeshBinding& source_binding = recipe.binding.source;
    const AuthoritativeSource current_authoritative_source{
        source_binding.identity, source_binding.revision, source_binding.content_digest};
    const auto payload = store.read(key.value(), current_authoritative_source, policy);
    if (!payload) return core::Result<HeightDisplacementMapArtifact>::failure(payload.error());
    auto artifact = HeightDisplacementMapArtifact::deserialize(payload.value());
    if (!artifact) return artifact;
    if (artifact.value().recipe.serialize() != recipe.serialize()) {
        return core::Result<HeightDisplacementMapArtifact>::failure(validation(
            "stored height/displacement artifact does not match the requested recipe"));
    }
    return artifact;
}

ThicknessExecutionReport compile_thickness_cpu(
    const ThicknessRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    ThicknessExecutionReport report;
    if (auto result = recipe.validate(); !result) {
        add_error(report, "thickness_recipe_invalid", result.error().message);
        return report;
    }
    if (auto result = validate_bake_sources(recipe.binding, source, target); !result) {
        add_error(report, "bake_input_invalid", result.error().message);
        return report;
    }
    auto source_triangles = collect_triangles(source, std::nullopt);
    auto target_triangles = collect_triangles(target, recipe.binding.target_uv_set);
    if (!source_triangles || !target_triangles) {
        const auto& error = !source_triangles
            ? source_triangles.error() : target_triangles.error();
        add_error(report, "thickness_mesh_invalid", error.message);
        return report;
    }

    const std::size_t texel_count = static_cast<std::size_t>(
        recipe.binding.width) * recipe.binding.height;
    std::vector<std::uint8_t> pixels(texel_count * 4U, 0U);
    std::vector<bool> occupied(texel_count, false);
    std::vector<geometry::FaceId> owners(texel_count);
    BakeBvh bvh(source_triangles.value());
    std::uint64_t raster_candidates = 0U;
    std::vector<BakeProjectionMissSample> projection_miss_samples;
    std::vector<BakeProjectionMissSample> thickness_miss_samples;
    bool projection_samples_truncated = false;
    bool thickness_samples_truncated = false;
    double minimum_thickness = std::numeric_limits<double>::infinity();
    double maximum_thickness = -std::numeric_limits<double>::infinity();
    const double tile_left = static_cast<double>(recipe.binding.tile_u);
    const double tile_bottom = static_cast<double>(recipe.binding.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;

    for (BakeTriangle triangle : target_triangles.value()) {
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
                "thickness bake requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }
        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kBakeScalarEpsilon) {
            add_error(report, "degenerate_target_uv",
                "thickness target triangle has a degenerate UV chart", triangle.face);
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
                        "thickness target rasterization exceeded its deterministic work budget",
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
                    target_normal.length() <= kBakeScalarEpsilon) {
                    add_error(report, "target_shading_frame_invalid",
                        "thickness target interpolation produced a degenerate frame",
                        triangle.face, x, y);
                    continue;
                }

                const double projection_extent = recipe.binding.projection_half_extent;
                const core::Vec3d projection_origin =
                    target_position + target_normal * projection_extent;
                const auto projected = bvh.cast(projection_origin, target_normal * -1.0,
                    0.0, projection_extent * 2.0, recipe.binding.backface_policy);
                if (!projected) {
                    ++report.projection_misses;
                    if (projection_miss_samples.size() < kMaxBakeMissSamples) {
                        projection_miss_samples.push_back({triangle.face, x, y});
                    } else {
                        projection_samples_truncated = true;
                    }
                    add_diagnostic(report, "thickness_projection_miss",
                        "target texel did not project onto the bound source surface",
                        recipe.binding.reject_projection_misses, triangle.face, x, y);
                    continue;
                }
                ++report.projected_texels;
                const core::Vec3d source_surface = projection_origin -
                    target_normal * projected->distance;
                const core::Vec3d inward_origin =
                    source_surface + target_normal * recipe.surface_bias;
                const double maximum_ray_distance =
                    recipe.surface_bias + recipe.maximum_distance;
                const auto exit = bvh.cast(inward_origin, target_normal * -1.0,
                    recipe.surface_bias * 2.0, maximum_ray_distance,
                    BakeBackfacePolicy::both_sides);
                if (!exit) {
                    ++report.thickness_misses;
                    if (thickness_miss_samples.size() < kMaxBakeMissSamples) {
                        thickness_miss_samples.push_back({triangle.face, x, y});
                    } else {
                        thickness_samples_truncated = true;
                    }
                    add_diagnostic(report, "thickness_exit_miss",
                        "projected source surface has no inward exit within the bound distance",
                        recipe.reject_thickness_misses, triangle.face, x, y);
                    continue;
                }
                const double measured = exit->distance - recipe.surface_bias;
                if (!std::isfinite(measured) || measured < 0.0 ||
                    measured > recipe.maximum_distance + kBakeScalarEpsilon) {
                    add_error(report, "thickness_measurement_invalid",
                        "inward exit distance is outside the declared thickness range",
                        triangle.face, x, y);
                    continue;
                }
                const double thickness = std::clamp(measured, 0.0, recipe.maximum_distance);
                const auto encoded = static_cast<std::uint8_t>(std::clamp(
                    std::lround(thickness / recipe.maximum_distance * 255.0), 0L, 255L));
                const std::size_t pixel = texel * 4U;
                pixels[pixel] = encoded;
                pixels[pixel + 1U] = encoded;
                pixels[pixel + 2U] = encoded;
                pixels[pixel + 3U] = 255U;
                ++report.thickness_hits;
                minimum_thickness = std::min(minimum_thickness, thickness);
                maximum_thickness = std::max(maximum_thickness, thickness);
            }
        }
    }

    if (std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        return report;
    }
    if (report.covered_texels == 0U || report.projected_texels == 0U ||
        report.thickness_hits == 0U) {
        add_error(report, "thickness_measurement_empty",
            "selected target UV tile produced no complete thickness measurements");
        return report;
    }
    ThicknessMapArtifact artifact;
    artifact.recipe = recipe;
    artifact.covered_texels = report.covered_texels;
    artifact.projected_texels = report.projected_texels;
    artifact.thickness_hits = report.thickness_hits;
    artifact.projection_misses = report.projection_misses;
    artifact.thickness_misses = report.thickness_misses;
    artifact.projection_miss_samples = std::move(projection_miss_samples);
    artifact.projection_miss_samples_truncated = projection_samples_truncated;
    artifact.thickness_miss_samples = std::move(thickness_miss_samples);
    artifact.thickness_miss_samples_truncated = thickness_samples_truncated;
    artifact.minimum_thickness = minimum_thickness;
    artifact.maximum_thickness = maximum_thickness;
    artifact.rgba8 = std::move(pixels);
    if (auto result = artifact.refresh_digests(); !result) {
        add_error(report, "thickness_artifact_invalid", result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        add_error(report, "thickness_artifact_invalid", result.error().message);
        return report;
    }
    report.artifact = std::move(artifact);
    return report;
}

HeightDisplacementExecutionReport compile_height_displacement_cpu(
    const HeightDisplacementRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    HeightDisplacementExecutionReport report;
    if (auto result = recipe.validate(); !result) {
        add_height_error(report, "height_displacement_recipe_invalid", result.error().message);
        return report;
    }
    if (auto result = validate_bake_sources(recipe.binding, source, target); !result) {
        add_height_error(report, "bake_input_invalid", result.error().message);
        return report;
    }
    auto source_triangles = collect_triangles(source, std::nullopt);
    auto target_triangles = collect_triangles(target, recipe.binding.target_uv_set);
    if (!source_triangles || !target_triangles) {
        const auto& error = !source_triangles
            ? source_triangles.error() : target_triangles.error();
        add_height_error(report, "height_displacement_mesh_invalid", error.message);
        return report;
    }

    const std::size_t texel_count = static_cast<std::size_t>(
        recipe.binding.width) * recipe.binding.height;
    std::vector<std::uint8_t> pixels(texel_count * 4U, 0U);
    for (std::size_t pixel = 0U; pixel < pixels.size(); pixel += 4U) {
        pixels[pixel] = 128U;
        pixels[pixel + 1U] = 128U;
        pixels[pixel + 2U] = 128U;
    }
    std::vector<bool> occupied(texel_count, false);
    std::vector<geometry::FaceId> owners(texel_count);
    BakeBvh bvh(source_triangles.value());
    std::uint64_t raster_candidates = 0U;
    std::vector<BakeProjectionMissSample> miss_samples;
    bool miss_samples_truncated = false;
    double minimum_displacement = std::numeric_limits<double>::infinity();
    double maximum_displacement = -std::numeric_limits<double>::infinity();
    const double tile_left = static_cast<double>(recipe.binding.tile_u);
    const double tile_bottom = static_cast<double>(recipe.binding.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;

    for (BakeTriangle triangle : target_triangles.value()) {
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
            add_height_error(report, "cross_tile_uv_triangle",
                "height/displacement bake requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }
        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kBakeScalarEpsilon) {
            add_height_error(report, "degenerate_target_uv",
                "height/displacement target triangle has a degenerate UV chart", triangle.face);
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
                    add_height_error(report, "raster_work_budget_exceeded",
                        "height/displacement target rasterization exceeded its deterministic work budget",
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
                    add_height_error(report, "overlapping_uv_islands", message.str(),
                        triangle.face, x, y);
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
                    target_normal.length() <= kBakeScalarEpsilon) {
                    add_height_error(report, "target_shading_frame_invalid",
                        "height/displacement target interpolation produced a degenerate frame",
                        triangle.face, x, y);
                    continue;
                }

                const double projection_extent = recipe.binding.projection_half_extent;
                const core::Vec3d ray_origin = target_position +
                    target_normal * projection_extent;
                const auto hit = bvh.cast(ray_origin, target_normal * -1.0,
                    0.0, projection_extent * 2.0, recipe.binding.backface_policy);
                if (!hit) {
                    ++report.projection_misses;
                    if (miss_samples.size() < kMaxBakeMissSamples) {
                        miss_samples.push_back({triangle.face, x, y});
                    } else {
                        miss_samples_truncated = true;
                    }
                    add_height_diagnostic(report, "height_displacement_projection_miss",
                        "target texel did not project onto the bound source surface",
                        recipe.binding.reject_projection_misses, triangle.face, x, y);
                    continue;
                }
                ++report.projected_texels;
                const double displacement = projection_extent - hit->distance;
                if (!std::isfinite(displacement)) {
                    add_height_error(report, "height_displacement_measurement_invalid",
                        "projected source produced a non-finite signed displacement",
                        triangle.face, x, y);
                    continue;
                }
                minimum_displacement = std::min(minimum_displacement, displacement);
                maximum_displacement = std::max(maximum_displacement, displacement);
                const bool clamped = std::abs(displacement) > recipe.normalization_distance;
                if (clamped) {
                    ++report.clamped_texels;
                    add_height_diagnostic(report, "height_displacement_clamped",
                        "signed displacement exceeds the declared encoding range",
                        recipe.reject_clamped_samples, triangle.face, x, y);
                }
                const double normalized = std::clamp(
                    displacement / recipe.normalization_distance, -1.0, 1.0);
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
        add_height_error(report, "height_displacement_projection_empty",
            "selected target UV tile produced no projected displacement texels");
        return report;
    }
    HeightDisplacementMapArtifact artifact;
    artifact.recipe = recipe;
    artifact.covered_texels = report.covered_texels;
    artifact.projected_texels = report.projected_texels;
    artifact.projection_misses = report.projection_misses;
    artifact.projection_miss_samples = std::move(miss_samples);
    artifact.projection_miss_samples_truncated = miss_samples_truncated;
    artifact.clamped_texels = report.clamped_texels;
    artifact.minimum_signed_displacement = minimum_displacement;
    artifact.maximum_signed_displacement = maximum_displacement;
    artifact.rgba8 = std::move(pixels);
    if (auto result = artifact.refresh_digests(); !result) {
        add_height_error(report, "height_displacement_artifact_invalid", result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        add_height_error(report, "height_displacement_artifact_invalid", result.error().message);
        return report;
    }
    report.artifact = std::move(artifact);
    return report;
}

core::Result<void> WorldPositionRecipe::validate() const {
    if (!world_position_identity_is_safe(identity) ||
        !world_position_identity_is_safe(source_namespace) ||
        source_digest.is_zero() || target_scene_node == 0U ||
        target_mesh_asset == 0U || target_mesh_digest.is_zero() ||
        target_uv_set == 0U || width == 0U || height == 0U ||
        width > kMaxBakeTextureExtent || height > kMaxBakeTextureExtent ||
        static_cast<std::uint64_t>(width) * height > kMaxWorldPositionTexels ||
        tile_u > 9'999U || tile_v > 9'999U || !target_world_transform.finite()) {
        return core::Result<void>::failure(invalid(
            "world-position recipe identity, binding, transform, or dimensions are invalid"));
    }
    return core::Result<void>::success();
}

std::string WorldPositionRecipe::serialize() const {
    if (!validate()) return {};
    const auto field = [](std::string_view name, std::string value) {
        return std::string(name) + "=" + value + "\n";
    };
    std::string text(kWorldPositionRecipeMagic);
    text.push_back('\n');
    text += field("identity.hex", hex_encode(identity));
    text += field("source.namespace.hex", hex_encode(source_namespace));
    text += field("source.revision", std::to_string(source_revision.value()));
    text += field("source.digest", source_digest.hex());
    text += field("target.scene.node", std::to_string(target_scene_node));
    text += field("target.mesh.asset", std::to_string(target_mesh_asset));
    text += field("target.mesh.revision", std::to_string(target_mesh_revision.value()));
    text += field("target.mesh.digest", target_mesh_digest.hex());
    text += field("target.uv.set", std::to_string(target_uv_set));
    text += field("output.width", std::to_string(width));
    text += field("output.height", std::to_string(height));
    text += field("output.tile.u", std::to_string(tile_u));
    text += field("output.tile.v", std::to_string(tile_v));
    text += field("transform.translation.x", double_bits_hex(target_world_transform.translation.x));
    text += field("transform.translation.y", double_bits_hex(target_world_transform.translation.y));
    text += field("transform.translation.z", double_bits_hex(target_world_transform.translation.z));
    text += field("transform.rotation.x", double_bits_hex(target_world_transform.rotation.x));
    text += field("transform.rotation.y", double_bits_hex(target_world_transform.rotation.y));
    text += field("transform.rotation.z", double_bits_hex(target_world_transform.rotation.z));
    text += field("transform.rotation.w", double_bits_hex(target_world_transform.rotation.w));
    text += field("transform.scale.x", double_bits_hex(target_world_transform.scale.x));
    text += field("transform.scale.y", double_bits_hex(target_world_transform.scale.y));
    text += field("transform.scale.z", double_bits_hex(target_world_transform.scale.z));
    return text.size() <= kMaxWorldPositionRecipeBytes ? text : std::string{};
}

core::Result<assets::Sha256Digest> WorldPositionRecipe::canonical_digest() const {
    const std::string text = serialize();
    if (text.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid or oversized world-position recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(text));
}

core::Result<WorldPositionRecipe> WorldPositionRecipe::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxWorldPositionRecipeBytes) {
        return core::Result<WorldPositionRecipe>::failure(invalid(
            "world-position recipe byte size is invalid"));
    }
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || line != kWorldPositionRecipeMagic) {
        return core::Result<WorldPositionRecipe>::failure(validation(
            "world-position recipe header is unsupported"));
    }
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<WorldPositionRecipe>::failure(validation(
                "world-position recipe contains a malformed or duplicate field"));
        }
    }
    constexpr std::size_t kWorldPositionRecipeFieldCount = 23U;
    if (fields.size() != kWorldPositionRecipeFieldCount) {
        return core::Result<WorldPositionRecipe>::failure(validation(
            "world-position recipe does not contain the exact supported field set"));
    }
    const auto get = [&fields](std::string_view key) -> const std::string* {
        const auto found = fields.find(std::string(key));
        return found == fields.end() ? nullptr : &found->second;
    };
    const auto identity_field = get("identity.hex");
    const auto namespace_field = get("source.namespace.hex");
    const auto source_revision_field = get("source.revision");
    const auto source_digest_field = get("source.digest");
    const auto node_field = get("target.scene.node");
    const auto mesh_asset_field = get("target.mesh.asset");
    const auto mesh_revision_field = get("target.mesh.revision");
    const auto mesh_digest_field = get("target.mesh.digest");
    const auto uv_field = get("target.uv.set");
    const auto width_field = get("output.width");
    const auto height_field = get("output.height");
    const auto tile_u_field = get("output.tile.u");
    const auto tile_v_field = get("output.tile.v");
    if (identity_field == nullptr || namespace_field == nullptr ||
        source_revision_field == nullptr || source_digest_field == nullptr ||
        node_field == nullptr || mesh_asset_field == nullptr ||
        mesh_revision_field == nullptr || mesh_digest_field == nullptr ||
        uv_field == nullptr || width_field == nullptr || height_field == nullptr ||
        tile_u_field == nullptr || tile_v_field == nullptr) {
        return core::Result<WorldPositionRecipe>::failure(validation(
            "world-position recipe is missing a required field"));
    }
    const auto identity = hex_decode(*identity_field, 1024U);
    const auto source_namespace = hex_decode(*namespace_field, 1024U);
    const auto source_revision = parse_u64(*source_revision_field);
    const auto source_digest = assets::Sha256Digest::from_hex(*source_digest_field);
    const auto node = parse_u64(*node_field);
    const auto mesh_asset = parse_u64(*mesh_asset_field);
    const auto mesh_revision = parse_u64(*mesh_revision_field);
    const auto mesh_digest = assets::Sha256Digest::from_hex(*mesh_digest_field);
    const auto uv = parse_u64(*uv_field);
    const auto width = parse_u64(*width_field);
    const auto height = parse_u64(*height_field);
    const auto tile_u = parse_u64(*tile_u_field);
    const auto tile_v = parse_u64(*tile_v_field);
    if (!identity || !source_namespace || !source_revision || !source_digest ||
        !node || !mesh_asset || !mesh_revision || !mesh_digest || !uv || !width ||
        !height || !tile_u || !tile_v ||
        *uv > std::numeric_limits<attributes::UvSetId>::max() ||
        *width > std::numeric_limits<std::uint32_t>::max() ||
        *height > std::numeric_limits<std::uint32_t>::max() ||
        *tile_u > std::numeric_limits<std::uint32_t>::max() ||
        *tile_v > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<WorldPositionRecipe>::failure(validation(
            "world-position recipe contains a malformed binding value"));
    }
    WorldPositionRecipe recipe;
    recipe.identity = *identity;
    recipe.source_namespace = *source_namespace;
    recipe.source_revision = core::Revision{*source_revision};
    recipe.source_digest = source_digest.value();
    recipe.target_scene_node = *node;
    recipe.target_mesh_asset = *mesh_asset;
    recipe.target_mesh_revision = core::Revision{*mesh_revision};
    recipe.target_mesh_digest = mesh_digest.value();
    recipe.target_uv_set = static_cast<attributes::UvSetId>(*uv);
    recipe.width = static_cast<std::uint32_t>(*width);
    recipe.height = static_cast<std::uint32_t>(*height);
    recipe.tile_u = static_cast<std::uint32_t>(*tile_u);
    recipe.tile_v = static_cast<std::uint32_t>(*tile_v);
    const auto parse_transform = [&get](
        std::string_view key) -> std::optional<double> {
        const std::string* encoded = get(key);
        return encoded == nullptr ? std::nullopt : parse_double_bits(*encoded);
    };
    const std::array<std::string_view, 10U> transform_fields{
        "transform.translation.x", "transform.translation.y", "transform.translation.z",
        "transform.rotation.x", "transform.rotation.y", "transform.rotation.z",
        "transform.rotation.w", "transform.scale.x", "transform.scale.y",
        "transform.scale.z"};
    std::array<double, 10U> transform_values{};
    for (std::size_t index = 0U; index < transform_fields.size(); ++index) {
        const auto value = parse_transform(transform_fields[index]);
        if (!value) {
            return core::Result<WorldPositionRecipe>::failure(validation(
                "world-position recipe contains a malformed world transform"));
        }
        transform_values[index] = *value;
    }
    recipe.target_world_transform = core::Transform{
        {transform_values[0U], transform_values[1U], transform_values[2U]},
        {transform_values[3U], transform_values[4U], transform_values[5U],
            transform_values[6U]},
        {transform_values[7U], transform_values[8U], transform_values[9U]}};
    if (auto result = recipe.validate(); !result) {
        return core::Result<WorldPositionRecipe>::failure(result.error());
    }
    if (recipe.serialize() != text) {
        return core::Result<WorldPositionRecipe>::failure(validation(
            "world-position recipe serialization is not canonical"));
    }
    return core::Result<WorldPositionRecipe>::success(std::move(recipe));
}

core::Result<void> WorldPositionMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t expected_size = static_cast<std::uint64_t>(recipe.width) *
        recipe.height * 32ULL;
    if (expected_size != output_bytes.size() ||
        output_bytes.size() > kMaxWorldPositionArtifactBytes) {
        return core::Result<void>::failure(invalid(
            "world-position output size does not match its bounded recipe"));
    }
    if (algorithm != kBakeWorldPositionAlgorithm) {
        return core::Result<void>::failure(invalid(
            "world-position algorithm identity is unsupported"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return core::Result<void>::failure(recipe_digest.error());
    settings_digest = hash_text(std::string(kBakeWorldPositionAlgorithm) + "\n" +
        recipe_digest.value().hex());
    output_digest = hash_bytes(output_bytes);
    artifact_digest = hash_bytes(canonical_world_position_artifact_bytes(*this, false));
    return core::Result<void>::success();
}

core::Result<void> WorldPositionMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t expected_size = static_cast<std::uint64_t>(recipe.width) *
        recipe.height * 32ULL;
    if (algorithm != kBakeWorldPositionAlgorithm || settings_digest.is_zero() ||
        output_digest.is_zero() || artifact_digest.is_zero() || covered_texels == 0U ||
        expected_size != output_bytes.size() ||
        output_bytes.size() > kMaxWorldPositionArtifactBytes) {
        return core::Result<void>::failure(validation(
            "world-position artifact metadata or output size is invalid"));
    }
    std::uint64_t measured_coverage = 0U;
    for (std::size_t offset = 0U; offset < output_bytes.size(); offset += 32U) {
        const std::uint64_t face_id = load_u64(output_bytes, offset);
        const std::uint64_t x_bits = load_u64(output_bytes, offset + 8U);
        const std::uint64_t y_bits = load_u64(output_bytes, offset + 16U);
        const std::uint64_t z_bits = load_u64(output_bytes, offset + 24U);
        if (face_id == 0U) {
            if (x_bits != 0U || y_bits != 0U || z_bits != 0U) {
                return core::Result<void>::failure(validation(
                    "uncovered world-position texel contains nonzero data"));
            }
            continue;
        }
        ++measured_coverage;
        if (!std::isfinite(std::bit_cast<double>(x_bits)) ||
            !std::isfinite(std::bit_cast<double>(y_bits)) ||
            !std::isfinite(std::bit_cast<double>(z_bits))) {
            return core::Result<void>::failure(validation(
                "world-position texel contains non-finite coordinates"));
        }
    }
    if (measured_coverage != covered_texels) {
        return core::Result<void>::failure(validation(
            "world-position texel coverage disagrees with the artifact receipt"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest || hash_text(std::string(kBakeWorldPositionAlgorithm) + "\n" +
            recipe_digest.value().hex()) != settings_digest ||
        hash_bytes(output_bytes) != output_digest ||
        hash_bytes(canonical_world_position_artifact_bytes(*this, false)) != artifact_digest) {
        return core::Result<void>::failure(validation(
            "world-position artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> WorldPositionMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    auto bytes = canonical_world_position_artifact_bytes(*this, true);
    if (bytes.size() > kMaxWorldPositionArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "world-position artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<WorldPositionMapArtifact> WorldPositionMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxWorldPositionArtifactBytes) {
        return core::Result<WorldPositionMapArtifact>::failure(invalid(
            "world-position artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string recipe_text;
    std::string algorithm_value;
    if (!reader.read_string(magic, kWorldPositionArtifactMagic.size()) ||
        magic != kWorldPositionArtifactMagic || !reader.read_u32(version) ||
        version != kWorldPositionArtifactVersion ||
        !reader.read_string(recipe_text, kMaxWorldPositionRecipeBytes) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<WorldPositionMapArtifact>::failure(validation(
            "world-position artifact header or recipe is malformed"));
    }
    const auto recipe = WorldPositionRecipe::deserialize(recipe_text);
    if (!recipe) return core::Result<WorldPositionMapArtifact>::failure(recipe.error());
    WorldPositionMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.algorithm = std::move(algorithm_value);
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_payload(artifact.output_bytes,
            static_cast<std::size_t>(kMaxWorldPositionTexels * 32ULL)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<WorldPositionMapArtifact>::failure(validation(
            "world-position artifact payload is truncated, oversized, or has trailing bytes"));
    }
    if (auto result = artifact.validate(); !result) {
        return core::Result<WorldPositionMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<WorldPositionMapArtifact>::failure(validation(
            "world-position artifact serialization is not canonical"));
    }
    return core::Result<WorldPositionMapArtifact>::success(std::move(artifact));
}

core::Result<WorldPositionRecipe> bind_world_position_recipe(
    std::string identity,
    const project::ProjectDocument& document,
    std::uint64_t target_scene_node,
    attributes::UvSetId target_uv_set,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t tile_u,
    std::uint32_t tile_v) {
    if (auto result = document.validate(); !result) {
        return core::Result<WorldPositionRecipe>::failure(
            result.error().with_context("world-position source document"));
    }
    const scene::SceneObject* object = document.scene().find(
        scene::ObjectId{target_scene_node});
    if (object == nullptr || !object->mesh_asset.has_value()) {
        return core::Result<WorldPositionRecipe>::failure(validation(
            "world-position target scene node is missing or has no mesh asset"));
    }
    const auto mesh = document.meshes().find(object->mesh_asset.value());
    if (mesh == document.meshes().end()) {
        return core::Result<WorldPositionRecipe>::failure(validation(
            "world-position target scene node references a missing mesh asset"));
    }
    const auto transform = document.scene().world_transform(object->id);
    const auto mesh_digest = bake_mesh_target_digest(mesh->second, target_uv_set);
    if (!transform) return core::Result<WorldPositionRecipe>::failure(transform.error());
    if (!mesh_digest) return core::Result<WorldPositionRecipe>::failure(mesh_digest.error());
    WorldPositionRecipe recipe;
    recipe.identity = std::move(identity);
    recipe.source_namespace = document.source_namespace();
    recipe.source_revision = document.revision();
    recipe.source_digest = hash_text(document.serialize());
    recipe.target_scene_node = target_scene_node;
    recipe.target_mesh_asset = object->mesh_asset.value();
    recipe.target_mesh_revision = mesh->second.revision();
    recipe.target_mesh_digest = mesh_digest.value();
    recipe.target_uv_set = target_uv_set;
    recipe.width = width;
    recipe.height = height;
    recipe.tile_u = tile_u;
    recipe.tile_v = tile_v;
    recipe.target_world_transform = transform.value();
    if (auto result = recipe.validate(); !result) {
        return core::Result<WorldPositionRecipe>::failure(result.error());
    }
    return core::Result<WorldPositionRecipe>::success(std::move(recipe));
}

core::Result<void> validate_world_position_source(
    const WorldPositionRecipe& recipe,
    const project::ProjectDocument& document) {
    if (auto result = recipe.validate(); !result) return result;
    if (auto result = document.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("world-position current source document"));
    }
    if (recipe.source_namespace != document.source_namespace() ||
        recipe.source_revision != document.revision() ||
        recipe.source_digest != hash_text(document.serialize())) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "world-position recipe does not match the exact project snapshot"));
    }
    const scene::SceneObject* object = document.scene().find(
        scene::ObjectId{recipe.target_scene_node});
    if (object == nullptr || !object->mesh_asset.has_value() ||
        object->mesh_asset.value() != recipe.target_mesh_asset) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "world-position target scene node no longer binds the recipe mesh"));
    }
    const auto mesh = document.meshes().find(recipe.target_mesh_asset);
    const auto transform = document.scene().world_transform(object->id);
    if (mesh == document.meshes().end() || !transform) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "world-position target mesh or world transform no longer resolves"));
    }
    const auto mesh_digest = bake_mesh_target_digest(mesh->second, recipe.target_uv_set);
    if (!mesh_digest) return core::Result<void>::failure(mesh_digest.error());
    if (mesh->second.revision() != recipe.target_mesh_revision ||
        mesh_digest.value() != recipe.target_mesh_digest ||
        !same_transform_bits(transform.value(), recipe.target_world_transform)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "world-position target mesh, UV set, or world transform changed"));
    }
    return core::Result<void>::success();
}

core::Result<ProductionCookKey> world_position_cook_key(
    const WorldPositionRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto digest = recipe.canonical_digest();
    if (!digest) return core::Result<ProductionCookKey>::failure(digest.error());
    ProductionCookKey key;
    key.source = AuthoritativeSource{
        recipe.source_namespace, recipe.source_revision, recipe.source_digest};
    key.kind = ArtifactKind::baked_texture;
    key.form = ExecutionForm::offline;
    key.algorithm = std::string(kBakeWorldPositionAlgorithm);
    key.settings_digest = hash_text(std::string(kBakeWorldPositionAlgorithm) + "\n" +
        digest.value().hex());
    key.platform_profile = std::move(platform_profile);
    key.representation_kind = "world-position-uv64-faceid-v1";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_world_position_graph_node(
    ProductionCookGraph& graph,
    const WorldPositionRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = world_position_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

WorldPositionExecutionReport compile_world_position_cpu(
    const WorldPositionRecipe& recipe,
    const project::ProjectDocument& document) {
    WorldPositionExecutionReport report;
    if (auto result = recipe.validate(); !result) {
        add_world_position_error(report, "world_position_recipe_invalid", result.error().message);
        return report;
    }
    if (auto result = validate_world_position_source(recipe, document); !result) {
        add_world_position_error(report, "world_position_source_invalid", result.error().message);
        return report;
    }
    const auto mesh = document.meshes().find(recipe.target_mesh_asset);
    if (mesh == document.meshes().end()) {
        add_world_position_error(report, "world_position_mesh_missing",
            "bound target mesh asset is no longer present");
        return report;
    }
    const auto triangles = collect_triangles(mesh->second, recipe.target_uv_set);
    if (!triangles) {
        add_world_position_error(report, "world_position_mesh_invalid", triangles.error().message);
        return report;
    }

    const std::size_t texel_count = static_cast<std::size_t>(recipe.width) * recipe.height;
    std::vector<std::uint8_t> output_bytes(texel_count * 32U, 0U);
    std::vector<bool> occupied(texel_count, false);
    std::uint64_t raster_candidates = 0U;
    const double tile_left = static_cast<double>(recipe.tile_u);
    const double tile_bottom = static_cast<double>(recipe.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;
    const core::Quaternion rotation = recipe.target_world_transform.rotation.normalized();
    const auto to_world = [&recipe, rotation](core::Vec3d position) {
        return recipe.target_world_transform.translation + rotation.rotate(
            core::componentwise_multiply(recipe.target_world_transform.scale, position));
    };

    for (BakeTriangle triangle : triangles.value()) {
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
            add_world_position_error(report, "cross_tile_uv_triangle",
                "world-position output requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }
        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kBakeScalarEpsilon) {
            add_world_position_error(report, "degenerate_target_uv",
                "world-position target triangle has a degenerate UV chart", triangle.face);
            continue;
        }
        if (area < 0.0) {
            std::swap(triangle.positions[1U], triangle.positions[2U]);
            std::swap(triangle.uvs[1U], triangle.uvs[2U]);
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
                    add_world_position_error(report, "raster_work_budget_exceeded",
                        "world-position rasterization exceeded its deterministic work budget",
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
                    !inside(edge_two, top_left_edge(triangle.uvs[0U], triangle.uvs[1U]))) continue;
                ++report.target_texels_visited;
                const std::size_t texel = static_cast<std::size_t>(y) * recipe.width + x;
                const std::size_t offset = texel * 32U;
                if (occupied[texel]) {
                    const std::uint64_t prior_face = load_u64(output_bytes, offset);
                    std::ostringstream message;
                    message << "target UV triangles overlap at texel " << x << ',' << y
                            << " (previous face " << prior_face << ')';
                    add_world_position_error(report, "overlapping_uv_islands", message.str(),
                        triangle.face, x, y);
                    continue;
                }
                occupied[texel] = true;
                const std::array<double, 3U> barycentric{
                    edge_zero / area, edge_one / area, edge_two / area};
                const core::Vec3d local_position =
                    triangle.positions[0U] * barycentric[0U] +
                    triangle.positions[1U] * barycentric[1U] +
                    triangle.positions[2U] * barycentric[2U];
                const core::Vec3d world_position = to_world(local_position);
                if (!local_position.finite() || !world_position.finite()) {
                    add_world_position_error(report, "world_position_non_finite",
                        "world-position interpolation produced non-finite coordinates",
                        triangle.face, x, y);
                    continue;
                }
                store_u64(output_bytes, offset, triangle.face.value);
                store_double(output_bytes, offset + 8U, world_position.x);
                store_double(output_bytes, offset + 16U, world_position.y);
                store_double(output_bytes, offset + 24U, world_position.z);
                ++report.covered_texels;
            }
        }
    }
    if (std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        return report;
    }
    if (report.covered_texels == 0U) {
        add_world_position_error(report, "world_position_output_empty",
            "selected target UV tile produced no world-position texels");
        return report;
    }
    WorldPositionMapArtifact artifact;
    artifact.recipe = recipe;
    artifact.covered_texels = report.covered_texels;
    artifact.output_bytes = std::move(output_bytes);
    if (auto result = artifact.refresh_digests(); !result) {
        add_world_position_error(report, "world_position_artifact_invalid", result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        add_world_position_error(report, "world_position_artifact_invalid", result.error().message);
        return report;
    }
    report.artifact = std::move(artifact);
    return report;
}

namespace {

constexpr std::string_view kMaterialSurfaceIdRecipeMagic =
    "CARTO_MATERIAL_SURFACE_ID_RECIPE 1";
constexpr std::string_view kMaterialSurfaceIdArtifactMagic =
    "CARTO_BAKE_MATERIAL_SURFACE_ID_ARTIFACT";
constexpr std::size_t kMaxMaterialSurfaceIdArtifactRecipeBytes =
    kMaxMaterialSurfaceIdRecipeBytes;

std::vector<std::uint8_t> canonical_material_surface_id_artifact_bytes(
    const MaterialSurfaceIdMapArtifact& artifact,
    bool include_artifact_digest) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kMaterialSurfaceIdArtifactMagic);
    append_u32(bytes, kMaterialSurfaceIdArtifactVersion);
    append_string(bytes, artifact.recipe.serialize());
    append_string(bytes, artifact.algorithm);
    append_digest(bytes, artifact.settings_digest);
    append_digest(bytes, artifact.output_digest);
    append_u64(bytes, artifact.covered_texels);
    append_u64(bytes, static_cast<std::uint64_t>(artifact.output_bytes.size()));
    bytes.insert(bytes.end(), artifact.output_bytes.begin(), artifact.output_bytes.end());
    if (include_artifact_digest) append_digest(bytes, artifact.artifact_digest);
    return bytes;
}

void add_material_surface_id_error(
    MaterialSurfaceIdExecutionReport& report,
    std::string code,
    std::string message,
    std::optional<geometry::FaceId> face = std::nullopt,
    std::optional<std::uint32_t> x = std::nullopt,
    std::optional<std::uint32_t> y = std::nullopt) {
    if (report.diagnostics.size() < 64U) {
        report.diagnostics.push_back(BakeDiagnostic{
            std::move(code), std::move(message), true,
            face, std::nullopt, x, y});
    } else if (report.diagnostics.back().code != "diagnostics_truncated") {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated",
            "additional material/surface ID diagnostics were omitted",
            true, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    }
}

std::uint32_t load_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

} // namespace

core::Result<void> MaterialSurfaceIdRecipe::validate() const {
    if (!world_position_identity_is_safe(identity) ||
        !world_position_identity_is_safe(source_namespace) ||
        source_revision.exhausted() || source_digest.is_zero() ||
        target_mesh_asset == 0U || target_mesh_revision.exhausted() ||
        target_mesh_digest.is_zero() || target_uv_set == 0U ||
        !world_position_identity_is_safe(material_region_layer) ||
        width == 0U || height == 0U || width > kMaxBakeTextureExtent ||
        height > kMaxBakeTextureExtent ||
        static_cast<std::uint64_t>(width) * height > kMaxMaterialSurfaceIdTexels ||
        tile_u > 9'999U || tile_v > 9'999U || surface_domains.empty() ||
        surface_domains.size() > kMaxMaterialSurfaceDomains) {
        return core::Result<void>::failure(invalid(
            "material/surface ID recipe identity, binding, domains, or dimensions are invalid"));
    }
    std::set<surface::SurfaceDomainId> domain_ids;
    std::set<geometry::FaceId> assigned_faces;
    std::size_t total_faces = 0U;
    surface::SurfaceDomainId previous_id{};
    for (const surface::SurfaceDomain& domain : surface_domains) {
        if (!domain.id || (!domain_ids.empty() && !(previous_id < domain.id)) ||
            domain.source_revision != target_mesh_revision || domain.source_faces.empty() ||
            !std::is_sorted(domain.source_faces.begin(), domain.source_faces.end()) ||
            std::adjacent_find(domain.source_faces.begin(), domain.source_faces.end()) !=
                domain.source_faces.end() || domain.semantic_class.size() > 256U ||
            domain.material_class.size() > 256U ||
            domain.semantic_class.find('\n') != std::string::npos ||
            domain.material_class.find('\n') != std::string::npos ||
            !assets::Sha256Digest::from_hex(domain.source_content_digest) ||
            !assets::Sha256Digest::from_hex(domain.digest) ||
            hash_text(domain.canonical()).hex() != domain.digest) {
            return core::Result<void>::failure(validation(
                "material/surface ID recipe contains a malformed or non-canonical surface domain"));
        }
        if (domain.source_content_digest !=
            assets::Sha256Digest::from_hex(domain.source_content_digest).value().hex()) {
            return core::Result<void>::failure(validation(
                "surface-domain source digest is not canonically encoded"));
        }
        domain_ids.insert(domain.id);
        previous_id = domain.id;
        if (domain.source_faces.size() > kMaxBakeSourceTriangles - total_faces) {
            return core::Result<void>::failure(validation(
                "material/surface ID recipe exceeds the bounded face-membership budget"));
        }
        total_faces += domain.source_faces.size();
        for (const geometry::FaceId face : domain.source_faces) {
            if (!face || !assigned_faces.insert(face).second) {
                return core::Result<void>::failure(validation(
                    "material/surface ID recipe has a zero or multiply assigned face"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string MaterialSurfaceIdRecipe::serialize() const {
    if (!validate()) return {};
    const auto field = [](std::string_view name, std::string value) {
        return std::string(name) + "=" + value + "\n";
    };
    std::string text(kMaterialSurfaceIdRecipeMagic);
    text.push_back('\n');
    text += field("identity.hex", hex_encode(identity));
    text += field("source.namespace.hex", hex_encode(source_namespace));
    text += field("source.revision", std::to_string(source_revision.value()));
    text += field("source.digest", source_digest.hex());
    text += field("target.mesh.asset", std::to_string(target_mesh_asset));
    text += field("target.mesh.revision", std::to_string(target_mesh_revision.value()));
    text += field("target.mesh.digest", target_mesh_digest.hex());
    text += field("target.uv.set", std::to_string(target_uv_set));
    text += field("target.material.region_layer.hex", hex_encode(material_region_layer));
    text += field("output.width", std::to_string(width));
    text += field("output.height", std::to_string(height));
    text += field("output.tile.u", std::to_string(tile_u));
    text += field("output.tile.v", std::to_string(tile_v));
    text += field("surface.count", std::to_string(surface_domains.size()));
    for (std::size_t index = 0U; index < surface_domains.size(); ++index) {
        const std::string prefix = "surface." + std::to_string(index) + ".";
        const surface::SurfaceDomain& domain = surface_domains[index];
        text += field(prefix + "id", std::to_string(domain.id.value));
        text += field(prefix + "revision", std::to_string(domain.source_revision.value()));
        text += field(prefix + "semantic.hex", hex_encode(domain.semantic_class));
        text += field(prefix + "material.hex", hex_encode(domain.material_class));
        text += field(prefix + "source.digest", domain.source_content_digest);
        text += field(prefix + "digest", domain.digest);
        text += field(prefix + "faces", [&domain] {
            std::string value;
            for (std::size_t face_index = 0U;
                 face_index < domain.source_faces.size(); ++face_index) {
                if (face_index != 0U) value.push_back(',');
                value += std::to_string(domain.source_faces[face_index].value);
            }
            return value;
        }());
    }
    return text.size() <= kMaxMaterialSurfaceIdRecipeBytes ? text : std::string{};
}

core::Result<assets::Sha256Digest> MaterialSurfaceIdRecipe::canonical_digest() const {
    const std::string text = serialize();
    if (text.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid or oversized material/surface ID recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(text));
}

core::Result<MaterialSurfaceIdRecipe> MaterialSurfaceIdRecipe::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxMaterialSurfaceIdRecipeBytes) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(invalid(
            "material/surface ID recipe byte size is invalid"));
    }
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || line != kMaterialSurfaceIdRecipeMagic) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
            "material/surface ID recipe header is unsupported"));
    }
    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
                "material/surface ID recipe contains a malformed or duplicate field"));
        }
    }
    const auto get = [&fields](std::string_view key) -> const std::string* {
        const auto found = fields.find(std::string(key));
        return found == fields.end() ? nullptr : &found->second;
    };
    const auto identity_text = get("identity.hex");
    const auto namespace_text = get("source.namespace.hex");
    const auto source_revision_text = get("source.revision");
    const auto source_digest_text = get("source.digest");
    const auto mesh_asset_text = get("target.mesh.asset");
    const auto mesh_revision_text = get("target.mesh.revision");
    const auto mesh_digest_text = get("target.mesh.digest");
    const auto uv_text = get("target.uv.set");
    const auto layer_text = get("target.material.region_layer.hex");
    const auto width_text = get("output.width");
    const auto height_text = get("output.height");
    const auto tile_u_text = get("output.tile.u");
    const auto tile_v_text = get("output.tile.v");
    const auto domain_count_text = get("surface.count");
    if (identity_text == nullptr || namespace_text == nullptr ||
        source_revision_text == nullptr || source_digest_text == nullptr ||
        mesh_asset_text == nullptr || mesh_revision_text == nullptr ||
        mesh_digest_text == nullptr || uv_text == nullptr || layer_text == nullptr ||
        width_text == nullptr || height_text == nullptr || tile_u_text == nullptr ||
        tile_v_text == nullptr || domain_count_text == nullptr) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
            "material/surface ID recipe is missing a required field"));
    }
    const auto source_revision = parse_u64(*source_revision_text);
    const auto source_digest = assets::Sha256Digest::from_hex(*source_digest_text);
    const auto mesh_asset = parse_u64(*mesh_asset_text);
    const auto mesh_revision = parse_u64(*mesh_revision_text);
    const auto mesh_digest = assets::Sha256Digest::from_hex(*mesh_digest_text);
    const auto uv = parse_u64(*uv_text);
    const auto width = parse_u64(*width_text);
    const auto height = parse_u64(*height_text);
    const auto tile_u = parse_u64(*tile_u_text);
    const auto tile_v = parse_u64(*tile_v_text);
    const auto domain_count = parse_u64(*domain_count_text);
    const auto identity = hex_decode(*identity_text, 1024U);
    const auto source_namespace = hex_decode(*namespace_text, 1024U);
    const auto layer = hex_decode(*layer_text, 1024U);
    if (!source_revision || !source_digest || !mesh_asset || !mesh_revision ||
        !mesh_digest || !uv || !width || !height || !tile_u || !tile_v ||
        !domain_count || *domain_count == 0U ||
        *domain_count > kMaxMaterialSurfaceDomains || !identity ||
        !source_namespace || !layer ||
        *uv > std::numeric_limits<attributes::UvSetId>::max() ||
        *width > std::numeric_limits<std::uint32_t>::max() ||
        *height > std::numeric_limits<std::uint32_t>::max() ||
        *tile_u > std::numeric_limits<std::uint32_t>::max() ||
        *tile_v > std::numeric_limits<std::uint32_t>::max() ||
        fields.size() != 14U + 7U * static_cast<std::size_t>(*domain_count)) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
            "material/surface ID recipe contains malformed or unexpected fields"));
    }
    MaterialSurfaceIdRecipe recipe;
    recipe.identity = *identity;
    recipe.source_namespace = *source_namespace;
    recipe.source_revision = core::Revision{*source_revision};
    recipe.source_digest = source_digest.value();
    recipe.target_mesh_asset = *mesh_asset;
    recipe.target_mesh_revision = core::Revision{*mesh_revision};
    recipe.target_mesh_digest = mesh_digest.value();
    recipe.target_uv_set = static_cast<attributes::UvSetId>(*uv);
    recipe.material_region_layer = *layer;
    recipe.width = static_cast<std::uint32_t>(*width);
    recipe.height = static_cast<std::uint32_t>(*height);
    recipe.tile_u = static_cast<std::uint32_t>(*tile_u);
    recipe.tile_v = static_cast<std::uint32_t>(*tile_v);
    recipe.surface_domains.reserve(static_cast<std::size_t>(*domain_count));
    for (std::size_t index = 0U; index < static_cast<std::size_t>(*domain_count); ++index) {
        const std::string prefix = "surface." + std::to_string(index) + ".";
        const auto id_text = get(prefix + "id");
        const auto revision_text = get(prefix + "revision");
        const auto semantic_text = get(prefix + "semantic.hex");
        const auto material_text = get(prefix + "material.hex");
        const auto source_domain_digest_text = get(prefix + "source.digest");
        const auto digest_text = get(prefix + "digest");
        const auto faces_text = get(prefix + "faces");
        if (id_text == nullptr || revision_text == nullptr || semantic_text == nullptr ||
            material_text == nullptr || source_domain_digest_text == nullptr ||
            digest_text == nullptr || faces_text == nullptr) {
            return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
                "material/surface ID recipe is missing a surface-domain field"));
        }
        const auto id = parse_u64(*id_text);
        const auto revision = parse_u64(*revision_text);
        const auto semantic = hex_decode(*semantic_text, 256U);
        const auto material = hex_decode(*material_text, 256U);
        const auto source_domain_digest = assets::Sha256Digest::from_hex(
            *source_domain_digest_text);
        const auto domain_digest = assets::Sha256Digest::from_hex(*digest_text);
        if (!id || !revision || !semantic || !material || !source_domain_digest ||
            !domain_digest) {
            return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
                "material/surface ID recipe contains a malformed surface domain"));
        }
        surface::SurfaceDomain domain;
        domain.id = surface::SurfaceDomainId{*id};
        domain.source_revision = core::Revision{*revision};
        domain.semantic_class = *semantic;
        domain.material_class = *material;
        domain.source_content_digest = *source_domain_digest_text;
        domain.digest = *digest_text;
        std::string_view faces = *faces_text;
        while (!faces.empty()) {
            const std::size_t separator = faces.find(',');
            const std::string_view token = faces.substr(0U, separator);
            const auto face = parse_u64(token);
            if (!face) {
                return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
                    "material/surface ID recipe contains a malformed face membership"));
            }
            domain.source_faces.push_back(geometry::FaceId{*face});
            if (separator == std::string_view::npos) break;
            faces.remove_prefix(separator + 1U);
            if (faces.empty()) {
                return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
                    "material/surface ID recipe face membership has a trailing separator"));
            }
        }
        recipe.surface_domains.push_back(std::move(domain));
    }
    if (auto result = recipe.validate(); !result) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(result.error());
    }
    if (recipe.serialize() != text) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
            "material/surface ID recipe serialization is not canonical"));
    }
    return core::Result<MaterialSurfaceIdRecipe>::success(std::move(recipe));
}

core::Result<void> MaterialSurfaceIdMapArtifact::validate() const {
    if (auto result = recipe.validate(); !result) return result;
    const std::uint64_t texel_count =
        static_cast<std::uint64_t>(recipe.width) * recipe.height;
    if (algorithm != kBakeMaterialSurfaceIdAlgorithm || texel_count == 0U ||
        output_bytes.size() != texel_count * kMaterialSurfaceIdTexelBytes) {
        return core::Result<void>::failure(validation(
            "material/surface ID artifact algorithm or payload size is invalid"));
    }
    std::uint64_t measured_coverage = 0U;
    std::map<std::uint64_t, std::uint64_t> domain_by_face;
    for (const surface::SurfaceDomain& domain : recipe.surface_domains) {
        for (const geometry::FaceId face : domain.source_faces) {
            domain_by_face.emplace(face.value, domain.id.value);
        }
    }
    for (std::size_t offset = 0U; offset < output_bytes.size();
         offset += kMaterialSurfaceIdTexelBytes) {
        const std::uint64_t face = load_u64(output_bytes, offset);
        const std::uint64_t material = load_u64(output_bytes, offset + 8U);
        const std::uint64_t variant = load_u64(output_bytes, offset + 16U);
        const std::uint64_t domain = load_u64(output_bytes, offset + 24U);
        const std::uint32_t region = load_u32(output_bytes, offset + 32U);
        const std::uint32_t reserved = load_u32(output_bytes, offset + 36U);
        if (reserved != 0U ||
            (face == 0U && (material != 0U || variant != 0U || domain != 0U || region != 0U)) ||
            (face != 0U && (material == 0U || domain == 0U || region == 0U))) {
            return core::Result<void>::failure(validation(
                "material/surface ID texel is partially populated or has reserved data"));
        }
        if (face != 0U) {
            const auto expected_domain = domain_by_face.find(face);
            if (expected_domain == domain_by_face.end() || expected_domain->second != domain) {
                return core::Result<void>::failure(validation(
                    "material/surface ID texel face and caller-authored domain disagree"));
            }
            ++measured_coverage;
        }
    }
    if (measured_coverage == 0U || measured_coverage != covered_texels) {
        return core::Result<void>::failure(validation(
            "material/surface ID texel coverage disagrees with the artifact receipt"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest || hash_text(std::string(kBakeMaterialSurfaceIdAlgorithm) + "\n" +
            recipe_digest.value().hex()) != settings_digest ||
        hash_bytes(output_bytes) != output_digest ||
        hash_bytes(canonical_material_surface_id_artifact_bytes(*this, false)) !=
            artifact_digest) {
        return core::Result<void>::failure(validation(
            "material/surface ID artifact digest verification failed"));
    }
    return core::Result<void>::success();
}

core::Result<void> MaterialSurfaceIdMapArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != kBakeMaterialSurfaceIdAlgorithm) {
        return core::Result<void>::failure(validation(
            "material/surface ID artifact algorithm is unsupported"));
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return core::Result<void>::failure(recipe_digest.error());
    settings_digest = hash_text(std::string(kBakeMaterialSurfaceIdAlgorithm) + "\n" +
        recipe_digest.value().hex());
    output_digest = hash_bytes(output_bytes);
    artifact_digest = hash_bytes(canonical_material_surface_id_artifact_bytes(*this, false));
    return validate();
}

core::Result<std::vector<std::uint8_t>> MaterialSurfaceIdMapArtifact::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    auto bytes = canonical_material_surface_id_artifact_bytes(*this, true);
    if (bytes.size() > kMaxMaterialSurfaceIdArtifactBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "material/surface ID artifact exceeds the serialized size limit"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<MaterialSurfaceIdMapArtifact> MaterialSurfaceIdMapArtifact::deserialize(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > kMaxMaterialSurfaceIdArtifactBytes) {
        return core::Result<MaterialSurfaceIdMapArtifact>::failure(invalid(
            "material/surface ID artifact byte size is invalid"));
    }
    ByteReader reader{bytes};
    std::string magic;
    std::uint32_t version = 0U;
    std::string recipe_text;
    std::string algorithm_value;
    if (!reader.read_string(magic, kMaterialSurfaceIdArtifactMagic.size()) ||
        magic != kMaterialSurfaceIdArtifactMagic || !reader.read_u32(version) ||
        version != kMaterialSurfaceIdArtifactVersion ||
        !reader.read_string(recipe_text, kMaxMaterialSurfaceIdArtifactRecipeBytes) ||
        !reader.read_string(algorithm_value, 256U)) {
        return core::Result<MaterialSurfaceIdMapArtifact>::failure(validation(
            "material/surface ID artifact header or recipe is malformed"));
    }
    const auto recipe = MaterialSurfaceIdRecipe::deserialize(recipe_text);
    if (!recipe) {
        return core::Result<MaterialSurfaceIdMapArtifact>::failure(recipe.error());
    }
    MaterialSurfaceIdMapArtifact artifact;
    artifact.recipe = recipe.value();
    artifact.algorithm = std::move(algorithm_value);
    if (!reader.read_digest(artifact.settings_digest) ||
        !reader.read_digest(artifact.output_digest) ||
        !reader.read_u64(artifact.covered_texels) ||
        !reader.read_payload(artifact.output_bytes,
            static_cast<std::size_t>(kMaxMaterialSurfaceIdTexels *
                kMaterialSurfaceIdTexelBytes)) ||
        !reader.read_digest(artifact.artifact_digest) || reader.offset != bytes.size()) {
        return core::Result<MaterialSurfaceIdMapArtifact>::failure(validation(
            "material/surface ID artifact payload is truncated, oversized, or has trailing bytes"));
    }
    if (auto result = artifact.validate(); !result) {
        return core::Result<MaterialSurfaceIdMapArtifact>::failure(result.error());
    }
    const auto canonical = artifact.serialize();
    if (!canonical || canonical.value().size() != bytes.size() ||
        !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
        return core::Result<MaterialSurfaceIdMapArtifact>::failure(validation(
            "material/surface ID artifact serialization is not canonical"));
    }
    return core::Result<MaterialSurfaceIdMapArtifact>::success(std::move(artifact));
}

core::Result<MaterialSurfaceIdRecipe> bind_material_surface_id_recipe(
    std::string identity,
    const project::ProjectDocument& document,
    std::uint64_t target_mesh_asset,
    attributes::UvSetId target_uv_set,
    std::string material_region_layer,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<surface::SurfaceDomain> surface_domains,
    std::uint32_t tile_u,
    std::uint32_t tile_v) {
    if (auto result = document.validate(); !result) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(
            result.error().with_context("material/surface ID source document"));
    }
    const auto mesh = document.meshes().find(target_mesh_asset);
    if (mesh == document.meshes().end()) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(validation(
            "material/surface ID target mesh asset is missing"));
    }
    const auto mesh_digest = bake_mesh_target_digest(mesh->second, target_uv_set);
    if (!mesh_digest) return core::Result<MaterialSurfaceIdRecipe>::failure(mesh_digest.error());
    std::sort(surface_domains.begin(), surface_domains.end(),
        [](const surface::SurfaceDomain& left, const surface::SurfaceDomain& right) {
            return left.id < right.id;
        });
    MaterialSurfaceIdRecipe recipe;
    recipe.identity = std::move(identity);
    recipe.source_namespace = document.source_namespace();
    recipe.source_revision = document.revision();
    recipe.source_digest = hash_text(document.serialize());
    recipe.target_mesh_asset = target_mesh_asset;
    recipe.target_mesh_revision = mesh->second.revision();
    recipe.target_mesh_digest = mesh_digest.value();
    recipe.target_uv_set = target_uv_set;
    recipe.material_region_layer = std::move(material_region_layer);
    recipe.width = width;
    recipe.height = height;
    recipe.tile_u = tile_u;
    recipe.tile_v = tile_v;
    recipe.surface_domains = std::move(surface_domains);
    if (auto result = recipe.validate(); !result) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(result.error());
    }
    if (auto result = validate_material_surface_id_source(recipe, document); !result) {
        return core::Result<MaterialSurfaceIdRecipe>::failure(result.error());
    }
    return core::Result<MaterialSurfaceIdRecipe>::success(std::move(recipe));
}

core::Result<void> validate_material_surface_id_source(
    const MaterialSurfaceIdRecipe& recipe,
    const project::ProjectDocument& document) {
    if (auto result = recipe.validate(); !result) return result;
    if (auto digest = recipe.canonical_digest(); !digest) {
        return core::Result<void>::failure(digest.error());
    }
    if (auto result = document.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("material/surface ID current source document"));
    }
    if (recipe.source_namespace != document.source_namespace() ||
        recipe.source_revision != document.revision() ||
        recipe.source_digest != hash_text(document.serialize())) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material/surface ID recipe does not match the exact project snapshot"));
    }
    const auto mesh = document.meshes().find(recipe.target_mesh_asset);
    if (mesh == document.meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material/surface ID target mesh is no longer present"));
    }
    const auto mesh_digest = bake_mesh_target_digest(mesh->second, recipe.target_uv_set);
    const auto compiled = mesh->second.compile();
    if (!mesh_digest || !compiled) {
        return core::Result<void>::failure(
            mesh_digest ? compiled.error() : mesh_digest.error());
    }
    if (mesh->second.revision() != recipe.target_mesh_revision ||
        mesh_digest.value() != recipe.target_mesh_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material/surface ID target mesh, topology, or UV set changed"));
    }
    const auto* material_regions =
        mesh->second.attributes().find(recipe.material_region_layer);
    const std::vector<geometry::Face> faces = mesh->second.faces_sorted();
    if (material_regions == nullptr ||
        material_regions->descriptor.domain != attributes::AttributeDomain::face ||
        material_regions->descriptor.type != attributes::AttributeType::uint32 ||
        material_regions->values.size() != faces.size()) {
        return core::Result<void>::failure(validation(
            "material/surface ID source requires a complete face-domain uint32 material-region layer"));
    }
    std::set<geometry::FaceId> remaining_faces;
    for (const geometry::Face& face : faces) remaining_faces.insert(face.id);
    for (const surface::SurfaceDomain& domain : recipe.surface_domains) {
        if (auto result = domain.validate(compiled.value()); !result) {
            return core::Result<void>::failure(
                result.error().with_context("material/surface ID domain"));
        }
        for (const geometry::FaceId face : domain.source_faces) {
            if (remaining_faces.erase(face) == 0U) {
                return core::Result<void>::failure(validation(
                    "material/surface ID domains overlap or reference a face twice"));
            }
        }
    }
    if (!remaining_faces.empty()) {
        return core::Result<void>::failure(validation(
            "material/surface ID domains do not cover every source mesh face"));
    }
    const auto& assignments = document.material_catalog().assignments();
    for (std::size_t index = 0U; index < faces.size(); ++index) {
        const auto* region = std::get_if<std::uint32_t>(&material_regions->values[index]);
        if (region == nullptr || *region == 0U) {
            return core::Result<void>::failure(validation(
                "material/surface ID source has a zero or malformed face material region"));
        }
        if (!assignments.contains(project::MaterialRegionKey{
                recipe.target_mesh_asset, *region})) {
            return core::Result<void>::failure(validation(
                "material/surface ID source has a face region without a durable material assignment"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> validate_material_surface_id_artifact_source(
    const MaterialSurfaceIdMapArtifact& artifact,
    const project::ProjectDocument& document) {
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = validate_material_surface_id_source(artifact.recipe, document); !result) {
        return result;
    }
    const MaterialSurfaceIdExecutionReport expected =
        compile_material_surface_id_cpu(artifact.recipe, document);
    if (!expected.succeeded()) {
        return core::Result<void>::failure(validation(
            "material/surface ID artifact cannot be verified by the CPU reference"));
    }
    const auto expected_bytes = expected.artifact->serialize();
    const auto artifact_bytes = artifact.serialize();
    if (!expected_bytes || !artifact_bytes ||
        expected_bytes.value() != artifact_bytes.value()) {
        return core::Result<void>::failure(validation(
            "material/surface ID artifact differs from the deterministic CPU reference output"));
    }
    return core::Result<void>::success();
}

core::Result<ProductionCookKey> material_surface_id_cook_key(
    const MaterialSurfaceIdRecipe& recipe,
    std::string platform_profile) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    const auto digest = recipe.canonical_digest();
    if (!digest) return core::Result<ProductionCookKey>::failure(digest.error());
    ProductionCookKey key;
    key.source = AuthoritativeSource{
        recipe.source_namespace, recipe.source_revision, recipe.source_digest};
    key.kind = ArtifactKind::baked_texture;
    key.form = ExecutionForm::offline;
    key.algorithm = std::string(kBakeMaterialSurfaceIdAlgorithm);
    key.settings_digest = hash_text(std::string(kBakeMaterialSurfaceIdAlgorithm) + "\n" +
        digest.value().hex());
    key.platform_profile = std::move(platform_profile);
    key.representation_kind = "material-surface-domain-id-v1";
    if (auto result = key.validate(); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return core::Result<ProductionCookKey>::success(std::move(key));
}

core::Result<ProductionCookKey> register_material_surface_id_graph_node(
    ProductionCookGraph& graph,
    const MaterialSurfaceIdRecipe& recipe,
    std::vector<ProductionCookKey> dependencies,
    std::string platform_profile) {
    const auto key = material_surface_id_cook_key(recipe, std::move(platform_profile));
    if (!key) return core::Result<ProductionCookKey>::failure(key.error());
    if (auto result = graph.add_node(key.value(), std::move(dependencies)); !result) {
        return core::Result<ProductionCookKey>::failure(result.error());
    }
    return key;
}

MaterialSurfaceIdExecutionReport compile_material_surface_id_cpu(
    const MaterialSurfaceIdRecipe& recipe,
    const project::ProjectDocument& document) {
    MaterialSurfaceIdExecutionReport report;
    if (auto result = recipe.validate(); !result) {
        add_material_surface_id_error(report, "material_surface_id_recipe_invalid",
            result.error().message);
        return report;
    }
    if (auto result = validate_material_surface_id_source(recipe, document); !result) {
        add_material_surface_id_error(report, "material_surface_id_source_invalid",
            result.error().message);
        return report;
    }
    const auto mesh = document.meshes().find(recipe.target_mesh_asset);
    const auto triangles = collect_triangles(mesh->second, recipe.target_uv_set);
    if (!triangles) {
        add_material_surface_id_error(report, "material_surface_id_mesh_invalid",
            triangles.error().message);
        return report;
    }
    const auto* material_regions =
        mesh->second.attributes().find(recipe.material_region_layer);
    const auto faces = mesh->second.faces_sorted();
    std::map<geometry::FaceId, std::uint32_t> region_by_face;
    for (std::size_t index = 0U; index < faces.size(); ++index) {
        region_by_face.emplace(faces[index].id,
            *std::get_if<std::uint32_t>(&material_regions->values[index]));
    }
    std::map<geometry::FaceId, surface::SurfaceDomainId> domain_by_face;
    for (const surface::SurfaceDomain& domain : recipe.surface_domains) {
        for (const geometry::FaceId face : domain.source_faces) {
            domain_by_face.emplace(face, domain.id);
        }
    }

    const std::size_t texel_count =
        static_cast<std::size_t>(recipe.width) * recipe.height;
    std::vector<std::uint8_t> output_bytes(
        texel_count * kMaterialSurfaceIdTexelBytes, 0U);
    std::vector<bool> occupied(texel_count, false);
    std::uint64_t raster_candidates = 0U;
    const double tile_left = static_cast<double>(recipe.tile_u);
    const double tile_bottom = static_cast<double>(recipe.tile_v);
    const double tile_right = tile_left + 1.0;
    const double tile_top = tile_bottom + 1.0;
    for (BakeTriangle triangle : triangles.value()) {
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
            add_material_surface_id_error(report, "cross_tile_uv_triangle",
                "material/surface ID output requires each target triangle to remain within one UV tile",
                triangle.face);
            continue;
        }
        double area = edge_value(triangle.uvs[0U], triangle.uvs[1U], triangle.uvs[2U]);
        if (!std::isfinite(area) || std::abs(area) <= kBakeScalarEpsilon) {
            add_material_surface_id_error(report, "degenerate_target_uv",
                "material/surface ID target triangle has a degenerate UV chart",
                triangle.face);
            continue;
        }
        if (area < 0.0) {
            std::swap(triangle.positions[1U], triangle.positions[2U]);
            std::swap(triangle.uvs[1U], triangle.uvs[2U]);
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
                if (++raster_candidates > kMaxBakeRasterCandidates) {
                    add_material_surface_id_error(report, "raster_work_budget_exceeded",
                        "material/surface ID rasterization exceeded its deterministic work budget",
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
                    !inside(edge_two, top_left_edge(triangle.uvs[0U], triangle.uvs[1U]))) continue;
                ++report.target_texels_visited;
                const std::size_t texel = static_cast<std::size_t>(y) * recipe.width + x;
                const std::size_t offset = texel * kMaterialSurfaceIdTexelBytes;
                if (occupied[texel]) {
                    const std::uint64_t prior_face = load_u64(output_bytes, offset);
                    std::ostringstream message;
                    message << "target UV triangles overlap at texel " << x << ',' << y
                            << " (previous face " << prior_face << ')';
                    add_material_surface_id_error(report, "overlapping_uv_islands",
                        message.str(), triangle.face, x, y);
                    continue;
                }
                occupied[texel] = true;
                const auto region = region_by_face.find(triangle.face);
                const auto domain = domain_by_face.find(triangle.face);
                if (region == region_by_face.end() || domain == domain_by_face.end()) {
                    add_material_surface_id_error(report, "face_correspondence_missing",
                        "rasterized face has no material-region or surface-domain correspondence",
                        triangle.face, x, y);
                    continue;
                }
                const auto assignment = document.material_catalog().assignments().find(
                    project::MaterialRegionKey{recipe.target_mesh_asset, region->second});
                if (assignment == document.material_catalog().assignments().end()) {
                    add_material_surface_id_error(report, "material_assignment_missing",
                        "rasterized face material region has no durable catalog assignment",
                        triangle.face, x, y);
                    continue;
                }
                store_u64(output_bytes, offset, triangle.face.value);
                store_u64(output_bytes, offset + 8U, assignment->second.material);
                store_u64(output_bytes, offset + 16U,
                    assignment->second.variant.value_or(0U));
                store_u64(output_bytes, offset + 24U, domain->second.value);
                for (unsigned index = 0U; index < 4U; ++index) {
                    output_bytes[offset + 32U + index] =
                        static_cast<std::uint8_t>((region->second >> (index * 8U)) & 0xffU);
                }
                ++report.covered_texels;
            }
        }
    }
    if (std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        return report;
    }
    if (report.covered_texels == 0U) {
        add_material_surface_id_error(report, "material_surface_id_output_empty",
            "selected target UV tile produced no material/surface ID texels");
        return report;
    }
    MaterialSurfaceIdMapArtifact artifact;
    artifact.recipe = recipe;
    artifact.covered_texels = report.covered_texels;
    artifact.output_bytes = std::move(output_bytes);
    if (auto result = artifact.refresh_digests(); !result) {
        add_material_surface_id_error(report, "material_surface_id_artifact_invalid",
            result.error().message);
        return report;
    }
    if (auto result = artifact.validate(); !result) {
        add_material_surface_id_error(report, "material_surface_id_artifact_invalid",
            result.error().message);
        return report;
    }
    report.artifact = std::move(artifact);
    return report;
}

} // namespace carto::production
