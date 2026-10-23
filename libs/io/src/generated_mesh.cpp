#include <carto/io/generated_mesh.hpp>

#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/core/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace carto::io {
namespace {

using core::Diagnostic;
using core::ErrorCode;
using core::json::Member;

constexpr std::size_t kMaxGltfBytes = 32U * 1024U * 1024U;
constexpr std::size_t kMaxBufferBytes = 24U * 1024U * 1024U;
constexpr std::size_t kMaxGlbBytes = 40U * 1024U * 1024U;
constexpr std::size_t kMaxVertices = 2'000'000U;
constexpr std::size_t kMaxFaces = 2'000'000U;
constexpr std::size_t kMaxArrayItems = 4096U;
constexpr std::string_view kBase64Prefix = "data:application/octet-stream;base64,";

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\n' ||
        value.front() == '\r' || value.front() == '\t')) value.remove_prefix(1U);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\n' ||
        value.back() == '\r' || value.back() == '\t')) value.remove_suffix(1U);
    return value;
}

core::Result<std::vector<Member>> object(std::string_view raw, std::string_view label) {
    auto parsed = core::json::parse_object(raw);
    if (!parsed) return core::Result<std::vector<Member>>::failure(invalid(
        std::string(label) + " is not a valid JSON object"));
    return core::Result<std::vector<Member>>::success(std::move(parsed).value());
}

const Member* member(const std::vector<Member>& fields, std::string_view name) noexcept {
    return core::json::find_member(fields, name);
}

bool only_fields(const std::vector<Member>& fields,
    std::initializer_list<std::string_view> allowed) noexcept {
    return std::all_of(fields.begin(), fields.end(), [allowed](const Member& field) {
        return std::find(allowed.begin(), allowed.end(), field.key) != allowed.end();
    });
}

core::Result<std::uint64_t> uint_field(
    const std::vector<Member>& fields, std::string_view name) {
    const auto* value = member(fields, name);
    if (value == nullptr) return core::Result<std::uint64_t>::failure(
        invalid("generated glTF is missing an integer field"));
    auto parsed = core::json::parse_uint(value->raw_value);
    if (!parsed) return core::Result<std::uint64_t>::failure(
        invalid("generated glTF contains an invalid integer field"));
    return parsed;
}

core::Result<std::string> string_field(
    const std::vector<Member>& fields, std::string_view name) {
    const auto* value = member(fields, name);
    if (value == nullptr) return core::Result<std::string>::failure(
        invalid("generated glTF is missing a string field"));
    auto parsed = core::json::decode_string(value->raw_value);
    if (!parsed) return core::Result<std::string>::failure(
        invalid("generated glTF contains an invalid string field"));
    return parsed;
}

core::Result<std::vector<std::string_view>> array_items(
    std::string_view raw, std::size_t maximum, std::string_view label) {
    raw = trim(raw);
    if (raw.size() < 2U || raw.front() != '[' || raw.back() != ']') {
        return core::Result<std::vector<std::string_view>>::failure(invalid(
            std::string(label) + " is not a JSON array"));
    }
    std::vector<std::string_view> items;
    std::size_t start = 1U;
    std::size_t depth = 0U;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t cursor = 1U; cursor + 1U < raw.size(); ++cursor) {
        const char ch = raw[cursor];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') { quoted = true; continue; }
        if (ch == '[' || ch == '{') { ++depth; continue; }
        if (ch == ']' || ch == '}') {
            if (depth == 0U) return core::Result<std::vector<std::string_view>>::failure(
                invalid(std::string(label) + " nesting is malformed"));
            --depth;
            continue;
        }
        if (ch == ',' && depth == 0U) {
            const auto item = trim(raw.substr(start, cursor - start));
            if (item.empty() || items.size() >= maximum) {
                return core::Result<std::vector<std::string_view>>::failure(invalid(
                    std::string(label) + " has an empty or excessive item"));
            }
            items.push_back(item);
            start = cursor + 1U;
        }
    }
    const auto last = trim(raw.substr(start, raw.size() - 1U - start));
    if (quoted || escaped || depth != 0U || (!last.empty() && items.size() >= maximum)) {
        return core::Result<std::vector<std::string_view>>::failure(invalid(
            std::string(label) + " structure is malformed or excessive"));
    }
    if (!last.empty()) items.push_back(last);
    else if (!items.empty()) return core::Result<std::vector<std::string_view>>::failure(
        invalid(std::string(label) + " has a trailing empty item"));
    return core::Result<std::vector<std::string_view>>::success(std::move(items));
}

