#include <carto/io/obj.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <string_view>
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

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

constexpr std::uintmax_t kMaxObjBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxObjVertices = 1'000'000U;
constexpr std::size_t kMaxObjUvs = 1'000'000U;
constexpr std::size_t kMaxObjNormals = 1'000'000U;
constexpr std::size_t kMaxObjFaces = 1'000'000U;
constexpr std::size_t kMaxObjFaceVertices = 1'000'000U;
constexpr std::size_t kMaxObjWarnings = 10'000U;
std::mutex g_obj_export_mutex;
std::atomic<std::uint64_t> g_obj_temp_counter{0U};

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_obj_temp_counter.fetch_add(1U, std::memory_order_relaxed);
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
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error, "atomic OBJ replacement failed on Windows"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error, "atomic OBJ replacement failed: " + error.message()));
    }
    return core::Result<void>::success();
#endif
}

core::Result<std::size_t> parse_index_component(
    std::string_view token, std::size_t count, std::string_view component) {
    if (token.empty()) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed,
                "OBJ face token has no " + std::string(component) + " index"));
    }
    const std::string position_text(token);
    long long raw = 0;
    std::size_t consumed = 0;
    try {
        raw = std::stoll(position_text, &consumed);
    } catch (...) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed,
                "OBJ face " + std::string(component) + " index is not an integer"));
    }
    if (consumed != position_text.size()) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed,
                "OBJ face " + std::string(component) + " index is not an integer"));
    }
    if (raw == 0) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed,
                "OBJ indices are one-based and cannot be zero"));
    }
    const long long resolved = raw > 0 ? raw - 1 : static_cast<long long>(count) + raw;
    if (resolved < 0 || static_cast<std::size_t>(resolved) >= count) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed,
                "OBJ face " + std::string(component) + " index is out of range"));
    }
    return core::Result<std::size_t>::success(static_cast<std::size_t>(resolved));
}

struct ObjFaceReference final {
    std::size_t position = 0U;
    std::optional<std::size_t> uv;
    std::optional<std::size_t> normal;
};

core::Result<ObjFaceReference> parse_face_reference(
    std::string_view token,
    std::size_t vertex_count,
    std::size_t uv_count,
    std::size_t normal_count) {
    const std::size_t first_slash = token.find('/');
    const std::string_view position_token = token.substr(0U, first_slash);
    auto position = parse_index_component(position_token, vertex_count, "position");
    if (!position) return core::Result<ObjFaceReference>::failure(position.error());
    if (first_slash == std::string_view::npos) {
        return core::Result<ObjFaceReference>::success(
            {position.value(), std::nullopt, std::nullopt});
    }

    const std::size_t second_slash = token.find('/', first_slash + 1U);
    const std::size_t uv_end = second_slash == std::string_view::npos
        ? token.size() : second_slash;
    const std::string_view uv_token = token.substr(
        first_slash + 1U, uv_end - first_slash - 1U);
    std::optional<std::size_t> uv_index;
    if (!uv_token.empty()) {
        auto parsed_uv = parse_index_component(uv_token, uv_count, "texture-coordinate");
        if (!parsed_uv) return core::Result<ObjFaceReference>::failure(parsed_uv.error());
        uv_index = parsed_uv.value();
    } else if (second_slash == std::string_view::npos) {
        return core::Result<ObjFaceReference>::failure(Diagnostic(
            ErrorCode::validation_failed, "OBJ face texture-coordinate index is empty"));
    }

    if (second_slash != std::string_view::npos) {
        if (token.find('/', second_slash + 1U) != std::string_view::npos) {
            return core::Result<ObjFaceReference>::failure(Diagnostic(
                ErrorCode::validation_failed, "OBJ face token has too many index separators"));
        }
        const std::string_view normal_token = token.substr(second_slash + 1U);
        if (normal_token.empty()) {
            return core::Result<ObjFaceReference>::failure(Diagnostic(
                ErrorCode::validation_failed, "OBJ face normal index is empty"));
        }
        auto parsed_normal = parse_index_component(normal_token, normal_count, "normal");
        if (!parsed_normal) {
            return core::Result<ObjFaceReference>::failure(parsed_normal.error());
        }
        return core::Result<ObjFaceReference>::success(
            {position.value(), uv_index, parsed_normal.value()});
    }
    return core::Result<ObjFaceReference>::success(
        {position.value(), uv_index, std::nullopt});
}

