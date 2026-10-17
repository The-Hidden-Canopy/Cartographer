#include <carto/editor/snap_candidates.hpp>
#include <carto/project/project.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace carto::editor {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

core::Result<void> validate_request(
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius) {
    if (!origin.finite()) {
        return core::Result<void>::failure(invalid("snap candidate origin must be finite"));
    }
    if (!std::isfinite(radius) || radius < 0.0) {
        return core::Result<void>::failure(
            invalid("snap candidate radius must be finite and non-negative"));
    }
    if (auto result = settings.validate(); !result) return result;
    if (!settings.enabled) return core::Result<void>::success();
    switch (settings.kind) {
    case SnapKind::vertex:
    case SnapKind::edge_midpoint:
    case SnapKind::face_center:
        return core::Result<void>::success();
    case SnapKind::grid:
    case SnapKind::increment:
        return core::Result<void>::failure(
            unsupported("grid and increment snapping do not select geometry candidates"));
    case SnapKind::edge:
    case SnapKind::face:
    case SnapKind::surface:
    case SnapKind::normal:
        return core::Result<void>::failure(
            unsupported("requested geometry snap target has no bounded candidate contract"));
    }
    return core::Result<void>::failure(invalid("snap kind is invalid"));
}

std::uint64_t candidate_id(const SnapCandidate& candidate) {
    if (candidate.vertex.has_value()) return candidate.vertex->value;
    if (candidate.edge.has_value()) return candidate.edge->value;
    if (candidate.face.has_value()) return candidate.face->value;
    return 0U;
}

core::Vec3d transform_point(const core::Transform& transform, core::Vec3d point) {
    return transform.translation + transform.rotation.rotate(
        core::componentwise_multiply(transform.scale, point));
}

