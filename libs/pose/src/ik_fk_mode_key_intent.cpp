#include <carto/pose/ik_fk_mode_key_intent.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace carto::pose {
namespace {
core::Diagnostic invalid(const char* message) {
    return {core::ErrorCode::validation_failed, message};
}
bool identity(std::string_view value) {
    return !value.empty() && value.size() <= ModeKeyIntentContext::kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= 0x20U && c != 0x7fU;
        }) && std::any_of(value.begin(), value.end(), [](unsigned char c) { return c != ' '; });
}
bool interval(character::RationalTime start, character::RationalTime at,
              character::RationalTime end) {
    return start.validate() && at.validate() && end.validate() &&
        character::same_timebase(start, at) && character::same_timebase(at, end) &&
        start.tick <= at.tick && at.tick <= end.tick;
}
bool mode_valid(IkFkMode mode) {
    return mode == IkFkMode::fk || mode == IkFkMode::ik;
}
bool pose_assertion_valid(SourcePoseAssertion value) {
    switch (value) {
    case SourcePoseAssertion::authored_rest:
    case SourcePoseAssertion::evaluated_preview:
    case SourcePoseAssertion::current_admitted_assertion:
    case SourcePoseAssertion::authored_intended_future:
    case SourcePoseAssertion::predicted_alternative:
    case SourcePoseAssertion::observed_assertion:
    case SourcePoseAssertion::executed_assertion:
        return true;
    case SourcePoseAssertion::unspecified:
        return false;
    }
    return false;
}
bool within_duration(character::RationalTime end, double duration) {
    // Integers through 2^53 convert exactly to double. Use an outward bound
    // for the subsequent rational division; exact integral endpoints have no
    // division error and remain admissible at the boundary.
    if (end.tick < 0 || end.tick > (std::int64_t{1} << 53)) return false;
    const auto converted = character::to_seconds(end);
    const bool exact_integral = end.tick % end.ticks_per_second == 0 &&
        static_cast<double>(end.tick / end.ticks_per_second) == duration;
    return std::isfinite(converted.seconds) && (exact_integral ||
        std::nextafter(converted.seconds + converted.max_abs_error,
            std::numeric_limits<double>::infinity()) <= duration);
}
}

core::Result<void> ModeIntentLineage::validate() const {
    if (!identity(scenario) || !identity(branch) || !identity(session) || (restore && !identity(*restore)))
        return core::Result<void>::failure(invalid("explicit scenario, branch and session, and any asserted restore identity, must be valid"));
    return core::Result<void>::success();
}

core::Result<void> ModeIntentCrosswalk::validate() const {
    if (!stable_target.validate() || stable_target.kind == temporal::TargetBindingKind::none ||
        !identity(stable_target.stable_id) ||
        !channel || channel_digest.is_zero() || !layer || !curve || curve_digest.is_zero() ||
        !control || axis >= character::kAxisCount)
        return core::Result<void>::failure(invalid("explicit mode crosswalk is invalid"));
    return core::Result<void>::success();
}

core::Result<void> ModeKeyIntentContext::validate() const {
    if (!identity(source_namespace) || !identity(reference_frame) ||
        solver_input_frame != SolverInputFrameAssertion::graph_world ||
        source_revision.value() == 0U || source_revision.exhausted() ||
        source_digest.is_zero() || motion_document_digest.is_zero() || !rig || rig_digest.is_zero() ||
        rest_graph_digest.is_zero() || limits_digest.is_zero() || source_pose_digest.is_zero() ||
        !pose_assertion_valid(source_pose_assertion) || authoring_digest.is_zero() ||
        !lineage.validate() || !interval(clip_start, clip_tick, clip_end) ||
        !interval(source_start, source_tick, source_end) ||
        !interval(channel_start, channel_tick, channel_end) ||
        clip_start.tick < 0 || channel_start.tick < 0 ||
        clip_end.tick > (std::int64_t{1} << 53) || channel_end.tick > (std::int64_t{1} << 53) ||
        clip_tick.tick <= clip_start.tick)
        return core::Result<void>::failure(invalid("mode intent requires bound source, lineage, frame and exact valid clock intervals"));
    return core::Result<void>::success();
}

