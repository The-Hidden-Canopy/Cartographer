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

Diagnostic exhausted_revision() {
    return Diagnostic(ErrorCode::invalid_state, "project revision space is exhausted");
}

constexpr std::uint64_t kMaxSerializedObjects = 1'000'000U;
constexpr std::uint64_t kMaxSerializedMeshes = 1'000'000U;
constexpr std::uint64_t kMaxSerializedVertices = 1'000'000U;
constexpr std::uint64_t kMaxSerializedFaces = 1'000'000U;
constexpr std::uint64_t kMaxSerializedFaceVertices = 1'000'000U;
constexpr std::uintmax_t kMaxSerializedProjectBytes = 128ULL * 1024ULL * 1024ULL;

core::Result<void> validate_serialized_count(
    std::uint64_t count,
    std::uint64_t limit,
    std::string_view field) {
    if (count > limit) {
        return core::Result<void>::failure(parse_error(
            std::string(field) + " exceeds the Cartographer v1 limit of " +
            std::to_string(limit)));
    }
    return core::Result<void>::success();
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
    std::string token;
    if (!(input >> token)) {
        return core::Result<std::uint64_t>::failure(
            parse_error("project field is not a valid unsigned integer: " + std::string(field)));
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (revision_.exhausted()) {
        return core::Result<scene::ObjectId>::failure(exhausted_revision());
    }
    auto result = scene_.create_object(std::move(name), transform);
    if (!result) {
        return result;
    }
    bump_revision();
    return result;
}

core::Result<void> ProjectDocument::insert_object(scene::SceneObject object) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = scene_.remove_object(object); !result) {
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<std::uint64_t> ProjectDocument::add_mesh(geometry::EditableMesh mesh) {
    if (revision_.exhausted()) {
        return core::Result<std::uint64_t>::failure(exhausted_revision());
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<std::uint64_t>::failure(result.error());
    }
    if (mesh.revision().exhausted()) {
        return core::Result<std::uint64_t>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh asset revision space is exhausted"));
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (mesh.revision().exhausted()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh asset revision space is exhausted"));
    }
    meshes_.emplace(mesh_asset, std::move(mesh));
    if (mesh_asset < std::numeric_limits<std::uint64_t>::max()) {
        next_mesh_id_ = std::max(next_mesh_id_, mesh_asset + 1U);
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::replace_mesh(
    std::uint64_t mesh_asset,
    geometry::EditableMesh mesh) {
    const auto iterator = meshes_.find(mesh_asset);
    if (iterator == meshes_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot replace a missing mesh asset"));
    }
    auto result = replace_mesh_if_revision(mesh_asset, iterator->second.revision(), std::move(mesh));
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    return core::Result<void>::success();
}

core::Result<core::Revision> ProjectDocument::replace_mesh_if_revision(
    std::uint64_t mesh_asset,
    core::Revision expected_revision,
    geometry::EditableMesh mesh) {
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(exhausted_revision());
    }
    const auto iterator = meshes_.find(mesh_asset);
    if (iterator == meshes_.end()) {
        return core::Result<core::Revision>::failure(
            Diagnostic(ErrorCode::not_found, "cannot replace a missing mesh asset"));
    }
    if (iterator->second.revision() != expected_revision) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data,
            "mesh replacement revision does not match the current asset"));
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    if (iterator->second.revision().exhausted() || mesh.revision().exhausted()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "mesh asset revision space is exhausted"));
    }
    const core::Revision minimum_revision = iterator->second.revision().next();
    if (mesh.revision() < minimum_revision) {
        if (auto result = mesh.restore_revision(minimum_revision); !result) {
            return core::Result<core::Revision>::failure(result.error());
        }
    }
    const core::Revision applied_revision = mesh.revision();
    iterator->second = std::move(mesh);
    bump_revision();
    return core::Result<core::Revision>::success(applied_revision);
}

core::Result<void> ProjectDocument::remove_mesh(std::uint64_t mesh_asset) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
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

core::Result<void> ProjectDocument::set_object_transform(
    scene::ObjectId object,
    core::Transform transform) {
    auto result = set_object_transform_if_revision(object, revision_, transform);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    return core::Result<void>::success();
}

