#include <carto/io/obj.hpp>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::io {

namespace {

using core::Diagnostic;
using core::ErrorCode;

core::Result<std::size_t> parse_index(std::string_view token, std::size_t count) {
    const std::size_t slash = token.find('/');
    const std::string_view position = token.substr(0, slash);
    if (position.empty()) {
        return core::Result<std::size_t>::failure(
            Diagnostic(ErrorCode::validation_failed, "OBJ face token has no position index"));
    }
    long long raw = 0;
    try {
        raw = std::stoll(std::string(position));
    } catch (...) {
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
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open OBJ output"));
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
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "failed while writing OBJ output"));
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
    ImportResult result;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream fields(line);
        std::string tag;
        fields >> tag;
        if (tag.empty() || tag[0] == '#') {
            continue;
        }
        if (tag == "v") {
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
            std::vector<geometry::VertexId> vertices;
            std::string token;
            while (fields >> token) {
                auto parsed = parse_index(token, result.mesh.vertex_count());
                if (!parsed) {
                    return core::Result<ImportResult>::failure(parsed.error().with_context("OBJ line " + std::to_string(line_number)));
                }
                const auto existing = result.mesh.vertices_sorted();
                vertices.push_back(existing.at(parsed.value()).id);
            }
            auto face = result.mesh.add_face(std::move(vertices));
            if (!face) {
                return core::Result<ImportResult>::failure(face.error().with_context("OBJ line " + std::to_string(line_number)));
            }
            continue;
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