core::Result<void> isolate_conflicting_directed_edges(
    std::vector<geometry::Vertex>& vertices,
    std::vector<geometry::Face>& faces,
    IoReport& report) {
    std::map<geometry::VertexId, core::Vec3d> positions;
    std::uint64_t next_vertex_id = 1U;
    for (const auto& vertex : vertices) {
        positions.emplace(vertex.id, vertex.position);
        if (vertex.id.value < std::numeric_limits<std::uint64_t>::max()) {
            next_vertex_id = std::max(next_vertex_id, vertex.id.value + 1U);
        }
    }

    std::map<std::pair<geometry::VertexId, geometry::VertexId>, geometry::FaceId> directed_edges;
    for (auto& face : faces) {
        bool conflicts = false;
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const auto edge = std::make_pair(
                face.vertices[index], face.vertices[(index + 1U) % face.vertices.size()]);
            if (directed_edges.contains(edge)) {
                conflicts = true;
                break;
            }
        }

        if (conflicts) {
            if (vertices.size() > kMaxObjVertices - face.vertices.size()) {
                return core::Result<void>::failure(
                    validation("OBJ topology repair would exceed the vertex import limit"));
            }
            std::vector<geometry::VertexId> isolated_vertices;
            isolated_vertices.reserve(face.vertices.size());
            for (const geometry::VertexId original : face.vertices) {
                const auto position = positions.find(original);
                if (position == positions.end() || next_vertex_id == std::numeric_limits<std::uint64_t>::max()) {
                    return core::Result<void>::failure(
                        validation("OBJ topology repair found an invalid vertex reference"));
                }
                const geometry::VertexId isolated{next_vertex_id++};
                vertices.push_back(geometry::Vertex{isolated, position->second});
                positions.emplace(isolated, position->second);
                isolated_vertices.push_back(isolated);
            }
            face.vertices = std::move(isolated_vertices);
            if (report.warnings.size() >= kMaxObjWarnings) {
                return core::Result<void>::failure(
                    validation("OBJ topology repair warning limit exceeded"));
            }
            report.warnings.push_back(
                "isolated OBJ face " + std::to_string(face.id.value) +
                " by duplicating vertices across a conflicting directed edge");
        }

        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const auto edge = std::make_pair(
                face.vertices[index], face.vertices[(index + 1U) % face.vertices.size()]);
            if (!directed_edges.emplace(edge, face.id).second) {
                return core::Result<void>::failure(
                    validation("OBJ topology contains an unrecoverable duplicate directed edge"));
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<IoReport> export_obj(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path) {
    std::lock_guard lock(g_obj_export_mutex);
    if (path.empty() || path.filename().empty()) {
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::invalid_argument, "OBJ export path must name a file"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code parent_error;
        const bool parent_exists = std::filesystem::exists(parent, parent_error);
        if (parent_error || !parent_exists ||
            !std::filesystem::is_directory(parent, parent_error) || parent_error) {
            return core::Result<IoReport>::failure(
                Diagnostic(ErrorCode::io_error, "OBJ export parent directory is unavailable"));
        }
    }
    auto compiled = mesh.compile();
    if (!compiled) {
        return core::Result<IoReport>::failure(compiled.error());
    }
    const auto* render_uv_set = mesh.uv_sets().active_for_render();
    const auto* custom_normals = mesh.attributes().find("cartographer.normal0");
    const bool has_uv_sets = !mesh.uv_sets().descriptors().empty();
    const auto* condition_material_regions = mesh.attributes().find(
        "condition.material_region");
    const auto* generic_material_regions = mesh.attributes().find(
        "cartographer.material_slot");
    const auto* material_regions = condition_material_regions != nullptr
        ? condition_material_regions : generic_material_regions;
    std::map<geometry::FaceId, std::uint32_t> material_region_by_face;
    std::string material_region_layer_id;
    if (condition_material_regions != nullptr && generic_material_regions != nullptr) {
        if (condition_material_regions->descriptor.domain != attributes::AttributeDomain::face ||
            condition_material_regions->descriptor.type != attributes::AttributeType::uint32 ||
            generic_material_regions->descriptor.domain != attributes::AttributeDomain::face ||
            generic_material_regions->descriptor.type != attributes::AttributeType::uint32 ||
            condition_material_regions->values.size() != generic_material_regions->values.size()) {
            return core::Result<IoReport>::failure(validation(
                "OBJ export found conflicting Cartographer face material-region layers"));
        }
        for (std::size_t index = 0U;
             index < condition_material_regions->values.size(); ++index) {
            const auto* condition_region = std::get_if<std::uint32_t>(
                &condition_material_regions->values[index]);
            const auto* generic_region = std::get_if<std::uint32_t>(
                &generic_material_regions->values[index]);
            if (condition_region == nullptr || generic_region == nullptr ||
                *condition_region != *generic_region) {
                return core::Result<IoReport>::failure(validation(
                    "OBJ export found conflicting Cartographer face material-region layers"));
            }
        }
    }
    if (material_regions != nullptr) {
        const auto faces = mesh.faces_sorted();
        if (material_regions->descriptor.domain != attributes::AttributeDomain::face ||
            material_regions->descriptor.type != attributes::AttributeType::uint32 ||
            material_regions->values.size() != faces.size()) {
            return core::Result<IoReport>::failure(validation(
                "OBJ export material-region payload does not match source faces"));
        }
        material_region_layer_id = material_regions->descriptor.id;
        for (std::size_t index = 0U; index < faces.size(); ++index) {
            const auto* region = std::get_if<std::uint32_t>(
                &material_regions->values[index]);
            if (region == nullptr || *region == 0U ||
                !material_region_by_face.emplace(faces[index].id, *region).second) {
                return core::Result<IoReport>::failure(validation(
                    "OBJ export material-region payload contains an invalid face region"));
            }
        }
    }
    std::vector<core::Vec2d> triangle_uvs;
    std::vector<core::Vec3d> triangle_normals;
    bool export_uvs = false;
    bool export_normals = false;
    if (render_uv_set != nullptr) {
        const auto* uv_layer = mesh.attributes().find(render_uv_set->layer_id);
        const auto topology = mesh.topology();
        if (uv_layer == nullptr || !topology ||
            uv_layer->descriptor.domain != attributes::AttributeDomain::corner ||
            uv_layer->descriptor.type != attributes::AttributeType::vec2 ||
            uv_layer->values.size() != topology.value().corners.size()) {
            return core::Result<IoReport>::failure(validation(
                "active render UV set does not match OBJ source topology"));
        }
        std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec2d>
            uv_by_corner;
        for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
            const auto* uv = std::get_if<core::Vec2d>(&uv_layer->values[index]);
            const auto& corner = topology.value().corners[index];
            if (uv == nullptr || !uv->finite() ||
                !uv_by_corner.emplace(std::make_pair(corner.face, corner.vertex), *uv).second) {
                return core::Result<IoReport>::failure(validation(
                    "active render UV set contains an invalid or ambiguous corner"));
            }
        }
        triangle_uvs.reserve(compiled.value().indices.size());
        for (std::size_t index = 0U; index < compiled.value().indices.size(); ++index) {
            const std::size_t triangle = index / 3U;
            const auto vertex_index = compiled.value().indices[index];
            if (vertex_index >= compiled.value().vertex_ids.size() ||
                triangle >= compiled.value().triangle_faces.size()) {
                return core::Result<IoReport>::failure(validation(
                    "compiled OBJ triangle has no source corner identity"));
            }
            const auto found = uv_by_corner.find(std::make_pair(
                compiled.value().triangle_faces[triangle],
                compiled.value().vertex_ids[vertex_index]));
            if (found == uv_by_corner.end()) {
                return core::Result<IoReport>::failure(validation(
                    "active render UV set is missing a compiled triangle corner"));
            }
            triangle_uvs.push_back(found->second);
        }
        export_uvs = true;
    }
    if (custom_normals != nullptr) {
        const auto topology = mesh.topology();
        if (!topology ||
            custom_normals->descriptor.domain != attributes::AttributeDomain::corner ||
            custom_normals->descriptor.type != attributes::AttributeType::vec3 ||
            custom_normals->values.size() != topology.value().corners.size()) {
            return core::Result<IoReport>::failure(validation(
                "OBJ custom-normal layer does not match source corners"));
        }
        std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec3d>
            normal_by_corner;
        for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
            const auto* normal = std::get_if<core::Vec3d>(&custom_normals->values[index]);
            const auto& corner = topology.value().corners[index];
            const double length_squared = normal == nullptr ? 0.0 : normal->length_squared();
            if (normal == nullptr || !normal->finite() || !std::isfinite(length_squared) ||
                length_squared <= 1.0e-24 ||
                !normal_by_corner.emplace(std::make_pair(corner.face, corner.vertex), *normal).second) {
                return core::Result<IoReport>::failure(validation(
                    "OBJ custom-normal layer contains an invalid or ambiguous corner"));
            }
        }
        triangle_normals.reserve(compiled.value().indices.size());
        for (std::size_t index = 0U; index < compiled.value().indices.size(); ++index) {
            const std::size_t triangle = index / 3U;
            const auto vertex_index = compiled.value().indices[index];
            if (vertex_index >= compiled.value().vertex_ids.size() ||
                triangle >= compiled.value().triangle_faces.size()) {
                return core::Result<IoReport>::failure(validation(
                    "compiled OBJ triangle has no source corner identity"));
            }
            const auto found = normal_by_corner.find(std::make_pair(
                compiled.value().triangle_faces[triangle],
                compiled.value().vertex_ids[vertex_index]));
            if (found == normal_by_corner.end()) {
                return core::Result<IoReport>::failure(validation(
                    "custom-normal layer is missing a compiled triangle corner"));
            }
            triangle_normals.push_back(found->second);
        }
        export_normals = true;
    }
    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open OBJ temporary output"));
    }
    output << "# Cartographer OBJ export; polygon topology is triangulated for interchange\n";
    if (material_regions != nullptr) {
        output << "# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 "
               << material_region_layer_id << '\n';
        for (std::size_t triangle = 0U;
             triangle < compiled.value().triangle_faces.size(); ++triangle) {
            const auto region = material_region_by_face.find(
                compiled.value().triangle_faces[triangle]);
            if (region == material_region_by_face.end()) {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return core::Result<IoReport>::failure(validation(
                    "OBJ export triangle lost its source material region"));
            }
            output << "# CARTOGRAPHER_FACE_MATERIAL " << triangle + 1U << ' '
                   << region->second << '\n';
        }
    }
    output << std::setprecision(17);
    for (const auto& position : compiled.value().positions) {
        output << "v " << position.x << ' ' << position.y << ' ' << position.z << '\n';
    }
    if (export_uvs) {
        for (const auto& uv : triangle_uvs) output << "vt " << uv.x << ' ' << uv.y << '\n';
    }
    if (export_normals) {
        for (const auto& normal : triangle_normals) {
            output << "vn " << normal.x << ' ' << normal.y << ' ' << normal.z << '\n';
        }
    }
    for (std::size_t index = 0; index < compiled.value().indices.size(); index += 3U) {
        output << "f";
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            output << ' ' << compiled.value().indices[index + corner] + 1U;
            if (export_uvs) output << '/' << index + corner + 1U;
            if (export_normals) {
                if (!export_uvs) output << "//";
                else output << '/';
                output << index + corner + 1U;
            }
        }
        output << '\n';
    }
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "failed while writing OBJ temporary output"));
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "failed while closing OBJ temporary output"));
    }
    auto replacement = atomic_replace(temporary, path);
    if (!replacement) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(replacement.error());
    }
    std::vector<std::string> warnings;
    std::string export_summary = "OBJ export includes positions and triangulated faces";
    if (export_uvs) export_summary += ", active render UVs";
    if (material_regions != nullptr) export_summary += ", Cartographer face material-region IDs";
    if (export_normals) export_summary += ", custom corner normals";
    export_summary +=
        "; material assets/assignments, additional UV sets, and unsupported attributes are omitted";
    warnings.push_back(std::move(export_summary));
    if (!export_uvs && has_uv_sets) {
        warnings.push_back(
            "UV payload was omitted because no UV set is active for rendering");
    }
    return core::Result<IoReport>::success(IoReport{
        compiled.value().positions.size(), mesh.face_count(),
        compiled.value().indices.size() / 3U, std::move(warnings)});
}

