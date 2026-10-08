#include <carto/editor/snap_candidates.hpp>

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

core::Result<std::optional<SnapCandidate>> find_snap_candidate(
    const geometry::CompiledMesh& mesh,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius) {
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

    std::optional<SnapCandidate> best;
    const auto consider = [&](SnapCandidate candidate) -> core::Result<void> {
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
        candidate.source_revision = mesh.source_revision;
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
                if (!edge.has_value()) {
                    return core::Result<std::optional<SnapCandidate>>::failure(
                        invalid("compiled edge candidate is missing stable identity"));
                }
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

} // namespace carto::editor