core::Result<IkFkModeKeyIntent> propose_ik_fk_mode_key_intent(
    const character::MotionDocument& document, const temporal::MotionClip& clip,
    const temporal::TemporalChannel& channel, const motion::AnimationLayer& layer,
    const ModeKeyIntentContext& context, const ModeIntentCrosswalk& crosswalk,
    const IkFkAuthoring& authoring, const character::RigDefinition& rig,
    const kinematic::KinematicGraph& graph, const kinematic::JointLimitSet& limits,
    IkFkMode source_mode, IkFkMode target_mode, const LocalPose& source_locals,
    const std::optional<IkControlState>& source_ik) {
    using Result = core::Result<IkFkModeKeyIntent>;
    if (auto r = document.validate(); !r) return Result::failure(r.error());
    if (auto r = clip.validate(); !r) return Result::failure(r.error());
    if (auto r = channel.validate(); !r) return Result::failure(r.error());
    if (auto r = layer.validate(); !r) return Result::failure(r.error());
    if (auto r = context.validate(); !r) return Result::failure(r.error());
    if (auto r = crosswalk.validate(); !r) return Result::failure(r.error());
    const auto found = std::find_if(layer.curves.begin(), layer.curves.end(), [&crosswalk](const auto& item) {
        return !item.target.joint && item.target.control == crosswalk.control;
    });
    if (found == layer.curves.end())
        return Result::failure(invalid("mode control has no curve in the declared layer"));
    const auto& mode_curve = *found;
    if (!mode_valid(source_mode) || !mode_valid(target_mode) || source_mode == target_mode ||
        layer.id != crosswalk.layer ||
        crosswalk.control != authoring.mode_control || crosswalk.axis != authoring.mode_axis ||
        mode_curve.target.joint || mode_curve.target.control != crosswalk.control ||
        mode_curve.curve.id != crosswalk.curve || channel.id != crosswalk.channel ||
        !(channel.target == crosswalk.stable_target) ||
        channel.value_kind != temporal::ChannelValueKind::scalar ||
        mode_curve.curve.interpolation != motion::InterpolationKind::step ||
        document.timebase_ticks_per_second != context.clip_tick.ticks_per_second ||
        mode_curve.curve.timebase_ticks_per_second != context.clip_tick.ticks_per_second ||
        !character::same_timebase(layer.window_start, context.clip_tick) ||
        !character::same_timebase(layer.window_end, context.clip_tick) ||
        context.clip_tick.tick < layer.window_start.tick || context.clip_tick.tick > layer.window_end.tick ||
        context.rig != authoring.rig || context.rig_digest != authoring.rig_digest ||
        context.rest_graph_digest != authoring.graph_digest || context.limits_digest != authoring.limits_digest)
        return Result::failure(invalid("mode target, clocks, control crosswalk or rest graph do not match"));
    const auto authoring_digest = authoring.canonical_digest();
    const auto channel_digest = channel.canonical_digest();
    const auto clip_digest = clip.canonical_digest();
    const auto curve_digest = mode_curve.curve.canonical_digest();
    if (!authoring_digest || !channel_digest || !clip_digest || !curve_digest ||
        authoring_digest.value() != context.authoring_digest ||
        channel_digest.value() != crosswalk.channel_digest ||
        curve_digest.value() != crosswalk.curve_digest)
        return Result::failure(invalid("mode source or authoring digest does not match"));
    std::size_t document_channels = 0U, document_clips = 0U, clip_channels = 0U;
    for (const auto& ref : document.channels) if (ref.channel == channel.id) {
        ++document_channels;
        if (ref.digest != channel_digest.value()) return Result::failure(invalid("motion document channel digest is stale"));
    }
    for (const auto& ref : document.clips) if (ref.clip == clip.id) {
        ++document_clips;
        if (ref.digest != clip_digest.value()) return Result::failure(invalid("motion document clip digest is stale"));
    }
    for (const auto& ref : clip.channels) if (ref.channel == channel.id) {
        ++clip_channels;
        if (ref.channel_digest != channel_digest.value()) return Result::failure(invalid("clip channel digest is stale"));
    }
    if (document_channels != 1U || document_clips != 1U || clip_channels != 1U ||
        std::count(document.targets.begin(), document.targets.end(), mode_curve.target) != 1)
        return Result::failure(invalid("motion document and clip must bind the unique channel and control target"));
    if (!within_duration(context.clip_end, clip.duration) ||
        !within_duration(context.channel_end, channel.duration))
        return Result::failure(invalid("declared clip or channel interval exceeds its duration"));
    for (const auto& key : mode_curve.curve.keys)
        if (key.value != authoring.fk_mode_value && key.value != authoring.ik_mode_value)
            return Result::failure(invalid("mode curve has a value outside the authored discrete modes"));
    const character::RationalTime previous{context.clip_tick.tick - 1, context.clip_tick.ticks_per_second};
    if (previous.tick < layer.window_start.tick ||
        mode_curve.curve.keys.front().time.tick > previous.tick)
        return Result::failure(invalid("the prior mode tick must be inside the layer window and preceded by an authored key"));
    const auto before = mode_curve.curve.sample(previous);
    const double source_value = source_mode == IkFkMode::fk ? authoring.fk_mode_value : authoring.ik_mode_value;
    const double target_value = target_mode == IkFkMode::fk ? authoring.fk_mode_value : authoring.ik_mode_value;
    if (!before || before.value() != source_value)
        return Result::failure(invalid("source mode does not match the exact prior clip tick"));
    auto match = propose_ik_fk_switch(authoring, rig, graph, limits, target_mode, source_locals, source_ik);
    if (!match) return Result::failure(match.error());
    auto edit = motion::insert_key(mode_curve.curve, {context.clip_tick, target_value, 0.0, 0.0}, false);
    if (!edit) return Result::failure(edit.error());
    const auto preserved = edit.value().curve.sample(previous);
    const auto at_key = edit.value().curve.sample(context.clip_tick);
    if (!preserved || !at_key || preserved.value() != before.value() || at_key.value() != target_value)
        return Result::failure(invalid("mode-key samples do not match the requested intent"));
    return Result::success({context, crosswalk, document.id, clip.id, clip_digest.value(),
        source_mode, target_mode, before.value(), at_key.value(), std::move(edit.value().curve),
        std::move(edit.value().receipt), std::move(match.value())});
}
} // namespace carto::pose
