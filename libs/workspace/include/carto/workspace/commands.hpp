#pragma once

#include <carto/workspace/session.hpp>
#include <carto/workspace/store.hpp>

#include <carto/character/bindings.hpp>
#include <carto/character/definition.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/morph.hpp>
#include <carto/character/motion_document.hpp>
#include <carto/character/receipt.hpp>
#include <carto/character/time.hpp>

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace carto::workspace {

// Typed per-domain command surface (ch19 application mechanics). Every payload
// carries an expected source revision: a command authored against a stale
// revision fails closed and never touches source (ch04). Every command
// validates against the session and the character definitions before it is
// allowed to map onto a journaled mutation, and every command has an exact
// inverse so undo is a forward journal entry, never a mutation of old
// receipts.

struct RigAddJoint {
    core::Revision expected_source_revision;
    kinematic::JointId joint;
    std::optional<kinematic::JointId> parent;
    core::Transform rest = core::Transform::identity();
    kinematic::JointKind kind = kinematic::JointKind::fixed;
    std::string semantic_type;
};

struct RigRemoveJoint {
    core::Revision expected_source_revision;
    kinematic::JointId joint;
};

struct RigSetRest {
    core::Revision expected_source_revision;
    kinematic::JointId joint;
    core::Transform rest = core::Transform::identity();
};

struct RigAddControl {
    core::Revision expected_source_revision;
    character::ControlId control;
    character::ControlShape shape = character::ControlShape::locator;
    core::Transform rest = core::Transform::identity();
    std::vector<kinematic::JointId> affected_joints;
    std::uint8_t allowed_axes = 0U;
    std::string control_set;
};

struct RigRemoveControl {
    core::Revision expected_source_revision;
    character::ControlId control;
};

struct RigSetControlAxes {
    core::Revision expected_source_revision;
    character::ControlId control;
    std::uint8_t allowed_axes = 0U;
};

struct RigBindAttachment {
    core::Revision expected_source_revision;
    kinematic::JointId joint;
    character::MeshAssetRef mesh;
    core::Transform offset = core::Transform::identity();
    std::string role;
};

struct RigUnbindAttachment {
    core::Revision expected_source_revision;
    kinematic::JointId joint;
    std::uint64_t mesh_asset = 0U;
};

struct SkinPaintStamp {
    core::Revision expected_source_revision;
    std::uint32_t vertex = 0U;
    // 1..8 influences, admitted by character::validate_influences exactly as
    // for every other skin weight producer.
    std::vector<kinematic::JointInfluence> influences;
};

struct SkinSetWeightLock {
    core::Revision expected_source_revision;
    std::uint32_t vertex = 0U;
    bool locked = false;
};

// Rebinds the weight layer to the current joint graph after a structural
// change. The caller must present the current graph digest explicitly so a
// stale rebind cannot silently bless changed topology. The optional previous
// state is inverse bookkeeping written by invert_command; when present, the
// command restores that state instead of rebinding.
struct SkinRebindLayer {
    core::Revision expected_source_revision;
    assets::Sha256Digest joint_graph_digest;
    std::optional<character::WeightStaleness> previous_staleness;
    std::optional<core::Revision> previous_stale_since;
    std::optional<assets::Sha256Digest> previous_joint_graph_digest;
};

struct AnimateSetKey {
    core::Revision expected_source_revision;
    temporal::ChannelId channel;
    character::RationalTime time;
    double value = 0.0;
};

struct AnimateRemoveKey {
    core::Revision expected_source_revision;
    temporal::ChannelId channel;
    character::RationalTime time;
};

struct AnimateSetRootMotion {
    core::Revision expected_source_revision;
    character::RootMotionPolicy policy = character::RootMotionPolicy::none;
};

struct FaceAddMorphTarget {
    core::Revision expected_source_revision;
    character::MorphTarget morph;
};

struct FaceRemoveMorphTarget {
    core::Revision expected_source_revision;
    character::MorphTargetId morph;
};

struct SimBindGarment {
    core::Revision expected_source_revision;
    character::GarmentBinding binding;
};

struct SimUnbindGarment {
    core::Revision expected_source_revision;
    character::GarmentBindingId binding;
};

struct SimEnqueueBake {
    core::Revision expected_source_revision;
    std::uint64_t job_id = 0U;
    character::RationalTime frame_start;
    character::RationalTime frame_end;
    std::uint32_t max_frames = 0U;
    assets::Sha256Digest settings_digest;
};

struct SimCancelBake {
    core::Revision expected_source_revision;
    std::uint64_t job_id = 0U;
};

// Removes a queued job record entirely; refused once a receipt is attached so
// accepted evidence is never silently discarded.
struct SimDiscardBake {
    core::Revision expected_source_revision;
    std::uint64_t job_id = 0U;
};

// Admits an external solver receipt for a queued job. The session layer never
// fabricates solve results; without a receipt digest this command does not
// exist and the job stays queued (honest unsupported, ch00 invariant 7).
struct SimAttachBakeReceipt {
    core::Revision expected_source_revision;
    std::uint64_t job_id = 0U;
    assets::Sha256Digest receipt_digest;
};

// Inverse bookkeeping for an attached receipt: returns the job to the queued
// state while preserving the rest of the record.
struct SimClearBakeReceipt {
    core::Revision expected_source_revision;
    std::uint64_t job_id = 0U;
};

struct ReviewRecordGate {
    core::Revision expected_source_revision;
    std::string gate_id;
    character::GateStatus status = character::GateStatus::not_run;
    // Inverse bookkeeping: the status before this command, or nullopt when the
    // gate did not exist (inverse then erases the entry).
    std::optional<character::GateStatus> previous_status;
};

enum class WorkspaceCommandKind : std::uint8_t {
    rig_add_joint,
    rig_remove_joint,
    rig_set_rest,
    rig_add_control,
    rig_remove_control,
    rig_set_control_axes,
    rig_bind_attachment,
    rig_unbind_attachment,
    skin_paint_stamp,
    skin_set_weight_lock,
    skin_rebind_layer,
    animate_set_key,
    animate_remove_key,
    animate_set_root_motion,
    face_add_morph_target,
    face_remove_morph_target,
    sim_bind_garment,
    sim_unbind_garment,
    sim_enqueue_bake,
    sim_cancel_bake,
    sim_discard_bake,
    sim_attach_bake_receipt,
    sim_clear_bake_receipt,
    review_record_gate,
};

[[nodiscard]] std::string_view workspace_command_kind_name(
    WorkspaceCommandKind kind) noexcept;

using WorkspaceCommandPayload = std::variant<
    RigAddJoint,
    RigRemoveJoint,
    RigSetRest,
    RigAddControl,
    RigRemoveControl,
    RigSetControlAxes,
    RigBindAttachment,
    RigUnbindAttachment,
    SkinPaintStamp,
    SkinSetWeightLock,
    SkinRebindLayer,
    AnimateSetKey,
    AnimateRemoveKey,
    AnimateSetRootMotion,
    FaceAddMorphTarget,
    FaceRemoveMorphTarget,
    SimBindGarment,
    SimUnbindGarment,
    SimEnqueueBake,
    SimCancelBake,
    SimDiscardBake,
    SimAttachBakeReceipt,
    SimClearBakeReceipt,
    ReviewRecordGate>;

// One typed workspace command: the domain it belongs to, the character it
// addresses, and a bounded payload. Payload-level validation is pure (no
// store access); the controller performs referential validation against the
// session and character definitions before journaling anything.
struct WorkspaceCommand {
    static constexpr std::size_t kMaxSemanticBytes = 128U;
    static constexpr std::size_t kMaxRoleBytes = 64U;
    static constexpr std::size_t kMaxControlSetBytes = 128U;
    static constexpr std::size_t kMaxGateIdBytes = 128U;

    WorkspaceDomain domain = WorkspaceDomain::rig;
    character::CharacterId target_character;
    WorkspaceCommandPayload payload;

    [[nodiscard]] WorkspaceCommandKind kind() const noexcept {
        return static_cast<WorkspaceCommandKind>(payload.index());
    }

    [[nodiscard]] core::Revision expected_source_revision() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<WorkspaceCommand> deserialize(
        std::string_view text);
};

// Computes the exact inverse of an applied command against the store state
// BEFORE the mutation. Inverse commands carry the same expected revision
// discipline; the controller rewrites their expected revision at apply time.
[[nodiscard]] core::Result<WorkspaceCommand> invert_command(
    const WorkspaceCommand& command, const CharacterSourceStore& store);

} // namespace carto::workspace
