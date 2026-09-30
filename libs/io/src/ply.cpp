#include <carto/io/ply.hpp>

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
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
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

constexpr std::uintmax_t kMaxPlyBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxPlyVertices = 1'000'000U;
constexpr std::size_t kMaxPlyFaces = 1'000'000U;
constexpr std::size_t kMaxPlyFaceVertices = 1'000'000U;
std::mutex g_ply_export_mutex;
std::atomic<std::uint64_t> g_ply_temp_counter{0U};

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_ply_temp_counter.fetch_add(1U, std::memory_order_relaxed);
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
            Diagnostic(ErrorCode::io_error, "atomic PLY replacement failed on Windows"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error, "atomic PLY replacement failed: " + error.message()));
    }
    return core::Result<void>::success();
#endif
}

core::Result<void> validate_output_path(const std::filesystem::path& path, std::string_view format) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_argument, std::string(format) + " export path must name a file"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        if (!std::filesystem::exists(parent, error) || error ||
            !std::filesystem::is_directory(parent, error) || error) {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::io_error, std::string(format) + " export parent directory is unavailable"));
        }
    }
    return core::Result<void>::success();
}

core::Result<std::size_t> parse_count(std::string_view text, std::string_view field) {
    std::size_t consumed = 0;
    try {
        const auto value = std::stoull(std::string(text), &consumed);
        if (consumed != text.size() || value > std::numeric_limits<std::size_t>::max()) {
            throw std::out_of_range("count");
        }
        return core::Result<std::size_t>::success(static_cast<std::size_t>(value));
    } catch (...) {
        return core::Result<std::size_t>::failure(
            validation("PLY " + std::string(field) + " is not a valid non-negative count"));
    }
}

core::Result<double> parse_real(std::string_view text, std::string_view field) {
    std::size_t consumed = 0;
    try {
        const double value = std::stod(std::string(text), &consumed);
        if (consumed != text.size() || !std::isfinite(value)) throw std::out_of_range("real");
        return core::Result<double>::success(value);
    } catch (...) {
        return core::Result<double>::failure(
            validation("PLY " + std::string(field) + " is not a finite number"));
    }
}

struct Property {
    bool list = false;
    std::string name;
};

} // namespace

core::Result<IoReport> export_ply(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path) {
    std::lock_guard lock(g_ply_export_mutex);
    if (auto result = validate_output_path(path, "PLY"); !result) return core::Result<IoReport>::failure(result.error());
    if (auto result = mesh.validate(); !result) return core::Result<IoReport>::failure(result.error());
    const auto vertices = mesh.vertices_sorted();
    const auto faces = mesh.faces_sorted();
    if (vertices.size() > kMaxPlyVertices || faces.size() > kMaxPlyFaces) {
        return core::Result<IoReport>::failure(validation("PLY export exceeds the Cartographer record limit"));
    }

    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open PLY temporary output"));
    }
    output << "ply\nformat ascii 1.0\ncomment Cartographer polygon authoring export\n"
           << "element vertex " << vertices.size() << "\n"
           << "property double x\nproperty double y\nproperty double z\n"
           << "element face " << faces.size() << "\n"
           << "property list uchar uint vertex_indices\nend_header\n";
    output << std::setprecision(17);
    std::map<geometry::VertexId, std::size_t> indices;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        indices.emplace(vertices[index].id, index);
        output << vertices[index].position.x << ' ' << vertices[index].position.y << ' '
               << vertices[index].position.z << '\n';
    }
    for (const auto& face : faces) {
        output << face.vertices.size();
        for (const auto vertex : face.vertices) output << ' ' << indices.at(vertex);
        output << '\n';
    }
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "failed while writing PLY temporary output"));
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "failed while closing PLY temporary output"));
    }
    auto replacement = atomic_replace(temporary, path);
    if (!replacement) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(replacement.error());
    }
    std::size_t triangles = 0;
    for (const auto& face : faces) triangles += face.vertices.size() - 2U;
    return core::Result<IoReport>::success(IoReport{
        vertices.size(),
        faces.size(),
        triangles,
        {"PLY preserves polygon topology; normals, UVs, materials, and tangents are not included"},
    });
}

