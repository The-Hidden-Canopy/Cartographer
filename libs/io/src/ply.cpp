#include <carto/io/ply.hpp>

#include <atomic>
#include <charconv>
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
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::size_t>::failure(
            validation("PLY " + std::string(field) + " is not a valid non-negative count"));
    }
    return core::Result<std::size_t>::success(static_cast<std::size_t>(value));
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
    std::string type;
    std::string count_type;
    std::string name;
};

std::string ply_header_text(std::size_t vertex_count, std::size_t face_count, PlyFormat format) {
    std::ostringstream text;
    text << "ply\nformat "
         << (format == PlyFormat::binary_little_endian ? "binary_little_endian" : "ascii")
         << " 1.0\ncomment Cartographer polygon authoring export\n"
         << "element vertex " << vertex_count << "\n"
         << "property double x\nproperty double y\nproperty double z\n"
         << "element face " << face_count << "\n"
         << "property list uchar uint vertex_indices\nend_header\n";
    return text.str();
}

void append_u32_le(std::string& out, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
        out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void append_u64_le(std::string& out, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U)
        out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void append_f64_le(std::string& out, double value) {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64_le(out, bits);
}

std::size_t scalar_type_size(std::string_view type) {
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") return 1U;
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") return 2U;
    if (type == "int" || type == "uint" || type == "int32" || type == "uint32" ||
        type == "float" || type == "float32") return 4U;
    if (type == "double" || type == "float64") return 8U;
    return 0U;
}

std::uint32_t decode_uint_le(const unsigned char* bytes, std::size_t size) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < size; ++index)
        value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
    return value;
}

