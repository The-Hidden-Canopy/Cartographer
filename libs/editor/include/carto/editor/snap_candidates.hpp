#pragma once

#include <carto/editor/authoring_context.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/render/render_scene.hpp>
#include <carto/scene/scene.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::editor {

// A read-only candidate returned by the authoring snap service. The source
// revision is part of the result so a pointer/UI consumer cannot apply a
// target discovered against an older topology snapshot without rechecking it.
struct SnapCandidate {
    SnapKind kind = SnapKind::vertex;
    std::optional<geometry::VertexId> vertex;
    std::optional<geometry::EdgeId> edge;
    std::optional<geometry::FaceId> face;
    // Scene-wide targets carry both owners. Their pair distinguishes stable
    // mesh element IDs reused by different scene instances.
    std::optional<scene::ObjectId> object;
    std::optional<std::uint64_t> mesh_asset;
    core::Vec3d position{};
    double distance = 0.0;
    core::Revision source_revision;
    core::Revision mesh_revision;
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

// Builds deterministic, world-space candidates from visible project objects.
// The document revision binds every result across both mesh and transform
// changes; mesh_revision preserves the narrower topology provenance. This is
// a query only and does not create viewport markers or apply transforms.
[[nodiscard]] core::Result<std::vector<SnapCandidate>> find_snap_candidates(
    const project::ProjectDocument& project,
    core::Vec3d origin,
    const SnapSettings& settings,
    double radius);

// Queries an already-compiled viewport snapshot. Results bind the snapshot's
// project revision and each compiled mesh revision, avoiding per-pointer-move
// mesh recompilation in native transform interactions. Missing mesh-asset
// identities are not eligible snap targets.
[[nodiscard]] core::Result<std::vector<SnapCandidate>> find_snap_candidates(
    const render::RenderScene& scene,
    core::Vec3d origin,
    std::span<const SnapKind> kinds,
    core::Revision source_revision,
    double radius);

} // namespace carto::editor