core::Result<PlyImportResult> import_ply(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return core::Result<PlyImportResult>::failure(
        Diagnostic(ErrorCode::io_error, "unable to open PLY input"));
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) return core::Result<PlyImportResult>::failure(
        Diagnostic(ErrorCode::io_error, "unable to inspect PLY input size"));
    if (file_size > kMaxPlyBytes) return core::Result<PlyImportResult>::failure(
        validation("PLY input exceeds the Cartographer import limit of 128 MiB"));

    std::string line;
    if (!std::getline(input, line) || line != "ply") return core::Result<PlyImportResult>::failure(
        validation("PLY magic is missing"));
    enum class Element { none, vertex, face } element = Element::none;
    std::size_t vertex_count = 0;
    std::size_t face_count = 0;
    std::vector<Property> vertex_properties;
    std::vector<Property> face_properties;
    bool saw_ascii = false;
    bool saw_header_end = false;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string tag;
        fields >> tag;
        if (tag.empty() || tag == "comment") continue;
        if (tag == "format") {
            std::string format;
            fields >> format;
            if (format != "ascii") return core::Result<PlyImportResult>::failure(
                validation("only ASCII PLY is supported"));
            saw_ascii = true;
            continue;
        }
        if (tag == "element") {
            std::string name;
            std::string count_text;
            fields >> name >> count_text;
            auto count = parse_count(count_text, name + " count");
            if (!count) return core::Result<PlyImportResult>::failure(count.error());
            if (name == "vertex") {
                element = Element::vertex;
                vertex_count = count.value();
            } else if (name == "face") {
                element = Element::face;
                face_count = count.value();
            } else {
                return core::Result<PlyImportResult>::failure(
                    validation("PLY element '" + name + "' is unsupported"));
            }
            continue;
        }
        if (tag == "property") {
            std::string type;
            fields >> type;
            Property property;
            if (type == "list") {
                std::string count_type;
                std::string value_type;
                fields >> count_type >> value_type >> property.name;
                property.list = true;
            } else {
                fields >> property.name;
            }
            if (property.name.empty() || element == Element::none) return core::Result<PlyImportResult>::failure(
                validation("PLY property record is malformed"));
            if (element == Element::vertex) vertex_properties.push_back(std::move(property));
            else face_properties.push_back(std::move(property));
            continue;
        }
        if (tag == "end_header") {
            saw_header_end = true;
            break;
        }
        return core::Result<PlyImportResult>::failure(
            validation("PLY header contains an unsupported record '" + tag + "'"));
    }
    if (!saw_ascii || !saw_header_end || vertex_count == 0U || face_count == 0U ||
        vertex_count > kMaxPlyVertices || face_count > kMaxPlyFaces) {
        return core::Result<PlyImportResult>::failure(validation("PLY header is incomplete or exceeds limits"));
    }
    const auto property_index = [&vertex_properties](std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < vertex_properties.size(); ++index) {
            if (vertex_properties[index].name == name && !vertex_properties[index].list) return index;
        }
        return std::nullopt;
    };
    const auto x_index = property_index("x");
    const auto y_index = property_index("y");
    const auto z_index = property_index("z");
    if (!x_index.has_value() || !y_index.has_value() || !z_index.has_value()) {
        return core::Result<PlyImportResult>::failure(validation("PLY vertex x/y/z properties are required"));
    }
    std::optional<std::size_t> face_list_index;
    for (std::size_t index = 0; index < face_properties.size(); ++index) {
        if (face_properties[index].list &&
            (face_properties[index].name == "vertex_indices" || face_properties[index].name == "vertex_index")) {
            face_list_index = index;
            break;
        }
    }
    if (!face_list_index.has_value()) return core::Result<PlyImportResult>::failure(
        validation("PLY face vertex_indices list property is required"));

    PlyImportResult result;
    result.mesh = {};
    std::vector<geometry::VertexId> vertex_ids;
    vertex_ids.reserve(vertex_count);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (!std::getline(input, line)) return core::Result<PlyImportResult>::failure(
            validation("PLY ended before all vertex records were read"));
        std::istringstream fields(line);
        std::vector<std::string> tokens;
        std::string token;
        while (fields >> token) tokens.push_back(std::move(token));
        if (tokens.size() < vertex_properties.size()) return core::Result<PlyImportResult>::failure(
            validation("PLY vertex record has too few properties"));
        auto x = parse_real(tokens[*x_index], "vertex x");
        auto y = parse_real(tokens[*y_index], "vertex y");
        auto z = parse_real(tokens[*z_index], "vertex z");
        if (!x || !y || !z) return core::Result<PlyImportResult>::failure(
            validation("PLY vertex record contains an invalid coordinate"));
        auto added = result.mesh.add_vertex({x.value(), y.value(), z.value()});
        if (!added) return core::Result<PlyImportResult>::failure(added.error());
        vertex_ids.push_back(added.value());
    }
    for (std::size_t face = 0; face < face_count; ++face) {
        if (!std::getline(input, line)) return core::Result<PlyImportResult>::failure(
            validation("PLY ended before all face records were read"));
        std::istringstream fields(line);
        std::vector<std::string> tokens;
        std::string token;
        while (fields >> token) tokens.push_back(std::move(token));
        std::size_t cursor = 0;
        std::vector<std::size_t> indices;
        for (std::size_t property = 0; property < face_properties.size(); ++property) {
            if (property == *face_list_index) {
                if (cursor >= tokens.size()) return core::Result<PlyImportResult>::failure(
                    validation("PLY face record is missing its vertex count"));
                auto count = parse_count(tokens[cursor++], "face vertex count");
                if (!count || count.value() < 3U || count.value() > kMaxPlyFaceVertices ||
                    count.value() > tokens.size() - cursor) {
                    return core::Result<PlyImportResult>::failure(validation("PLY face vertex list is invalid"));
                }
                indices.reserve(count.value());
                for (std::size_t index = 0; index < count.value(); ++index) {
                    auto parsed = parse_count(tokens[cursor++], "face vertex index");
                    if (!parsed || parsed.value() >= vertex_ids.size()) return core::Result<PlyImportResult>::failure(
                        validation("PLY face vertex index is out of range"));
                    indices.push_back(parsed.value());
                }
            } else {
                if (face_properties[property].list) return core::Result<PlyImportResult>::failure(
                    validation("PLY supports only one face list property"));
                if (cursor >= tokens.size()) return core::Result<PlyImportResult>::failure(
                    validation("PLY face record has too few properties"));
                ++cursor;
            }
        }
        std::vector<geometry::VertexId> face_vertices;
        face_vertices.reserve(indices.size());
        for (const auto index : indices) face_vertices.push_back(vertex_ids[index]);
        auto added = result.mesh.add_face(std::move(face_vertices));
        if (!added) return core::Result<PlyImportResult>::failure(added.error());
    }
    auto compiled = result.mesh.compile();
    if (!compiled) return core::Result<PlyImportResult>::failure(compiled.error());
    result.report = IoReport{
        result.mesh.vertex_count(),
        result.mesh.face_count(),
        compiled.value().indices.size() / 3U,
        {"PLY normals, UVs, materials, and tangents were not imported"},
    };
    return core::Result<PlyImportResult>::success(std::move(result));
}

} // namespace carto::io
