#include <carto/production/material_binding.hpp>
#include <carto/production/authoring_aging.hpp>
#include "gltf_material.hpp"
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
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
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

std::filesystem::path temporary_path(const std::filesystem::path &target) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_gltf_export_temp_counter.fetch_add(1U, std::memory_order_relaxed);
    return target.parent_path() / (target.filename().string() + ".carto.tmp-" +
                                   std::to_string(static_cast<unsigned long long>(ticks)) + "-" +
                                   std::to_string(static_cast<unsigned long long>(thread)) + "-" +
                                   std::to_string(static_cast<unsigned long long>(counter)));
}

core::Result<void> atomic_replace(const std::filesystem::path &temporary,
                                  const std::filesystem::path &target) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
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
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
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

void append_bytes(std::vector<std::uint8_t> &buffer, const void *data, std::size_t bytes) {
    const auto *source = static_cast<const std::uint8_t *>(data);
    buffer.insert(buffer.end(), source, source + bytes);
}

void align_four(std::vector<std::uint8_t> &buffer) {
    while ((buffer.size() & 3U) != 0U)
        buffer.push_back(0U);
}

template <typename T>
void append_vector(std::vector<std::uint8_t> &buffer, const std::vector<T> &values) {
    if (!values.empty())
        append_bytes(buffer, values.data(), values.size() * sizeof(T));
}

std::string base64(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
    std::size_t position_accessor = 0, normal_accessor = 0, index_accessor = 0, material = 0;
    std::optional<std::size_t> uv_accessor, tangent_accessor;
    std::optional<std::uint32_t> slot;
    std::vector<geometry::FaceId> triangle_faces;
};
struct MeshRecord {
    std::uint64_t source_id = 0;
    std::uint64_t source_mesh_revision = 0;
    std::string region_layer;
    std::string name;
    std::vector<PrimitiveRecord> primitives;
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
        return core::Result<float>::failure(
            validation("glTF export cannot represent " + std::string(field) + " as float32"));
    }
    return core::Result<float>::success(narrowed);
}

core::Result<std::array<float, 3>> finite_vec3(core::Vec3d value, std::string_view field) {
    const auto x = finite_float(value.x, field);
    const auto y = finite_float(value.y, field);
    const auto z = finite_float(value.z, field);
    if (!x || !y || !z) {
        return core::Result<std::array<float, 3>>::failure((!x   ? x.error()
                                                            : !y ? y.error()
                                                                 : z.error()));
    }
    return core::Result<std::array<float, 3>>::success({x.value(), y.value(), z.value()});
}

core::Result<std::array<float, 2>> finite_vec2(core::Vec2d value, std::string_view field) {
    const auto x = finite_float(value.x, field);
    const auto y = finite_float(value.y, field);
    if (!x || !y) {
        return core::Result<std::array<float, 2>>::failure(!x ? x.error() : y.error());
    }
    return core::Result<std::array<float, 2>>::success({x.value(), y.value()});
}

using MaterialRegionPayload = std::vector<std::pair<std::uint64_t, std::uint32_t>>;

core::Result<std::optional<MaterialRegionPayload>>
condition_material_regions(const geometry::EditableMesh &mesh) {
    constexpr std::string_view kLayerId = "condition.material_region";
    const auto *layer = mesh.attributes().find(kLayerId);
    if (layer == nullptr) {
        return core::Result<std::optional<MaterialRegionPayload>>::success(std::nullopt);
    }
    if (layer->descriptor.domain != attributes::AttributeDomain::face ||
        layer->descriptor.type != attributes::AttributeType::uint32) {
        return core::Result<std::optional<MaterialRegionPayload>>::failure(
            validation("condition.material_region must be a face-domain uint32 layer"));
    }
    const auto faces = mesh.faces_sorted();
    const auto face_count = mesh.attributes().domain_count(attributes::AttributeDomain::face);
    if (!face_count.has_value() || *face_count != faces.size() ||
        layer->values.size() != faces.size()) {
        return core::Result<std::optional<MaterialRegionPayload>>::failure(
            validation("condition.material_region cardinality does not match mesh faces"));
    }

    MaterialRegionPayload payload;
    payload.reserve(faces.size());
    for (std::size_t index = 0U; index < faces.size(); ++index) {
        const auto *value = std::get_if<std::uint32_t>(&layer->values[index]);
        if (value == nullptr || *value == 0U) {
            return core::Result<std::optional<MaterialRegionPayload>>::failure(
                validation("condition.material_region values must be non-zero uint32 "
                           "region IDs"));
        }
        payload.emplace_back(faces[index].id.value, *value);
    }
    return core::Result<std::optional<MaterialRegionPayload>>::success(std::move(payload));
}

