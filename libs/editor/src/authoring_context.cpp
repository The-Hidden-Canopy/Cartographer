#include <carto/editor/authoring_context.hpp>

#include <algorithm>
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
    context.component_groups = selection.component_groups();
    context.component_object = selection.component_object();
    if (context.component_object.has_value()) {
        context.vertices = selection.selected_vertices();
        context.vertex_selection_order = selection.selected_vertices_in_order();
        context.edges = selection.selected_edges();
        context.faces = selection.selected_faces();
        context.component_mesh_revision = selection.component_mesh_revision();
    }
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
    if (auto result = validate_ids(
            vertex_selection_order, "authoring vertex selection order"); !result) {
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
    if (!vertex_selection_order.empty()) {
        if (selection_mode != SelectionMode::vertex ||
            std::set<geometry::VertexId>(vertex_selection_order.begin(),
                                         vertex_selection_order.end()) !=
                std::set<geometry::VertexId>(vertices.begin(), vertices.end())) {
            return core::Result<void>::failure(invalid(
                "authoring vertex selection order does not match selected identities"));
        }
    }
    if (selection_mode == SelectionMode::edge && (!vertices.empty() || !faces.empty())) {
        return core::Result<void>::failure(
            invalid("edge authoring context contains another component selection"));
    }
    if (selection_mode == SelectionMode::face && (!vertices.empty() || !edges.empty())) {
        return core::Result<void>::failure(
            invalid("face authoring context contains another component selection"));
    }
    if (selection_mode == SelectionMode::object && !component_groups.empty()) {
        return core::Result<void>::failure(
            invalid("object authoring context contains component selection groups"));
    }
    std::set<scene::ObjectId> group_owners;
    for (const auto& group : component_groups) {
        if (!group.object || group.mesh_revision.exhausted() ||
            !group_owners.insert(group.object).second) {
            return core::Result<void>::failure(invalid(
                "authoring component groups require unique owners and valid revisions"));
        }
        if (auto result = validate_ids(group.vertices, "component-group vertices"); !result) {
            return result;
        }
        if (auto result = validate_ids(
                group.vertex_selection_order, "component-group vertex order"); !result) {
            return result;
        }
        if (auto result = validate_ids(group.edges, "component-group edges"); !result) {
            return result;
        }
        if (auto result = validate_ids(group.faces, "component-group faces"); !result) {
            return result;
        }
        if ((selection_mode == SelectionMode::vertex &&
             (!group.edges.empty() || !group.faces.empty())) ||
            (selection_mode == SelectionMode::edge &&
             (!group.vertices.empty() || !group.faces.empty() || group.edges.empty())) ||
            (selection_mode == SelectionMode::face &&
             (!group.vertices.empty() || !group.edges.empty() || group.faces.empty()))) {
            return core::Result<void>::failure(invalid(
                "authoring component group does not match its selection mode"));
        }
        if (!group.vertex_selection_order.empty() &&
            std::set<geometry::VertexId>(group.vertex_selection_order.begin(),
                                         group.vertex_selection_order.end()) !=
                std::set<geometry::VertexId>(group.vertices.begin(), group.vertices.end())) {
            return core::Result<void>::failure(invalid(
                "component-group vertex order does not match selected identities"));
        }
    }
    if (!component_groups.empty()) {
        if (component_object.has_value()) {
            if (!component_mesh_revision.has_value() ||
                !group_owners.contains(*component_object)) {
                return core::Result<void>::failure(invalid(
                    "authoring component groups do not contain the active owner"));
            }
            const auto active = std::find_if(component_groups.begin(), component_groups.end(),
                [this](const ComponentSelectionGroup& group) {
                    return group.object == *component_object;
                });
            if (active == component_groups.end() ||
                active->mesh_revision != *component_mesh_revision ||
                active->vertices != vertices ||
                active->vertex_selection_order != vertex_selection_order ||
                active->edges != edges || active->faces != faces) {
                return core::Result<void>::failure(invalid(
                    "authoring active component group does not match its selection snapshot"));
            }
        } else {
            if (component_mesh_revision.has_value() || component_groups.size() < 2U ||
                !vertices.empty() || !vertex_selection_order.empty() ||
                !edges.empty() || !faces.empty()) {
                return core::Result<void>::failure(invalid(
                    "multi-object authoring context requires grouped component identities"));
            }
        }
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
