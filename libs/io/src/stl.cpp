#include <carto/io/stl.hpp>

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
#include <stdexcept>
#include <sstream>
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

constexpr std::uintmax_t kMaxStlBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxStlTriangles = 1'000'000U;
std::mutex g_stl_export_mutex;
std::atomic<std::uint64_t> g_stl_temp_counter{0U};

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_stl_temp_counter.fetch_add(1U, std::memory_order_relaxed);
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
            Diagnostic(ErrorCode::io_error, "atomic STL replacement failed on Windows"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) return core::Result<void>::failure(
        Diagnostic(ErrorCode::io_error, "atomic STL replacement failed: " + error.message()));
    return core::Result<void>::success();
#endif
}

core::Result<void> validate_output_path(const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) return core::Result<void>::failure(
        Diagnostic(ErrorCode::invalid_argument, "STL export path must name a file"));
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        if (!std::filesystem::exists(parent, error) || error ||
            !std::filesystem::is_directory(parent, error) || error) return core::Result<void>::failure(
                Diagnostic(ErrorCode::io_error, "STL export parent directory is unavailable"));
    }
    return core::Result<void>::success();
}

void put_u32(std::array<char, 4>& bytes, std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        bytes[shift / 8U] = static_cast<char>((value >> shift) & 0xffU);
    }
}

std::uint32_t get_u32(const char* bytes) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[shift / 8U])) << shift;
    }
    return value;
}

float get_f32(const char* bytes) noexcept {
    float value = 0.0F;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

core::Vec3d read_vertex(const char* bytes) noexcept {
    return {static_cast<double>(get_f32(bytes)), static_cast<double>(get_f32(bytes + 4)),
            static_cast<double>(get_f32(bytes + 8))};
}

core::Vec3d triangle_normal(core::Vec3d a, core::Vec3d b, core::Vec3d c) noexcept {
    return core::cross(b - a, c - a).normalized();
}

} // namespace

core::Result<IoReport> export_stl(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path) {
    std::lock_guard lock(g_stl_export_mutex);
    if (auto result = validate_output_path(path); !result) return core::Result<IoReport>::failure(result.error());
    auto compiled = mesh.compile();
    if (!compiled) return core::Result<IoReport>::failure(compiled.error());
    const auto& positions = compiled.value().positions;
    const auto triangle_count = compiled.value().indices.size() / 3U;
    if (triangle_count > std::numeric_limits<std::uint32_t>::max() || triangle_count > kMaxStlTriangles) {
        return core::Result<IoReport>::failure(validation("STL export exceeds the Cartographer triangle limit"));
    }
    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return core::Result<IoReport>::failure(
        Diagnostic(ErrorCode::io_error, "unable to open STL temporary output"));
    std::array<char, 80> header{};
    const std::string label = "Cartographer binary STL export";
    std::copy(label.begin(), label.end(), header.begin());
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    std::array<char, 4> count_bytes{};
    put_u32(count_bytes, static_cast<std::uint32_t>(triangle_count));
    output.write(count_bytes.data(), static_cast<std::streamsize>(count_bytes.size()));
    for (std::size_t triangle = 0; triangle < triangle_count; ++triangle) {
        const auto ia = compiled.value().indices[triangle * 3U];
        const auto ib = compiled.value().indices[triangle * 3U + 1U];
        const auto ic = compiled.value().indices[triangle * 3U + 2U];
        const core::Vec3d normal = triangle_normal(positions[ia], positions[ib], positions[ic]);
        std::array<float, 12> values{
            static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z),
            static_cast<float>(positions[ia].x), static_cast<float>(positions[ia].y), static_cast<float>(positions[ia].z),
            static_cast<float>(positions[ib].x), static_cast<float>(positions[ib].y), static_cast<float>(positions[ib].z),
            static_cast<float>(positions[ic].x), static_cast<float>(positions[ic].y), static_cast<float>(positions[ic].z),
        };
        for (const float value : values) {
            if (!std::isfinite(value)) {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return core::Result<IoReport>::failure(validation("STL export cannot represent a value as float32"));
            }
        }
        output.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(float)));
        const std::array<char, 2> attribute{0, 0};
        output.write(attribute.data(), static_cast<std::streamsize>(attribute.size()));
    }
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(Diagnostic(ErrorCode::io_error, "failed while writing STL temporary output"));
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(Diagnostic(ErrorCode::io_error, "failed while closing STL temporary output"));
    }
    auto replacement = atomic_replace(temporary, path);
    if (!replacement) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<IoReport>::failure(replacement.error());
    }
    return core::Result<IoReport>::success(IoReport{
        positions.size(), triangle_count, triangle_count,
        {"STL is triangle-only and does not preserve names, polygon topology, UVs, materials, or tangents"},
    });
}