core::Result<void> append_transform_json(std::ostringstream &json,
                                         const core::Transform &transform) {
    const auto translation = finite_vec3(transform.translation, "transform translation");
    const auto scale = finite_vec3(transform.scale, "transform scale");
    const auto rotation = transform.rotation.normalized();
    const auto rotation_x = finite_float(rotation.x, "transform rotation");
    const auto rotation_y = finite_float(rotation.y, "transform rotation");
    const auto rotation_z = finite_float(rotation.z, "transform rotation");
    const auto rotation_w = finite_float(rotation.w, "transform rotation");
    if (!translation || !scale || !rotation_x || !rotation_y || !rotation_z || !rotation_w) {
        if (!translation)
            return core::Result<void>::failure(translation.error());
        if (!scale)
            return core::Result<void>::failure(scale.error());
        if (!rotation_x)
            return core::Result<void>::failure(rotation_x.error());
        if (!rotation_y)
            return core::Result<void>::failure(rotation_y.error());
        if (!rotation_z)
            return core::Result<void>::failure(rotation_z.error());
        return core::Result<void>::failure(rotation_w.error());
    }
    json << std::setprecision(9) << "\"translation\":[" << translation.value()[0] << ','
         << translation.value()[1] << ',' << translation.value()[2] << "],\"rotation\":["
         << rotation_x.value() << ',' << rotation_y.value() << ',' << rotation_z.value() << ','
         << rotation_w.value() << "],\"scale\":[" << scale.value()[0] << ',' << scale.value()[1]
         << ',' << scale.value()[2] << ']';
    return core::Result<void>::success();
}

void append_float_array(std::ostringstream &json, const std::array<float, 3> &values) {
    json << std::setprecision(9) << '[' << values[0] << ',' << values[1] << ',' << values[2] << ']';
}

void write_u32_le(std::ofstream &output, std::uint32_t value) {
    const std::array<std::uint8_t, 4U> bytes{
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
    };
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

} // namespace

