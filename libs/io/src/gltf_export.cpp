#include <carto/io/gltf_export.hpp>

#include <carto/project/project.hpp>
#include <carto/render/tangent.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#endif

namespace carto::io {

namespace {

using core::Diagnostic;
using core::ErrorCode;

constexpr std::uint32_t kGlFloat = 5126U;
constexpr std::uint32_t kGlUnsignedShort = 5123U;
constexpr std::uint32_t kGlUnsignedInt = 5125U;
constexpr std::uint32_t kGlArrayBuffer = 34962U;
constexpr std::uint32_t kGlElementArrayBuffer = 34963U;
constexpr std::uint32_t kGlTriangles = 4U;
constexpr std::size_t kMaxExportBufferBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxExportJsonBytes = 512ULL * 1024ULL * 1024ULL;

std::mutex g_gltf_export_mutex;
std::atomic<std::uint64_t> g_gltf_export_temp_counter{0U};

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

Diagnostic io_error(std::string message) {
    return Diagnostic(ErrorCode::io_error, std::move(message));
}

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_gltf_export_temp_counter.fetch_add(1U, std::memory_order_relaxed);
    return target.parent_path() / (target.filename().string() + ".carto.tmp-" +
        std::to_string(static_cast<unsigned long long>(ticks)) + "-" +
        std::to_string(static_cast<unsigned long long>(thread)) + "-" +
        std::to_string(static_cast<unsigned long long>(counter)));
}

core::Result<void> atomic_replace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.wstring().c_str(),
            target.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return core::Result<void>::failure(io_error("atomic glTF replacement failed on Windows"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        return core::Result<void>::failure(
            io_error("atomic glTF replacement failed: " + error.message()));
    }
    return core::Result<void>::success();
#endif
}

std::string json_escape(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (byte < 0x20U) {
                escaped += "\\u00";
                escaped.push_back(hex[byte >> 4U]);
                escaped.push_back(hex[byte & 0x0fU]);
            } else {
                escaped.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    return escaped;
}

void append_bytes(std::vector<std::uint8_t>& buffer, const void* data, std::size_t bytes) {
    const auto* source = static_cast<const std::uint8_t*>(data);
    buffer.insert(buffer.end(), source, source + bytes);
}

void align_four(std::vector<std::uint8_t>& buffer) {
    while ((buffer.size() & 3U) != 0U) buffer.push_back(0U);
}

template <typename T>
void append_vector(std::vector<std::uint8_t>& buffer, const std::vector<T>& values) {
    if (!values.empty()) append_bytes(buffer, values.data(), values.size() * sizeof(T));
}

std::string base64(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t offset = 0U; offset < bytes.size(); offset += 3U) {
        const std::size_t remaining = bytes.size() - offset;
        const std::uint32_t first = bytes[offset];
        const std::uint32_t second = remaining > 1U ? bytes[offset + 1U] : 0U;
        const std::uint32_t third = remaining > 2U ? bytes[offset + 2U] : 0U;
        const std::uint32_t packed = (first << 16U) | (second << 8U) | third;
        encoded.push_back(alphabet[(packed >> 18U) & 0x3fU]);
        encoded.push_back(alphabet[(packed >> 12U) & 0x3fU]);
        encoded.push_back(remaining > 1U ? alphabet[(packed >> 6U) & 0x3fU] : '=');
        encoded.push_back(remaining > 2U ? alphabet[packed & 0x3fU] : '=');
    }
    return encoded;
}

struct BufferView {
    std::size_t offset = 0U;
    std::size_t length = 0U;
    std::uint32_t target = 0U;
};

struct Accessor {
    std::size_t buffer_view = 0U;
    std::uint32_t component_type = 0U;
    std::size_t count = 0U;
    std::string type;
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    bool has_bounds = false;
};

struct MeshRecord {
    std::uint64_t source_id = 0U;
    std::string name;
    std::size_t position_accessor = 0U;
    std::size_t normal_accessor = 0U;
    std::optional<std::size_t> uv_accessor;
    std::optional<std::size_t> tangent_accessor;
    std::size_t index_accessor = 0U;
    std::size_t vertices = 0U;
    std::size_t triangles = 0U;
    std::size_t index_bytes = 0U;
    std::optional<std::vector<std::pair<std::uint64_t, std::uint32_t>>> material_regions;
};

struct NodeRecord {
    scene::ObjectId id;
    std::string name;
    std::optional<std::size_t> parent;
    std::optional<std::size_t> mesh;
    core::Transform transform;
    bool visible = true;
    bool locked = false;
};

core::Result<float> finite_float(double value, std::string_view field) {
    const float narrowed = static_cast<float>(value);
    if (!std::isfinite(narrowed)) {
        return core::Result<float>::failure(validation(
            "glTF export cannot represent " + std::string(field) + " as float32"));
    }
    return core::Result<float>::success(narrowed);
}

core::Result<std::array<float, 3>> finite_vec3(
    core::Vec3d value,
    std::string_view field) {
    const auto x = finite_float(value.x, field);
    const auto y = finite_float(value.y, field);
    const auto z = finite_float(value.z, field);
    if (!x || !y || !z) {
        return core::Result<std::array<float, 3>>::failure(
            (!x ? x.error() : !y ? y.error() : z.error()));
    }
    return core::Result<std::array<float, 3>>::success({x.value(), y.value(), z.value()});
}

core::Result<std::array<float, 2>> finite_vec2(
    core::Vec2d value,
    std::string_view field) {
    const auto x = finite_float(value.x, field);
    const auto y = finite_float(value.y, field);
    if (!x || !y) {
        return core::Result<std::array<float, 2>>::failure(!x ? x.error() : y.error());
    }
    return core::Result<std::array<float, 2>>::success({x.value(), y.value()});
}

struct UvCornerPayload {
    std::map<std::pair<std::uint64_t, std::uint64_t>, core::Vec2d> values;
};

using MaterialRegionPayload = std::vector<std::pair<std::uint64_t, std::uint32_t>>;

core::Result<std::optional<MaterialRegionPayload>> condition_material_regions(
    const geometry::EditableMesh& mesh) {
    constexpr std::string_view kLayerId = "condition.material_region";
    const auto* layer = mesh.attributes().find(kLayerId);
    if (layer == nullptr) {
        return core::Result<std::optional<MaterialRegionPayload>>::success(std::nullopt);
    }
    if (layer->descriptor.domain != attributes::AttributeDomain::face ||
        layer->descriptor.type != attributes::AttributeType::uint32) {
        return core::Result<std::optional<MaterialRegionPayload>>::failure(validation(
            "condition.material_region must be a face-domain uint32 layer"));
    }
    const auto faces = mesh.faces_sorted();
    const auto face_count = mesh.attributes().domain_count(attributes::AttributeDomain::face);
    if (!face_count.has_value() || *face_count != faces.size() ||
        layer->values.size() != faces.size()) {
        return core::Result<std::optional<MaterialRegionPayload>>::failure(validation(
            "condition.material_region cardinality does not match mesh faces"));
    }

    MaterialRegionPayload payload;
    payload.reserve(faces.size());
    for (std::size_t index = 0U; index < faces.size(); ++index) {
        const auto* value = std::get_if<std::uint32_t>(&layer->values[index]);
        if (value == nullptr || *value == 0U) {
            return core::Result<std::optional<MaterialRegionPayload>>::failure(validation(
                "condition.material_region values must be non-zero uint32 region IDs"));
        }
        payload.emplace_back(faces[index].id.value, *value);
    }
    return core::Result<std::optional<MaterialRegionPayload>>::success(std::move(payload));
}

core::Result<std::optional<UvCornerPayload>> active_render_uv(
    const geometry::EditableMesh& mesh) {
    const auto* active = mesh.uv_sets().active_for_render();
    if (active == nullptr) {
        return core::Result<std::optional<UvCornerPayload>>::success(std::nullopt);
    }
    const auto* layer = mesh.attributes().find(active->layer_id);
    if (layer == nullptr || layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2) {
        return core::Result<std::optional<UvCornerPayload>>::failure(validation(
            "active render UV set does not reference a corner-domain vec2 layer"));
    }
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<std::optional<UvCornerPayload>>::failure(topology.error());
    }
    if (layer->values.size() != topology.value().corners.size()) {
        return core::Result<std::optional<UvCornerPayload>>::failure(validation(
            "active render UV layer cardinality does not match mesh corners"));
    }