core::Result<core::Revision> ProjectDocument::set_object_transform_if_revision(
    scene::ObjectId object,
    core::Revision expected_revision,
    core::Transform transform) {
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(exhausted_revision());
    }
    if (revision_ != expected_revision) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data,
            "object transform revision does not match the current project"));
    }
    if (auto result = scene_.set_local_transform(object, transform); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    bump_revision();
    return core::Result<core::Revision>::success(revision_);
}

core::Result<void> ProjectDocument::validate() const {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(parse_error("project revision space is exhausted"));
    }
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
        if (mesh.revision().exhausted()) {
            return core::Result<void>::failure(parse_error("mesh revision space is exhausted"));
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
    if (text.size() > kMaxSerializedProjectBytes) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project text exceeds the Cartographer v1 size limit of 128 MiB"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project magic is missing or invalid"));
    }
    auto version = read_uint(input, "schema version");
    if (!version) {
        return core::Result<ProjectDocument>::failure(version.error());
    }
    if (version.value() != kSchemaVersion) {
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
    if (document.revision_.exhausted()) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project revision space is exhausted"));
    }

    if (auto result = require_line(input, "OBJECTS"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    auto object_count = read_uint(input, "object count");
    if (!object_count) {
        return core::Result<ProjectDocument>::failure(object_count.error());
    }
    if (auto result = validate_serialized_count(
            object_count.value(), kMaxSerializedObjects, "project object count");
        !result) {
            return core::Result<ProjectDocument>::failure(result.error());
    }
    std::vector<std::pair<scene::ObjectId, std::optional<scene::ObjectId>>> parent_links;
    parent_links.reserve(static_cast<std::size_t>(object_count.value()));
    for (std::uint64_t index = 0; index < object_count.value(); ++index) {
        if (auto result = require_line(input, "OBJECT"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        scene::SceneObject object;
        auto serialized_object_id = read_uint(input, "object id");
        auto parent_id = read_uint(input, "object parent id");
        int visible = 0;
        int locked = 0;
        if (!serialized_object_id || !parent_id || !(input >> std::quoted(object.name) >>
              object.local_transform.translation.x >> object.local_transform.translation.y >>
              object.local_transform.translation.z >> object.local_transform.rotation.x >>
              object.local_transform.rotation.y >> object.local_transform.rotation.z >>
              object.local_transform.rotation.w >> object.local_transform.scale.x >>
              object.local_transform.scale.y >> object.local_transform.scale.z >> visible >> locked)) {
            return core::Result<ProjectDocument>::failure(parse_error("invalid scene object record"));
        }
        auto mesh_asset = read_uint(input, "scene object mesh id");
        if (!mesh_asset) {
            return core::Result<ProjectDocument>::failure(mesh_asset.error());
        }
        object.id.value = serialized_object_id.value();
        if (visible != 0 && visible != 1) {
            return core::Result<ProjectDocument>::failure(
                parse_error("scene object visibility flag must be 0 or 1"));
        }
        if (locked != 0 && locked != 1) {
            return core::Result<ProjectDocument>::failure(
                parse_error("scene object lock flag must be 0 or 1"));
        }
        if (parent_id.value() != 0) {
            object.parent = scene::ObjectId{parent_id.value()};
        }
        object.visible = visible != 0;
        object.locked = locked != 0;
        if (mesh_asset.value() != 0) {
            object.mesh_asset = mesh_asset.value();
        }
        const scene::ObjectId object_id = object.id;
        const auto parent_link = object.parent;
        object.parent.reset();
        if (auto result = document.scene_.insert_object(std::move(object)); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        parent_links.emplace_back(object_id, parent_link);
    }
    for (const auto& [object, parent] : parent_links) {
        if (auto result = document.scene_.set_parent(object, parent); !result) {
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
    if (auto result = validate_serialized_count(
            mesh_count.value(), kMaxSerializedMeshes, "project mesh count");
        !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    for (std::uint64_t mesh_index = 0; mesh_index < mesh_count.value(); ++mesh_index) {
        if (auto result = require_line(input, "MESH"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto mesh_id = read_uint(input, "mesh id");
        auto ignored_mesh_revision = read_uint(input, "mesh revision");
        if (!mesh_id || !ignored_mesh_revision || mesh_id.value() == 0) {
            return core::Result<ProjectDocument>::failure(parse_error("invalid mesh record"));
        }
        if (ignored_mesh_revision.value() == core::Revision::max_value()) {
            return core::Result<ProjectDocument>::failure(
                parse_error("mesh revision space is exhausted"));
        }
        geometry::EditableMesh mesh;
        if (auto result = require_line(input, "VERTICES"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto vertex_count = read_uint(input, "vertex count");
        if (!vertex_count) {
            return core::Result<ProjectDocument>::failure(vertex_count.error());
        }
        if (auto result = validate_serialized_count(
                vertex_count.value(), kMaxSerializedVertices, "mesh vertex count");
            !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::map<std::uint64_t, geometry::VertexId> vertex_ids;
        for (std::uint64_t vertex_index = 0; vertex_index < vertex_count.value(); ++vertex_index) {
            if (auto result = require_line(input, "VERTEX"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            core::Vec3d position;
            auto serialized_id = read_uint(input, "vertex id");
            if (!serialized_id || !(input >> position.x >> position.y >> position.z) ||
                serialized_id.value() == 0 || !position.finite()) {
                return core::Result<ProjectDocument>::failure(parse_error("invalid mesh vertex record"));
            }
            const geometry::VertexId vertex_id{serialized_id.value()};
            if (auto added = mesh.insert_vertex(geometry::Vertex{vertex_id, position}); !added) {
                return core::Result<ProjectDocument>::failure(added.error());
            }
            vertex_ids.emplace(serialized_id.value(), vertex_id);
        }
        if (auto result = require_line(input, "FACES"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto face_count = read_uint(input, "face count");
        if (!face_count) {
            return core::Result<ProjectDocument>::failure(face_count.error());
        }
        if (auto result = validate_serialized_count(
                face_count.value(), kMaxSerializedFaces, "mesh face count");
            !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t face_index = 0; face_index < face_count.value(); ++face_index) {
            if (auto result = require_line(input, "FACE"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto serialized_face_id = read_uint(input, "face id");
            auto face_vertex_count = read_uint(input, "face vertex count");
            if (!serialized_face_id || !face_vertex_count || serialized_face_id.value() == 0 ||
                face_vertex_count.value() > kMaxSerializedFaceVertices) {
                return core::Result<ProjectDocument>::failure(parse_error("invalid mesh face record"));
            }
            std::vector<geometry::VertexId> face_vertices;
            face_vertices.reserve(static_cast<std::size_t>(face_vertex_count.value()));
            for (std::uint64_t vertex_index = 0; vertex_index < face_vertex_count.value(); ++vertex_index) {
                auto serialized_vertex = read_uint(input, "face vertex id");
                if (!serialized_vertex || !vertex_ids.contains(serialized_vertex.value())) {
                    return core::Result<ProjectDocument>::failure(
                        parse_error("mesh face references an unknown serialized vertex"));
                }
                face_vertices.push_back(vertex_ids.at(serialized_vertex.value()));
            }
            const geometry::FaceId face_id{serialized_face_id.value()};
            if (auto added = mesh.insert_face(geometry::Face{face_id, std::move(face_vertices)});
                !added) {
                return core::Result<ProjectDocument>::failure(
                    added.error().with_context("serialized face " + std::to_string(serialized_face_id.value())));
            }
        }
        if (auto result = mesh.restore_revision(core::Revision(ignored_mesh_revision.value())); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        const auto [mesh_iterator, inserted] = document.meshes_.emplace(mesh_id.value(), std::move(mesh));
        static_cast<void>(mesh_iterator);
        if (!inserted) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project contains a duplicate mesh asset id"));
        }
        if (mesh_id.value() < std::numeric_limits<std::uint64_t>::max()) {
            document.next_mesh_id_ = std::max(document.next_mesh_id_, mesh_id.value() + 1U);
        }
    }

    if (auto result = require_line(input, "END"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    std::string trailing_token;
    if (input >> trailing_token) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project contains unexpected data after END"));
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
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (!size_error && file_size > kMaxSerializedProjectBytes) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project file exceeds the Cartographer v1 size limit of 128 MiB"));
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
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<SaveReceipt>::failure(
            Diagnostic(ErrorCode::io_error, "failed while closing project temporary file"));
    }

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
