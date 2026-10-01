#include <carto/io/gltf_export.hpp>

#include <carto/project/project.hpp>

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
    std::string name;
    std::size_t position_accessor = 0U;
    std::size_t normal_accessor = 0U;
    std::size_t index_accessor = 0U;
    std::size_t vertices = 0U;
    std::size_t triangles = 0U;
    std::size_t index_bytes = 0U;
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
    report.warnings = {
        "Cartographer authoring currently has no UV, tangent, or material channels; "
        "consumers must use their default material path",
        "float64 authoring positions and normals are narrowed to glTF float32",
    };

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

        std::vector<std::array<float, 3>> positions;
        std::vector<std::array<float, 3>> normals;
        positions.reserve(source.positions.size());
        normals.reserve(source.normals.size());
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
        for (const auto position : source.positions) {
            const auto narrowed = finite_vec3(position, "mesh position");
            if (!narrowed) return core::Result<GltfExportReport>::failure(narrowed.error());
            positions.push_back(narrowed.value());
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
                minimum[axis] = std::min(minimum[axis], narrowed.value()[axis]);
                maximum[axis] = std::max(maximum[axis], narrowed.value()[axis]);
            }
        }
        for (const auto normal : source.normals) {
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

        const bool use_uint16 = source.positions.size() <= 65'535U;
        std::vector<std::uint16_t> indices_16;
        std::vector<std::uint32_t> indices_32;
        if (use_uint16) {
            indices_16.reserve(source.indices.size());
            for (const std::uint32_t index : source.indices) {
                if (index > std::numeric_limits<std::uint16_t>::max()) {
                    return core::Result<GltfExportReport>::failure(
                        validation("glTF 16-bit index selection was unsafe"));
                }
                indices_16.push_back(static_cast<std::uint16_t>(index));
            }
        } else {
            indices_32 = source.indices;
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

        align_four(buffer);
        const std::size_t index_offset = buffer.size();
        if (use_uint16) append_vector(buffer, indices_16);
        else append_vector(buffer, indices_32);
        const std::size_t index_bytes = use_uint16 ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
        views.push_back({index_offset, source.indices.size() * index_bytes, kGlElementArrayBuffer});
        const std::size_t index_view = views.size() - 1U;
        accessors.push_back({
            index_view,
            use_uint16 ? kGlUnsignedShort : kGlUnsignedInt,
            source.indices.size(),
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
            "cartographer-mesh-" + std::to_string(mesh_id),
            position_accessor,
            normal_accessor,
            index_accessor,
            source.positions.size(),
            source.indices.size() / 3U,
            index_bytes,
        });
        report.vertices += source.positions.size();
        report.triangles += source.indices.size() / 3U;
        report.index_bytes += source.indices.size() * index_bytes;
    }

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
             << ",\"NORMAL\":" << mesh.normal_accessor << "},\"indices\":"
             << mesh.index_accessor << ",\"mode\":" << kGlTriangles << "}]}"
             << (index + 1U == meshes.size() ? "\n" : ",\n");
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