    UvCornerPayload payload;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto* value = std::get_if<core::Vec2d>(&layer->values[index]);
        if (value == nullptr || !value->finite()) {
            return core::Result<std::optional<UvCornerPayload>>::failure(validation(
                "active render UV layer contains a non-finite or non-vec2 value"));
        }
        const auto key = std::make_pair(
            topology.value().corners[index].face.value,
            topology.value().corners[index].vertex.value);
        if (!payload.values.emplace(key, *value).second) {
            return core::Result<std::optional<UvCornerPayload>>::failure(validation(
                "active render UV layer contains duplicate face-corner identity"));
        }
    }
    return core::Result<std::optional<UvCornerPayload>>::success(std::move(payload));
}

core::Result<void> append_transform_json(
    std::ostringstream& json,
    const core::Transform& transform) {
    const auto translation = finite_vec3(transform.translation, "transform translation");
    const auto scale = finite_vec3(transform.scale, "transform scale");
    const auto rotation_x = finite_float(transform.rotation.x, "transform rotation");
    const auto rotation_y = finite_float(transform.rotation.y, "transform rotation");
    const auto rotation_z = finite_float(transform.rotation.z, "transform rotation");
    const auto rotation_w = finite_float(transform.rotation.w, "transform rotation");
    if (!translation || !scale || !rotation_x || !rotation_y || !rotation_z || !rotation_w) {
        if (!translation) return core::Result<void>::failure(translation.error());
        if (!scale) return core::Result<void>::failure(scale.error());
        if (!rotation_x) return core::Result<void>::failure(rotation_x.error());
        if (!rotation_y) return core::Result<void>::failure(rotation_y.error());
        if (!rotation_z) return core::Result<void>::failure(rotation_z.error());
        return core::Result<void>::failure(rotation_w.error());
    }
    json << std::setprecision(9)
         << "\"translation\":[" << translation.value()[0] << ',' << translation.value()[1]
         << ',' << translation.value()[2] << "],\"rotation\":[" << rotation_x.value() << ','
         << rotation_y.value() << ',' << rotation_z.value() << ',' << rotation_w.value()
         << "],\"scale\":[" << scale.value()[0] << ',' << scale.value()[1] << ','
         << scale.value()[2] << ']';
    return core::Result<void>::success();
}

