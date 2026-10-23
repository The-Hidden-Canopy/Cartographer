#include <carto/character/controls.hpp>

#include <algorithm>
#include <iomanip>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_shape(ControlShape shape) {
    switch (shape) {
    case ControlShape::sphere:
    case ControlShape::box:
    case ControlShape::curve:
    case ControlShape::locator:
        return true;
    }
    return false;
}

bool safe_token(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

constexpr std::string_view kSerializeMagic = "CARTOGRAPHER_CONTROL_GRAPH";

void write_transform(std::ostream& output, const core::Transform& transform) {
    output << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

// Canonical text for one control: every authored field, in a fixed field
// order, with the affected joints in their sorted order.
void write_control(std::ostream& output, const ControlDefinition& control) {
    output << "CONTROL " << control.id.value << ' '
           << static_cast<unsigned>(control.shape) << ' ';
    write_transform(output, control.transform);
    output << ' ' << static_cast<unsigned>(control.allowed_axes);
    for (std::size_t axis = 0U; axis < kAxisCount; ++axis) {
        output << ' ' << control.axis_minimum[axis] << ' '
               << control.axis_maximum[axis];
    }
    output << ' ' << control.affected_joints.size();
    for (const kinematic::JointId joint : control.affected_joints) {
        output << ' ' << joint.value;
    }
    output << ' ' << (control.follow_space.has_value()
                         ? control.follow_space->value
                         : 0U)
           << ' ' << std::quoted(control.control_set) << '\n';
}

} // namespace

std::string_view control_shape_name(ControlShape shape) noexcept {
    switch (shape) {
    case ControlShape::sphere: return "sphere";
    case ControlShape::box: return "box";
    case ControlShape::curve: return "curve";
    case ControlShape::locator: return "locator";
    }
    return "unknown";
}

core::Result<void> ControlDefinition::validate() const {
    if (!id || !valid_shape(shape) || !transform.finite() ||
        affected_joints.size() > ControlGraph::kMaxAffectedJointsPerControl ||
        (allowed_axes & ~kAllowedAxesMask) != 0U ||
        !safe_token(control_set, ControlGraph::kMaxControlSetBytes, true)) {
        return core::Result<void>::failure(invalid(
            "control identity, shape, transform, joints, axes, or set is invalid"));
    }
    if (!std::is_sorted(affected_joints.begin(), affected_joints.end()) ||
        std::adjacent_find(affected_joints.begin(), affected_joints.end()) !=
            affected_joints.end() ||
        std::any_of(affected_joints.begin(), affected_joints.end(),
            [](kinematic::JointId joint) { return !joint; })) {
        return core::Result<void>::failure(validation(
            "control affected joints must be non-zero, sorted, and unique"));
    }
    if (follow_space.has_value() && follow_space.value() == id) {
        return core::Result<void>::failure(validation(
            "control may not follow itself"));
    }
    for (std::size_t axis = 0U; axis < kAxisCount; ++axis) {
        if (!std::isfinite(axis_minimum[axis]) || !std::isfinite(axis_maximum[axis]) ||
            axis_minimum[axis] > axis_maximum[axis]) {
            return core::Result<void>::failure(validation(
                "control axis limits must be finite and ordered"));
        }
        const std::uint8_t bit = static_cast<std::uint8_t>(1U << axis);
        if ((axis_minimum[axis] != 0.0 || axis_maximum[axis] != 0.0) &&
            (allowed_axes & bit) == 0U) {
            return core::Result<void>::failure(validation(
                "control may only limit axes it allows"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ControlGraph::insert(ControlDefinition control) {
    if (auto result = control.validate(); !result) return result;
    if (nodes_.size() >= kMaxControls) {
        return core::Result<void>::failure(validation(
            "control graph has reached its bounded capacity"));
    }
    if (!nodes_.emplace(control.id, std::move(control)).second) {
        return core::Result<void>::failure(invalid(
            "control identity is duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<void> ControlGraph::insert_edge(ControlEdge edge) {
    if (!edge.driver || !edge.follower || edge.driver == edge.follower) {
        return core::Result<void>::failure(invalid(
            "control edge endpoints must be distinct non-zero controls"));
    }
    if (edges_.size() >= kMaxEdges) {
        return core::Result<void>::failure(validation(
            "control graph edges have reached their bounded capacity"));
    }
    if (!std::is_sorted(edges_.begin(), edges_.end()) ||
        std::adjacent_find(edges_.begin(), edges_.end()) != edges_.end()) {
        return core::Result<void>::failure(validation(
            "control graph edges must remain sorted and unique"));
    }
    const auto position = std::lower_bound(edges_.begin(), edges_.end(), edge);
    if (position != edges_.end() && *position == edge) {
        return core::Result<void>::failure(invalid(
            "control edge is duplicated"));
    }
    edges_.insert(position, edge);
    return core::Result<void>::success();
}

const ControlDefinition* ControlGraph::find(ControlId id) const noexcept {
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? nullptr : &found->second;
}

core::Result<void> ControlGraph::validate() const {
    if (nodes_.size() > kMaxControls || edges_.size() > kMaxEdges) {
        return core::Result<void>::failure(validation(
            "control graph exceeds its bounded capacity"));
    }
    for (const auto& [identity, control] : nodes_) {
        if (identity != control.id) {
            return core::Result<void>::failure(validation(
                "control map identity does not match its record"));
        }
        if (auto result = control.validate(); !result) return result;
        if (control.follow_space.has_value() &&
            !nodes_.contains(control.follow_space.value())) {
            return core::Result<void>::failure(validation(
                "control follow-space referent is missing from the graph"));
        }
    }
    if (!std::is_sorted(edges_.begin(), edges_.end()) ||
        std::adjacent_find(edges_.begin(), edges_.end()) != edges_.end()) {
        return core::Result<void>::failure(validation(
            "control graph edges must be sorted and unique"));
    }
    for (const ControlEdge& edge : edges_) {
        if (!nodes_.contains(edge.driver) || !nodes_.contains(edge.follower)) {
            return core::Result<void>::failure(validation(
                "control edge references a missing control"));
        }
    }
    // Acyclicity over follow-space dependencies and explicit edges combined:
    // the control side of the control+rig dependency DAG.
    std::map<ControlId, std::vector<ControlId>> dependents;
    for (const auto& [identity, control] : nodes_) {
        if (control.follow_space.has_value()) {
            dependents[control.follow_space.value()].push_back(identity);
        }
    }
    for (const ControlEdge& edge : edges_) {
        dependents[edge.driver].push_back(edge.follower);
    }
    enum class VisitState : std::uint8_t { unvisited, visiting, done };
    std::map<ControlId, VisitState> states;
    for (const auto& [identity, control] : nodes_) {
        static_cast<void>(control);
        states.emplace(identity, VisitState::unvisited);
    }
    const auto visit = [&](const ControlId start) -> core::Result<void> {
        std::vector<ControlId> stack{start};
        while (!stack.empty()) {
            const ControlId current = stack.back();
            states[current] = VisitState::visiting;
            bool descended = false;
            const auto found = dependents.find(current);
            if (found != dependents.end()) {
                for (const ControlId next : found->second) {
                    if (states[next] == VisitState::visiting) {
                        return core::Result<void>::failure(validation(
                            "control dependency DAG contains a cycle"));
                    }
                    if (states[next] == VisitState::unvisited) {
                        stack.push_back(next);
                        descended = true;
                        break;
                    }
                }
            }
            if (!descended) {
                states[current] = VisitState::done;
                stack.pop_back();
            }
        }
        return core::Result<void>::success();
    };
    for (const auto& [identity, state] : states) {
        if (state == VisitState::unvisited) {
            if (auto result = visit(identity); !result) return result;
        }
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> ControlGraph::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::ostringstream output;
    output << kSerializeMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "CONTROLS " << nodes_.size() << '\n';
    for (const auto& [identity, control] : nodes_) {
        static_cast<void>(identity);
        write_control(output, control);
    }
    output << "EDGES " << edges_.size() << '\n';
    for (const ControlEdge& edge : edges_) {
        output << "EDGE " << edge.driver.value << ' ' << edge.follower.value
               << '\n';
    }
    output << "END\n";
    const std::string canonical = output.str();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()),
            canonical.size()}));
}

} // namespace carto::character