core::Result<void> append_viewport_candidates(
    const geometry::CompiledMesh& mesh,
    core::Vec3d origin,
    double radius,
    SnapKind kind,
    const core::Transform& transform,
    scene::ObjectId object,
    std::uint64_t mesh_asset,
    core::Revision source_revision,
    std::vector<SnapCandidate>& candidates) {
    constexpr std::size_t kMaxViewportSnapCandidates = 65'536U;
    const auto consider = [&](SnapCandidate candidate) -> core::Result<void> {
        candidate.position = transform_point(transform, candidate.position);
        if (!candidate.position.finite()) {
            return core::Result<void>::failure(invalid(
                "compiled viewport snap target has a non-finite world position"));
        }
        // The native work surface is an X/Z plan projection. Elevation is
        // preserved by the active transform axis and does not affect proximity.
        const double distance = std::hypot(
            candidate.position.x - origin.x,
            candidate.position.z - origin.z);
        if (!std::isfinite(distance)) {
            return core::Result<void>::failure(invalid(
                "compiled viewport snap distance is non-finite"));
        }
        if (distance > radius) return core::Result<void>::success();
        if (candidates.size() >= kMaxViewportSnapCandidates) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "compiled viewport snap query exceeds its candidate budget"));
        }
        candidate.object = object;
        candidate.mesh_asset = mesh_asset;
        candidate.distance = distance;
        candidate.source_revision = source_revision;
        candidate.mesh_revision = mesh.source_revision;
        candidates.push_back(std::move(candidate));
        return core::Result<void>::success();
    };

    if (kind == SnapKind::vertex) {
        for (std::size_t index = 0U; index < mesh.positions.size(); ++index) {
            SnapCandidate candidate;
            candidate.kind = kind;
            candidate.vertex = mesh.vertex_ids[index];
            candidate.position = mesh.positions[index];
            if (auto result = consider(std::move(candidate)); !result) return result;
        }
        return core::Result<void>::success();
    }

    if (kind == SnapKind::edge_midpoint) {
        std::set<geometry::EdgeId> seen;
        for (std::size_t triangle = 0U; triangle < mesh.triangle_edges.size(); ++triangle) {
            for (std::size_t edge_index = 0U; edge_index < 3U; ++edge_index) {
                const auto edge = mesh.triangle_edges[triangle][edge_index];
                // Only source-topology edges have stable identities. Skip
                // compiler-created polygon diagonals rather than inventing IDs.
                if (!edge.has_value()) continue;
                if (!seen.insert(*edge).second) continue;
                const auto first = mesh.indices[triangle * 3U + edge_index];
                const auto second = mesh.indices[triangle * 3U + (edge_index + 1U) % 3U];
                SnapCandidate candidate;
                candidate.kind = kind;
                candidate.edge = *edge;
                candidate.position = (mesh.positions[first] + mesh.positions[second]) * 0.5;
                if (auto result = consider(std::move(candidate)); !result) return result;
            }
        }
        return core::Result<void>::success();
    }

    std::map<geometry::FaceId, std::set<std::uint32_t>> face_vertices;
    for (std::size_t triangle = 0U; triangle < mesh.triangle_faces.size(); ++triangle) {
        auto& vertices = face_vertices[mesh.triangle_faces[triangle]];
        vertices.insert(mesh.indices[triangle * 3U]);
        vertices.insert(mesh.indices[triangle * 3U + 1U]);
        vertices.insert(mesh.indices[triangle * 3U + 2U]);
    }
    for (const auto& [face, vertices] : face_vertices) {
        if (vertices.empty()) {
            return core::Result<void>::failure(invalid(
                "compiled face candidate has no vertices"));
        }
        core::Vec3d center{};
        for (const auto index : vertices) center = center + mesh.positions[index];
        center = center * (1.0 / static_cast<double>(vertices.size()));
        SnapCandidate candidate;
        candidate.kind = kind;
        candidate.face = face;
        candidate.position = center;
        if (auto result = consider(std::move(candidate)); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<bool> effectively_visible(
    const scene::Scene& source,
    const scene::SceneObject& object) {
    const scene::SceneObject* current = &object;
    std::size_t visited = 0U;
    while (current != nullptr) {
        if (!current->visible) {
            return core::Result<bool>::success(false);
        }
        if (!current->parent.has_value()) {
            return core::Result<bool>::success(true);
        }
        if (++visited > source.size()) {
            return core::Result<bool>::failure(invalid(
                "scene hierarchy contains a cycle while collecting snap targets"));
        }
        current = source.find(*current->parent);
        if (current == nullptr) {
            return core::Result<bool>::failure(invalid(
                "scene hierarchy contains a missing parent while collecting snap targets"));
        }
    }
    return core::Result<bool>::success(true);
}

} // namespace

core::Result<std::optional<SnapCandidate>> find_snap_candidate(
    const geometry::EditableMesh& mesh,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius) {
    if (auto result = validate_request(origin, settings, radius); !result) {
        return core::Result<std::optional<SnapCandidate>>::failure(result.error());
    }
    if (!settings.enabled) {
        return core::Result<std::optional<SnapCandidate>>::success(std::nullopt);
    }
    const auto compiled = mesh.compile();
    if (!compiled) {
        return core::Result<std::optional<SnapCandidate>>::failure(compiled.error());
    }
    return find_snap_candidate(compiled.value(), origin, settings, radius);
}

namespace {

core::Result<std::optional<SnapCandidate>> find_candidate(
    const geometry::CompiledMesh& mesh,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius,
    const core::Transform& transform,
    std::optional<scene::ObjectId> object,
    std::optional<std::uint64_t> mesh_asset,
    core::Revision source_revision) {
    if (auto result = validate_request(origin, settings, radius); !result) {
        return core::Result<std::optional<SnapCandidate>>::failure(result.error());
    }
    if (!settings.enabled) {
        return core::Result<std::optional<SnapCandidate>>::success(std::nullopt);
    }
    if (!mesh.valid()) {
        return core::Result<std::optional<SnapCandidate>>::failure(
            invalid("compiled snap source is invalid"));
    }
    if (!transform.finite() || object.has_value() != mesh_asset.has_value() ||
        (mesh_asset.has_value() && *mesh_asset == 0U)) {
        return core::Result<std::optional<SnapCandidate>>::failure(
            invalid("snap candidate transform or scene identity is invalid"));
    }

    std::optional<SnapCandidate> best;
    const auto consider = [&](SnapCandidate candidate) -> core::Result<void> {
        candidate.position = transform_point(transform, candidate.position);
        if (!candidate.position.finite()) {
            return core::Result<void>::failure(invalid(
                "snap candidate position is non-finite"));
        }
        const double distance = (candidate.position - origin).length();
        if (!std::isfinite(distance)) {
            return core::Result<void>::failure(invalid(
                "snap candidate distance is non-finite"));
        }
        if (distance > radius) return core::Result<void>::success();
        candidate.distance = distance;
        candidate.object = object;
        candidate.mesh_asset = mesh_asset;
        candidate.source_revision = source_revision;
        candidate.mesh_revision = mesh.source_revision;
        const bool closer = !best.has_value() || distance < best->distance;
        const bool tied_and_stable = best.has_value() && distance == best->distance &&
            candidate_id(candidate) < candidate_id(*best);
        if (closer || tied_and_stable) best = std::move(candidate);
        return core::Result<void>::success();
    };

    if (settings.kind == SnapKind::vertex) {
        for (std::size_t index = 0U; index < mesh.positions.size(); ++index) {
            SnapCandidate candidate;
            candidate.kind = SnapKind::vertex;
            candidate.vertex = mesh.vertex_ids[index];
            candidate.position = mesh.positions[index];
            if (auto result = consider(std::move(candidate)); !result) {
                return core::Result<std::optional<SnapCandidate>>::failure(result.error());
            }
        }
    } else if (settings.kind == SnapKind::edge_midpoint) {
        std::set<geometry::EdgeId> seen;
        for (std::size_t triangle = 0U; triangle < mesh.triangle_edges.size(); ++triangle) {
            for (std::size_t edge_index = 0U; edge_index < 3U; ++edge_index) {
                const auto edge = mesh.triangle_edges[triangle][edge_index];
                // A triangulation diagonal has no authored EdgeId and must
                // never become an editable snap target.
                if (!edge.has_value()) continue;
                if (!seen.insert(*edge).second) continue;
                const std::size_t first_index = triangle * 3U + edge_index;
                const std::size_t second_index = triangle * 3U + (edge_index + 1U) % 3U;
                const auto first = mesh.indices[first_index];
                const auto second = mesh.indices[second_index];
                SnapCandidate candidate;
                candidate.kind = SnapKind::edge_midpoint;
                candidate.edge = *edge;
                candidate.position = (mesh.positions[first] + mesh.positions[second]) * 0.5;
                if (auto result = consider(std::move(candidate)); !result) {
                    return core::Result<std::optional<SnapCandidate>>::failure(result.error());
                }
            }
        }
    } else {
        std::map<geometry::FaceId, std::set<std::uint32_t>> face_vertices;
        for (std::size_t triangle = 0U; triangle < mesh.triangle_faces.size(); ++triangle) {
            auto& vertices = face_vertices[mesh.triangle_faces[triangle]];
            vertices.insert(mesh.indices[triangle * 3U]);
            vertices.insert(mesh.indices[triangle * 3U + 1U]);
            vertices.insert(mesh.indices[triangle * 3U + 2U]);
        }
        for (const auto& [face, vertices] : face_vertices) {
            if (vertices.empty()) {
                return core::Result<std::optional<SnapCandidate>>::failure(
                    invalid("compiled face candidate has no vertices"));
            }
            core::Vec3d center{};
            for (const auto index : vertices) {
                center = center + mesh.positions[index];
                if (!center.finite()) {
                    return core::Result<std::optional<SnapCandidate>>::failure(
                        invalid("compiled face center is non-finite"));
                }
            }
            center = center * (1.0 / static_cast<double>(vertices.size()));
            SnapCandidate candidate;
            candidate.kind = SnapKind::face_center;
            candidate.face = face;
            candidate.position = center;
            if (auto result = consider(std::move(candidate)); !result) {
                return core::Result<std::optional<SnapCandidate>>::failure(result.error());
            }
        }
    }
    return core::Result<std::optional<SnapCandidate>>::success(std::move(best));
}

} // namespace

core::Result<std::optional<SnapCandidate>> find_snap_candidate(
    const geometry::CompiledMesh& mesh,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius) {
    return find_candidate(mesh, origin, settings, radius, core::Transform::identity(),
        std::nullopt, std::nullopt, mesh.source_revision);
}

core::Result<std::vector<SnapCandidate>> find_snap_candidates(
    const project::ProjectDocument& project,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius) {
    if (auto result = validate_request(origin, settings, radius); !result) {
        return core::Result<std::vector<SnapCandidate>>::failure(result.error());
    }
    if (!settings.enabled) {
        return core::Result<std::vector<SnapCandidate>>::success({});
    }

    const auto& source_scene = project.scene();
    const auto& meshes = project.meshes();
    std::map<std::uint64_t, geometry::CompiledMesh> compiled_meshes;
    std::vector<SnapCandidate> candidates;
    for (const auto& object : source_scene.objects_sorted()) {
        const auto visible = effectively_visible(source_scene, object);
        if (!visible) {
            return core::Result<std::vector<SnapCandidate>>::failure(visible.error());
        }
        if (!visible.value() || !object.mesh_asset.has_value()) continue;

        const auto mesh = meshes.find(*object.mesh_asset);
        if (mesh == meshes.end()) {
            return core::Result<std::vector<SnapCandidate>>::failure(invalid(
                "scene snap target references a missing mesh asset"));
        }
        auto compiled = compiled_meshes.find(*object.mesh_asset);
        if (compiled == compiled_meshes.end()) {
            const auto result = mesh->second.compile();
            if (!result) {
                return core::Result<std::vector<SnapCandidate>>::failure(result.error());
            }
            compiled = compiled_meshes.emplace(*object.mesh_asset, result.value()).first;
        }
        const auto transform = source_scene.world_transform(object.id);
        if (!transform) {
            return core::Result<std::vector<SnapCandidate>>::failure(transform.error());
        }
        auto candidate = find_candidate(compiled->second, origin, settings, radius,
            transform.value(), object.id, *object.mesh_asset, project.revision());
        if (!candidate) {
            return core::Result<std::vector<SnapCandidate>>::failure(candidate.error());
        }
        if (candidate.value().has_value()) {
            candidates.push_back(std::move(*candidate.value()));
        }
    }
    return core::Result<std::vector<SnapCandidate>>::success(std::move(candidates));
}

core::Result<std::vector<SnapCandidate>> find_snap_candidates(
    const render::RenderScene& scene,
    core::Vec3d origin,
    std::span<const SnapKind> kinds,
    core::Revision source_revision,
    double radius) {
    if (!origin.finite() || !std::isfinite(radius) || radius < 0.0) {
        return core::Result<std::vector<SnapCandidate>>::failure(invalid(
            "compiled snap scene origin or radius is invalid"));
    }
    if (kinds.empty()) {
        return core::Result<std::vector<SnapCandidate>>::success({});
    }
    std::array<bool, 3> seen{};
    for (const SnapKind kind : kinds) {
        if (kind != SnapKind::vertex && kind != SnapKind::edge_midpoint &&
            kind != SnapKind::face_center) {
            return core::Result<std::vector<SnapCandidate>>::failure(unsupported(
                "compiled snap scene only supports vertex, edge-midpoint, and face-center targets"));
        }
        const auto slot = kind == SnapKind::vertex ? 0U :
            kind == SnapKind::edge_midpoint ? 1U : 2U;
        if (seen[slot]) {
            return core::Result<std::vector<SnapCandidate>>::failure(invalid(
                "compiled snap scene target kinds must be unique"));
        }
        seen[slot] = true;
    }

    constexpr std::size_t kMaxViewportSnapCandidates = 65'536U;
    std::vector<SnapCandidate> candidates;
    const auto instances = scene.instances();
    if (instances.size() > kMaxViewportSnapCandidates / kinds.size()) {
        return core::Result<std::vector<SnapCandidate>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "compiled snap scene exceeds its bounded candidate budget"));
    }
    candidates.reserve(instances.size() * kinds.size());
    for (const auto& instance : instances) {
        if (!instance.mesh_asset.has_value()) continue;
        if (*instance.mesh_asset == 0U || instance.mesh == nullptr ||
            instance.source_revision != instance.mesh->source_revision) {
            return core::Result<std::vector<SnapCandidate>>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "compiled snap scene contains an unbound or stale mesh instance"));
        }
        for (const SnapKind kind : kinds) {
            if (auto result = append_viewport_candidates(
                    *instance.mesh, origin, radius, kind, instance.world_transform,
                    instance.object, *instance.mesh_asset, source_revision, candidates);
                !result) {
                return core::Result<std::vector<SnapCandidate>>::failure(
                    result.error().with_context("compiled viewport snap query"));
            }
        }
    }
    return core::Result<std::vector<SnapCandidate>>::success(std::move(candidates));
}

} // namespace carto::editor
