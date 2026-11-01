#pragma once

#include <carto/character/motion_document.hpp>
#include <carto/motion/curve_edit.hpp>
#include <carto/motion/layers.hpp>
#include <carto/pose/ik_fk_authoring.hpp>
#include <carto/temporal/sequence.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace carto::pose {

// Identities are caller assertions. An absent restore means not asserted,
// not proof that no restore occurred. None confers execution authority.
struct ModeIntentLineage {
    std::string scenario;
    std::string branch;
    std::string session;
    std::optional<std::string> restore;
    [[nodiscard]] core::Result<void> validate() const;
};

// Explicit caller-authored association, not an inferred or verified mapping:
// TemporalChannel does not identify a control axis and KeyedCurve does not
// identify a TemporalChannel. LayerCurve does bind the curve to a control.
struct ModeIntentCrosswalk {
    temporal::TargetBinding stable_target;
    temporal::ChannelId channel;
    assets::Sha256Digest channel_digest;
    motion::AnimationLayerId layer;
    motion::MotionCurveId curve;
    assets::Sha256Digest curve_digest;
    character::ControlId control;
    std::uint8_t axis = 0U;
    [[nodiscard]] core::Result<void> validate() const;
};

// Caller-asserted classification of the pose input, not authenticated status
// or evidence of an observed/executed physical event. The caller must select
// one explicitly; unknown and unspecified values are refused.
enum class SourcePoseAssertion : std::uint8_t {
    unspecified = 0U,
    authored_rest = 1U,
    evaluated_preview = 2U,
    current_admitted_assertion = 3U,
    authored_intended_future = 4U,
    predicted_alternative = 5U,
    observed_assertion = 6U,
    executed_assertion = 7U,
};

// Caller asserts the frame of world-space solver inputs (IK target/pole).
// `graph_world` means the default coordinate-policy world frame used by the
// current matcher; FK local overrides remain parent-local. This label does
// not prove frame provenance or perform a coordinate conversion.
enum class SolverInputFrameAssertion : std::uint8_t {
    unspecified = 0U,
    graph_world = 1U,
};

// Clock values are exact integer ticks. Clip-local, source and channel-local
// clocks and their correspondence are caller assertions; this operation does
// not infer or verify retiming. Each closed interval has its own timebase.
struct ModeKeyIntentContext {
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest motion_document_digest;
    character::RigId rig;
    assets::Sha256Digest rig_digest;
    assets::Sha256Digest rest_graph_digest;
    assets::Sha256Digest limits_digest;
    assets::Sha256Digest source_pose_digest;
    SourcePoseAssertion source_pose_assertion = SourcePoseAssertion::unspecified;
    assets::Sha256Digest authoring_digest;
    std::string reference_frame;
    SolverInputFrameAssertion solver_input_frame = SolverInputFrameAssertion::unspecified;
    ModeIntentLineage lineage;
    character::RationalTime clip_tick;
    character::RationalTime clip_start;
    character::RationalTime clip_end;
    character::RationalTime source_tick;
    character::RationalTime source_start;
    character::RationalTime source_end;
    character::RationalTime channel_tick;
    character::RationalTime channel_start;
    character::RationalTime channel_end;
    [[nodiscard]] core::Result<void> validate() const;
};

// The caller may review the new curve and proposed match independently. The
// original curve/channel/document and all runtime control state remain intact.
// mode_before and mode_at_key are raw layer-curve samples, not an evaluated
// layer-stack/control mode or evidence of pose continuity.
struct IkFkModeKeyIntent {
    ModeKeyIntentContext context;
    ModeIntentCrosswalk crosswalk;
    character::MotionDocumentId document;
    temporal::ClipId clip;
    assets::Sha256Digest clip_digest;
    IkFkMode source_mode;
    IkFkMode target_mode;
    double mode_before = 0.0;
    double mode_at_key = 0.0;
    motion::KeyedCurve proposed_curve;
    motion::CurveEditReceipt edit;
    IkFkSwitchProposal match;
};

// Pure authored-intent proposal. It neither edits a TemporalChannel nor
// proves the caller's source/pose digests, clock correspondence, channel-axis
// crosswalk or reference frame against an external authority. The existing
// matcher reports a same-input position jump; it does not prove time-sampled
// pose continuity, execution, observation, or physical state.
[[nodiscard]] core::Result<IkFkModeKeyIntent> propose_ik_fk_mode_key_intent(
    const character::MotionDocument& document,
    const temporal::MotionClip& clip,
    const temporal::TemporalChannel& channel,
    const motion::AnimationLayer& layer,
    const ModeKeyIntentContext& context,
    const ModeIntentCrosswalk& crosswalk,
    const IkFkAuthoring& authoring,
    const character::RigDefinition& rig,
    const kinematic::KinematicGraph& graph,
    const kinematic::JointLimitSet& limits,
    IkFkMode source_mode,
    IkFkMode target_mode,
    const LocalPose& source_locals,
    const std::optional<IkControlState>& source_ik = std::nullopt);

} // namespace carto::pose