int base64_value(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

core::Result<std::vector<std::uint8_t>> decode_base64(std::string_view encoded) {
    if (encoded.empty() || encoded.size() % 4U != 0U ||
        encoded.size() > ((kMaxBufferBytes + 2U) / 3U) * 4U) {
        return core::Result<std::vector<std::uint8_t>>::failure(invalid(
            "generated glTF embedded buffer encoding is invalid or oversized"));
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve((encoded.size() / 4U) * 3U);
    for (std::size_t i = 0U; i < encoded.size(); i += 4U) {
        const bool final = i + 4U == encoded.size();
        const int a = base64_value(encoded[i]);
        const int b = base64_value(encoded[i + 1U]);
        const int c = encoded[i + 2U] == '=' ? -2 : base64_value(encoded[i + 2U]);
        const int d = encoded[i + 3U] == '=' ? -2 : base64_value(encoded[i + 3U]);
        if (a < 0 || b < 0 || (!final && (c < 0 || d < 0)) || c == -1 || d == -1 ||
            (c == -2 && d != -2) ||
            (!final && (encoded[i + 2U] == '=' || encoded[i + 3U] == '='))) {
            return core::Result<std::vector<std::uint8_t>>::failure(invalid(
                "generated glTF embedded buffer has invalid base64 padding"));
        }
        const std::uint32_t block = (static_cast<std::uint32_t>(a) << 18U) |
            (static_cast<std::uint32_t>(b) << 12U) |
            (static_cast<std::uint32_t>(std::max(c, 0)) << 6U) |
            static_cast<std::uint32_t>(std::max(d, 0));
        bytes.push_back(static_cast<std::uint8_t>((block >> 16U) & 0xffU));
        if (c != -2) bytes.push_back(static_cast<std::uint8_t>((block >> 8U) & 0xffU));
        if (d != -2) bytes.push_back(static_cast<std::uint8_t>(block & 0xffU));
        if ((c == -2 && (b & 0x0f) != 0) ||
            (d == -2 && c >= 0 && (c & 0x03) != 0)) {
            return core::Result<std::vector<std::uint8_t>>::failure(invalid(
                "generated glTF embedded buffer has noncanonical base64 padding"));
        }
    }
    if (bytes.empty() || bytes.size() > kMaxBufferBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(invalid(
            "generated glTF embedded buffer is empty or oversized"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

struct BufferView final { std::size_t offset = 0U; std::size_t length = 0U; };
struct Accessor final {
    std::size_t view = 0U;
    std::size_t count = 0U;
    std::uint32_t component_type = 0U;
    std::string type;
};

core::Result<std::size_t> checked_size(
    std::uint64_t value, std::size_t maximum, std::string_view label) {
    if (value > maximum || value > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::size_t>::failure(invalid(
            std::string(label) + " exceeds its supported bound"));
    }
    return core::Result<std::size_t>::success(static_cast<std::size_t>(value));
}

core::Result<std::size_t> accessor_index(
    const std::vector<Member>& primitive, std::string_view name,
    const std::vector<std::vector<Member>>& accessors) {
    const auto* raw = member(primitive, name);
    if (raw == nullptr) return core::Result<std::size_t>::failure(invalid(
        "generated glTF primitive is missing an accessor"));
    auto parsed = core::json::parse_uint(raw->raw_value);
    if (!parsed || parsed.value() >= accessors.size()) {
        return core::Result<std::size_t>::failure(invalid(
            "generated glTF primitive accessor index is invalid"));
    }
    return core::Result<std::size_t>::success(static_cast<std::size_t>(parsed.value()));
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> data, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

float read_f32_le(const std::vector<std::uint8_t>& data, std::size_t offset) noexcept {
    return std::bit_cast<float>(read_u32_le(data, offset));
}

} // namespace

static core::Result<GeneratedMeshImport> import_generated_botanical_gltf_impl(
    std::span<const std::uint8_t> bytes,
    std::span<const std::uint8_t> glb_binary_chunk,
    std::string_view expected_source_identity,
    core::Revision expected_source_revision,
    assets::Sha256Digest expected_artifact_digest) {
    if (bytes.empty() || bytes.size() > kMaxGltfBytes || expected_source_identity.empty() ||
        expected_source_identity.size() > 128U || expected_source_revision.value() == 0U ||
        expected_artifact_digest.is_zero()) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF size or source receipt is invalid"));
    }
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto root = object(text, "generated glTF root");
    if (!root) return core::Result<GeneratedMeshImport>::failure(root.error());
    if (!only_fields(root.value(), {"asset", "scene", "scenes", "nodes", "meshes",
            "materials", "buffers", "bufferViews", "accessors", "extras"})) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF root contains unsupported fields"));
    }
    const auto* asset_raw = member(root.value(), "asset");
    const auto* extras_raw = member(root.value(), "extras");
    if (asset_raw == nullptr || extras_raw == nullptr) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF is missing required metadata"));
    }
    auto asset = object(asset_raw->raw_value, "generated glTF asset");
    auto extras = object(extras_raw->raw_value, "generated glTF extras");
    if (!asset || !extras || !only_fields(asset.value(), {"version", "generator"}) ||
        !only_fields(extras.value(), {"cartographer"})) return core::Result<GeneratedMeshImport>::failure(invalid(
        "generated glTF metadata is malformed"));
    auto version = string_field(asset.value(), "version");
    auto generator = string_field(asset.value(), "generator");
    const auto* cartographer_raw = member(extras.value(), "cartographer");
    if (!version || version.value() != "2.0" || !generator ||
        generator.value() != "Cartographer" || cartographer_raw == nullptr) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF producer profile is not supported"));
    }
    auto provenance = object(cartographer_raw->raw_value, "generated glTF provenance");
    if (!provenance || !only_fields(provenance.value(), {"sourceIdentity", "sourceRevision"})) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF provenance contains unsupported fields"));
    }
    auto source_identity = string_field(provenance.value(), "sourceIdentity");
    auto source_revision = string_field(provenance.value(), "sourceRevision");
    if (!source_identity || source_identity.value() != expected_source_identity || !source_revision ||
        source_revision.value() != std::to_string(expected_source_revision.value())) {
        return core::Result<GeneratedMeshImport>::failure(Diagnostic(
            ErrorCode::stale_data,
            "generated glTF source identity or revision does not match the submitted job"));
    }

    const auto* buffers_raw = member(root.value(), "buffers");
    const auto* views_raw = member(root.value(), "bufferViews");
    const auto* accessors_raw = member(root.value(), "accessors");
    const auto* meshes_raw = member(root.value(), "meshes");
    const auto* materials_raw = member(root.value(), "materials");
    if (buffers_raw == nullptr || views_raw == nullptr || accessors_raw == nullptr ||
        meshes_raw == nullptr || materials_raw == nullptr) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF is missing required mesh data"));
    }
    auto buffers = array_items(buffers_raw->raw_value, 1U, "glTF buffers");
    auto views = array_items(views_raw->raw_value, kMaxArrayItems, "glTF buffer views");
    auto accessors_raw_items = array_items(accessors_raw->raw_value, kMaxArrayItems,
        "glTF accessors");
    auto meshes = array_items(meshes_raw->raw_value, 1U, "glTF meshes");
    auto materials = array_items(materials_raw->raw_value, 16U, "glTF materials");
    if (!buffers || buffers.value().size() != 1U || !views || views.value().empty() ||
        !accessors_raw_items || accessors_raw_items.value().empty() || !meshes ||
        meshes.value().size() != 1U || !materials || materials.value().empty()) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF array cardinality is unsupported"));
    }
    const auto* scene_index_raw = member(root.value(), "scene");
    const auto* scenes_raw = member(root.value(), "scenes");
    const auto* nodes_raw = member(root.value(), "nodes");
    if (scene_index_raw == nullptr || scenes_raw == nullptr || nodes_raw == nullptr) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF scene binding is incomplete"));
    }
    auto scene_index = core::json::parse_uint(scene_index_raw->raw_value);
    auto scenes = array_items(scenes_raw->raw_value, 1U, "glTF scenes");
    auto nodes = array_items(nodes_raw->raw_value, 1U, "glTF nodes");
    if (!scene_index || scene_index.value() != 0U || !scenes || scenes.value().size() != 1U ||
        !nodes || nodes.value().size() != 1U) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF must contain one identity-transform scene node"));
    }
    auto scene = object(scenes.value().front(), "glTF scene");
    auto node = object(nodes.value().front(), "glTF node");
    if (!scene || !node || !only_fields(scene.value(), {"nodes"}) ||
        !only_fields(node.value(), {"mesh"})) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF scene contains unsupported transforms or hierarchy"));
    }
    const auto* scene_nodes_raw = member(scene.value(), "nodes");
    auto scene_nodes = scene_nodes_raw == nullptr
        ? core::Result<std::vector<std::string_view>>::failure(invalid("missing scene nodes"))
        : array_items(scene_nodes_raw->raw_value, 1U, "glTF scene nodes");
    auto node_mesh = uint_field(node.value(), "mesh");
    if (!scene_nodes || scene_nodes.value().size() != 1U ||
        trim(scene_nodes.value().front()) != "0" || !node_mesh || node_mesh.value() != 0U) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF scene does not reference the supported mesh"));
    }
    auto buffer_record = object(buffers.value().front(), "glTF buffer");
    if (!buffer_record || !only_fields(buffer_record.value(), {"byteLength", "uri"})) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF buffer contains unsupported fields"));
    }
    auto declared_length = uint_field(buffer_record.value(), "byteLength");
    if (!declared_length) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF buffer length is invalid"));
    }
    std::vector<std::uint8_t> buffer;
    const auto* uri_raw = member(buffer_record.value(), "uri");
    if (!glb_binary_chunk.empty()) {
        if (uri_raw != nullptr || declared_length.value() > glb_binary_chunk.size() ||
            glb_binary_chunk.size() - declared_length.value() > 3U) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated GLB BIN chunk does not match its single buffer declaration"));
        }
        const auto padding = glb_binary_chunk.subspan(
            static_cast<std::size_t>(declared_length.value()));
        if (!std::all_of(padding.begin(), padding.end(), [](std::uint8_t byte) {
                return byte == 0U;
            })) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated GLB BIN chunk has non-zero alignment padding"));
        }
        const auto buffer_end = glb_binary_chunk.begin() +
            static_cast<std::ptrdiff_t>(declared_length.value());
        buffer.assign(glb_binary_chunk.begin(), buffer_end);
    } else {
        auto uri = string_field(buffer_record.value(), "uri");
        if (!uri || !uri.value().starts_with(kBase64Prefix)) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF must use one bounded embedded binary buffer"));
        }
        auto decoded = decode_base64(std::string_view(uri.value()).substr(kBase64Prefix.size()));
        if (!decoded) return core::Result<GeneratedMeshImport>::failure(decoded.error());
        buffer = std::move(decoded).value();
    }
    if (buffer.empty() || declared_length.value() != buffer.size()) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF buffer length does not match its embedded bytes"));
    }

    std::vector<BufferView> parsed_views;
    parsed_views.reserve(views.value().size());
    for (const auto raw_view : views.value()) {
        auto fields = object(raw_view, "glTF buffer view");
        if (!fields || !only_fields(fields.value(), {"buffer", "byteOffset", "byteLength", "target"})) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF buffer view contains unsupported fields"));
        }
        auto buffer_index = uint_field(fields.value(), "buffer");
        auto offset = uint_field(fields.value(), "byteOffset");
        auto length = uint_field(fields.value(), "byteLength");
        if (!buffer_index || buffer_index.value() != 0U || !offset || !length) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF buffer view reference is invalid"));
        }
        auto safe_offset = checked_size(offset.value(), buffer.size(), "buffer view offset");
        auto safe_length = checked_size(length.value(), buffer.size(), "buffer view length");
        if (!safe_offset || !safe_length || (safe_offset.value() & 3U) != 0U ||
            safe_length.value() == 0U || safe_offset.value() > buffer.size() ||
            safe_length.value() > buffer.size() - safe_offset.value()) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF buffer view exceeds its embedded buffer"));
        }
        parsed_views.push_back({safe_offset.value(), safe_length.value()});
    }

    std::vector<std::vector<Member>> accessors;
    accessors.reserve(accessors_raw_items.value().size());
    for (const auto raw_accessor : accessors_raw_items.value()) {
        auto fields = object(raw_accessor, "glTF accessor");
        if (!fields || !only_fields(fields.value(), {"bufferView", "byteOffset", "componentType",
                "count", "type", "min", "max"})) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF accessor contains unsupported fields"));
        }
        auto view = uint_field(fields.value(), "bufferView");
        auto offset = uint_field(fields.value(), "byteOffset");
        auto component = uint_field(fields.value(), "componentType");
        auto count = uint_field(fields.value(), "count");
        auto type = string_field(fields.value(), "type");
        if (!view || view.value() >= parsed_views.size() || !offset || offset.value() != 0U ||
            !component || !count || !type) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF accessor fields are invalid"));
        }
        auto safe_count = checked_size(count.value(), kMaxVertices * 3U, "accessor count");
        if (!safe_count) return core::Result<GeneratedMeshImport>::failure(safe_count.error());
        accessors.push_back(std::move(fields).value());
    }

    auto mesh_fields = object(meshes.value().front(), "glTF mesh");
    if (!mesh_fields) return core::Result<GeneratedMeshImport>::failure(mesh_fields.error());
    const auto* primitives_raw = member(mesh_fields.value(), "primitives");
    if (primitives_raw == nullptr) return core::Result<GeneratedMeshImport>::failure(invalid(
        "generated glTF mesh has no primitives"));
    auto primitives = array_items(primitives_raw->raw_value, 8U, "glTF primitives");
    if (!primitives || primitives.value().empty()) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF primitive count is invalid"));
    }

    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    std::vector<attributes::AttributeValue> material_indices;
    std::vector<attributes::AttributeValue> corner_uvs;
    std::optional<bool> primitives_have_uvs;
    std::uint64_t next_vertex = 1U;
    std::uint64_t next_face = 1U;
    for (const auto raw_primitive : primitives.value()) {
        auto primitive = object(raw_primitive, "glTF primitive");
        if (!primitive || !only_fields(primitive.value(), {"attributes", "indices", "material", "mode"})) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF primitive contains unsupported fields"));
        }
        auto mode = uint_field(primitive.value(), "mode");
        auto material = uint_field(primitive.value(), "material");
        if (!mode || mode.value() != 4U || !material || material.value() >= materials.value().size()) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF accepts only bounded triangle primitives and declared materials"));
        }
        const auto* attrs_raw = member(primitive.value(), "attributes");
        if (attrs_raw == nullptr) return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF primitive has no attributes"));
        auto attrs = object(attrs_raw->raw_value, "glTF primitive attributes");
        if (!attrs || !only_fields(attrs.value(), {"POSITION", "NORMAL", "TEXCOORD_0"}) ||
            attrs.value().size() < 2U || attrs.value().size() > 3U) return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF primitive attributes are not supported"));
        auto positions_index = accessor_index(attrs.value(), "POSITION", accessors);
        auto normals_index = accessor_index(attrs.value(), "NORMAL", accessors);
        auto indices_index = accessor_index(primitive.value(), "indices", accessors);
        if (!positions_index || !normals_index || !indices_index) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF primitive accessor binding is invalid"));
        }
        const bool has_uvs = member(attrs.value(), "TEXCOORD_0") != nullptr;
        if (primitives_have_uvs.has_value() && *primitives_have_uvs != has_uvs) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF primitives must consistently provide TEXCOORD_0"));
        }
        primitives_have_uvs = has_uvs;
        std::optional<std::size_t> uvs_index;
        if (has_uvs) {
            auto parsed_index = accessor_index(attrs.value(), "TEXCOORD_0", accessors);
            if (!parsed_index) return core::Result<GeneratedMeshImport>::failure(
                parsed_index.error());
            uvs_index = parsed_index.value();
        }
        const auto accessor_meta = [&accessors](std::size_t index) -> core::Result<Accessor> {
            const auto& fields = accessors[index];
            auto view = uint_field(fields, "bufferView");
            auto component = uint_field(fields, "componentType");
            auto count = uint_field(fields, "count");
            auto type = string_field(fields, "type");
            if (!view || !component || !count || !type ||
                view.value() > std::numeric_limits<std::size_t>::max() ||
                count.value() > std::numeric_limits<std::size_t>::max() ||
                component.value() > std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<Accessor>::failure(invalid(
                    "generated glTF accessor metadata is invalid"));
            }
            return core::Result<Accessor>::success(Accessor{
                static_cast<std::size_t>(view.value()), static_cast<std::size_t>(count.value()),
                static_cast<std::uint32_t>(component.value()), std::move(type).value()});
        };
        auto positions = accessor_meta(positions_index.value());
        auto normals = accessor_meta(normals_index.value());
        auto indices = accessor_meta(indices_index.value());
        std::optional<Accessor> uvs;
        if (uvs_index.has_value()) {
            auto parsed_uvs = accessor_meta(*uvs_index);
            if (!parsed_uvs) return core::Result<GeneratedMeshImport>::failure(
                parsed_uvs.error());
            uvs = std::move(parsed_uvs).value();
        }
        if (!positions || !normals || !indices || positions.value().type != "VEC3" ||
            normals.value().type != "VEC3" || indices.value().type != "SCALAR" ||
            positions.value().component_type != 5126U || normals.value().component_type != 5126U ||
            indices.value().component_type != 5125U ||
            positions.value().count != normals.value().count ||
            (uvs.has_value() && (uvs->type != "VEC2" || uvs->component_type != 5126U ||
                uvs->count != positions.value().count)) ||
            indices.value().count == 0U || indices.value().count % 3U != 0U ||
            positions.value().count > kMaxVertices - vertices.size() ||
            indices.value().count / 3U > kMaxFaces - faces.size()) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF attribute types, cardinalities, or budgets are invalid"));
        }
        const auto view_fits = [&parsed_views](const Accessor& accessor,
            std::size_t required_bytes) {
            return accessor.view < parsed_views.size() &&
                parsed_views[accessor.view].length == required_bytes;
        };
        if (!view_fits(positions.value(), positions.value().count * 12U) ||
            !view_fits(normals.value(), normals.value().count * 12U) ||
            !view_fits(indices.value(), indices.value().count * 4U) ||
            (uvs.has_value() && !view_fits(*uvs, uvs->count * 8U))) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated glTF accessor byte lengths do not match their declared types"));
        }
        const auto position_offset = parsed_views[positions.value().view].offset;
        const auto normal_offset = parsed_views[normals.value().view].offset;
        const auto index_offset = parsed_views[indices.value().view].offset;
        std::vector<core::Vec2d> vertex_uvs;
        if (uvs.has_value()) {
            const auto uv_offset = parsed_views[uvs->view].offset;
            vertex_uvs.reserve(uvs->count);
            for (std::size_t vertex = 0U; vertex < uvs->count; ++vertex) {
                const core::Vec2d uv{
                    read_f32_le(buffer, uv_offset + vertex * 8U),
                    read_f32_le(buffer, uv_offset + vertex * 8U + 4U)};
                if (!uv.finite() || uv.x < 0.0 || uv.x > 1.0 ||
                    uv.y < 0.0 || uv.y > 1.0) {
                    return core::Result<GeneratedMeshImport>::failure(invalid(
                        "generated botanical UV coordinates must be finite and normalized"));
                }
                vertex_uvs.push_back(uv);
            }
        }
        const std::uint64_t first_vertex = next_vertex;
        for (std::size_t vertex = 0U; vertex < positions.value().count; ++vertex) {
            const std::size_t offset = position_offset + vertex * 12U;
            const core::Vec3d point{read_f32_le(buffer, offset),
                read_f32_le(buffer, offset + 4U), read_f32_le(buffer, offset + 8U)};
            const core::Vec3d normal{read_f32_le(buffer, normal_offset + vertex * 12U),
                read_f32_le(buffer, normal_offset + vertex * 12U + 4U),
                read_f32_le(buffer, normal_offset + vertex * 12U + 8U)};
            if (!point.finite() || !normal.finite() || normal.length() <= 1.0e-12 ||
                next_vertex == std::numeric_limits<std::uint64_t>::max()) {
                return core::Result<GeneratedMeshImport>::failure(invalid(
                    "generated glTF positions or normals are non-finite or invalid"));
            }
            vertices.push_back({geometry::VertexId{next_vertex++}, point});
        }
        for (std::size_t triangle = 0U; triangle < indices.value().count; triangle += 3U) {
            std::vector<geometry::VertexId> boundary;
            boundary.reserve(3U);
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const auto local = read_u32_le(buffer, index_offset + (triangle + corner) * 4U);
                if (local >= positions.value().count) {
                    return core::Result<GeneratedMeshImport>::failure(invalid(
                        "generated glTF triangle index is outside its position accessor"));
                }
                boundary.emplace_back(geometry::VertexId{first_vertex + local});
                if (uvs.has_value()) corner_uvs.emplace_back(vertex_uvs[local]);
            }
            if (boundary[0] == boundary[1] || boundary[1] == boundary[2] ||
                boundary[2] == boundary[0] || next_face == std::numeric_limits<std::uint64_t>::max()) {
                return core::Result<GeneratedMeshImport>::failure(invalid(
                    "generated glTF contains a degenerate or unrepresentable triangle"));
            }
            faces.push_back({geometry::FaceId{next_face++}, std::move(boundary)});
            material_indices.emplace_back(static_cast<std::uint32_t>(material.value()));
        }
    }
    geometry::EditableMesh mesh;
    if (auto inserted = mesh.insert_bulk(std::move(vertices), std::move(faces)); !inserted) {
        return core::Result<GeneratedMeshImport>::failure(
            inserted.error().with_context("generated glTF topology admission"));
    }
    std::map<attributes::AttributeDomain, std::size_t> domain_counts{
        {attributes::AttributeDomain::face, material_indices.size()}};
    if (primitives_have_uvs.value_or(false)) {
        domain_counts.emplace(attributes::AttributeDomain::corner, corner_uvs.size());
    }
    attributes::AttributeSet attributes(std::move(domain_counts));
    if (auto added = attributes.add_layer(attributes::AttributeLayer{
            {"gltf.material_index", attributes::AttributeDomain::face,
                attributes::AttributeType::uint32}, std::move(material_indices)}); !added) {
        return core::Result<GeneratedMeshImport>::failure(
            added.error().with_context("generated glTF material index admission"));
    }
    attributes::UvSetTable uv_sets;
    if (primitives_have_uvs.value_or(false)) {
        if (auto added = attributes.add_layer(attributes::AttributeLayer{
                {"cartographer.uv0", attributes::AttributeDomain::corner,
                    attributes::AttributeType::vec2}, std::move(corner_uvs)}); !added) {
            return core::Result<GeneratedMeshImport>::failure(
                added.error().with_context("generated glTF UV admission"));
        }
        if (auto added = uv_sets.add(attributes::UvSetDescriptor{
                1U, "Generated botanical UV", "cartographer.uv0",
                true, true, false, 1U}); !added) {
            return core::Result<GeneratedMeshImport>::failure(
                added.error().with_context("generated glTF UV-set admission"));
        }
    }
    if (auto attached = mesh.set_attribute_payload(
            std::move(attributes), std::move(uv_sets)); !attached) {
        return core::Result<GeneratedMeshImport>::failure(
            attached.error().with_context("generated glTF material index attachment"));
    }
    if (auto valid = mesh.validate(); !valid) {
        return core::Result<GeneratedMeshImport>::failure(
            valid.error().with_context("generated glTF mesh validation"));
    }
    return core::Result<GeneratedMeshImport>::success(GeneratedMeshImport{
        std::move(mesh), std::move(source_identity).value(), expected_source_revision,
        expected_artifact_digest});
}

