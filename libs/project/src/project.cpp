#include <carto/project/project.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#endif

namespace carto::project {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic parse_error(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

core::Result<void> require_line(std::istream& input, std::string_view expected) {
    std::string token;
    if (!(input >> token) || token != expected) {
        return core::Result<void>::failure(
            parse_error("project file is missing the expected " + std::string(expected) + " record"));
    }
    return core::Result<void>::success();
}

core::Result<std::uint64_t> read_uint(std::istream& input, std::string_view field) {
    std::uint64_t value = 0;
    if (!(input >> value)) {
        return core::Result<std::uint64_t>::failure(
            parse_error("project field is not a valid unsigned integer: " + std::string(field)));
    }
    return core::Result<std::uint64_t>::success(value);
}

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
            Diagnostic(ErrorCode::io_error, "atomic project replacement failed on Windows"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error, "atomic project replacement failed: " + error.message()));
    }
    return core::Result<void>::success();
#endif
}

} // namespace

core::Result<void> AssetReference::validate() const {
    if (relative_path.empty()) {
        return core::Result<void>::failure(invalid("asset reference must not be empty"));
    }
    const std::filesystem::path path(relative_path);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_argument, "asset reference must be project-relative"));
    }
    for (const auto& part : path) {
        if (part == "..") {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::invalid_argument, "asset reference may not traverse the project root"));
        }
    }
    return core::Result<void>::success();
}

