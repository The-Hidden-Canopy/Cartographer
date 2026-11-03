#include <carto/io/gltf_export.hpp>

#include <carto/production/render_mesh.hpp>
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
#include <locale>
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

struct PrimitiveRecord {
    std::size_t index_accessor = 0U;
    std::optional<std::size_t> material_index;
    std::optional<std::uint32_t> material_region;
    std::string region_layer;
    std::vector<geometry::FaceId> triangle_faces;
};

struct MeshRecord {
    std::uint64_t source_id = 0U;
    std::uint64_t source_mesh_revision = 0U;
    std::string name;
    std::size_t position_accessor = 0U;
    std::size_t normal_accessor = 0U;
    std::optional<std::size_t> uv_accessor;
    std::optional<std::size_t> tangent_accessor;
    std::vector<PrimitiveRecord> primitives;
    std::size_t vertices = 0U;
    std::size_t triangles = 0U;
    std::size_t index_bytes = 0U;
    std::optional<std::vector<std::pair<std::uint64_t, std::uint32_t>>> material_regions;
};

// The first appearance-preserving profile is deliberately limited to
// texture-free, opaque base-color/metallic/roughness factors. Refuse rather
// than silently flatten authored graph, variant, texture or lighting semantics.
core::Result<void> validate_constant_gltf_material(const render::MaterialAsset& asset) {
    const auto& material = asset.definition;
    if (material.base_color_factor.x > 1.0 || material.base_color_factor.y > 1.0 ||
        material.base_color_factor.z > 1.0 || material.alpha_mode != render::AlphaMode::opaque ||
        material.base_color || material.metallic_roughness || material.normal ||
        material.occlusion || material.emissive || material.normal_scale != 1.0 ||
        material.occlusion_strength != 1.0 || material.dielectric_f0 != 0.04 ||
        material.clearcoat != 0.0 || material.clearcoat_roughness != 0.1 ||
        material.emissive_factor.x != 0.0 || material.emissive_factor.y != 0.0 ||
        material.emissive_factor.z != 0.0) {
        return core::Result<void>::failure(validation(
            "glTF material catalog contains a material outside the texture-free opaque PBR profile"));
    }
    return core::Result<void>::success();
}

struct GltfMaterialRegions {
    std::string layer;
    std::map<std::uint32_t, std::size_t> material_by_region;
};

core::Result<GltfMaterialRegions> gltf_material_regions(
    const geometry::EditableMesh& mesh, std::uint64_t mesh_id,
    const project::MaterialCatalog& catalog,
    const std::map<render::MaterialId, std::size_t>& material_indices) {
    const auto* generic = mesh.attributes().find("cartographer.material_slot");
    const auto* condition = mesh.attributes().find("condition.material_region");
    if ((generic == nullptr) == (condition == nullptr))
        return core::Result<GltfMaterialRegions>::failure(validation(
            "glTF material catalog requires exactly one authored face-region layer per mesh"));
    const auto* layer = generic != nullptr ? generic : condition;
    if (layer->descriptor.domain != attributes::AttributeDomain::face ||
        layer->descriptor.type != attributes::AttributeType::uint32 ||
        layer->values.size() != mesh.faces_sorted().size() || layer->values.empty())
        return core::Result<GltfMaterialRegions>::failure(validation(
            "glTF material catalog requires a complete face-domain uint32 region layer"));
    GltfMaterialRegions result{generic != nullptr ? "cartographer.material_slot" :
                                                    "condition.material_region", {}};
    for (const auto& value : layer->values) {
        const auto* region = std::get_if<std::uint32_t>(&value);
        if (region == nullptr || *region == 0U)
            return core::Result<GltfMaterialRegions>::failure(validation(
                "glTF material catalog requires a nonzero assigned region on every face"));
        if (result.material_by_region.contains(*region)) continue;
        const auto assignment = catalog.assignments().find({mesh_id, *region});
        if (assignment == catalog.assignments().end() || assignment->second.variant.has_value())
            return core::Result<GltfMaterialRegions>::failure(validation(
                "glTF material catalog requires an explicit non-variant region assignment"));
        const auto material = material_indices.find(assignment->second.material);
        if (material == material_indices.end())
            return core::Result<GltfMaterialRegions>::failure(validation(
                "glTF material region refers to an unavailable material"));
        result.material_by_region.emplace(*region, material->second);
    }
    return core::Result<GltfMaterialRegions>::success(std::move(result));
}

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

