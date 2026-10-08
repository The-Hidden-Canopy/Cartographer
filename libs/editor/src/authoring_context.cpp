#include <carto/editor/authoring_context.hpp>

#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace carto::editor {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

template <typename Id>
core::Result<void> validate_ids(const std::vector<Id>& ids, const char* label) {
    std::set<Id> unique;
    for (const Id id : ids) {
        if (!id) {
            return core::Result<void>::failure(
                invalid(std::string(label) + " contains a zero identity"));
        }
        if (!unique.insert(id).second) {
            return core::Result<void>::failure(
                invalid(std::string(label) + " contains duplicate identities"));
        }
    }
    return core::Result<void>::success();
}

bool valid_selection_mode(SelectionMode mode) noexcept {
    return mode == SelectionMode::object || mode == SelectionMode::vertex ||
        mode == SelectionMode::edge || mode == SelectionMode::face;
}

bool valid_coordinate_space(CoordinateSpace space) noexcept {
    return space == CoordinateSpace::world || space == CoordinateSpace::local ||
        space == CoordinateSpace::parent || space == CoordinateSpace::normal ||
        space == CoordinateSpace::view;
}

bool valid_pivot_mode(PivotMode mode) noexcept {
    return mode == PivotMode::median || mode == PivotMode::bounding_box ||
        mode == PivotMode::active_element || mode == PivotMode::cursor ||
        mode == PivotMode::individual_origins || mode == PivotMode::custom;
}

bool valid_snap_kind(SnapKind kind) noexcept {
    return kind == SnapKind::grid || kind == SnapKind::increment ||
        kind == SnapKind::vertex || kind == SnapKind::edge ||
        kind == SnapKind::edge_midpoint || kind == SnapKind::face ||
        kind == SnapKind::face_center || kind == SnapKind::surface ||
        kind == SnapKind::normal;
}

} // namespace

core::Result<void> SnapSettings::validate() const {
    if (!valid_snap_kind(kind)) {
        return core::Result<void>::failure(invalid("snap kind is invalid"));
    }
    if (!std::isfinite(increment) || increment <= 0.0) {
        return core::Result<void>::failure(
            invalid("snap increment must be finite and strictly positive"));
    }
    return core::Result<void>::success();
}

core::Result<double> snap_scalar(double value, const SnapSettings& settings) {
    if (!std::isfinite(value)) {
        return core::Result<double>::failure(invalid(
            "snap input must be finite"));
    }
    if (auto result = settings.validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (!settings.enabled) return core::Result<double>::success(value);
    if (settings.kind != SnapKind::grid && settings.kind != SnapKind::increment) {
        return core::Result<double>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "geometry-target snapping requires a candidate service"));
    }
    const double units = value / settings.increment;
    const double rounded = std::round(units);
    const double snapped = rounded * settings.increment;
    if (!std::isfinite(units) || !std::isfinite(rounded) || !std::isfinite(snapped)) {
        return core::Result<double>::failure(invalid(
            "snap result is non-finite"));
    }
    return core::Result<double>::success(snapped);
}

core::Result<core::Vec3d> snap_vector(
    core::Vec3d value,
    const SnapSettings& settings) {
    const auto x = snap_scalar(value.x, settings);
    if (!x) return core::Result<core::Vec3d>::failure(x.error());
    const auto y = snap_scalar(value.y, settings);
    if (!y) return core::Result<core::Vec3d>::failure(y.error());
    const auto z = snap_scalar(value.z, settings);
    if (!z) return core::Result<core::Vec3d>::failure(z.error());
    return core::Result<core::Vec3d>::success({x.value(), y.value(), z.value()});
}

core::Result<AuthoringContext> AuthoringContext::from_selection(
    core::Revision project_revision,
    const SelectionState& selection) {
    AuthoringContext context;
    context.project_revision = project_revision;
    context.selection_mode = selection.mode();
    context.objects = selection.selected_objects();
    context.vertices = selection.selected_vertices();
    context.edges = selection.selected_edges();
    context.faces = selection.selected_faces();
    context.component_object = selection.component_object();
    context.component_mesh_revision = selection.component_mesh_revision();
    if (auto result = context.validate(); !result) {
        return core::Result<AuthoringContext>::failure(result.error());
    }
    return core::Result<AuthoringContext>::success(std::move(context));
}

core::Result<void> AuthoringContext::validate() const {
    if (!valid_selection_mode(selection_mode)) {
        return core::Result<void>::failure(invalid("authoring selection mode is invalid"));
    }
    if (auto result = validate_ids(objects, "authoring object selection"); !result) {
        return result;
    }
    if (auto result = validate_ids(vertices, "authoring vertex selection"); !result) {
        return result;
    }
    if (auto result = validate_ids(edges, "authoring edge selection"); !result) {
        return result;
    }
    if (auto result = validate_ids(faces, "authoring face selection"); !result) {
        return result;
    }
    if (component_object.has_value() && !*component_object) {
        return core::Result<void>::failure(invalid("authoring component object is zero"));
    }
    if (component_mesh_revision.has_value() && component_mesh_revision->exhausted()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state,
                             "authoring component mesh revision is exhausted"));
    }
    if (selection_mode == SelectionMode::object &&
        (!vertices.empty() || !edges.empty() || !faces.empty() || component_object.has_value() ||
         component_mesh_revision.has_value())) {
        return core::Result<void>::failure(
            invalid("object authoring context contains component selection state"));
    }
    if (selection_mode == SelectionMode::vertex && (!edges.empty() || !faces.empty())) {
        return core::Result<void>::failure(
            invalid("vertex authoring context contains another component selection"));
    }
    if (selection_mode == SelectionMode::edge && (!vertices.empty() || !faces.empty())) {
        return core::Result<void>::failure(
            invalid("edge authoring context contains another component selection"));
    }
    if (selection_mode == SelectionMode::face && (!vertices.empty() || !edges.empty())) {
        return core::Result<void>::failure(
            invalid("face authoring context contains another component selection"));
    }
    if (!valid_coordinate_space(coordinate_space)) {
        return core::Result<void>::failure(invalid("authoring coordinate space is invalid"));
    }
    if (!valid_pivot_mode(pivot_mode)) {
        return core::Result<void>::failure(invalid("authoring pivot mode is invalid"));
    }
    return snap.validate();
}

} // namespace carto::editor