void append_float_array(
    std::ostringstream& json,
    const std::array<float, 3>& values) {
    json << std::setprecision(9) << '[' << values[0] << ',' << values[1] << ',' << values[2] << ']';
}

} // namespace

core::Result<GltfExportReport> export_gltf(
    const project::ProjectDocument& document,
    const std::filesystem::path& path) {
    std::lock_guard lock(g_gltf_export_mutex);
    if (path.empty() || path.filename().empty()) {
        return core::Result<GltfExportReport>::failure(
            invalid("glTF export path must name a file"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code parent_error;
        const bool parent_exists = std::filesystem::exists(parent, parent_error);
        if (parent_error || !parent_exists ||
            !std::filesystem::is_directory(parent, parent_error) || parent_error) {
            return core::Result<GltfExportReport>::failure(
                io_error("glTF export parent directory is unavailable"));
        }
    }
    if (auto valid = document.validate(); !valid) {
        return core::Result<GltfExportReport>::failure(
            valid.error().with_context("glTF export source project"));
    }
    if (document.meshes().empty()) {
        return core::Result<GltfExportReport>::failure(
            validation("glTF export requires at least one mesh asset"));
    }

    std::vector<std::uint8_t> buffer;
    std::vector<BufferView> views;
    std::vector<Accessor> accessors;
    std::vector<MeshRecord> meshes;
    meshes.reserve(document.meshes().size());
    GltfExportReport report;
    report.path = path;
    report.source_revision = document.revision();
    report.objects = document.scene().size();
    report.meshes = document.meshes().size();
    bool any_active_uv = false;
    bool any_missing_uv = false;
    bool any_missing_tangent = false;
    bool any_material_regions = false;

    for (const auto& [mesh_id, mesh] : document.meshes()) {
        auto compiled = mesh.compile();
        if (!compiled) {
            return core::Result<GltfExportReport>::failure(
                compiled.error().with_context("mesh asset " + std::to_string(mesh_id)));
        }
        const auto& source = compiled.value();
        if (source.positions.empty() || source.indices.empty() || source.indices.size() % 3U != 0U) {
            return core::Result<GltfExportReport>::failure(
                validation("glTF export requires non-empty triangle meshes"));
        }

        const auto active_uv = active_render_uv(mesh);
        if (!active_uv) {
            return core::Result<GltfExportReport>::failure(
                active_uv.error().with_context("mesh asset " + std::to_string(mesh_id)));
        }
        const auto material_regions = condition_material_regions(mesh);
        if (!material_regions) {
            return core::Result<GltfExportReport>::failure(
                material_regions.error().with_context("mesh asset " + std::to_string(mesh_id)));
        }
        const bool has_uv = active_uv.value().has_value();
        any_active_uv = any_active_uv || has_uv;
        any_missing_uv = any_missing_uv || !has_uv;
        any_material_regions = any_material_regions || material_regions.value().has_value();

        std::vector<core::Vec3d> render_positions = source.positions;
        std::vector<core::Vec3d> render_normals = source.normals;
        std::vector<std::uint32_t> render_indices = source.indices;
        std::vector<std::array<float, 2>> texcoords;
        std::vector<std::array<float, 4>> tangents;
        if (has_uv) {
            const auto& uv_payload = *active_uv.value();
            render_positions.clear();
            render_normals.clear();
            render_indices.clear();
            texcoords.clear();
            render_positions.reserve(source.indices.size());
            render_normals.reserve(source.indices.size());
            texcoords.reserve(source.indices.size());
            render_indices.reserve(source.indices.size());

            std::map<std::uint64_t, std::uint32_t> compiled_indices;
            for (std::size_t index = 0U; index < source.vertex_ids.size(); ++index) {
                compiled_indices.emplace(source.vertex_ids[index].value,
                    static_cast<std::uint32_t>(index));
            }
            const auto append_corner = [&](std::uint64_t face_id,
                                           geometry::VertexId vertex_id) -> core::Result<void> {
                const auto compiled_index = compiled_indices.find(vertex_id.value);
                if (compiled_index == compiled_indices.end()) {
                    return core::Result<void>::failure(validation(
                        "active render UV references a vertex missing from compiled geometry"));
                }
                const auto uv = uv_payload.values.find(std::make_pair(face_id, vertex_id.value));
                if (uv == uv_payload.values.end()) {
                    return core::Result<void>::failure(validation(
                        "active render UV is missing a face-corner value"));
                }
                const auto narrowed_uv = finite_vec2(uv->second, "mesh UV");
                if (!narrowed_uv) return core::Result<void>::failure(narrowed_uv.error());
                const std::uint32_t source_index = compiled_index->second;
                render_positions.push_back(source.positions[source_index]);
                render_normals.push_back(source.normals[source_index]);
                texcoords.push_back(narrowed_uv.value());
                render_indices.push_back(static_cast<std::uint32_t>(render_indices.size()));
                return core::Result<void>::success();
            };

            for (const auto& face : mesh.faces_sorted()) {
                for (std::size_t index = 1U; index + 1U < face.vertices.size(); ++index) {
                    if (auto result = append_corner(face.id.value, face.vertices.front()); !result) {
                        return core::Result<GltfExportReport>::failure(result.error());
                    }
                    if (auto result = append_corner(face.id.value, face.vertices[index]); !result) {
                        return core::Result<GltfExportReport>::failure(result.error());
                    }
                    if (auto result = append_corner(face.id.value, face.vertices[index + 1U]); !result) {
                        return core::Result<GltfExportReport>::failure(result.error());
                    }
                }
            }
            if (render_indices.size() != source.indices.size() ||
                texcoords.size() != render_positions.size()) {
                return core::Result<GltfExportReport>::failure(validation(
                    "corner-domain UV expansion changed the source triangle count"));
            }

            std::vector<render::TangentVertex> tangent_input;
            tangent_input.reserve(render_positions.size());
            for (std::size_t index = 0U; index < render_positions.size(); ++index) {
                tangent_input.push_back({
                    render_positions[index],
                    render_normals[index],
                    {static_cast<double>(texcoords[index][0]),
                     static_cast<double>(texcoords[index][1])},
                });
            }
            const auto generated_tangents = render::generate_tangents(
                tangent_input, render_indices);
            if (!generated_tangents) {
                any_missing_tangent = true;
            } else {
                tangents.reserve(generated_tangents.value().size());
                for (const auto tangent : generated_tangents.value()) {
                    const auto x = finite_float(tangent.x, "mesh tangent");
                    const auto y = finite_float(tangent.y, "mesh tangent");
                    const auto z = finite_float(tangent.z, "mesh tangent");
                    const auto w = finite_float(tangent.w, "mesh tangent handedness");
                    if (!x || !y || !z || !w) {
                        any_missing_tangent = true;
                        tangents.clear();
                        break;
                    }
                    tangents.push_back({x.value(), y.value(), z.value(), w.value()});
                }
            }
        }

        std::vector<std::array<float, 3>> positions;
        std::vector<std::array<float, 3>> normals;
        positions.reserve(render_positions.size());
        normals.reserve(render_normals.size());
        std::array<float, 3> minimum{
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::infinity(),
        };
        std::array<float, 3> maximum{
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
        };
        for (const auto position : render_positions) {
            const auto narrowed = finite_vec3(position, "mesh position");
            if (!narrowed) return core::Result<GltfExportReport>::failure(narrowed.error());
            positions.push_back(narrowed.value());
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
                minimum[axis] = std::min(minimum[axis], narrowed.value()[axis]);
                maximum[axis] = std::max(maximum[axis], narrowed.value()[axis]);
            }
        }
        for (const auto normal : render_normals) {
            const auto narrowed = finite_vec3(normal, "mesh normal");
            if (!narrowed) return core::Result<GltfExportReport>::failure(narrowed.error());
            const float magnitude = std::sqrt(
                narrowed.value()[0] * narrowed.value()[0] +
                narrowed.value()[1] * narrowed.value()[1] +
                narrowed.value()[2] * narrowed.value()[2]);
            if (!(magnitude > 1.0e-6F) || !std::isfinite(magnitude)) {
                return core::Result<GltfExportReport>::failure(
                    validation("glTF export produced a zero-length mesh normal"));
            }
            normals.push_back({
                narrowed.value()[0] / magnitude,
                narrowed.value()[1] / magnitude,
                narrowed.value()[2] / magnitude,
            });
        }

        const bool use_uint16 = render_positions.size() <= 65'535U;
        std::vector<std::uint16_t> indices_16;
        std::vector<std::uint32_t> indices_32;
        if (use_uint16) {
            indices_16.reserve(render_indices.size());
            for (const std::uint32_t index : render_indices) {
                if (index > std::numeric_limits<std::uint16_t>::max()) {
                    return core::Result<GltfExportReport>::failure(
                        validation("glTF 16-bit index selection was unsafe"));
                }
                indices_16.push_back(static_cast<std::uint16_t>(index));
            }
        } else {
            indices_32 = render_indices;
            report.used_uint32_indices = true;
        }

        align_four(buffer);
        const std::size_t position_offset = buffer.size();
        append_vector(buffer, positions);
        views.push_back({position_offset, positions.size() * sizeof(positions.front()), kGlArrayBuffer});
        const std::size_t position_view = views.size() - 1U;
        accessors.push_back({
            position_view, kGlFloat, positions.size(), "VEC3", minimum, maximum, true,
        });
        const std::size_t position_accessor = accessors.size() - 1U;

        align_four(buffer);
        const std::size_t normal_offset = buffer.size();
        append_vector(buffer, normals);
        views.push_back({normal_offset, normals.size() * sizeof(normals.front()), kGlArrayBuffer});
        const std::size_t normal_view = views.size() - 1U;
        accessors.push_back({
            normal_view, kGlFloat, normals.size(), "VEC3", {}, {}, false,
        });
        const std::size_t normal_accessor = accessors.size() - 1U;

        std::optional<std::size_t> uv_accessor;
        if (!texcoords.empty()) {
            align_four(buffer);
            const std::size_t uv_offset = buffer.size();
            append_vector(buffer, texcoords);
            views.push_back({uv_offset, texcoords.size() * sizeof(texcoords.front()), kGlArrayBuffer});
            const std::size_t uv_view = views.size() - 1U;
            accessors.push_back({
                uv_view, kGlFloat, texcoords.size(), "VEC2", {}, {}, false,
            });
            uv_accessor = accessors.size() - 1U;
        }

        std::optional<std::size_t> tangent_accessor;
        if (!tangents.empty()) {
            align_four(buffer);
            const std::size_t tangent_offset = buffer.size();
            append_vector(buffer, tangents);
            views.push_back({tangent_offset,
                tangents.size() * sizeof(tangents.front()), kGlArrayBuffer});
            const std::size_t tangent_view = views.size() - 1U;
            accessors.push_back({
                tangent_view, kGlFloat, tangents.size(), "VEC4", {}, {}, false,
            });
            tangent_accessor = accessors.size() - 1U;
        }

        align_four(buffer);
        const std::size_t index_offset = buffer.size();
        if (use_uint16) append_vector(buffer, indices_16);
        else append_vector(buffer, indices_32);
        const std::size_t index_bytes = use_uint16 ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
        views.push_back({index_offset, render_indices.size() * index_bytes, kGlElementArrayBuffer});
        const std::size_t index_view = views.size() - 1U;
        accessors.push_back({
            index_view,
            use_uint16 ? kGlUnsignedShort : kGlUnsignedInt,
            render_indices.size(),
            "SCALAR",
            {},
            {},
            false,
        });
        const std::size_t index_accessor = accessors.size() - 1U;

        if (buffer.size() > kMaxExportBufferBytes) {
            return core::Result<GltfExportReport>::failure(
                validation("glTF export exceeds the 256 MiB derived-buffer limit"));
        }
        meshes.push_back({
            mesh_id,
            "cartographer-mesh-" + std::to_string(mesh_id),
            position_accessor,
            normal_accessor,
            uv_accessor,
            tangent_accessor,
            index_accessor,
            render_positions.size(),
            render_indices.size() / 3U,
            index_bytes,
            material_regions.value(),
        });
        report.vertices += render_positions.size();
        report.triangles += render_indices.size() / 3U;
        report.index_bytes += render_indices.size() * index_bytes;
    }

    if (any_missing_uv) {
        if (any_active_uv) {
            report.warnings.push_back(
                "Some Cartographer meshes have no active render UV set; those meshes use the default material path");
        } else {
            report.warnings.push_back(
                "Cartographer authoring has no UV, tangent, or material channels; consumers must use their default material path");
        }
    }
    if (any_active_uv && any_missing_tangent) {
        report.warnings.push_back(
            "Some UV-bearing Cartographer meshes do not produce a valid tangent frame; those meshes use the default tangent path");
    }
    if (any_material_regions) {
        report.warnings.push_back(
            "Cartographer glTF export preserves condition.material_region face identity in mesh extras; visual material realization remains consumer-owned");
    } else if (any_active_uv) {
        report.warnings.push_back(
            "Cartographer glTF export does not include material channels; consumers must use their default material path");
    }
    report.warnings.push_back("float64 authoring positions and normals are narrowed to glTF float32");

    std::map<scene::ObjectId, std::size_t> node_indices;
    std::vector<NodeRecord> nodes;
    const auto objects = document.scene().objects_sorted();
    nodes.reserve(objects.size());
    for (const auto& object : objects) {
        const auto mesh = object.mesh_asset.has_value()
            ? std::find_if(document.meshes().begin(), document.meshes().end(),
                [&object](const auto& entry) { return entry.first == *object.mesh_asset; })
            : document.meshes().end();
        if (object.mesh_asset.has_value() && mesh == document.meshes().end()) {
            return core::Result<GltfExportReport>::failure(
                validation("scene object references a mesh missing from the export"));
        }
        std::optional<std::size_t> mesh_index;
        if (mesh != document.meshes().end()) {
            const auto ordinal = static_cast<std::size_t>(
                std::distance(document.meshes().begin(), mesh));
            mesh_index = ordinal;
        }
        node_indices.emplace(object.id, nodes.size());
        nodes.push_back({
            object.id,
            object.name,
            std::nullopt,
            mesh_index,
            object.local_transform,
            object.visible,
            object.locked,
        });
    }
    for (std::size_t index = 0U; index < objects.size(); ++index) {
        if (objects[index].parent.has_value()) {
            const auto node_parent = node_indices.find(*objects[index].parent);
            if (node_parent == node_indices.end()) {
                return core::Result<GltfExportReport>::failure(
                    validation("scene object parent is missing from the export"));
            }
            nodes[index].parent = node_parent->second;
        }
    }

    std::ostringstream json;
    json << "{\n"
         << "  \"asset\":{\"version\":\"2.0\",\"generator\":\"Cartographer "
         << CARTOGRAPHER_VERSION << "\"},\n"
         << "  \"scene\":0,\n"
         << "  \"scenes\":[{\"nodes\":[";
    bool first = true;
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        if (nodes[index].parent.has_value()) continue;
        if (!first) json << ',';
        first = false;
        json << index;
    }
    json << "]}],\n  \"nodes\":[\n";
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        json << "    {\"name\":\"" << json_escape(node.name) << "\"";
        if (node.mesh.has_value()) json << ",\"mesh\":" << *node.mesh;
        json << ',';
        if (auto transform = append_transform_json(json, node.transform); !transform) {
            return core::Result<GltfExportReport>::failure(transform.error());
        }
        json << ",\"extras\":{\"cartographer_object_id\":" << node.id.value
             << ",\"visible\":" << (node.visible ? "true" : "false")
             << ",\"locked\":" << (node.locked ? "true" : "false") << "}";
        std::vector<std::size_t> children;
        for (std::size_t child = 0U; child < nodes.size(); ++child) {
            if (nodes[child].parent == index) children.push_back(child);
        }
        if (!children.empty()) {
            json << ",\"children\":[";
            for (std::size_t child = 0U; child < children.size(); ++child) {
                if (child != 0U) json << ',';
                json << children[child];
            }
            json << ']';
        }
        json << "}" << (index + 1U == nodes.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"meshes\":[\n";
    for (std::size_t index = 0U; index < meshes.size(); ++index) {
        const auto& mesh = meshes[index];
        json << "    {\"name\":\"" << json_escape(mesh.name) << "\",\"primitives\":[{"
             << "\"attributes\":{\"POSITION\":" << mesh.position_accessor
             << ",\"NORMAL\":" << mesh.normal_accessor;
        if (mesh.uv_accessor.has_value()) {
            json << ",\"TEXCOORD_0\":" << *mesh.uv_accessor;
        }
        if (mesh.tangent_accessor.has_value()) {
            json << ",\"TANGENT\":" << *mesh.tangent_accessor;
        }
        json << "},\"indices\":" << mesh.index_accessor << ",\"mode\":"
             << kGlTriangles << ",\"extras\":{\"cartographer_mesh_id\":"
             << mesh.source_id;
        if (mesh.material_regions.has_value()) {
            json << ",\"condition_material_region\":{\"layer\":\"condition.material_region\","
                    "\"domain\":\"face\",\"faces\":[";
            for (std::size_t region_index = 0U;
                 region_index < mesh.material_regions->size(); ++region_index) {
                if (region_index != 0U) json << ',';
                const auto [face_id, region_id] = (*mesh.material_regions)[region_index];
                json << "{\"id\":" << face_id << ",\"region\":" << region_id << '}';
            }
            json << "]}";
        }
        json << "}}]}" << (index + 1U == meshes.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"accessors\":[\n";
    for (std::size_t index = 0U; index < accessors.size(); ++index) {
        const auto& accessor = accessors[index];
        json << "    {\"bufferView\":" << accessor.buffer_view
             << ",\"componentType\":" << accessor.component_type
             << ",\"count\":" << accessor.count << ",\"type\":\"" << accessor.type << '"';
        if (accessor.has_bounds) {
            json << ",\"min\":";
            append_float_array(json, accessor.minimum);
            json << ",\"max\":";
            append_float_array(json, accessor.maximum);
        }
        json << '}' << (index + 1U == accessors.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"bufferViews\":[\n";
    for (std::size_t index = 0U; index < views.size(); ++index) {
        const auto& view = views[index];
        json << "    {\"buffer\":0,\"byteOffset\":" << view.offset
             << ",\"byteLength\":" << view.length << ",\"target\":" << view.target << '}'
             << (index + 1U == views.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"buffers\":[{\"byteLength\":" << buffer.size()
         << ",\"uri\":\"data:application/octet-stream;base64,"
         << base64(buffer) << "\"}],\n"
         << "  \"extras\":{\"cartographer\":{\"profile\":\"cartographer-gltf\","
         << "\"profile_version\":" << GltfExportReport::kProfileVersion
         << ",\"project_schema\":" << document.schema_version()
         << ",\"project_revision\":" << document.revision().value()
         << ",\"project_name\":\"" << json_escape(document.name()) << "\"}}\n"
         << "}\n";
    const std::string text = json.str();
    if (text.size() > kMaxExportJsonBytes) {
        return core::Result<GltfExportReport>::failure(
            validation("glTF export exceeds the 512 MiB JSON limit"));
    }

    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<GltfExportReport>::failure(
            io_error("unable to open glTF temporary output"));
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<GltfExportReport>::failure(
            io_error("failed while writing glTF temporary output"));
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<GltfExportReport>::failure(
            io_error("failed while closing glTF temporary output"));
    }
    auto replacement = atomic_replace(temporary, path);
    if (!replacement) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<GltfExportReport>::failure(replacement.error());
    }
    return core::Result<GltfExportReport>::success(std::move(report));
}

} // namespace carto::io