core::Result<void> append_transform_json(
    std::ostringstream& json,
    const core::Transform& transform) {
    const auto translation = finite_vec3(transform.translation, "transform translation");
    const auto scale = finite_vec3(transform.scale, "transform scale");
    const auto rotation = transform.rotation.normalized();
    const auto rotation_x = finite_float(rotation.x, "transform rotation");
    const auto rotation_y = finite_float(rotation.y, "transform rotation");
    const auto rotation_z = finite_float(rotation.z, "transform rotation");
    const auto rotation_w = finite_float(rotation.w, "transform rotation");
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

void write_u32_le(std::ofstream& output, std::uint32_t value) {
    const std::array<std::uint8_t, 4U> bytes{
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
    };
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

} // namespace

static core::Result<GltfExportReport> export_interchange(
    const project::ProjectDocument& document,
    const std::filesystem::path& path,
    bool binary_container) {
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
    const auto& catalog = document.material_catalog();
    const bool export_materials = !catalog.empty();
    std::map<render::MaterialId, std::size_t> material_indices;
    std::map<std::uint64_t, GltfMaterialRegions> mesh_material_indices;
    if (export_materials) {
        if (catalog.materials().empty() || catalog.materials().size() > 32U ||
            catalog.assignments().empty() || !catalog.textures().empty() ||
            !catalog.samplers().empty() || !catalog.variants().empty() ||
            !catalog.material_graphs().empty() || !catalog.material_graph_bindings().empty())
            return core::Result<GltfExportReport>::failure(validation(
                "glTF export cannot preserve this material catalog: the supported profile requires 1..32 assigned, texture-free opaque materials without graphs or variants"));
        for (const auto& [id, material] : catalog.materials()) {
            if (auto result = validate_constant_gltf_material(material); !result)
                return core::Result<GltfExportReport>::failure(result.error().with_context(
                    "material " + std::to_string(id)));
            material_indices.emplace(id, material_indices.size());
        }
    }
    if (!document.accepted_autorigs().empty() || !document.unadmitted_mode_key_drafts().empty() ||
        !document.kinematic_graphs().empty() || !document.pose_libraries().empty() ||
        !document.rig_module_templates().empty() || !document.garment_assets().empty() ||
        !document.sculpt_stacks().empty()) {
        return core::Result<GltfExportReport>::failure(validation(
            "glTF export cannot preserve the authored rig or deformation records"));
    }
    for (const auto& [mesh_id, mesh] : document.meshes()) {
        for (const auto& [id, layer] : mesh.attributes().layers()) {
            if (id.starts_with("cartographer.tangent.") || id.starts_with("cartographer.tangent_uv.")) {
                return core::Result<GltfExportReport>::failure(validation(
                    "glTF export cannot preserve authored tangent frames on mesh " + std::to_string(mesh_id)));
            }
            if (layer.descriptor.type == attributes::AttributeType::color4) {
                return core::Result<GltfExportReport>::failure(validation(
                    "glTF export cannot preserve authored color layers on mesh " + std::to_string(mesh_id)));
            }
        }
        if (!export_materials && mesh.attributes().find("cartographer.material_slot") != nullptr) {
            return core::Result<GltfExportReport>::failure(validation(
                "glTF export cannot preserve authored material slots on mesh " + std::to_string(mesh_id)));
        }
        const auto& uv_sets = mesh.uv_sets();
        if (uv_sets.descriptors().size() > 1U ||
            (!uv_sets.descriptors().empty() && uv_sets.active_for_render() == nullptr)) {
            return core::Result<GltfExportReport>::failure(validation(
                "glTF export cannot preserve all authored UV sets on mesh " + std::to_string(mesh_id)));
        }
        if (export_materials) {
            auto material = gltf_material_regions(mesh, mesh_id, catalog, material_indices);
            if (!material) return core::Result<GltfExportReport>::failure(
                material.error().with_context("mesh asset " + std::to_string(mesh_id)));
            mesh_material_indices.emplace(mesh_id, material.value());
        }
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

    // One enclosing-project identity for the shared viewport/export compiler.
    const auto canonical = document.serialize();
    if (canonical.size() > 128U * 1024U * 1024U)
        return core::Result<GltfExportReport>::failure(
            validation("glTF export source identity exceeds 128 MiB"));
    const production::AuthoritativeSource source_identity{
        "gltf.export", document.revision(),
        assets::sha256(
            {reinterpret_cast<const std::uint8_t *>(canonical.data()), canonical.size()})};
    for (const auto& [mesh_id, mesh] : document.meshes()) {
        const auto *active_uv = mesh.uv_sets().active_for_render();
        const bool has_uv = active_uv != nullptr;
        production::RenderMeshCompileOptions options;
        if (active_uv)
            options.uv_set = active_uv->id;
        options.generate_tangents = has_uv;
        if (export_materials) options.material_slot_layer = mesh_material_indices.at(mesh_id).layer;
        auto artifact = production::compile_render_mesh_artifact(source_identity, mesh, options);
        if (!artifact && has_uv) {
            // Preserve the existing explicit missing-tangent fallback only if
            // geometry, UVs and custom normals themselves validate successfully.
            options.generate_tangents = false;
            artifact = production::compile_render_mesh_artifact(source_identity, mesh, options);
            if (artifact)
                any_missing_tangent = true;
        }
        if (!artifact)
            return core::Result<GltfExportReport>::failure(
                artifact.error().with_context("mesh asset " + std::to_string(mesh_id)));
        const auto &source = artifact.value();
        if (export_materials && source.submeshes.size() > 128U)
            return core::Result<GltfExportReport>::failure(validation(
                "glTF material export exceeds the 128-region scene-import limit"));
        const auto material_regions = condition_material_regions(mesh);
        if (!material_regions) {
            return core::Result<GltfExportReport>::failure(
                material_regions.error().with_context("mesh asset " + std::to_string(mesh_id)));
        }
        any_active_uv = any_active_uv || has_uv;
        any_missing_uv = any_missing_uv || !has_uv;
        any_material_regions = any_material_regions || material_regions.value().has_value();

        // Consume the same corner-normal/UV seam splitting and tangent domains
        // used by the authoring viewport, rather than averaged compiled normals.
        std::vector<core::Vec3d> render_positions, render_normals;
        const auto &render_indices = source.indices;
        std::vector<std::array<float, 2>> texcoords;
        std::vector<std::array<float, 4>> tangents;
        render_positions.reserve(source.vertices.size());
        render_normals.reserve(source.vertices.size());
        for (const auto &vertex : source.vertices) {
            render_positions.push_back(vertex.position);
            render_normals.push_back(vertex.normal);
            if (vertex.uv) {
                auto uv = finite_vec2(*vertex.uv, "mesh UV");
                if (!uv)
                    return core::Result<GltfExportReport>::failure(uv.error());
                texcoords.push_back(uv.value());
            }
            if (vertex.tangent) {
                std::array<float, 4> value{};
                const auto t = *vertex.tangent;
                const std::array<double, 4> input{t.x, t.y, t.z, t.w};
                for (unsigned c = 0; c < 4; ++c) {
                    auto v = finite_float(input[c], "mesh tangent");
                    if (!v)
                        return core::Result<GltfExportReport>::failure(v.error());
                    value[c] = v.value();
                }
                tangents.push_back(value);
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
        // The consumer evaluates triangle area in float32.  A valid authoring
        // triangle can collapse when large positions are narrowed; exporting it
        // would otherwise make the consumer silently drop source geometry.
        for (std::size_t triangle = 0; triangle < render_indices.size(); triangle += 3U) {
            const auto &a = positions[render_indices[triangle]];
            const auto &b = positions[render_indices[triangle + 1U]];
            const auto &c = positions[render_indices[triangle + 2U]];
            const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
            const float acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
            const float x = aby * acz - abz * acy;
            const float y = abz * acx - abx * acz;
            const float z = abx * acy - aby * acx;
            const float area_squared = x * x + y * y + z * z;
            if (!std::isfinite(area_squared) || static_cast<double>(area_squared) * 0.25 <= 1.0e-24) {
                return core::Result<GltfExportReport>::failure(validation(
                    "glTF float32 narrowing collapses a triangle or makes its area non-finite"));
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
        std::vector<PrimitiveRecord> primitives;
        primitives.reserve(source.submeshes.size());
        for (const auto& submesh : source.submeshes) {
            views.push_back({index_offset + static_cast<std::size_t>(submesh.first_index) * index_bytes,
                             static_cast<std::size_t>(submesh.index_count) * index_bytes,
                             kGlElementArrayBuffer});
            accessors.push_back({views.size() - 1U,
                                 use_uint16 ? kGlUnsignedShort : kGlUnsignedInt,
                                 submesh.index_count, "SCALAR", {}, {}, false});
            PrimitiveRecord primitive;
            primitive.index_accessor = accessors.size() - 1U;
            if (export_materials) {
                if (!submesh.material_slot ||
                    !mesh_material_indices.at(mesh_id).material_by_region.contains(*submesh.material_slot))
                    return core::Result<GltfExportReport>::failure(validation(
                        "glTF compiled submesh has no explicit material assignment"));
                primitive.material_region = *submesh.material_slot;
                primitive.region_layer = mesh_material_indices.at(mesh_id).layer;
                primitive.material_index = mesh_material_indices.at(mesh_id).material_by_region.at(*submesh.material_slot);
                const auto first = source.triangle_faces.begin() + submesh.first_index / 3U;
                primitive.triangle_faces.assign(first, first + submesh.index_count / 3U);
            }
            primitives.push_back(std::move(primitive));
        }

        if (buffer.size() > kMaxExportBufferBytes) {
            return core::Result<GltfExportReport>::failure(
                validation("glTF export exceeds the 256 MiB derived-buffer limit"));
        }
        meshes.push_back({
            mesh_id,
            mesh.revision().value(),
            "cartographer-mesh-" + std::to_string(mesh_id),
            position_accessor,
            normal_accessor,
            uv_accessor,
            tangent_accessor,
            std::move(primitives),
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
        if (export_materials) {
            report.warnings.push_back(
                "Some meshes have no active render UV set; texture-free material factors remain assigned");
        } else if (any_active_uv) {
            report.warnings.push_back(
                "Some Cartographer meshes have no active render UV set; those meshes use the default material path");
        } else {
            report.warnings.push_back(
                "Meshes without an active render UV set omit UVs and tangents; visual material "
                "realization remains consumer-owned");
        }
    }
    if (any_active_uv && any_missing_tangent) {
        report.warnings.push_back(
            "Some UV-bearing Cartographer meshes do not produce a valid tangent frame; those meshes use the default tangent path");
    }
    if (any_material_regions && !export_materials) {
        report.warnings.push_back(
            "Cartographer glTF export preserves condition.material_region face identity in mesh extras; visual material realization remains consumer-owned");
    } else if (any_active_uv && !export_materials) {
        report.warnings.push_back(
            "Cartographer glTF export does not include material channels; consumers must use their default material path");
    }
    if (export_materials) report.warnings.push_back(
        "Texture-free opaque PBR factors are exported; native and downstream lighting or back-face shading may differ");
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
    json.imbue(std::locale::classic());
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
    json << "]}],\n";
    if (export_materials) {
        json << "  \"materials\":[\n";
        std::size_t material_index = 0;
        for (const auto& [id, material] : catalog.materials()) {
            const auto& pbr = material.definition;
            std::ostringstream factors;
            factors.imbue(std::locale::classic());
            factors << std::setprecision(std::numeric_limits<double>::max_digits10)
                << "{\"name\":\"" << json_escape(material.name) << "\","
                << "\"doubleSided\":true,\"alphaMode\":\"OPAQUE\","
                << "\"pbrMetallicRoughness\":{\"baseColorFactor\":["
                << pbr.base_color_factor.x << ',' << pbr.base_color_factor.y << ','
                << pbr.base_color_factor.z << ',' << pbr.base_color_factor.w << "],"
                << "\"metallicFactor\":" << pbr.metallic << ",\"roughnessFactor\":" << pbr.roughness
                << "},\"extras\":{\"cartographer_material_id\":" << id << "}}";
            json << "    " << factors.str() << (++material_index == catalog.materials().size() ? "\n" : ",\n");
        }
        json << "  ],\n";
    }
    json << "  \"nodes\":[\n";
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
        json << "    {\"name\":\"" << json_escape(mesh.name) << "\",\"primitives\":[";
        for (std::size_t primitive_index = 0U; primitive_index < mesh.primitives.size(); ++primitive_index) {
            const auto& primitive = mesh.primitives[primitive_index];
            if (primitive_index != 0U) json << ',';
            json << "{\"attributes\":{\"POSITION\":" << mesh.position_accessor
                 << ",\"NORMAL\":" << mesh.normal_accessor;
            if (mesh.uv_accessor.has_value()) json << ",\"TEXCOORD_0\":" << *mesh.uv_accessor;
            if (mesh.tangent_accessor.has_value()) json << ",\"TANGENT\":" << *mesh.tangent_accessor;
            json << "},\"indices\":" << primitive.index_accessor << ",\"mode\":" << kGlTriangles;
            if (primitive.material_index.has_value()) json << ",\"material\":" << *primitive.material_index;
            json << ",\"extras\":{\"cartographer_mesh_id\":" << mesh.source_id;
            if (primitive.material_region.has_value()) {
                json << ",\"cartographer_material_region\":" << *primitive.material_region
                     << ",\"cartographer_material_region_layer\":\"" << json_escape(primitive.region_layer) << "\""
                     << ",\"cartographer_mesh_revision\":" << mesh.source_mesh_revision
                     << ",\"cartographer_triangle_faces\":[";
                for (std::size_t face_index = 0U; face_index < primitive.triangle_faces.size(); ++face_index) {
                    if (face_index != 0U) json << ',';
                    json << primitive.triangle_faces[face_index].value;
                }
                json << ']';
            }
            // Retain the legacy whole-mesh CONDITION face table once, on the first primitive.
            if (primitive_index == 0U && mesh.material_regions.has_value()) {
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
            json << "}}";
        }
        json << "]}" << (index + 1U == meshes.size() ? "\n" : ",\n");
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
        json << "  ],\n  \"buffers\":[{\"byteLength\":" << buffer.size();
    if (!binary_container) {
        json << ",\"uri\":\"data:application/octet-stream;base64,"
             << base64(buffer) << '\"';
    }
    json << "}],\n"
         << "  \"extras\":{\"cartographer\":{\"profile\":\"cartographer-gltf\","
         << "\"profile_version\":" << GltfExportReport::kProfileVersion
         << ",\"project_schema\":" << document.schema_version()
         << ",\"project_revision\":" << document.revision().value();
    if (export_materials)
        json << ",\"material_regions_version\":1,\"canonical_project_sha256\":\""
             << source_identity.content_digest.hex() << "\"";
    json << ",\"project_name\":\"" << json_escape(document.name()) << "\"}}\n"
         << "}\n";
    const std::string text = json.str();
    if (text.size() > kMaxExportJsonBytes) {
        return core::Result<GltfExportReport>::failure(
            validation("interchange export exceeds the 512 MiB JSON limit"));
    }
    std::size_t padded_json_size = text.size();
    std::size_t padded_buffer_size = buffer.size();
    std::size_t glb_size = 0U;
    if (binary_container) {
        padded_json_size += (4U - padded_json_size % 4U) % 4U;
        padded_buffer_size += (4U - padded_buffer_size % 4U) % 4U;
        glb_size = 12U + 8U + padded_json_size + 8U + padded_buffer_size;
        if (text.size() > std::numeric_limits<std::uint32_t>::max() ||
            buffer.size() > std::numeric_limits<std::uint32_t>::max() ||
            padded_json_size > std::numeric_limits<std::uint32_t>::max() ||
            padded_buffer_size > std::numeric_limits<std::uint32_t>::max() ||
            glb_size > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<GltfExportReport>::failure(validation(
                "GLB export exceeds the version 2 container size limit"));
        }
    }

    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<GltfExportReport>::failure(
            io_error("unable to open glTF temporary output"));
    }
    if (binary_container) {
        constexpr std::uint32_t kGlbMagic = 0x46546c67U;
        constexpr std::uint32_t kGlbVersion = 2U;
        constexpr std::uint32_t kJsonChunkType = 0x4e4f534aU;
        constexpr std::uint32_t kBinChunkType = 0x004e4942U;
        const auto json_chunk_length = static_cast<std::uint32_t>(padded_json_size);
        const auto bin_chunk_length = static_cast<std::uint32_t>(padded_buffer_size);
        write_u32_le(output, kGlbMagic);
        write_u32_le(output, kGlbVersion);
        write_u32_le(output, static_cast<std::uint32_t>(glb_size));
        write_u32_le(output, json_chunk_length);
        write_u32_le(output, kJsonChunkType);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        constexpr std::array<char, 3U> json_padding{' ', ' ', ' '};
        const auto json_padding_size = padded_json_size - text.size();
        output.write(json_padding.data(), static_cast<std::streamsize>(json_padding_size));
        write_u32_le(output, bin_chunk_length);
        write_u32_le(output, kBinChunkType);
        if (!buffer.empty()) {
            output.write(reinterpret_cast<const char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        }
        constexpr std::array<char, 3U> binary_padding{};
        const auto binary_padding_size = padded_buffer_size - buffer.size();
        output.write(binary_padding.data(), static_cast<std::streamsize>(binary_padding_size));
    } else {
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
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

core::Result<GltfExportReport> export_gltf(
    const project::ProjectDocument& document,
    const std::filesystem::path& path) {
    return export_interchange(document, path, false);
}

core::Result<GltfExportReport> export_glb(
    const project::ProjectDocument& document,
    const std::filesystem::path& path) {
    return export_interchange(document, path, true);
}

} // namespace carto::io
