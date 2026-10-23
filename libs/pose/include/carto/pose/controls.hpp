#pragma once

#include <carto/character/controls.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/pose/fk.hpp>

#include <array>
#include <map>
#include <vector>

namespace carto::pose {

// Control-graph evaluation: user-facing control values become per-joint local
// transform offsets. Evaluation respects each control's allowed-axes bitmask
// and per-axis limits, applies follow-space inheritance (a follower inherits
// its follow driver's effective value before adding its own), and walks the
// control DAG in a deterministic topological order. Explicit ControlEdge
// entries declare evaluation order only; value inheritance flows through
// follow_space so a driver wired in twice orders the evaluation without
// double-applying its offset. The authored ControlGraph is contractually
// acyclic (its validate() fails closed on cycles); evaluation re-checks
// acyclicity and fails closed rather than silently dropping an edge.

using ControlValues =
    std::map<character::ControlId, std::array<double, character::kAxisCount>>;

struct ControlEvaluation {
    // Net local offset per affected joint (identity for unaffected joints).
    LocalPose joint_offsets;
    // Deterministic topological order the values were applied in.
    std::vector<character::ControlId> evaluation_order;
};

// Fails closed when: the control graph is invalid, a value names an unknown
// control, a value drives an axis the control does not allow, or any value is
// non-finite. Missing values are treated as zero (a control the user did not
// touch exerts no offset). Axis order is the character::kAxis bitmask order:
// translation X/Y/Z then rotation X/Y/Z, rotations in radians under the
// library's intrinsic-X/Y/Z Euler convention (see rotation.hpp).
[[nodiscard]] core::Result<ControlEvaluation> evaluate_controls(
    const character::ControlGraph& graph,
    const ControlValues& values);

} // namespace carto::pose
