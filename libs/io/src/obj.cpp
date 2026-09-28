#include <carto/io/obj.hpp>

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <string_view>
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

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    return target.parent_path() / (target.filename().string() + ".carto.tmp");
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

} // namespace

core::Result<IoReport> export_obj(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path) {
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
    if (!size_error && file_size > kMaxObjBytes) {
        return core::Result<ImportResult>::failure(
            validation("OBJ input exceeds the Cartographer import limit of 128 MiB"));
    }
    ImportResult result;
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
            if (result.mesh.vertex_count() >= kMaxObjVertices) {
                return core::Result<ImportResult>::failure(
                    validation("OBJ vertex count exceeds the Cartographer import limit"));
            }
            core::Vec3d position;
            if (!(fields >> position.x >> position.y >> position.z) || !position.finite()) {
                return core::Result<ImportResult>::failure(
                    Diagnostic(ErrorCode::validation_failed, "invalid OBJ vertex at line " + std::to_string(line_number)));
            }
            if (auto vertex = result.mesh.add_vertex(position); !vertex) {
                return core::Result<ImportResult>::failure(vertex.error().with_context("OBJ line " + std::to_string(line_number)));
            }
            continue;
        }
        if (tag == "f") {
            if (result.mesh.face_count() >= kMaxObjFaces) {
                return core::Result<ImportResult>::failure(
                    validation("OBJ face count exceeds the Cartographer import limit"));
            }
            const auto existing = result.mesh.vertices_sorted();
            std::vector<geometry::VertexId> vertices;
            std::string token;
            std::size_t token_count = 0;
            while (fields >> token) {
                if (token_count >= kMaxObjFaceVertices) {
                    return core::Result<ImportResult>::failure(
                        validation("OBJ face vertex count exceeds the Cartographer import limit"));
                }
                auto parsed = parse_index(token, result.mesh.vertex_count());
                if (!parsed) {
                    return core::Result<ImportResult>::failure(parsed.error().with_context("OBJ line " + std::to_string(line_number)));
                }
                vertices.push_back(existing.at(parsed.value()).id);
                ++token_count;
            }
            auto face = result.mesh.add_face(std::move(vertices));
            if (!face) {
                return core::Result<ImportResult>::failure(face.error().with_context("OBJ line " + std::to_string(line_number)));
            }
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
