#include <carto/pose/controls.hpp>

#include <carto/pose/rotation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace carto::pose {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

// Clamps and masks one control's raw value: disallowed axes must be zero
// (fail closed rather than silently dropping artist input), allowed axes are
// clamped to the control's authored per-axis limits.
core::Result<std::array<double, character::kAxisCount>> clamped_value(
    const character::ControlDefinition& control,
    const std::array<double, character::kAxisCount>& raw) {
    std::array<double, character::kAxisCount> value{};
    for (std::size_t axis = 0U; axis < character::kAxisCount; ++axis) {
        if (!std::isfinite(raw[axis])) {
            return core::Result<std::array<double, character::kAxisCount>>::failure(invalid(
                "control value on a non-finite axis"));
        }
        const std::uint8_t bit = static_cast<std::uint8_t>(1U << axis);
        if ((control.allowed_axes & bit) == 0U) {
            if (raw[axis] != 0.0) {
                return core::Result<std::array<double, character::kAxisCount>>::failure(validation(
                    "control value drives an axis the control does not allow"));
            }
            value[axis] = 0.0;
            continue;
        }
        value[axis] = std::clamp(
            raw[axis], control.axis_minimum[axis], control.axis_maximum[axis]);
    }
    return core::Result<std::array<double, character::kAxisCount>>::success(value);
}

core::Transform offset_from_value(const std::array<double, character::kAxisCount>& value) {
    core::Transform offset = core::Transform::identity();
    offset.translation = {value[0], value[1], value[2]};
    offset.rotation = from_euler_xyz({value[3], value[4], value[5]});
    return offset;
}

} // namespace

core::Result<ControlEvaluation> evaluate_controls(
    const character::ControlGraph& graph,
    const ControlValues& values) {
    if (auto result = graph.validate(); !result) {
        return core::Result<ControlEvaluation>::failure(result.error().with_context(
            "control evaluation requires a valid control graph"));
    }
    for (const auto& [control_id, raw] : values) {
        if (graph.find(control_id) == nullptr) {
            return core::Result<ControlEvaluation>::failure(invalid(
                "control values reference a control outside the graph"));
        }
        static_cast<void>(raw);
    }

    // Ordering edges: follow-space inheritance plus explicit dependency
    // edges. Inheritance values flow through follow_space only; explicit
    // edges declare evaluation order without double-applying a driver's
    // offset (documented ControlEdge semantics).
    std::map<character::ControlId, std::vector<character::ControlId>> followers;
    std::map<character::ControlId, std::vector<character::ControlId>> drivers_of;
    std::map<character::ControlId, std::size_t> indegree;
    for (const auto& [identity, control] : graph.nodes()) {
        static_cast<void>(control);
        indegree.emplace(identity, 0U);
    }
    const auto add_ordering_edge = [&](character::ControlId driver, character::ControlId follower) {
        followers[driver].push_back(follower);
        ++indegree[follower];
    };
    for (const auto& [identity, control] : graph.nodes()) {
        if (control.follow_space.has_value()) {
            add_ordering_edge(control.follow_space.value(), identity);
            drivers_of[identity].push_back(control.follow_space.value());
        }
    }
    for (const character::ControlEdge& edge : graph.edges()) {
        add_ordering_edge(edge.driver, edge.follower);
    }

    // Deterministic Kahn topological order: drivers before followers, ready
    // set tie-broken by control id. The authored graph is contractually
    // acyclic; the walk re-checks instead of trusting the contract.
    std::vector<character::ControlId> ready;
    for (const auto& [identity, degree] : indegree) {
        if (degree == 0U) ready.push_back(identity);
    }
    ControlEvaluation evaluation;
    std::map<character::ControlId, std::array<double, character::kAxisCount>> effective;
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end());
        const character::ControlId current = ready.front();
        ready.erase(ready.begin());
        const character::ControlDefinition* control = graph.find(current);
        std::array<double, character::kAxisCount> own{};
        const auto raw = values.find(current);
        const bool touched = raw != values.end();
        if (touched) own = raw->second;
        auto clamped = clamped_value(*control, own);
        if (!clamped) return core::Result<ControlEvaluation>::failure(clamped.error());
        // Follow-space inheritance: the effective value is this control's own
        // clamped value plus each follow driver's effective value.
        std::array<double, character::kAxisCount> total = clamped.value();
        for (const character::ControlId driver : drivers_of[current]) {
            const auto inherited = effective.find(driver);
            if (inherited == effective.end()) {
                return core::Result<ControlEvaluation>::failure(validation(
                    "control dependency DAG contains a cycle"));
            }
            for (std::size_t axis = 0U; axis < character::kAxisCount; ++axis) {
                total[axis] += inherited->second[axis];
            }
        }
        effective.emplace(current, total);
        // A control the user did not touch exerts no offset: it still
        // participates in ordering and inheritance, but emits nothing.
        if (!touched) {
            evaluation.evaluation_order.push_back(current);
            for (const character::ControlId next : followers[current]) {
                if (--indegree[next] == 0U) ready.push_back(next);
            }
            continue;
        }
        const core::Transform offset = offset_from_value(total);
        // A follower already carries its follow driver's effective value, so
        // on joints the driver also affects the follower's offset supersedes
        // the driver's rather than accumulating it (the inheritance would
        // otherwise be applied twice). Unrelated controls sharing a joint
        // still accumulate.
        const character::ControlDefinition* follow_driver =
            control->follow_space.has_value()
                ? graph.find(control->follow_space.value())
                : nullptr;
        for (const kinematic::JointId joint : control->affected_joints) {
            const bool supersedes =
                follow_driver != nullptr &&
                std::find(follow_driver->affected_joints.begin(),
                    follow_driver->affected_joints.end(), joint) !=
                    follow_driver->affected_joints.end();
            const auto inserted = evaluation.joint_offsets.emplace(joint, offset);
            if (!inserted.second) {
                if (supersedes) {
                    inserted.first->second = offset;
                    continue;
                }
                core::Transform& accumulated = inserted.first->second;
                accumulated.translation = accumulated.translation + offset.translation;
                accumulated.rotation =
                    (accumulated.rotation * offset.rotation).normalized();
            }
        }
        evaluation.evaluation_order.push_back(current);
        for (const character::ControlId next : followers[current]) {
            if (--indegree[next] == 0U) ready.push_back(next);
        }
    }
    if (evaluation.evaluation_order.size() != graph.nodes().size()) {
        return core::Result<ControlEvaluation>::failure(validation(
            "control dependency DAG contains a cycle"));
    }
    return core::Result<ControlEvaluation>::success(std::move(evaluation));
}

} // namespace carto::pose