core::Result<GeneratedMeshImport> import_generated_botanical_gltf(
    std::span<const std::uint8_t> bytes,
    std::string_view expected_source_identity,
    core::Revision expected_source_revision,
    assets::Sha256Digest expected_artifact_digest) {
    if (bytes.empty() || bytes.size() > kMaxGltfBytes || expected_artifact_digest.is_zero() ||
        assets::sha256(bytes) != expected_artifact_digest) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated glTF size or artifact digest is invalid"));
    }
    return import_generated_botanical_gltf_impl(
        bytes, {}, expected_source_identity, expected_source_revision,
        expected_artifact_digest);
}

core::Result<GeneratedMeshImport> import_generated_botanical_glb(
    std::span<const std::uint8_t> bytes,
    std::string_view expected_source_identity,
    core::Revision expected_source_revision,
    assets::Sha256Digest expected_artifact_digest) {
    constexpr std::uint32_t kGlbMagic = 0x46546c67U;
    constexpr std::uint32_t kGlbVersion = 2U;
    constexpr std::uint32_t kJsonChunkType = 0x4e4f534aU;
    constexpr std::uint32_t kBinChunkType = 0x004e4942U;
    constexpr std::size_t kGlbHeaderBytes = 12U;
    constexpr std::size_t kChunkHeaderBytes = 8U;

    if (bytes.size() < kGlbHeaderBytes + kChunkHeaderBytes ||
        bytes.size() > kMaxGlbBytes || expected_artifact_digest.is_zero() ||
        assets::sha256(bytes) != expected_artifact_digest ||
        read_u32_le(bytes, 0U) != kGlbMagic ||
        read_u32_le(bytes, 4U) != kGlbVersion ||
        read_u32_le(bytes, 8U) != bytes.size()) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated GLB header, size, or artifact digest is invalid"));
    }

    std::size_t cursor = kGlbHeaderBytes;
    const std::size_t json_length = read_u32_le(bytes, cursor);
    const std::uint32_t json_type = read_u32_le(bytes, cursor + 4U);
    cursor += kChunkHeaderBytes;
    if (json_type != kJsonChunkType || json_length == 0U || (json_length & 3U) != 0U ||
        json_length > kMaxGltfBytes || json_length > bytes.size() - cursor) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated GLB must begin with one bounded, aligned JSON chunk"));
    }
    const auto json_chunk = bytes.subspan(cursor, json_length);
    cursor += json_length;

    std::span<const std::uint8_t> binary_chunk;
    if (cursor < bytes.size()) {
        if (bytes.size() - cursor < kChunkHeaderBytes) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated GLB has a truncated trailing chunk header"));
        }
        const std::size_t binary_length = read_u32_le(bytes, cursor);
        const std::uint32_t binary_type = read_u32_le(bytes, cursor + 4U);
        cursor += kChunkHeaderBytes;
        if (binary_type != kBinChunkType || binary_length == 0U ||
            (binary_length & 3U) != 0U || binary_length > kMaxBufferBytes ||
            binary_length != bytes.size() - cursor) {
            return core::Result<GeneratedMeshImport>::failure(invalid(
                "generated GLB has an unsupported, unbounded, or malformed BIN chunk"));
        }
        binary_chunk = bytes.subspan(cursor, binary_length);
        cursor += binary_length;
    }
    if (cursor != bytes.size()) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated GLB contains trailing or unsupported chunks"));
    }
    if (std::any_of(json_chunk.begin(), json_chunk.end(), [](std::uint8_t byte) {
            return byte == 0U;
        })) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated GLB JSON chunk contains NUL padding"));
    }
    auto json_end = json_chunk.end();
    while (json_end != json_chunk.begin() && json_end[-1] == static_cast<std::uint8_t>(' ')) {
        --json_end;
    }
    if (json_end == json_chunk.begin() || json_end[-1] != static_cast<std::uint8_t>('}')) {
        return core::Result<GeneratedMeshImport>::failure(invalid(
            "generated GLB JSON chunk has noncanonical trailing padding"));
    }

    return import_generated_botanical_gltf_impl(
        json_chunk, binary_chunk, expected_source_identity, expected_source_revision,
        expected_artifact_digest);
}

} // namespace carto::io