core::Result<ProjectDocument> ProjectDocument::create(std::string name) {
    ProjectDocument document;
    if (auto result = document.set_name(std::move(name)); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    return core::Result<ProjectDocument>::success(std::move(document));
}

core::Result<void> ProjectDocument::set_name(std::string name) {
    if (name.empty()) {
        return core::Result<void>::failure(invalid("project name must not be empty"));
    }
    name_ = std::move(name);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<scene::ObjectId> ProjectDocument::create_object(
    std::string name,
    core::Transform transform) {
    auto result = scene_.create_object(std::move(name), transform);
    if (!result) {
        return result;
    }
    bump_revision();
    return result;
}

core::Result<void> ProjectDocument::insert_object(scene::SceneObject object) {
    if (object.mesh_asset.has_value() && !meshes_.contains(*object.mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot insert an object with a missing mesh asset"));
    }
    if (auto result = scene_.insert_object(std::move(object)); !result) {
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::remove_object(scene::ObjectId object) {
    if (auto result = scene_.remove_object(object); !result) {
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<std::uint64_t> ProjectDocument::add_mesh(geometry::EditableMesh mesh) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<std::uint64_t>::failure(result.error());
    }
    if (next_mesh_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::uint64_t>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh asset id space is exhausted"));
    }
    const std::uint64_t id = next_mesh_id_++;
    meshes_.emplace(id, std::move(mesh));
    bump_revision();
    return core::Result<std::uint64_t>::success(id);
}

core::Result<void> ProjectDocument::insert_mesh(
    std::uint64_t mesh_asset,
    geometry::EditableMesh mesh) {
    if (mesh_asset == 0) {
        return core::Result<void>::failure(invalid("mesh asset id must be non-zero"));
    }
    if (meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "duplicate mesh asset id"));
    }
    if (auto result = mesh.validate(); !result) {
        return result;
    }
    meshes_.emplace(mesh_asset, std::move(mesh));
    if (mesh_asset < std::numeric_limits<std::uint64_t>::max()) {
        next_mesh_id_ = std::max(next_mesh_id_, mesh_asset + 1U);
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::remove_mesh(std::uint64_t mesh_asset) {
    if (!meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot remove a missing mesh asset"));
    }
    for (const auto& object : scene_.objects_sorted()) {
        if (object.mesh_asset.has_value() && *object.mesh_asset == mesh_asset) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "cannot remove a mesh asset still referenced by a scene object"));
        }
    }
    meshes_.erase(mesh_asset);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::attach_mesh(scene::ObjectId object, std::uint64_t mesh_asset) {
    if (mesh_asset == 0 || !meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot attach a missing mesh asset"));
    }
    auto result = scene_.attach_mesh(object, mesh_asset);
    if (!result) {
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::validate() const {
    if (name_.empty()) {
        return core::Result<void>::failure(parse_error("project name is empty"));
    }
    if (auto result = scene_.validate(); !result) {
        return result;
    }
    for (const auto& [mesh_id, mesh] : meshes_) {
        if (mesh_id == 0) {
            return core::Result<void>::failure(parse_error("project contains a zero mesh asset id"));
        }
        if (auto result = mesh.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("mesh asset"));
        }
    }
    for (const auto& object : scene_.objects_sorted()) {
        if (object.mesh_asset.has_value() && !meshes_.contains(*object.mesh_asset)) {
            return core::Result<void>::failure(
                parse_error("scene object references a missing mesh asset"));
        }
    }
    return core::Result<void>::success();
}

std::string ProjectDocument::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "NAME " << std::quoted(name_) << '\n';
    output << "REVISION " << revision_.value() << '\n';

    const auto objects = scene_.objects_sorted();
    output << "OBJECTS " << objects.size() << '\n';
    output << std::setprecision(17);
    for (const auto& object : objects) {
        output << "OBJECT " << object.id.value << ' '
               << (object.parent.has_value() ? object.parent->value : 0U) << ' '
               << std::quoted(object.name) << ' '
               << object.local_transform.translation.x << ' '
               << object.local_transform.translation.y << ' '
               << object.local_transform.translation.z << ' '
               << object.local_transform.rotation.x << ' '
               << object.local_transform.rotation.y << ' '
               << object.local_transform.rotation.z << ' '
               << object.local_transform.rotation.w << ' '
               << object.local_transform.scale.x << ' '
               << object.local_transform.scale.y << ' '
               << object.local_transform.scale.z << ' '
               << (object.visible ? 1 : 0) << ' '
               << (object.locked ? 1 : 0) << ' '
               << (object.mesh_asset.has_value() ? object.mesh_asset.value() : 0U) << '\n';
    }

    output << "MESHES " << meshes_.size() << '\n';
    for (const auto& [mesh_id, mesh] : meshes_) {
        output << "MESH " << mesh_id << ' ' << mesh.revision().value() << '\n';
        const auto vertices = mesh.vertices_sorted();
        output << "VERTICES " << vertices.size() << '\n';
        for (const auto& vertex : vertices) {
            output << "VERTEX " << vertex.id.value << ' ' << vertex.position.x << ' '
                   << vertex.position.y << ' ' << vertex.position.z << '\n';
        }
        const auto faces = mesh.faces_sorted();
        output << "FACES " << faces.size() << '\n';
        for (const auto& face : faces) {
            output << "FACE " << face.id.value << ' ' << face.vertices.size();
            for (const auto vertex : face.vertices) {
                output << ' ' << vertex.value;
            }
            output << '\n';
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<ProjectDocument> ProjectDocument::deserialize(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0;
    if (!(input >> magic >> version) || magic != kMagic) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project magic is missing or invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<ProjectDocument>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported Cartographer project schema version"));
    }

    if (auto result = require_line(input, "NAME"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    ProjectDocument document;
    if (!(input >> std::quoted(document.name_)) || document.name_.empty()) {
        return core::Result<ProjectDocument>::failure(parse_error("project name is invalid"));
    }
    if (auto result = require_line(input, "REVISION"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    auto revision = read_uint(input, "revision");
    if (!revision) {
        return core::Result<ProjectDocument>::failure(revision.error());
    }
    document.revision_ = core::Revision(revision.value());

    if (auto result = require_line(input, "OBJECTS"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    auto object_count = read_uint(input, "object count");
    if (!object_count) {
        return core::Result<ProjectDocument>::failure(object_count.error());
    }
    for (std::uint64_t index = 0; index < object_count.value(); ++index) {
        if (auto result = require_line(input, "OBJECT"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        scene::SceneObject object;
        std::uint64_t parent = 0;
        int visible = 0;
        int locked = 0;
        std::uint64_t mesh_asset = 0;
        if (!(input >> object.id.value >> parent >> std::quoted(object.name) >>
              object.local_transform.translation.x >> object.local_transform.translation.y >>
              object.local_transform.translation.z >> object.local_transform.rotation.x >>
              object.local_transform.rotation.y >> object.local_transform.rotation.z >>
              object.local_transform.rotation.w >> object.local_transform.scale.x >>
              object.local_transform.scale.y >> object.local_transform.scale.z >> visible >> locked >>
              mesh_asset)) {
            return core::Result<ProjectDocument>::failure(parse_error("invalid scene object record"));
        }
        if (visible != 0 && visible != 1) {
            return core::Result<ProjectDocument>::failure(
                parse_error("scene object visibility flag must be 0 or 1"));
        }
        if (locked != 0 && locked != 1) {
            return core::Result<ProjectDocument>::failure(
                parse_error("scene object lock flag must be 0 or 1"));
        }
        if (parent != 0) {
            object.parent = scene::ObjectId{parent};
        }
        object.visible = visible != 0;
        object.locked = locked != 0;
        if (mesh_asset != 0) {
            object.mesh_asset = mesh_asset;
        }
        if (auto result = document.scene_.insert_object(std::move(object)); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
    }

    if (auto result = require_line(input, "MESHES"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    auto mesh_count = read_uint(input, "mesh count");
    if (!mesh_count) {
        return core::Result<ProjectDocument>::failure(mesh_count.error());
    }
    for (std::uint64_t mesh_index = 0; mesh_index < mesh_count.value(); ++mesh_index) {
        if (auto result = require_line(input, "MESH"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t mesh_id = 0;
        std::uint64_t ignored_mesh_revision = 0;
        if (!(input >> mesh_id >> ignored_mesh_revision) || mesh_id == 0) {
            return core::Result<ProjectDocument>::failure(parse_error("invalid mesh record"));
        }
        geometry::EditableMesh mesh;
        if (auto result = require_line(input, "VERTICES"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto vertex_count = read_uint(input, "vertex count");
        if (!vertex_count) {
            return core::Result<ProjectDocument>::failure(vertex_count.error());
        }
        std::map<std::uint64_t, geometry::VertexId> vertex_ids;
        for (std::uint64_t vertex_index = 0; vertex_index < vertex_count.value(); ++vertex_index) {
            if (auto result = require_line(input, "VERTEX"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::uint64_t serialized_id = 0;
            core::Vec3d position;
            if (!(input >> serialized_id >> position.x >> position.y >> position.z) ||
                serialized_id == 0 || !position.finite()) {
                return core::Result<ProjectDocument>::failure(parse_error("invalid mesh vertex record"));
            }
            const geometry::VertexId vertex_id{serialized_id};
            if (auto added = mesh.insert_vertex(geometry::Vertex{vertex_id, position}); !added) {
                return core::Result<ProjectDocument>::failure(added.error());
            }
            vertex_ids.emplace(serialized_id, vertex_id);
        }
        if (auto result = require_line(input, "FACES"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto face_count = read_uint(input, "face count");
        if (!face_count) {
            return core::Result<ProjectDocument>::failure(face_count.error());
        }
        for (std::uint64_t face_index = 0; face_index < face_count.value(); ++face_index) {
            if (auto result = require_line(input, "FACE"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::uint64_t serialized_face_id = 0;
            std::uint64_t face_vertex_count = 0;
            if (!(input >> serialized_face_id >> face_vertex_count) || serialized_face_id == 0 ||
                face_vertex_count > 1'000'000U) {
                return core::Result<ProjectDocument>::failure(parse_error("invalid mesh face record"));
            }
            std::vector<geometry::VertexId> face_vertices;
            face_vertices.reserve(static_cast<std::size_t>(face_vertex_count));
            for (std::uint64_t vertex_index = 0; vertex_index < face_vertex_count; ++vertex_index) {
                std::uint64_t serialized_vertex = 0;
                if (!(input >> serialized_vertex) || !vertex_ids.contains(serialized_vertex)) {
                    return core::Result<ProjectDocument>::failure(
                        parse_error("mesh face references an unknown serialized vertex"));
                }
                face_vertices.push_back(vertex_ids.at(serialized_vertex));
            }
            const geometry::FaceId face_id{serialized_face_id};
            if (auto added = mesh.insert_face(geometry::Face{face_id, std::move(face_vertices)});
                !added) {
                return core::Result<ProjectDocument>::failure(
                    added.error().with_context("serialized face " + std::to_string(serialized_face_id)));
            }
        }
        mesh.restore_revision(core::Revision(ignored_mesh_revision));
        document.meshes_.emplace(mesh_id, std::move(mesh));
        if (mesh_id < std::numeric_limits<std::uint64_t>::max()) {
            document.next_mesh_id_ = std::max(document.next_mesh_id_, mesh_id + 1U);
        }
    }

    if (auto result = require_line(input, "END"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    if (auto result = document.validate(); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    return core::Result<ProjectDocument>::success(std::move(document));
}

core::Result<ProjectDocument> ProjectDocument::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<ProjectDocument>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open project file for reading"));
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return core::Result<ProjectDocument>::failure(
            Diagnostic(ErrorCode::io_error, "failed while reading project file"));
    }
    return deserialize(contents.str());
}

core::Result<SaveReceipt> ProjectDocument::save_atomic(const std::filesystem::path& path) const {
    if (path.empty() || path.filename().empty()) {
        return core::Result<SaveReceipt>::failure(invalid("project save path must name a file"));
    }
    if (auto result = validate(); !result) {
        return core::Result<SaveReceipt>::failure(result.error());
    }
    const auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        return core::Result<SaveReceipt>::failure(
            Diagnostic(ErrorCode::io_error, "project save parent directory does not exist"));
    }

    const std::string serialized = serialize();
    if (auto result = deserialize(serialized); !result) {
        return core::Result<SaveReceipt>::failure(
            result.error().with_context("serialized project failed pre-commit validation"));
    }

    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<SaveReceipt>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open project temporary file"));
    }
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<SaveReceipt>::failure(
            Diagnostic(ErrorCode::io_error, "failed while writing project temporary file"));
    }
    output.close();

    auto replacement = atomic_replace(temporary, path);
    if (!replacement) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<SaveReceipt>::failure(replacement.error());
    }
    return core::Result<SaveReceipt>::success(
        SaveReceipt{path, static_cast<std::uint64_t>(serialized.size()), kSchemaVersion, revision_});
}

} // namespace carto::project
