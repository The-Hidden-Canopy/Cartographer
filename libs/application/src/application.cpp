#include <carto/application/application.hpp>
#include <carto/editor/viewport_interaction.hpp>
#include <carto/project/recovery.hpp>
#include <carto/production/lightmap_uv.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
#include <memory>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace carto::application {

assets::Sha256Digest canonical_mesh_geometry_digest(
    const geometry::EditableMesh& mesh) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "CARTOGRAPHER_MESH_GEOMETRY 1\n";
    const auto vertices = mesh.vertices_sorted();
    canonical << "VERTICES " << vertices.size() << '\n' << std::hex << std::setfill('0');
    for (const auto& vertex : vertices) {
        canonical << vertex.id.value << ' '
            << std::setw(16) << std::bit_cast<std::uint64_t>(vertex.position.x) << ' '
            << std::setw(16) << std::bit_cast<std::uint64_t>(vertex.position.y) << ' '
            << std::setw(16) << std::bit_cast<std::uint64_t>(vertex.position.z) << '\n';
    }
    const auto faces = mesh.faces_sorted();
    canonical << "FACES " << std::dec << faces.size() << '\n';
    for (const auto& face : faces) {
        canonical << face.id.value << ' ' << face.vertices.size();
        for (const auto vertex : face.vertices) canonical << ' ' << vertex.value;
        canonical << '\n';
    }
    const std::string bytes = canonical.str();
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

namespace {

constexpr std::size_t kMaxNativeStructureMeshElements = 100'000U;
constexpr std::size_t kMaxNativeStructureMatches = 4096U;
constexpr std::size_t kMaxViewportAttributeOverlaySegments = 4096U;

core::Result<std::shared_ptr<const ViewportState::MeshAttributeOverlayData>>
build_viewport_attribute_overlay(
    const geometry::EditableMesh& mesh,
    const geometry::CompiledMesh& compiled) {
    using OverlayData = ViewportState::MeshAttributeOverlayData;
    using AttributeLine = ViewportState::AttributeLine;
    const auto fail = [](core::ErrorCode code, std::string message) {
        return core::Result<std::shared_ptr<const OverlayData>>::failure(
            core::Diagnostic(code, std::move(message)));
    };
    if (!compiled.valid() || compiled.source_revision != mesh.revision()) {
        return fail(core::ErrorCode::stale_data,
            "viewport attribute overlay compiled mesh does not match its editable source");
    }

    auto data = std::make_shared<OverlayData>();
    data->evaluated_mesh_revision = compiled.source_revision;
    const auto* sharp_layer = mesh.attributes().find("cartographer.edge_sharp");
    if (sharp_layer != nullptr) {
        data->has_sharp_edge_layer = true;
        if (sharp_layer->descriptor.domain != attributes::AttributeDomain::edge ||
            sharp_layer->descriptor.type != attributes::AttributeType::boolean ||
            sharp_layer->values.size() != mesh.edge_count()) {
            return fail(core::ErrorCode::validation_failed,
                "viewport edge-sharp overlay requires a complete edge-domain boolean layer");
        }
        if (auto valid = sharp_layer->validate(mesh.edge_count()); !valid) {
            return core::Result<std::shared_ptr<const OverlayData>>::failure(valid.error());
        }
        data->hard_edge_scan_capped = mesh.edge_count() >
            kMaxViewportAttributeOverlaySegments;
        const auto edges = mesh.edges_sorted(kMaxViewportAttributeOverlaySegments);
        const std::size_t edge_count = std::min(
            edges.size(), kMaxViewportAttributeOverlaySegments);
        data->hard_edges.reserve(std::min(edge_count, kMaxViewportAttributeOverlaySegments));
        for (std::size_t index = 0U; index < edge_count; ++index) {
            const auto* marked = std::get_if<bool>(&sharp_layer->values[index]);
            if (marked == nullptr) {
                return fail(core::ErrorCode::validation_failed,
                    "viewport edge-sharp overlay contains a non-boolean value");
            }
            if (!*marked) continue;
            const auto& edge = edges[index];
            const geometry::Vertex* first = mesh.find_vertex(edge.first);
            const geometry::Vertex* second = mesh.find_vertex(edge.second);
            if (first == nullptr || second == nullptr) {
                return fail(core::ErrorCode::stale_data,
                    "viewport edge-sharp overlay references a missing endpoint");
            }
            data->hard_edges.push_back(AttributeLine{first->position, second->position});
        }
    }

    const auto* normal_layer = mesh.attributes().find("cartographer.normal0");
    if (normal_layer != nullptr) {
        data->has_custom_normal_layer = true;
        const auto corner_count = mesh.attributes().domain_count(
            attributes::AttributeDomain::corner);
        if (normal_layer->descriptor.domain != attributes::AttributeDomain::corner ||
            normal_layer->descriptor.type != attributes::AttributeType::vec3 ||
            !corner_count.has_value() || normal_layer->values.size() != corner_count.value()) {
            return fail(core::ErrorCode::validation_failed,
                "viewport custom-normal overlay requires a complete corner-domain vec3 layer");
        }
        if (auto valid = normal_layer->validate(corner_count.value()); !valid) {
            return core::Result<std::shared_ptr<const OverlayData>>::failure(valid.error());
        }
        data->corner_normal_scan_capped = corner_count.value() >
            kMaxViewportAttributeOverlaySegments;
        const auto corners = mesh.corners_sorted(kMaxViewportAttributeOverlaySegments);
        const std::size_t displayed_corner_count = std::min(
            corner_count.value(), kMaxViewportAttributeOverlaySegments);
        if (corners.size() != displayed_corner_count) {
            return fail(core::ErrorCode::stale_data,
                "viewport custom-normal overlay corner order does not match its attribute layer");
        }
        const auto& bounds = compiled.bounds;
        const double diagonal = std::hypot(
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z);
        if (!std::isfinite(diagonal) || diagonal <= 0.0) {
            return fail(core::ErrorCode::validation_failed,
                "viewport custom-normal overlay requires finite compiled bounds");
        }
        const double normal_length = std::clamp(diagonal * 0.035, 0.01, 0.25);
        data->corner_normals.reserve(displayed_corner_count);
        for (std::size_t index = 0U; index < displayed_corner_count; ++index) {
            const auto* normal = std::get_if<core::Vec3d>(&normal_layer->values[index]);
            if (normal == nullptr || !normal->finite() || normal->length_squared() <= 1.0e-24) {
                return fail(core::ErrorCode::validation_failed,
                    "viewport custom-normal overlay contains an invalid corner normal");
            }
            const geometry::Vertex* vertex = mesh.find_vertex(corners[index].vertex);
            if (vertex == nullptr) {
                return fail(core::ErrorCode::stale_data,
                    "viewport custom-normal overlay references a missing corner vertex");
            }
            data->corner_normals.push_back(AttributeLine{
                vertex->position, vertex->position + normal->normalized() * normal_length});
        }
    }
    return core::Result<std::shared_ptr<const OverlayData>>::success(std::move(data));
}

core::Result<std::optional<assets::Sha256Digest>> mesh_material_region_digest(
    const geometry::EditableMesh& mesh) {
    const auto* condition = mesh.attributes().find("condition.material_region");
    const auto* generic = mesh.attributes().find("cartographer.material_slot");
    if (condition != nullptr && generic != nullptr) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "mesh declares both material-region aliases; source binding is ambiguous"));
    }
    const auto* layer = condition != nullptr ? condition : generic;
    if (layer == nullptr) {
        return core::Result<std::optional<assets::Sha256Digest>>::success(std::nullopt);
    }
    const auto faces = mesh.faces_sorted();
    if (layer->descriptor.domain != attributes::AttributeDomain::face ||
        layer->descriptor.type != attributes::AttributeType::uint32 ||
        layer->values.size() != faces.size()) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "mesh material-region layer is not a complete face-domain uint32 table"));
    }
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "CARTOGRAPHER_MESH_MATERIAL_REGIONS 1\n"
              << std::quoted(layer->descriptor.id) << ' ' << faces.size() << '\n';
    for (std::size_t index = 0U; index < faces.size(); ++index) {
        const auto* region = std::get_if<std::uint32_t>(&layer->values[index]);
        if (region == nullptr || *region == 0U) {
            return core::Result<std::optional<assets::Sha256Digest>>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "mesh material-region table contains an invalid region ID"));
        }
        canonical << faces[index].id.value << ' ' << *region << '\n';
    }
    const std::string bytes = canonical.str();
    return core::Result<std::optional<assets::Sha256Digest>>::success(
        assets::sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())));
}

struct NativeStructureSignatureIndex {
    assets::Sha256Digest object;
    std::map<geometry::FaceId, assets::Sha256Digest> faces;
    std::map<geometry::VertexId, assets::Sha256Digest> vertices;
};
assets::Sha256Digest road_terrain_preview_digest(
    const RoadTerrainPreview& preview) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17)
           << "CARTOGRAPHER_ROAD_TERRAIN_PREVIEW 1\n"
           << std::quoted(preview.road_surface.road_identity) << ' '
           << preview.road_surface.road_revision.value() << ' '
           << preview.road_surface.project_generation << ' '
           << preview.road_surface.road_digest.hex() << ' '
           << preview.road_surface.centerline_digest.hex() << '\n'
           << std::quoted(preview.terrain_identity) << ' '
           << preview.terrain_revision.value() << ' '
           << preview.terrain_digest.hex() << ' '
           << static_cast<unsigned>(preview.terrain_semantic) << ' '
           << preview.terrain_origin_xz.x << ' ' << preview.terrain_origin_xz.y << ' '
           << preview.terrain_cell_size_meters << ' '
           << preview.terrain_width << ' ' << preview.terrain_height << ' '
           << static_cast<unsigned>(preview.coordinate_frame) << ' '
           << preview.candidate_grid_tests << ' '
           << preview.cut_volume_cubic_meters << ' '
           << preview.fill_volume_cubic_meters << '\n'
           << preview.road_surface.mesh.samples_per_segment << ' '
           << preview.cells.size() << '\n';
    for (const auto& cell : preview.cells) {
        output << cell.u << ' ' << cell.v << ' '
               << cell.position.x << ' ' << cell.position.y << ' '
               << cell.position.z << ' ' << cell.terrain_elevation_meters << ' '
               << cell.design_elevation_meters << ' '
               << cell.signed_cut_depth_meters << ' '
               << cell.represented_area_square_meters << '\n';
    }
    const std::string bytes = output.str();
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

assets::Sha256Digest structure_digest(std::string_view bytes) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

core::Result<NativeStructureSignatureIndex> build_native_structure_signature_index(
    const geometry::EditableMesh& mesh,
    bool include_face_signatures,
    bool include_vertex_signatures,
    std::optional<geometry::FaceId> only_face = std::nullopt,
    std::optional<geometry::VertexId> only_vertex = std::nullopt) {
    if (mesh.vertex_count() > kMaxNativeStructureMeshElements ||
        mesh.face_count() > kMaxNativeStructureMeshElements ||
        mesh.edge_count() > kMaxNativeStructureMeshElements) {
        return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "structural correspondence exceeds the bounded mesh element limit"));
    }
    if (auto valid = mesh.validate(); !valid) {
        return core::Result<NativeStructureSignatureIndex>::failure(valid.error().with_context(
            "structural correspondence source mesh"));
    }

    const auto vertices = mesh.vertices_sorted();
    const auto faces = mesh.faces_sorted();
    std::map<geometry::VertexId, std::vector<std::size_t>> incident_face_arities;
    std::map<geometry::FaceId, std::vector<std::size_t>> neighboring_face_arities;
    std::map<std::pair<geometry::VertexId, geometry::VertexId>, std::vector<geometry::FaceId>>
        edge_faces;
    std::map<geometry::VertexId, std::vector<geometry::VertexId>> neighbors;
    std::vector<std::size_t> face_arities;
    face_arities.reserve(faces.size());
    for (const auto& face : faces) {
        face_arities.push_back(face.vertices.size());
        for (const auto vertex : face.vertices) {
            incident_face_arities[vertex].push_back(face.vertices.size());
        }
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            auto first = face.vertices[index];
            auto second = face.vertices[(index + 1U) % face.vertices.size()];
            if (second < first) std::swap(first, second);
            edge_faces[{first, second}].push_back(face.id);
            neighbors[first].push_back(second);
            neighbors[second].push_back(first);
        }
    }

    std::size_t boundary_edge_count = 0U;
    std::vector<std::size_t> edge_face_counts;
    edge_face_counts.reserve(edge_faces.size());
    for (auto& [edge, attached_faces] : edge_faces) {
        static_cast<void>(edge);
        std::sort(attached_faces.begin(), attached_faces.end());
        attached_faces.erase(std::unique(attached_faces.begin(), attached_faces.end()),
            attached_faces.end());
        if (attached_faces.empty() || attached_faces.size() > 2U) {
            return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "structural correspondence requires manifold edge incidence"));
        }
        edge_face_counts.push_back(attached_faces.size());
        if (attached_faces.size() == 1U) {
            ++boundary_edge_count;
        } else {
            const auto first = mesh.find_face(attached_faces[0]);
            const auto second = mesh.find_face(attached_faces[1]);
            if (first == nullptr || second == nullptr) {
                return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "structural correspondence found a missing adjacent face"));
            }
            neighboring_face_arities[first->id].push_back(second->vertices.size());
            neighboring_face_arities[second->id].push_back(first->vertices.size());
        }
    }

    for (auto& [vertex, arities] : incident_face_arities) {
        static_cast<void>(vertex);
        std::sort(arities.begin(), arities.end());
    }
    for (auto& [face, arities] : neighboring_face_arities) {
        static_cast<void>(face);
        std::sort(arities.begin(), arities.end());
    }
    for (auto& [vertex, adjacent] : neighbors) {
        static_cast<void>(vertex);
        std::sort(adjacent.begin(), adjacent.end());
        adjacent.erase(std::unique(adjacent.begin(), adjacent.end()), adjacent.end());
    }
    std::sort(face_arities.begin(), face_arities.end());
    std::sort(edge_face_counts.begin(), edge_face_counts.end());

    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "CARTOGRAPHER_NATIVE_STRUCTURE 1\n"
        << "M " << vertices.size() << ' ' << faces.size() << ' '
        << edge_faces.size() << ' ' << boundary_edge_count << '\n';
    const auto append_values = [&canonical](const auto& values) {
        canonical << values.size();
        for (const auto value : values) canonical << ' ' << value;
        canonical << '\n';
    };
    append_values(face_arities);
    append_values(edge_face_counts);
    std::vector<std::size_t> vertex_valences;
    vertex_valences.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        const auto found = incident_face_arities.find(vertex.id);
        vertex_valences.push_back(found == incident_face_arities.end() ? 0U : found->second.size());
    }
    std::sort(vertex_valences.begin(), vertex_valences.end());
    append_values(vertex_valences);

    NativeStructureSignatureIndex index;
    index.object = structure_digest(canonical.str());
    if (include_face_signatures) {
        for (const auto& face : faces) {
            if (only_face.has_value() && face.id != *only_face) continue;
            std::vector<std::size_t> local_vertex_valences;
            local_vertex_valences.reserve(face.vertices.size());
            std::size_t local_boundary_edges = 0U;
            for (std::size_t boundary_index = 0U;
                 boundary_index < face.vertices.size(); ++boundary_index) {
                const auto vertex = face.vertices[boundary_index];
                const auto incident = incident_face_arities.find(vertex);
                local_vertex_valences.push_back(
                    incident == incident_face_arities.end() ? 0U : incident->second.size());
                auto first = vertex;
                auto second = face.vertices[(boundary_index + 1U) % face.vertices.size()];
                if (second < first) std::swap(first, second);
                const auto edge = edge_faces.find({first, second});
                if (edge == edge_faces.end()) {
                    return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "structural correspondence face edge is missing"));
                }
                if (edge->second.size() == 1U) ++local_boundary_edges;
            }
            std::sort(local_vertex_valences.begin(), local_vertex_valences.end());
            std::ostringstream local;
            local.imbue(std::locale::classic());
            local << "CARTOGRAPHER_NATIVE_COMPONENT 1 FACE " << index.object.hex() << '\n'
                << face.vertices.size() << ' ' << local_boundary_edges << '\n';
            const auto append_local_values = [&local](const auto& values) {
                local << values.size();
                for (const auto value : values) local << ' ' << value;
                local << '\n';
            };
            append_local_values(local_vertex_valences);
            const auto adjacent = neighboring_face_arities.find(face.id);
            if (adjacent == neighboring_face_arities.end()) {
                append_local_values(std::vector<std::size_t>{});
            } else {
                append_local_values(adjacent->second);
            }
            index.faces.emplace(face.id, structure_digest(local.str()));
        }
    }

    if (include_vertex_signatures) {
        for (const auto& vertex : vertices) {
            if (only_vertex.has_value() && vertex.id != *only_vertex) continue;
            const auto incident = incident_face_arities.find(vertex.id);
            const auto adjacent = neighbors.find(vertex.id);
            std::vector<std::size_t> neighbor_valences;
            std::size_t local_boundary_edges = 0U;
            if (adjacent != neighbors.end()) {
                neighbor_valences.reserve(adjacent->second.size());
                for (const auto neighbor : adjacent->second) {
                    const auto neighbor_incident = incident_face_arities.find(neighbor);
                    neighbor_valences.push_back(neighbor_incident == incident_face_arities.end()
                        ? 0U : neighbor_incident->second.size());
                    auto first = vertex.id;
                    auto second = neighbor;
                    if (second < first) std::swap(first, second);
                    const auto edge = edge_faces.find({first, second});
                    if (edge == edge_faces.end()) {
                        return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                            core::ErrorCode::stale_data,
                            "structural correspondence vertex edge is missing"));
                    }
                    if (edge->second.size() == 1U) ++local_boundary_edges;
                }
            }
            std::sort(neighbor_valences.begin(), neighbor_valences.end());
            std::ostringstream local;
            local.imbue(std::locale::classic());
            local << "CARTOGRAPHER_NATIVE_COMPONENT 1 VERTEX " << index.object.hex() << '\n'
                << (incident == incident_face_arities.end() ? 0U : incident->second.size())
                << ' ' << local_boundary_edges << '\n';
            const auto append_local_values = [&local](const auto& values) {
                local << values.size();
                for (const auto value : values) local << ' ' << value;
                local << '\n';
            };
            if (incident == incident_face_arities.end()) {
                append_local_values(std::vector<std::size_t>{});
            } else {
                append_local_values(incident->second);
            }
            append_local_values(neighbor_valences);
            index.vertices.emplace(vertex.id, structure_digest(local.str()));
        }
    }
    return core::Result<NativeStructureSignatureIndex>::success(std::move(index));
}

core::Result<assets::Sha256Digest> structure_signature_for_mesh(
    const geometry::EditableMesh& mesh,
    const std::variant<std::monostate, geometry::FaceId, geometry::VertexId>& component) {
    const bool include_faces = std::holds_alternative<geometry::FaceId>(component);
    const bool include_vertices = std::holds_alternative<geometry::VertexId>(component);
    auto index = build_native_structure_signature_index(
        mesh, include_faces, include_vertices,
        include_faces ? std::optional<geometry::FaceId>{
            std::get<geometry::FaceId>(component)} : std::nullopt,
        include_vertices ? std::optional<geometry::VertexId>{
            std::get<geometry::VertexId>(component)} : std::nullopt);
    if (!index) return core::Result<assets::Sha256Digest>::failure(index.error());
    if (std::holds_alternative<std::monostate>(component)) {
        return core::Result<assets::Sha256Digest>::success(index.value().object);
    }
    if (std::holds_alternative<geometry::FaceId>(component)) {
        const auto found = index.value().faces.find(std::get<geometry::FaceId>(component));
        if (found != index.value().faces.end()) {
            return core::Result<assets::Sha256Digest>::success(found->second);
        }
    } else if (std::holds_alternative<geometry::VertexId>(component)) {
        const auto found = index.value().vertices.find(std::get<geometry::VertexId>(component));
        if (found != index.value().vertices.end()) {
            return core::Result<assets::Sha256Digest>::success(found->second);
        }
    }
    return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
        core::ErrorCode::stale_data,
        "structural correspondence target is missing from its source mesh"));
}

bool condition_kinematic_bindings_current(
    const condition::KinematicAssemblySource& assembly,
    const project::ProjectDocument& document) {
    if (assembly.part_bindings.empty()) return false;
    for (const auto& binding : assembly.part_bindings) {
        const auto* object = document.scene().find(
            scene::ObjectId{binding.scene_object_id});
        const auto mesh = document.meshes().find(binding.mesh_asset_id);
        if (object == nullptr || !object->mesh_asset.has_value() ||
            *object->mesh_asset != binding.mesh_asset_id ||
            mesh == document.meshes().end() ||
            mesh->second.revision() != binding.mesh_revision) {
            return false;
        }
    }
    return true;
}

bool same_transform(const core::Transform& left, const core::Transform& right) noexcept {
    return left.translation.x == right.translation.x &&
        left.translation.y == right.translation.y &&
        left.translation.z == right.translation.z &&
        left.rotation.x == right.rotation.x && left.rotation.y == right.rotation.y &&
        left.rotation.z == right.rotation.z && left.rotation.w == right.rotation.w &&
        left.scale.x == right.scale.x && left.scale.y == right.scale.y &&
        left.scale.z == right.scale.z;
}

bool condition_prefab_instance_bindings_current(
    const condition::PrefabInstanceRecord& instance,
    const condition::AuthoringDocument& authoring,
    const project::ProjectDocument& document) {
    const auto prefab = std::lower_bound(authoring.prefabs.begin(), authoring.prefabs.end(),
        instance.prefab_id, [](const auto& item, condition::Id id) {
            return item.id < id;
        });
    if (prefab == authoring.prefabs.end() || prefab->id != instance.prefab_id ||
        prefab->source_revision != instance.prefab_source_revision ||
        prefab->asset_ids.size() != instance.members.size()) {
        return false;
    }
    const auto* root = document.scene().find(scene::ObjectId{instance.root_object_id});
    if (root == nullptr || root->parent.has_value() || root->mesh_asset.has_value()) return false;
    for (std::size_t index = 0U; index < instance.members.size(); ++index) {
        const auto& member = instance.members[index];
        if (prefab->asset_ids[index] != member.asset_id) return false;
        const auto asset = std::lower_bound(authoring.assets.begin(), authoring.assets.end(),
            member.asset_id, [](const auto& item, condition::Id id) {
                return item.id < id;
            });
        if (asset == authoring.assets.end() || asset->id != member.asset_id ||
            asset->authored_object_id != member.source_object_id ||
            asset->source_revision != member.source_revision ||
            asset->source_digest != member.source_digest) {
            return false;
        }
        const auto evidence = std::lower_bound(authoring.sources.begin(), authoring.sources.end(),
            asset->source_id, [](const auto& item, condition::Id id) {
                return item.id < id;
            });
        if (evidence == authoring.sources.end() || evidence->id != asset->source_id ||
            !evidence->current || evidence->revision != member.source_revision ||
            evidence->digest != member.source_digest) {
            return false;
        }
        const auto* source_object = document.scene().find(
            scene::ObjectId{member.source_object_id});
        const auto source_mesh = document.meshes().find(member.mesh_asset_id);
        const auto source_world = document.scene().world_transform(
            scene::ObjectId{member.source_object_id});
        const auto* instance_object = document.scene().find(
            scene::ObjectId{member.instance_object_id});
        const auto instance_mesh = document.meshes().find(member.mesh_asset_id);
        if (source_object == nullptr || !source_object->mesh_asset.has_value() ||
            *source_object->mesh_asset != member.mesh_asset_id ||
            source_mesh == document.meshes().end() ||
            source_mesh->second.revision() != member.mesh_revision || !source_world ||
            !same_transform(source_world.value(), member.source_world_transform) ||
            instance_object == nullptr || instance_object->parent !=
                std::optional<scene::ObjectId>{scene::ObjectId{instance.root_object_id}} ||
            instance_object->mesh_asset !=
                std::optional<std::uint64_t>{member.mesh_asset_id} ||
            instance_mesh == document.meshes().end() ||
            instance_mesh->second.revision() != member.mesh_revision) {
            return false;
        }
    }
    return true;
}

core::Result<void> verify_source_blob(
    const std::filesystem::path& project_path,
    const assets::SourceImportRecord& record) {
    const bool supported_standard_mesh_importer =
        (record.importer_identity == "carto.import.obj" &&
            (record.importer_version == 1U || record.importer_version == 2U ||
             record.importer_version == 3U)) ||
        ((record.importer_identity == "carto.import.ply" ||
          record.importer_identity == "carto.import.stl") &&
            record.importer_version == 1U);
    const bool receipt_bound_botanical_importer =
        (record.importer_identity == "carto.import.generated-gltf" ||
         record.importer_identity == "carto.import.generated-glb") &&
        record.importer_version == 1U && record.botanical_generation.has_value();
    if (record.kind != assets::SourceAssetKind::mesh ||
        (!supported_standard_mesh_importer && !receipt_bound_botanical_importer)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "source-backed mesh import requires a supported built-in importer"));
    }
    constexpr std::uintmax_t kMaxMeshSourceBytes = 128ULL * 1024ULL * 1024ULL;
    std::error_code error;
    const auto requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path.parent_path();
    if (error) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to resolve the saved project directory"));

    assets::BlobStore store(project_root / ".carto-assets" / "source-blobs");
    const auto expected_path = store.path_for(record.source_digest);
    const auto stored_path = std::filesystem::canonical(expected_path, error);
    if (error) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::not_found, "source-backed mesh bytes are missing"));
    const auto relative_path = stored_path.lexically_relative(project_root);
    if (relative_path.empty() || relative_path.is_absolute() ||
        relative_path.generic_string() != record.relative_source_path ||
        std::any_of(relative_path.begin(), relative_path.end(), [](const auto& component) {
            return component == "..";
        })) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "source-backed mesh path is not the project-local content-addressed blob"));
    }
    const auto size = std::filesystem::file_size(stored_path, error);
    if (error || size > kMaxMeshSourceBytes) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "source-backed mesh blob is unavailable or exceeds the importer limit"));
    }
    return store.verify(record.source_digest);
}

core::Result<std::string> locate_staged_source(
    const std::filesystem::path& project_path,
    const assets::Sha256Digest& digest) {
    constexpr std::uintmax_t kMaxMeshSourceBytes = 128ULL * 1024ULL * 1024ULL;
    std::error_code error;
    const auto requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path.parent_path();
    if (error) return core::Result<std::string>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to resolve the saved project directory"));
    }
    assets::BlobStore store(project_root / ".carto-assets" / "source-blobs");
    const auto expected_path = store.path_for(digest);
    const auto stored_path = std::filesystem::canonical(expected_path, error);
    if (error) return core::Result<std::string>::failure(core::Diagnostic(
        core::ErrorCode::not_found, "staged source bytes are missing"));
    const auto relative_path = stored_path.lexically_relative(project_root);
    if (relative_path.empty() || relative_path.is_absolute() ||
        std::any_of(relative_path.begin(), relative_path.end(), [](const auto& component) {
            return component == "..";
        })) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "staged source path is not the project-local content-addressed blob"));
    }
    const auto size = std::filesystem::file_size(stored_path, error);
    if (error || size > kMaxMeshSourceBytes) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "staged source blob is unavailable or exceeds the importer limit"));
    }
    if (auto result = store.verify(digest); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    return core::Result<std::string>::success(relative_path.generic_string());
}

core::Result<assets::TextureSourceAdmission> inspect_project_texture_blob(
    const std::filesystem::path& project_path,
    const assets::Sha256Digest& digest,
    assets::TextureSourceFormat expected_format,
    std::uint32_t expected_width,
    std::uint32_t expected_height) {
    assets::TextureSourceLimits texture_limits;
    std::error_code error;
    const auto requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path.parent_path();
    if (error) return core::Result<assets::TextureSourceAdmission>::failure(
        core::Diagnostic(core::ErrorCode::io_error,
            "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) {
        return core::Result<assets::TextureSourceAdmission>::failure(
            core::Diagnostic(core::ErrorCode::io_error,
                "unable to resolve the saved project directory"));
    }

    assets::BlobStore store(
        project_root / ".carto-assets" / "source-blobs",
        assets::BlobStoreLimits{texture_limits.max_source_bytes, 256U});
    const auto expected_path = store.path_for(digest);
    const auto stored_path = std::filesystem::canonical(expected_path, error);
    if (error) return core::Result<assets::TextureSourceAdmission>::failure(
        core::Diagnostic(core::ErrorCode::not_found,
            "material texture source bytes are missing from the project asset store"));
    const auto expected_relative = expected_path.lexically_relative(project_root);
    const auto stored_relative = stored_path.lexically_relative(project_root);
    if (stored_path.lexically_normal() != expected_path.lexically_normal() ||
        expected_relative.empty() || expected_relative.is_absolute() ||
        stored_relative.generic_string() != expected_relative.generic_string() ||
        std::any_of(stored_relative.begin(), stored_relative.end(), [](const auto& component) {
            return component == "..";
        })) {
        return core::Result<assets::TextureSourceAdmission>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "material texture blob is not at its project-local content-addressed path"));
    }
    const auto size = std::filesystem::file_size(stored_path, error);
    if (error || size > texture_limits.max_source_bytes) {
        return core::Result<assets::TextureSourceAdmission>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "material texture blob is unavailable or exceeds the inspection limit"));
    }
    auto bytes = store.read(digest);
    if (!bytes) return core::Result<assets::TextureSourceAdmission>::failure(
        bytes.error().with_context("material texture blob verification"));
    const auto inspected = assets::inspect_texture_source(
        bytes.value(), assets::texture_source_media_type(expected_format), texture_limits);
    if (!inspected) return core::Result<assets::TextureSourceAdmission>::failure(
        inspected.error().with_context("material texture source inspection"));
    if (inspected.value().source.digest != digest ||
        inspected.value().format != expected_format ||
        inspected.value().width != expected_width ||
        inspected.value().height != expected_height) {
        return core::Result<assets::TextureSourceAdmission>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "material texture source metadata does not match its stored bytes"));
    }
    return inspected;
}

constexpr std::size_t kMaxProblems = 128U;
// ProjectDocument bounds serialized projects at 128 MiB. Keep the durable
// snapshot envelope capable of carrying that governed project state so large
// imported anatomy meshes do not require an unjournaled mutation path.
constexpr std::size_t kMaxJournalPayloadBytes = 128U * 1024U * 1024U;
constexpr std::string_view kSnapshotEvent = "cartographer.snapshot";
constexpr std::string_view kActionEvent = "cartographer.application_action";

journal::JournalLimits application_journal_limits() {
    journal::JournalLimits limits;
    limits.max_payload_bytes = kMaxJournalPayloadBytes;
    limits.max_record_bytes = 256ULL * 1024ULL * 1024ULL;
    limits.max_file_bytes = 1024ULL * 1024ULL * 1024ULL;
    return limits;
}

std::filesystem::path journal_path_for(const std::filesystem::path& project_path) {
    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    return journal_path;
}

std::string document_snapshot_payload(
    std::string_view header,
    std::string_view operation,
    std::string_view serialized_document) {
    return std::string(header) + "\n" +
        (operation.empty() ? std::string{} : "operation=" + std::string(operation) + "\n") +
        "document_bytes=" + std::to_string(serialized_document.size()) + "\n" +
        std::string(serialized_document);
}

bool journal_entry_matches_document(
    const journal::JournalEntry& entry,
    std::string_view serialized_document) {
    const std::string payload(entry.payload.begin(), entry.payload.end());
    const bool is_snapshot = entry.event_type == kSnapshotEvent;
    const bool is_mutation = entry.event_type == kActionEvent;
    if (!is_snapshot && !is_mutation) return false;

    std::size_t cursor = 0U;
    const auto next_line = [&payload, &cursor]() -> std::optional<std::string_view> {
        const std::size_t end = payload.find('\n', cursor);
        if (end == std::string::npos) return std::nullopt;
        const std::string_view line(payload.data() + cursor, end - cursor);
        cursor = end + 1U;
        return line;
    };
    const auto header = next_line();
    if (!header.has_value() || *header !=
            (is_snapshot ? "CARTOGRAPHER_PROJECT_SNAPSHOT_V1"
                         : "CARTOGRAPHER_APPLICATION_MUTATION_V1")) {
        return false;
    }
    if (is_mutation) {
        const auto operation = next_line();
        if (!operation.has_value() || !operation->starts_with("operation=") ||
            operation->size() == std::string_view("operation=").size() ||
            operation->find_first_of("\r\n") != std::string_view::npos) {
            return false;
        }
    }
    const auto document_bytes = next_line();
    constexpr std::string_view kDocumentBytes = "document_bytes=";
    if (!document_bytes.has_value() || !document_bytes->starts_with(kDocumentBytes)) {
        return false;
    }
    const std::string_view value = document_bytes->substr(kDocumentBytes.size());
    if (value.empty()) return false;
    std::uint64_t declared_bytes = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), declared_bytes);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        declared_bytes != serialized_document.size()) {
        return false;
    }
    return std::string_view(payload).substr(cursor) == serialized_document;
}

} // namespace

core::Result<std::optional<assets::Sha256Digest>>
canonical_mesh_material_region_digest(const geometry::EditableMesh& mesh) {
    return mesh_material_region_digest(mesh);
}

core::Result<DispatchReceipt> HumanApplicationAccess::dispatch(
    ApplicationSession& session,
    const ApplicationAction& action) {
    return session.dispatch(HumanActionAdmission{}, action);
}

core::Result<DispatchReceipt> HumanApplicationAccess::commit_preview(
    ApplicationSession& session,
    editor::AuthoringPreview& preview) {
    return session.commit_preview(HumanActionAdmission{}, preview);
}

core::Result<DispatchReceipt> HumanApplicationAccess::commit_preview(
    ApplicationSession& session,
    editor::AuthoringPreview& preview,
    OperationSource source,
    std::string_view provenance_id) {
    return session.commit_preview(HumanActionAdmission{}, preview, source, provenance_id);
}

core::Result<void> WorkspaceState::validate() const {
    if (visible_panes.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "workspace must expose at least one pane"));
    }
    std::set<Pane> unique_panes;
    for (const Pane pane : visible_panes) {
        const bool known_pane = pane == Pane::outliner || pane == Pane::viewport ||
            pane == Pane::inspector || pane == Pane::problems || pane == Pane::history;
        if (!known_pane) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "workspace contains an unknown pane"));
        }
        if (!unique_panes.insert(pane).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "workspace cannot contain duplicate panes"));
        }
    }
    if (!unique_panes.contains(Pane::viewport)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workspace must keep the viewport pane visible"));
    }
    return core::Result<void>::success();
}

core::Result<void> RecoveryInspection::validate() const {
    if (project_path.empty() || project_path.filename().empty() ||
        journal_path.empty() || journal_path.filename().empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery inspection paths must name files"));
    }
    if (journal_entry_count != journal_entries.size()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "recovery inspection entry count does not match its entries"));
    }
    if (journal_entries.empty()) {
        if (durable_revision != core::Revision{}) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "empty recovery inspection cannot have a durable revision"));
        }
    } else if (durable_revision != journal_entries.back().revision_after) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "recovery inspection durable revision does not match its journal tail"));
    }
    if (state == RecoveryInspectionState::blocked && !diagnostic.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "blocked recovery inspection requires a diagnostic"));
    }
    if (state != RecoveryInspectionState::blocked && diagnostic.has_value() &&
        state == RecoveryInspectionState::clean) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "clean recovery inspection cannot carry a blocking diagnostic"));
    }
    for (const auto& entry : journal_entries) {
        if (entry.event_type.empty() || entry.revision_after <= entry.revision_before) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "recovery inspection contains an invalid journal summary"));
        }
    }
    return core::Result<void>::success();
}

ApplicationSession::ApplicationSession() {
    static_cast<void>(tools_.register_builtin_tools());
    saved_revision_ = document_.revision();
}

ApplicationSession::ApplicationSession(
    const ApplicationSession& source,
    SpeculativeCopyTag)
    : document_(source.document_),
      saved_revision_(source.document_.revision()),
      project_generation_(source.project_generation_),
      selection_(source.selection_),
      workspace_(source.workspace_) {
    static_cast<void>(tools_.register_builtin_tools());
}

namespace {

// Shared recovery classification for the leased static inspection entry
// point and for an open that already holds the project writer lease. Callers
// must guarantee exclusive access to the project and journal evidence.
core::Result<RecoveryInspection> inspect_recovery_unlocked(
    const std::filesystem::path& project_path) {
    RecoveryInspection inspection;
    inspection.project_path = project_path;
    inspection.journal_path = journal_path_for(project_path);

    std::error_code error;
    const bool project_exists = std::filesystem::exists(project_path, error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project path"));
    }
    const bool journal_exists = std::filesystem::exists(inspection.journal_path, error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project journal path"));
    }

    if (project_exists) {
        const auto document = project::ProjectDocument::load(project_path);
        if (!document) {
            inspection.state = RecoveryInspectionState::blocked;
            inspection.diagnostic = document.error().with_context(
                "recovery inspection project load");
        } else {
            inspection.project_revision = document.value().revision();
        }
    }

    if (!journal_exists) {
        if (inspection.state != RecoveryInspectionState::blocked) {
            inspection.state = RecoveryInspectionState::unavailable;
        }
        if (auto result = inspection.validate(); !result) {
            return core::Result<RecoveryInspection>::failure(result.error());
        }
        return core::Result<RecoveryInspection>::success(std::move(inspection));
    }

    journal::Journal journal(inspection.journal_path, application_journal_limits());
    const auto entries = journal.read_all();
    if (!entries) {
        inspection.state = RecoveryInspectionState::blocked;
        inspection.diagnostic = entries.error().with_context(
            "recovery inspection journal verification");
    } else {
        inspection.journal_entry_count = entries.value().size();
        inspection.journal_entries.reserve(entries.value().size());
        for (const auto& entry : entries.value()) {
            inspection.journal_entries.push_back(RecoveryJournalEntry{
                entry.revision_before, entry.revision_after, entry.event_type});
        }
        if (!entries.value().empty()) {
            inspection.durable_revision = entries.value().back().revision_after;
        }

        if (inspection.state != RecoveryInspectionState::blocked) {
            if (!project_exists) {
                inspection.state = RecoveryInspectionState::pending;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::not_found,
                    "durable journal exists but the project file is missing");
            } else if (entries.value().empty()) {
                if (inspection.project_revision.value_or(core::Revision{}) == core::Revision{}) {
                    inspection.state = RecoveryInspectionState::clean;
                } else {
                    inspection.state = RecoveryInspectionState::blocked;
                    inspection.diagnostic = core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "project has a revision but its durable journal is empty");
                }
            } else if (inspection.project_revision.value() < inspection.durable_revision) {
                inspection.state = RecoveryInspectionState::pending;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "durable journal is ahead of the saved project");
            } else if (inspection.project_revision.value() > inspection.durable_revision) {
                inspection.state = RecoveryInspectionState::blocked;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "saved project is ahead of its durable journal");
            } else {
                const auto document = project::ProjectDocument::load(project_path);
                if (!document || !journal_entry_matches_document(
                        entries.value().back(), document.value().serialize())) {
                    inspection.state = RecoveryInspectionState::blocked;
                    inspection.diagnostic = core::Diagnostic(
                        core::ErrorCode::validation_failed,
                        "durable journal tail does not match the saved project");
                } else {
                    inspection.state = RecoveryInspectionState::clean;
                }
            }
        }
    }

    if (auto result = inspection.validate(); !result) {
        return core::Result<RecoveryInspection>::failure(result.error());
    }
    return core::Result<RecoveryInspection>::success(std::move(inspection));
}

} // namespace

core::Result<RecoveryInspection> ApplicationSession::inspect_recovery(
    const std::filesystem::path& project_path) {
    if (project_path.empty() || project_path.filename().empty()) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery inspection requires a project file path"));
    }

    std::error_code error;
    const bool project_exists = std::filesystem::exists(project_path, error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project path"));
    }
    const bool journal_exists =
        std::filesystem::exists(journal_path_for(project_path), error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project journal path"));
    }

    // Inspect the project and its journal under the same writer lease used by
    // save, mutation publication, and recovery. Without this lease a writer
    // could replace the project between the project load and journal read,
    // producing a recovery classification for two different revisions.
    std::optional<project::FileLock> project_lock;
    if (project_exists || journal_exists) {
        project_lock.emplace(project_path);
        if (auto result = project_lock->acquire(); !result) {
            return core::Result<RecoveryInspection>::failure(result.error().with_context(
                "recovery inspection lock"));
        }
    }
    return inspect_recovery_unlocked(project_path);
}

core::Result<ApplicationSession::PreparedJournal> ApplicationSession::prepare_journal(
    const std::filesystem::path& project_path,
    const project::ProjectDocument& document) const {
    if (project_path.empty() || project_path.filename().empty()) {
        return core::Result<PreparedJournal>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "journal binding requires a project file path"));
    }

    const auto path = journal_path_for(project_path);
    std::error_code exists_error;
    const bool existed = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return core::Result<PreparedJournal>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project journal path"));
    }
    std::uintmax_t original_bytes = 0U;
    if (existed) {
        std::error_code size_error;
        original_bytes = std::filesystem::file_size(path, size_error);
        if (size_error) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to inspect the project journal size"));
        }
    }

    journal::Journal candidate(path, application_journal_limits());
    const auto entries = candidate.read_all();
    if (!entries) {
        return core::Result<PreparedJournal>::failure(entries.error().with_context(
            "project journal verification"));
    }

    if (!entries.value().empty()) {
        const core::Revision tail = entries.value().back().revision_after;
        if (tail != document.revision()) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project revision does not match the durable journal; recovery is required"));
        }
        const std::string serialized = document.serialize();
        if (!journal_entry_matches_document(entries.value().back(), serialized)) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project contents do not match the latest durable journal snapshot"));
        }
        return core::Result<PreparedJournal>::success(
            PreparedJournal{std::move(candidate), false, false, original_bytes});
    }

    if (document.revision().value() != 0U) {
        const std::string snapshot = document.serialize();
        if (snapshot.size() > kMaxJournalPayloadBytes) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project snapshot exceeds the durable journal payload limit"));
        }
        const std::string payload_text = document_snapshot_payload(
            "CARTOGRAPHER_PROJECT_SNAPSHOT_V1", {}, snapshot);
        if (payload_text.size() > kMaxJournalPayloadBytes) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project snapshot journal envelope exceeds the payload limit"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload_text.data());
        const std::span<const std::uint8_t> payload(bytes, payload_text.size());
        const auto baseline = candidate.append(journal::JournalAppend{
            core::Revision{},
            document.revision(),
            std::string(kSnapshotEvent),
            payload,
            std::nullopt,
        });
        if (!baseline) {
            return core::Result<PreparedJournal>::failure(baseline.error().with_context(
                "project journal baseline"));
        }
        return core::Result<PreparedJournal>::success(
            PreparedJournal{std::move(candidate), !existed, true, original_bytes});
    }

    return core::Result<PreparedJournal>::success(
        PreparedJournal{std::move(candidate), !existed, false, original_bytes});
}

core::Result<void> ApplicationSession::append_mutation_event(
    std::string_view action,
    core::Revision revision_before,
    core::Revision revision_after) {
    if (!journal_.has_value() || revision_before == revision_after) {
        return core::Result<void>::success();
    }
    const std::string serialized_document = document_.serialize();
    const std::string payload_text = document_snapshot_payload(
        "CARTOGRAPHER_APPLICATION_MUTATION_V1", action, serialized_document);
    if (payload_text.size() > kMaxJournalPayloadBytes) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "application mutation snapshot exceeds the durable journal payload limit"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload_text.data());
    const std::span<const std::uint8_t> payload(bytes, payload_text.size());
    const auto append = [&]() -> core::Result<void> {
        const auto appended = journal_->append(journal::JournalAppend{
            revision_before,
            revision_after,
            std::string(kActionEvent),
            payload,
            std::nullopt,
        });
        if (!appended) {
            return core::Result<void>::failure(appended.error().with_context(
                "durable application journal append"));
        }
        return core::Result<void>::success();
    };
    if (project_path_.has_value()) {
        project::FileLock project_lock(*project_path_);
        if (auto result = project_lock.acquire(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project mutation lock"));
        }
        return append();
    }
    return append();
}

core::Result<DispatchReceipt> ApplicationSession::accept_speculative_result(
    const project::ProjectDocument& candidate,
    core::Revision expected_revision,
    std::string_view task_id,
    std::string_view objective_digest,
    std::string_view diff_digest) {
    const auto parsed_objective_digest = assets::Sha256Digest::from_hex(objective_digest);
    const auto parsed_digest = assets::Sha256Digest::from_hex(diff_digest);
    if (active_source_ != OperationSource::human || task_id.empty() ||
        task_id.size() > 64U || task_id.find_first_of("\r\n") != std::string_view::npos ||
        !parsed_objective_digest || !parsed_digest) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "speculative result acceptance requires a valid human task boundary"));
    }
    if (document_.revision() != expected_revision) {
        return core::Result<DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project changed after the speculative task began"));
    }
    if (expected_revision.exhausted() || candidate.revision() <= expected_revision) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "speculative result is empty or its base revision cannot advance"));
    }
    if (auto result = candidate.validate(); !result) {
        return core::Result<DispatchReceipt>::failure(
            result.error().with_context("speculative result acceptance"));
    }

    editor::CommandBus prior_history = std::move(history_);
    history_ = editor::CommandBus{};
    editor::ProjectCommandAdmission admission;
    const OperationSource previous_source = active_source_;
    std::string previous_provenance = std::move(active_provenance_id_);
    active_source_ = OperationSource::ai_proposal;
    active_provenance_id_ = std::string(task_id);
    const std::string operation = "Accept Agent Speculative Result " +
        std::string(task_id) + " objective=" + std::string(objective_digest) +
        " diff=" + std::string(diff_digest);
    auto result = execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, candidate),
        operation);
    active_source_ = previous_source;
    active_provenance_id_ = std::move(previous_provenance);
    if (!result) {
        history_ = std::move(prior_history);
        return result;
    }

    selection_.clear();
    last_tool_action_.reset();
    last_tool_context_.reset();
    viewport_mesh_cache_.clear();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0U;
    } else {
        ++project_generation_;
    }
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::install_agent_rebase_branch(
    const project::ProjectDocument& candidate,
    core::Revision expected_revision,
    std::string_view checkpoint_task_id,
    std::string_view plan_context_digest,
    std::string_view checkpoint_source_digest) {
    const auto parsed_plan_digest = assets::Sha256Digest::from_hex(plan_context_digest);
    const auto parsed_source_digest = assets::Sha256Digest::from_hex(checkpoint_source_digest);
    if (active_source_ != OperationSource::human || checkpoint_task_id.empty() ||
        checkpoint_task_id.size() > 64U ||
        checkpoint_task_id.find_first_of("\r\n") != std::string_view::npos ||
        !parsed_plan_digest || !parsed_source_digest) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "agent branch rebase requires a validated checkpoint lineage"));
    }
    if (document_.revision() != expected_revision) {
        return core::Result<DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "speculative source changed before the rebased branch was installed"));
    }
    if (expected_revision.exhausted() || candidate.revision() <= expected_revision) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "rebased speculative branch must advance its exact base revision"));
    }
    if (auto result = candidate.validate(); !result) {
        return core::Result<DispatchReceipt>::failure(
            result.error().with_context("agent speculative rebase branch"));
    }

    editor::CommandBus prior_history = std::move(history_);
    history_ = editor::CommandBus{};
    editor::ProjectCommandAdmission admission;
    const OperationSource previous_source = active_source_;
    std::string previous_provenance = std::move(active_provenance_id_);
    active_source_ = OperationSource::ai_proposal;
    active_provenance_id_ = std::string(checkpoint_task_id);
    const std::string operation = "Rebase Agent Speculative Branch " +
        std::string(checkpoint_task_id) + " plan=" + parsed_plan_digest.value().hex() +
        " source=" + parsed_source_digest.value().hex();
    auto result = execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, candidate),
        operation);
    active_source_ = previous_source;
    active_provenance_id_ = std::move(previous_provenance);
    if (!result) {
        history_ = std::move(prior_history);
        return result;
    }

    selection_.clear();
    last_tool_action_.reset();
    last_tool_context_.reset();
    viewport_mesh_cache_.clear();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0U;
    } else {
        ++project_generation_;
    }
    return result;
}

core::Result<project::ProjectDocument> ApplicationSession::merge_agent_rebase_document(
    const project::ProjectDocument& base,
    const project::ProjectDocument& speculative,
    const std::vector<scene::ObjectId>& read_objects,
    const std::vector<std::uint64_t>& read_mesh_assets,
    const std::vector<scene::ObjectId>& write_objects,
    const std::vector<std::uint64_t>& write_mesh_assets,
    const std::vector<project::MaterialRegionKey>& read_material_regions,
    const std::vector<project::MaterialRegionKey>& write_material_regions) const {
    return project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, document_, speculative, read_objects, read_mesh_assets,
        write_objects, write_mesh_assets, read_material_regions, write_material_regions);
}

core::Result<project::ProjectDocument> ApplicationSession::merge_agent_rebase_step_document(
    const project::ProjectDocument& base,
    const project::ProjectDocument& speculative,
    const std::vector<scene::ObjectId>& write_objects,
    const std::vector<std::uint64_t>& write_mesh_assets,
    std::uint64_t revision_delta) const {
    auto selected = project::ProjectDocument::select_agent_rebase_step_changes(
        base, speculative, write_objects, write_mesh_assets, revision_delta);
    if (!selected) return selected;
    return project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, document_, selected.value(), write_objects, write_mesh_assets,
        write_objects, write_mesh_assets);
}

core::Result<void> ApplicationSession::install_agent_checkpoint_source(
    project::ProjectDocument checkpoint_source) {
    if (auto result = checkpoint_source.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("historical agent checkpoint source"));
    }
    document_.swap(checkpoint_source);
    saved_revision_ = document_.revision();
    reset_editor_state();
    viewport_mesh_cache_.clear();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0U;
    } else {
        ++project_generation_;
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::dispatch(
    HumanActionAdmission,
    const ApplicationAction& action,
    OperationSource source,
    std::string_view provenance_id) {
    const OperationSource previous_source = active_source_;
    std::string previous_provenance = std::move(active_provenance_id_);
    active_source_ = source;
    active_provenance_id_ = std::string(provenance_id);
    struct SourceRestore final {
        ApplicationSession* session;
        OperationSource source;
        std::string provenance;
        ~SourceRestore() {
            session->active_source_ = source;
            session->active_provenance_id_ = std::move(provenance);
        }
    } restore{this, previous_source, std::move(previous_provenance)};
    return std::visit(
        [this](const auto& value) -> core::Result<DispatchReceipt> {
            using Action = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Action, NewProjectAction>) {
                return new_project(value);
            } else if constexpr (std::is_same_v<Action, OpenProjectAction>) {
                return open_project(value);
            } else if constexpr (std::is_same_v<Action, RecoverProjectAction>) {
                return recover_project(value);
            } else if constexpr (std::is_same_v<Action, SaveProjectAction>) {
                return save_project(value);
            } else if constexpr (std::is_same_v<Action, SetWorkspaceAction>) {
                const core::Revision before = document_.revision();
                if (auto result = set_workspace(value.workspace); !result) {
                    return failure(result.error().with_context("workspace update"));
                }
                return accepted("Set Workspace", before);
            } else if constexpr (std::is_same_v<Action, SetSelectionModeAction>) {
                const core::Revision before = document_.revision();
                if (auto result = selection_.set_mode(value.mode); !result) {
                    return failure(result.error().with_context("selection mode"));
                }
                return accepted("Set Selection Mode", before);
            } else if constexpr (std::is_same_v<Action, ConvertSelectionAction>) {
                return convert_selection(value);
            } else if constexpr (std::is_same_v<Action, ExpandSelectionAction>) {
                return expand_selection(value);
            } else if constexpr (std::is_same_v<Action, SelectShortestPathAction>) {
                return select_shortest_path(value);
            } else if constexpr (std::is_same_v<Action, SelectObjectAction>) {
                return select_object(value);
            } else if constexpr (std::is_same_v<Action, SelectVertexAction>) {
                return select_vertex(value);
            } else if constexpr (std::is_same_v<Action, SelectFaceAction>) {
                return select_face(value);
            } else if constexpr (std::is_same_v<Action, SelectEdgeAction>) {
                return select_edge(value);
            } else if constexpr (std::is_same_v<Action, SetObjectTransformAction>) {
                if (value.expected_project_revision.has_value() &&
                    *value.expected_project_revision != document_.revision()) {
                    return failure(core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "object transform was authored against a stale project revision"));
                }
                const auto* object = document_.scene().find(value.object);
                if (!object) {
                    return failure(core::Diagnostic(
                        core::ErrorCode::not_found,
                        "cannot transform a missing scene object"));
                }
                if (object->locked) {
                    return failure(invalid_state("locked scene objects cannot be transformed"));
                }
                const auto before_transform = object->local_transform;
                const bool is_root_object = !object->parent.has_value();
                std::optional<assets::Sha256Digest> structure_signature;
                if (object->mesh_asset.has_value()) {
                    const auto mesh = document_.meshes().find(*object->mesh_asset);
                    if (mesh != document_.meshes().end()) {
                        const auto signature = structure_signature_for_mesh(
                            mesh->second, std::monostate{});
                        if (signature) structure_signature = signature.value();
                    }
                }
                editor::ProjectCommandAdmission admission;
                auto result = execute_command(
                    std::make_unique<editor::SetProjectObjectTransformCommand>(
                        admission,
                        document_, value.object, before_transform, value.transform),
                    "Set Object Transform",
                    AffectedSet{{value.object}, {}, {}, {}},
                    ParameterPayload{
                        value.transform, std::nullopt, std::nullopt, std::nullopt,
                        std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                        std::nullopt});
                const bool translation_only =
                    before_transform.rotation.x == value.transform.rotation.x &&
                    before_transform.rotation.y == value.transform.rotation.y &&
                    before_transform.rotation.z == value.transform.rotation.z &&
                    before_transform.rotation.w == value.transform.rotation.w &&
                    before_transform.scale.x == value.transform.scale.x &&
                    before_transform.scale.y == value.transform.scale.y &&
                    before_transform.scale.z == value.transform.scale.z &&
                    is_root_object;
                if (result && result.value().document_changed) {
                    const auto displacement = value.transform.translation -
                        before_transform.translation;
                    const double magnitude = displacement.length();
                    if (active_source_ == OperationSource::human && translation_only &&
                        displacement.finite() && std::isfinite(magnitude) && magnitude > 0.0) {
                        std::optional<assets::Sha256Digest> result_object_signature;
                        const auto* result_object = document_.scene().find(value.object);
                        if (result_object != nullptr && result_object->mesh_asset.has_value()) {
                            const auto result_mesh = document_.meshes().find(
                                *result_object->mesh_asset);
                            if (result_mesh != document_.meshes().end()) {
                                const auto signature = structure_signature_for_mesh(
                                    result_mesh->second, std::monostate{});
                                if (signature) result_object_signature = signature.value();
                            }
                        }
                        append_native_operation_trace(NativeOperationTraceEntry{
                            result.value().operation_id,
                            "object.translate",
                            result.value().source,
                            result.value().revision_before,
                            result.value().revision_after,
                            value.object,
                            std::monostate{},
                            NativeTraceParameterization::relative_displacement,
                            0.0,
                            displacement,
                            structure_signature,
                            result_object_signature});
                    } else {
                        native_operation_trace_.clear();
                    }
                }
                return result;
            } else if constexpr (std::is_same_v<Action, InvokeToolAction>) {
                return invoke_tool(value);
            } else if constexpr (std::is_same_v<Action, RepeatLastToolAction>) {
                return repeat_last_tool();
            } else if constexpr (std::is_same_v<Action, AdjustLastToolAction>) {
                return adjust_last_tool(value);
            } else if constexpr (std::is_same_v<Action, CreateBoxAction>) {
                return create_box(value);
            } else if constexpr (std::is_same_v<Action, CreatePlaneAction>) {
                return create_plane(value);
            } else if constexpr (std::is_same_v<Action, CreateMeshObjectAction>) {
                return create_mesh_object(value);
            } else if constexpr (std::is_same_v<Action, AppendSculptStrokeAction>) {
                return append_sculpt_stroke(value);
            } else if constexpr (std::is_same_v<Action, AppendSculptSurfaceStrokeAction>) {
                return append_sculpt_surface_stroke(value);
            } else if constexpr (std::is_same_v<Action, SetSculptLayerVisibilityAction>) {
                return set_sculpt_layer_visibility(value);
            } else if constexpr (std::is_same_v<Action, DuplicateMeshInstanceAction>) {
                return duplicate_mesh_instance(value);
            } else if constexpr (std::is_same_v<Action, DuplicateMeshInstanceBatchAction>) {
                return duplicate_mesh_instance_batch(value);
            } else if constexpr (std::is_same_v<Action, CreatePlantScatterBatchAction>) {
                return create_plant_scatter_batch(value);
            } else if constexpr (std::is_same_v<Action, ImportEngineeringFixtureAction>) {
                return import_engineering_fixture();
            } else if constexpr (std::is_same_v<Action, ImportEngineeringAuthoringAction>) {
                return import_engineering_authoring(value);
            } else if constexpr (std::is_same_v<Action, ImportEngineeringBomCsvAction>) {
                return import_engineering_bom_csv(value);
            } else if constexpr (std::is_same_v<Action, ImportEngineeringRequirementCsvAction>) {
                return import_engineering_requirement_csv(value);
            } else if constexpr (std::is_same_v<Action, ImportEngineeringPathResultAction>) {
                return import_engineering_path_result(value);
            } else if constexpr (std::is_same_v<Action, AttachEngineeringEvidenceAction>) {
                return attach_engineering_evidence(value);
            } else if constexpr (std::is_same_v<Action, CreateEngineeringRequirementAction>) {
                return create_engineering_requirement(value);
            } else if constexpr (std::is_same_v<Action, ReviewEngineeringRequirementAction>) {
                return review_engineering_requirement(value);
            } else if constexpr (std::is_same_v<Action, CreateEngineeringCharacteristicAction>) {
                return create_engineering_characteristic(value);
            } else if constexpr (std::is_same_v<Action, CreateEngineeringTargetAction>) {
                return create_engineering_target(value);
            } else if constexpr (std::is_same_v<Action, UpdateEngineeringWorkcellAction>) {
                return update_engineering_workcell(value);
            } else if constexpr (std::is_same_v<Action, CreateEngineeringKeepOutAction>) {
                return create_engineering_keep_out(value);
            } else if constexpr (std::is_same_v<Action, CreateEngineeringWorkcellFrameAction>) {
                return create_engineering_workcell_frame(value);
            } else if constexpr (std::is_same_v<Action, ReviewEngineeringTargetAction>) {
                return review_engineering_target(value);
            } else if constexpr (std::is_same_v<Action, InvalidateEngineeringSourceAction>) {
                return invalidate_engineering_source(value);
            } else if constexpr (std::is_same_v<Action, ImportConditionFixtureAction>) {
                return import_condition_fixture();
            } else if constexpr (std::is_same_v<Action, ImportConditionAuthoringAction>) {
                return import_condition_authoring(value);
            } else if constexpr (std::is_same_v<Action, BindConditionAssetObjectAction>) {
                return bind_condition_asset_object(value);
            } else if constexpr (std::is_same_v<Action, AddConditionInteractionAnchorAction>) {
                return add_condition_interaction_anchor(value);
            } else if constexpr (std::is_same_v<Action, AddConditionPrefabAction>) {
                return add_condition_prefab(value);
            } else if constexpr (std::is_same_v<Action, InstantiateConditionPrefabAction>) {
                return instantiate_condition_prefab(value);
            } else if constexpr (std::is_same_v<Action, AddConditionCollectionAction>) {
                return add_condition_collection(value);
            } else if constexpr (std::is_same_v<Action, AddConditionRegionKitAction>) {
                return add_condition_region_kit(value);
            } else if constexpr (std::is_same_v<Action, AddConditionKinematicAssemblyAction>) {
                return add_condition_kinematic_assembly(value);
            } else if constexpr (std::is_same_v<Action, ReplaceConditionAuthoredRecordAction>) {
                return replace_condition_authored_record(value);
            } else if constexpr (std::is_same_v<Action, DeleteConditionAuthoredRecordAction>) {
                return delete_condition_authored_record(value);
            } else if constexpr (std::is_same_v<Action, InvalidateConditionSourceAction>) {
                return invalidate_condition_source(value);
            } else if constexpr (std::is_same_v<Action, ApplySourceReimportPlanAction>) {
                return apply_source_reimport_plan(value);
            } else if constexpr (std::is_same_v<Action, RelinkMovedSourceAction>) {
                return relink_moved_source(value);
            } else if constexpr (std::is_same_v<Action, CreateSpatialFieldAction>) {
                return create_spatial_field(value);
            } else if constexpr (std::is_same_v<Action, PaintSpatialFieldAction>) {
                return paint_spatial_field(value);
            } else if constexpr (std::is_same_v<Action, CreateMaterialAction>) {
                return create_material(value);
            } else if constexpr (std::is_same_v<Action, UpdateMaterialAction>) {
                return update_material(value);
            } else if constexpr (std::is_same_v<Action, SetMaterialRegionAssignmentAction>) {
                return set_material_region_assignment(value);
            } else if constexpr (std::is_same_v<Action, GenerateUniqueLightmapUvAction>) {
                return generate_unique_lightmap_uv(value);
            } else if constexpr (std::is_same_v<Action, SetMaterialGraphAction>) {
                return set_material_graph(value);
            } else if constexpr (std::is_same_v<Action, SetMaterialGraphBindingAction>) {
                return set_material_graph_binding(value);
            } else if constexpr (std::is_same_v<Action, ImportMaterialTextureSourceAction>) {
                return import_material_texture_source(value);
            } else if constexpr (std::is_same_v<Action, SetPresentationCatalogAction>) {
                return set_presentation_catalog(value);
            } else if constexpr (std::is_same_v<Action, SetProjectAuthoringStandardsAction>) {
                return set_project_authoring_standards(value);
            } else if constexpr (std::is_same_v<Action, SetRoadAuthoringAction>) {
                return set_road_authoring(value);
            } else if constexpr (
                std::is_same_v<Action, CreateRoadSurfaceInstanceAction>) {
                return create_road_surface_instance(value);
            } else if constexpr (std::is_same_v<Action, SetSweepSourceAction>) {
                return set_sweep_source(value);
            } else if constexpr (std::is_same_v<Action, SetBiologicalAuthoringAction>) {
                return set_biological_authoring(value);
            } else if constexpr (std::is_same_v<Action, SetSignTextSourceAction>) {
                return set_sign_text_source(value);
            } else if constexpr (std::is_same_v<Action, SetDecalPlacementAction>) {
                return set_decal_placement(value);
            } else if constexpr (std::is_same_v<Action, CreateSignTextObjectAction>) {
                return create_sign_text_object(value);
            } else if constexpr (
                std::is_same_v<Action, CreateBiologicalProxyInstanceAction>) {
                return create_biological_proxy_instance(value);
            } else if constexpr (std::is_same_v<Action, UndoAction>) {
                return undo();
            } else {
                return redo();
            }
        },
        action);
}

core::Result<editor::AuthoringContext> ApplicationSession::authoring_context() const {
    auto context = editor::AuthoringContext::from_selection(document_.revision(), selection_);
    if (!context) {
        return context;
    }

    if (selection_.mode() == editor::SelectionMode::object) {
        if (auto result = selection_.validate(document_.scene(), nullptr); !result) {
            return core::Result<editor::AuthoringContext>::failure(result.error().with_context(
                "authoring selection"));
        }
        return context;
    }

    const auto owner = selection_.component_object();
    if (!owner.has_value() || context.value().component_groups.size() > 1U) {
        if (context.value().component_groups.size() < 2U) {
            return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "component authoring context has no owning scene object"));
        }
        std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
        for (const auto& group : context.value().component_groups) {
            const auto* group_object = document_.scene().find(group.object);
            if (!group_object) {
                return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "component authoring group owner is no longer present"));
            }
            if (!group_object->mesh_asset.has_value()) {
                return core::Result<editor::AuthoringContext>::failure(invalid_state(
                    "component authoring group owner has no mesh asset"));
            }
            const auto group_mesh = document_.meshes().find(*group_object->mesh_asset);
            if (group_mesh == document_.meshes().end()) {
                return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "component authoring group references a missing mesh asset"));
            }
            owner_meshes.emplace(group.object, &group_mesh->second);
        }
        if (auto result = selection_.validate(document_.scene(), owner_meshes); !result) {
            return core::Result<editor::AuthoringContext>::failure(
                result.error().with_context("authoring selection"));
        }
        return context;
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<editor::AuthoringContext>::failure(invalid_state(
            "component authoring context owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context references a missing mesh asset"));
    }
    if (auto result = selection_.validate(document_.scene(), &mesh->second); !result) {
        return core::Result<editor::AuthoringContext>::failure(result.error().with_context(
            "authoring selection"));
    }
    return context;
}

core::Result<engineering::AuthoringDocument> ApplicationSession::engineering_authoring() const {
    if (!document_.engineering_authoring().has_value()) {
        return core::Result<engineering::AuthoringDocument>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "the project has no engineering authoring document"));
    }
    const auto decoded = engineering::AuthoringDocument::deserialize(
        document_.engineering_authoring()->bytes);
    if (!decoded) {
        return core::Result<engineering::AuthoringDocument>::failure(decoded.error().with_context(
            "project engineering authoring"));
    }
    return decoded;
}

core::Result<condition::AuthoringDocument> ApplicationSession::condition_authoring() const {
    if (!document_.condition_authoring().has_value()) {
        return core::Result<condition::AuthoringDocument>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "the project has no CONDITION source-authoring document"));
    }
    const auto decoded = condition::AuthoringDocument::deserialize(
        document_.condition_authoring()->bytes);
    if (!decoded) {
        return core::Result<condition::AuthoringDocument>::failure(decoded.error().with_context(
            "project CONDITION authoring"));
    }
    return decoded;
}

core::Result<assets::SourceReimportPlan> ApplicationSession::preview_source_reimport(
    std::vector<assets::SourceImportRecord> incoming) const {
    return assets::plan_source_reimport(document_.source_asset_registry(), std::move(incoming));
}

core::Result<assets::SourceReimportPlan>
ApplicationSession::preview_source_asset_addition(
    assets::SourceImportRecord incoming) const {
    const auto& registry = document_.source_asset_registry();
    if (registry.records().contains(incoming.identity)) {
        return core::Result<assets::SourceReimportPlan>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source asset identity is already active"));
    }

    core::Revision expected_generation{1U};
    const auto lineage = registry.lineage().find(incoming.identity);
    if (lineage != registry.lineage().end()) {
        if (lineage->second.exhausted()) {
            return core::Result<assets::SourceReimportPlan>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "source asset generation space is exhausted"));
        }
        expected_generation = lineage->second.next();
    }
    incoming.source_generation = expected_generation;

    std::vector<assets::SourceImportRecord> complete_candidate;
    complete_candidate.reserve(registry.records().size() + 1U);
    for (const auto& [identity, record] : registry.records()) {
        static_cast<void>(identity);
        complete_candidate.push_back(record);
    }
    complete_candidate.push_back(std::move(incoming));
    return assets::plan_source_reimport(registry, std::move(complete_candidate));
}

core::Result<assets::DecodedRgba8Texture>
ApplicationSession::decode_reference_image_tga(std::string_view identity) const {
    constexpr std::uint64_t kMaxReferencePreviewPixels = 16'777'216ULL;
    const auto plane = document_.presentation_catalog().reference_planes().find(
        std::string(identity));
    if (plane == document_.presentation_catalog().reference_planes().end()) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "reference-image plane does not exist in the current project"));
    }
    if (!project_path_.has_value()) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "reference-image decoding requires a saved project"));
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(plane->second.image_width_pixels) *
        static_cast<std::uint64_t>(plane->second.image_height_pixels);
    if (pixel_count == 0U || pixel_count > kMaxReferencePreviewPixels) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "reference-image pixel count exceeds the bounded TGA preview budget"));
    }
    std::error_code error;
    const auto requested_root = project_path_->parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path_->parent_path();
    if (error) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to resolve the current project directory for reference-image decoding"));
    }
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to resolve the current project directory for reference-image decoding"));
    }
    auto encoded = project::read_reference_image_source_bytes(
        project_root, plane->second, document_.source_asset_registry());
    if (!encoded) {
        return core::Result<assets::DecodedRgba8Texture>::failure(
            encoded.error().with_context("reference-image TGA source verification"));
    }
    assets::TextureSourceLimits limits;
    limits.max_pixels = kMaxReferencePreviewPixels;
    auto decoded = assets::decode_tga_rgba8(encoded.value(), limits);
    if (!decoded) {
        return core::Result<assets::DecodedRgba8Texture>::failure(
            decoded.error().with_context("reference-image TGA pixel decode"));
    }
    if (decoded.value().admission.source.digest != plane->second.source_digest ||
        decoded.value().admission.width != plane->second.image_width_pixels ||
        decoded.value().admission.height != plane->second.image_height_pixels ||
        decoded.value().pixels.size() != pixel_count * 4U) {
        return core::Result<assets::DecodedRgba8Texture>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "decoded reference-image pixels do not match the plane's source binding"));
    }
    return decoded;
}

core::Result<condition::EffectPreviewResult> ApplicationSession::condition_effect_preview(
    condition::Id effect_source_id,
    std::string_view operating_state,
    double elapsed_seconds) const {
    const auto authoring = condition_authoring();
    if (!authoring) {
        return core::Result<condition::EffectPreviewResult>::failure(
            authoring.error().with_context("CONDITION effect preview"));
    }
    return condition::preview_effect(authoring.value(), document_.spatial_fields(),
        effect_source_id, operating_state, elapsed_seconds);
}

core::Result<condition::VegetationPreviewResult>
ApplicationSession::condition_vegetation_preview(condition::Id recipe_id) const {
    const auto authoring = condition_authoring();
    if (!authoring) {
        return core::Result<condition::VegetationPreviewResult>::failure(
            authoring.error().with_context("CONDITION vegetation preview"));
    }
    std::map<std::string, condition::VegetationPlantSourceReceipt> plant_sources;
    for (const auto& [identity, record] : document_.source_asset_registry().records()) {
        if (record.kind != assets::SourceAssetKind::mesh ||
            !record.artifact_identity.starts_with("mesh.asset.")) {
            continue;
        }
        plant_sources.emplace(identity, condition::VegetationPlantSourceReceipt{
            identity, record.source_generation, record.source_digest});
    }
    auto preview = condition::preview_vegetation(authoring.value(),
        document_.spatial_fields(), plant_sources, recipe_id);
    if (!preview) return preview;
    preview.value().application_revision = document_.revision();
    std::ostringstream bound_digest_input;
    bound_digest_input.imbue(std::locale::classic());
    bound_digest_input << "CARTOGRAPHER_VEGETATION_PROJECT_PREVIEW 1\n"
        << preview.value().application_revision.value() << ' '
        << preview.value().preview_digest.hex() << '\n';
    const std::string bound_digest_bytes = bound_digest_input.str();
    preview.value().preview_digest = assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bound_digest_bytes.data()),
        bound_digest_bytes.size()));
    return preview;
}

core::Result<condition::StateComparisonResult>
ApplicationSession::condition_state_comparison(
    condition::Id left_branch_id,
    condition::Id right_branch_id) const {
    const auto authoring = condition_authoring();
    if (!authoring) {
        return core::Result<condition::StateComparisonResult>::failure(
            authoring.error().with_context("CONDITION state comparison"));
    }
    return condition::compare_state_branches(
        authoring.value(), left_branch_id, right_branch_id);
}

core::Result<SpatialFieldPreview> ApplicationSession::spatial_field_preview(
    std::string_view identity) const {
    const auto found = document_.spatial_fields().find(std::string(identity));
    if (found == document_.spatial_fields().end()) {
        return core::Result<SpatialFieldPreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "spatial field preview requires a field present in this project"));
    }
    const auto& field = found->second;
    SpatialFieldPreview preview;
    preview.identity = field.identity();
    preview.semantic = field.semantic();
    preview.source_revision = field.source_revision();
    preview.origin_uv = field.origin_uv();
    preview.cell_size = field.cell_size();
    preview.minimum_value = field.minimum_value();
    preview.maximum_value = field.maximum_value();
    preview.source_width = field.width();
    preview.source_height = field.height();
    preview.preview_width = std::min(
        field.width(), SpatialFieldPreview::kMaxPreviewAxis);
    preview.preview_height = std::min(
        field.height(), SpatialFieldPreview::kMaxPreviewAxis);
    preview.values.reserve(static_cast<std::size_t>(preview.preview_width) *
                           preview.preview_height);
    for (std::uint32_t v = 0U; v < preview.preview_height; ++v) {
        const std::uint32_t source_v = preview.preview_height == 1U
            ? 0U
            : static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(v) * (field.height() - 1U)) /
                (preview.preview_height - 1U));
        for (std::uint32_t u = 0U; u < preview.preview_width; ++u) {
            const std::uint32_t source_u = preview.preview_width == 1U
                ? 0U
                : static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(u) * (field.width() - 1U)) /
                    (preview.preview_width - 1U));
            const std::size_t index = static_cast<std::size_t>(source_v) *
                field.width() + source_u;
            if (index >= field.values().size()) {
                return core::Result<SpatialFieldPreview>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "spatial field preview index exceeds the validated source"));
            }
            preview.values.push_back(field.values()[index]);
        }
    }
    return core::Result<SpatialFieldPreview>::success(std::move(preview));
}

core::Result<spatial_field::VectorFieldPreview>
ApplicationSession::spatial_vector_field_preview(
    std::string_view u_component_identity,
    std::string_view v_component_identity,
    std::uint32_t max_preview_axis) const {
    const auto& fields = document_.spatial_fields();
    const auto u_component = fields.find(std::string(u_component_identity));
    const auto v_component = fields.find(std::string(v_component_identity));
    if (u_component == fields.end() || v_component == fields.end()) {
        return core::Result<spatial_field::VectorFieldPreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "vector preview requires both component sources in this project"));
    }
    return spatial_field::build_vector_field_preview(
        u_component->second, v_component->second, max_preview_axis);
}

core::Result<terrain::TerrainTileProduct> ApplicationSession::terrain_tile_preview(
    std::string_view identity,
    std::uint32_t lod_step,
    double max_navigation_slope_degrees,
    std::optional<double> water_level) const {
    const auto found = document_.spatial_fields().find(std::string(identity));
    if (found == document_.spatial_fields().end()) {
        return core::Result<terrain::TerrainTileProduct>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "terrain preview requires a source field present in this project"));
    }
    const auto key = terrain::parse_terrain_tile_identity(identity);
    if (!key) return core::Result<terrain::TerrainTileProduct>::failure(key.error());
    return terrain::build_terrain_tile(found->second, key.value(), lod_step,
        max_navigation_slope_degrees, water_level);
}

core::Result<geometry::CurvePath> ApplicationSession::curve_path_source(
    std::string_view identity) const {
    const auto found = document_.curve_paths().find(std::string(identity));
    if (found == document_.curve_paths().end()) {
        return core::Result<geometry::CurvePath>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "curve source is not present in this project"));
    }
    return core::Result<geometry::CurvePath>::success(found->second);
}

core::Result<project::MaterialGraph> ApplicationSession::material_graph_source(
    std::string_view identity) const {
    const auto found = document_.material_catalog().material_graphs().find(
        std::string(identity));
    if (found == document_.material_catalog().material_graphs().end()) {
        return core::Result<project::MaterialGraph>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "material graph source is not present in this project"));
    }
    return core::Result<project::MaterialGraph>::success(found->second);
}

core::Result<RoadSurfacePreview> ApplicationSession::road_surface_preview(
    std::string_view identity,
    std::uint32_t samples_per_segment) const {
    const auto road = document_.road_sources().find(std::string(identity));
    if (road == document_.road_sources().end()) {
        return core::Result<RoadSurfacePreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "road surface preview requires a road source present in this project"));
    }
    if (auto result = road->second.validate(); !result) {
        return core::Result<RoadSurfacePreview>::failure(
            result.error().with_context("road surface preview source"));
    }
    const auto centerline = document_.curve_paths().find(road->second.centerline_identity);
    if (centerline == document_.curve_paths().end()) {
        return core::Result<RoadSurfacePreview>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road surface preview centerline is no longer present"));
    }
    if (centerline->second.source_identity != road->second.centerline_identity ||
        centerline->second.source_revision != road->second.centerline_revision) {
        return core::Result<RoadSurfacePreview>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road surface preview requires the exact authored centerline revision"));
    }
    const auto surface = geometry::build_road_surface(centerline->second,
        road->second.width_meters, road->second.shoulder_meters,
        road->second.maximum_grade_ratio, road->second.minimum_curve_radius_meters,
        samples_per_segment);
    if (!surface) {
        return core::Result<RoadSurfacePreview>::failure(
            surface.error().with_context("road surface design preview"));
    }
    const std::string encoded_centerline = centerline->second.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_centerline.data());
    const auto centerline_digest = assets::sha256(
        std::span<const std::uint8_t>(bytes, encoded_centerline.size()));
    return core::Result<RoadSurfacePreview>::success(RoadSurfacePreview{
        road->first, road->second.source_revision, project_generation_,
        road->second.source_digest(), centerline_digest,
        road->second.surface_class, std::move(surface).value()});
}

core::Result<void> RoadTerrainPreview::validate() const {
    const std::uint64_t cell_count = static_cast<std::uint64_t>(terrain_width) *
        terrain_height;
    if (project_generation == 0U ||
        road_surface.project_generation != project_generation ||
        road_surface.road_identity.empty() ||
        road_surface.road_revision.value() == 0U ||
        road_surface.road_digest.is_zero() ||
        road_surface.centerline_digest.is_zero() ||
        !road_surface.mesh.validate() || terrain_identity.empty() ||
        terrain_revision.value() == 0U || terrain_digest.is_zero() ||
        terrain_semantic != spatial_field::FieldSemantic::terrain_elevation ||
        !terrain::parse_terrain_tile_identity(terrain_identity) ||
        !terrain_origin_xz.finite() ||
        !std::isfinite(terrain_cell_size_meters) || terrain_cell_size_meters <= 0.0 ||
        terrain_width < 2U || terrain_height < 2U ||
        terrain_width > spatial_field::ScalarField2D::kMaxAxisSamples ||
        terrain_height > spatial_field::ScalarField2D::kMaxAxisSamples ||
        cell_count > kMaxCells ||
        coordinate_frame != RoadTerrainCoordinateFrame::world_xz_elevation_y ||
        candidate_grid_tests == 0U || candidate_grid_tests > kMaxCandidateGridTests ||
        cells.empty() || cells.size() > cell_count ||
        !std::isfinite(cut_volume_cubic_meters) || cut_volume_cubic_meters < 0.0 ||
        !std::isfinite(fill_volume_cubic_meters) || fill_volume_cubic_meters < 0.0 ||
        preview_digest.is_zero()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "road/terrain preview header, source receipt, or bounded arrays are invalid"));
    }

    std::vector<std::uint8_t> visited(static_cast<std::size_t>(cell_count), 0U);
    double expected_cut = 0.0;
    double expected_fill = 0.0;
    for (const auto& cell : cells) {
        if (cell.u >= terrain_width || cell.v >= terrain_height ||
            !cell.position.finite() ||
            !std::isfinite(cell.terrain_elevation_meters) ||
            !std::isfinite(cell.design_elevation_meters) ||
            !std::isfinite(cell.signed_cut_depth_meters) ||
            !std::isfinite(cell.represented_area_square_meters) ||
            cell.represented_area_square_meters <= 0.0) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "road/terrain preview contains an invalid sample"));
        }
        const std::size_t index = static_cast<std::size_t>(cell.v) * terrain_width + cell.u;
        if (visited[index] != 0U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "road/terrain preview repeats a terrain sample"));
        }
        visited[index] = 1U;
        const double expected_signed_depth =
            cell.terrain_elevation_meters - cell.design_elevation_meters;
        if (cell.signed_cut_depth_meters != expected_signed_depth) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "road/terrain preview cut/fill sign does not match its elevations"));
        }
        if (expected_signed_depth > 0.0) {
            expected_cut += expected_signed_depth * cell.represented_area_square_meters;
        } else if (expected_signed_depth < 0.0) {
            expected_fill += -expected_signed_depth * cell.represented_area_square_meters;
        }
    }
    if (expected_cut != cut_volume_cubic_meters ||
        expected_fill != fill_volume_cubic_meters ||
        road_terrain_preview_digest(*this) != preview_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "road/terrain preview totals or canonical digest do not match its samples"));
    }
    return core::Result<void>::success();
}

bool RoadTerrainPreview::matches_sources(
    std::uint64_t current_project_generation,
    const RoadSourceView& road,
    const CurvePathSourceView& centerline,
    const SpatialFieldView& terrain_source) const noexcept {
    return current_project_generation == project_generation &&
        road_surface.project_generation == project_generation &&
        road.identity == road_surface.road_identity &&
        road.source_revision == road_surface.road_revision &&
        road.digest == road_surface.road_digest &&
        road.surface_class == road_surface.surface_class &&
        road.centerline_identity == road_surface.mesh.centerline_identity &&
        road.centerline_revision == road_surface.mesh.centerline_revision &&
        road.width_meters == road_surface.mesh.road_width_meters &&
        road.shoulder_meters == road_surface.mesh.shoulder_width_meters &&
        road.maximum_grade_ratio == road_surface.mesh.maximum_grade_ratio &&
        road.minimum_curve_radius_meters == road_surface.mesh.minimum_curve_radius_meters &&
        centerline.identity == road_surface.mesh.centerline_identity &&
        centerline.source_revision == road_surface.mesh.centerline_revision &&
        centerline.digest == road_surface.centerline_digest &&
        terrain_source.identity == terrain_identity &&
        terrain_source.semantic == terrain_semantic &&
        terrain_source.source_revision == terrain_revision &&
        terrain_source.digest == terrain_digest &&
        terrain_source.width == terrain_width &&
        terrain_source.height == terrain_height;
}

bool RoadTerrainPreview::matches(
    std::uint64_t current_project_generation,
    const RoadSourceView& road,
    const CurvePathSourceView& centerline,
    const SpatialFieldView& terrain_source) const {
    return matches_sources(current_project_generation, road, centerline, terrain_source) &&
        static_cast<bool>(validate());
}

core::Result<RoadTerrainPreview> ApplicationSession::road_terrain_preview(
    std::string_view road_identity,
    std::string_view terrain_identity,
    RoadTerrainCoordinateFrame coordinate_frame,
    std::uint32_t samples_per_segment) const {
    if (coordinate_frame != RoadTerrainCoordinateFrame::world_xz_elevation_y) {
        return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "road/terrain preview requires an explicitly supported X/Z and Y-elevation frame"));
    }
    const auto terrain_source = document_.spatial_fields().find(
        std::string(terrain_identity));
    if (terrain_source == document_.spatial_fields().end()) {
        return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "road/terrain preview requires a terrain source present in this project"));
    }
    if (auto valid = terrain_source->second.validate(); !valid) {
        return core::Result<RoadTerrainPreview>::failure(
            valid.error().with_context("road/terrain elevation source"));
    }
    if (terrain_source->second.semantic() !=
            spatial_field::FieldSemantic::terrain_elevation ||
        !terrain::parse_terrain_tile_identity(terrain_source->second.identity())) {
        return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "road/terrain preview requires a canonical terrain_elevation tile"));
    }
    const std::uint64_t terrain_cell_count =
        static_cast<std::uint64_t>(terrain_source->second.width()) *
        terrain_source->second.height();
    if (terrain_cell_count > RoadTerrainPreview::kMaxCells) {
        return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "road/terrain preview exceeds its bounded terrain-sample budget"));
    }

    const auto road_surface = road_surface_preview(road_identity, samples_per_segment);
    if (!road_surface) {
        return core::Result<RoadTerrainPreview>::failure(
            road_surface.error().with_context("road/terrain road source"));
    }
    const auto& field = terrain_source->second;
    const double minimum_x = field.origin_uv().x;
    const double minimum_z = field.origin_uv().y;
    const double maximum_x = minimum_x +
        static_cast<double>(field.width() - 1U) * field.cell_size();
    const double maximum_z = minimum_z +
        static_cast<double>(field.height() - 1U) * field.cell_size();
    const double coordinate_tolerance = 1.0e-9 * std::max({
        1.0, std::abs(minimum_x), std::abs(minimum_z),
        std::abs(maximum_x), std::abs(maximum_z)});
    for (const auto& station : road_surface.value().mesh.stations) {
        for (const auto point : {station.shoulder_left, station.shoulder_right}) {
            if (point.x < minimum_x - coordinate_tolerance ||
                point.x > maximum_x + coordinate_tolerance ||
                point.z < minimum_z - coordinate_tolerance ||
                point.z > maximum_z + coordinate_tolerance) {
                return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "road/terrain preview rejects a road corridor outside terrain coverage"));
            }
        }
    }

    const std::uint32_t width = field.width();
    const std::uint32_t height = field.height();
    const std::size_t sample_count = static_cast<std::size_t>(width) * height;
    const double corridor_radius =
        road_surface.value().mesh.road_width_meters * 0.5 +
        road_surface.value().mesh.shoulder_width_meters;
    const double corridor_radius_squared = corridor_radius * corridor_radius;
    std::vector<double> nearest_distance_squared(
        sample_count, std::numeric_limits<double>::infinity());
    std::vector<double> design_elevation(sample_count, 0.0);
    std::uint64_t candidate_grid_tests = 0U;

    const auto index_range = [](double segment_minimum,
                                double segment_maximum,
                                double field_origin,
                                double cell_size,
                                std::uint32_t count)
        -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
        const double first_raw = (segment_minimum - field_origin) / cell_size;
        const double last_raw = (segment_maximum - field_origin) / cell_size;
        if (!std::isfinite(first_raw) || !std::isfinite(last_raw)) return std::nullopt;
        const double first = std::ceil(first_raw - 1.0e-12);
        const double last = std::floor(last_raw + 1.0e-12);
        if (last < 0.0 || first > static_cast<double>(count - 1U)) return std::nullopt;
        const auto clamped_first = static_cast<std::uint32_t>(
            std::clamp(first, 0.0, static_cast<double>(count - 1U)));
        const auto clamped_last = static_cast<std::uint32_t>(
            std::clamp(last, 0.0, static_cast<double>(count - 1U)));
        if (clamped_first > clamped_last) return std::nullopt;
        return std::pair{clamped_first, clamped_last};
    };

    const auto& stations = road_surface.value().mesh.stations;
    for (std::size_t segment_index = 0U; segment_index + 1U < stations.size();
         ++segment_index) {
        const auto& first = stations[segment_index].center;
        const auto& last = stations[segment_index + 1U].center;
        const auto u_range = index_range(
            std::min(first.x, last.x) - corridor_radius,
            std::max(first.x, last.x) + corridor_radius,
            minimum_x, field.cell_size(), width);
        const auto v_range = index_range(
            std::min(first.z, last.z) - corridor_radius,
            std::max(first.z, last.z) + corridor_radius,
            minimum_z, field.cell_size(), height);
        if (!u_range.has_value() || !v_range.has_value()) continue;
        const double segment_x = last.x - first.x;
        const double segment_z = last.z - first.z;
        const double segment_length_squared =
            segment_x * segment_x + segment_z * segment_z;
        if (!std::isfinite(segment_length_squared) || segment_length_squared <= 1.0e-20) {
            return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "road/terrain preview encountered a degenerate centerline interval"));
        }
        for (std::uint32_t v = v_range->first; v <= v_range->second; ++v) {
            const double point_z = minimum_z + static_cast<double>(v) * field.cell_size();
            for (std::uint32_t u = u_range->first; u <= u_range->second; ++u) {
                if (candidate_grid_tests == RoadTerrainPreview::kMaxCandidateGridTests) {
                    return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
                        core::ErrorCode::validation_failed,
                        "road/terrain preview exceeds its bounded corridor evaluation budget"));
                }
                ++candidate_grid_tests;
                const double point_x = minimum_x + static_cast<double>(u) * field.cell_size();
                const double projection = std::clamp(
                    ((point_x - first.x) * segment_x +
                     (point_z - first.z) * segment_z) / segment_length_squared,
                    0.0, 1.0);
                const double nearest_x = first.x + projection * segment_x;
                const double nearest_z = first.z + projection * segment_z;
                const double dx = point_x - nearest_x;
                const double dz = point_z - nearest_z;
                const double distance_squared = dx * dx + dz * dz;
                if (!std::isfinite(distance_squared) ||
                    distance_squared > corridor_radius_squared) {
                    continue;
                }
                const std::size_t sample_index = static_cast<std::size_t>(v) * width + u;
                if (distance_squared < nearest_distance_squared[sample_index]) {
                    nearest_distance_squared[sample_index] = distance_squared;
                    design_elevation[sample_index] = first.y +
                        projection * (last.y - first.y);
                }
            }
        }
    }

    const auto terrain_digest = field.canonical_digest();
    if (!terrain_digest) {
        return core::Result<RoadTerrainPreview>::failure(terrain_digest.error());
    }
    RoadTerrainPreview preview;
    preview.project_generation = project_generation_;
    preview.road_surface = road_surface.value();
    preview.terrain_identity = field.identity();
    preview.terrain_revision = field.source_revision();
    preview.terrain_digest = terrain_digest.value();
    preview.terrain_semantic = field.semantic();
    preview.terrain_origin_xz = field.origin_uv();
    preview.terrain_cell_size_meters = field.cell_size();
    preview.terrain_width = width;
    preview.terrain_height = height;
    preview.coordinate_frame = coordinate_frame;
    preview.candidate_grid_tests = candidate_grid_tests;
    preview.cells.reserve(sample_count);
    for (std::uint32_t v = 0U; v < height; ++v) {
        const double point_z = minimum_z + static_cast<double>(v) * field.cell_size();
        const double edge_v_weight = (v == 0U || v + 1U == height) ? 0.5 : 1.0;
        for (std::uint32_t u = 0U; u < width; ++u) {
            const std::size_t sample_index = static_cast<std::size_t>(v) * width + u;
            if (!std::isfinite(nearest_distance_squared[sample_index])) continue;
            const double terrain_elevation = field.values()[sample_index];
            const double cut_depth = terrain_elevation - design_elevation[sample_index];
            const double edge_u_weight = (u == 0U || u + 1U == width) ? 0.5 : 1.0;
            const double represented_area = field.cell_size() * field.cell_size() *
                edge_u_weight * edge_v_weight;
            preview.cells.push_back(RoadTerrainCell{
                u, v,
                {minimum_x + static_cast<double>(u) * field.cell_size(),
                 terrain_elevation, point_z},
                terrain_elevation, design_elevation[sample_index], cut_depth,
                represented_area});
            if (cut_depth > 0.0) {
                preview.cut_volume_cubic_meters += cut_depth * represented_area;
            } else if (cut_depth < 0.0) {
                preview.fill_volume_cubic_meters += -cut_depth * represented_area;
            }
        }
    }
    if (preview.cells.empty()) {
        return core::Result<RoadTerrainPreview>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "road corridor overlaps terrain coverage but contains no terrain samples"));
    }
    preview.preview_digest = road_terrain_preview_digest(preview);
    if (auto valid = preview.validate(); !valid) {
        return core::Result<RoadTerrainPreview>::failure(valid.error());
    }
    return core::Result<RoadTerrainPreview>::success(std::move(preview));
}

core::Result<geometry::SweptMeshProduct> ApplicationSession::sweep_source_preview(
    std::string_view identity) const {
    const auto source = document_.sweep_sources().find(std::string(identity));
    if (source == document_.sweep_sources().end()) {
        return core::Result<geometry::SweptMeshProduct>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "sweep preview requires a source present in this project"));
    }
    const auto centerline = document_.curve_paths().find(source->second.centerline_identity);
    if (centerline == document_.curve_paths().end() ||
        centerline->second.source_revision != source->second.centerline_revision) {
        return core::Result<geometry::SweptMeshProduct>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep preview requires the exact authored centerline revision"));
    }
    return geometry::build_sweep_mesh(source->second, centerline->second);
}

core::Result<production::DecalProjection> ApplicationSession::decal_placement_preview(
    const production::DecalPlacement& placement,
    core::Revision expected_project_revision) const {
    if (document_.revision() != expected_project_revision) {
        return core::Result<production::DecalProjection>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "decal preview targets a stale project revision"));
    }
    if (auto result = placement.validate(); !result) {
        return core::Result<production::DecalProjection>::failure(
            result.error().with_context("decal placement preview"));
    }
    const auto material = document_.material_catalog().materials().find(placement.material_id);
    if (material == document_.material_catalog().materials().end()) {
        return core::Result<production::DecalProjection>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "decal preview requires a material present in the project catalog"));
    }
    if (auto result = document_.material_catalog().resolve_material(placement.material_id);
        !result) {
        return core::Result<production::DecalProjection>::failure(
            result.error().with_context("decal material binding"));
    }
    const auto mesh = document_.meshes().find(placement.target_mesh_asset);
    if (mesh == document_.meshes().end()) {
        return core::Result<production::DecalProjection>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "decal preview target mesh is not present in the project"));
    }
    const auto mesh_digest = canonical_mesh_geometry_digest(mesh->second);
    if (auto result = production::validate_decal_source(
            placement, placement.target_mesh_asset, mesh->second.revision(), mesh_digest);
        !result) {
        return core::Result<production::DecalProjection>::failure(
            result.error().with_context("decal preview source binding"));
    }
    return production::project_decal(
        placement, mesh->second, placement.target_mesh_asset, mesh_digest);
}

core::Result<geometry::CompiledMesh> ApplicationSession::decal_placement_source_mesh(
    const production::DecalPlacement& placement,
    core::Revision expected_project_revision) const {
    using SourceMeshResult = core::Result<geometry::CompiledMesh>;
    if (document_.revision() != expected_project_revision) {
        return SourceMeshResult::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "decal source mesh read targets a stale project revision"));
    }
    if (auto result = placement.validate(); !result) {
        return SourceMeshResult::failure(result.error().with_context(
            "decal source mesh read"));
    }
    const auto mesh = document_.meshes().find(placement.target_mesh_asset);
    if (mesh == document_.meshes().end()) {
        return SourceMeshResult::failure(core::Diagnostic(core::ErrorCode::not_found,
            "decal source mesh is not present in the project"));
    }
    const auto digest = canonical_mesh_geometry_digest(mesh->second);
    if (auto result = production::validate_decal_source(
            placement, placement.target_mesh_asset, mesh->second.revision(), digest);
        !result) {
        return SourceMeshResult::failure(result.error().with_context(
            "decal source mesh binding"));
    }
    auto compiled = mesh->second.compile();
    if (!compiled) return SourceMeshResult::failure(compiled.error().with_context(
        "decal source mesh compilation"));
    if (!compiled.value().valid() ||
        compiled.value().source_revision != placement.target_mesh_revision) {
        return SourceMeshResult::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "decal source mesh compilation does not match its bound revision"));
    }
    if (document_.revision() != expected_project_revision) {
        return SourceMeshResult::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "project changed while the decal source mesh was compiled"));
    }
    return SourceMeshResult::success(std::move(compiled.value()));
}

core::Result<production::DecalPlacement> ApplicationSession::make_decal_placement(
    scene::ObjectId target_object,
    std::uint64_t material_id,
    core::Vec3d center,
    core::Vec3d normal,
    core::Vec3d tangent,
    double width,
    double height,
    double depth,
    double minimum_normal_dot,
    core::Revision expected_project_revision) const {
    if (document_.revision() != expected_project_revision) {
        return core::Result<production::DecalPlacement>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "decal placement draft targets a stale project revision"));
    }
    const auto* object = document_.scene().find(target_object);
    if (object == nullptr || !object->visible || object->locked ||
        !object->mesh_asset.has_value()) {
        return core::Result<production::DecalPlacement>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "decal placement requires a visible, unlocked mesh object in this project"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return core::Result<production::DecalPlacement>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "decal placement target mesh is not present in this project"));
    }
    const auto material = document_.material_catalog().materials().find(material_id);
    if (material == document_.material_catalog().materials().end()) {
        return core::Result<production::DecalPlacement>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "decal placement material is not present in the project catalog"));
    }
    if (auto result = document_.material_catalog().resolve_material(material_id); !result) {
        return core::Result<production::DecalPlacement>::failure(
            result.error().with_context("decal placement material binding"));
    }

    std::uint64_t placement_id = 1U;
    for (const auto& [stored_id, stored] : document_.decal_placements()) {
        static_cast<void>(stored);
        if (stored_id < placement_id) continue;
        if (stored_id > placement_id) break;
        if (placement_id == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<production::DecalPlacement>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "decal placement identity space is exhausted"));
        }
        ++placement_id;
    }

    production::DecalPlacement placement;
    placement.id = placement_id;
    placement.revision = core::Revision{1U};
    placement.target_mesh_asset = *object->mesh_asset;
    placement.target_mesh_revision = mesh->second.revision();
    placement.target_mesh_digest = canonical_mesh_geometry_digest(mesh->second);
    placement.material_id = material_id;
    placement.center = center;
    placement.normal = normal;
    placement.tangent = tangent;
    placement.width = width;
    placement.height = height;
    placement.depth = depth;
    placement.minimum_normal_dot = minimum_normal_dot;
    if (auto result = placement.validate(); !result) {
        return core::Result<production::DecalPlacement>::failure(
            result.error().with_context("decal placement draft"));
    }
    const auto preview = decal_placement_preview(placement, expected_project_revision);
    if (!preview) {
        return core::Result<production::DecalPlacement>::failure(preview.error());
    }
    if (preview.value().polygons.empty()) {
        return core::Result<production::DecalPlacement>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "decal placement does not project onto an admitted target face"));
    }
    return core::Result<production::DecalPlacement>::success(std::move(placement));
}

core::Result<biological::BiologicalAuthoringDocument>
ApplicationSession::biological_authoring_source(
    const project::BiologicalAuthoringKey& key,
    core::Revision expected_project_revision) const {
    if (document_.revision() != expected_project_revision) {
        return core::Result<biological::BiologicalAuthoringDocument>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "biological authoring read targets a stale project revision"));
    }
    const auto stored = document_.biological_authoring_sources().find(key);
    if (stored == document_.biological_authoring_sources().end()) {
        return core::Result<biological::BiologicalAuthoringDocument>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "biological authoring source is not present in this project"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(stored->second.bytes.data());
    const auto digest = assets::sha256(std::span<const std::uint8_t>{
        bytes, stored->second.bytes.size()});
    if (digest != stored->second.digest) {
        return core::Result<biological::BiologicalAuthoringDocument>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "biological authoring payload digest is inconsistent"));
    }
    auto source = biological::BiologicalAuthoringDocument::deserialize(stored->second.bytes);
    if (!source) {
        return core::Result<biological::BiologicalAuthoringDocument>::failure(
            source.error().with_context("biological authoring source read"));
    }
    if (source.value().source.source_namespace != key.first ||
        source.value().source.asset_id != key.second) {
        return core::Result<biological::BiologicalAuthoringDocument>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "biological authoring source identity does not match its project key"));
    }
    return source;
}

core::Result<biological::ProxyMeshProduct> ApplicationSession::biological_proxy_preview(
    const project::BiologicalAuthoringKey& key,
    core::Revision expected_project_revision,
    biological::StructureId structure) const {
    const auto source = biological_authoring_source(key, expected_project_revision);
    if (!source) {
        return core::Result<biological::ProxyMeshProduct>::failure(source.error());
    }
    auto product = biological::compile_ellipsoid_proxy(source.value(), structure);
    if (!product) return product;
    if (document_.revision() != expected_project_revision) {
        return core::Result<biological::ProxyMeshProduct>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "project changed while the biological proxy preview was generated"));
    }
    return product;
}


core::Result<core::Revision> ApplicationSession::next_curve_path_revision(
    std::string_view identity) const {
    const auto lineage = document_.curve_path_lineage_.find(std::string(identity));
    if (lineage == document_.curve_path_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "curve source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<core::Revision> ApplicationSession::next_road_source_revision(
    std::string_view identity) const {
    const auto lineage = document_.road_source_lineage_.find(std::string(identity));
    if (lineage == document_.road_source_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "road source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<core::Revision> ApplicationSession::next_sweep_source_revision(
    std::string_view identity) const {
    const auto lineage = document_.sweep_source_lineage_.find(std::string(identity));
    if (lineage == document_.sweep_source_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "sweep source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<core::Revision> ApplicationSession::next_biological_authoring_revision(
    const project::BiologicalAuthoringKey& key) const {
    const auto lineage = document_.biological_authoring_lineage_.find(key);
    if (lineage == document_.biological_authoring_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "biological authoring source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<core::Revision> ApplicationSession::next_sign_text_source_revision(
    std::string_view identity) const {
    const auto lineage = document_.sign_text_source_lineage_.find(std::string(identity));
    if (lineage == document_.sign_text_source_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "sign-text source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<editor::AuthoringPreview> ApplicationSession::begin_preview(
    editor::PreviewKind kind) const {
    auto context = authoring_context();
    if (!context) {
        return core::Result<editor::AuthoringPreview>::failure(context.error());
    }
    return editor::AuthoringPreview::begin(document_.revision(), context.value(), kind);
}

core::Result<ApplicationAction> ApplicationSession::preview_action(
    const editor::AuthoringPreview& preview) const {
    const auto& context = preview.context();
    const auto& parameters = preview.parameters();
    if (preview.kind() == editor::PreviewKind::object_transform) {
        if (!parameters.transform.has_value() || context.objects.size() != 1U) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "object transform preview has no complete action payload"));
        }
        return core::Result<ApplicationAction>::success(SetObjectTransformAction{
            context.objects.front(), *parameters.transform, context.project_revision});
    }
    if (preview.kind() == editor::PreviewKind::object_translation_delta) {
        if (!parameters.position.has_value() || context.objects.size() != 1U) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "object translation preview has no complete action payload"));
        }
        const auto* object = document_.scene().find(context.objects.front());
        if (object == nullptr) {
            return core::Result<ApplicationAction>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "object translation preview target no longer exists"));
        }
        core::Transform translated = object->local_transform;
        translated.translation = translated.translation + *parameters.position;
        if (!translated.finite()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "object translation preview produced a non-finite transform"));
        }
        return core::Result<ApplicationAction>::success(SetObjectTransformAction{
            context.objects.front(), translated, context.project_revision});
    }
    if (preview.kind() == editor::PreviewKind::extrude_face) {
        if (!parameters.distance.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face extrusion preview has no distance"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.extrude-face", editor::ToolArguments{
                *parameters.distance, {}, true, 0.5, std::nullopt}});
    }
    if (preview.kind() == editor::PreviewKind::inset_face) {
        if (!parameters.distance.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face inset preview has no distance"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.inset-face", editor::ToolArguments{
                *parameters.distance, {}, true, 0.5, std::nullopt}});
    }
    if (preview.kind() == editor::PreviewKind::poke_face) {
        if (parameters.transform.has_value() || parameters.distance.has_value() ||
            parameters.position.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face poke preview contains unexpected parameters"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.poke-face", editor::ToolArguments{}});
    }
    if (preview.kind() == editor::PreviewKind::set_material_region_assignment) {
        if (context.objects.size() != 1U ||
            !parameters.material_region_id.has_value() ||
            !parameters.material_id.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "material assignment preview has no complete object and ID payload"));
        }
        const auto* object = document_.scene().find(context.objects.front());
        if (object == nullptr || !object->mesh_asset.has_value()) {
            return core::Result<ApplicationAction>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material assignment preview target no longer has an authored mesh"));
        }
        std::optional<project::MaterialVariantId> variant;
        if (parameters.material_variant_id.has_value()) {
            variant = static_cast<project::MaterialVariantId>(
                *parameters.material_variant_id);
        }
        return core::Result<ApplicationAction>::success(SetMaterialRegionAssignmentAction{
            context.project_revision,
            {*object->mesh_asset, *parameters.material_region_id},
            static_cast<render::MaterialId>(*parameters.material_id), variant});
    }
    if (preview.kind() == editor::PreviewKind::generate_unique_lightmap_uv) {
        if (context.objects.size() != 1U || !parameters.lightmap_uv_set_id.has_value() ||
            !parameters.lightmap_resolution.has_value() ||
            !parameters.lightmap_padding_texels.has_value() ||
            !parameters.lightmap_maximum_udim_tiles.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "lightmap UV preview has no complete target and parameter payload"));
        }
        return core::Result<ApplicationAction>::success(GenerateUniqueLightmapUvAction{
            context.project_revision,
            context.objects.front(),
            *parameters.lightmap_uv_set_id,
            *parameters.lightmap_resolution,
            *parameters.lightmap_padding_texels,
            *parameters.lightmap_maximum_udim_tiles});
    }
    if (preview.kind() != editor::PreviewKind::set_vertex_position) {
        return core::Result<ApplicationAction>::failure(invalid_state(
            "preview kind has no native application action"));
    }
    if (!parameters.position.has_value()) {
        return core::Result<ApplicationAction>::failure(invalid_state(
            "vertex position preview has no position"));
    }
    return core::Result<ApplicationAction>::success(InvokeToolAction{
        "mesh.set-vertex-position", editor::ToolArguments{
            0.0, *parameters.position, true, 0.5, std::nullopt}});
}

core::Result<DispatchReceipt> ApplicationSession::commit_preview(
    HumanActionAdmission admission,
    editor::AuthoringPreview& preview,
    OperationSource source,
    std::string_view provenance_id) {
    if (auto result = preview.validate_commit(document_.revision()); !result) {
        return failure(result.error().with_context("preview commit"));
    }
    auto current_context = authoring_context();
    if (!current_context) {
        return failure(current_context.error().with_context("preview commit context"));
    }
    if (current_context.value() != preview.context()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "authoring preview context no longer matches the current selection"));
    }
    auto action = preview_action(preview);
    if (!action) {
        return failure(action.error().with_context("preview commit action"));
    }
    auto result = dispatch(admission, action.value(), source, provenance_id);
    if (result) {
        preview.mark_committed();
    }
    return result;
}

core::Result<void> ApplicationSession::set_workspace(WorkspaceState workspace) {
    if (auto result = workspace.validate(); !result) {
        return result;
    }
    workspace_ = std::move(workspace);
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::new_project(const NewProjectAction& action) {
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    auto created = project::ProjectDocument::create(action.name);
    if (!created) {
        return failure(created.error());
    }
    const core::Revision before = document_.revision();
    document_ = std::move(created.value());
    viewport_mesh_cache_.clear();
    project_path_.reset();
    journal_.reset();
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    return accepted("New Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::open_project(const OpenProjectAction& action) {
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    if (action.options.auto_recover) {
        if (active_source_ != OperationSource::human) {
            return failure(invalid_state(
                "automatic startup recovery requires an explicit human application action"));
        }
        if (document_.revision() != saved_revision_) {
            return failure(invalid_state(
                "automatic startup recovery refuses to discard unsaved changes; "
                "use the explicit recovery action"));
        }
    }
    project::FileLock project_lock(action.path);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project read lock"));
    }
    if (action.options.auto_recover) {
        // The held writer lease above serializes this classification with
        // save, mutation publication, and explicit recovery, so the evidence
        // cannot change between inspection and apply.
        auto inspection = inspect_recovery_unlocked(action.path);
        if (!inspection) {
            return failure(inspection.error().with_context("automatic project recovery"));
        }
        if (inspection.value().state == RecoveryInspectionState::blocked) {
            return failure(inspection.value().diagnostic.value_or(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "recovery evidence is blocked")));
        }
        if (inspection.value().state == RecoveryInspectionState::pending) {
            // Replay through the exact same primitives as the explicit human
            // recovery action so both paths converge on identical document
            // state. Session state is replaced only after the journal and
            // project evidence are durably consistent.
            journal::Journal journal(
                journal_path_for(action.path), application_journal_limits());
            const auto recovered = project::recover_latest_snapshot(journal);
            if (!recovered) {
                return failure(recovered.error().with_context("automatic project recovery"));
            }
            auto prepared_journal =
                prepare_journal(action.path, recovered.value().document);
            if (!prepared_journal) {
                return failure(prepared_journal.error().with_context(
                    "automatic project recovery journal"));
            }
            const auto saved = recovered.value().document.save_atomic_unlocked(action.path);
            if (!saved) {
                return failure(saved.error().with_context("automatic project recovery save"));
            }

            const core::Revision before = document_.revision();
            document_ = recovered.value().document;
            viewport_mesh_cache_.clear();
            project_path_ = action.path;
            journal_ = std::move(prepared_journal.value().journal);
            reset_editor_state();
            saved_revision_ = document_.revision();
            if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
                project_generation_ = 0;
            } else {
                ++project_generation_;
            }
            const OperationSource previous_source = active_source_;
            active_source_ = OperationSource::recovery;
            auto receipt = accepted("Open Project", before);
            active_source_ = previous_source;
            if (receipt) {
                // Recovery can restore a revision already present in the
                // session while still replacing the complete document state.
                receipt.value().document_changed = true;
                receipt.value().automatic_recovery_applied = true;
            }
            return receipt;
        }
    }
    auto loaded = project::ProjectDocument::load(action.path);
    if (!loaded) {
        return failure(loaded.error().with_context("open project"));
    }
    auto prepared_journal = prepare_journal(action.path, loaded.value());
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("open project journal"));
    }
    const core::Revision before = document_.revision();
    document_ = std::move(loaded.value());
    viewport_mesh_cache_.clear();
    project_path_ = action.path;
    journal_ = std::move(prepared_journal.value().journal);
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    return accepted("Open Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::recover_project(
    const RecoverProjectAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(invalid_state(
            "journal recovery requires an explicit human application action"));
    }
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    if (action.path.empty() || action.path.filename().empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery requires a project file path"));
    }

    project::FileLock project_lock(action.path);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project recovery lock"));
    }
    journal::Journal journal(journal_path_for(action.path), application_journal_limits());
    const auto recovered = project::recover_latest_snapshot(journal);
    if (!recovered) {
        return failure(recovered.error().with_context("recover project"));
    }
    auto prepared_journal = prepare_journal(action.path, recovered.value().document);
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("recover project journal"));
    }
    const auto saved = recovered.value().document.save_atomic_unlocked(action.path);
    if (!saved) {
        return failure(saved.error().with_context("recover project save"));
    }

    const core::Revision before = document_.revision();
    document_ = recovered.value().document;
    viewport_mesh_cache_.clear();
    project_path_ = action.path;
    journal_ = std::move(prepared_journal.value().journal);
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    const OperationSource previous_source = active_source_;
    active_source_ = OperationSource::recovery;
    auto receipt = accepted("Recover Project", before);
    active_source_ = previous_source;
    if (receipt) {
        // Recovery can restore a revision already present in the active
        // session while still replacing the complete document state.
        receipt.value().document_changed = true;
    }
    return receipt;
}

core::Result<void> ApplicationSession::can_close(bool discard_dirty) const {
    return require_discard_confirmation(discard_dirty);
}

core::Result<void> ApplicationSession::require_discard_confirmation(bool discard_dirty) const {
    if (!discard_dirty && document_.revision() != saved_revision_) {
        return core::Result<void>::failure(invalid_state(
            "unsaved project changes require explicit discard confirmation"));
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::save_project(const SaveProjectAction& action) {
    const std::filesystem::path* selected = nullptr;
    if (action.path.has_value()) {
        selected = &action.path.value();
    } else if (project_path_.has_value()) {
        selected = &project_path_.value();
    }
    if (!selected) {
        return failure(invalid_state("save requires a project path"));
    }
    project::FileLock project_lock(*selected);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project write lock"));
    }
    auto prepared_journal = prepare_journal(*selected, document_);
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("save project journal"));
    }
    const auto saved = document_.save_atomic_unlocked(*selected);
    if (!saved) {
        std::error_code cleanup_error;
        if (prepared_journal.value().created_file) {
            std::filesystem::remove(prepared_journal.value().journal.path(), cleanup_error);
        } else if (prepared_journal.value().appended_baseline) {
            std::filesystem::resize_file(
                prepared_journal.value().journal.path(),
                prepared_journal.value().original_bytes,
                cleanup_error);
        }
        if (cleanup_error) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project save failed and durable journal rollback failed: " +
                    cleanup_error.message()));
        }
        return failure(saved.error().with_context("save project"));
    }
    const core::Revision before = document_.revision();
    project_path_ = *selected;
    journal_ = std::move(prepared_journal.value().journal);
    saved_revision_ = document_.revision();
    return accepted("Save Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::execute_command(
    std::unique_ptr<editor::EditorCommand> command,
    std::string action,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.execute(std::move(command)); !result) {
        return failure(result.error().with_context(action));
    }
    return accept_command_mutation(
        std::move(action), before, std::move(before_document), std::move(affected),
        std::move(parameters));
}

core::Result<DispatchReceipt> ApplicationSession::accept_command_mutation(
    std::string action,
    core::Revision revision_before,
    project::ProjectDocument before_document,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const core::Revision revision_after = document_.revision();
    if (auto result = append_mutation_event(action, revision_before, revision_after); !result) {
        const auto rollback = history_.rollback_last_execute();
        document_ = std::move(before_document);
        viewport_mesh_cache_.clear();
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and command rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("mutation rolled back"));
    }
    return accepted(
        std::move(action), revision_before, std::move(affected), std::move(parameters));
}

core::Result<DispatchReceipt> ApplicationSession::select_object(const SelectObjectAction& action) {
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_object(document_.scene(), action.object, action.operation); !result) {
        return failure(result.error().with_context("object selection"));
    }
    return accepted("Select Object", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_vertex(const SelectVertexAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select a vertex on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_vertex(
            document_.scene(), action.object, mesh->second, action.vertex, action.operation);
        !result) {
        return failure(result.error().with_context("vertex selection"));
    }
    return accepted("Select Vertex", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_face(const SelectFaceAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select a face on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_face(
            document_.scene(), action.object, mesh->second, action.face, action.operation);
        !result) {
        return failure(result.error().with_context("face selection"));
    }
    return accepted("Select Face", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_edge(const SelectEdgeAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select an edge on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_edge(
            document_.scene(), action.object, mesh->second, action.edge, action.operation);
        !result) {
        return failure(result.error().with_context("edge selection"));
    }
    return accepted("Select Edge", before);
}

core::Result<DispatchReceipt> ApplicationSession::convert_selection(
    const ConvertSelectionAction& action) {
    const auto groups = selection_.component_groups();
    if (groups.empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "selection conversion requires an object-bound component selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    for (const auto& group : groups) {
        const auto* object = document_.scene().find(group.object);
        if (!object) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selection conversion owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return failure(invalid_state(
                "selection conversion owner has no mesh asset"));
        }
        const auto mesh = document_.meshes().find(*object->mesh_asset);
        if (mesh == document_.meshes().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selection conversion owner references a missing mesh"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.convert_mode(action.mode, document_.scene(), owner_meshes); !result) {
        return failure(result.error().with_context("selection conversion"));
    }
    return accepted("Convert Selection", before);
}

core::Result<DispatchReceipt> ApplicationSession::expand_selection(
    const ExpandSelectionAction& action) {
    const auto groups = selection_.component_groups();
    if (groups.empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "selection expansion requires an object-bound component selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    for (const auto& group : groups) {
        const auto* object = document_.scene().find(group.object);
        if (!object) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selection expansion owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return failure(invalid_state(
                "selection expansion owner has no mesh asset"));
        }
        const auto mesh = document_.meshes().find(*object->mesh_asset);
        if (mesh == document_.meshes().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selection expansion owner references a missing mesh"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.expand(action.expansion, document_.scene(), owner_meshes); !result) {
        return failure(result.error().with_context("selection expansion"));
    }
    return accepted("Expand Selection", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_shortest_path(
    const SelectShortestPathAction& action) {
    if (action.owner.has_value()) {
        const auto groups = selection_.component_groups();
        if (groups.empty()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "owner-scoped shortest-path selection requires an object-bound component selection"));
        }
        std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
        for (const auto& group : groups) {
            const auto* object = document_.scene().find(group.object);
            if (!object) {
                return failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "shortest-path selection owner object is no longer present"));
            }
            if (!object->mesh_asset.has_value()) {
                return failure(invalid_state(
                    "shortest-path selection owner has no mesh asset"));
            }
            const auto mesh = document_.meshes().find(*object->mesh_asset);
            if (mesh == document_.meshes().end()) {
                return failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "shortest-path selection owner references a missing mesh"));
            }
            owner_meshes.emplace(group.object, &mesh->second);
        }
        const core::Revision before = document_.revision();
        if (auto result = selection_.select_shortest_path(
                *action.owner, action.goal, document_.scene(), owner_meshes); !result) {
            return failure(result.error().with_context("owner-scoped shortest-path selection"));
        }
        return accepted("Select Shortest Path", before);
    }

    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "shortest-path selection requires an object-bound component selection"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "shortest-path selection owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state(
            "shortest-path selection owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "shortest-path selection owner references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_shortest_path(action.goal, mesh->second); !result) {
        return failure(result.error().with_context("shortest-path selection"));
    }
    return accepted("Select Shortest Path", before);
}

core::Result<DispatchReceipt> ApplicationSession::invoke_tool(const InvokeToolAction& action) {
    if (action.tool_id == "mesh.knife-cut" &&
        active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "viewport knife cuts require an explicit human action"));
    }
    if ((action.tool_id == "mesh.set-face-normal" ||
         action.tool_id == "mesh.set-edge-sharp") &&
        active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "custom normal and edge-sharp authoring require an explicit human action"));
    }
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    editor::ProjectCommandAdmission admission;
    editor::ToolContext context(admission, document_, selection_);
    AffectedSet affected;
    std::optional<ParameterPayload> parameters;
    std::optional<editor::AuthoringContext> invocation_context;
    if (auto authoring = authoring_context(); authoring) {
        invocation_context = authoring.value();
        affected.objects = authoring.value().objects;
        affected.vertices = authoring.value().vertices;
        affected.edges = authoring.value().edges;
        affected.faces = authoring.value().faces;
        if (authoring.value().component_object.has_value()) {
            affected.objects.push_back(*authoring.value().component_object);
        }
        const auto selected_component_groups = selection_.component_groups();
        const bool affects_grouped_edges = action.tool_id == "mesh.dissolve-edge" ||
            action.tool_id == "mesh.bevel-edge" || action.tool_id == "mesh.loop-cut" ||
            action.tool_id == "mesh.set-edge-sharp";
        const bool affects_grouped_vertices = action.tool_id == "mesh.bevel-vertex" ||
            action.tool_id == "mesh.set-vertex-position" ||
            action.tool_id == "mesh.slide-vertex" ||
            action.tool_id == "mesh.merge-vertices" ||
            action.tool_id == "mesh.merge-by-distance";
        const bool affects_grouped_faces = action.tool_id == "mesh.poke-face" ||
            action.tool_id == "mesh.inset-face" ||
            action.tool_id == "mesh.extrude-face" ||
            action.tool_id == "mesh.set-face-normal" ||
            action.tool_id == "mesh.tri-to-quad" ||
            action.tool_id == "mesh.knife-cut";
        if ((affects_grouped_edges || affects_grouped_vertices || affects_grouped_faces) &&
            selected_component_groups.size() > 1U) {
            affected.objects.clear();
            if (affects_grouped_edges) affected.edges.clear();
            if (affects_grouped_vertices) affected.vertices.clear();
            if (affects_grouped_faces) affected.faces.clear();
            for (const auto& group : selected_component_groups) {
                affected.objects.push_back(group.object);
                if (affects_grouped_edges) {
                    affected.edges.insert(
                        affected.edges.end(), group.edges.begin(), group.edges.end());
                }
                if (affects_grouped_vertices) {
                    affected.vertices.insert(
                        affected.vertices.end(), group.vertices.begin(), group.vertices.end());
                }
                if (affects_grouped_faces) {
                    affected.faces.insert(
                        affected.faces.end(), group.faces.begin(), group.faces.end());
                }
            }
        }
        if (action.tool_id == "mesh.extrude-face") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.inset-face") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.merge-by-distance") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.bevel-edge") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.segments, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.bevel-vertex") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.segments, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.remove-face") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.remove_orphaned_vertices,
                std::nullopt,
                std::nullopt,
                std::nullopt, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.set-vertex-position") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                action.arguments.position,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.set-face-normal") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.normal,
                std::nullopt};
        } else if (action.tool_id == "mesh.set-edge-sharp") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.edge_sharp};
        } else if (action.tool_id == "mesh.split-edge") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.factor,
                std::nullopt,
                std::nullopt, std::nullopt, std::nullopt};
        } else if (action.tool_id == "mesh.slide-vertex") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.factor,
                action.arguments.support_edge,
                std::nullopt, std::nullopt, std::nullopt};
            if (action.arguments.support_edge.has_value()) {
                affected.edges.push_back(*action.arguments.support_edge);
            }
        }
    }
    std::optional<NativeOperationTraceEntry> trace_candidate;
    if (active_source_ == OperationSource::human && invocation_context.has_value() &&
        invocation_context->component_object.has_value()) {
        const scene::ObjectId target_object = *invocation_context->component_object;
        const auto* object = before_document.scene().find(target_object);
        if (object != nullptr && object->mesh_asset.has_value()) {
            const auto mesh = before_document.meshes().find(*object->mesh_asset);
            if (mesh != before_document.meshes().end() && action.tool_id == "mesh.inset-face" &&
                invocation_context->faces.size() == 1U) {
                const auto face_id = invocation_context->faces.front();
                const auto* face = mesh->second.find_face(face_id);
                double longest_edge = 0.0;
                if (face != nullptr && face->vertices.size() >= 3U) {
                    for (std::size_t index = 0U; index < face->vertices.size(); ++index) {
                        const auto* first = mesh->second.find_vertex(face->vertices[index]);
                        const auto* second = mesh->second.find_vertex(
                            face->vertices[(index + 1U) % face->vertices.size()]);
                        if (first == nullptr || second == nullptr) {
                            longest_edge = 0.0;
                            break;
                        }
                        longest_edge = std::max(longest_edge,
                            (second->position - first->position).length());
                    }
                }
                const double normalized_distance = longest_edge > 0.0
                    ? action.arguments.distance / longest_edge : 0.0;
                std::optional<assets::Sha256Digest> structure_signature;
                const auto descriptor = structure_signature_for_mesh(mesh->second, face_id);
                if (descriptor) structure_signature = descriptor.value();
                if (std::isfinite(normalized_distance) && normalized_distance > 0.0) {
                    trace_candidate = NativeOperationTraceEntry{
                        0U, action.tool_id, OperationSource::human, before, {}, target_object, face_id,
                        NativeTraceParameterization::normalized_face_distance,
                        normalized_distance, {}, structure_signature, std::nullopt};
                }
            } else if (mesh != before_document.meshes().end() &&
                action.tool_id == "mesh.extrude-face" &&
                invocation_context->faces.size() == 1U) {
                const auto face_id = invocation_context->faces.front();
                const auto* face = mesh->second.find_face(face_id);
                double longest_edge = 0.0;
                if (face != nullptr && face->vertices.size() >= 3U) {
                    for (std::size_t index = 0U; index < face->vertices.size(); ++index) {
                        const auto* first = mesh->second.find_vertex(face->vertices[index]);
                        const auto* second = mesh->second.find_vertex(
                            face->vertices[(index + 1U) % face->vertices.size()]);
                        if (first == nullptr || second == nullptr) {
                            longest_edge = 0.0;
                            break;
                        }
                        longest_edge = std::max(longest_edge,
                            (second->position - first->position).length());
                    }
                }
                const double normalized_distance = longest_edge > 0.0
                    ? action.arguments.distance / longest_edge : 0.0;
                std::optional<assets::Sha256Digest> structure_signature;
                const auto descriptor = structure_signature_for_mesh(mesh->second, face_id);
                if (descriptor) structure_signature = descriptor.value();
                if (std::isfinite(normalized_distance) && normalized_distance > 0.0) {
                    trace_candidate = NativeOperationTraceEntry{
                        0U, action.tool_id, OperationSource::human, before, {}, target_object, face_id,
                        NativeTraceParameterization::normalized_face_distance,
                        normalized_distance, {}, structure_signature, std::nullopt};
                }
            } else if (mesh != before_document.meshes().end() &&
                action.tool_id == "mesh.poke-face" &&
                invocation_context->faces.size() == 1U) {
                const auto face_id = invocation_context->faces.front();
                std::optional<assets::Sha256Digest> structure_signature;
                const auto descriptor = structure_signature_for_mesh(mesh->second, face_id);
                if (descriptor) structure_signature = descriptor.value();
                trace_candidate = NativeOperationTraceEntry{
                    0U, action.tool_id, OperationSource::human, before, {}, target_object, face_id,
                    NativeTraceParameterization::parameterless,
                    0.0, {}, structure_signature, std::nullopt};
            } else if (action.tool_id == "mesh.set-vertex-position" &&
                invocation_context->vertices.size() == 1U) {
                const auto vertex_id = invocation_context->vertices.front();
                const auto* vertex = mesh->second.find_vertex(vertex_id);
                if (vertex != nullptr) {
                    const auto displacement = action.arguments.position - vertex->position;
                    const double magnitude = displacement.length();
                    std::optional<assets::Sha256Digest> structure_signature;
                    const auto descriptor = structure_signature_for_mesh(
                        mesh->second, vertex_id);
                    if (descriptor) structure_signature = descriptor.value();
                    if (displacement.finite() && std::isfinite(magnitude) && magnitude > 0.0) {
                        trace_candidate = NativeOperationTraceEntry{
                            0U, action.tool_id, OperationSource::human, before, {}, target_object, vertex_id,
                            NativeTraceParameterization::relative_displacement,
                            0.0, displacement, structure_signature, std::nullopt};
                    }
                }
            }
        }
    }
    if (auto result = tools_.invoke(action.tool_id, context, action.arguments, history_); !result) {
        return failure(result.error().with_context("tool invocation"));
    }
    auto result = accept_command_mutation(
        "Invoke Tool: " + action.tool_id,
        before,
        std::move(before_document),
        std::move(affected),
        std::move(parameters));
    if (result && result.value().document_changed) {
        if (trace_candidate.has_value() && result.value().source == OperationSource::human) {
            const auto* result_object = document_.scene().find(trace_candidate->target_object);
            if (result_object != nullptr && result_object->mesh_asset.has_value()) {
                const auto result_mesh = document_.meshes().find(*result_object->mesh_asset);
                if (result_mesh != document_.meshes().end()) {
                    const auto signature = structure_signature_for_mesh(
                        result_mesh->second, std::monostate{});
                    if (signature) {
                        trace_candidate->result_object_structure_signature = signature.value();
                    }
                }
            }
            trace_candidate->operation_id = result.value().operation_id;
            trace_candidate->revision_before = result.value().revision_before;
            trace_candidate->revision_after = result.value().revision_after;
            trace_candidate->source = result.value().source;
            append_native_operation_trace(std::move(*trace_candidate));
        } else {
            native_operation_trace_.clear();
        }
    }
    if (result && (action.tool_id == "mesh.inset-face" ||
                   action.tool_id == "mesh.poke-face" ||
                   action.tool_id == "mesh.remove-face" ||
                   action.tool_id == "mesh.split-edge" ||
                   action.tool_id == "mesh.loop-cut" ||
                   action.tool_id == "mesh.bevel-edge" ||
                   action.tool_id == "mesh.bevel-vertex" ||
                   action.tool_id == "mesh.dissolve-edge" ||
                   action.tool_id == "mesh.tri-to-quad" ||
                   action.tool_id == "mesh.merge-vertices" ||
                   action.tool_id == "mesh.merge-by-distance")) {
        selection_.clear();
    }
    if (result) {
        last_tool_action_ = action;
        last_tool_context_ = std::move(invocation_context);
    }
    return result;
}

core::Result<void> ApplicationSession::rebind_last_tool_selection() {
    if (!last_tool_action_.has_value() || !last_tool_context_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "repeat-last requires a previously accepted tool invocation"));
    }

    const auto& saved = *last_tool_context_;
    if (auto result = saved.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "repeat-last authoring context"));
    }

    selection_.clear();
    if (auto result = selection_.set_mode(saved.selection_mode); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "repeat-last selection mode"));
    }

    if (saved.selection_mode == editor::SelectionMode::object) {
        for (const scene::ObjectId object : saved.objects) {
                if (auto result = selection_.select_object(
                    document_.scene(), object,
                    selection_.empty()
                        ? editor::SelectionOperation::replace
                    : editor::SelectionOperation::add);
                !result) {
                return core::Result<void>::failure(result.error().with_context(
                    "repeat-last object selection"));
            }
        }
    } else if (saved.component_groups.size() > 1U) {
        bool first_component = true;
        auto groups = saved.component_groups;
        if (saved.component_object.has_value()) {
            std::stable_sort(groups.begin(), groups.end(),
                [&](const editor::ComponentSelectionGroup& left,
                    const editor::ComponentSelectionGroup& right) {
                    const bool left_active = left.object == *saved.component_object;
                    const bool right_active = right.object == *saved.component_object;
                    return !left_active && right_active;
                });
        }
        for (const auto& group : groups) {
            const auto* object = document_.scene().find(group.object);
            if (!object) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "repeat-last component owner is no longer present"));
            }
            if (!object->mesh_asset.has_value()) {
                return core::Result<void>::failure(invalid_state(
                    "repeat-last component owner has no mesh asset"));
            }
            const auto mesh = document_.meshes().find(*object->mesh_asset);
            if (mesh == document_.meshes().end()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "repeat-last component owner references a missing mesh"));
            }
            const auto operation = [&]() {
                const auto value = first_component
                    ? editor::SelectionOperation::replace
                    : editor::SelectionOperation::add;
                first_component = false;
                return value;
            };
            const auto select_ids = [&](const auto& ids, auto&& select) -> core::Result<void> {
                for (const auto id : ids) {
                    if (auto result = select(id, operation()); !result) return result;
                }
                return core::Result<void>::success();
            };
            core::Result<void> rebound = core::Result<void>::success();
            if (saved.selection_mode == editor::SelectionMode::vertex) {
                const auto& ids = group.vertex_selection_order.empty()
                    ? group.vertices : group.vertex_selection_order;
                rebound = select_ids(ids, [&](geometry::VertexId id, editor::SelectionOperation op) {
                    return selection_.select_vertex(
                        document_.scene(), group.object, mesh->second, id, op);
                });
            } else if (saved.selection_mode == editor::SelectionMode::edge) {
                rebound = select_ids(group.edges,
                    [&](geometry::EdgeId id, editor::SelectionOperation op) {
                        return selection_.select_edge(
                            document_.scene(), group.object, mesh->second, id, op);
                    });
            } else {
                rebound = select_ids(group.faces,
                    [&](geometry::FaceId id, editor::SelectionOperation op) {
                        return selection_.select_face(
                            document_.scene(), group.object, mesh->second, id, op);
                    });
            }
            if (!rebound) {
                return core::Result<void>::failure(rebound.error().with_context(
                    "repeat-last grouped selection rebind"));
            }
        }
    } else {
        if (!saved.component_object.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last component context has no owning scene object"));
        }
        const auto* object = document_.scene().find(*saved.component_object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(invalid_state(
                "repeat-last owner object has no mesh asset"));
        }
        const auto mesh = document_.meshes().find(*object->mesh_asset);
        if (mesh == document_.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last owner object references a missing mesh"));
        }
        const auto bind = [&](auto&& ids, auto&& select) -> core::Result<void> {
            bool first = true;
            for (const auto id : ids) {
                if (auto result = select(
                        id,
                        first ? editor::SelectionOperation::replace
                              : editor::SelectionOperation::add);
                    !result) {
                    return result;
                }
                first = false;
            }
            return core::Result<void>::success();
        };
        core::Result<void> rebound = core::Result<void>::success();
        if (saved.selection_mode == editor::SelectionMode::vertex) {
            if (!saved.vertices.empty() && saved.vertex_selection_order.empty()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "repeat-last vertex context has no preserved selection order"));
            }
            rebound = bind(saved.vertex_selection_order,
                [&](geometry::VertexId id, editor::SelectionOperation op) {
                return selection_.select_vertex(document_.scene(), *saved.component_object,
                                                mesh->second, id, op);
            });
        } else if (saved.selection_mode == editor::SelectionMode::edge) {
            rebound = bind(saved.edges, [&](geometry::EdgeId id, editor::SelectionOperation op) {
                return selection_.select_edge(document_.scene(), *saved.component_object,
                                              mesh->second, id, op);
            });
        } else {
            rebound = bind(saved.faces, [&](geometry::FaceId id, editor::SelectionOperation op) {
                return selection_.select_face(document_.scene(), *saved.component_object,
                                              mesh->second, id, op);
            });
        }
        if (!rebound) {
            return core::Result<void>::failure(rebound.error().with_context(
                "repeat-last selection rebind"));
        }
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::repeat_last_tool() {
    const editor::SelectionState before_selection = selection_;
    if (auto result = rebind_last_tool_selection(); !result) {
        selection_ = before_selection;
        return failure(result.error());
    }
    auto result = invoke_tool(*last_tool_action_);
    if (!result) selection_ = before_selection;
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::adjust_last_tool(
    const AdjustLastToolAction& action) {
    if (!last_tool_action_.has_value() || !last_tool_context_.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "adjust-last requires a previously accepted tool invocation"));
    }
    const editor::SelectionState before_selection = selection_;
    const InvokeToolAction previous_action = *last_tool_action_;
    const editor::AuthoringContext previous_context = *last_tool_context_;
    last_tool_action_->arguments = action.arguments;
    if (auto result = rebind_last_tool_selection(); !result) {
        last_tool_action_ = previous_action;
        last_tool_context_ = previous_context;
        selection_ = before_selection;
        return failure(result.error().with_context("adjust-last selection"));
    }
    auto result = invoke_tool(*last_tool_action_);
    if (!result) {
        last_tool_action_ = previous_action;
        last_tool_context_ = previous_context;
        selection_ = before_selection;
    }
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::create_box(const CreateBoxAction& action) {
    auto mesh = geometry::make_box(action.size);
    if (!mesh) {
        return failure(mesh.error().with_context("create box"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, std::move(mesh.value())),
        "Create Box");
}

core::Result<DispatchReceipt> ApplicationSession::create_plane(const CreatePlaneAction& action) {
    auto mesh = geometry::make_plane(action.width, action.depth);
    if (!mesh) {
        return failure(mesh.error().with_context("create plane"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, std::move(mesh.value())),
        "Create Plane");
}

core::Result<DispatchReceipt> ApplicationSession::create_mesh_object(
    const CreateMeshObjectAction& action) {
    if (action.expected_project_revision.has_value() &&
        *action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data,
            "mesh authoring snapshot no longer matches the project revision"));
    }
    if (action.source_record.has_value()) {
        if (active_source_ != OperationSource::human) {
            return failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "source-backed mesh import requires explicit human review"));
        }
        if (auto result = action.source_record->validate(); !result) {
            return failure(result.error().with_context("source-backed mesh import"));
        }
        if (!project_path_.has_value()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "source-linked mesh imports require a saved project"));
        }
        if (auto result = verify_source_blob(*project_path_, *action.source_record); !result) {
            return failure(result.error().with_context("source-backed mesh bytes"));
        }
        if (auto result = action.mesh.validate(); !result) {
            return failure(result.error().with_context("source-backed mesh geometry"));
        }
        if (canonical_mesh_geometry_digest(action.mesh) !=
            action.source_record->artifact_digest) {
            return failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "source mesh artifact digest does not match the imported geometry"));
        }
        auto material_digest = mesh_material_region_digest(action.mesh);
        if (!material_digest) {
            return failure(material_digest.error().with_context(
                "source-backed mesh material regions"));
        }
        const auto recorded_material = std::find_if(
            action.source_record->domain_digests.begin(),
            action.source_record->domain_digests.end(),
            [](const assets::SourceDomainDigest& item) {
                return item.domain == assets::SourceAssetDomain::material;
            });
        const bool has_material_regions = material_digest.value().has_value();
        if (has_material_regions !=
                (recorded_material != action.source_record->domain_digests.end()) ||
            (has_material_regions && recorded_material->digest !=
                *material_digest.value()) ||
            (action.source_record->importer_identity == "carto.import.obj" &&
             action.source_record->importer_version == 3U && !has_material_regions)) {
            return failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "source mesh material-region domain digest does not match its imported payload"));
        }
        const auto active = document_.source_asset_registry().records().find(
            action.source_record->identity);
        if (active != document_.source_asset_registry().records().end()) {
            if (active->second.kind != assets::SourceAssetKind::mesh ||
                active->second.artifact_identity != action.source_record->artifact_identity) {
                return failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "source reimport cannot change its bound mesh artifact"));
            }
            auto candidate = *action.source_record;
            candidate.source_generation = active->second.source_generation;
            if (candidate.serialize() == active->second.serialize()) {
                return accepted("Import Source Mesh · No Changes", document_.revision());
            }
        }
        editor::ProjectCommandAdmission admission;
        return execute_command(
            std::make_unique<editor::ImportSourceMeshCommand>(
                admission,
                document_,
                action.object_name,
                action.mesh,
                *action.source_record),
            "Import Source Mesh");
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, action.mesh),
        "Create Mesh Object");
}

core::Result<DispatchReceipt> ApplicationSession::append_sculpt_stroke(
    const AppendSculptStrokeAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "SculptStack authoring requires the human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack authoring snapshot no longer matches the project revision"));
    }
    if (selection_.mode() != editor::SelectionMode::vertex ||
        selection_.selected_vertices().size() != 1U ||
        !selection_.component_object().has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "SculptStack point strokes require exactly one selected mesh vertex"));
    }

    const scene::ObjectId object_id = *selection_.component_object();
    const scene::SceneObject* object = document_.scene().find(object_id);
    if (!object || !object->mesh_asset.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack selection owner or mesh asset is missing"));
    }
    if (object->locked || !object->visible) {
        return failure(invalid_state(
            "hidden or locked scene objects cannot receive sculpt strokes"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh = document_.meshes().find(mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack selection references a missing mesh asset"));
    }
    if (auto result = selection_.validate(document_.scene(), &mesh->second); !result) {
        return failure(result.error().with_context("SculptStack selection"));
    }
    const geometry::VertexId vertex_id = selection_.selected_vertices().front();
    const geometry::Vertex* vertex = mesh->second.find_vertex(vertex_id);
    if (!vertex) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack selected vertex is no longer present in the source mesh"));
    }

    production::SculptStroke stroke;
    stroke.mode = action.mode;
    stroke.radius = action.radius;
    stroke.strength = action.strength;
    stroke.samples.push_back(production::SculptStrokeSample{vertex->position, 1.0});
    return commit_sculpt_stroke(
        mesh_asset,
        object_id,
        std::move(stroke),
        "Add Sculpt Stroke",
        AffectedSet{{object_id}, {vertex_id}, {}, {}},
        ParameterPayload{
            std::nullopt, action.radius, vertex->position, std::nullopt,
            action.strength, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

core::Result<DispatchReceipt> ApplicationSession::append_sculpt_surface_stroke(
    const AppendSculptSurfaceStrokeAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "Sculpt surface authoring requires the human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt surface snapshot no longer matches the project revision"));
    }
    const scene::SceneObject* object = document_.scene().find(action.object);
    if (object == nullptr || !object->mesh_asset.has_value() ||
        *object->mesh_asset != action.mesh_asset) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt surface target no longer matches its authored mesh"));
    }
    if (object->locked || !object->visible) {
        return failure(invalid_state(
            "hidden or locked scene objects cannot receive sculpt surface strokes"));
    }
    const auto mesh = document_.meshes().find(action.mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt surface target mesh is missing"));
    }
    if (action.samples.empty() ||
        action.samples.size() > production::kMaxSculptSamplesPerStroke) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "Sculpt surface stroke sample count is outside its supported bound"));
    }

    production::SculptStroke stroke;
    stroke.mode = action.mode;
    stroke.radius = action.radius;
    stroke.strength = action.strength;
    stroke.samples = action.samples;
    std::vector<geometry::VertexId> affected_vertices;
    std::vector<geometry::FaceId> affected_faces;
    affected_vertices.reserve(action.samples.size() * 3U);
    affected_faces.reserve(action.samples.size());
    for (const production::SculptStrokeSample& sample : action.samples) {
        if (!sample.surface_anchor.has_value()) {
            return failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "Sculpt surface stroke samples must retain stable source anchors"));
        }
        const production::SculptSurfaceAnchor& anchor = *sample.surface_anchor;
        if (auto result = anchor.validate(); !result) {
            return failure(result.error().with_context("Sculpt surface stroke anchor"));
        }
        const auto resolved = production::resolve_sculpt_surface_anchor(mesh->second, anchor);
        if (!resolved) {
            return failure(resolved.error().with_context("Sculpt surface stroke source"));
        }
        if (sample.position.x != resolved.value().x ||
            sample.position.y != resolved.value().y ||
            sample.position.z != resolved.value().z) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "Sculpt surface sample position no longer matches its source anchor"));
        }
        affected_faces.push_back(anchor.face);
        affected_vertices.insert(
            affected_vertices.end(), anchor.vertices.begin(), anchor.vertices.end());
    }
    std::sort(affected_vertices.begin(), affected_vertices.end(),
        [](geometry::VertexId left, geometry::VertexId right) {
            return left.value < right.value;
        });
    affected_vertices.erase(
        std::unique(affected_vertices.begin(), affected_vertices.end()),
        affected_vertices.end());
    std::sort(affected_faces.begin(), affected_faces.end(),
        [](geometry::FaceId left, geometry::FaceId right) {
            return left.value < right.value;
        });
    affected_faces.erase(std::unique(affected_faces.begin(), affected_faces.end()),
        affected_faces.end());

    return commit_sculpt_stroke(
        action.mesh_asset,
        action.object,
        std::move(stroke),
        "Add Sculpt Surface Stroke",
        AffectedSet{{action.object}, std::move(affected_vertices), {}, std::move(affected_faces)},
        ParameterPayload{
            std::nullopt, action.radius, action.samples.front().position, std::nullopt,
            action.strength, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

core::Result<DispatchReceipt> ApplicationSession::commit_sculpt_stroke(
    std::uint64_t mesh_asset,
    scene::ObjectId object_id,
    production::SculptStroke stroke,
    std::string action_name,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const scene::SceneObject* object = document_.scene().find(object_id);
    if (object == nullptr || !object->mesh_asset.has_value() ||
        *object->mesh_asset != mesh_asset) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt stroke owner no longer matches its mesh asset"));
    }
    const auto mesh = document_.meshes().find(mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt stroke mesh asset is missing"));
    }

    std::optional<project::SerializedSculptStack> before;
    production::SculptStack stack;
    const auto current = document_.sculpt_stacks().find(mesh_asset);
    if (current == document_.sculpt_stacks().end()) {
        auto created = production::make_sculpt_stack(document_, mesh_asset);
        if (!created) return failure(created.error().with_context("create SculptStack"));
        stack = std::move(created.value());
    } else {
        before = current->second;
        auto decoded = production::SculptStack::deserialize(current->second.bytes);
        if (!decoded) return failure(decoded.error().with_context("read existing SculptStack"));
        stack = std::move(decoded.value());
    }
    stack.format_version = production::kSculptStackVersion;
    for (production::SculptLayer& layer : stack.layers) {
        for (production::SculptStroke& existing : layer.strokes) {
            if (existing.symmetry != production::SculptSymmetryMode::mirror_x) {
                existing.symmetry_plane_x = 0.0;
            }
        }
    }

    production::SculptLayer* target_layer = nullptr;
    for (auto layer = stack.layers.rbegin(); layer != stack.layers.rend(); ++layer) {
        if (layer->visible && layer->weight > 0.0) {
            target_layer = &*layer;
            break;
        }
    }
    if (target_layer == nullptr) {
        std::set<std::string> used_layer_ids;
        for (const production::SculptLayer& layer : stack.layers) {
            used_layer_ids.insert(layer.identity);
        }
        const std::string revision_tag = std::to_string(document_.revision().value());
        std::string layer_id = "ui.layer.rev" + revision_tag;
        for (std::uint32_t suffix = 1U; used_layer_ids.contains(layer_id); ++suffix) {
            layer_id = "ui.layer.rev" + revision_tag + "." + std::to_string(suffix);
        }
        stack.layers.push_back(production::SculptLayer{
            std::move(layer_id), "Sculpt", true, 1.0, {}});
        target_layer = &stack.layers.back();
    }

    const std::string revision_tag = std::to_string(document_.revision().value());
    stroke.identity = "ui.stroke.rev" + revision_tag;
    const auto stroke_identity_exists = [&stack](std::string_view identity) {
        for (const production::SculptLayer& layer : stack.layers) {
            for (const production::SculptStroke& existing : layer.strokes) {
                if (existing.identity == identity) return true;
            }
        }
        return false;
    };
    for (std::uint32_t suffix = 1U; stroke_identity_exists(stroke.identity); ++suffix) {
        stroke.identity = "ui.stroke.rev" + revision_tag + "." + std::to_string(suffix);
    }
    target_layer->strokes.push_back(std::move(stroke));

    if (auto result = stack.validate(); !result) {
        return failure(result.error().with_context("SculptStack point stroke"));
    }
    const std::string serialized = stack.serialize();
    const auto digest = stack.digest();
    if (serialized.empty() || !digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "SculptStack stroke did not produce canonical source bytes"));
    }

    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSculptStackCommand>(
            admission, document_, mesh_asset, std::move(before),
            project::SerializedSculptStack{serialized, digest.value()}),
        std::move(action_name),
        std::move(affected),
        std::move(parameters));
}

core::Result<DispatchReceipt> ApplicationSession::set_sculpt_layer_visibility(
    const SetSculptLayerVisibilityAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "SculptStack layer visibility requires the human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack layer visibility snapshot no longer matches the project revision"));
    }
    if (!selection_.component_object().has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "SculptStack layer visibility requires a selected mesh component"));
    }
    const scene::ObjectId object_id = *selection_.component_object();
    const scene::SceneObject* object = document_.scene().find(object_id);
    if (!object || !object->mesh_asset.has_value() ||
        *object->mesh_asset != action.mesh_asset) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack layer visibility target no longer matches the selected mesh"));
    }
    if (object->locked || !object->visible) {
        return failure(invalid_state(
            "hidden or locked scene objects cannot edit SculptStack layers"));
    }
    const auto mesh = document_.meshes().find(action.mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack layer visibility target mesh is missing"));
    }
    if (auto result = selection_.validate(document_.scene(), &mesh->second); !result) {
        return failure(result.error().with_context("SculptStack layer selection"));
    }
    const auto current = document_.sculpt_stacks().find(action.mesh_asset);
    if (current == document_.sculpt_stacks().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "selected mesh has no authored SculptStack layers"));
    }
    auto decoded = production::SculptStack::deserialize(current->second.bytes);
    if (!decoded) {
        return failure(decoded.error().with_context("read SculptStack layers"));
    }
    production::SculptStack stack = std::move(decoded.value());
    const auto layer = std::find_if(
        stack.layers.begin(), stack.layers.end(), [&action](const production::SculptLayer& item) {
            return item.identity == action.layer_identity;
        });
    if (layer == stack.layers.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "SculptStack layer identity is not present on the selected mesh"));
    }
    if (layer->visible == action.visible) {
        return accepted("Set Sculpt Layer Visibility", document_.revision());
    }

    layer->visible = action.visible;
    if (auto result = stack.validate(); !result) {
        return failure(result.error().with_context("SculptStack layer visibility"));
    }
    const std::string serialized = stack.serialize();
    const auto digest = stack.digest();
    if (serialized.empty() || !digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "SculptStack layer visibility did not produce canonical source bytes"));
    }

    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSculptStackCommand>(
            admission,
            document_,
            action.mesh_asset,
            current->second,
            project::SerializedSculptStack{serialized, digest.value()}),
        "Set Sculpt Layer Visibility",
        AffectedSet{{object_id}, {}, {}, {}});
}

core::Result<DispatchReceipt> ApplicationSession::duplicate_mesh_instance(
    const DuplicateMeshInstanceAction& action) {
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::DuplicateProjectMeshInstanceCommand>(
            admission,
            document_,
            action.source,
            action.object_name,
            action.local_translation_offset),
        "Duplicate Mesh Instance",
        AffectedSet{{action.source}, {}, {}, {}},
        ParameterPayload{
            std::nullopt,
            std::nullopt,
            action.local_translation_offset,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt, std::nullopt, std::nullopt});
}

core::Result<DispatchReceipt> ApplicationSession::duplicate_mesh_instance_batch(
    const DuplicateMeshInstanceBatchAction& action) {
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::DuplicateProjectMeshInstanceBatchCommand>(
            admission,
            document_,
            action.source,
            action.instances),
        "Duplicate Mesh Instance Batch",
        AffectedSet{{action.source}, {}, {}, {}});
}

core::Result<DispatchReceipt> ApplicationSession::create_plant_scatter_batch(
    const CreatePlantScatterBatchAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(core::ErrorCode::unsupported,
            "CONDITION vegetation placement requires explicit human review"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data,
            "CONDITION vegetation placement targets a stale project revision"));
    }
    const auto preview = condition_vegetation_preview(action.recipe_id);
    if (!preview) return failure(preview.error().with_context(
        "CONDITION vegetation placement preview"));
    if (action.expected_preview_digest.is_zero() ||
        preview.value().application_revision != action.expected_project_revision ||
        preview.value().preview_digest != action.expected_preview_digest) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data,
            "CONDITION vegetation preview changed before placement commit"));
    }
    if (!preview.value().plant_source.has_value()) {
        return failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "CONDITION vegetation placement requires an explicitly bound plant source"));
    }
    const auto& source_receipt = *preview.value().plant_source;
    const auto source_record = document_.source_asset_registry().records().find(
        source_receipt.identity);
    if (source_record == document_.source_asset_registry().records().end() ||
        source_record->second.kind != assets::SourceAssetKind::mesh ||
        source_record->second.source_generation != source_receipt.source_revision ||
        source_record->second.source_digest != source_receipt.digest) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data,
            "CONDITION vegetation source receipt is not the exact active mesh source"));
    }
    constexpr std::string_view kMeshPrefix = "mesh.asset.";
    const std::string_view artifact_identity = source_record->second.artifact_identity;
    if (!artifact_identity.starts_with(kMeshPrefix)) {
        return failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "bound plant source is not linked to a mesh artifact"));
    }
    const std::string_view mesh_id_text = artifact_identity.substr(kMeshPrefix.size());
    std::uint64_t mesh_asset = 0U;
    const auto parsed_mesh_id = std::from_chars(
        mesh_id_text.data(), mesh_id_text.data() + mesh_id_text.size(), mesh_asset);
    if (mesh_id_text.empty() || parsed_mesh_id.ec != std::errc{} ||
        parsed_mesh_id.ptr != mesh_id_text.data() + mesh_id_text.size() || mesh_asset == 0U) {
        return failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "bound plant mesh artifact identity is malformed"));
    }
    const auto* source_object = document_.scene().find(action.botanical_source_object);
    if (!source_object || !source_object->mesh_asset.has_value() ||
        *source_object->mesh_asset != mesh_asset || source_object->parent.has_value() ||
        !source_object->visible || source_object->locked) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data,
            "CONDITION vegetation prototype no longer matches the bound botanical mesh"));
    }
    if (preview.value().instances.size() > editor::kMaxMeshInstanceBatchInstances) {
        return failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "CONDITION vegetation preview exceeds the atomic placement batch bound"));
    }
    editor::ProjectCommandAdmission admission;
    auto command = std::make_unique<editor::CreatePlantScatterBatchCommand>(
        admission, document_, action.botanical_source_object, preview.value());
    auto* command_receipt = command.get();
    auto result = execute_command(std::move(command),
        "Commit CONDITION Vegetation Placements",
        AffectedSet{{action.botanical_source_object}, {}, {}, {}});
    if (result && command_receipt != nullptr) {
        result.value().affected.objects.insert(result.value().affected.objects.end(),
            command_receipt->created_objects().begin(),
            command_receipt->created_objects().end());
    }
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::import_engineering_fixture() {
    auto fixture = engineering::make_dv012_fixture();
    if (!fixture) return failure(fixture.error().with_context("DV012 engineering fixture"));
    const auto serialized = fixture.value().serialize();
    const auto digest = fixture.value().canonical_digest();
    if (!digest) {
        return failure(digest.error().with_context("DV012 engineering fixture digest"));
    }
    const auto current = document_.engineering_authoring();
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import DV012 Engineering Authoring", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Import DV012 Engineering Authoring");
}

core::Result<DispatchReceipt> ApplicationSession::import_engineering_authoring(
    const ImportEngineeringAuthoringAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering authoring import requires the human command boundary"));
    }
    const auto decoded = engineering::AuthoringDocument::deserialize(action.serialized_document);
    if (!decoded) return failure(decoded.error().with_context("engineering authoring import"));
    const std::string serialized = decoded.value().serialize();
    const auto digest = decoded.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context("engineering authoring import digest"));
    const auto current = document_.engineering_authoring();
    if (current.has_value()) {
        const auto current_authoring = engineering_authoring();
        if (!current_authoring) {
            return failure(current_authoring.error().with_context("engineering authoring import"));
        }
        if (current_authoring.value().workcell.has_value()) {
            if (!decoded.value().workcell.has_value()) {
                return failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "an existing workcell cannot be removed through an authoring document import"));
            }
            const auto current_workcell_digest =
                current_authoring.value().workcell->canonical_digest();
            const auto imported_workcell_digest = decoded.value().workcell->canonical_digest();
            if (!current_workcell_digest || !imported_workcell_digest) {
                return failure((!current_workcell_digest
                    ? current_workcell_digest.error() : imported_workcell_digest.error())
                    .with_context("engineering workcell import"));
            }
            if (current_workcell_digest.value() != imported_workcell_digest.value() ||
                current_authoring.value().workcell_revision_history !=
                    decoded.value().workcell_revision_history) {
                return failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "workcell changes require the revision-bound workcell authoring command"));
            }
        }
    }
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import Engineering Authoring Document", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Import Engineering Authoring Document");
}

core::Result<DispatchReceipt> ApplicationSession::import_engineering_bom_csv(
    const ImportEngineeringBomCsvAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering BOM import requires the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering BOM import was based on a stale project revision"));
    }
    if (action.locator.empty() || action.locator.size() > 255U ||
        action.locator == "." || action.locator == ".." ||
        action.locator.find_first_of("/\\:") != std::string::npos ||
        action.csv_bytes.empty() || action.csv_bytes.size() > engineering::kMaxBomCsvBytes) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering BOM import requires a safe filename locator and bounded CSV bytes"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context("engineering BOM import"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering BOM import was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }

    const auto source_digest = structure_digest(action.csv_bytes);
    std::vector<const engineering::EngineeringSource*> same_locator;
    for (const auto& source : current.value().sources) {
        if (source.locator != action.locator) continue;
        if (source.kind != engineering::SourceKind::bom) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "source locator is already bound to a non-BOM engineering source"));
        }
        same_locator.push_back(&source);
    }
    std::sort(same_locator.begin(), same_locator.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    if (!same_locator.empty() && same_locator.back()->digest == source_digest &&
        std::none_of(same_locator.back()->diagnostics.begin(),
            same_locator.back()->diagnostics.end(), [](const std::string& diagnostic) {
                return diagnostic.find("source revision changed") != std::string::npos;
            })) {
        return accepted("Import Engineering BOM CSV", document_.revision());
    }

    engineering::Id source_id = 1U;
    for (const auto& source : current.value().sources) {
        if (source.id == std::numeric_limits<engineering::Id>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "engineering source identity space is exhausted"));
        }
        source_id = std::max(source_id, source.id + 1U);
    }
    engineering::Id first_line_id = 1U;
    for (const auto& line : current.value().bom) {
        if (line.id == std::numeric_limits<engineering::Id>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "engineering BOM line identity space is exhausted"));
        }
        first_line_id = std::max(first_line_id, line.id + 1U);
    }
    const auto parsed = engineering::parse_bom_csv(
        action.csv_bytes, source_id, first_line_id);
    if (!parsed) return failure(parsed.error().with_context("engineering BOM CSV parse"));
    if (parsed.value().size() > std::numeric_limits<engineering::Id>::max() - first_line_id) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering BOM line identity space is exhausted"));
    }
    const auto reconciled = engineering::reconcile_bom_lines(
        current.value(), parsed.value(), source_id,
        first_line_id + static_cast<engineering::Id>(parsed.value().size()));
    if (!reconciled) {
        return failure(reconciled.error().with_context("engineering BOM reconciliation"));
    }

    auto updated = current.value();
    for (const auto* previous : same_locator) {
        auto mutable_previous = std::find_if(updated.sources.begin(), updated.sources.end(),
            [previous](const auto& candidate) { return candidate.id == previous->id; });
        const std::string supersession = "superseded by BOM source " +
            std::to_string(source_id);
        if (mutable_previous != updated.sources.end() &&
            std::find(mutable_previous->diagnostics.begin(),
                mutable_previous->diagnostics.end(), supersession) ==
                    mutable_previous->diagnostics.end()) {
            mutable_previous->diagnostics.push_back(supersession);
        }
        for (auto& line : updated.bom) {
            if (line.source_id == previous->id) {
                line.reconciliation = engineering::ReconciliationStatus::unresolved;
            }
        }
    }

    engineering::EngineeringSource source;
    source.id = source_id;
    source.kind = engineering::SourceKind::bom;
    source.locator = action.locator;
    source.revision = "content-sha256:" + source_digest.hex();
    source.digest = source_digest;
    source.provider_identity = "cartographer.bom-csv";
    source.provider_version = "1.0.0";
    source.import_settings_digest = structure_digest(
        "cartographer.bom-csv.v1;header=part_number,revision,description,material,quantity,unit");
    source.import_run_id = "bom-csv-" + source_digest.hex();
    source.units = "source-defined";
    source.diagnostics.push_back(
        "content-addressed source revision; supplier revision was not inferred");
    updated.sources.push_back(std::move(source));
    updated.bom.insert(updated.bom.end(), reconciled.value().begin(), reconciled.value().end());
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("engineering BOM import"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("engineering BOM import digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Import and Reconcile Engineering BOM CSV");
}

core::Result<DispatchReceipt> ApplicationSession::import_engineering_requirement_csv(
    const ImportEngineeringRequirementCsvAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering requirement import requires the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering requirement import was based on a stale project revision"));
    }
    if (action.locator.empty() || action.locator.size() > 255U ||
        action.locator == "." || action.locator == ".." ||
        action.locator.find_first_of("/\\:") != std::string::npos ||
        action.csv_bytes.empty() ||
        action.csv_bytes.size() > engineering::kMaxRequirementCsvBytes) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering requirement import requires a safe filename and bounded CSV bytes"));
    }

    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context(
        "engineering requirement import"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering requirement import was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    const auto parsed = engineering::parse_requirement_csv(action.csv_bytes);
    if (!parsed) return failure(parsed.error().with_context(
        "engineering requirement CSV parse"));
    if (parsed.value().size() > engineering::AuthoringDocument::kMaxCollectionEntries -
            current.value().requirements.size()) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering requirement collection reached its configured limit"));
    }

    const auto source_digest = structure_digest(action.csv_bytes);
    std::vector<engineering::Id> superseded_source_ids;
    for (const auto& source : current.value().sources) {
        if (source.locator != action.locator) continue;
        if (source.kind != engineering::SourceKind::specification) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "source locator is already bound to a non-specification engineering source"));
        }
        superseded_source_ids.push_back(source.id);
    }
    std::sort(superseded_source_ids.begin(), superseded_source_ids.end());
    if (!superseded_source_ids.empty()) {
        const auto latest = std::find_if(current.value().sources.begin(),
            current.value().sources.end(), [&superseded_source_ids](const auto& source) {
                return source.id == superseded_source_ids.back();
            });
        if (latest != current.value().sources.end() && latest->digest == source_digest &&
            std::none_of(latest->diagnostics.begin(), latest->diagnostics.end(),
                [](const std::string& diagnostic) {
                    return diagnostic.starts_with("superseded by engineering specification ");
                })) {
            return accepted("Import Engineering Requirement CSV", document_.revision());
        }
    }

    engineering::Id source_id = 1U;
    for (const auto& source : current.value().sources) {
        if (source.id == std::numeric_limits<engineering::Id>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "engineering source identity space is exhausted"));
        }
        source_id = std::max(source_id, source.id + 1U);
    }
    engineering::Id first_requirement_id = 1U;
    for (const auto& requirement : current.value().requirements) {
        if (requirement.id == std::numeric_limits<engineering::Id>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "engineering requirement identity space is exhausted"));
        }
        first_requirement_id = std::max(first_requirement_id, requirement.id + 1U);
    }
    if (!parsed.value().empty() && parsed.value().size() - 1U >
            std::numeric_limits<engineering::Id>::max() - first_requirement_id) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering requirement identity space is exhausted"));
    }

    std::vector<engineering::Id> topology_ids;
    topology_ids.reserve(parsed.value().size());
    for (std::size_t index = 0U; index < parsed.value().size(); ++index) {
        const auto& record = parsed.value()[index];
        std::vector<const engineering::TopologyBinding*> matches;
        for (const auto& binding : current.value().topology) {
            if (binding.external_id != record.topology_external_id) continue;
            const auto source = std::find_if(current.value().sources.begin(),
                current.value().sources.end(), [&binding](const auto& candidate) {
                    return candidate.id == binding.source_id;
                });
            if (source != current.value().sources.end() &&
                source->kind == engineering::SourceKind::step_ap242 &&
                binding.source_revision == source->revision) {
                matches.push_back(&binding);
            }
        }
        if (matches.empty()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "requirement CSV row " + std::to_string(index + 2U) +
                    " references an unknown or stale topology external ID"));
        }
        if (matches.size() != 1U) {
            return failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "requirement CSV row " + std::to_string(index + 2U) +
                    " has an ambiguous topology external ID"));
        }
        topology_ids.push_back(matches.front()->id);
    }

    auto updated = current.value();
    std::set<engineering::Id> affected_requirement_ids;
    for (auto& previous : updated.sources) {
        if (std::find(superseded_source_ids.begin(), superseded_source_ids.end(),
                previous.id) == superseded_source_ids.end()) continue;
        const std::string supersession =
            "superseded by engineering specification " + std::to_string(source_id);
        if (std::find(previous.diagnostics.begin(), previous.diagnostics.end(),
                supersession) == previous.diagnostics.end()) {
            previous.diagnostics.push_back(supersession);
        }
        for (auto& requirement : updated.requirements) {
            if (requirement.provenance.source_id == previous.id &&
                requirement.extraction != engineering::ExtractionState::rejected) {
                requirement.extraction = engineering::ExtractionState::stale;
                affected_requirement_ids.insert(requirement.id);
            }
        }
    }
    std::set<engineering::Id> affected_characteristic_ids;
    for (auto& characteristic : updated.characteristics) {
        if (!affected_requirement_ids.contains(characteristic.requirement_id)) continue;
        characteristic.status = engineering::InspectionStatus::stale;
        affected_characteristic_ids.insert(characteristic.id);
    }
    std::set<engineering::Id> affected_target_ids;
    for (auto& target : updated.targets) {
        if (!affected_characteristic_ids.contains(target.characteristic_id)) continue;
        target.status = engineering::TargetStatus::stale;
        affected_target_ids.insert(target.id);
    }
    if (updated.path.has_value() && std::any_of(
            updated.path->samples.begin(), updated.path->samples.end(),
            [&affected_target_ids](const auto& sample) {
                return sample.target_id.has_value() &&
                    affected_target_ids.contains(*sample.target_id);
            })) {
        updated.path->status = engineering::DerivedProductStatus::stale;
        for (auto& sample : updated.path->samples) {
            sample.status = engineering::PathSampleStatus::stale;
        }
    }
    if (!superseded_source_ids.empty()) {
        for (auto& product : updated.derived_products) {
            product.status = engineering::DerivedProductStatus::stale;
            product.stale_reason = "engineering specification source was superseded";
        }
    }

    engineering::EngineeringSource source;
    source.id = source_id;
    source.kind = engineering::SourceKind::specification;
    source.locator = action.locator;
    source.revision = "content-sha256:" + source_digest.hex();
    source.digest = source_digest;
    source.provider_identity = "cartographer.structured-requirement-csv";
    source.provider_version = "1.0.0";
    source.import_settings_digest = structure_digest(
        "cartographer.structured-requirement-csv.v1;exact-topology;human-review-required");
    source.import_run_id = "requirement-csv-" + source_digest.hex();
    source.units = "per-record";
    source.diagnostics.push_back(
        "bounded structured requirement import; every record remains a proposal until human review");
    updated.sources.push_back(std::move(source));

    for (std::size_t index = 0U; index < parsed.value().size(); ++index) {
        const auto& record = parsed.value()[index];
        engineering::EngineeringRequirement requirement;
        requirement.id = first_requirement_id + static_cast<engineering::Id>(index);
        requirement.kind = record.kind;
        requirement.extraction = engineering::ExtractionState::machine_extracted_proposed;
        requirement.provenance.source_id = source_id;
        requirement.provenance.page = record.page;
        requirement.provenance.section = record.section;
        requirement.provenance.callout = record.callout;
        requirement.provenance.extractor_identity =
            "cartographer.structured-requirement-csv/1";
        requirement.provenance.extraction_confidence = 0.0;
        requirement.provenance.original_text = record.original_text;
        requirement.unsupported_content = record.unsupported_content;
        requirement.topology_ids = {topology_ids[index]};
        requirement.nominal = record.nominal;
        requirement.plus = record.plus;
        requirement.minus = record.minus;
        requirement.unit = record.unit;
        requirement.acceptance_rule = record.acceptance_rule;
        updated.requirements.push_back(std::move(requirement));
    }
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("engineering requirement CSV import"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "engineering requirement CSV import digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Import Engineering Requirement CSV");
}

core::Result<DispatchReceipt> ApplicationSession::attach_engineering_evidence(
    const AttachEngineeringEvidenceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering evidence attachment requires the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering evidence attachment was based on a stale project revision"));
    }
    if ((action.kind != engineering::SourceKind::drawing &&
            action.kind != engineering::SourceKind::specification) ||
        action.locator.empty() || action.locator.size() > 255U ||
        action.locator == "." || action.locator == ".." ||
        action.locator.find_first_of("/\\:") != std::string::npos ||
        action.source_bytes.empty() ||
        action.source_bytes.size() > engineering::kMaxEngineeringEvidenceBytes) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering evidence must be a bounded drawing or specification file name and byte stream"));
    }

    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context(
        "engineering evidence attachment"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering evidence attachment was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }

    const auto source_digest = structure_digest(action.source_bytes);
    std::vector<engineering::Id> superseded_source_ids;
    for (const auto& source : current.value().sources) {
        if (source.kind == action.kind && source.locator == action.locator) {
            superseded_source_ids.push_back(source.id);
        }
    }
    std::sort(superseded_source_ids.begin(), superseded_source_ids.end());
    if (!superseded_source_ids.empty()) {
        const auto latest = std::find_if(current.value().sources.begin(),
            current.value().sources.end(), [&superseded_source_ids](const auto& source) {
                return source.id == superseded_source_ids.back();
            });
        if (latest != current.value().sources.end() && latest->digest == source_digest &&
            std::none_of(latest->diagnostics.begin(), latest->diagnostics.end(),
                [](const std::string& diagnostic) {
                    return diagnostic.starts_with("superseded by engineering evidence ") ||
                        diagnostic.find("source revision changed") != std::string::npos;
                })) {
            return accepted("Attach Engineering Evidence", document_.revision());
        }
    }

    engineering::Id source_id = 1U;
    for (const auto& source : current.value().sources) {
        if (source.id == std::numeric_limits<engineering::Id>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "engineering source identity space is exhausted"));
        }
        source_id = std::max(source_id, source.id + 1U);
    }

    auto updated = current.value();
    std::set<engineering::Id> affected_requirement_ids;
    for (auto& previous : updated.sources) {
        if (std::find(superseded_source_ids.begin(), superseded_source_ids.end(),
                previous.id) == superseded_source_ids.end()) {
            continue;
        }
        const std::string supersession = "superseded by engineering evidence " +
            std::to_string(source_id);
        if (std::find(previous.diagnostics.begin(), previous.diagnostics.end(),
                supersession) == previous.diagnostics.end()) {
            previous.diagnostics.push_back(supersession);
        }
        for (auto& requirement : updated.requirements) {
            if (requirement.provenance.source_id == previous.id) {
                requirement.extraction = engineering::ExtractionState::stale;
                affected_requirement_ids.insert(requirement.id);
            }
        }
    }

    std::set<engineering::Id> affected_characteristic_ids;
    for (auto& characteristic : updated.characteristics) {
        if (!affected_requirement_ids.contains(characteristic.requirement_id)) continue;
        characteristic.status = engineering::InspectionStatus::stale;
        affected_characteristic_ids.insert(characteristic.id);
    }
    std::set<engineering::Id> affected_target_ids;
    for (auto& target : updated.targets) {
        if (!affected_characteristic_ids.contains(target.characteristic_id)) continue;
        target.status = engineering::TargetStatus::stale;
        affected_target_ids.insert(target.id);
    }
    if (updated.path.has_value() && std::any_of(
            updated.path->samples.begin(), updated.path->samples.end(),
            [&affected_target_ids](const auto& sample) {
                return sample.target_id.has_value() &&
                    affected_target_ids.contains(*sample.target_id);
            })) {
        updated.path->status = engineering::DerivedProductStatus::stale;
        for (auto& sample : updated.path->samples) {
            sample.status = engineering::PathSampleStatus::stale;
        }
    }
    if (!superseded_source_ids.empty()) {
        for (auto& product : updated.derived_products) {
            product.status = engineering::DerivedProductStatus::stale;
            product.stale_reason = "engineering evidence source was superseded";
        }
    }

    engineering::EngineeringSource source;
    source.id = source_id;
    source.kind = action.kind;
    source.locator = action.locator;
    source.revision = "content-sha256:" + source_digest.hex();
    source.digest = source_digest;
    source.provider_identity = "cartographer.opaque-source-attachment";
    source.provider_version = "1.0.0";
    source.import_settings_digest = structure_digest(
        "cartographer.opaque-source-attachment.v1;mode=digest-only");
    source.import_run_id = "opaque-source-" + source_digest.hex();
    source.units = "unspecified";
    source.diagnostics.push_back(
        "opaque evidence only; no parsing, interpretation, or extraction performed");
    source.diagnostics.push_back(
        "source bytes are not embedded; the content digest binds the external file");
    updated.sources.push_back(std::move(source));
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("engineering evidence attachment"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "engineering evidence attachment digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Attach Engineering Evidence");
}

core::Result<DispatchReceipt> ApplicationSession::create_engineering_requirement(
    const CreateEngineeringRequirementAction& action) {
    const auto has_non_whitespace = [](std::string_view value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n' &&
                byte != '\v' && byte != '\f';
        });
    };
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering requirement authoring requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering requirement was based on a stale project revision"));
    }
    if (action.source_id == 0U || action.topology_ids.empty() ||
        action.topology_ids.size() > 64U || !has_non_whitespace(action.original_text) ||
        !has_non_whitespace(action.acceptance_rule)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering requirement needs source evidence, topology, original text, and an acceptance rule"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context(
        "engineering requirement authoring"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering requirement was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    const auto source = std::find_if(current.value().sources.begin(),
        current.value().sources.end(), [&action](const auto& candidate) {
            return candidate.id == action.source_id;
        });
    if (source == current.value().sources.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "engineering requirement evidence source does not exist"));
    }

    const auto supported_kind = [](engineering::RequirementKind kind) {
        using engineering::RequirementKind;
        return kind == RequirementKind::dimensional_distance ||
            kind == RequirementKind::diameter || kind == RequirementKind::radius ||
            kind == RequirementKind::angle || kind == RequirementKind::thickness ||
            kind == RequirementKind::clearance;
    };
    if (!supported_kind(action.kind)) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "this authoring path supports distance, diameter, radius, angle, thickness, and clearance requirements"));
    }
    const bool angular = action.kind == engineering::RequirementKind::angle;
    const bool positive_nominal = action.kind == engineering::RequirementKind::diameter ||
        action.kind == engineering::RequirementKind::radius ||
        action.kind == engineering::RequirementKind::thickness;
    const bool supported_unit = angular
        ? action.unit == "degrees"
        : action.unit == "millimeters" || action.unit == "meters" || action.unit == "inches";
    if (!supported_unit || !std::isfinite(action.nominal) ||
        (positive_nominal ? action.nominal <= 0.0 : action.nominal < 0.0) ||
        (angular && (action.nominal > 360.0 || action.nominal - action.minus < 0.0 ||
                     action.nominal + action.plus > 360.0))) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering requirement value or unit is outside the supported bounded profile"));
    }

    std::set<std::uint64_t> unique_topology;
    for (const auto topology_id : action.topology_ids) {
        if (topology_id == 0U || !unique_topology.insert(topology_id).second) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "engineering requirement topology identities must be non-zero and unique"));
        }
        const auto binding = std::find_if(current.value().topology.begin(),
            current.value().topology.end(), [topology_id](const auto& candidate) {
                return candidate.id == topology_id;
            });
        if (binding == current.value().topology.end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "engineering requirement topology binding does not exist"));
        }
        const auto topology_source = std::find_if(current.value().sources.begin(),
            current.value().sources.end(), [&binding](const auto& candidate) {
                return candidate.id == binding->source_id;
            });
        if (topology_source == current.value().sources.end() ||
            binding->source_revision != topology_source->revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "engineering requirement topology binding is stale against its CAD source"));
        }
    }

    if (current.value().requirements.size() >= engineering::AuthoringDocument::kMaxCollectionEntries) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering requirement collection reached its configured limit"));
    }
    const auto maximum_id = std::max_element(current.value().requirements.begin(),
        current.value().requirements.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
    const std::uint64_t next_id = maximum_id == current.value().requirements.end()
        ? 1U
        : maximum_id->id == std::numeric_limits<std::uint64_t>::max()
            ? 0U : maximum_id->id + 1U;
    if (next_id == 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering requirement identity space is exhausted"));
    }

    auto updated = current.value();
    engineering::EngineeringRequirement requirement;
    requirement.id = next_id;
    requirement.kind = action.kind;
    requirement.extraction = engineering::ExtractionState::manual;
    requirement.provenance.source_id = action.source_id;
    requirement.provenance.page = action.source_page;
    requirement.provenance.section = action.source_section;
    requirement.provenance.callout = action.source_callout;
    requirement.provenance.extraction_confidence = 0.0;
    requirement.provenance.original_text = action.original_text;
    requirement.topology_ids = action.topology_ids;
    requirement.nominal = action.nominal;
    requirement.plus = action.plus;
    requirement.minus = action.minus;
    requirement.unit = action.unit;
    requirement.acceptance_rule = action.acceptance_rule;
    updated.requirements.push_back(std::move(requirement));
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("engineering requirement authoring"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "engineering requirement authoring digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Create Manual Engineering Requirement");
}

core::Result<DispatchReceipt> ApplicationSession::review_engineering_requirement(
    const ReviewEngineeringRequirementAction& action) {
    const auto has_non_whitespace = [](std::string_view value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n' &&
                byte != '\v' && byte != '\f';
        });
    };
    const auto bounded_single_line = [](std::string_view value) {
        return value.size() <= 4096U && std::all_of(
            value.begin(), value.end(), [](unsigned char byte) {
                return byte >= 0x20U && byte != 0x7fU && byte != '\r' && byte != '\n';
            });
    };
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering requirement review requires the explicit human command boundary"));
    }
    if (action.decision != RequirementReviewDecision::verify &&
        action.decision != RequirementReviewDecision::correct &&
        action.decision != RequirementReviewDecision::reject) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering requirement review decision is invalid"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering requirement review was based on a stale project revision"));
    }
    if (action.requirement_id == 0U || !has_non_whitespace(action.justification) ||
        !bounded_single_line(action.justification)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering requirement review requires an identity and one-line justification"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context(
        "engineering requirement review"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering requirement review was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    const auto requirement = std::find_if(current.value().requirements.begin(),
        current.value().requirements.end(), [&action](const auto& candidate) {
            return candidate.id == action.requirement_id;
        });
    if (requirement == current.value().requirements.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "engineering requirement proposal does not exist"));
    }
    if (!requirement->unsupported_content.empty() &&
        action.decision != RequirementReviewDecision::reject) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "requirement with opaque unsupported source content cannot be verified or corrected"));
    }
    if (!requirement->enabled || requirement->extraction !=
            engineering::ExtractionState::machine_extracted_proposed) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "only enabled, unresolved engineering requirement proposals can be reviewed"));
    }
    const auto source = std::find_if(current.value().sources.begin(),
        current.value().sources.end(), [&requirement](const auto& candidate) {
            return candidate.id == requirement->provenance.source_id;
        });
    if (source == current.value().sources.end() ||
        std::any_of(source->diagnostics.begin(), source->diagnostics.end(),
            [](const std::string& diagnostic) {
                return diagnostic.starts_with("superseded by engineering specification ");
            })) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering requirement source is missing or superseded"));
    }
    for (const auto topology_id : requirement->topology_ids) {
        const auto binding = std::find_if(current.value().topology.begin(),
            current.value().topology.end(), [topology_id](const auto& candidate) {
                return candidate.id == topology_id;
            });
        if (binding == current.value().topology.end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "engineering requirement proposal has an unresolved topology binding"));
        }
        const auto topology_source = std::find_if(current.value().sources.begin(),
            current.value().sources.end(), [&binding](const auto& candidate) {
                return candidate.id == binding->source_id;
            });
        if (topology_source == current.value().sources.end() ||
            topology_source->kind != engineering::SourceKind::step_ap242 ||
            topology_source->revision != binding->source_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "engineering requirement proposal is bound to stale source topology"));
        }
    }

    if (action.decision != RequirementReviewDecision::reject) {
        const auto supported_kind = [](engineering::RequirementKind kind) {
            using engineering::RequirementKind;
            return kind == RequirementKind::dimensional_distance ||
                kind == RequirementKind::diameter || kind == RequirementKind::radius ||
                kind == RequirementKind::angle || kind == RequirementKind::thickness ||
                kind == RequirementKind::clearance;
        };
        const auto candidate_kind = action.decision == RequirementReviewDecision::correct
            ? action.corrected_kind : requirement->kind;
        const auto candidate_nominal = action.decision == RequirementReviewDecision::correct
            ? action.corrected_nominal : requirement->nominal;
        const auto candidate_plus = action.decision == RequirementReviewDecision::correct
            ? action.corrected_plus : requirement->plus;
        const auto candidate_minus = action.decision == RequirementReviewDecision::correct
            ? action.corrected_minus : requirement->minus;
        const std::string_view candidate_unit = action.decision ==
                RequirementReviewDecision::correct
            ? action.corrected_unit : requirement->unit;
        const std::string_view candidate_rule = action.decision ==
                RequirementReviewDecision::correct
            ? action.corrected_acceptance_rule : requirement->acceptance_rule;
        const bool angular = candidate_kind == engineering::RequirementKind::angle;
        const bool positive_nominal =
            candidate_kind == engineering::RequirementKind::diameter ||
            candidate_kind == engineering::RequirementKind::radius ||
            candidate_kind == engineering::RequirementKind::thickness;
        const bool supported_unit = angular
            ? candidate_unit == "degrees"
            : candidate_unit == "millimeters" || candidate_unit == "meters" ||
                candidate_unit == "inches";
        if (!supported_kind(candidate_kind) || !supported_unit ||
            !std::isfinite(candidate_nominal) || !std::isfinite(candidate_plus) ||
            !std::isfinite(candidate_minus) || candidate_plus < 0.0 ||
            candidate_minus < 0.0 ||
            (positive_nominal ? candidate_nominal <= 0.0 : candidate_nominal < 0.0) ||
            (angular && (candidate_nominal > 360.0 ||
                candidate_nominal - candidate_minus < 0.0 ||
                candidate_nominal + candidate_plus > 360.0)) ||
            !has_non_whitespace(candidate_rule) || !bounded_single_line(candidate_rule)) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "reviewed requirement is outside the supported dimensional profile"));
        }
    }

    auto updated = current.value();
    const auto updated_requirement = std::find_if(
        updated.requirements.begin(), updated.requirements.end(), [&action](const auto& candidate) {
            return candidate.id == action.requirement_id;
        });
    updated_requirement->human_review_justification = action.justification;
    if (action.decision == RequirementReviewDecision::verify) {
        updated_requirement->extraction = engineering::ExtractionState::human_verified;
    } else if (action.decision == RequirementReviewDecision::correct) {
        updated_requirement->kind = action.corrected_kind;
        updated_requirement->nominal = action.corrected_nominal;
        updated_requirement->plus = action.corrected_plus;
        updated_requirement->minus = action.corrected_minus;
        updated_requirement->unit = action.corrected_unit;
        updated_requirement->acceptance_rule = action.corrected_acceptance_rule;
        updated_requirement->extraction = engineering::ExtractionState::human_corrected;
    } else {
        updated_requirement->enabled = false;
        updated_requirement->extraction = engineering::ExtractionState::rejected;
    }

    if (action.decision != RequirementReviewDecision::verify) {
        std::set<engineering::Id> affected_characteristic_ids;
        for (auto& characteristic : updated.characteristics) {
            if (characteristic.requirement_id != action.requirement_id) continue;
            characteristic.status = engineering::InspectionStatus::stale;
            affected_characteristic_ids.insert(characteristic.id);
        }
        std::set<engineering::Id> affected_target_ids;
        for (auto& target : updated.targets) {
            if (!affected_characteristic_ids.contains(target.characteristic_id)) continue;
            target.status = engineering::TargetStatus::stale;
            affected_target_ids.insert(target.id);
        }
        if (updated.path.has_value() && std::any_of(
                updated.path->samples.begin(), updated.path->samples.end(),
                [&affected_target_ids](const auto& sample) {
                    return sample.target_id.has_value() &&
                        affected_target_ids.contains(*sample.target_id);
                })) {
            updated.path->status = engineering::DerivedProductStatus::stale;
            for (auto& sample : updated.path->samples) {
                sample.status = engineering::PathSampleStatus::stale;
            }
        }
        for (auto& product : updated.derived_products) {
            product.status = engineering::DerivedProductStatus::stale;
            product.stale_reason = "engineering requirement was corrected or rejected";
        }
    }
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("engineering requirement review"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "engineering requirement review digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    const std::string action_name = action.decision == RequirementReviewDecision::verify
        ? "Verify Engineering Requirement"
        : action.decision == RequirementReviewDecision::correct
            ? "Correct Engineering Requirement" : "Reject Engineering Requirement";
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        action_name);
}

core::Result<DispatchReceipt> ApplicationSession::create_engineering_characteristic(
    const CreateEngineeringCharacteristicAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "inspection characteristic authoring requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection characteristic was based on a stale project revision"));
    }
    if (action.requirement_id == 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection characteristic requires a non-zero requirement identity"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context(
        "inspection characteristic authoring"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection characteristic was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    const auto requirement = std::find_if(current.value().requirements.begin(),
        current.value().requirements.end(), [&action](const auto& candidate) {
            return candidate.id == action.requirement_id;
        });
    if (requirement == current.value().requirements.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "inspection characteristic requirement does not exist"));
    }
    if (!requirement->enabled ||
        requirement->extraction == engineering::ExtractionState::stale ||
        requirement->extraction == engineering::ExtractionState::rejected ||
        !requirement->unsupported_content.empty() ||
        requirement->extraction == engineering::ExtractionState::machine_extracted_proposed ||
        requirement->topology_ids.empty() || requirement->topology_ids.size() > 64U) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection characteristic requires an enabled, verified requirement with bounded topology"));
    }

    if (!engineering::inspection_kind_compatible(requirement->kind, action.kind)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection characteristic kind does not measure the selected requirement"));
    }
    const auto requirement_source = std::find_if(current.value().sources.begin(),
        current.value().sources.end(), [&requirement](const auto& source) {
            return source.id == requirement->provenance.source_id;
        });
    if (requirement_source == current.value().sources.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection characteristic requirement source is unresolved"));
    }
    for (const auto topology_id : requirement->topology_ids) {
        const auto topology = std::find_if(current.value().topology.begin(),
            current.value().topology.end(), [topology_id](const auto& binding) {
                return binding.id == topology_id;
            });
        if (topology == current.value().topology.end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "inspection characteristic topology binding is unresolved"));
        }
        const auto source = std::find_if(current.value().sources.begin(),
            current.value().sources.end(), [&topology](const auto& candidate) {
                return candidate.id == topology->source_id;
            });
        if (source == current.value().sources.end() ||
            topology->source_revision != source->revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "inspection characteristic topology binding is stale against its source"));
        }
    }

    static const std::set<std::string> workcell_axis_directions{
        "+x", "-x", "+y", "-y", "+z", "-z"};
    std::set<std::string> unique_directions;
    for (const auto& direction : action.access.allowed_approach_directions) {
        if (!workcell_axis_directions.contains(direction) ||
            !unique_directions.insert(direction).second) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "allowed approach directions must be unique workcell axes"));
        }
    }
    for (const auto& direction : action.access.forbidden_approach_directions) {
        if (!workcell_axis_directions.contains(direction) ||
            !unique_directions.insert(direction).second) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "approach directions must be unique workcell axes without contradictory rules"));
        }
    }
    if (action.access.allowed_approach_directions.empty() ||
        (!action.sensor.contact_required && action.sensor.max_standoff <= 0.0)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection characteristic requires an allowed approach and a positive non-contact standoff"));
    }
    const auto has_text = [](std::string_view value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return !std::isspace(byte);
        });
    };
    if (!has_text(action.sensor.sensor_type) || !has_text(action.sensor.tool_center_frame) ||
        !has_text(action.coverage.kind)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection characteristic requires non-blank sensor, tool frame, and coverage rule"));
    }

    engineering::InspectionCharacteristic candidate;
    candidate.requirement_id = requirement->id;
    candidate.kind = action.kind;
    candidate.topology_ids = requirement->topology_ids;
    candidate.sensor = action.sensor;
    candidate.coverage = action.coverage;
    candidate.access = action.access;
    candidate.priority = action.priority;
    const auto duplicate = std::find_if(current.value().characteristics.begin(),
        current.value().characteristics.end(), [&candidate](const auto& existing) {
            return existing.requirement_id == candidate.requirement_id &&
                existing.kind == candidate.kind && existing.topology_ids == candidate.topology_ids &&
                existing.sensor.sensor_type == candidate.sensor.sensor_type &&
                existing.sensor.tool_center_frame == candidate.sensor.tool_center_frame &&
                existing.sensor.min_standoff == candidate.sensor.min_standoff &&
                existing.sensor.max_standoff == candidate.sensor.max_standoff &&
                existing.sensor.preferred_standoff == candidate.sensor.preferred_standoff &&
                existing.sensor.orientation_cone_degrees == candidate.sensor.orientation_cone_degrees &&
                existing.sensor.field_of_view_degrees == candidate.sensor.field_of_view_degrees &&
                existing.sensor.min_clearance == candidate.sensor.min_clearance &&
                existing.sensor.line_of_sight_required == candidate.sensor.line_of_sight_required &&
                existing.sensor.contact_required == candidate.sensor.contact_required &&
                existing.sensor.measurement_duration_seconds ==
                    candidate.sensor.measurement_duration_seconds &&
                existing.coverage.kind == candidate.coverage.kind &&
                existing.coverage.percentage == candidate.coverage.percentage &&
                existing.access.allowed_approach_directions ==
                    candidate.access.allowed_approach_directions &&
                existing.access.forbidden_approach_directions ==
                    candidate.access.forbidden_approach_directions &&
                existing.access.minimum_approach_clearance ==
                    candidate.access.minimum_approach_clearance &&
                existing.access.fixture_occlusion_allowed ==
                    candidate.access.fixture_occlusion_allowed &&
                existing.access.robot_base_reposition_permitted ==
                    candidate.access.robot_base_reposition_permitted &&
                existing.priority == candidate.priority &&
                existing.status == engineering::InspectionStatus::authored;
        });
    if (duplicate != current.value().characteristics.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "an identical active inspection characteristic already exists"));
    }
    if (current.value().characteristics.size() >=
        engineering::AuthoringDocument::kMaxCollectionEntries) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "inspection characteristic collection reached its configured limit"));
    }
    std::uint64_t next_id = 1U;
    for (const auto& existing : current.value().characteristics) {
        if (existing.id == std::numeric_limits<std::uint64_t>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "inspection characteristic identity space is exhausted"));
        }
        next_id = std::max(next_id, existing.id + 1U);
    }

    auto updated = current.value();
    candidate.id = next_id;
    if (auto valid = candidate.validate(); !valid) {
        return failure(valid.error().with_context("inspection characteristic fields"));
    }
    updated.characteristics.push_back(std::move(candidate));
    if (updated.path.has_value()) {
        updated.path->status = engineering::DerivedProductStatus::stale;
        for (auto& sample : updated.path->samples) {
            sample.status = engineering::PathSampleStatus::stale;
        }
    }
    for (auto& product : updated.derived_products) {
        product.status = engineering::DerivedProductStatus::stale;
        product.stale_reason = "inspection characteristic changed; path requires regeneration";
    }
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("inspection characteristic authoring"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "inspection characteristic digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Create Inspection Characteristic");
}

core::Result<DispatchReceipt> ApplicationSession::create_engineering_target(
    const CreateEngineeringTargetAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "inspection target authoring requires the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target was based on a stale project revision"));
    }
    if (action.characteristic_id == 0U || action.source_topology_id == 0U ||
        action.pose_frame != "workcell" || action.justification.size() > 1024U ||
        !std::any_of(action.justification.begin(), action.justification.end(),
            [](unsigned char byte) { return !std::isspace(byte); }) ||
        !std::all_of(action.justification.begin(), action.justification.end(),
            [](unsigned char byte) { return byte >= 0x20U && byte != 0x7fU; })) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection target requires a characteristic, topology, workcell frame, and one-line justification"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context("inspection target authoring"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    if (!current.value().workcell.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "inspection target authoring requires an authored workcell frame"));
    }
    const auto characteristic = std::find_if(current.value().characteristics.begin(),
        current.value().characteristics.end(), [&action](const auto& candidate) {
            return candidate.id == action.characteristic_id;
        });
    if (characteristic == current.value().characteristics.end() ||
        characteristic->status != engineering::InspectionStatus::authored) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target requires a current authored characteristic"));
    }
    const auto requirement = std::find_if(current.value().requirements.begin(),
        current.value().requirements.end(), [&characteristic](const auto& candidate) {
            return candidate.id == characteristic->requirement_id;
        });
    if (requirement == current.value().requirements.end() || !requirement->enabled ||
        requirement->extraction == engineering::ExtractionState::stale ||
        requirement->extraction == engineering::ExtractionState::rejected ||
        !requirement->unsupported_content.empty() ||
        requirement->extraction == engineering::ExtractionState::machine_extracted_proposed ||
        std::find(requirement->topology_ids.begin(), requirement->topology_ids.end(),
            action.source_topology_id) == requirement->topology_ids.end() ||
        std::find(characteristic->topology_ids.begin(), characteristic->topology_ids.end(),
            action.source_topology_id) == characteristic->topology_ids.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target topology must remain bound to its verified requirement and characteristic"));
    }
    const auto topology = std::find_if(current.value().topology.begin(),
        current.value().topology.end(), [&action](const auto& binding) {
            return binding.id == action.source_topology_id;
        });
    if (topology == current.value().topology.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target topology binding is unresolved"));
    }
    const auto source = std::find_if(current.value().sources.begin(),
        current.value().sources.end(), [&topology](const auto& candidate) {
            return candidate.id == topology->source_id;
        });
    if (source == current.value().sources.end() ||
        topology->source_revision != source->revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target topology binding is stale against its source"));
    }
    if (!std::isfinite(action.standoff) ||
        (!characteristic->sensor.contact_required && action.standoff <= 0.0) ||
        action.standoff < characteristic->sensor.min_standoff ||
        action.standoff > characteristic->sensor.max_standoff ||
        !action.sensor_pose.finite() ||
        action.sensor_pose.scale.x != 1.0 || action.sensor_pose.scale.y != 1.0 ||
        action.sensor_pose.scale.z != 1.0) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection target pose must be finite, unscaled, and inside the sensor standoff bounds"));
    }

    engineering::InspectionTarget candidate;
    candidate.characteristic_id = characteristic->id;
    candidate.source_topology_id = action.source_topology_id;
    candidate.sensor_pose = action.sensor_pose;
    candidate.sensor_pose.rotation = candidate.sensor_pose.rotation.normalized();
    candidate.standoff = action.standoff;
    candidate.status = engineering::TargetStatus::proposed;
    candidate.generation_basis = "human-authored;frame=workcell;workcell-revision=" +
        current.value().workcell->revision + ";justification=" + action.justification;
    candidate.human_authored = true;
    const auto same_pose = [](const core::Transform& left, const core::Transform& right) {
        return left.translation.x == right.translation.x &&
            left.translation.y == right.translation.y &&
            left.translation.z == right.translation.z &&
            left.rotation.x == right.rotation.x && left.rotation.y == right.rotation.y &&
            left.rotation.z == right.rotation.z && left.rotation.w == right.rotation.w &&
            left.scale.x == right.scale.x && left.scale.y == right.scale.y &&
            left.scale.z == right.scale.z;
    };
    const auto duplicate = std::find_if(current.value().targets.begin(),
        current.value().targets.end(), [&candidate, &same_pose](const auto& existing) {
            return existing.characteristic_id == candidate.characteristic_id &&
                existing.source_topology_id == candidate.source_topology_id &&
                existing.standoff == candidate.standoff &&
                same_pose(existing.sensor_pose, candidate.sensor_pose) &&
                existing.status != engineering::TargetStatus::stale;
        });
    if (duplicate != current.value().targets.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "an equivalent inspection target already exists for this characteristic topology"));
    }
    if (current.value().targets.size() >= engineering::AuthoringDocument::kMaxCollectionEntries) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "inspection target collection reached its configured limit"));
    }
    std::uint64_t next_id = 1U;
    for (const auto& existing : current.value().targets) {
        if (existing.id == std::numeric_limits<std::uint64_t>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "inspection target identity space is exhausted"));
        }
        next_id = std::max(next_id, existing.id + 1U);
    }

    auto updated = current.value();
    candidate.id = next_id;
    if (auto valid = candidate.validate(); !valid) {
        return failure(valid.error().with_context("inspection target fields"));
    }
    updated.targets.push_back(std::move(candidate));
    if (updated.path.has_value()) {
        updated.path->status = engineering::DerivedProductStatus::stale;
        for (auto& sample : updated.path->samples) {
            sample.status = engineering::PathSampleStatus::stale;
        }
    }
    for (auto& product : updated.derived_products) {
        product.status = engineering::DerivedProductStatus::stale;
        product.stale_reason = "inspection target changed; path requires regeneration";
    }
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("inspection target authoring"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("inspection target digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Create Proposed Inspection Target");
}

core::Result<DispatchReceipt> ApplicationSession::update_engineering_workcell(
    const UpdateEngineeringWorkcellAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "workcell edits require the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell edit was based on a stale project revision"));
    }
    const bool edits_entity = action.entity_id.has_value();
    const bool edits_keep_out = action.keep_out_id.has_value();
    const auto has_nonblank_text = [](std::string_view value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return !std::isspace(byte);
        });
    };
    const auto is_single_line = [](std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        });
    };
    if (edits_entity == edits_keep_out || action.expected_workcell_revision.empty() ||
        action.expected_workcell_revision.size() > 128U ||
        action.new_workcell_revision.empty() || action.new_workcell_revision.size() > 128U ||
        action.new_workcell_revision == action.expected_workcell_revision ||
        action.justification.size() > 1024U || !has_nonblank_text(action.justification) ||
        !has_nonblank_text(action.expected_workcell_revision) ||
        !has_nonblank_text(action.new_workcell_revision) ||
        !is_single_line(action.expected_workcell_revision) ||
        !is_single_line(action.new_workcell_revision) || !is_single_line(action.justification)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "workcell edit requires one target, a new revision label, and a one-line justification"));
    }
    if (action.transform.scale.x != 1.0 || action.transform.scale.y != 1.0 ||
        action.transform.scale.z != 1.0 || !action.transform.finite()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "workcell transforms must be finite rigid transforms without scale"));
    }

    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context("workcell edit"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell edit was based on a stale engineering authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    if (!current.value().workcell.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell edit requires an existing authored workcell"));
    }
    const auto& existing_workcell = *current.value().workcell;
    if (existing_workcell.revision != action.expected_workcell_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell edit was based on a stale workcell revision"));
    }
    if (current.value().workcell_revision_history.size() >=
            engineering::AuthoringDocument::kMaxCollectionEntries ||
        std::any_of(current.value().workcell_revision_history.begin(),
            current.value().workcell_revision_history.end(), [&action](const auto& record) {
                return record.revision == action.new_workcell_revision;
            })) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell revision history is full or the new revision already exists"));
    }

    auto updated = current.value();
    core::Transform authored_transform = action.transform;
    authored_transform.rotation = authored_transform.rotation.normalized();
    std::string operation_name;
    std::string subject_kind;
    std::uint64_t subject_id = 0U;
    if (edits_entity) {
        auto entity = std::find_if(updated.workcell->entities.begin(),
            updated.workcell->entities.end(), [&action](const auto& value) {
                return value.id == *action.entity_id;
            });
        bool reference_frame = false;
        if (entity == updated.workcell->entities.end()) {
            entity = std::find_if(updated.workcell->frames.begin(),
                updated.workcell->frames.end(), [&action](const auto& value) {
                    return value.id == *action.entity_id;
                });
            reference_frame = true;
        }
        if (entity == (reference_frame ? updated.workcell->frames.end()
                                      : updated.workcell->entities.end())) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "workcell edit references a missing entity or frame"));
        }
        entity->transform = authored_transform;
        subject_kind = reference_frame ? "frame" : "entity";
        subject_id = entity->id;
        operation_name = reference_frame ? "Update Workcell Frame" : "Update Workcell Entity";
    } else {
        const auto keep_out = std::find_if(updated.workcell->keep_outs.begin(),
            updated.workcell->keep_outs.end(), [&action](const auto& value) {
                return value.id == *action.keep_out_id;
            });
        if (keep_out == updated.workcell->keep_outs.end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "workcell edit references a missing keep-out region"));
        }
        keep_out->transform = authored_transform;
        keep_out->dimensions = action.dimensions;
        subject_kind = "keep-out";
        subject_id = keep_out->id;
        if (auto valid = keep_out->validate(); !valid) {
            return failure(valid.error().with_context("workcell keep-out edit"));
        }
        operation_name = "Update Workcell Keep-Out";
    }
    updated.workcell->revision = action.new_workcell_revision;
    const auto workcell_digest = updated.workcell->canonical_digest();
    if (!workcell_digest) {
        return failure(workcell_digest.error().with_context("workcell revision digest"));
    }
    updated.workcell_revision_history.push_back(engineering::WorkcellRevisionRecord{
        action.new_workcell_revision, action.expected_workcell_revision, subject_kind,
        subject_id, workcell_digest.value(), action.justification});
    for (auto& target : updated.targets) {
        if (target.status != engineering::TargetStatus::rejected) {
            target.status = engineering::TargetStatus::stale;
        }
    }
    if (updated.path.has_value()) {
        updated.path->status = engineering::DerivedProductStatus::stale;
        for (auto& sample : updated.path->samples) {
            sample.status = engineering::PathSampleStatus::stale;
        }
    }
    for (auto& product : updated.derived_products) {
        product.status = engineering::DerivedProductStatus::stale;
        product.stale_reason = "workcell revision changed; inspection targets and paths require review";
    }
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("workcell edit"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("workcell edit digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        std::move(operation_name));
}

core::Result<DispatchReceipt> ApplicationSession::create_engineering_keep_out(
    const CreateEngineeringKeepOutAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "workcell keep-out authoring requires the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell keep-out was based on a stale project revision"));
    }
    const auto has_nonblank_text = [](std::string_view value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return !std::isspace(byte);
        });
    };
    const auto is_single_line = [](std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        });
    };
    if (action.expected_workcell_revision.empty() ||
        action.expected_workcell_revision.size() > 128U ||
        action.new_workcell_revision.empty() || action.new_workcell_revision.size() > 128U ||
        action.new_workcell_revision == action.expected_workcell_revision ||
        action.purpose.size() > 256U || action.justification.size() > 1024U ||
        !has_nonblank_text(action.expected_workcell_revision) ||
        !has_nonblank_text(action.new_workcell_revision) || !has_nonblank_text(action.purpose) ||
        !has_nonblank_text(action.justification) ||
        !is_single_line(action.expected_workcell_revision) ||
        !is_single_line(action.new_workcell_revision) || !is_single_line(action.purpose) ||
        !is_single_line(action.justification) || !action.center.finite() ||
        !action.dimensions.finite() || action.dimensions.x <= 0.0 ||
        action.dimensions.y <= 0.0 || action.dimensions.z <= 0.0) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "box keep-out requires a new workcell revision, purpose, finite positive dimensions, and one-line justification"));
    }

    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context("workcell keep-out authoring"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell keep-out was based on a stale engineering authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    if (!current.value().workcell.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell keep-out authoring requires an existing authored workcell"));
    }
    const auto& existing_workcell = *current.value().workcell;
    if (existing_workcell.revision != action.expected_workcell_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell keep-out was based on a stale workcell revision"));
    }
    if (current.value().workcell_revision_history.size() >=
            engineering::AuthoringDocument::kMaxCollectionEntries ||
        existing_workcell.keep_outs.size() >=
            engineering::AuthoringDocument::kMaxCollectionEntries ||
        std::any_of(current.value().workcell_revision_history.begin(),
            current.value().workcell_revision_history.end(), [&action](const auto& record) {
                return record.revision == action.new_workcell_revision;
            })) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell revision history or keep-out collection is full, or the new revision already exists"));
    }

    auto updated = current.value();
    auto& workcell = *updated.workcell;
    std::uint64_t maximum_id = workcell.id;
    for (const auto& entity : workcell.entities) maximum_id = std::max(maximum_id, entity.id);
    for (const auto& frame : workcell.frames) maximum_id = std::max(maximum_id, frame.id);
    for (const auto& keep_out : workcell.keep_outs) maximum_id = std::max(maximum_id, keep_out.id);
    if (maximum_id == std::numeric_limits<std::uint64_t>::max()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell identity space is exhausted"));
    }
    const std::uint64_t keep_out_id = maximum_id + 1U;
    engineering::KeepOutRegion keep_out;
    keep_out.id = keep_out_id;
    keep_out.kind = engineering::KeepOutKind::box;
    keep_out.purpose = action.purpose;
    keep_out.transform.translation = action.center;
    keep_out.dimensions = action.dimensions;
    keep_out.source = "manual://cartographer/workcell";
    if (auto valid = keep_out.validate(); !valid) {
        return failure(valid.error().with_context("new workcell keep-out"));
    }
    workcell.keep_outs.push_back(std::move(keep_out));
    workcell.revision = action.new_workcell_revision;
    const auto workcell_digest = workcell.canonical_digest();
    if (!workcell_digest) {
        return failure(workcell_digest.error().with_context("workcell revision digest"));
    }
    updated.workcell_revision_history.push_back(engineering::WorkcellRevisionRecord{
        action.new_workcell_revision, action.expected_workcell_revision, "keep-out",
        keep_out_id, workcell_digest.value(), action.justification});
    for (auto& target : updated.targets) {
        if (target.status != engineering::TargetStatus::rejected) {
            target.status = engineering::TargetStatus::stale;
        }
    }
    if (updated.path.has_value()) {
        updated.path->status = engineering::DerivedProductStatus::stale;
        for (auto& sample : updated.path->samples) {
            sample.status = engineering::PathSampleStatus::stale;
        }
    }
    for (auto& product : updated.derived_products) {
        product.status = engineering::DerivedProductStatus::stale;
        product.stale_reason = "workcell revision changed; inspection targets and paths require review";
    }
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("workcell keep-out authoring"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("workcell keep-out digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Create Workcell Keep-Out");
}

core::Result<DispatchReceipt> ApplicationSession::create_engineering_workcell_frame(
    const CreateEngineeringWorkcellFrameAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "workcell frame authoring requires the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell frame was based on a stale project revision"));
    }
    const auto has_nonblank_text = [](std::string_view value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return !std::isspace(byte);
        });
    };
    const auto is_single_line = [](std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        });
    };
    if (action.expected_workcell_revision.empty() ||
        action.expected_workcell_revision.size() > 128U ||
        action.new_workcell_revision.empty() || action.new_workcell_revision.size() > 128U ||
        action.new_workcell_revision == action.expected_workcell_revision ||
        action.name.size() > 256U || action.justification.size() > 1024U ||
        !has_nonblank_text(action.expected_workcell_revision) ||
        !has_nonblank_text(action.new_workcell_revision) || !has_nonblank_text(action.name) ||
        !has_nonblank_text(action.justification) ||
        !is_single_line(action.expected_workcell_revision) ||
        !is_single_line(action.new_workcell_revision) || !is_single_line(action.name) ||
        !is_single_line(action.justification) || !action.transform.finite() ||
        !action.transform.rotation.normalizable() || action.transform.scale.x != 1.0 ||
        action.transform.scale.y != 1.0 || action.transform.scale.z != 1.0) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "reference frame requires a new workcell revision, name, finite rigid pose, and one-line justification"));
    }

    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context("workcell frame authoring"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell frame was based on a stale engineering authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    if (!current.value().workcell.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell frame authoring requires an existing authored workcell"));
    }
    const auto& existing_workcell = *current.value().workcell;
    if (existing_workcell.revision != action.expected_workcell_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "workcell frame was based on a stale workcell revision"));
    }
    if (current.value().workcell_revision_history.size() >=
            engineering::AuthoringDocument::kMaxCollectionEntries ||
        existing_workcell.frames.size() >=
            engineering::AuthoringDocument::kMaxCollectionEntries ||
        std::any_of(current.value().workcell_revision_history.begin(),
            current.value().workcell_revision_history.end(), [&action](const auto& record) {
                return record.revision == action.new_workcell_revision;
            })) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell revision history or frame collection is full, or the new revision already exists"));
    }

    auto updated = current.value();
    auto& workcell = *updated.workcell;
    std::uint64_t maximum_id = workcell.id;
    for (const auto& entity : workcell.entities) maximum_id = std::max(maximum_id, entity.id);
    for (const auto& frame : workcell.frames) maximum_id = std::max(maximum_id, frame.id);
    for (const auto& keep_out : workcell.keep_outs) maximum_id = std::max(maximum_id, keep_out.id);
    if (maximum_id == std::numeric_limits<std::uint64_t>::max()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workcell identity space is exhausted"));
    }
    const std::uint64_t frame_id = maximum_id + 1U;
    engineering::WorkcellEntity frame;
    frame.id = frame_id;
    frame.kind = engineering::WorkcellEntityKind::reference_frame;
    frame.name = action.name;
    frame.transform = action.transform;
    frame.transform.rotation = frame.transform.rotation.normalized();
    frame.source = "manual://cartographer/workcell";
    if (auto valid = frame.validate(); !valid) {
        return failure(valid.error().with_context("new workcell reference frame"));
    }
    workcell.frames.push_back(std::move(frame));
    workcell.revision = action.new_workcell_revision;
    const auto workcell_digest = workcell.canonical_digest();
    if (!workcell_digest) {
        return failure(workcell_digest.error().with_context("workcell revision digest"));
    }
    updated.workcell_revision_history.push_back(engineering::WorkcellRevisionRecord{
        action.new_workcell_revision, action.expected_workcell_revision, "frame",
        frame_id, workcell_digest.value(), action.justification});
    for (auto& target : updated.targets) {
        if (target.status != engineering::TargetStatus::rejected) {
            target.status = engineering::TargetStatus::stale;
        }
    }
    if (updated.path.has_value()) {
        updated.path->status = engineering::DerivedProductStatus::stale;
        for (auto& sample : updated.path->samples) {
            sample.status = engineering::PathSampleStatus::stale;
        }
    }
    for (auto& product : updated.derived_products) {
        product.status = engineering::DerivedProductStatus::stale;
        product.stale_reason = "workcell revision changed; inspection targets and paths require review";
    }
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("workcell frame authoring"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("workcell frame digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Create Workcell Reference Frame");
}

core::Result<DispatchReceipt> ApplicationSession::review_engineering_target(
    const ReviewEngineeringTargetAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "inspection target decisions require the explicit human command boundary"));
    }
    if (action.decision != engineering::TargetStatus::accepted &&
        action.decision != engineering::TargetStatus::rejected) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection target review can only accept or reject a proposed target"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target review was based on a stale project revision"));
    }
    if (action.target_id == 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "inspection target review requires a non-zero target identity"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context(
        "inspection target review"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target review was based on a stale authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    const auto target = std::find_if(current.value().targets.begin(),
        current.value().targets.end(), [&action](const auto& candidate) {
            return candidate.id == action.target_id;
        });
    if (target == current.value().targets.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "inspection target does not exist"));
    }
    if (target->status != engineering::TargetStatus::proposed) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "only proposed inspection targets can receive a human decision"));
    }
    constexpr std::string_view workcell_basis_prefix =
        "human-authored;frame=workcell;workcell-revision=";
    if (target->human_authored && target->generation_basis.starts_with(workcell_basis_prefix)) {
        const auto revision_start = workcell_basis_prefix.size();
        const auto revision_end = target->generation_basis.find(';', revision_start);
        const auto recorded_revision = target->generation_basis.substr(
            revision_start, revision_end == std::string::npos
                ? std::string::npos : revision_end - revision_start);
        if (!current.value().workcell.has_value() || recorded_revision.empty() ||
            current.value().workcell->revision != recorded_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "inspection target workcell frame changed after the pose was authored"));
        }
    }
    const auto characteristic = std::find_if(current.value().characteristics.begin(),
        current.value().characteristics.end(), [&target](const auto& candidate) {
            return candidate.id == target->characteristic_id;
        });
    if (characteristic == current.value().characteristics.end() ||
        characteristic->status != engineering::InspectionStatus::authored ||
        std::find(characteristic->topology_ids.begin(), characteristic->topology_ids.end(),
            target->source_topology_id) == characteristic->topology_ids.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target no longer matches an active characteristic topology binding"));
    }
    const auto requirement = std::find_if(current.value().requirements.begin(),
        current.value().requirements.end(), [&characteristic](const auto& candidate) {
            return candidate.id == characteristic->requirement_id;
        });
    if (requirement == current.value().requirements.end() || !requirement->enabled ||
        requirement->extraction == engineering::ExtractionState::stale ||
        requirement->extraction == engineering::ExtractionState::rejected ||
        !requirement->unsupported_content.empty() ||
        requirement->extraction == engineering::ExtractionState::machine_extracted_proposed ||
        std::find(requirement->topology_ids.begin(), requirement->topology_ids.end(),
            target->source_topology_id) == requirement->topology_ids.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target cannot be reviewed while its requirement is disabled, stale, or unverified"));
    }
    const auto topology = std::find_if(current.value().topology.begin(),
        current.value().topology.end(), [&target](const auto& candidate) {
            return candidate.id == target->source_topology_id;
        });
    if (topology == current.value().topology.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target source topology is unresolved"));
    }
    const auto source = std::find_if(current.value().sources.begin(),
        current.value().sources.end(), [&topology](const auto& candidate) {
            return candidate.id == topology->source_id;
        });
    if (source == current.value().sources.end() ||
        topology->source_revision != source->revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inspection target source topology is stale"));
    }

    auto updated = current.value();
    const auto updated_target = std::find_if(updated.targets.begin(), updated.targets.end(),
        [&action](const auto& candidate) { return candidate.id == action.target_id; });
    updated_target->status = action.decision;
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("inspection target review"));
    }
    const std::string serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "inspection target review digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        action.decision == engineering::TargetStatus::accepted
            ? "Accept Inspection Target" : "Reject Inspection Target");
}

core::Result<DispatchReceipt> ApplicationSession::import_engineering_path_result(
    const ImportEngineeringPathResultAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "VANTA path result import requires the explicit human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "VANTA path result was based on a stale project revision"));
    }
    if (action.result_bytes.empty() ||
        action.result_bytes.size() > engineering::VantaPathResult::kMaxSerializedBytes) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "VANTA path result bytes are empty or exceed the configured bound"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error().with_context("VANTA path result import"));
    if (current.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "VANTA path result was based on a stale engineering authoring revision"));
    }
    if (current.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring revision space is exhausted"));
    }
    const auto decoded = engineering::VantaPathResult::deserialize(action.result_bytes);
    if (!decoded) {
        return failure(decoded.error().with_context("VANTA path result import"));
    }
    const auto package = engineering::InspectionPackage::publish(
        current.value(), document_.revision(), current.value().project_namespace);
    if (!package) {
        return failure(package.error().with_context(
            "VANTA path result source package"));
    }
    const auto package_admission = engineering::admit_for_consumer(package.value());
    if (!package_admission) {
        return failure(package_admission.error().with_context(
            "VANTA path result request admission"));
    }
    if (package_admission.value().status != engineering::PackageAdmissionStatus::admit) {
        std::string reasons;
        for (const auto& reason : package_admission.value().reasons) {
            if (!reasons.empty()) reasons += "; ";
            reasons += reason;
        }
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "VANTA path result request is refused by the local admission contract: " +
                reasons));
    }
    if (decoded.value().project_namespace != package.value().project_namespace ||
        decoded.value().project_revision != package.value().project_revision ||
        decoded.value().authoring_revision != current.value().project_revision ||
        decoded.value().publication_digest != package.value().publication_digest ||
        decoded.value().authoring_digest != package.value().authoring_digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "VANTA path result does not match the current published package digests and revisions"));
    }
    for (const auto& sample : decoded.value().path.samples) {
        if (!sample.target_id.has_value()) continue;
        const auto target = std::find_if(current.value().targets.begin(),
            current.value().targets.end(), [&sample](const engineering::InspectionTarget& value) {
                return value.id == *sample.target_id;
            });
        if (target == current.value().targets.end() ||
            target->status != engineering::TargetStatus::accepted) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "VANTA path result references a missing or non-accepted inspection target"));
        }
    }

    auto updated = current.value();
    if (updated.path.has_value() && updated.path->vanta_receipt.has_value()) {
        const auto& previous = *updated.path->vanta_receipt;
        updated.evidence.runtime_validation.push_back(
            "VANTA path result " + previous.run_id + " with digest " +
            previous.result_digest.hex() + " was superseded by run " + decoded.value().run_id);
    }
    auto imported_path = decoded.value().path;
    const auto reported_status = imported_path.status;
    if (reported_status == engineering::DerivedProductStatus::current) {
        imported_path.status = engineering::DerivedProductStatus::unverified;
    }
    imported_path.vanta_receipt = engineering::VantaPathResultReceipt{
        decoded.value().project_namespace,
        decoded.value().project_revision,
        decoded.value().authoring_revision,
        decoded.value().publication_digest,
        decoded.value().authoring_digest,
        decoded.value().provider_identity,
        decoded.value().provider_version,
        decoded.value().run_id,
        decoded.value().result_digest,
        reported_status};
    updated.path = std::move(imported_path);
    updated.project_revision = updated.project_revision.next();
    if (auto valid = updated.validate(); !valid) {
        return failure(valid.error().with_context("VANTA path result import"));
    }
    const auto serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("VANTA path result digest"));
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Import VANTA Path Result");
}

core::Result<DispatchReceipt> ApplicationSession::invalidate_engineering_source(
    const InvalidateEngineeringSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering source invalidation requires the human command boundary"));
    }
    if (action.source_id == 0U || action.new_revision.empty() || action.new_digest.is_zero()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering source invalidation requires a source, revision, and digest"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error());
    auto updated = current.value();
    if (auto result = updated.invalidate_source(
            action.source_id, action.new_revision, action.new_digest); !result) {
        return failure(result.error().with_context("engineering source invalidation"));
    }
    const auto serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) {
        return failure(digest.error().with_context("engineering invalidation digest"));
    }
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Invalidate Engineering Source");
}

core::Result<DispatchReceipt> ApplicationSession::import_condition_fixture() {
    auto fixture = condition::make_condition_fixture();
    if (!fixture) return failure(fixture.error().with_context("CONDITION source fixture"));
    const auto serialized = fixture.value().serialize();
    const auto digest = fixture.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context("CONDITION source fixture digest"));
    const auto current = document_.condition_authoring();
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import CONDITION Source Fixture", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Import CONDITION Source Fixture");
}

core::Result<DispatchReceipt> ApplicationSession::import_condition_authoring(
    const ImportConditionAuthoringAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION authoring import requires the human command boundary"));
    }
    const auto decoded = condition::AuthoringDocument::deserialize(action.serialized_document);
    if (!decoded) return failure(decoded.error().with_context("CONDITION authoring import"));
    const std::string serialized = decoded.value().serialize();
    const auto digest = decoded.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context("CONDITION authoring import digest"));
    const auto current = document_.condition_authoring();
    if (current.has_value() && current->bytes != serialized) {
        const auto current_authoring = condition::AuthoringDocument::deserialize(current->bytes);
        if (!current_authoring) {
            return failure(current_authoring.error().with_context(
                "current CONDITION authoring revision cannot be verified"));
        }
        if (current_authoring.value().project_namespace == decoded.value().project_namespace &&
            decoded.value().project_revision != current_authoring.value().project_revision.next()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "CONDITION authoring replacement must advance the current namespace by exactly one revision"));
        }
    }
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import CONDITION Authoring Document", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Import CONDITION Authoring Document");
}

core::Result<DispatchReceipt> ApplicationSession::bind_condition_asset_object(
    const BindConditionAssetObjectAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION scene-object binding requires the explicit human authoring boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION scene-object binding was based on a stale project revision"));
    }
    if (action.asset_id == 0U || action.object_id.value == 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION scene-object binding requires an asset and object identity"));
    }
    if (document_.scene().find(action.object_id) == nullptr) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "CONDITION scene-object binding target does not exist in this project"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION scene-object binding"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION scene-object binding was based on a stale authoring revision"));
    }
    if (authoring.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION authoring revision space is exhausted"));
    }
    const auto asset = std::find_if(authoring.value().assets.begin(),
        authoring.value().assets.end(), [&action](const auto& candidate) {
            return candidate.id == action.asset_id;
        });
    if (asset == authoring.value().assets.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "CONDITION scene-object binding asset does not exist"));
    }
    if (asset->authored_object_id == action.object_id.value) {
        return accepted("Bind CONDITION Asset · No Changes", document_.revision());
    }
    asset->authored_object_id = action.object_id.value;
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION scene-object binding"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION scene-object binding digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Bind CONDITION Asset to Scene Object");
}

core::Result<DispatchReceipt> ApplicationSession::add_condition_interaction_anchor(
    const AddConditionInteractionAnchorAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION interaction-anchor authoring requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION interaction-anchor authoring used a stale project revision"));
    }
    if (action.anchor.id != 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION interaction-anchor identity is assigned by the application"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION interaction-anchor authoring"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION interaction-anchor authoring used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U) {
        return failure(invalid_state(
            "CONDITION authoring revision space cannot advance safely"));
    }
    if (document_.revision().exhausted()) {
        return failure(invalid_state("project revision space is exhausted"));
    }

    auto& anchors = authoring.value().interaction_anchors;
    std::uint64_t greatest_id = std::max(
        authoring.value().project_revision.value(), document_.revision().value());
    for (const auto& anchor : anchors) greatest_id = std::max(greatest_id, anchor.id);
    if (greatest_id == std::numeric_limits<condition::Id>::max()) {
        return failure(invalid_state(
            "CONDITION interaction-anchor identity space is exhausted"));
    }
    condition::InteractionAnchor anchor = action.anchor;
    anchor.id = greatest_id + 1U;
    if (auto valid = anchor.validate(); !valid) {
        return failure(valid.error().with_context(
            "CONDITION interaction-anchor authoring"));
    }
    const auto insertion = std::lower_bound(anchors.begin(), anchors.end(), anchor.id,
        [](const auto& existing, condition::Id id) { return existing.id < id; });
    anchors.insert(insertion, std::move(anchor));
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context(
            "CONDITION interaction-anchor authoring"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION interaction-anchor authoring digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Add CONDITION Interaction Anchor");
}

core::Result<DispatchReceipt> ApplicationSession::add_condition_prefab(
    const AddConditionPrefabAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION prefab authoring requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION prefab authoring used a stale project revision"));
    }
    if (action.prefab.id != 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION prefab identity is assigned by the application"));
    }
    if (action.prefab.asset_ids.empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION prefab authoring requires at least one member asset"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION prefab authoring"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION prefab authoring used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U) {
        return failure(invalid_state(
            "CONDITION authoring revision space cannot advance safely"));
    }
    if (document_.revision().exhausted()) {
        return failure(invalid_state("project revision space is exhausted"));
    }

    auto& prefabs = authoring.value().prefabs;
    std::uint64_t greatest_id = std::max(
        authoring.value().project_revision.value(), document_.revision().value());
    for (const auto& prefab : prefabs) greatest_id = std::max(greatest_id, prefab.id);
    if (greatest_id == std::numeric_limits<condition::Id>::max()) {
        return failure(invalid_state("CONDITION prefab identity space is exhausted"));
    }
    condition::PrefabRecord prefab = action.prefab;
    prefab.id = greatest_id + 1U;
    prefab.source_revision = authoring.value().project_revision;
    if (auto valid = prefab.validate(); !valid) {
        return failure(valid.error().with_context("CONDITION prefab authoring"));
    }
    const auto insertion = std::lower_bound(prefabs.begin(), prefabs.end(), prefab.id,
        [](const auto& existing, condition::Id id) { return existing.id < id; });
    prefabs.insert(insertion, std::move(prefab));
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION prefab authoring"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION prefab authoring digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Add CONDITION Prefab");
}

core::Result<DispatchReceipt> ApplicationSession::instantiate_condition_prefab(
    const InstantiateConditionPrefabAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION prefab placement requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION prefab placement used a stale project revision"));
    }
    if (!action.world_position.finite()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION prefab placement position must be finite"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION prefab placement"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION prefab placement used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U || document_.revision().exhausted()) {
        return failure(invalid_state(
            "CONDITION prefab placement cannot safely advance project and authoring revisions"));
    }
    const auto prefab = std::lower_bound(authoring.value().prefabs.begin(),
        authoring.value().prefabs.end(), action.prefab_id,
        [](const auto& item, condition::Id id) { return item.id < id; });
    if (prefab == authoring.value().prefabs.end() || prefab->id != action.prefab_id) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "CONDITION prefab placement references a missing prefab definition"));
    }
    if (prefab->asset_ids.empty() || prefab->asset_ids.size() >
            condition::AuthoringDocument::kMaxPrefabInstanceMembers) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION prefab placement exceeds the supported member bound"));
    }

    std::uint64_t greatest_id = std::max(
        authoring.value().project_revision.value(), document_.revision().value());
    for (const auto& existing : authoring.value().prefab_instances) {
        greatest_id = std::max(greatest_id, existing.id);
    }
    if (greatest_id == std::numeric_limits<condition::Id>::max()) {
        return failure(invalid_state(
            "CONDITION prefab instance identity space is exhausted"));
    }

    struct ResolvedMember {
        const condition::SourceAsset* asset = nullptr;
        scene::ObjectId source_object;
        std::uint64_t mesh_asset_id = 0U;
        core::Revision mesh_revision;
        core::Transform source_world_transform = core::Transform::identity();
    };
    std::vector<ResolvedMember> resolved_members;
    resolved_members.reserve(prefab->asset_ids.size());
    for (const condition::Id asset_id : prefab->asset_ids) {
        const auto asset = std::lower_bound(authoring.value().assets.begin(),
            authoring.value().assets.end(), asset_id,
            [](const auto& item, condition::Id id) { return item.id < id; });
        if (asset == authoring.value().assets.end() || asset->id != asset_id ||
            asset->authored_object_id == 0U) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "CONDITION prefab placement requires every member asset to be bound to a source object"));
        }
        const auto evidence = std::lower_bound(authoring.value().sources.begin(),
            authoring.value().sources.end(), asset->source_id,
            [](const auto& item, condition::Id id) { return item.id < id; });
        if (evidence == authoring.value().sources.end() || evidence->id != asset->source_id ||
            !evidence->current || evidence->revision != asset->source_revision ||
            evidence->digest != asset->source_digest) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "CONDITION prefab placement is blocked by stale source evidence"));
        }
        const scene::ObjectId source_object{asset->authored_object_id};
        const auto* object = document_.scene().find(source_object);
        if (object == nullptr || !object->mesh_asset.has_value()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "CONDITION prefab placement requires a live source object with a mesh"));
        }
        const auto mesh = document_.meshes().find(*object->mesh_asset);
        if (mesh == document_.meshes().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "CONDITION prefab placement source mesh is missing"));
        }
        const auto source_world = document_.scene().world_transform(source_object);
        if (!source_world) return failure(source_world.error().with_context(
            "CONDITION prefab source world transform"));
        resolved_members.push_back(ResolvedMember{
            &*asset, source_object, *object->mesh_asset, mesh->second.revision(),
            source_world.value()});
    }

    const core::Transform source_root_transform =
        resolved_members.front().source_world_transform;
    const auto transforms_close = [](const core::Transform& left,
                                     const core::Transform& right) {
        const auto close = [](double a, double b) {
            const double scale = std::max({1.0, std::abs(a), std::abs(b)});
            return std::abs(a - b) <= 1.0e-10 * scale;
        };
        return close(left.translation.x, right.translation.x) &&
            close(left.translation.y, right.translation.y) &&
            close(left.translation.z, right.translation.z) &&
            close(left.rotation.x, right.rotation.x) &&
            close(left.rotation.y, right.rotation.y) &&
            close(left.rotation.z, right.rotation.z) &&
            close(left.rotation.w, right.rotation.w) &&
            close(left.scale.x, right.scale.x) && close(left.scale.y, right.scale.y) &&
            close(left.scale.z, right.scale.z);
    };

    condition::PrefabInstanceRecord instance;
    instance.id = greatest_id + 1U;
    instance.prefab_id = prefab->id;
    instance.name = action.name.empty()
        ? prefab->name + " Instance " + std::to_string(instance.id)
        : action.name;
    instance.captured_project_revision = document_.revision();
    instance.source_authoring_revision = authoring.value().project_revision;
    instance.prefab_source_revision = prefab->source_revision;
    auto root_transform = source_root_transform;
    root_transform.translation = action.world_position;

    project::ProjectDocument candidate(document_);
    auto root_result = candidate.scene_.create_object(instance.name, root_transform);
    if (!root_result) return failure(root_result.error().with_context(
        "CONDITION prefab instance root"));
    instance.root_object_id = root_result.value().value;
    AffectedSet affected;
    affected.objects.push_back(root_result.value());
    instance.members.reserve(resolved_members.size());
    for (const auto& resolved : resolved_members) {
        const auto source_local = editor::viewport_local_transform_from_world(
            source_root_transform, resolved.source_world_transform);
        if (!source_local) return failure(source_local.error().with_context(
            "CONDITION prefab member transform"));
        if (!transforms_close(
                source_root_transform.combine(source_local.value()),
                resolved.source_world_transform)) {
            return failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "CONDITION prefab member layout contains a transform that cannot be represented exactly as local TRS"));
        }
        auto member_result = candidate.scene_.create_object(
            resolved.asset->stable_name, source_local.value());
        if (!member_result) return failure(member_result.error().with_context(
            "CONDITION prefab instance member"));
        if (auto attached = candidate.scene_.attach_mesh(
                member_result.value(), resolved.mesh_asset_id); !attached) {
            return failure(attached.error().with_context(
                "CONDITION prefab instance mesh binding"));
        }
        if (auto parented = candidate.scene_.set_parent(
                member_result.value(), root_result.value()); !parented) {
            return failure(parented.error().with_context(
                "CONDITION prefab instance hierarchy"));
        }
        affected.objects.push_back(member_result.value());
        instance.members.push_back(condition::PrefabInstanceMember{
            resolved.asset->id,
            resolved.source_object.value,
            member_result.value().value,
            resolved.mesh_asset_id,
            resolved.mesh_revision,
            resolved.asset->source_revision,
            resolved.asset->source_digest,
            resolved.source_world_transform});
    }
    if (auto valid = instance.validate(); !valid) {
        return failure(valid.error().with_context("CONDITION prefab instance"));
    }
    auto& instances = authoring.value().prefab_instances;
    const auto insertion = std::lower_bound(instances.begin(), instances.end(), instance.id,
        [](const auto& existing, condition::Id id) { return existing.id < id; });
    instances.insert(insertion, std::move(instance));
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION prefab instance authoring"));
    }
    const auto serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION prefab instance authoring digest"));
    if (auto set = candidate.set_condition_authoring(serialized, digest.value()); !set) {
        return failure(set.error().with_context(
            "CONDITION prefab instance source binding"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, std::move(candidate), "Instantiate CONDITION Prefab"),
        "Instantiate CONDITION Prefab", std::move(affected));
}

core::Result<DispatchReceipt> ApplicationSession::add_condition_collection(
    const AddConditionCollectionAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION collection authoring requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION collection authoring used a stale project revision"));
    }
    if (action.collection.id != 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION collection identity is assigned by the application"));
    }
    if (action.collection.asset_ids.empty() &&
        action.collection.child_collection_ids.empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION collections require at least one asset or child collection"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION collection authoring"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION collection authoring used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U) {
        return failure(invalid_state(
            "CONDITION authoring revision space cannot advance safely"));
    }
    if (document_.revision().exhausted()) {
        return failure(invalid_state("project revision space is exhausted"));
    }

    auto& collections = authoring.value().collections;
    std::uint64_t greatest_id = std::max(
        authoring.value().project_revision.value(), document_.revision().value());
    for (const auto& collection : collections) {
        greatest_id = std::max(greatest_id, collection.id);
    }
    if (greatest_id == std::numeric_limits<condition::Id>::max()) {
        return failure(invalid_state("CONDITION collection identity space is exhausted"));
    }
    condition::CollectionRecord collection = action.collection;
    collection.id = greatest_id + 1U;
    if (auto valid = collection.validate(); !valid) {
        return failure(valid.error().with_context("CONDITION collection authoring"));
    }
    const auto insertion = std::lower_bound(collections.begin(), collections.end(),
        collection.id, [](const auto& existing, condition::Id id) {
            return existing.id < id;
        });
    collections.insert(insertion, std::move(collection));
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION collection authoring"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION collection authoring digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission, document_, document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Add CONDITION Collection");
}

core::Result<DispatchReceipt> ApplicationSession::add_condition_region_kit(
    const AddConditionRegionKitAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION region-kit authoring requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION region-kit authoring used a stale project revision"));
    }
    if (action.region_kit.id != 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION region-kit identity is assigned by the application"));
    }
    if (action.region_kit.asset_ids.empty() && action.region_kit.prefab_ids.empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION region kits require at least one asset or prefab"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION region-kit authoring"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION region-kit authoring used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U) {
        return failure(invalid_state(
            "CONDITION authoring revision space cannot advance safely"));
    }
    if (document_.revision().exhausted()) {
        return failure(invalid_state("project revision space is exhausted"));
    }

    auto& region_kits = authoring.value().region_kits;
    std::uint64_t greatest_id = std::max(
        authoring.value().project_revision.value(), document_.revision().value());
    for (const auto& kit : region_kits) greatest_id = std::max(greatest_id, kit.id);
    if (greatest_id == std::numeric_limits<condition::Id>::max()) {
        return failure(invalid_state("CONDITION region-kit identity space is exhausted"));
    }
    condition::RegionKitManifest kit = action.region_kit;
    kit.id = greatest_id + 1U;
    if (auto valid = kit.validate(); !valid) {
        return failure(valid.error().with_context("CONDITION region-kit authoring"));
    }
    const auto insertion = std::lower_bound(region_kits.begin(), region_kits.end(), kit.id,
        [](const auto& existing, condition::Id id) { return existing.id < id; });
    region_kits.insert(insertion, std::move(kit));
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION region-kit authoring"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION region-kit authoring digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission, document_, document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Add CONDITION Region Kit");
}

core::Result<DispatchReceipt> ApplicationSession::add_condition_kinematic_assembly(
    const AddConditionKinematicAssemblyAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION articulation authoring requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION articulation authoring used a stale project revision"));
    }
    if (action.assembly.id != 0U || !action.assembly.source_identity.empty() ||
        action.assembly.source_revision.value() != 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION articulation identity and revision are assigned by the application"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION articulation authoring"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION articulation authoring used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U) {
        return failure(invalid_state(
            "CONDITION authoring revision space cannot advance safely"));
    }
    if (document_.revision().exhausted()) {
        return failure(invalid_state("project revision space is exhausted"));
    }
    auto& assemblies = authoring.value().kinematic_assemblies;
    std::uint64_t greatest_id = std::max(
        authoring.value().project_revision.value(), document_.revision().value());
    const auto consider_ids = [&greatest_id](const auto& records) {
        for (const auto& record : records) greatest_id = std::max(greatest_id, record.id);
    };
    consider_ids(authoring.value().sources);
    consider_ids(authoring.value().assets);
    consider_ids(authoring.value().safe_places);
    consider_ids(authoring.value().familiarity);
    consider_ids(authoring.value().view_conditions);
    consider_ids(authoring.value().variants);
    consider_ids(authoring.value().temporal_packages);
    consider_ids(authoring.value().surface_states);
    consider_ids(authoring.value().interaction_anchors);
    consider_ids(authoring.value().collections);
    consider_ids(authoring.value().prefabs);
    consider_ids(authoring.value().region_kits);
    consider_ids(authoring.value().audio_sources);
    consider_ids(authoring.value().acoustic_zones);
    consider_ids(authoring.value().effect_sources);
    consider_ids(authoring.value().particle_emitters);
    consider_ids(authoring.value().field_influences);
    consider_ids(authoring.value().vegetation_scatters);
    consider_ids(authoring.value().state_branches);
    consider_ids(assemblies);
    for (const auto& branch : authoring.value().state_branches) {
        for (const auto& delta : branch.deltas) greatest_id = std::max(greatest_id, delta.id);
    }
    if (greatest_id == std::numeric_limits<condition::Id>::max()) {
        return failure(invalid_state(
            "CONDITION kinematic assembly identity space is exhausted"));
    }

    condition::KinematicAssemblySource assembly = action.assembly;
    assembly.id = greatest_id + 1U;
    assembly.source_identity = "condition.assembly." + std::to_string(assembly.id);
    assembly.source_revision = authoring.value().project_revision.next();
    const auto asset = std::find_if(authoring.value().assets.begin(),
        authoring.value().assets.end(), [&assembly](const auto& candidate) {
            return candidate.id == assembly.asset_id;
        });
    if (asset == authoring.value().assets.end() || asset->authored_object_id == 0U ||
        document_.scene().find(scene::ObjectId{asset->authored_object_id}) == nullptr) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "CONDITION articulation requires an asset bound to an existing scene object"));
    }
    if (auto valid = assembly.validate(); !valid) {
        return failure(valid.error().with_context("CONDITION articulation authoring"));
    }
    if (!condition_kinematic_bindings_current(assembly, document_)) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION articulation requires current scene-object and mesh-revision bindings for every movable part"));
    }
    const auto insertion = std::lower_bound(assemblies.begin(), assemblies.end(), assembly.id,
        [](const auto& existing, condition::Id id) { return existing.id < id; });
    assemblies.insert(insertion, std::move(assembly));
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION articulation authoring"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION articulation authoring digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission, document_, document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Add CONDITION Kinematic Assembly");
}

core::Result<DispatchReceipt> ApplicationSession::replace_condition_authored_record(
    const ReplaceConditionAuthoredRecordAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION record editing requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION record editing used a stale project revision"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION record editing"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION record editing used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U) {
        return failure(invalid_state(
            "CONDITION authoring revision space cannot advance safely"));
    }
    if (document_.revision().exhausted()) {
        return failure(invalid_state("project revision space is exhausted"));
    }

    std::string action_name;
    const auto replaced = std::visit([this, &authoring, &action_name](const auto& candidate)
        -> core::Result<void> {
        using Record = std::decay_t<decltype(candidate)>;
        if (candidate.id == 0U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "CONDITION record edits require an existing application-assigned identity"));
        }
        auto replace_in = [](auto& records, const auto& replacement) -> core::Result<void> {
            const auto found = std::lower_bound(records.begin(), records.end(),
                replacement.id, [](const auto& existing, condition::Id id) {
                    return existing.id < id;
                });
            if (found == records.end() || found->id != replacement.id) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "CONDITION record identity is not present"));
            }
            if (auto valid = replacement.validate(); !valid) {
                return core::Result<void>::failure(valid.error());
            }
            *found = std::move(replacement);
            return core::Result<void>::success();
        };
        core::Result<void> result = core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION record type is unsupported for editing"));
        if constexpr (std::is_same_v<Record, condition::InteractionAnchor>) {
            result = replace_in(authoring.value().interaction_anchors, candidate);
            action_name = "Update CONDITION Interaction Anchor";
        } else if constexpr (std::is_same_v<Record, condition::PrefabRecord>) {
            auto replacement = candidate;
            replacement.source_revision = authoring.value().project_revision;
            result = replace_in(authoring.value().prefabs, replacement);
            action_name = "Update CONDITION Prefab";
        } else if constexpr (std::is_same_v<Record, condition::CollectionRecord>) {
            result = replace_in(authoring.value().collections, candidate);
            action_name = "Update CONDITION Collection";
        } else if constexpr (std::is_same_v<Record, condition::RegionKitManifest>) {
            result = replace_in(authoring.value().region_kits, candidate);
            action_name = "Update CONDITION Region Kit";
        } else if constexpr (std::is_same_v<Record, condition::KinematicAssemblySource>) {
            const auto found = std::lower_bound(
                authoring.value().kinematic_assemblies.begin(),
                authoring.value().kinematic_assemblies.end(), candidate.id,
                [](const auto& existing, condition::Id id) { return existing.id < id; });
            if (found == authoring.value().kinematic_assemblies.end() ||
                found->id != candidate.id) {
                result = core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "CONDITION kinematic assembly identity is not present"));
            } else if (found->source_revision != candidate.source_revision) {
                result = core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "CONDITION kinematic assembly edit used a stale source revision"));
            } else if (auto valid = candidate.validate(); !valid) {
                result = core::Result<void>::failure(valid.error().with_context(
                    "CONDITION kinematic assembly edit"));
            } else if (!condition_kinematic_bindings_current(candidate, document_)) {
                result = core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "CONDITION kinematic assembly edit has invalid or stale part bindings"));
            } else {
                auto replacement = candidate;
                replacement.source_revision = authoring.value().project_revision.next();
                result = replace_in(authoring.value().kinematic_assemblies, replacement);
            }
            action_name = "Update CONDITION Kinematic Assembly";
        }
        return result;
    }, action.record);
    if (!replaced) {
        return failure(replaced.error().with_context("CONDITION record editing"));
    }

    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION record editing"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION record editing digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission, document_, document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        action_name);
}

core::Result<DispatchReceipt> ApplicationSession::delete_condition_authored_record(
    const DeleteConditionAuthoredRecordAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION record deletion requires the explicit human boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION record deletion used a stale project revision"));
    }
    if (action.id == 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION record deletion requires a non-zero identity"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION record deletion"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION record deletion used a stale authoring revision"));
    }
    if (authoring.value().project_revision.value() >=
        core::Revision::max_value() - 1U) {
        return failure(invalid_state(
            "CONDITION authoring revision space cannot advance safely"));
    }
    if (document_.revision().exhausted()) {
        return failure(invalid_state("project revision space is exhausted"));
    }

    auto erase_by_id = [id = action.id](auto& records) -> core::Result<void> {
        const auto found = std::lower_bound(records.begin(), records.end(), id,
            [](const auto& existing, condition::Id target) {
                return existing.id < target;
            });
        if (found == records.end() || found->id != id) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "CONDITION record identity is not present"));
        }
        records.erase(found);
        return core::Result<void>::success();
    };
    std::string action_name;
    core::Result<void> erased = core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument,
        "CONDITION record kind is invalid"));
    switch (action.kind) {
    case ConditionAuthoredRecordKind::interaction_anchor:
        erased = erase_by_id(authoring.value().interaction_anchors);
        action_name = "Delete CONDITION Interaction Anchor";
        break;
    case ConditionAuthoredRecordKind::prefab:
        erased = erase_by_id(authoring.value().prefabs);
        action_name = "Delete CONDITION Prefab";
        break;
    case ConditionAuthoredRecordKind::collection:
        erased = erase_by_id(authoring.value().collections);
        action_name = "Delete CONDITION Collection";
        break;
    case ConditionAuthoredRecordKind::region_kit:
        erased = erase_by_id(authoring.value().region_kits);
        action_name = "Delete CONDITION Region Kit";
        break;
    case ConditionAuthoredRecordKind::kinematic_assembly:
        erased = erase_by_id(authoring.value().kinematic_assemblies);
        action_name = "Delete CONDITION Kinematic Assembly";
        break;
    default:
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION record kind is invalid"));
    }
    if (!erased) {
        return failure(erased.error().with_context("CONDITION record deletion"));
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context(
            "CONDITION record deletion would invalidate references"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION record deletion digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission, document_, document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        action_name);
}

core::Result<DispatchReceipt> ApplicationSession::invalidate_condition_source(
    const InvalidateConditionSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION source invalidation requires the human command boundary"));
    }
    if (action.source_id == 0U || action.new_revision.empty() || action.new_digest.is_zero()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION source invalidation requires a source, revision, and digest"));
    }
    const auto current = condition_authoring();
    if (!current) return failure(current.error());
    auto updated = current.value();
    if (auto result = updated.invalidate_source(
            action.source_id, action.new_revision, action.new_digest); !result) {
        return failure(result.error().with_context("CONDITION source invalidation"));
    }
    const auto serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("CONDITION invalidation digest"));
    const auto before = document_.condition_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Invalidate CONDITION Source");
}

core::Result<DispatchReceipt> ApplicationSession::apply_source_reimport_plan(
    const ApplySourceReimportPlanAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "source reimport adoption requires explicit human review"));
    }
    if (auto result = action.plan.validate(); !result) {
        return failure(result.error().with_context("source reimport review"));
    }
    const bool has_changes = std::any_of(
        action.plan.entries.begin(), action.plan.entries.end(), [](const auto& entry) {
            return entry.change != assets::SourceReimportChange::unchanged;
        });
    if (!has_changes) {
        const auto verified = assets::apply_source_reimport_plan(
            document_.source_asset_registry(), action.plan);
        if (!verified) return failure(verified.error().with_context(
            "source reimport no-change review"));
        return accepted("Review Source Reimport · No Changes", document_.revision());
    }
    const auto digest = action.plan.canonical_digest();
    if (!digest) return failure(digest.error().with_context("source reimport review digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::ApplySourceReimportPlanCommand>(
            admission, document_, action.plan),
        "Apply Source Reimport " + digest.value().hex().substr(0U, 16U));
}

core::Result<DispatchReceipt> ApplicationSession::relink_moved_source(
    const RelinkMovedSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "moved source relinking requires explicit human review"));
    }
    if (!project_path_.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "moved source relinking requires a saved project"));
    }
    const auto staged_path = locate_staged_source(*project_path_, action.staged_digest);
    if (!staged_path) {
        return failure(staged_path.error().with_context("moved source staged bytes"));
    }
    const auto& registry = document_.source_asset_registry();
    const auto active = registry.records().find(action.identity);
    if (active == registry.records().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "source identity is not present in the project registry"));
    }
    const auto relink = assets::plan_source_relink(registry, action.identity,
        active->second.source_generation, staged_path.value(), action.staged_digest);
    if (!relink) {
        return failure(relink.error().with_context("moved source relink review"));
    }
    const auto adoption = assets::plan_source_reimport(registry, {relink.value().incoming});
    if (!adoption) {
        return failure(adoption.error().with_context("moved source relink adoption"));
    }
    const auto digest = relink.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context("moved source relink review digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::ApplySourceReimportPlanCommand>(
            admission, document_, adoption.value()),
        "Relink Moved Source " + digest.value().hex().substr(0U, 16U));
}

core::Result<DispatchReceipt> ApplicationSession::create_spatial_field(
    const CreateSpatialFieldAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "spatial field creation requires the human authoring boundary"));
    }
    if (document_.spatial_fields().contains(action.identity)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "spatial field identity is already active"));
    }
    auto field = spatial_field::ScalarField2D::create(
        action.identity, action.semantic, action.origin_uv, action.cell_size,
        action.width, action.height, action.minimum_value, action.maximum_value,
        action.initial_value);
    if (!field) return failure(field.error().with_context("create spatial field"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSpatialFieldCommand>(
            admission, document_, std::nullopt, std::move(field.value())),
        "Create Spatial Field");
}

core::Result<DispatchReceipt> ApplicationSession::paint_spatial_field(
    const PaintSpatialFieldAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "spatial field paint requires the human authoring boundary"));
    }
    const auto found = document_.spatial_fields().find(action.identity);
    if (found == document_.spatial_fields().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "spatial field identity is not present in this project"));
    }
    auto updated = found->second;
    auto receipt = updated.paint(action.brush);
    if (!receipt) return failure(receipt.error().with_context("paint spatial field"));
    if (!receipt.value().changed) {
        return accepted("Paint Spatial Field (No Change)", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSpatialFieldCommand>(
            admission, document_, found->second, std::move(updated)),
        "Paint Spatial Field");
}

core::Result<DispatchReceipt> ApplicationSession::create_material(
    const CreateMaterialAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material creation requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material creation snapshot no longer matches the project revision"));
    }
    if (action.name.empty() || action.name.size() > 128U ||
        action.name.find('\0') != std::string::npos) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "material name is empty, too long, or contains a null byte"));
    }
    if (auto result = action.definition.validate(); !result) {
        return failure(result.error().with_context("material definition"));
    }
    const auto& existing = document_.material_catalog().materials();
    if (existing.size() >= project::MaterialCatalog::kMaxRecordsPerKind) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported, "project material catalog reached its record limit"));
    }
    render::MaterialId next_id = 1U;
    if (!existing.empty()) {
        const render::MaterialId last_id = existing.rbegin()->first;
        if (last_id == std::numeric_limits<render::MaterialId>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state, "material identity space is exhausted"));
        }
        next_id = last_id + 1U;
    }
    render::MaterialAsset material;
    material.id = next_id;
    material.name = action.name;
    material.definition = action.definition;
    material.revision = core::Revision{1U};
    auto catalog = document_.material_catalog();
    if (auto result = catalog.insert_material(std::move(material)); !result) {
        return failure(result.error().with_context("create material"));
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision, "Create Material");
}

core::Result<DispatchReceipt> ApplicationSession::update_material(
    const UpdateMaterialAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material editing requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material edit snapshot no longer matches the project revision"));
    }
    const auto& existing = document_.material_catalog().materials();
    const auto found = existing.find(action.material_id);
    if (found == existing.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found, "material edit references a missing material"));
    }
    if (found->second.revision != action.expected_material_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material edit snapshot no longer matches the material source revision"));
    }
    if (found->second.revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "material source revision space is exhausted"));
    }
    if (auto result = action.definition.validate(); !result) {
        return failure(result.error().with_context("material definition"));
    }
    auto updated = found->second;
    updated.definition = action.definition;
    updated.revision = found->second.revision.next();
    auto catalog = document_.material_catalog();
    if (auto result = catalog.replace_material(std::move(updated)); !result) {
        return failure(result.error().with_context("update material"));
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision, "Update Material");
}

core::Result<DispatchReceipt> ApplicationSession::set_material_region_assignment(
    const SetMaterialRegionAssignmentAction& action) {
    if (active_source_ != OperationSource::human &&
        active_source_ != OperationSource::agent_workbench) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material-region assignment requires the human or scoped Agent Workbench boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material-region assignment snapshot no longer matches the project revision"));
    }
    if (action.key.mesh_asset == 0U || action.key.region_id == 0U ||
        (!action.material.has_value() && action.variant.has_value()) ||
        (action.material.has_value() && action.material.value() == 0U)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "material-region assignment requires non-zero identities and a material for variants"));
    }

    auto catalog = document_.material_catalog();
    const auto current = catalog.assignments().find(action.key);
    if (!action.material.has_value()) {
        if (current == catalog.assignments().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material-region assignment to clear does not exist"));
        }
        if (auto result = catalog.erase_assignment(action.key); !result) {
            return failure(result.error().with_context("clear material-region assignment"));
        }
    } else {
        if (!catalog.materials().contains(action.material.value())) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material-region assignment references a missing material"));
        }
        if (action.variant.has_value() &&
            !catalog.variants().contains(action.variant.value())) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material-region assignment references a missing material variant"));
        }
        const project::MaterialRegionAssignment assignment{
            action.key, action.material.value(), action.variant};
        if (current != catalog.assignments().end()) {
            if (current->second.material == assignment.material &&
                current->second.variant == assignment.variant) {
                return failure(invalid_state(
                    "material region already uses the selected material"));
            }
            if (auto result = catalog.replace_assignment(assignment); !result) {
                return failure(result.error().with_context("replace material-region assignment"));
            }
        } else if (auto result = catalog.insert_assignment(assignment); !result) {
            return failure(result.error().with_context("assign material region"));
        }
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision,
        "Set Material Region Assignment");
}

core::Result<DispatchReceipt> ApplicationSession::generate_unique_lightmap_uv(
    const GenerateUniqueLightmapUvAction& action) {
    if (active_source_ != OperationSource::human &&
        active_source_ != OperationSource::agent_workbench) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "lightmap UV generation requires the human or scoped Agent Workbench boundary"));
    }
    if (!action.object || action.output_uv_set == 0U || action.resolution < 16U ||
        action.padding_texels == 0U || action.maximum_udim_tiles == 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "lightmap UV generation requires non-zero object, set, padding, and tile values"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "lightmap UV generation snapshot no longer matches the project revision"));
    }
    const auto* object = document_.scene().find(action.object);
    if (object == nullptr || !object->mesh_asset.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "lightmap UV generation target no longer has an authored mesh"));
    }
    if (object->locked || !object->visible) {
        return failure(invalid_state(
            "hidden or locked scene objects cannot receive generated lightmap UVs"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh = document_.meshes().find(mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "lightmap UV generation target references a missing mesh asset"));
    }
    for (const auto& instance : document_.scene().objects_sorted()) {
        if (instance.mesh_asset == object->mesh_asset && instance.locked) {
            return failure(invalid_state(
                "lightmap UV generation would affect a locked shared-mesh instance"));
        }
    }
    const std::string suffix = std::to_string(action.output_uv_set);
    const auto recipe = production::make_unique_lightmap_uv_recipe(
        document_, mesh_asset, action.output_uv_set,
        "Agent Lightmap " + suffix, "uv.agent.lightmap." + suffix,
        action.resolution, action.padding_texels, action.maximum_udim_tiles);
    if (!recipe) return failure(recipe.error().with_context("lightmap UV recipe"));
    const auto artifact = production::compile_unique_lightmap_uv(recipe.value(), mesh->second);
    if (!artifact) return failure(artifact.error().with_context("lightmap UV generation"));
    auto updated_mesh = production::with_unique_lightmap_uv(mesh->second, artifact.value());
    if (!updated_mesh) {
        return failure(updated_mesh.error().with_context("lightmap UV artifact application"));
    }
    const std::string digest_operation =
        production::lightmap_uv_transaction_operation(artifact.value());
    if (digest_operation.empty()) {
        return failure(invalid_state("lightmap UV artifact has no transaction digest"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectMeshCommand>(
            admission, document_, mesh_asset, action.expected_project_revision,
            mesh->second.revision(), std::move(updated_mesh.value())),
        "Generate Unique Lightmap UVs " + digest_operation,
        AffectedSet{{action.object}, {}, {}, {}});
}

core::Result<DispatchReceipt> ApplicationSession::set_material_graph(
    const SetMaterialGraphAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material graph authoring requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material graph edit snapshot no longer matches the project revision"));
    }
    if (action.expected_graph_revision.has_value() !=
        action.expected_graph_digest.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "material graph edits require both an expected source revision and digest"));
    }
    if (auto result = action.graph.validate(); !result) {
        return failure(result.error().with_context("material graph source"));
    }

    auto catalog = document_.material_catalog();
    const auto found = catalog.material_graphs().find(action.graph.identity);
    if (!action.expected_graph_revision.has_value()) {
        if (found != catalog.material_graphs().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "material graph identity already exists; update its exact source instead"));
        }
        if (action.graph.revision != core::Revision{1U}) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "new material graph source must begin at revision one"));
        }
        if (!action.graph.retired_node_ids.empty()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "new material graph sources cannot invent retired node lineage"));
        }
        if (auto result = catalog.insert_material_graph(action.graph); !result) {
            return failure(result.error().with_context("create material graph"));
        }
    } else {
        if (found == catalog.material_graphs().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material graph update references a missing source"));
        }
        if (found->second.revision != *action.expected_graph_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material graph source revision no longer matches the edit snapshot"));
        }
        const auto current_digest = found->second.canonical_digest();
        if (!current_digest || current_digest.value() != *action.expected_graph_digest) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material graph source digest no longer matches the edit snapshot"));
        }
        if (found->second.revision.exhausted()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "material graph source revision space is exhausted"));
        }
        if (action.graph.revision != found->second.revision.next()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "updated material graph must advance exactly one source revision"));
        }
        std::set<std::uint64_t> expected_retired(
            found->second.retired_node_ids.begin(),
            found->second.retired_node_ids.end());
        for (const auto& previous_node : found->second.nodes) {
            const bool retained = std::any_of(
                action.graph.nodes.begin(), action.graph.nodes.end(),
                [&previous_node](const auto& candidate) {
                    return candidate.id == previous_node.id;
                });
            if (!retained) expected_retired.insert(previous_node.id);
        }
        const std::vector<std::uint64_t> expected_retired_ids(
            expected_retired.begin(), expected_retired.end());
        if (action.graph.retired_node_ids != expected_retired_ids) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "graph edits must preserve prior node tombstones and record each removed node exactly once"));
        }
        if (auto result = catalog.replace_material_graph(action.graph); !result) {
            return failure(result.error().with_context("update material graph"));
        }
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision,
        "Set Material Graph");
}

core::Result<DispatchReceipt> ApplicationSession::set_material_graph_binding(
    const SetMaterialGraphBindingAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material graph binding requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material graph binding snapshot no longer matches the project revision"));
    }
    const auto& existing = document_.material_catalog().materials();
    const auto material = existing.find(action.material_id);
    if (material == existing.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "material graph binding references a missing material"));
    }
    if (material->second.revision != action.expected_material_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material graph binding snapshot no longer matches the material source revision"));
    }
    if (material->second.revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "material source revision space is exhausted"));
    }
    if (action.graph_identity.has_value() != action.expected_graph_revision.has_value() ||
        action.graph_identity.has_value() != action.expected_graph_digest.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "graph binding requires the exact graph identity, revision, and digest"));
    }

    auto catalog = document_.material_catalog();
    if (action.graph_identity.has_value()) {
        const auto graph = catalog.material_graphs().find(*action.graph_identity);
        if (graph == catalog.material_graphs().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material graph binding references a missing graph source"));
        }
        const auto digest = graph->second.canonical_digest();
        if (!digest || graph->second.revision != *action.expected_graph_revision ||
            digest.value() != *action.expected_graph_digest) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material graph source revision or digest no longer matches the binding snapshot"));
        }
        if (auto result = catalog.bind_material_graph(
                action.material_id, *action.graph_identity); !result) {
            return failure(result.error().with_context("bind material graph source"));
        }
    } else if (auto result = catalog.unbind_material_graph(action.material_id); !result) {
        return failure(result.error().with_context("unbind material graph source"));
    }

    auto updated_material = material->second;
    updated_material.revision = material->second.revision.next();
    if (auto result = catalog.replace_material(std::move(updated_material)); !result) {
        return failure(result.error().with_context("advance material graph binding revision"));
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision,
        action.graph_identity.has_value() ? "Bind Material Graph" : "Unbind Material Graph");
}

core::Result<DispatchReceipt> ApplicationSession::import_material_texture_source(
    const ImportMaterialTextureSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material texture intake requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material texture import snapshot no longer matches the project revision"));
    }
    if (!project_path_.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "save the project before importing a managed material texture source"));
    }
    if (action.content_digest.is_zero()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "material texture import requires a non-zero content digest"));
    }
    const auto& existing = document_.material_catalog().textures();
    if (existing.size() >= project::MaterialCatalog::kMaxRecordsPerKind) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "project material texture source count reached its limit"));
    }
    if (std::any_of(existing.begin(), existing.end(), [&action](const auto& item) {
            return item.second.content_digest == action.content_digest;
        })) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "this content-addressed texture source is already in the project"));
    }

    const auto inspected = inspect_project_texture_blob(
        *project_path_, action.content_digest, action.format,
        action.width, action.height);
    if (!inspected) {
        return failure(inspected.error().with_context("import material texture source"));
    }

    assets::TextureAssetId next_id = 1U;
    if (!existing.empty()) {
        const assets::TextureAssetId last_id = existing.rbegin()->first;
        if (last_id == std::numeric_limits<assets::TextureAssetId>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "material texture source identity space is exhausted"));
        }
        next_id = last_id + 1U;
    }
    std::error_code error;
    const auto requested_root = project_path_->parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path_->parent_path();
    if (error) return failure(core::Diagnostic(
        core::ErrorCode::io_error,
        "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) return failure(core::Diagnostic(
        core::ErrorCode::io_error,
        "unable to resolve the saved project directory"));
    assets::BlobStore store(project_root / ".carto-assets" / "source-blobs");

    project::TextureSourceRecord source;
    source.id = next_id;
    source.name = "Texture " + action.content_digest.hex().substr(0U, 12U);
    source.relative_locator = store.path_for(action.content_digest)
        .lexically_relative(project_root).generic_string();
    source.content_digest = action.content_digest;
    source.format = inspected.value().format;
    source.width = inspected.value().width;
    source.height = inspected.value().height;
    source.provenance = "cartographer.native_texture_import.v1";
    source.revision = core::Revision{1U};

    auto catalog = document_.material_catalog();
    if (auto result = catalog.insert_texture(std::move(source)); !result) {
        return failure(result.error().with_context("create material texture source"));
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision,
        "Import Material Texture Source");
}

core::Result<DispatchReceipt> ApplicationSession::commit_material_catalog(
    project::MaterialCatalog catalog,
    core::Revision expected_project_revision,
    std::string action) {
    if (active_source_ != OperationSource::human &&
        active_source_ != OperationSource::agent_workbench) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material catalog changes require the human or scoped Agent Workbench boundary"));
    }
    if (document_.revision() != expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material catalog snapshot no longer matches the project revision"));
    }
    if (auto result = catalog.validate(); !result) {
        return failure(result.error().with_context("material catalog validation"));
    }
    if (catalog.serialize() == document_.material_catalog().serialize()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument, "material catalog edit contains no changes"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectMaterialCatalogCommand>(
            admission, document_, document_.material_catalog(), std::move(catalog)),
        std::move(action));
}

core::Result<DispatchReceipt> ApplicationSession::set_presentation_catalog(
    const SetPresentationCatalogAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "scene presentation authoring requires the human authoring boundary"));
    }
    if (project_generation_ != action.expected_project_generation ||
        document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "scene presentation snapshot no longer matches the project generation and revision"));
    }
    const auto& before = document_.presentation_catalog();
    const auto& after = action.catalog;
    if (auto result = after.validate(); !result) {
        return failure(result.error().with_context("scene presentation validation"));
    }
    if (before.serialize() == after.serialize()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "scene presentation edit contains no changes"));
    }

    const auto camera_record = [](project::AuthoredCamera camera) {
        project::PresentationCatalog single;
        if (!single.insert_camera(std::move(camera))) return std::string{};
        return single.serialize();
    };
    for (const auto& [object, previous] : before.cameras()) {
        const auto next = after.cameras().find(object);
        if (next == after.cameras().end()) continue;
        if (next->second.identity != previous.identity) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "authored camera identity is stable and cannot be replaced"));
        }
        auto normalized_next = next->second;
        normalized_next.revision = previous.revision;
        const bool changed = camera_record(previous) != camera_record(normalized_next);
        const core::Revision required_revision = changed
            ? previous.revision.next() : previous.revision;
        if (next->second.revision != required_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "authored camera revision must advance exactly once when its source changes"));
        }
    }
    for (const auto& [object, camera] : after.cameras()) {
        if (!before.cameras().contains(object) && camera.revision != core::Revision{1U}) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "new authored cameras must begin at source revision one"));
        }
    }
    const auto light_record = [](project::AuthoredLight light) {
        project::PresentationCatalog single;
        if (!single.insert_light(std::move(light))) return std::string{};
        return single.serialize();
    };
    for (const auto& [object, previous] : before.lights()) {
        const auto next = after.lights().find(object);
        if (next == after.lights().end()) continue;
        if (next->second.identity != previous.identity) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "authored light identity is stable and cannot be replaced"));
        }
        auto normalized_next = next->second;
        normalized_next.revision = previous.revision;
        const bool changed = light_record(previous) != light_record(normalized_next);
        const core::Revision required_revision = changed
            ? previous.revision.next() : previous.revision;
        if (next->second.revision != required_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "authored light revision must advance exactly once when its source changes"));
        }
    }
    for (const auto& [object, light] : after.lights()) {
        if (!before.lights().contains(object) && light.revision != core::Revision{1U}) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "new authored lights must begin at source revision one"));
        }
    }

    const auto reference_plane_record = [](project::ReferenceImagePlane plane) {
        project::PresentationCatalog single;
        if (!single.insert_reference_plane(std::move(plane))) return std::string{};
        return single.serialize();
    };
    const auto locked_plane_source_record = [&reference_plane_record](
            project::ReferenceImagePlane plane) {
        plane.opacity = 1.0;
        plane.visible = true;
        plane.locked = false;
        plane.revision = core::Revision{1U};
        return reference_plane_record(std::move(plane));
    };
    for (const auto& [identity, previous] : before.reference_planes()) {
        const auto next = after.reference_planes().find(identity);
        if (next == after.reference_planes().end()) {
            if (previous.locked) {
                return failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "locked reference-image planes cannot be removed"));
            }
            continue;
        }
        if (previous.locked && locked_plane_source_record(previous) !=
                locked_plane_source_record(next->second)) {
            return failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "locked reference-image source, placement, and calibration cannot change"));
        }
        auto normalized_next = next->second;
        normalized_next.revision = previous.revision;
        const bool changed = reference_plane_record(previous) !=
            reference_plane_record(normalized_next);
        const core::Revision required_revision = changed
            ? previous.revision.next() : previous.revision;
        if (next->second.revision != required_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "reference-image revision must advance exactly once when its source changes"));
        }
    }
    for (const auto& [identity, plane] : after.reference_planes()) {
        if (!before.reference_planes().contains(identity) &&
            plane.revision != core::Revision{1U}) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "new reference-image planes must begin at source revision one"));
        }
    }

    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectPresentationCatalogCommand>(
            admission, document_, before, after),
        "Set Scene Presentation");
}

core::Result<DispatchReceipt> ApplicationSession::set_project_authoring_standards(
    const SetProjectAuthoringStandardsAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "project authoring standards require the human authoring boundary"));
    }
    if (project_generation_ != action.expected_project_generation ||
        document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project standards snapshot no longer matches the project generation and revision"));
    }
    if (auto result = action.standards.validate(); !result) {
        return failure(result.error().with_context("project authoring standards validation"));
    }
    if (action.standards.serialize() == document_.authoring_standards().serialize()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project standards edit contains no changes"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectAuthoringStandardsCommand>(
            admission, document_, document_.authoring_standards(), action.standards),
        "Update Project Standards");
}

core::Result<DispatchReceipt> ApplicationSession::set_road_authoring(
    const SetRoadAuthoringAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "road authoring requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road authoring snapshot no longer matches the project revision"));
    }
    if (auto result = action.centerline.validate(); !result) {
        return failure(result.error().with_context("road centerline authoring"));
    }
    if (auto result = action.road.validate(); !result) {
        return failure(result.error().with_context("road source authoring"));
    }
    if (action.road.centerline_identity != action.centerline.source_identity ||
        action.road.centerline_revision != action.centerline.source_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "road source must bind the exact authored centerline identity and revision"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectRoadAuthoringCommand>(
            admission, document_, action.centerline, action.road),
        "Set Road Authoring");
}

core::Result<DispatchReceipt> ApplicationSession::create_road_surface_instance(
    const CreateRoadSurfaceInstanceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "committing editable road geometry requires explicit human approval"));
    }
    if (action.expected_project_generation != project_generation_ ||
        action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road mesh request was authored against a stale project snapshot"));
    }
    if (action.samples_per_segment == 0U ||
        action.samples_per_segment > geometry::CurvePath::kMaxSamplesPerSegment ||
        action.expected_road_digest.is_zero() ||
        action.expected_centerline_digest.is_zero()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "road mesh request has an invalid sampling bound or missing source digest"));
    }

    const auto preview = road_surface_preview(
        action.road_identity, action.samples_per_segment);
    if (!preview) {
        return failure(preview.error().with_context("road mesh source revalidation"));
    }
    if (preview.value().project_generation != action.expected_project_generation ||
        preview.value().road_revision != action.expected_road_revision ||
        preview.value().road_digest != action.expected_road_digest ||
        preview.value().mesh.centerline_identity != action.centerline_identity ||
        preview.value().mesh.centerline_revision != action.expected_centerline_revision ||
        preview.value().centerline_digest != action.expected_centerline_digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road or centerline source changed after the displayed preview was built"));
    }
    const auto road = document_.road_sources().find(action.road_identity);
    const auto centerline = document_.curve_paths().find(action.centerline_identity);
    if (road == document_.road_sources().end() ||
        centerline == document_.curve_paths().end() ||
        road->second.source_revision != action.expected_road_revision ||
        centerline->second.source_revision != action.expected_centerline_revision ||
        road->second.centerline_identity != action.centerline_identity ||
        road->second.centerline_revision != action.expected_centerline_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road mesh source pair is no longer the exact reviewed source pair"));
    }

    auto mesh = geometry::build_road_surface_editable_mesh(preview.value().mesh);
    if (!mesh) {
        return failure(mesh.error().with_context("road editable mesh generation"));
    }
    const std::string road_snapshot = road->second.serialize();
    const std::string centerline_snapshot = centerline->second.serialize();
    const auto* road_bytes = reinterpret_cast<const std::uint8_t*>(road_snapshot.data());
    const auto* centerline_bytes =
        reinterpret_cast<const std::uint8_t*>(centerline_snapshot.data());
    const assets::Sha256Digest generated_mesh_digest =
        canonical_mesh_geometry_digest(mesh.value());
    project::RoadSurfaceInstanceReceipt receipt{
        {}, 0U, road->second.source_identity, road->second.source_revision,
        road->second.surface_class, road_snapshot,
        assets::sha256(std::span<const std::uint8_t>(road_bytes, road_snapshot.size())),
        centerline->second.source_identity, centerline->second.source_revision,
        centerline_snapshot,
        assets::sha256(std::span<const std::uint8_t>(
            centerline_bytes, centerline_snapshot.size())),
        action.samples_per_segment, generated_mesh_digest};
    if (document_.revision() != action.expected_project_revision ||
        project_generation_ != action.expected_project_generation) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project changed while the road mesh candidate was being generated"));
    }

    const std::string object_name = "Road " + road->second.source_identity + " r" +
        std::to_string(road->second.source_revision.value());
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission, document_, object_name, std::move(mesh).value(),
            core::Transform::identity(), std::nullopt, std::nullopt,
            std::move(receipt)),
        "Create Road Surface · " + road->second.source_identity + " · r" +
            std::to_string(road->second.source_revision.value()));
}

core::Result<DispatchReceipt> ApplicationSession::set_sweep_source(
    const SetSweepSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "procedural sweep authoring requires the human command boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "procedural sweep authoring snapshot no longer matches the project revision"));
    }
    if (auto result = action.source.validate(); !result) {
        return failure(result.error().with_context("procedural sweep source authoring"));
    }
    const auto centerline = document_.curve_paths().find(action.source.centerline_identity);
    if (centerline == document_.curve_paths().end() ||
        centerline->second.source_revision != action.source.centerline_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "procedural sweep must bind an exact current centerline source"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSweepSourceCommand>(
            admission, document_, action.source),
        "Set Procedural Sweep Source");
}

core::Result<DispatchReceipt> ApplicationSession::set_biological_authoring(
    const SetBiologicalAuthoringAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "biological authoring requires the human command boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological authoring draft targets a stale project revision"));
    }
    if (auto result = action.source.validate(); !result) {
        return failure(result.error().with_context("biological authoring source"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectBiologicalAuthoringCommand>(
            admission, document_, action.source),
        "Set Biological Authoring Source · asset " +
            std::to_string(action.source.source.asset_id));
}

core::Result<DispatchReceipt> ApplicationSession::set_sign_text_source(
    const SetSignTextSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "sign-text authoring requires the human command boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign-text authoring snapshot no longer matches the project revision"));
    }
    if (auto result = action.source.validate(); !result) {
        return failure(result.error().with_context("sign-text source authoring"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSignTextSourceCommand>(
            admission, document_, action.source),
        "Set Sign Text Source");
}

core::Result<DispatchReceipt> ApplicationSession::set_decal_placement(
    const SetDecalPlacementAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "decal placement requires explicit human approval"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "decal placement targets a stale project revision"));
    }
    if (auto preview = decal_placement_preview(
            action.placement, action.expected_project_revision); !preview) {
        return failure(preview.error());
    }

    const auto stored = document_.decal_placements().find(action.placement.id);
    if (stored == document_.decal_placements().end()) {
        if (action.expected_placement_revision != core::Revision{} ||
            action.placement.revision != core::Revision{1U}) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "new decal placement must start at revision one with no prior revision"));
        }
    } else {
        if (stored->second.revision != action.expected_placement_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "decal placement edit targets a stale placement revision"));
        }
        const auto current = production::load_decal_placement(document_, action.placement.id);
        if (!current) return failure(current.error().with_context("current decal placement"));
        const auto revised = production::revise_decal_placement(
            current.value(), action.expected_placement_revision, action.placement);
        if (!revised) return failure(revised.error().with_context("revise decal placement"));
        const auto encoded_current = production::serialize_decal_placement(current.value());
        const auto encoded_desired = production::serialize_decal_placement(revised.value());
        if (!encoded_current || !encoded_desired) {
            return failure(!encoded_current ? encoded_current.error() : encoded_desired.error());
        }
        if (encoded_current.value() == encoded_desired.value()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "decal placement edit contains no changes"));
        }
    }

    const auto encoded = production::serialize_decal_placement(action.placement);
    if (!encoded) return failure(encoded.error().with_context("serialize decal placement"));
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.value().data());
    const auto digest = assets::sha256(std::span<const std::uint8_t>(
        bytes, encoded.value().size()));
    project::ProjectDocument candidate(document_);
    const auto admitted_revision = candidate.set_decal_placement(
        action.placement.id, action.expected_placement_revision, encoded.value(), digest);
    if (!admitted_revision) {
        return failure(admitted_revision.error().with_context("stage decal placement"));
    }
    if (admitted_revision.value() != action.placement.revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "decal placement payload revision does not match retained project lineage"));
    }
    if (auto result = candidate.validate(); !result) {
        return failure(result.error().with_context("decal placement project validation"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, std::move(candidate), "Set Decal Placement"),
        "Set Decal Placement · " + std::to_string(action.placement.id));
}

core::Result<DispatchReceipt> ApplicationSession::create_sign_text_object(
    const CreateSignTextObjectAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "creating editable sign geometry requires explicit human approval"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign geometry request was authored against a stale project revision"));
    }
    const auto source = document_.sign_text_sources().find(action.source_identity);
    if (source == document_.sign_text_sources().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "sign geometry requires a saved Cartographer sign-text source"));
    }
    if (source->second.source_revision != action.expected_source_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign geometry request does not match the current source revision"));
    }
    const auto product = geometry::build_sign_text(source->second);
    if (!product) {
        return failure(product.error().with_context("sign-text geometry generation"));
    }
    auto mesh = geometry::combine_sign_text(product.value());
    if (!mesh) {
        return failure(mesh.error().with_context("sign-text mesh creation"));
    }
    const std::string source_snapshot = source->second.serialize();
    const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(source_snapshot.data());
    project::SignTextInstanceReceipt source_receipt{
        {}, 0U, source->second.source_identity, source->second.source_revision,
        source_snapshot,
        assets::sha256(std::span<const std::uint8_t>(source_bytes, source_snapshot.size()))};
    const std::string object_name = "Sign " + source->second.source_identity + " r" +
        std::to_string(source->second.source_revision.value());
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission, document_, object_name, std::move(mesh).value(),
            core::Transform::identity(), std::nullopt, std::move(source_receipt)),
        "Create Sign · " + source->second.source_identity + " · r" +
            std::to_string(source->second.source_revision.value()));
}

core::Result<DispatchReceipt> ApplicationSession::create_biological_proxy_instance(
    const CreateBiologicalProxyInstanceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "committing a biological proxy requires explicit human approval"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological proxy request was authored against a stale project revision"));
    }
    if (action.source_key.first.empty() || action.source_key.second == 0U ||
        action.expected_source_revision.value() == 0U || !action.structure ||
        action.expected_source_payload_digest.is_zero() ||
        action.expected_product_digest.is_zero()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "biological proxy commit requires exact source, structure, and preview receipts"));
    }
    const auto source = biological_authoring_source(
        action.source_key, action.expected_project_revision);
    if (!source) return failure(source.error());
    if (source.value().source.revision != action.expected_source_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological proxy request does not match the current source revision"));
    }
    const std::string source_snapshot = source.value().serialize();
    const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(source_snapshot.data());
    const assets::Sha256Digest source_snapshot_digest = assets::sha256(
        std::span<const std::uint8_t>(source_bytes, source_snapshot.size()));
    if (source_snapshot_digest != action.expected_source_payload_digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological proxy preview payload differs from the saved source snapshot"));
    }
    auto product = biological::compile_ellipsoid_proxy(source.value(), action.structure);
    if (!product) return failure(product.error().with_context(
        "biological proxy scene materialization"));
    if (product.value().product_digest != action.expected_product_digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological proxy product differs from the accepted preview"));
    }
    project::BiologicalProxyInstanceReceipt source_receipt{
        {}, 0U, source.value().source.source_namespace,
        source.value().source.asset_id, source.value().source.revision,
        action.structure.value, source_snapshot, source_snapshot_digest,
        product.value().product_digest};
    const std::string object_name = "Biological proxy " +
        std::to_string(source.value().source.asset_id) + " structure " +
        std::to_string(action.structure.value);
    const std::string operation = "Create Biological Proxy · " +
        std::to_string(source.value().source.asset_id) + " · structure " +
        std::to_string(action.structure.value) + " · r" +
        std::to_string(source.value().source.revision.value());
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission, document_, object_name, std::move(product.value().mesh),
            core::Transform::identity(), std::nullopt, std::nullopt, std::nullopt,
            std::move(source_receipt)),
        operation);
}

core::Result<DispatchReceipt> ApplicationSession::undo() {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.undo(); !result) {
        return failure(result.error().with_context("undo"));
    }
    if (auto result = append_mutation_event("Undo", before, document_.revision()); !result) {
        const auto rollback = history_.redo();
        document_ = std::move(before_document);
        viewport_mesh_cache_.clear();
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and undo rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("undo rolled back"));
    }
    return accepted("Undo", before);
}

core::Result<DispatchReceipt> ApplicationSession::redo() {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.redo(); !result) {
        return failure(result.error().with_context("redo"));
    }
    if (auto result = append_mutation_event("Redo", before, document_.revision()); !result) {
        const auto rollback = history_.undo();
        document_ = std::move(before_document);
        viewport_mesh_cache_.clear();
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and redo rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("redo rolled back"));
    }
    return accepted("Redo", before);
}

core::Result<DispatchReceipt> ApplicationSession::accepted(
    std::string action,
    core::Revision revision_before,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const bool supported_native_trace_action = active_source_ == OperationSource::human &&
        (action == "Invoke Tool: mesh.inset-face" ||
         action == "Invoke Tool: mesh.extrude-face" ||
         action == "Invoke Tool: mesh.poke-face" ||
         action == "Invoke Tool: mesh.set-vertex-position" ||
         action == "Set Object Transform");
    if (revision_before != document_.revision() && !supported_native_trace_action) {
        native_operation_trace_.clear();
    }
    const OperationId operation_id = next_operation_id_;
    next_operation_id_ = next_operation_id_ == std::numeric_limits<OperationId>::max()
        ? OperationId{1U}
        : next_operation_id_ + 1U;
    return core::Result<DispatchReceipt>::success(DispatchReceipt{
        std::move(action), revision_before, document_.revision(),
        revision_before != document_.revision(), operation_id, active_source_,
        active_provenance_id_,
        std::move(affected), std::move(parameters), {}});
}

void ApplicationSession::append_native_operation_trace(NativeOperationTraceEntry entry) {
    constexpr std::size_t kMaxNativeTraceEntries = 256U;
    if (entry.source != OperationSource::human || entry.operation_id == 0U ||
        !entry.target_object || entry.revision_after <= entry.revision_before) {
        native_operation_trace_.clear();
        return;
    }
    if (!native_operation_trace_.empty() &&
        native_operation_trace_.back().revision_after != entry.revision_before) {
        native_operation_trace_.clear();
    }
    if (native_operation_trace_.size() >= kMaxNativeTraceEntries) {
        native_operation_trace_.erase(native_operation_trace_.begin());
    }
    native_operation_trace_.push_back(std::move(entry));
}

core::Result<DispatchReceipt> ApplicationSession::failure(core::Diagnostic diagnostic) {
    if (problems_.size() >= kMaxProblems) {
        problems_.erase(problems_.begin());
    }
    problems_.push_back(diagnostic);
    return core::Result<DispatchReceipt>::failure(std::move(diagnostic));
}

void ApplicationSession::reset_editor_state() noexcept {
    selection_.clear();
    history_.clear_history();
    problems_.clear();
    last_tool_action_.reset();
    last_tool_context_.reset();
    native_operation_trace_.clear();
}

ApplicationSnapshot ApplicationSession::snapshot() const {
    ApplicationSnapshot result;
    result.project_name = document_.name();
    result.project_path = project_path_;
    result.project_generation = project_generation_;
    result.project_revision = document_.revision();
    result.dirty = document_.revision() != saved_revision_;
    result.presentation_catalog = document_.presentation_catalog();
    result.authoring_standards = document_.authoring_standards();
    result.workspace = workspace_;
    result.tools = tools_.descriptors();
    result.undo_count = history_.undo_count();
    result.redo_count = history_.redo_count();
    result.problems = problems_;
    result.native_operation_trace = native_operation_trace_;
    const auto append_problem = [&result](core::Diagnostic diagnostic) {
        if (result.problems.size() >= kMaxProblems) {
            result.problems.erase(result.problems.begin());
        }
        result.problems.push_back(std::move(diagnostic));
    };

    result.spatial_fields.reserve(document_.spatial_fields().size());
    for (const auto& [identity, field] : document_.spatial_fields()) {
        const auto digest = field.canonical_digest();
        if (!digest) {
            append_problem(digest.error().with_context("spatial field snapshot"));
            continue;
        }
        result.spatial_fields.push_back(SpatialFieldView{
            identity, field.semantic(), field.source_revision(),
            field.width(), field.height(), digest.value()});
    }
    result.curve_paths.reserve(document_.curve_paths().size());
    for (const auto& [identity, path] : document_.curve_paths()) {
        const std::string encoded = path.serialize();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
        const auto digest = assets::sha256(std::span<const std::uint8_t>(bytes, encoded.size()));
        result.curve_paths.push_back(CurvePathSourceView{
            identity, path.source_revision, path.segments.size(), digest});
    }
    result.road_sources.reserve(document_.road_sources().size());
    for (const auto& [identity, road] : document_.road_sources()) {
        result.road_sources.push_back(RoadSourceView{
            identity, road.source_revision, road.centerline_identity,
            road.centerline_revision, road.width_meters, road.shoulder_meters,
            road.maximum_grade_ratio, road.minimum_curve_radius_meters,
            road.surface_class, road.function_class, road.access_state,
            road.drainage_intent, road.source_digest()});
    }
    result.sweep_sources.reserve(document_.sweep_sources().size());
    for (const auto& [identity, source] : document_.sweep_sources()) {
        static_cast<void>(identity);
        const std::string encoded = source.serialize();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
        result.sweep_sources.push_back(SweepSourceView{
            source, assets::sha256(std::span<const std::uint8_t>(bytes, encoded.size()))});
    }
    result.biological_sources.reserve(document_.biological_authoring_sources().size());
    for (const auto& [key, source] : document_.biological_authoring_sources()) {
        result.biological_sources.push_back(BiologicalAuthoringSourceView{
            key, source.digest});
    }
    result.sign_text_sources.reserve(document_.sign_text_sources().size());
    for (const auto& [identity, source] : document_.sign_text_sources()) {
        static_cast<void>(identity);
        const std::string encoded = source.serialize();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
        result.sign_text_sources.push_back(SignTextSourceView{
            source, assets::sha256(std::span<const std::uint8_t>(bytes, encoded.size()))});
    }
    result.source_assets.reserve(document_.source_asset_registry().records().size());
    for (const auto& [identity, record] : document_.source_asset_registry().records()) {
        result.source_assets.push_back(SourceAssetView{
            identity, record.relative_source_path, record.kind, record.source_generation,
            record.source_digest, record.importer_identity, record.importer_version,
            record.artifact_identity, record.artifact_digest});
    }
    result.source_asset_tombstone_count =
        document_.source_asset_registry().lineage().size() -
        document_.source_asset_registry().records().size();
    result.texture_sources.reserve(document_.material_catalog().textures().size());
    for (const auto& [identity, source] : document_.material_catalog().textures()) {
        static_cast<void>(identity);
        result.texture_sources.push_back(source);
    }
    result.materials.reserve(document_.material_catalog().materials().size());
    for (const auto& [identity, material] : document_.material_catalog().materials()) {
        static_cast<void>(identity);
        result.materials.push_back(material);
    }
    result.material_graphs.reserve(
        document_.material_catalog().material_graphs().size());
    for (const auto& [identity, graph] : document_.material_catalog().material_graphs()) {
        result.material_graphs.push_back(MaterialGraphView{
            identity, graph.revision, graph.nodes.size()});
    }
    result.material_graph_bindings.reserve(
        document_.material_catalog().material_graph_bindings().size());
    for (const auto& [material, graph_identity] :
            document_.material_catalog().material_graph_bindings()) {
        const auto graph = document_.material_catalog().material_graphs().find(
            graph_identity.graph_identity);
        bool source_current = false;
        if (graph != document_.material_catalog().material_graphs().end()) {
            const auto digest = graph->second.canonical_digest();
            source_current = digest && graph->second.revision == graph_identity.source_revision &&
                digest.value() == graph_identity.source_digest;
        }
        result.material_graph_bindings.push_back(MaterialGraphBindingView{
            material, graph_identity.graph_identity, graph_identity.source_revision,
            graph_identity.source_digest, source_current});
    }
    result.decal_placements.reserve(document_.decal_placements().size());
    for (const auto& [identity, stored] : document_.decal_placements()) {
        const auto placement = production::load_decal_placement(document_, identity);
        if (!placement) {
            append_problem(placement.error().with_context("decal placement snapshot"));
            continue;
        }
        result.decal_placements.push_back(DecalPlacementView{
            placement.value(), stored.digest});
    }
    result.material_assignments.reserve(
        document_.material_catalog().assignments().size());
    for (const auto& [key, assignment] : document_.material_catalog().assignments()) {
        static_cast<void>(key);
        result.material_assignments.push_back(assignment);
    }

    if (!document_.engineering_authoring().has_value()) {
        result.engineering.status = "No engineering authoring is bound to this project.";
    } else {
        result.engineering.present = true;
        result.engineering.digest = document_.engineering_authoring()->digest;
        const auto decoded = engineering::AuthoringDocument::deserialize(
            document_.engineering_authoring()->bytes);
        if (!decoded) {
            result.engineering.status = "Engineering authoring is invalid; publication is blocked.";
            append_problem(decoded.error().with_context("engineering authoring snapshot"));
        } else {
            const auto& authoring = decoded.value();
            result.engineering.project_namespace = authoring.project_namespace;
            result.engineering.authored_revision = authoring.project_revision;
            result.engineering.source_count = authoring.sources.size();
            result.engineering.sources.reserve(authoring.sources.size());
            for (const auto& source : authoring.sources) {
                result.engineering.sources.push_back(EngineeringAuthoringView::Source{
                    source.id,
                    engineering::source_kind_name(source.kind),
                    source.locator,
                    source.revision,
                    source.digest});
            }
            result.engineering.part_count = authoring.parts.size();
            result.engineering.occurrence_count = authoring.occurrences.size();
            result.engineering.topology_count = authoring.topology.size();
            result.engineering.topology.reserve(authoring.topology.size());
            for (const auto& binding : authoring.topology) {
                result.engineering.topology.push_back(EngineeringAuthoringView::Topology{
                    binding.id,
                    binding.source_id,
                    binding.part_id,
                    engineering::topology_kind_name(binding.kind),
                    binding.external_id,
                    binding.source_revision});
            }
            result.engineering.bom_count = authoring.bom.size();
            result.engineering.bom.reserve(authoring.bom.size());
            for (const auto& line : authoring.bom) {
                EngineeringAuthoringView::BomLine projected{
                    line.id,
                    line.source_id,
                    line.part_number,
                    line.revision,
                    line.description,
                    line.material,
                    line.quantity,
                    line.unit,
                    line.mapped_part_id,
                    {},
                    {},
                    std::nullopt,
                    engineering::reconciliation_status_name(line.reconciliation)};
                if (line.mapped_part_id.has_value()) {
                    const auto part = std::find_if(authoring.parts.begin(), authoring.parts.end(),
                        [&line](const auto& candidate) {
                            return candidate.id == *line.mapped_part_id;
                        });
                    if (part != authoring.parts.end()) {
                        projected.mapped_revision = part->revision;
                        projected.mapped_material = part->material;
                        projected.assembly_quantity =
                            engineering::assembly_quantity_for_part(authoring, part->id);
                    }
                }
                result.engineering.bom.push_back(std::move(projected));
            }
            std::sort(result.engineering.bom.begin(), result.engineering.bom.end(),
                [](const auto& left, const auto& right) { return left.id < right.id; });
            result.engineering.requirement_count = authoring.requirements.size();
            result.engineering.requirements.reserve(authoring.requirements.size());
            for (const auto& requirement : authoring.requirements) {
                result.engineering.requirements.push_back(EngineeringAuthoringView::Requirement{
                    requirement.id,
                    requirement.provenance.source_id,
                    requirement.topology_ids,
                    requirement.kind,
                    engineering::requirement_kind_name(requirement.kind),
                    engineering::extraction_state_name(requirement.extraction),
                    requirement.nominal,
                    requirement.plus,
                    requirement.minus,
                    requirement.unit,
                    requirement.provenance.callout,
                    requirement.provenance.original_text,
                    requirement.acceptance_rule,
                    requirement.enabled,
                    requirement.provenance.page,
                    requirement.provenance.section,
                    requirement.human_review_justification,
                    requirement.unsupported_content});
            }
            result.engineering.characteristic_count = authoring.characteristics.size();
            result.engineering.characteristics.reserve(authoring.characteristics.size());
            for (const auto& characteristic : authoring.characteristics) {
                result.engineering.characteristics.push_back(
                    EngineeringAuthoringView::Characteristic{
                        characteristic.id,
                        characteristic.requirement_id,
                        characteristic.topology_ids,
                        engineering::inspection_kind_name(characteristic.kind),
                        engineering::inspection_status_name(characteristic.status),
                        characteristic.sensor.sensor_type,
                        characteristic.sensor.tool_center_frame,
                        characteristic.coverage.kind,
                        characteristic.coverage.percentage,
                        characteristic.priority,
                        characteristic.sensor.min_standoff,
                        characteristic.sensor.preferred_standoff,
                        characteristic.sensor.max_standoff,
                        characteristic.sensor.contact_required});
            }
            std::sort(result.engineering.characteristics.begin(),
                result.engineering.characteristics.end(),
                [](const auto& left, const auto& right) { return left.id < right.id; });
            result.engineering.target_count = authoring.targets.size();
            result.engineering.targets.reserve(authoring.targets.size());
            for (const auto& target : authoring.targets) {
                result.engineering.targets.push_back(EngineeringAuthoringView::Target{
                    target.id,
                    target.characteristic_id,
                    target.source_topology_id,
                    engineering::target_status_name(target.status),
                    target.generation_basis,
                    target.standoff,
                    target.human_authored,
                    target.sensor_pose.translation});
            }
            std::sort(result.engineering.targets.begin(), result.engineering.targets.end(),
                [](const auto& left, const auto& right) { return left.id < right.id; });
            result.engineering.derived_product_count = authoring.derived_products.size();
            result.engineering.stale =
                std::any_of(authoring.requirements.begin(), authoring.requirements.end(),
                    [](const engineering::EngineeringRequirement& value) {
                        return value.extraction == engineering::ExtractionState::stale;
                    }) ||
                std::any_of(authoring.characteristics.begin(), authoring.characteristics.end(),
                    [](const engineering::InspectionCharacteristic& value) {
                        return value.status == engineering::InspectionStatus::stale;
                    }) ||
                std::any_of(authoring.targets.begin(), authoring.targets.end(),
                    [](const engineering::InspectionTarget& value) {
                        return value.status == engineering::TargetStatus::stale;
                    }) ||
                (authoring.path.has_value() &&
                 authoring.path->status == engineering::DerivedProductStatus::stale) ||
                std::any_of(authoring.derived_products.begin(), authoring.derived_products.end(),
                    [](const engineering::DerivedProductState& value) {
                        return value.status == engineering::DerivedProductStatus::stale;
                    });
            result.engineering.workcell_present = authoring.workcell.has_value();
            if (authoring.workcell.has_value()) {
                EngineeringAuthoringView::Workcell workcell_view;
                const auto& workcell = *authoring.workcell;
                workcell_view.id = workcell.id;
                workcell_view.revision = workcell.revision;
                workcell_view.robot_model_identity = workcell.robot_model_identity;
                workcell_view.robot_source_identity = workcell.robot_source_identity;
                workcell_view.active_tool_frame = workcell.active_tool_frame;
                workcell_view.capability_profile = workcell.capability_profile;
                workcell_view.sensor_identity = workcell.sensor_identity;
                workcell_view.revision_history = authoring.workcell_revision_history;
                for (const auto& entity : workcell.entities) {
                    workcell_view.entities.push_back(EngineeringAuthoringView::WorkcellEntity{
                        entity.id, engineering::workcell_entity_kind_name(entity.kind),
                        entity.name, entity.source, entity.transform, entity.enabled, false});
                }
                for (const auto& frame : workcell.frames) {
                    workcell_view.entities.push_back(EngineeringAuthoringView::WorkcellEntity{
                        frame.id, engineering::workcell_entity_kind_name(frame.kind),
                        frame.name, frame.source, frame.transform, frame.enabled, true});
                }
                for (const auto& keep_out : workcell.keep_outs) {
                    workcell_view.keep_outs.push_back(EngineeringAuthoringView::WorkcellKeepOut{
                        keep_out.id, engineering::keep_out_kind_name(keep_out.kind),
                        keep_out.purpose, keep_out.source, keep_out.transform,
                        keep_out.dimensions, keep_out.enabled});
                }
                result.engineering.workcell = std::move(workcell_view);
            }
            result.engineering.path_present = authoring.path.has_value();
            if (authoring.path.has_value()) {
                EngineeringAuthoringView::Path path_view;
                const auto& path = *authoring.path;
                path_view.id = path.id;
                path_view.basis_digest = path.basis_digest;
                path_view.basis_revision = path.basis_revision;
                path_view.status = engineering::derived_product_status_name(path.status);
                path_view.vanta_receipt = path.vanta_receipt;
                path_view.samples.reserve(path.samples.size());
                for (const auto& sample : path.samples) {
                    path_view.samples.push_back(EngineeringAuthoringView::PathSample{
                        sample.sequence, sample.normalized, sample.tool_pose, sample.target_id,
                        engineering::path_sample_status_name(sample.status), sample.clearance});
                }
                result.engineering.path = std::move(path_view);
            }
            const auto package = engineering::InspectionPackage::publish(
                authoring, document_.revision(), authoring.project_namespace);
            if (!package) {
                result.engineering.status = "Authoring is present; publication is blocked: " +
                    package.error().message;
            } else if (result.engineering.stale) {
                result.engineering.status =
                    "Engineering source drift is recorded; dependent evidence is stale and publication is blocked.";
            } else {
                result.engineering.publication_ready = true;
                const auto admission = engineering::admit_for_consumer(package.value());
                if (admission && admission.value().status ==
                        engineering::PackageAdmissionStatus::admit) {
                    result.engineering.consumer_admissible = true;
                    result.engineering.status =
                        "Package passes local VANTA handoff-contract checks; no VANTA runtime admission has been received.";
                } else {
                    result.engineering.status =
                        "Local VANTA handoff-contract checks refused this package.";
                }
            }
        }
    }

    if (!document_.condition_authoring().has_value()) {
        result.condition.status =
            "No CONDITION source-authoring document is bound to this project.";
    } else {
        result.condition.present = true;
        result.condition.digest = document_.condition_authoring()->digest;
        const auto decoded = condition::AuthoringDocument::deserialize(
            document_.condition_authoring()->bytes);
        if (!decoded) {
            result.condition.status =
                "CONDITION source authoring is invalid; publication is blocked.";
            append_problem(decoded.error().with_context("CONDITION authoring snapshot"));
        } else {
            const auto& authoring = decoded.value();
            result.condition.project_namespace = authoring.project_namespace;
            result.condition.authored_revision = authoring.project_revision;
            result.condition.source_count = authoring.sources.size();
            result.condition.sources.reserve(authoring.sources.size());
            for (const auto& source : authoring.sources) {
                result.condition.sources.push_back(ConditionAuthoringView::Source{
                    source.id,
                    std::string(condition::source_kind_name(source.kind)),
                    source.label,
                    source.logical_locator,
                    source.revision,
                    source.digest,
                    source.current,
                    source.reviewed,
                    source.diagnostics});
            }
            result.condition.asset_count = authoring.assets.size();
            result.condition.assets.reserve(authoring.assets.size());
            for (const auto& asset : authoring.assets) {
                result.condition.assets.push_back(ConditionAuthoringView::Asset{
                    asset.id, asset.stable_name,
                    std::string(condition::asset_class_name(asset.asset_class)),
                    asset.authored_object_id});
            }
            result.condition.interaction_anchors.reserve(authoring.interaction_anchors.size());
            for (const auto& anchor : authoring.interaction_anchors) {
                result.condition.interaction_anchors.push_back(
                    ConditionAuthoringView::InteractionAnchor{
                        anchor.id, anchor.asset_id,
                        std::string(condition::anchor_kind_name(anchor.kind)),
                        anchor.interaction_class, anchor.transform,
                        anchor.approach_direction, anchor.clearance, anchor.enabled});
            }
            result.condition.collections.reserve(authoring.collections.size());
            for (const auto& collection : authoring.collections) {
                result.condition.collections.push_back(ConditionAuthoringView::Collection{
                    collection.id, collection.name, collection.asset_ids,
                    collection.child_collection_ids, collection.semantic_class});
            }
            result.condition.prefabs.reserve(authoring.prefabs.size());
            for (const auto& prefab : authoring.prefabs) {
                result.condition.prefabs.push_back(ConditionAuthoringView::Prefab{
                    prefab.id, prefab.name, prefab.asset_ids, prefab.sockets,
                    prefab.variant_ids, prefab.source_revision});
            }
            result.condition.prefab_instance_count = authoring.prefab_instances.size();
            result.condition.prefab_instances.reserve(authoring.prefab_instances.size());
            for (const auto& instance : authoring.prefab_instances) {
                const bool current = condition_prefab_instance_bindings_current(
                    instance, authoring, document_);
                result.condition.prefab_instances.push_back(
                    ConditionAuthoringView::PrefabInstance{
                        instance.id, instance.prefab_id, instance.name,
                        instance.root_object_id, instance.members.size(), current,
                        current ? "Current · source and mesh revisions match."
                                : "Stale · prefab, source, or scene binding changed."});
            }
            result.condition.region_kits.reserve(authoring.region_kits.size());
            for (const auto& kit : authoring.region_kits) {
                std::vector<std::string> required_classes;
                required_classes.reserve(kit.required_classes.size());
                for (const auto asset_class : kit.required_classes) {
                    required_classes.emplace_back(
                        condition::asset_class_name(asset_class));
                }
                result.condition.region_kits.push_back(ConditionAuthoringView::RegionKit{
                    kit.id, kit.name, kit.asset_ids, kit.prefab_ids,
                    std::move(required_classes), kit.interaction_coverage,
                    kit.familiarity_coverage, kit.collision_validation_required,
                    kit.semantic_validation_required});
            }
            result.condition.variants.reserve(authoring.variants.size());
            for (const auto& variant : authoring.variants) {
                result.condition.variants.push_back(ConditionAuthoringView::Variant{
                    variant.id, variant.asset_id, variant.name,
                    std::string(condition::variant_kind_name(variant.kind)),
                    variant.semantic_id});
            }
            result.condition.safe_place_count = authoring.safe_places.size();
            result.condition.safe_places.reserve(authoring.safe_places.size());
            for (const auto& profile : authoring.safe_places) {
                result.condition.safe_places.push_back(ConditionAuthoringView::SafePlace{
                    profile.id, profile.asset_id,
                    {profile.privacy, profile.human_proximity,
                     profile.controllable_entries, profile.supply_access,
                     profile.ambient_noise, profile.visibility_outward,
                     profile.visibility_inward, profile.isolation}});
            }
            result.condition.surface_states.reserve(authoring.surface_states.size());
            for (const auto& surface : authoring.surface_states) {
                result.condition.surface_states.push_back(
                    ConditionAuthoringView::SurfaceState{
                        surface.id, surface.asset_id,
                        std::string(condition::occupancy_state_name(surface.occupancy)),
                        std::string(condition::maintenance_state_name(surface.maintenance)),
                        std::string(condition::damage_state_name(surface.damage)),
                        surface.wetness});
            }
            result.condition.state_branches.reserve(authoring.state_branches.size());
            result.condition.state_branch_count = authoring.state_branches.size();
            for (const auto& branch : authoring.state_branches) {
                result.condition.state_branches.push_back(
                    ConditionAuthoringView::StateBranch{
                        branch.id, branch.parent_branch_id, branch.name,
                        branch.applicability, branch.deltas.size()});
            }
            result.condition.familiarity_count = authoring.familiarity.size();
            result.condition.view_condition_count = authoring.view_conditions.size();
            result.condition.variant_count = authoring.variants.size();
            result.condition.temporal_package_count = authoring.temporal_packages.size();
            result.condition.surface_state_count = authoring.surface_states.size();
            result.condition.interaction_anchor_count = authoring.interaction_anchors.size();
            result.condition.collection_count = authoring.collections.size();
            result.condition.prefab_count = authoring.prefabs.size();
            result.condition.region_kit_count = authoring.region_kits.size();
            result.condition.kinematic_assembly_count = authoring.kinematic_assemblies.size();
            result.condition.audio_source_count = authoring.audio_sources.size();
            result.condition.acoustic_zone_count = authoring.acoustic_zones.size();
            result.condition.effect_source_count = authoring.effect_sources.size();
            result.condition.particle_emitter_count = authoring.particle_emitters.size();
            result.condition.field_influence_count = authoring.field_influences.size();
            result.condition.vegetation_scatter_count = authoring.vegetation_scatters.size();
            result.condition.vegetation_scatter_ids.reserve(authoring.vegetation_scatters.size());
            for (const auto& recipe : authoring.vegetation_scatters) {
                result.condition.vegetation_scatter_ids.push_back(recipe.id);
            }
            result.condition.audio_sources.reserve(authoring.audio_sources.size());
            for (const auto& source : authoring.audio_sources) {
                result.condition.audio_sources.push_back(ConditionAuthoringView::AudioSource{
                    source.id, source.asset_id, source.clip_source_id, source.local_transform,
                    std::string(condition::audio_bus_class_name(source.bus)), source.gain,
                    source.minimum_distance, source.maximum_distance,
                    source.spatial, source.enabled});
            }
            result.condition.acoustic_zones.reserve(authoring.acoustic_zones.size());
            for (const auto& zone : authoring.acoustic_zones) {
                result.condition.acoustic_zones.push_back(ConditionAuthoringView::AcousticZone{
                    zone.id, zone.asset_id, zone.local_transform,
                    std::string(condition::acoustic_zone_shape_name(zone.shape)),
                    zone.half_extents, zone.radius, zone.acoustic_class,
                    zone.semantic_region, zone.priority, zone.enabled});
            }
            result.condition.effect_sources.reserve(authoring.effect_sources.size());
            for (const auto& effect : authoring.effect_sources) {
                result.condition.effect_sources.push_back(ConditionAuthoringView::EffectSource{
                    effect.id, effect.asset_id, effect.semantic_owner,
                    effect.normal_operating_state, effect.disabled_operating_state,
                    effect.enabled});
            }
            result.condition.kinematic_assemblies.reserve(
                authoring.kinematic_assemblies.size());
            for (const auto& assembly : authoring.kinematic_assemblies) {
                const auto graph = kinematic::KinematicGraph::deserialize(
                    assembly.graph_bytes);
                if (!graph) {
                    append_problem(graph.error().with_context(
                        "CONDITION kinematic assembly snapshot"));
                    continue;
                }
                const bool bindings_current = condition_kinematic_bindings_current(
                    assembly, document_);
                std::vector<std::uint64_t> hinge_joint_ids;
                for (const auto& [joint_id, node] : graph.value().nodes()) {
                    if (node.kind == kinematic::JointKind::hinge) {
                        hinge_joint_ids.push_back(joint_id.value);
                    }
                }
                result.condition.kinematic_assemblies.push_back(
                    ConditionAuthoringView::KinematicAssembly{
                        assembly.id, assembly.asset_id, assembly.source_identity,
                        assembly.source_revision, graph.value().nodes().size(),
                        assembly.part_bindings.size(), bindings_current,
                        std::move(hinge_joint_ids)});
            }
            const bool kinematic_bindings_current = std::all_of(
                authoring.kinematic_assemblies.begin(),
                authoring.kinematic_assemblies.end(), [this](const auto& assembly) {
                    return condition_kinematic_bindings_current(assembly, document_);
                });
            const bool prefab_instances_current = std::all_of(
                authoring.prefab_instances.begin(), authoring.prefab_instances.end(),
                [this, &authoring](const auto& instance) {
                    return condition_prefab_instance_bindings_current(
                        instance, authoring, document_);
                });
            result.condition.stale = authoring.has_stale_sources() ||
                !kinematic_bindings_current || !prefab_instances_current;
            const auto package = kinematic_bindings_current && prefab_instances_current
                ? condition::Package::publish(
                    authoring, document_.revision(), authoring.project_namespace)
                : core::Result<condition::Package>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    !kinematic_bindings_current
                        ? "CONDITION publication is blocked by missing or stale articulated mesh bindings"
                        : "CONDITION publication is blocked by stale prefab instance source or scene bindings"));
            if (!package) {
                result.condition.status = "Source authoring is present; publication is blocked: " +
                    package.error().message;
            } else {
                result.condition.publication_ready = true;
                const auto admission = condition::admit_for_consumer(package.value());
                if (admission && admission.value().status ==
                        condition::PackageAdmissionStatus::admit) {
                    result.condition.consumer_admissible = true;
                    result.condition.status =
                        "Source package is current and admitted at the public consumer boundary.";
                } else {
                    result.condition.status =
                        "Source package is current; consumer admission is refused.";
                }
            }
        }
    }

    const auto scene_objects = document_.scene().objects_sorted();
    result.objects.reserve(scene_objects.size());
    for (const auto& object : scene_objects) {
        auto world = document_.scene().world_transform(object.id);
        if (!world) {
            append_problem(world.error().with_context("scene snapshot"));
            result.viewport.error = world.error().with_context("viewport transform");
            continue;
        }
        std::optional<core::Revision> mesh_revision;
        if (object.mesh_asset.has_value()) {
            const auto mesh = document_.meshes().find(*object.mesh_asset);
            if (mesh != document_.meshes().end()) mesh_revision = mesh->second.revision();
        }
        result.objects.push_back(ObjectView{object, world.value(), mesh_revision});
    }

    result.selection.mode = selection_.mode();
    result.selection.objects = selection_.selected_objects();
    result.selection.component_object = selection_.component_object();
    result.selection.component_mesh_revision = selection_.component_mesh_revision();
    result.selection.component_groups = selection_.component_groups();
    if (result.selection.component_object.has_value()) {
        result.selection.vertices = selection_.selected_vertices();
        result.selection.edges = selection_.selected_edges();
        result.selection.faces = selection_.selected_faces();
    }
    if (result.selection.component_object.has_value()) {
        const scene::SceneObject* selected_object = document_.scene().find(
            *result.selection.component_object);
        if (selected_object != nullptr && selected_object->mesh_asset.has_value()) {
            result.sculpt_mesh_asset = *selected_object->mesh_asset;
            const auto stored_stack = document_.sculpt_stacks().find(*result.sculpt_mesh_asset);
            if (stored_stack != document_.sculpt_stacks().end()) {
                result.sculpt_stack_digest = stored_stack->second.digest;
                auto stack = production::SculptStack::deserialize(stored_stack->second.bytes);
                if (!stack) {
                    append_problem(stack.error().with_context("SculptStack snapshot"));
                } else {
                    result.sculpt_layers.reserve(stack.value().layers.size());
                    for (const production::SculptLayer& layer : stack.value().layers) {
                        result.sculpt_layers.push_back(SculptLayerView{
                            layer.identity,
                            layer.name,
                            layer.visible,
                            layer.weight,
                            layer.strokes.size()});
                    }
                }
            }
        }
    }
    result.viewport.project_revision = document_.revision();

    const auto wants_attribute_overlay = [&result](scene::ObjectId object) {
        return result.selection.component_object == object ||
            (result.selection.mode == editor::SelectionMode::object &&
             result.selection.objects.size() == 1U &&
             result.selection.objects.front() == object);
    };
    render::RenderScene viewport_scene;
    for (auto cache = viewport_mesh_cache_.begin(); cache != viewport_mesh_cache_.end();) {
        if (!document_.meshes().contains(cache->first)) {
            cache = viewport_mesh_cache_.erase(cache);
        } else {
            ++cache;
        }
    }
    for (const auto& object_view : result.objects) {
        const auto& object = object_view.object;
        if (!object.visible || !object.mesh_asset.has_value()) {
            continue;
        }
        const auto mesh = document_.meshes().find(*object.mesh_asset);
        if (mesh == document_.meshes().end()) {
            result.viewport.error = core::Diagnostic(
                core::ErrorCode::stale_data,
                "scene object references a missing mesh asset");
            break;
        }
        const auto source_revision = mesh->second.revision();
        const auto stored_stack = document_.sculpt_stacks().find(*object.mesh_asset);
        const std::optional<assets::Sha256Digest> sculpt_stack_digest =
            stored_stack == document_.sculpt_stacks().end()
                ? std::nullopt
                : std::optional<assets::Sha256Digest>{stored_stack->second.digest};
        const auto cached = viewport_mesh_cache_.find(*object.mesh_asset);
        std::shared_ptr<const geometry::CompiledMesh> compiled_mesh;
        if (cached != viewport_mesh_cache_.end() &&
            cached->second.project_generation == project_generation_ &&
            cached->second.source == &mesh->second &&
            cached->second.source_revision == source_revision &&
            cached->second.sculpt_stack_digest == sculpt_stack_digest &&
            cached->second.mesh != nullptr) {
            compiled_mesh = cached->second.mesh;
        } else {
            geometry::EditableMesh viewport_mesh = mesh->second;
            if (stored_stack != document_.sculpt_stacks().end()) {
                auto stack = production::SculptStack::deserialize(stored_stack->second.bytes);
                if (!stack) {
                    result.viewport.error = stack.error().with_context(
                        "SculptStack viewport source");
                    break;
                }
                const auto digest = stack.value().digest();
                if (!digest || digest.value() != stored_stack->second.digest) {
                    result.viewport.error = core::Diagnostic(
                        core::ErrorCode::validation_failed,
                        "SculptStack viewport bytes do not match their recorded digest");
                    break;
                }

            }
            auto compiled = viewport_mesh.compile();
            if (!compiled) {
                result.viewport.error = compiled.error().with_context("viewport compilation");
                break;
            }
            auto attribute_overlay = build_viewport_attribute_overlay(
                viewport_mesh, compiled.value());
            std::shared_ptr<const ViewportState::MeshAttributeOverlayData> overlay_data;
            std::optional<core::Diagnostic> overlay_error;
            if (attribute_overlay) {
                overlay_data = std::move(attribute_overlay.value());
            } else {
                overlay_error = attribute_overlay.error().with_context(
                    "viewport authored mesh attributes");
            }
            auto owned_mesh = std::make_shared<geometry::CompiledMesh>(std::move(compiled.value()));
            compiled_mesh = owned_mesh;
            viewport_mesh_cache_[*object.mesh_asset] = ViewportMeshCacheEntry{
                project_generation_, &mesh->second, source_revision,
                sculpt_stack_digest, std::move(owned_mesh), std::move(overlay_data),
                std::move(overlay_error)};
        }
        if (auto inserted = viewport_scene.upsert({
                object.id, compiled_mesh, object_view.world_transform,
                compiled_mesh->source_revision, object.mesh_asset});
            !inserted) {
            result.viewport.error = inserted.error().with_context("viewport snapshot");
            break;
        }
        if (wants_attribute_overlay(object.id)) {
            const auto cached_overlay = viewport_mesh_cache_.find(*object.mesh_asset);
            if (cached_overlay != viewport_mesh_cache_.end()) {
                if (cached_overlay->second.attribute_overlay_error.has_value()) {
                    append_problem(*cached_overlay->second.attribute_overlay_error);
                } else if (cached_overlay->second.attribute_overlay &&
                    (cached_overlay->second.attribute_overlay->has_sharp_edge_layer ||
                     cached_overlay->second.attribute_overlay->has_custom_normal_layer)) {
                    result.viewport.attribute_overlays.push_back(
                        ViewportState::MeshAttributeOverlay{
                            object.id,
                            *object.mesh_asset,
                            source_revision,
                            project_generation_,
                            cached_overlay->second.attribute_overlay});
                }
            }
        }
    }
    if (!result.viewport.error.has_value()) {
        result.viewport.scene = std::move(viewport_scene);
    }
    return result;
}

core::Result<assets::Sha256Digest> ApplicationSession::native_structure_signature(
    scene::ObjectId object_id,
    std::variant<std::monostate, geometry::FaceId, geometry::VertexId> component) const {
    const auto* object = document_.scene().find(object_id);
    if (object == nullptr || !object->mesh_asset.has_value()) {
        return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "structural correspondence target has no current mesh object"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "structural correspondence target references a missing mesh asset"));
    }
    return structure_signature_for_mesh(mesh->second, component);
}

core::Result<std::vector<NativeStructureMatch>>
ApplicationSession::find_native_structure_matches(
    NativeTraceTargetKind kind,
    const assets::Sha256Digest& signature,
    std::optional<scene::ObjectId> excluded_object) const {
    if (signature.is_zero()) {
        return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "structural correspondence signature is empty"));
    }
    if (kind != NativeTraceTargetKind::object && kind != NativeTraceTargetKind::face &&
        kind != NativeTraceTargetKind::vertex) {
        return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "structural correspondence target kind is unknown"));
    }
    if (document_.scene().size() > kMaxNativeStructureMeshElements) {
        return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "structural correspondence exceeds the bounded scene object limit"));
    }

    std::vector<NativeStructureMatch> matches;
    std::size_t inspected_components = 0U;
    for (const auto& object : document_.scene().objects_sorted()) {
        if ((excluded_object.has_value() && object.id == *excluded_object) ||
            !object.visible || object.locked || !object.mesh_asset.has_value() ||
            (kind == NativeTraceTargetKind::object && object.parent.has_value())) {
            continue;
        }
        const auto mesh = document_.meshes().find(*object.mesh_asset);
        if (mesh == document_.meshes().end()) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "structural correspondence candidate references a missing mesh asset"));
        }
        const std::size_t component_count = kind == NativeTraceTargetKind::face
            ? mesh->second.face_count()
            : (kind == NativeTraceTargetKind::vertex ? mesh->second.vertex_count() : 1U);
        if (component_count > kMaxNativeStructureMeshElements - inspected_components) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "structural correspondence exceeds the bounded candidate scan limit"));
        }
        inspected_components += component_count;
        auto index = build_native_structure_signature_index(
            mesh->second,
            kind == NativeTraceTargetKind::face,
            kind == NativeTraceTargetKind::vertex);
        if (!index) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(index.error());
        }

        if (kind == NativeTraceTargetKind::object) {
            if (index.value().object == signature) {
                matches.push_back({object.id, std::monostate{}});
            }
        } else if (kind == NativeTraceTargetKind::face) {
            for (const auto& [face, candidate_signature] : index.value().faces) {
                if (candidate_signature == signature) {
                    matches.push_back({object.id, face});
                }
            }
        } else {
            for (const auto& [vertex, candidate_signature] : index.value().vertices) {
                if (candidate_signature == signature) {
                    matches.push_back({object.id, vertex});
                }
            }
        }
        if (matches.size() > kMaxNativeStructureMatches) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "structural correspondence exceeds the bounded match result limit"));
        }
    }
    return core::Result<std::vector<NativeStructureMatch>>::success(std::move(matches));
}

core::Result<std::vector<NativeFaceChainResolution>>
ApplicationSession::resolve_native_face_chain(
    scene::ObjectId object_id,
    geometry::FaceId first_face,
    std::span<const NativeFaceChainStep> steps) const {
    using Resolutions = std::vector<NativeFaceChainResolution>;
    const auto fail = [](core::ErrorCode code, std::string message) {
        return core::Result<Resolutions>::failure(core::Diagnostic(code, std::move(message)));
    };
    if (steps.empty() || steps.size() > kMaxNativeFaceChainSteps || !first_face) {
        return fail(core::ErrorCode::invalid_argument,
            "face-topology chain requires a first face and between 1 and 64 steps");
    }
    if (steps.front().reuse_previous_face) {
        return fail(core::ErrorCode::invalid_argument,
            "the first face-topology chain step cannot reuse a previous face");
    }
    const auto* object = document_.scene().find(object_id);
    if (object == nullptr || !object->mesh_asset.has_value()) {
        return fail(core::ErrorCode::not_found,
            "face-topology chain target has no current mesh object");
    }
    const auto source_mesh = document_.meshes().find(*object->mesh_asset);
    if (source_mesh == document_.meshes().end()) {
        return fail(core::ErrorCode::stale_data,
            "face-topology chain target references a missing mesh asset");
    }

    geometry::EditableMesh mesh = source_mesh->second;
    geometry::FaceId face = first_face;
    std::vector<geometry::FaceId> previous_created;
    Resolutions resolutions;
    resolutions.reserve(steps.size());
    for (std::size_t index = 0U; index < steps.size(); ++index) {
        const NativeFaceChainStep& step = steps[index];
        const bool poke = step.tool_id == "mesh.poke-face";
        if (!poke && step.tool_id != "mesh.inset-face" && step.tool_id != "mesh.extrude-face") {
            return fail(core::ErrorCode::unsupported,
                "face-topology chains support only inset, extrude, and poke");
        }
        if (index > 0U && !step.reuse_previous_face) {
            // The step targets a face the previous step created; it must be
            // the unique created face whose local structure was recorded.
            std::optional<geometry::FaceId> match;
            for (const geometry::FaceId created : previous_created) {
                if (mesh.find_face(created) == nullptr) continue;
                const auto signature = structure_signature_for_mesh(mesh, created);
                if (!signature) return core::Result<Resolutions>::failure(signature.error());
                if (signature.value() != step.component_signature) continue;
                if (match.has_value()) {
                    return fail(core::ErrorCode::unsupported,
                        "face-topology chain step matches more than one created face");
                }
                match = created;
            }
            if (!match.has_value()) {
                return fail(core::ErrorCode::not_found,
                    "face-topology chain step has no created face matching its recorded structure");
            }
            face = *match;
        }
        const geometry::Face* target = mesh.find_face(face);
        if (target == nullptr) {
            return fail(core::ErrorCode::stale_data,
                "face-topology chain step targets a face that no longer exists");
        }
        const auto before = structure_signature_for_mesh(mesh, face);
        if (!before) return core::Result<Resolutions>::failure(before.error());
        if (before.value() != step.component_signature) {
            return fail(core::ErrorCode::stale_data,
                "face-topology chain step no longer matches its recorded face structure");
        }

        NativeFaceChainResolution resolution{face, std::nullopt};
        if (poke && step.normalized_distance != 0.0) {
            return fail(core::ErrorCode::invalid_argument,
                "face poke chain steps are parameterless");
        }
        if (!poke) {
            double longest_edge = 0.0;
            for (std::size_t corner = 0U; corner < target->vertices.size(); ++corner) {
                const auto* first = mesh.find_vertex(target->vertices[corner]);
                const auto* second = mesh.find_vertex(
                    target->vertices[(corner + 1U) % target->vertices.size()]);
                if (first == nullptr || second == nullptr) {
                    return fail(core::ErrorCode::stale_data,
                        "face-topology chain face has a missing boundary vertex");
                }
                longest_edge = std::max(longest_edge, (second->position - first->position).length());
            }
            const double distance = step.normalized_distance * longest_edge;
            if (!std::isfinite(step.normalized_distance) || step.normalized_distance <= 0.0 ||
                !std::isfinite(distance) || distance <= 0.0) {
                return fail(core::ErrorCode::invalid_argument,
                    "face-topology chain distance is invalid for the resolved face");
            }
            resolution.distance = distance;
        }
        const auto applied = [&]() {
            if (poke) return mesh.poke_face(face);
            return step.tool_id == "mesh.inset-face"
                ? mesh.inset_face(face, *resolution.distance)
                : mesh.extrude_face_with_receipt(face, *resolution.distance);
        }();
        if (!applied) {
            return core::Result<Resolutions>::failure(applied.error().with_context(
                "face-topology chain replay"));
        }
        const auto after = structure_signature_for_mesh(mesh, std::monostate{});
        if (!after) return core::Result<Resolutions>::failure(after.error());
        if (after.value() != step.result_object_signature) {
            return fail(core::ErrorCode::stale_data,
                "face-topology chain step does not reproduce its recorded result structure");
        }
        previous_created = applied.value().created_faces;
        resolutions.push_back(resolution);
    }
    return core::Result<Resolutions>::success(std::move(resolutions));
}

} // namespace carto::application
