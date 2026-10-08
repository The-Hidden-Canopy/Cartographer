#pragma once

#include <carto/editor/authoring_context.hpp>
#include <carto/geometry/mesh.hpp>

#include <optional>

namespace carto::editor {

// A read-only candidate returned by the authoring snap service. The source
// revision is part of the result so a pointer/UI consumer cannot apply a
// target discovered against an older topology snapshot without rechecking it.
struct SnapCandidate {
    SnapKind kind = SnapKind::vertex;
    std::optional<geometry::VertexId> vertex;
    std::optional<geometry::EdgeId> edge;
    std::optional<geometry::FaceId> face;
    core::Vec3d position{};
    double distance = 0.0;
    core::Revision source_revision;
};

// Finds the nearest discrete authoring target within radius. This is
// deliberately not a viewport projection or mutation path: vertex,
// edge-midpoint, and face-center candidates are supported; edge projection,
// surface projection, and normal snapping fail closed until they have an
// explicit geometric contract and visible target identity.
[[nodiscard]] core::Result<std::optional<SnapCandidate>> find_snap_candidate(
    const geometry::EditableMesh& mesh,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius);

// Viewport consumers can query an already-compiled snapshot without exposing
// mutable authoring state. The returned source revision is the compiled
// snapshot's revision and must be checked before any later authoring action.
[[nodiscard]] core::Result<std::optional<SnapCandidate>> find_snap_candidate(
    const geometry::CompiledMesh& mesh,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius);

} // namespace carto::editor