static core::Result<GltfExportReport> export_interchange(const project::ProjectDocument &document,
                                                         const std::filesystem::path &path,
                                                         bool binary_container,
                                                         const GltfExportOptions &export_options) {
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
    if (!document.accepted_autorigs().empty() || !document.unadmitted_mode_key_drafts().empty() ||
        !document.kinematic_graphs().empty() || !document.pose_libraries().empty() ||
        !document.rig_module_templates().empty() || !document.garment_assets().empty() ||
        !document.sculpt_stacks().empty()) {
        return core::Result<GltfExportReport>::failure(validation(
            "glTF export cannot preserve the authored rig or deformation records"));
    }
    std::map<std::uint64_t, std::set<attributes::UvSetId>> authored_tangent_sets;
    for (const auto& [mesh_id, mesh] : document.meshes()) {
        for (const auto& [id, layer] : mesh.attributes().layers()) {
            if (id.starts_with("cartographer.tangent.") || id.starts_with("cartographer.tangent_uv.")) {
                const auto descriptor = std::find_if(mesh.uv_sets().descriptors().begin(),
                    mesh.uv_sets().descriptors().end(), [&](const auto& entry) {
                        const auto suffix = std::to_string(entry.first);
                        return id == "cartographer.tangent." + suffix ||
                               id == "cartographer.tangent_uv." + suffix;
                    });
                if (descriptor == mesh.uv_sets().descriptors().end())
                    return core::Result<GltfExportReport>::failure(validation(
                        "authored tangent has no matching UV set on mesh " + std::to_string(mesh_id)));
                const auto suffix = std::to_string(descriptor->first);
                const auto* tangent = mesh.attributes().find("cartographer.tangent." + suffix);
                const auto* original = mesh.attributes().find("cartographer.tangent_uv." + suffix);
                const auto* current = mesh.attributes().find(descriptor->second.layer_id);
                if (!tangent || !original || !current ||
                    tangent->descriptor.domain != attributes::AttributeDomain::corner ||
                    tangent->descriptor.type != attributes::AttributeType::vec4 ||
                    original->descriptor.domain != attributes::AttributeDomain::corner ||
                    original->descriptor.type != attributes::AttributeType::vec2 ||
                    current->descriptor.domain != attributes::AttributeDomain::corner ||
                    current->descriptor.type != attributes::AttributeType::vec2 ||
                    tangent->values.size() != current->values.size() ||
                    original->values.size() != current->values.size())
                    return core::Result<GltfExportReport>::failure(validation(
                        "authored tangent correspondence is incomplete on mesh " + std::to_string(mesh_id)));
                for (std::size_t i = 0; i < current->values.size(); ++i) {
                    const auto* before = std::get_if<core::Vec2d>(&original->values[i]);
                    const auto* now = std::get_if<core::Vec2d>(&current->values[i]);
                    if (!before || !now || before->x != now->x || before->y != now->y)
                        return core::Result<GltfExportReport>::failure(validation(
                            "authored tangent UV correspondence changed on mesh " + std::to_string(mesh_id)));
                }
                authored_tangent_sets[mesh_id].insert(descriptor->first);
            }
            if (layer.descriptor.type == attributes::AttributeType::color4)
                return core::Result<GltfExportReport>::failure(validation(
                    "glTF export cannot preserve authored color layers on mesh " + std::to_string(mesh_id)));
        }
        if (!export_materials && mesh.attributes().find("cartographer.material_slot") != nullptr)
            return core::Result<GltfExportReport>::failure(validation(
                "glTF export cannot preserve authored material slots without a catalog on mesh " + std::to_string(mesh_id)));
        const auto& uv_sets = mesh.uv_sets();
        if ((!export_materials && uv_sets.descriptors().size() > 1U) ||
            (!uv_sets.descriptors().empty() && uv_sets.active_for_render() == nullptr))
            return core::Result<GltfExportReport>::failure(validation(
                "glTF export cannot preserve all authored UV sets on mesh " + std::to_string(mesh_id)));
        for (const auto& [id, layer] : mesh.attributes().layers()) {
            static_cast<void>(layer);
            const bool declared_uv = std::any_of(uv_sets.descriptors().begin(),
                uv_sets.descriptors().end(), [&](const auto& entry) {
                    return id == entry.second.layer_id;
                });
            // Tangent pairs were validated above and every authored frame must
            // still be consumed below. Material primitives may select any
            // declared UV set; the existing report discloses unused streams.
            const bool authored_tangent = id.starts_with("cartographer.tangent.") ||
                id.starts_with("cartographer.tangent_uv.");
            const bool supported = id == "cartographer.normal0" ||
                id == "condition.material_region" ||
                (id == "cartographer.material_slot" && export_materials) ||
                declared_uv || authored_tangent;
            if (!supported) {
                return core::Result<GltfExportReport>::failure(validation(
                    "glTF export cannot preserve attribute layer '" + id +
                    "' on mesh " + std::to_string(mesh_id)));
            }
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
    for(const auto& [object,layer]:document.aging_layers()){static_cast<void>(object);if(production::aging_needs_rebake(document,layer))return core::Result<GltfExportReport>::failure(validation("aging appearance is stale; re-bake or unlink the operation before exporting"));}
    if(!document.aging_layers().empty())report.warnings.push_back("Aging is exported as its supported baked material appearance; editable cause recipes and seed/operation identity remain in the .carto source, not glTF/GLB.");
    bool any_active_uv = false;
    bool any_missing_uv = false;
    bool any_missing_tangent = false;
    bool any_material_regions = false;
    detail::GltfMaterialWriter material_writer(document, export_options, report);
    if (export_materials) {
        auto prepared = material_writer.prepare_catalog();
        if (!prepared) return core::Result<GltfExportReport>::failure(prepared.error());
    }

    // One enclosing-project identity for the shared viewport/export compiler.
    const auto canonical = document.serialize();
    if (canonical.size() > 128U * 1024U * 1024U)
        return core::Result<GltfExportReport>::failure(
            validation("glTF export source identity exceeds 128 MiB"));
    const production::AuthoritativeSource source_identity{
        "gltf.export", document.revision(),
        assets::sha256(
            {reinterpret_cast<const std::uint8_t *>(canonical.data()), canonical.size()})};
    for (const auto &[mesh_id, mesh] : document.meshes()) {
        auto remaining_authored_tangents = authored_tangent_sets[mesh_id];
        production::RenderMeshCompileOptions base_options;
        auto slots=production::authoring_material_slot_layer(mesh);
        if(!slots)return core::Result<GltfExportReport>::failure(slots.error());
        base_options.material_slot_layer=slots.value();
        auto base = production::compile_render_mesh_artifact(source_identity, mesh, base_options);
        if (!base)
            return core::Result<GltfExportReport>::failure(base.error());
        if (export_materials && (base.value().submeshes.size() > 128U || !slots.value()))
            return core::Result<GltfExportReport>::failure(validation(
                "glTF material catalog exceeds the 128-region limit or lacks an assigned face-region layer"));
        const auto material_regions = condition_material_regions(mesh);
        if (!material_regions)
            return core::Result<GltfExportReport>::failure(material_regions.error());
        any_material_regions = any_material_regions || material_regions.value().has_value();
        if (mesh.uv_sets().descriptors().size() > 1U)
            report.warnings.push_back("Each material primitive exports its resolved UV set; other authored UV streams remain in the source project");
        MeshRecord record{
            mesh_id, mesh.revision().value(), slots.value().value_or(""),
            "cartographer-mesh-" + std::to_string(mesh_id), {}, material_regions.value()};
        struct Stream {
            production::RenderMeshArtifact artifact;
            PrimitiveRecord primitive;
        };
        std::map<std::optional<render::UvSetId>, Stream> streams;
        for (const auto &group : base.value().submeshes) {
            if (export_materials && (!group.material_slot || *group.material_slot == 0U ||
                !catalog.assignments().contains({mesh_id, *group.material_slot})))
                return core::Result<GltfExportReport>::failure(validation(
                    "glTF material export requires an explicit assignment for every face region"));
            auto material = material_writer.material(mesh_id, group.material_slot);
            if (!material)
                return core::Result<GltfExportReport>::failure(
                    material.error().with_context("mesh asset " + std::to_string(mesh_id)));
            auto uv = material.value().uv;
            if (!uv)
                if (const auto *active = mesh.uv_sets().active_for_render())
                    uv = active->id;
            auto found = streams.find(uv);
            if (found == streams.end()) {
                auto options = base_options;
                options.uv_set = uv;
                options.generate_tangents = uv.has_value();
                auto artifact =
                    production::compile_render_mesh_artifact(source_identity, mesh, options);
                if (!artifact && uv && !material.value().normal_mapping) {
                    options.generate_tangents = false;
                    artifact =
                        production::compile_render_mesh_artifact(source_identity, mesh, options);
                    if (artifact)
                        any_missing_tangent = true;
                }
                if (!artifact)
                    return core::Result<GltfExportReport>::failure(
                        artifact.error().with_context("mesh asset " + std::to_string(mesh_id)));
                any_active_uv = any_active_uv || uv.has_value();
                any_missing_uv = any_missing_uv || !uv.has_value();
                const auto &source = artifact.value();
                // Consume the same corner-normal/UV seam splitting and tangent domains
                // used by the authoring viewport, rather than averaged compiled
                // normals.
                std::vector<core::Vec3d> render_positions, render_normals;
                std::vector<std::array<float, 2>> texcoords;
                std::vector<std::array<float, 4>> tangents;
                render_positions.reserve(source.vertices.size());
                render_normals.reserve(source.vertices.size());
                for (const auto &vertex : source.vertices) {
                    render_positions.push_back(vertex.position);
                    render_normals.push_back(vertex.normal);
                    if (vertex.uv) {
                        auto coord = finite_vec2(*vertex.uv, "mesh UV");
                        if (!coord)
                            return core::Result<GltfExportReport>::failure(coord.error());
                        texcoords.push_back(coord.value());
                    }
                    if (vertex.tangent) {
                        std::array<float, 4> value{};
                        const auto t = *vertex.tangent;
                        const core::Vec3d direction{t.x, t.y, t.z};
                        if (!direction.finite() || !std::isfinite(t.w) ||
                            std::abs(direction.length_squared() - 1.0) > 1e-4 ||
                            std::abs(core::dot(direction, vertex.normal)) > 1e-4 ||
                            std::abs(t.w) != 1.0)
                            return core::Result<GltfExportReport>::failure(validation(
                                "glTF export cannot preserve a tangent inconsistent with its normal"));
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
                    if (!narrowed)
                        return core::Result<GltfExportReport>::failure(narrowed.error());
                    positions.push_back(narrowed.value());
                    for (std::size_t axis = 0U; axis < 3U; ++axis) {
                        minimum[axis] = std::min(minimum[axis], narrowed.value()[axis]);
                        maximum[axis] = std::max(maximum[axis], narrowed.value()[axis]);
                    }
                }
                // Judge the actual float32 payload using double arithmetic, as
                // native PBR upload does. Float area arithmetic would overflow
                // for otherwise representable large triangles. A nonzero area
                // alone also cannot detect a winding reversal from rounding.
                for (std::size_t triangle = 0; triangle < source.indices.size(); triangle += 3U) {
                    const auto ia = source.indices[triangle];
                    const auto ib = source.indices[triangle + 1U];
                    const auto ic = source.indices[triangle + 2U];
                    const auto position = [&](std::uint32_t index) {
                        const auto& value = positions[index];
                        return core::Vec3d{value[0], value[1], value[2]};
                    };
                    const auto projected_area = core::cross(
                        position(ib) - position(ia), position(ic) - position(ia));
                    const double area_squared = projected_area.length_squared();
                    if (!projected_area.finite() || !std::isfinite(area_squared) || area_squared <= 4e-24)
                        return core::Result<GltfExportReport>::failure(validation(
                            "glTF float32 narrowing collapses a triangle or makes its area non-finite"));
                    const auto source_area = core::cross(
                        render_positions[ib] - render_positions[ia],
                        render_positions[ic] - render_positions[ia]);
                    if (!(core::dot(source_area, projected_area) > 0.0))
                        return core::Result<GltfExportReport>::failure(validation(
                            "glTF float32 narrowing reverses or loses a triangle orientation"));
                }
                for (const auto normal : render_normals) {
                    const auto narrowed = finite_vec3(normal, "mesh normal");
                    if (!narrowed)
                        return core::Result<GltfExportReport>::failure(narrowed.error());
                    const float magnitude = std::sqrt(narrowed.value()[0] * narrowed.value()[0] +
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

                align_four(buffer);
                const std::size_t position_offset = buffer.size();
                append_vector(buffer, positions);
                views.push_back({position_offset, positions.size() * sizeof(positions.front()),
                                 kGlArrayBuffer});
                const std::size_t position_view = views.size() - 1U;
                accessors.push_back({
                    position_view,
                    kGlFloat,
                    positions.size(),
                    "VEC3",
                    minimum,
                    maximum,
                    true,
                });
                const std::size_t position_accessor = accessors.size() - 1U;

                align_four(buffer);
                const std::size_t normal_offset = buffer.size();
                append_vector(buffer, normals);
                views.push_back(
                    {normal_offset, normals.size() * sizeof(normals.front()), kGlArrayBuffer});
                const std::size_t normal_view = views.size() - 1U;
                accessors.push_back({
                    normal_view,
                    kGlFloat,
                    normals.size(),
                    "VEC3",
                    {},
                    {},
                    false,
                });
                const std::size_t normal_accessor = accessors.size() - 1U;

                std::optional<std::size_t> uv_accessor;
                if (!texcoords.empty()) {
                    align_four(buffer);
                    const std::size_t uv_offset = buffer.size();
                    append_vector(buffer, texcoords);
                    views.push_back(
                        {uv_offset, texcoords.size() * sizeof(texcoords.front()), kGlArrayBuffer});
                    const std::size_t uv_view = views.size() - 1U;
                    accessors.push_back({
                        uv_view,
                        kGlFloat,
                        texcoords.size(),
                        "VEC2",
                        {},
                        {},
                        false,
                    });
                    uv_accessor = accessors.size() - 1U;
                }

                std::optional<std::size_t> tangent_accessor;
                if (!tangents.empty()) {
                    align_four(buffer);
                    const std::size_t tangent_offset = buffer.size();
                    append_vector(buffer, tangents);
                    views.push_back({tangent_offset, tangents.size() * sizeof(tangents.front()),
                                     kGlArrayBuffer});
                    const std::size_t tangent_view = views.size() - 1U;
                    accessors.push_back({
                        tangent_view,
                        kGlFloat,
                        tangents.size(),
                        "VEC4",
                        {},
                        {},
                        false,
                    });
                    tangent_accessor = accessors.size() - 1U;
                }

                PrimitiveRecord primitive;
                primitive.position_accessor = position_accessor;
                primitive.normal_accessor = normal_accessor;
                primitive.uv_accessor = uv_accessor;
                primitive.tangent_accessor = tangent_accessor;
                report.vertices += source.vertices.size();
                found = streams.emplace(uv, Stream{std::move(artifact.value()), primitive}).first;
            }
            auto &stream = found->second;
            if (uv && stream.primitive.tangent_accessor)
                remaining_authored_tangents.erase(*uv);
            if (material.value().normal_mapping && !stream.primitive.tangent_accessor)
                return core::Result<GltfExportReport>::failure(
                    validation("normal-mapped material requires a valid tangent frame"));
            const auto range = std::find_if(
                stream.artifact.submeshes.begin(), stream.artifact.submeshes.end(),
                [&](const auto &value) { return value.material_slot == group.material_slot; });
            if (range == stream.artifact.submeshes.end())
                return core::Result<GltfExportReport>::failure(
                    validation("material range changed during UV projection"));
            const auto indices =
                std::span(stream.artifact.indices).subspan(range->first_index, range->index_count);
            // Validate only emitted primitive triangles, not unrelated faces
            // in another material's UV stream. This also runs when tangent
            // generation falls back to UV-only export.
            if (stream.primitive.uv_accessor) {
                for (std::size_t triangle = 0U; triangle < indices.size(); triangle += 3U) {
                    std::array<core::Vec2d, 3U> source_uvs;
                    std::array<core::Vec2d, 3U> projected_uvs;
                    for (std::size_t corner = 0U; corner < 3U; ++corner) {
                        const auto coordinate = *stream.artifact.vertices[indices[triangle + corner]].uv;
                        source_uvs[corner] = coordinate;
                        // These are the same finite float32 values already
                        // encoded into this stream's TEXCOORD_0 accessor.
                        projected_uvs[corner] = {
                            static_cast<float>(coordinate.x), static_cast<float>(coordinate.y)};
                    }
                    const auto valid_projection = production::validate_uv_triangle_projection(
                        source_uvs, projected_uvs);
                    if (!valid_projection) {
                        return core::Result<GltfExportReport>::failure(validation(
                            "glTF float32 UV narrowing: " + valid_projection.error().message));
                    }
                }
            }
            if(material.value().baked)for(auto index:indices) {
                const auto& coord=stream.artifact.vertices.at(index).uv;
                if(!coord||coord->x<0||coord->x>1||coord->y<0||coord->y>1)
                    return core::Result<GltfExportReport>::failure(validation(
                        "explicit material graph bake requires all bound primitive UVs within the unit tile; UDIM/out-of-tile baking is unsupported"));
            }
            const bool short_index = stream.artifact.vertices.size() <= 65535;
            align_four(buffer);
            const auto offset = buffer.size();
            if (short_index) {
                std::vector<std::uint16_t> packed;
                packed.reserve(indices.size());
                for (auto index : indices) {
                    if (index > 65535)
                        return core::Result<GltfExportReport>::failure(
                            validation("unsafe 16-bit index"));
                    packed.push_back(static_cast<std::uint16_t>(index));
                }
                append_vector(buffer, packed);
            } else {
                for (auto index : indices)
                    append_bytes(buffer, &index, sizeof(index));
                report.used_uint32_indices = true;
            }
            const std::size_t width = short_index ? 2 : 4;
            views.push_back({offset, indices.size() * width, kGlElementArrayBuffer});
            accessors.push_back({views.size() - 1,
                                 short_index ? kGlUnsignedShort : kGlUnsignedInt,
                                 indices.size(),
                                 "SCALAR",
                                 {},
                                 {},
                                 false});
            auto primitive = stream.primitive;
            primitive.index_accessor = accessors.size() - 1;
            primitive.material = material.value().index;
            primitive.slot = group.material_slot;
            if (export_materials) {
                const auto first = stream.artifact.triangle_faces.begin() + range->first_index / 3U;
                primitive.triangle_faces.assign(first, first + range->index_count / 3U);
            }
            record.primitives.push_back(primitive);
            report.triangles += indices.size() / 3;
            report.index_bytes += indices.size() * width;
            if (buffer.size() > kMaxExportBufferBytes)
                return core::Result<GltfExportReport>::failure(
                    validation("glTF export exceeds 256 MiB derived-buffer limit"));
        }
        if (!remaining_authored_tangents.empty())
            return core::Result<GltfExportReport>::failure(validation(
                "glTF export cannot preserve unused authored tangent frames on mesh " + std::to_string(mesh_id)));
        meshes.push_back(std::move(record));
    }
    auto material_json = material_writer.finish();
    if (!material_json)
        return core::Result<GltfExportReport>::failure(material_json.error());

    if (any_missing_uv) {
        if (any_active_uv) {
            report.warnings.push_back(
                "Some Cartographer meshes have no active render UV set; those meshes "
                "omit UVs and tangents; material bindings requiring UVs fail explicitly");
        } else {
            report.warnings.push_back("Meshes without an active render UV set omit "
                                      "UVs and tangents; visual material "
                                      "bindings requiring UVs fail explicitly");
        }
    }
    if (any_active_uv && any_missing_tangent) {
        report.warnings.push_back(
            "Some UV-bearing Cartographer meshes do not produce a valid tangent "
            "frame; those meshes use the default tangent path");
    }
    if (any_material_regions) {
        report.warnings.push_back(
            "condition.material_region face identity is retained in extras; visual "
            "assignment uses the single authored material-region alias");
    }
    report.warnings.push_back(
        "Export disables face culling like the authoring viewport; glTF back-face normal reversal "
        "and consumer lighting still require rendered acceptance");
    report.warnings.push_back(
        "float64 authoring positions and normals are narrowed to glTF float32");
    if (!document.source_asset_registry().records().empty()) {
        report.warnings.push_back(
            "Imported source files and reimport links are not included in this glTF artifact; "
            "retain the Cartographer project and original source files for reimport");
    }
    const auto& presentation = document.presentation_catalog();
    if (presentation.environment() || !presentation.lights().empty() || !presentation.cameras().empty()) {
        report.warnings.push_back(
            "This derived mesh/material profile does not export authored cameras, lights or "
            "HDR environment lighting; configure consumer lighting before judging appearance parity");
    }

    std::map<scene::ObjectId, std::size_t> node_indices;
    std::vector<NodeRecord> nodes;
    const auto objects = document.scene().objects_sorted();
    nodes.reserve(objects.size());
    for (const auto &object : objects) {
        const auto mesh = object.mesh_asset.has_value()
                              ? std::find_if(document.meshes().begin(), document.meshes().end(),
                                             [&object](const auto &entry) {
                                                 return entry.first == *object.mesh_asset;
                                             })
                              : document.meshes().end();
        if (object.mesh_asset.has_value() && mesh == document.meshes().end()) {
            return core::Result<GltfExportReport>::failure(
                validation("scene object references a mesh missing from the export"));
        }
        std::optional<std::size_t> mesh_index;
        if (mesh != document.meshes().end()) {
            const auto ordinal =
                static_cast<std::size_t>(std::distance(document.meshes().begin(), mesh));
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
         << "  \"asset\":{\"version\":\"2.0\",\"generator\":\"Cartographer " << CARTOGRAPHER_VERSION
         << "\"},\n"
         << "  \"scene\":0,\n"
         << "  \"scenes\":[{\"nodes\":[";
    bool first = true;
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        if (nodes[index].parent.has_value())
            continue;
        if (!first)
            json << ',';
        first = false;
        json << index;
    }
    json << "]}],\n  \"nodes\":[\n";
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const auto &node = nodes[index];
        json << "    {\"name\":\"" << json_escape(node.name) << "\"";
        if (node.mesh.has_value())
            json << ",\"mesh\":" << *node.mesh;
        json << ',';
        if (auto transform = append_transform_json(json, node.transform); !transform) {
            return core::Result<GltfExportReport>::failure(transform.error());
        }
        json << ",\"extras\":{\"cartographer_object_id\":" << node.id.value
             << ",\"vanta_object_id\":" << node.id.value
             << ",\"visible\":" << (node.visible ? "true" : "false")
             << ",\"locked\":" << (node.locked ? "true" : "false")
             << ",\"vanta_visible\":" << (node.visible ? "true" : "false")
             << ",\"vanta_locked\":" << (node.locked ? "true" : "false") << "}";
        std::vector<std::size_t> children;
        for (std::size_t child = 0U; child < nodes.size(); ++child) {
            if (nodes[child].parent == index)
                children.push_back(child);
        }
        if (!children.empty()) {
            json << ",\"children\":[";
            for (std::size_t child = 0U; child < children.size(); ++child) {
                if (child != 0U)
                    json << ',';
                json << children[child];
            }
            json << ']';
        }
        json << "}" << (index + 1U == nodes.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"meshes\":[\n";
    for (std::size_t index = 0U; index < meshes.size(); ++index) {
        const auto &mesh = meshes[index];
        json << "    {\"name\":\"" << json_escape(mesh.name) << "\",\"primitives\":[";
        for (std::size_t n = 0; n < mesh.primitives.size(); ++n) {
            const auto &primitive = mesh.primitives[n];
            if (n)
                json << ',';
            json << "{\"attributes\":{\"POSITION\":" << primitive.position_accessor
                 << ",\"NORMAL\":" << primitive.normal_accessor;
            if (primitive.uv_accessor)
                json << ",\"TEXCOORD_0\":" << *primitive.uv_accessor;
            if (primitive.tangent_accessor)
                json << ",\"TANGENT\":" << *primitive.tangent_accessor;
            json << "},\"indices\":" << primitive.index_accessor
                 << ",\"material\":" << primitive.material << ",\"mode\":" << kGlTriangles
                 << ",\"extras\":{\"cartographer_mesh_id\":" << mesh.source_id;
            if (primitive.slot)
                json << ",\"cartographer_material_slot\":" << *primitive.slot;
            if (export_materials && primitive.slot) {
                json << ",\"cartographer_material_region\":" << *primitive.slot
                     << ",\"cartographer_material_region_layer\":\"" << json_escape(mesh.region_layer) << "\""
                     << ",\"cartographer_mesh_revision\":" << mesh.source_mesh_revision
                     << ",\"cartographer_triangle_faces\":[";
                for (std::size_t f = 0; f < primitive.triangle_faces.size(); ++f) {
                    if (f) json << ',';
                    json << primitive.triangle_faces[f].value;
                }
                json << ']';
            }
            if (n == 0U && mesh.material_regions.has_value()) {
                json << ",\"condition_material_region\":{\"layer\":\"condition.material_region\","
                        "\"domain\":\"face\",\"faces\":[";
                for (std::size_t f = 0; f < mesh.material_regions->size(); ++f) {
                    if (f) json << ',';
                    const auto [face, region] = (*mesh.material_regions)[f];
                    json << "{\"id\":" << face << ",\"region\":" << region << '}';
                }
                json << "]}";
            }
            json << "}}";
        }
        json << "],\"extras\":{\"cartographer_mesh_id\":" << mesh.source_id;
        if (mesh.material_regions.has_value()) {
            json << ",\"condition_material_region\":{\"layer\":\"condition.material_"
                    "region\","
                    "\"domain\":\"face\",\"faces\":[";
            for (std::size_t region_index = 0U; region_index < mesh.material_regions->size();
                 ++region_index) {
                if (region_index != 0U)
                    json << ',';
                const auto [face_id, region_id] = (*mesh.material_regions)[region_index];
                json << "{\"id\":" << face_id << ",\"region\":" << region_id << '}';
            }
            json << "]}";
        }
        json << "}}" << (index + 1U == meshes.size() ? "\n" : ",\n");
    }
    json << "  ],\n" << material_json.value() << "  \"accessors\":[\n";
    for (std::size_t index = 0U; index < accessors.size(); ++index) {
        const auto &accessor = accessors[index];
        json << "    {\"bufferView\":" << accessor.buffer_view
             << ",\"componentType\":" << accessor.component_type << ",\"count\":" << accessor.count
             << ",\"type\":\"" << accessor.type << '"';
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
        const auto &view = views[index];
        json << "    {\"buffer\":0,\"byteOffset\":" << view.offset
             << ",\"byteLength\":" << view.length << ",\"target\":" << view.target << '}'
             << (index + 1U == views.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"buffers\":[{\"byteLength\":" << buffer.size();
    if (!binary_container) {
        json << ",\"uri\":\"data:application/octet-stream;base64," << base64(buffer) << '\"';
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
            return core::Result<GltfExportReport>::failure(
                validation("GLB export exceeds the version 2 container size limit"));
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
            output.write(reinterpret_cast<const char *>(buffer.data()),
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

core::Result<GltfExportReport> export_gltf(const project::ProjectDocument &document,
                                           const std::filesystem::path &path) {
    return export_interchange(document, path, false, {});
}

core::Result<GltfExportReport> export_glb(const project::ProjectDocument &document,
                                          const std::filesystem::path &path) {
    return export_interchange(document, path, true, {});
}

core::Result<GltfExportReport> export_gltf(const project::ProjectDocument &document,
                                           const std::filesystem::path &path,
                                           const GltfExportOptions &options) {
    return export_interchange(document, path, false, options);
}
core::Result<GltfExportReport> export_glb(const project::ProjectDocument &document,
                                          const std::filesystem::path &path,
                                          const GltfExportOptions &options) {
    return export_interchange(document, path, true, options);
}
} // namespace carto::io
