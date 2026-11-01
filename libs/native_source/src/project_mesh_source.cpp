#include <carto/native_source/project_mesh_source.hpp>

#include <carto/production/render_mesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <span>
#include <sstream>
#include <utility>

namespace carto::native_source {
namespace {

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

bool rigid(const core::Transform& value) {
    if (!value.finite()) return false;
    const auto& q = value.rotation;
    const double n = std::hypot(std::hypot(q.x, q.y), std::hypot(q.z, q.w));
    return std::isfinite(n) && std::abs(n - 1.0) <= 1e-9 &&
        value.scale.x == 1.0 && value.scale.y == 1.0 && value.scale.z == 1.0;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

using TriangleKey = std::array<std::uint64_t, 4>;

TriangleKey triangle_key(geometry::FaceId face, geometry::VertexId a,
                         geometry::VertexId b, geometry::VertexId c) {
    // Cyclic rotations retain winding while permitting a different start vertex.
    return std::min({TriangleKey{face.value, a.value, b.value, c.value},
        TriangleKey{face.value, b.value, c.value, a.value},
        TriangleKey{face.value, c.value, a.value, b.value}});
}

bool same_triangles(const geometry::EditableMesh& source,
                    const geometry::CompiledMesh& compiled,
                    const production::RenderMeshArtifact& render) {
    if (compiled.triangle_faces.size() != render.triangle_faces.size()) return false;
    std::map<TriangleKey, std::size_t> counts;
    for (std::size_t i = 0; i < compiled.triangle_faces.size(); ++i) {
        const auto a = compiled.vertex_ids[compiled.indices[3U * i]];
        const auto b = compiled.vertex_ids[compiled.indices[3U * i + 1U]];
        const auto c = compiled.vertex_ids[compiled.indices[3U * i + 2U]];
        if (a == b || b == c || c == a) return false;
        ++counts[triangle_key(compiled.triangle_faces[i], a, b, c)];
    }
    for (const auto& vertex : render.vertices) {
        const auto* original = source.find_vertex(vertex.source_vertex);
        if (!original || original->position.x != vertex.position.x ||
            original->position.y != vertex.position.y || original->position.z != vertex.position.z)
            return false;
    }
    for (std::size_t i = 0; i < render.triangle_faces.size(); ++i) {
        const auto a = render.vertices[render.indices[3U * i]].source_vertex;
        const auto b = render.vertices[render.indices[3U * i + 1U]].source_vertex;
        const auto c = render.vertices[render.indices[3U * i + 2U]].source_vertex;
        if (a == b || b == c || c == a) return false;
        const auto it = counts.find(triangle_key(render.triangle_faces[i], a, b, c));
        if (it == counts.end() || it->second == 0U) return false;
        --it->second;
    }
    return std::all_of(counts.begin(), counts.end(), [](const auto& item) { return item.second == 0U; });
}

bool same_vec(core::Vec3d a, core::Vec3d b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_transform(const core::Transform& a, const core::Transform& b) {
    return same_vec(a.translation, b.translation) && same_vec(a.scale, b.scale) &&
        a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y &&
        a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w;
}

bool same_compiled(const geometry::CompiledMesh& a, const geometry::CompiledMesh& b) {
    return a.valid() && b.valid() && a.source_revision == b.source_revision &&
        a.indices == b.indices && a.vertex_ids == b.vertex_ids &&
        a.triangle_faces == b.triangle_faces && a.triangle_edges == b.triangle_edges &&
        a.positions.size() == b.positions.size() && a.normals.size() == b.normals.size() &&
        std::equal(a.positions.begin(), a.positions.end(), b.positions.begin(), same_vec) &&
        std::equal(a.normals.begin(), a.normals.end(), b.normals.begin(), same_vec) &&
        same_vec(a.bounds.minimum, b.bounds.minimum) && same_vec(a.bounds.maximum, b.bounds.maximum);
}

std::string source_projection(const project::ProjectDocument& project,
                              const scene::SceneObject& object,
                              const geometry::EditableMesh& mesh,
                              const assets::Sha256Digest& mesh_digest) {
    static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                  std::numeric_limits<double>::is_iec559);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "CARTO_NATIVE_PROJECT_MESH 1\n" << std::quoted(project.source_namespace())
        << ' ' << std::quoted(object.name) << '\n'
        << object.id.value << ' ' << *object.mesh_asset << ' '
        << mesh.revision().value() << '\n'
        << mesh_digest.hex() << '\n'
        << project::ProjectDocument::kAuthoringUnits << ' '
        << project::ProjectDocument::kAuthoringCoordinateSystem << '\n';
    const auto& t = object.local_transform;
    const double values[] = {t.translation.x, t.translation.y, t.translation.z,
        t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
        t.scale.x, t.scale.y, t.scale.z};
    out << std::hex << std::setfill('0');
    for (const double v : values) out << std::setw(16) << std::bit_cast<std::uint64_t>(v) << ' ';
    out << '\n';
    return out.str();
}

} // namespace

core::Result<ProjectMeshSource> derive_project_mesh_source(
    const project::ProjectDocument& project, scene::ObjectId selected_object) {
    using Result = core::Result<ProjectMeshSource>;
    if (!selected_object) return Result::failure(invalid("native source requires a selected object"));
    const project::ProjectDocument snapshot(project);
    if (auto valid = snapshot.validate(); !valid) return Result::failure(valid.error());
    const auto* object = snapshot.scene().find(selected_object);
    if (!object || object->parent || !object->mesh_asset || !rigid(object->local_transform)) {
        return Result::failure(invalid("native source requires a root mesh object with a rigid transform"));
    }
    const auto found = snapshot.meshes().find(*object->mesh_asset);
    if (found == snapshot.meshes().end()) return Result::failure(invalid("native source mesh is missing"));
    const auto& mesh = found->second;
    auto compiled = mesh.compile();
    if (!compiled) return Result::failure(compiled.error());
    if (!compiled.value().valid() || compiled.value().triangle_faces.empty()) {
        return Result::failure(invalid("native source compiled mesh is empty or invalid"));
    }

    ProjectMeshSource result;
    result.source_namespace = snapshot.source_namespace();
    result.object = object->id;
    result.mesh_asset = *object->mesh_asset;
    result.observed_project_revision = snapshot.revision();
    result.scene_revision = snapshot.scene().revision();
    result.mesh_revision = mesh.revision();
    result.local_to_project_world = object->local_transform;
    result.editable_mesh_digest = project::canonical_mesh_snapshot_digest(mesh);
    result.source_projection = source_projection(snapshot, *object, mesh, result.editable_mesh_digest);
    result.source_digest = digest_text(result.source_projection);
    const std::string source_revision = result.source_digest.hex();
    const std::string part_external = "mesh:" + std::to_string(result.mesh_asset);
    production::AuthoritativeSource source{
        "native-project:" + snapshot.source_namespace() + ":object:" +
            std::to_string(object->id.value) + ":mesh:" + std::to_string(result.mesh_asset),
        result.mesh_revision, result.source_digest};
    auto render = production::compile_render_mesh_artifact(source, mesh);
    if (!render) return Result::failure(render.error());
    auto bytes = render.value().serialize();
    if (!bytes) return Result::failure(bytes.error());
    if (!same_triangles(mesh, compiled.value(), render.value())) {
        return Result::failure(invalid("render triangles do not match the compiled authored mesh"));
    }
    result.render_mesh_bytes = std::move(bytes.value());
    result.render_mesh_digest = assets::sha256(result.render_mesh_bytes);
    result.physical_mesh_candidate = std::move(compiled.value());

    constexpr engineering::Id source_id = 1U, part_id = 1U, occurrence_id = 1U;
    result.part = {part_id, part_external, object->name, source_revision, "", std::nullopt, source_id};
    result.occurrence = {occurrence_id, "object:" + std::to_string(object->id.value),
        part_id, std::nullopt, object->local_transform, 1U, source_id};
    engineering::TopologyCorrespondenceMap candidate_map;
    candidate_map.source_namespace = snapshot.source_namespace();
    candidate_map.source_revision = source_revision;
    candidate_map.source_digest = result.source_digest;
    std::map<geometry::FaceId, engineering::Id> face_ids;
    for (std::size_t i = 0; i < render.value().triangle_faces.size(); ++i) {
        const auto face = render.value().triangle_faces[i];
        if (!mesh.find_face(face) || i > std::numeric_limits<std::uint32_t>::max()) {
            return Result::failure(invalid("render triangle references an invalid authored face"));
        }
        const std::string external_face = "face:" + std::to_string(face.value);
        auto [it, inserted] = face_ids.emplace(face, face.value);
        if (inserted) {
            result.faces.push_back({it->second, source_id, part_id, occurrence_id,
                engineering::TopologyKind::face, external_face, source_revision,
                result.mesh_asset, static_cast<std::uint32_t>(i)});
        }
        engineering::TopologyIdentityKey key{snapshot.source_namespace(), part_external,
            engineering::TopologyKind::face, external_face};
        auto id = engineering::persistent_topology_id(key);
        if (!id) return Result::failure(id.error());
        candidate_map.bindings.push_back({std::move(key), id.value(), result.mesh_asset,
            mesh.revision(), result.render_mesh_digest, static_cast<std::uint32_t>(i)});
    }
    if (auto valid = result.part.validate(); !valid) return Result::failure(valid.error());
    if (auto valid = result.occurrence.validate(); !valid) return Result::failure(valid.error());
    for (const auto& face : result.faces) {
        if (auto valid = face.validate(); !valid) return Result::failure(valid.error());
    }
    if (auto valid = candidate_map.validate(); !valid) return Result::failure(valid.error());
    result.render_face_candidates = std::move(candidate_map.bindings);
    return Result::success(std::move(result));
}

core::Result<ProjectViewportFace> verify_project_viewport_face(
    const project::ProjectDocument& project, const render::RenderScene& viewport,
    scene::ObjectId selected_object, geometry::FaceId face) {
    using Result = core::Result<ProjectViewportFace>;
    if (!face) return Result::failure(invalid("viewport source face is missing"));
    const project::ProjectDocument snapshot(project);
    auto derived = derive_project_mesh_source(snapshot, selected_object);
    if (!derived) return Result::failure(derived.error());
    const auto* object = snapshot.scene().find(selected_object);
    const auto* instance = viewport.find(selected_object);
    auto world = snapshot.scene().world_transform(selected_object);
    if (!object || !object->visible || !instance || !instance->mesh || !world ||
        !instance->mesh_asset || *instance->mesh_asset != derived.value().mesh_asset ||
        instance->source_revision != derived.value().mesh_revision ||
        !same_transform(instance->world_transform, world.value()) ||
        !same_compiled(*instance->mesh, derived.value().physical_mesh_candidate)) {
        return Result::failure(invalid("viewport instance does not match the current project source"));
    }
    ProjectViewportFace result;
    result.object = selected_object;
    result.mesh_asset = derived.value().mesh_asset;
    result.face = face;
    result.source_digest = derived.value().source_digest;
    result.topology = {derived.value().source_namespace,
        "mesh:" + std::to_string(result.mesh_asset), engineering::TopologyKind::face,
        "face:" + std::to_string(face.value)};
    auto topology = engineering::persistent_topology_id(result.topology);
    if (!topology) return Result::failure(topology.error());
    result.persistent_topology = topology.value();
    const auto& faces = instance->mesh->triangle_faces;
    if (faces.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Result::failure(invalid("viewport triangle count exceeds the binding limit"));
    }
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (faces[i] == face) result.compiled_triangles.push_back(static_cast<std::uint32_t>(i));
    }
    if (result.compiled_triangles.empty()) return Result::failure(invalid("viewport face has no triangles"));
    return Result::success(std::move(result));
}

} // namespace carto::native_source