core::Result<StlImportResult> import_stl(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return core::Result<StlImportResult>::failure(
        Diagnostic(ErrorCode::io_error, "unable to open STL input"));
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) return core::Result<StlImportResult>::failure(
        Diagnostic(ErrorCode::io_error, "unable to inspect STL input size"));
    if (file_size > kMaxStlBytes) return core::Result<StlImportResult>::failure(
        validation("STL input exceeds the Cartographer import limit of 128 MiB"));
    std::vector<char> bytes(static_cast<std::size_t>(file_size));
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) return core::Result<StlImportResult>::failure(
        Diagnostic(ErrorCode::io_error, "failed while reading STL input"));

    StlImportResult result;
    const auto add_triangle = [&result](const std::array<core::Vec3d, 3>& points) -> core::Result<void> {
        std::vector<geometry::VertexId> ids;
        ids.reserve(3U);
        for (const auto point : points) {
            auto vertex = result.mesh.add_vertex(point);
            if (!vertex) return core::Result<void>::failure(vertex.error());
            ids.push_back(vertex.value());
        }
        auto face = result.mesh.add_face(std::move(ids));
        if (!face) return core::Result<void>::failure(face.error());
        return core::Result<void>::success();
    };

    const std::size_t prefix_size = std::min<std::size_t>(bytes.size(), 256U);
    std::string prefix(bytes.data(), prefix_size);
    const bool ascii_hint = prefix.rfind("solid", 0) == 0;
    if (ascii_hint) {
        std::istringstream text(std::string(bytes.begin(), bytes.end()));
        std::string line;
        std::array<core::Vec3d, 3> points{};
        std::size_t point_count = 0;
        while (std::getline(text, line)) {
            std::istringstream fields(line);
            std::string tag;
            fields >> tag;
            if (tag != "vertex") continue;
            core::Vec3d point;
            if (!(fields >> point.x >> point.y >> point.z) || !point.finite()) {
                return core::Result<StlImportResult>::failure(validation("ASCII STL vertex is invalid"));
            }
            points[point_count++] = point;
            if (point_count == 3U) {
                if (result.mesh.face_count() >= kMaxStlTriangles) return core::Result<StlImportResult>::failure(
                    validation("STL triangle count exceeds the Cartographer import limit"));
                if (auto added = add_triangle(points); !added) return core::Result<StlImportResult>::failure(added.error());
                point_count = 0;
            }
        }
        if (point_count != 0U || result.mesh.face_count() != 0U) {
            if (point_count != 0U) return core::Result<StlImportResult>::failure(
                validation("ASCII STL ended with an incomplete facet"));
            auto compiled = result.mesh.compile();
            if (!compiled) return core::Result<StlImportResult>::failure(compiled.error());
            result.report = IoReport{
                result.mesh.vertex_count(), result.mesh.face_count(), compiled.value().indices.size() / 3U,
                {"ASCII STL facet normals were ignored; normals are recomputed from topology"},
            };
            return core::Result<StlImportResult>::success(std::move(result));
        }
    }

    if (bytes.size() < 84U) return core::Result<StlImportResult>::failure(validation("STL binary header is truncated"));
    const std::uint32_t triangle_count = get_u32(bytes.data() + 80U);
    if (triangle_count == 0U || triangle_count > kMaxStlTriangles ||
        bytes.size() != 84ULL + static_cast<std::uint64_t>(triangle_count) * 50ULL) {
        return core::Result<StlImportResult>::failure(validation("STL binary size does not match its triangle count"));
    }
    std::map<std::tuple<float, float, float>, geometry::VertexId> welded;
    const auto binary_vertex = [&result, &welded](core::Vec3d point) -> core::Result<geometry::VertexId> {
        if (!point.finite()) return core::Result<geometry::VertexId>::failure(
            validation("binary STL contains a non-finite vertex"));
        const std::tuple<float, float, float> key{
            static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)};
        const auto existing = welded.find(key);
        if (existing != welded.end()) return core::Result<geometry::VertexId>::success(existing->second);
        auto added = result.mesh.add_vertex(point);
        if (!added) return added;
        welded.emplace(key, added.value());
        return added;
    };
    for (std::uint32_t triangle = 0; triangle < triangle_count; ++triangle) {
        const char* record = bytes.data() + 84U + static_cast<std::size_t>(triangle) * 50U;
        std::vector<geometry::VertexId> ids;
        for (std::size_t vertex = 0; vertex < 3U; ++vertex) {
            auto added = binary_vertex(read_vertex(record + 12U + vertex * 12U));
            if (!added) return core::Result<StlImportResult>::failure(added.error());
            ids.push_back(added.value());
        }
        auto face = result.mesh.add_face(std::move(ids));
        if (!face) return core::Result<StlImportResult>::failure(face.error());
    }
    auto compiled = result.mesh.compile();
    if (!compiled) return core::Result<StlImportResult>::failure(compiled.error());
    result.report = IoReport{
        result.mesh.vertex_count(), result.mesh.face_count(), compiled.value().indices.size() / 3U,
        {"Binary STL facet normals were ignored; normals are recomputed from topology"},
    };
    return core::Result<StlImportResult>::success(std::move(result));
}

} // namespace carto::io