core::Result<ImportResult> import_obj(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<ImportResult>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open OBJ input"));
    }
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return core::Result<ImportResult>::failure(
            Diagnostic(ErrorCode::io_error, "unable to inspect OBJ input size"));
    }
    if (file_size > kMaxObjBytes) {
        return core::Result<ImportResult>::failure(
            validation("OBJ input exceeds the Cartographer import limit of 128 MiB"));
    }
    ImportResult result;
    std::vector<geometry::Vertex> parsed_vertices;
    std::vector<core::Vec2d> parsed_uvs;
    std::vector<core::Vec3d> parsed_normals;
    std::vector<geometry::Face> faces;
    std::vector<std::vector<core::Vec2d>> face_uvs;
    std::vector<std::vector<core::Vec3d>> face_normals;
    std::optional<std::string> material_region_layer_id;
    std::map<std::uint64_t, std::uint32_t> face_material_regions;
    bool any_face_has_uvs = false;
    bool any_face_lacks_uvs = false;
    bool any_face_has_normals = false;
    bool any_face_lacks_normals = false;
    std::string line;
    std::size_t line_number = 0;
    std::uintmax_t bytes_read = 0;
    while (std::getline(input, line)) {
        ++line_number;
        bytes_read += static_cast<std::uintmax_t>(line.size()) + 1U;
        if (bytes_read > kMaxObjBytes) {
            return core::Result<ImportResult>::failure(
                validation("OBJ input exceeds the Cartographer import limit of 128 MiB"));
        }
        std::istringstream fields(line);
        std::string tag;
        fields >> tag;
        if (tag.empty()) {
            continue;
        }
        if (tag[0] == '#') {
            std::string marker;
            if (tag == "#") fields >> marker;
            if (marker == "CARTOGRAPHER_FACE_MATERIAL_REGIONS") {
                std::uint64_t version = 0U;
                std::string layer_id;
                std::string trailing;
                if (material_region_layer_id.has_value() ||
                    !(fields >> version >> layer_id) || version != 1U ||
                    (layer_id != "condition.material_region" &&
                     layer_id != "cartographer.material_slot") ||
                    (fields >> trailing && !trailing.starts_with('#'))) {
                    return core::Result<ImportResult>::failure(validation(
                        "OBJ Cartographer material-region header is malformed or unsupported"));
                }
                material_region_layer_id = std::move(layer_id);
                continue;
            }
            if (marker == "CARTOGRAPHER_FACE_MATERIAL") {
                std::uint64_t face_id = 0U;
                std::uint64_t region_id = 0U;
                std::string trailing;
                if (!material_region_layer_id.has_value() ||
                    !(fields >> face_id >> region_id) || face_id == 0U ||
                    region_id == 0U ||
                    region_id > std::numeric_limits<std::uint32_t>::max() ||
                    (fields >> trailing && !trailing.starts_with('#')) ||
                    !face_material_regions.emplace(face_id,
                        static_cast<std::uint32_t>(region_id)).second) {
                    return core::Result<ImportResult>::failure(validation(
                        "OBJ Cartographer face material-region record is malformed or duplicated"));
                }
                continue;
            }
            continue;
        }
        if (tag == "v") {
            if (parsed_vertices.size() >= kMaxObjVertices) {
                return core::Result<ImportResult>::failure(
                    validation("OBJ vertex count exceeds the Cartographer import limit"));
            }
            core::Vec3d position;
            if (!(fields >> position.x >> position.y >> position.z) || !position.finite()) {
                return core::Result<ImportResult>::failure(
                    Diagnostic(ErrorCode::validation_failed, "invalid OBJ vertex at line " + std::to_string(line_number)));
            }
            const geometry::VertexId id{static_cast<std::uint64_t>(parsed_vertices.size()) + 1U};
            parsed_vertices.push_back(geometry::Vertex{id, position});
            continue;
        }
        if (tag == "vt") {
            if (parsed_uvs.size() >= kMaxObjUvs) {
                return core::Result<ImportResult>::failure(
                    validation("OBJ texture-coordinate count exceeds the Cartographer import limit"));
            }
            core::Vec2d uv;
            if (!(fields >> uv.x >> uv.y) || !uv.finite()) {
                return core::Result<ImportResult>::failure(Diagnostic(
                    ErrorCode::validation_failed,
                    "invalid OBJ texture coordinate at line " + std::to_string(line_number)));
            }
            std::string trailing;
            if (fields >> trailing && !trailing.starts_with('#')) {
                double third_coordinate = 0.0;
                std::size_t consumed = 0U;
                try {
                    third_coordinate = std::stod(trailing, &consumed);
                } catch (...) {
                    consumed = 0U;
                }
                if (consumed != trailing.size() || !std::isfinite(third_coordinate)) {
                    return core::Result<ImportResult>::failure(Diagnostic(
                        ErrorCode::validation_failed,
                        "invalid optional OBJ texture-coordinate component at line " +
                            std::to_string(line_number)));
                }
                if (fields >> trailing && !trailing.starts_with('#')) {
                    return core::Result<ImportResult>::failure(Diagnostic(
                        ErrorCode::validation_failed,
                        "unexpected OBJ texture-coordinate data at line " +
                            std::to_string(line_number)));
                }
            }
            parsed_uvs.push_back(uv);
            continue;
        }
        if (tag == "vn") {
            if (parsed_normals.size() >= kMaxObjNormals) {
                return core::Result<ImportResult>::failure(
                    validation("OBJ normal count exceeds the Cartographer import limit"));
            }
            core::Vec3d normal;
            if (!(fields >> normal.x >> normal.y >> normal.z) || !normal.finite() ||
                !std::isfinite(normal.length_squared()) ||
                normal.length_squared() <= 1.0e-24) {
                return core::Result<ImportResult>::failure(Diagnostic(
                    ErrorCode::validation_failed,
                    "invalid OBJ normal at line " + std::to_string(line_number)));
            }
            std::string trailing;
            if (fields >> trailing && !trailing.starts_with('#')) {
                return core::Result<ImportResult>::failure(Diagnostic(
                    ErrorCode::validation_failed,
                    "unexpected OBJ normal data at line " + std::to_string(line_number)));
            }
            parsed_normals.push_back(normal);
            continue;
        }
        if (tag == "f") {
            if (faces.size() >= kMaxObjFaces) {
                return core::Result<ImportResult>::failure(
                    validation("OBJ face count exceeds the Cartographer import limit"));
            }
            std::vector<geometry::VertexId> face_vertices;
            std::vector<std::size_t> face_uv_indices;
            std::vector<std::size_t> face_normal_indices;
            std::string token;
            std::size_t token_count = 0;
            bool face_has_uv = false;
            bool face_has_missing_uv = false;
            bool face_has_normal = false;
            bool face_has_missing_normal = false;
            while (fields >> token) {
                if (!token.empty() && token.front() == '#') break;
                if (token_count >= kMaxObjFaceVertices) {
                    return core::Result<ImportResult>::failure(
                        validation("OBJ face vertex count exceeds the Cartographer import limit"));
                }
                auto parsed = parse_face_reference(
                    token, parsed_vertices.size(), parsed_uvs.size(), parsed_normals.size());
                if (!parsed) {
                    return core::Result<ImportResult>::failure(parsed.error().with_context("OBJ line " + std::to_string(line_number)));
                }
                face_vertices.push_back(geometry::VertexId{
                    static_cast<std::uint64_t>(parsed.value().position) + 1U});
                if (parsed.value().uv.has_value()) {
                    face_has_uv = true;
                    face_uv_indices.push_back(*parsed.value().uv);
                } else {
                    face_has_missing_uv = true;
                }
                if (parsed.value().normal.has_value()) {
                    face_has_normal = true;
                    face_normal_indices.push_back(*parsed.value().normal);
                } else {
                    face_has_missing_normal = true;
                }
                ++token_count;
            }
            if (face_has_uv && face_has_missing_uv) {
                return core::Result<ImportResult>::failure(Diagnostic(
                    ErrorCode::validation_failed,
                    "OBJ face mixes corners with and without texture coordinates at line " +
                        std::to_string(line_number)));
            }
            if (face_has_normal && face_has_missing_normal) {
                return core::Result<ImportResult>::failure(Diagnostic(
                    ErrorCode::validation_failed,
                    "OBJ face mixes corners with and without normals at line " +
                        std::to_string(line_number)));
            }
            any_face_has_uvs = any_face_has_uvs || face_has_uv;
            any_face_lacks_uvs = any_face_lacks_uvs || !face_has_uv;
            any_face_has_normals = any_face_has_normals || face_has_normal;
            any_face_lacks_normals = any_face_lacks_normals || !face_has_normal;
            if (face_has_uv) {
                std::vector<core::Vec2d> values;
                values.reserve(face_uv_indices.size());
                for (const std::size_t index : face_uv_indices) values.push_back(parsed_uvs[index]);
                face_uvs.push_back(std::move(values));
            } else {
                face_uvs.emplace_back();
            }
            if (face_has_normal) {
                std::vector<core::Vec3d> values;
                values.reserve(face_normal_indices.size());
                for (const std::size_t index : face_normal_indices) {
                    values.push_back(parsed_normals[index]);
                }
                face_normals.push_back(std::move(values));
            } else {
                face_normals.emplace_back();
            }
            faces.push_back(geometry::Face{
                geometry::FaceId{static_cast<std::uint64_t>(faces.size()) + 1U},
                std::move(face_vertices)});
            continue;
        }
        if (result.report.warnings.size() >= kMaxObjWarnings) {
            return core::Result<ImportResult>::failure(
                validation("OBJ unsupported-record warning limit exceeded"));
        }
        result.report.warnings.push_back("ignored unsupported OBJ record '" + tag + "' at line " + std::to_string(line_number));
    }
    if (!input.eof() && input.fail()) {
        return core::Result<ImportResult>::failure(
            Diagnostic(ErrorCode::io_error, "failed while reading OBJ input"));
    }
    if (any_face_has_uvs && any_face_lacks_uvs) {
        return core::Result<ImportResult>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "OBJ has partial texture-coordinate coverage; Cartographer requires UVs for every face"));
    }
    if (any_face_has_normals && any_face_lacks_normals) {
        return core::Result<ImportResult>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "OBJ has partial normal coverage; Cartographer requires normals for every face"));
    }
    if (auto repaired = isolate_conflicting_directed_edges(parsed_vertices, faces, result.report);
        !repaired) {
        return core::Result<ImportResult>::failure(repaired.error().with_context("OBJ topology repair"));
    }
    if (auto inserted = result.mesh.insert_bulk(std::move(parsed_vertices), std::move(faces)); !inserted) {
        return core::Result<ImportResult>::failure(inserted.error().with_context("bulk OBJ topology admission"));
    }
    attributes::AttributeSet mesh_attributes;
    attributes::UvSetTable uv_sets;
    bool has_attribute_payload = false;
    if (any_face_has_uvs) {
        std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec2d>
            uv_by_face_vertex;
        const auto admitted_faces = result.mesh.faces_sorted();
        if (admitted_faces.size() != face_uvs.size()) {
            return core::Result<ImportResult>::failure(validation(
                "OBJ UV face mapping differs from admitted topology"));
        }
        for (std::size_t face_index = 0U; face_index < admitted_faces.size(); ++face_index) {
            const auto& face = admitted_faces[face_index];
            const auto& values = face_uvs[face_index];
            if (face.vertices.size() != values.size()) {
                return core::Result<ImportResult>::failure(validation(
                    "OBJ UV corner count differs from its admitted face"));
            }
            for (std::size_t corner = 0U; corner < face.vertices.size(); ++corner) {
                if (!uv_by_face_vertex.emplace(
                        std::make_pair(face.id, face.vertices[corner]), values[corner]).second) {
                    return core::Result<ImportResult>::failure(validation(
                        "OBJ UV corner identity is ambiguous"));
                }
            }
        }
        const auto topology = result.mesh.topology();
        if (!topology) {
            return core::Result<ImportResult>::failure(
                topology.error().with_context("OBJ UV topology lookup"));
        }
        std::vector<attributes::AttributeValue> uv_values;
        uv_values.reserve(topology.value().corners.size());
        for (const auto& corner : topology.value().corners) {
            const auto found = uv_by_face_vertex.find(std::make_pair(corner.face, corner.vertex));
            if (found == uv_by_face_vertex.end()) {
                return core::Result<ImportResult>::failure(validation(
                    "OBJ UV payload is missing an admitted face corner"));
            }
            uv_values.emplace_back(found->second);
        }
        if (auto counted = mesh_attributes.set_domain_count(
                attributes::AttributeDomain::corner, uv_values.size()); !counted) {
            return core::Result<ImportResult>::failure(
                counted.error().with_context("OBJ UV domain count"));
        }
        if (auto added = mesh_attributes.add_layer(attributes::AttributeLayer{
                {"cartographer.uv0", attributes::AttributeDomain::corner,
                    attributes::AttributeType::vec2}, std::move(uv_values)}); !added) {
            return core::Result<ImportResult>::failure(
                added.error().with_context("OBJ UV layer admission"));
        }
        if (auto added = uv_sets.add(attributes::UvSetDescriptor{
                1U, "OBJ UV 0", "cartographer.uv0", true, true, false, 1U}); !added) {
            return core::Result<ImportResult>::failure(
                added.error().with_context("OBJ UV-set admission"));
        }
        has_attribute_payload = true;
    }
    if (any_face_has_normals) {
        const auto admitted_faces = result.mesh.faces_sorted();
        if (admitted_faces.size() != face_normals.size()) {
            return core::Result<ImportResult>::failure(validation(
                "OBJ normal face mapping differs from admitted topology"));
        }
        std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec3d>
            normal_by_face_vertex;
        for (std::size_t face_index = 0U; face_index < admitted_faces.size(); ++face_index) {
            const auto& face = admitted_faces[face_index];
            const auto& values = face_normals[face_index];
            if (face.vertices.size() != values.size()) {
                return core::Result<ImportResult>::failure(validation(
                    "OBJ normal corner count differs from its admitted face"));
            }
            for (std::size_t corner = 0U; corner < face.vertices.size(); ++corner) {
                if (!normal_by_face_vertex.emplace(
                        std::make_pair(face.id, face.vertices[corner]), values[corner]).second) {
                    return core::Result<ImportResult>::failure(validation(
                        "OBJ normal corner identity is ambiguous"));
                }
            }
        }
        const auto topology = result.mesh.topology();
        if (!topology) {
            return core::Result<ImportResult>::failure(
                topology.error().with_context("OBJ normal topology lookup"));
        }
        std::vector<attributes::AttributeValue> normal_values;
        normal_values.reserve(topology.value().corners.size());
        for (const auto& corner : topology.value().corners) {
            const auto found = normal_by_face_vertex.find(std::make_pair(corner.face, corner.vertex));
            if (found == normal_by_face_vertex.end()) {
                return core::Result<ImportResult>::failure(validation(
                    "OBJ normal payload is missing an admitted face corner"));
            }
            normal_values.emplace_back(found->second);
        }
        if (auto counted = mesh_attributes.set_domain_count(
                attributes::AttributeDomain::corner, normal_values.size()); !counted) {
            return core::Result<ImportResult>::failure(
                counted.error().with_context("OBJ normal domain count"));
        }
        if (auto added = mesh_attributes.add_layer(attributes::AttributeLayer{
                {"cartographer.normal0", attributes::AttributeDomain::corner,
                    attributes::AttributeType::vec3}, std::move(normal_values)}); !added) {
            return core::Result<ImportResult>::failure(
                added.error().with_context("OBJ normal layer admission"));
        }
        has_attribute_payload = true;
    }
    if (material_region_layer_id.has_value()) {
        const auto admitted_faces = result.mesh.faces_sorted();
        if (admitted_faces.empty() ||
            face_material_regions.size() != admitted_faces.size()) {
            return core::Result<ImportResult>::failure(validation(
                "OBJ material-region table does not cover every admitted face"));
        }
        std::vector<attributes::AttributeValue> values;
        values.reserve(admitted_faces.size());
        for (const auto& face : admitted_faces) {
            const auto region = face_material_regions.find(face.id.value);
            if (region == face_material_regions.end() || region->second == 0U) {
                return core::Result<ImportResult>::failure(validation(
                    "OBJ material-region table is missing a face or contains a zero region"));
            }
            values.emplace_back(region->second);
        }
        if (auto counted = mesh_attributes.set_domain_count(
                attributes::AttributeDomain::face, values.size()); !counted) {
            return core::Result<ImportResult>::failure(
                counted.error().with_context("OBJ material-region domain count"));
        }
        if (auto added = mesh_attributes.add_layer(attributes::AttributeLayer{
                {*material_region_layer_id, attributes::AttributeDomain::face,
                    attributes::AttributeType::uint32}, std::move(values)}); !added) {
            return core::Result<ImportResult>::failure(
                added.error().with_context("OBJ material-region layer admission"));
        }
        has_attribute_payload = true;
    } else if (!face_material_regions.empty()) {
        return core::Result<ImportResult>::failure(validation(
            "OBJ material-region records are present without a table header"));
    }
    if (has_attribute_payload) {
        if (auto attached = result.mesh.set_attribute_payload(
                std::move(mesh_attributes), std::move(uv_sets)); !attached) {
            return core::Result<ImportResult>::failure(
                attached.error().with_context("OBJ attribute payload attachment"));
        }
    }
    result.report.vertices = result.mesh.vertex_count();
    result.report.faces = result.mesh.face_count();
    auto compiled = result.mesh.compile();
    if (!compiled) {
        return core::Result<ImportResult>::failure(compiled.error());
    }
    result.report.triangles = compiled.value().indices.size() / 3U;
    return core::Result<ImportResult>::success(std::move(result));
}

} // namespace carto::io
