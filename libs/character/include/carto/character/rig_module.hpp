#pragma once

#include <carto/character/controls.hpp>
#include <carto/character/definition.hpp>
#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::character {

// Rig module templates (reusable rig fragments: a finger, an ear, a tentacle).
// A template is durable authoring truth only: a bounded name token, a joint
// fragment with one explicit attachment root, a control fragment with its
// edges, and internal symmetry declared over fragment JointIds. Instantiation
// grafts the attachment root under the target graph's existing root (or makes
// it the sole root of an empty target) and remaps every fragment identity
// atomically, so a module is never edited in place and a target graph is
// never mutated: both results are new graphs the caller may adopt or drop.

// How fragment identities land in the target graphs. preserve_fragment_ids
// keeps the authored ids and refuses any identity the target already owns;
// allocate_fresh_ids issues deterministic stable ids past everything the
// target and the fragment already use, so a template may be instantiated
// many times into one rig without collision.
enum class ModuleIdPolicy : std::uint8_t {
    preserve_fragment_ids,
    allocate_fresh_ids,
};

struct RigModuleTemplate {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxNameBytes = 128U;
    static constexpr std::size_t kMaxJoints = 1'024U;
    static constexpr std::size_t kMaxControls = 512U;
    static constexpr std::size_t kMaxEdges = 2'048U;
    static constexpr std::size_t kMaxSymmetryPairs = 512U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    std::uint32_t schema_version = kSchemaVersion;
    // Bounded safe token naming the module ("finger.index", "ear.left").
    std::string name;
    // Fragment joints. Exactly one node carries std::nullopt parent: the
    // attachment root, grafted onto the target root at instantiation. Every
    // other parent must resolve inside the fragment.
    std::vector<kinematic::KinematicNode> joints;
    // Fragment control nodes and edges over ControlDefinition fragments.
    std::vector<ControlDefinition> controls;
    std::vector<ControlEdge> control_edges;
    // Internal symmetry over fragment JointIds; the pairs are remapped, not
    // re-inferred, at instantiation.
    std::vector<SymmetryPair> symmetry;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<RigModuleTemplate> deserialize(
        std::string_view text);
};

struct RigModuleReceipt {
    std::string module_name;
    assets::Sha256Digest source_graph_digest;
    assets::Sha256Digest result_graph_digest;
    std::size_t added_joint_count = 0U;
    std::size_t added_control_count = 0U;
    std::size_t added_edge_count = 0U;
    std::size_t added_symmetry_pair_count = 0U;
};

struct RigModuleInstance {
    kinematic::KinematicGraph kinematic_graph;
    ControlGraph control_graph;
    // The module's symmetry pairs remapped into target JointId space, sorted
    // by left joint; the caller merges them into the target rig's symmetry
    // map (and re-checks uniqueness against the pairs already there).
    std::vector<SymmetryPair> symmetry;
    RigModuleReceipt receipt;
};

// Validates the template and both targets, then produces new graphs: the
// target contents plus the fragment, with every fragment parent,
// affected_joints entry, follow-space referent, and symmetry side remapped
// under one id mapping computed up front. Fails closed, without producing
// any graph, when: a bound would be exceeded in the TARGET (kinematic node
// capacity, control count, edge count, or the pair count the target rig
// admits); a fragment semantic-type token collides with one the target
// kinematic graph already carries; preserve_fragment_ids is asked to reuse
// an identity the target owns; or any fragment reference does not resolve.
[[nodiscard]] core::Result<RigModuleInstance> instantiate_rig_module(
    const RigModuleTemplate& module,
    const kinematic::KinematicGraph& target_kinematic,
    const ControlGraph& target_controls,
    ModuleIdPolicy policy);

} // namespace carto::character
