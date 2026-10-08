#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/editor/selection.hpp>

#include <optional>
#include <vector>

namespace carto::editor {

enum class CoordinateSpace {
    world,
    local,
    parent,
    normal,
    view,
};

enum class PivotMode {
    median,
    bounding_box,
    active_element,
    cursor,
    individual_origins,
    custom,
};

enum class SnapKind {
    grid,
    increment,
    vertex,
    edge,
    edge_midpoint,
    face,
    face_center,
    surface,
    normal,
};

struct SnapSettings {
    bool enabled = false;
    SnapKind kind = SnapKind::increment;
    double increment = 1.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] constexpr bool operator==(const SnapSettings&) const noexcept = default;
};

// Applies only grid/increment snapping. Geometry-target kinds require a
// candidate service with visible target identity and are rejected here rather
// than silently treated as grid snapping.
[[nodiscard]] core::Result<double> snap_scalar(double value, const SnapSettings& settings);
[[nodiscard]] core::Result<core::Vec3d> snap_vector(
    core::Vec3d value,
    const SnapSettings& settings);

// AuthoringContext is a value snapshot of the operator's validated intent.
// It contains no project references and therefore cannot grant mutation
// authority. The application/session layer is responsible for validating the
// copied identities against the current document before using it.
struct AuthoringContext {
    core::Revision project_revision;

    SelectionMode selection_mode = SelectionMode::object;
    std::vector<scene::ObjectId> objects;
    std::vector<geometry::VertexId> vertices;
    std::vector<geometry::EdgeId> edges;
    std::vector<geometry::FaceId> faces;
    std::optional<scene::ObjectId> component_object;
    std::optional<core::Revision> component_mesh_revision;

    CoordinateSpace coordinate_space = CoordinateSpace::world;
    PivotMode pivot_mode = PivotMode::median;
    SnapSettings snap;

    [[nodiscard]] static core::Result<AuthoringContext> from_selection(
        core::Revision project_revision,
        const SelectionState& selection);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] constexpr bool operator==(const AuthoringContext&) const noexcept = default;
};

} // namespace carto::editor
