#include <carto/io/obj.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <string_view>
#include <thread>
#include <utility>

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

core::Result<std::size_t> parse_index(std::string_view token, std::size_t count) {
    const std::size_t slash = token.find('/');
    const std::string_view position = token.substr(0, slash);
    if (position.empty()) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed, "OBJ face token has no position index"));
    }
    const std::string position_text(position);
    long long raw = 0;
    std::size_t consumed = 0;
    try {
        raw = std::stoll(position_text, &consumed);
    } catch (...) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed, "OBJ face position index is not an integer"));
    }
    if (consumed != position_text.size()) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed, "OBJ face position index is not an integer"));
    }
    if (raw == 0) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed, "OBJ indices are one-based and cannot be zero"));
    }
    const long long resolved = raw > 0 ? raw - 1 : static_cast<long long>(count) + raw;
    if (resolved < 0 || static_cast<std::size_t>(resolved) >= count) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed, "OBJ face position index is out of range"));
    }
    return core::Result<std::size_t>::success(static_cast<std::size_t>(resolved));
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
    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open OBJ temporary output"));
    }
    output << "# Cartographer OBJ export; polygon topology is triangulated for interchange\n";
    output << std::setprecision(17);
    for (const auto& position : compiled.value().positions) {
        output << "v " << position.x << ' ' << position.y << ' ' << position.z << '\n';
    }
    for (std::size_t index = 0; index < compiled.value().indices.size(); index += 3U) {
        output << "f " << compiled.value().indices[index] + 1U << ' '
               << compiled.value().indices[index + 1U] + 1U << ' '
               << compiled.value().indices[index + 2U] + 1U << '\n';
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
    return core::Result<IoReport>::success(IoReport{
        compiled.value().positions.size(),
        mesh.face_count(),
        compiled.value().indices.size() / 3U,
        {"OBJ export preserves positions and triangulated faces only; materials, UVs, and custom normals are not included"},
    });
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
    std::vector<geometry::Face> faces;
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
        if (tag.empty() || tag[0] == '#') {
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
        if (tag == "f") {
            if (faces.size() >= kMaxObjFaces) {
                return core::Result<ImportResult>::failure(
                    validation("OBJ face count exceeds the Cartographer import limit"));
            }
            std::vector<geometry::VertexId> face_vertices;
            std::string token;
            std::size_t token_count = 0;
            while (fields >> token) {
                if (token_count >= kMaxObjFaceVertices) {
                    return core::Result<ImportResult>::failure(
                        validation("OBJ face vertex count exceeds the Cartographer import limit"));
                }
                auto parsed = parse_index(token, parsed_vertices.size());
                if (!parsed) {
                    return core::Result<ImportResult>::failure(parsed.error().with_context("OBJ line " + std::to_string(line_number)));
                }
                face_vertices.push_back(geometry::VertexId{static_cast<std::uint64_t>(parsed.value()) + 1U});
                ++token_count;
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
    if (auto repaired = isolate_conflicting_directed_edges(parsed_vertices, faces, result.report);
        !repaired) {
        return core::Result<ImportResult>::failure(repaired.error().with_context("OBJ topology repair"));
    }
    if (auto inserted = result.mesh.insert_bulk(std::move(parsed_vertices), std::move(faces)); !inserted) {
        return core::Result<ImportResult>::failure(inserted.error().with_context("bulk OBJ topology admission"));
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