double decode_f64_le(const unsigned char* bytes) {
    std::uint64_t bits = 0U;
    for (std::size_t index = 0U; index < 8U; ++index)
        bits |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

core::Result<PlyImportResult> read_ply_binary(
    std::istream& input,
    std::uintmax_t file_size,
    std::uintmax_t payload_offset,
    std::size_t vertex_count,
    std::size_t face_count,
    const std::vector<Property>& vertex_properties,
    const std::vector<Property>& face_properties) {
    std::size_t vertex_stride = 0U;
    std::optional<std::size_t> x_offset;
    std::optional<std::size_t> y_offset;
    std::optional<std::size_t> z_offset;
    for (const auto& property : vertex_properties) {
        if (property.list) return core::Result<PlyImportResult>::failure(
            validation("PLY vertex list properties are unsupported"));
        const auto size = scalar_type_size(property.type);
        if (size == 0U) return core::Result<PlyImportResult>::failure(
            validation("PLY property type '" + property.type + "' is unsupported"));
        if (property.name == "x" && !x_offset.has_value()) {
            if (property.type != "double") return core::Result<PlyImportResult>::failure(
                validation("PLY vertex x/y/z properties must be double"));
            x_offset = vertex_stride;
        } else if (property.name == "y" && !y_offset.has_value()) {
            if (property.type != "double") return core::Result<PlyImportResult>::failure(
                validation("PLY vertex x/y/z properties must be double"));
            y_offset = vertex_stride;
        } else if (property.name == "z" && !z_offset.has_value()) {
            if (property.type != "double") return core::Result<PlyImportResult>::failure(
                validation("PLY vertex x/y/z properties must be double"));
            z_offset = vertex_stride;
        }
        vertex_stride += size;
    }
    if (!x_offset.has_value() || !y_offset.has_value() || !z_offset.has_value()) {
        return core::Result<PlyImportResult>::failure(validation("PLY vertex x/y/z properties are required"));
    }
    std::optional<std::size_t> face_list_index;
    for (std::size_t index = 0; index < face_properties.size(); ++index) {
        if (!face_properties[index].list) {
            if (scalar_type_size(face_properties[index].type) == 0U) {
                return core::Result<PlyImportResult>::failure(
                    validation("PLY property type '" + face_properties[index].type + "' is unsupported"));
            }
            continue;
        }
        if (face_list_index.has_value()) return core::Result<PlyImportResult>::failure(
            validation("PLY supports only one face list property"));
        face_list_index = index;
    }
    if (!face_list_index.has_value()) return core::Result<PlyImportResult>::failure(
        validation("PLY face vertex_indices list property is required"));
    const auto list_count_size = scalar_type_size(face_properties[*face_list_index].count_type);
    const auto list_value_size = scalar_type_size(face_properties[*face_list_index].type);
    if (list_count_size == 0U || list_count_size > 4U || list_value_size != 4U) {
        return core::Result<PlyImportResult>::failure(
            validation("PLY face vertex_indices list type is unsupported"));
    }
    const auto remaining = file_size - payload_offset;
    if (static_cast<std::uintmax_t>(vertex_count) > remaining / vertex_stride) {
        return core::Result<PlyImportResult>::failure(
            validation("PLY ended before all vertex records were read"));
    }

    PlyImportResult result;
    result.mesh = {};
    std::vector<geometry::VertexId> vertex_ids;
    vertex_ids.reserve(vertex_count);
    std::vector<unsigned char> record(vertex_stride);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        input.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(vertex_stride));
        if (input.gcount() != static_cast<std::streamsize>(vertex_stride)) {
            return core::Result<PlyImportResult>::failure(
                validation("PLY ended before all vertex records were read"));
        }
        auto added = result.mesh.add_vertex({
            decode_f64_le(record.data() + *x_offset),
            decode_f64_le(record.data() + *y_offset),
            decode_f64_le(record.data() + *z_offset)});
        if (!added) return core::Result<PlyImportResult>::failure(added.error());
        vertex_ids.push_back(added.value());
    }
    std::vector<unsigned char> index_bytes;
    for (std::size_t face = 0; face < face_count; ++face) {
        std::vector<std::size_t> indices;
        for (std::size_t property = 0; property < face_properties.size(); ++property) {
            if (property == *face_list_index) {
                input.read(reinterpret_cast<char*>(record.data()),
                    static_cast<std::streamsize>(list_count_size));
                if (input.gcount() != static_cast<std::streamsize>(list_count_size)) {
                    return core::Result<PlyImportResult>::failure(
                        validation("PLY ended before all face records were read"));
                }
                const auto count = decode_uint_le(record.data(), list_count_size);
                if (count < 3U || count > kMaxPlyFaceVertices) {
                    return core::Result<PlyImportResult>::failure(
                        validation("PLY face vertex list is invalid"));
                }
                index_bytes.resize(static_cast<std::size_t>(count) * list_value_size);
                input.read(reinterpret_cast<char*>(index_bytes.data()),
                    static_cast<std::streamsize>(index_bytes.size()));
                if (input.gcount() != static_cast<std::streamsize>(index_bytes.size())) {
                    return core::Result<PlyImportResult>::failure(
                        validation("PLY ended before all face records were read"));
                }
                indices.reserve(count);
                for (std::size_t index = 0; index < count; ++index) {
                    const auto parsed = decode_uint_le(
                        index_bytes.data() + index * list_value_size, list_value_size);
                    if (parsed >= vertex_ids.size()) return core::Result<PlyImportResult>::failure(
                        validation("PLY face vertex index is out of range"));
                    indices.push_back(parsed);
                }
            } else {
                const auto size = scalar_type_size(face_properties[property].type);
                input.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(size));
                if (input.gcount() != static_cast<std::streamsize>(size)) {
                    return core::Result<PlyImportResult>::failure(
                        validation("PLY ended before all face records were read"));
                }
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

} // namespace

core::Result<IoReport> export_ply(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path,
    PlyFormat format) {
    std::lock_guard lock(g_ply_export_mutex);
    if (auto result = validate_output_path(path, "PLY"); !result) return core::Result<IoReport>::failure(result.error());
    if (auto result = mesh.validate(); !result) return core::Result<IoReport>::failure(result.error());
    const auto vertices = mesh.vertices_sorted();
    const auto faces = mesh.faces_sorted();
    if (vertices.size() > kMaxPlyVertices || faces.size() > kMaxPlyFaces) {
        return core::Result<IoReport>::failure(validation("PLY export exceeds the Cartographer record limit"));
    }
    if (format == PlyFormat::binary_little_endian) {
        for (const auto& face : faces) {
            if (face.vertices.size() > 255U) {
                return core::Result<IoReport>::failure(
                    validation("PLY binary export supports at most 255 vertices per face"));
            }
        }
    }

    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<IoReport>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open PLY temporary output"));
    }
    std::map<geometry::VertexId, std::size_t> indices;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        indices.emplace(vertices[index].id, index);
    }
    output << ply_header_text(vertices.size(), faces.size(), format);
    if (format == PlyFormat::binary_little_endian) {
        std::string payload;
        for (const auto& vertex : vertices) {
            append_f64_le(payload, vertex.position.x);
            append_f64_le(payload, vertex.position.y);
            append_f64_le(payload, vertex.position.z);
        }
        for (const auto& face : faces) {
            payload.push_back(static_cast<char>(face.vertices.size()));
            for (const auto vertex : face.vertices) {
                append_u32_le(payload, static_cast<std::uint32_t>(indices.at(vertex)));
            }
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    } else {
        output << std::setprecision(17);
        for (const auto& vertex : vertices) {
            output << vertex.position.x << ' ' << vertex.position.y << ' '
                   << vertex.position.z << '\n';
        }
        for (const auto& face : faces) {
            output << face.vertices.size();
            for (const auto vertex : face.vertices) output << ' ' << indices.at(vertex);
            output << '\n';
        }
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
    bool saw_binary_little_endian = false;
    bool saw_header_end = false;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string tag;
        fields >> tag;
        if (tag.empty() || tag == "comment") continue;
        if (tag == "format") {
            std::string format;
            std::string version;
            fields >> format >> version;
            if (format == "binary_big_endian") return core::Result<PlyImportResult>::failure(
                validation("big-endian PLY is not supported"));
            if (format != "ascii" && format != "binary_little_endian") {
                return core::Result<PlyImportResult>::failure(
                    validation("PLY format '" + format + "' is unsupported"));
            }
            if (version != "1.0") return core::Result<PlyImportResult>::failure(
                validation("PLY format version is unsupported"));
            if (format == "ascii") saw_ascii = true;
            else saw_binary_little_endian = true;
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
                fields >> property.count_type >> property.type >> property.name;
                property.list = true;
            } else {
                property.type = type;
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
    if ((!saw_ascii && !saw_binary_little_endian) || !saw_header_end || vertex_count == 0U || face_count == 0U ||
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
    if (saw_binary_little_endian) {
        const auto position = static_cast<std::streamoff>(input.tellg());
        const auto payload_offset = position >= 0 ? static_cast<std::uintmax_t>(position) : file_size;
        return read_ply_binary(
            input, file_size, payload_offset, vertex_count, face_count,
            vertex_properties, face_properties);
    }

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
