#pragma once

#include <carto/character/definition.hpp>
#include <carto/pose/ik_fk_match.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace carto::pose {

// Explicit authoring only. No chain, control role, or mode channel is inferred.
// The target is a world position and the optional pole is a world direction;
// generic control values are not automatically interpreted as either.
struct IkFkAuthoring {
    static constexpr std::size_t kMaxSerializedBytes = 4096U;
    static constexpr std::size_t kMaxSetSize = 128U;
    character::RigId rig;
    assets::Sha256Digest rig_digest;
    assets::Sha256Digest graph_digest;
    assets::Sha256Digest limits_digest;
    TwoBoneChain chain;
    character::ControlId fk_root;
    character::ControlId fk_mid;
    character::ControlId fk_effector;
    character::ControlId ik_target;
    std::optional<character::ControlId> ik_pole;
    character::ControlId mode_control;
    std::uint8_t mode_axis = 0U;
    double fk_mode_value = 0.0;
    double ik_mode_value = 1.0;
    IkBudget budget;
    double max_position_jump = 1e-6;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_against(
        const character::RigDefinition&, const kinematic::KinematicGraph&,
        const kinematic::JointLimitSet&) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<IkFkAuthoring> deserialize(std::string_view);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Validates the shared rig, graph and limits once, then validates each
// record's pins and roles against them. Does not impose cross-record ownership
// or order; callers managing several chains must enforce those policies.
// An empty set succeeds without validating the shared inputs.
[[nodiscard]] core::Result<void> validate_ik_fk_authoring_set(
    std::span<const IkFkAuthoring>, const character::RigDefinition&,
    const kinematic::KinematicGraph&, const kinematic::JointLimitSet&);

enum class IkFkMode : std::uint8_t { fk, ik };

// A proposal is not an applied control change. The caller supplies or maps
// world-space IK values and FK locals and decides how to persist/apply them.
// The mode value is a raw suggestion, not an applied switch; generic control
// evaluation may inherit values or affect joints. Proposals currently require
// the default coordinate policy and no posed ancestors outside the chain.
// Effector orientation is reported, not bounded by this contract.
struct IkFkSwitchProposal {
    IkFkMode target_mode;
    character::ControlId mode_control;
    std::uint8_t mode_axis;
    double mode_value;
    IkFkMatchResult match;
};

[[nodiscard]] core::Result<IkFkSwitchProposal> propose_ik_fk_switch(
    const IkFkAuthoring&, const character::RigDefinition&,
    const kinematic::KinematicGraph&, const kinematic::JointLimitSet&,
    IkFkMode target_mode, const LocalPose& fk_or_base_locals,
    const std::optional<IkControlState>& world_ik = std::nullopt);

} // namespace carto::pose
