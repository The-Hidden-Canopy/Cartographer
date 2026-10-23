#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/definition.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace carto::character {

// Rig symmetry edits (ch06 "mirror, symmetry and deliberate asymmetry").
// Symmetry is declared by RigDefinition::symmetry pairs over stable JointIds;
// these operations read it, never infer it. Mirroring rewrites rest
// transforms only: identities, parentage, joint kinds and node classes are
// preserved, and every result is a new graph the caller may adopt or drop.
// Divergence analysis reports authored asymmetry without correcting it, so a
// deliberately asymmetric creature is never silently "fixed".

enum class SymmetryAxis : std::uint8_t { x, y, z };

enum class SymmetryDirection : std::uint8_t {
    left_to_right,
    right_to_left,
};

struct SymmetryDivergence {
    kinematic::JointId left;
    kinematic::JointId right;
    // Distance between the right rest translation and the mirrored left one.
    double translation_error = 0.0;
    // Angle in radians between the right rest rotation and the mirrored left.
    double rotation_error = 0.0;
    // The right joint's parent is not the mirror of the left joint's parent.
    bool parent_mismatch = false;
};

struct SymmetryReport {
    std::size_t pair_count = 0U;
    // Pairs outside tolerance or with mismatched parents, in pair order.
    std::vector<SymmetryDivergence> divergent;
};

struct SymmetryTolerance {
    double translation = 1e-9;
    double rotation = 1e-9;

    [[nodiscard]] core::Result<void> validate() const;
};

// Reflects a local rest transform across the plane normal to `axis`
// (translation component negated, rotation reflected as a pseudovector).
[[nodiscard]] core::Transform reflect_local_transform(
    const core::Transform& transform,
    SymmetryAxis axis) noexcept;

[[nodiscard]] core::Result<SymmetryReport> analyze_rig_symmetry(
    const kinematic::KinematicGraph& graph,
    std::span<const SymmetryPair> pairs,
    SymmetryAxis axis,
    const SymmetryTolerance& tolerance);

struct RigMirrorReceipt {
    SymmetryAxis axis = SymmetryAxis::x;
    SymmetryDirection direction = SymmetryDirection::left_to_right;
    assets::Sha256Digest source_graph_digest;
    assets::Sha256Digest result_graph_digest;
    std::size_t changed_joint_count = 0U;
};

struct RigMirrorResult {
    kinematic::KinematicGraph graph;
    RigMirrorReceipt receipt;
};

// Copies mirrored rest transforms from one side of every pair onto the
// other. Refuses, without producing a graph, when a pair names a missing
// joint, a joint appears in two pairs, or a destination joint's parent is not
// the mirror of its source's parent (local mirroring would then misplace the
// limb). Applying left_to_right then right_to_left is the identity.
[[nodiscard]] core::Result<RigMirrorResult> mirror_rig_rest(
    const kinematic::KinematicGraph& graph,
    std::span<const SymmetryPair> pairs,
    SymmetryAxis axis,
    SymmetryDirection direction);

struct RigControlMirrorReceipt {
    SymmetryAxis axis = SymmetryAxis::x;
    SymmetryDirection direction = SymmetryDirection::left_to_right;
    assets::Sha256Digest source_graph_digest;
    assets::Sha256Digest result_graph_digest;
    assets::Sha256Digest source_control_graph_digest;
    assets::Sha256Digest result_control_graph_digest;
    // Destination controls whose rewritten definition differed from the
    // authored one; zero when the copy was already exact.
    std::size_t changed_control_count = 0U;
};

struct RigControlMirrorResult {
    // The joint graph is returned unchanged (rest mirroring is
    // mirror_rig_rest's contract); both outputs are new objects the caller
    // may adopt or drop.
    kinematic::KinematicGraph graph;
    ControlGraph control_graph;
    RigControlMirrorReceipt receipt;
};

// Copies control definitions from one side of every control pair onto the
// other: the destination takes the source's shape, transform, axes and
// limits, its affected joints and follow_space remapped through the joint
// and control mirror maps, and its control set carried verbatim. The joint
// graph itself is not rewritten here. Refuses, without producing graphs,
// when a pair names a missing control, a control appears in two pairs, a
// source control affects a joint outside the joint mirror map, or the
// remapped follow-space graph would cycle. Applying left_to_right then
// right_to_left is the identity.
[[nodiscard]] core::Result<RigControlMirrorResult> mirror_rig_controls(
    const kinematic::KinematicGraph& graph,
    const ControlGraph& control_graph,
    std::span<const SymmetryPair> joint_pairs,
    std::span<const ControlSymmetryPair> control_pairs,
    SymmetryAxis axis,
    SymmetryDirection direction);

} // namespace carto::character
